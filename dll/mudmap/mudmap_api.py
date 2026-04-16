"""Unified API — load MDB, compute layouts, export to compact JSON for C side.

Usage:
    python3 mudmap_api.py export <mdb_path> <out_json>
    python3 mudmap_api.py layout <mdb_path> <map> <room> <out_json>

Compact JSON schema written for C to parse with a tiny handwritten parser:

{
  "rooms": [
    {"m":1,"r":1,"name":"Town Gates","npc":0,"shop":0,"lair":0,"spell":0,"cmd":0,
     "exits":[{"d":"N","m":1,"r":3,"flags":0,"key":0}, ...]}
  ],
  "maps": [{"m":1,"rooms":[1,2,3,...]}, ...]
}
"""
from __future__ import annotations
import json
import sys
from mudmap_load import load
from mudmap_exit import parse_room_exits, EXIT_DIRECTIONS

# Flag bits for exits (packed for C)
F_DOOR      = 1 << 0
F_HIDDEN    = 1 << 1
F_TRAP      = 1 << 2
F_KEY       = 1 << 3
F_ITEM      = 1 << 4
F_CLASS     = 1 << 5
F_RACE      = 1 << 6
F_LEVEL     = 1 << 7
F_TOLL      = 1 << 8
F_BLOCKED   = 1 << 9
F_CAST      = 1 << 10
F_GATE      = 1 << 11


def _to_int(v, default=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def _encode_exit(ex):
    flags = 0
    if ex.door: flags |= F_DOOR
    if ex.hidden: flags |= F_HIDDEN
    if ex.trap: flags |= F_TRAP
    if ex.key_item: flags |= F_KEY
    if ex.req_item: flags |= F_ITEM
    if ex.class_req: flags |= F_CLASS
    if ex.race_req: flags |= F_RACE
    if ex.level_req: flags |= F_LEVEL
    if ex.toll: flags |= F_TOLL
    if ex.blocked_flag: flags |= F_BLOCKED
    if ex.cast_pre: flags |= F_CAST
    if ex.gate: flags |= F_GATE
    return {
        "d": ex.dir,
        "m": ex.map,
        "r": ex.room,
        "f": flags,
        "k": ex.key_item or ex.req_item,
    }


def export(mdb_path: str, out_path: str) -> None:
    data = load(mdb_path)
    rooms = []
    for (m, rn), row in sorted(data["rooms"].items()):
        exits = [_encode_exit(ex) for ex in parse_room_exits(row)]
        rooms.append({
            "m": m,
            "r": rn,
            "name": str(row.get("Name") or "").strip(),
            "npc": _to_int(row.get("NPC")),
            "shop": _to_int(row.get("Shop")),
            "lair": 1 if str(row.get("Lair") or "").strip() else 0,
            "spell": _to_int(row.get("Spell")),
            "cmd": _to_int(row.get("CMD")),
            "light": _to_int(row.get("Light")),
            "exits": exits,
        })

    # Keep monster/shop names for the info panel
    monster_names = {int(num): str(r.get("Name") or "").strip()
                     for num, r in data["monsters"].items()}
    shop_names = {int(num): str(r.get("Name") or "").strip()
                  for num, r in data["shops"].items()}

    out = {
        "rooms": rooms,
        "maps": [{"m": m, "rooms": rs} for m, rs in sorted(data["maps"].items())],
        "monsters": monster_names,
        "shops": shop_names,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f, separators=(",", ":"))
    print(f"wrote {out_path}: {len(rooms)} rooms, {len(out['maps'])} maps")


# ---- Binary format (for C consumption) ----
#
# Layout (version 2):
#   magic       u32  'MMUD' (0x44554D4D LE-on-disk)
#   version     u32  = 2
#   room_count  u32
#   map_count   u32
#   [rooms]     room_count × room_record         (optimized — see below)
#   [maps]      map_count × map_record           (optimized — see below)
#   table_count u32                              (generic aux tables)
#   [tables]    table_count × table_record
#
# The optimized section is for fast map rendering. The generic table section
# at the end carries the rest of the MDB (Items, Spells, Monsters, Shops,
# Classes, Races, Lairs, Info) as column-tagged rows so future features can
# pull any field without re-exporting.
#
# room_record:
#   map u16, room u16, npc u16, shop u16, spell u16, cmd u16
#   lair u8, light i8, exit_count u8, name_len u8
#   name name_len bytes (latin-1)
#   checksum u32
#   [exits] exit_count × exit_record
#
# exit_record:
#   dir_code u8 (0=N,1=S,2=E,3=W,4=NE,5=NW,6=SE,7=SW,8=U,9=D)
#   flags u16, target_map u16, target_room u16, key_item u16
#
# map_record:
#   map_num u16, room_count u32
#   [(room_num u16, index u16)] × room_count
#
# table_record:
#   name_len u8, name (latin-1)
#   col_count u8
#   [cols] col_count × (name_len u8, name bytes)
#   row_count u32
#   [rows] row_count × [cells] col_count × (len u16, bytes ; 0xFFFF = NULL)

import re
import struct
from mudmap_checksum import room_checksum, detect_special_items
from mudmap_exit import parse_room_exits

DIR_CODE = {"N":0,"S":1,"E":2,"W":3,"NE":4,"NW":5,"SE":6,"SW":7,"U":8,"D":9}


def _pack_str(s) -> bytes:
    """Encode a cell value as (u16 len, bytes). len=0xFFFF marks NULL."""
    if s is None:
        return struct.pack("<H", 0xFFFF)
    if isinstance(s, bytes):
        b = s
    else:
        b = str(s).encode("latin-1", "replace")
    if len(b) > 65534:
        b = b[:65534]
    return struct.pack("<H", len(b)) + b


def _pack_table(db, name: str) -> bytes | None:
    """Dump a Jet table generically: name + cols + rows (all cells stringified)."""
    try:
        cols = db.parse_table(name)
    except Exception:
        return None
    if not cols:
        return None
    col_names = list(cols.keys())
    row_count = len(cols[col_names[0]]) if col_names else 0
    nb = name.encode("latin-1", "replace")[:255]
    out = bytearray()
    out += struct.pack("<B", len(nb)) + nb
    out += struct.pack("<B", min(len(col_names), 255))
    for cn in col_names[:255]:
        cnb = cn.encode("latin-1", "replace")[:255]
        out += struct.pack("<B", len(cnb)) + cnb
    out += struct.pack("<I", row_count)
    for i in range(row_count):
        for cn in col_names[:255]:
            out += _pack_str(cols[cn][i])
    return bytes(out)


# Tables to export generically (Rooms is already in the optimized section).
# MSysObjects / TBInfo are Jet internals — skip them.
AUX_TABLES = ("Items", "Spells", "Monsters", "Shops", "Classes", "Races", "Lairs", "Info")


def _pack_rooms_md(rooms_md_path: str) -> bytes | None:
    """Bake Rooms.md into an aux-table blob named _RoomsMD.

    Columns: cksum, flags, code, area, name (all stored as strings).
    Keyed on uppercase 8-hex cksum; C side looks up by checksum.
    """
    from mudmap_rooms_md import load_all
    try:
        rooms = load_all(rooms_md_path)
    except Exception:
        return None
    if not rooms:
        return None
    cols = ("cksum", "flags", "code", "area", "name")
    name = "_RoomsMD"
    out = bytearray()
    nb = name.encode("latin-1", "replace")[:255]
    out += struct.pack("<B", len(nb)) + nb
    out += struct.pack("<B", len(cols))
    for cn in cols:
        cnb = cn.encode("latin-1", "replace")
        out += struct.pack("<B", len(cnb)) + cnb
    out += struct.pack("<I", len(rooms))
    for cksum in sorted(rooms.keys()):
        r = rooms[cksum]
        for val in (r.checksum, r.flags, r.code, r.area, r.name):
            out += _pack_str(val)
    return bytes(out)


def export_bin(mdb_path: str, out_path: str, rooms_md_path: str | None = None) -> None:
    from mudmap_load import load
    from access_parser import AccessParser
    data = load(mdb_path)
    # Stable ordering: by (map, room)
    rooms = sorted(data["rooms"].items())
    room_index: dict[tuple[int,int], int] = {}
    buf = bytearray()
    buf += struct.pack("<IIII", 0x44554D4D, 3, len(rooms), len(data["maps"]))
    for i, ((m, rn), row) in enumerate(rooms):
        room_index[(m, rn)] = i
        wv, oo = detect_special_items(row, data["items"])
        ck = int(room_checksum(row, wv, oo), 16)
        exits = parse_room_exits(row)
        name = (str(row.get("Name") or "").strip().encode("latin-1", "replace"))[:255]
        def _i(v):
            try: return int(v) if v not in (None, "") else 0
            except: return 0
        buf += struct.pack("<HHHHHH",
            m & 0xFFFF, rn & 0xFFFF,
            _i(row.get("NPC")) & 0xFFFF,
            _i(row.get("Shop")) & 0xFFFF,
            _i(row.get("Spell")) & 0xFFFF,
            _i(row.get("CMD")) & 0xFFFF,
        )
        # Lair field looks like '(Max 3): 833,834,866,[29-38-38-3]'
        #   Max N  → lair size (monster count cap for the room)
        # Empty field or no "Max N" → 0 (not a lair).
        lair_raw = str(row.get("Lair") or "").strip()
        lair_count = 0
        if lair_raw:
            mm = re.search(r"max\s+(\d+)", lair_raw, re.I)
            lair_count = min(255, int(mm.group(1))) if mm else 0
        light = max(-127, min(127, _i(row.get("Light"))))
        buf += struct.pack("<BbBB", lair_count, light, len(exits), len(name))
        buf += name
        buf += struct.pack("<I", ck & 0xFFFFFFFF)
        for ex in exits:
            flags = 0
            if ex.door:          flags |= 0x0001
            if ex.hidden:        flags |= 0x0002
            if ex.trap:          flags |= 0x0004
            if ex.key_item:      flags |= 0x0008
            if ex.req_item:      flags |= 0x0010
            if ex.class_req:     flags |= 0x0020
            if ex.race_req:      flags |= 0x0040
            if ex.level_req:     flags |= 0x0080
            if ex.toll:          flags |= 0x0100
            if ex.blocked_flag:  flags |= 0x0200
            if ex.cast_pre:      flags |= 0x0400
            if ex.gate:          flags |= 0x0800
            buf += struct.pack("<BHHHH",
                DIR_CODE.get(ex.dir, 0) & 0xFF,
                flags & 0xFFFF,
                ex.map & 0xFFFF, ex.room & 0xFFFF,
                (ex.key_item or ex.req_item) & 0xFFFF,
            )
    for m, rs in sorted(data["maps"].items()):
        buf += struct.pack("<HI", m & 0xFFFF, len(rs))
        for rn in rs:
            idx = room_index.get((m, rn), 0xFFFF)
            buf += struct.pack("<HH", rn & 0xFFFF, idx & 0xFFFF)

    # Generic aux-tables section (v2+).  Dump every table we know about.
    db = AccessParser(mdb_path)
    tbuf = bytearray()
    ncount = 0
    for tname in AUX_TABLES:
        blob = _pack_table(db, tname)
        if blob is None:
            continue
        tbuf += blob
        ncount += 1
    if rooms_md_path:
        md_blob = _pack_rooms_md(rooms_md_path)
        if md_blob is not None:
            tbuf += md_blob
            ncount += 1
    buf += struct.pack("<I", ncount)
    buf += tbuf

    with open(out_path, "wb") as f:
        f.write(buf)
    print(f"wrote {out_path}: {len(buf)} bytes, {len(rooms)} rooms, "
          f"{len(data['maps'])} maps, {ncount} aux tables")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    cmd = sys.argv[1]
    if cmd == "export":
        export(sys.argv[2], sys.argv[3])
    elif cmd == "export_bin":
        rmd = sys.argv[4] if len(sys.argv) > 4 else None
        export_bin(sys.argv[2], sys.argv[3], rmd)
    else:
        print(f"unknown cmd: {cmd}")
        sys.exit(1)
