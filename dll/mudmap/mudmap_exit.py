"""Parse MME exit strings into structured exits.

Input format examples (MME Rooms table N/S/E/W/NE/NW/SE/SW/U/D fields):
    '1/3'                          plain exit
    '1/1381 (Door)'                has a door
    '1/1381 (Door [41 picklocks/strength])'  door with pick/bash threshold
    '5/211 (Key: 42)'              needs item #42
    '5/211 (Key: 42 [or 11 picklocks/strength])'  key or pick/bash
    '3/50 (Hidden)'                hidden exit, 'search' first
    '3/50 (Hidden/Searchable)'     hidden, searchable variant
    '4/7 (Class: 3)'               class-restricted (e.g. Thief only)
    '2/15 (Race: 5)'               race-restricted
    '6/90 (Level: 20)'             level-restricted
    '7/33 (Toll: 1000)'            costs platinum
    '8/44 (Gate: 100)'             gate timer
    '9/55 (Blocked: 5)'            blocked until flag
    '1/77 (Cast: 42, 43)'          pre/post cast spells
    '1/88 (Trap)'                  has trap
    '1/607 (Text: go manhole, go man, enter manhole)'  text command exit
"""
from __future__ import annotations
import re
from dataclasses import dataclass, field

EXIT_DIRECTIONS = ("N", "S", "E", "W", "NE", "NW", "SE", "SW", "U", "D")
REVERSE_DIR = {
    "N": "S", "S": "N", "E": "W", "W": "E",
    "NE": "SW", "NW": "SE", "SE": "NW", "SW": "NE",
    "U": "D", "D": "U",
}

_EXIT_RE = re.compile(r"^\s*(\d+)\s*[/:]\s*(\d+)\s*(.*)$")
_QUAL_RE = re.compile(r"\(([^)]*)\)")
_PICKBASH_RE = re.compile(r"\[(?:or\s+)?(\d+|any)\s+picklocks(?:/strength)?\]", re.I)


@dataclass
class Exit:
    dir: str                    # 'N', 'S', etc
    map: int = 0
    room: int = 0
    door: bool = False
    gate: int = 0
    hidden: bool = False
    trap: bool = False
    key_item: int = 0
    req_item: int = 0
    class_req: int = 0
    race_req: int = 0
    level_req: int = 0
    toll: int = 0
    blocked_flag: int = 0
    cast_pre: int = 0
    cast_post: int = 0
    pick_req: int = 0
    bash_req: int = 0
    text_cmd: str = ""
    time_req: str = ""
    raw: str = ""
    flags: list[str] = field(default_factory=list)

    def is_real(self) -> bool:
        return self.map > 0 and self.room > 0

    def is_restricted(self) -> bool:
        """True if exit cannot be traversed without special handling."""
        return bool(
            self.door or self.hidden or self.key_item or self.req_item
            or self.class_req or self.race_req or self.level_req
            or self.toll or self.blocked_flag or self.cast_pre
        )


def _num(s: str) -> int:
    """Extract first integer from string, ignoring trailing junk like '[or ...]'."""
    m = re.match(r"\s*(\d+)", str(s).strip())
    return int(m.group(1)) if m else 0


def _parse_pickbash(q: str, ex: Exit) -> None:
    """Extract [N picklocks/strength] or [any picklocks/strength] from qualifier."""
    m = _PICKBASH_RE.search(q)
    if not m:
        return
    val = m.group(1).lower()
    if val == "any":
        ex.pick_req = 1
        ex.bash_req = 1 if "/strength" in m.group(0).lower() else 0
    else:
        n = int(val)
        ex.pick_req = n
        ex.bash_req = n if "/strength" in m.group(0).lower() else 0


def parse_exit(direction: str, raw) -> Exit | None:
    """Decode one exit field value for direction ('N', 'SW', etc)."""
    if raw is None:
        return None
    s = str(raw).strip()
    if not s or s == "0":
        return None
    m = _EXIT_RE.match(s)
    if not m:
        alt = re.match(r"^\s*(\d+)\s*,\s*(\d+)\s*(.*)$", s)
        if not alt:
            return None
        map_n, room_n, tail = alt.group(1), alt.group(2), alt.group(3)
    else:
        map_n, room_n, tail = m.group(1), m.group(2), m.group(3)

    ex = Exit(dir=direction, map=int(map_n), room=int(room_n), raw=s)
    for q in _QUAL_RE.findall(tail):
        q = q.strip()
        if not q:
            continue
        ex.flags.append(q)
        low = q.lower()
        if low.startswith("door"):
            ex.door = True
            _parse_pickbash(q, ex)
        elif low.startswith("hidden"):
            ex.hidden = True
        elif low.startswith("trap"):
            ex.trap = True
        elif low.startswith("gate"):
            parts = q.split(":", 1)
            ex.gate = _num(parts[1]) if len(parts) > 1 else 1
            if ex.gate == 0:
                ex.gate = 1
        elif low.startswith("key"):
            parts = q.split(":", 1)
            ex.key_item = _num(parts[1]) if len(parts) > 1 else 0
            _parse_pickbash(q, ex)
        elif low.startswith("item"):
            parts = q.split(":", 1)
            ex.req_item = _num(parts[1]) if len(parts) > 1 else 0
        elif low.startswith("class"):
            parts = q.split(":", 1)
            ex.class_req = _num(parts[1]) if len(parts) > 1 else 0
        elif low.startswith("race"):
            parts = q.split(":", 1)
            ex.race_req = _num(parts[1]) if len(parts) > 1 else 0
        elif low.startswith("level"):
            parts = q.split(":", 1)
            ex.level_req = _num(parts[1]) if len(parts) > 1 else 0
        elif low.startswith("toll"):
            parts = q.split(":", 1)
            ex.toll = _num(parts[1]) if len(parts) > 1 else 0
        elif low.startswith("block"):
            parts = q.split(":", 1)
            ex.blocked_flag = _num(parts[1]) if len(parts) > 1 else 0
        elif low.startswith("cast"):
            parts = q.split(":", 1)
            if len(parts) > 1:
                vals = [_num(x) for x in parts[1].split(",")]
                if vals:
                    ex.cast_pre = vals[0]
                if len(vals) > 1:
                    ex.cast_post = vals[1]
        elif low.startswith("text"):
            parts = q.split(":", 1)
            if len(parts) > 1:
                cmds = [c.strip() for c in parts[1].split(",")]
                if cmds and cmds[0]:
                    ex.text_cmd = cmds[0]
        elif low.startswith("time"):
            parts = q.split(":", 1)
            ex.time_req = parts[1].strip() if len(parts) > 1 else ""
    return ex


def parse_room_exits(room_row) -> list[Exit]:
    """Given a room dict, return a list of Exit objects (only real ones)."""
    out = []
    for d in EXIT_DIRECTIONS:
        ex = parse_exit(d, room_row.get(d))
        if ex and ex.is_real():
            out.append(ex)
    return out


if __name__ == "__main__":
    import mudmap_load, sys
    data = mudmap_load.load(
        sys.argv[1] if len(sys.argv) > 1
        else "/home/bucka/AI/mmud-explorer-src/syntax53/data-v1.11p.mdb"
    )
    from collections import Counter
    flag_count = Counter()
    exits_total = 0
    for _, r in data["rooms"].items():
        for ex in parse_room_exits(r):
            exits_total += 1
            for f in ex.flags:
                flag_count[f.split(":", 1)[0].lower().strip()] += 1
    print(f"total exits: {exits_total}")
    print("qualifier histogram:")
    for name, n in flag_count.most_common():
        print(f"  {name:10s} {n}")
