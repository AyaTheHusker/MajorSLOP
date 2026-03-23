IAC  = 0xFF
WILL = 0xFB
WONT = 0xFC
DO   = 0xFD
DONT = 0xFE
SB   = 0xFA
SE   = 0xF0
GA   = 0xF9
NOP  = 0xF1

# Option codes
OPT_BINARY = 0x00
OPT_ECHO   = 0x01
OPT_SGA    = 0x03
OPT_TTYPE  = 0x18
OPT_NAWS   = 0x1F

# Terminal type subneg: IS=0, SEND=1
TTYPE_IS   = 0x00
TTYPE_SEND = 0x01


def build_iac(cmd: int, opt: int) -> bytes:
    return bytes([IAC, cmd, opt])


def build_ttype_is(ttype: str = "ANSI") -> bytes:
    """Build IAC SB TTYPE IS <name> IAC SE"""
    return bytes([IAC, SB, OPT_TTYPE, TTYPE_IS]) + ttype.encode('ascii') + bytes([IAC, SE])


def build_naws(width: int = 80, height: int = 24) -> bytes:
    """Build IAC SB NAWS <w_hi> <w_lo> <h_hi> <h_lo> IAC SE"""
    return bytes([
        IAC, SB, OPT_NAWS,
        (width >> 8) & 0xFF, width & 0xFF,
        (height >> 8) & 0xFF, height & 0xFF,
        IAC, SE
    ])


class TelnetParser:
    """Separates IAC commands from data in a raw byte stream."""

    def __init__(self):
        self._pending = bytearray()
        self._in_sb = False
        self._sb_buf = bytearray()

    def feed(self, raw: bytes) -> tuple[bytes, list[tuple]]:
        """
        Returns (clean_data, iac_commands).
        clean_data: bytes with IAC sequences removed.
        iac_commands: list of tuples like (WILL, opt), (DO, opt), ('SB', opt, payload), (GA,).
        """
        data = bytearray()
        commands = []
        buf = self._pending + raw
        self._pending.clear()
        i = 0

        while i < len(buf):
            b = buf[i]

            if self._in_sb:
                if b == IAC and i + 1 < len(buf):
                    if buf[i + 1] == SE:
                        commands.append(('SB', self._sb_buf[0] if self._sb_buf else 0,
                                         bytes(self._sb_buf[1:])))
                        self._in_sb = False
                        self._sb_buf.clear()
                        i += 2
                        continue
                    elif buf[i + 1] == IAC:
                        self._sb_buf.append(IAC)
                        i += 2
                        continue
                elif b == IAC and i + 1 == len(buf):
                    self._pending.append(b)
                    break
                self._sb_buf.append(b)
                i += 1
                continue

            if b == IAC:
                if i + 1 >= len(buf):
                    self._pending.append(b)
                    break
                cmd = buf[i + 1]
                if cmd == IAC:
                    data.append(0xFF)
                    i += 2
                elif cmd == SB:
                    self._in_sb = True
                    self._sb_buf.clear()
                    i += 2
                elif cmd in (WILL, WONT, DO, DONT):
                    if i + 2 >= len(buf):
                        self._pending.extend(buf[i:])
                        break
                    opt = buf[i + 2]
                    commands.append((cmd, opt))
                    i += 3
                else:
                    commands.append((cmd,))
                    i += 2
            else:
                data.append(b)
                i += 1

        return bytes(data), commands
