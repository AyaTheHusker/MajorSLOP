#!/usr/bin/env python3
"""Scan memory for differences between idle and looping states.
Uses MajorSLOP's web API to read memory via the DLL bridge.

Usage:
  python3 scan_loop_diff.py idle    # take idle snapshot
  python3 scan_loop_diff.py loop    # take loop snapshot
  python3 scan_loop_diff.py diff    # compare them
"""
import requests
import json
import sys
import os

BASE = "http://127.0.0.1:8000"
SNAP_DIR = "/tmp/megamud_snaps"

# We'll read memory via the toggle endpoint's read, but we need a raw read.
# Let's use the mem/state endpoint plus direct DLL reads via a temp endpoint.
# Actually, let's just add ranges to read via existing debug output.

# For now, use the POST toggle with a read-only approach:
# Read i32 at each offset
def read_i32(offset):
    """Read an i32 by using the toggle endpoint with a GET-like read."""
    # Hack: use the mem reader's internal read via a custom endpoint
    # Actually we don't have a raw read endpoint. Let me add one approach:
    # Read via the debug info that start_loop returns... no that won't work.
    # Let's just call the DLL directly through a second connection.
    pass

# Better approach: add a temporary scan endpoint or read through the existing state.
# Simplest: make the script talk to DLL on a DIFFERENT port or through MajorSLOP's internals.

# Actually, let me just write a quick endpoint into server.py... no, can't restart.
# Let me use the websocket or...

# Simplest working approach: read /proc/PID/mem directly from Linux side
import struct
import glob

def find_megamud_pid():
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            with open(f"/proc/{p}/cmdline", "rb") as f:
                cmdline = f.read().decode("utf-8", errors="ignore").lower()
                if "megamud" in cmdline:
                    return int(p)
        except:
            pass
    return None

def find_struct_base(pid):
    """Read the DLL's struct base from its log or from the port file."""
    # Check for mudplugin port file
    port_file = glob.glob(os.path.expanduser("~/.wine/drive_c/MegaMUD/mudplugin_*.port"))
    if not port_file:
        # Try to get base from MajorSLOP API
        try:
            r = requests.get(f"{BASE}/api/mem/state", timeout=3)
            # The base isn't directly exposed... let's try another way
        except:
            pass

    # Read from the DLL bridge banner via MajorSLOP's debug
    # Actually, let's just scan for the struct base like mem_reader does
    # The struct base has specific markers we can search for

    # Simplest: read it from the process maps
    # The struct is found by scanning for writable memory with known patterns
    # But we already know the base from the DLL banner: 0x00521398
    # Let's just hardcode it from the last run, or better, read it dynamically

    # Read the MajorSLOP logs or ask the API
    return None

def read_proc_mem(pid, addr, size):
    """Read memory directly via /proc/pid/mem."""
    try:
        with open(f"/proc/{pid}/mem", "rb") as f:
            f.seek(addr)
            return f.read(size)
    except:
        return None

def take_snapshot(name):
    # Find MegaMUD PID
    pid = find_megamud_pid()
    if not pid:
        print("ERROR: Can't find MegaMUD process")
        sys.exit(1)
    print(f"MegaMUD PID: {pid}")

    # Get struct base from DLL banner cached in MajorSLOP
    # Let's try to read it from the API state
    try:
        r = requests.get(f"{BASE}/api/mem/state", timeout=3)
        data = r.json()
        if not data.get("attached"):
            print("ERROR: MajorSLOP not attached to MegaMUD")
            sys.exit(1)
    except Exception as e:
        print(f"ERROR: Can't reach MajorSLOP API: {e}")
        sys.exit(1)

    # We need the struct base. Let's get it from the DLL bridge connection info.
    # The DLLBridge stores _base after receiving the banner.
    # We can't easily get this from the API, so let's try a hack:
    # Read a known value and work backwards, or just scan.

    # Actually, simplest approach: read from the MajorSLOP log or use the
    # player name to find the struct base by scanning.

    # EVEN SIMPLER: just use the /api/mem/toggle to read values one at a time
    # The toggle endpoint reads current value when no "value" is given... wait no,
    # it flips it. We need a read-only endpoint.

    # Let me just create a batch read by hitting individual known offsets
    # through a hacky approach: toggle with current value (no-op write)

    # ACTUALLY: The cleanest way is to just add a mem read API endpoint.
    # But we can't restart the server. So let's use proc mem.

    # To find struct base: the room name "Building Rooftop" is at offset 0x2D9C
    # Search for it in the process memory
    room_name = data.get("room", {}).get("name", "")
    player_name = data.get("player", {}).get("name", "")
    print(f"Player: {player_name}, Room: {room_name}")

    # Parse /proc/pid/maps for writable regions
    regions = []
    try:
        with open(f"/proc/{pid}/maps") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and 'w' in parts[1]:
                    addrs = parts[0].split('-')
                    start = int(addrs[0], 16)
                    end = int(addrs[1], 16)
                    if end - start < 0x200000:  # skip huge regions
                        regions.append((start, end))
    except:
        print("ERROR: Can't read /proc maps")
        sys.exit(1)

    # Search for player name to find struct base
    player_bytes = player_name.encode("ascii")
    struct_base = None
    for start, end in regions:
        data_bytes = read_proc_mem(pid, start, end - start)
        if not data_bytes:
            continue
        idx = data_bytes.find(player_bytes)
        if idx >= 0:
            # Player name is at offset 0x537C in struct
            candidate = start + idx - 0x537C
            # Verify: room name should be at candidate + 0x2D9C
            room_check = read_proc_mem(pid, candidate + 0x2D9C, 50)
            if room_check and room_name.encode("ascii") in room_check:
                struct_base = candidate
                break

    if not struct_base:
        print("ERROR: Can't find struct base")
        sys.exit(1)

    print(f"Struct base: 0x{struct_base:08X}")

    # Now read all the ranges we care about
    RANGES = [
        (0x1E00, 0x100),
        (0x2E00, 0x100),
        (0x3500, 0x100),
        (0x4D00, 0x100),
        (0x5400, 0x300),
        (0x5700, 0x200),
        (0x9400, 0x300),
    ]

    snap = {}
    for off, size in RANGES:
        raw = read_proc_mem(pid, struct_base + off, size)
        if raw:
            snap[str(off)] = raw.hex()

    os.makedirs(SNAP_DIR, exist_ok=True)
    path = f"{SNAP_DIR}/{name}.json"
    with open(path, "w") as f:
        json.dump(snap, f)
    total = sum(len(v) for v in snap.values())
    print(f"Saved {name} snapshot to {path} ({total//2} bytes)")

def do_diff():
    with open(f"{SNAP_DIR}/idle.json") as f:
        before = json.load(f)
    with open(f"{SNAP_DIR}/loop.json") as f:
        after = json.load(f)

    diffs = []
    for key in before:
        b = before[key]
        a = after.get(key, "")
        if not a or len(b) != len(a):
            continue
        start = int(key)
        for i in range(0, len(b), 8):  # 8 hex chars = 4 bytes
            bval = b[i:i+8]
            aval = a[i:i+8]
            if bval != aval:
                offset = start + i // 2
                try:
                    bint = int.from_bytes(bytes.fromhex(bval), 'little', signed=True)
                    aint = int.from_bytes(bytes.fromhex(aval), 'little', signed=True)
                    buint = int.from_bytes(bytes.fromhex(bval), 'little', signed=False)
                    auint = int.from_bytes(bytes.fromhex(aval), 'little', signed=False)
                except:
                    continue
                diffs.append((offset, bint, aint, buint, auint))

    print(f"\n{'='*80}")
    print(f"DIFFERENCES: {len(diffs)} i32 values changed (idle -> loop)")
    print(f"{'='*80}")
    print(f"{'Offset':>10s}  {'Before':>12s}  {'After':>12s}  {'Before hex':>12s}  {'After hex':>12s}")
    print(f"{'-'*10}  {'-'*12}  {'-'*12}  {'-'*12}  {'-'*12}")
    for offset, bint, aint, buint, auint in sorted(diffs):
        print(f"  0x{offset:04X}  {bint:>12d}  {aint:>12d}  0x{buint:08X}  0x{auint:08X}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: scan_loop_diff.py <idle|loop|diff>")
        sys.exit(1)
    if sys.argv[1] in ("idle", "loop"):
        take_snapshot(sys.argv[1])
    elif sys.argv[1] == "diff":
        do_diff()
