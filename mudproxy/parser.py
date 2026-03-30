import re
from enum import Enum, auto
from dataclasses import dataclass, field
from typing import Optional, Callable

# Strip ANSI escape sequences (e.g. [K, [0m, [1;32m, ESC[...)
RE_ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]|\[[ -/]*[A-Za-z]')

# Known exit directions in MajorMUD
_EXIT_DIRECTIONS = {
    'north', 'south', 'east', 'west',
    'up', 'down',
    'northeast', 'northwest', 'southeast', 'southwest',
    'ne', 'nw', 'se', 'sw',
}

def normalize_exit(exit_str: str) -> str:
    """Extract the direction from an exit string.

    MajorMUD exits can have descriptors before the direction:
      'open door south' -> 'south'
      'rocky passageway east' -> 'east'
      'hidden staircase down' -> 'down'
      'open gate west' -> 'west'
    The direction is always the last word.
    """
    words = exit_str.strip().split()
    if words and words[-1].lower() in _EXIT_DIRECTIONS:
        return words[-1].lower()
    return exit_str


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
    currency: dict = field(default_factory=dict)  # {platinum: 14, gold: 1191, ...}


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
    # Miss/dodge/fumble — also signify a combat round tick
    RE_COMBAT_MISS = re.compile(
        r'(?:misses?\s|dodges?\s|fumbles?\s|blocks?\s|bounces?\s+off|parries?\s)',
        re.IGNORECASE
    )
    # Player swing miss: "You swing at slaver", "You swipe at slaver"
    RE_PLAYER_SWING = re.compile(
        r'^You \w+ at .+',
        re.IGNORECASE
    )
    # Glancing blow miss: "Your sword glances off slaver"
    RE_GLANCES_OFF = re.compile(
        r'^Your .+ glances off .+',
        re.IGNORECASE
    )
    RE_ALREADY_CAST = re.compile(
        r'^You have already cast a spell this round',
        re.IGNORECASE
    )
    RE_SPELL_CAST = re.compile(
        r'^You (?:cast|sing|invoke) ',
        re.IGNORECASE
    )
    RE_SPELL_FAIL = re.compile(
        r'^You attempt to cast ',
        re.IGNORECASE
    )
    RE_XP_GAIN = re.compile(r'^You gain (\d+) experience', re.IGNORECASE)
    RE_EXP_LINE = re.compile(
        r'^Exp:\s*([\d,]+)\s+Level:\s*(\d+)\s+Exp needed for next level:\s*([\d,]+)\s+\(([\d,]+)\)\s+\[(\d+)%\]',
        re.IGNORECASE
    )
    RE_COIN_DROP = re.compile(
        r'^(\d+)\s+(copper|silver|gold|platinum|runic)\s+drops?\s+to\s+the\s+ground',
        re.IGNORECASE
    )
    RE_YOU_DIED = re.compile(r'(?:You die|You have been killed|You are dead)', re.IGNORECASE)

    # Coin transfer patterns
    RE_COIN_DROPPED = re.compile(
        r'^You dropped (\d+)\s+(copper|silver|gold|platinum|runic)',
        re.IGNORECASE
    )
    RE_COIN_PICKED_UP = re.compile(
        r'^You (?:picked up|pick up) (\d+)\s+(copper|silver|gold|platinum|runic)',
        re.IGNORECASE
    )
    RE_COIN_GAVE = re.compile(
        r'^You (?:gave|give) (\d+)\s+(copper|silver|gold|platinum|runic)\s+\w+\s+to\s+(.+?)\.?\s*$',
        re.IGNORECASE
    )
    RE_COIN_RECEIVED = re.compile(
        r'^(.+?)\s+(?:gives|gave)\s+you\s+(\d+)\s+(copper|silver|gold|platinum|runic)',
        re.IGNORECASE
    )

    # Item pickup/drop/equip patterns
    RE_ITEM_TOOK = re.compile(r'^You took ', re.IGNORECASE)
    RE_ITEM_DROPPED = re.compile(r'^You dropped ', re.IGNORECASE)
    RE_ITEM_EQUIPPED = re.compile(r'^You are now (?:wearing|holding) ', re.IGNORECASE)
    RE_ITEM_REMOVED = re.compile(r'^You have removed ', re.IGNORECASE)
    RE_COIN_DEPOSITED = re.compile(
        r'^You deposit(?:ed)?\s+(\d+)\s+copper',
        re.IGNORECASE
    )

    # Inventory line: "You are carrying ..."
    RE_INVENTORY = re.compile(r'^You are carrying\s+(.+)', re.IGNORECASE)
    # Currency patterns in inventory
    RE_CURRENCY = re.compile(
        r'(\d+)\s+(platinum\s+pieces?|gold\s+crowns?|silver\s+nobles?|'
        r'copper\s+farthings?|runic\s+coins?|runic\s+copper)',
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

        # Top list collection state
        self._collecting_top = False
        self._top_lines: list[str] = []
        self._top_seen_dashes = False
        self._char_name_lower = ""  # first name, lowercase, for filtering own echoes
        self._room_has_exits = False  # True once "Obvious exits/paths" seen for current room

        # Pro command collection state
        self._collecting_pro = False
        self._pro_lines: list[str] = []

        # Stat advanced collection state
        self._collecting_stat_adv = False
        self._stat_adv_lines: list[str] = []

        # Spells/Powers collection state
        self._collecting_spells = False
        self._spell_lines: list[str] = []
        self._spell_type = "spells"  # "spells" or "powers" (kai)

        self.on_room_update: Optional[Callable[[RoomData], None]] = None
        self.on_coin_drop: Optional[Callable[[int, str], None]] = None  # (amount, type)
        self.on_coin_transfer: Optional[Callable[[dict], None]] = None  # {action, amount, coin_type, player?}
        self.on_item_transfer: Optional[Callable[[dict], None]] = None  # {action, name}
        self.on_already_cast: Optional[Callable[[], None]] = None  # "already cast this round"
        self.on_spell_cast: Optional[Callable[[dict], None]] = None  # {success: bool}

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

    # Wound status pattern: "The goblin appears to be heavily wounded."
    RE_WOUND_STATUS = re.compile(
        r'^(.+?)\s+appears?\s+to\s+be\s+'
        r'(very critically wounded|critically wounded|severely wounded|'
        r'heavily wounded|moderately wounded|slightly wounded|unwounded)\.',
        re.IGNORECASE
    )

    # Chat message patterns — match actual MajorMUD output formats
    # Gossip: "PlayerName gossips: msg" / "You gossip: msg"
    RE_GOSSIP = re.compile(r'^(\w+)\s+gossips?:\s*(.+)$', re.IGNORECASE)
    # Broadcast: "Broadcast from PlayerName "msg""
    RE_BROADCAST = re.compile(r'^Broadcast from (\w+)\s+"(.+)"$', re.IGNORECASE)
    # Telepath: "PlayerName telepaths: msg" / "You telepath: msg"
    RE_TELEPATH = re.compile(r'^(\w+)\s+telepaths?\s+to\s+you:\s*(.+)$', re.IGNORECASE)
    RE_TELEPATH2 = re.compile(r'^(\w+)\s+telepaths?:\s*(.+)$', re.IGNORECASE)
    # Gangpath: "PlayerName gangpaths: msg" / "You gangpath: msg"
    RE_GANGPATH = re.compile(r'^(\w+)\s+gangpaths?:\s*(.+)$', re.IGNORECASE)
    # Auction: "PlayerName auctions: msg" / "You auction: msg"
    RE_AUCTION = re.compile(r'^(\w+)\s+auctions?:\s*(.+)$', re.IGNORECASE)
    # Say: 'PlayerName says "msg"' / 'You say "msg"'
    RE_SAY = re.compile(r'^(\w+)\s+says?\s+"(.+)"$', re.IGNORECASE)

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
            if self._collecting_top:
                self._finalize_top()
            if self._collecting_pro:
                self._finalize_pro()
            if self._collecting_stat_adv:
                self._finalize_stat_adv()
            if self._collecting_spells:
                self._finalize_spells()
            self.state = ParserState.IDLE
            # Check for text after HP prompt (e.g. "[HP=88/MA=27]:Room Name")
            remainder = line[hp_match.end():].strip()
            if remainder:
                self.feed_line(remainder)
            return

        # Chat message detection (gangpath, broadcast, telepath, say)
        if self.on_chat:
            stripped = line.strip()
            low = stripped.lower()
            for pat, channel in (
                (self.RE_BROADCAST, "broadcast"),
                (self.RE_GOSSIP, "gossip"),
                (self.RE_GANGPATH, "gangpath"),
                (self.RE_TELEPATH, "telepath"),
                (self.RE_TELEPATH2, "telepath"),
                (self.RE_AUCTION, "auction"),
                (self.RE_SAY, "say"),
            ):
                m = pat.match(stripped)
                if m:
                    sender, message = m.group(1), m.group(2)
                    self.on_chat(sender, message, channel)
                    break

        # Collecting inventory lines (multi-line "You are carrying ...")
        if self._collecting_inventory:
            if not line.strip():
                self._finalize_inventory()
                return
            # HP prompt means inventory output ended
            if self.RE_HP_PROMPT.match(line):
                self._finalize_inventory()
                # Fall through to process HP line normally
            else:
                self._inventory_lines.append(line)
                return

        # Collecting who list
        if self._collecting_who:
            if not line.strip() and self._who_seen_dashes:
                self._finalize_who()
                return
            # HP prompt means WHO output ended (no trailing blank line)
            if self.RE_HP_PROMPT.match(line):
                self._finalize_who()
                # Fall through to process the HP line normally
            elif '---' in line:
                self._who_seen_dashes = True
                return
            elif self._who_seen_dashes and line.strip():
                self._who_lines.append(line.strip())
                return
            else:
                return

        # Collecting top list
        if self._collecting_top:
            if not line.strip() and self._top_seen_dashes:
                self._finalize_top()
                return
            if self.RE_HP_PROMPT.match(line):
                self._finalize_top()
                # Fall through to process HP line normally
            elif '=-=-' in line:
                self._top_seen_dashes = True
                return
            elif self._top_seen_dashes and line.strip():
                self._top_lines.append(line.strip())
                return
            else:
                return

        # Detect leaving MajorMUD (back to BBS menu, relog, etc.)
        if self._in_game:
            stripped_lower = line.strip().lower()
            if (stripped_lower.startswith("enter your selection") or
                stripped_lower.startswith("please make a selection") or
                stripped_lower.startswith("enter your name") or
                stripped_lower.startswith("enter your password") or
                stripped_lower == "goodbye" or
                stripped_lower.startswith("re-enter the game")):
                self._in_game = False
                import logging
                logging.getLogger('mudproxy.parser').info(
                    f"Left game detected: {line.strip()!r}")

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

        # Start top list collection
        if line.strip().startswith("Top Heroes of the Realm"):
            self._collecting_top = True
            self._top_lines = []
            self._top_seen_dashes = False
            return

        # Start pro command collection
        if line.strip().startswith("Player ID:"):
            self._collecting_pro = True
            self._pro_lines = [line.strip()]
            return

        # Continue pro collection
        if self._collecting_pro:
            stripped = line.strip()
            if not stripped:
                self._finalize_pro()
                return
            # HP prompt means pro output ended
            if self.RE_HP_PROMPT.match(line):
                self._finalize_pro()
                # Fall through to process HP line normally
            else:
                self._pro_lines.append(stripped)
                return

        # Start stat advanced collection (triggered by "HP Regen:" which is unique to st a)
        if re.match(r'^HP Regen:\s', line.strip()):
            self._collecting_stat_adv = True
            self._stat_adv_lines = [line.strip()]
            return

        # Continue stat advanced collection
        if self._collecting_stat_adv:
            stripped = line.strip()
            if not stripped:
                self._finalize_stat_adv()
                return
            if self.RE_HP_PROMPT.match(line):
                self._finalize_stat_adv()
                # Fall through to process HP line normally
            else:
                self._stat_adv_lines.append(stripped)
                return

        # Start spells/powers collection
        if line.strip() == 'You have the following spells:':
            self._collecting_spells = True
            self._spell_lines = []
            self._spell_type = "spells"
            return
        if line.strip() == 'You have the following powers:':
            self._collecting_spells = True
            self._spell_lines = []
            self._spell_type = "powers"
            return

        # Continue spells collection
        if self._collecting_spells:
            stripped = line.strip()
            # Skip the header line
            if stripped.startswith('Level') and ('Mana' in stripped or 'Kai' in stripped):
                return
            if not stripped:
                self._finalize_spells()
                return
            if self.RE_HP_PROMPT.match(line):
                self._finalize_spells()
                # Fall through to process HP line normally
            else:
                self._spell_lines.append(stripped)
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
            self._room.exits = [normalize_exit(e.strip().rstrip('.'))
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

        # Coin drops: "4 gold drop to the ground."
        coin_match = self.RE_COIN_DROP.match(line)
        if coin_match:
            if self.on_coin_drop:
                self.on_coin_drop(int(coin_match.group(1)), coin_match.group(2).lower())
            return

        # Coin transfers: drop, pickup, give, receive, deposit
        dropped = self.RE_COIN_DROPPED.match(line)
        if dropped:
            if self.on_coin_transfer:
                self.on_coin_transfer({"action": "drop", "amount": int(dropped.group(1)),
                                       "coin_type": dropped.group(2).lower()})
            return
        picked = self.RE_COIN_PICKED_UP.match(line)
        if picked:
            if self.on_coin_transfer:
                self.on_coin_transfer({"action": "pickup", "amount": int(picked.group(1)),
                                       "coin_type": picked.group(2).lower()})
            return
        gave = self.RE_COIN_GAVE.match(line)
        if gave:
            if self.on_coin_transfer:
                self.on_coin_transfer({"action": "give", "amount": int(gave.group(1)),
                                       "coin_type": gave.group(2).lower(), "player": gave.group(3).strip()})
            return
        received = self.RE_COIN_RECEIVED.match(line)
        if received:
            if self.on_coin_transfer:
                self.on_coin_transfer({"action": "receive", "amount": int(received.group(2)),
                                       "coin_type": received.group(3).lower(), "player": received.group(1).strip()})
            return
        deposited = self.RE_COIN_DEPOSITED.match(line)
        if deposited:
            if self.on_coin_transfer:
                self.on_coin_transfer({"action": "deposit", "amount": int(deposited.group(1)),
                                       "coin_type": "copper"})
            return

        # Item pickup/drop
        if self.RE_ITEM_TOOK.match(line):
            if self.on_item_transfer:
                name = line[9:].rstrip('. ')
                self.on_item_transfer({"action": "pickup", "name": name})
            return
        if self.RE_ITEM_DROPPED.match(line):
            if self.on_item_transfer:
                name = line[13:].rstrip('. ')
                self.on_item_transfer({"action": "drop", "name": name})
            return
        if self.RE_ITEM_EQUIPPED.match(line):
            if self.on_item_transfer:
                # "You are now wearing X." or "You are now holding X."
                name = line.split(' ', 4)[-1].rstrip('. ')
                self.on_item_transfer({"action": "equip", "name": name})
            return
        if self.RE_ITEM_REMOVED.match(line):
            if self.on_item_transfer:
                name = line[18:].rstrip('. ')
                self.on_item_transfer({"action": "unequip", "name": name})
            return

        # Spell cast success (You cast / You sing)
        if self.RE_SPELL_CAST.match(line):
            if self.on_spell_cast:
                self.on_spell_cast({"success": True})
            return

        # Spell cast fail (You attempt to cast)
        if self.RE_SPELL_FAIL.match(line):
            if self.on_spell_cast:
                self.on_spell_cast({"success": False})
            return

        # Spell blocked (same-round cast attempt)
        if self.RE_ALREADY_CAST.match(line):
            if self.on_already_cast:
                self.on_already_cast()
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

        # Combat miss/dodge/fumble — also a round tick
        if self.RE_COMBAT_MISS.search(line):
            if self.on_combat:
                self.on_combat(line)
            return

        # Player swing miss: "You swing at slaver"
        if self.RE_PLAYER_SWING.match(line):
            if self.on_combat:
                self.on_combat(line)
            return

        # Glancing blow (DR): "Your sword glances off slaver"
        if self.RE_GLANCES_OFF.match(line):
            if self.on_combat:
                self.on_combat(line)
            return

        # Wound status from look: "The goblin appears to be heavily wounded."
        wound_match = self.RE_WOUND_STATUS.match(line)
        if wound_match:
            monster_name = wound_match.group(1).strip()
            wound_level = wound_match.group(2).strip().lower()
            if self.on_wound_status:
                self.on_wound_status(monster_name, wound_level)
            return

        # XP gain
        xp_match = self.RE_XP_GAIN.match(line)
        if xp_match:
            if self.on_xp:
                self.on_xp(int(xp_match.group(1)))
            return

        # Exp command output: "Exp: 4660532 Level: 15 Exp needed for next level: 0 (4562569) [102%]"
        exp_match = self.RE_EXP_LINE.match(line)
        if exp_match:
            if hasattr(self, 'on_exp_status') and self.on_exp_status:
                self.on_exp_status({
                    "exp": int(exp_match.group(1).replace(',', '')),
                    "level": int(exp_match.group(2)),
                    "needed": int(exp_match.group(3).replace(',', '')),
                    "total_for_level": int(exp_match.group(4).replace(',', '')),
                    "percent": int(exp_match.group(5)),
                })
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

        # Parse currency_parts into structured dict: {platinum: 14, gold: 1191, ...}
        currency = {}
        for cp in currency_parts:
            m = self.RE_CURRENCY.match(cp)
            if m:
                amount = int(m.group(1))
                coin_name = m.group(2).lower()
                # Map coin name to type
                if 'platinum' in coin_name:
                    currency['platinum'] = amount
                elif 'gold' in coin_name:
                    currency['gold'] = amount
                elif 'silver' in coin_name:
                    currency['silver'] = amount
                elif 'runic' in coin_name:
                    currency['runic'] = amount
                elif 'copper' in coin_name:
                    currency['copper'] = amount

        inv = InventoryData(
            items=items,
            wealth=wealth,
            encumbrance=encumbrance,
            currency=currency,
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

    def _finalize_top(self) -> None:
        """Parse collected top list into player names + classes."""
        self._collecting_top = False
        self._top_seen_dashes = False
        if not self._top_lines:
            return

        # Format: "  1. Farmer                Druid      Feckless Mortals    44097400000"
        # Fixed-width: rank(~6) name(~22) class(~11) gang(~20) exp
        import re
        players = []  # (full_name, class_name)
        for line in self._top_lines:
            # Strip rank number + dot
            m = re.match(r'\s*\d+\.\s+(.+)', line)
            if not m:
                continue
            rest = m.group(1)
            # The class is a single known word — scan for it
            # Split into tokens and find the class name
            # Strategy: everything before the class word is the name,
            # the class word itself, then gang + exp after
            tokens = rest.split()
            if len(tokens) < 2:
                continue
            # Find class token — scan from position 1 onwards
            name_parts = []
            class_name = None
            for i, tok in enumerate(tokens):
                if tok.lower() in self._MAJORMUD_CLASSES and i >= 1:
                    class_name = tok
                    break
                name_parts.append(tok)
            if class_name and name_parts:
                full_name = ' '.join(name_parts)
                players.append((full_name, class_name))

        if players and self.on_top_list:
            self.on_top_list(players)

    _MAJORMUD_CLASSES = {
        "warrior", "witchunter", "paladin", "cleric", "priest",
        "missionary", "ninja", "thief", "bard", "gypsy",
        "warlock", "mage", "druid", "ranger", "mystic",
    }

    def _finalize_pro(self) -> None:
        """Parse collected pro command output into a structured dict."""
        self._collecting_pro = False
        if not self._pro_lines:
            return

        pro_data = {}
        recent_deaths = []
        in_deaths = False

        for line in self._pro_lines:
            if line.startswith("Recent Deaths:"):
                in_deaths = True
                continue
            if in_deaths:
                # Death lines: "3/22/2026 11:22 PM - thief - 9/150"
                recent_deaths.append(line)
                continue

            # Key: Value parsing
            if ':' in line:
                key, _, val = line.partition(':')
                key = key.strip()
                val = val.strip()
                if key == "Player ID":
                    pro_data["player_id"] = int(val) if val.isdigit() else val
                elif key == "Location":
                    # "9,13" or "Silvermere, Town Square (9,13)"
                    import re
                    loc_match = re.search(r'(\d+),(\d+)', val)
                    if loc_match:
                        pro_data["map_num"] = int(loc_match.group(1))
                        pro_data["room_num"] = int(loc_match.group(2))
                    pro_data["location_raw"] = val
                elif key == "Regen Time":
                    pro_data["regen_time"] = val
                elif key == "Room Illu":
                    pro_data["room_illumination"] = val
                elif key == "EPs":
                    pro_data["eps"] = val
                elif key == "Min. EPs":
                    pro_data["min_eps"] = val
                elif key == "Display Mode":
                    pro_data["display_mode"] = val
                elif key == "Statusline":
                    pro_data["statusline"] = val
                elif key == "Broadcast Channel":
                    pro_data["broadcast_channel"] = val
                elif key == "Style":
                    pro_data["style"] = val
                else:
                    # Store anything else with normalized key
                    pro_data[key.lower().replace(' ', '_')] = val

        if recent_deaths:
            pro_data["recent_deaths"] = recent_deaths

        if pro_data and hasattr(self, 'on_pro') and self.on_pro:
            self.on_pro(pro_data)

    def _finalize_stat_adv(self) -> None:
        """Parse collected 'stat advanced' output into a structured dict."""
        self._collecting_stat_adv = False
        if not self._stat_adv_lines:
            return

        data = {
            "resistances": {},
            "attacks": [],
            "spells": [],
        }

        in_attacks = False
        in_spells = False
        attack_header_seen = False
        spell_header_seen = False

        for line in self._stat_adv_lines:
            # Section headers
            if line.startswith("Attacks:"):
                in_attacks = True
                in_spells = False
                attack_header_seen = False
                continue
            if line.startswith("Spells:"):
                in_spells = True
                in_attacks = False
                spell_header_seen = False
                continue

            if in_attacks:
                # Skip the column header line (Type, Swings, Accy, ...)
                if line.startswith("Type"):
                    attack_header_seen = True
                    continue
                if not attack_header_seen:
                    continue
                parts = line.split()
                if len(parts) >= 6:
                    # Extract QnD(Total) if present
                    qnd_total = None
                    avg_rnd = None
                    for p in parts:
                        if '(' in p and ')' in p and qnd_total is None:
                            qnd_total = p
                        elif '(' in p and ')' in p:
                            avg_rnd = p
                    data["attacks"].append({
                        "type": parts[0],
                        "swings": parts[1],
                        "accuracy": int(parts[2]) if parts[2].isdigit() else parts[2],
                        "min": int(parts[3]) if parts[3].isdigit() else parts[3],
                        "max": int(parts[4]) if parts[4].isdigit() else parts[4],
                        "qnd_total": qnd_total,
                        "avg_rnd": avg_rnd,
                    })
                continue

            if in_spells:
                # Skip column header
                if line.startswith("Short Name"):
                    spell_header_seen = True
                    continue
                if not spell_header_seen:
                    continue
                parts = line.split()
                if len(parts) >= 4:
                    avg_rnd = None
                    for p in parts:
                        if p.isdigit() and avg_rnd is None:
                            pass  # skip
                    data["spells"].append({
                        "name": parts[0],
                        "casts": int(parts[1]) if parts[1].isdigit() else parts[1],
                        "difficulty": int(parts[2]) if parts[2].isdigit() else parts[2],
                        "min": int(parts[3]) if parts[3].isdigit() else parts[3],
                        "max": int(parts[4]) if len(parts) > 4 and parts[4].isdigit() else 0,
                        "avg_rnd": parts[-1] if len(parts) > 5 else None,
                    })
                continue

            # Key-value pairs from the stat block (multiple per line)
            # Format: "HP Regen:   3/9        AC vs Evil:  39           Cold Resist:     0"
            # Split on multiple spaces to find key:value pairs
            pairs = re.findall(r'([A-Za-z][A-Za-z .]+?):\s+(\S+)', line)
            for key, val in pairs:
                key = key.strip()
                norm_key = key.lower().replace(' ', '_').replace('.', '')
                # Resistances
                if 'resist' in norm_key:
                    data["resistances"][key.replace(" Resist", "")] = int(val) if val.lstrip('-').isdigit() else val
                elif norm_key == 'hp_regen':
                    data["hp_regen"] = val
                elif norm_key == 'ma_regen':
                    data["ma_regen"] = val
                elif norm_key == 'max_hp':
                    data["max_hp_bonus"] = int(val) if val.lstrip('-').isdigit() else val
                elif norm_key == 'max_mana':
                    data["max_mana_bonus"] = int(val) if val.lstrip('-').isdigit() else val
                elif norm_key == 'encum':
                    data["encumbrance"] = int(val) if val.isdigit() else val
                elif norm_key == 'dodge':
                    data["dodge"] = int(val) if val.isdigit() else val
                elif norm_key == 'crits':
                    data["crits"] = int(val) if val.isdigit() else val
                elif norm_key == 'spell_damage':
                    data["spell_damage"] = int(val) if val.lstrip('-').isdigit() else val
                elif norm_key == 'ac_vs_evil':
                    data["ac_vs_evil"] = int(val) if val.isdigit() else val
                elif norm_key == 'vs_good':
                    data["ac_vs_good"] = int(val) if val.isdigit() else val
                elif norm_key == 'shadow':
                    data["shadow"] = int(val) if val.isdigit() else val
                elif norm_key == 'party':
                    data["party"] = int(val) if val.isdigit() else val
                elif norm_key == 'prev':
                    data["prev"] = int(val) if val.isdigit() else val
                elif norm_key == 'prgd':
                    data["prgd"] = int(val) if val.isdigit() else val
                else:
                    data[norm_key] = int(val) if val.lstrip('-').isdigit() else val

        if data and hasattr(self, 'on_stat_advanced') and self.on_stat_advanced:
            self.on_stat_advanced(data)

    def _finalize_spells(self) -> None:
        """Parse collected 'spells' or 'powers' output into a structured list."""
        self._collecting_spells = False
        if not self._spell_lines:
            return

        # Format from DLL: "%3d   %-4d %4.4s  %-30.30s"
        # Actual: "  1   2    lore  song of lore"
        spells = []
        re_spell = re.compile(
            r'^\s*(\d+)\s+(\d+)\s+(\w{2,5})\s+(.+)$'
        )
        for line in self._spell_lines:
            m = re_spell.match(line)
            if m:
                spells.append({
                    "level": int(m.group(1)),
                    "cost": int(m.group(2)),
                    "short": m.group(3).strip(),
                    "name": m.group(4).strip(),
                })

        if spells and hasattr(self, 'on_spellbook') and self.on_spellbook:
            self.on_spellbook({
                "type": self._spell_type,  # "spells" or "powers"
                "cost_type": "mana" if self._spell_type == "spells" else "kai",
                "spells": spells,
            })
