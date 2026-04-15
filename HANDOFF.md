# MegaMUD Crash Debugging Handoff

> **For the Claude instance running on the Win11 box.** The developer (my friend)
> has been debugging this from Linux; I've asked for your help because we've hit
> the limit of what log-iteration can tell us — we need a live debugger on the
> actual crashing process. This doc brings you up to speed.

---

## The Bug

**`C:\megamud\megamud.exe` deterministically crashes at `python312.dll+0x244045`**

- Exception: `0xC0000005` (ACCESS_VIOLATION, page fault)
- Same fault offset across every run — specific Python C-API hotspot
- Reproducible: launch megamud.exe normally, wait, game dies
- Windows Event Viewer: `Faulting module: python312.dll (3.12.8150.1013), offset 0x00244045`

## What We've Ruled Out

1. **Python 3.12 embeddable init on this machine** — A standalone probe
   (`py_probe.exe`, source in repo at `dll/py_probe.c`) tests 5 different init
   strategies against the exact same `plugins\python\python312.dll`. **All 5
   succeed.** Python can init perfectly on this box; the bug is not "Python
   can't start here."

2. **Python init inside mmudpy.dll** — `C:\megamud\mmudpy_debug.log` shows
   `[mmudpy] Python initialized` followed by `[mmudpy] Python ready, console
   open`. Bootstrap ran, Vulkan came up on his RTX 2070, all 6 plugins loaded,
   struct base resolved from MMMAIN window data. **The crash is post-init,
   during game runtime.**

3. **ASLR / hardcoded VAs** — Already fixed in a prior session. `mega_va()`
   helper in `dll/msimg32_proxy.c` rebases all VA_* constants via
   `GetModuleHandleA(NULL)`. Hooks install cleanly regardless of load base.

## Current Hypothesis (UNVERIFIED — this is what you're testing)

**GIL mismanagement.** `dll/mmudpy.c` around line 2943 calls
`pPyEval_SaveThread()` right after `load_python()` succeeds, which releases the
GIL and hands back a saved thread state. After that:

- TTS plugins (`plugins/TTS_SAM.dll`, `plugins/TTS_eSpeak.dll`) do "Python
  injection" — they call back into the mmudpy Python interpreter to register
  themselves. Log shows `[TTS_SAM] Python injection OK (rc=0)` — that's their
  init hook being called by mmudpy's plugin scanner.
- vk_terminal registers 113 ctypes ext commands into the Python `mud` namespace
  via the same scanner.
- A 50ms timer in `console_thread` drives `eval_pump()` (mmudpy.c:~2960) which
  calls `PyRun_SimpleString` with `PyGILState_Ensure`/`Release` bracketing.
- `on_line` / `get_line` callbacks (delivered from Windows message loop /
  comhook thread) push MUD data into Python.

If any of these re-enter Python without holding the GIL correctly, thread state
gets corrupted and a later internal Python operation (GC, hash table access,
refcount decrement) will fault at a deterministic hot path — exactly the
signature we see.

## Alternative Hypothesis

**Loader-lock race during mmudpy init.** `mmudpy_init` spawns `console_thread`
and returns immediately. The background thread then `LoadLibrary`s python312.dll
and runs `Py_Initialize` — which internally loads many `.pyd` files. Meanwhile
the main thread continues loading TTS_eSpeak, TTS_SAM, vk_terminal (each a
LoadLibrary). These compete for the Windows loader lock. On Wine/Linux this
resolves fine. On Win11 with tighter loader enforcement + CFG, races can leave
Python in a state where later callbacks find half-initialized module tables
and fault. Less likely given init completed cleanly in the log, but worth
keeping in mind.

## Your Job

**Get ground truth from a live debugger attached to megamud.exe.** Steps:

### 1. Prep

```powershell
# already done: Claude Code, Git, Node, WinDbg Preview installed
# Set symbol path so Python 3.12 public symbols auto-resolve
setx _NT_SYMBOL_PATH "srv*C:\symbols*https://msdl.microsoft.com/download/symbols"
```

Close + reopen your terminal so the env var is picked up.

### 2. Read the relevant source

In priority order:

- `dll/mmudpy.c` — lines ~2540-2730 (SEH guard, `load_python`, Python init),
  ~2920-2960 (`console_thread`, GIL release, message loop),
  ~2960-3070 (`eval_pump`, which handles Python eval requests),
  ~3073-3134 (`mmudpy_init`, `mmudpy_shutdown`)
- `dll/msimg32_proxy.c` — the DLL loader. Look at the plugin scanner and how
  it calls each plugin's `.init` hook (including mmudpy's and the TTS injection
  callbacks).
- `dll/py_probe.c` — for reference: this is the standalone probe that proves
  Python can init on this machine. Its init code is the "known good" pattern.

Also read:

- `C:\megamud\mmudpy_debug.log` — most recent run's log, confirms plugin init
  order and the point the log stops (shutdown sequence after the crash).
- `C:\megamud\mudplugin.log` — the msimg32 proxy's log, shows plugin load order
  and any hook failures.

### 3. Attach WinDbg Preview to megamud.exe

Two approaches:

**(a) Launch under debugger** (preferred — catches crashes during early init too):
- Open WinDbg Preview → "Launch executable (advanced)"
- Executable: `C:\megamud\megamud.exe`
- Start directory: `C:\megamud`
- Click "OK"
- At initial breakpoint: `g` (go)
- When it crashes: WinDbg automatically halts on the AV

**(b) Attach to running process** (if you need to attach post-launch):
- Launch megamud.exe normally
- In WinDbg Preview → "Attach to process" → pick megamud.exe
- `g`
- Wait for crash, WinDbg halts

### 4. At the crash

```
!analyze -v          # WinDbg's auto-analyzer, often identifies cause
.symfix              # ensure symbols are set
.reload /f           # force-reload with symbols
k                    # stack trace — this is the gold
kb                   # stack with args
.frame 0             # go to crashing frame
u @eip L20           # disassemble around the faulting instruction
r                    # register dump
dc @esp L40          # stack dump
```

Key questions to answer from the stack trace:
- What Python function is `python312+0x244045` actually inside?
  (expected: something like `_PyObject_GetState`, `_Py_Dealloc`,
  `take_gil`, `PyObject_GC_*` — this tells us the code path)
- What is the **caller**? (mmudpy.dll? vk_terminal.dll? TTS_SAM.dll?
  A Python-internal caller with no external frame?)
- What thread is crashing? (`~` lists threads, `~<N>k` shows the crashing
  thread's stack)
- Is the GIL held? Check `_PyThreadState_Current` or walk the `PyInterpreterState`
  list if symbols are loaded.

### 5. Report back

A stack trace from frame 0 up through at least 10 frames, plus the resolved
symbol for `python312+0x244045`, is enough for my friend on Linux to make
the fix. If you can identify whether it's a GIL invariant violation,
corrupted module state, or something else entirely, even better.

If you have time: try to fix it on the box (the repo at `C:\mudproxy\` is the
source; rebuilds need `i686-w64-mingw32-gcc` which isn't on Windows — but you
can iterate by editing and having my friend cross-compile, or MinGW-w64 via
MSYS2 on Windows if you want to build locally).

## File Map

```
C:\mudproxy\               <- the source (cloned from GitHub)
  dll\
    mmudpy.c               <- Python bridge DLL (main suspect)
    msimg32_proxy.c        <- proxy loader, installs hooks
    py_probe.c             <- standalone init probe (proved Python works)
    vk_terminal.c          <- Vulkan terminal + MUDRadio
    tts_sam.c              <- SAM TTS plugin
    tts_espeak.c           <- eSpeak TTS plugin
    release.sh             <- build+package script (Linux only)

C:\megamud\                <- the deployed game (what runs)
  megamud.exe              <- the target (32-bit)
  msimg32.dll              <- our proxy loader
  plugins\
    mmudpy.dll             <- Python bridge (from release v0.1.7-alpha)
    vk_terminal.dll
    TTS_SAM.dll
    TTS_eSpeak.dll
    script_manager.dll
    autoroam_watchdog.dll
    python\                <- Python 3.12.8 embeddable distribution
      python312.dll        <- the crashing module
      python312._pth
      python312.zip
    MMUDPy\
      mmudpy.dll           <- duplicate of plugins\mmudpy.dll (intentional)
      scripts\             <- user Python scripts, auto-loaded on bootstrap
  mmudpy_debug.log         <- mmudpy plugin's log
  mudplugin.log            <- msimg32 proxy's log
```

## Quick Win If You're Short On Time

If WinDbg is giving you trouble, a fast diagnostic:

1. Rename `C:\megamud\plugins\MMUDPy\scripts` to `scripts_disabled`
2. Launch megamud.exe
3. Does it still crash?
   - **Yes, same offset**: bug is in mmudpy bootstrap or plugin callbacks,
     not user scripts. Keep investigating.
   - **No, runs fine**: one of the auto-loaded scripts has an import or
     top-level call that crashes Python on Win11. Binary-search which
     script by moving them back one at a time.

## Thanks

The human this plugin suite is for is playing MajorMUD — a BBS game from 1990.
Getting it working on his Win11 box is the goal. Every iteration ships fixes
to him as `MajorSLOP-vX.Y.Z-alpha.zip`. Any insight you can get from the
debugger saves him dozens of blind-guess build cycles.
