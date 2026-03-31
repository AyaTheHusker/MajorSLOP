# MajorSLOP DLL Reference (msimg32_proxy.c)

## Overview

Proxy DLL that masquerades as `msimg32.dll` — drops into the MegaMUD folder, gets loaded by the process, forwards real msimg32 functions, and runs a TCP control server for the proxy to talk to.

## Build & Deploy

```bash
# Build (32-bit, cross-compile from Linux)
cd dll/
i686-w64-mingw32-gcc -shared -o msimg32.dll msimg32_proxy.c msimg32.def -lws2_32 -lgdi32 -luser32 -lmsimg32 -Wl,--kill-at

# Deploy — MUST be lowercase, NEVER create uppercase copy
cp msimg32.dll ~/.wine/drive_c/MegaMUD/msimg32.dll

# MegaMUD must be restarted to load new DLL (DLL loads at process start)
```

## Forwarded Exports (msimg32.def)

All 5 real msimg32.dll exports MUST be forwarded or MegaMUD's toolbar icons, dialog buttons, and common controls break:

| Export | Ordinal | Used By |
|--------|---------|---------|
| AlphaBlend | @1 | Layered/alpha drawing |
| DllInitialize | @2 | Init callback |
| GradientFill | @3 | Toolbar icons, themed controls |
| TransparentBlt | @4 | Toolbar icons, button faces |
| vSetDdrawflag | @5 | DirectDraw state |

## Architecture

```
DllMain (DLL_PROCESS_ATTACH)
  ├─ Load real msimg32.dll from system dir, resolve all 5 exports
  └─ CreateThread → payload_main()
       ├─ Sleep(2000) — wait for MegaMUD to initialize
       ├─ inject_menu() — add "MajorSLOP" to menu bar, subclass MMMAIN wndproc
       └─ TCP server on 127.0.0.1:9901 (single client)
            └─ handle_client() — line-based protocol, one command per line
```

## TCP Protocol (port 9901)

Single client at a time (proxy holds the connection). Line-based, `\n` terminated.

### Connection
```
→ (connect)
← "MajorSLOP DLL v0.1\n"    # banner on connect
```

### Commands (no struct base required)
| Command | Response | Notes |
|---------|----------|-------|
| `PING` | `PONG` | Health check |
| `FIND <name>` | `OK <hex_base>` or `ERR ...` | Scan heap for game struct by player name |
| `SETBASE <hex>` | `OK base set ...` | Set struct base directly |
| `SETPORTS <proxy> <web>` | `OK` | Tell DLL the proxy/web ports |
| `ENUMWIN` | Multi-line dump + `END` | Dump all MMMAIN child windows |
| `ROUNDTICK <n>` | `OK` | Notify round timer of combat tick |
| `COMBATEND` | `OK` | Notify round timer combat ended |
| `SETTHEME <idx_or_name>` | `OK` | Set theme by index (0-15) or name prefix |
| `STATUS` | Tab-separated parts | Read all 8 statusbar text sections |
| `DLGTEXT <class> <ctrl_id>` | Text content | Read text from any dialog control |
| `CLICK <class> <ctrl_id>` | `OK` | Send BM_CLICK to a button |

### Commands (require FIND/SETBASE first)
| Command | Response | Notes |
|---------|----------|-------|
| `READ32 <hex_offset>` | Decimal u32 | Read 32-bit int at struct+offset |
| `WRITE32 <hex_offset> <decimal>` | `OK` | Write 32-bit int |
| `READS <hex_offset> <maxlen>` | String | Read null-terminated string |
| `WRITES <hex_offset> <string>` | `OK` | Write null-terminated string |
| `READ <hex_offset> <size>` | Hex bytes | Read raw bytes |
| `WRITE <hex_offset> <hex_bytes>` | `OK` | Write raw bytes |
| `READABS <hex_addr> <size>` | Hex bytes | Read at absolute address |

## MegaMUD Window Hierarchy

Discovered via ENUMWIN scan:

### MMMAIN (main window)
```
MMMAIN                          1928x1040   Main application window
├─ msctls_statusbar32  id=107   1920x26     Bottom status bar (8 parts, tab-separated text)
├─ ToolbarWindow32     id=106   1920x26     Icon toolbar (uses imagelist, needs GradientFill/TransparentBlt)
└─ MMANSI              id=103   1920x935    Custom ANSI terminal (NOT RichEdit, custom class)
```

### Top-Level Dialogs (separate #32770 windows, not MMMAIN children)
```
#32770  "Quick Tools"           216x124     Compass rose + toolbar + combo
  ├─ ToolbarWindow32   id=106   92x71       Button toolbar
  ├─ ComboBox          id=1091  210x21      Dropdown
  │   └─ Edit          id=1001              Edit field inside combo
  └─ Static            id=1092  112x77      Icon/image area

#32770  "Player Statistics"     222x463     Exp, accuracy, gold tracking
  ├─ Experience section (id=1753 header)
  │   ├─ Static id=1134  "Duration"
  │   ├─ Static id=1141  "Exp. made"
  │   ├─ Static id=1142  "Exp. needed"
  │   ├─ Static id=1143  "Exp. rate"
  │   ├─ Static id=1209  "Will level in"
  │   └─ Button id=1721  Collapse toggle
  ├─ Accuracy section (id=1754 header)
  │   ├─ Static id=1174  "Hit" (pct)
  │   ├─ Static id=1175  "Hit" (range/avg)
  │   ├─ Static id=1661  "Extra" (pct)
  │   ├─ Static id=1662/1663  "Extra" (range/avg)
  │   ├─ Static id=1106  "Crit" (pct)
  │   ├─ Static id=1107/1103  "Crit" (range/avg)
  │   ├─ Static id=1064  "BS" (pct)
  │   ├─ Static id=1065/1060  "BS" (range/avg)
  │   ├─ Static id=1876  "Pre" (pct)
  │   ├─ Static id=1877/1878  "Pre" (range/avg)
  │   ├─ Static id=1080  "Cast" (pct)
  │   ├─ Static id=1081/1077  "Cast" (range/avg)
  │   ├─ Static id=1285  "Miss" (pct)
  │   └─ Button id=1722  Collapse toggle
  ├─ Other section (id=1755 header)
  │   ├─ Static id=1127  "Dodge"
  │   ├─ Static id=1442  "Sneak"
  │   └─ Button id=1723  Collapse toggle
  └─ Gold section
      ├─ Static id=1883/1884  "Collected" (copper/items)
      ├─ Static id=1118/1885  "Deposit/Sold"
      ├─ Static id=1882/1886  "Stashed"
      ├─ Static id=1635  "Income rate"
      └─ Button id=1550  "Copy"

#32770  "Session Statistics"    176x403     Time, comms, visitors, monsters, events
  ├─ Time section (id=1753 header)
  │   ├─ Static id=1287  "MegaMud" time
  │   ├─ Static id=1459  "Online" time
  │   └─ Button id=1720  Collapse toggle
  ├─ Comms section (id=1754 header)
  │   ├─ Static id=1119  "Dialed"
  │   ├─ Static id=1120  "Failed"
  │   ├─ Static id=1098  "Connected"
  │   ├─ Static id=1075  "Lost carrier"
  │   └─ Button id=1717  Collapse toggle
  ├─ Visitors section (id=1755 header)
  │   ├─ Static id=1617  "People seen"
  │   ├─ Static id=1370  "Attacked"
  │   └─ Button id=1718  Collapse toggle
  ├─ Monsters section (id=1756 header)
  │   ├─ Static id=1661  "Killed"
  │   ├─ Static id=1621  "Had to run"
  │   ├─ Static id=1619  "Health low"
  │   ├─ Static id=1762  "Mana low"
  │   ├─ Button id=1634  "List" button
  │   └─ Button id=1719  Collapse toggle
  └─ Event section (id=1804 header)
      ├─ Static id=1805  Action
      ├─ Static id=1806  Due time
      └─ Button id=1720  Collapse toggle

#32770  "Time Analysis"         194x321     Resting/moving/attacking time breakdown
  ├─ Totals section (id=1753 header)
  │   ├─ Static id=1134  "Duration"
  │   ├─ Static id=1629  "Moving"
  │   ├─ Static id=1628  "Attacking"
  │   ├─ Static id=1623  "Resting"
  │   ├─ Static id=1630  "Other"
  │   └─ Button id=1720  Collapse toggle
  └─ Time Spent Resting section (id=1754 header)
      ├─ Static id=1414  "Resting HPs"
      ├─ Static id=1415  "Resting Mana/Kai"
      ├─ Static id=1625  "Waiting for party"
      ├─ Static id=1627  "Unable to move"
      ├─ Static id=1057  "Blinded"
      ├─ Static id=1348  "Poisoned"
      ├─ Static id=1123  "Diseased"
      ├─ Static id=1097  "Confused"
      └─ Static id=1626  "Other conditions"

#32770  "Experience Graph"      707x504     XP graph with graph/clear buttons
  └─ Static id=1948  699x459  Graph canvas area

MMTALK  "Conversations"         512x367     Chat/conversation window
  ├─ RichEdit20A  id=1113  504x291         Chat display (rich text)
  └─ ComboBox     id=1091  504x33          Input with dropdown
      └─ Edit     id=1001                  Text input field

MMPRTY  "Tripmunk (100%)"      1x1         Party status (hidden when no party visible)
  └─ ListBox  id=1306  1x1                 Party member list
```

### Custom Window Classes (MegaMUD-specific)
| Class | Purpose | Notes |
|-------|---------|-------|
| `MMMAIN` | Main application frame | Standard Win32 frame, hosts menu bar |
| `MMANSI` | ANSI terminal display | **Custom class** — NOT RichEdit/Edit. Standard EM_* messages won't work. Needs reverse engineering (Ghidra) to understand paint/color model |
| `MMTALK` | Conversation window | Contains standard RichEdit20A + ComboBox |
| `MMPRTY` | Party status | Contains standard ListBox |

## DLL Features

### Menu Injection
Appends "MajorSLOP" popup menu to MMMAIN's menu bar with:
- **Proxy Info...** — Plasma fractal about window showing connection status
- **Themes...** — Theme selection dropdown (16 themes matching web client)
- **Show/Hide Round Timer** — Toggle floating round timer widget

### Round Timer (RT_CLASS = "MajorSLOPRoundTimer")
- Floating WS_EX_TOOLWINDOW, 190x220
- GDI double-buffered rendering: ring, tick marks, progress arc, orbiting ball
- Receives ROUNDTICK/COMBATEND from proxy via TCP
- Calibrates round period with weighted moving average
- Shows round number, elapsed/period, delta, freewheel indicator
- Uses "Arial" font (Wine doesn't have "Segoe UI")

### Theme System (slop_theme_t)
16 themes with RGB values for: accent, text, dim, hp_color, mana_color
Mirrors the web client's themes.js. Currently only affects round timer widget.

## Known Limitations / Wine Issues
- **WS_EX_LAYERED** overlay windows don't work under Wine (bleeds across desktop, stays on top when minimized)
- **"Segoe UI"** font doesn't exist in Wine — always use "Arial"
- **Case-sensitive filenames** — Wine on Linux is case-sensitive, DLL must be lowercase `msimg32.dll`
- **Single TCP client** — Only one connection to port 9901 at a time (proxy holds it)

## Theming Research (TODO)
- `msctls_statusbar32` — Can theme with SB_SETBKCOLOR, owner-draw parts
- Menu bar — Owner-draw menus (MF_OWNERDRAW) for custom bg/text colors
- `#32770` dialogs — WM_CTLCOLORDLG, WM_CTLCOLORSTATIC for bg/text colors
- `MMANSI` — Custom class, needs Ghidra RE to understand rendering. Eventually: gradient text like web client
- `ToolbarWindow32` — TB_SETBKCOLOR, custom draw (NM_CUSTOMDRAW)
