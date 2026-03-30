"""Standalone memory reader test — run under Wine Python to iterate fast.

Usage:
    wine python test_memread.py
    wine python test_memread.py --watch
    wine python test_memread.py --name Tripmunk
    wine python test_memread.py --monitor
"""
import argparse
import json
import struct
import sys
import time

from mudproxy.mem_reader import MegaMUDMemReader


# Key offsets to monitor during loop/pathing actions
MONITOR_OFFSETS = {
    # Pathing/loop control
    0x2EA4: ("dest_checksum", "u32"),
    0x5664: ("PATHING_ACTIVE", "i32"),
    0x5668: ("LOOPING", "i32"),
    0x566C: ("AUTO_ROAMING", "i32"),
    0x54B4: ("ON_ENTRY_ACTION", "i32"),
    # Room
    0x2E70: ("room_checksum", "u32"),
    0x2D9C: ("room_name", "str32"),
    # Path step
    0x5898: ("cur_path_step", "i32"),
    # Combat / status
    0x5698: ("IN_COMBAT", "i32"),
    0x5678: ("IS_RESTING", "i32"),
    0x567C: ("IS_MEDITATING", "i32"),
    0x5674: ("IS_FLEEING", "i32"),
    0x5670: ("IS_LOST", "i32"),
    # HP/mana
    0x53D4: ("cur_hp", "i32"),
    0x53DC: ("max_hp", "i32"),
    0x53E0: ("cur_mana", "i32"),
    # Path file area
    0x5834: ("path_file", "str32"),
    # Nearby unknowns around loop/path control
    0x5660: ("unk_5660", "i32"),
    0x565C: ("FIX_STEPS", "i32"),
    0x5680: ("unk_5680", "i32"),
    0x5684: ("unk_5684", "i32"),
    0x569C: ("unk_569C", "i32"),
    0x56A0: ("unk_56A0", "i32"),
    0x56A4: ("unk_56A4", "i32"),
    0x56A8: ("unk_56A8", "i32"),
    # Around path data
    0x5880: ("path_wp_0", "u32"),
    0x5884: ("path_wp_1", "u32"),
    0x5888: ("path_wp_2", "u32"),
    0x588C: ("path_wp_3", "u32"),
    0x5890: ("path_wp_4", "u32"),
    0x5894: ("path_wp_5", "u32"),
    # Action/auto-combat
    0x574C: ("AUTOCOMBAT_SLOT", "i32"),
    # Lag
    0x8A98: ("LAG_WAIT", "i32"),
    # Entity count
    0x1ED4: ("entity_count", "i32"),
}


def _read_field(reader, offset, typ):
    if typ == "i32":
        return reader._read_i32(offset)
    elif typ == "u32":
        v = reader._read_u32(offset)
        return f"0x{v:08X}" if v is not None else None
    elif typ.startswith("str"):
        maxlen = int(typ[3:])
        return reader._read_string(offset, maxlen)
    return None


def main():
    parser = argparse.ArgumentParser(description="Test MegaMUD memory reader")
    parser.add_argument("--name", type=str, default=None, help="Player name for targeted scan")
    parser.add_argument("--watch", action="store_true", help="Poll continuously and show changes")
    parser.add_argument("--monitor", action="store_true", help="Monitor loop/path fields for changes")
    parser.add_argument("--interval", type=float, default=0.25, help="Poll interval in seconds")
    parser.add_argument("--raw", type=str, default=None, help="Read raw hex at offset, e.g. '0x537C:32'")
    parser.add_argument("--scan", type=str, default=None, help="Scan range for changes, e.g. '0x5600:256'")
    args = parser.parse_args()

    reader = MegaMUDMemReader()

    print("Attaching...")
    ok = reader.attach(args.name)
    if not ok:
        print("FAILED to attach. Is MegaMUD running?")
        sys.exit(1)

    print(f"Attached! base=0x{reader._base:08X} pid={reader._pid}")

    if args.raw:
        # Read raw bytes at offset
        offset_str, size_str = args.raw.split(":")
        offset = int(offset_str, 0)
        size = int(size_str)
        data = reader._read_raw(offset, size)
        if data:
            print(f"Raw bytes at +0x{offset:X} ({size} bytes):")
            for i in range(0, len(data), 16):
                hex_part = " ".join(f"{b:02X}" for b in data[i:i+16])
                ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in data[i:i+16])
                print(f"  {offset+i:06X}: {hex_part:<48s} {ascii_part}")
        else:
            print("Failed to read")
        return

    if args.scan:
        # Scan a memory range for any i32 changes over time
        offset_str, size_str = args.scan.split(":")
        offset = int(offset_str, 0)
        size = int(size_str)
        print(f"Scanning +0x{offset:X} to +0x{offset+size:X} for changes (Ctrl+C to stop)...")
        baseline = reader._read_raw(offset, size)
        if not baseline:
            print("Failed to read baseline")
            return
        try:
            while True:
                time.sleep(args.interval)
                data = reader._read_raw(offset, size)
                if not data:
                    continue
                for i in range(0, min(len(data), len(baseline)) - 3, 4):
                    old_val = struct.unpack_from("<i", baseline, i)[0]
                    new_val = struct.unpack_from("<i", data, i)[0]
                    if old_val != new_val:
                        ts = time.strftime("%H:%M:%S")
                        off = offset + i
                        print(f"  [{ts}] +0x{off:04X}: {old_val} -> {new_val}  (0x{old_val & 0xFFFFFFFF:08X} -> 0x{new_val & 0xFFFFFFFF:08X})")
                baseline = data
        except KeyboardInterrupt:
            print("\nDone.")
        return

    if args.monitor:
        # Monitor specific named fields
        print("Reading initial state...")
        last = {}
        for offset, (name, typ) in sorted(MONITOR_OFFSETS.items()):
            val = _read_field(reader, offset, typ)
            last[(offset, name, typ)] = val
            print(f"  +0x{offset:04X} {name:20s} = {val}")

        print(f"\n--- Monitoring {len(MONITOR_OFFSETS)} fields (Ctrl+C to stop) ---")
        try:
            while True:
                time.sleep(args.interval)
                for offset, (name, typ) in sorted(MONITOR_OFFSETS.items()):
                    val = _read_field(reader, offset, typ)
                    key = (offset, name, typ)
                    if val != last[key]:
                        ts = time.strftime("%H:%M:%S")
                        print(f"  [{ts}] +0x{offset:04X} {name:20s}: {last[key]} -> {val}")
                        last[key] = val
        except KeyboardInterrupt:
            print("\nDone.")
        return

    # Show full state
    state = reader.get_full_state()
    if not state:
        print("Failed to read state")
        sys.exit(1)

    print(json.dumps(state, indent=2))

    if args.watch:
        print("\n--- Watching for changes (Ctrl+C to stop) ---")
        last = state
        try:
            while True:
                time.sleep(args.interval)
                ok = reader.attach(args.name)
                if not ok:
                    print("[detached]")
                    continue
                state = reader.get_full_state()
                if not state:
                    continue
                # Show diffs
                changes = _diff(last, state)
                if changes:
                    ts = time.strftime("%H:%M:%S")
                    for path, old, new in changes:
                        print(f"  [{ts}] {path}: {old} -> {new}")
                    last = state
        except KeyboardInterrupt:
            print("\nDone.")


def _diff(old, new, prefix=""):
    """Find differences between two nested dicts."""
    changes = []
    if isinstance(old, dict) and isinstance(new, dict):
        for key in set(list(old.keys()) + list(new.keys())):
            p = f"{prefix}.{key}" if prefix else key
            changes.extend(_diff(old.get(key), new.get(key), p))
    elif old != new:
        changes.append((prefix, old, new))
    return changes


if __name__ == "__main__":
    main()
