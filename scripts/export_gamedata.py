#!/usr/bin/env python3
"""Export MajorMUD MDB database to JSON files for mudproxy gamedata.

Usage:
    python export_gamedata.py /path/to/database.mdb
    python export_gamedata.py /path/to/database.mdb --outdir /custom/output

Requires mdbtools: sudo pacman -S mdbtools  (or apt install mdbtools)
"""

import argparse
import csv
import io
import json
import subprocess
import sys
from pathlib import Path

DEFAULT_OUTDIR = Path.home() / ".cache" / "mudproxy" / "gamedata"


def mdb_read(table: str) -> list[dict]:
    """Run mdb-export and return list of row dicts."""
    result = subprocess.run(
        ["mdb-export", MDB, table],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(f"  Warning: failed to export table '{table}': {result.stderr.strip()}")
        return []
    reader = csv.DictReader(io.StringIO(result.stdout))
    return list(reader)


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
    rows = mdb_read("Monsters")
    monsters = {}
    name_lookup = {}

    for r in rows:
        num = to_int(r["Number"])
        name = r["Name"].strip()

        entry = {
            "Number": num,
            "Name": name,
            "HP": to_int(r["HP"]),
            "EXP": to_int(r["EXP"]),
            "ArmourClass": to_int(r["ArmourClass"]),
            "DamageResist": to_int(r["DamageResist"]),
            "MagicRes": to_int(r["MagicRes"]),
            "HPRegen": to_int(r["HPRegen"]),
            "Type": to_int(r["Type"]),
            "Undead": to_int(r["Undead"]),
            "Align": to_int(r["Align"]),
            "RegenTime": to_int(r["RegenTime"]),
            "Follow%": to_int(r["Follow%"]),
            "Energy": to_int(r["Energy"]),
            "AvgDmg": to_float(r["AvgDmg"]),
        }

        # Attacks (0-4)
        attacks = []
        for i in range(5):
            att_name = r.get(f"AttName-{i}", "").strip()
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
    print(f"Monsters: {len(monsters)} entries")


def export_items():
    rows = mdb_read("Items")
    items = {}
    name_lookup = {}

    for r in rows:
        num = to_int(r["Number"])
        name = r["Name"].strip()

        entry = {
            "Number": num,
            "Name": name,
            "ItemType": to_int(r["ItemType"]),
            "Price": to_int(r["Price"]),
            "Min": to_int(r["Min"]),
            "Max": to_int(r["Max"]),
            "ArmourClass": to_int(r["ArmourClass"]),
            "DamageResist": to_int(r["DamageResist"]),
            "WeaponType": to_int(r["WeaponType"]),
            "ArmourType": to_int(r["ArmourType"]),
            "Worn": to_int(r["Worn"]),
            "Accy": to_int(r["Accy"]),
            "Speed": to_int(r["Speed"]),
            "StrReq": to_int(r["StrReq"]),
            "Encum": to_int(r["Encum"]),
            "In Game": to_int(r.get("In Game", 0)),
            "Obtained From": r.get("Obtained From", "").strip(),
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
    print(f"Items: {len(items)} entries")


def export_rooms():
    rows = mdb_read("Rooms")
    rooms = {}
    directions = ["N", "S", "E", "W", "NE", "NW", "SE", "SW", "U", "D"]

    for r in rows:
        map_num = to_int(r["Map Number"])
        room_num = to_int(r["Room Number"])
        key = f"{map_num}-{room_num}"

        entry = {
            "Map Number": map_num,
            "Room Number": room_num,
            "Name": r["Name"].strip(),
            "Light": to_int(r["Light"]),
            "Shop": to_int(r["Shop"]),
            "Lair": r.get("Lair", "").strip(),
            "Delay": to_int(r["Delay"]),
            "Placed": r.get("Placed", "").strip(),
        }

        exits = {}
        for d in directions:
            val = r.get(d, "").strip()
            if val and val != "0":
                exits[d] = val
        entry["exits"] = exits

        rooms[key] = entry

    write_json("rooms.json", rooms)
    print(f"Rooms: {len(rooms)} entries")


def export_spells():
    rows = mdb_read("Spells")
    spells = {}

    for r in rows:
        num = to_int(r["Number"])

        entry = {
            "Number": num,
            "Name": r["Name"].strip(),
            "Short": r["Short"].strip(),
            "ReqLevel": to_int(r["ReqLevel"]),
            "EnergyCost": to_int(r["EnergyCost"]),
            "ManaCost": to_int(r["ManaCost"]),
            "MinBase": to_int(r["MinBase"]),
            "MaxBase": to_int(r["MaxBase"]),
            "Dur": to_int(r["Dur"]),
            "AttType": to_int(r["AttType"]),
            "Magery": to_int(r["Magery"]),
            "MageryLVL": to_int(r["MageryLVL"]),
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
    print(f"Spells: {len(spells)} entries")


def write_json(filename, data):
    path = OUTDIR / filename
    with open(path, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    print(f"  Wrote {path} ({path.stat().st_size:,} bytes)")


def export_classes():
    rows = mdb_read("Classes")
    classes = {}
    for r in rows:
        num = to_int(r["Number"])
        entry = {
            "Number": num,
            "Name": r["Name"].strip(),
            "MinHits": to_int(r["MinHits"]),
            "MaxHits": to_int(r["MaxHits"]),
            "ExpTable": to_int(r["ExpTable"]),
            "MageryType": to_int(r["MageryType"]),
            "MageryLVL": to_int(r["MageryLVL"]),
            "WeaponType": to_int(r["WeaponType"]),
            "ArmourType": to_int(r["ArmourType"]),
            "CombatLVL": to_int(r["CombatLVL"]),
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
    print(f"Classes: {len(classes)} entries")


def export_races():
    rows = mdb_read("Races")
    races = {}
    for r in rows:
        num = to_int(r["Number"])
        entry = {
            "Number": num,
            "Name": r["Name"].strip(),
            "mINT": to_int(r["mINT"]), "mWIL": to_int(r["mWIL"]),
            "mSTR": to_int(r["mSTR"]), "mHEA": to_int(r["mHEA"]),
            "mAGL": to_int(r["mAGL"]), "mCHM": to_int(r["mCHM"]),
            "xINT": to_int(r["xINT"]), "xWIL": to_int(r["xWIL"]),
            "xSTR": to_int(r["xSTR"]), "xHEA": to_int(r["xHEA"]),
            "xAGL": to_int(r["xAGL"]), "xCHM": to_int(r["xCHM"]),
            "HPPerLVL": to_int(r["HPPerLVL"]),
            "ExpTable": to_int(r["ExpTable"]),
            "BaseCP": to_int(r["BaseCP"]),
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
    print(f"Races: {len(races)} entries")


def export_lairs():
    rows = mdb_read("Lairs")
    lairs = {}
    for r in rows:
        key = r["GroupIndex"].strip()
        entry = {
            "GroupIndex": key,
            "MobList": r["MobList"].strip(),
            "Mobs": to_int(r["Mobs"]),
            "TotalLairs": to_int(r["TotalLairs"]),
            "AvgDelay": to_int(r["AvgDelay"]),
            "AvgExp": to_float(r["AvgExp"]),
            "AvgDmg": to_float(r["AvgDmg"]),
            "AvgHP": to_float(r["AvgHP"]),
        }
        lairs[key] = entry
    write_json("lairs.json", lairs)
    print(f"Lairs: {len(lairs)} entries")


def export_shops():
    rows = mdb_read("Shops")
    shops = {}
    for r in rows:
        num = to_int(r["Number"])
        entry = {
            "Number": num,
            "Name": r["Name"].strip(),
            "ShopType": to_int(r["ShopType"]),
            "MinLVL": to_int(r["MinLVL"]),
            "MaxLVL": to_int(r["MaxLVL"]),
            "Markup%": to_int(r["Markup%"]),
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
    print(f"Shops: {len(shops)} entries")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Export MajorMUD .mdb database to JSON gamedata files")
    parser.add_argument("mdb", help="Path to the .mdb file")
    parser.add_argument("--outdir", default=str(DEFAULT_OUTDIR),
                        help=f"Output directory (default: {DEFAULT_OUTDIR})")
    args = parser.parse_args()

    MDB = args.mdb
    if not Path(MDB).exists():
        print(f"Error: MDB file not found: {MDB}", file=sys.stderr)
        sys.exit(1)

    # Check mdbtools is installed
    try:
        subprocess.run(["mdb-export", "--version"], capture_output=True, check=True)
    except FileNotFoundError:
        print("Error: mdbtools not installed. Install with: sudo pacman -S mdbtools", file=sys.stderr)
        sys.exit(1)

    OUTDIR = Path(args.outdir)
    OUTDIR.mkdir(parents=True, exist_ok=True)

    print(f"Exporting: {MDB}")
    print(f"Output:    {OUTDIR}")
    print()

    export_monsters()
    export_items()
    export_rooms()
    export_spells()
    export_classes()
    export_races()
    export_lairs()
    export_shops()
    print(f"\nDone. Gamedata exported to {OUTDIR}")
