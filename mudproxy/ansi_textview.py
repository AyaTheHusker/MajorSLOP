"""Cross-platform ANSI terminal widget using Gtk.TextView.

Drop-in replacement for VTE.Terminal — supports ANSI SGR colors,
bold, cursor movement, carriage return, and scrollback.
Works on both Linux and Windows (via MSYS2/MinGW GTK4).
"""
import re
import gi
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, GLib, Pango

# Match a complete ANSI CSI sequence: ESC [ <params> <final byte>
_CSI_RE = re.compile(r'\x1b\[([0-9;]*)([A-Za-z])')


class AnsiTextView(Gtk.ScrolledWindow):
    """ANSI-capable terminal display widget using Gtk.TextView."""

    def __init__(self):
        super().__init__()

        self._textview = Gtk.TextView()
        self._textview.set_editable(False)
        self._textview.set_cursor_visible(False)
        self._textview.set_wrap_mode(Gtk.WrapMode.CHAR)
        self._textview.set_monospace(True)
        self._textview.set_vexpand(True)
        self._textview.set_hexpand(True)
        self.set_child(self._textview)

        self._buf = self._textview.get_buffer()
        self._scroll_on_output = True
        self._max_lines = 10000

        # Current SGR state
        self._bold = False
        self._fg = -1  # -1 = default
        self._bg = -1
        self._underline = False
        self._reverse = False

        # Partial escape sequence buffer (for sequences split across feed() calls)
        self._esc_buf = ""

        # 16-color palette (set by set_colors)
        self._palette = [
            '#000000', '#aa0000', '#00aa00', '#aa5500',
            '#0000aa', '#aa00aa', '#00aaaa', '#aaaaaa',
            '#555555', '#ff5555', '#55ff55', '#ffff55',
            '#5555ff', '#ff55ff', '#55ffff', '#ffffff',
        ]
        self._fg_default = '#aaaaaa'
        self._bg_default = '#000000'

        # Pre-create text tags for all color combinations
        self._tags = {}
        self._ensure_base_tags()

    def _ensure_base_tags(self):
        """Create the default text tags."""
        table = self._buf.get_tag_table()

        # Default tag
        tag = self._buf.create_tag("default",
                                    foreground=self._fg_default)
        # Bold tag (applied additively)
        self._buf.create_tag("bold", weight=Pango.Weight.BOLD)
        self._buf.create_tag("underline", underline=Pango.Underline.SINGLE)

    def _get_color_tag(self, fg: int, bg: int, bold: bool) -> Gtk.TextTag:
        """Get or create a tag for the given fg/bg/bold combination."""
        # Map bold + normal color (0-7) to bright color (8-15)
        effective_fg = fg
        if bold and 0 <= fg <= 7:
            effective_fg = fg + 8

        key = (effective_fg, bg)
        if key in self._tags:
            return self._tags[key]

        tag_name = f"c_{effective_fg}_{bg}"
        tag_table = self._buf.get_tag_table()
        tag = tag_table.lookup(tag_name)
        if tag:
            self._tags[key] = tag
            return tag

        props = {}
        if effective_fg >= 0:
            props['foreground'] = self._palette[effective_fg % 16]
        else:
            props['foreground'] = self._fg_default
        if bg >= 0:
            props['background'] = self._palette[bg % 16]

        tag = self._buf.create_tag(tag_name, **props)
        self._tags[key] = tag
        return tag

    # --- Public API (VTE-compatible) ---

    def set_font(self, font_desc: Pango.FontDescription):
        """Set the terminal font."""
        css = Gtk.CssProvider()
        family = font_desc.get_family()
        size_pt = font_desc.get_size() / Pango.SCALE
        css.load_from_data(
            f"textview {{ font-family: {family}; font-size: {size_pt}pt; }}".encode()
        )
        self._textview.get_style_context().add_provider(
            css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    def set_color_foreground(self, rgba: Gdk.RGBA):
        self._fg_default = self._rgba_to_hex(rgba)
        # Update default tag
        tag = self._buf.get_tag_table().lookup("default")
        if tag:
            tag.set_property("foreground", self._fg_default)

    def set_color_background(self, rgba: Gdk.RGBA):
        self._bg_default = self._rgba_to_hex(rgba)
        css = Gtk.CssProvider()
        css.load_from_data(
            f"textview text {{ background-color: {self._bg_default}; }}".encode()
        )
        self._textview.get_style_context().add_provider(
            css, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)

    @staticmethod
    def _rgba_to_hex(rgba: Gdk.RGBA) -> str:
        """Convert RGBA to #RRGGBB hex string for text tags."""
        r = int(rgba.red * 255)
        g = int(rgba.green * 255)
        b = int(rgba.blue * 255)
        return f'#{r:02x}{g:02x}{b:02x}'

    def set_colors(self, fg: Gdk.RGBA, bg: Gdk.RGBA, palette: list):
        """Set fg, bg, and 16-color ANSI palette."""
        self.set_color_foreground(fg)
        self.set_color_background(bg)
        self._palette = [self._rgba_to_hex(c) for c in palette]
        # Clear cached tags so they get recreated with new colors
        self._tags.clear()

    def set_size(self, cols: int, rows: int):
        """Hint for terminal size (used for width calculation)."""
        self._textview.set_size_request(cols * 8, rows * 16)

    def set_scroll_on_output(self, scroll: bool):
        self._scroll_on_output = scroll

    def set_scrollback_lines(self, lines: int):
        self._max_lines = lines

    def feed(self, data: bytes):
        """Feed raw bytes (UTF-8 with ANSI escapes) to the terminal.
        This is the main VTE-compatible entry point.
        """
        text = self._esc_buf + data.decode('utf-8', errors='replace')
        self._esc_buf = ""

        # Check if we have a partial escape sequence at the end
        # Look for ESC that might be start of incomplete sequence
        last_esc = text.rfind('\x1b')
        if last_esc >= 0:
            tail = text[last_esc:]
            # If it looks like an incomplete CSI sequence, buffer it
            if tail == '\x1b' or (tail.startswith('\x1b[') and
                                   not _CSI_RE.match(tail)):
                self._esc_buf = tail
                text = text[:last_esc]

        self._process_text(text)

        # Trim scrollback
        line_count = self._buf.get_line_count()
        if line_count > self._max_lines:
            start = self._buf.get_start_iter()
            _, trim_to = self._buf.get_iter_at_line(line_count - self._max_lines)
            self._buf.delete(start, trim_to)

        # Auto-scroll
        if self._scroll_on_output:
            end_mark = self._buf.create_mark(None, self._buf.get_end_iter(), False)
            self._textview.scroll_mark_onscreen(end_mark)
            self._buf.delete_mark(end_mark)

    def _process_text(self, text: str):
        """Parse ANSI sequences and insert styled text."""
        pos = 0
        while pos < len(text):
            # Find next ESC
            esc_pos = text.find('\x1b', pos)

            if esc_pos < 0:
                # No more escapes — insert remaining text
                self._insert_chunk(text[pos:])
                break

            # Insert text before the escape
            if esc_pos > pos:
                self._insert_chunk(text[pos:esc_pos])

            # Try to match CSI sequence
            m = _CSI_RE.match(text, esc_pos)
            if m:
                params_str = m.group(1)
                cmd = m.group(2)
                params = [int(p) if p else 0 for p in params_str.split(';')] if params_str else [0]
                self._handle_csi(cmd, params)
                pos = m.end()
            else:
                # Unknown escape — skip ESC char
                pos = esc_pos + 1

    def _insert_chunk(self, text: str):
        """Insert a chunk of plain text with current SGR styling."""
        if not text:
            return

        # Handle carriage return and newlines
        # \r\n = newline
        # bare \r = move cursor to start of current line (overwrite mode)
        # bare \n = newline (VTE keeps cursor column, but for MUD use just newline)
        parts = text.split('\r')
        for i, part in enumerate(parts):
            if i > 0:
                if part.startswith('\n'):
                    # \r\n — standard newline (no column padding)
                    self._do_insert_raw('\n')
                    part = part[1:]
                    if part:
                        self._do_insert(part)
                else:
                    # Bare \r — delete current line content and rewrite
                    self._erase_current_line()
                    if part:
                        self._do_insert(part)
            else:
                if part:
                    self._do_insert(part)

    def _do_insert(self, text: str):
        """Insert text at the end with current style tags.
        Handles bare \\n by preserving cursor column (VTE behavior).
        """
        # Handle bare \n: pad with spaces to maintain cursor column
        if '\n' in text:
            parts = text.split('\n')
            for j, part in enumerate(parts):
                if j > 0:
                    # Bare \n — find current column, then insert \n + spaces
                    end_iter = self._buf.get_end_iter()
                    line = end_iter.get_line()
                    _, line_start = self._buf.get_iter_at_line(line)
                    col = end_iter.get_offset() - line_start.get_offset()
                    self._do_insert_raw('\n' + ' ' * col)
                if part:
                    self._do_insert_raw(part)
            return

        self._do_insert_raw(text)

    def _do_insert_raw(self, text: str):
        """Insert text at end of buffer with current style tags (no \\n handling)."""
        end_iter = self._buf.get_end_iter()
        offset_before = end_iter.get_offset()
        self._buf.insert(end_iter, text)

        # Apply tags
        start_iter = self._buf.get_iter_at_offset(offset_before)
        end_iter = self._buf.get_end_iter()

        # Color tag
        fg = self._fg
        bg = self._bg
        if self._reverse:
            fg, bg = bg, fg
            if fg < 0:
                fg = -2  # sentinel for "use bg_default as fg"
            if bg < 0:
                bg = -2

        color_tag = self._get_color_tag(fg, bg, self._bold)
        self._buf.apply_tag(color_tag, start_iter, end_iter)

        if self._bold:
            self._buf.apply_tag_by_name("bold", start_iter, end_iter)
        if self._underline:
            self._buf.apply_tag_by_name("underline", start_iter, end_iter)

    def _erase_current_line(self):
        """Delete from start of current line to end of buffer (handles \\r overwrite)."""
        end_iter = self._buf.get_end_iter()
        line = end_iter.get_line()
        _, line_start = self._buf.get_iter_at_line(line)
        if line_start.get_offset() < end_iter.get_offset():
            self._buf.delete(line_start, end_iter)

    def _handle_csi(self, cmd: str, params: list[int]):
        """Handle a CSI escape sequence."""
        if cmd == 'm':
            self._handle_sgr(params)
        elif cmd == 'H' or cmd == 'f':
            # Cursor position — for BBS use we just handle ESC[H as home
            pass
        elif cmd == 'J':
            # Erase display
            n = params[0] if params else 0
            if n == 2 or n == 3:
                # Clear entire screen
                self._buf.set_text("")
        elif cmd == 'K':
            # Erase in line
            n = params[0] if params else 0
            if n == 0:
                # Erase from cursor to end of line
                end_iter = self._buf.get_end_iter()
                line = end_iter.get_line()
                line_end = end_iter.copy()
                # Already at end usually, nothing to erase
            elif n == 2:
                # Erase entire line
                end_iter = self._buf.get_end_iter()
                line = end_iter.get_line()
                _, line_start = self._buf.get_iter_at_line(line)
                self._buf.delete(line_start, end_iter)
        elif cmd == 'A':
            # Cursor up — ignore for text display
            pass
        elif cmd == 'B':
            # Cursor down — ignore
            pass
        elif cmd == 'C':
            # Cursor forward — insert spaces
            n = params[0] if params else 1
            self._do_insert(' ' * n)
        elif cmd == 'D':
            # Cursor back — ignore (handled by proxy's ANSI stripping)
            pass

    def _handle_sgr(self, params: list[int]):
        """Handle SGR (Select Graphic Rendition) parameters."""
        i = 0
        while i < len(params):
            p = params[i]
            if p == 0:
                # Reset
                self._bold = False
                self._fg = -1
                self._bg = -1
                self._underline = False
                self._reverse = False
            elif p == 1:
                self._bold = True
            elif p == 2:
                # Dim — treat as not bold
                self._bold = False
            elif p == 3:
                # Italic — ignore
                pass
            elif p == 4:
                self._underline = True
            elif p == 7:
                self._reverse = True
            elif p == 22:
                self._bold = False
            elif p == 24:
                self._underline = False
            elif p == 27:
                self._reverse = False
            elif 30 <= p <= 37:
                self._fg = p - 30
            elif p == 38:
                # Extended color: 38;5;n (256-color) or 38;2;r;g;b
                if i + 1 < len(params) and params[i + 1] == 5:
                    if i + 2 < len(params):
                        self._fg = params[i + 2] % 16  # map to 16-color
                        i += 2
                elif i + 1 < len(params) and params[i + 1] == 2:
                    # 24-bit — just use closest basic color
                    i += 4  # skip r,g,b
            elif p == 39:
                self._fg = -1  # default
            elif 40 <= p <= 47:
                self._bg = p - 40
            elif p == 48:
                # Extended bg color
                if i + 1 < len(params) and params[i + 1] == 5:
                    if i + 2 < len(params):
                        self._bg = params[i + 2] % 16
                        i += 2
                elif i + 1 < len(params) and params[i + 1] == 2:
                    i += 4
            elif p == 49:
                self._bg = -1  # default
            elif 90 <= p <= 97:
                # Bright foreground
                self._fg = p - 90 + 8
            elif 100 <= p <= 107:
                # Bright background
                self._bg = p - 100 + 8
            i += 1
