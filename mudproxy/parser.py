import re
from enum import Enum, auto
from dataclasses import dataclass, field
from typing import Optional, Callable

# Strip ANSI escape sequences (e.g. [K, [0m, [1;32m, ESC[...)
RE_ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]|\[[ -/]*[A-Za-z]')


class ParserState(Enum):
    IDLE = auto()
    ROOM_DESC = auto()


@dataclass
class RoomData:
    name: str = ""
    description: str = ""
    items: list[str] = field(default_factory=list)
    item_quantities: dict = field(default_factory=dict)  # item_name -> count
    monsters: list[str] = field(default_factory=list)
    exits: list[str] = field(default_factory=list)


@dataclass
class CombatEvent:
    attacker: str = ""
    target: str = ""
    damage: int = 0
    message: str = ""


@dataclass
class InventoryData:
    items: list[dict] = field(default_factory=list)  # [{name, slot}]
    wealth: str = ""  # "51 platinum, 3 gold, ..."
    encumbrance: str = ""


class MudParser:
    RE_HP_PROMPT = re.compile(r'\[HP=(\d+)/MA=(\d+)\]:')
    RE_STAT_HITS = re.compile(r'Hits:\s+(\d+)/(\d+)')
    RE_STAT_MANA = re.compile(r'Mana:\s*\*?\s*(\d+)/(\d+)')
    RE_OBVIOUS_EXITS = re.compile(r'^Obvious (?:exits|paths):\s*(.+)$', re.IGNORECASE)
    RE_ALSO_HERE = re.compile(r'^Also here:\s*(.+?)\.?\s*$', re.IGNORECASE)
    RE_YOU_NOTICE = re.compile(r'^You notice (.+?)(?:\s+here)?\.?\s*$', re.IGNORECASE)
    RE_QUANTITY_PREFIX = re.compile(r'^\d+\s+')
    RE_DAMAGE = re.compile(r'for\s+(\d+)\s+damage!', re.IGNORECASE)
    RE_KILLED = re.compile(
        r'^(.+?)\s+(?:drops dead|is killed|collapses|crumples|falls to the ground)',
        re.IGNORECASE
    )
    RE_XP_GAIN = re.compile(r'^You gain (\d+) experience', re.IGNORECASE)
    RE_YOU_DIED = re.compile(r'(?:You die|You have been killed|You are dead)', re.IGNORECASE)

    # Inventory line: "You are carrying ..."
    RE_INVENTORY = re.compile(r'^You are carrying\s+(.+)', re.IGNORECASE)
    # Currency patterns in inventory
    RE_CURRENCY = re.compile(
        r'(\d+)\s+(platinum\s+pieces?|gold\s+crowns?|silver\s+nobles?|'
        r'copper\s+farthings?|runic\s+copper)',
        re.IGNORECASE
    )
    # Item with slot: "black chainmail hauberk (Torso)" or "golden sickle (Weapon Hand)"
    RE_ITEM_SLOT = re.compile(r'^(.+?)\s*\(([^)]+)\)$')
    # Encumbrance line
    RE_ENCUMBRANCE = re.compile(r'^Encumbrance:\s*(.+)', re.IGNORECASE)
    # Who list header
    RE_WHO_HEADER = re.compile(r'^Current\s+(?:adventurers|players)', re.IGNORECASE)
    # Who list entry: "Bisquent the Human Warrior" or similar
    # MajorMUD who format: "    Good Abbath            -  Divine Protector"
    # Reputation prefix, then player first name (aligned with "C" in "Current")
    # MajorMUD reputation words that prefix player names in WHO list
    _REPUTATION_WORDS = {
        "saint", "good", "neutral", "seedy", "outlaw",
        "criminal", "villain", "fiend", "lawful", "evil", "chaotic",
    }
    RE_WHO_ENTRY = re.compile(r'^\s*(?:\w+)\s+(\w+)\s+', re.IGNORECASE)

    # MajorMUD room names: "Area, Place" (e.g., "Newhaven, Healer")
    # or standalone names (e.g., "Guild Street", "River Street")
    RE_ROOM_NAME = re.compile(r'^[A-Z][a-zA-Z\' ]+,\s+[A-Z][a-zA-Z\' ]+$')
    # Loose match: 2-6 words, at least one capital letter somewhere
    # Allow periods (St.), ampersands (&), hyphens, and commas in room names
    RE_ROOM_NAME_SHORT = re.compile(r"^[a-zA-Z][a-zA-Z'.\-]+(?:\s+[a-zA-Z'.&\-]+){1,7}$")

    # Lines that should never be treated as room names
    NON_ROOM_PATTERNS = [
        re.compile(r'^Attempting to ', re.IGNORECASE),
        re.compile(r'^Sneaking', re.IGNORECASE),
        re.compile(r'^There is no exit', re.IGNORECASE),
        re.compile(r'^Wealth:', re.IGNORECASE),
        re.compile(r'^Name:', re.IGNORECASE),
        re.compile(r'^Race:', re.IGNORECASE),
        re.compile(r'^Item\s+Quantity', re.IGNORECASE),
        re.compile(r'^Please make a selection', re.IGNORECASE),
        re.compile(r'^Enter your', re.IGNORECASE),
        re.compile(r'^Greetings,', re.IGNORECASE),
        re.compile(r'^There is mail', re.IGNORECASE),
        re.compile(r'^By the way,', re.IGNORECASE),
        re.compile(r'^We are currently', re.IGNORECASE),
        re.compile(r'^For more information', re.IGNORECASE),
        re.compile(r'^Fantasy awaits', re.IGNORECASE),
        re.compile(r'^If you already', re.IGNORECASE),
        re.compile(r'^Auto-sensing', re.IGNORECASE),
        re.compile(r'^M\s+A\s+J\s+O\s+R', re.IGNORECASE),
        re.compile(r'^Good\s+\w+\s+\w+\s+-\s+', re.IGNORECASE),  # alignment line
        re.compile(r'^\d+\s+copper', re.IGNORECASE),
    ]

    NON_ROOM_PREFIXES = (
        "You ", "Your ", "[", "(", ">", "*",
        "Also here", "Obvious exits", "Obvious paths",
        "Nothing ", "Someone ", "Something ", "Nobody ",
        "Welcome ", "Goodbye ", "Thank ",
        "The room is ",  # lighting lines: "The room is dimly lit", "The room is barely visible"
        "The following ",  # "The following people are in your travel party:"
    )

    def __init__(self):
        self.state = ParserState.IDLE
        self._room = RoomData()
        self._desc_lines: list[str] = []
        self._in_game = False  # Track if we're inside MajorMUD
        self._also_here_partial = ""  # for multi-line "Also here:" wrapping
        self._you_notice_partial = ""  # for multi-line "You notice:" wrapping
        self._max_hp = 0
        self._max_mana = 0
        self._last_descriptions: dict[str, str] = {}  # room name -> last known description

        # Inventory collection state
        self._collecting_inventory = False
        self._inventory_lines: list[str] = []

        # Who collection state
        self._collecting_who = False
        self._who_lines: list[str] = []
        self._who_seen_dashes = False
        self._char_name_lower = ""  # first name, lowercase, for filtering own echoes
        self._room_has_exits = False  # True once "Obvious exits/paths" seen for current room

        self.on_room_update: Optional[Callable[[RoomData], None]] = None

    @staticmethod
    def _replace_last_and(text: str) -> str:
        """Replace only the LAST ' and ' with ', ' (list separator).
        Earlier ' and ' may be part of item names like 'rope and grapple'."""
        idx = text.rfind(' and ')
        if idx < 0:
            return text
        return text[:idx] + ', ' + text[idx + 5:]

    def _split_items(self, content: str) -> list[str]:
        """Split comma-separated item list and strip quantity prefixes.
        '2 fingerbone bracelet, sword' -> ['fingerbone bracelet', 'sword']
        Also populates self._room.item_quantities with counts."""
        import re
        items = []
        self._room.item_quantities = {}
        for raw in content.split(','):
            name = raw.strip().rstrip('.')
            if not name:
                continue
            qty_match = re.match(r'^(\d+)\s+', name)
            if qty_match:
                qty = int(qty_match.group(1))
                name = name[qty_match.end():]
            else:
                qty = 1
            if name:
                items.append(name)
                self._room.item_quantities[name] = qty
        return items
        self.on_combat: Optional[Callable[[str], None]] = None
        self.on_hp_update: Optional[Callable[[int, int, int, int], None]] = None  # hp, max_hp, mana, max_mana
        self.on_death: Optional[Callable[[], None]] = None
        self.on_xp: Optional[Callable[[int], None]] = None
        self.on_inventory: Optional[Callable[[InventoryData], None]] = None
        self.on_who_list: Optional[Callable[[list[str]], None]] = None
        self.on_char_name: Optional[Callable[[str], None]] = None  # from stat "Name:" line
        # Chat callback: (sender, message, channel) where channel is
        # "gangpath", "broadcast", "telepath", or "say"
        self.on_chat: Optional[Callable[[str, str, str], None]] = None

    # Chat message patterns
    RE_GANGPATH = re.compile(r'^(\w+)\s+gangpaths:\s*(.+)$', re.IGNORECASE)
    RE_BROADCAST = re.compile(r'^(\w+)\s+broadcasts:\s*(.+)$', re.IGNORECASE)
    RE_TELEPATH = re.compile(r'^(\w+)\s+telepaths:\s*(.+)$', re.IGNORECASE)
    RE_GOSSIP = re.compile(r'^(\w+)\s+gossips:\s*(.+)$', re.IGNORECASE)
    RE_SAY = re.compile(r'^(\w+)\s+says\s+"(.+)"$', re.IGNORECASE)

    def feed_line(self, line: str, ansi_line: str = '') -> None:
        line = line.rstrip()
        self._current_ansi_line = ansi_line

        # Parse max HP/Mana from stat output (Hits: 24/24, Mana: * 22/22)
        hits_match = self.RE_STAT_HITS.search(line)
        if hits_match:
            self._max_hp = int(hits_match.group(2))
        mana_match = self.RE_STAT_MANA.search(line)
        if mana_match:
            self._max_mana = int(mana_match.group(2))

        # HP prompt - always check, finalizes room block and inventory/who
        # Also means we're in-game
        hp_match = self.RE_HP_PROMPT.search(line)
        if hp_match:
            self._in_game = True
            hp = int(hp_match.group(1))
            mana = int(hp_match.group(2))
            if hp > self._max_hp:
                self._max_hp = hp
            if mana > self._max_mana:
                self._max_mana = mana
            if self.on_hp_update:
                self.on_hp_update(hp, self._max_hp, mana, self._max_mana)
            if self.state != ParserState.IDLE:
                self._finalize_room()
            if self._collecting_inventory:
                self._finalize_inventory()
            if self._collecting_who:
                self._finalize_who()
            self.state = ParserState.IDLE
            # Check for text after HP prompt (e.g. "[HP=88/MA=27]:Room Name")
            remainder = line[hp_match.end():].strip()
            if remainder:
                self.feed_line(remainder)
            return

        # Chat message detection (gangpath, broadcast, telepath, say)
        if self.on_chat:
            stripped = line.strip()
            # Log lines that contain chat keywords for debugging
            low = stripped.lower()
            if any(kw in low for kw in ('gossips:', 'gangpaths:', 'broadcasts:', 'telepaths:', 'says "')):
                import logging
                logging.getLogger('mudproxy.parser').info(f"Chat candidate line: {stripped!r}")
            for pat, channel in (
                (self.RE_GANGPATH, "gangpath"),
                (self.RE_BROADCAST, "broadcast"),
                (self.RE_TELEPATH, "telepath"),
                (self.RE_GOSSIP, "gossip"),
                (self.RE_SAY, "say"),
            ):
                m = pat.match(stripped)
                if m:
                    sender, message = m.group(1), m.group(2)
                    # Skip own echoes
                    if sender.lower() != "you" and sender.lower() != self._char_name_lower:
                        self.on_chat(sender, message, channel)
                    break

        # Collecting inventory lines (multi-line "You are carrying ...")
        if self._collecting_inventory:
            if not line.strip():
                self._finalize_inventory()
                return
            self._inventory_lines.append(line)
            return

        # Collecting who list
        if self._collecting_who:
            if not line.strip() and self._who_seen_dashes:
                self._finalize_who()
                return
            if '---' in line:
                self._who_seen_dashes = True
                return
            if self._who_seen_dashes and line.strip():
                self._who_lines.append(line.strip())
            return

        # Detect character name from stat output: "Name: Bisquent Souleb"
        if line.strip().startswith("Name:"):
            name_val = line.strip()[5:].strip()
            if name_val:
                self._char_name_lower = name_val.split()[0].lower()
                if self.on_char_name:
                    self.on_char_name(name_val)
            return

        # Start inventory collection
        inv_match = self.RE_INVENTORY.match(line)
        if inv_match:
            self._collecting_inventory = True
            self._inventory_lines = [line]
            return

        # Start who collection
        if self.RE_WHO_HEADER.match(line.strip()):
            self._collecting_who = True
            self._who_lines = []
            self._who_seen_dashes = False
            return

        # Check for multi-line "Also here:" continuation
        if self._also_here_partial:
            stripped = line.strip()
            # If we hit Obvious exits or HP prompt, finalize partial as-is
            if self.RE_OBVIOUS_EXITS.match(line) or self.RE_HP_PROMPT.search(line):
                # Finalize what we have
                content = self._also_here_partial.rstrip('.')
                self._also_here_partial = ""
                entities = [e.strip().rstrip('.') for e in content.split(',')]
                self._room.monsters = [e for e in entities if e]
                # Don't return — let this line be processed normally below
            elif stripped.endswith('.'):
                # End of the wrapped line
                combined = self._also_here_partial + " " + stripped
                self._also_here_partial = ""
                combined = combined.rstrip('.')
                entities = [e.strip().rstrip('.') for e in combined.split(',')]
                self._room.monsters = [e for e in entities if e]
                return
            elif stripped:
                # Still wrapping
                self._also_here_partial = self._also_here_partial + " " + stripped
                return
            else:
                # Blank line — finalize what we have
                content = self._also_here_partial.rstrip('.')
                self._also_here_partial = ""
                entities = [e.strip().rstrip('.') for e in content.split(',')]
                self._room.monsters = [e for e in entities if e]
                return

        # Check for multi-line "You notice:" continuation
        if self._you_notice_partial:
            stripped = line.strip()
            if self.RE_OBVIOUS_EXITS.match(line) or self.RE_ALSO_HERE.match(line) or self.RE_HP_PROMPT.search(line):
                # Finalize partial
                content = self._replace_last_and(self._you_notice_partial).rstrip('.').rstrip()
                self._you_notice_partial = ""
                if content.lower().endswith(' here'):
                    content = content[:-5].rstrip()
                self._room.items = self._split_items(content)
                # Don't return — process this line normally
            elif stripped.endswith('.') or stripped.endswith('here'):
                combined = self._you_notice_partial + " " + stripped
                self._you_notice_partial = ""
                combined = self._replace_last_and(combined).rstrip('.').rstrip()
                if combined.lower().endswith(' here'):
                    combined = combined[:-5].rstrip()
                self._room.items = self._split_items(combined)
                return
            elif stripped:
                self._you_notice_partial = self._you_notice_partial + " " + stripped
                return
            else:
                content = self._replace_last_and(self._you_notice_partial).rstrip('.').rstrip()
                self._you_notice_partial = ""
                if content.lower().endswith(' here'):
                    content = content[:-5].rstrip()
                self._room.items = self._split_items(content)
                return

        # Obvious exits - confirms this is a real room and finalizes it
        exits_match = self.RE_OBVIOUS_EXITS.match(line)
        if exits_match:
            self._also_here_partial = ""
            self._you_notice_partial = ""
            self._room_has_exits = True
            self._room.exits = [e.strip().rstrip('.')
                                for e in exits_match.group(1).split(',')
                                if e.strip()]
            self._finalize_room()
            return

        # Also here (monsters/players) — may wrap across lines
        also_match = self.RE_ALSO_HERE.match(line)
        if also_match:
            content = also_match.group(1).strip()
            # Check original line for trailing period (regex strips it)
            if line.rstrip().endswith('.'):
                # Complete on one line
                content = content.rstrip('.')
                entities = [e.strip().rstrip('.') for e in content.split(',')]
                self._room.monsters = [e for e in entities if e]
            else:
                # Line wraps — save partial and wait for continuation
                self._also_here_partial = content
            return

        # You notice (items) — may wrap across lines
        notice_match = self.RE_YOU_NOTICE.match(line)
        if notice_match:
            content = notice_match.group(1).strip()
            # Check original line for completeness
            if line.rstrip().endswith('.') or line.rstrip().rstrip('.').endswith('here'):
                content = self._replace_last_and(content)
                content = content.rstrip('.').rstrip()
                if content.lower().endswith(' here'):
                    content = content[:-5].rstrip()
                self._room.items = self._split_items(content)
            else:
                self._you_notice_partial = content
            return

        # Combat damage
        if self.RE_DAMAGE.search(line):
            if self.on_combat:
                self.on_combat(line)
            return

        # Monster killed
        if self.RE_KILLED.match(line):
            if self.on_combat:
                self.on_combat(line)
            return

        # XP gain
        xp_match = self.RE_XP_GAIN.match(line)
        if xp_match:
            if self.on_xp:
                self.on_xp(int(xp_match.group(1)))
            return

        # Death
        if self.RE_YOU_DIED.search(line):
            if self.on_death:
                self.on_death()
            return

        # Blank line - potential room block separator
        if not line:
            if self.state == ParserState.ROOM_DESC:
                self._finalize_room()
            return

        # Room name detection (idle state)
        if self.state == ParserState.IDLE:
            if self._looks_like_room_name(line):
                self._room = RoomData(name=line.strip())
                self._desc_lines = []
                self._also_here_partial = ""
                self._you_notice_partial = ""
                self._room_has_exits = False
                self.state = ParserState.ROOM_DESC
                return

        # While collecting a room desc, check if a new room name appears
        # with no/minimal description — means the previous "room" was junk
        # (e.g. a monster death message parsed as a room name)
        if self.state == ParserState.ROOM_DESC and len(self._desc_lines) == 0:
            if self._looks_like_room_name(line):
                # Replace the bogus room with this real one
                self._room = RoomData(name=line.strip())
                self._desc_lines = []
                self._also_here_partial = ""
                self._you_notice_partial = ""
                self._room_has_exits = False
                return

        # Accumulate description lines (strip ANSI escape sequences like [K)
        if self.state == ParserState.ROOM_DESC:
            # "You gain X experience" right after a "room name" means the
            # name was actually a monster death message — discard it
            if 'experience' in line.lower() and line.strip().startswith('You gain'):
                self.state = ParserState.IDLE
                return
            self._desc_lines.append(RE_ANSI.sub('', line))

    # Bold cyan = room name marker (consistent across all palettes 0-3)
    _ROOM_NAME_ANSI = '\x1b[1;36m'

    def _looks_like_room_name(self, line: str) -> bool:
        s = line.strip()
        if not s or len(s) > 65:
            return False

        # Primary detection: ANSI bold cyan at the "start" of the line
        # (after cursor/clear codes like \x1b[0m \x1b[79D \x1b[K)
        ansi = getattr(self, '_current_ansi_line', '')
        if ansi:
            # Strip cursor movement and clear codes to find the first color code
            import re as _re
            cleaned = _re.sub(r'\x1b\[\d*[A-HJK]|\x1b\[\d+D|\x1b\[0m', '', ansi).lstrip()
            if cleaned.startswith(self._ROOM_NAME_ANSI):
                return True
            # If we have ANSI data but it doesn't start with bold cyan, it's not a room
            return False

        # Fallback for lines without ANSI data (e.g. recursive feed_line calls)
        if not self._in_game:
            return False

        # Check against known non-room patterns
        for pat in self.NON_ROOM_PATTERNS:
            if pat.match(s):
                return False

        if any(s.startswith(p) for p in self.NON_ROOM_PREFIXES):
            return False

        if 'damage!' in s.lower() or 'experience' in s.lower():
            return False

        if s.endswith('!'):
            return False

        sl = s.lower()
        _sentence_verbs = (
            ' crumbles ', ' collapses ', ' falls ', ' dies ', ' dissolves ',
            ' explodes ', ' shatters ', ' screams ', ' shrieks ', ' groans ',
            ' staggers ', ' slumps ', ' topples ', ' attacks ', ' strikes ',
            ' breathes ', ' casts ', ' dodges ', ' picked up ', ' dropped ',
            ' appears to ', ' seems to ', ' begins to ', ' starts to ',
            ' walks ', ' leaves ', ' arrives ', ' fled ', ' wanders ',
            ' just arrived', ' into a pile', ' into dust',
            ' moves to attack', ' moves to ',
            ' lunges ', ' bludgeons ', ' smashes ', ' swipe ', ' whap ',
            ' slashes ', ' stabs ', ' swings ', ' cleaves ', ' pounds ',
            ' drop to the ground', ' drops to the ground',
            ' walks into the room', ' just entered', ' just left',
        )
        if any(v in sl for v in _sentence_verbs):
            return False

        if self.RE_ROOM_NAME.match(s):
            return True
        if self.RE_ROOM_NAME_SHORT.match(s) and any(c.isupper() for c in s):
            return True
        if re.match(r'^[A-Z][a-zA-Z\']+$', s) and len(s) >= 4:
            return True

        return False

    def _finalize_room(self) -> None:
        # Only emit a room update if we saw "Obvious exits/paths" — confirms it's a real room
        if not self._room_has_exits:
            import logging
            logging.getLogger('mudproxy.parser').debug(
                f"Discarding room candidate (no exits): {self._room.name!r}"
            )
            self.state = ParserState.IDLE
            return
        if self._desc_lines:
            self._room.description = '\n'.join(self._desc_lines)
            # Remember this description for brief redisplays
            self._last_descriptions[self._room.name] = self._room.description
        elif self._room.name in self._last_descriptions:
            # Brief display (no description) — use the last known one
            self._room.description = self._last_descriptions[self._room.name]
        if self._room.name and self.on_room_update:
            self.on_room_update(self._room)
        self.state = ParserState.IDLE

    def _finalize_inventory(self) -> None:
        """Parse collected inventory lines into structured data."""
        self._collecting_inventory = False
        if not self._inventory_lines:
            return

        # Separate the carrying line(s) from wealth/encumbrance/keys
        carrying_lines = []
        wealth = ""
        encumbrance = ""
        in_carrying = True

        for line in self._inventory_lines:
            stripped = line.strip()
            if not stripped:
                continue
            # These end the carrying section
            if stripped.lower().startswith("you have no keys") or stripped.lower().startswith("you have the following keys"):
                in_carrying = False
                continue
            if stripped.lower().startswith("wealth:"):
                in_carrying = False
                wealth = stripped[7:].strip()
                continue
            enc_match = self.RE_ENCUMBRANCE.match(stripped)
            if enc_match:
                in_carrying = False
                encumbrance = enc_match.group(1).strip()
                continue
            if in_carrying:
                carrying_lines.append(stripped)

        # Join carrying lines into one string
        full_text = ' '.join(carrying_lines)

        # Extract "You are carrying ..." content
        inv_match = self.RE_INVENTORY.match(full_text)
        if not inv_match:
            return

        carrying_text = inv_match.group(1).rstrip('.')

        # Parse into individual tokens (split by comma, handling "and")
        carrying_text = self._replace_last_and(carrying_text)
        tokens = [t.strip() for t in carrying_text.split(',') if t.strip()]

        items = []
        currency_parts = []

        for token in tokens:
            # Check if it's currency
            if self.RE_CURRENCY.match(token):
                currency_parts.append(token)
                continue

            # Check if it's an item with slot: "black chainmail hauberk (Torso)"
            slot_match = self.RE_ITEM_SLOT.match(token)
            if slot_match:
                items.append({
                    "name": slot_match.group(1).strip(),
                    "slot": slot_match.group(2).strip(),
                })
            elif token:
                items.append({"name": token, "slot": ""})

        # Use parsed wealth or build from currency
        if not wealth and currency_parts:
            wealth = ', '.join(currency_parts)

        inv = InventoryData(
            items=items,
            wealth=wealth,
            encumbrance=encumbrance,
        )

        if self.on_inventory:
            self.on_inventory(inv)

    def _finalize_who(self) -> None:
        """Parse collected who list into player names."""
        self._collecting_who = False
        self._who_seen_dashes = False
        if not self._who_lines:
            return

        players = []
        for line in self._who_lines:
            # Format: "    Good Abbath            -  Divine Protector  of -Immortal-"
            # Split by " - " to get the name part before the title
            parts = line.split(' - ', 1)
            if not parts:
                continue
            name_part = parts[0].strip()
            words = name_part.split()
            if not words:
                continue
            # Skip reputation prefix (Good, Lawful, etc.)
            if words[0].lower() in self._REPUTATION_WORDS:
                words = words[1:]
            # First word after reputation is the player's first name
            if words and words[0][0].isupper():
                players.append(words[0])

        if players and self.on_who_list:
            self.on_who_list(players)
