"""Game data module — loads exported MajorMUD database (monsters, items, rooms, spells).

Data files live in ~/.cache/mudproxy/gamedata/ as JSON, exported from MDB via scripts/export_gamedata.py.
Provides fast lookup by name or number for entity validation, monster stats, drop tables, etc.
"""
import json
import logging
from pathlib import Path

logger = logging.getLogger(__name__)

from .paths import default_gamedata_dir
GAMEDATA_DIR = default_gamedata_dir()

# MajorMUD random spawn prefixes — only these are applied by the server
# at spawn time and won't appear in the monster database
_CREATURE_PREFIXES = {
    "large", "small", "big", "short", "tall", "fat", "fierce", "nasty", "thin",
    "angry", "happy", "old",
}


def _strip_prefix(name: str) -> str:
    """Strip common creature prefixes: 'a fat Orc Rogue' -> 'Orc Rogue'."""
    words = name.strip().split()
    while len(words) > 1 and words[0].lower() in _CREATURE_PREFIXES:
        words.pop(0)
    return " ".join(words)


class GameData:
    def __init__(self, data_dir: Path = GAMEDATA_DIR):
        self._data_dir = data_dir
        self._monsters: dict[int, dict] = {}
        self._items: dict[int, dict] = {}
        self._rooms: dict[str, dict] = {}
        self._spells: dict[int, dict] = {}
        self._lairs: dict[str, dict] = {}
        self._classes: dict[int, dict] = {}
        self._races: dict[int, dict] = {}
        self._shops: dict[int, dict] = {}
        self._monster_by_name: dict[str, int | list[int]] = {}  # name -> number or [numbers]
        self._item_by_name: dict[str, int] = {}
        self._teleport_messages: dict[str, list[dict]] = {}  # "You climb..." -> [{src/dst}]
        self._messages: dict[int, dict] = {}  # msg_num -> {you, others, dest}
        self._loaded = False
        # Current player location for disambiguating monsters with same name
        self.current_map: int | None = None
        self.current_room: int | None = None

    def load(self) -> bool:
        """Load all gamedata JSON files. Returns True if data was loaded."""
        if not self._data_dir.exists():
            logger.warning(f"Gamedata directory not found: {self._data_dir}")
            return False

        # Clear existing data for clean reload
        self._monsters.clear()
        self._items.clear()
        self._rooms.clear()
        self._spells.clear()
        self._lairs.clear()
        self._classes.clear()
        self._races.clear()
        self._shops.clear()
        self._monster_by_name.clear()
        self._item_by_name.clear()
        self._teleport_messages.clear()
        self._messages.clear()
        if hasattr(self, '_map_mob_cache'):
            self._map_mob_cache.clear()
        self._loaded = False

        loaded_any = False
        for name, target, key_type in [
            ("monsters.json", "_monsters", int),
            ("items.json", "_items", int),
            ("rooms.json", "_rooms", str),
            ("spells.json", "_spells", int),
            ("lairs.json", "_lairs", str),
            ("classes.json", "_classes", int),
            ("races.json", "_races", int),
            ("shops.json", "_shops", int),
        ]:
            path = self._data_dir / name
            if path.exists():
                try:
                    raw = json.loads(path.read_text())
                    if key_type == int:
                        setattr(self, target, {int(k): v for k, v in raw.items()})
                    else:
                        setattr(self, target, raw)
                    loaded_any = True
                    logger.info(f"Loaded {len(raw)} entries from {name}")
                except Exception as e:
                    logger.error(f"Failed to load {name}: {e}")

        # Name lookup tables
        for name in ["monster_names.json", "item_names.json"]:
            path = self._data_dir / name
            if path.exists():
                try:
                    raw = json.loads(path.read_text())
                    if "monster" in name:
                        self._monster_by_name = {k.lower(): v for k, v in raw.items()}
                    else:
                        self._item_by_name = {k.lower(): v for k, v in raw.items()}
                except Exception as e:
                    logger.error(f"Failed to load {name}: {e}")

        # Load teleport messages (special exit confirmation texts)
        tp_path = self._data_dir / "teleport_messages.json"
        if tp_path.exists():
            try:
                self._teleport_messages = json.loads(tp_path.read_text())
                logger.info(f"Loaded {len(self._teleport_messages)} teleport message texts")
            except Exception as e:
                logger.error(f"Failed to load teleport_messages.json: {e}")

        # Load all messages (for spell expiry, combat, etc.)
        msg_path = self._data_dir / "messages.json"
        if msg_path.exists():
            try:
                raw_msgs = json.loads(msg_path.read_text())
                self._messages = {int(k): v for k, v in raw_msgs.items()}
                logger.info(f"Loaded {len(self._messages)} messages from messages.json")
            except Exception as e:
                logger.error(f"Failed to load messages.json: {e}")

        self._loaded = loaded_any
        return loaded_any

    @property
    def is_loaded(self) -> bool:
        return self._loaded

    # ── Monster lookups ──

    def _resolve_monster_nums(self, name: str) -> list[int]:
        """Get all monster numbers matching a name.
        Tries exact name first, then with prefixes stripped one at a time."""
        name_lower = name.strip().lower()
        # Try exact match first (e.g. "ancient dragon" is a real monster)
        val = self._monster_by_name.get(name_lower)
        if val is not None:
            return val if isinstance(val, list) else [val]
        # Try stripping prefixes one at a time
        words = name_lower.split()
        while len(words) > 1 and words[0] in _CREATURE_PREFIXES:
            words.pop(0)
            attempt = " ".join(words)
            val = self._monster_by_name.get(attempt)
            if val is not None:
                return val if isinstance(val, list) else [val]
        return []

    def _parse_lair_mob_nums(self, lair_str: str) -> set[int]:
        """Parse monster numbers from a lair string like '(Max 2): 1141,2175,[5-6-8-2]'."""
        import re
        mob_nums = set()
        for part in lair_str.split(","):
            part = part.strip()
            if part.startswith("[") or part.startswith("("):
                continue
            m = re.match(r"(\d+)", part)
            if m:
                mob_nums.add(int(m.group(1)))
        return mob_nums

    def _get_map_monster_nums(self) -> set[int] | None:
        """Get all monster numbers that can spawn anywhere on the current map.
        Monsters wander, so we check all rooms on the same map, not just the current one."""
        if self.current_map is None:
            return None
        if not hasattr(self, '_map_mob_cache'):
            self._map_mob_cache: dict[int, set[int]] = {}
        if self.current_map in self._map_mob_cache:
            return self._map_mob_cache[self.current_map]

        mob_nums = set()
        prefix = f"{self.current_map}-"
        for key, room in self._rooms.items():
            if key.startswith(prefix):
                lair_str = room.get("Lair", "")
                if lair_str:
                    mob_nums.update(self._parse_lair_mob_nums(lair_str))

        if mob_nums:
            self._map_mob_cache[self.current_map] = mob_nums
            logger.info(f"Map {self.current_map}: {len(mob_nums)} possible monsters from lairs")
        return mob_nums if mob_nums else None

    def get_monster(self, name: str) -> dict | None:
        """Look up monster by name (case-insensitive, prefix-stripped).
        If multiple monsters share the same name, uses current room's lair data to disambiguate."""
        nums = self._resolve_monster_nums(name)
        if not nums:
            return None
        if len(nums) == 1:
            return self._monsters.get(nums[0])

        # Multiple matches — disambiguate by what can spawn on this map
        map_mobs = self._get_map_monster_nums()
        if map_mobs:
            candidates = [n for n in nums if n in map_mobs]
            if len(candidates) == 1:
                return self._monsters.get(candidates[0])
            # Still ambiguous — use candidates if any, otherwise all
            if candidates:
                nums = candidates

        # Fallback: return the one with highest HP (most likely the "real" version)
        best = None
        for num in nums:
            m = self._monsters.get(num)
            if m and (best is None or m.get("HP", 0) > best.get("HP", 0)):
                best = m
        return best

    def is_valid_monster(self, name: str) -> bool:
        """Check if a monster name exists in the database."""
        return len(self._resolve_monster_nums(name)) > 0

    def get_monster_hp(self, name: str) -> int | None:
        """Get monster's max HP."""
        m = self.get_monster(name)
        return m["HP"] if m else None

    def get_monster_hp_regen(self, name: str) -> int:
        """Get monster's HP regen per tick."""
        m = self.get_monster(name)
        return m.get("HPRegen", 0) if m else 0

    # Monster type IDs from MajorMUD
    MONSTER_TYPES = {
        0: "Normal", 1: "Humanoid", 2: "Humanoid", 3: "Unique/Boss",
    }

    def get_monster_stats(self, name: str) -> dict | None:
        """Get a summary of monster combat stats for display."""
        m = self.get_monster(name)
        if not m:
            return None
        return {
            "name": m["Name"],
            "hp": m["HP"],
            "exp": m["EXP"],
            "ac": m["ArmourClass"],
            "dr": m["DamageResist"],
            "mr": m["MagicRes"],
            "bs_def": m.get("BSDefense", 0),
            "cash": {k: m.get(f"Cash{k}", 0) for k in "RPGSC"},
            "regen": m.get("HPRegen", 0),
            "regen_time": m.get("RegenTime", 0),
            "undead": bool(m.get("Undead", 0)),
            "align": m.get("Align", 0),
            "mob_type": self.MONSTER_TYPES.get(m.get("Type", 0), ""),
            "follow_pct": m.get("Follow%", 0),
            "energy": m.get("Energy", 0),
            "avg_dmg": m.get("AvgDmg", 0),
            "attacks": m.get("attacks", []),
        }

    def get_monster_drops(self, name: str) -> list[dict]:
        """Get monster drop table with item names resolved.
        Returns [{name, chance, item_num, key}, ...]"""
        m = self.get_monster(name)
        if not m:
            return []
        drops = []
        for drop in m.get("drops", []):
            item_num = drop.get("DropItem") or drop.get("item")
            chance = drop.get("DropItem%") or drop.get("chance", 0)
            if not item_num or chance <= 0:
                continue
            item = self._items.get(item_num)
            item_name = item["Name"] if item else f"Item #{item_num}"
            drops.append({
                "name": item_name,
                "chance": chance,
                "item_num": item_num,
                "key": item_name.strip().lower(),
            })
        return drops

    # ── Item lookups ──

    def get_item(self, name_or_num) -> dict | None:
        """Look up item by name (str) or number (int)."""
        if isinstance(name_or_num, int):
            return self._items.get(name_or_num)
        name_lower = str(name_or_num).strip().lower()
        num = self._item_by_name.get(name_lower)
        if num is not None:
            return self._items.get(num)
        return None

    def is_valid_item(self, name: str) -> bool:
        """Check if an item name exists in the database."""
        return self.get_item(name) is not None

    # ── Room lookups ──

    def get_room(self, map_num: int, room_num: int) -> dict | None:
        """Look up room by map and room number."""
        key = f"{map_num}-{room_num}"
        return self._rooms.get(key)

    def get_room_name(self, map_num: int, room_num: int) -> str | None:
        """Get room name by map/room number."""
        room = self.get_room(map_num, room_num)
        return room["Name"] if room else None

    def check_teleport_message(self, text: str, current_map: int | None = None,
                                current_room: int | None = None) -> dict | None:
        """Check if a server line matches a teleport confirmation message.
        Returns {src_map, src_room, dst_map, dst_room, keyword} or None."""
        text = text.strip()
        entries = self._teleport_messages.get(text)
        if not entries:
            return None
        # If we know current position, narrow to matching source room
        if current_map is not None and current_room is not None:
            for e in entries:
                if e['src_map'] == current_map and e['src_room'] == current_room:
                    return e
        # Return first match if position unknown
        return entries[0] if entries else None

    def get_message(self, num: int) -> dict | None:
        """Get message by number. Returns {you, others, dest} or None."""
        return self._messages.get(num)

    # ── Shop lookups ──

    def get_shop_name(self, num: int) -> str | None:
        """Get shop name by number."""
        shop = self._shops.get(num)
        return shop["Name"] if shop else None

    def resolve_shop_names(self, obtained_from: str) -> str:
        """Replace 'Shop #N' and 'Shop(sell) #N' with resolved names."""
        import re
        def _replace(m):
            num = int(m.group(2))
            name = self.get_shop_name(num)
            if name:
                return f"{m.group(0)} ({name})"
            return m.group(0)
        return re.sub(r'Shop(\([^)]*\))?\s*#(\d+)', _replace, obtained_from)

    # ── Spell lookups ──

    def get_spell(self, num: int) -> dict | None:
        return self._spells.get(num)

    def get_heal_spell_names(self) -> set[str]:
        """Return lowercase names of all spells with Ability 18 (heal flag)."""
        names = set()
        for spell in self._spells.values():
            if any(a.get('Abil') == 18 for a in spell.get('abilities', [])):
                names.add(spell['Name'].lower())
        return names

    # ── Validation ──

    def is_valid_entity(self, name: str, entity_type: str = "creature") -> bool:
        """Check if an entity name is valid in the game database.
        Returns True if found, or True if we have no data (permissive fallback)."""
        if not self._loaded:
            return True  # no data loaded, allow everything
        if entity_type == "item":
            return self.is_valid_item(name)
        return self.is_valid_monster(name)
