"""Game state orchestrator — wires proxy + parser + entity DB + slop to event bus."""
import asyncio
import hashlib
import logging
from collections import deque
from pathlib import Path

from event_bus import EventBus
from slop_loader import SlopLoader, ASSET_NPC_THUMB, ASSET_ITEM_THUMB, ASSET_ROOM_IMAGE

# Import reusable modules from mudproxy
import sys
_mudproxy_parent = str(Path(__file__).resolve().parent.parent)
if _mudproxy_parent not in sys.path:
    sys.path.insert(0, _mudproxy_parent)

from mudproxy.proxy import MudProxy
from mudproxy.parser import MudParser
from mudproxy.config import Config
from mudproxy.gamedata import GameData
from mudproxy.hp_tracker import HPTracker
from mudproxy.entity_db import EntityDB

logger = logging.getLogger(__name__)

# MajorMUD currency types — matched by substring in item name (lowercase)
_CURRENCY_TYPES = {
    "runic coin": "runic",
    "platinum piece": "platinum",
    "gold crown": "gold",
    "silver noble": "silver",
    "copper farthing": "copper",
}


def _detect_currency(name: str) -> str | None:
    """Return currency type string if name is a currency item, else None."""
    lower = name.strip().lower()
    for pattern, ctype in _CURRENCY_TYPES.items():
        if pattern in lower:
            return ctype
    return None


def _enrich_item_data(item_data: dict | None, gamedata: "GameData") -> dict | None:
    """Add resolved shop names to item_data before sending to browser."""
    if not item_data or not gamedata.is_loaded:
        return item_data
    of = item_data.get("Obtained From", "")
    if of and "Shop" in of:
        item_data = dict(item_data)  # shallow copy to avoid mutating cache
        item_data["Obtained From"] = gamedata.resolve_shop_names(of)
    return item_data


class Orchestrator:
    def __init__(self, config: Config, event_bus: EventBus, slop: SlopLoader,
                 dat_dir: Path | None = None):
        self.config = config
        self.bus = event_bus
        self.slop = slop
        self.dat_dir = dat_dir
        self._loop: asyncio.AbstractEventLoop | None = None
        self._msg_queue = deque()
        self._drain_scheduled = False

        # Core modules
        self.proxy = MudProxy(config)
        self.parser = MudParser()
        self.gamedata = GameData()  # loads from ~/.cache/mudproxy/gamedata/
        self.hp_tracker = HPTracker(self.gamedata)
        self.entity_db = EntityDB()

        # Current state
        self._room_name = ""
        self._room_desc = ""
        self._room_exits: list[str] = []
        self._room_monsters: list[str] = []
        self._room_items: list[str] = []
        self._room_item_quantities: dict[str, int] = {}
        self._hp = 0
        self._max_hp = 0
        self._mana = 0
        self._max_mana = 0
        self._combat_target: str | None = None
        self._char_name = ""
        self._broadcast_channel = ""
        self._inv_sync_task: asyncio.Task | None = None
        self._last_inv_sync = 0

        self._wire_callbacks()

    def _wire_callbacks(self):
        """Connect proxy and parser callbacks to event bus emissions."""
        # Proxy → parser
        self.proxy.on_raw_line = self._on_raw_line
        self.proxy.on_connect = self._on_connect
        self.proxy.on_disconnect = self._on_disconnect
        self.proxy.on_client_command = self._on_client_command

        # Parser → events
        self.parser.on_room_update = self._on_room_update
        self.parser.on_hp_update = self._on_hp_update
        self.parser.on_combat = self._on_combat
        self.parser.on_death = self._on_death
        self.parser.on_xp = self._on_xp
        self.parser.on_chat = self._on_chat
        self.parser.on_char_name = self._on_char_name
        self.parser.on_who_list = self._on_who_list
        self.parser.on_wound_status = self._on_wound_status
        self.parser.on_pro = self._on_pro
        self.parser.on_inventory = self._on_inventory

    async def start(self):
        """Initialize and load data, start TCP proxy listener for MegaMud."""
        self._loop = asyncio.get_event_loop()
        self.gamedata.load()
        # Only auto-load all slop if no files were pre-loaded via --slop
        if not self.slop._loaded_files:
            self.slop.load_all()
        stats = self.slop.get_stats()
        logger.info(f"Slop loaded: {stats['total_assets']} assets from {stats['files']} files")

        # Start TCP proxy listener so MegaMud can connect through us
        await self.proxy.start()
        logger.info(f"Proxy listener on {self.config.proxy_host}:{self.config.proxy_port}")

        # BBS connection happens automatically when MegaMud connects to proxy

    async def connect(self) -> bool:
        return await self.proxy.connect_to_bbs()

    async def disconnect(self):
        await self.proxy.disconnect()

    async def inject_command(self, cmd: str):
        await self.proxy.inject_command(cmd)

    async def ghost_command(self, ghost_name: str, command: str):
        """Inject a fake telepath from the ghost character into MegaMud's stream."""
        # Set up interception for this ghost name
        self.proxy.set_ghost_name(ghost_name)
        self.proxy.set_ghost_callback(lambda resp: self._on_ghost_response(ghost_name, resp))

        # Format: "GhostName telepaths: @command"
        fake_line = f"{ghost_name} telepaths: {command}"
        await self.proxy.inject_to_client(fake_line)
        logger.info(f"Ghost command sent: {fake_line}")

    def _on_ghost_response(self, ghost_name: str, response: str):
        """Handle intercepted ghost character response from MegaMud."""
        logger.info(f"Ghost [{ghost_name}]: {response}")
        import asyncio
        try:
            loop = asyncio.get_event_loop()
            loop.call_soon_threadsafe(
                asyncio.ensure_future,
                self.bus.broadcast("ghost_response", {
                    "name": ghost_name,
                    "response": response,
                })
            )
        except Exception as e:
            logger.error(f"Ghost broadcast error: {e}")

    # ── Proxy callbacks ──

    def _on_raw_line(self, stripped: str, ansi: str):
        try:
            # Log raw ANSI for chat lines to file
            low = stripped.strip().lower()
            if any(kw in low for kw in ('telepaths:', 'gossips:', 'broadcasts:', 'says "')):
                with open('/tmp/ghost_ansi.log', 'a') as f:
                    f.write(f"ANSI: {ansi!r}\n")
                    f.write(f"STRIPPED: {stripped!r}\n\n")
            self.parser.feed_line(stripped, ansi)
        except Exception as e:
            logger.error(f"Parser error (non-fatal): {e}")

        # Detect receiving items from other players
        low = stripped.strip().lower()
        if 'just gave you' in low or 'you just received' in low:
            self._schedule_inv_sync(delay=2)

    def _on_connect(self):
        self._emit("connection", {"status": "connected"})
        # Start periodic inventory sync
        if self._inv_sync_task:
            self._inv_sync_task.cancel()
        if self._loop:
            self._inv_sync_task = self._loop.create_task(self._inv_sync_loop())

    def _schedule_inv_sync(self, delay=2):
        """Schedule a one-shot inventory sync after a delay."""
        import time
        if time.monotonic() - self._last_inv_sync < 5:
            return  # don't spam
        if self._loop and self.proxy.connected:
            self._loop.create_task(self._delayed_inv_sync(delay))

    async def _delayed_inv_sync(self, delay):
        import time
        await asyncio.sleep(delay)
        try:
            if self.proxy.connected:
                await self.proxy.inject_command("i")
                self._last_inv_sync = time.monotonic()
        except Exception:
            pass

    async def _inv_sync_loop(self):
        """Periodically inject 'i' to keep inventory synced."""
        import time
        await asyncio.sleep(2)  # initial delay after connect
        try:
            await self.proxy.inject_command("i")
            self._last_inv_sync = time.monotonic()
        except Exception:
            pass
        while True:
            await asyncio.sleep(60)  # sync every 60 seconds
            try:
                if self.proxy.connected:
                    await self.proxy.inject_command("i")
                    self._last_inv_sync = time.monotonic()
            except Exception:
                pass

    def _on_disconnect(self):
        self._emit("connection", {"status": "disconnected"})
        self._combat_target = None
        if self._inv_sync_task:
            self._inv_sync_task.cancel()
            self._inv_sync_task = None

    def _on_client_command(self, cmd: str):
        # Track attack commands for combat target
        lower = cmd.strip().lower()
        if lower.startswith(("a ", "at ", "att ", "attack ", "ba ", "back ", "backstab ", "k ", "kill ")):
            parts = cmd.strip().split(None, 1)
            if len(parts) > 1:
                self._combat_target = parts[1]

        # Inventory-changing commands → sync after a short delay
        if lower.startswith(("g ", "get ", "dr ", "drop ", "w ", "wea ", "wear ",
                             "rem ", "remove ", "sell ", "buy ", "give ")):
            self._schedule_inv_sync(delay=2)

    # ── Parser callbacks ──

    def _on_room_update(self, room):
        self._room_name = room.name
        self._room_desc = room.description
        self._room_exits = room.exits
        self._room_monsters = room.monsters
        self._room_items = room.items
        self._room_item_quantities = getattr(room, 'item_quantities', {})

        # Update HP tracker with new room monster list (preserves damage on survivors)
        self.hp_tracker.set_room_monsters(room.monsters)

        # Look up room image
        room_image_key = self.slop.get_room_image_key(room.name, room.exits)
        depth_key = self.slop.get_depth_key(room_image_key) if room_image_key else None
        if not room_image_key:
            logger.warning(f"No room image for: name={room.name!r} exits={room.exits!r}")
        else:
            logger.debug(f"Room image: {room.name!r} -> {room_image_key} depth={depth_key}")

        # Build entity thumbnail list
        entities = []
        for idx, name in enumerate(room.monsters):
            key = self.slop.get_entity_thumb_key(name)
            hp_frac = self.hp_tracker.get_hp_fraction(idx)
            monster_data = self.gamedata.get_monster_stats(name) if self.gamedata.is_loaded else None
            drops = self.gamedata.get_monster_drops(name) if self.gamedata.is_loaded else None
            entities.append({
                "name": name,
                "key": key,
                "type": "npc",
                "hp_fraction": hp_frac,
                "stats": monster_data,
                "drops": drops or [],
            })

        items = []
        quantities = getattr(room, 'item_quantities', {})
        for name in room.items:
            currency = _detect_currency(name)
            key = None if currency else self.slop.get_entity_thumb_key(name)
            item_data = None if currency else (
                self.gamedata.get_item(name) if self.gamedata.is_loaded else None)
            item_data = _enrich_item_data(item_data, self.gamedata)
            entry = {
                "name": name,
                "key": key,
                "type": "item",
                "item_data": item_data,
                "quantity": quantities.get(name, 1),
            }
            if currency:
                entry["currency"] = currency
            items.append(entry)

        self._emit("room_update", {
            "name": room.name,
            "description": room.description,
            "exits": room.exits,
            "room_image_key": room_image_key,
            "depth_key": depth_key,
            "entities": entities,
            "items": items,
        })

    def _on_hp_update(self, hp: int, max_hp: int, mana: int, max_mana: int):
        self._hp = hp
        self._max_hp = max_hp
        self._mana = mana
        self._max_mana = max_mana
        self._emit("hp_update", {
            "hp": hp, "max_hp": max_hp,
            "mana": mana, "max_mana": max_mana,
        })

    def _on_combat(self, line: str):
        # Parse damage from combat line and update HP tracker
        import re

        dmg_match = re.search(r'for (\d+) damage', line)
        if dmg_match:
            dmg = int(dmg_match.group(1))
            line_lower = line.lower()

            # Skip damage dealt BY monsters TO the player
            if ' you for ' in line_lower and not line_lower.startswith('you'):
                logger.debug(f"COMBAT(skip monster->player): {line}")
            else:
                # Extract target by checking which room monster appears in the line
                target = None
                for name in self._room_monsters:
                    if name.strip().lower() in line_lower:
                        target = name
                        break

                if not target:
                    target = self._combat_target

                if target:
                    self._combat_target = target
                    result = self.hp_tracker.record_damage(target, dmg)
                    if result:
                        idx, frac = result
                        logger.info(f"COMBAT: {dmg} dmg -> '{target}'[{idx}] (frac={frac}) | line: {line}")
                    else:
                        logger.info(f"COMBAT: {dmg} dmg -> '{target}' (untracked) | line: {line}")
                else:
                    logger.warning(f"COMBAT(no target): {line} | room_monsters={self._room_monsters}")

        # Get updated HP fractions for monsters in room (by index)
        monster_hp = self.hp_tracker.get_all_fractions()

        self._emit("combat", {
            "line": line,
            "monster_hp": monster_hp,
            "target": self._combat_target,
        })

    def _on_death(self):
        self._combat_target = None
        self._emit("death", {})

    def _on_xp(self, amount: int):
        self._emit("xp_gain", {"amount": amount})

    def _on_chat(self, sender: str, message: str, channel: str):
        self._emit("chat", {
            "sender": sender,
            "message": message,
            "channel": channel,
        })

    def _match_room_monster(self, name: str) -> str | None:
        """Match a name from combat text to a monster actually in the room.
        Handles abbreviations and case differences."""
        lower = name.strip().lower()
        # Exact match
        for m in self._room_monsters:
            if m.strip().lower() == lower:
                return m
        # Prefix match (MegaMud sends abbreviated names)
        for m in self._room_monsters:
            if m.strip().lower().startswith(lower):
                return m
        # Substring match
        for m in self._room_monsters:
            if lower in m.strip().lower():
                return m
        return None

    def _on_char_name(self, name: str):
        self._char_name = name
        self._emit("char_name", {"name": name})

    def _on_wound_status(self, monster_name: str, wound_level: str):
        """Handle wound status from looking at a monster — update HP estimate."""
        result = self.hp_tracker.estimate_from_wound(monster_name, wound_level)
        if result is not None:
            idx, frac = result
            monster_hp = self.hp_tracker.get_all_fractions()
            self._emit("combat", {
                "line": f"{monster_name} appears to be {wound_level}.",
                "monster_hp": monster_hp,
                "target": monster_name,
            })

    def _on_who_list(self, players: list[str]):
        self._emit("who_list", {"players": players})

    # MUD slot names → char panel slot IDs
    _SLOT_MAP = {
        "weapon hand": 1, "weapon": 1,
        "head": 2,
        "hands": 3,
        "finger": "4a",    # first ring goes to Ring 1
        "feet": 5,
        "arms": 6,
        "back": 7,
        "neck": 8,
        "legs": 9,
        "waist": 10,
        "torso": 11,
        "off hand": 12, "off-hand": 12, "offhand": 12, "shield": 12,
        "wrist": "14a",    # first wrist goes to Wrist 1
        "ears": 15,
        "worn": 16,
        "light": 17,
        "face": 19,
    }

    def _on_inventory(self, inv_data):
        """Handle inventory update from parser."""
        equipped = {}     # slot_id -> {name, key}
        carried = []      # [{name, key}]
        ring_count = 0
        wrist_count = 0

        for item in inv_data.items:
            name = item["name"]
            slot = item.get("slot", "")
            key = self.slop.get_entity_thumb_key(name)
            item_data = self.gamedata.get_item(name) if self.gamedata.is_loaded else None
            item_data = _enrich_item_data(item_data, self.gamedata)

            entry = {"name": name, "key": key, "item_data": item_data}

            if slot:
                slot_lower = slot.lower()
                slot_id = self._SLOT_MAP.get(slot_lower)

                # Handle dual ring/wrist slots
                if slot_lower == "finger":
                    ring_count += 1
                    slot_id = "4a" if ring_count <= 1 else "4b"
                elif slot_lower == "wrist":
                    wrist_count += 1
                    slot_id = "14a" if wrist_count <= 1 else "14b"

                if slot_id is not None:
                    equipped[str(slot_id)] = entry
                else:
                    carried.append(entry)
            else:
                carried.append(entry)

        logger.info(f"INVENTORY: {len(equipped)} equipped, {len(carried)} carried")
        for sid, e in equipped.items():
            logger.info(f"  slot {sid}: {e['name']} (key={e['key']})")
        for e in carried:
            logger.info(f"  carried: {e['name']}")

        self._emit("inventory", {
            "equipped": equipped,
            "carried": carried,
            "wealth": inv_data.wealth,
            "encumbrance": getattr(inv_data, 'encumbrance', ''),
        })

    def _on_pro(self, pro_data: dict):
        bc = pro_data.get("broadcast_channel", "")
        self._broadcast_channel = bc
        self._emit("pro_data", pro_data)

    # ── Helpers ──

    def _emit(self, event_type: str, data: dict):
        if self._loop and self.bus._clients:
            # Queue the message, don't await anything
            self._msg_queue.append((event_type, data))
            if not self._drain_scheduled:
                self._drain_scheduled = True
                self._loop.call_soon(self._schedule_drain)

    def _schedule_drain(self):
        self._drain_scheduled = False
        asyncio.ensure_future(self._drain_queue())

    async def _drain_queue(self):
        while self._msg_queue:
            event_type, data = self._msg_queue.popleft()
            try:
                await asyncio.wait_for(
                    self.bus.broadcast(event_type, data), timeout=1.0)
            except Exception:
                pass  # drop message rather than block proxy

    def get_state(self) -> dict:
        """Get current game state snapshot for newly connected clients."""
        room_image_key = self.slop.get_room_image_key(self._room_name, self._room_exits)
        depth_key = self.slop.get_depth_key(room_image_key) if room_image_key else None

        entities = []
        for idx, name in enumerate(self._room_monsters):
            key = self.slop.get_entity_thumb_key(name)
            hp_frac = self.hp_tracker.get_hp_fraction(idx)
            monster_data = self.gamedata.get_monster_stats(name) if self.gamedata.is_loaded else None
            drops = self.gamedata.get_monster_drops(name) if self.gamedata.is_loaded else None
            entities.append({
                "name": name, "key": key, "type": "npc",
                "hp_fraction": hp_frac,
                "stats": monster_data,
                "drops": drops or [],
            })

        items = []
        for name in self._room_items:
            currency = _detect_currency(name)
            key = None if currency else self.slop.get_entity_thumb_key(name)
            item_data = None if currency else (
                self.gamedata.get_item(name) if self.gamedata.is_loaded else None)
            item_data = _enrich_item_data(item_data, self.gamedata)
            entry = {
                "name": name, "key": key, "type": "item",
                "item_data": item_data,
                "quantity": self._room_item_quantities.get(name, 1),
            }
            if currency:
                entry["currency"] = currency
            items.append(entry)

        return {
            "room_name": self._room_name,
            "name": self._room_name,
            "description": self._room_desc,
            "exits": self._room_exits,
            "room_image_key": room_image_key,
            "depth_key": depth_key,
            "entities": entities,
            "items": items,
            "hp": self._hp,
            "max_hp": self._max_hp,
            "mana": self._mana,
            "max_mana": self._max_mana,
            "combat_target": self._combat_target,
            "char_name": self._char_name,
            "broadcast_channel": self._broadcast_channel,
            "connected": self.proxy.connected,
            "slop_stats": self.slop.get_stats(),
        }
