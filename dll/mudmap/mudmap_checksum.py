"""Compute MegaMUD room checksums.

Ported from MMUD Explorer `Get_MegaMUD_RoomHash` + `Get_MegaMUD_ExitsCode`
(modMain.bas:7432 and :7526).

Full checksum (8 hex) = room_name_hash (3 hex) + exits_code (5 hex)

Room name hash (modMain.bas:7552-7573):
    dwTmp = 0
    for i, c in enumerate(name, start=1):
        dwTmp += i * ord(c)
    if has_whirling_vortex: dwTmp += 1   # placed-item check
    if has_obsidian_obelisk: dwTmp += 2
    hash = dwTmp & 0xFFF                  # 12-bit

Exits code (modMain.bas:7432-7516):
    Five accumulators indexed by direction position (1..5):
        pos 1: U/D, pos 2: SE/SW, pos 3: NE/NW, pos 4: E/W, pos 5: N/S
    Value per exit:
        N/E/NE/SE/U = 1;  S/W/NW/SW/D = 4
        Value is doubled if the exit is a Door/Key/Gate (bDoor = True)
    Exits that are Hidden/Text/Action are NOT counted (GoTo exit_not_seen).
    Output: concatenated hex digits of accumulators 1..5.

Note: Get_MegaMUD_ExitsCode in MME concatenates 5 hex strings of potentially
variable length — but in practice each accumulator fits in one hex digit
(max value with all 4 combined exits all being doors = 4+4 = 8, then *2
for door = 16 = 0x10, so up to 2 hex chars is possible per slot). To
match MME exactly we do the same variable-width concatenation.
"""
from __future__ import annotations
from mudmap_exit import parse_exit, EXIT_DIRECTIONS

# Position (1..5) each direction goes into
DIR_POSITION = {
    "N": 5, "S": 5,
    "E": 4, "W": 4,
    "NE": 3, "NW": 3,
    "SE": 2, "SW": 2,
    "U": 1, "D": 1,
}

# Base value per direction (before doubling for doors)
DIR_VALUE = {
    "N": 1, "E": 1, "NE": 1, "SE": 1, "U": 1,
    "S": 4, "W": 4, "NW": 4, "SW": 4, "D": 4,
}


def room_name_hash(name: str, has_whirling_vortex: bool = False,
                   has_obsidian_obelisk: bool = False) -> str:
    """3-hex-char name hash (Get_MegaMUD_RoomHash)."""
    dw = 0
    for i, ch in enumerate(name or "", start=1):
        dw += i * ord(ch)
    if has_whirling_vortex:
        dw += 1
    if has_obsidian_obelisk:
        dw += 2
    return f"{dw & 0xFFF:03X}"


def exits_code(room_row) -> str:
    """5 hex-char exits signature (Get_MegaMUD_ExitsCode)."""
    acc = [0, 0, 0, 0, 0, 0]  # index 0 unused, 1..5
    for d in EXIT_DIRECTIONS:
        val = room_row.get(d)
        if not val or str(val).strip() == "0":
            continue
        ex = parse_exit(d, val)
        if ex is None or not ex.is_real():
            continue
        # Hidden / text / action exits don't count
        if ex.hidden:
            continue
        # Check for (Text:) / (Action:) — these are in ex.flags as strings
        skip = False
        for f in ex.flags:
            low = f.lower()
            if low.startswith("text") or low.startswith("actio"):
                skip = True
                break
        if skip:
            continue
        is_door = bool(ex.door or ex.key_item or ex.gate)
        base = DIR_VALUE[d]
        if is_door:
            base *= 2
        acc[DIR_POSITION[d]] += base
    # Variable-width hex concatenation (matches VB6 Hex() behavior)
    return "".join(f"{acc[i]:X}" for i in range(1, 6))


def room_checksum(room_row, has_whirling_vortex: bool = False,
                  has_obsidian_obelisk: bool = False) -> str:
    """Full 8-hex-char checksum matching Rooms.md format."""
    name = str(room_row.get("Name") or "")
    hash3 = room_name_hash(name, has_whirling_vortex, has_obsidian_obelisk)
    ecode = exits_code(room_row)
    # Zero-pad the exits portion to 5 chars
    ecode = ecode.rjust(5, "0")[:5]
    return (hash3 + ecode).upper()


def detect_special_items(room_row, items_by_num: dict) -> tuple[bool, bool]:
    """Scan room's Placed field for Whirling Vortex / Obsidian Obelisk."""
    placed = str(room_row.get("Placed") or "").strip()
    if not placed:
        return False, False
    has_wv = has_oo = False
    for tok in placed.split(","):
        tok = tok.strip()
        try:
            num = int(tok)
        except (TypeError, ValueError):
            continue
        if num <= 0:
            continue
        item = items_by_num.get(num)
        if not item:
            continue
        iname = str(item.get("Name") or "").lower()
        if "whirling vortex" in iname:
            has_wv = True
        if "obsidian obelisk" in iname:
            has_oo = True
    return has_wv, has_oo


if __name__ == "__main__":
    import sys
    import mudmap_load
    data = mudmap_load.load(
        sys.argv[1] if len(sys.argv) > 1
        else "/home/bucka/AI/mmud-explorer-src/syntax53/data-v1.11p.mdb"
    )
    # Sample a few rooms
    for key in [(1, 1), (1, 2), (1, 3), (1, 500)]:
        row = data["rooms"].get(key)
        if not row:
            continue
        wv, oo = detect_special_items(row, data["items"])
        cksum = room_checksum(row, wv, oo)
        print(f"{key[0]:2d}:{key[1]:<5d} {cksum}  {row.get('Name','?')}")
