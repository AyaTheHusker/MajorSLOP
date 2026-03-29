"""
MegaMUD process memory reader — reads live game state directly from
the MegaMUD.exe process memory.

Supports two backends:
  - Linux:   /proc/<pid>/mem  (MegaMUD running under Wine)
  - Windows: OpenProcess + ReadProcessMemory via ctypes

Designed for async integration with the MajorSLOP orchestrator.
"""
import asyncio
import logging
import os
import struct
import sys
from pathlib import Path

from .megamud_offsets import *
from .dll_bridge import DLLBridge

logger = logging.getLogger(__name__)

IS_WINDOWS = sys.platform == "win32"


# ═══════════════════════════════════════════════════════════════════
#  Platform backend — abstracts process discovery and memory access
# ═══════════════════════════════════════════════════════════════════

if IS_WINDOWS:
    import ctypes
    import ctypes.wintypes as wt

    _k32 = ctypes.windll.kernel32
    _user32 = ctypes.windll.user32
    _psapi = ctypes.windll.psapi

    PROCESS_VM_READ = 0x0010
    PROCESS_VM_WRITE = 0x0020
    PROCESS_VM_OPERATION = 0x0008
    PROCESS_QUERY_INFORMATION = 0x0400
    MEM_COMMIT = 0x1000
    PAGE_READWRITE = 0x04
    PAGE_WRITECOPY = 0x08
    PAGE_EXECUTE_READWRITE = 0x40
    PAGE_EXECUTE_WRITECOPY = 0x80
    _WRITABLE_PAGES = (PAGE_READWRITE, PAGE_WRITECOPY, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_WRITECOPY)
    TH32CS_SNAPPROCESS = 0x02
    GWL_USERDATA = -21  # not used yet but available for direct struct ptr

    class PROCESSENTRY32(ctypes.Structure):
        _fields_ = [
            ("dwSize", wt.DWORD),
            ("cntUsage", wt.DWORD),
            ("th32ProcessID", wt.DWORD),
            ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
            ("th32ModuleID", wt.DWORD),
            ("cntThreads", wt.DWORD),
            ("th32ParentProcessID", wt.DWORD),
            ("pcPriClassBase", ctypes.c_long),
            ("dwFlags", wt.DWORD),
            ("szExeFile", ctypes.c_char * 260),
        ]

    class MEMORY_BASIC_INFORMATION(ctypes.Structure):
        _fields_ = [
            ("BaseAddress", ctypes.c_void_p),
            ("AllocationBase", ctypes.c_void_p),
            ("AllocationProtect", wt.DWORD),
            ("RegionSize", ctypes.c_size_t),
            ("State", wt.DWORD),
            ("Protect", wt.DWORD),
            ("Type", wt.DWORD),
        ]

    def _find_megamud_pid() -> int | None:
        """Find megamud.exe PID via CreateToolhelp32Snapshot."""
        snap = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if snap == -1:
            return None
        try:
            pe = PROCESSENTRY32()
            pe.dwSize = ctypes.sizeof(pe)
            if not _k32.Process32First(snap, ctypes.byref(pe)):
                return None
            while True:
                name = pe.szExeFile.decode("ascii", errors="ignore").lower()
                if "megamud" in name:
                    return pe.th32ProcessID
                if not _k32.Process32Next(snap, ctypes.byref(pe)):
                    break
        finally:
            _k32.CloseHandle(snap)
        return None

    def _open_process(pid: int):
        """Open process handle with VM_READ + VM_WRITE access."""
        h = _k32.OpenProcess(
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
            False, pid,
        )
        return h if h else None

    def _close_handle(handle):
        if handle:
            _k32.CloseHandle(handle)

    def _rpm(handle, addr: int, size: int) -> bytes | None:
        """ReadProcessMemory wrapper."""
        buf = ctypes.create_string_buffer(size)
        read = ctypes.c_size_t(0)
        ok = _k32.ReadProcessMemory(handle, ctypes.c_void_p(addr), buf, size, ctypes.byref(read))
        if ok and read.value == size:
            return buf.raw
        return None

    def _parse_regions(handle) -> list[dict]:
        """Enumerate writable memory regions via VirtualQueryEx."""
        regions = []
        addr = 0
        mbi = MEMORY_BASIC_INFORMATION()
        mbi_size = ctypes.sizeof(mbi)
        while addr < 0x7FFFFFFF:  # 32-bit address space
            ret = _k32.VirtualQueryEx(handle, ctypes.c_void_p(addr), ctypes.byref(mbi), mbi_size)
            if ret == 0:
                break
            base = mbi.BaseAddress or 0  # c_void_p returns None for address 0
            size = mbi.RegionSize or 0
            if mbi.State == MEM_COMMIT and mbi.Protect in _WRITABLE_PAGES:
                regions.append({
                    "start": base,
                    "end": base + size,
                    "perms": "rw",
                    "path": "",
                })
            addr = base + size
            if size == 0:
                break
        return regions

    # ── Unified read functions (Windows) ──

    _proc_handle = None  # cached process handle

    def _ensure_handle(pid: int):
        global _proc_handle
        if _proc_handle is None:
            _proc_handle = _open_process(pid)
        return _proc_handle

    def _release_handle():
        global _proc_handle
        if _proc_handle:
            _close_handle(_proc_handle)
            _proc_handle = None

    def _read_bytes(pid: int, addr: int, size: int) -> bytes | None:
        h = _ensure_handle(pid)
        if not h:
            return None
        return _rpm(h, addr, size)

    def _read_i32(pid: int, addr: int) -> int | None:
        data = _read_bytes(pid, addr, 4)
        return struct.unpack("<i", data)[0] if data and len(data) == 4 else None

    def _read_u32(pid: int, addr: int) -> int | None:
        data = _read_bytes(pid, addr, 4)
        return struct.unpack("<I", data)[0] if data and len(data) == 4 else None

    def _read_string(pid: int, addr: int, max_len: int = 256) -> str:
        data = _read_bytes(pid, addr, max_len)
        if not data:
            return ""
        null = data.find(b"\x00")
        if null >= 0:
            data = data[:null]
        return data.decode("ascii", errors="replace")

    def _write_bytes(pid: int, addr: int, data: bytes) -> bool:
        h = _ensure_handle(pid)
        if not h:
            return False
        written = ctypes.c_size_t(0)
        ok = _k32.WriteProcessMemory(h, ctypes.c_void_p(addr), data, len(data), ctypes.byref(written))
        return bool(ok and written.value == len(data))

    def _write_i32(pid: int, addr: int, value: int) -> bool:
        return _write_bytes(pid, addr, struct.pack("<i", value))

    def _write_u32(pid: int, addr: int, value: int) -> bool:
        return _write_bytes(pid, addr, struct.pack("<I", value))

    def _parse_maps(pid: int) -> list[dict]:
        h = _ensure_handle(pid)
        if not h:
            return []
        return _parse_regions(h)

else:
    # ── Linux backend ──

    def _find_megamud_pid() -> int | None:
        """Find the MegaMUD Wine process PID by scanning /proc.
        Matches 'megamud.exe' specifically to avoid matching konsole/shell
        processes that have MegaMUD in their working directory path."""
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                cmdline = (entry / "cmdline").read_bytes().lower()
                if b"megamud.exe" in cmdline:
                    return int(entry.name)
            except (PermissionError, FileNotFoundError, ProcessLookupError):
                continue
        return None

    def _parse_maps(pid: int) -> list[dict]:
        """Parse /proc/<pid>/maps to find memory regions."""
        regions = []
        try:
            with open(f"/proc/{pid}/maps") as f:
                for line in f:
                    parts = line.split()
                    start_s, end_s = parts[0].split("-")
                    regions.append({
                        "start": int(start_s, 16),
                        "end": int(end_s, 16),
                        "perms": parts[1],
                        "path": parts[-1] if len(parts) > 5 else "",
                    })
        except (FileNotFoundError, PermissionError):
            pass
        return regions

    def _read_bytes(pid: int, addr: int, size: int) -> bytes | None:
        try:
            with open(f"/proc/{pid}/mem", "rb") as f:
                f.seek(addr)
                return f.read(size)
        except (OSError, ValueError):
            return None

    def _read_i32(pid: int, addr: int) -> int | None:
        data = _read_bytes(pid, addr, 4)
        return struct.unpack("<i", data)[0] if data and len(data) == 4 else None

    def _read_u32(pid: int, addr: int) -> int | None:
        data = _read_bytes(pid, addr, 4)
        return struct.unpack("<I", data)[0] if data and len(data) == 4 else None

    def _read_string(pid: int, addr: int, max_len: int = 256) -> str:
        data = _read_bytes(pid, addr, max_len)
        if not data:
            return ""
        null = data.find(b"\x00")
        if null >= 0:
            data = data[:null]
        return data.decode("ascii", errors="replace")

    def _write_bytes(pid: int, addr: int, data: bytes) -> bool:
        try:
            with open(f"/proc/{pid}/mem", "wb") as f:
                f.seek(addr)
                f.write(data)
                return True
        except (OSError, ValueError):
            return False

    def _write_i32(pid: int, addr: int, value: int) -> bool:
        return _write_bytes(pid, addr, struct.pack("<i", value))

    def _write_u32(pid: int, addr: int, value: int) -> bool:
        return _write_bytes(pid, addr, struct.pack("<I", value))

    def _release_handle():
        pass  # no-op on Linux


# ═══════════════════════════════════════════════════════════════════
#  Struct scanner — platform-independent
# ═══════════════════════════════════════════════════════════════════

def _find_struct_base(pid: int, regions: list[dict], player_name: str | None = None) -> int | None:
    """
    Scan writable memory for the game state struct.

    Uses known offsets as a signature: if we find the player name at +0x537C
    from a candidate base, and the connected flag at +0x563C is non-zero,
    and HP at +0x53D4 is reasonable, we've found it.
    """
    for r in regions:
        if "w" not in r["perms"]:
            continue
        size = r["end"] - r["start"]
        if size > 100_000_000 or size < 0x9600:
            continue

        data = _read_bytes(pid, r["start"], size)
        if not data:
            continue

        if player_name:
            name_bytes = player_name.encode("ascii")
            offset = 0
            while True:
                idx = data.find(name_bytes, offset)
                if idx < 0:
                    break
                base_off = idx - PLAYER_NAME
                if base_off >= 0 and base_off + 0x9600 <= len(data):
                    hp_off = base_off + PLAYER_CUR_HP
                    level_off = base_off + PLAYER_LEVEL
                    if hp_off + 4 <= len(data) and level_off + 4 <= len(data):
                        hp = struct.unpack_from("<i", data, hp_off)[0]
                        level = struct.unpack_from("<i", data, level_off)[0]
                        if 0 < hp < 100000 and 1 <= level <= 1000:
                            return r["start"] + base_off
                offset = idx + 1
        else:
            # Blind scan: look for valid HP/mana/level/name signature.
            # Scan every 4 bytes — the struct is heap-allocated and not
            # necessarily page-aligned.
            for off in range(0, len(data) - 0x9600, 4):
                hp_off = off + PLAYER_CUR_HP
                if hp_off + 4 > len(data):
                    break
                hp = struct.unpack_from("<i", data, hp_off)[0]
                if not (1 < hp < 100000):
                    continue
                # max_hp must be reasonable and >= hp
                max_hp = struct.unpack_from("<i", data, off + PLAYER_MAX_HP)[0]
                if not (1 < max_hp < 100000) or max_hp < hp:
                    continue
                # mana fields must also be reasonable
                mana = struct.unpack_from("<i", data, off + PLAYER_CUR_MANA)[0]
                max_mana = struct.unpack_from("<i", data, off + PLAYER_MAX_MANA)[0]
                if not (0 <= mana <= 100000) or not (0 <= max_mana <= 100000):
                    continue
                if max_mana > 0 and mana > max_mana:
                    continue
                # Level 1-1000
                level = struct.unpack_from("<i", data, off + PLAYER_LEVEL)[0]
                if not (1 <= level <= 1000):
                    continue
                # Name must be 4+ alpha ASCII chars (MUD names are real words)
                name = data[off + PLAYER_NAME:off + PLAYER_NAME + 32]
                null = name.find(b"\x00")
                if null < 4:
                    continue
                candidate = name[:null]
                if not candidate.isascii() or not candidate.isalpha():
                    continue
                # Check room name is also readable ASCII
                room = data[off + ROOM_NAME:off + ROOM_NAME + 64]
                room_null = room.find(b"\x00")
                if room_null < 3:
                    continue
                room_str = room[:room_null]
                if not room_str.decode("ascii", errors="replace").isprintable():
                    continue
                return r["start"] + off
    return None


class MegaMUDMemReader:
    """
    Reads MegaMUD process memory to extract game state that isn't
    available through the telnet stream (stats, flags, room data, etc).

    Integrates with the MajorSLOP orchestrator as an optional data source.
    """

    def __init__(self):
        self._pid: int | None = None
        self._base: int | None = None
        self._attached = False
        self._last_attach_attempt = 0.0
        self._poll_task: asyncio.Task | None = None
        self._on_update: callable = None  # callback(state_dict)
        self._last_state: dict | None = None
        self._player_name: str | None = None
        self._dll = DLLBridge()

    @property
    def attached(self) -> bool:
        return self._attached and self._pid is not None and self._base is not None

    def set_player_name(self, name: str):
        """Update player name. If attached to wrong struct, force re-scan."""
        if not name:
            return
        if self._attached and self._base is not None:
            current = self._read_string(PLAYER_NAME)
            if current and current != name:
                logger.info(f"Player name mismatch: memory has '{current}', expected '{name}' — re-scanning")
                self._attached = False
                self._base = None
                self._last_attach_attempt = 0.0  # allow immediate re-attach
        self._player_name = name

    def attach(self, player_name: str | None = None) -> bool:
        """Try to find and attach to the MegaMUD process. Returns True on success."""
        import time
        now = time.time()
        # Don't spam attach attempts
        if now - self._last_attach_attempt < 5.0:
            return self._attached
        self._last_attach_attempt = now

        pid = _find_megamud_pid()
        if not pid:
            if self._attached:
                logger.info("MegaMUD process gone — detached")
                self._attached = False
                self._pid = None
                self._base = None
            return False

        # If PID changed, re-scan
        if pid != self._pid:
            self._pid = pid
            self._base = None
            self._attached = False
            logger.info(f"Found MegaMUD PID: {pid}")

        if not self._base:
            regions = _parse_maps(pid)
            self._base = _find_struct_base(pid, regions, player_name)
            if self._base:
                name = self._read_string(PLAYER_NAME)
                hp = self._read_i32(PLAYER_CUR_HP)
                logger.info(f"Attached to MegaMUD — base=0x{self._base:08X} player={name} hp={hp}")
                self._attached = True
                # Connect DLL bridge and hand it the struct base
                if self._dll.connect():
                    self._dll.set_base(self._base)
                    logger.info("DLL bridge connected and base set")
            else:
                logger.debug("MegaMUD found but struct not located (not in-game?)")
                return False

        return self._attached

    def detach(self):
        self._attached = False
        self._pid = None
        self._base = None
        _release_handle()
        if self._poll_task:
            self._poll_task.cancel()
            self._poll_task = None

    # ── Low-level reads ──

    def _read_i32(self, offset: int) -> int | None:
        if self._dll.connected and self._dll._base_set:
            return self._dll.read_i32(offset)
        if not self._pid or not self._base:
            return None
        return _read_i32(self._pid, self._base + offset)

    def _read_u32(self, offset: int) -> int | None:
        if self._dll.connected and self._dll._base_set:
            return self._dll.read_u32(offset)
        if not self._pid or not self._base:
            return None
        return _read_u32(self._pid, self._base + offset)

    def _read_string(self, offset: int, max_len: int = 256) -> str:
        if self._dll.connected and self._dll._base_set:
            return self._dll.read_string(offset, max_len)
        if not self._pid or not self._base:
            return ""
        return _read_string(self._pid, self._base + offset, max_len)

    def _read_bytes(self, offset: int, size: int) -> bytes | None:
        if self._dll.connected and self._dll._base_set:
            return self._dll.read_bytes(offset, size)
        if not self._pid or not self._base:
            return None
        return _read_bytes(self._pid, self._base + offset, size)

    def _read_raw(self, offset: int, size: int) -> bytes | None:
        """Read raw bytes at struct offset (for debugging)."""
        return self._read_bytes(offset, size)

    # ── Low-level writes ──

    def _write_i32(self, offset: int, value: int) -> bool:
        if self._dll.connected and self._dll._base_set:
            return self._dll.write_i32(offset, value)
        if not self._pid or not self._base:
            return False
        return _write_i32(self._pid, self._base + offset, value)

    def _write_u32(self, offset: int, value: int) -> bool:
        if self._dll.connected and self._dll._base_set:
            return self._dll.write_u32(offset, value)
        if not self._pid or not self._base:
            return False
        return _write_u32(self._pid, self._base + offset, value)

    # ── Room checksum DB ──

    _room_checksums: dict[str, int] = {}  # room name -> checksum
    _room_codes: dict[str, int] = {}      # 4-char code -> checksum

    @classmethod
    def load_room_checksums(cls, rooms_md_path: str | Path):
        """Parse MegaMUD Rooms.md to build room name -> checksum lookup."""
        cls._room_checksums.clear()
        cls._room_codes.clear()
        try:
            with open(rooms_md_path, "r", errors="replace") as f:
                for line in f:
                    parts = line.strip().split(":")
                    if len(parts) < 8:
                        continue
                    try:
                        checksum = int(parts[0], 16)
                        code = parts[5]
                        name = parts[7]
                        cls._room_checksums[name] = checksum
                        cls._room_codes[code] = checksum
                    except (ValueError, IndexError):
                        continue
            logger.info(f"Loaded {len(cls._room_checksums)} room checksums from {rooms_md_path}")
        except FileNotFoundError:
            logger.warning(f"Rooms.md not found: {rooms_md_path}")

    def find_room_checksum(self, query: str) -> tuple[str | None, int | None]:
        """Find room checksum by exact name, code, or partial match.
        Returns (room_name, checksum) or (None, None)."""
        # Exact name match
        if query in self._room_checksums:
            return query, self._room_checksums[query]
        # 4-char code match
        upper = query.upper()
        if upper in self._room_codes:
            # Find the name for this code
            for name, cksum in self._room_checksums.items():
                if cksum == self._room_codes[upper]:
                    return name, cksum
            return upper, self._room_codes[upper]
        # Case-insensitive partial match
        lower = query.lower()
        for name, cksum in self._room_checksums.items():
            if lower in name.lower():
                return name, cksum
        return None, None

    def goto_room(self, query: str, run: bool = False) -> dict:
        """@goto — walk to a room (auto-combat ON).
        If run=True, @run — disable auto-combat first (re-enabled on arrival)."""
        if not self.attached:
            return {"ok": False, "error": "not attached"}

        name, checksum = self.find_room_checksum(query)
        if checksum is None:
            return {"ok": False, "error": f"room not found: {query}"}

        current = self._read_u32(ROOM_CHECKSUM)
        if current == checksum:
            return {"ok": True, "room": name, "already_there": True}

        if run:
            self._write_u32(AUTO_COMBAT, 0)

        self._write_u32(DEST_CHECKSUM, checksum)
        self._write_u32(GO_FLAG, 1)
        self._write_u32(PATHING_ACTIVE, 1)

        logger.info(f"{'Run' if run else 'Goto'}: {name} (0x{checksum:08X})")
        return {"ok": True, "room": name, "checksum": checksum, "mode": "run" if run else "goto"}

    def start_loop(self, mp_file: str, from_checksum: int, to_checksum: int,
                   total_steps: int) -> dict:
        """@loop — load .mp path and start looping.
        Must be at or near the loop's starting room.

        Args:
            mp_file: filename like "DCCWLOOP.mp"
            from_checksum: start room checksum (from .mp header field 0)
            to_checksum: end room checksum (from .mp header field 1)
            total_steps: total steps in path (from .mp header field 2)
        """
        if not self.attached:
            return {"ok": False, "error": "not attached"}

        # Ensure .mp extension
        if not mp_file.lower().endswith(".mp"):
            mp_file += ".mp"

        # Full recipe from MEGAMUD_RE.md (VERIFIED WORKING):
        # 1. Write path filename
        if self._dll.connected and self._dll._base_set:
            self._dll.write_string(PATH_FILE_NAME, mp_file)
        else:
            padded = mp_file.encode("ascii") + b"\x00" * (76 - len(mp_file))
            self._write_bytes(PATH_FILE_NAME, padded)
        # 2-3. Write waypoint checksums
        self._write_u32(PATH_WP_0, from_checksum)
        self._write_u32(PATH_WP_1, to_checksum)
        # 4. Dest checksum (same as from_checksum for loops)
        self._write_u32(DEST_CHECKSUM, from_checksum)
        # 5-7. Write step info
        self._write_i32(PATH_TOTAL_STEPS, total_steps)
        self._write_i32(CUR_PATH_STEP, 1)
        self._write_i32(STEPS_REMAINING, total_steps)
        # 8. Clear sentinel
        self._write_i32(UNK_54DC, 0)
        # 9. MODE = 14 (walking) — CRITICAL: breaks out of rest/idle
        self._write_i32(MODE, 14)
        # 10. Set looping flag
        self._write_i32(LOOPING, 1)
        # 11-12. Activate
        self._write_u32(GO_FLAG, 1)
        self._write_u32(PATHING_ACTIVE, 1)

        logger.info(f"Loop started: {mp_file} ({total_steps} steps)")

        # Debug: read back all values to verify writes stuck
        debug = {
            "path_file": self._read_string(PATH_FILE_NAME, 76),
            "wp0": f"0x{(self._read_u32(PATH_WP_0) or 0):08X}",
            "wp1": f"0x{(self._read_u32(PATH_WP_1) or 0):08X}",
            "dest": f"0x{(self._read_u32(DEST_CHECKSUM) or 0):08X}",
            "total_steps": self._read_i32(PATH_TOTAL_STEPS),
            "cur_step": self._read_i32(CUR_PATH_STEP),
            "remaining": self._read_i32(STEPS_REMAINING),
            "unk_54dc": self._read_i32(UNK_54DC),
            "on_entry_action": self._read_i32(ON_ENTRY_ACTION),
            "mode": self._read_i32(MODE),
            "looping": self._read_i32(LOOPING),
            "go_flag": self._read_i32(GO_FLAG),
            "pathing": self._read_i32(PATHING_ACTIVE),
        }
        logger.info(f"Loop verify: {debug}")

        return {"ok": True, "file": mp_file, "steps": total_steps, "debug": debug}

    def stop_pathing(self) -> dict:
        """Stop button — clears GO_FLAG and PATHING_ACTIVE only."""
        if not self.attached:
            return {"ok": False, "error": "not attached"}

        self._write_u32(GO_FLAG, 0)
        self._write_u32(PATHING_ACTIVE, 0)

        logger.info("Pathing/looping stopped")
        return {"ok": True}

    def _write_bytes(self, offset: int, data: bytes) -> bool:
        """Write raw bytes at struct offset."""
        if self._dll.connected and self._dll._base_set:
            hex_str = data.hex()
            resp = self._dll._cmd(f"WRITE {offset:X} {hex_str}")
            return resp is not None and resp.startswith("OK")
        if not self._pid or not self._base:
            return False
        return _write_bytes(self._pid, self._base + offset, data)

    # ── High-level state reads ──

    def get_player(self) -> dict:
        return {
            "name": self._read_string(PLAYER_NAME),
            "level": self._read_i32(PLAYER_LEVEL),
            "race": self._read_i32(PLAYER_RACE),
            "class_id": self._read_i32(PLAYER_CLASS),
            "lives": self._read_i32(PLAYER_LIVES),
        }

    def get_hp_mana(self) -> dict:
        return {
            "hp": self._read_i32(PLAYER_CUR_HP),
            "max_hp": self._read_i32(PLAYER_MAX_HP),
            "mana": self._read_i32(PLAYER_CUR_MANA),
            "max_mana": self._read_i32(PLAYER_MAX_MANA),
        }

    def get_stats(self) -> dict:
        return {
            "strength": self._read_i32(PLAYER_STRENGTH),
            "agility": self._read_i32(PLAYER_AGILITY),
            "intellect": self._read_i32(PLAYER_INTELLECT),
            "wisdom": self._read_i32(PLAYER_WISDOM),
            "health": self._read_i32(PLAYER_HEALTH),
            "charm": self._read_i32(PLAYER_CHARM),
            "perception": self._read_i32(PLAYER_PERCEPTION),
            "stealth": self._read_i32(PLAYER_STEALTH),
            "thievery": self._read_i32(PLAYER_THIEVERY),
            "traps": self._read_i32(PLAYER_TRAPS),
            "picklocks": self._read_i32(PLAYER_PICKLOCKS),
            "tracking": self._read_i32(PLAYER_TRACKING),
            "martial_arts": self._read_i32(PLAYER_MARTIAL_ARTS),
            "magic_res": self._read_i32(PLAYER_MAGIC_RES),
            "spell_casting": self._read_i32(PLAYER_SPELL_CASTING),
        }

    def get_experience(self) -> dict:
        lo = self._read_u32(PLAYER_EXP_LO) or 0
        hi = self._read_u32(PLAYER_EXP_HI) or 0
        nlo = self._read_u32(PLAYER_NEED_LO) or 0
        nhi = self._read_u32(PLAYER_NEED_HI) or 0
        return {
            "experience": lo | (hi << 32),
            "needed": nlo | (nhi << 32),
        }

    def get_room(self) -> dict:
        return {
            "name": self._read_string(ROOM_NAME),
            "checksum": self._read_u32(ROOM_CHECKSUM),
            "darkness": self._read_i32(ROOM_DARKNESS),
        }

    def get_flags(self) -> dict:
        return {
            "connected": bool(self._read_i32(CONNECTED)),
            "in_game": bool(self._read_i32(IN_GAME)),
            "in_combat": bool(self._read_i32(IN_COMBAT)),
            "resting": bool(self._read_i32(IS_RESTING)),
            "meditating": bool(self._read_i32(IS_MEDITATING)),
            "sneaking": bool(self._read_i32(IS_SNEAKING)),
            "hiding": bool(self._read_i32(IS_HIDING)),
            "fleeing": bool(self._read_i32(IS_FLEEING)),
            "lost": bool(self._read_i32(IS_LOST)),
            "pathing": bool(self._read_i32(PATHING_ACTIVE)),
            "looping": bool(self._read_i32(LOOPING)),
            "auto_roaming": bool(self._read_i32(AUTO_ROAMING)),
            "blinded": bool(self._read_i32(IS_BLINDED)),
            "confused": bool(self._read_i32(IS_CONFUSED)),
            "poisoned": bool(self._read_i32(IS_POISONED)),
            "diseased": bool(self._read_i32(IS_DISEASED)),
            "losing_hp": bool(self._read_i32(IS_LOSING_HP)),
            "regenerating": bool(self._read_i32(IS_REGENERATING)),
            "fluxing": bool(self._read_i32(IS_FLUXING)),
            "held": bool(self._read_i32(IS_HELD)),
            "stunned": bool(self._read_i32(IS_STUNNED)),
            "resting_hp_full": bool(self._read_i32(RESTING_TO_FULL_HP)),
            "resting_mana_full": bool(self._read_i32(RESTING_TO_FULL_MANA)),
        }

    def get_party(self) -> dict:
        size = self._read_i32(PARTY_SIZE) or 0
        leader = self._read_string(PARTY_LEADER_NAME)
        members = []
        for i in range(min(size, 20)):
            base = PARTY_MEMBERS + (i * 0x38)
            members.append({
                "name": self._read_string(base, 12),
                "hp": self._read_i32(base + 0x10),
                "max_hp": self._read_i32(base + 0x14),
                "pct": self._read_i32(base + 0x0C),
                "flags": self._read_i32(base + 0x28) or 0,
            })
        return {
            "size": size,
            "leader": leader,
            "members": members,
            "following": bool(self._read_i32(FOLLOWING)),
        }

    def get_exp_meter(self) -> dict:
        """Read MegaMUD's Player Statistics exp meter via dialog controls."""
        if not self._dll.connected:
            return {}
        return {
            "duration": self._dll.read_dialog_text("#32770", 1134),
            "exp_made": self._dll.read_dialog_text("#32770", 1141),
            "exp_needed": self._dll.read_dialog_text("#32770", 1142),
            "exp_rate": self._dll.read_dialog_text("#32770", 1143),
            "level_in": self._dll.read_dialog_text("#32770", 1209),
        }

    def reset_exp_meter(self) -> bool:
        """Reset MegaMUD's exp meter by clicking button 1721."""
        if not self._dll.connected:
            return False
        return self._dll.click_button("#32770", 1721)

    def get_toggles(self) -> dict:
        """Read the 11 MegaMUD toolbar automation toggles + go/loop state."""
        return {
            "auto_combat": bool(self._read_i32(AUTO_COMBAT)),
            "auto_nuke": bool(self._read_i32(AUTO_NUKE)),
            "auto_heal": bool(self._read_i32(AUTO_HEAL)),
            "auto_bless": bool(self._read_i32(AUTO_BLESS)),
            "auto_light": bool(self._read_i32(AUTO_LIGHT)),
            "auto_cash": bool(self._read_i32(AUTO_CASH)),
            "auto_get": bool(self._read_i32(AUTO_GET)),
            "auto_search": bool(self._read_i32(AUTO_SEARCH)),
            "auto_sneak": bool(self._read_i32(AUTO_SNEAK)),
            "auto_hide": bool(self._read_i32(AUTO_HIDE)),
            "auto_track": bool(self._read_i32(AUTO_TRACK)),
            "go": bool(self._read_i32(GO_FLAG)),
            "looping": bool(self._read_i32(LOOPING)),
            "auto_roaming": bool(self._read_i32(AUTO_ROAMING)),
        }

    def get_pathing(self) -> dict:
        mode = self._read_i32(MODE) or 0
        pathing = bool(self._read_i32(PATHING_ACTIVE))
        looping = bool(self._read_i32(LOOPING))
        go = bool(self._read_i32(GO_FLAG))
        step = self._read_i32(CUR_PATH_STEP) or 0
        total = self._read_i32(PATH_TOTAL_STEPS) or 0
        remaining = self._read_i32(STEPS_REMAINING) or 0
        path_file = self._read_string(PATH_FILE_NAME, 76)

        # Read actual MegaMUD status bar text via DLL
        statusbar_parts = []
        if self._dll.connected:
            parts = self._dll.read_status()
            if parts:
                statusbar_parts = parts

        # Derive a fallback status from flags (only used if DLL can't read statusbar)
        if self._read_i32(IN_COMBAT):
            status = "combat"
        elif pathing and looping:
            status = "looping"
        elif pathing and mode == 14:
            status = "walking"
        elif self._read_i32(IS_RESTING):
            status = "resting"
        elif self._read_i32(IS_MEDITATING):
            status = "meditating"
        elif self._read_i32(AUTO_ROAMING):
            status = "roaming"
        else:
            status = "idle"

        return {
            "status": status,
            "statusbar": statusbar_parts,
            "pathing": pathing,
            "looping": looping,
            "go": go,
            "mode": mode,
            "step": step,
            "total_steps": total,
            "remaining": remaining,
            "path_file": path_file,
        }

    def get_loop_debug(self) -> dict | None:
        """Raw loop/pathing flag snapshot for monitoring. Returns ints, not bools."""
        if not self._attached or self._base is None:
            return None
        return {
            "on_entry_action": self._read_i32(ON_ENTRY_ACTION),
            "mode": self._read_i32(MODE),
            "steps_remaining": self._read_i32(STEPS_REMAINING),
            "unk_54dc": self._read_i32(UNK_54DC),
            "go_flag": self._read_i32(GO_FLAG),
            "fix_steps": self._read_i32(FIX_STEPS),
            "pathing_active": self._read_i32(PATHING_ACTIVE),
            "looping": self._read_i32(LOOPING),
            "auto_roaming": self._read_i32(AUTO_ROAMING),
            "is_lost": self._read_i32(IS_LOST),
            "is_fleeing": self._read_i32(IS_FLEEING),
            "is_resting": self._read_i32(IS_RESTING),
            "is_meditating": self._read_i32(IS_MEDITATING),
            "in_combat": self._read_i32(IN_COMBAT),
            "room_checksum": f"0x{(self._read_u32(ROOM_CHECKSUM) or 0):08X}",
            "dest_checksum": f"0x{(self._read_u32(DEST_CHECKSUM) or 0):08X}",
            "path_wp_0": f"0x{(self._read_u32(PATH_WP_0) or 0):08X}",
            "path_wp_1": f"0x{(self._read_u32(PATH_WP_1) or 0):08X}",
            "path_total_steps": self._read_i32(PATH_TOTAL_STEPS),
            "cur_path_step": self._read_i32(CUR_PATH_STEP),
            "path_file": self._read_string(PATH_FILE_NAME, 76),
        }

    def get_full_state(self) -> dict | None:
        """Full state snapshot. Returns None if not attached."""
        if not self._attached or self._base is None:
            return None
        try:
            return {
                "player": self.get_player(),
                "hp_mana": self.get_hp_mana(),
                "stats": self.get_stats(),
                "experience": self.get_experience(),
                "room": self.get_room(),
                "flags": self.get_flags(),
                "party": self.get_party(),
                "exp_meter": self.get_exp_meter(),
                "toggles": self.get_toggles(),
                "pathing": self.get_pathing(),
                "lag_wait": self._read_i32(LAG_WAIT_SECONDS),
            }
        except Exception as e:
            logger.warning(f"Memory read failed: {e}")
            self._attached = False
            return None

    # ── Async polling loop ──

    async def start_polling(self, interval: float = 1.0,
                            on_update: callable = None,
                            player_name: str | None = None):
        """Start background polling loop. Calls on_update(state) when state changes."""
        self._on_update = on_update

        async def _loop():
            while True:
                try:
                    if not self._attached:
                        self.attach(self._player_name or player_name)
                        if not self._attached:
                            await asyncio.sleep(5.0)
                            continue

                    state = self.get_full_state()
                    if state and state != self._last_state:
                        self._last_state = state
                        if self._on_update:
                            await self._on_update(state)

                    await asyncio.sleep(interval)
                except asyncio.CancelledError:
                    break
                except Exception as e:
                    import traceback
                    logger.warning(f"Memory poll error: {e} (pid={self._pid}, base={'0x{:08X}'.format(self._base) if self._base else None}, attached={self._attached})\n{traceback.format_exc()}")
                    self._attached = False
                    self._base = None
                    await asyncio.sleep(5.0)

        self._poll_task = asyncio.create_task(_loop())
        logger.info(f"Memory reader polling started (interval={interval}s)")

    def stop_polling(self):
        if self._poll_task:
            self._poll_task.cancel()
            self._poll_task = None
