"""Entity database — stores descriptions and thumbnail images for creatures, NPCs, players, and items.

Persists to ~/.cache/mudproxy/entities/ so we don't re-look or re-generate across sessions.
"""
import hashlib
import json
import logging
import os
import re
import threading
import time
import urllib.request
from dataclasses import dataclass, field, fields, asdict
from pathlib import Path
from typing import Optional, Callable

from .ansi import strip_ansi
from .gamedata import GameData

logger = logging.getLogger(__name__)

from .paths import default_cache_dir
CACHE_BASE = default_cache_dir()
ENTITY_DIR = CACHE_BASE / "entities"
NPC_THUMB_DIR = CACHE_BASE / "npc"
ITEM_THUMB_DIR = CACHE_BASE / "item"

# MajorMUD random spawn prefixes — server adds these at spawn time
CREATURE_PREFIXES = {"large", "small", "big", "short", "fat", "fierce", "nasty", "thin", "angry", "happy"}

# LLM config for generating image prompts from descriptions
OLLAMA_URL = "http://localhost:11434/api/generate"
OLLAMA_MODEL = "qwen2.5:3b"

PORTRAIT_SYSTEM_PROMPT = (
    "You convert a text-game character/creature description into a concise portrait prompt "
    "for a fantasy art AI. Focus on: physical appearance, clothing, armor, weapons, "
    "facial features, body type, coloring. Output ONLY the portrait prompt as a single "
    "short paragraph. No thinking tags, no explanation."
)

ITEM_SYSTEM_PROMPT = (
    "You convert a text-game item description into a concise image prompt "
    "for a fantasy art AI. ALWAYS start with the specific item type "
    "(e.g. 'A steel hatchet', 'A wooden longbow', 'A glass potion vial'). "
    "Focus on: the exact item type/weapon class, material, color, shape, "
    "magical effects, engravings, size. "
    "CRITICAL: Describe ONLY the object itself laid flat or floating. "
    "NEVER describe a person, figure, mannequin, or anyone wearing/holding it. "
    "For clothing/armor: describe the garment laid out flat as if on a table. "
    "Output ONLY the image prompt as a single short paragraph. "
    "No thinking tags, no explanation."
)


@dataclass
class EntityInfo:
    name: str  # base name (e.g., "Orc Rogue")
    description: str = ""  # from 'look' command
    entity_type: str = "creature"  # creature, player, npc, item
    base_prompt: str = ""  # LLM-generated image prompt
    equipment: list[str] = field(default_factory=list)  # for players
    full_name: str = ""  # full display name (e.g., "Bisquent Souleb" from brackets)
    last_seen: float = 0.0
    race: str = ""  # parsed from player description (e.g., "Halfling")
    class_name: str = ""  # parsed from player description (e.g., "Bard")
    gender: str = ""  # "male" or "female" from pronouns in description
    hair: str = ""  # hair description (e.g., "short brown")
    eyes: str = ""  # eye color (e.g., "crimson")

    @property
    def portrait_key(self) -> str | None:
        """Get slop key for pre-generated portrait based on race/class/gender."""
        if self.race and self.class_name and self.gender:
            key = f"portrait_{self.race}_{self.class_name}_{self.gender}"
            return key.lower().replace(" ", "_").replace("-", "_")
        return None


class EntityDB:
    def __init__(
        self,
        generate_thumbnail_fn: Optional[Callable[[str, str, str], None]] = None,
        on_look_complete: Optional[Callable[[str, "EntityInfo"], None]] = None,
    ):
        """
        Args:
            generate_thumbnail_fn: callback(entity_key, prompt, size) to request thumbnail generation
            on_look_complete: callback(target_name, entity_info) when a look response is finalized
        """
        self.generate_thumbnail_fn = generate_thumbnail_fn
        self.on_look_complete = on_look_complete
        self.on_status: Optional[Callable[[str], None]] = None  # status text callback
        # Portrait art style prefix (user-configurable, e.g. "anime style", "photorealistic")
        self.portrait_style: str = ""
        self.my_char_name: str = ""  # the player's own character name (for style)
        self.gamedata: Optional[GameData] = None  # game database for entity validation
        self._entities: dict[str, EntityInfo] = {}  # base_name -> EntityInfo
        self._pending_looks: dict[str, float] = {}  # name -> timestamp of look attempt
        self._look_target: Optional[str] = None  # current look target
        self._look_buffer: list[str] = []  # accumulating look response
        self._collecting_look = False
        self._look_waiting_for_response = False
        self._look_waiting_for_echo = False  # waiting for HP echo of our command
        self._look_type_hint = "npc"
        self._look_start_time = 0.0
        self._look_bracket_name = None
        self._lock = threading.Lock()

        # Verification: store candidate descriptions that need a second look to confirm
        # Maps base_name.lower() -> (description_text, timestamp)
        self._candidate_descriptions: dict[str, tuple[str, float]] = {}

        # Known player character names (from 'who' command)
        self._known_players: set[str] = set()
        self._load_known_players()

        ENTITY_DIR.mkdir(parents=True, exist_ok=True)
        NPC_THUMB_DIR.mkdir(parents=True, exist_ok=True)
        ITEM_THUMB_DIR.mkdir(parents=True, exist_ok=True)

        # Load persisted entities
        self._load_from_disk()

    def _entity_file(self, name: str) -> Path:
        safe = hashlib.sha256(name.lower().encode()).hexdigest()[:16]
        return ENTITY_DIR / f"{safe}.json"

    def _thumb_dir_for(self, key: str) -> Path:
        """Get the correct thumbnail directory based on entity type."""
        info = self._entities.get(key.lower())
        if info and info.entity_type == "item":
            return ITEM_THUMB_DIR
        # Also check base name for prefixed entities
        _prefix, base = self.parse_prefix_and_base(key)
        info = self._entities.get(base.lower())
        if info and info.entity_type == "item":
            return ITEM_THUMB_DIR
        return NPC_THUMB_DIR

    def _thumb_file(self, key: str) -> Path:
        safe = hashlib.sha256(key.lower().encode()).hexdigest()[:16]
        thumb_dir = self._thumb_dir_for(key)
        return thumb_dir / f"{safe}.webp"

    def _load_from_disk(self) -> None:
        """Load all persisted entity JSON files."""
        reparsed = 0
        for f in ENTITY_DIR.glob("*.json"):
            try:
                data = json.loads(f.read_text())
                # Strip unknown fields that don't match EntityInfo
                known = {fld.name for fld in fields(EntityInfo)}
                data = {k: v for k, v in data.items() if k in known}
                info = EntityInfo(**data)
                self._entities[info.name.lower()] = info
                # Re-parse player descriptions missing race/class from old cache
                if info.entity_type == "player" and info.description and not info.race:
                    self._parse_player_description(info)
                    if info.race:
                        self._save_entity(info)
                        reparsed += 1
            except Exception as e:
                logger.warning(f"Failed to load entity {f}: {e}")
        logger.info(f"Loaded {len(self._entities)} entities from cache"
                     + (f" (re-parsed {reparsed} players)" if reparsed else ""))

    def _load_known_players(self) -> None:
        """Load known player names from disk."""
        path = CACHE_BASE / "known_players.json"
        if path.exists():
            try:
                self._known_players = set(json.loads(path.read_text()))
                logger.info(f"Loaded {len(self._known_players)} known players")
            except Exception:
                pass

    def _save_known_players(self) -> None:
        """Persist known player names."""
        path = CACHE_BASE / "known_players.json"
        path.write_text(json.dumps(sorted(self._known_players)))

    def update_known_players(self, players: list[str]) -> None:
        """Add player names from 'who' command output."""
        for name in players:
            self._known_players.add(name)
            # Mark existing entities as player type
            info = self._entities.get(name.lower())
            if info and info.entity_type != "player":
                info.entity_type = "player"
                self._save_entity(info)
        self._save_known_players()
        logger.info(f"Known players updated: {self._known_players}")

    def update_players_from_top(self, players: list[tuple[str, str]]) -> None:
        """Add player names + classes from 'top' command output.
        players: [(full_name, class_name), ...]"""
        rerolled = []
        for full_name, class_name in players:
            first_name = full_name.split()[0]
            self._known_players.add(first_name)
            # Create or update entity with class info
            key = first_name.lower()
            info = self._entities.get(key)
            if info is None:
                info = EntityInfo(name=first_name, entity_type="player")
                self._entities[key] = info
            info.entity_type = "player"
            info.full_name = full_name
            # Detect reroll — class changed, invalidate cached description
            new_class = class_name.title()
            if info.class_name and info.class_name != new_class:
                logger.info(f"Player {first_name} rerolled: {info.class_name} -> {new_class}")
                info.description = ""
                info.race = ""
                info.gender = ""
                info.hair = ""
                info.eyes = ""
                # Allow re-look
                self._pending_looks.pop(key, None)
                rerolled.append(first_name)
            info.class_name = new_class
            self._save_entity(info)
        self._save_known_players()
        logger.info(f"Top list: updated {len(players)} players with class info"
                     + (f" ({len(rerolled)} rerolled)" if rerolled else ""))

    def is_known_player(self, name: str) -> bool:
        """Check if a name is a known player character."""
        # Check direct match
        if name in self._known_players:
            return True
        # Check base name after prefix stripping
        _prefix, base = self.parse_prefix_and_base(name)
        if base in self._known_players:
            return True
        # Check entity type
        info = self._entities.get(name.lower()) or self._entities.get(base.lower())
        return info is not None and info.entity_type == "player"

    def get_player_equipment(self, name: str) -> list[str]:
        """Get a player's equipment list from their look description."""
        _prefix, base = self.parse_prefix_and_base(name)
        info = self._entities.get(name.lower()) or self._entities.get(base.lower())
        if info:
            return info.equipment
        return []

    def queue_missing_thumbnails_on_startup(self) -> None:
        """Queue thumbnail generation for any entities that have prompts but no thumbnails.
        Also generates prompts for entities that have descriptions but no prompt yet.
        Call after renderer is ready."""
        if not self.generate_thumbnail_fn:
            return
        queued = 0
        need_prompts = []
        for info in self._entities.values():
            if not info.description:
                continue
            if not info.base_prompt:
                # Only queue prompt generation if thumbnail is also missing
                if not self.has_thumbnail(info.name):
                    need_prompts.append(info)
                continue
            if not self.has_thumbnail(info.name):
                is_self = self.my_char_name and info.name.lower() == self.my_char_name.lower()
                if info.entity_type == "item":
                    thumb_prompt = (f"{info.base_prompt}, single object laid flat on dark background, "
                                   f"no person, no mannequin, no figure, item icon, fantasy art, detailed")
                elif is_self:
                    style = self.portrait_style or "fantasy art"
                    thumb_prompt = f"{info.base_prompt}, portrait, square frame, {style}, detailed"
                else:
                    thumb_prompt = f"{info.base_prompt}, portrait, square frame, fantasy art, detailed"
                self.generate_thumbnail_fn(info.name, thumb_prompt, "base")
                queued += 1
        if queued:
            logger.info(f"Queued {queued} missing thumbnails on startup")
        if need_prompts:
            logger.info(f"Generating prompts for {len(need_prompts)} entities on startup...")
            import threading
            def _gen_prompts():
                for info in need_prompts:
                    self._generate_entity_prompt(info)
            threading.Thread(target=_gen_prompts, daemon=True).start()

    def _save_entity(self, info: EntityInfo) -> None:
        """Persist entity to disk."""
        data = asdict(info)
        self._entity_file(info.name).write_text(json.dumps(data, indent=2))

    @staticmethod
    def parse_prefix_and_base(name: str) -> tuple[str, str]:
        """Split 'Thin Orc Rogue' into ('thin', 'Orc Rogue').
        Returns ('', name) if no known prefix."""
        words = name.strip().split()
        if len(words) > 1 and words[0].lower() in CREATURE_PREFIXES:
            return words[0].lower(), " ".join(words[1:])
        return "", name

    # Currency items that should never be looked at or thumbnailed
    CURRENCY_NAMES = frozenset({
        "silver nobles", "silver noble",
        "copper farthings", "copper farthing",
        "gold crowns", "gold crown",
        "platinum pieces", "platinum piece",
        "runic coins", "runic coin",
    })

    @staticmethod
    def strip_quantity(name: str) -> str:
        """Strip leading quantity number from item names.
        '2 fingerbone bracelet' -> 'fingerbone bracelet'"""
        words = name.strip().split()
        if len(words) > 1 and words[0].isdigit():
            return " ".join(words[1:])
        return name

    @classmethod
    def _should_skip_item(cls, name: str) -> bool:
        """Return True for items that shouldn't be looked at (currency, numeric-only).
        NEVER look at anything starting with a number.
        NEVER look at currency (gold crowns, copper farthings, etc)."""
        s = name.strip()
        if not s:
            return True
        # Never look at anything starting with a number
        first_word = s.split()[0]
        if first_word.isdigit():
            return True
        # Check if the name itself is purely numeric
        if s.isdigit():
            return True
        # Strip quantity prefix, then check currency
        base = cls.strip_quantity(s)
        if not base or base.isdigit():
            return True
        if base.lower() in cls.CURRENCY_NAMES or s.lower() in cls.CURRENCY_NAMES:
            return True
        return False

    def get_entity(self, name: str) -> Optional[EntityInfo]:
        """Get entity info by base name."""
        return self._entities.get(name.lower())

    def has_description(self, base_name: str) -> bool:
        """Check if we already have a description for this base creature."""
        info = self._entities.get(base_name.lower())
        return info is not None and bool(info.description)

    def has_thumbnail(self, key: str) -> bool:
        """Check if a thumbnail exists for this entity key."""
        return self._thumb_file(key).exists()

    def get_thumbnail(self, key: str) -> Optional[bytes]:
        """Get thumbnail PNG bytes, or None."""
        path = self._thumb_file(key)
        if path.exists():
            return path.read_bytes()
        return None

    def save_thumbnail(self, key: str, png_bytes: bytes) -> None:
        """Save thumbnail PNG."""
        self._thumb_file(key).write_bytes(png_bytes)
        logger.info(f"Saved thumbnail for: {key}")

    def delete_thumbnail(self, key: str) -> None:
        """Delete a thumbnail so it can be regenerated."""
        path = self._thumb_file(key)
        if path.exists():
            path.unlink()
            logger.info(f"Deleted thumbnail for: {key}")

    def get_look_targets(self, also_here: list[str]) -> list[str]:
        """Given 'Also here' list, return full names we need to look at.
        Deduplicates by base creature name. Retries after 30s if a previous look didn't complete.
        Validates against gamedata if available — skips entities not in the database."""
        now = time.time()
        targets = []
        seen_bases = set()
        for name in also_here:
            if self._should_skip_item(name):
                continue
            _prefix, base = self.parse_prefix_and_base(name)
            base_lower = base.lower()
            if base_lower in seen_bases:
                continue
            seen_bases.add(base_lower)
            # Validate against gamedata — skip unknown monsters
            # Try full name first (e.g. "nasty looking man" is a real monster)
            # then base name after prefix strip
            if self.gamedata and self.gamedata.is_loaded:
                if (not self.gamedata.is_valid_monster(name) and
                    not self.gamedata.is_valid_monster(base) and
                    not self.is_known_player(name)):
                    logger.debug(f"Skipping unknown entity (not in gamedata): {name}")
                    continue
            needs_verify = base_lower in self._candidate_descriptions
            if self.has_description(base) and not needs_verify:
                continue
            # Faster retry for verification looks (10s vs 30s)
            retry_delay = 10 if needs_verify else 30
            last_attempt = self._pending_looks.get(base_lower, 0)
            if now - last_attempt < retry_delay:
                continue
            # Use full name for look command (MajorMUD needs it)
            targets.append(name)
            self._pending_looks[base_lower] = now
        return targets

    def start_look(self, target: str, entity_type: str = "npc") -> None:
        """Mark that we've sent 'l <target>' and should collect the response."""
        self._look_target = target
        self._look_type_hint = entity_type
        self._look_buffer = []
        self._collecting_look = True
        self._look_waiting_for_echo = True  # wait for HP echo of our command first
        self._look_waiting_for_response = False
        self._look_start_time = time.time()
        logger.info(f"Auto-look started for: {target} (type={entity_type})")

    def feed_look_line(self, line: str) -> bool:
        """Feed a line that might be part of a look response.
        Returns True if this line was consumed as look data."""
        if not self._collecting_look:
            return False

        # Timeout: if we've been waiting more than 3s
        if time.time() - self._look_start_time > 3.0:
            if self._look_buffer:
                # We have collected lines — finalize what we have
                logger.info(f"Look timeout with data for: {self._look_target}, finalizing")
                self._finalize_look()
                return False
            else:
                # No data collected — give up and allow retry
                logger.warning(f"Look timed out for: {self._look_target}")
                target = self._look_target
                self._collecting_look = False
                self._look_waiting_for_response = False
                self._look_waiting_for_echo = False
                self._look_target = None
                # Allow retry
                if target:
                    _prefix, base = self.parse_prefix_and_base(target)
                    self._pending_looks.pop(base.lower(), None)
                return False

        stripped = line.rstrip()

        # Phase 0: wait for the command echo HP line (e.g. "[HP=88/MA=22]:l padded helm")
        # This ensures we ignore any room text that arrives before our command is processed
        if self._look_waiting_for_echo:
            # Find the LAST HP prompt (double prompts like [HP=x/MA=y]:[HP=x/MA=y]:l target)
            hp_matches = list(re.finditer(r'\[HP=\d+/MA=\d+\]', stripped))
            hp_match = hp_matches[-1] if hp_matches else None
            if hp_match and self._look_target:
                # Check if our look command is echoed in this line
                after_hp = stripped[hp_match.end():].strip().lstrip(':').strip()
                # Strip "(Resting)" or similar status prefixes
                after_hp = re.sub(r'^\([\w\s]+\)\s*', '', after_hp).strip()
                if after_hp.lower().startswith('l ') and self._look_target.lower() in after_hp.lower():
                    self._look_waiting_for_echo = False
                    self._look_waiting_for_response = True
                    logger.debug(f"Look echo detected for: {self._look_target}")
                    return False  # let parser also handle the HP line
            return False  # ignore everything until we see our echo

        # Phase 1: waiting for the response to start (after echo confirmed)
        if self._look_waiting_for_response:
            # Another HP prompt = response didn't come, skip
            if re.search(r'\[HP=\d+/MA=\d+\]', stripped):
                return False

            # Skip blank lines before response
            if not stripped:
                return False

            # For NPCs/players: response starts with the entity name as header
            # MajorMUD self-look format: "[ Bisquent Souleb ]"
            if stripped and self._look_target:
                target_lower = self._look_target.lower()
                line_lower = stripped.lower()
                # Direct name match
                if line_lower == target_lower:
                    self._look_waiting_for_response = False
                    return True
                # Bracket format: "[ Name Surname ]" or "[ Name Surname ](Gang Name)"
                bracket_match = re.match(r'^\[\s*(.+?)\s*\](?:\s*\(.*\))?$', stripped)
                if bracket_match:
                    bracket_name = bracket_match.group(1)
                    # Check if target name appears in the bracket
                    if target_lower in bracket_name.lower():
                        self._look_waiting_for_response = False
                        # Store the full name from brackets for character window
                        self._look_bracket_name = bracket_name
                        # This is a player — override type hint so equipment is collected
                        self._look_type_hint = "player"
                        return True

            # For items: no name header, description starts immediately
            # Any non-empty line that isn't the HP prompt is the start of description
            if stripped and self._look_type_hint == "item":
                self._look_waiting_for_response = False
                self._look_buffer.append(stripped)
                return True

            return False

        # Phase 2: collecting description lines
        # Blank line after content — for players, keep collecting (equipment follows blank line)
        if not stripped and self._look_buffer:
            if self._look_type_hint == "player":
                # Player looks have: description, blank, "equipped with:", items
                # Don't finalize on first blank — only on second consecutive blank or HP prompt
                if self._look_buffer and self._look_buffer[-1] == "":
                    # Second consecutive blank — finalize
                    self._finalize_look()
                    return True
                self._look_buffer.append("")
                return True
            else:
                self._finalize_look()
                return True

        # HP prompt ends look — but not if it's just echoing another user command
        # during combat (e.g. "[HP=35/MA=14]:aa cave bear" while look is collecting)
        hp_match = re.search(r'\[HP=\d+/MA=\d+\]', stripped)
        if hp_match:
            after = stripped[hp_match.end():].strip().lstrip(':').strip()
            # If HP prompt has a command after it, don't finalize yet unless we have data
            if after and not self._look_buffer:
                # No data collected yet, just a command echo — keep waiting
                return False
            self._finalize_look()
            return False  # let the parser also handle the HP line

        if stripped:
            self._look_buffer.append(stripped)
        return True

    def _finalize_look(self) -> None:
        """Process accumulated look response."""
        self._collecting_look = False
        self._look_waiting_for_response = False
        self._look_waiting_for_echo = False
        if not self._look_target or not self._look_buffer:
            logger.warning(f"Look finalized with no data for: {self._look_target}")
            self._look_target = None
            self._look_buffer = []
            return

        target = self._look_target
        self._look_target = None
        raw_lines = self._look_buffer[:]
        self._look_buffer = []

        # Strip ANSI codes from each line and clean up
        clean_lines = []
        for line in raw_lines:
            cleaned = strip_ansi(line).strip()
            if cleaned:
                # Skip MajorMUD wound/health status lines
                if self._is_wound_status(cleaned):
                    continue
                # Skip combat lines that got interleaved with look response
                if self._is_combat_line(cleaned):
                    continue
                clean_lines.append(cleaned)

        description = "\n".join(clean_lines)

        # Validate: description must be meaningful (at least 20 chars of real text)
        if len(description) < 20:
            logger.warning(f"Look response too short for {target} ({len(description)} chars), discarding: {description!r}")
            # Remove from pending so it can be retried
            _prefix, base = self.parse_prefix_and_base(target)
            self._pending_looks.pop(base.lower(), None)
            return

        # Reject descriptions that look like room text (wrong output captured)
        if self._looks_like_junk_text(description):
            logger.warning(f"Look response for {target} looks like room text, discarding: {description[:80]!r}")
            _prefix, base = self.parse_prefix_and_base(target)
            self._pending_looks.pop(base.lower(), None)
            return

        logger.info(f"Look response for {target}: {description[:80]}...")

        # Determine entity type from description content or hint
        entity_type = getattr(self, '_look_type_hint', 'npc')
        desc_lower = description.lower()
        if "is carrying" in desc_lower or "is wearing" in desc_lower:
            entity_type = "player"

        # Store — items use their full name as key (no prefix stripping)
        if entity_type == "item":
            base = target
        else:
            _prefix, base = self.parse_prefix_and_base(target)
        base_lower = base.lower()

        # Skip verification for NPCs and players — MajorMUD NPC descriptions
        # are always identical, and players change with HP/equipment.
        # Only verify items (which might capture wrong text).
        skip_verify = entity_type in ("npc", "player")

        existing = self._entities.get(base_lower)
        if existing and existing.description:
            # Already verified — update for players, skip for others
            existing.last_seen = time.time()
            if entity_type == "player":
                existing.description = description
                existing.equipment = self._parse_equipment(description)
                self._parse_player_description(existing)
                bracket_name = getattr(self, '_look_bracket_name', None)
                if bracket_name:
                    existing.full_name = bracket_name
                    self._look_bracket_name = None
                self._save_entity(existing)
                if self.on_look_complete:
                    self.on_look_complete(target, existing)
            return

        if not skip_verify:
            candidate = self._candidate_descriptions.get(base_lower)
            if candidate is None:
                # First look — store as candidate, schedule re-look
                self._candidate_descriptions[base_lower] = (description, time.time())
                self._pending_looks.pop(base_lower, None)  # allow re-look
                logger.info(f"Stored candidate description for {target}, will verify on second look")
                return

            # Second look — compare with candidate
            candidate_desc, _candidate_time = candidate
            if self._descriptions_match(candidate_desc, description):
                # Verified! Store it
                logger.info(f"Description verified for {target} (matched on second look)")
                del self._candidate_descriptions[base_lower]
            else:
                # Mismatch — replace candidate, try again
                logger.warning(f"Description mismatch for {target}, replacing candidate and retrying")
                self._candidate_descriptions[base_lower] = (description, time.time())
                self._pending_looks.pop(base_lower, None)
                return

        info = EntityInfo(name=base, entity_type=entity_type)

        info.description = description
        info.last_seen = time.time()

        # Store full bracket name if available (e.g., "Bisquent Souleb")
        bracket_name = getattr(self, '_look_bracket_name', None)
        if bracket_name:
            info.full_name = bracket_name
            self._look_bracket_name = None

        # Extract equipment and race/class/gender for players
        if entity_type == "player":
            info.equipment = self._parse_equipment(description)
            self._parse_player_description(info)

        self._entities[base_lower] = info
        self._save_entity(info)

        # Notify callback
        if self.on_look_complete:
            self.on_look_complete(target, info)

        # Generate base prompt via LLM, then request thumbnail
        threading.Thread(
            target=self._generate_entity_prompt, args=(info,), daemon=True
        ).start()

    # ── Player description parser ──
    # MajorMUD look format:
    #   "Tripmunk is a healthy, moderately built Halfling Bard with short brown
    #    hair and crimson eyes. He moves with catlike agility..."

    _MAJORMUD_RACES = {
        "human", "dwarf", "gnome", "halfling", "elf", "half-elf",
        "dark-elf", "half-orc", "goblin", "half-ogre", "kang",
        "nekojin", "gaunt one",
    }
    _MAJORMUD_CLASSES = {
        "warrior", "witchunter", "paladin", "cleric", "priest",
        "missionary", "ninja", "thief", "bard", "gypsy",
        "warlock", "mage", "druid", "ranger", "mystic",
    }

    # Pattern: "{Name} is a/an {descriptors} {Race} {Class} with {hair} hair and {eye} eyes."
    _RE_PLAYER_DESC = re.compile(
        r'(\w+)\s+is\s+(?:a|an)\s+(.+?)\s+with\s+(.+?)\s+hair\s+and\s+(.+?)\s+eyes\b',
        re.IGNORECASE | re.DOTALL,
    )
    # Fallback for bald/hairless races: "{Name} is a/an {descriptors} {Race} {Class} with {eye} eyes."
    _RE_PLAYER_DESC_NO_HAIR = re.compile(
        r'(\w+)\s+is\s+(?:a|an)\s+(.+?)\s+with\s+(.+?)\s+eyes\b',
        re.IGNORECASE | re.DOTALL,
    )

    def _parse_player_description(self, info: EntityInfo) -> None:
        """Extract race, class, gender, hair, eyes from a player's look description."""
        if not info.description or info.entity_type != "player":
            return

        desc = " ".join(info.description.split())  # normalize whitespace

        # Extract gender from pronoun usage
        desc_lower = desc.lower()
        if re.search(r'\bhe\b', desc_lower):
            info.gender = "male"
        elif re.search(r'\bshe\b', desc_lower):
            info.gender = "female"

        # Try full pattern first (with hair and eyes)
        m = self._RE_PLAYER_DESC.search(desc)
        if m:
            info.hair = m.group(3).strip()
            info.eyes = m.group(4).strip()
            middle = m.group(2).strip()
        else:
            # Fallback: no hair mention (bald races like Kang, Gaunt One)
            m = self._RE_PLAYER_DESC_NO_HAIR.search(desc)
            if not m:
                return
            info.eyes = m.group(3).strip()
            middle = m.group(2).strip()

        # The middle part has build descriptors + race + class
        # e.g., "healthy, moderately built Halfling Bard"
        words = middle.split()

        # Scan backwards to find class then race
        # Class is always one word, race can be two ("Gaunt One", "Half-Elf", etc.)
        found_class = ""
        found_race = ""
        for i in range(len(words) - 1, -1, -1):
            w = words[i].lower().rstrip(",")
            if not found_class and w in self._MAJORMUD_CLASSES:
                found_class = w
                continue
            if not found_race:
                # Check two-word races first
                if i > 0:
                    two_word = f"{words[i-1]} {words[i]}".lower().rstrip(",")
                    if two_word in self._MAJORMUD_RACES:
                        found_race = two_word
                        break
                if w in self._MAJORMUD_RACES:
                    found_race = w
                    break

        if found_race:
            # Title case for storage
            info.race = found_race.title()
        if found_class:
            info.class_name = found_class.title()

        if info.race and info.class_name and info.gender:
            logger.info(f"Parsed player {info.name}: {info.gender} {info.race} {info.class_name} "
                        f"(hair={info.hair}, eyes={info.eyes}, portrait={info.portrait_key})")

    # MajorMUD wound/health status phrases (shown at end of look responses)
    _WOUND_PATTERNS = re.compile(
        r'(?:he|she|it|they)\s+(?:appears?\s+to\s+be|is|are)\s+'
        r'(?:unwounded|barely wounded|wounded|severely wounded|'
        r'very badly wounded|mortally wounded|barely alive|'
        r'close to death|at death\'s door|near death|'
        r'almost dead|critically wounded|grievously wounded)',
        re.IGNORECASE
    )

    @classmethod
    def _is_wound_status(cls, line: str) -> bool:
        """Check if a line is a MajorMUD wound/health status line."""
        return bool(cls._WOUND_PATTERNS.search(line))

    _COMBAT_LINE = re.compile(
        r'(?:You (?:chop|cut|hack|slash|stab|swing|swipe|whap|bludgeon|smash|cleave|pound|strike)'
        r'|The .+ (?:lunges|snaps|swipes|bites|claws|strikes|breathes|casts)'
        r'|.+ for \d+ damage!'
        r'|.+ falls to the ground'
        r'|.+ collapses'
        r'|.+ drops? to the ground'
        r'|\*Combat (?:Engaged|Off)\*'
        r'|You gain \d+ experience)',
        re.IGNORECASE
    )

    @classmethod
    def _is_combat_line(cls, line: str) -> bool:
        """Check if a line is combat output that got interleaved with look response."""
        return bool(cls._COMBAT_LINE.search(line))

    @staticmethod
    def _looks_like_junk_text(description: str) -> bool:
        """Detect if a description is actually non-description MUD output captured by mistake."""
        desc_lower = description.lower()
        # Room indicators
        if "obvious exits:" in desc_lower:
            return True
        if re.search(r'you notice .+ here\.?', desc_lower):
            return True
        # Stat/score output
        if re.search(r'lives/cp:', desc_lower):
            return True
        if re.search(r'\[hp=\d+/ma=\d+\]', desc_lower):
            return True
        # Who list
        if "current adventurers" in desc_lower:
            return True
        # "You do not see X here!"
        if "you do not see" in desc_lower:
            return True
        # Room name at the start (e.g., "Temple, Halls of the Dead")
        first_line = description.split('\n')[0].strip()
        if len(first_line) < 60 and ',' in first_line and not any(
            w in first_line.lower() for w in ['this ', 'these ', 'a ', 'an ', 'the ']
        ):
            return True
        return False

    @staticmethod
    def _descriptions_match(desc1: str, desc2: str) -> bool:
        """Check if two descriptions are similar enough to be the same entity.
        Uses simple word overlap — descriptions from the same entity should be
        nearly identical (or identical) across multiple looks."""
        # Normalize whitespace
        w1 = set(desc1.lower().split())
        w2 = set(desc2.lower().split())
        if not w1 or not w2:
            return False
        # Jaccard similarity — require at least 80% overlap
        intersection = len(w1 & w2)
        union = len(w1 | w2)
        similarity = intersection / union
        return similarity >= 0.8

    def _parse_equipment(self, description: str) -> list[str]:
        """Extract equipment list from player description.
        MajorMUD format: 'He is equipped with:' followed by lines like
        'padded helm                    (Head)'
        """
        equipment = []
        in_equipment = False
        for line in description.split("\n"):
            line_lower = line.lower().strip()
            if "is equipped with" in line_lower or "is wearing" in line_lower or "is carrying" in line_lower:
                in_equipment = True
                continue
            if in_equipment and line.strip():
                # Parse "item_name            (Slot)" format
                slot_match = re.match(r'^(.+?)\s{2,}\(([^)]+)\)\s*$', line.strip())
                if slot_match:
                    equipment.append(f"{slot_match.group(1).strip()} ({slot_match.group(2).strip()})")
                else:
                    equipment.append(line.strip())
            elif in_equipment and not line.strip():
                break
        return equipment

    def _update_status(self, text: str) -> None:
        if self.on_status:
            self.on_status(text)

    def cleanup_invalid_entities(self) -> int:
        """Remove cached entities and thumbnails that don't exist in the game database.
        Returns the number of entities removed."""
        if not self.gamedata or not self.gamedata.is_loaded:
            return 0
        removed = 0
        to_remove = []
        for key, info in self._entities.items():
            if info.entity_type == "player":
                continue  # never remove players
            if info.entity_type == "item":
                if not self.gamedata.is_valid_item(info.name):
                    to_remove.append(key)
            else:
                if not self.gamedata.is_valid_monster(info.name) and not self.is_known_player(info.name):
                    to_remove.append(key)
        for key in to_remove:
            info = self._entities.pop(key, None)
            if info:
                # Delete entity JSON
                ef = self._entity_file(info.name)
                if ef.exists():
                    ef.unlink()
                # Delete thumbnail
                tf = self._thumb_file(info.name)
                if tf.exists():
                    tf.unlink()
                logger.info(f"Cleaned up invalid entity: {info.name}")
                removed += 1
        if removed:
            logger.info(f"Cleaned up {removed} invalid entities not found in gamedata")
        return removed

    def _generate_entity_prompt(self, info: EntityInfo, bypass_validation: bool = False) -> None:
        """Use LLM to convert entity description to image prompt, then request thumbnail."""
        # Validate against gamedata before wasting API calls (skip if user-requested regen)
        if not bypass_validation and self.gamedata and self.gamedata.is_loaded:
            if info.entity_type == "item" and not self.gamedata.is_valid_item(info.name):
                logger.info(f"Skipping prompt generation for unknown item: {info.name}")
                return
            if info.entity_type not in ("item", "player") and not self.gamedata.is_valid_monster(info.name):
                if not self.is_known_player(info.name):
                    logger.info(f"Skipping prompt generation for unknown entity: {info.name}")
                    return
        try:
            self._update_status(f"LLM prompt: {info.name}...")
            system = PORTRAIT_SYSTEM_PROMPT if info.entity_type != "item" else ITEM_SYSTEM_PROMPT

            # Build prompt — include equipment for players
            prompt_text = f"Name: {info.name}\nDescription: {info.description}"
            if info.entity_type == "player" and info.equipment:
                equip_str = ", ".join(info.equipment)
                prompt_text += f"\nEquipped items: {equip_str}"

            payload = json.dumps({
                "model": OLLAMA_MODEL,
                "system": system,
                "prompt": prompt_text,
                "stream": False,
            }).encode()

            req = urllib.request.Request(
                OLLAMA_URL,
                data=payload,
                headers={"Content-Type": "application/json"},
            )
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = json.loads(resp.read().decode())

            response = data.get("response", "").strip()

            # Strip deepseek think tags
            if "<think>" in response:
                parts = response.split("</think>")
                response = parts[-1].strip()
            response = response.strip('"\'')

            if len(response) > 20:
                info.base_prompt = response
                self._save_entity(info)
                logger.info(f"Entity prompt for {info.name}: {response[:80]}...")

                # Request base thumbnail
                if self.generate_thumbnail_fn and not self.has_thumbnail(info.name):
                    is_self = self.my_char_name and info.name.lower() == self.my_char_name.lower()
                    if info.entity_type == "item":
                        thumb_prompt = (f"{response}, single object laid flat on dark background, "
                                       f"no person, no mannequin, no figure, item icon, fantasy art, detailed")
                    elif is_self:
                        style = self.portrait_style or "fantasy art"
                        thumb_prompt = f"{response}, portrait, square frame, {style}, detailed"
                    else:
                        thumb_prompt = f"{response}, portrait, square frame, fantasy art, detailed"
                    self._update_status(f"Generating: {info.name}...")
                    self.generate_thumbnail_fn(info.name, thumb_prompt, "base")
            else:
                logger.warning(f"LLM response too short for {info.name}: {response}")
                self._update_status(f"LLM failed for {info.name}")

        except Exception as e:
            logger.error(f"Entity prompt generation failed for {info.name}: {e}")
            self._update_status(f"Error: {info.name}: {e}")

    def request_missing_thumbnails(self, also_here: list[str]) -> None:
        """For entities that have descriptions + prompts but no thumbnail yet, re-request generation."""
        if not self.generate_thumbnail_fn:
            return
        for name in also_here:
            _prefix, base = self.parse_prefix_and_base(name)
            info = self._entities.get(base.lower())
            if info and info.base_prompt and not self.has_thumbnail(base):
                thumb_prompt = f"{info.base_prompt}, portrait, square frame, fantasy art, detailed"
                logger.info(f"Re-requesting missing thumbnail for: {base}")
                self.generate_thumbnail_fn(base, thumb_prompt, "base")

    def get_entity_display_info(self, also_here: list[str]) -> list[dict]:
        """Get display info for all entities in the room.
        Returns list of {name, display_name, prefix, has_thumb, thumb_key}."""
        result = []
        for name in also_here:
            prefix, base = self.parse_prefix_and_base(name)
            thumb_key = name if prefix else base
            result.append({
                "name": name,
                "display_name": name,
                "base_name": base,
                "prefix": prefix,
                "thumb_key": thumb_key,
                "has_thumb": self.has_thumbnail(thumb_key),
                "has_base_thumb": self.has_thumbnail(base),
            })
        return result

    def get_item_look_targets(self, items: list[str]) -> list[str]:
        """Given 'You notice' items list, return names we need to look at.
        Strips leading quantity numbers (e.g. '2 fingerbone bracelet' -> 'fingerbone bracelet').
        Skips currency and numeric-only names."""
        now = time.time()
        targets = []
        seen = set()
        for name in items:
            if self._should_skip_item(name):
                continue
            base = self.strip_quantity(name)
            key = base.lower()
            if key in seen:
                continue
            seen.add(key)
            needs_verify = key in self._candidate_descriptions
            if self.has_description(base) and not needs_verify:
                continue
            retry_delay = 10 if needs_verify else 30
            last_attempt = self._pending_looks.get(key, 0)
            if now - last_attempt < retry_delay:
                continue
            targets.append(base)
            self._pending_looks[key] = now
        return targets

    def get_item_display_info(self, items: list[str],
                              quantities: dict[str, int] | None = None) -> list[dict]:
        """Get display info for items on the ground."""
        result = []
        for name in items:
            if self._should_skip_item(name):
                continue
            base = self.strip_quantity(name)
            qty = (quantities or {}).get(base, 1)
            result.append({
                "name": base,
                "display_name": base,
                "base_name": base,
                "prefix": "",
                "thumb_key": base,
                "has_thumb": self.has_thumbnail(base),
                "has_base_thumb": False,
                "quantity": qty,
            })
        return result

    def request_missing_item_thumbnails(self, items: list[str]) -> None:
        """Re-request thumbnails for items with descriptions but no thumbnail."""
        if not self.generate_thumbnail_fn:
            return
        for name in items:
            if self._should_skip_item(name):
                continue
            base = self.strip_quantity(name)
            info = self._entities.get(base.lower())
            if info and info.base_prompt and not self.has_thumbnail(base):
                thumb_prompt = f"{info.base_prompt}, item, square frame, fantasy art, detailed"
                logger.info(f"Re-requesting missing item thumbnail for: {base}")
                self.generate_thumbnail_fn(base, thumb_prompt, "item")
