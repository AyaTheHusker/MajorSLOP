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

    async def start(self):
        """Initialize and load data, start TCP proxy listener for MegaMud."""
        self._loop = asyncio.get_event_loop()
        self.gamedata.load()
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

    # ── Proxy callbacks ──

    def _on_raw_line(self, stripped: str, ansi: str):
        try:
            self.parser.feed_line(stripped, ansi)
        except Exception as e:
            logger.error(f"Parser error (non-fatal): {e}")

    def _on_connect(self):
        self._emit("connection", {"status": "connected"})

    def _on_disconnect(self):
        self._emit("connection", {"status": "disconnected"})
        self._combat_target = None

    def _on_client_command(self, cmd: str):
        # Track attack commands for combat target
        lower = cmd.strip().lower()
        if lower.startswith(("a ", "at ", "att ", "attack ", "ba ", "back ", "backstab ", "k ", "kill ")):
            parts = cmd.strip().split(None, 1)
            if len(parts) > 1:
                self._combat_target = parts[1]

    # ── Parser callbacks ──

    def _on_room_update(self, room):
        self._room_name = room.name
        self._room_desc = room.description
        self._room_exits = room.exits
        self._room_monsters = room.monsters
        self._room_items = room.items
        self._room_item_quantities = getattr(room, 'item_quantities', {})

        # Clear HP tracker on room change and pre-initialize monsters from gamedata
        self.hp_tracker.on_room_change()
        for name in room.monsters:
            self.hp_tracker._get_or_create(name)

        # Look up room image
        room_image_key = self.slop.get_room_image_key(room.name, room.exits)
        depth_key = self.slop.get_depth_key(room_image_key) if room_image_key else None

        # Build entity thumbnail list
        entities = []
        for name in room.monsters:
            key = self.slop.get_entity_thumb_key(name)
            hp_frac = self.hp_tracker.get_hp_fraction(name)
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
        # HP tracker uses record_damage, not process_combat_line
        import re
        dmg_match = re.search(r'for (\d+) damage', line)
        if dmg_match and self._combat_target:
            self.hp_tracker.record_damage(self._combat_target, int(dmg_match.group(1)))

        # Get updated HP fractions for monsters in room
        monster_hp = {}
        for name in self._room_monsters:
            frac = self.hp_tracker.get_hp_fraction(name)
            if frac is not None:
                monster_hp[name] = frac

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

    def _on_char_name(self, name: str):
        self._char_name = name
        self._emit("char_name", {"name": name})

    def _on_who_list(self, players: list[str]):
        self._emit("who_list", {"players": players})

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
        for name in self._room_monsters:
            key = self.slop.get_entity_thumb_key(name)
            hp_frac = self.hp_tracker.get_hp_fraction(name)
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
            "connected": self.proxy.connected,
            "slop_stats": self.slop.get_stats(),
        }
