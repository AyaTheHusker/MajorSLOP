import asyncio
import logging
import time
from typing import Optional, Callable, Awaitable

import re

from .telnet_protocol import (TelnetParser, IAC, WILL, WONT, DO, DONT, SB, SE,
                               OPT_BINARY, OPT_ECHO, OPT_SGA, OPT_TTYPE, OPT_NAWS,
                               TTYPE_SEND, build_iac, build_ttype_is, build_naws)
from .ansi import strip_ansi
from .config import Config

# BBS sends ESC[6n to ask "are you ANSI?" - we reply with cursor position
ANSI_DSR_RE = re.compile(rb'\x1b\[6n')

logger = logging.getLogger(__name__)


class MudProxy:
    def __init__(self, config: Config):
        self.config = config
        self.client_reader: Optional[asyncio.StreamReader] = None
        self.client_writer: Optional[asyncio.StreamWriter] = None
        self.server_reader: Optional[asyncio.StreamReader] = None
        self.server_writer: Optional[asyncio.StreamWriter] = None
        self.server_telnet = TelnetParser()
        self._server: Optional[asyncio.Server] = None
        self._running = False
        self._connected_to_bbs = False
        self._line_buffer = ""
        self._reconnecting = False
        self._server_read_task: Optional[asyncio.Task] = None

        # Telnet option state tracking (RFC 1143 Q Method)
        # State per option: 'NO', 'YES', 'WANTYES', 'WANTNO'
        self._opt_us: dict[int, str] = {}    # our side (WILL/WONT)
        self._opt_him: dict[int, str] = {}   # their side (DO/DONT)
        self._settled_options: set[int] = set()
        self._iac_log_counts: dict[tuple, int] = {}

        # Callbacks
        self.on_server_data: Optional[Callable[[str], None]] = None
        self.on_server_data_ansi: Optional[Callable[[str], None]] = None
        self.on_server_data_bytes: Optional[Callable[[bytes], None]] = None
        self.on_raw_line: Optional[Callable[[str], None]] = None
        self.on_connect: Optional[Callable[[], None]] = None
        self.on_disconnect: Optional[Callable[[], None]] = None
        # Line filter: callback(line) -> True to suppress line from MegaMUD
        self.line_filter: Optional[Callable[[str], bool]] = None
        self._filter_ansi_buffer = ''  # buffer for filtering ANSI lines before forwarding
        # Client command callback: called with each command MegaMUD sends to the server
        self.on_client_command: Optional[Callable[[str], None]] = None
        # Filter active callback: returns True only when filtering is needed
        self.filter_active: Optional[Callable[[], bool]] = None

        # Auto-login state
        self._auto_login_active = False
        self._auto_login_index = 0
        self._accumulated_text = ""

    @property
    def connected(self) -> bool:
        return self._connected_to_bbs

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._handle_client,
            self.config.proxy_host,
            self.config.proxy_port,
        )
        self._running = True
        logger.info(f"Proxy listening on {self.config.proxy_host}:{self.config.proxy_port}")

    async def stop(self) -> None:
        self._running = False
        await self._close_server_connection()
        if self._server:
            self._server.close()
            await self._server.wait_closed()

    async def connect_to_bbs(self) -> bool:
        try:
            self.server_reader, self.server_writer = await asyncio.wait_for(
                asyncio.open_connection(self.config.bbs_host, self.config.bbs_port),
                timeout=15,
            )
            self._connected_to_bbs = True
            self.server_telnet = TelnetParser()
            self._line_buffer = ""
            self._accumulated_text = ""
            # Reset telnet option state for new connection
            self._opt_us.clear()
            self._opt_him.clear()
            self._settled_options.clear()
            self._iac_log_counts.clear()
            logger.info(f"Connected to BBS {self.config.bbs_host}:{self.config.bbs_port}")

            # Proactively announce terminal capabilities
            # Track state as WANTYES/WANTNO until BBS responds
            self.server_writer.write(
                build_iac(WILL, OPT_TTYPE) +
                build_iac(WILL, OPT_NAWS) +
                build_naws(80, 24) +
                build_iac(WILL, OPT_SGA) +
                build_iac(DO, OPT_SGA) +
                build_iac(DO, OPT_ECHO) +
                build_iac(DO, OPT_BINARY) +
                build_iac(WILL, OPT_BINARY)
            )
            # Mark our WILL options as WANTYES (awaiting DO/DONT response)
            for opt in (OPT_TTYPE, OPT_NAWS, OPT_SGA, OPT_BINARY):
                self._opt_us[opt] = 'WANTYES'
            # Mark our DO options as WANTYES (awaiting WILL/WONT response)
            for opt in (OPT_SGA, OPT_ECHO, OPT_BINARY):
                self._opt_him[opt] = 'WANTYES'
            await self.server_writer.drain()
            logger.info("Telnet: Sent initial capability announcements")

            if self.on_connect:
                self.on_connect()
            # Start reading from BBS immediately so GUI shows data
            self._server_read_task = asyncio.create_task(self._forward_server_to_client())
            return True
        except Exception as e:
            logger.error(f"Failed to connect to BBS: {e}")
            if self.on_disconnect:
                self.on_disconnect()
            return False

    async def _close_server_connection(self) -> None:
        if self.server_writer and not self.server_writer.is_closing():
            try:
                self.server_writer.close()
                await self.server_writer.wait_closed()
            except Exception:
                pass
        self.server_writer = None
        self.server_reader = None
        was_connected = self._connected_to_bbs
        self._connected_to_bbs = False
        if was_connected and self.on_disconnect:
            self.on_disconnect()

    async def inject_command(self, command: str) -> None:
        if self.server_writer and not self.server_writer.is_closing():
            self.server_writer.write((command + '\r\n').encode('ascii', errors='replace'))
            await self.server_writer.drain()
            logger.info(f"Injected: {command}")

    async def _handle_client(self, reader: asyncio.StreamReader,
                             writer: asyncio.StreamWriter) -> None:
        logger.info("MegaMud client connected")
        self.client_reader = reader
        self.client_writer = writer

        # Check if existing BBS connection is actually alive
        needs_connect = not self._connected_to_bbs
        if self._connected_to_bbs:
            # Validate: is the writer dead or is the read task done?
            if (self.server_writer is None or self.server_writer.is_closing() or
                    (hasattr(self, '_server_read_task') and self._server_read_task.done())):
                logger.info("BBS connection stale, reconnecting")
                await self._close_server_connection()
                needs_connect = True

        if needs_connect:
            if not await self.connect_to_bbs():
                writer.close()
                return

        # MegaMud just bridges into the existing BBS connection.
        # Server read loop is already running from connect_to_bbs().
        # Just start forwarding client -> server.
        try:
            await self._forward_client_to_server()
        except Exception as e:
            logger.info(f"Client session ended: {e}")
        finally:
            self.client_reader = None
            self.client_writer = None
            if not writer.is_closing():
                writer.close()
            logger.info("MegaMud client disconnected")

    async def _auto_reconnect(self) -> None:
        if self._reconnecting:
            return
        self._reconnecting = True
        self._auto_login_active = True
        self._auto_login_index = 0

        while self._running and self.config.auto_reconnect:
            # Don't reconnect if no MegaMud client is connected — avoids
            # burning through BBS connections in a tight loop
            if not (self.client_writer and not self.client_writer.is_closing()):
                logger.info("No client connected, skipping auto-reconnect")
                self._reconnecting = False
                return
            logger.info(f"Reconnecting in {self.config.reconnect_delay}s...")
            await asyncio.sleep(self.config.reconnect_delay)
            if await self.connect_to_bbs():
                asyncio.create_task(self._forward_server_to_client())
                if self.client_writer and not self.client_writer.is_closing():
                    asyncio.create_task(self._forward_client_to_server())
                self._reconnecting = False
                return
        self._reconnecting = False

    def _check_auto_login(self, text: str) -> None:
        if not self._auto_login_active:
            return
        self._accumulated_text += text
        triggers = self._build_login_triggers()
        if self._auto_login_index >= len(triggers):
            self._auto_login_active = False
            return

        pattern, response, delay_ms = triggers[self._auto_login_index]
        if pattern.lower() in self._accumulated_text.lower():
            self._accumulated_text = ""
            self._auto_login_index += 1
            asyncio.create_task(self._delayed_send(response, delay_ms / 1000.0))

    def _build_login_triggers(self) -> list[tuple[str, str, int]]:
        triggers = []
        if self.config.auto_login_triggers:
            for t in self.config.auto_login_triggers:
                response = t['response']
                if response == '{username}':
                    response = self.config.username
                elif response == '{password}':
                    response = self.config.password
                triggers.append((t['pattern'], response, t.get('delay_ms', 500)))
        return triggers

    def _us_state(self, opt: int) -> str:
        return self._opt_us.get(opt, 'NO')

    def _him_state(self, opt: int) -> str:
        return self._opt_him.get(opt, 'NO')

    # Options we're willing to enable
    _SUPPORTED_US = frozenset({OPT_TTYPE, OPT_NAWS, OPT_SGA, OPT_BINARY})
    _SUPPORTED_HIM = frozenset({OPT_ECHO, OPT_SGA, OPT_BINARY})

    async def _handle_server_iac(self, commands: list[tuple]) -> None:
        """Respond to telnet option negotiations using RFC 1143 Q Method."""
        if not self.server_writer or self.server_writer.is_closing():
            return
        for cmd in commands:
            if len(cmd) == 2:
                verb, opt = cmd
                if verb == DO:
                    # BBS asks us to enable an option
                    state = self._us_state(opt)
                    if state == 'NO':
                        if opt in self._SUPPORTED_US:
                            self.server_writer.write(build_iac(WILL, opt))
                            await self.server_writer.drain()
                            self._opt_us[opt] = 'YES'
                            logger.info(f"Telnet: WILL opt {opt} (BBS said DO)")
                            # Send NAWS data after agreeing
                            if opt == OPT_NAWS:
                                self.server_writer.write(build_naws(80, 24))
                                await self.server_writer.drain()
                        else:
                            self.server_writer.write(build_iac(WONT, opt))
                            await self.server_writer.drain()
                            logger.debug(f"Telnet: WONT opt {opt} (unsupported)")
                    elif state == 'WANTNO':
                        # We sent WONT, BBS says DO — error per RFC, stay NO
                        self._opt_us[opt] = 'NO'
                        logger.warning(f"Telnet: opt {opt} DONT in WANTNO (error)")
                    elif state == 'WANTYES':
                        # We sent WILL, BBS agrees
                        self._opt_us[opt] = 'YES'
                        logger.info(f"Telnet: opt {opt} now YES")
                    # state == 'YES': already enabled, ignore (no response)

                elif verb == DONT:
                    # BBS tells us to disable an option
                    state = self._us_state(opt)
                    if state == 'YES':
                        # Currently enabled, must disable and acknowledge
                        self.server_writer.write(build_iac(WONT, opt))
                        await self.server_writer.drain()
                        self._opt_us[opt] = 'NO'
                        logger.info(f"Telnet: WONT opt {opt} (BBS said DONT, was YES)")
                    elif state == 'WANTYES':
                        # We sent WILL, BBS refused — just go to NO
                        self._opt_us[opt] = 'NO'
                        logger.info(f"Telnet: opt {opt} refused (DONT to our WILL)")
                    elif state == 'WANTNO':
                        # We sent WONT, BBS confirms — go to NO
                        self._opt_us[opt] = 'NO'
                        logger.debug(f"Telnet: opt {opt} confirmed NO")
                    # state == 'NO': already disabled, no response (breaks loop)

                elif verb == WILL:
                    # BBS offers to enable an option
                    state = self._him_state(opt)
                    if state == 'NO':
                        if opt in self._SUPPORTED_HIM:
                            self.server_writer.write(build_iac(DO, opt))
                            await self.server_writer.drain()
                            self._opt_him[opt] = 'YES'
                            logger.info(f"Telnet: DO opt {opt} (BBS said WILL)")
                        else:
                            self.server_writer.write(build_iac(DONT, opt))
                            await self.server_writer.drain()
                            logger.debug(f"Telnet: DONT opt {opt} (unsupported)")
                    elif state == 'WANTNO':
                        # Error per RFC
                        self._opt_him[opt] = 'NO'
                    elif state == 'WANTYES':
                        self._opt_him[opt] = 'YES'
                    # state == 'YES': already enabled, ignore

                elif verb == WONT:
                    # BBS refuses/disables an option
                    state = self._him_state(opt)
                    if state == 'YES':
                        self.server_writer.write(build_iac(DONT, opt))
                        await self.server_writer.drain()
                        self._opt_him[opt] = 'NO'
                        logger.info(f"Telnet: DONT opt {opt} (BBS said WONT, was YES)")
                    elif state == 'WANTYES':
                        self._opt_him[opt] = 'NO'
                        logger.info(f"Telnet: opt {opt} refused (WONT to our DO)")
                    elif state == 'WANTNO':
                        self._opt_him[opt] = 'NO'
                    # state == 'NO': already disabled, no response

            elif len(cmd) == 3 and cmd[0] == 'SB':
                _, opt, payload = cmd
                if opt == OPT_TTYPE and payload and payload[0] == TTYPE_SEND:
                    # BBS is asking "what's your terminal type?" - reply ANSI
                    self.server_writer.write(build_ttype_is("ANSI"))
                    await self.server_writer.drain()
                    logger.info("Telnet: Sent TTYPE IS ANSI")

    async def _delayed_send(self, text: str, delay: float) -> None:
        await asyncio.sleep(delay)
        if self.server_writer and not self.server_writer.is_closing():
            self.server_writer.write((text + '\r\n').encode('ascii', errors='replace'))
            await self.server_writer.drain()
            logger.info(f"Auto-login sent: {text[:20]}{'...' if len(text) > 20 else ''}")

    async def _forward_server_to_client(self) -> None:
        while self._running and self.server_reader:
            try:
                data = await self.server_reader.read(4096)
            except Exception:
                break
            if not data:
                break

            # Respond to ANSI cursor position request (ESC[6n) - this is how
            # Worldgroup/MajorBBS detects ANSI terminal support
            if ANSI_DSR_RE.search(data):
                if not (self.client_writer and not self.client_writer.is_closing()):
                    # No MegaMud client, proxy responds directly
                    self.server_writer.write(b'\x1b[24;80R')
                    await self.server_writer.drain()
                    logger.info("ANSI: Responded to DSR (ESC[6n) with cursor pos 24;80")

            # Parse telnet first (need clean data for filtering)
            clean_data, iac_cmds = self.server_telnet.feed(data)

            # Handle telnet negotiation (always, not just when no client)
            if iac_cmds:
                for cmd in iac_cmds:
                    self._iac_log_counts[cmd] = self._iac_log_counts.get(cmd, 0) + 1
                    count = self._iac_log_counts[cmd]
                    if count <= 3 or count % 100 == 0:
                        logger.debug(f"BBS IAC: {cmd} (x{count})")
                await self._handle_server_iac(iac_cmds)

            # Forward to MegaMUD — with optional line filtering
            # Only use the buffered filter when actively suppressing something
            use_filter = (self.line_filter and clean_data and
                          self.filter_active and self.filter_active())
            if self.client_writer and not self.client_writer.is_closing():
                try:
                    if use_filter:
                        # Filter using clean_data (post-telnet, no IAC bytes)
                        # to avoid mangling telnet control sequences
                        text = clean_data.decode('cp437', errors='replace')
                        self._filter_ansi_buffer += text
                        forward_parts = []
                        while '\r\n' in self._filter_ansi_buffer:
                            ansi_line, self._filter_ansi_buffer = \
                                self._filter_ansi_buffer.split('\r\n', 1)
                            stripped_line = strip_ansi(ansi_line)
                            if not self.line_filter(stripped_line):
                                forward_parts.append(ansi_line + '\r\n')
                        # Flush partial data — but only if it looks safe
                        # (HP prompts end with ]: which MegaMUD needs to see)
                        if self._filter_ansi_buffer and '\r\n' not in self._filter_ansi_buffer:
                            # Check if the partial contains "pro" echo we should suppress
                            partial_stripped = strip_ansi(self._filter_ansi_buffer).strip()
                            if partial_stripped.lower() == 'pro' or partial_stripped.lower().endswith(':pro'):
                                # Hold it — will be filtered as a complete line when \r\n arrives
                                pass
                            else:
                                forward_parts.append(self._filter_ansi_buffer)
                                self._filter_ansi_buffer = ''
                        if forward_parts:
                            forward_text = ''.join(forward_parts)
                            self.client_writer.write(
                                forward_text.encode('cp437', errors='replace'))
                            await self.client_writer.drain()
                        # Still forward any IAC commands in the raw data
                        if iac_cmds and not clean_data:
                            self.client_writer.write(data)
                            await self.client_writer.drain()
                    else:
                        # No filter — forward raw bytes untouched
                        # Flush any leftover filter buffer first
                        if self._filter_ansi_buffer:
                            leftover = self._filter_ansi_buffer.encode(
                                'cp437', errors='replace')
                            self._filter_ansi_buffer = ''
                            self.client_writer.write(leftover)
                        self.client_writer.write(data)
                        await self.client_writer.drain()
                except Exception:
                    pass

            if clean_data:
                # Decode CP437 to Unicode, then encode as UTF-8 for VTE
                text = clean_data.decode('cp437', errors='replace')
                if self.on_server_data_bytes:
                    self.on_server_data_bytes(text.encode('utf-8', errors='replace'))
                stripped = strip_ansi(text)

                # Check auto-login triggers
                self._check_auto_login(stripped)

                # Buffer ANSI lines in parallel with stripped lines
                self._ansi_line_buffer = getattr(self, '_ansi_line_buffer', '') + text

                # Buffer into lines for the parser
                self._line_buffer += stripped
                while '\r\n' in self._line_buffer:
                    line, self._line_buffer = self._line_buffer.split('\r\n', 1)
                    ansi_line = ''
                    if '\r\n' in self._ansi_line_buffer:
                        ansi_line, self._ansi_line_buffer = self._ansi_line_buffer.split('\r\n', 1)
                    if self.on_raw_line:
                        self.on_raw_line(line, ansi_line)
                while '\n' in self._line_buffer:
                    line, self._line_buffer = self._line_buffer.split('\n', 1)
                    ansi_line = ''
                    if '\n' in self._ansi_line_buffer:
                        ansi_line, self._ansi_line_buffer = self._ansi_line_buffer.split('\n', 1)
                    if self.on_raw_line:
                        self.on_raw_line(line, ansi_line)

        await self._close_server_connection()
        if self.config.auto_reconnect and self._running:
            asyncio.create_task(self._auto_reconnect())

    async def _forward_client_to_server(self) -> None:
        client_buffer = ''
        while self._running and self.client_reader:
            try:
                data = await self.client_reader.read(4096)
            except Exception:
                break
            if not data:
                break
            if self.server_writer and not self.server_writer.is_closing():
                try:
                    self.server_writer.write(data)
                    await self.server_writer.drain()
                except Exception:
                    break
            # Parse client commands for look detection etc.
            if self.on_client_command:
                try:
                    client_buffer += data.decode('ascii', errors='replace')
                    while '\r' in client_buffer or '\n' in client_buffer:
                        for sep in ('\r\n', '\r', '\n'):
                            if sep in client_buffer:
                                cmd, client_buffer = client_buffer.split(sep, 1)
                                cmd = cmd.strip()
                                if cmd:
                                    self.on_client_command(cmd)
                                break
                except Exception:
                    pass
