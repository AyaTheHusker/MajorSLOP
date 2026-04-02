/*
 * vk_terminal.c — Vulkan Fullscreen Terminal Plugin for MajorSLOP
 * ================================================================
 *
 * Renders MMANSI terminal buffer using Vulkan. F11 toggles fullscreen.
 * Pixel-perfect CP437 bitmap font, DOS color palette, input bar with
 * command history. Writes buffer to /tmp/mmansi_buf for external tools.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o vk_terminal.dll vk_terminal.c \
 *       -I. -L/usr/lib/wine/i386-windows -lvulkan-1 \
 *       -lgdi32 -luser32 -mwindows -O2
 */

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_headers/vulkan.h"
#include "slop_plugin.h"
#include "cp437.h"          /* original 8x16 bitmap (fallback) */
#include "cp437_fonts.h"    /* 14 pre-rendered TTF font atlases */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Proper byte-by-byte ANSI/VT100 state machine parser */
#define AP_ROWS 25
#define AP_COLS 80
#define ANSI_PARSER_IMPLEMENTATION
#include "ansi_parser.h"

/* ---- Terminal constants ---- */

#define TERM_ROWS       25
#define TERM_COLS       80

#define INPUT_BAR_H     40      /* pixels for input bar */
#define MAX_HISTORY     64
#define INPUT_BUF_SZ    256

#define IDM_VKT_TOGGLE  51000
#define IDM_VKT_RES_BASE 51010  /* 51010..51019 for resolutions */
#define IDM_VKT_THEME_BASE 51020
#define IDM_VKT_SETTINGS 51030
#define IDM_VKT_FONT_BASE 51040  /* 51040..51059 for fonts */
#define IDM_VKT_FONT_BITMAP 51039  /* special: original 8x16 bitmap */

/* Max quads: terminal ~16k + menus ~500 + windows ~8k */
#define MAX_QUADS       25000
#define VERTS_PER_QUAD  4
#define IDXS_PER_QUAD   6

/* ---- Vertex format ---- */

typedef struct {
    float x, y;     /* position in NDC */
    float u, v;     /* texture UV */
    float r, g, b;  /* color */
    float a;        /* alpha (1.0 for bg, texture alpha for text) */
} vertex_t;

/* ---- Color palettes ---- */

typedef struct { float r, g, b; } rgb_t;

/* Classic VGA ANSI palette — always used for terminal text rendering */
static rgb_t pal_classic[16] = {
    {0.00f,0.00f,0.00f},  /*  0 black   */
    {0.67f,0.00f,0.00f},  /*  1 red     */
    {0.00f,0.67f,0.00f},  /*  2 green   */
    {0.67f,0.33f,0.00f},  /*  3 brown   */
    {0.00f,0.00f,0.67f},  /*  4 blue    */
    {0.67f,0.00f,0.67f},  /*  5 magenta */
    {0.00f,0.67f,0.67f},  /*  6 cyan    */
    {0.67f,0.67f,0.67f},  /*  7 light gray */
    {0.33f,0.33f,0.33f},  /*  8 dark gray  */
    {1.00f,0.33f,0.33f},  /*  9 light red  */
    {0.33f,1.00f,0.33f},  /* 10 light green */
    {1.00f,1.00f,0.33f},  /* 11 yellow     */
    {0.33f,0.33f,1.00f},  /* 12 light blue */
    {1.00f,0.33f,1.00f},  /* 13 light magenta */
    {0.33f,1.00f,1.00f},  /* 14 light cyan */
    {1.00f,1.00f,1.00f},  /* 15 bright white */
};

/* ---- UI Themes — matches MajorSLOP web UI themes ---- */
/* Each theme provides colors for menu/chrome rendering.
 * bg=menu background, text=primary text, dim=secondary text, accent=highlight */
typedef struct {
    const char *name;
    float bg[3];
    float text[3];
    float dim[3];
    float accent[3];
} ui_theme_t;

/* Hex-to-float helper: RGB(0xCC, 0xAA, 0x44) */
#define F3(r,g,b) {(r)/255.0f, (g)/255.0f, (b)/255.0f}

#define NUM_THEMES 17
static const ui_theme_t ui_themes[NUM_THEMES] = {
    {"Grey Lord",      F3(12,12,28),   F3(200,200,224), F3(102,102,170), F3(68,68,204)},
    {"Black Fort",     F3(4,4,10),     F3(153,154,176), F3(68,68,102),   F3(51,51,102)},
    {"Khazarad",       F3(18,14,8),    F3(212,196,160), F3(138,122,86),  F3(170,136,68)},
    {"Silvermere",     F3(22,22,30),   F3(216,216,232), F3(136,136,170), F3(119,136,187)},
    {"Annora",         F3(30,30,40),   F3(238,238,244), F3(170,170,204), F3(153,170,221)},
    {"Jorah",          F3(8,12,28),    F3(176,192,232), F3(85,102,170),  F3(51,102,204)},
    {"Putakwa",        F3(6,16,14),    F3(160,216,200), F3(68,136,102),  F3(34,170,136)},
    {"Void",           F3(14,6,24),    F3(208,176,232), F3(119,68,170),  F3(136,68,204)},
    {"Ozzrinom",       F3(16,6,8),     F3(224,176,176), F3(136,68,68),   F3(204,51,68)},
    {"Phoenix",        F3(16,8,4),     F3(232,208,168), F3(136,102,64),  F3(221,102,34)},
    {"Mad Wizard",     F3(10,6,20),    F3(224,240,255), F3(102,170,204), F3(0,238,255)},
    {"Tasloi",         F3(10,16,8),    F3(192,216,160), F3(102,136,68),  F3(68,170,34)},
    {"Frostborn",      F3(8,14,22),    F3(200,224,240), F3(85,136,170),  F3(68,170,221)},
    {"Sandstorm",      F3(16,12,8),    F3(224,212,184), F3(138,122,86),  F3(204,170,68)},
    {"Crystal Cavern", F3(12,8,20),    F3(208,192,232), F3(119,102,170), F3(153,102,238)},
    {"Afroman",        F3(8,8,18),     F3(232,224,240), F3(136,119,170), F3(204,34,68)},
    {"Bog Lord",       F3(10,14,8),    F3(176,184,144), F3(96,104,64),   F3(119,136,51)},
};

/* ---- Resolution presets ---- */

typedef struct { int w, h; const char *name; } res_t;
static res_t resolutions[] = {
    { 1920, 1080, "1920x1080 (Full HD)" },
    { 1600,  900, "1600x900" },
    { 1366,  768, "1366x768" },
    { 1280,  720, "1280x720 (HD)" },
    { 1024,  768, "1024x768" },
};
#define NUM_RES (sizeof(resolutions)/sizeof(resolutions[0]))

/* ---- Embedded SPIR-V shaders ---- */

#include "shaders/term_vert.h"
#include "shaders/term_frag.h"

/* ---- Plugin state ---- */

static const slop_api_t *api = NULL;
static HWND mmansi_hwnd = NULL;
static HWND mmmain_hwnd = NULL;
static HWND vkt_hwnd = NULL;
static HANDLE vkt_thread_handle = NULL;
static volatile int vkt_running = 0;
static volatile int vkt_visible = 0;
static volatile int vkt_screenshot_pending = 0;
static uint32_t vkt_screenshot_img_idx = 0;

/* Settings */
static int current_theme = 0;          /* index into ui_themes[] */
static rgb_t *palette = pal_classic;   /* ANSI palette — always Classic VGA */
static int fs_res_idx = 0;     /* fullscreen resolution index */
static int fs_width = 1920;
static int fs_height = 1080;

/* Font selection: -1 = original bitmap, 0..NUM_FONTS-1 = TTF atlas */
static int current_font = -1;
static volatile int font_change_pending = 0;
static int pending_font_idx = -1;

/* ---- Vulkan-rendered context menu ---- */
#define VKM_CLOSED     0
#define VKM_ROOT       1

/* Root menu item indices */
#define VKM_ITEM_THEME   0
#define VKM_ITEM_FONT    1
#define VKM_ITEM_SEP     2
#define VKM_ITEM_CONSOLE 3
#define VKM_ITEM_SEP2    4
#define VKM_ITEM_CLOSE   5
#define VKM_ROOT_COUNT   6

/* Submenu types */
#define VKM_SUB_NONE   0
#define VKM_SUB_THEME  1
#define VKM_SUB_FONT   2

static int vkm_open = 0;          /* VKM_CLOSED or VKM_ROOT */
static int vkm_x = 0, vkm_y = 0; /* root menu top-left in pixels */
static int vkm_hover = -1;        /* hovered root item */
static int vkm_sub = VKM_SUB_NONE;/* which submenu is expanded */
static int vkm_sub_hover = -1;    /* hovered submenu item */
static int vkm_mouse_x = 0, vkm_mouse_y = 0;

/* Menu rendering constants */
#define VKM_ITEM_H   32
#define VKM_PAD      10
#define VKM_ROOT_W   210
#define VKM_SUB_W    280
#define VKM_SEP_H    12
#define VKM_CHAR_W   10   /* menu text character width */
#define VKM_CHAR_H   20   /* menu text character height */
#define VKM_SHADOW   6    /* drop shadow offset */

/* Forward declarations for rendering helpers (defined after Vulkan init) */
static void push_solid(float x0, float y0, float x1, float y1,
                       float r, float g, float b, float a, int vp_w, int vp_h);
static void push_text(int px, int py, const char *str,
                      float r, float g, float b, int vp_w, int vp_h, int char_w, int char_h);

/* ---- Vulkan Window System (vkw_) ---- */
/* Floating windows rendered as quads inside the same Vulkan surface.
 * No OS windows — zero alt-tab entries, zero focus issues. */

#define VKW_MAX_WINDOWS  8
#define VKW_MAX_LINES    200
#define VKW_LINE_LEN     128
#define VKW_TITLEBAR_H   24
#define VKW_BORDER_W     1
#define VKW_SHADOW_OFF   6
#define VKW_CHAR_W       8
#define VKW_CHAR_H       16
#define VKW_PAD          4
#define VKW_CLOSE_W      20     /* close button width */
#define VKW_RESIZE_ZONE  8      /* pixels from edge = resize grab */
#define VKW_INPUT_H      20     /* input line height */
#define VKW_MIN_W        160
#define VKW_MIN_H        100

typedef void (*vkw_input_cb_t)(int wnd_id, const char *text);

typedef struct {
    int active;
    int id;
    char title[64];
    float x, y, w, h;          /* position and size in pixels */
    float opacity;              /* 0.0–1.0, default 0.92 */

    /* Text content (circular buffer) */
    char lines[VKW_MAX_LINES][VKW_LINE_LEN];
    int line_head;              /* next write slot */
    int line_count;             /* total lines stored (max VKW_MAX_LINES) */
    int scroll;                 /* lines scrolled up from bottom */

    /* Input line */
    int has_input;
    char input[VKW_LINE_LEN];
    int input_len;
    int input_cursor;

    /* Command history (per-window) */
    #define VKW_MAX_HISTORY 64
    char *cmd_hist[VKW_MAX_HISTORY];
    int hist_count;
    int hist_idx;

    /* Interaction state */
    int dragging;
    float drag_ox, drag_oy;
    int resizing;
    float resize_ox, resize_oy, resize_sw, resize_sh;
} vkw_window_t;

static vkw_window_t vkw_windows[VKW_MAX_WINDOWS];
static int vkw_order[VKW_MAX_WINDOWS];  /* z-order: order[0]=back, order[n-1]=front */
static int vkw_count = 0;
static int vkw_next_id = 1;
static int vkw_focus = -1;             /* index into vkw_windows[], -1 = terminal has focus */
static vkw_input_cb_t vkw_input_callback = NULL;

/* Console window shortcut */
static int vkw_console_idx = -1;

/* Forward decl for window printing */
static void vkw_print(int idx, const char *text);

/* MMUDPy eval bridge — resolved lazily from mmudpy.dll */
typedef void (*mmudpy_queue_eval_fn)(const char *code, int target);
static mmudpy_queue_eval_fn pQueueEval = NULL;
static int eval_resolved = 0;

static mmudpy_queue_eval_fn vkt_resolve_eval(void)
{
    if (!eval_resolved) {
        HMODULE h = GetModuleHandleA("mmudpy.dll");
        if (!h) h = GetModuleHandleA("MMUDPy.dll");
        if (h) pQueueEval = (mmudpy_queue_eval_fn)GetProcAddress(h, "mmudpy_queue_eval");
        eval_resolved = 1;
    }
    return pQueueEval;
}

static void vkt_eval_python(const char *code, int target_wnd_id)
{
    mmudpy_queue_eval_fn fn = vkt_resolve_eval();
    if (fn) {
        fn(code, target_wnd_id);
    } else if (vkw_console_idx >= 0) {
        vkw_print(vkw_console_idx, "[error] MMUDPy not loaded — cannot eval Python");
    }
}

static int vkw_create(const char *title, int x, int y, int w, int h, int has_input)
{
    if (vkw_count >= VKW_MAX_WINDOWS) return -1;
    int idx = -1;
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        if (!vkw_windows[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;

    vkw_window_t *win = &vkw_windows[idx];
    memset(win, 0, sizeof(*win));
    win->active = 1;
    win->id = vkw_next_id++;
    if (title) { strncpy(win->title, title, 63); win->title[63] = 0; }
    win->x = (float)x; win->y = (float)y;
    win->w = (float)w; win->h = (float)h;
    win->opacity = 0.92f;
    win->has_input = has_input;

    /* Add to z-order (top) */
    vkw_order[vkw_count] = idx;
    vkw_count++;
    return idx;
}

static void vkw_close(int idx)
{
    if (idx < 0 || idx >= VKW_MAX_WINDOWS || !vkw_windows[idx].active) return;
    vkw_windows[idx].active = 0;
    if (vkw_focus == idx) vkw_focus = -1;
    if (vkw_console_idx == idx) vkw_console_idx = -1;

    /* Remove from z-order */
    int found = 0;
    for (int i = 0; i < vkw_count; i++) {
        if (vkw_order[i] == idx) found = 1;
        if (found && i + 1 < vkw_count) vkw_order[i] = vkw_order[i + 1];
    }
    if (found) vkw_count--;
}

static void vkw_bring_to_front(int idx)
{
    /* Move idx to top of z-order */
    int pos = -1;
    for (int i = 0; i < vkw_count; i++) {
        if (vkw_order[i] == idx) { pos = i; break; }
    }
    if (pos < 0 || pos == vkw_count - 1) return;
    for (int i = pos; i < vkw_count - 1; i++)
        vkw_order[i] = vkw_order[i + 1];
    vkw_order[vkw_count - 1] = idx;
}

static void vkw_print(int idx, const char *text)
{
    if (idx < 0 || idx >= VKW_MAX_WINDOWS || !vkw_windows[idx].active) return;
    vkw_window_t *w = &vkw_windows[idx];

    /* Split text on newlines, write each as a line */
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len >= VKW_LINE_LEN) len = VKW_LINE_LEN - 1;
        memcpy(w->lines[w->line_head], p, len);
        w->lines[w->line_head][len] = 0;
        w->line_head = (w->line_head + 1) % VKW_MAX_LINES;
        if (w->line_count < VKW_MAX_LINES) w->line_count++;
        p += len + (nl ? 1 : len); /* advance past newline or to end */
        if (!nl) break;
    }
    w->scroll = 0; /* auto-scroll to bottom */
}

static void vkw_clear(int idx)
{
    if (idx < 0 || idx >= VKW_MAX_WINDOWS || !vkw_windows[idx].active) return;
    vkw_windows[idx].line_head = 0;
    vkw_windows[idx].line_count = 0;
    vkw_windows[idx].scroll = 0;
}

/* ---- Window rendering ---- */

static void vkw_draw_one(vkw_window_t *w, int is_focused, int vp_w, int vp_h)
{
    const ui_theme_t *t = &ui_themes[current_theme];
    float op = w->opacity;
    int ix = (int)w->x, iy = (int)w->y;
    int iw = (int)w->w, ih = (int)w->h;

    /* Drop shadow */
    push_solid(ix + VKW_SHADOW_OFF, iy + VKW_SHADOW_OFF,
               ix + iw + VKW_SHADOW_OFF, iy + ih + VKW_SHADOW_OFF,
               0.0f, 0.0f, 0.0f, 0.5f * op, vp_w, vp_h);

    /* Window background */
    push_solid(ix, iy, ix + iw, iy + ih,
               t->bg[0], t->bg[1], t->bg[2], op, vp_w, vp_h);

    /* Title bar */
    float tb_r = t->bg[0] + 0.06f, tb_g = t->bg[1] + 0.06f, tb_b = t->bg[2] + 0.06f;
    if (is_focused) {
        tb_r = t->bg[0] * 0.5f + t->accent[0] * 0.3f + 0.05f;
        tb_g = t->bg[1] * 0.5f + t->accent[1] * 0.3f + 0.05f;
        tb_b = t->bg[2] * 0.5f + t->accent[2] * 0.3f + 0.05f;
    }
    push_solid(ix, iy, ix + iw, iy + VKW_TITLEBAR_H,
               tb_r, tb_g, tb_b, op, vp_w, vp_h);

    /* Title text */
    push_text(ix + VKW_PAD, iy + (VKW_TITLEBAR_H - VKW_CHAR_H) / 2,
              w->title, t->text[0], t->text[1], t->text[2],
              vp_w, vp_h, VKW_CHAR_W, VKW_CHAR_H);

    /* Close button [X] */
    int cx = ix + iw - VKW_CLOSE_W - 2;
    int cy = iy + 2;
    push_solid(cx, cy, cx + VKW_CLOSE_W, cy + VKW_TITLEBAR_H - 4,
               0.6f, 0.15f, 0.15f, op, vp_w, vp_h);
    push_text(cx + (VKW_CLOSE_W - VKW_CHAR_W) / 2,
              cy + (VKW_TITLEBAR_H - 4 - VKW_CHAR_H) / 2,
              "X", 1.0f, 0.8f, 0.8f, vp_w, vp_h, VKW_CHAR_W, VKW_CHAR_H);

    /* Border */
    float br = t->accent[0] * 0.5f, bg = t->accent[1] * 0.5f, bb = t->accent[2] * 0.5f;
    if (is_focused) { br = t->accent[0] * 0.8f; bg = t->accent[1] * 0.8f; bb = t->accent[2] * 0.8f; }
    push_solid(ix, iy, ix + iw, iy + 1, br, bg, bb, op, vp_w, vp_h);
    push_solid(ix, iy + ih - 1, ix + iw, iy + ih, br, bg, bb, op, vp_w, vp_h);
    push_solid(ix, iy, ix + 1, iy + ih, br, bg, bb, op, vp_w, vp_h);
    push_solid(ix + iw - 1, iy, ix + iw, iy + ih, br, bg, bb, op, vp_w, vp_h);

    /* Content area */
    int content_y = iy + VKW_TITLEBAR_H + 1;
    int content_h = ih - VKW_TITLEBAR_H - 1;
    if (w->has_input) content_h -= VKW_INPUT_H;
    int visible_lines = content_h / VKW_CHAR_H;
    int max_chars = (iw - VKW_PAD * 2) / VKW_CHAR_W;

    /* Draw text lines (bottom-up from most recent) */
    for (int i = 0; i < visible_lines && i < w->line_count; i++) {
        int line_idx = w->line_head - 1 - i - w->scroll;
        while (line_idx < 0) line_idx += VKW_MAX_LINES;
        line_idx %= VKW_MAX_LINES;
        if (i + w->scroll >= w->line_count) break;

        int ly = content_y + content_h - (i + 1) * VKW_CHAR_H;
        if (ly < content_y) break;

        /* Truncate display to window width */
        char tmp[VKW_LINE_LEN];
        strncpy(tmp, w->lines[line_idx], max_chars);
        tmp[max_chars] = 0;
        push_text(ix + VKW_PAD, ly, tmp,
                  t->text[0] * 0.9f, t->text[1] * 0.9f, t->text[2] * 0.9f,
                  vp_w, vp_h, VKW_CHAR_W, VKW_CHAR_H);
    }

    /* Input line */
    if (w->has_input) {
        int iy2 = iy + ih - VKW_INPUT_H;
        /* Input bg */
        push_solid(ix + 1, iy2, ix + iw - 1, iy + ih - 1,
                   t->bg[0] + 0.03f, t->bg[1] + 0.03f, t->bg[2] + 0.03f, op, vp_w, vp_h);
        /* Separator */
        push_solid(ix + 1, iy2, ix + iw - 1, iy2 + 1,
                   br * 0.5f, bg * 0.5f, bb * 0.5f, op, vp_w, vp_h);
        /* Prompt */
        push_text(ix + VKW_PAD, iy2 + (VKW_INPUT_H - VKW_CHAR_H) / 2,
                  ">>>", t->accent[0], t->accent[1], t->accent[2],
                  vp_w, vp_h, VKW_CHAR_W, VKW_CHAR_H);
        /* Input text */
        if (w->input_len > 0) {
            push_text(ix + VKW_PAD + VKW_CHAR_W * 4,
                      iy2 + (VKW_INPUT_H - VKW_CHAR_H) / 2,
                      w->input, t->text[0], t->text[1], t->text[2],
                      vp_w, vp_h, VKW_CHAR_W, VKW_CHAR_H);
        }
        /* Cursor */
        if (is_focused) {
            int cur_x = ix + VKW_PAD + VKW_CHAR_W * (4 + w->input_cursor);
            push_solid(cur_x, iy2 + 2, cur_x + VKW_CHAR_W, iy2 + VKW_INPUT_H - 2,
                       t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        }
    }
}

static void vkw_draw_all(int vp_w, int vp_h)
{
    /* Draw back-to-front in z-order */
    for (int i = 0; i < vkw_count; i++) {
        int idx = vkw_order[i];
        if (vkw_windows[idx].active) {
            vkw_draw_one(&vkw_windows[idx], (idx == vkw_focus), vp_w, vp_h);
        }
    }
}

/* ---- Window mouse handling ---- */

/* Returns window index at (mx, my), front-to-back. -1 = none */
static int vkw_hit_test(int mx, int my)
{
    for (int i = vkw_count - 1; i >= 0; i--) {
        int idx = vkw_order[i];
        vkw_window_t *w = &vkw_windows[idx];
        if (!w->active) continue;
        if (mx >= (int)w->x && mx < (int)(w->x + w->w) &&
            my >= (int)w->y && my < (int)(w->y + w->h))
            return idx;
    }
    return -1;
}

static int vkw_hit_close(vkw_window_t *w, int mx, int my)
{
    int cx = (int)w->x + (int)w->w - VKW_CLOSE_W - 2;
    int cy = (int)w->y + 2;
    return (mx >= cx && mx < cx + VKW_CLOSE_W &&
            my >= cy && my < cy + VKW_TITLEBAR_H - 4);
}

static int vkw_hit_titlebar(vkw_window_t *w, int mx, int my)
{
    return (mx >= (int)w->x && mx < (int)(w->x + w->w - VKW_CLOSE_W - 4) &&
            my >= (int)w->y && my < (int)(w->y + VKW_TITLEBAR_H));
}

static int vkw_hit_resize(vkw_window_t *w, int mx, int my)
{
    int rx = (int)(w->x + w->w) - VKW_RESIZE_ZONE;
    int ry = (int)(w->y + w->h) - VKW_RESIZE_ZONE;
    return (mx >= rx && my >= ry);
}

/* Handle mouse button down on window. Returns 1 if handled. */
static int vkw_mouse_down(int mx, int my)
{
    int idx = vkw_hit_test(mx, my);
    if (idx < 0) {
        vkw_focus = -1;  /* click on terminal = terminal gets focus */
        return 0;
    }

    vkw_window_t *w = &vkw_windows[idx];
    vkw_focus = idx;
    vkw_bring_to_front(idx);

    if (vkw_hit_close(w, mx, my)) {
        vkw_close(idx);
        return 1;
    }
    if (vkw_hit_resize(w, mx, my)) {
        w->resizing = 1;
        w->resize_ox = (float)mx;
        w->resize_oy = (float)my;
        w->resize_sw = w->w;
        w->resize_sh = w->h;
        return 1;
    }
    if (vkw_hit_titlebar(w, mx, my)) {
        w->dragging = 1;
        w->drag_ox = (float)mx - w->x;
        w->drag_oy = (float)my - w->y;
        return 1;
    }
    return 1; /* clicked on window content */
}

static int vkw_mouse_move(int mx, int my)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        vkw_window_t *w = &vkw_windows[i];
        if (!w->active) continue;
        if (w->dragging) {
            w->x = (float)mx - w->drag_ox;
            w->y = (float)my - w->drag_oy;
            return 1;
        }
        if (w->resizing) {
            w->w = w->resize_sw + ((float)mx - w->resize_ox);
            w->h = w->resize_sh + ((float)my - w->resize_oy);
            if (w->w < VKW_MIN_W) w->w = VKW_MIN_W;
            if (w->h < VKW_MIN_H) w->h = VKW_MIN_H;
            return 1;
        }
    }
    return 0;
}

static void vkw_mouse_up(void)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        vkw_windows[i].dragging = 0;
        vkw_windows[i].resizing = 0;
    }
}

/* Handle keyboard input for focused window. Returns 1 if consumed. */
static int vkw_key_char(unsigned int ch)
{
    if (vkw_focus < 0) return 0;
    vkw_window_t *w = &vkw_windows[vkw_focus];
    if (!w->active || !w->has_input) return 0;

    if (ch == '\r' || ch == '\n') {
        /* Submit input */
        w->input[w->input_len] = 0;
        /* Echo input as a line */
        char echo[VKW_LINE_LEN + 8];
        wsprintfA(echo, ">>> %s", w->input);
        vkw_print(vkw_focus, echo);
        /* Add to per-window history */
        if (w->input[0]) {
            if (w->hist_count < VKW_MAX_HISTORY) {
                w->cmd_hist[w->hist_count++] = _strdup(w->input);
            } else {
                free(w->cmd_hist[0]);
                memmove(w->cmd_hist, w->cmd_hist + 1, (VKW_MAX_HISTORY - 1) * sizeof(char *));
                w->cmd_hist[VKW_MAX_HISTORY - 1] = _strdup(w->input);
            }
            w->hist_idx = w->hist_count;
        }
        /* Console window → Python eval; other windows → callback */
        if (vkw_focus == vkw_console_idx && w->input[0]) {
            vkt_eval_python(w->input, w->id);
        } else if (vkw_input_callback) {
            vkw_input_callback(w->id, w->input);
        }
        w->input_len = 0;
        w->input_cursor = 0;
        w->input[0] = 0;
        return 1;
    }
    if (ch == '\b' || ch == 127) {
        if (w->input_cursor > 0) {
            memmove(&w->input[w->input_cursor - 1],
                    &w->input[w->input_cursor],
                    w->input_len - w->input_cursor + 1);
            w->input_cursor--;
            w->input_len--;
        }
        return 1;
    }
    if (ch >= 32 && ch < 127 && w->input_len < VKW_LINE_LEN - 1) {
        memmove(&w->input[w->input_cursor + 1],
                &w->input[w->input_cursor],
                w->input_len - w->input_cursor + 1);
        w->input[w->input_cursor] = (char)ch;
        w->input_cursor++;
        w->input_len++;
        w->input[w->input_len] = 0;
        return 1;
    }
    return 1;
}

static int vkw_key_down(unsigned int vk)
{
    if (vkw_focus < 0) return 0;
    vkw_window_t *w = &vkw_windows[vkw_focus];
    if (!w->active) return 0;

    /* Scroll with PageUp/PageDown */
    if (vk == VK_PRIOR) { /* PageUp */
        int content_h = (int)w->h - VKW_TITLEBAR_H - 1 - (w->has_input ? VKW_INPUT_H : 0);
        int page = content_h / VKW_CHAR_H;
        w->scroll += page;
        if (w->scroll > w->line_count - 1) w->scroll = w->line_count - 1;
        return 1;
    }
    if (vk == VK_NEXT) { /* PageDown */
        int content_h = (int)w->h - VKW_TITLEBAR_H - 1 - (w->has_input ? VKW_INPUT_H : 0);
        int page = content_h / VKW_CHAR_H;
        w->scroll -= page;
        if (w->scroll < 0) w->scroll = 0;
        return 1;
    }

    if (!w->has_input) return 0;

    /* Up/Down arrow = command history */
    if (vk == VK_UP) {
        if (w->hist_idx > 0) {
            w->hist_idx--;
            strncpy(w->input, w->cmd_hist[w->hist_idx], VKW_LINE_LEN - 1);
            w->input[VKW_LINE_LEN - 1] = 0;
            w->input_len = (int)strlen(w->input);
            w->input_cursor = w->input_len;
        }
        return 1;
    }
    if (vk == VK_DOWN) {
        if (w->hist_idx < w->hist_count - 1) {
            w->hist_idx++;
            strncpy(w->input, w->cmd_hist[w->hist_idx], VKW_LINE_LEN - 1);
            w->input[VKW_LINE_LEN - 1] = 0;
            w->input_len = (int)strlen(w->input);
            w->input_cursor = w->input_len;
        } else {
            w->hist_idx = w->hist_count;
            w->input[0] = 0;
            w->input_len = 0;
            w->input_cursor = 0;
        }
        return 1;
    }

    if (vk == VK_LEFT && w->input_cursor > 0) { w->input_cursor--; return 1; }
    if (vk == VK_RIGHT && w->input_cursor < w->input_len) { w->input_cursor++; return 1; }
    if (vk == VK_HOME) { w->input_cursor = 0; return 1; }
    if (vk == VK_END) { w->input_cursor = w->input_len; return 1; }
    return 0;
}

/* Toggle the built-in console window */
static void vkw_toggle_console(int vp_w, int vp_h)
{
    if (vkw_console_idx >= 0 && vkw_windows[vkw_console_idx].active) {
        vkw_close(vkw_console_idx);
        vkw_console_idx = -1;
        return;
    }
    int cw = 640, ch = 400;
    int cx = (vp_w - cw) / 2, cy = (vp_h - ch) / 2;
    vkw_console_idx = vkw_create("MMUDPy Console", cx, cy, cw, ch, 1);
    if (vkw_console_idx >= 0) {
        vkw_focus = vkw_console_idx;
        vkw_print(vkw_console_idx, "MMUDPy Python Console");
        vkw_print(vkw_console_idx, "Type Python expressions. help() for commands.");
        vkw_print(vkw_console_idx, "");
    }
}

/* ---- Exported functions for Python API ---- */

__declspec(dllexport) int vkt_wnd_create(const char *title, int x, int y, int w, int h, int has_input)
{
    int idx = vkw_create(title, x, y, w, h, has_input);
    return (idx >= 0) ? vkw_windows[idx].id : -1;
}

__declspec(dllexport) void vkt_wnd_print(int wnd_id, const char *text)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        if (vkw_windows[i].active && vkw_windows[i].id == wnd_id) {
            vkw_print(i, text);
            return;
        }
    }
}

__declspec(dllexport) void vkt_wnd_close(int wnd_id)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        if (vkw_windows[i].active && vkw_windows[i].id == wnd_id) {
            vkw_close(i);
            return;
        }
    }
}

__declspec(dllexport) void vkt_wnd_clear(int wnd_id)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        if (vkw_windows[i].active && vkw_windows[i].id == wnd_id) {
            vkw_clear(i);
            return;
        }
    }
}

__declspec(dllexport) void vkt_wnd_set_opacity(int wnd_id, float opacity)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        if (vkw_windows[i].active && vkw_windows[i].id == wnd_id) {
            vkw_windows[i].opacity = opacity;
            return;
        }
    }
}

__declspec(dllexport) void vkt_wnd_set_input_cb(vkw_input_cb_t cb)
{
    vkw_input_callback = cb;
}

__declspec(dllexport) void vkt_console_print(const char *text)
{
    if (vkw_console_idx >= 0) vkw_print(vkw_console_idx, text);
}

__declspec(dllexport) void vkt_console_show(void)
{
    if (vkw_console_idx < 0 || !vkw_windows[vkw_console_idx].active) {
        vkw_toggle_console(fs_width, fs_height);
    }
}

/* Terminal buffer */
/* ANSI terminal state machine — replaces old term_text/term_attr arrays */
static ap_term_t ansi_term;

/* Input state */
static char input_buf[INPUT_BUF_SZ];
static int input_len = 0;
static int input_cursor = 0;
static char *cmd_history[MAX_HISTORY];
static int history_count = 0;
static int history_idx = 0;
static int input_mode = 0;  /* 0 = split, 1 = raw */

/* Frame counter for cursor blink */
static int frame_count = 0;

/* ---- Vulkan state ---- */

static VkInstance vk_inst = VK_NULL_HANDLE;
static VkPhysicalDevice vk_pdev = VK_NULL_HANDLE;
static VkDevice vk_dev = VK_NULL_HANDLE;
static VkQueue vk_queue = VK_NULL_HANDLE;
static uint32_t vk_qfam = 0;

static VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
static VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;
static VkFormat vk_sc_fmt;
static VkExtent2D vk_sc_extent;
static uint32_t vk_sc_count = 0;
static VkImage *vk_sc_images = NULL;
static VkImageView *vk_sc_views = NULL;
static VkFramebuffer *vk_sc_fbs = NULL;

static VkRenderPass vk_renderpass = VK_NULL_HANDLE;
static VkPipelineLayout vk_pipe_layout = VK_NULL_HANDLE;
static VkPipeline vk_pipeline = VK_NULL_HANDLE;
static VkDescriptorSetLayout vk_desc_layout = VK_NULL_HANDLE;
static VkDescriptorPool vk_desc_pool = VK_NULL_HANDLE;
static VkDescriptorSet vk_desc_set = VK_NULL_HANDLE;

static VkCommandPool vk_cmd_pool = VK_NULL_HANDLE;
static VkCommandBuffer vk_cmd_buf = VK_NULL_HANDLE;
static VkSemaphore vk_sem_avail = VK_NULL_HANDLE;
static VkSemaphore vk_sem_done = VK_NULL_HANDLE;
static VkFence vk_fence = VK_NULL_HANDLE;

/* Font texture */
static VkImage vk_font_img = VK_NULL_HANDLE;
static VkDeviceMemory vk_font_mem = VK_NULL_HANDLE;
static VkImageView vk_font_view = VK_NULL_HANDLE;
static VkSampler vk_font_sampler = VK_NULL_HANDLE;

/* Vertex + index buffers */
static VkBuffer vk_vbuf = VK_NULL_HANDLE;
static VkDeviceMemory vk_vmem = VK_NULL_HANDLE;
static VkBuffer vk_ibuf = VK_NULL_HANDLE;
static VkDeviceMemory vk_imem = VK_NULL_HANDLE;
static vertex_t *vk_vdata = NULL;  /* persistently mapped */
static int quad_count = 0;

/* ---- Forward declarations ---- */

static void vkt_destroy_swapchain(void);
static int  vkt_create_swapchain(void);
static void vkt_cleanup_vulkan(void);
void vkt_show(void);
void vkt_hide(void);
void vkt_toggle(void);
static void vkt_screenshot(uint32_t img_idx);

/* ---- Helpers ---- */

static uint32_t vk_find_memory(VkPhysicalDevice pd, uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/* ---- ANSI Terminal — uses ansi_parser.h state machine ---- */

static CRITICAL_SECTION ansi_lock;

/* on_data callback — raw server bytes fed to state machine */
static void vkt_on_data(const char *data, int len)
{
    EnterCriticalSection(&ansi_lock);
    ap_feed(&ansi_term, (const uint8_t *)data, len);
    LeaveCriticalSection(&ansi_lock);
}

/* Read buffer for renderer — snapshot under lock */
static void vkt_read_buffer(void)
{
    EnterCriticalSection(&ansi_lock);
    LeaveCriticalSection(&ansi_lock);
}

/* ---- Vertex building ---- */

static void push_quad(float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a)
{
    if (quad_count >= MAX_QUADS) return;
    int vi = quad_count * 4;
    /* NDC: x in [-1,1], y in [-1,1] */
    vk_vdata[vi+0] = (vertex_t){ x0, y0, u0, v0, r, g, b, a };
    vk_vdata[vi+1] = (vertex_t){ x1, y0, u1, v0, r, g, b, a };
    vk_vdata[vi+2] = (vertex_t){ x1, y1, u1, v1, r, g, b, a };
    vk_vdata[vi+3] = (vertex_t){ x0, y1, u0, v1, r, g, b, a };
    quad_count++;
}

/* Push a solid filled rect (uses glyph 219 █ for solid pixels) */
static void push_solid(float x0, float y0, float x1, float y1,
                       float r, float g, float b, float a,
                       int vp_w, int vp_h)
{
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    float su0 = (219 % 16) * tex_cw, sv0 = (219 / 16) * tex_ch;
    float su1 = su0 + tex_cw, sv1 = sv0 + tex_ch;
    #define S2X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define S2Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)
    push_quad(S2X(x0), S2Y(y0), S2X(x1), S2Y(y1),
              su0, sv0, su1, sv1, r, g, b, a);
    #undef S2X
    #undef S2Y
}

/* Push a text string at pixel position */
static void push_text(int px, int py, const char *str,
                      float r, float g, float b,
                      int vp_w, int vp_h, int char_w, int char_h)
{
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    #define T2X(p) (((float)(p) / (float)vp_w) * 2.0f - 1.0f)
    #define T2Y(p) (((float)(p) / (float)vp_h) * 2.0f - 1.0f)
    for (int i = 0; str[i]; i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch <= 32) { px += char_w; continue; }
        float u0 = (ch % 16) * tex_cw, v0 = (ch / 16) * tex_ch;
        push_quad(T2X(px), T2Y(py), T2X(px + char_w), T2Y(py + char_h),
                  u0, v0, u0 + tex_cw, v0 + tex_ch, r, g, b, 1.0f);
        px += char_w;
    }
    #undef T2X
    #undef T2Y
}

/* ---- Vulkan menu rendering ---- */

static int vkm_is_sep(int idx) { return idx == VKM_ITEM_SEP || idx == VKM_ITEM_SEP2; }

static void vkm_get_root_item_rect(int idx, int *iy, int *ih)
{
    int y = vkm_y + VKM_PAD;
    for (int i = 0; i < idx; i++) {
        y += vkm_is_sep(i) ? VKM_SEP_H : VKM_ITEM_H;
    }
    *iy = y;
    *ih = vkm_is_sep(idx) ? VKM_SEP_H : VKM_ITEM_H;
}

static int vkm_root_height(void)
{
    int h = VKM_PAD * 2;
    for (int i = 0; i < VKM_ROOT_COUNT; i++)
        h += vkm_is_sep(i) ? VKM_SEP_H : VKM_ITEM_H;
    return h;
}

static int vkm_sub_count(void)
{
    if (vkm_sub == VKM_SUB_THEME) return NUM_THEMES;
    if (vkm_sub == VKM_SUB_FONT) return NUM_FONTS + 1; /* +1 for bitmap */
    return 0;
}

static int vkm_sub_height(void)
{
    return VKM_PAD * 2 + vkm_sub_count() * VKM_ITEM_H;
}

/* Hit test: returns root item index or -1 */
static int vkm_hit_root(int mx, int my)
{
    if (mx < vkm_x || mx >= vkm_x + VKM_ROOT_W) return -1;
    for (int i = 0; i < VKM_ROOT_COUNT; i++) {
        int iy, ih;
        vkm_get_root_item_rect(i, &iy, &ih);
        if (my >= iy && my < iy + ih && !vkm_is_sep(i)) return i;
    }
    return -1;
}

/* Hit test submenu: returns item index or -1 */
static int vkm_hit_sub(int mx, int my)
{
    if (vkm_sub == VKM_SUB_NONE) return -1;
    int sx = vkm_x + VKM_ROOT_W;
    int sy_item;
    /* submenu y = aligned to the parent item */
    int parent = (vkm_sub == VKM_SUB_THEME) ? VKM_ITEM_THEME : VKM_ITEM_FONT;
    int parent_y, parent_h;
    vkm_get_root_item_rect(parent, &parent_y, &parent_h);
    int sy = parent_y;

    if (mx < sx || mx >= sx + VKM_SUB_W) return -1;
    int count = vkm_sub_count();
    for (int i = 0; i < count; i++) {
        sy_item = sy + VKM_PAD + i * VKM_ITEM_H;
        if (my >= sy_item && my < sy_item + VKM_ITEM_H) return i;
    }
    return -1;
}

/* Draw a bordered panel with drop shadow */
static void vkm_draw_panel(int x, int y, int w, int h,
                           const ui_theme_t *t, int vp_w, int vp_h)
{
    /* Drop shadow */
    push_solid(x + VKM_SHADOW, y + VKM_SHADOW,
               x + w + VKM_SHADOW, y + h + VKM_SHADOW,
               0.0f, 0.0f, 0.0f, 0.6f, vp_w, vp_h);
    /* Background */
    push_solid(x, y, x + w, y + h,
               t->bg[0], t->bg[1], t->bg[2], 1.0f, vp_w, vp_h);
    /* Border — derive from accent at ~35% blend with bg */
    float br = t->bg[0] * 0.6f + t->accent[0] * 0.4f;
    float bg = t->bg[1] * 0.6f + t->accent[1] * 0.4f;
    float bb = t->bg[2] * 0.6f + t->accent[2] * 0.4f;
    push_solid(x, y, x + w, y + 1, br, bg, bb, 1.0f, vp_w, vp_h);
    push_solid(x, y + h - 1, x + w, y + h, br, bg, bb, 1.0f, vp_w, vp_h);
    push_solid(x, y, x + 1, y + h, br, bg, bb, 1.0f, vp_w, vp_h);
    push_solid(x + w - 1, y, x + w, y + h, br, bg, bb, 1.0f, vp_w, vp_h);
    /* Top accent line — bright accent at top edge */
    push_solid(x + 1, y + 1, x + w - 1, y + 2,
               t->accent[0] * 0.7f, t->accent[1] * 0.7f, t->accent[2] * 0.7f,
               0.5f, vp_w, vp_h);
}

static void vkm_draw(int vp_w, int vp_h)
{
    if (!vkm_open) return;

    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = VKM_CHAR_W, ch = VKM_CHAR_H;

    /* Scrim: dim the entire screen behind menu */
    push_solid(0, 0, vp_w, vp_h,
               0.0f, 0.0f, 0.0f, 0.35f, vp_w, vp_h);

    /* Root menu panel */
    int rh = vkm_root_height();
    vkm_draw_panel(vkm_x, vkm_y, VKM_ROOT_W, rh, t, vp_w, vp_h);

    /* Root menu items */
    static const char *root_labels[VKM_ROOT_COUNT] = {
        "Theme  \x10", "Font  \x10", "", "Console", "", "Close  (F11)"
    };

    /* Hover bg colors: accent-tinted lift */
    float hvr = t->bg[0] + (t->accent[0] - t->bg[0]) * 0.25f + 0.06f;
    float hvg = t->bg[1] + (t->accent[1] - t->bg[1]) * 0.25f + 0.06f;
    float hvb = t->bg[2] + (t->accent[2] - t->bg[2]) * 0.25f + 0.06f;

    for (int i = 0; i < VKM_ROOT_COUNT; i++) {
        int iy, ih;
        vkm_get_root_item_rect(i, &iy, &ih);

        if (vkm_is_sep(i)) {
            float sr = t->bg[0] * 0.6f + t->accent[0] * 0.4f;
            float sg = t->bg[1] * 0.6f + t->accent[1] * 0.4f;
            float sb = t->bg[2] * 0.6f + t->accent[2] * 0.4f;
            push_solid(vkm_x + VKM_PAD, iy + ih/2,
                       vkm_x + VKM_ROOT_W - VKM_PAD, iy + ih/2 + 1,
                       sr, sg, sb, 0.5f, vp_w, vp_h);
            continue;
        }

        /* Hover highlight */
        if (i == vkm_hover) {
            push_solid(vkm_x + 2, iy + 1, vkm_x + VKM_ROOT_W - 2, iy + ih - 1,
                       hvr, hvg, hvb, 1.0f, vp_w, vp_h);
        }

        /* Text color: accent when expanded, normal otherwise */
        float tr = t->text[0], tg = t->text[1], tb = t->text[2];
        if ((i == VKM_ITEM_THEME && vkm_sub == VKM_SUB_THEME) ||
            (i == VKM_ITEM_FONT && vkm_sub == VKM_SUB_FONT)) {
            tr = t->accent[0]; tg = t->accent[1]; tb = t->accent[2];
        }
        push_text(vkm_x + VKM_PAD, iy + (ih - ch) / 2,
                  root_labels[i], tr, tg, tb, vp_w, vp_h, cw, ch);
    }

    /* Submenu */
    if (vkm_sub != VKM_SUB_NONE) {
        int parent = (vkm_sub == VKM_SUB_THEME) ? VKM_ITEM_THEME : VKM_ITEM_FONT;
        int parent_y, parent_h;
        vkm_get_root_item_rect(parent, &parent_y, &parent_h);

        int sx = vkm_x + VKM_ROOT_W - 1; /* overlap border by 1px */
        int sy = parent_y;
        int count = vkm_sub_count();
        int sh = vkm_sub_height();

        /* Clamp submenu to screen */
        if (sy + sh > vp_h) sy = vp_h - sh;
        if (sx + VKM_SUB_W > vp_w) sx = vkm_x - VKM_SUB_W + 1;

        vkm_draw_panel(sx, sy, VKM_SUB_W, sh, t, vp_w, vp_h);

        for (int i = 0; i < count; i++) {
            int iy2 = sy + VKM_PAD + i * VKM_ITEM_H;

            /* Hover highlight */
            if (i == vkm_sub_hover) {
                push_solid(sx + 2, iy2 + 1, sx + VKM_SUB_W - 2, iy2 + VKM_ITEM_H - 1,
                           hvr, hvg, hvb, 1.0f, vp_w, vp_h);
            }

            const char *label = NULL;
            int is_active = 0;

            if (vkm_sub == VKM_SUB_THEME) {
                label = ui_themes[i].name;
                is_active = (i == current_theme);
            } else {
                if (i == 0) {
                    label = "CP437 Bitmap (Original)";
                    is_active = (current_font < 0);
                } else {
                    label = font_table[i - 1].name;
                    is_active = (current_font == i - 1);
                }
            }

            float tr, tg, tb;
            if (is_active) {
                tr = t->accent[0]; tg = t->accent[1]; tb = t->accent[2];
            } else {
                tr = t->text[0]; tg = t->text[1]; tb = t->text[2];
            }

            if (label) {
                /* Active marker: small filled square before active item */
                if (is_active) {
                    push_text(sx + 3, iy2 + (VKM_ITEM_H - ch) / 2,
                              "\x04", tr, tg, tb, vp_w, vp_h, cw, ch);
                }
                push_text(sx + VKM_PAD + cw + 2, iy2 + (VKM_ITEM_H - ch) / 2,
                          label, tr, tg, tb, vp_w, vp_h, cw, ch);
            }
        }
    }
}

static void vkt_build_vertices(void)
{
    quad_count = 0;
    if (!palette || !vk_vdata) return;

    int vp_w = (int)vk_sc_extent.width;
    int vp_h = (int)vk_sc_extent.height;
    int top_pad = 4;
    int bot_pad = 4;
    int term_h = vp_h - INPUT_BAR_H - top_pad - bot_pad;

    /* Maintain 1:2 (w:h) CP437 aspect ratio per character.
     * Scale uniformly to fit, center horizontally with black bars. */
    float cw_fill = (float)vp_w / (float)TERM_COLS;
    float ch_fill = (float)term_h / (float)TERM_ROWS;
    float cw, ch;
    /* Pick scale that maintains 1:2 aspect and fits */
    if (cw_fill * 2.0f <= ch_fill) {
        /* Width-limited: chars fit by width, won't overflow height */
        cw = cw_fill;
        ch = cw * 2.0f;
    } else {
        /* Height-limited: chars fit by height */
        ch = ch_fill;
        cw = ch / 2.0f;
    }
    float grid_w = cw * TERM_COLS;
    float x_offset = ((float)vp_w - grid_w) / 2.0f; /* center horizontally */

    /* UV constants for font atlas (512x1024, 32x64 glyphs in 16x16 grid) */
    float tex_cw = 1.0f / 16.0f;
    float tex_ch = 1.0f / 16.0f;

    /* Helper: pixel to NDC */
    #define PX2NDC_X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define PX2NDC_Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)

    /* Pass 1: background colors */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            int bg = ansi_term.grid[r][c].attr.bg & 0x07;
            if (bg == 0) continue;
            float px0 = c * cw, py0 = r * ch;
            float px1 = px0 + cw, py1 = py0 + ch;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      0, 0, 0, 0, /* no texture for bg */
                      palette[bg].r, palette[bg].g, palette[bg].b, 0.0f);
            /* alpha=0 so fragment shader: texAlpha=texture(0,0).a, output alpha=0*texAlpha=0
               Hmm, that won't work. Need a different approach for bg vs text. */
        }
    }

    /* Actually, for bg quads we need alpha=1 and a white pixel in the atlas.
       The fragment shader does: outColor = vec4(color.rgb, color.a * texAlpha).
       For bg quads, if we use UV (0,0) which is glyph 0 (null) — its alpha is 0.

       Better approach: use a special region of the atlas that's solid white.
       Or: modify the shader to handle this differently.

       Simplest: glyph 219 (█ full block) has all bits set → all alpha=1.
       Use its UVs for background quads. */

    /* Re-do: reset and use glyph 219 UVs for bg */
    quad_count = 0;
    float bg_u0 = (219 % 16) * tex_cw;
    float bg_v0 = (219 / 16) * tex_ch;
    float bg_u1 = bg_u0 + tex_cw;
    float bg_v1 = bg_v0 + tex_ch;

    /* Pass 1: backgrounds using solid glyph */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            int bg = ansi_term.grid[r][c].attr.bg & 0x07;
            if (bg == 0) continue;
            float px0 = x_offset + c * cw, py0 = top_pad + r * ch;
            float px1 = px0 + cw, py1 = py0 + ch;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      palette[bg].r, palette[bg].g, palette[bg].b, 1.0f);
        }
    }

    /* Pass 2: text characters */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            unsigned char byte = ansi_term.grid[r][c].ch;
            if (byte == 0 || byte == 32) continue;
            ap_attr_t *a = &ansi_term.grid[r][c].attr;
            int fg = (a->fg & 0x07) | (a->bold ? 0x08 : 0);
            float u0 = (byte % 16) * tex_cw;
            float v0 = (byte / 16) * tex_ch;
            float u1 = u0 + tex_cw;
            float v1 = v0 + tex_ch;
            float px0 = x_offset + c * cw, py0 = top_pad + r * ch;
            float px1 = px0 + cw, py1 = py0 + ch;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      u0, v0, u1, v1,
                      palette[fg].r, palette[fg].g, palette[fg].b, 1.0f);
        }
    }

    /* Pass 3: input bar background — tinted with UI theme */
    {
        const ui_theme_t *t = &ui_themes[current_theme];
        float bar_y0 = (float)(top_pad + term_h + bot_pad);
        push_quad(PX2NDC_X(0), PX2NDC_Y(bar_y0), PX2NDC_X(vp_w), PX2NDC_Y(vp_h),
                  bg_u0, bg_v0, bg_u1, bg_v1,
                  t->bg[0] + 0.04f, t->bg[1] + 0.04f, t->bg[2] + 0.04f, 1.0f);
    }

    /* Pass 4: input bar text */
    {
        float ibar_cw = cw;  /* same char width as terminal */
        float ibar_ch = (float)INPUT_BAR_H;
        float bar_y0 = (float)(top_pad + term_h + bot_pad);
        for (int i = 0; i < input_len && i < TERM_COLS; i++) {
            unsigned char ch = (unsigned char)input_buf[i];
            if (ch <= 32) continue;
            float u0 = (ch % 16) * tex_cw;
            float v0 = (ch / 16) * tex_ch;
            float u1 = u0 + tex_cw;
            float v1 = v0 + tex_ch;
            float px0 = x_offset + i * ibar_cw, py0 = bar_y0 + 2;
            float px1 = px0 + ibar_cw, py1 = bar_y0 + ibar_ch - 2;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      u0, v0, u1, v1,
                      1.0f, 1.0f, 1.0f, 1.0f);
        }

        /* Blinking cursor */
        if ((frame_count / 30) % 2 == 0) {
            float cx0 = x_offset + input_cursor * ibar_cw;
            float cy0 = bar_y0 + ibar_ch - 4;
            float cx1 = cx0 + ibar_cw;
            float cy1 = bar_y0 + ibar_ch - 1;
            push_quad(PX2NDC_X(cx0), PX2NDC_Y(cy0), PX2NDC_X(cx1), PX2NDC_Y(cy1),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      0.7f, 0.7f, 0.7f, 1.0f);
        }
    }

    #undef PX2NDC_X
    #undef PX2NDC_Y

    /* Draw Vulkan menu on top of everything */
    vkm_draw(vp_w, vp_h);

    /* Draw floating windows on top of everything */
    vkw_draw_all(vp_w, vp_h);
}

/* ---- Input handling ---- */

static void input_send(void)
{
    if (!mmansi_hwnd) return;

    /* Empty enter = just send CR (MajorMUD uses this for continue/refresh) */
    if (!input_buf[0]) {
        PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)'\r', 0);
        return;
    }

    /* Add to history */
    if (history_count < MAX_HISTORY) {
        cmd_history[history_count++] = _strdup(input_buf);
    } else {
        free(cmd_history[0]);
        memmove(cmd_history, cmd_history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        cmd_history[MAX_HISTORY - 1] = _strdup(input_buf);
    }
    history_idx = history_count;

    /* `` prefix = Python eval via MMUDPy — output goes to terminal (not console) */
    if (input_buf[0] == '`' && input_buf[1] == '`') {
        const char *code = input_buf + 2;
        while (*code == ' ') code++;  /* skip leading spaces */
        if (*code) {
            vkt_eval_python(code, 0);  /* target 0 = terminal */
        }
        input_buf[0] = '\0';
        input_len = 0;
        input_cursor = 0;
        return;
    }

    /* Send to MMANSI */
    for (int i = 0; input_buf[i]; i++)
        PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)(unsigned char)input_buf[i], 0);
    PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)'\r', 0);

    input_buf[0] = '\0';
    input_len = 0;
    input_cursor = 0;
}

static void input_handle_key(WPARAM vk, int is_char)
{
    if (is_char) {
        if (vk >= 32 && vk < 127 && input_len < INPUT_BUF_SZ - 1) {
            /* Insert at cursor */
            memmove(input_buf + input_cursor + 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_buf[input_cursor] = (char)vk;
            input_len++;
            input_cursor++;
        }
        return;
    }

    /* Key down */
    switch (vk) {
    case VK_RETURN:
        input_send();
        break;
    case VK_BACK:
        if (input_cursor > 0) {
            memmove(input_buf + input_cursor - 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_cursor--;
            input_len--;
        }
        break;
    case VK_DELETE:
        if (input_cursor < input_len) {
            memmove(input_buf + input_cursor, input_buf + input_cursor + 1,
                    input_len - input_cursor);
            input_len--;
        }
        break;
    case VK_LEFT:
        if (input_cursor > 0) input_cursor--;
        break;
    case VK_RIGHT:
        if (input_cursor < input_len) input_cursor++;
        break;
    case VK_HOME:
        input_cursor = 0;
        break;
    case VK_END:
        input_cursor = input_len;
        break;
    case VK_UP:
        if (history_idx > 0) {
            history_idx--;
            strncpy(input_buf, cmd_history[history_idx], INPUT_BUF_SZ - 1);
            input_len = (int)strlen(input_buf);
            input_cursor = input_len;
        }
        break;
    case VK_DOWN:
        if (history_idx < history_count - 1) {
            history_idx++;
            strncpy(input_buf, cmd_history[history_idx], INPUT_BUF_SZ - 1);
            input_len = (int)strlen(input_buf);
            input_cursor = input_len;
        } else {
            history_idx = history_count;
            input_buf[0] = '\0';
            input_len = 0;
            input_cursor = 0;
        }
        break;
    case VK_ESCAPE:
        input_buf[0] = '\0';
        input_len = 0;
        input_cursor = 0;
        history_idx = history_count;
        break;
    case VK_F11:
        vkt_hide();
        break;
    case VK_F12:
        vkt_screenshot_pending = 1;
        break;
    }
}

/* ---- Screenshot (F12) ---- */

static uint32_t vkt_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_pdev, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static void vkt_screenshot(uint32_t img_idx)
{
    if (!vk_dev || !vk_swapchain || !vk_sc_images) return;

    uint32_t w = vk_sc_extent.width;
    uint32_t h = vk_sc_extent.height;

    /* Wait for any pending rendering */
    vkDeviceWaitIdle(vk_dev);

    /* Create host-visible buffer to copy swapchain image into */
    VkDeviceSize buf_size = w * h * 4;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = buf_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(vk_dev, &bci, NULL, &staging_buf) != VK_SUCCESS) return;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(vk_dev, staging_buf, &mem_req);

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex = vkt_find_memory_type(mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(vk_dev, &mai, NULL, &staging_mem) != VK_SUCCESS) {
        vkDestroyBuffer(vk_dev, staging_buf, NULL);
        return;
    }
    vkBindBufferMemory(vk_dev, staging_buf, staging_mem, 0);

    /* Record copy command: swapchain image[0] → staging buffer */
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = vk_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk_dev, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    /* Transition image to TRANSFER_SRC */
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk_sc_images[img_idx];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = w;
    region.imageExtent.height = h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, vk_sc_images[img_idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_buf, 1, &region);

    /* Transition back to PRESENT_SRC */
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_queue);
    vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &cmd);

    /* Map and write BMP */
    void *mapped = NULL;
    vkMapMemory(vk_dev, staging_mem, 0, buf_size, 0, &mapped);
    if (mapped) {
        unsigned char *src = (unsigned char *)mapped;
        int row_bytes = ((w * 3 + 3) & ~3);
        int img_size = row_bytes * h;
        unsigned char *bmp_data = (unsigned char *)malloc(img_size);
        if (bmp_data) {
            /* Convert BGRA → BGR, flip vertically for BMP bottom-up */
            for (uint32_t y = 0; y < h; y++) {
                unsigned char *dst_row = bmp_data + (h - 1 - y) * row_bytes;
                unsigned char *src_row = src + y * w * 4;
                for (uint32_t x = 0; x < w; x++) {
                    dst_row[x * 3 + 0] = src_row[x * 4 + 0]; /* B */
                    dst_row[x * 3 + 1] = src_row[x * 4 + 1]; /* G */
                    dst_row[x * 3 + 2] = src_row[x * 4 + 2]; /* R */
                }
            }

            char fname[128];
            SYSTEMTIME st;
            GetLocalTime(&st);
            sprintf(fname, "/tmp/vk_screenshot_%04d%02d%02d_%02d%02d%02d_%03d.bmp",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            FILE *fp = fopen(fname, "wb");
            if (fp) {
                /* BITMAPFILEHEADER */
                uint16_t bfType = 0x4D42;
                uint32_t bfSize = 14 + 40 + img_size;
                uint16_t bfReserved = 0;
                uint32_t bfOffBits = 14 + 40;
                fwrite(&bfType, 2, 1, fp);
                fwrite(&bfSize, 4, 1, fp);
                fwrite(&bfReserved, 2, 1, fp);
                fwrite(&bfReserved, 2, 1, fp);
                fwrite(&bfOffBits, 4, 1, fp);
                /* BITMAPINFOHEADER */
                uint32_t biSize = 40;
                int32_t biWidth = (int32_t)w;
                int32_t biHeight = (int32_t)h; /* bottom-up */
                uint16_t biPlanes = 1;
                uint16_t biBitCount = 24;
                uint32_t biCompression = 0;
                uint32_t biSizeImage = img_size;
                int32_t biXPels = 0, biYPels = 0;
                uint32_t biClrUsed = 0, biClrImportant = 0;
                fwrite(&biSize, 4, 1, fp);
                fwrite(&biWidth, 4, 1, fp);
                fwrite(&biHeight, 4, 1, fp);
                fwrite(&biPlanes, 2, 1, fp);
                fwrite(&biBitCount, 2, 1, fp);
                fwrite(&biCompression, 4, 1, fp);
                fwrite(&biSizeImage, 4, 1, fp);
                fwrite(&biXPels, 4, 1, fp);
                fwrite(&biYPels, 4, 1, fp);
                fwrite(&biClrUsed, 4, 1, fp);
                fwrite(&biClrImportant, 4, 1, fp);
                fwrite(bmp_data, img_size, 1, fp);
                fclose(fp);
                if (api) api->log("[vk_terminal] Screenshot saved: %s (%dx%d)\n", fname, w, h);
            }
            free(bmp_data);
        }
        vkUnmapMemory(vk_dev, staging_mem);
    }

    vkDestroyBuffer(vk_dev, staging_buf, NULL);
    vkFreeMemory(vk_dev, staging_mem, NULL);
}

/* ---- Vulkan initialization ---- */

static int vkt_init_vulkan(void)
{
    VkResult res;

    /* Instance */
    const char *inst_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VkTerminal";
    app.apiVersion = VK_API_VERSION_1_0;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    res = vkCreateInstance(&ici, NULL, &vk_inst);
    if (res != VK_SUCCESS) {
        api->log("[vk_terminal] vkCreateInstance failed: %d\n", res);
        return -1;
    }

    /* Physical device */
    uint32_t pdc = 0;
    vkEnumeratePhysicalDevices(vk_inst, &pdc, NULL);
    if (pdc == 0) { api->log("[vk_terminal] No Vulkan devices\n"); return -1; }
    VkPhysicalDevice *pdevs = (VkPhysicalDevice *)malloc(pdc * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(vk_inst, &pdc, pdevs);
    vk_pdev = pdevs[0];
    free(pdevs);

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(vk_pdev, &pdp);
    api->log("[vk_terminal] GPU: %s\n", pdp.deviceName);

    /* Find graphics queue family */
    uint32_t qfc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_pdev, &qfc, NULL);
    VkQueueFamilyProperties *qfp = (VkQueueFamilyProperties *)malloc(qfc * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(vk_pdev, &qfc, qfp);
    vk_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qfc; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk_qfam = i; break; }
    }
    free(qfp);
    if (vk_qfam == UINT32_MAX) { api->log("[vk_terminal] No graphics queue\n"); return -1; }

    /* Logical device */
    float qpri = 1.0f;
    VkDeviceQueueCreateInfo dqci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    dqci.queueFamilyIndex = vk_qfam;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &qpri;
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    res = vkCreateDevice(vk_pdev, &dci, NULL, &vk_dev);
    if (res != VK_SUCCESS) { api->log("[vk_terminal] vkCreateDevice failed: %d\n", res); return -1; }
    vkGetDeviceQueue(vk_dev, vk_qfam, 0, &vk_queue);

    /* Command pool */
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = vk_qfam;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(vk_dev, &cpci, NULL, &vk_cmd_pool);

    /* Command buffer */
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = vk_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk_dev, &cbai, &vk_cmd_buf);

    /* Sync objects */
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(vk_dev, &sci, NULL, &vk_sem_avail);
    vkCreateSemaphore(vk_dev, &sci, NULL, &vk_sem_done);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vk_dev, &fci, NULL, &vk_fence);

    api->log("[vk_terminal] Vulkan init OK\n");
    return 0;
}

/* ---- Font texture ---- */

/* Build RGBA pixels from either the original bitmap or a TTF atlas.
 * font_idx: -1 = original cp437 bitmap (4x upscale), 0..NUM_FONTS-1 = TTF atlas.
 * Caller must free() the returned buffer. Sets *out_w, *out_h. */
static uint8_t *vkt_build_font_pixels(int font_idx, uint32_t *out_w, uint32_t *out_h)
{
    if (font_idx < 0 || font_idx >= NUM_FONTS) {
        /* Original 8x16 bitmap, 4x nearest-neighbor upscale */
        #define FONT_SCALE 4
        uint32_t aw = 128 * FONT_SCALE, ah = 256 * FONT_SCALE;
        uint8_t *pixels = (uint8_t *)calloc(aw * ah * 4, 1);
        for (int g = 0; g < 256; g++) {
            int gx = (g % 16) * 8, gy = (g / 16) * 16;
            for (int row = 0; row < 16; row++) {
                uint8_t bits = cp437_bitmap[g][row];
                for (int col = 0; col < 8; col++) {
                    if (bits & (0x80 >> col)) {
                        for (int sy = 0; sy < FONT_SCALE; sy++)
                            for (int sx = 0; sx < FONT_SCALE; sx++) {
                                int px = (gx + col) * FONT_SCALE + sx;
                                int py = (gy + row) * FONT_SCALE + sy;
                                int idx = (py * (int)aw + px) * 4;
                                pixels[idx] = pixels[idx+1] = pixels[idx+2] = pixels[idx+3] = 255;
                            }
                    }
                }
            }
        }
        *out_w = aw; *out_h = ah;
        return pixels;
        #undef FONT_SCALE
    }

    /* TTF atlas: single-channel alpha → RGBA (white + alpha).
     * Threshold low alpha values to prevent bleed between cells
     * when bilinear filtering samples neighboring glyph edges. */
    uint32_t aw = FONT_ATLAS_W, ah = FONT_ATLAS_H;
    const unsigned char *src = font_table[font_idx].data;
    uint8_t *pixels = (uint8_t *)calloc(aw * ah * 4, 1);
    for (uint32_t i = 0; i < aw * ah; i++) {
        uint8_t a = src[i];
        if (a < 32) a = 0;          /* kill sub-pixel bleed */
        else if (a > 224) a = 255;   /* sharpen near-solid */
        pixels[i*4+0] = 255;
        pixels[i*4+1] = 255;
        pixels[i*4+2] = 255;
        pixels[i*4+3] = a;
    }
    *out_w = aw; *out_h = ah;
    return pixels;
}

/* Upload RGBA pixel data to a new Vulkan font texture.
 * Destroys existing font texture resources first. */
static int vkt_upload_font_texture(uint8_t *pixels, uint32_t atlas_w, uint32_t atlas_h)
{
    /* Tear down old texture */
    if (vk_dev) vkDeviceWaitIdle(vk_dev);
    if (vk_font_sampler) { vkDestroySampler(vk_dev, vk_font_sampler, NULL); vk_font_sampler = VK_NULL_HANDLE; }
    if (vk_font_view) { vkDestroyImageView(vk_dev, vk_font_view, NULL); vk_font_view = VK_NULL_HANDLE; }
    if (vk_font_img) { vkDestroyImage(vk_dev, vk_font_img, NULL); vk_font_img = VK_NULL_HANDLE; }
    if (vk_font_mem) { vkFreeMemory(vk_dev, vk_font_mem, NULL); vk_font_mem = VK_NULL_HANDLE; }

    /* Create VkImage */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ atlas_w, atlas_h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    vkCreateImage(vk_dev, &ici, NULL, &vk_font_img);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_dev, vk_font_img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_find_memory(vk_pdev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &mai, NULL, &vk_font_mem);
    vkBindImageMemory(vk_dev, vk_font_img, vk_font_mem, 0);

    /* Copy pixel data with proper row pitch */
    void *mapped;
    vkMapMemory(vk_dev, vk_font_mem, 0, mr.size, 0, &mapped);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk_dev, vk_font_img, &sub, &layout);
    for (uint32_t row = 0; row < atlas_h; row++) {
        memcpy((uint8_t *)mapped + layout.offset + row * layout.rowPitch,
               pixels + row * atlas_w * 4, atlas_w * 4);
    }
    vkUnmapMemory(vk_dev, vk_font_mem);

    /* Transition to SHADER_READ_ONLY_OPTIMAL using a one-shot command buffer.
     * We must NOT touch the render loop's vk_cmd_buf or vk_fence here,
     * because that corrupts the render loop's sync state. */
    VkCommandBuffer tmp_cmd;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = vk_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk_dev, &cbai, &tmp_cmd);

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(tmp_cmd, &cbbi);
    VkImageMemoryBarrier imb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    imb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.image = vk_font_img;
    imb.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(tmp_cmd, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &imb);
    vkEndCommandBuffer(tmp_cmd);

    VkFence tmp_fence;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(vk_dev, &fci, NULL, &tmp_fence);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &tmp_cmd;
    vkQueueSubmit(vk_queue, 1, &si, tmp_fence);
    vkWaitForFences(vk_dev, 1, &tmp_fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(vk_dev, tmp_fence, NULL);
    vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &tmp_cmd);

    /* Image view */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = vk_font_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vk_dev, &ivci, NULL, &vk_font_view);

    /* Sampler — NEAREST for bitmap, LINEAR for TTF antialiased */
    VkSamplerCreateInfo saci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    saci.magFilter = (current_font < 0) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    saci.minFilter = (current_font < 0) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_dev, &saci, NULL, &vk_font_sampler);

    /* Update descriptor set to point to new texture */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii.imageView = vk_font_view;
    dii.sampler = vk_font_sampler;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet = vk_desc_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);

    return 0;
}

static int vkt_create_font_texture(void)
{
    uint32_t aw, ah;
    uint8_t *pixels = vkt_build_font_pixels(current_font, &aw, &ah);
    int ret = vkt_upload_font_texture(pixels, aw, ah);
    free(pixels);

    const char *fname = (current_font < 0) ? "CP437 Bitmap 4x"
                       : font_table[current_font].name;
    api->log("[vk_terminal] Font: %s (%dx%d)\n", fname, aw, ah);
    return ret;
}

/* Switch font at runtime — called ONLY from inside vkt_render_frame
 * after the fence wait, so previous frame is guaranteed complete.
 * No vkDeviceWaitIdle needed (which can deadlock under Wine FIFO present
 * when the message pump isn't running). */
static void vkt_switch_font(int font_idx)
{
    if (font_idx == current_font) return;
    current_font = font_idx;
    if (!vk_dev) return;

    /* Use the same proven upload path as initial load:
     * vkDeviceWaitIdle + tmp command buffer + tmp fence.
     * MUST NOT touch vk_cmd_buf or vk_fence — that corrupts
     * the render loop's synchronization state. */
    uint32_t aw, ah;
    uint8_t *pixels = vkt_build_font_pixels(current_font, &aw, &ah);
    vkt_upload_font_texture(pixels, aw, ah);
    free(pixels);

    /* Clear terminal so old glyph shapes don't linger */
    EnterCriticalSection(&ansi_lock);
    ap_init(&ansi_term, TERM_ROWS, TERM_COLS);
    LeaveCriticalSection(&ansi_lock);

    const char *fname = (current_font < 0) ? "CP437 Bitmap 4x"
                       : font_table[current_font].name;
    if (api) api->log("[vk_terminal] Switched to font: %s\n", fname);
}

/* ---- Vertex/Index buffers ---- */

static int vkt_create_buffers(void)
{
    /* Vertex buffer — host visible, updated every frame */
    VkBufferCreateInfo vbci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    vbci.size = MAX_QUADS * 4 * sizeof(vertex_t);
    vbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vkCreateBuffer(vk_dev, &vbci, NULL, &vk_vbuf);

    VkMemoryRequirements vmr;
    vkGetBufferMemoryRequirements(vk_dev, vk_vbuf, &vmr);
    VkMemoryAllocateInfo vmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    vmai.allocationSize = vmr.size;
    vmai.memoryTypeIndex = vk_find_memory(vk_pdev, vmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &vmai, NULL, &vk_vmem);
    vkBindBufferMemory(vk_dev, vk_vbuf, vk_vmem, 0);
    vkMapMemory(vk_dev, vk_vmem, 0, vmr.size, 0, (void **)&vk_vdata);

    /* Index buffer — static, generated once */
    uint32_t idx_count = MAX_QUADS * 6;
    VkBufferCreateInfo ibci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ibci.size = idx_count * sizeof(uint16_t);
    ibci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vkCreateBuffer(vk_dev, &ibci, NULL, &vk_ibuf);

    VkMemoryRequirements imr;
    vkGetBufferMemoryRequirements(vk_dev, vk_ibuf, &imr);
    VkMemoryAllocateInfo imai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex = vk_find_memory(vk_pdev, imr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &imai, NULL, &vk_imem);
    vkBindBufferMemory(vk_dev, vk_ibuf, vk_imem, 0);

    /* Fill index buffer: 0,1,2, 2,3,0 for each quad */
    uint16_t *idata;
    vkMapMemory(vk_dev, vk_imem, 0, imr.size, 0, (void **)&idata);
    for (int q = 0; q < MAX_QUADS; q++) {
        uint16_t base = (uint16_t)(q * 4);
        idata[q * 6 + 0] = base + 0;
        idata[q * 6 + 1] = base + 1;
        idata[q * 6 + 2] = base + 2;
        idata[q * 6 + 3] = base + 2;
        idata[q * 6 + 4] = base + 3;
        idata[q * 6 + 5] = base + 0;
    }
    vkUnmapMemory(vk_dev, vk_imem);

    return 0;
}

/* ---- Descriptor set ---- */

static int vkt_create_descriptors(void)
{
    /* Layout */
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    vkCreateDescriptorSetLayout(vk_dev, &dslci, NULL, &vk_desc_layout);

    /* Pool */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    vkCreateDescriptorPool(vk_dev, &dpci, NULL, &vk_desc_pool);

    /* Allocate */
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = vk_desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &vk_desc_layout;
    vkAllocateDescriptorSets(vk_dev, &dsai, &vk_desc_set);

    /* Update with font texture */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii.imageView = vk_font_view;
    dii.sampler = vk_font_sampler;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet = vk_desc_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);

    return 0;
}

/* ---- Swapchain + pipeline ---- */

static int vkt_create_swapchain(void)
{
    VkResult res;

    /* Surface capabilities */
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_pdev, vk_surface, &caps);

    /* Format */
    uint32_t fmtc;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_pdev, vk_surface, &fmtc, NULL);
    VkSurfaceFormatKHR *fmts = (VkSurfaceFormatKHR *)malloc(fmtc * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_pdev, vk_surface, &fmtc, fmts);
    vk_sc_fmt = fmts[0].format;
    VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < fmtc; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            vk_sc_fmt = fmts[i].format;
            cs = fmts[i].colorSpace;
            break;
        }
    }
    free(fmts);

    vk_sc_extent = caps.currentExtent;
    if (vk_sc_extent.width == UINT32_MAX) {
        vk_sc_extent.width = (uint32_t)fs_width;
        vk_sc_extent.height = (uint32_t)fs_height;
    }
    api->log("[vk_terminal] Surface extent: %dx%d (window: %dx%d)\n",
             vk_sc_extent.width, vk_sc_extent.height, fs_width, fs_height);

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = vk_surface;
    sci.minImageCount = img_count;
    sci.imageFormat = vk_sc_fmt;
    sci.imageColorSpace = cs;
    sci.imageExtent = vk_sc_extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    res = vkCreateSwapchainKHR(vk_dev, &sci, NULL, &vk_swapchain);
    if (res != VK_SUCCESS) { api->log("[vk_terminal] Swapchain failed: %d\n", res); return -1; }

    vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &vk_sc_count, NULL);
    vk_sc_images = (VkImage *)malloc(vk_sc_count * sizeof(VkImage));
    vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &vk_sc_count, vk_sc_images);

    /* Image views */
    vk_sc_views = (VkImageView *)malloc(vk_sc_count * sizeof(VkImageView));
    for (uint32_t i = 0; i < vk_sc_count; i++) {
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image = vk_sc_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = vk_sc_fmt;
        ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(vk_dev, &ivci, NULL, &vk_sc_views[i]);
    }

    /* Render pass */
    VkAttachmentDescription att = {0};
    att.format = vk_sc_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &aref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    vkCreateRenderPass(vk_dev, &rpci, NULL, &vk_renderpass);

    /* Framebuffers */
    vk_sc_fbs = (VkFramebuffer *)malloc(vk_sc_count * sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < vk_sc_count; i++) {
        VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = vk_renderpass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &vk_sc_views[i];
        fbci.width = vk_sc_extent.width;
        fbci.height = vk_sc_extent.height;
        fbci.layers = 1;
        vkCreateFramebuffer(vk_dev, &fbci, NULL, &vk_sc_fbs[i]);
    }

    /* Pipeline layout */
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &vk_desc_layout;
    vkCreatePipelineLayout(vk_dev, &plci, NULL, &vk_pipe_layout);

    /* Shader modules */
    VkShaderModuleCreateInfo vsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsci.codeSize = term_vert_spv_size;
    vsci.pCode = term_vert_spv;
    VkShaderModule vs;
    vkCreateShaderModule(vk_dev, &vsci, NULL, &vs);

    VkShaderModuleCreateInfo fsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fsci.codeSize = term_frag_spv_size;
    fsci.pCode = term_frag_spv;
    VkShaderModule fs;
    vkCreateShaderModule(vk_dev, &fsci, NULL, &fs);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
          VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };

    /* Vertex input */
    VkVertexInputBindingDescription vibd = { 0, sizeof(vertex_t), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription viad[] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(vertex_t, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(vertex_t, u) },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vertex_t, r) },
        { 3, 0, VK_FORMAT_R32_SFLOAT,       offsetof(vertex_t, a) },
    };
    VkPipelineVertexInputStateCreateInfo visci = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    visci.vertexBindingDescriptionCount = 1;
    visci.pVertexBindingDescriptions = &vibd;
    visci.vertexAttributeDescriptionCount = 4;
    visci.pVertexAttributeDescriptions = viad;

    VkPipelineInputAssemblyStateCreateInfo iasci = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = { 0, 0, (float)vk_sc_extent.width, (float)vk_sc_extent.height, 0, 1 };
    VkRect2D scissor = { {0,0}, vk_sc_extent };
    VkPipelineViewportStateCreateInfo vpsci = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpsci.viewportCount = 1;
    vpsci.pViewports = &viewport;
    vpsci.scissorCount = 1;
    vpsci.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rsci = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.lineWidth = 1.0f;
    rsci.cullMode = VK_CULL_MODE_NONE;
    rsci.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo msci = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Alpha blending for text overlay */
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbsci = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbsci.attachmentCount = 1;
    cbsci.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState = &vpsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState = &msci;
    gpci.pColorBlendState = &cbsci;
    gpci.layout = vk_pipe_layout;
    gpci.renderPass = vk_renderpass;
    gpci.subpass = 0;
    res = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &gpci, NULL, &vk_pipeline);

    vkDestroyShaderModule(vk_dev, vs, NULL);
    vkDestroyShaderModule(vk_dev, fs, NULL);

    if (res != VK_SUCCESS) { api->log("[vk_terminal] Pipeline failed: %d\n", res); return -1; }
    api->log("[vk_terminal] Swapchain %dx%d, %d images\n",
             vk_sc_extent.width, vk_sc_extent.height, vk_sc_count);
    return 0;
}

static void vkt_destroy_swapchain(void)
{
    vkDeviceWaitIdle(vk_dev);
    if (vk_pipeline) { vkDestroyPipeline(vk_dev, vk_pipeline, NULL); vk_pipeline = VK_NULL_HANDLE; }
    if (vk_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_pipe_layout, NULL); vk_pipe_layout = VK_NULL_HANDLE; }
    if (vk_renderpass) { vkDestroyRenderPass(vk_dev, vk_renderpass, NULL); vk_renderpass = VK_NULL_HANDLE; }
    if (vk_sc_fbs) {
        for (uint32_t i = 0; i < vk_sc_count; i++) vkDestroyFramebuffer(vk_dev, vk_sc_fbs[i], NULL);
        free(vk_sc_fbs); vk_sc_fbs = NULL;
    }
    if (vk_sc_views) {
        for (uint32_t i = 0; i < vk_sc_count; i++) vkDestroyImageView(vk_dev, vk_sc_views[i], NULL);
        free(vk_sc_views); vk_sc_views = NULL;
    }
    if (vk_sc_images) { free(vk_sc_images); vk_sc_images = NULL; }
    if (vk_swapchain) { vkDestroySwapchainKHR(vk_dev, vk_swapchain, NULL); vk_swapchain = VK_NULL_HANDLE; }
}

/* ---- Render frame ---- */

static void vkt_render_frame(void)
{
    vkWaitForFences(vk_dev, 1, &vk_fence, VK_TRUE, UINT64_MAX);

    /* Font switch here — fence waited, previous frame fully complete.
     * This is the ONLY safe place to do heavy Vulkan resource recreation
     * because we know nothing is in flight. */
    if (font_change_pending) {
        font_change_pending = 0;
        vkt_switch_font(pending_font_idx);
    }

    vkResetFences(vk_dev, 1, &vk_fence);

    uint32_t img_idx;
    VkResult res = vkAcquireNextImageKHR(vk_dev, vk_swapchain, UINT64_MAX, vk_sem_avail, VK_NULL_HANDLE, &img_idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        vkt_destroy_swapchain();
        vkt_create_swapchain();
        return;
    }

    /* Build vertex data */
    vkt_read_buffer();
    vkt_build_vertices();

    /* Record command buffer */
    vkResetCommandBuffer(vk_cmd_buf, 0);
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(vk_cmd_buf, &cbbi);

    VkClearValue clear = {{{ palette[0].r, palette[0].g, palette[0].b, 1.0f }}};
    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass = vk_renderpass;
    rpbi.framebuffer = vk_sc_fbs[img_idx];
    rpbi.renderArea.extent = vk_sc_extent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(vk_cmd_buf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
    vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(vk_cmd_buf, 0, 1, &vk_vbuf, &offset);
    vkCmdBindIndexBuffer(vk_cmd_buf, vk_ibuf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(vk_cmd_buf, quad_count * 6, 1, 0, 0, 0);

    vkCmdEndRenderPass(vk_cmd_buf);
    vkEndCommandBuffer(vk_cmd_buf);

    /* Submit */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk_sem_avail;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk_cmd_buf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk_sem_done;
    vkQueueSubmit(vk_queue, 1, &si, vk_fence);

    /* Present */
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk_sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk_swapchain;
    pi.pImageIndices = &img_idx;
    vkQueuePresentKHR(vk_queue, &pi);

    /* Screenshot after render if requested */
    if (vkt_screenshot_pending) {
        vkt_screenshot_pending = 0;
        vkt_screenshot(img_idx);
    }

    frame_count++;
}

/* ---- Window procedure ---- */

static LRESULT CALLBACK vkt_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (vkm_open) { vkm_open = 0; vkm_sub = VKM_SUB_NONE; return 0; }
            if (vkw_focus >= 0) { vkw_focus = -1; return 0; } /* Esc = back to terminal */
        }
        /* Tab cycles focus: terminal → windows → terminal */
        if (wParam == VK_TAB) {
            if (vkw_count > 0) {
                if (vkw_focus < 0) {
                    vkw_focus = vkw_order[vkw_count - 1]; /* focus top window */
                } else {
                    vkw_focus = -1; /* back to terminal */
                }
            }
            return 0;
        }
        /* Route to focused window first */
        if (vkw_key_down((unsigned int)wParam)) return 0;
        input_handle_key(wParam, 0);
        return 0;
    case WM_CHAR:
        /* Route to focused window first */
        if (vkw_key_char((unsigned int)wParam)) return 0;
        input_handle_key(wParam, 1);
        return 0;
    case WM_MOUSEMOVE: {
        int mx2 = (short)LOWORD(lParam);
        int my2 = (short)HIWORD(lParam);
        vkm_mouse_x = mx2;
        vkm_mouse_y = my2;
        /* Window drag/resize takes priority */
        if (vkw_mouse_move(mx2, my2)) return 0;
        if (vkm_open) {
            int rh = vkm_hit_root(mx2, my2);
            int sh = vkm_hit_sub(mx2, my2);
            if (rh >= 0) {
                vkm_hover = rh;
                if (rh == VKM_ITEM_THEME) vkm_sub = VKM_SUB_THEME;
                else if (rh == VKM_ITEM_FONT) vkm_sub = VKM_SUB_FONT;
                else vkm_sub = VKM_SUB_NONE;
                vkm_sub_hover = -1;
            }
            if (sh >= 0) vkm_sub_hover = sh;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);
        if (!vkm_open) {
            vkw_mouse_down(mx, my); /* start drag/resize or focus window */
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int mx = (short)LOWORD(lParam);
        int my = (short)HIWORD(lParam);
        vkw_mouse_up(); /* end any drag/resize */

        if (!vkm_open) return 0;

        /* Check submenu click first */
        int si = vkm_hit_sub(mx, my);
        if (si >= 0 && vkm_sub == VKM_SUB_THEME) {
            current_theme = si;
            /* theme changed — palette stays Classic VGA */
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }
        if (si >= 0 && vkm_sub == VKM_SUB_FONT) {
            if (si == 0) {
                pending_font_idx = -1;
            } else {
                pending_font_idx = si - 1;
            }
            font_change_pending = 1;
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }

        /* Check root click */
        int ri = vkm_hit_root(mx, my);
        if (ri == VKM_ITEM_CONSOLE) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            vkw_toggle_console((int)vk_sc_extent.width, (int)vk_sc_extent.height);
            return 0;
        }
        if (ri == VKM_ITEM_CLOSE) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            vkt_hide();
            return 0;
        }
        if (ri == VKM_ITEM_THEME || ri == VKM_ITEM_FONT) {
            return 0;
        }

        /* Click outside menu = close */
        vkm_open = 0;
        vkm_sub = VKM_SUB_NONE;
        return 0;
    }
    case WM_RBUTTONUP: {
        /* Toggle Vulkan-rendered context menu at mouse position */
        if (vkm_open) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
        } else {
            vkm_x = (short)LOWORD(lParam);
            vkm_y = (short)HIWORD(lParam);
            vkm_open = 1;
            vkm_hover = -1;
            vkm_sub = VKM_SUB_NONE;
            vkm_sub_hover = -1;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        /* Scroll focused window */
        if (vkw_focus >= 0) {
            int delta = (short)HIWORD(wParam);
            vkw_window_t *fw = &vkw_windows[vkw_focus];
            if (fw->active) {
                fw->scroll += (delta > 0) ? 3 : -3;
                if (fw->scroll < 0) fw->scroll = 0;
                if (fw->scroll > fw->line_count - 1) fw->scroll = fw->line_count - 1;
            }
        }
        return 0;
    }
    case WM_CLOSE:
        vkt_hide();
        return 0;
    case WM_DESTROY:
        vkt_visible = 0;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---- Thread ---- */

static DWORD WINAPI vkt_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    /* Wait for MMANSI */
    for (int i = 0; i < 100 && !mmansi_hwnd; i++) {
        HWND mw = FindWindowA("MMMAIN", NULL);
        if (mw) {
            mmmain_hwnd = mw;
            mmansi_hwnd = FindWindowExA(mw, NULL, "MMANSI", NULL);
        }
        if (!mmansi_hwnd) Sleep(100);
    }
    if (!mmansi_hwnd) { api->log("[vk_terminal] MMANSI not found\n"); return 1; }

    /* Init Vulkan (instance, device, etc.) */
    if (vkt_init_vulkan() != 0) return 1;
    if (vkt_create_font_texture() != 0) return 1;
    if (vkt_create_buffers() != 0) return 1;
    if (vkt_create_descriptors() != 0) return 1;

    /* Register window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = vkt_wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SlopVkTerminal";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    palette = pal_classic;
    vkt_running = 1;
    api->log("[vk_terminal] Ready (F11 to toggle fullscreen)\n");

    /* Main loop — wait for F11 toggle, then render */
    while (vkt_running) {
        if (!vkt_visible) {
            /* Tear down presentation if window exists */
            if (vkt_hwnd) {
                if (vk_dev) vkDeviceWaitIdle(vk_dev);
                vkt_destroy_swapchain();
                if (vk_surface) { vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE; }
                DestroyWindow(vkt_hwnd);
                vkt_hwnd = NULL;
                /* Return focus to MegaMUD */
                if (mmmain_hwnd) {
                    SetForegroundWindow(mmmain_hwnd);
                    if (mmansi_hwnd) SetFocus(mmansi_hwnd);
                }
                api->log("[vk_terminal] Closed fullscreen\n");
            }
            Sleep(50);
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { vkt_running = 0; break; }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            continue;
        }

        /* Create fullscreen window if needed */
        if (!vkt_hwnd) {
            /* Use GetSystemMetrics for logical desktop size.
             * Under XWayland with KDE 200% scaling, Wine sees 1920x1080 logical
             * which is the correct coordinate space for window creation. */
            {
                int sm_w = GetSystemMetrics(SM_CXSCREEN);
                int sm_h = GetSystemMetrics(SM_CYSCREEN);
                if (sm_w > 0 && sm_h > 0) {
                    fs_width = sm_w;
                    fs_height = sm_h;
                }
                api->log("[vk_terminal] GetSystemMetrics: %dx%d\n", sm_w, sm_h);
            }
            api->log("[vk_terminal] Fullscreen target: %dx%d\n", fs_width, fs_height);

            vkt_hwnd = CreateWindowExA(
                WS_EX_APPWINDOW,   /* normal alt-tab entry, not always-on-top */
                "SlopVkTerminal", "MajorMUD Terminal",
                WS_POPUP | WS_VISIBLE,
                0, 0, fs_width, fs_height,
                NULL, NULL, hInst, NULL);
            if (!vkt_hwnd) {
                api->log("[vk_terminal] CreateWindow failed\n");
                vkt_visible = 0;
                continue;
            }

            /* Create Vulkan surface */
            VkWin32SurfaceCreateInfoKHR wsci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
            wsci.hinstance = hInst;
            wsci.hwnd = vkt_hwnd;
            VkResult res = vkCreateWin32SurfaceKHR(vk_inst, &wsci, NULL, &vk_surface);
            if (res != VK_SUCCESS) {
                api->log("[vk_terminal] Surface failed: %d\n", res);
                DestroyWindow(vkt_hwnd); vkt_hwnd = NULL;
                vkt_visible = 0;
                continue;
            }

            /* Verify surface support */
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(vk_pdev, vk_qfam, vk_surface, &supported);
            if (!supported) {
                api->log("[vk_terminal] Surface not supported by queue family\n");
                vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE;
                DestroyWindow(vkt_hwnd); vkt_hwnd = NULL;
                vkt_visible = 0;
                continue;
            }

            if (vkt_create_swapchain() != 0) {
                vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE;
                DestroyWindow(vkt_hwnd); vkt_hwnd = NULL;
                vkt_visible = 0;
                continue;
            }

            ShowWindow(vkt_hwnd, SW_SHOW);
            SetForegroundWindow(vkt_hwnd);
            SetFocus(vkt_hwnd);
            api->log("[vk_terminal] Fullscreen %dx%d\n", fs_width, fs_height);

        }

        /* Process messages */
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { vkt_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!vkt_running) break;

        /* Render if visible (font changes handled inside render_frame) */
        if (vkt_visible && vkt_hwnd && vk_swapchain) {
            vkt_render_frame();
        }

        Sleep(1); /* ~vsync via FIFO present mode */
    }

    /* Cleanup */
    vkt_cleanup_vulkan();
    if (vkt_hwnd) { DestroyWindow(vkt_hwnd); vkt_hwnd = NULL; }
    return 0;
}

/* ---- Vulkan cleanup ---- */

static void vkt_cleanup_vulkan(void)
{
    if (vk_dev) vkDeviceWaitIdle(vk_dev);

    vkt_destroy_swapchain();

    if (vk_vdata) { vkUnmapMemory(vk_dev, vk_vmem); vk_vdata = NULL; }
    if (vk_vbuf) { vkDestroyBuffer(vk_dev, vk_vbuf, NULL); vk_vbuf = VK_NULL_HANDLE; }
    if (vk_vmem) { vkFreeMemory(vk_dev, vk_vmem, NULL); vk_vmem = VK_NULL_HANDLE; }
    if (vk_ibuf) { vkDestroyBuffer(vk_dev, vk_ibuf, NULL); vk_ibuf = VK_NULL_HANDLE; }
    if (vk_imem) { vkFreeMemory(vk_dev, vk_imem, NULL); vk_imem = VK_NULL_HANDLE; }

    if (vk_font_sampler) { vkDestroySampler(vk_dev, vk_font_sampler, NULL); vk_font_sampler = VK_NULL_HANDLE; }
    if (vk_font_view) { vkDestroyImageView(vk_dev, vk_font_view, NULL); vk_font_view = VK_NULL_HANDLE; }
    if (vk_font_img) { vkDestroyImage(vk_dev, vk_font_img, NULL); vk_font_img = VK_NULL_HANDLE; }
    if (vk_font_mem) { vkFreeMemory(vk_dev, vk_font_mem, NULL); vk_font_mem = VK_NULL_HANDLE; }

    if (vk_desc_pool) { vkDestroyDescriptorPool(vk_dev, vk_desc_pool, NULL); vk_desc_pool = VK_NULL_HANDLE; }
    if (vk_desc_layout) { vkDestroyDescriptorSetLayout(vk_dev, vk_desc_layout, NULL); vk_desc_layout = VK_NULL_HANDLE; }

    if (vk_sem_avail) { vkDestroySemaphore(vk_dev, vk_sem_avail, NULL); vk_sem_avail = VK_NULL_HANDLE; }
    if (vk_sem_done) { vkDestroySemaphore(vk_dev, vk_sem_done, NULL); vk_sem_done = VK_NULL_HANDLE; }
    if (vk_fence) { vkDestroyFence(vk_dev, vk_fence, NULL); vk_fence = VK_NULL_HANDLE; }
    if (vk_cmd_pool) { vkDestroyCommandPool(vk_dev, vk_cmd_pool, NULL); vk_cmd_pool = VK_NULL_HANDLE; }

    if (vk_surface) { vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE; }
    if (vk_dev) { vkDestroyDevice(vk_dev, NULL); vk_dev = VK_NULL_HANDLE; }
    if (vk_inst) { vkDestroyInstance(vk_inst, NULL); vk_inst = VK_NULL_HANDLE; }
}

/* ---- Exported commands ---- */

__declspec(dllexport) void vkt_show(void)
{
    vkt_visible = 1;
}

__declspec(dllexport) void vkt_hide(void)
{
    vkt_visible = 0;
    /* Don't destroy Vulkan objects here — the render thread handles it.
     * Just set the flag and the thread loop will tear down the window
     * on the next iteration. */
}

__declspec(dllexport) void vkt_toggle(void)
{
    if (vkt_visible) vkt_hide(); else vkt_show();
}

__declspec(dllexport) void vkt_set_resolution(int idx)
{
    if (idx >= 0 && idx < (int)NUM_RES) {
        fs_res_idx = idx;
        fs_width = resolutions[idx].w;
        fs_height = resolutions[idx].h;
    }
}

static slop_command_t vkt_commands[] = {
    { "vkt_show",     "vkt_show",     "",  "v", "Show Vulkan fullscreen terminal" },
    { "vkt_hide",     "vkt_hide",     "",  "v", "Hide Vulkan terminal" },
    { "vkt_toggle",   "vkt_toggle",   "",  "v", "Toggle Vulkan terminal (F11)" },
    { "vkt_set_res",  "vkt_set_resolution", "i", "v", "Set resolution (0=1080p, 1=900, 2=768, 3=720, 4=1024x768)" },
    { "wnd_create",   "vkt_wnd_create", "siiiiii", "i", "Create window(title,x,y,w,h,has_input) -> id" },
    { "wnd_print",    "vkt_wnd_print",  "is", "v", "Print text to window(id, text)" },
    { "wnd_close",    "vkt_wnd_close",  "i",  "v", "Close window(id)" },
    { "wnd_clear",    "vkt_wnd_clear",  "i",  "v", "Clear window text(id)" },
    { "wnd_opacity",  "vkt_wnd_set_opacity", "if", "v", "Set window opacity(id, 0.0-1.0)" },
    { "console_show", "vkt_console_show", "", "v", "Show the Python console window" },
    { "console_print","vkt_console_print","s", "v", "Print to console window" },
};

__declspec(dllexport) slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(vkt_commands) / sizeof(vkt_commands[0]);
    return vkt_commands;
}

/* ---- Plugin interface ---- */

static int vkt_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_VKT_TOGGLE) {
        vkt_toggle();
        return 1;
    }
    /* F11 from MegaMUD window opens the Vulkan terminal */
    if (msg == WM_KEYDOWN && wParam == VK_F11) {
        vkt_toggle();
        return 1;
    }
    return 0;
}

static int vkt_init(const slop_api_t *a)
{
    api = a;
    api->log("[vk_terminal] Initializing Vulkan Terminal...\n");
    api->add_menu_item("Vulkan Terminal (F11)", IDM_VKT_TOGGLE);

    /* Initialize ANSI terminal emulator */
    InitializeCriticalSection(&ansi_lock);
    ap_init(&ansi_term, TERM_ROWS, TERM_COLS);

    palette = pal_classic;
    input_buf[0] = '\0';

    HINSTANCE hInst = GetModuleHandleA(NULL);
    vkt_thread_handle = CreateThread(NULL, 0, vkt_thread, (LPVOID)hInst, 0, NULL);
    return 0;
}

static void vkt_shutdown(void)
{
    vkt_running = 0;
    if (vkt_hwnd) PostMessageA(vkt_hwnd, WM_QUIT, 0, 0);
    if (vkt_thread_handle) {
        WaitForSingleObject(vkt_thread_handle, 2000);
        CloseHandle(vkt_thread_handle);
        vkt_thread_handle = NULL;
    }
    for (int i = 0; i < history_count; i++) free(cmd_history[i]);
    history_count = 0;
    DeleteCriticalSection(&ansi_lock);
    if (api) api->log("[vk_terminal] Shutdown\n");
}

static slop_plugin_t vkt_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Vulkan Terminal",
    .author      = "Tripmunk",
    .description = "Fullscreen Vulkan terminal — F11 toggle, CP437 font, DOS colors",
    .version     = "0.1.0",
    .init        = vkt_init,
    .shutdown    = vkt_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = vkt_on_wndproc,
    .on_data     = vkt_on_data,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &vkt_plugin; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
