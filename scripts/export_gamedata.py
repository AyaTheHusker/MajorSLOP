#!/usr/bin/env python3
"""Export MajorMUD MDB database to JSON files for mudproxy gamedata.

Usage:
    python export_gamedata.py /path/to/database.mdb
    python export_gamedata.py /path/to/database.mdb --outdir /custom/output

Uses access-parser (pure Python, no external tools needed).
"""

import argparse
import json
import logging
import sys
from pathlib import Path

logger = logging.getLogger(__name__)

DEFAULT_OUTDIR = Path.home() / ".cache" / "mudproxy" / "gamedata"

# Module-level state (set by run_export or __main__)
OUTDIR = Path(".")
_db = None


def _parse_table(table_name: str) -> list[dict]:
    """Parse a table and return as list of row dicts."""
    try:
        cols_data = _db.parse_table(table_name)
    except Exception as e:
        logger.warning(f"Failed to read table '{table_name}': {e}")
        return []

    if not cols_data:
        return []

    # Convert column-oriented {col: [vals]} to row-oriented [{col: val}]
    columns = list(cols_data.keys())
    num_rows = len(cols_data[columns[0]]) if columns else 0
    rows = []
    for i in range(num_rows):
        row = {}
        for col in columns:
            val = cols_data[col][i]
            # access-parser returns bytes for strings
            if isinstance(val, bytes):
                val = val.decode('utf-8', errors='replace')
            row[col] = val
        rows.append(row)
    return rows


def to_int(val, default=0):
    try:
        return int(val)
    except (ValueError, TypeError):
        return default


def to_float(val, default=0.0):
    try:
        return float(val)
    except (ValueError, TypeError):
        return default


def export_monsters():
    rows = _parse_table("Monsters")
    monsters = {}
    name_lookup = {}

    for r in rows:
        num = to_int(r.get("Number"))
        name = str(r.get("Name", "")).strip()

        entry = {
            "Number": num,
            "Name": name,
            "HP": to_int(r.get("HP")),
            "EXP": to_int(r.get("EXP")),
            "ArmourClass": to_int(r.get("ArmourClass")),
            "DamageResist": to_int(r.get("DamageResist")),
            "MagicRes": to_int(r.get("MagicRes")),
            "BSDefense": to_int(r.get("BSDefense", 0)),
            "CashR": to_int(r.get("R", 0)),
            "CashP": to_int(r.get("P", 0)),
            "CashG": to_int(r.get("G", 0)),
            "CashS": to_int(r.get("S", 0)),
            "CashC": to_int(r.get("C", 0)),
            "HPRegen": to_int(r.get("HPRegen")),
            "Type": to_int(r.get("Type")),
            "Undead": to_int(r.get("Undead")),
            "Align": to_int(r.get("Align")),
            "RegenTime": to_int(r.get("RegenTime")),
            "Follow%": to_int(r.get("Follow%")),
            "Energy": to_int(r.get("Energy")),
            "AvgDmg": to_float(r.get("AvgDmg")),
        }

        # Attacks (0-4)
        attacks = []
        for i in range(5):
            att_name = str(r.get(f"AttName-{i}", "")).strip()
            if not att_name or att_name == "None":
                continue
            attacks.append({
                "AttName": att_name,
                "AttType": to_int(r.get(f"AttType-{i}")),
                "AttAcc": to_int(r.get(f"AttAcc-{i}")),
                "Att%": to_int(r.get(f"Att%-{i}")),
                "AttMin": to_int(r.get(f"AttMin-{i}")),
                "AttMax": to_int(r.get(f"AttMax-{i}")),
                "AttHitSpell": to_int(r.get(f"AttHitSpell-{i}")),
            })
        entry["attacks"] = attacks

        # Drops (0-9)
        drops = []
        for i in range(10):
            item = to_int(r.get(f"DropItem-{i}"))
            if item == 0:
                continue
            drops.append({
                "DropItem": item,
                "DropItem%": to_int(r.get(f"DropItem%-{i}")),
            })
        entry["drops"] = drops

        # MidSpells (0-4)
        midspells = []
        for i in range(5):
            spell = to_int(r.get(f"MidSpell-{i}"))
            if spell == 0:
                continue
            midspells.append({
                "MidSpell": spell,
                "MidSpell%": to_int(r.get(f"MidSpell%-{i}")),
                "MidSpellLVL": to_int(r.get(f"MidSpellLVL-{i}")),
            })
        entry["midspells"] = midspells

        # Abilities (0-9)
        abilities = []
        for i in range(10):
            abil = to_int(r.get(f"Abil-{i}"))
            if abil == 0:
                continue
            abilities.append({
                "Abil": abil,
                "AbilVal": to_int(r.get(f"AbilVal-{i}")),
            })
        entry["abilities"] = abilities

        monsters[str(num)] = entry
        if name:
            key = name.lower()
            if key in name_lookup:
                if isinstance(name_lookup[key], list):
                    name_lookup[key].append(num)
                else:
                    name_lookup[key] = [name_lookup[key], num]
            else:
                name_lookup[key] = num

    write_json("monsters.json", monsters)
    write_json("monster_names.json", name_lookup)
    logger.info(f"Monsters: {len(monsters)} entries")


def export_items():
    rows = _parse_table("Items")
    items = {}
    name_lookup = {}

    for r in rows:
        num = to_int(r.get("Number"))
        name = str(r.get("Name", "")).strip()

        entry = {
            "Number": num,
            "Name": name,
            "ItemType": to_int(r.get("ItemType")),
            "Price": to_int(r.get("Price")),
            "Min": to_int(r.get("Min")),
            "Max": to_int(r.get("Max")),
            "ArmourClass": to_int(r.get("ArmourClass")),
            "DamageResist": to_int(r.get("DamageResist")),
            "WeaponType": to_int(r.get("WeaponType")),
            "ArmourType": to_int(r.get("ArmourType")),
            "Worn": to_int(r.get("Worn")),
            "Accy": to_int(r.get("Accy")),
            "Speed": to_int(r.get("Speed")),
            "StrReq": to_int(r.get("StrReq")),
            "Encum": to_int(r.get("Encum")),
            "In Game": to_int(r.get("In Game", 0)),
            "Obtained From": str(r.get("Obtained From", "")).strip(),
        }

        # Abilities (0-19)
        abilities = []
        for i in range(20):
            abil = to_int(r.get(f"Abil-{i}"))
            if abil == 0:
                continue
            abilities.append({
                "Abil": abil,
                "AbilVal": to_int(r.get(f"AbilVal-{i}")),
            })
        entry["abilities"] = abilities

        items[str(num)] = entry
        if name:
            name_lookup[name.lower()] = num

    write_json("items.json", items)
    write_json("item_names.json", name_lookup)
    logger.info(f"Items: {len(items)} entries")


def export_rooms():
    rows = _parse_table("Rooms")
    rooms = {}
    directions = ["N", "S", "E", "W", "NE", "NW", "SE", "SW", "U", "D"]

    for r in rows:
        map_num = to_int(r.get("Map Number"))
        room_num = to_int(r.get("Room Number"))
        key = f"{map_num}-{room_num}"

        entry = {
            "Map Number": map_num,
            "Room Number": room_num,
            "Name": str(r.get("Name", "")).strip(),
            "Light": to_int(r.get("Light")),
            "Shop": to_int(r.get("Shop")),
            "Lair": str(r.get("Lair", "")).strip(),
            "Delay": to_int(r.get("Delay")),
            "Placed": str(r.get("Placed", "")).strip(),
        }

        exits = {}
        for d in directions:
            val = str(r.get(d, "")).strip()
            if val and val != "0":
                exits[d] = val
        entry["exits"] = exits

        rooms[key] = entry

    write_json("rooms.json", rooms)
    logger.info(f"Rooms: {len(rooms)} entries")


def export_spells():
    rows = _parse_table("Spells")
    spells = {}

    for r in rows:
        num = to_int(r.get("Number"))

        entry = {
            "Number": num,
            "Name": str(r.get("Name", "")).strip(),
            "Short": str(r.get("Short", "")).strip(),
            "ReqLevel": to_int(r.get("ReqLevel")),
            "EnergyCost": to_int(r.get("EnergyCost")),
            "ManaCost": to_int(r.get("ManaCost")),
            "MinBase": to_int(r.get("MinBase")),
            "MaxBase": to_int(r.get("MaxBase")),
            "Dur": to_int(r.get("Dur")),
            "AttType": to_int(r.get("AttType")),
            "Magery": to_int(r.get("Magery")),
            "MageryLVL": to_int(r.get("MageryLVL")),
        }

        abilities = []
        for i in range(10):
            abil = to_int(r.get(f"Abil-{i}"))
            if abil == 0:
                continue
            abilities.append({
                "Abil": abil,
                "AbilVal": to_int(r.get(f"AbilVal-{i}")),
            })
        entry["abilities"] = abilities

        spells[str(num)] = entry

    write_json("spells.json", spells)
    logger.info(f"Spells: {len(spells)} entries")


def export_classes():
    rows = _parse_table("Classes")
    classes = {}
    for r in rows:
        num = to_int(r.get("Number"))
        entry = {
            "Number": num,
            "Name": str(r.get("Name", "")).strip(),
            "MinHits": to_int(r.get("MinHits")),
            "MaxHits": to_int(r.get("MaxHits")),
            "ExpTable": to_int(r.get("ExpTable")),
            "MageryType": to_int(r.get("MageryType")),
            "MageryLVL": to_int(r.get("MageryLVL")),
            "WeaponType": to_int(r.get("WeaponType")),
            "ArmourType": to_int(r.get("ArmourType")),
            "CombatLVL": to_int(r.get("CombatLVL")),
        }
        abilities = []
        for i in range(10):
            abil = to_int(r.get(f"Abil-{i}"))
            if abil == 0:
                continue
            abilities.append({"Abil": abil, "AbilVal": to_int(r.get(f"AbilVal-{i}"))})
        entry["abilities"] = abilities
        classes[str(num)] = entry
    write_json("classes.json", classes)
    logger.info(f"Classes: {len(classes)} entries")


def export_races():
    rows = _parse_table("Races")
    races = {}
    for r in rows:
        num = to_int(r.get("Number"))
        entry = {
            "Number": num,
            "Name": str(r.get("Name", "")).strip(),
            "mINT": to_int(r.get("mINT")), "mWIL": to_int(r.get("mWIL")),
            "mSTR": to_int(r.get("mSTR")), "mHEA": to_int(r.get("mHEA")),
            "mAGL": to_int(r.get("mAGL")), "mCHM": to_int(r.get("mCHM")),
            "xINT": to_int(r.get("xINT")), "xWIL": to_int(r.get("xWIL")),
            "xSTR": to_int(r.get("xSTR")), "xHEA": to_int(r.get("xHEA")),
            "xAGL": to_int(r.get("xAGL")), "xCHM": to_int(r.get("xCHM")),
            "HPPerLVL": to_int(r.get("HPPerLVL")),
            "ExpTable": to_int(r.get("ExpTable")),
            "BaseCP": to_int(r.get("BaseCP")),
        }
        abilities = []
        for i in range(10):
            abil = to_int(r.get(f"Abil-{i}"))
            if abil == 0:
                continue
            abilities.append({"Abil": abil, "AbilVal": to_int(r.get(f"AbilVal-{i}"))})
        entry["abilities"] = abilities
        races[str(num)] = entry
    write_json("races.json", races)
    logger.info(f"Races: {len(races)} entries")


def export_lairs():
    rows = _parse_table("Lairs")
    lairs = {}
    for r in rows:
        key = str(r.get("GroupIndex", "")).strip()
        entry = {
            "GroupIndex": key,
            "MobList": str(r.get("MobList", "")).strip(),
            "Mobs": to_int(r.get("Mobs")),
            "TotalLairs": to_int(r.get("TotalLairs")),
            "AvgDelay": to_int(r.get("AvgDelay")),
            "AvgExp": to_float(r.get("AvgExp")),
            "AvgDmg": to_float(r.get("AvgDmg")),
            "AvgHP": to_float(r.get("AvgHP")),
        }
        lairs[key] = entry
    write_json("lairs.json", lairs)
    logger.info(f"Lairs: {len(lairs)} entries")


def export_shops():
    rows = _parse_table("Shops")
    shops = {}
    for r in rows:
        num = to_int(r.get("Number"))
        entry = {
            "Number": num,
            "Name": str(r.get("Name", "")).strip(),
            "ShopType": to_int(r.get("ShopType")),
            "MinLVL": to_int(r.get("MinLVL")),
            "MaxLVL": to_int(r.get("MaxLVL")),
            "Markup%": to_int(r.get("Markup%")),
            "In Game": to_int(r.get("In Game", 0)),
        }
        items = []
        for i in range(20):
            item_num = to_int(r.get(f"Item-{i}"))
            if item_num == 0:
                continue
            items.append({
                "Item": item_num,
                "Max": to_int(r.get(f"Max-{i}")),
                "Time": to_int(r.get(f"Time-{i}")),
                "Amount": to_int(r.get(f"Amount-{i}")),
                "%": to_int(r.get(f"%-{i}")),
            })
        entry["items"] = items
        shops[str(num)] = entry
    write_json("shops.json", shops)
    logger.info(f"Shops: {len(shops)} entries")


def write_json(filename, data):
    path = OUTDIR / filename
    with open(path, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def run_export(mdb_path: str, outdir: str | None = None) -> dict:
    """Export MDB to JSON. Returns dict with counts. Callable from anywhere."""
    global OUTDIR, _db

    from access_parser import AccessParser

    if not Path(mdb_path).exists():
        raise FileNotFoundError(f"MDB file not found: {mdb_path}")

    OUTDIR = Path(outdir) if outdir else DEFAULT_OUTDIR
    OUTDIR.mkdir(parents=True, exist_ok=True)

    _db = AccessParser(mdb_path)

    export_monsters()
    export_items()
    export_rooms()
    export_spells()
    export_classes()
    export_races()
    export_lairs()
    export_shops()

    # Export text blocks (quest/scripting system)
    try:
        from mudproxy.textblock import load_from_mdb, export_json
        blocks = load_from_mdb(mdb_path)
        export_json(blocks, OUTDIR / "textblocks.json")
    except Exception as e:
        logger.warning(f"Text block export failed: {e}")

    # Return summary
    counts = {}
    for name in ["monsters", "items", "rooms", "spells", "classes", "races", "lairs", "shops", "textblocks"]:
        path = OUTDIR / f"{name}.json"
        if path.exists():
            counts[name] = len(json.loads(path.read_text()))
    return counts


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(message)s")

    parser = argparse.ArgumentParser(
        description="Export MajorMUD .mdb database to JSON gamedata files")
    parser.add_argument("mdb", help="Path to the .mdb file")
    parser.add_argument("--outdir", default=str(DEFAULT_OUTDIR),
                        help=f"Output directory (default: {DEFAULT_OUTDIR})")
    args = parser.parse_args()

    counts = run_export(args.mdb, args.outdir)
    total = sum(counts.values())
    print(f"\nDone. Exported {total} records: {counts}")
