#!/usr/bin/env python3
"""Test AnsiTextView against VTE.Terminal with identical input.

Opens a window with VTE on left and AnsiTextView on right,
feeds identical ANSI byte streams to both, for visual comparison.
After all blocks are fed, extracts and compares plain text from both.
"""
import sys
import gi
gi.require_version('Gtk', '4.0')
gi.require_version('Adw', '1')
gi.require_version('Vte', '3.91')
from gi.repository import Gtk, Gdk, GLib, Pango, Adw, Vte

from mudproxy.ansi_textview import AnsiTextView

FONT = Pango.FontDescription("DejaVu Sans Mono 13")

PALETTE_HEX = [
    '#000000', '#aa0000', '#00aa00', '#aa5500',
    '#0000aa', '#aa00aa', '#00aaaa', '#aaaaaa',
    '#555555', '#ff5555', '#55ff55', '#ffff55',
    '#5555ff', '#ff55ff', '#55ffff', '#ffffff',
]

def parse_rgba(h):
    rgba = Gdk.RGBA()
    rgba.parse(h)
    return rgba

FG = parse_rgba('#aaaaaa')
BG = parse_rgba('#000000')
PALETTE = [parse_rgba(c) for c in PALETTE_HEX]

# --- Test data ---
TEST_BLOCKS = [
    # 1. Basic colored text
    b"\x1b[1;33mWelcome to MajorMUD!\x1b[0m\r\n",

    # 2. Health prompt with multiple color switches
    b"\x1b[1;31m[HP=\x1b[1;37m94\x1b[1;31m/\x1b[1;37m94\x1b[1;31m MA=\x1b[1;36m29\x1b[1;31m/\x1b[1;36m29\x1b[1;31m]\x1b[0m:\x1b[1;33mTown Square\x1b[0m\r\n",

    # 3. Multi-line room description
    b"\x1b[0;36mYou are standing in the town square. A large fountain\r\n"
    b"bubbles merrily in the center. Cobblestone paths lead\r\n"
    b"off in several directions.\x1b[0m\r\n",

    # 4. Also here with bold names
    b"\x1b[0mAlso here: \x1b[1;35mGoblin warrior\x1b[0m, \x1b[1;35mGoblin shaman\x1b[0m.\r\n",

    # 5. Exits
    b"\x1b[1;32mObvious exits\x1b[0m: \x1b[1;37mnorth\x1b[0m, \x1b[1;37msouth\x1b[0m, \x1b[1;37meast\x1b[0m, \x1b[1;37mwest\x1b[0m\r\n",

    # 6. Combat
    b"\x1b[1;33m*Combat Engaged*\x1b[0m\r\n"
    b"\x1b[1;37mYou swing your sword at the Goblin warrior!\x1b[0m\r\n"
    b"\x1b[1;31mYou hit the Goblin warrior for \x1b[1;37m23\x1b[1;31m damage!\x1b[0m\r\n"
    b"\x1b[1;33mThe Goblin warrior is DEAD!\x1b[0m\r\n",

    # 7. Carriage return overwrite
    b"\x1b[1;36mLoading: [          ] 0%\r"
    b"\x1b[1;36mLoading: [######    ] 60%\r"
    b"\x1b[1;36mLoading: [##########] 100%\x1b[0m\r\n",

    # 8. Background colors
    b"\x1b[41m RED BG \x1b[42m GREEN BG \x1b[44m BLUE BG \x1b[0m\r\n",

    # 9. Reverse video
    b"\x1b[7mReverse Video\x1b[0m Normal\r\n",

    # 10. All 16 colors
    b"\x1b[30m0 \x1b[31m1 \x1b[32m2 \x1b[33m3 \x1b[34m4 \x1b[35m5 \x1b[36m6 \x1b[37m7\x1b[0m\r\n"
    b"\x1b[90m8 \x1b[91m9 \x1b[92mA \x1b[93mB \x1b[94mC \x1b[95mD \x1b[96mE \x1b[97mF\x1b[0m\r\n",

    # 11. Bold+color (bold maps 0-7 to 8-15)
    b"\x1b[1;31mBold Red\x1b[0m \x1b[31mNormal Red\x1b[0m\r\n",

    # 12. Underline
    b"\x1b[4mUnderlined\x1b[24m Not\x1b[0m\r\n",

    # 13. Multiple SGR in one
    b"\x1b[1;4;33;44mBold+UL+Yel/Blue\x1b[0m\r\n",

    # 14. Cursor forward
    b"A\x1b[5CB (5 spaces)\r\n",

    # 15. Stat block
    b"\x1b[1;33mName: \x1b[1;37mBisquent\x1b[0m\r\n"
    b"\x1b[1;33mHits: \x1b[1;32m94\x1b[1;33m/\x1b[1;32m94\x1b[0m\r\n",

    # 16. Split escape (fed in two parts)
    None,

    # 17. Triple reset
    b"\x1b[0m\x1b[0m\x1b[0mTriple reset\r\n",

    # 18. Bare LF
    b"Line1\nLine2\r\n",

    # 19. Long line
    b"\x1b[1;34m" + b"=" * 80 + b"\x1b[0m\r\n",

    # 20. Erase line (ESC[2K)
    b"XXXXX\r\x1b[2KReplaced\r\n",
]


class TestApp(Adw.Application):
    def __init__(self):
        super().__init__(application_id='com.mudproxy.test_ansi')
        self.connect('activate', self._on_activate)

    def _on_activate(self, app):
        win = Gtk.ApplicationWindow(application=app)
        win.set_title("VTE vs AnsiTextView")
        win.set_default_size(1600, 900)

        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        paned.set_position(800)

        # LEFT: VTE
        left = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        left.append(Gtk.Label(label="VTE.Terminal (reference)"))
        self._vte = Vte.Terminal()
        self._vte.set_font(FONT)
        self._vte.set_color_foreground(FG)
        self._vte.set_color_background(BG)
        self._vte.set_colors(FG, BG, PALETTE)
        self._vte.set_size(80, 24)
        self._vte.set_scroll_on_output(True)
        self._vte.set_scrollback_lines(10000)
        self._vte.set_vexpand(True)
        self._vte.set_hexpand(True)
        left.append(self._vte)
        paned.set_start_child(left)

        # RIGHT: AnsiTextView
        right = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        right.append(Gtk.Label(label="AnsiTextView (replacement)"))
        self._atv = AnsiTextView()
        self._atv.set_font(FONT)
        self._atv.set_color_foreground(FG)
        self._atv.set_color_background(BG)
        self._atv.set_colors(FG, BG, PALETTE)
        self._atv.set_size(80, 24)
        self._atv.set_scroll_on_output(True)
        self._atv.set_scrollback_lines(10000)
        self._atv.set_vexpand(True)
        self._atv.set_hexpand(True)
        right.append(self._atv)
        paned.set_end_child(right)

        win.set_child(paned)
        win.present()

        self._test_idx = 0
        GLib.timeout_add(300, self._feed_next)

    def _feed_next(self):
        if self._test_idx >= len(TEST_BLOCKS):
            # Compare after feeding
            GLib.timeout_add(500, self._compare)
            return False

        block = TEST_BLOCKS[self._test_idx]
        self._test_idx += 1

        if block is None:
            part1 = b"\x1b["
            part2 = b"1;31mSplit escape!\x1b[0m\r\n"
            self._vte.feed(part1)
            self._atv.feed(part1)
            GLib.timeout_add(50, lambda: (self._vte.feed(part2), self._atv.feed(part2), False)[-1])
        else:
            self._vte.feed(block)
            self._atv.feed(block)

        return True

    def _compare(self):
        """Extract plain text from both and compare."""
        # Get AnsiTextView text
        buf = self._atv._buf
        start = buf.get_start_iter()
        end = buf.get_end_iter()
        atv_text = buf.get_text(start, end, False)

        # Get VTE text via get_text_range (non-deprecated)
        try:
            # Get all rows from scrollback to current
            row_count = self._vte.get_row_count()
            col_count = self._vte.get_column_count()
            # Use get_text_range: start_row, start_col, end_row, end_col
            result = self._vte.get_text_range_format(
                Vte.Format.TEXT, 0, 0, row_count - 1, col_count - 1)
            # Result might be (bool, text) or (len, text) — find the string
            if isinstance(result, tuple):
                for r in result:
                    if isinstance(r, str):
                        vte_text = r
                        break
                else:
                    vte_text = str(result)
            else:
                vte_text = str(result)
        except Exception as e:
            print(f"  VTE text extraction failed: {e}")
            # Fallback: just show ATV text
            vte_text = ""

        # VTE pads lines with spaces to terminal width, strip trailing spaces
        vte_lines = [line.rstrip() for line in vte_text.split('\n')]
        atv_lines = [line.rstrip() for line in atv_text.split('\n')]

        # Remove trailing empty lines
        while vte_lines and not vte_lines[-1]:
            vte_lines.pop()
        while atv_lines and not atv_lines[-1]:
            atv_lines.pop()

        # Write comparison to file
        with open('/tmp/ansi_compare_result.txt', 'w') as f:
            f.write("=" * 60 + "\n")
            f.write("TEXT COMPARISON (VTE vs AnsiTextView)\n")
            f.write("=" * 60 + "\n")

            max_lines = max(len(vte_lines), len(atv_lines))
            diffs = 0
            for i in range(max_lines):
                vl = vte_lines[i] if i < len(vte_lines) else "<MISSING>"
                al = atv_lines[i] if i < len(atv_lines) else "<MISSING>"
                if vl == al:
                    f.write(f"  OK {i+1}: {repr(vl)}\n")
                else:
                    diffs += 1
                    f.write(f"\n  LINE {i+1} DIFFERS:\n")
                    f.write(f"    VTE: {repr(vl)}\n")
                    f.write(f"    ATV: {repr(al)}\n\n")

            if diffs == 0:
                f.write("\n  ALL LINES MATCH!\n")
            else:
                f.write(f"\n  {diffs} line(s) differ out of {max_lines}\n")

            f.write(f"\n  VTE lines: {len(vte_lines)}, ATV lines: {len(atv_lines)}\n")
            f.write("=" * 60 + "\n")

        print(f"Comparison written to /tmp/ansi_compare_result.txt ({diffs} diffs)")
        return False


if __name__ == '__main__':
    app = TestApp()
    app.run([])
