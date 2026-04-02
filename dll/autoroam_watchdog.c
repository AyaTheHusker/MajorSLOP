/*
 * autoroam_watchdog.c — Auto-Roam Recovery Plugin
 *
 * Problem: GreaterMUD sometimes doesn't send the "Obvious exits:" line after
 * a room name. MegaMUD can't parse exits, so auto-roam stops dead. This can
 * waste days of unattended scripting.
 *
 * Fix: Monitor auto-roam state via a timer. When a stall is detected (was
 * auto-roaming, now idle), inject "look" to refresh the room description,
 * then re-enable auto-roam flags. If that fails, inject a direction command
 * to force a room change. Retries indefinitely with backoff.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o autoroam_watchdog.dll autoroam_watchdog.c \
 *     -lgdi32 -luser32 -static-libgcc
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ---- Struct offsets ---- */
#define OFF_ROOM_NAME       0x2D9C
#define OFF_ROOM_CHECKSUM   0x2E70
#define OFF_ROOM_EXITS      0x2E78   /* i32[10] — exit types per direction */
#define OFF_AUTO_MOVE       0x3274   /* u16 — direction bitmask (255=all) */
#define OFF_ON_ENTRY_ACTION 0x54B4   /* 0=nothing, 1=resume loop, 2=auto-roam */
#define OFF_MODE            0x54BC   /* 11=idle, 14=walking, 15=looping, 17=lost */
#define OFF_CONNECTED       0x563C
#define OFF_GO_FLAG         0x564C
#define OFF_PATHING_ACTIVE  0x5664
#define OFF_AUTO_ROAMING    0x566C
#define OFF_IS_LOST         0x5670
#define OFF_IN_COMBAT       0x5698

/* Direction indices matching ROOM_EXITS[10] */
#define DIR_NORTH     0
#define DIR_SOUTH     1
#define DIR_EAST      2
#define DIR_WEST      3
#define DIR_NORTHEAST 4
#define DIR_NORTHWEST 5
#define DIR_SOUTHEAST 6
#define DIR_SOUTHWEST 7
#define DIR_UP        8
#define DIR_DOWN      9

static const char *dir_cmds[] = {
    "n", "s", "e", "w", "ne", "nw", "se", "sw", "u", "d"
};

#define TIMER_ID_WATCHDOG  0xAE01
#define CHECK_INTERVAL_MS  3000   /* check every 3 seconds */
#define STALL_TICKS_ENTER  1      /* immediate: send Enter to refresh room */
#define STALL_TICKS_REENABLE 2    /* ~5-6s: re-enable auto-roam */
#define STALL_TICKS_LOOK   4      /* ~12s: inject "look" if still stuck */
#define STALL_TICKS_MOVE   6      /* ~18s: inject direction + full reset */

static const slop_api_t *api = NULL;

/* Watchdog state */
static int watchdog_enabled = 0;
static int was_autoroaming  = 0;
static int stall_ticks      = 0;
static int recovery_phase   = 0;   /* 0=watching, 1=tried look, 2=tried move */
static int total_recoveries = 0;
static unsigned int last_room_cksum = 0;
static DWORD last_user_action = 0;  /* GetTickCount() of last WM_COMMAND from user */
#define MANUAL_STOP_GRACE_MS 2000   /* if user clicked something <2s ago, it's intentional */

/* Menu IDs */
#define IDM_WATCHDOG_TOGGLE  50400
#define IDM_WATCHDOG_STATUS  50401
#define IDM_WATCHDOG_SIMSTALL 50402

/* ---- Helpers ---- */

static void write_i32(unsigned int offset, int value)
{
    unsigned int base = api->get_struct_base();
    if (!base) return;
    *(int *)((char *)base + offset) = value;
}

static int has_any_exits(void)
{
    for (int i = 0; i < 10; i++) {
        int exit_type = api->read_struct_i32(OFF_ROOM_EXITS + i * 4);
        if (exit_type != 0) return 1;
    }
    return 0;
}

static const char *pick_random_direction(void)
{
    unsigned int base = api->get_struct_base();
    if (!base) return "n";

    /* Read auto-move bitmask */
    int automove = api->read_struct_i16(OFF_AUTO_MOVE);
    if (automove == 0) automove = 0xFF; /* fallback: all cardinal */

    /* Collect allowed directions */
    int allowed[10];
    int count = 0;
    for (int i = 0; i < 10; i++) {
        if (automove & (1 << i))
            allowed[count++] = i;
    }
    if (count == 0) return "n";

    return dir_cmds[allowed[rand() % count]];
}

/* Dismiss any modal dialog (MessageBox) owned by MMMAIN */
static void dismiss_modal_dialog(void)
{
    HWND parent = api ? api->get_mmmain_hwnd() : NULL;
    if (!parent) return;

    /* Find any popup dialog owned by MMMAIN */
    HWND dialog = GetWindow(parent, GW_ENABLEDPOPUP);
    if (dialog && dialog != parent) {
        api->log("[Watchdog] Dismissing modal dialog (hwnd=0x%08X)\n", (unsigned int)dialog);
        PostMessageA(dialog, WM_COMMAND, IDOK, 0);
    }

    /* Also check #32770 class dialogs (standard MessageBox) */
    HWND msgbox = FindWindowExA(NULL, NULL, "#32770", NULL);
    while (msgbox) {
        HWND owner = GetWindow(msgbox, GW_OWNER);
        if (owner == parent && IsWindowVisible(msgbox)) {
            api->log("[Watchdog] Dismissing MessageBox (hwnd=0x%08X)\n", (unsigned int)msgbox);
            PostMessageA(msgbox, WM_COMMAND, IDOK, 0);
        }
        msgbox = FindWindowExA(NULL, msgbox, "#32770", NULL);
    }
}

static void reenable_autoroam(void)
{
    dismiss_modal_dialog();
    write_i32(OFF_AUTO_ROAMING, 1);
    write_i32(OFF_ON_ENTRY_ACTION, 2);  /* 2 = auto-roam on room entry */
    write_i32(OFF_GO_FLAG, 1);
    write_i32(OFF_PATHING_ACTIVE, 1);
    write_i32(OFF_MODE, 16);            /* 16 = auto-roaming */
    if (api) api->log("[Watchdog] Flags written: AUTO_ROAMING=1 ON_ENTRY=2 GO=1 PATHING=1 MODE=16\n");
}

/* ---- Timer check ---- */

static void watchdog_check(void)
{
    if (!api || !watchdog_enabled) return;

    unsigned int base = api->get_struct_base();
    if (!base) return;

    int connected = api->read_struct_i32(OFF_CONNECTED);
    if (!connected) {
        was_autoroaming = 0;
        stall_ticks = 0;
        recovery_phase = 0;
        return;
    }

    int auto_roaming = api->read_struct_i32(OFF_AUTO_ROAMING);
    int mode         = api->read_struct_i32(OFF_MODE);
    int in_combat    = api->read_struct_i32(OFF_IN_COMBAT);
    unsigned int room_cksum = (unsigned int)api->read_struct_i32(OFF_ROOM_CHECKSUM);

    /* If currently auto-roaming, all good — reset stall tracking
     * mode: 14=walking, 15=looping, 16=auto-roaming */
    if (auto_roaming && (mode == 14 || mode == 16 || in_combat)) {
        was_autoroaming = 1;
        stall_ticks = 0;
        recovery_phase = 0;
        last_room_cksum = room_cksum;
        return;
    }

    /* If we weren't auto-roaming before, nothing to recover */
    if (!was_autoroaming) return;

    /* In combat — don't interfere, just wait */
    if (in_combat) {
        stall_ticks = 0;
        return;
    }

    /* Check if user manually stopped (clicked a button recently) */
    if (stall_ticks == 0) {
        DWORD now = GetTickCount();
        if (last_user_action && (now - last_user_action) < MANUAL_STOP_GRACE_MS) {
            api->log("[Watchdog] Auto-roam stopped by user (button click %dms ago), not recovering\n",
                     (int)(now - last_user_action));
            was_autoroaming = 0;
            return;
        }
    }

    /* Auto-roam was active but now it's stopped. Stall detected. */
    stall_ticks++;

    if (stall_ticks == 1) {
        total_recoveries++;
        api->log("[Watchdog] Stall #%d detected (mode=%d, exits=%s, room=0x%08X)\n",
                 total_recoveries, mode, has_any_exits() ? "yes" : "MISSING", room_cksum);
    }

    /* Phase 1: immediately send Enter to refresh room */
    if (stall_ticks >= STALL_TICKS_ENTER && recovery_phase == 0) {
        api->log("[Watchdog] Stall #%d: sending Enter to refresh room\n", total_recoveries);
        api->inject_command("");
        recovery_phase = 1;
        return;
    }

    /* Phase 2: ~5s later, re-enable auto-roam — room should have exits now */
    if (stall_ticks >= STALL_TICKS_REENABLE && recovery_phase == 1) {
        api->log("[Watchdog] Stall #%d: re-enabling auto-roam (exits=%s)\n",
                 total_recoveries, has_any_exits() ? "yes" : "MISSING");
        reenable_autoroam();
        recovery_phase = 2;
        return;
    }

    /* Phase 3: if still stuck, Enter again + re-enable */
    if (stall_ticks >= STALL_TICKS_LOOK && recovery_phase == 2) {
        api->log("[Watchdog] Stall #%d: still stuck, Enter + re-enable\n", total_recoveries);
        api->inject_command("");
        reenable_autoroam();
        recovery_phase = 3;
        return;
    }

    /* Phase 4: last resort — random direction to force room change */
    if (stall_ticks >= STALL_TICKS_MOVE && recovery_phase == 3) {
        const char *dir = pick_random_direction();
        api->log("[Watchdog] Stall #%d: injecting '%s' + re-enable, restarting cycle\n",
                 total_recoveries, dir);
        api->inject_command(dir);
        reenable_autoroam();
        stall_ticks = 0;
        recovery_phase = 0;
        return;
    }
}

/* ---- Plugin callbacks ---- */

static int watchdog_init(const slop_api_t *a)
{
    api = a;
    srand((unsigned int)time(NULL));

    api->add_menu_item("Auto-Roam Watchdog: OFF", IDM_WATCHDOG_TOGGLE);
    api->add_menu_item("Watchdog Status", IDM_WATCHDOG_STATUS);
    api->add_menu_item("Simulate Stall (test)", IDM_WATCHDOG_SIMSTALL);

    /* Set up timer on MMMAIN window */
    HWND hwnd = api->get_mmmain_hwnd();
    if (hwnd) {
        SetTimer(hwnd, TIMER_ID_WATCHDOG, CHECK_INTERVAL_MS, NULL);
        api->log("[Watchdog] Timer started (every %dms)\n", CHECK_INTERVAL_MS);
    }

    api->log("[Watchdog] Auto-Roam Watchdog loaded (disabled by default, enable via menu)\n");
    return 0;
}

static void watchdog_shutdown(void)
{
    HWND hwnd = api ? api->get_mmmain_hwnd() : NULL;
    if (hwnd) KillTimer(hwnd, TIMER_ID_WATCHDOG);
    if (api) api->log("[Watchdog] Unloaded (%d total recoveries)\n", total_recoveries);
}

static int watchdog_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;

    /* Track user button clicks (but not our own menu items or timer) */
    if (msg == WM_COMMAND) {
        WORD cmd = LOWORD(wParam);
        /* DEBUG: log all WM_COMMAND IDs to find stop button — remove after discovery */
        if (api && cmd < 50400) /* skip our own menu items */
            api->log("[Watchdog:DBG] WM_COMMAND id=%d (0x%04X) notify=%d\n",
                     cmd, cmd, HIWORD(wParam));
        if (cmd != IDM_WATCHDOG_TOGGLE && cmd != IDM_WATCHDOG_STATUS && cmd != IDM_WATCHDOG_SIMSTALL)
            last_user_action = GetTickCount();
    }

    if (msg == WM_TIMER && wParam == TIMER_ID_WATCHDOG) {
        watchdog_check();
        return 0;
    }

    if (msg == WM_COMMAND) {
        WORD id = LOWORD(wParam);

        if (id == IDM_WATCHDOG_TOGGLE) {
            watchdog_enabled = !watchdog_enabled;
            was_autoroaming = 0;
            stall_ticks = 0;
            recovery_phase = 0;

            /* Update menu item text */
            HMENU menu = GetMenu(hwnd);
            if (menu) {
                MENUITEMINFOA mii = {0};
                mii.cbSize = sizeof(mii);
                mii.fMask = MIIM_STRING;
                mii.dwTypeData = watchdog_enabled
                    ? "Auto-Roam Watchdog: ON"
                    : "Auto-Roam Watchdog: OFF";
                SetMenuItemInfoA(menu, IDM_WATCHDOG_TOGGLE, FALSE, &mii);
                DrawMenuBar(hwnd);
            }

            if (api) api->log("[Watchdog] %s\n", watchdog_enabled ? "ENABLED" : "DISABLED");
            return 1;
        }

        if (id == IDM_WATCHDOG_STATUS) {
            char buf[512];
            int auto_roaming = 0, mode = 0, exits = 0;
            unsigned int base = api ? api->get_struct_base() : 0;
            if (base) {
                auto_roaming = api->read_struct_i32(OFF_AUTO_ROAMING);
                mode = api->read_struct_i32(OFF_MODE);
                exits = has_any_exits();
            }
            sprintf(buf,
                "Watchdog: %s\n"
                "Auto-Roaming: %s\n"
                "Mode: %d (%s)\n"
                "Room Exits: %s\n"
                "Stall Ticks: %d\n"
                "Recovery Phase: %d\n"
                "Total Recoveries: %d\n"
                "Tracking: %s",
                watchdog_enabled ? "ON" : "OFF",
                auto_roaming ? "YES" : "NO",
                mode,
                mode == 11 ? "idle" : mode == 14 ? "walking" :
                mode == 15 ? "looping" : mode == 16 ? "auto-roaming" :
                mode == 17 ? "lost" : "?",
                exits ? "present" : "MISSING",
                stall_ticks,
                recovery_phase,
                total_recoveries,
                was_autoroaming ? "was auto-roaming" : "not tracking");
            MessageBoxA(hwnd, buf, "Auto-Roam Watchdog", MB_OK | MB_ICONINFORMATION);
            return 1;
        }

        if (id == IDM_WATCHDOG_SIMSTALL) {
            unsigned int base = api ? api->get_struct_base() : 0;
            if (!base) {
                MessageBoxA(hwnd, "Not connected — no struct base.", "Simulate Stall", MB_OK | MB_ICONWARNING);
                return 1;
            }

            if (!api->inject_server_data) {
                MessageBoxA(hwnd, "inject_server_data not available.",
                            "Simulate Stall", MB_OK | MB_ICONWARNING);
                return 1;
            }

            /* Feed fake server data through the real pipeline:
             * A room description with "Obvious exits: NONE" will make MegaMUD
             * think there are no exits, triggering its real stall behavior.
             * This goes through MMANSI terminal rendering + the full parser. */
            was_autoroaming = 1;
            stall_ticks = 0;
            recovery_phase = 0;

            api->log("[Watchdog] *** SIMULATED STALL — injecting fake server data ***\n");

            /* Send a fake room with no valid exits through the real pipeline.
             * Actual MMANSI attribute bytes from live terminal:
             *   Room name:      0x0E = bold yellow  = \033[1;33m
             *   Description:    0x06 = dark cyan     = \033[0;36m
             *   Obvious exits:  0x02 = dark green    = \033[0;32m (entire line, same color)
             */
            const char *fake_data =
                "\r\n\033[1;33mTest Room (Simulated)\033[0m\r\n"
                "\033[0;36mThis is a simulated room with no exits.\033[0m\r\n"
                "\033[0;32mObvious exits: NONE\033[0m\r\n";
            api->inject_server_data(fake_data, (int)strlen(fake_data));
            return 1;
        }
    }

    return 0;
}

/* ---- Plugin descriptor ---- */

static slop_plugin_t watchdog_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Auto-Roam Watchdog",
    .author      = "Tripmunk",
    .description = "Recovers auto-roam from missing exits stalls",
    .version     = "1.0.0",
    .init        = watchdog_init,
    .shutdown    = watchdog_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = watchdog_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &watchdog_plugin; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
