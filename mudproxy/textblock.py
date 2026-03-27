"""MajorMUD Text Block parser and interpreter.

Parses the TBInfo table from the MME .mdb export into a structured
representation of the game's scripting/quest system.

Text block format (from MDB TBInfo table):
  Number    — unique text block ID
  LinkTo    — next text block in chain (0 = terminal)
  Action    — newline-delimited trigger:command:command:... lines
  Called From — human-readable source (Monster #N, Room M/N, Textblock #N, Spell #N)

Action line format:
  trigger_keyword:cmd1 arg1 arg2:cmd2 arg1:...

Commands can be:
  - Conditionals: class, race, minlevel, maxlevel, checkitem, failitem,
    checkability, failability, nomonsters, needmonster, roomitem, failroomitem,
    goodaligned, evilaligned, checklives, checkspell, failspell, levelcheck
  - Actions: message, text, teleport, cast, summon, giveitem, takeitem,
    giveability, addability, removeability, setability, testability, testskill,
    addexp, adddelay, delay, price, learnspell, givecoins, droproomitem,
    clearitem, clearmob, remoteaction, addlife, addevil, random
  - Display: %s substitution (monster/player name)
"""

import json
import logging
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

logger = logging.getLogger(__name__)


# ── Data structures ──

@dataclass
class TBCommand:
    """A single command in a text block action chain."""
    cmd: str           # command name (message, teleport, cast, etc.)
    args: list[str]    # positional arguments
    raw: str = ""      # original text for debugging

    def to_dict(self) -> dict:
        d = {"cmd": self.cmd, "args": self.args}
        if self.raw:
            d["raw"] = self.raw
        return d


@dataclass
class TBTrigger:
    """A trigger keyword and its command chain."""
    keyword: str              # what the player types (e.g., "touch anvil")
    commands: list[TBCommand] # sequence of commands to execute
    raw: str = ""             # original line for debugging

    def to_dict(self) -> dict:
        return {
            "keyword": self.keyword,
            "commands": [c.to_dict() for c in self.commands],
        }


@dataclass
class TextBlock:
    """A complete text block record."""
    number: int
    link_to: int = 0              # next block in chain
    triggers: list[TBTrigger] = field(default_factory=list)
    display_text: str = ""        # raw display text (for %s blocks)
    called_from: str = ""         # source reference
    called_from_parsed: list[dict] = field(default_factory=list)

    def to_dict(self) -> dict:
        d = {
            "number": self.number,
            "link_to": self.link_to,
            "triggers": [t.to_dict() for t in self.triggers],
            "called_from": self.called_from,
            "called_from_parsed": self.called_from_parsed,
        }
        if self.display_text:
            d["display_text"] = self.display_text
        return d


# ── Known commands ──
# Commands that take specific argument patterns

CONDITIONAL_COMMANDS = {
    "class", "race", "minlevel", "maxlevel", "levelcheck",
    "checkitem", "failitem", "checkability", "failability",
    "nomonsters", "needmonster", "roomitem", "failroomitem",
    "goodaligned", "evilaligned", "checklives", "checkspell", "failspell",
}

ACTION_COMMANDS = {
    "message", "text", "teleport", "cast", "summon",
    "giveitem", "takeitem", "giveability", "addability",
    "removeability", "setability", "testability", "testskill",
    "addexp", "adddelay", "delay", "price", "learnspell",
    "givecoins", "droproomitem", "clearitem", "clearmob",
    "remoteaction", "addlife", "addevil", "random",
    "monsters",  # alternate form of needmonster
}

ALL_COMMANDS = CONDITIONAL_COMMANDS | ACTION_COMMANDS


# ── Parser ──

def _parse_called_from(cf: str) -> list[dict]:
    """Parse 'Called From' field into structured references."""
    refs = []
    if not cf:
        return refs
    for part in cf.split(","):
        part = part.strip().rstrip("+")
        if not part:
            continue
        # Monster #39, Room 5/294, Textblock #1, Spell #5
        m = re.match(r'(Monster|Spell|Textblock)\s*#?(\d+)', part)
        if m:
            refs.append({"type": m.group(1).lower(), "id": int(m.group(2))})
            continue
        m = re.match(r'Room\s+(\d+)/(\d+)', part)
        if m:
            refs.append({"type": "room", "map": int(m.group(1)), "room": int(m.group(2))})
            continue
        # Textblock #5435(?%)
        m = re.match(r'Textblock\s*#?(\d+)\(\?\%\)', part)
        if m:
            refs.append({"type": "textblock", "id": int(m.group(1)), "random": True})
            continue
        # Fallback
        refs.append({"type": "unknown", "raw": part})
    return refs


def _parse_command(raw: str) -> TBCommand:
    """Parse a single command string like 'teleport 724 8' or 'message 1231'."""
    raw = raw.strip()
    if not raw:
        return TBCommand(cmd="noop", args=[], raw=raw)

    parts = raw.split()
    cmd = parts[0].lower()
    args = parts[1:]

    # Check if this is actually a bare number (continuation of previous command)
    if cmd.isdigit():
        return TBCommand(cmd="message", args=[cmd], raw=raw)

    return TBCommand(cmd=cmd, args=args, raw=raw)


def _parse_action_line(line: str) -> TBTrigger | None:
    """Parse a single action line like 'touch anvil:roomitem 767 1114:summon 316'."""
    line = line.strip()
    if not line:
        return None

    parts = line.split(":")
    if not parts:
        return None

    keyword = parts[0].strip()
    commands = []

    for part in parts[1:]:
        part = part.strip()
        if not part:
            continue
        commands.append(_parse_command(part))

    return TBTrigger(keyword=keyword, commands=commands, raw=line)


def _is_display_text(action: str) -> bool:
    """Check if the action field is display text rather than a trigger:command chain."""
    # Display text blocks have %s substitution and no colon-delimited commands
    # They also often start directly with text content
    stripped = action.strip()
    if not stripped:
        return False
    # If it contains %s and no recognized command pattern, it's display text
    if "%s" in stripped and ":" not in stripped:
        return True
    # If it starts with ANSI escape, it's display text
    if stripped.startswith("\x1b["):
        return True
    # If the first "part" before colon is very long, it's likely display text
    first = stripped.split(":")[0] if ":" in stripped else stripped
    # Check if first part contains a recognized command
    first_word = first.split()[0].lower() if first.split() else ""
    if first_word not in ALL_COMMANDS and ":" not in stripped:
        return True
    return False


def parse_textblock(number: int, link_to: int, action: str, called_from: str) -> TextBlock:
    """Parse a single text block record into structured form."""
    tb = TextBlock(
        number=number,
        link_to=link_to,
        called_from=called_from,
        called_from_parsed=_parse_called_from(called_from),
    )

    if not action or not action.strip():
        return tb

    # Check if this is a display text block (monster flavor text with %s)
    if _is_display_text(action):
        tb.display_text = action.strip()
        return tb

    # Parse as trigger:command lines
    for line in action.split("\n"):
        trigger = _parse_action_line(line)
        if trigger:
            tb.triggers.append(trigger)

    return tb


# ── MDB loader ──

def load_from_mdb(mdb_path: str) -> list[TextBlock]:
    """Load all text blocks from an MME .mdb file."""
    from access_parser import AccessParser

    db = AccessParser(mdb_path)
    tb_data = db.parse_table("TBInfo")
    cols = list(tb_data.keys())
    n = len(tb_data[cols[0]]) if cols else 0

    blocks = []
    for i in range(n):
        row = {c: tb_data[c][i] for c in cols}
        # Decode bytes
        for k, v in row.items():
            if isinstance(v, bytes):
                row[k] = v.decode("latin-1", errors="replace").rstrip("\x00")

        tb = parse_textblock(
            number=row.get("Number", 0),
            link_to=row.get("LinkTo", 0),
            action=str(row.get("Action", "")),
            called_from=str(row.get("Called From", "")),
        )
        blocks.append(tb)

    logger.info(f"Loaded {len(blocks)} text blocks from {mdb_path}")
    return blocks


def export_json(blocks: list[TextBlock], outpath: str | Path) -> None:
    """Export text blocks to JSON."""
    data = [b.to_dict() for b in blocks]
    Path(outpath).write_text(json.dumps(data, indent=2))
    logger.info(f"Exported {len(data)} text blocks to {outpath}")


# ── Resolver / interpreter helpers ──

class TextBlockDB:
    """In-memory text block database with resolution and traversal."""

    def __init__(self, blocks: list[TextBlock]):
        self._by_number: dict[int, TextBlock] = {}
        self._by_monster: dict[int, list[TextBlock]] = {}
        self._by_room: dict[tuple[int, int], list[TextBlock]] = {}
        self._by_spell: dict[int, list[TextBlock]] = {}

        for b in blocks:
            self._by_number[b.number] = b
            for ref in b.called_from_parsed:
                if ref["type"] == "monster":
                    self._by_monster.setdefault(ref["id"], []).append(b)
                elif ref["type"] == "room":
                    key = (ref["map"], ref["room"])
                    self._by_room.setdefault(key, []).append(b)
                elif ref["type"] == "spell":
                    self._by_spell.setdefault(ref["id"], []).append(b)

    def get(self, number: int) -> TextBlock | None:
        return self._by_number.get(number)

    def get_chain(self, number: int, max_depth: int = 20) -> list[TextBlock]:
        """Follow a text block chain via LinkTo references."""
        chain = []
        seen = set()
        current = number
        while current and current not in seen and len(chain) < max_depth:
            seen.add(current)
            tb = self._by_number.get(current)
            if not tb:
                break
            chain.append(tb)
            current = tb.link_to
        return chain

    def get_for_monster(self, monster_id: int) -> list[TextBlock]:
        return self._by_monster.get(monster_id, [])

    def get_for_room(self, map_id: int, room_id: int) -> list[TextBlock]:
        return self._by_room.get((map_id, room_id), [])

    def get_for_spell(self, spell_id: int) -> list[TextBlock]:
        return self._by_spell.get(spell_id, [])

    def resolve_triggers(self, number: int) -> list[dict]:
        """Get all trigger keywords available from a text block and its chain."""
        triggers = []
        for tb in self.get_chain(number):
            for t in tb.triggers:
                triggers.append({
                    "keyword": t.keyword,
                    "block": tb.number,
                    "commands": [c.to_dict() for c in t.commands],
                })
        return triggers

    def describe_block(self, number: int) -> str:
        """Human-readable description of a text block chain for LLM context."""
        chain = self.get_chain(number)
        if not chain:
            return f"Text block #{number}: not found"

        lines = []
        for tb in chain:
            lines.append(f"=== Text Block #{tb.number} ===")
            if tb.called_from:
                lines.append(f"  Source: {tb.called_from}")
            if tb.display_text:
                lines.append(f"  Display: {tb.display_text}")
            if tb.link_to:
                lines.append(f"  Chain → #{tb.link_to}")
            for t in tb.triggers:
                cmd_strs = []
                for c in t.commands:
                    cmd_strs.append(f"{c.cmd}({', '.join(c.args)})" if c.args else c.cmd)
                lines.append(f"  [{t.keyword}] → {' → '.join(cmd_strs)}")

        return "\n".join(lines)

    @property
    def stats(self) -> dict:
        total = len(self._by_number)
        with_triggers = sum(1 for b in self._by_number.values() if b.triggers)
        with_display = sum(1 for b in self._by_number.values() if b.display_text)
        terminal = sum(1 for b in self._by_number.values() if b.link_to == 0)
        return {
            "total": total,
            "with_triggers": with_triggers,
            "with_display_text": with_display,
            "terminal": terminal,
            "monsters": len(self._by_monster),
            "rooms": len(self._by_room),
            "spells": len(self._by_spell),
        }


# ── CLI ──

if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.INFO)

    if len(sys.argv) < 2:
        print("Usage: python -m mudproxy.textblock <path-to-mdb> [output.json]")
        sys.exit(1)

    mdb_path = sys.argv[1]
    blocks = load_from_mdb(mdb_path)
    db = TextBlockDB(blocks)

    print(f"\nText Block Stats:")
    for k, v in db.stats.items():
        print(f"  {k}: {v}")

    # Show some examples
    print("\n--- Example: Monster #39 text blocks ---")
    for tb in db.get_for_monster(39):
        print(db.describe_block(tb.number))

    print("\n--- Example: Touch hydra (TB #476) ---")
    print(db.describe_block(476))

    if len(sys.argv) >= 3:
        export_json(blocks, sys.argv[2])
