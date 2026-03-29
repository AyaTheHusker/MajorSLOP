#!/usr/bin/env python3
"""Test different loop activation sequences against the DLL bridge."""
import socket
import time
import sys

HOST = "127.0.0.1"
PORT = 9901

def cmd(s, c):
    s.sendall((c + "\n").encode())
    return s.recv(4096).decode().strip()

def read32(s, off):
    r = cmd(s, f"READ32 {off:X}")
    if r.startswith("ERR"):
        return None
    return int(r)

def write32(s, off, val):
    r = cmd(s, f"WRITE32 {off:X} {val}")
    return r

def write_bytes(s, off, hexdata):
    r = cmd(s, f"WRITE {off:X} {hexdata}")
    return r

def read_status(s):
    return cmd(s, "STATUS")

def dump_state(s, label=""):
    print(f"\n{'='*60}")
    if label:
        print(f"  {label}")
        print(f"{'='*60}")
    fields = [
        (0x54B4, "ON_ENTRY_ACTION"),
        (0x54BC, "MODE"),
        (0x54D4, "MSG_CODE"),
        (0x54D8, "STEPS_REMAINING"),
        (0x54DC, "UNK_54DC"),
        (0x563C, "CONNECTED"),
        (0x5644, "IN_GAME"),
        (0x564C, "GO_FLAG"),
        (0x565C, "FIX_STEPS"),
        (0x5664, "PATHING_ACTIVE"),
        (0x5668, "LOOPING"),
        (0x566C, "AUTO_ROAMING"),
        (0x2EA4, "DEST_CHECKSUM"),
        (0x5894, "PATH_TOTAL_STEPS"),
        (0x5898, "CUR_PATH_STEP"),
        (0x5880, "PATH_WP_0"),
        (0x5884, "PATH_WP_1"),
        (0x5888, "PATH_WP_2"),
        (0x588C, "PATH_WP_3"),
        (0x5890, "PATH_SENTINEL"),
        (0x581C, "AUTO_AUTO_COMBAT"),
        (0x5820, "AUTO_AUTO_HEAL"),
    ]
    for off, name in fields:
        v = read32(s, off)
        if v is not None and v > 0x7FFFFFFF:
            v = v - 0x100000000  # signed
        print(f"  0x{off:04X} {name:25s} = {v}")

    # Read path filename
    r = cmd(s, f"READ {0x5834:X} 76")
    if not r.startswith("ERR"):
        fname = bytes.fromhex(r).split(b'\x00')[0].decode('ascii', errors='replace')
        print(f"  0x5834 {'PATH_FILE_NAME':25s} = {fname!r}")

    status = read_status(s)
    print(f"\n  STATUS BAR: {status}")

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((HOST, PORT))
    s.settimeout(5)

    # Read the banner
    banner = s.recv(4096).decode().strip()
    print(f"Banner: {banner}")
    r = cmd(s, "PING")
    print(f"PING: {r}")

    # Read current room checksum
    room_cksum = read32(s, 0x2E70)
    print(f"Current room checksum: 0x{room_cksum:08X}" if room_cksum else "Can't read room")

    # FERTLOOP.mp data
    mp_name = "FERTLOOP.mp"
    from_cksum = 0x60F00001
    to_cksum = 0x60F00001  # same = loop
    total_steps = 8

    mp_hex = mp_name.encode('ascii').hex() + "00" * (76 - len(mp_name))

    print("\n\n*** DUMP CURRENT STATE (before any writes) ***")
    dump_state(s, "BEFORE")

    input("\nPress Enter to try the loop sequence...")

    # ── ATTEMPT: Full clean state transition ──
    print("\n>>> Step 0: STOP everything")
    write32(s, 0x564C, 0)   # GO_FLAG = 0
    write32(s, 0x5664, 0)   # PATHING_ACTIVE = 0
    write32(s, 0x5668, 0)   # LOOPING = 0
    write32(s, 0x54BC, 11)  # MODE = idle
    time.sleep(0.5)
    dump_state(s, "After STOP")

    print("\n>>> Step 1: Write path data")
    write_bytes(s, 0x5834, mp_hex)          # PATH_FILE_NAME
    write32(s, 0x5880, from_cksum)          # PATH_WP_0
    write32(s, 0x5884, to_cksum)            # PATH_WP_1
    write32(s, 0x2EA4, from_cksum)          # DEST_CHECKSUM
    write32(s, 0x5894, total_steps)         # PATH_TOTAL_STEPS
    write32(s, 0x5898, 1)                   # CUR_PATH_STEP
    write32(s, 0x54D8, total_steps)         # STEPS_REMAINING
    dump_state(s, "After path data")

    print("\n>>> Step 2: Set sentinel and ON_ENTRY_ACTION")
    write32(s, 0x54DC, 0)                   # UNK_54DC = 0
    write32(s, 0x54B4, 1)                   # ON_ENTRY_ACTION = 1 (resume loop)
    dump_state(s, "After sentinel+entry_action")

    print("\n>>> Step 3: Set LOOPING=1")
    write32(s, 0x5668, 1)                   # LOOPING
    dump_state(s, "After LOOPING=1")

    print("\n>>> Step 4: Set MODE=14")
    write32(s, 0x54BC, 14)                  # MODE = walking
    dump_state(s, "After MODE=14")

    print("\n>>> Step 5: Set GO_FLAG=1")
    write32(s, 0x564C, 1)                   # GO_FLAG
    dump_state(s, "After GO_FLAG=1")

    print("\n>>> Step 6: Set PATHING_ACTIVE=1")
    write32(s, 0x5664, 1)                   # PATHING_ACTIVE
    time.sleep(1)
    dump_state(s, "After PATHING_ACTIVE=1 (+1s)")

    time.sleep(2)
    dump_state(s, "After 3 seconds")

    s.close()

if __name__ == "__main__":
    main()
