#!/usr/bin/env python3
"""Monitor MegaMUD loop flags in real-time via the proxy's web API.

Usage:
  1. Have the proxy exe running (MajorSLOP.exe under Wine)
  2. Start a loop using MegaMUD's own UI
  3. Run: python3 scripts/monitor_loop.py
  4. Let it loop a few passes, then Ctrl+C for summary

Uses /api/mem/toggle to read raw offsets — works with the CURRENT exe,
no rebuild needed. Just needs the proxy + mem_reader attached.
"""
import requests
import time
import sys

BASE = "http://127.0.0.1:8000"

# All offsets to monitor during loop behavior
FIELDS = [
    (0x54B4, "ON_ENTRY_ACTION"),
    (0x54BC, "MODE"),
    (0x54D8, "STEPS_REMAINING"),
    (0x54DC, "UNK_54DC"),
    (0x564C, "GO_FLAG"),
    (0x565C, "FIX_STEPS"),
    (0x5664, "PATHING_ACTIVE"),
    (0x5668, "LOOPING"),
    (0x566C, "AUTO_ROAMING"),
    (0x5670, "IS_LOST"),
    (0x5674, "IS_FLEEING"),
    (0x5678, "IS_RESTING"),
    (0x567C, "IS_MEDITATING"),
    (0x5698, "IN_COMBAT"),
    (0x2E70, "ROOM_CHECKSUM"),
    (0x2EA4, "DEST_CHECKSUM"),
    (0x5880, "PATH_WP_0"),
    (0x5884, "PATH_WP_1"),
    (0x5894, "PATH_TOTAL_STEPS"),
    (0x5898, "CUR_PATH_STEP"),
]


def read_offset(offset):
    """Read a single i32 via the toggle endpoint (GET-style read hack)."""
    # Use toggle endpoint with explicit value to read without flipping
    # Actually we need a raw read — let's use the state endpoint instead
    # Hack: post to toggle with the CURRENT value to effectively just read it
    # Better: use /api/state which includes pathing data
    try:
        r = requests.post(f"{BASE}/api/mem/toggle",
                          json={"offset": f"0x{offset:X}", "value": None},
                          timeout=2)
        # This won't work cleanly... let me think
    except:
        return None


def read_all_via_state():
    """Read loop fields via /api/state endpoint."""
    try:
        r = requests.get(f"{BASE}/api/state", timeout=2)
        return r.json()
    except:
        return None


def read_all_raw():
    """Read all offsets individually using toggle endpoint (read+write-back)."""
    state = {}
    for offset, name in FIELDS:
        try:
            # First read current value by toggling, then toggle back
            # This is destructive — bad idea. We need a read-only endpoint.
            pass
        except:
            state[offset] = None
    return state


def read_pathing():
    """Get pathing state from /api/state — this is what's available without rebuild."""
    try:
        r = requests.get(f"{BASE}/api/state", timeout=2)
        data = r.json()
        return data.get("pathing", {})
    except Exception as e:
        print(f"Error: {e}")
        return None


def fmt_val(name, val):
    if val is None:
        return "None"
    if isinstance(val, bool):
        return str(val)
    if isinstance(val, str):
        return val
    if "checksum" in name.lower() or "wp_" in name.lower():
        return f"0x{val & 0xFFFFFFFF:08X}"
    return str(val)


def main():
    interval = float(sys.argv[1]) if len(sys.argv) > 1 else 0.2

    print("Checking connection...")
    p = read_pathing()
    if p is None:
        print("Can't reach proxy at", BASE)
        sys.exit(1)

    print(f"Connected. Current state:")
    for k, v in sorted(p.items()):
        print(f"  {k:20s} = {v}")

    t0 = time.time()
    prev = p
    changes = []
    polls = 0

    print(f"\nMonitoring at {interval}s interval... (Ctrl+C to stop)")
    print(f"{'Time':>10s}  {'Field':20s}  {'Old':>14s} -> {'New':>14s}")
    print(f"{'-'*10}  {'-'*20}  {'-'*14}    {'-'*14}")

    try:
        while True:
            time.sleep(interval)
            polls += 1
            cur = read_pathing()
            if cur is None:
                continue

            for key in cur:
                old = prev.get(key)
                new = cur.get(key)
                if old != new:
                    elapsed = time.time() - t0
                    print(f"{elapsed:10.3f}  {key:20s}  {str(old):>14s} -> {str(new):>14s}")
                    changes.append((elapsed, key, old, new))

            prev = cur

    except KeyboardInterrupt:
        elapsed = time.time() - t0
        print(f"\n\nStopped after {elapsed:.1f}s ({polls} polls)")

        if changes:
            print(f"\n{'='*65}")
            print(f"  CHANGE LOG ({len(changes)} events)")
            print(f"{'='*65}")
            for t, name, old, new in changes:
                print(f"  {t:10.3f}  {name:20s}  {str(old):>14s} -> {str(new):>14s}")

            # Find step resets
            print(f"\n  STEP RESETS:")
            for i, (t, name, old, new) in enumerate(changes):
                if name == "step" and isinstance(new, int) and isinstance(old, int) and new < old:
                    print(f"  t={t:.3f}s: step {old} -> {new} (LOOP RESTART)")
                    for j, (t2, n2, o2, nw2) in enumerate(changes):
                        if abs(t2 - t) < 1.0 and j != i:
                            print(f"    t={t2:.3f}s: {n2} {o2} -> {nw2}")
        else:
            print("No changes detected.")

        print(f"\nFinal state:")
        final = read_pathing()
        if final:
            for k, v in sorted(final.items()):
                print(f"  {k:20s} = {v}")


if __name__ == "__main__":
    main()
