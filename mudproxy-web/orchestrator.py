"""Game state orchestrator — wires proxy + parser + entity DB + slop to event bus."""
import asyncio
import hashlib
import json
import logging
import re
import time
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
from mudproxy.entity_db import EntityDB, EntityInfo

logger = logging.getLogger(__name__)

# MajorMUD currency types — matched by substring in item name (lowercase)
_CURRENCY_TYPES = {
    "runic coin": "runic",
    "platinum piece": "platinum",
    "gold crown": "gold",
    "silver noble": "silver",
    "copper farthing": "copper",
}


# ── Entity movement patterns (from WCCMMUD DLL strings) ──
_DIRECTIONS = r'(?:north|south|east|west|northeast|northwest|southeast|southwest|above|below)'

# Enter patterns — what you see when someone arrives
_RE_ENTER = [
    re.compile(r'.+ walks into the room from the ' + _DIRECTIONS + r'[.!]?$', re.IGNORECASE),
    re.compile(r'.+ just arrived from the ' + _DIRECTIONS + r'[.!]?$', re.IGNORECASE),
    re.compile(r'.+ just arrived from nowhere[.!]?$', re.IGNORECASE),
    re.compile(r'.+ moves into the room from the ' + _DIRECTIONS + r'[.!]?$', re.IGNORECASE),
    re.compile(r'.+ wanders into the room[.!]?$', re.IGNORECASE),
    re.compile(r'.+ just entered the Realm[.!]?$', re.IGNORECASE),
    re.compile(r'You notice .+ sneak in from the ' + _DIRECTIONS + r'[.!]?$', re.IGNORECASE),
    re.compile(r'You notice .+ sneak in from (?:above|below)[.!]?$', re.IGNORECASE),
]

# Leave patterns — what you see when someone departs
_RE_LEAVE = [
    re.compile(r'.+ just left to the ' + _DIRECTIONS + r'[.!]?$', re.IGNORECASE),
    re.compile(r'.+ just left (?:upwards|downwards)[.!]?$', re.IGNORECASE),
    re.compile(r'.+ just left the Realm[.!]?$', re.IGNORECASE),
    re.compile(r'You notice .+ sneaking out to the ' + _DIRECTIONS + r'[.!]?$', re.IGNORECASE),
    re.compile(r'You notice .+ sneaking out (?:upwards|downwards)[.!]?$', re.IGNORECASE),
]

# Cooldown: don't spam Enter if multiple entities move at once
_ENTITY_MOVE_COOLDOWN = 0.5  # seconds


def _is_entity_movement(stripped: str) -> bool:
    """Return True if stripped line matches a known enter/leave pattern."""
    s = stripped.strip()
    for pat in _RE_ENTER:
        if pat.match(s):
            return True
    for pat in _RE_LEAVE:
        if pat.match(s):
            return True
    return False


# ── Ambient messages (from MajorMUD 1.11p core dats) ──
# These are flavor/sound messages that confuse MegaMud's parser.
# Exact match on stripped text — fast set lookup.
_AMBIENT_MESSAGES = frozenset({
    # Silvermere town sounds
    "A dog barks off in the distance.",
    "A cheer of many voices can be heard in the distance.",
    "A guardsman shouts out the time of day.",
    'A voice shouts aloud "Read the bulletin in the Adventurer\'s Guild!"',
    "Children rush past you hopping around in youthful glee.",
    "The awful sound of a drunken chorus echoes through the streets.",
    # Forest sounds
    "A dry twig snaps loudly behind you.",
    "A flock of birds fly overhead.",
    "An ominous wind blows through the trees.",
    "The forest becomes strangely silent.",
    "The leaves begin to rustle, as if some beast were about to spring forth!",
    # Blackwood forest sounds
    "A sudden burst of wind blows down upon you.",
    "Some great beast howls in the",
    "The trees creak towards you, almost with an awful yearning.",
    "A strange gurgling is heard close by.",
    "The bitter scent of evil is rich in the air.",
    "You hear faint whispering behind you.",
    # Desert sounds
    "The howl of some awful beast can be heard far over the dunes.",
    # Temple sounds
    "The angelic sound of a choir floats down through the air.",
    "The large temple bell clangs loudly, echoing the time of day.",
    # Dungeon/misc sounds
    "Doors on this level creak and thump!",
    "Something sinister seems to tickle your senses.",
})


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
        self.entity_db = EntityDB(on_look_complete=self._on_look_complete)

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
        self._online_players: list[str] = []
        self._self_looked = False
        self._last_bracket_player = None  # tracks "[ Name ]" bracket line for player detection
        self._player_desc_buf = None  # buffers wrapped description lines
        self._inv_sync_task: asyncio.Task | None = None
        self._last_inv_sync = 0
        self._last_entity_move = 0.0  # cooldown for entity movement Enter
        self.ambient_filter_enabled = True  # strip ambient sounds from MegaMud
        # Pro mode: "silent" (ParaMUD, invisible), "loud" (ParaMUD, visible), "off" (Legacy MMUD)
        self.pro_mode = "loud"
        self._stealthed = False  # True after "Attempting to sneak..." until broken
        self._hidden = False     # True after "Attempting to hide..." until broken
        self._last_room_update = 0.0  # timestamp of last on_room_update
        self._move_pro_task: asyncio.Task | None = None  # delayed pro for special exits
        self._room_count_since_pro = 0  # rooms visited since last pro
        self._pro_every_n_rooms = 5     # fire pro every N rooms
        self._spellbook: dict | None = None  # parsed from 'spells'/'powers' command

        # Room position tracking — ground truth from pro, predictions from MDB exits
        self._current_map: int | None = None
        self._current_room: int | None = None
        self._predicted_map: int | None = None
        self._predicted_room: int | None = None
        self._last_move_dir: str | None = None  # last movement direction (N/S/E/W etc)
        self._position_confirmed = False  # True after pro confirms position

        # Optional GUI callbacks (set by main_window for bidirectional sync)
        self.on_pro_mode_changed: callable | None = None
        self.on_ambient_filter_changed: callable | None = None

        self._wire_callbacks()

    def _wire_callbacks(self):
        """Connect proxy and parser callbacks to event bus emissions."""
        # Proxy → parser
        self.proxy.on_raw_line = self._on_raw_line
        self.proxy.on_server_data_bytes = self._on_server_data
        self.proxy.on_connect = self._on_connect
        self.proxy.on_disconnect = self._on_disconnect
        self.proxy.on_client_command = self._on_client_command
        # Line filter — suppress ambient messages from reaching MegaMud
        self.proxy.line_filter = self._line_filter
        self.proxy.filter_active = lambda: self.ambient_filter_enabled

        # Parser → events
        self.parser.on_room_update = self._on_room_update
        self.parser.on_hp_update = self._on_hp_update
        self.parser.on_combat = self._on_combat
        self.parser.on_death = self._on_death
        self.parser.on_xp = self._on_xp
        self.parser.on_chat = self._on_chat
        self.parser.on_char_name = self._on_char_name
        self.parser.on_who_list = self._on_who_list
        self.parser.on_top_list = self._on_top_list
        self.parser.on_wound_status = self._on_wound_status
        self.parser.on_pro = self._on_pro
        self.parser.on_stat_advanced = self._on_stat_advanced
        self.parser.on_inventory = self._on_inventory
        self.parser.on_spellbook = self._on_spellbook

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

    async def inject_raw(self, data: bytes):
        await self.proxy.inject_raw(data)

    async def ghost_command(self, ghost_name: str, command: str):
        """Inject a fake telepath from the ghost character into MegaMud's stream."""
        # Set up interception for this ghost name
        self.proxy.set_ghost_name(ghost_name)
        self.proxy.set_ghost_callback(lambda resp: self._on_ghost_response(ghost_name, resp))

        # Match real telepath ANSI: green sender, white message
        # Include HP prompt — MegaMud needs it to "commit" the text block to its parser
        fake_line = f"\x1b[0;32m{ghost_name} telepaths: \x1b[0;37m{command}"
        await self.proxy.inject_to_client(fake_line, hp=self._hp, mana=self._mana)
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

    def _line_filter(self, stripped: str) -> bool:
        """Return True to suppress this line from MegaMud. Used for ambient messages."""
        return stripped.strip() in _AMBIENT_MESSAGES

    def _on_raw_line(self, stripped: str, ansi: str):
        try:
            self.parser.feed_line(stripped, ansi)
        except Exception as e:
            logger.error(f"Parser error (non-fatal): {e}")

        # ── Player detection from look output ──
        s = stripped.strip()
        # "[ Eldritch Palmer ](Stoneheart Group)" = player bracket line
        if s.startswith('[ ') and ']' in s:
            bracket_m = re.match(r'^\[\s*(.+?)\s*\]', s)
            if bracket_m:
                self._last_bracket_player = bracket_m.group(1)
                self._player_desc_buf = None
        # Buffer description lines after bracket (may wrap across 2+ lines)
        elif self._last_bracket_player:
            if self._player_desc_buf is None and ' is a' in s:
                # First description line — check if complete (has "eyes")
                if re.search(r'\beyes\b', s, re.IGNORECASE):
                    self._try_parse_player_inline(self._last_bracket_player, s)
                    self._last_bracket_player = None
                else:
                    self._player_desc_buf = s
            elif self._player_desc_buf is not None:
                # Continuation line — join and try parse
                joined = self._player_desc_buf + ' ' + s
                self._try_parse_player_inline(self._last_bracket_player, joined)
                self._last_bracket_player = None
                self._player_desc_buf = None

        # ── Stealth tracking ──
        if s == 'Attempting to sneak...':
            self._stealthed = True
        elif s == 'Attempting to hide...':
            self._hidden = True
        elif s.startswith('Attempting to hide...') and len(s) > len('Attempting to hide...'):
            self._hidden = False  # failed hide attempt
        elif s == 'You are no longer sneaking.' or s.startswith("You don't think you are sneaking"):
            self._stealthed = False
        elif s == 'There is no exit in that direction!':
            self._stealthed = False
            self._hidden = False

        # Detect receiving items from other players
        low = s.lower()
        if 'just gave you' in low or 'you just received' in low:
            self._schedule_inv_sync(delay=2)

        # ── Teleport confirmation (special exit messages from dats) ──
        if self.gamedata.is_loaded and s:
            tp = self.gamedata.check_teleport_message(
                s, self._current_map, self._current_room)
            if tp:
                self._last_move_dir = 'teleport'
                self._predicted_map = tp['dst_map']
                self._predicted_room = tp['dst_room']
                dst_name = self.gamedata.get_room_name(tp['dst_map'], tp['dst_room']) or '???'
                logger.info(
                    f"Teleport confirmed: \"{s[:60]}\" -> "
                    f"Map {tp['dst_map']}/Room {tp['dst_room']} ({dst_name})"
                )
                self._emit("teleport", {
                    "message": s,
                    "dst_map": tp['dst_map'],
                    "dst_room": tp['dst_room'],
                    "dst_name": dst_name,
                })

        # Entity enter/leave → send Enter to refresh MegaMud's room view
        if _is_entity_movement(stripped):
            now = time.monotonic()
            if now - self._last_entity_move >= _ENTITY_MOVE_COOLDOWN:
                self._last_entity_move = now
                logger.debug(f"Entity movement: {stripped.strip()}")
                asyncio.ensure_future(self.proxy.inject_command(""))

    def _on_server_data(self, data: bytes):
        """Forward raw server bytes to the web terminal for full ANSI emulation."""
        try:
            text = data.decode('utf-8', errors='replace')
            self._emit("raw_data", {"data": text})
        except Exception:
            pass

    def _on_connect(self):
        self._emit("connection", {"status": "connected"})
        self._in_game = False  # wait for first HP prompt before auto-injecting
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

    def _fire_pro(self):
        """Inject pro command based on current pro_mode setting."""
        if self.pro_mode == "off" or not self.proxy.connected:
            return
        if self.pro_mode == "silent":
            if self.proxy._suppressing_pro:
                return
            # Discard any partial HP prompt sitting in the line filter
            # buffer — it will get re-sent by the server naturally.
            # If we leave it, it gets flushed later as a duplicate.
            self.proxy._filter_ansi_buffer = ''
            self.proxy._suppressing_pro = True
            self.proxy._pro_suppress_buf = b''
            self.proxy._pro_suppress_start = time.monotonic()
            asyncio.ensure_future(self.proxy.inject_command("pro"))
        elif self.pro_mode == "loud":
            self.proxy._web_hiding_pro = True
            asyncio.ensure_future(self.proxy.inject_command("pro"))

    _TOP_CACHE_FILE = Path.home() / ".cache" / "mudproxy" / "top_list_last.json"
    _TOP_STALE_DAYS = 3

    def _maybe_schedule_top(self):
        """Schedule 'top 1000' 20s after entering game if stale or never run."""
        try:
            if self._TOP_CACHE_FILE.exists():
                data = json.loads(self._TOP_CACHE_FILE.read_text())
                last = data.get("timestamp", 0)
                age_days = (time.time() - last) / 86400
                if age_days < self._TOP_STALE_DAYS:
                    logger.info(f"Top list is {age_days:.1f} days old, skipping auto-top")
                    return
        except Exception:
            pass
        logger.info("Scheduling auto 'top 1000' in 20s")
        asyncio.ensure_future(self._delayed_top())

    async def _delayed_top(self):
        await asyncio.sleep(20)
        if self._in_game and self.proxy.connected:
            logger.info("Sending auto 'top 1000'")
            await self.proxy.inject_command("top 1000")

    async def _delayed_who(self):
        """Send 'who' to update online player list."""
        if hasattr(self, '_who_pending') and self._who_pending:
            return
        self._who_pending = True
        await asyncio.sleep(3)
        if self._in_game and self.proxy.connected:
            await self.proxy.inject_command("who")
        self._who_pending = False

    async def _delayed_self_look(self, name: str):
        """Look at ourselves once to populate own portrait data."""
        await asyncio.sleep(2)
        if self._in_game and self.proxy.connected:
            logger.info(f"Auto-looking at self: {name}")
            self.entity_db.start_look(name, "player")
            await self.proxy.inject_command(f"look {name}")

    def _on_disconnect(self):
        self._emit("connection", {"status": "disconnected"})
        self._combat_target = None
        if self._inv_sync_task:
            self._inv_sync_task.cancel()
            self._inv_sync_task = None
        # Clear any stuck pro suppression
        self.proxy._suppressing_pro = False

    # Movement commands that could transition rooms (including special exit verbs)
    _MOVE_CMDS = frozenset({
        'n', 's', 'e', 'w', 'ne', 'nw', 'se', 'sw', 'u', 'd',
        'north', 'south', 'east', 'west', 'northeast', 'northwest',
        'southeast', 'southwest', 'up', 'down',
        'go', 'enter', 'leave', 'climb', 'jump', 'swim', 'crawl',
        'open', 'push', 'pull', 'touch', 'move',
    })

    def _on_client_command(self, cmd: str):
        lower = cmd.strip().lower()
        first_word = lower.split()[0] if lower else ''

        # Track attack commands for combat target
        if lower.startswith(("a ", "at ", "att ", "attack ", "ba ", "back ", "backstab ", "k ", "kill ")):
            self._stealthed = False
            self._hidden = False
            parts = cmd.strip().split(None, 1)
            if len(parts) > 1:
                self._combat_target = parts[1]

        # Break — exits combat and breaks stealth/hide
        if lower in ("break", "br"):
            self._stealthed = False
            self._hidden = False

        # Movement command — track direction and predict destination
        if first_word in self._MOVE_CMDS and self._in_game:
            move_dir = self._normalize_direction(first_word)
            if move_dir:
                self._last_move_dir = move_dir
                self._predict_destination(move_dir)
            if self.pro_mode != "off":
                self._schedule_move_pro()

        # Inventory-changing commands → sync after a short delay
        if lower.startswith(("g ", "get ", "dr ", "drop ", "w ", "wea ", "wear ",
                             "rem ", "remove ", "sell ", "buy ", "give ")):
            self._schedule_inv_sync(delay=2)

    def _schedule_move_pro(self):
        """After a movement command, wait 1.5s — if no room_update fires, pro anyway."""
        if self._move_pro_task and not self._move_pro_task.done():
            self._move_pro_task.cancel()
        if self._loop:
            self._move_pro_task = self._loop.create_task(self._delayed_move_pro())

    async def _delayed_move_pro(self):
        """Fire pro if no room update arrived within 1.5s of a movement command."""
        before = self._last_room_update
        await asyncio.sleep(1.5)
        # If no room_update happened since the movement command, fire pro
        if self._last_room_update == before and self._in_game and self.proxy.connected:
            logger.debug("No room update after movement — firing fallback pro")
            self._fire_pro()

    # ── Room position tracking ──

    _DIR_MAP = {
        'n': 'N', 'north': 'N', 's': 'S', 'south': 'S',
        'e': 'E', 'east': 'E', 'w': 'W', 'west': 'W',
        'ne': 'NE', 'northeast': 'NE', 'nw': 'NW', 'northwest': 'NW',
        'se': 'SE', 'southeast': 'SE', 'sw': 'SW', 'southwest': 'SW',
        'u': 'U', 'up': 'U', 'd': 'D', 'down': 'D',
    }

    def _normalize_direction(self, cmd: str) -> str | None:
        """Convert movement command to MDB exit key (N/S/E/W/NE/etc)."""
        return self._DIR_MAP.get(cmd.lower())

    def _predict_destination(self, direction: str):
        """Use MDB room exits to predict where this move leads."""
        if self._current_map is None or self._current_room is None:
            self._predicted_map = None
            self._predicted_room = None
            return

        room = self.gamedata.get_room(self._current_map, self._current_room)
        if not room or 'exits' not in room:
            self._predicted_map = None
            self._predicted_room = None
            logger.debug(f"No MDB room data for Map {self._current_map}/Room {self._current_room}")
            return

        exits = room['exits']
        dest = exits.get(direction)
        if not dest:
            self._predicted_map = None
            self._predicted_room = None
            logger.debug(f"No {direction} exit from Map {self._current_map}/Room {self._current_room}")
            return

        # Parse "map/room" or "map/room (Door)" format
        try:
            dest_clean = dest.split()[0]  # strip "(Door)" etc
            parts = dest_clean.split('/')
            self._predicted_map = int(parts[0])
            self._predicted_room = int(parts[1])
            pred_room_data = self.gamedata.get_room(self._predicted_map, self._predicted_room)
            pred_name = pred_room_data['Name'] if pred_room_data else '???'
            logger.info(
                f"Walked {direction} → Expected Map {self._predicted_map}/"
                f"Room {self._predicted_room} ({pred_name})"
            )
            self._emit("room_prediction", {
                "direction": direction,
                "predicted_map": self._predicted_map,
                "predicted_room": self._predicted_room,
                "predicted_name": pred_name,
                "from_map": self._current_map,
                "from_room": self._current_room,
            })
        except (ValueError, IndexError):
            self._predicted_map = None
            self._predicted_room = None
            logger.warning(f"Bad exit format: {dest!r}")

    def _on_pro_position(self, pro_data: dict):
        """Update position from pro and check against prediction."""
        map_num = pro_data.get('map_num')
        room_num = pro_data.get('room_num')
        if map_num is None or room_num is None:
            return

        old_map, old_room = self._current_map, self._current_room
        self._current_map = map_num
        self._current_room = room_num
        self._position_confirmed = True

        room_data = self.gamedata.get_room(map_num, room_num)
        room_name = room_data['Name'] if room_data else '???'

        # Check prediction
        if self._predicted_map is not None and self._predicted_room is not None:
            if self._predicted_map == map_num and self._predicted_room == room_num:
                logger.info(f"✓ Position MATCHED: Map {map_num}/Room {room_num} ({room_name})")
                match_status = "matched"
            else:
                pred_name = '???'
                pred_room = self.gamedata.get_room(self._predicted_map, self._predicted_room)
                if pred_room:
                    pred_name = pred_room['Name']
                logger.warning(
                    f"✗ Position MISMATCH: Expected Map {self._predicted_map}/"
                    f"Room {self._predicted_room} ({pred_name}), "
                    f"Actual Map {map_num}/Room {room_num} ({room_name})"
                )
                match_status = "mismatch"
        else:
            logger.info(f"Pro position: Map {map_num}/Room {room_num} ({room_name})")
            match_status = "no_prediction"

        self._predicted_map = None
        self._predicted_room = None

        self._emit("room_position", {
            "map": map_num,
            "room": room_num,
            "name": room_name,
            "match": match_status,
            "exits": room_data.get('exits', {}) if room_data else {},
        })

    # ── Parser callbacks ──

    def _on_room_update(self, room):
        old_room = self._room_name
        self._room_name = room.name
        self._room_desc = room.description
        self._room_exits = room.exits
        self._room_monsters = room.monsters
        self._room_items = room.items
        self._room_item_quantities = getattr(room, 'item_quantities', {})

        # Mark room update timestamp — cancels the movement fallback pro
        self._last_room_update = time.monotonic()
        if self._move_pro_task and not self._move_pro_task.done():
            self._move_pro_task.cancel()
            self._move_pro_task = None

        # Dead reckoning — if we predicted a destination, accept it as current
        # position between pro verifications
        if self._predicted_map is not None and self._predicted_room is not None:
            self._current_map = self._predicted_map
            self._current_room = self._predicted_room
            pred_room = self.gamedata.get_room(self._current_map, self._current_room)
            pred_name = pred_room['Name'] if pred_room else '???'
            # Verify room name matches what parser saw
            if pred_room and pred_room['Name'].lower() == room.name.lower():
                logger.info(
                    f"Dead reckoning: Map {self._current_map}/Room {self._current_room} "
                    f"({pred_name}) — name matches"
                )
            else:
                logger.warning(
                    f"Dead reckoning: Map {self._current_map}/Room {self._current_room} "
                    f"({pred_name}) — name MISMATCH (saw: {room.name!r})"
                )
            self._predicted_map = None
            self._predicted_room = None
            self._emit("room_position", {
                "map": self._current_map,
                "room": self._current_room,
                "name": pred_name,
                "match": "dead_reckoning",
                "exits": pred_room.get('exits', {}) if pred_room else {},
            })

        # Only count as a real move if we had a pending movement direction
        if self._last_move_dir is not None:
            self._last_move_dir = None
            self._room_count_since_pro += 1
            if self._in_game and self.proxy.connected and self._room_count_since_pro >= self._pro_every_n_rooms:
                self._room_count_since_pro = 0
                self._fire_pro()

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
            etype = "npc"
            # Check if this is a known player
            _pfx, base_name = self.entity_db.parse_prefix_and_base(name)
            entity_info = self.entity_db.get_entity(base_name)
            if entity_info and entity_info.entity_type == "player":
                etype = "player"
                if entity_info.portrait_key and self.slop.has_asset(entity_info.portrait_key):
                    key = entity_info.portrait_key
            elif self.entity_db.is_known_player(name):
                etype = "player"
            elif not monster_data and self.gamedata.is_loaded:
                # Not in monster database = assumed player
                etype = "player"
            entities.append({
                "name": name,
                "key": key,
                "type": etype,
                "hp_fraction": hp_frac,
                "stats": monster_data,
                "drops": drops or [],
            })

        # If we found unknown players, schedule a who to confirm
        unknown_players = [e for e in entities if e["type"] == "player"
                           and not self.entity_db.is_known_player(e["name"])]
        if unknown_players:
            asyncio.ensure_future(self._delayed_who())

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
        # First HP prompt means we're in-game — fire initial pro
        if not self._in_game:
            self._in_game = True
            self._fire_pro()
            self._maybe_schedule_top()
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
        # Auto-look at ourselves once to populate our own portrait
        if not self._self_looked:
            first_name = name.split()[0]
            entity_info = self.entity_db.get_entity(first_name)
            if not entity_info or not entity_info.portrait_key:
                self._self_looked = True
                asyncio.ensure_future(self._delayed_self_look(first_name))

    def _try_parse_player_inline(self, full_name: str, desc_line: str):
        """Parse player race/class/gender directly from description line in the stream."""
        first_name = full_name.split()[0]
        info = self.entity_db.get_entity(first_name)
        if info is None:
            info = EntityInfo(name=first_name, entity_type="player", full_name=full_name)
            self.entity_db._entities[first_name.lower()] = info
        info.entity_type = "player"
        info.full_name = full_name
        info.description = desc_line
        self.entity_db._parse_player_description(info)
        if info.race and info.class_name and info.gender:
            self.entity_db._save_entity(info)
            self.entity_db._known_players.add(first_name)
            logger.info(f"Inline player parse: {first_name} = {info.gender} {info.race} {info.class_name} -> {info.portrait_key}")
            # If this is our own char, emit portrait key
            my_first = self._char_name.split()[0] if self._char_name else ""
            if my_first and first_name.lower() == my_first.lower() and info.portrait_key:
                self._emit("char_portrait", {"key": info.portrait_key})

    def _on_look_complete(self, target: str, info):
        """Called when a look response is fully parsed for any entity."""
        if info.entity_type == "player" and info.portrait_key:
            # If this is our own character, send portrait key to char panel
            first_name = self._char_name.split()[0] if self._char_name else ""
            if first_name and target.lower().startswith(first_name.lower()):
                self._emit("char_portrait", {"key": info.portrait_key})

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
        self.entity_db.update_known_players(players)
        self._online_players = players
        self._emit("who_list", {"players": players})

    def _on_top_list(self, players: list[tuple[str, str]]):
        self.entity_db.update_players_from_top(players)
        # Save timestamp so we don't re-fetch for a few days
        try:
            self._TOP_CACHE_FILE.parent.mkdir(parents=True, exist_ok=True)
            self._TOP_CACHE_FILE.write_text(json.dumps({
                "timestamp": time.time(),
                "count": len(players),
            }))
        except Exception as e:
            logger.warning(f"Failed to save top list timestamp: {e}")
        self._emit("top_list", {"players": [
            {"name": name, "class": cls} for name, cls in players
        ]})

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
        # Update room position tracking from pro
        self._on_pro_position(pro_data)

    def _on_stat_advanced(self, data: dict):
        self._emit("stat_advanced", data)

    def _on_spellbook(self, data: dict):
        """Player typed 'spells' or 'powers' — store and broadcast their spellbook."""
        self._spellbook = data
        logger.info(f"Spellbook parsed: {data['type']}, {len(data['spells'])} entries")
        self._emit("spellbook", data)

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
            etype = "npc"
            _pfx, base_name = self.entity_db.parse_prefix_and_base(name)
            entity_info = self.entity_db.get_entity(base_name)
            if entity_info and entity_info.entity_type == "player":
                etype = "player"
                if entity_info.portrait_key and self.slop.has_asset(entity_info.portrait_key):
                    key = entity_info.portrait_key
            elif self.entity_db.is_known_player(name):
                etype = "player"
            elif not monster_data and self.gamedata.is_loaded:
                etype = "player"
            entities.append({
                "name": name, "key": key, "type": etype,
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
            "ambient_filter_enabled": self.ambient_filter_enabled,
            "pro_mode": self.pro_mode,
            "position": {
                "map": self._current_map,
                "room": self._current_room,
                "confirmed": self._position_confirmed,
            } if self._current_map is not None else None,
            "online_players": self._online_players,
        }
