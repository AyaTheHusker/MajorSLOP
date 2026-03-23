import re

# ANSI CSI sequences: ESC[ followed by params and a letter
ANSI_CSI_RE = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
# OSC sequences: ESC] ... BEL or ESC\
ANSI_OSC_RE = re.compile(r'\x1b\].*?(?:\x07|\x1b\\)')
# Single-char escape sequences
ANSI_ESC_RE = re.compile(r'\x1b[^[\]]')
# Backspace-based overstriking
ANSI_BS_RE = re.compile(r'.\x08')
# Remaining control chars (keep \r \n \t)
ANSI_CTRL_RE = re.compile(r'[\x00-\x08\x0b\x0c\x0e-\x1f]')
# Orphaned CSI sequences where ESC was stripped but [params;letter remains
# Match orphaned CSI codes where ESC was in a prior chunk
# Covers SGR (m), cursor movement (A-H), erase (J,K), and other CSI finals
ANSI_ORPHAN_CSI_RE = re.compile(r'\[\d+(?:;\d+)*[A-Za-z]')


def strip_ansi(text: str) -> str:
    text = ANSI_CSI_RE.sub('', text)
    text = ANSI_OSC_RE.sub('', text)
    text = ANSI_ESC_RE.sub('', text)
    text = ANSI_BS_RE.sub('', text)
    text = ANSI_CTRL_RE.sub('', text)
    # Strip orphaned CSI codes (ESC was in a prior chunk)
    text = ANSI_ORPHAN_CSI_RE.sub('', text)
    return text
