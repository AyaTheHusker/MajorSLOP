"""Parse MegaMud Rooms.md, Loops, and Paths into JSON for web client."""
import json
import os
from pathlib import Path

MEGAMUD_DIR = Path(os.path.expanduser("~/.wine/drive_c/MegaMUD"))
LOOPS_DIR = Path(os.path.expanduser("~/.wine/drive_c/users/bucka/Documents/Loops"))
OUTPUT = Path(os.path.expanduser("~/.cache/mudproxy/gamedata/megamud_rooms.json"))

def parse_rooms_md(path: Path) -> list[dict]:
    """Parse a Rooms.md file. Format: hash:flags:v1:v2:v3:CODE:Category:RoomName"""
    rooms = []
    if not path.exists():
        return rooms
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(":")
        if len(parts) < 8:
            continue
        flags = int(parts[1], 16)
        code = parts[5]
        category = parts[6]
        name = ":".join(parts[7:])  # room name might contain colons
        rooms.append({
            "code": code,
            "category": category,
            "name": name,
            "flags": flags,
            "hash": parts[0],
        })
    return rooms


def parse_loops(loops_dir: Path) -> list[dict]:
    """Parse .mp loop files. Header: [LoopName][Creator] then [CODE:Category:RoomName]"""
    loops = []
    if not loops_dir.exists():
        return loops
    for mp in sorted(loops_dir.glob("*.mp")):
        try:
            lines = mp.read_text().splitlines()
            if len(lines) < 2:
                continue
            # Header: [LoopName][Creator]
            header = lines[0]
            # Extract loop name from first bracket pair
            loop_name = ""
            creator = ""
            if header.startswith("["):
                end = header.index("]")
                loop_name = header[1:end]
                rest = header[end+1:]
                if rest.startswith("[") and "]" in rest:
                    creator = rest[1:rest.index("]")]
            # Start room: [CODE:Category:RoomName]
            room_line = lines[1]
            start_code = ""
            start_category = ""
            start_name = ""
            if room_line.startswith("[") and "]" in room_line:
                inner = room_line[1:room_line.rindex("]")]
                rparts = inner.split(":", 2)
                if len(rparts) >= 3:
                    start_code = rparts[0]
                    start_category = rparts[1]
                    start_name = rparts[2]
            loops.append({
                "file": mp.stem,
                "name": loop_name,
                "creator": creator,
                "start_code": start_code,
                "start_category": start_category,
                "start_room": start_name,
                "steps": len(lines) - 2,  # exclude header lines
            })
        except Exception as e:
            print(f"Error parsing {mp.name}: {e}")
    return loops


def scan_mp_files(mp_dir: Path) -> tuple[list[dict], list[dict]]:
    """Scan .mp files for loops (start==end) and paths (start!=end).
    Returns (loops, paths)."""
    loops = []
    paths = []
    if not mp_dir.exists():
        return loops, paths
    for mp in sorted(mp_dir.glob("*.mp")):
        try:
            lines = mp.read_text().splitlines()
            if len(lines) < 3:
                continue
            header = lines[0]
            room_line = lines[1]
            data_line = lines[2]
            parts = data_line.split(":")
            if len(parts) < 2:
                continue
            start_hash = parts[0]
            end_hash = parts[1]
            # Parse header: [name][creator]
            name = ""
            creator = ""
            if header.startswith("["):
                end = header.index("]")
                name = header[1:end]
                rest = header[end+1:]
                if rest.startswith("[") and "]" in rest:
                    creator = rest[1:rest.index("]")]
            # Parse room: [CODE:Category:RoomName]
            start_code = ""
            start_category = ""
            start_name = ""
            if room_line.startswith("[") and "]" in room_line:
                inner = room_line[1:room_line.rindex("]")]
                rparts = inner.split(":", 2)
                if len(rparts) >= 3:
                    start_code = rparts[0]
                    start_category = rparts[1]
                    start_name = rparts[2]
            # Parse step count from data line field[2]
            try:
                step_count = int(parts[2]) if len(parts) > 2 else len(lines) - 3
            except ValueError:
                step_count = len(lines) - 3
            # Also parse end room from lines[2] if it's a path (has 2nd room bracket)
            end_code = ""
            end_category = ""
            end_name = ""
            if len(lines) > 2:
                end_room_line = lines[2] if not lines[2].startswith("[") else ""
                # For paths, line[2] is [CODE:Cat:Name], for loops line[1] is start room
                # Actually line[2] is the data line. For paths there's a 3rd header line
                pass
            # Check if there's a second room header (paths have 3 header lines)
            if len(lines) > 2 and lines[2].startswith("["):
                inner2 = lines[2][1:lines[2].rindex("]")]
                rparts2 = inner2.split(":", 2)
                if len(rparts2) >= 3:
                    end_code = rparts2[0]
                    end_category = rparts2[1]
                    end_name = rparts2[2]

            entry = {
                "file": mp.stem,
                "name": name,
                "creator": creator,
                "start_code": start_code,
                "start_category": start_category,
                "start_room": start_name,
                "start_hash": start_hash,
                "end_hash": end_hash,
                "steps": step_count,
            }
            # All .mp files go in both lists — MegaMUD shows all in its loop dialog
            loops.append(entry)
            if start_hash != end_hash:
                paths.append(entry)
        except Exception:
            pass
    return loops, paths


def main():
    # Load rooms from Default and Chars/All (player overrides)
    all_rooms = parse_rooms_md(MEGAMUD_DIR / "Default" / "Rooms.md")
    char_rooms = parse_rooms_md(MEGAMUD_DIR / "Chars" / "All" / "Rooms.md")

    # Merge: char rooms override default by code
    by_code = {r["code"]: r for r in all_rooms}
    for r in char_rooms:
        by_code[r["code"]] = r
    all_rooms = list(by_code.values())

    # Categorize
    categories = {}
    hidden = []
    for r in all_rooms:
        # Bit 4 (0x10) = monster/NPC rooms — hidden from nav list
        if r["flags"] & 0x10:
            hidden.append(r)
            continue
        cat = r["category"] or "Uncategorized"
        if cat not in categories:
            categories[cat] = []
        categories[cat].append({
            "code": r["code"],
            "name": r["name"],
            "flags": r["flags"],
        })

    # Sort categories and rooms within them
    for cat in categories:
        categories[cat].sort(key=lambda x: x["name"])

    # Load loops and paths from Default .mp files
    default_loops, default_paths = scan_mp_files(MEGAMUD_DIR / "Default")
    # Also check user's Loops directory
    user_loops = parse_loops(LOOPS_DIR)
    # Merge: user loops override by file stem
    loop_by_file = {l["file"]: l for l in default_loops}
    for l in user_loops:
        loop_by_file[l["file"]] = l
    all_loops = sorted(loop_by_file.values(), key=lambda x: x.get("start_category", "") + x.get("name", ""))

    # Group loops by start category
    loop_categories = {}
    for l in all_loops:
        cat = l.get("start_category") or "Uncategorized"
        if cat not in loop_categories:
            loop_categories[cat] = []
        loop_categories[cat].append(l)

    # Group paths by start category
    all_paths = sorted(default_paths, key=lambda x: x.get("start_category", "") + x.get("name", ""))
    path_categories = {}
    for p in all_paths:
        cat = p.get("start_category") or "Uncategorized"
        if cat not in path_categories:
            path_categories[cat] = []
        path_categories[cat].append(p)

    output = {
        "rooms": dict(sorted(categories.items())),
        "hidden_rooms": [{"code": r["code"], "category": r["category"], "name": r["name"]} for r in hidden],
        "loops": dict(sorted(loop_categories.items())),
        "paths": dict(sorted(path_categories.items())),
        "stats": {
            "total_rooms": len(all_rooms),
            "visible_rooms": sum(len(v) for v in categories.values()),
            "hidden_rooms": len(hidden),
            "categories": len(categories),
            "loops": len(all_loops),
            "paths": len(all_paths),
        },
    }

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(json.dumps(output, indent=2))
    print(f"Written to {OUTPUT}")
    print(f"  {output['stats']['total_rooms']} total rooms")
    print(f"  {output['stats']['visible_rooms']} visible in {output['stats']['categories']} categories")
    print(f"  {output['stats']['hidden_rooms']} hidden (monster/NPC)")
    print(f"  {output['stats']['loops']} loops in {len(loop_categories)} categories")
    print(f"  {output['stats']['paths']} point-to-point paths")


if __name__ == "__main__":
    main()
