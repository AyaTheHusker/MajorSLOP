"""
Crit Tracker — Floating text on every crit, escalating for multi-crits.
Groups crits within a configurable window, then announces with scaling style.

=== HOW TO MAKE A SETTINGS PANEL FROM A SCRIPT ===

This script demonstrates building a full Win32 GUI settings panel using
only Python's ctypes module. No tkinter, no external libraries — just
raw Win32 API calls, exactly how Windows programs did it in the 90s.

Key concepts:
  1. Window Classes — Every window type needs a WNDCLASS registered first.
     Think of it as a template: it defines the message handler (wndproc),
     background color, cursor, etc.

  2. Window Procedure (wndproc) — A callback function that Windows calls
     every time something happens to your window. Mouse clicks, paint
     requests, timer ticks — everything is a "message" with a code
     (WM_CREATE, WM_COMMAND, etc.) and two params (wParam, lParam).

  3. Child Controls — Buttons, sliders, combo boxes are just windows too.
     You create them as children of your main window using standard class
     names like "BUTTON", "COMBOBOX", "msctls_trackbar32".

  4. Message Pump — Your GUI thread sits in a loop calling GetMessage +
     DispatchMessage. This is what keeps the window alive and responsive.
     Without it, the window is dead.

  5. Thread Safety — Win32 windows belong to the thread that created them.
     Our tracker loop runs on one thread, the GUI on another. They share
     data through the _cfg dict. Python's GIL makes simple dict reads/writes
     safe enough for our purposes.

Usage:
    mud.load('crit_tracker')   # loads, starts tracking, opens settings
    settings()                 # toggle settings panel visibility
    stop()                     # stop tracking + close panel
"""
import re
import threading
import time
import ctypes
import ctypes.wintypes as wt
import traceback
import random

# ============================================================================
# Win32 Constants
# ============================================================================
# These are numeric codes that Windows uses for everything. You can find them
# all on Microsoft's docs (learn.microsoft.com) by searching the constant name.

user32 = ctypes.windll.user32    # User interface functions (windows, controls)
gdi32 = ctypes.windll.gdi32      # Graphics (fonts, colors, drawing)
kernel32 = ctypes.windll.kernel32 # System functions (module handles, etc)

# --- Window Styles (WS_*) ---
# Combined with bitwise OR to define how a window looks and behaves.
WS_OVERLAPPED   = 0x00000000  # Basic overlapped window (has title bar)
WS_CAPTION       = 0x00C00000  # Window has a title bar
WS_SYSMENU       = 0x00080000  # Window has a system menu (close button)
WS_MINIMIZEBOX   = 0x00020000  # Window has a minimize button
WS_CHILD         = 0x40000000  # This is a child of another window (controls)
WS_VISIBLE       = 0x10000000  # Window is initially visible
WS_TABSTOP       = 0x00010000  # Control can receive focus via Tab key
WS_BORDER        = 0x00800000  # Thin border
WS_VSCROLL       = 0x00200000  # Vertical scrollbar
WS_GROUP         = 0x00020000  # First control in a group
WS_EX_TOOLWINDOW = 0x00000080  # Doesn't show in taskbar (floating tool)
WS_EX_TOPMOST    = 0x00000008  # Always on top of other windows
WS_EX_NOACTIVATE = 0x08000000  # Clicking doesn't steal focus from other apps

# --- Button Styles (BS_*) ---
BS_AUTOCHECKBOX  = 0x00000003  # Checkbox that toggles automatically on click
BS_PUSHBUTTON    = 0x00000000  # Standard push button
BS_GROUPBOX      = 0x00000007  # Visual group box (label + border)

# --- Combo Box Styles (CBS_*) ---
CBS_DROPDOWNLIST = 0x0003      # Drop-down list (no typing, pick from list)
CBS_HASSTRINGS   = 0x0200      # The combo box stores string items

# --- Trackbar (Slider) Styles (TBS_*) ---
TBS_HORZ         = 0x0000      # Horizontal slider
TBS_AUTOTICKS    = 0x0001      # Show tick marks automatically

# --- Messages sent TO controls (we send these with SendMessageA) ---
BM_GETCHECK      = 0x00F0      # Get checkbox state (checked/unchecked)
BM_SETCHECK      = 0x00F1      # Set checkbox state
BST_CHECKED      = 0x0001      # Value meaning "checked"

CB_ADDSTRING     = 0x0143      # Add a string item to a combo box
CB_SETCURSEL     = 0x014E      # Select an item by index
CB_GETCURSEL     = 0x0147      # Get currently selected index

TBM_SETRANGE     = 0x0406      # Set slider min/max range
TBM_SETPOS       = 0x0405      # Set slider position
TBM_GETPOS       = 0x0400      # Get slider position

# --- Messages sent TO our wndproc (Windows sends these to us) ---
WM_CREATE        = 0x0001      # Window is being created — set up controls here
WM_DESTROY       = 0x0002      # Window is being destroyed — clean up
WM_CLOSE         = 0x0010      # User clicked X — we hide instead of destroy
WM_COMMAND       = 0x0111      # A child control was clicked/changed
WM_HSCROLL       = 0x0114      # A horizontal scrollbar/slider was moved
WM_TIMER         = 0x0113      # A timer fired
WM_CTLCOLORSTATIC = 0x0138    # Static control needs its colors set
WM_CTLCOLORDLG   = 0x0136     # Dialog needs its background color
WM_ERASEBKGND    = 0x0014     # Window background needs erasing

SW_SHOW          = 5           # ShowWindow: make visible
SW_HIDE          = 0           # ShowWindow: hide

COLOR_BTNFACE    = 15          # System color: standard button/dialog gray
IDC_ARROW        = 32512       # Standard arrow cursor

# This is the function signature for a Win32 window procedure.
# Windows calls this: long wndproc(HWND, UINT, WPARAM, LPARAM)
WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_long, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)

# WNDCLASSA struct — not in embeddable Python's ctypes.wintypes, so we define it.
# This is the 32-bit ANSI version of WNDCLASS.
class WNDCLASSA(ctypes.Structure):
    _fields_ = [
        ("style",         wt.UINT),
        ("lpfnWndProc",   WNDPROC),
        ("cbClsExtra",    ctypes.c_int),
        ("cbWndExtra",    ctypes.c_int),
        ("hInstance",     wt.HINSTANCE),
        ("hIcon",         wt.HICON),
        ("hCursor",       wt.HICON),      # HCURSOR is same as HICON
        ("hbrBackground", wt.HBRUSH),
        ("lpszMenuName",  ctypes.c_char_p),
        ("lpszClassName", ctypes.c_char_p),
    ]

# --- ctypes function prototypes ---
# Without these, ctypes defaults all args/returns to c_int which can corrupt
# the stack on functions with many pointer arguments (like CreateWindowExA).
user32.RegisterClassA.argtypes = [ctypes.POINTER(WNDCLASSA)]
user32.RegisterClassA.restype = wt.ATOM
user32.CreateWindowExA.argtypes = [
    wt.DWORD, ctypes.c_char_p, ctypes.c_char_p, wt.DWORD,
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    wt.HWND, wt.HMENU, wt.HINSTANCE, ctypes.c_void_p
]
user32.CreateWindowExA.restype = wt.HWND

# ============================================================================
# Crit Detection Regex
# ============================================================================
# Strip ANSI escape codes (color sequences) from raw terminal lines.
# Lines come in raw from the MUD with color codes like \x1b[1;31m
_RE_ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')

# Match crit lines. Anchored to ^ (start of line) so chat messages like
# "PlayerName gossips: You critically hit..." won't match — real combat
# output ALWAYS starts with "You critically".
_RE_CRIT = re.compile(r'^You critically \w+ .+ for (\d+) damage!')

# ============================================================================
# VFT Intro/Outro Effect Names
# ============================================================================
# These map to VFT effect names in vft_text.c
MOTION_NAMES = [
    "Fade",         # 0: fade in
    "Zoom In",      # 1: scale big -> normal
    "Zoom Out",     # 2: scale small -> normal
    "Slide Left",   # 3: slide from left
    "Slide Right",  # 4: slide from right
    "Slide Up",     # 5: slide from top
    "Slide Down",   # 6: slide from bottom
    "Bounce",       # 7: drop + bounce
    "Pop",          # 8: quick scale overshoot
    "Typewriter",   # 9: chars appear one by one
    "Wave In",      # 10: wave sweep across chars
    "Spin",         # 11: rotate + scale in
]
# VFT intro effect IDs matching the names above
_INTRO_FX = [
    "fade", "zoom_in", "zoom_out", "slide_l", "slide_r",
    "slide_u", "slide_d", "bounce", "pop", "typewriter",
    "wave_in", "spin",
]

EXIT_NAMES = [
    "Fade",         # 0: alpha fade out
    "Shatter",      # 1: per-char physics explosion
    "Explode",      # 2: radial burst outward
    "Melt",         # 3: chars drip down
    "Dissolve",     # 4: random chars vanish
    "Evaporate",    # 5: float up + fade
    "Spin",         # 6: rotate + scale out
]
# VFT outro effect IDs matching the names above
_OUTRO_FX = [
    "fade", "shatter", "explode", "melt", "dissolve",
    "evaporate", "spin",
]

# ============================================================================
# Settings Dict
# ============================================================================
# The tracker loop reads these, the settings panel writes them.
# Python's GIL ensures dict operations are atomic enough for this.
_cfg = {
    'enabled': True,       # Master on/off switch
    'window': 0.5,         # Seconds to group crits before announcing
    'motion': 0,           # Default motion mode (index into MOTION_NAMES)
    'exit': 0,             # Default exit mode (index into EXIT_NAMES)
    'randomize': False,    # If True, pick random motion+exit each time
}

# ============================================================================
# Escalating Visual Styles (VFT)
# ============================================================================
# Each crit count (1-5) has its own look. Uses VFT hex colors and params.
# color = hex RRGGBB, glow_color = hex for glow, size = VFT scale multiplier.
_STYLES = {
    1: {  # Single crit — red with orange glow
        'size': 3, 'color': 'FF3C1E', 'glow_color': 'FF8C00',
        'shadow': 1, 'glow': 1, 'mods': '',
    },
    2: {  # Double crit — bright orange-yellow
        'size': 4, 'color': 'FFDC00', 'glow_color': 'FF6400',
        'shadow': 1, 'glow': 1, 'mods': '',
    },
    3: {  # Triple crit — hot white with fire
        'size': 5, 'color': 'FFFFFF', 'glow_color': 'FF5000',
        'shadow': 1, 'glow': 1, 'mods': 'fire=1',
    },
    4: {  # Quad crit — magenta fire
        'size': 6, 'color': 'FF32C8', 'glow_color': 'FF0096',
        'shadow': 1, 'glow': 1, 'mods': 'fire=1|sparks=1',
    },
    5: {  # 5+ crits — electric purple with everything
        'size': 7, 'color': 'DCB4FF', 'glow_color': 'B400FF',
        'shadow': 1, 'glow': 1, 'mods': 'fire=1|sparks=1|chromatic=1',
    },
}

_LABELS = {
    1: "CRIT!",
    2: "DOUBLE CRIT!",
    3: "TRIPLE CRIT!",
    4: "QUAD CRIT!",
    5: "ALL CRITS!!",
}

# ============================================================================
# Tracker State
# ============================================================================
_running = False          # Is the tracking loop active?
_thread = None            # Thread running _loop()
_reader_id = -1           # DLL line reader slot (0-3)
_total_crits = 0          # Crits this session
_session_start = 0.0      # time.time() when start() was called
_biggest_crit = 0         # Largest single crit damage this session

# ============================================================================
# Settings Panel State
# ============================================================================
_panel_hwnd = None         # HWND of the settings window (None if not created)
_panel_thread = None       # Thread running the GUI message pump
_panel_controls = {}       # Maps control name -> HWND for reading values

# --- Control IDs ---
# When a child control fires an event (click, selection change), Windows
# sends WM_COMMAND to the parent with the control's ID in wParam.
# We assign unique IDs so we know which control triggered the event.
_ID_ENABLED      = 1001
_ID_RANDOMIZE    = 1002
_ID_MOTION       = 1003
_ID_EXIT         = 1004
_ID_WINDOW_SLIDER = 1005
_ID_TEST         = 1006
_ID_STATS_TIMER  = 1007   # Timer ID for updating stats display

# ============================================================================
# Win32 GUI Helpers
# ============================================================================

def _make_wndclass(name, wndproc_func):
    """
    Register a Win32 window class.

    Every window needs a class registered first. The class defines:
    - lpfnWndProc: the callback function for handling messages
    - hbrBackground: background color (COLOR_BTNFACE+1 = standard gray)
    - hCursor: default cursor (IDC_ARROW = normal pointer)

    Returns the class atom (a small integer identifying the class).
    """
    wc = WNDCLASSA()
    wc.lpfnWndProc = wndproc_func
    wc.hInstance = kernel32.GetModuleHandleA(None)
    wc.lpszClassName = name.encode('ascii')
    # COLOR_BTNFACE + 1 is a Win32 trick: system colors are passed as
    # (color_index + 1) cast to HBRUSH. Don't ask why, it's Win32.
    wc.hbrBackground = ctypes.cast(COLOR_BTNFACE + 1, wt.HBRUSH)
    wc.hCursor = user32.LoadCursorA(None, IDC_ARROW)
    return user32.RegisterClassA(ctypes.byref(wc))


def _create_ctrl(parent, cls, text, x, y, w, h, style=0, ctrl_id=0):
    """
    Create a child control (button, label, slider, etc).

    Args:
        parent:  HWND of the parent window
        cls:     Win32 class name — "BUTTON", "STATIC", "COMBOBOX",
                 "msctls_trackbar32" (slider), "EDIT" (text input), etc.
        text:    Initial text (label text, button caption, etc.)
        x, y:    Position relative to parent's client area
        w, h:    Width and height in pixels
        style:   Additional style flags (combined with WS_CHILD | WS_VISIBLE)
        ctrl_id: Unique ID for identifying this control in WM_COMMAND

    Returns: HWND of the created control
    """
    full_style = WS_CHILD | WS_VISIBLE | style
    hw = user32.CreateWindowExA(
        0,                                          # extended style
        cls.encode('ascii'),                        # class name
        text.encode('ascii') if text else None,     # window text
        full_style,                                 # style
        x, y, w, h,                                # position and size
        parent,                                     # parent window
        ctypes.cast(ctrl_id, wt.HMENU),            # control ID (cast to HMENU)
        kernel32.GetModuleHandleA(None),            # module instance
        None)                                       # create params
    return hw


# ============================================================================
# Window Procedure — The Heart of Any Win32 GUI
# ============================================================================
# Windows calls this function for EVERY message sent to our settings window.
# We handle the ones we care about and pass the rest to DefWindowProcA.

def _panel_wndproc(hwnd, msg, wparam, lparam):
    """
    Handle messages for the settings panel window.

    Common messages we handle:
      WM_CREATE  — window is being created, set up all controls
      WM_COMMAND — a button was clicked or combo box selection changed
      WM_HSCROLL — a slider was moved
      WM_TIMER   — our stats update timer fired
      WM_CLOSE   — user clicked X (we hide instead of destroying)
    """
    global _panel_hwnd

    if msg == WM_CREATE:
        # ---- CREATE ALL CONTROLS ----
        # This runs once when the window is first created.
        # We build the entire UI here.

        # --- Group Box: "Tracker Settings" ---
        # A group box is just a labeled rectangle that visually groups controls.
        _create_ctrl(hwnd, "BUTTON", "Tracker Settings", 10, 5, 280, 180,
                     BS_GROUPBOX)

        # --- Enabled Checkbox ---
        # BS_AUTOCHECKBOX = toggles check state on click automatically
        hw = _create_ctrl(hwnd, "BUTTON", "Enabled", 25, 28, 120, 20,
                          BS_AUTOCHECKBOX | WS_TABSTOP, _ID_ENABLED)
        _panel_controls['enabled'] = hw
        if _cfg['enabled']:
            # Pre-check the box to match current setting
            user32.SendMessageA(hw, BM_SETCHECK, BST_CHECKED, 0)

        # --- Randomize Checkbox ---
        hw = _create_ctrl(hwnd, "BUTTON", "RNG Mode", 155, 28, 130, 20,
                          BS_AUTOCHECKBOX | WS_TABSTOP, _ID_RANDOMIZE)
        _panel_controls['randomize'] = hw
        if _cfg['randomize']:
            user32.SendMessageA(hw, BM_SETCHECK, BST_CHECKED, 0)

        # --- Motion Mode Combo Box ---
        # "STATIC" is a text label. "COMBOBOX" is a drop-down selector.
        _create_ctrl(hwnd, "STATIC", "Motion:", 25, 58, 55, 18)
        hw = _create_ctrl(hwnd, "COMBOBOX", None, 85, 55, 195, 200,
                          CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP,
                          _ID_MOTION)
        _panel_controls['motion'] = hw
        # Populate the dropdown by sending CB_ADDSTRING for each item
        for name in MOTION_NAMES:
            user32.SendMessageA(hw, CB_ADDSTRING, 0, name.encode('ascii'))
        # Select the current setting
        user32.SendMessageA(hw, CB_SETCURSEL, _cfg['motion'], 0)

        # --- Exit Mode Combo Box ---
        _create_ctrl(hwnd, "STATIC", "Exit:", 25, 88, 55, 18)
        hw = _create_ctrl(hwnd, "COMBOBOX", None, 85, 85, 195, 200,
                          CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_TABSTOP,
                          _ID_EXIT)
        _panel_controls['exit'] = hw
        for name in EXIT_NAMES:
            user32.SendMessageA(hw, CB_ADDSTRING, 0, name.encode('ascii'))
        user32.SendMessageA(hw, CB_SETCURSEL, _cfg['exit'], 0)

        # --- Crit Window Slider (Trackbar) ---
        # "msctls_trackbar32" is the Win32 common control for sliders.
        # Range is in integer units — we use 1-20 and divide by 10 for 0.1-2.0s.
        _create_ctrl(hwnd, "STATIC", "Group Window:", 25, 120, 90, 18)
        hw_lbl = _create_ctrl(hwnd, "STATIC", "0.5s", 230, 120, 50, 18)
        _panel_controls['window_label'] = hw_lbl

        hw = _create_ctrl(hwnd, "msctls_trackbar32", None,
                          115, 118, 110, 22,
                          TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP,
                          _ID_WINDOW_SLIDER)
        _panel_controls['window_slider'] = hw
        # TBM_SETRANGE: lParam = (max << 16) | min. Packs both into one int.
        user32.SendMessageA(hw, TBM_SETRANGE, 1, (20 << 16) | 1)
        user32.SendMessageA(hw, TBM_SETPOS, 1, int(_cfg['window'] * 10))

        # --- Test Button ---
        # Fires a random test float so you can preview the current settings.
        _create_ctrl(hwnd, "BUTTON", "Test Float", 25, 148, 100, 26,
                     BS_PUSHBUTTON | WS_TABSTOP, _ID_TEST)

        # --- Group Box: "Session Stats" ---
        _create_ctrl(hwnd, "BUTTON", "Session Stats", 10, 192, 280, 95,
                     BS_GROUPBOX)
        hw = _create_ctrl(hwnd, "STATIC",
                          "Total Crits: 0\nBiggest Hit: 0\nSession: 0:00",
                          25, 215, 255, 60)
        _panel_controls['stats'] = hw

        # --- Stats Update Timer ---
        # SetTimer makes Windows send us WM_TIMER every 1000ms.
        # We use it to refresh the stats display.
        user32.SetTimer(hwnd, _ID_STATS_TIMER, 1000, None)
        return 0

    elif msg == WM_COMMAND:
        # --- HANDLE CONTROL EVENTS ---
        # wParam low word = control ID, high word = notification code.
        ctrl_id = wparam & 0xFFFF
        notify = (wparam >> 16) & 0xFFFF

        if ctrl_id == _ID_ENABLED:
            # Checkbox clicked — read new check state
            _cfg['enabled'] = bool(user32.SendMessageA(
                _panel_controls['enabled'], BM_GETCHECK, 0, 0))

        elif ctrl_id == _ID_RANDOMIZE:
            _cfg['randomize'] = bool(user32.SendMessageA(
                _panel_controls['randomize'], BM_GETCHECK, 0, 0))

        elif ctrl_id == _ID_MOTION and notify == 1:  # CBN_SELCHANGE = 1
            # Combo box selection changed
            _cfg['motion'] = user32.SendMessageA(
                _panel_controls['motion'], CB_GETCURSEL, 0, 0)

        elif ctrl_id == _ID_EXIT and notify == 1:
            _cfg['exit'] = user32.SendMessageA(
                _panel_controls['exit'], CB_GETCURSEL, 0, 0)

        elif ctrl_id == _ID_TEST:
            # Test button clicked — fire a random float with current settings
            _fire_test()

        return 0

    elif msg == WM_HSCROLL:
        # --- SLIDER MOVED ---
        # When a trackbar is moved, the parent gets WM_HSCROLL.
        # We read the new position and update the config + label.
        pos = user32.SendMessageA(
            _panel_controls.get('window_slider', 0), TBM_GETPOS, 0, 0)
        _cfg['window'] = pos / 10.0
        label = "%.1fs" % _cfg['window']
        user32.SetWindowTextA(
            _panel_controls.get('window_label', 0), label.encode('ascii'))
        return 0

    elif msg == WM_TIMER:
        # --- TIMER FIRED ---
        # Update the stats display every second.
        if wparam == _ID_STATS_TIMER:
            _update_stats()
        return 0

    elif msg == WM_CLOSE:
        # --- CLOSE BUTTON ---
        # Instead of destroying the window, just hide it.
        # This way settings() can show it again without recreating.
        user32.ShowWindow(hwnd, SW_HIDE)
        return 0

    elif msg == WM_DESTROY:
        # --- WINDOW DESTROYED ---
        # Clean up the timer. The window is gone for real.
        user32.KillTimer(hwnd, _ID_STATS_TIMER)
        _panel_hwnd = None
        return 0

    # For any message we don't handle, pass to default handler.
    return user32.DefWindowProcA(hwnd, msg, wparam, lparam)


# IMPORTANT: Must keep a reference to the WNDPROC wrapper!
# If Python garbage-collects it, Windows will call a dead pointer = crash.
_wndproc_ref = WNDPROC(_panel_wndproc)


# ============================================================================
# Panel Helper Functions
# ============================================================================

def _update_stats():
    """Refresh the stats label in the settings panel."""
    hw = _panel_controls.get('stats')
    if not hw:
        return
    elapsed = time.time() - _session_start if _session_start else 0
    mins = int(elapsed) // 60
    secs = int(elapsed) % 60
    text = "Total Crits: %d\nBiggest Hit: %d\nSession: %d:%02d" % (
        _total_crits, _biggest_crit, mins, secs)
    user32.SetWindowTextA(hw, text.encode('ascii'))


def _build_vft(text, style, intro_fx, outro_fx):
    """Build a VFT command string from style dict and effect names."""
    parts = [text, "center", "center",
             "size=%s" % style['size'],
             "color=%s" % style['color'],
             "intro=%s" % intro_fx,
             "outro=%s" % outro_fx,
             "hold=2"]
    if style.get('shadow'):
        parts.append("shadow=1")
    if style.get('glow'):
        parts.append("glow=%s" % style['glow_color'])
    if style.get('mods'):
        parts.append(style['mods'])
    return "|".join(parts)


def _fire_test():
    """Fire a test float text using current settings (called from Test button)."""
    motion = _cfg['motion']
    exit_m = _cfg['exit']
    if _cfg['randomize']:
        motion = random.randint(0, len(MOTION_NAMES) - 1)
        exit_m = random.randint(0, len(EXIT_NAMES) - 1)
    level = random.randint(1, 5)
    s = _STYLES[level]
    dmg = random.randint(30, 150)
    text = "%s  %d dmg" % (_LABELS[level], dmg)
    intro = _INTRO_FX[motion] if motion < len(_INTRO_FX) else "fade"
    outro = _OUTRO_FX[exit_m] if exit_m < len(_OUTRO_FX) else "fade"
    mud.vft(_build_vft(text, s, intro, outro))


def _panel_thread_fn():
    """
    GUI thread — creates the settings window and runs the message pump.

    This MUST run on its own thread because:
    1. The message pump (GetMessage loop) blocks until a message arrives
    2. Our crit tracking loop is already blocking on another thread
    3. Win32 windows belong to the thread that created them — all their
       messages get dispatched on that thread's message queue
    """
    global _panel_hwnd

    try:
        # Initialize common controls (needed for trackbar/slider).
        # Just LoadLibrary isn't enough — we need InitCommonControlsEx
        # to register the window classes like "msctls_trackbar32".
        comctl32 = ctypes.windll.comctl32

        class INITCOMMONCONTROLSEX(ctypes.Structure):
            _fields_ = [("dwSize", wt.DWORD), ("dwICC", wt.DWORD)]

        ICC_BAR_CLASSES = 0x0004  # Trackbar, status bar, toolbar
        icc = INITCOMMONCONTROLSEX()
        icc.dwSize = ctypes.sizeof(INITCOMMONCONTROLSEX)
        icc.dwICC = ICC_BAR_CLASSES
        comctl32.InitCommonControlsEx(ctypes.byref(icc))

        # Step 1: Register our custom window class
        atom = _make_wndclass("CritTrackerSettings", _wndproc_ref)
        print("[crit] RegisterClass atom=%s err=%s" % (atom, kernel32.GetLastError()))

        # Step 2: Position next to MegaMUD window (clamped to screen)
        PANEL_W, PANEL_H = 305, 325
        mw = user32.FindWindowA(b"MMMAIN", None)
        mx, my = 100, 100
        if mw:
            rc = wt.RECT()
            user32.GetWindowRect(mw, ctypes.byref(rc))
            mx = rc.right + 5   # Right edge of MegaMUD + 5px gap
            my = rc.top
        # Clamp to screen bounds so panel is always visible
        scr_w = user32.GetSystemMetrics(0)   # SM_CXSCREEN
        scr_h = user32.GetSystemMetrics(1)   # SM_CYSCREEN
        if mx + PANEL_W > scr_w:
            # Doesn't fit right of MegaMUD — put it on the left side instead
            if mw:
                mx = rc.left - PANEL_W - 5
            if mx < 0:
                mx = scr_w - PANEL_W - 10
        if my < 0:
            my = 0
        if my + PANEL_H > scr_h:
            my = scr_h - PANEL_H - 40
        print("[crit] Panel pos: %d,%d (screen %dx%d) mw=%s" % (mx, my, scr_w, scr_h, mw))

        # Step 3: Create the settings window
        # WS_EX_TOOLWINDOW = doesn't appear in taskbar (acts like a tool palette)
        _panel_hwnd = user32.CreateWindowExA(
            WS_EX_TOOLWINDOW,
            b"CritTrackerSettings",        # Our registered class name
            b"Crit Tracker Settings",       # Title bar text
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            mx, my, PANEL_W, PANEL_H,      # x, y, width, height
            0, None,                        # No parent, no menu
            kernel32.GetModuleHandleA(None),
            None)
        print("[crit] Panel hwnd=%s err=%s" % (_panel_hwnd, kernel32.GetLastError()))

        if not _panel_hwnd:
            print("[crit] FAILED to create settings window!")
            return

        user32.ShowWindow(_panel_hwnd, SW_SHOW)
        user32.UpdateWindow(_panel_hwnd)
        print("[crit] Settings panel visible")

        # Step 4: Message pump — this keeps the window alive.
        msg = wt.MSG()
        while user32.GetMessageA(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageA(ctypes.byref(msg))
    except Exception as e:
        print("[crit] PANEL THREAD CRASHED: %s" % e)
        traceback.print_exc()


def settings():
    """
    Show/hide the settings panel.
    Creates it on first call, toggles visibility after that.
    """
    global _panel_thread
    if _panel_hwnd:
        # Panel exists — toggle visibility
        if user32.IsWindowVisible(_panel_hwnd):
            user32.ShowWindow(_panel_hwnd, SW_HIDE)
        else:
            user32.ShowWindow(_panel_hwnd, SW_SHOW)
        return
    # First call — spawn the GUI thread
    _panel_thread = threading.Thread(target=_panel_thread_fn, daemon=True)
    _panel_thread.start()


# ============================================================================
# Crit Announcement
# ============================================================================

def _announce(count, total_dmg):
    """Show floating text for a batch of crits."""
    if not _cfg['enabled']:
        return
    if count > 5:
        count = 5

    label = _LABELS[count]
    s = _STYLES[count]
    text = "%s  %d dmg" % (label, total_dmg)

    # Pick motion/exit from settings, or randomize
    motion = _cfg['motion']
    exit_m = _cfg['exit']
    if _cfg['randomize']:
        motion = random.randint(0, len(MOTION_NAMES) - 1)
        exit_m = random.randint(0, len(EXIT_NAMES) - 1)

    intro = _INTRO_FX[motion] if motion < len(_INTRO_FX) else "fade"
    outro = _OUTRO_FX[exit_m] if exit_m < len(_OUTRO_FX) else "fade"
    mud.vft(_build_vft(text, s, intro, outro))


# ============================================================================
# Main Tracking Loop
# ============================================================================

def _loop():
    """
    Poll for new MUD lines and detect crits.

    Uses the DLL's multi-reader API:
    - mmudpy_line_register() gets a reader slot (0-3)
    - mmudpy_get_line(reader_id, buf, size) returns 1 if a line was available
    - Each reader has its own read pointer, so multiple scripts can
      independently read the same stream of lines.

    Crits within _cfg['window'] seconds are grouped into one announcement.
    """
    global _running, _total_crits, _biggest_crit
    try:
        buf = ctypes.create_string_buffer(4096)
        crits = 0
        crit_dmg = 0
        last_crit = 0.0

        while _running:
            got = _dll.mmudpy_get_line(_reader_id, buf, 4096)
            if got:
                # Decode the raw line and strip ANSI color codes
                raw = buf.value.decode('utf-8', errors='replace').strip()
                line = _RE_ANSI.sub('', raw)

                # Check for crit (anchored to line start)
                m = _RE_CRIT.match(line)
                if m:
                    dmg = int(m.group(1))
                    _total_crits += 1
                    if dmg > _biggest_crit:
                        _biggest_crit = dmg
                    crits += 1
                    crit_dmg += dmg
                    last_crit = time.time()

            # If we have pending crits and the window has elapsed, announce
            now = time.time()
            if crits > 0 and last_crit > 0 and (now - last_crit) > _cfg['window']:
                _announce(crits, crit_dmg)
                crits = 0
                crit_dmg = 0
                last_crit = 0.0

            # Sleep when no data to avoid busy-waiting
            if not got:
                time.sleep(0.05)
    except Exception as e:
        print("[crit] THREAD DIED: %s" % e)
        traceback.print_exc()


# ============================================================================
# Public API — called from the MMUDPy console
# ============================================================================

def start():
    """Start the crit tracker and open the settings panel."""
    global _running, _thread, _reader_id, _total_crits, _session_start, _biggest_crit
    if _running:
        print("Crit tracker already running.")
        return

    # Register a line reader slot with the DLL
    _reader_id = _dll.mmudpy_line_register()
    if _reader_id < 0:
        print("ERROR: No reader slots available.")
        return

    _running = True
    _total_crits = 0
    _biggest_crit = 0
    _session_start = time.time()

    # Start the tracking loop on a background thread
    _thread = threading.Thread(target=_loop, daemon=True)
    _thread.start()

    print("Crit tracker started.")
    mud.vft("CRIT TRACKER ON|center|center|size=3|color=00FF00|intro=pop|outro=fade|hold=2")

    # Auto-open the settings panel
    settings()


def stop():
    """Stop the crit tracker and close the settings panel."""
    global _running, _reader_id
    _running = False

    # Release our line reader slot
    if _reader_id >= 0:
        _dll.mmudpy_line_unregister(_reader_id)
        _reader_id = -1

    # Close the settings panel
    if _panel_hwnd:
        user32.DestroyWindow(_panel_hwnd)

    print("Crit tracker stopped. Total crits: %d" % _total_crits)
