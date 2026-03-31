"""Bridge to the MegaMUD in-process DLL (MSIMG32.dll proxy).

Connects to the DLL's TCP control socket on 127.0.0.1:9901.
Provides the same read/write interface as /proc/pid/mem but runs
in-process — no syscalls, just pointer dereferences.

The DLL needs SETBASE before any struct-relative commands work.
The mem_reader calls bridge.set_base() once it finds the struct.
"""

import logging
import socket
import struct
from typing import Optional

logger = logging.getLogger(__name__)

DLL_HOST = "127.0.0.1"
DLL_PORT = 9901
TIMEOUT = 2.0


class DLLBridge:
    """TCP client for the in-process MegaMUD DLL."""

    def __init__(self):
        self._sock: Optional[socket.socket] = None
        self._connected = False
        self._base_set = False

    @property
    def connected(self) -> bool:
        return self._connected

    def connect(self) -> bool:
        """Try to connect to the DLL socket. Returns True on success."""
        if self._connected:
            return True
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(TIMEOUT)
            s.connect((DLL_HOST, DLL_PORT))
            # Read banner
            banner = s.recv(256).decode("ascii", errors="replace").strip()
            logger.info(f"DLL bridge connected: {banner}")
            self._sock = s
            self._connected = True
            return True
        except (ConnectionRefusedError, OSError, TimeoutError) as e:
            logger.debug(f"DLL bridge not available: {e}")
            self._connected = False
            return False

    def disconnect(self):
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
        self._sock = None
        self._connected = False
        self._base_set = False

    def _cmd(self, command: str) -> Optional[str]:
        """Send a command, return the response line. Returns None on error."""
        if not self._connected or not self._sock:
            return None
        try:
            self._sock.sendall((command + "\n").encode("ascii"))
            data = b""
            while b"\n" not in data:
                chunk = self._sock.recv(4096)
                if not chunk:
                    self._connected = False
                    return None
                data += chunk
            return data.decode("ascii", errors="replace").strip()
        except (OSError, TimeoutError) as e:
            logger.warning(f"DLL bridge error: {e}")
            self.disconnect()
            return None

    def _cmd_multiline(self, command: str, end_marker: str = "END") -> Optional[str]:
        """Send a command that returns multiple lines, terminated by end_marker."""
        if not self._connected or not self._sock:
            return None
        try:
            self._sock.sendall((command + "\n").encode("ascii"))
            data = b""
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    self._connected = False
                    return None
                data += chunk
                if (end_marker + "\n").encode() in data:
                    break
            return data.decode("ascii", errors="replace")
        except (OSError, TimeoutError) as e:
            logger.warning(f"DLL bridge error: {e}")
            self.disconnect()
            return None

    def enum_windows(self) -> Optional[str]:
        """Enumerate all child windows of MMMAIN. Returns raw text dump."""
        return self._cmd_multiline("ENUMWIN", "END")

    def ping(self) -> bool:
        resp = self._cmd("PING")
        return resp == "PONG"

    def set_base(self, base_addr: int) -> bool:
        """Tell the DLL where the game state struct is."""
        resp = self._cmd(f"SETBASE {base_addr:X}")
        if resp and resp.startswith("OK"):
            self._base_set = True
            logger.info(f"DLL bridge base set to 0x{base_addr:08X}")
            return True
        return False

    def set_ports(self, proxy_port: int, web_port: int) -> bool:
        """Tell the DLL what ports the proxy and web client are on."""
        resp = self._cmd(f"SETPORTS {proxy_port} {web_port}")
        return resp is not None and resp.startswith("OK")

    def read_i32(self, offset: int) -> Optional[int]:
        """Read a signed 32-bit int at struct offset."""
        resp = self._cmd(f"READ32 {offset:X}")
        if resp is None:
            return None
        try:
            val = int(resp)
            # Convert unsigned to signed
            if val >= 0x80000000:
                val -= 0x100000000
            return val
        except ValueError:
            return None

    def read_u32(self, offset: int) -> Optional[int]:
        """Read an unsigned 32-bit int at struct offset."""
        resp = self._cmd(f"READ32 {offset:X}")
        if resp is None:
            return None
        try:
            return int(resp)
        except ValueError:
            return None

    def read_string(self, offset: int, max_len: int = 256) -> str:
        """Read a null-terminated string at struct offset."""
        resp = self._cmd(f"READS {offset:X} {max_len}")
        if resp is None:
            return ""
        return resp

    def write_i32(self, offset: int, value: int) -> bool:
        """Write a 32-bit int at struct offset."""
        if value < 0:
            value += 0x100000000
        resp = self._cmd(f"WRITE32 {offset:X} {value}")
        return resp is not None and resp.startswith("OK")

    def write_u32(self, offset: int, value: int) -> bool:
        """Write an unsigned 32-bit int at struct offset."""
        resp = self._cmd(f"WRITE32 {offset:X} {value}")
        return resp is not None and resp.startswith("OK")

    def write_string(self, offset: int, value: str) -> bool:
        """Write a null-terminated string at struct offset."""
        resp = self._cmd(f"WRITES {offset:X} {value}")
        return resp is not None and resp.startswith("OK")

    def read_bytes(self, offset: int, size: int) -> Optional[bytes]:
        """Read raw bytes at struct offset."""
        resp = self._cmd(f"READ {offset:X} {size}")
        if resp is None:
            return None
        try:
            return bytes.fromhex(resp)
        except ValueError:
            return None

    def read_status(self) -> Optional[list[str]]:
        """Read all 8 parts of MegaMUD's status bar. Returns list of strings."""
        resp = self._cmd("STATUS")
        if resp is None or resp.startswith("ERR"):
            return None
        return resp.split("\t")

    def read_dialog_text(self, parent_class: str, ctrl_id: int) -> str:
        """Read text from a dialog control. Returns empty string on failure."""
        resp = self._cmd(f"DLGTEXT {parent_class} {ctrl_id}")
        if resp is None or resp.startswith("ERR"):
            return ""
        return resp

    def click_button(self, parent_class: str, ctrl_id: int) -> bool:
        """Send BM_CLICK to a button in a dialog."""
        resp = self._cmd(f"CLICK {parent_class} {ctrl_id}")
        return resp is not None and resp.startswith("OK")

    def round_tick(self, round_num: int) -> bool:
        """Notify DLL of a combat round tick for the round timer widget."""
        resp = self._cmd(f"ROUNDTICK {round_num}")
        return resp is not None and resp.startswith("OK")

    def combat_end(self) -> bool:
        """Notify DLL that combat has ended."""
        resp = self._cmd("COMBATEND")
        return resp is not None and resp.startswith("OK")

    def set_theme(self, theme: str) -> bool:
        """Set the DLL widget theme by index or name prefix."""
        resp = self._cmd(f"SETTHEME {theme}")
        return resp is not None and resp.startswith("OK")

    def snap(self, offset: int, size: int = 256) -> Optional[str]:
        """Memory snapshot/diff. First call = snapshot (OK), second call = diff."""
        if not self._connected or not self._sock:
            return None
        try:
            cmd = f"SNAP {offset:X} {size}\n"
            self._sock.sendall(cmd.encode("ascii"))
            data = b""
            while True:
                chunk = self._sock.recv(4096)
                if not chunk:
                    self._connected = False
                    return None
                data += chunk
                text = data.decode("ascii", errors="replace")
                if "END\n" in text or (text.startswith("OK") and "\n" in text):
                    return text.strip()
        except (OSError, TimeoutError) as e:
            logger.warning(f"DLL bridge error: {e}")
            self.disconnect()
            return None
