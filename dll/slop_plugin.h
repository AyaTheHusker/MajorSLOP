/*
 * slop_plugin.h — MajorSLOP Plugin SDK
 * =====================================
 *
 * Include this header in your plugin DLL to interface with MajorSLOP.
 *
 * QUICK START:
 *   1. #include "slop_plugin.h"
 *   2. Define your plugin descriptor (slop_plugin_t)
 *   3. Export slop_get_plugin() returning a pointer to it
 *   4. Compile as 32-bit DLL, drop into MegaMUD/plugins/
 *
 * BUILD (MinGW):
 *   i686-w64-mingw32-gcc -shared -o my_plugin.dll my_plugin.c -lgdi32 -luser32
 *
 * EXAMPLE:
 *   See bottom of this file for a minimal plugin template.
 *
 * The magic value SLOP_PLUGIN_MAGIC must match or MajorSLOP will refuse
 * to load the DLL. This prevents random DLLs from being dropped in.
 */

#ifndef SLOP_PLUGIN_H
#define SLOP_PLUGIN_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Magic & Version ---- */

/*
 * "SL0P" in little-endian: 0x50304C53
 * Plugin must set descriptor->magic to this exact value.
 */
#define SLOP_PLUGIN_MAGIC    0x50304C53u
#define SLOP_API_VERSION     1

/* ---- API provided by MajorSLOP to plugins ---- */

typedef struct slop_api {
    unsigned int    api_version;        /* SLOP_API_VERSION */

    /* Logging — writes to MajorSLOP's log file */
    void          (*log)(const char *fmt, ...);

    /* MMANSI terminal access */
    HWND          (*get_mmansi_hwnd)(void);         /* MMANSI child window */
    int           (*read_terminal_row)(int row, char *out, int out_sz);
    int             terminal_rows;                  /* max rows (60) */
    int             terminal_cols;                  /* max cols (132) */

    /* MegaMUD window handles */
    HWND          (*get_mmmain_hwnd)(void);         /* MMMAIN parent window */

    /* Game struct memory access */
    unsigned int  (*get_struct_base)(void);          /* base addr of player struct, 0 if not found */
    int           (*read_struct_i32)(unsigned int offset);   /* read i32 at struct_base + offset */
    int           (*read_struct_i16)(unsigned int offset);   /* read i16 at struct_base + offset */

    /* Command injection — types text into MMANSI terminal */
    void          (*inject_text)(const char *text);  /* sends each char via WM_CHAR */
    void          (*inject_command)(const char *cmd); /* inject_text(cmd) + Enter */

    /* Menu integration — returns menu item ID, 0 on failure */
    int           (*add_menu_item)(const char *label, unsigned int id);
    int           (*add_menu_separator)(void);

    /* Round timer events — register callbacks (NULL to unregister) */
    void          (*on_round_tick)(void (*callback)(int round_num));
    void          (*on_terminal_line)(void (*callback)(const char *line));

    /* Server data injection — feeds raw bytes through MegaMUD's real pipeline.
     * Data appears in MMANSI terminal and goes through the full line parser,
     * exit detection, etc. Use ANSI/telnet format.
     * Example: inject_server_data("\r\nObvious exits: NONE\r\n", 23) */
    void          (*inject_server_data)(const char *data, int len);

    /* Fake remote — execute @commands without telepath overhead.
     * Calls MegaMUD's internal functions directly (Ghidra RE of FUN_0047cf70).
     * No telepath response is sent. Returns 0 on success, -1 on failure.
     * Commands: "stop", "rego", "roam on", "roam off",
     *           "loop <name>", "goto <room>", "reset" */
    int           (*fake_remote)(const char *cmd);

    /* Set by the loader before init() — the base menu command ID for this plugin.
     * Menu items are at menu_base_id+0, menu_base_id+1, etc. (up to +9) */
    int             menu_base_id;

    /* Strip ANSI escape sequences (CSI and non-CSI) from a NUL-terminated
     * string in place. Plugins that want to pattern-match against clean
     * text can call this directly instead of re-implementing it. (Added
     * in place of _reserved[0] — still ABI-compatible with v1 since
     * _reserved slots were always unused by plugin code.) */
    void          (*strip_ansi)(char *line);

    /* Import a .mp path file into MegaMUD's path database.
     * Copies file to Default\, parses it, adds to DB, refreshes menu.
     * Preserves active pathing state (step, loop position, etc).
     * Returns 0 on success, negative on error. */
    int           (*import_path)(const char *mp_filepath);

    /* Code a room into MegaMUD's Rooms.md and reload the database.
     * cksum: 8-hex-char room checksum (e.g. 0xD4E00091)
     * code: 4-char room code (e.g. "STON")
     * area: area name (e.g. "Silvermere")
     * room_name: full room name (e.g. "Town Square")
     * Returns 0 on success, 1 if already known, negative on error.
     * Preserves active pathing state across the reload. */
    int           (*code_room)(unsigned int cksum, const char *code,
                               const char *area, const char *room_name);

    /* Check if a room checksum is known in MegaMUD's Rooms.md.
     * If known, copies the 4-char code into out_code (up to out_sz).
     * Returns 1 if found, 0 if not. */
    int           (*room_is_known)(unsigned int cksum, char *out_code, int out_sz);

    /* Reserved for future expansion */
    void          *_reserved[1];
} slop_api_t;


/* ---- Plugin descriptor ---- */

typedef struct slop_plugin {
    unsigned int    magic;          /* MUST be SLOP_PLUGIN_MAGIC */
    unsigned int    api_version;    /* MUST be SLOP_API_VERSION */
    const char     *name;           /* short plugin name, e.g. "Vulkan Overlay" */
    const char     *author;         /* author name */
    const char     *description;    /* one-line description */
    const char     *version;        /* version string, e.g. "1.0.0" */

    /* Called after loading — return 0 for success, nonzero to abort */
    int           (*init)(const slop_api_t *api);

    /* Called on DLL unload / MegaMUD exit */
    void          (*shutdown)(void);

    /* Optional: called on every new terminal line (NULL if unused) */
    void          (*on_line)(const char *line);

    /* Optional: called on each combat round tick (NULL if unused) */
    void          (*on_round)(int round_num);

    /* Optional: WndProc hook — return non-zero if handled (NULL if unused) */
    int           (*on_wndproc)(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /* Optional: called with raw server data bytes (NULL if unused).
     * Unlike on_line, data is NOT split on \n — arrives in arbitrary chunks
     * with full ANSI escape codes intact. For terminal emulators that need
     * byte-by-byte processing. */
    void          (*on_data)(const char *data, int len);

    /* Optional: like on_line but the line has been run through strip_ansi
     * first, so it's plain text. Prefer this over on_line for any parser
     * that does substring/prefix matching — avoids having to re-implement
     * the ANSI stripper in every plugin. (Added in place of _reserved[0] —
     * ABI-compatible with v1; old plugins simply never set it so it's NULL
     * and the dispatcher skips it.) */
    void          (*on_clean_line)(const char *line);

    /* Reserved for future expansion */
    void          *_reserved[2];
} slop_plugin_t;


/* ---- Extension command registration (for MMUDPy discovery) ---- */

/*
 * Plugins that want to expose functions to MMUDPy's Python `mud` object
 * export slop_get_commands() returning a table of slop_command_t.
 *
 * MMUDPy scans loaded plugin DLLs for this export and auto-wraps each
 * command as mud.{py_name}() using ctypes.
 *
 * arg_types chars: 'i' = int, 's' = c_char_p (string), 'f' = float
 * ret_type  chars: 'v' = void, 'i' = int, 's' = c_char_p, 'f' = float
 */
typedef struct slop_command {
    const char *py_name;     /* Python method name on mud, e.g. "rt_show" */
    const char *c_func;      /* DLL export name, e.g. "gl_rt_show" */
    const char *arg_types;   /* "" = none, "i" = int, "siii" = str+3 ints */
    const char *ret_type;    /* "v" = void, "i" = int, "s" = string */
    const char *doc;         /* docstring for mud.help() */
} slop_command_t;

/* Plugin exports this to advertise commands.  Returns count. */
typedef slop_command_t *(*slop_get_commands_fn)(int *count);

/* ---- Export macro ---- */

/*
 * Every plugin MUST export this function.
 * MajorSLOP calls it to get the plugin descriptor.
 */
#define SLOP_EXPORT __declspec(dllexport)

/* Use this in your plugin:
 *   SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &my_plugin; }
 */
typedef slop_plugin_t *(*slop_get_plugin_fn)(void);


#ifdef __cplusplus
}
#endif

#endif /* SLOP_PLUGIN_H */


/*
 * =========================================================================
 * MINIMAL PLUGIN TEMPLATE
 * =========================================================================
 *
 * Copy-paste this into a new .c file to get started:
 *
 *   #include "slop_plugin.h"
 *
 *   static const slop_api_t *g_api = NULL;
 *
 *   static int my_init(const slop_api_t *api) {
 *       g_api = api;
 *       api->log("[my_plugin] Loaded!\n");
 *       return 0;  // success
 *   }
 *
 *   static void my_shutdown(void) {
 *       if (g_api) g_api->log("[my_plugin] Unloaded.\n");
 *   }
 *
 *   static void my_on_line(const char *line) {
 *       // Called for every terminal line — parse triggers here
 *   }
 *
 *   static slop_plugin_t my_plugin = {
 *       .magic       = SLOP_PLUGIN_MAGIC,
 *       .api_version = SLOP_API_VERSION,
 *       .name        = "My Plugin",
 *       .author      = "YourName",
 *       .description = "Does cool stuff",
 *       .version     = "1.0.0",
 *       .init        = my_init,
 *       .shutdown    = my_shutdown,
 *       .on_line     = my_on_line,
 *       .on_round    = NULL,
 *       .on_wndproc  = NULL,
 *   };
 *
 *   SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) {
 *       return &my_plugin;
 *   }
 *
 *   BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) {
 *       return TRUE;
 *   }
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o my_plugin.dll my_plugin.c -lgdi32 -luser32
 *
 * Install:
 *   Copy my_plugin.dll to MegaMUD/plugins/
 *   Restart MegaMUD — check MajorSLOP > Plugins...
 *
 * =========================================================================
 */
