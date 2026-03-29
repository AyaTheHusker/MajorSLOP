"""Reset Player Statistics window sections via Windows API button clicks.
Run with: wine python scripts/reset_stats.py [exp|accuracy|other|all]
"""
import sys
import ctypes
import ctypes.wintypes as wt

u32 = ctypes.windll.user32
BM_CLICK = 0x00F5
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

BUTTONS = {
    "exp": 1721,       # Experience section (duration, exp made, exp rate)
    "accuracy": 1722,  # Accuracy section (hit%, crit%, BS, rounds)
    "other": 1723,     # Other section (sneak%, dodge%, collected)
}

def find_stats_window():
    result = [None]
    def cb(hwnd, lp):
        buf = ctypes.create_string_buffer(256)
        u32.GetWindowTextA(hwnd, buf, 256)
        if b'Player Statistics' in buf.value:
            result[0] = hwnd
        return True
    u32.EnumWindows(WNDENUMPROC(cb), 0)
    return result[0]

def click_reset(section="exp"):
    hwnd = find_stats_window()
    if not hwnd:
        print("Player Statistics window not found")
        return False
    ctrl_id = BUTTONS.get(section)
    if not ctrl_id:
        print(f"Unknown section: {section}. Use: {', '.join(BUTTONS.keys())}")
        return False
    btn = u32.GetDlgItem(hwnd, ctrl_id)
    if not btn:
        print(f"Button {ctrl_id} not found")
        return False
    u32.SendMessageA(btn, BM_CLICK, 0, 0)
    print(f"Reset {section} (button {ctrl_id})")
    return True

if __name__ == "__main__":
    sections = sys.argv[1:] or ["exp"]
    if "all" in sections:
        sections = list(BUTTONS.keys())
    for s in sections:
        click_reset(s)
