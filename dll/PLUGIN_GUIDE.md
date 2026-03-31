# MajorSLOP Plugin Development Guide

**SDK Version:** 1 | **Target:** MegaMUD 2.0 Alpha (32-bit) | **Language:** C/C++

---

## Table of Contents

1. [Overview](#overview)
2. [Quick Start](#quick-start)
3. [Plugin Structure](#plugin-structure)
4. [Building](#building)
5. [Installation](#installation)
6. [API Reference](#api-reference)
7. [Game Struct Memory Map](#game-struct-memory-map)
8. [Terminal Access](#terminal-access)
9. [Event Callbacks](#event-callbacks)
10. [Menu Integration](#menu-integration)
11. [Window & UI Creation](#window--ui-creation)
12. [Best Practices](#best-practices)
13. [Example Plugins](#example-plugins)

---

## Overview

MajorSLOP plugins are **32-bit Windows DLLs** that run inside the MegaMUD process.
They have full access to the Win32 API, MegaMUD's memory, the MMANSI terminal buffer,
and can create their own windows, threads, network connections, and rendering contexts.

**What you can build:**
- Custom overlays (GDI, OpenGL, Vulkan, Direct2D)
- Trigger systems (parse terminal output, react to game events)
- Combat analyzers, DPS meters, loot trackers
- Map renderers, mini-maps
- Discord/webhook integrations
- Custom automation logic
- Hotkey/macro systems
- Sound/alert systems

**How it works:**

```
MegaMUD.exe loads msimg32.dll (MajorSLOP proxy DLL)
    -> MajorSLOP scans plugins/*.dll on startup
    -> Validates magic header (SLOP_PLUGIN_MAGIC)
    -> Calls your plugin's init() with the API struct
    -> Feeds terminal lines and round ticks to your callbacks
    -> Calls shutdown() on exit
```

**Security:** Every plugin DLL must contain the magic value `0x50304C53` ("SL0P") in its
plugin descriptor. MajorSLOP checks this before loading — random DLLs dropped into the
folder will be ignored.

---

## Quick Start

### 1. Create `hello_plugin.c`

```c
#include "slop_plugin.h"

static const slop_api_t *api = NULL;

static int my_init(const slop_api_t *a) {
    api = a;
    api->log("[hello] Plugin loaded!\n");
    return 0;
}

static void my_shutdown(void) {
    api->log("[hello] Plugin unloaded.\n");
}

static void my_on_line(const char *line) {
    /* Fired for every new terminal line */
    if (strstr(line, "drops dead")) {
        api->log("[hello] Something died: %s\n", line);
    }
}

static slop_plugin_t plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Hello Plugin",
    .author      = "YourName",
    .description = "Logs monster deaths",
    .version     = "1.0.0",
    .init        = my_init,
    .shutdown    = my_shutdown,
    .on_line     = my_on_line,
    .on_round    = NULL,
    .on_wndproc  = NULL,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) {
    return &plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) {
    (void)h; (void)r; (void)p;
    return TRUE;
}
```

### 2. Build

```bash
i686-w64-mingw32-gcc -shared -o hello_plugin.dll hello_plugin.c \
    -lgdi32 -luser32 -mwindows
```

### 3. Install

Copy `hello_plugin.dll` into your MegaMUD `plugins/` folder and restart MegaMUD.
Check **MajorSLOP > Plugins...** to verify it loaded.

---

## Plugin Structure

Every plugin must:

1. **Include `slop_plugin.h`** — the SDK header
2. **Define a `slop_plugin_t` descriptor** — with the required magic and version
3. **Export `slop_get_plugin()`** — returns a pointer to the descriptor
4. **Have a `DllMain`** — standard DLL entry point (can be empty)

### The Plugin Descriptor (`slop_plugin_t`)

| Field         | Type             | Required | Description                                    |
|---------------|------------------|----------|------------------------------------------------|
| `magic`       | `unsigned int`   | YES      | Must be `SLOP_PLUGIN_MAGIC` (`0x50304C53`)     |
| `api_version` | `unsigned int`   | YES      | Must be `SLOP_API_VERSION` (currently `1`)      |
| `name`        | `const char *`   | YES      | Short display name (shown in Plugins dialog)   |
| `author`      | `const char *`   | YES      | Author name                                    |
| `description` | `const char *`   | YES      | One-line description                           |
| `version`     | `const char *`   | YES      | Version string (e.g. `"1.0.0"`)               |
| `init`        | function ptr     | YES      | Called on load. Return 0 = success, else abort |
| `shutdown`    | function ptr     | YES      | Called on unload / MegaMUD exit                |
| `on_line`     | function ptr     | optional | Called for every new terminal line (NULL = skip)|
| `on_round`    | function ptr     | optional | Called on each combat round tick (NULL = skip) |
| `on_wndproc`  | function ptr     | optional | WndProc hook, return non-zero if handled       |
| `_reserved`   | `void *[4]`      | -        | Zero-initialize. Future expansion.             |

### Lifecycle

```
LoadLibrary("plugins/your_plugin.dll")
    -> GetProcAddress("slop_get_plugin")
    -> desc = slop_get_plugin()
    -> Validate: desc->magic == 0x50304C53
    -> Validate: desc->api_version == 1
    -> desc->init(&api)          <-- you receive the API here
    -> Plugin is now active
    ...
    [MegaMUD exits]
    -> desc->shutdown()
    -> FreeLibrary(dll)
```

---

## Building

### Requirements

- **MinGW-w64 (i686 / 32-bit target)** — plugins MUST be 32-bit
- `slop_plugin.h` — copy from the MajorSLOP `dll/` folder

### Compile Command

```bash
i686-w64-mingw32-gcc -shared -o my_plugin.dll my_plugin.c \
    -lgdi32 -luser32 -lws2_32 -lcomctl32 -mwindows
```

Only link libraries you actually use. Most plugins need at minimum `-lgdi32 -luser32`.

### C++ Support

```cpp
// C++ plugins work fine — just use extern "C" for the export:
extern "C" SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) {
    return &my_plugin;
}
```

### Tips

- **Plugins MUST be 32-bit.** MegaMUD is a 32-bit process and Windows cannot load
  64-bit DLLs into a 32-bit process (LoadLibrary will fail). Always use
  `i686-w64-mingw32-gcc`, never `x86_64-w64-mingw32-gcc`.
- Use `-mwindows` to suppress the console window.
- Use `-O2` for release builds.
- The plugin runs in Wine on Linux — most Win32 API works fine.

---

## Installation

1. MajorSLOP auto-creates a `plugins/` folder in the MegaMUD directory on first launch
2. Drop your `.dll` into `plugins/`
3. Restart MegaMUD
4. Check **MajorSLOP > Plugins...** in the menu bar to verify

Plugin load order is filesystem order (alphabetical). Plugins are loaded once during
MegaMUD startup — hot-reloading is not supported (restart MegaMUD to reload).

---

## API Reference

The `slop_api_t` struct is passed to your `init()` function. Store it globally —
you'll use it throughout your plugin's lifetime.

```c
static const slop_api_t *api = NULL;

static int my_init(const slop_api_t *a) {
    api = a;
    return 0;
}
```

### Logging

```c
void (*log)(const char *fmt, ...);
```

Printf-style logging to MajorSLOP's log file (`majorslop.log` in the MegaMUD folder).
Use this for debugging — visible in the log file immediately.

```c
api->log("[my_plugin] Player HP: %d/%d\n", cur_hp, max_hp);
```

---

### Terminal Access

```c
HWND (*get_mmansi_hwnd)(void);
int  (*read_terminal_row)(int row, char *out, int out_sz);
int    terminal_rows;   // 60
int    terminal_cols;   // 132
```

**`get_mmansi_hwnd()`** — Returns the HWND of the MMANSI terminal child window.
Returns NULL if MegaMUD hasn't created it yet.

**`read_terminal_row(row, out, out_sz)`** — Reads one row of plain text from the
terminal buffer. Row 0 is the top, row 59 is the bottom. Returns the string length.
Trailing spaces are stripped. Text is plain ASCII — ANSI color codes are stored in a
separate attribute buffer (not exposed via API).

```c
char line[140];
for (int r = 0; r < api->terminal_rows; r++) {
    if (api->read_terminal_row(r, line, sizeof(line)) > 0) {
        api->log("Row %d: %s\n", r, line);
    }
}
```

**Terminal buffer layout** (for advanced direct access via `get_mmansi_hwnd()`):

| Offset | Description |
|--------|-------------|
| `GetWindowLongA(hwnd, 4)` | Pointer to MMANSI window data struct (0x4058 bytes) |
| `+0x62` | Text buffer start (plain ASCII, 132 bytes per row) |
| `+0x1F52` | Attribute buffer start (color/style, 132 bytes per row) |
| Stride | `0x84` (132) bytes per row |
| Rows | 60 max |

---

### Window Handles

```c
HWND (*get_mmmain_hwnd)(void);
HWND (*get_mmansi_hwnd)(void);
```

**`get_mmmain_hwnd()`** — The main MegaMUD window (class `"MMMAIN"`).
Use this as a parent for your own windows, or to access MegaMUD's menu bar and status bar.

**`get_mmansi_hwnd()`** — The ANSI terminal child window (class `"MMANSI"`).
This is where MUD output is displayed and where typed commands go.

---

### Game Struct Memory Access

```c
unsigned int (*get_struct_base)(void);
int          (*read_struct_i32)(unsigned int offset);
int          (*read_struct_i16)(unsigned int offset);
```

MegaMUD stores all game state in a single large struct in memory. The struct base is
found by scanning for the player name in the process heap. Once found, every field is
at a fixed offset.

**`get_struct_base()`** — Returns the base address of the game struct, or 0 if not
yet found. The struct finder runs in a background thread and usually locates it within
a few seconds of connecting to the game.

**`read_struct_i32(offset)`** — Reads a 32-bit integer at `struct_base + offset`.
Returns 0 if struct_base is not set.

**`read_struct_i16(offset)`** — Reads a 16-bit integer at `struct_base + offset`.

**Writing to memory:** Not exposed via API for safety, but since your plugin runs
in the same process, you can cast `get_struct_base()` and read/write any offset directly:

```c
unsigned int base = api->get_struct_base();
if (base) {
    *(int *)(base + 0x4D00) = 1;   // enable auto-combat
    *(int *)(base + 0x564C) = 1;   // set Go flag
    strcpy((char *)(base + 0x5834), "mypath.mp");  // set path filename
}
```

**Every field in the struct is both readable and writable.** It's all just memory —
MegaMUD doesn't protect any of it. Write carefully though: invalid values can crash
MegaMUD or cause unexpected behavior. The game reads these values on its own timers,
so writes take effect on the next read cycle.

See the [Game Struct Memory Map](#game-struct-memory-map) section below for all offsets.

---

### Command Injection

```c
void (*inject_text)(const char *text);
void (*inject_command)(const char *cmd);
```

**`inject_text(text)`** — Types each character into the MMANSI terminal via
`PostMessage(WM_CHAR)`. Does NOT press Enter.

**`inject_command(cmd)`** — Same as `inject_text()` but also presses Enter afterward.
This is how you send commands to the MUD.

```c
api->inject_command("cast heal");   // types "cast heal" + Enter
api->inject_text("Hello ");         // types "Hello " without Enter
```

**Timing:** Commands are injected immediately. If you need to pace commands (e.g. one
per round), use `on_round` callbacks or your own timer thread with `Sleep()`.

---

### Menu Integration

```c
int (*add_menu_item)(const char *label, unsigned int id);
int (*add_menu_separator)(void);
```

Add items to the MajorSLOP dropdown menu. Use IDs in the range **41000-41099**
(`IDM_PLUGIN_BASE` to `IDM_PLUGIN_BASE + 99`).

```c
#define MY_MENU_TOGGLE  41010
#define MY_MENU_CONFIG  41011

static int my_init(const slop_api_t *a) {
    api = a;
    api->add_menu_separator();
    api->add_menu_item("My Plugin [ON]", MY_MENU_TOGGLE);
    api->add_menu_item("My Plugin Config...", MY_MENU_CONFIG);
    return 0;
}
```

Handle clicks in your `on_wndproc` callback:

```c
static int my_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND) {
        WORD id = LOWORD(wParam);
        if (id == MY_MENU_TOGGLE) {
            /* toggle something */
            return 1;  // handled
        }
        if (id == MY_MENU_CONFIG) {
            /* show config dialog */
            return 1;
        }
    }
    return 0;  // not handled
}
```

---

### Event Registration

```c
void (*on_round_tick)(void (*callback)(int round_num));
void (*on_terminal_line)(void (*callback)(const char *line));
```

Register additional callbacks beyond the ones in your plugin descriptor. Useful if you
want to register callbacks dynamically or from helper modules.

```c
static void my_extra_line_handler(const char *line) {
    /* another line handler */
}

api->on_terminal_line(my_extra_line_handler);
```

---

## Game Struct Memory Map

All offsets are relative to `get_struct_base()`. Read with `read_struct_i32(offset)` or
`read_struct_i16(offset)`. **Every field is both readable and writable** — the entire
struct is unprotected process memory. Write with direct pointer casts.

String fields can be read by casting:

```c
unsigned int base = api->get_struct_base();
const char *name = (const char *)(base + 0x537C);
```

### Player Identity

| Offset   | Type      | Field              | Description                        |
|----------|-----------|--------------------|------------------------------------|
| `0x537C` | char[]    | Player Name        | Null-terminated                    |
| `0x5387` | char[]    | Player Surname     | Null-terminated                    |
| `0x539A` | char[]    | Gang Name          | Null-terminated                    |
| `0x53C0` | i32       | Character Points   |                                    |
| `0x53C4` | i32       | Race               | Race ID (integer, not string)      |
| `0x53C8` | i32       | Class              | Class ID (integer, not string)     |
| `0x53D0` | i32       | Level              |                                    |

### Player HP / Mana

| Offset   | Type      | Field              | Description                        |
|----------|-----------|--------------------|------------------------------------|
| `0x53D4` | i32       | Current HP         |                                    |
| `0x53DC` | i32       | Max HP             |                                    |
| `0x53E0` | i32       | Current Mana       |                                    |
| `0x53E8` | i32       | Max Mana           |                                    |
| `0x53EC` | i32       | Encumbrance Lo     | Current weight                     |
| `0x53F0` | i32       | Encumbrance Hi     | Max weight                         |

### Player Experience (64-bit)

| Offset   | Type      | Field              | Description                        |
|----------|-----------|--------------------|------------------------------------|
| `0x53B0` | i32       | EXP (low dword)    | Current experience                 |
| `0x53B4` | i32       | EXP (high dword)   |                                    |
| `0x53B8` | i32       | Need (low dword)   | EXP needed to next level           |
| `0x53BC` | i32       | Need (high dword)  |                                    |

Read 64-bit exp:
```c
unsigned int base = api->get_struct_base();
long long exp = *(int *)(base + 0x53B0) | ((long long)*(int *)(base + 0x53B4) << 32);
```

### Player Stats

| Offset   | Type | Field           |
|----------|------|-----------------|
| `0x53F4` | i32  | Strength        |
| `0x53F8` | i32  | Agility         |
| `0x53FC` | i32  | Intellect       |
| `0x5400` | i32  | Wisdom          |
| `0x5404` | i32  | Health (Con)    |
| `0x5408` | i32  | Charm           |
| `0x540C` | i32  | Perception      |
| `0x5410` | i32  | Stealth         |
| `0x5414` | i32  | Thievery        |
| `0x5418` | i32  | Traps           |
| `0x541C` | i32  | Picklocks       |
| `0x5420` | i32  | Tracking        |
| `0x5424` | i32  | Martial Arts    |
| `0x5428` | i32  | Magic Resist    |
| `0x542C` | i32  | Spell Casting   |

### Connection & State Flags

| Offset   | Type | Field             | Values                                       |
|----------|------|-------------------|----------------------------------------------|
| `0x563C` | i32  | Connected         | Non-zero = connected to BBS                  |
| `0x5644` | i32  | In Game           | Non-zero = in the game world                 |
| `0x5698` | i32  | In Combat         | Non-zero = currently fighting                |
| `0x5678` | i32  | Is Resting        | Non-zero = resting                           |
| `0x567C` | i32  | Is Meditating     | Non-zero = meditating                        |
| `0x5688` | i32  | Is Sneaking       | Non-zero = sneaking                          |
| `0x5690` | i32  | Is Hiding         | Non-zero = hiding                            |
| `0x5664` | i32  | Pathing Active    | Non-zero = currently following a path        |
| `0x5668` | i32  | Looping           | Non-zero = loop mode active                  |
| `0x566C` | i32  | Auto Roaming      | Non-zero = auto-roam mode                    |
| `0x5670` | i32  | Is Lost           | Non-zero = player is lost                    |
| `0x5674` | i32  | Is Fleeing        | Non-zero = currently fleeing                 |
| `0x564C` | i32  | Go Flag           | 1=go, 0=stop                   |
| `0x54BC` | i32  | Mode              | 11=idle, 14=walking, 15=looping, 17=lost     |

### Status Effects

| Offset   | Type | Field              | Description                |
|----------|------|--------------------|----------------------------|
| `0x56AC` | i32  | Is Blinded         |                            |
| `0x56B0` | i32  | Is Confused        |                            |
| `0x56B4` | i32  | Is Poisoned        | Flag                       |
| `0x56B8` | i32  | Poison Amount      | Damage per tick            |
| `0x56BC` | i32  | Is Diseased        |                            |
| `0x56C0` | i32  | Losing HP          | HP drain active            |
| `0x56C4` | i32  | Regenerating       |                            |
| `0x56C8` | i32  | Fluxing            |                            |
| `0x56CC` | i32  | Is Held            | Paralyzed/held             |
| `0x56D0` | i32  | Is Stunned         |                            |
| `0x56E0` | i32  | Resting to Full HP |                            |
| `0x56E4` | i32  | Resting to Full Ma |                            |

### Room Data

| Offset   | Type      | Field              | Description                            |
|----------|-----------|--------------------|----------------------------------------|
| `0x2D9C` | char[]    | Room Name          | Current room name string               |
| `0x2E70` | u32       | Room Checksum      | Hash identifying the room              |
| `0x2E78` | i32[10]   | Room Exits         | Per-direction: 0=none, 2=closed, 3=open, 4=special, 5=unlocked |
| `0x5528` | i32       | Room Darkness      | 0=normal, 2=dark, 3=pitch black       |
| `0x2C18` | i32       | Room DB Count      | Total rooms in database                |
| `0x2C20` | ptr       | Room DB Pointer    | Array of room entries (+0x44=checksum, +0x06=name) |

### Entities in Room

| Offset   | Type      | Field              | Description                            |
|----------|-----------|--------------------|----------------------------------------|
| `0x1ED4` | i32       | Entity Count       | Number of entities in room             |
| `0x1EE0` | ptr       | Entity List Ptr    | Array of entities                      |

Entity struct layout (at each array entry):
- `+0x00` i32: type (1=player, 2=monster, 3=item)
- `+0x0C` i32: quantity
- `+0x10` char[]: name
- `+0x34` / `+0x38`: known flags

### Combat Target

| Offset   | Type      | Field              | Description                            |
|----------|-----------|--------------------|----------------------------------------|
| `0x552C` | char[]    | Combat Target      | Name with size prefix ("small", "fierce") |

### Path / Navigation

| Offset   | Type      | Field              | Description                            |
|----------|-----------|--------------------|----------------------------------------|
| `0x5898` | i32       | Current Path Step  | 1-based step index        |
| `0x5894` | i32       | Total Path Steps   | total steps in path       |
| `0x5834` | char[76]  | Path Filename      | current .mp filename      |
| `0x5880` | u32       | Waypoint 0         | from_checksum             |
| `0x5884` | u32       | Waypoint 1         | to_checksum               |
| `0x2EA4` | u32       | Dest Checksum      | destination room hash     |
| `0x358C` | i32       | Backtrack Rooms    |                                        |
| `0x5508` | i32       | Run Away Rooms     |                                        |

### Automation Toggles (1=on, 0=off)

| Offset   | Field              | Description                |
|----------|--------------------|----------------------------|
| `0x4D00` | Auto Combat        | Walk sets 1, Run sets 0    |
| `0x4D04` | Auto Nuke          | Offensive spells           |
| `0x4D08` | Auto Heal          | Self-healing               |
| `0x4D0C` | Auto Bless         | Buff spells                |
| `0x4D10` | Auto Light         | Light in dark rooms        |
| `0x4D14` | Auto Cash          | Pick up cash               |
| `0x4D18` | Auto Get           | Pick up items              |
| `0x4D1C` | Auto Search        | Search rooms               |
| `0x4D20` | Auto Sneak         | Sneak movement             |
| `0x4D24` | Auto Hide          | Hide after combat          |
| `0x4D28` | Auto Track         | Track monsters             |

### Health Thresholds

| Offset   | Field              | Description                         |
|----------|--------------------|-------------------------------------|
| `0x3788` | HP Full %          | HP considered "full"                |
| `0x378C` | HP Rest %          | Rest when HP below this             |
| `0x3790` | HP Heal %          | Cast heal when below this           |
| `0x3794` | HP Heal Att %      | Heal in combat when below this      |
| `0x3798` | HP Run %           | Run away when below this (0=off)    |
| `0x379C` | HP Logoff %        | Emergency logoff threshold          |
| `0x37A4` | Mana Full %        | Mana considered "full"              |
| `0x37A8` | Mana Rest %        | Rest when mana below this           |
| `0x37AC` | Mana Heal %        | Min mana to cast heal               |
| `0x37B0` | Mana Heal Att %    | Min mana to heal in combat          |
| `0x37B4` | Mana Run %         | Run when mana below this            |
| `0x37B8` | Mana Bless %       | Min mana to cast bless              |

### Spell Commands (all char[21] strings)

| Offset   | Field              | Description                    |
|----------|--------------------|--------------------------------|
| `0x4E40` | Heal Cmd           | Heal spell command             |
| `0x4E55` | Regen Cmd          | Regen spell                    |
| `0x4E6A` | Flux Cmd           | Flux spell                     |
| `0x4E8C` | Blind Cmd          | Cure blindness                 |
| `0x4EA1` | Poison Cmd         | Cure poison                    |
| `0x4EB6` | Disease Cmd        | Cure disease                   |
| `0x4ECB` | Freedom Cmd        | Cure held/paralysis            |
| `0x4EE0` | Light Cmd          | Light spell                    |
| `0x4F1F` | Bless Cmd 1        | Bless spell slot 1             |
| `0x4F34` | Bless Cmd 2        | Bless spell slot 2             |
| `0x4F49` | Bless Cmd 3-10     | ... (stride 0x15 = 21 bytes)   |

### Combat Settings

| Offset   | Type    | Field               | Description                        |
|----------|---------|---------------------|------------------------------------|
| `0x37D0` | i32     | Can Backstab        |                                    |
| `0x37D4` | i32     | Don't BS if Multi   | Skip backstab if multiple enemies  |
| `0x37D8` | i32     | Run if BS Fails     |                                    |
| `0x37DC` | i32     | Attack Neutral      |                                    |
| `0x37E0` | i32     | Polite Attacks      |                                    |
| `0x4D88` | i32     | Max Monsters        | Max monsters to engage             |
| `0x4D90` | i32     | Max Monster Exp     | Max exp of monsters to fight       |
| `0x51D9` | char[]  | Attack Cmd          | Attack command (e.g. "a")          |
| `0x51E4` | char[]  | Multi Attack Cmd    | Multi-attack spell                 |
| `0x51EF` | char[]  | Pre Attack Cmd      | Pre-attack spell                   |
| `0x51FA` | char[]  | Attack Spell Cmd    | Primary attack spell               |
| `0x52C6` | char[]  | Normal Weapon       | Normal weapon name                 |
| `0x52E5` | char[]  | BS Weapon           | Backstab weapon name               |
| `0x5304` | char[]  | Alt Weapon          | Alternate weapon                   |
| `0x5323` | char[]  | Shield              | Shield item name                   |

### Party Data

| Offset   | Type      | Field                | Description                      |
|----------|-----------|----------------------|----------------------------------|
| `0x35F0` | i32       | Party Size           | Number of party members          |
| `0x3748` | char[]    | Party Leader Name    |                                  |
| `0x35F8` | struct[N] | Party Members        | 0x38 bytes each (see below)      |
| `0x5788` | i32       | Following            | Following flag                   |
| `0x578C` | i32       | Lost from Leader     |                                  |
| `0x5790` | i32       | Waiting on Member    |                                  |
| `0x5794` | i32       | Holding Up Leader    |                                  |

Party member struct (each 0x38 = 56 bytes):
- `+0x00` char[]: member name
- `+0x0C` i32: HP percentage
- `+0x10` i32: current HP
- `+0x14` i32: max HP
- `+0x28` i32: flags (bit 7=hungup, bit 3=missing, bit 2=invited)

### Talk / Chat Settings

| Offset   | Type    | Field              | Description                      |
|----------|---------|--------------------|----------------------------------|
| `0x3820` | i32     | Auto AFK           |                                  |
| `0x382C` | i32     | AFK Timeout        | Minutes                          |
| `0x3830` | char[]  | AFK Reply          | Auto-reply message               |
| `0x3A54` | i32     | Log Talk           | Enable talk logging              |
| `0x3A6C` | i32     | Divert Local       | Route local chat to talk window  |
| `0x3A70` | i32     | Divert Telepathy   |                                  |
| `0x3A74` | i32     | Divert Gossip      |                                  |
| `0x3A78` | i32     | Divert Gang        |                                  |
| `0x3A7C` | i32     | Divert Broadcast   |                                  |

### Display Settings

| Offset   | Type | Field              | Description                      |
|----------|------|--------------------|----------------------------------|
| `0x3800` | i32  | Show Info          |                                  |
| `0x3804` | i32  | Show Rounds        |                                  |
| `0x3808` | i32  | Show Send          |                                  |
| `0x380C` | i32  | Show Run Msg       |                                  |
| `0x1A44` | i32  | Scale ANSI         |                                  |
| `0x1A48` | i32  | ANSI Rows          | Terminal row count               |
| `0x1A4C` | i32  | ANSI Cols          | Terminal column count            |

### Timer / Tick Values

| Offset   | Type | Field              | Description                      |
|----------|------|--------------------|----------------------------------|
| `0x9468` | i32  | Tick Count A       |                                  |
| `0x9508` | i32  | Tick Count B       |                                  |
| `0x8A98` | i32  | Lag Wait (seconds) |                                  |

### Currency Pickup Settings

| Offset   | Type | Field              | Description                      |
|----------|------|--------------------|----------------------------------|
| `0x3208` | i32  | Want Copper        |                                  |
| `0x320C` | i32  | Want Silver        |                                  |
| `0x3210` | i32  | Want Gold          |                                  |
| `0x3214` | i32  | Want Platinum      |                                  |
| `0x3218` | i32  | Want Runic         |                                  |
| `0x321C` | i32  | Stash Coins        |                                  |
| `0x3240` | char[]| Bank Name          | For auto-deposits                |

---

## Terminal Access

The MMANSI terminal is MegaMUD's main output display. It shows all MUD text — combat,
movement, chat, system messages, everything.

### Reading Lines (Easy Way)

Use the `on_line` callback or `read_terminal_row()`:

```c
static void my_on_line(const char *line) {
    // Fired for every NEW line that appears in the terminal.
    // Lines are plain ASCII — no ANSI escape codes.
    // Trailing whitespace is already stripped.

    if (strstr(line, " drops dead")) {
        kill_count++;
    }
    if (strstr(line, " damage!")) {
        // parse damage
    }
}
```

### Scanning the Full Buffer

Read all 60 rows:

```c
void dump_terminal(void) {
    char buf[140];
    for (int i = 0; i < api->terminal_rows; i++) {
        int len = api->read_terminal_row(i, buf, sizeof(buf));
        if (len > 0) {
            api->log("[row %02d] %s\n", i, buf);
        }
    }
}
```

### Important Notes

- Terminal text is **plain ASCII** — color information is in a separate attribute buffer
- The terminal has **60 rows** and **132 columns**
- New text appears at the bottom and scrolls up
- The `on_line` callback fires for genuinely new lines (hash-based deduplication)
- Polling `read_terminal_row()` yourself is fine but `on_line` is more efficient

---

## Event Callbacks

### `on_line(const char *line)`

Called for every new line that appears in the MMANSI terminal. This is your primary
way to react to game output.

**What you get:** Plain ASCII text, trailing spaces stripped.

**Common patterns to match:**

```c
// Damage dealt/received
if (strstr(line, " damage!"))              // "X hits Y for N damage!"
// Misses
if (stristr(line, "misses "))              // "X misses Y"
if (stristr(line, "dodges "))              // "X dodges Y"
if (stristr(line, "fumbles "))             // "X fumbles"
if (stristr(line, "blocks "))              // "X blocks Y"
if (stristr(line, "bounces off"))          // "X bounces off Y"
if (stristr(line, "parries "))             // "X parries Y"
// Player attacks
if (starts_with("You ") && strstr(line, " at "))   // "You swing at X"
if (starts_with("Your ") && strstr(line, " glances off"))  // glancing blow
// Death
if (strstr(line, " drops dead"))           // monster died
// Room changes
if (strstr(line, "Also here:"))            // entities in room
// Communication
if (strstr(line, " gossips: "))            // gossip channel
if (strstr(line, " telepaths: "))          // telepathy
// Location (from `rm` command)
if (strncmp(line, "Location:", 9) == 0)    // "Location: X,Y"
```

### `on_round(int round_num)`

Called when a new combat round is detected. Rounds on ParaMUD are always **5 seconds**
apart. A round is detected when a combat event (hit/miss/damage/etc.) occurs after a
500ms gap from the previous cluster of events.

```c
static void my_on_round(int round_num) {
    api->log("[dps] Round %d started\n", round_num);
    // Good time to inject timed commands, update displays, etc.
}
```

### `on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)`

Receives Windows messages sent to the MegaMUD main window. Return non-zero if your
plugin handled the message (prevents further processing), or 0 to pass it through.

Most useful for handling your own menu item clicks (`WM_COMMAND`).

---

## Menu Integration

Plugins can add items to the MajorSLOP dropdown menu. Use IDs `41000`-`41099`.

```c
#define MY_TOGGLE_ID  41010

static int my_enabled = 1;

static int my_init(const slop_api_t *a) {
    api = a;
    api->add_menu_separator();
    api->add_menu_item("My Plugin [ON]", MY_TOGGLE_ID);
    return 0;
}

static int my_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND && LOWORD(wParam) == MY_TOGGLE_ID) {
        my_enabled = !my_enabled;
        /* To update the menu text, use ModifyMenuA on the MajorSLOP menu.
           You can get the menu via GetMenu(api->get_mmmain_hwnd()). */
        return 1;
    }
    return 0;
}
```

---

## Window & UI Creation

Plugins can create their own windows, dialogs, and overlays. You're running inside
a Win32 process with full API access.

### Creating a Window

```c
static HWND my_window = NULL;

static LRESULT CALLBACK my_wndproc_fn(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(hdc, RGB(0, 255, 0));
        SetBkMode(hdc, TRANSPARENT);
        TextOutA(hdc, 10, 10, "Hello from my plugin!", 21);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void create_my_window(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = my_wndproc_fn;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "MyPluginWindow";
    RegisterClassA(&wc);

    my_window = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "MyPluginWindow", "My Plugin",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_THICKFRAME,
        100, 100, 300, 200,
        NULL, NULL, GetModuleHandle(NULL), NULL);
}
```

### Overlays

For transparent overlays on top of MegaMUD, use `WS_EX_LAYERED`:

```c
HWND overlay = CreateWindowExA(
    WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT,
    "MyOverlayClass", NULL,
    WS_POPUP | WS_VISIBLE,
    x, y, w, h,
    NULL, NULL, GetModuleHandle(NULL), NULL);
SetLayeredWindowAttributes(overlay, RGB(0,0,0), 0, LWA_COLORKEY);
```

### Vulkan / OpenGL

Since you're in-process with full Win32 access, you can:

1. Create a window (or use MegaMUD's HWND as the surface target)
2. Initialize Vulkan/OpenGL on that window
3. Render whatever you want

Link against the appropriate libraries when compiling:
```bash
i686-w64-mingw32-gcc -shared -o vulkan_overlay.dll vulkan_overlay.c \
    -lgdi32 -luser32 -lvulkan-1 -mwindows
```

---

## Best Practices

### Thread Safety

- The `on_line` and `on_round` callbacks are called from MajorSLOP's scanner thread
- If you update UI from these callbacks, use `PostMessage` to marshal to the UI thread
- Don't do heavy work in callbacks — offload to your own thread if needed

### Performance

- `on_line` fires frequently during combat — keep your handler fast
- If you need to poll the struct, do it on a timer thread, not every callback
- Don't create GDI objects in tight loops without deleting them

### Memory Safety

- The game struct pointer (`get_struct_base()`) can be 0 early on — always check
- String fields in the struct may not be null-terminated at the full buffer length
- Don't write past field boundaries when modifying struct memory

### Config Files

Store your plugin's config in the MegaMUD folder:

```c
static void load_config(void) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat(path, "plugins\\my_plugin.cfg");
    /* read your config from path */
}
```

### Cleanup

Always clean up in `shutdown()`:
- Destroy windows
- Kill timers
- Close handles
- Free allocated memory

```c
static void my_shutdown(void) {
    if (my_window) DestroyWindow(my_window);
    /* clean up other resources */
}
```

---

## Example Plugins

### DPS Meter

```c
#include "slop_plugin.h"
#include <string.h>
#include <stdlib.h>

static const slop_api_t *api = NULL;
static int total_damage = 0;
static int round_count = 0;

static int parse_damage(const char *line) {
    /* Look for "for N damage!" */
    const char *p = strstr(line, " for ");
    if (!p) return 0;
    p += 5;
    int dmg = atoi(p);
    if (dmg > 0 && strstr(p, " damage!"))
        return dmg;
    return 0;
}

static void on_line(const char *line) {
    int dmg = parse_damage(line);
    if (dmg > 0) total_damage += dmg;
}

static void on_round(int round_num) {
    round_count = round_num;
    if (round_count > 0) {
        float dps = (float)total_damage / (round_count * 5.0f);
        api->log("[dps] Total: %d dmg over %d rounds (%.1f DPS)\n",
                 total_damage, round_count, dps);
    }
}

static int my_init(const slop_api_t *a) {
    api = a;
    api->log("[dps_meter] Loaded\n");
    return 0;
}

static void my_shutdown(void) {}

static slop_plugin_t plugin = {
    .magic = SLOP_PLUGIN_MAGIC, .api_version = SLOP_API_VERSION,
    .name = "DPS Meter", .author = "You", .version = "1.0.0",
    .description = "Tracks damage per second",
    .init = my_init, .shutdown = my_shutdown,
    .on_line = on_line, .on_round = on_round, .on_wndproc = NULL,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &plugin; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
```

### Kill Counter with Popup

```c
#include "slop_plugin.h"
#include <string.h>

static const slop_api_t *api = NULL;
static int kills = 0;
static HWND counter_hwnd = NULL;

static LRESULT CALLBACK counter_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        FillRect(hdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 80, 80));
        char buf[32]; sprintf(buf, "Kills: %d", kills);
        HFONT f = CreateFontA(28,0,0,0,FW_BOLD,0,0,0,0,0,0,ANTIALIASED_QUALITY,0,"Arial");
        SelectObject(hdc, f);
        DrawTextA(hdc, buf, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(f);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_CLOSE) { ShowWindow(hwnd, SW_HIDE); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void on_line(const char *line) {
    if (strstr(line, " drops dead")) {
        kills++;
        if (counter_hwnd) InvalidateRect(counter_hwnd, NULL, FALSE);
    }
}

static int my_init(const slop_api_t *a) {
    api = a;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = counter_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "KillCounter";
    RegisterClassA(&wc);
    counter_hwnd = CreateWindowExA(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
        "KillCounter","Kills",WS_POPUP|WS_VISIBLE|WS_CAPTION,
        20,200,160,60,NULL,NULL,GetModuleHandle(NULL),NULL);
    return 0;
}

static void my_shutdown(void) {
    if (counter_hwnd) DestroyWindow(counter_hwnd);
}

static slop_plugin_t plugin = {
    .magic = SLOP_PLUGIN_MAGIC, .api_version = SLOP_API_VERSION,
    .name = "Kill Counter", .author = "You", .version = "1.0.0",
    .description = "Floating kill counter overlay",
    .init = my_init, .shutdown = my_shutdown,
    .on_line = on_line, .on_round = NULL, .on_wndproc = NULL,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &plugin; }
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
```

---

## Appendix: Offset Cheat Sheet

Most commonly used offsets for quick reference:

```c
/* Identity */
#define OFF_NAME         0x537C    // char[] player name
#define OFF_LEVEL        0x53D0    // i32

/* Vitals */
#define OFF_CUR_HP       0x53D4    // i32
#define OFF_MAX_HP       0x53DC    // i32
#define OFF_CUR_MANA     0x53E0    // i32
#define OFF_MAX_MANA     0x53E8    // i32

/* State */
#define OFF_IN_COMBAT    0x5698    // i32
#define OFF_IS_RESTING   0x5678    // i32
#define OFF_CONNECTED    0x563C    // i32
#define OFF_GO_FLAG      0x564C    // i32

/* Room */
#define OFF_ROOM_NAME    0x2D9C    // char[]
#define OFF_ROOM_HASH    0x2E70    // u32
#define OFF_ROOM_EXITS   0x2E78    // i32[10]
#define OFF_DARKNESS     0x5528    // i32

/* Combat */
#define OFF_TARGET       0x552C    // char[]
#define OFF_AUTO_COMBAT  0x4D00    // i32

/* Path */
#define OFF_PATHING      0x5664    // i32
#define OFF_PATH_STEP    0x5898    // i32
#define OFF_PATH_TOTAL   0x5894    // i32
#define OFF_MODE         0x54BC    // i32
```
