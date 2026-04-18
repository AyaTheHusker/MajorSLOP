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
#include "line_fsm.h"
#include "cp437.h"          /* original 8x16 bitmap */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"   /* runtime TTF rasterization */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <commdlg.h>        /* GetSaveFileNameA for backscroll save */

/* Proper byte-by-byte ANSI/VT100 state machine parser */
#define AP_ROWS 25
#define AP_COLS 80
#define ANSI_PARSER_IMPLEMENTATION
#include "ansi_parser.h"

/* ---- Terminal constants ---- */

#define TERM_ROWS       25
#define TERM_COLS       80

#define INPUT_BAR_H_BASE 40     /* pixels for input bar at 1080p */
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

#define NUM_THEMES 18
static const ui_theme_t ui_themes[NUM_THEMES] = {
    {"Classic ANSI",   F3(24,24,24),   F3(192,192,192), F3(128,128,128), F3(85,85,255)},
    {"Grey Lord",      F3(12,12,28),   F3(200,200,224), F3(102,102,170), F3(68,68,204)},
    {"Black Fort",     F3(4,4,10),     F3(153,154,176), F3(68,68,102),   F3(51,51,102)},
    {"Khazarad",       F3(18,14,8),    F3(212,196,160), F3(138,122,86),  F3(170,136,68)},
    {"Silvermere",     F3(22,22,30),   F3(216,216,232), F3(136,136,170), F3(119,136,187)},
    {"Annora",         F3(30,30,40),   F3(238,238,244), F3(170,170,204), F3(153,170,221)},
    {"Jorah",          F3(8,12,28),    F3(176,192,232), F3(85,102,170),  F3(51,102,204)},
    {"Putakwa",        F3(6,16,14),    F3(160,216,200), F3(68,136,102),  F3(34,170,136)},
    {"Void",           F3(14,6,24),    F3(208,176,232), F3(119,68,170),  F3(136,68,204)},
    {"Ozzrinom",       F3(12,4,4),     F3(200,180,180), F3(120,50,50),   F3(180,20,20)},
    {"Phoenix",        F3(16,8,4),     F3(232,208,168), F3(136,102,64),  F3(221,102,34)},
    {"Mad Wizard",     F3(10,6,20),    F3(224,240,255), F3(102,170,204), F3(0,238,255)},
    {"Tasloi",         F3(10,16,8),    F3(192,216,160), F3(102,136,68),  F3(68,170,34)},
    {"Frostborn",      F3(8,14,22),    F3(200,224,240), F3(85,136,170),  F3(68,170,221)},
    {"Sandstorm",      F3(16,12,8),    F3(224,212,184), F3(138,122,86),  F3(204,170,68)},
    {"Crystal Cavern", F3(12,8,20),    F3(208,192,232), F3(119,102,170), F3(153,102,238)},
    {"Afroman",        F3(8,8,24),     F3(240,240,245), F3(140,160,210), F3(200,40,60)},
    {"Bog Lord",       F3(10,14,8),    F3(176,184,144), F3(96,104,64),   F3(119,136,51)},
};

/* ---- Resolution presets ---- */

typedef struct { int w, h; const char *name; } res_t;
static res_t resolutions[] = {
    { 3840, 2160, "3840x2160 (4K UHD)" },
    { 2560, 1440, "2560x1440 (QHD)" },
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
#include "shaders/liquid_vert.h"
#include "shaders/liquid_frag.h"
#include "shaders/bg_plasma_vert.h"
#include "shaders/bg_plasma_frag.h"
#include "shaders/bg_starfield_vert.h"
#include "shaders/bg_starfield_frag.h"
#include "shaders/bg_alienwall_frag.h"

/* ---- Plugin state ---- */

static const slop_api_t *api = NULL;
static HWND mmansi_hwnd = NULL;
static HWND mmmain_hwnd = NULL;
static HWND vkt_hwnd = NULL;
static HANDLE vkt_thread_handle = NULL;
static int vkt_mt_init_done = 0; /* Vulkan/font/buffer/descriptor init finished on main thread */
static volatile int vkt_running = 0;
static volatile int vkt_visible = 0;
static int vkt_autoconnect = 0;  /* --autoconnect flag */
static char vkt_loop_name[128] = {0}; /* --loop "name" */
static char vkt_loadscripts[512] = {0}; /* --loadscripts "a,b,c" */
static volatile int vkt_screenshot_pending = 0;
static uint32_t vkt_screenshot_img_idx = 0;

/* Settings */
static int current_theme = 0;          /* index into ui_themes[] */
static rgb_t tinted_palette[16];       /* theme-tinted ANSI palette */
static rgb_t *palette = pal_classic;   /* ANSI palette pointer */

/* ---- Per-word gradient color system ----
 * Each theme defines a 10-stop color ramp per ANSI color index.
 * Words interpolate through the ramp character by character. */

#define GRAD_STOPS 10

typedef struct {
    rgb_t stops[GRAD_STOPS];
} color_ramp_t;

static color_ramp_t theme_ramps[16]; /* current theme's ramps, one per ANSI color */

/* HSV to RGB helper */
static float fmod360(float h) { while (h >= 360.0f) h -= 360.0f; while (h < 0.0f) h += 360.0f; return h; }
static float fabsf_(float x) { return x < 0.0f ? -x : x; }
static rgb_t hsv2rgb(float h, float s, float v)
{
    h = fmod360(h);
    float hh = h / 60.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf_(hh - 2.0f * (int)(hh / 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    if      (h < 60)  { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else              { r=c; g=0; b=x; }
    rgb_t out = { r+m, g+m, b+m };
    return out;
}

/* Build a 10-stop ramp from HSV keyframes */
static void build_ramp(color_ramp_t *ramp, int n, const float keys[][4])
{
    /* keys[i] = {position 0-1, H, S, V} — we'll interpolate between them */
    for (int s = 0; s < GRAD_STOPS; s++) {
        float t = (float)s / (float)(GRAD_STOPS - 1);
        /* Find surrounding keyframes */
        int lo = 0, hi = 0;
        for (int k = 0; k < n - 1; k++) {
            if (t >= keys[k][0] && t <= keys[k+1][0]) { lo = k; hi = k+1; break; }
            if (k == n - 2) { lo = k; hi = k+1; }
        }
        float seg = keys[hi][0] - keys[lo][0];
        float f = (seg > 0.001f) ? (t - keys[lo][0]) / seg : 0.0f;
        float h = keys[lo][1] + f * (keys[hi][1] - keys[lo][1]);
        float sa = keys[lo][2] + f * (keys[hi][2] - keys[lo][2]);
        float v = keys[lo][3] + f * (keys[hi][3] - keys[lo][3]);
        ramp->stops[s] = hsv2rgb(h, sa, v);
    }
}

/* Sample a ramp at position t (0.0–1.0) with linear interpolation */
static rgb_t sample_ramp(const color_ramp_t *ramp, float t)
{
    if (t <= 0.0f) return ramp->stops[0];
    if (t >= 1.0f) return ramp->stops[GRAD_STOPS - 1];
    float fi = t * (GRAD_STOPS - 1);
    int lo = (int)fi;
    float f = fi - lo;
    if (lo >= GRAD_STOPS - 1) return ramp->stops[GRAD_STOPS - 1];
    rgb_t a = ramp->stops[lo], b = ramp->stops[lo + 1];
    rgb_t out = { a.r + f*(b.r-a.r), a.g + f*(b.g-a.g), a.b + f*(b.b-a.b) };
    return out;
}

/* Generate all 16 color ramps for a theme.
 * Each theme defines its artistic palette through HSV keyframe ramps.
 * ANSI colors 8-15 (bright) use the same base ramp as 0-7 but boosted. */
static void apply_theme_palette(int theme_idx)
{
    /* Per-theme ramp definitions.
     * Format: for each of the 8 base ANSI colors, define 3-5 HSV keyframes:
     *   {position, Hue, Saturation, Value}
     * Bright variants (8-15) are auto-generated by boosting value. */

    /* Theme gradient specs: [ansi_color 0-7][keyframe][4 floats] */
    /* Each spec has up to 5 keyframes */
    #define K5(a,b,c,d,e) {a,b,c,d,e}
    #define KF(p,h,s,v) {p,h,s,v}

    static const float specs[NUM_THEMES][8][5][4] = {

    /* 0: Classic ANSI — standard ANSI colors, dim=0.42 bright=1.0 via 2.4x boost */
    {
     /*blk*/ {KF(0,0,0.0,0.02), KF(1,0,0.0,0.02), {0},{0},{0}},
     /*red*/ {KF(0,0,1.0,0.42), KF(1,0,1.0,0.42), {0},{0},{0}},
     /*grn*/ {KF(0,120,1.0,0.42), KF(1,120,1.0,0.42), {0},{0},{0}},
     /*brn*/ {KF(0,30,1.0,0.42), KF(1,30,1.0,0.42), {0},{0},{0}},
     /*blu*/ {KF(0,240,1.0,0.42), KF(1,240,1.0,0.42), {0},{0},{0}},
     /*mag*/ {KF(0,300,1.0,0.42), KF(1,300,1.0,0.42), {0},{0},{0}},
     /*cyn*/ {KF(0,180,1.0,0.42), KF(1,180,1.0,0.42), {0},{0},{0}},
     /*wht*/ {KF(0,0,0.0,0.42), KF(1,0,0.0,0.42), {0},{0},{0}},
    },

    /* 1: Grey Lord — cold steel blues, icy silvers */
    {
     /*blk*/ {KF(0,220,0.1,0.04), KF(0.5,230,0.15,0.06), KF(1,220,0.1,0.04), {0},{0}},
     /*red*/ {KF(0,200,0.5,0.50), KF(0.3,215,0.6,0.65), KF(0.6,230,0.55,0.70), KF(0.8,240,0.5,0.60), KF(1,220,0.4,0.55)},
     /*grn*/ {KF(0,195,0.4,0.55), KF(0.3,210,0.5,0.65), KF(0.7,225,0.45,0.70), KF(1,240,0.35,0.60), {0}},
     /*brn*/ {KF(0,210,0.35,0.50), KF(0.4,225,0.45,0.65), KF(0.7,240,0.5,0.72), KF(1,250,0.4,0.60), {0}},
     /*blu*/ {KF(0,230,0.6,0.55), KF(0.3,245,0.65,0.70), KF(0.6,260,0.6,0.75), KF(0.8,250,0.55,0.65), KF(1,235,0.5,0.58)},
     /*mag*/ {KF(0,260,0.45,0.50), KF(0.4,275,0.5,0.65), KF(0.7,290,0.45,0.60), KF(1,270,0.4,0.55), {0}},
     /*cyn*/ {KF(0,190,0.45,0.55), KF(0.3,200,0.5,0.68), KF(0.6,215,0.55,0.72), KF(1,205,0.45,0.60), {0}},
     /*wht*/ {KF(0,220,0.12,0.70), KF(0.3,225,0.15,0.80), KF(0.6,230,0.1,0.85), KF(1,220,0.08,0.75), {0}},
    },

    /* 2: Black Fort — deep indigo, cold steel, midnight */
    {
     {KF(0,240,0.1,0.02), KF(1,240,0.1,0.03), {0},{0},{0}},
     {KF(0,250,0.5,0.35), KF(0.3,260,0.55,0.45), KF(0.7,275,0.5,0.50), KF(1,265,0.45,0.40), {0}},
     {KF(0,220,0.4,0.38), KF(0.4,235,0.5,0.48), KF(0.7,245,0.45,0.45), KF(1,230,0.4,0.40), {0}},
     {KF(0,235,0.35,0.40), KF(0.5,250,0.45,0.52), KF(1,260,0.4,0.45), {0},{0}},
     {KF(0,240,0.6,0.40), KF(0.3,255,0.65,0.52), KF(0.6,270,0.6,0.48), KF(1,250,0.55,0.42), {0}},
     {KF(0,280,0.5,0.38), KF(0.5,295,0.55,0.48), KF(1,285,0.45,0.42), {0},{0}},
     {KF(0,210,0.4,0.40), KF(0.5,225,0.5,0.52), KF(1,215,0.4,0.45), {0},{0}},
     {KF(0,235,0.1,0.55), KF(0.5,240,0.12,0.65), KF(1,235,0.08,0.58), {0},{0}},
    },

    /* 3: Khazarad — warm amber, burnished gold, aged leather */
    {
     {KF(0,30,0.15,0.04), KF(1,35,0.2,0.06), {0},{0},{0}},
     {KF(0,5,0.7,0.55), KF(0.3,15,0.75,0.65), KF(0.6,25,0.7,0.72), KF(0.8,35,0.65,0.68), KF(1,20,0.6,0.58)},
     {KF(0,45,0.6,0.50), KF(0.3,55,0.65,0.60), KF(0.7,40,0.6,0.65), KF(1,50,0.55,0.55), {0}},
     {KF(0,30,0.7,0.55), KF(0.3,40,0.75,0.68), KF(0.6,50,0.7,0.72), KF(1,35,0.65,0.60), {0}},
     {KF(0,20,0.55,0.45), KF(0.4,30,0.6,0.55), KF(0.7,15,0.55,0.50), KF(1,25,0.5,0.48), {0}},
     {KF(0,345,0.6,0.50), KF(0.4,355,0.65,0.60), KF(0.7,10,0.6,0.58), KF(1,0,0.55,0.52), {0}},
     {KF(0,35,0.5,0.58), KF(0.4,45,0.55,0.68), KF(0.7,55,0.5,0.65), KF(1,42,0.45,0.60), {0}},
     {KF(0,35,0.15,0.72), KF(0.4,40,0.2,0.82), KF(0.7,45,0.15,0.85), KF(1,38,0.1,0.78), {0}},
    },

    /* 4: Silvermere — silver, lavender, moonlight */
    {
     {KF(0,250,0.05,0.04), KF(1,250,0.05,0.05), {0},{0},{0}},
     {KF(0,260,0.35,0.60), KF(0.3,270,0.4,0.72), KF(0.7,280,0.35,0.68), KF(1,265,0.3,0.62), {0}},
     {KF(0,220,0.3,0.62), KF(0.4,230,0.35,0.72), KF(0.7,240,0.3,0.70), KF(1,225,0.28,0.65), {0}},
     {KF(0,240,0.3,0.60), KF(0.5,250,0.35,0.70), KF(1,245,0.28,0.65), {0},{0}},
     {KF(0,230,0.4,0.60), KF(0.3,245,0.5,0.72), KF(0.7,255,0.45,0.68), KF(1,240,0.38,0.62), {0}},
     {KF(0,275,0.35,0.58), KF(0.5,290,0.4,0.68), KF(1,280,0.32,0.62), {0},{0}},
     {KF(0,205,0.3,0.65), KF(0.4,215,0.35,0.75), KF(0.7,225,0.3,0.72), KF(1,210,0.28,0.68), {0}},
     {KF(0,240,0.08,0.78), KF(0.3,245,0.1,0.85), KF(0.7,250,0.06,0.88), KF(1,242,0.05,0.82), {0}},
    },

    /* 5: Annora — soft lavender, pink, ethereal */
    {
     {KF(0,270,0.05,0.05), KF(1,270,0.05,0.06), {0},{0},{0}},
     {KF(0,310,0.45,0.65), KF(0.3,320,0.5,0.75), KF(0.6,330,0.45,0.78), KF(0.8,340,0.4,0.72), KF(1,325,0.38,0.68)},
     {KF(0,250,0.35,0.62), KF(0.4,260,0.4,0.72), KF(0.7,270,0.38,0.75), KF(1,255,0.32,0.68), {0}},
     {KF(0,280,0.4,0.62), KF(0.5,290,0.45,0.72), KF(1,285,0.38,0.68), {0},{0}},
     {KF(0,240,0.45,0.60), KF(0.3,255,0.5,0.72), KF(0.7,265,0.48,0.75), KF(1,250,0.42,0.65), {0}},
     {KF(0,300,0.45,0.62), KF(0.4,315,0.5,0.72), KF(0.7,325,0.45,0.68), KF(1,310,0.4,0.65), {0}},
     {KF(0,220,0.35,0.68), KF(0.4,235,0.4,0.78), KF(0.7,245,0.35,0.75), KF(1,230,0.3,0.70), {0}},
     {KF(0,270,0.08,0.82), KF(0.3,275,0.1,0.88), KF(0.7,280,0.06,0.92), KF(1,272,0.05,0.85), {0}},
    },

    /* 6: Jorah — deep ocean blue, sapphire, wave crests */
    {
     {KF(0,220,0.15,0.03), KF(1,225,0.15,0.05), {0},{0},{0}},
     {KF(0,210,0.6,0.50), KF(0.3,220,0.7,0.62), KF(0.6,230,0.65,0.68), KF(0.8,240,0.6,0.60), KF(1,225,0.55,0.55)},
     {KF(0,175,0.55,0.50), KF(0.4,190,0.6,0.62), KF(0.7,200,0.55,0.58), KF(1,185,0.5,0.52), {0}},
     {KF(0,195,0.5,0.48), KF(0.5,210,0.6,0.60), KF(1,200,0.5,0.52), {0},{0}},
     {KF(0,225,0.7,0.55), KF(0.3,240,0.75,0.68), KF(0.6,255,0.7,0.72), KF(0.8,245,0.65,0.65), KF(1,232,0.6,0.58)},
     {KF(0,260,0.55,0.48), KF(0.4,275,0.6,0.58), KF(0.7,285,0.55,0.55), KF(1,270,0.5,0.50), {0}},
     {KF(0,190,0.55,0.55), KF(0.3,200,0.6,0.68), KF(0.7,210,0.55,0.65), KF(1,195,0.5,0.58), {0}},
     {KF(0,215,0.1,0.68), KF(0.4,220,0.15,0.78), KF(0.7,225,0.1,0.82), KF(1,218,0.08,0.72), {0}},
    },

    /* 7: Putakwa — emerald jungle, teal, tropical */
    {
     {KF(0,160,0.15,0.03), KF(1,165,0.15,0.05), {0},{0},{0}},
     {KF(0,140,0.6,0.50), KF(0.3,150,0.7,0.62), KF(0.6,165,0.65,0.68), KF(0.8,155,0.6,0.60), KF(1,145,0.55,0.55)},
     {KF(0,120,0.65,0.52), KF(0.3,135,0.7,0.65), KF(0.6,150,0.7,0.70), KF(0.8,140,0.65,0.62), KF(1,128,0.6,0.55)},
     {KF(0,70,0.6,0.52), KF(0.4,85,0.65,0.62), KF(0.7,95,0.6,0.58), KF(1,80,0.55,0.54), {0}},
     {KF(0,175,0.6,0.48), KF(0.4,190,0.65,0.58), KF(0.7,200,0.6,0.55), KF(1,185,0.55,0.50), {0}},
     {KF(0,310,0.5,0.48), KF(0.4,325,0.55,0.58), KF(0.7,340,0.5,0.55), KF(1,320,0.45,0.50), {0}},
     {KF(0,165,0.6,0.55), KF(0.3,178,0.65,0.68), KF(0.7,188,0.6,0.65), KF(1,175,0.55,0.58), {0}},
     {KF(0,160,0.1,0.72), KF(0.4,168,0.15,0.82), KF(0.7,175,0.1,0.85), KF(1,165,0.08,0.76), {0}},
    },

    /* 8: Void — deep purple, violet nebula, cosmic */
    {
     {KF(0,280,0.15,0.03), KF(1,285,0.15,0.04), {0},{0},{0}},
     {KF(0,320,0.6,0.50), KF(0.3,335,0.65,0.62), KF(0.6,350,0.6,0.58), KF(1,330,0.55,0.52), {0}},
     {KF(0,270,0.5,0.48), KF(0.4,285,0.55,0.58), KF(0.7,295,0.5,0.55), KF(1,280,0.45,0.50), {0}},
     {KF(0,290,0.5,0.50), KF(0.5,305,0.55,0.60), KF(1,300,0.48,0.55), {0},{0}},
     {KF(0,255,0.6,0.48), KF(0.3,268,0.65,0.60), KF(0.7,280,0.6,0.65), KF(1,270,0.55,0.55), {0}},
     {KF(0,285,0.65,0.55), KF(0.3,300,0.7,0.68), KF(0.6,315,0.68,0.72), KF(0.8,305,0.62,0.65), KF(1,292,0.58,0.58)},
     {KF(0,240,0.45,0.52), KF(0.4,255,0.5,0.62), KF(0.7,265,0.48,0.60), KF(1,250,0.42,0.55), {0}},
     {KF(0,278,0.1,0.68), KF(0.4,285,0.12,0.78), KF(0.7,290,0.08,0.82), KF(1,282,0.06,0.72), {0}},
    },

    /* 9: Ozzrinom — vampire blood red, dark grey/blacks only */
    {
     /*blk*/ {KF(0,355,0.12,0.03), KF(1,0,0.12,0.04), {0},{0},{0}},
     /*red*/ {KF(0,355,0.80,0.45), KF(0.3,0,0.85,0.55), KF(0.6,5,0.80,0.60), KF(0.8,0,0.75,0.50), KF(1,355,0.70,0.45)},
     /*grn*/ {KF(0,350,0.45,0.35), KF(0.4,355,0.50,0.42), KF(0.7,0,0.45,0.40), KF(1,352,0.40,0.35), {0}},
     /*brn*/ {KF(0,352,0.50,0.38), KF(0.5,358,0.55,0.45), KF(1,355,0.48,0.40), {0},{0}},
     /*blu*/ {KF(0,348,0.40,0.30), KF(0.3,352,0.45,0.38), KF(0.6,356,0.42,0.35), KF(1,350,0.38,0.30), {0}},
     /*mag*/ {KF(0,345,0.55,0.40), KF(0.4,350,0.60,0.48), KF(0.7,355,0.55,0.45), KF(1,348,0.50,0.40), {0}},
     /*cyn*/ {KF(0,355,0.30,0.35), KF(0.4,0,0.35,0.42), KF(0.7,5,0.32,0.40), KF(1,358,0.28,0.36), {0}},
     /*wht*/ {KF(0,355,0.08,0.68), KF(0.4,0,0.10,0.75), KF(0.7,5,0.08,0.78), KF(1,358,0.06,0.72), {0}},
    },

    /* 10: Phoenix — fire orange, solar flare, molten gold */
    {
     {KF(0,20,0.2,0.04), KF(1,25,0.2,0.06), {0},{0},{0}},
     {KF(0,0,0.8,0.58), KF(0.25,10,0.85,0.72), KF(0.5,25,0.82,0.80), KF(0.75,40,0.78,0.85), KF(1,15,0.7,0.65)},
     {KF(0,50,0.7,0.55), KF(0.3,60,0.75,0.68), KF(0.6,45,0.7,0.72), KF(0.8,55,0.65,0.65), KF(1,48,0.6,0.58)},
     {KF(0,30,0.8,0.60), KF(0.3,42,0.85,0.75), KF(0.6,55,0.8,0.82), KF(0.8,45,0.75,0.78), KF(1,35,0.7,0.65)},
     {KF(0,15,0.6,0.45), KF(0.4,25,0.65,0.55), KF(0.7,35,0.6,0.52), KF(1,20,0.55,0.48), {0}},
     {KF(0,340,0.65,0.52), KF(0.4,350,0.7,0.62), KF(0.7,5,0.68,0.65), KF(1,355,0.6,0.58), {0}},
     {KF(0,25,0.6,0.62), KF(0.3,38,0.65,0.75), KF(0.7,50,0.6,0.72), KF(1,35,0.55,0.65), {0}},
     {KF(0,28,0.12,0.78), KF(0.3,35,0.18,0.88), KF(0.6,42,0.15,0.92), KF(1,32,0.1,0.82), {0}},
    },

    /* 11: Mad Wizard — electric cyan, neon, arcane */
    {
     {KF(0,190,0.15,0.03), KF(1,195,0.15,0.05), {0},{0},{0}},
     {KF(0,180,0.7,0.55), KF(0.25,190,0.8,0.70), KF(0.5,200,0.75,0.78), KF(0.75,210,0.7,0.72), KF(1,195,0.65,0.60)},
     {KF(0,140,0.65,0.52), KF(0.3,155,0.7,0.65), KF(0.7,170,0.68,0.70), KF(1,158,0.6,0.58), {0}},
     {KF(0,165,0.6,0.55), KF(0.5,180,0.7,0.68), KF(1,172,0.6,0.60), {0},{0}},
     {KF(0,210,0.65,0.52), KF(0.3,225,0.7,0.65), KF(0.7,240,0.68,0.62), KF(1,228,0.6,0.55), {0}},
     {KF(0,270,0.6,0.50), KF(0.4,285,0.65,0.62), KF(0.7,300,0.6,0.58), KF(1,282,0.55,0.52), {0}},
     {KF(0,185,0.7,0.60), KF(0.25,195,0.75,0.75), KF(0.5,205,0.72,0.80), KF(0.75,198,0.68,0.72), KF(1,190,0.62,0.65)},
     {KF(0,192,0.12,0.78), KF(0.3,198,0.18,0.88), KF(0.7,205,0.12,0.92), KF(1,195,0.08,0.82), {0}},
    },

    /* 12: Tasloi — deep forest, moss, woodland */
    {
     {KF(0,110,0.15,0.03), KF(1,115,0.15,0.05), {0},{0},{0}},
     {KF(0,80,0.6,0.45), KF(0.3,95,0.65,0.55), KF(0.7,110,0.6,0.52), KF(1,90,0.55,0.48), {0}},
     {KF(0,100,0.65,0.48), KF(0.25,115,0.7,0.60), KF(0.5,130,0.72,0.65), KF(0.75,120,0.68,0.58), KF(1,108,0.6,0.52)},
     {KF(0,60,0.6,0.48), KF(0.4,75,0.65,0.58), KF(0.7,85,0.6,0.55), KF(1,70,0.55,0.50), {0}},
     {KF(0,150,0.5,0.42), KF(0.5,165,0.55,0.52), KF(1,155,0.48,0.45), {0},{0}},
     {KF(0,330,0.45,0.42), KF(0.5,345,0.5,0.52), KF(1,335,0.42,0.45), {0},{0}},
     {KF(0,140,0.55,0.52), KF(0.3,155,0.6,0.62), KF(0.7,165,0.58,0.60), KF(1,150,0.52,0.55), {0}},
     {KF(0,115,0.08,0.65), KF(0.4,120,0.12,0.75), KF(0.7,125,0.08,0.78), KF(1,118,0.06,0.70), {0}},
    },

    /* 13: Frostborn — ice blue, arctic frost, glacial */
    {
     {KF(0,200,0.1,0.04), KF(1,205,0.1,0.05), {0},{0},{0}},
     {KF(0,195,0.45,0.58), KF(0.3,205,0.5,0.72), KF(0.6,215,0.48,0.78), KF(0.8,210,0.42,0.72), KF(1,200,0.4,0.62)},
     {KF(0,180,0.4,0.58), KF(0.4,192,0.48,0.70), KF(0.7,200,0.42,0.68), KF(1,188,0.38,0.62), {0}},
     {KF(0,190,0.35,0.55), KF(0.5,202,0.42,0.68), KF(1,195,0.38,0.60), {0},{0}},
     {KF(0,210,0.55,0.55), KF(0.3,222,0.6,0.70), KF(0.6,235,0.58,0.75), KF(0.8,228,0.52,0.68), KF(1,218,0.48,0.60)},
     {KF(0,250,0.4,0.52), KF(0.5,262,0.48,0.65), KF(1,255,0.42,0.58), {0},{0}},
     {KF(0,192,0.5,0.60), KF(0.3,202,0.55,0.75), KF(0.6,212,0.52,0.78), KF(1,205,0.45,0.65), {0}},
     {KF(0,200,0.06,0.82), KF(0.3,205,0.08,0.90), KF(0.7,210,0.05,0.92), KF(1,202,0.04,0.85), {0}},
    },

    /* 14: Sandstorm — golden sand, desert sun, dune */
    {
     {KF(0,40,0.2,0.04), KF(1,45,0.2,0.06), {0},{0},{0}},
     {KF(0,15,0.7,0.55), KF(0.3,25,0.75,0.68), KF(0.6,38,0.72,0.75), KF(0.8,30,0.68,0.70), KF(1,20,0.62,0.60)},
     {KF(0,55,0.65,0.52), KF(0.3,68,0.7,0.65), KF(0.7,78,0.65,0.62), KF(1,62,0.6,0.55), {0}},
     {KF(0,40,0.75,0.58), KF(0.25,50,0.8,0.72), KF(0.5,60,0.78,0.78), KF(0.75,52,0.72,0.72), KF(1,42,0.68,0.62)},
     {KF(0,25,0.5,0.45), KF(0.5,35,0.58,0.55), KF(1,30,0.52,0.48), {0},{0}},
     {KF(0,350,0.55,0.48), KF(0.5,5,0.6,0.58), KF(1,355,0.52,0.52), {0},{0}},
     {KF(0,42,0.6,0.60), KF(0.3,52,0.65,0.72), KF(0.7,62,0.6,0.70), KF(1,50,0.55,0.62), {0}},
     {KF(0,42,0.1,0.78), KF(0.3,48,0.15,0.88), KF(0.7,52,0.1,0.90), KF(1,45,0.08,0.82), {0}},
    },

    /* 15: Crystal Cavern — amethyst purple, crystal facets */
    {
     {KF(0,270,0.12,0.04), KF(1,275,0.12,0.05), {0},{0},{0}},
     {KF(0,290,0.6,0.52), KF(0.3,305,0.65,0.65), KF(0.6,318,0.62,0.70), KF(0.8,310,0.58,0.65), KF(1,298,0.55,0.55)},
     {KF(0,250,0.5,0.50), KF(0.4,265,0.55,0.62), KF(0.7,275,0.52,0.60), KF(1,260,0.48,0.52), {0}},
     {KF(0,265,0.52,0.52), KF(0.5,280,0.58,0.65), KF(1,272,0.52,0.58), {0},{0}},
     {KF(0,240,0.55,0.50), KF(0.3,255,0.62,0.65), KF(0.7,268,0.58,0.68), KF(1,252,0.52,0.55), {0}},
     {KF(0,280,0.65,0.55), KF(0.25,295,0.72,0.70), KF(0.5,310,0.68,0.75), KF(0.75,300,0.65,0.68), KF(1,288,0.58,0.58)},
     {KF(0,225,0.45,0.55), KF(0.4,240,0.5,0.65), KF(0.7,250,0.48,0.62), KF(1,235,0.42,0.58), {0}},
     {KF(0,270,0.08,0.75), KF(0.3,278,0.12,0.85), KF(0.7,285,0.08,0.88), KF(1,275,0.06,0.80), {0}},
    },

    /* 16: Afroman / Giovanni — RED WHITE BLUE patriot, always R-W-B with slight hue shifts */
    {
     /*blk*/ {KF(0,230,0.2,0.05), KF(1,225,0.15,0.07), {0},{0},{0}},
     /*red*/ {KF(0,0,0.90,0.72), KF(0.25,0,0.06,0.95), KF(0.5,220,0.85,0.68), KF(0.75,355,0.08,0.93), KF(1,5,0.88,0.70)},
     /*grn*/ {KF(0,355,0.88,0.70), KF(0.25,355,0.07,0.93), KF(0.5,225,0.83,0.66), KF(0.75,0,0.06,0.94), KF(1,350,0.86,0.68)},
     /*yel*/ {KF(0,5,0.86,0.74), KF(0.25,5,0.05,0.96), KF(0.5,215,0.82,0.65), KF(0.75,350,0.07,0.92), KF(1,0,0.90,0.72)},
     /*blu*/ {KF(0,220,0.85,0.68), KF(0.25,0,0.06,0.94), KF(0.5,0,0.88,0.70), KF(0.75,355,0.06,0.95), KF(1,225,0.83,0.66)},
     /*mag*/ {KF(0,350,0.87,0.71), KF(0.25,350,0.06,0.94), KF(0.5,230,0.84,0.67), KF(0.75,5,0.07,0.93), KF(1,355,0.89,0.72)},
     /*cyn*/ {KF(0,225,0.83,0.66), KF(0.25,355,0.07,0.93), KF(0.5,5,0.87,0.71), KF(0.75,0,0.06,0.95), KF(1,220,0.84,0.68)},
     /*wht*/ {KF(0,0,0.06,0.94), KF(0.25,0,0.85,0.70), KF(0.5,355,0.06,0.96), KF(0.75,225,0.82,0.66), KF(1,5,0.05,0.95)},
    },

    /* 17: Bog Lord — murky marsh, sickly green, swamp gas */
    {
     {KF(0,90,0.2,0.03), KF(1,95,0.2,0.05), {0},{0},{0}},
     {KF(0,60,0.55,0.40), KF(0.3,75,0.6,0.50), KF(0.7,90,0.55,0.48), KF(1,72,0.5,0.42), {0}},
     {KF(0,85,0.6,0.42), KF(0.25,100,0.65,0.55), KF(0.5,115,0.68,0.58), KF(0.75,105,0.62,0.52), KF(1,92,0.58,0.45)},
     {KF(0,45,0.55,0.40), KF(0.4,58,0.6,0.50), KF(0.7,70,0.55,0.48), KF(1,55,0.5,0.42), {0}},
     {KF(0,120,0.45,0.35), KF(0.5,135,0.5,0.45), KF(1,128,0.42,0.38), {0},{0}},
     {KF(0,320,0.4,0.35), KF(0.5,335,0.45,0.45), KF(1,325,0.38,0.38), {0},{0}},
     {KF(0,110,0.5,0.45), KF(0.3,125,0.55,0.55), KF(0.7,138,0.52,0.52), KF(1,120,0.48,0.48), {0}},
     {KF(0,95,0.08,0.55), KF(0.4,100,0.12,0.65), KF(0.7,105,0.08,0.68), KF(1,98,0.06,0.60), {0}},
    },
    }; /* end specs */

    for (int ansi = 0; ansi < 8; ansi++) {
        /* Count valid keyframes (non-zero position or first entry) */
        int nk = 0;
        for (int k = 0; k < 5; k++) {
            if (k == 0 || specs[theme_idx][ansi][k][0] > 0.001f ||
                specs[theme_idx][ansi][k][2] > 0.001f ||
                specs[theme_idx][ansi][k][3] > 0.001f)
                nk = k + 1;
        }
        if (nk < 2) nk = 2;

        /* Build base ramp */
        build_ramp(&theme_ramps[ansi], nk, specs[theme_idx][ansi]);

        /* Afroman override: each ANSI color gets its OWN unique R-W-B gradient.
         * All start at a unique red, flow through a unique white, into a unique blue,
         * back through white, and repeat. Same ANSI color = same gradient always. */
        /* Build bright variant (ansi + 8): boost so peak channel hits 1.0.
         * This gives maximum dim/bright contrast for every theme. */
        color_ramp_t bright;
        /* Find the peak value across all stops in the dim ramp */
        float peak = 0.001f;
        for (int s = 0; s < GRAD_STOPS; s++) {
            if (theme_ramps[ansi].stops[s].r > peak) peak = theme_ramps[ansi].stops[s].r;
            if (theme_ramps[ansi].stops[s].g > peak) peak = theme_ramps[ansi].stops[s].g;
            if (theme_ramps[ansi].stops[s].b > peak) peak = theme_ramps[ansi].stops[s].b;
        }
        float boost = 1.0f / peak; /* scale so peak → 1.0 */
        if (boost < 1.3f) boost = 1.3f; /* minimum 30% boost even if already near 1.0 */
        for (int s = 0; s < GRAD_STOPS; s++) {
            bright.stops[s].r = theme_ramps[ansi].stops[s].r * boost;
            bright.stops[s].g = theme_ramps[ansi].stops[s].g * boost;
            bright.stops[s].b = theme_ramps[ansi].stops[s].b * boost;
            if (bright.stops[s].r > 1.0f) bright.stops[s].r = 1.0f;
            if (bright.stops[s].g > 1.0f) bright.stops[s].g = 1.0f;
            if (bright.stops[s].b > 1.0f) bright.stops[s].b = 1.0f;
        }
        theme_ramps[ansi + 8] = bright;
    }

    /* Afroman override: ALL 16 ANSI colors get their own WILDLY distinct R-W-B gradient.
     * Each entry: {red_rgb, white_rgb, blue_rgb} — must look completely different. */
    if (theme_idx == 16) {
        static const float afro[16][3][3] = {
            /* 0  dark black:  dim brick,         grey-white,       dim slate */
            {{0.40f,0.08f,0.08f}, {0.65f,0.65f,0.68f}, {0.15f,0.18f,0.40f}},
            /* 1  red:         neon orange-red,    warm cream,       electric blue */
            {{1.00f,0.35f,0.05f}, {1.00f,0.95f,0.82f}, {0.10f,0.40f,1.00f}},
            /* 2  green:       hot magenta-pink,   icy blue-white,   deep indigo */
            {{0.95f,0.10f,0.50f}, {0.85f,0.90f,1.00f}, {0.22f,0.08f,0.65f}},
            /* 3  yellow:      bright scarlet,     golden white,     teal-blue */
            {{0.95f,0.15f,0.05f}, {1.00f,0.96f,0.75f}, {0.05f,0.55f,0.72f}},
            /* 4  blue:        deep maroon,        cool silver,      vivid cyan */
            {{0.55f,0.02f,0.08f}, {0.85f,0.88f,0.92f}, {0.10f,0.72f,0.95f}},
            /* 5  magenta:     coral/salmon,        pink-white,       dark navy */
            {{1.00f,0.45f,0.30f}, {1.00f,0.88f,0.92f}, {0.08f,0.10f,0.50f}},
            /* 6  cyan:        wine/burgundy,      mint-white,       bright azure */
            {{0.62f,0.05f,0.22f}, {0.85f,1.00f,0.95f}, {0.15f,0.48f,0.95f}},
            /* 7  white:       pale rose,          brilliant white,  pale periwinkle */
            {{0.95f,0.68f,0.68f}, {1.00f,1.00f,1.00f}, {0.68f,0.68f,0.95f}},
            /* 8  bright black: muted rust,        ash white,        slate blue */
            {{0.50f,0.18f,0.10f}, {0.75f,0.75f,0.78f}, {0.25f,0.28f,0.52f}},
            /* 9  bright red:  NEON orange,        hot white,        laser blue */
            {{1.00f,0.50f,0.00f}, {1.00f,1.00f,0.90f}, {0.00f,0.35f,1.00f}},
            /* 10 bright green: vivid fuchsia,     lilac white,      purple */
            {{1.00f,0.20f,0.65f}, {0.92f,0.88f,1.00f}, {0.35f,0.12f,0.80f}},
            /* 11 bright yellow: fire engine red,  lemon white,      ocean teal */
            {{1.00f,0.22f,0.08f}, {1.00f,1.00f,0.80f}, {0.00f,0.62f,0.78f}},
            /* 12 bright blue: oxblood,            ice blue white,   neon cyan */
            {{0.65f,0.05f,0.12f}, {0.88f,0.95f,1.00f}, {0.00f,0.85f,1.00f}},
            /* 13 bright mag:  peach/apricot,      blush white,      midnight blue */
            {{1.00f,0.60f,0.45f}, {1.00f,0.93f,0.95f}, {0.05f,0.08f,0.55f}},
            /* 14 bright cyan: raspberry,          seafoam white,    sky blue */
            {{0.78f,0.08f,0.35f}, {0.88f,1.00f,0.98f}, {0.20f,0.60f,1.00f}},
            /* 15 bright white: lightest blush,    pure white,       lightest lavender */
            {{1.00f,0.85f,0.85f}, {1.00f,1.00f,1.00f}, {0.85f,0.85f,1.00f}},
        };
        for (int c = 0; c < 16; c++) {
            const float *cr = afro[c][0];
            const float *cw = afro[c][1];
            const float *cb = afro[c][2];
            float cy[10][3];
            /* 0: dark red, 1: full red */
            cy[0][0]=cr[0]*0.65f; cy[0][1]=cr[1]*0.65f; cy[0][2]=cr[2]*0.65f;
            cy[1][0]=cr[0];       cy[1][1]=cr[1];       cy[1][2]=cr[2];
            /* 2: red→white blend, 3: full white */
            cy[2][0]=(cr[0]+cw[0])*0.5f; cy[2][1]=(cr[1]+cw[1])*0.5f; cy[2][2]=(cr[2]+cw[2])*0.5f;
            cy[3][0]=cw[0];              cy[3][1]=cw[1];              cy[3][2]=cw[2];
            /* 4: white→blue blend, 5: full blue */
            cy[4][0]=(cw[0]+cb[0])*0.5f; cy[4][1]=(cw[1]+cb[1])*0.5f; cy[4][2]=(cw[2]+cb[2])*0.5f;
            cy[5][0]=cb[0];              cy[5][1]=cb[1];              cy[5][2]=cb[2];
            /* 6: dark blue, 7: full blue again */
            cy[6][0]=cb[0]*0.65f; cy[6][1]=cb[1]*0.65f; cy[6][2]=cb[2]*0.65f;
            cy[7][0]=cb[0];       cy[7][1]=cb[1];       cy[7][2]=cb[2];
            /* 8: blue→white, 9: white→red */
            cy[8][0]=(cb[0]+cw[0])*0.5f; cy[8][1]=(cb[1]+cw[1])*0.5f; cy[8][2]=(cb[2]+cw[2])*0.5f;
            cy[9][0]=(cw[0]+cr[0])*0.5f; cy[9][1]=(cw[1]+cr[1])*0.5f; cy[9][2]=(cw[2]+cr[2])*0.5f;
            for (int s = 0; s < GRAD_STOPS; s++) {
                theme_ramps[c].stops[s].r = cy[s][0];
                theme_ramps[c].stops[s].g = cy[s][1];
                theme_ramps[c].stops[s].b = cy[s][2];
            }
        }
    }

    /* Set flat palette to first stop (for box-drawing, UI elements, bg fills) */
    for (int i = 0; i < 16; i++)
        tinted_palette[i] = theme_ramps[i].stops[GRAD_STOPS / 2];
    palette = tinted_palette;

    #undef K5
    #undef KF
}
static int fs_res_idx = 0;     /* fullscreen resolution index */
static int fs_width = 1920;
static int fs_height = 1080;
static float ui_scale = 1.0f; /* DPI scale: vp_h/1080, updated each frame */
static int term_margin = 0;  /* left margin in 1080p pixels, scaled by ui_scale */


/* ---- Font system ----
 * current_font: -1 = original CP437 bitmap, 0+ = TTF font index.
 * TTF fonts are rasterized at runtime via stb_truetype at a pixel size
 * matched to the screen resolution. Box-drawing chars (0xB0-0xDF) are
 * always drawn programmatically regardless of font. */
static int current_font = -1;
static uint32_t cur_atlas_w = 576, cur_atlas_h = 1024;
static volatile int font_change_pending = 0;
static int pending_font_idx = -1;
static int term_font_adj = 0;  /* pixel adjustment to cell_h (-8..+16) */

/* CP437 to Unicode mapping for TTF rasterization */
static const unsigned int cp437_to_unicode[256] = {
    0x0000, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC,
    0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027,
    0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037,
    0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047,
    0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057,
    0x0058, 0x0059, 0x005A, 0x005B, 0x005C, 0x005D, 0x005E, 0x005F,
    0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067,
    0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077,
    0x0078, 0x0079, 0x007A, 0x007B, 0x007C, 0x007D, 0x007E, 0x007F,
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

/* TTF font table — name + path relative to DLL directory */
#define MAX_TTF_FONTS 32
typedef struct {
    const char *name;     /* display name in menu */
    const char *filename; /* .ttf filename in fonts/ subdir */
} ttf_font_entry_t;

static const ttf_font_entry_t ttf_fonts[] = {
    /* ---- Category: Bitmapped Terminal Fonts ---- */
    { "--= Bitmapped Terminal Fonts =--", NULL },
    { "IBM VGA 8x16",           "Px437_IBM_VGA_8x16.ttf" },
    { "IBM VGA 9x16",           "Px437_IBM_VGA_9x16.ttf" },
    { "IBM EGA 8x14",           "Px437_IBM_EGA_8x14.ttf" },
    { "IBM MDA",                "Px437_IBM_MDA.ttf" },
    { "IBM CGA",                "Px437_IBM_CGA.ttf" },
    { "Phoenix VGA",            "Px437_PhoenixVGA_8x16.ttf" },
    { "Compaq Portable",        "Px437_Compaq_Port3.ttf" },
    { "AT&T PC6300",            "Px437_ATT_PC6300.ttf" },
    { "Amstrad PC",             "Px437_Amstrad_PC.ttf" },
    { "Unscii 16",              "unscii-16.ttf" },
    { "Unscii Fantasy",         "unscii-8-fantasy.ttf" },
    { "Unscii MCR",             "unscii-8-mcr.ttf" },
    { "Unscii Tall",            "unscii-8-tall.ttf" },
    { "Unscii Thin",            "unscii-8-thin.ttf" },
    /* ---- Category: TTF Terminal Fonts ---- */
    { "-= TTF Terminal Fonts =-", NULL },
    { "JetBrains Mono",         "JetBrainsMono-Regular.ttf" },
    { "JetBrains Mono Bold",    "JetBrainsMono-Bold.ttf" },
    { "Fira Code",              "FiraCode-Regular.ttf" },
    { "Hack",                   "Hack-Regular.ttf" },
    { "Hack Bold",              "Hack-Bold.ttf" },
    { "Iosevka Fixed",          "IosevkaFixed-Regular.ttf" },
    { "DejaVu Sans Mono Bold",  "DejaVuSansMono-Bold.ttf" },
    { "Source Code Pro Medium",  "SourceCodePro-Medium.ttf" },
    { "Source Code Pro Semi",    "SourceCodePro-Semibold.ttf" },
    { "Source Code Pro Black",   "SourceCodePro-Black.ttf" },
    { "Terminus",               "TerminusTTF-Regular.ttf" },
    /* ---- Category: Fixed Width Fonts ---- */
    { "-= Fixed Width Fonts =-", NULL },
    { "Space Mono",             "SpaceMono-Regular.ttf" },
    { "Space Mono Bold",        "SpaceMono-Bold.ttf" },
    { "B612 Mono",              "B612Mono-Regular.ttf" },
    { "Share Tech Mono",        "ShareTechMono-Regular.ttf" },
    { "Overpass Mono",          "OverpassMono-Regular.ttf" },
    { "Victor Mono",            "VictorMono-Regular.ttf" },
    { "Fantasque Sans Mono",    "FantasqueSansMono-Regular.ttf" },
    { "Fantasque Sans Bold",    "FantasqueSansMono-Bold.ttf" },
    { "Comic Mono",             "ComicMono.ttf" },
    { "Comic Mono Bold",        "ComicMono-Bold.ttf" },
};
#define NUM_TTF_FONTS (int)(sizeof(ttf_fonts) / sizeof(ttf_fonts[0]))

/* Returns 1 if font is a bitmap-style font (CP437 built-in, Px437 TTFs, Unscii TTFs).
 * These have hard alpha edges that cause artifact lines in the liquid shader's
 * neighbor-sampling passes (shadow blur, dark outline, sobel). */
static int is_pixel_font(int font_idx)
{
    if (font_idx < 0) return 1; /* built-in CP437 bitmap */
    if (font_idx >= NUM_TTF_FONTS) return 0;
    const char *fn = ttf_fonts[font_idx].filename;
    if (!fn) return 0;
    /* Px437_* and unscii-* are bitmap-style TTFs with sharp alpha edges */
    if (fn[0] == 'P' && fn[1] == 'x' && fn[2] == '4') return 1;
    if (fn[0] == 'u' && fn[1] == 'n' && fn[2] == 's') return 1;
    return 0;
}

/* Cached TTF font data (loaded from disk once) */
static unsigned char *ttf_file_data[MAX_TTF_FONTS] = {0};
static int ttf_loaded[MAX_TTF_FONTS] = {0};

/* ---- Vulkan-rendered context menu ---- */
#define VKM_CLOSED     0
#define VKM_ROOT       1

/* Root menu item indices */
#define VKM_ITEM_VISUAL  0   /* Visual > (Theme, Font, FX) */
#define VKM_ITEM_WIDGETS 1   /* Widgets > (Round Timer, Conversation) */
#define VKM_ITEM_SEP     2
#define VKM_ITEM_MEGASETTINGS 3   /* MegaMUD Settings — opens settings window */
#define VKM_ITEM_PATHS   4   /* Paths & Loops — opens VKW window */
#define VKM_ITEM_RECENT  5   /* Recent > last 10 goto destinations */
#define VKM_ITEM_EXTRAS  6   /* Settings > (Sound, Color, HideMM) */
#define VKM_ITEM_XTRA2   7   /* Extras > (Visualizations) */
#define VKM_ITEM_SEP2    8
#define VKM_ITEM_CLOSE   9
#define VKM_ROOT_COUNT   10

/* Submenu types */
#define VKM_SUB_NONE     0
#define VKM_SUB_VISUAL   1
#define VKM_SUB_WIDGETS  2
#define VKM_SUB_RECENT   3
#define VKM_SUB_THEME    4   /* 2nd-level under Visual */
#define VKM_SUB_FONT     5
#define VKM_SUB_FX       6

/* Visual submenu items */
#define VKM_VIS_THEME    0
#define VKM_VIS_FONT     1
#define VKM_VIS_FX       2
#define VKM_VIS_COLOR    3
#define VKM_VIS_COUNT    4

/* Widgets submenu items */
#define VKM_WID_RTIMER   0
#define VKM_WID_CONVO    1
#define VKM_WID_STATUSBAR 2
#define VKM_WID_EXPBAR   3
#define VKM_WID_PSTATS   4   /* Player Statistics */
#define VKM_WID_RADIO    5   /* MUDRadio */
#define VKM_WID_BSCROLL  6   /* Backscroll */
#define VKM_WID_ICONBAR  7   /* MegaMUD Icon Bar */
#define VKM_WID_COUNT    8

/* FX submenu items */
#define VKM_FX_LIQUID    0
#define VKM_FX_WAVES     1
#define VKM_FX_FBM       2
#define VKM_FX_SOBEL     3
#define VKM_FX_SCANLINES 4
#define VKM_FX_SMOKY     5
#define VKM_FX_SHADOW    6
#define VKM_FX_RAIN      7
#define VKM_FX_CRAWL     8
#define VKM_FX_WOBBLE    9
#define VKM_FX_COUNT     10  /* wobble only shown when vk_experimental=True */

/* Settings submenu items */
#define VKM_SUB_EXTRAS   7
#define VKM_EXT_SOUND    0   /* Sound Settings */
#define VKM_EXT_COLOR    1   /* Color/Brightness */
#define VKM_EXT_SEP1     2   /* separator */
#define VKM_EXT_HIDEMM   3   /* Hide/Show MegaMUD */
#define VKM_EXT_RNDRECAP 4   /* Show Combat Round Totals */
#define VKM_EXT_HDR      5   /* HDR Toggle */
#define VKM_EXT_SEP2     6   /* separator */
#define VKM_EXT_CONSOLE  7   /* MMUDPy Console */
#define VKM_EXT_SCRIPTMGR 8  /* Script Manager */
#define VKM_EXT_SEP3     9   /* separator */
#define VKM_EXT_SAVE     10  /* Save Settings */
#define VKM_EXT_RESET    11  /* Reset to Defaults */
#define VKM_EXT_SEP4     12  /* separator */
#define VKM_EXT_PAIRANDROID 13  /* Pair Android Device (QR) */
#define VKM_EXT_COUNT    14

/* Extras submenu items */
#define VKM_SUB_XTRA2    8
#define VKM_X2_VIZ       0   /* Visualizations > */
#define VKM_X2_BG        1   /* Backgrounds > */
#define VKM_X2_SEP1      2   /* separator */
#define VKM_X2_RMSTEP    3   /* RM Every Step toggle (auto-room after walks) */
#define VKM_X2_MAPWALK   4   /* Map Walker window toggle */
#define VKM_X2_LOADDB    5   /* Load Map DB… (MDB file picker) */
#define VKM_X2_RMDEBUG   6   /* Show full RM debug text (bypass rm filter) */
#define VKM_X2_COUNT     7

/* Visualizations 3rd-level submenu */
#define VKM_SUB_VIZ      9
#define VKM_VIZ_BLADES   0
#define VKM_VIZ_CBALLS   1
#define VKM_VIZ_MATRIX   2
#define VKM_VIZ_VECTROIDS 3
#define VKM_VIZ_COUNT    4

/* Backgrounds 3rd-level submenu */
#define VKM_SUB_BG         10
#define VKM_BG_SETTINGS    0  /* First item: opens unified Background Settings dialog */
#define VKM_BG_PLASMA      1
#define VKM_BG_STARFIELD   2
#define VKM_BG_ALIENWALL   3
#define VKM_BG_COUNT       4

static int megamud_hidden = 0; /* 1 = MegaMUD windows hidden, VK-only mode */

/* ---- RM Every Step bridge (lives in msimg32_proxy.dll) ----
 * The real toggle + polling thread is in msimg32_proxy.c. We resolve two
 * small exports lazily so the menu can display the current state and flip
 * it without duplicating the struct_base / OFF_CUR_PATH_STEP logic. */
typedef int  (WINAPI *pfn_rm_get_t)(void);
typedef void (WINAPI *pfn_rm_set_t)(int);
typedef unsigned int (WINAPI *pfn_room_cksum_t)(void);
typedef unsigned int (WINAPI *pfn_location_t)(void);
typedef int  (WINAPI *pfn_auto_rm_q_t)(void);
static pfn_rm_get_t pfn_rm_every_step_get = NULL;
static pfn_rm_set_t pfn_rm_every_step_set = NULL;
static pfn_room_cksum_t pfn_get_room_checksum = NULL;
static pfn_location_t   pfn_get_location = NULL;
static pfn_auto_rm_q_t  pfn_auto_rm_queue_peek = NULL;
static pfn_auto_rm_q_t  pfn_auto_rm_queue_consume = NULL;
typedef void (WINAPI *pfn_auto_rm_inc_t)(void);
static pfn_auto_rm_inc_t pfn_auto_rm_queue_increment = NULL;

typedef struct {
    unsigned int   cksum;
    unsigned short flags;
    char           dir[32];
} dynpath_step_t;
typedef int (WINAPI *pfn_dynpath_inject_t)(dynpath_step_t *steps, int count,
                                           unsigned int src, unsigned int dst);
static pfn_dynpath_inject_t pfn_dynpath_inject = NULL;

typedef void (WINAPI *pfn_dynpath_set_suspend_t)(int);
typedef int  (WINAPI *pfn_dynpath_get_suspend_t)(void);
static pfn_dynpath_set_suspend_t pfn_dynpath_set_suspend_events = NULL;
static pfn_dynpath_get_suspend_t pfn_dynpath_get_suspend_events = NULL;

typedef void (WINAPI *pfn_dynpath_get_progress_t)(int *, int *, int *);
static pfn_dynpath_get_progress_t pfn_dynpath_get_progress = NULL;

typedef void (WINAPI *pfn_dynpath_set_label_t)(const char *);
static pfn_dynpath_set_label_t pfn_dynpath_set_label = NULL;

typedef int (WINAPI *pfn_get_stat_t)(void);
static pfn_get_stat_t pfn_get_player_strength  = NULL;
static pfn_get_stat_t pfn_get_player_picklocks = NULL;
static int          rm_bridge_resolved = 0;

static void rm_bridge_resolve(void)
{
    if (rm_bridge_resolved) return;
    rm_bridge_resolved = 1;
    /* msimg32 is our host DLL — it's guaranteed to be loaded before us. */
    HMODULE mm = GetModuleHandleA("msimg32.dll");
    if (!mm) mm = GetModuleHandleA("msimg32");
    if (!mm) return;
    pfn_rm_every_step_get = (pfn_rm_get_t)GetProcAddress(mm, "rm_every_step_get");
    pfn_rm_every_step_set = (pfn_rm_set_t)GetProcAddress(mm, "rm_every_step_set");
    pfn_get_room_checksum = (pfn_room_cksum_t)GetProcAddress(mm, "get_room_checksum");
    pfn_get_location      = (pfn_location_t)GetProcAddress(mm, "get_location");
    pfn_auto_rm_queue_peek    = (pfn_auto_rm_q_t)GetProcAddress(mm, "auto_rm_queue_peek");
    pfn_auto_rm_queue_consume = (pfn_auto_rm_q_t)GetProcAddress(mm, "auto_rm_queue_consume");
    pfn_auto_rm_queue_increment = (pfn_auto_rm_inc_t)GetProcAddress(mm, "auto_rm_queue_increment");
    pfn_dynpath_inject = (pfn_dynpath_inject_t)GetProcAddress(mm, "dynpath_inject");
    pfn_dynpath_set_suspend_events = (pfn_dynpath_set_suspend_t)GetProcAddress(mm, "dynpath_set_suspend_events");
    pfn_dynpath_get_suspend_events = (pfn_dynpath_get_suspend_t)GetProcAddress(mm, "dynpath_get_suspend_events");
    pfn_get_player_strength  = (pfn_get_stat_t)GetProcAddress(mm, "get_player_strength");
    pfn_get_player_picklocks = (pfn_get_stat_t)GetProcAddress(mm, "get_player_picklocks");
    pfn_dynpath_get_progress = (pfn_dynpath_get_progress_t)GetProcAddress(mm, "dynpath_get_progress");
    pfn_dynpath_set_label = (pfn_dynpath_set_label_t)GetProcAddress(mm, "dynpath_set_label");
}

static int auto_rm_queue_peek_safe(void)
{
    rm_bridge_resolve();
    return pfn_auto_rm_queue_peek ? pfn_auto_rm_queue_peek() : 0;
}

static int auto_rm_queue_consume_safe(void)
{
    rm_bridge_resolve();
    return pfn_auto_rm_queue_consume ? pfn_auto_rm_queue_consume() : 0;
}

static int rm_every_step_get_safe(void)
{
    rm_bridge_resolve();
    return pfn_rm_every_step_get ? pfn_rm_every_step_get() : 0;
}

static void rm_every_step_toggle_safe(void)
{
    rm_bridge_resolve();
    if (!pfn_rm_every_step_get || !pfn_rm_every_step_set) return;
    pfn_rm_every_step_set(pfn_rm_every_step_get() ? 0 : 1);
}

static unsigned int get_room_checksum_safe(void)
{
    rm_bridge_resolve();
    return pfn_get_room_checksum ? pfn_get_room_checksum() : 0;
}

/* Returns packed (map << 16) | room from the live RM parse, or 0 if unknown. */
static unsigned int get_location_safe(void)
{
    rm_bridge_resolve();
    return pfn_get_location ? pfn_get_location() : 0;
}

/* ---- PTT Voice Overlay ---- */
/* Pointers into voice.dll exported data (resolved lazily) */
static volatile float *vox_level_ptr = NULL;
static volatile int   *vox_active_ptr = NULL;
static char           *vox_text_ptr = NULL;
static volatile DWORD *vox_text_tick_ptr = NULL;
static float          *vox_wave_ptr = NULL;
static volatile int   *vox_wave_pos_ptr = NULL;
static int vox_resolved = 0;
static float vox_fade = 0.0f; /* fade-out alpha after PTT release */

static void vox_resolve(void) {
    if (vox_resolved) return;
    HMODULE vm = GetModuleHandleA("voice.dll");
    if (!vm) vm = LoadLibraryA("voice.dll");
    if (!vm) { vox_resolved = -1; return; }
    vox_level_ptr     = (volatile float *)GetProcAddress(vm, "voice_audio_level");
    vox_active_ptr    = (volatile int *)GetProcAddress(vm, "voice_ptt_active");
    vox_text_ptr      = (char *)GetProcAddress(vm, "voice_last_text");
    vox_text_tick_ptr = (volatile DWORD *)GetProcAddress(vm, "voice_last_text_tick");
    vox_wave_ptr      = (float *)GetProcAddress(vm, "voice_waveform");
    vox_wave_pos_ptr  = (volatile int *)GetProcAddress(vm, "voice_wave_pos");
    vox_resolved = (vox_level_ptr && vox_active_ptr) ? 1 : -1;
}

/* QR pairing overlay state — functions defined after push_solid/push_text forward declarations */
typedef const char *(*pfn_ab_conn_str_t)(void);
typedef int (*pfn_ab_gen_qr_t)(void);
typedef int (*pfn_ab_qr_size_t)(void);
typedef int (*pfn_ab_qr_mod_t)(int, int);
typedef int (*pfn_ab_client_count_t)(void);

static pfn_ab_conn_str_t    pfn_ab_conn_str    = NULL;
static pfn_ab_gen_qr_t      pfn_ab_gen_qr      = NULL;
static pfn_ab_qr_size_t     pfn_ab_qr_size     = NULL;
static pfn_ab_qr_mod_t      pfn_ab_qr_mod      = NULL;
static pfn_ab_client_count_t pfn_ab_client_count = NULL;
static int ab_resolved = 0;
static int qr_visible = 0;
static int qr_paired  = 0;
static DWORD qr_paired_tick = 0;
static DWORD qr_show_tick   = 0;
static char qr_conn_str[256] = {0};
#define QR_EXPIRY_MS 120000

static void ab_resolve(void)
{
    if (ab_resolved) return;
    ab_resolved = -1;
    HMODULE h = GetModuleHandleA("android_bridge.dll");
    if (!h) h = GetModuleHandleA("android_bridge");
    if (!h) h = LoadLibraryA("plugins\\android_bridge.dll");
    if (!h) return;
    pfn_ab_conn_str    = (pfn_ab_conn_str_t)GetProcAddress(h, "ab_get_connection_string");
    pfn_ab_gen_qr      = (pfn_ab_gen_qr_t)GetProcAddress(h, "ab_generate_qr");
    pfn_ab_qr_size     = (pfn_ab_qr_size_t)GetProcAddress(h, "ab_qr_get_size");
    pfn_ab_qr_mod      = (pfn_ab_qr_mod_t)GetProcAddress(h, "ab_qr_get_module");
    pfn_ab_client_count = (pfn_ab_client_count_t)GetProcAddress(h, "ab_client_count");
    if (pfn_ab_conn_str && pfn_ab_gen_qr && pfn_ab_qr_size && pfn_ab_qr_mod)
        ab_resolved = 1;
}

static void qr_show(void);
static void qr_draw(int vp_w, int vp_h);
static int  qr_mouse_down(int mx, int my);

/* Sound settings panel state */
static int   snd_visible = 0;
static float snd_x = 80, snd_y = 60;
static float snd_w = 440, snd_h = 520;
static int   snd_dragging = 0;
static float snd_drag_ox = 0, snd_drag_oy = 0;
static int   snd_active_slider = -1;  /* 0 = volume */
static int   snd_scroll = 0;
static void  snd_open_window(void);
static void  snd_draw(int vp_w, int vp_h);
static int   snd_mouse_down(int mx, int my);
static void  snd_mouse_move(int mx, int my);
static void  snd_mouse_up(void);
/* Forward declaration for slider helper used by multiple panels */
static void clr_draw_slider(float x0, float y0, float w, float h,
                             float val, float vmin, float vmax,
                             const char *label, const char *fmt_val,
                             int vp_w, int vp_h, const ui_theme_t *t);
static int  pst_visible = 0;
static int  pst_round_recap = 1;  /* Show Combat Round Totals in terminal */
static char pst_last_recap[256] = "";  /* plain-text last round recap for MMUDPy */
static int  pst_recap_dmg = 0, pst_recap_hits = 0, pst_recap_crits = 0;
static int  pst_recap_extra = 0, pst_recap_spell = 0, pst_recap_bs = 0;
static int  pst_recap_miss = 0, pst_recap_glance = 0, pst_recap_dodge = 0;
static int  pst_recap_seq = 0;  /* increments each recap, MMUDPy can detect new ones */
static float pst_x = 100.0f, pst_y = 60.0f;
static float pst_w = 340.0f, pst_h = 390.0f;
static int  pst_dragging = 0;
static float pst_drag_ox, pst_drag_oy;

/* ---- Backscroll Panel ---- */
#define BSP_MODE_PLAIN     0
#define BSP_MODE_ANSI_HTML 1
#define BSP_MODE_THEME_HTML 2
#define BSP_MODE_RAW_ANSI  3
static int   bsp_visible = 0;
static float bsp_x = 80.0f, bsp_y = 40.0f;
static float bsp_w = 680.0f, bsp_h = 500.0f;
static int   bsp_dragging = 0;
static float bsp_drag_ox, bsp_drag_oy;
static int   bsp_mode = BSP_MODE_PLAIN;
static int   bsp_scroll = 0;
static int   bsp_resizing = 0;
/* Text selection state */
static int   bsp_selecting = 0;     /* 1 = drag-selecting text */
static int   bsp_sel_active = 0;    /* 1 = selection exists (highlight visible) */
static int   bsp_sel_start_line = 0, bsp_sel_start_col = 0; /* anchor */
static int   bsp_sel_end_line = 0,   bsp_sel_end_col = 0;   /* current */
/* Cached layout for mouse→cell mapping */
static float bsp_disp_y0 = 0, bsp_disp_y1 = 0;
static int   bsp_disp_lx = 0;    /* text left x */
static int   bsp_disp_start = 0; /* first visible line index */
static int   bsp_disp_vis = 0;   /* number of visible lines */
static int   bsp_disp_total = 0; /* total lines */
/* Snapshot state — frozen on open */
static int   bsp_snap_plain_lines = 0;
static int   bsp_snap_raw_lines = 0;
static int   bsp_snap_cb_count = 0;
static int   bsp_snap_cb_head = 0;
static int   bsp_snap_bs_len = 0;
static int   bsp_snap_bs_head = 0;
static int   bsp_snap_ra_len = 0;
static int   bsp_snap_ra_head = 0;
static void  bsp_draw(int vp_w, int vp_h);
static void  bsp_toggle(int vp_w, int vp_h);
static int   bsp_mouse_down(int mx, int my);
static void  bsp_mouse_move(int mx, int my);
static void  bsp_mouse_up(void);
static void  bsp_save(void);
static void  bsp_copy_selection(void);

/* MUDRadio types + state + forward declarations */
#include "mudradio.h"
#include "vib_icons.h"
static void pst_draw(int vp_w, int vp_h);
static void pst_toggle(void);
static void stw_draw(int vp_w, int vp_h, int mx, int my, int clicked);
static void stw_toggle(void);
static void vft_draw(int vp_w, int vp_h);
static int  vft_quad_start, vft_quad_end;
static int  stw_visible;
static float stw_x, stw_y, stw_w, stw_h;
static int  stw_scaled;
static int  stw_dragging;
static float stw_drag_ox, stw_drag_oy;
static void pst_feed(const char *data, int len);
static void pst_reset_exp(void);
static void pst_reset_combat(void);
static void pst_recap_poll(void);
static void pst_flush_round(void);
static void pst_on_round_tick(int round_num);
static int vib_visible; /* forward decl for icon bar visibility */

static int vkm_open = 0;          /* VKM_CLOSED or VKM_ROOT */
static void vkt_save_settings(void);
static void vkt_reset_settings(void);
static int vkm_x = 0, vkm_y = 0; /* root menu top-left in pixels */
static int vkm_hover = -1;        /* hovered root item */
static int vkm_sub = VKM_SUB_NONE;/* which submenu is expanded */
static int vkm_sub_hover = -1;    /* hovered submenu item */
static int vkm_mouse_x = 0, vkm_mouse_y = 0;
static int vkm_hover_drill = -1;  /* sub item being hovered for drill-in */
static DWORD vkm_hover_tick = 0;  /* when hover started on that item */
static int vkm_hover_root = -1;   /* root item being hovered for sub-open */
static DWORD vkm_hover_root_tick = 0;
#define VKM_HOVER_DELAY_MS 500    /* ms to wait before auto-drilling into submenu */

/* Recent GOTO destinations (rotating list, newest first) */
#define VKM_GOTO_MAX 10
static char vkm_goto_names[VKM_GOTO_MAX][64];
static int  vkm_goto_count = 0;

/* Add a destination to the recent list (dedup + promote to top) */
static void vkm_goto_add(const char *name)
{
    if (!name || !name[0]) return;

    /* Check if already in list — if so, promote to position 0 */
    for (int i = 0; i < vkm_goto_count; i++) {
        if (_stricmp(vkm_goto_names[i], name) == 0) {
            /* Shift entries above it down to close the gap */
            char tmp[64];
            memcpy(tmp, vkm_goto_names[i], 64);
            for (int j = i; j > 0; j--)
                memcpy(vkm_goto_names[j], vkm_goto_names[j-1], 64);
            memcpy(vkm_goto_names[0], tmp, 64);
            return;
        }
    }

    /* Not in list — shift everything down and insert at 0 */
    int limit = vkm_goto_count < VKM_GOTO_MAX ? vkm_goto_count : VKM_GOTO_MAX - 1;
    for (int i = limit; i > 0; i--)
        memcpy(vkm_goto_names[i], vkm_goto_names[i-1], 64);
    strncpy(vkm_goto_names[0], name, 63);
    vkm_goto_names[0][63] = 0;
    if (vkm_goto_count < VKM_GOTO_MAX) vkm_goto_count++;
}

/* Mouse coordinate scaling: window client area may differ from Vulkan surface
 * extent under Wine/XWayland with DPI scaling. All mouse coords must be
 * transformed from window-space to render-space. */
static float mouse_scale_x = 1.0f, mouse_scale_y = 1.0f;
static int mouse_off_x = 0, mouse_off_y = 0;  /* client area offset from window origin */
static void mouse_update_scale(int surface_w, int surface_h) {
    if (!vkt_hwnd) return;
    RECT rc, wrc;
    GetClientRect(vkt_hwnd, &rc);
    GetWindowRect(vkt_hwnd, &wrc);
    /* Client area offset within the window (title bar, borders under Wine) */
    POINT pt = {0, 0};
    ClientToScreen(vkt_hwnd, &pt);
    mouse_off_x = pt.x - wrc.left;
    mouse_off_y = pt.y - wrc.top;
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw > 0 && ch > 0) {
        mouse_scale_x = (float)surface_w / (float)cw;
        mouse_scale_y = (float)surface_h / (float)ch;
    }
    if (api) api->log("[vk_terminal] mouse_scale=%.3f,%.3f off=%d,%d client=%dx%d surface=%dx%d\n",
                      mouse_scale_x, mouse_scale_y, mouse_off_x, mouse_off_y, cw, ch, surface_w, surface_h);
}
static inline int mouse_tx(int wx) { return (int)(wx * mouse_scale_x); }
static inline int mouse_ty(int wy) { return (int)(wy * mouse_scale_y); }

/* Menu rendering constants (base values at 1080p, scaled by ui_scale) */
#define VKM_ITEM_H_BASE   32
#define VKM_PAD_BASE      10
#define VKM_ROOT_W_BASE   210
#define VKM_SUB_W_BASE    280
#define VKM_SEP_H_BASE    12
#define VKM_CHAR_W   10   /* menu text character width */
#define VKM_CHAR_H   20   /* menu text character height */
#define VKM_SHADOW_BASE   6    /* drop shadow offset */
/* Scaled menu dimensions — updated when ui_scale changes */
static int VKM_ITEM_H, VKM_PAD, VKM_ROOT_W, VKM_SUB_W, VKM_SEP_H, VKM_SHADOW;
static void vkm_update_scale(void) {
    VKM_ITEM_H = (int)(VKM_ITEM_H_BASE * ui_scale);
    VKM_PAD    = (int)(VKM_PAD_BASE * ui_scale);
    VKM_ROOT_W = (int)(VKM_ROOT_W_BASE * ui_scale);
    VKM_SUB_W  = (int)(VKM_SUB_W_BASE * ui_scale);
    VKM_SEP_H  = (int)(VKM_SEP_H_BASE * ui_scale);
    VKM_SHADOW = (int)(VKM_SHADOW_BASE * ui_scale);
}

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
#define VKW_LINE_LEN     512  /* enough for a full gossip line without truncation */
#define VKW_TITLEBAR_H   24
#define VKW_BORDER_W     1
#define VKW_SHADOW_OFF   6
#define VKW_CHAR_W       10
#define VKW_CHAR_H       18
#define VKW_PAD          4
#define VKW_CLOSE_W      20     /* close button width */
#define VKW_RESIZE_ZONE  6      /* pixels from edge = resize grab */
#define VKW_CORNER_ZONE  14     /* larger zone at corners for diagonal resize */
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
    int  line_chan[VKW_MAX_LINES]; /* chat channel tag per line (-1=none, 0+=CHAT_*) */
    int  line_split[VKW_MAX_LINES]; /* char offset where message text starts (after tag+name) */
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

    /* Text selection (line-based) */
    int sel_active;             /* currently dragging a selection */
    int sel_start_line;         /* first selected line (visual index from bottom) */
    int sel_end_line;           /* last selected line */
    int has_selection;          /* selection exists (for rendering highlight) */

    /* Font scale (1.0 = base VKW_CHAR_W/H, scales with window resize) */
    float font_scale;

    /* Interaction state */
    int dragging;
    float drag_ox, drag_oy;
    int resizing;
    int resize_edge;  /* bitmask: 1=left, 2=right, 4=top, 8=bottom */
    float resize_ox, resize_oy, resize_sw, resize_sh, resize_sx, resize_sy;
} vkw_window_t;

static vkw_window_t vkw_windows[VKW_MAX_WINDOWS];
static int vkw_order[VKW_MAX_WINDOWS];  /* z-order: order[0]=back, order[n-1]=front */
static int vkw_count = 0;
static int vkw_next_id = 1;
static int vkw_focus = -1;             /* index into vkw_windows[], -1 = terminal has focus */
static vkw_input_cb_t vkw_input_callback = NULL;

/* Console window shortcut */
static int vkw_console_idx = -1;
/* Conversation window shortcut */
static int vkw_convo_idx = -1;
/* Backscroll window shortcut (defined later, declared here) */
static int vkw_backscroll_idx = -1;

/* Forward decl for convo input handler */
static void convo_on_input(const char *text);

/* ---- Experimental VK plugins ---- */
static int vk_experimental = 0;

/* ---- Compiz-style mass-spring wobble grid ---- */
static int fx_wobble_mode = 0;

/* Compiz wobbly windows: 8x8 grid, underdamped springs (bouncy!),
 * 4 substeps per frame for stability at high stiffness.
 * Anchor spring pulls each point to its rest position.
 * Edge springs connect cardinal + diagonal neighbors for shear resistance.
 * Damping ratio ζ ≈ 0.3 → ~2-3 visible bounces before settling. */
#define WOB_GRID_W  8
#define WOB_GRID_H  8
#define WOB_GRID_N  (WOB_GRID_W * WOB_GRID_H)
#define WOB_K       600.0f  /* anchor spring stiffness — high for snappy response */
#define WOB_K_EDGE  400.0f  /* neighbor spring stiffness */
#define WOB_K_DIAG  200.0f  /* diagonal spring stiffness (shear resistance) */
#define WOB_DAMP    8.0f    /* velocity damping — low for bouncy underdamped feel */
#define WOB_DT      0.004f  /* substep timestep (4 substeps × 0.004 ≈ 0.016s/frame) */
#define WOB_SUBSTEPS 4

typedef struct {
    float px, py;   /* current position */
    float vx, vy;   /* velocity */
    float rx, ry;   /* rest position */
    int pinned;     /* 1 = follows mouse exactly */
} wob_point_t;

typedef struct {
    wob_point_t pts[WOB_GRID_N];
    int active;
    int grab_row;
    float prev_x, prev_y;
} wob_grid_t;

static wob_grid_t wob_grids[VKW_MAX_WINDOWS];
static wob_grid_t wob_grid_rt;

static int wob_warp_active = 0;
static wob_grid_t *wob_warp_grid = NULL;
static float wob_warp_x, wob_warp_y, wob_warp_w, wob_warp_h;

static void wob_init_grid(wob_grid_t *g, float x, float y, float w, float h)
{
    for (int r = 0; r < WOB_GRID_H; r++) {
        for (int c = 0; c < WOB_GRID_W; c++) {
            wob_point_t *p = &g->pts[r * WOB_GRID_W + c];
            p->rx = x + w * (float)c / (float)(WOB_GRID_W - 1);
            p->ry = y + h * (float)r / (float)(WOB_GRID_H - 1);
            p->px = p->rx; p->py = p->ry;
            p->vx = p->vy = 0;
            p->pinned = 0;
        }
    }
    g->active = 0;
    g->grab_row = 0;
    g->prev_x = x; g->prev_y = y;
}

static void wob_move_rest(wob_grid_t *g, float x, float y, float w, float h)
{
    for (int r = 0; r < WOB_GRID_H; r++)
        for (int c = 0; c < WOB_GRID_W; c++) {
            wob_point_t *p = &g->pts[r * WOB_GRID_W + c];
            p->rx = x + w * (float)c / (float)(WOB_GRID_W - 1);
            p->ry = y + h * (float)r / (float)(WOB_GRID_H - 1);
        }
}

static void wob_pin_top(wob_grid_t *g)
{
    for (int c = 0; c < WOB_GRID_W; c++) {
        wob_point_t *p = &g->pts[c];
        p->pinned = 1;
        p->px = p->rx; p->py = p->ry;
        p->vx = p->vy = 0;
    }
}

static void wob_unpin(wob_grid_t *g)
{
    for (int i = 0; i < WOB_GRID_N; i++)
        g->pts[i].pinned = 0;
}

/* Add spring force between point i and neighbor at (nr,nc) */
static void wob_spring(wob_grid_t *g, float forces[][2], int i, int nr, int nc, float k)
{
    if (nr < 0 || nr >= WOB_GRID_H || nc < 0 || nc >= WOB_GRID_W) return;
    int j = nr * WOB_GRID_W + nc;
    wob_point_t *a = &g->pts[i], *b = &g->pts[j];
    /* Force = k * ((actual_offset) - (rest_offset)) */
    float dx = (b->px - a->px) - (b->rx - a->rx);
    float dy = (b->py - a->py) - (b->ry - a->ry);
    forces[i][0] += k * dx;
    forces[i][1] += k * dy;
}

static void wob_simulate(wob_grid_t *g)
{
    if (!g->active) return;

    for (int sub = 0; sub < WOB_SUBSTEPS; sub++) {
        float forces[WOB_GRID_N][2];
        for (int i = 0; i < WOB_GRID_N; i++)
            forces[i][0] = forces[i][1] = 0;

        for (int r = 0; r < WOB_GRID_H; r++) {
            for (int c = 0; c < WOB_GRID_W; c++) {
                int i = r * WOB_GRID_W + c;
                wob_point_t *p = &g->pts[i];
                if (p->pinned) continue;

                /* Anchor spring — pulls toward rest position */
                forces[i][0] += WOB_K * (p->rx - p->px);
                forces[i][1] += WOB_K * (p->ry - p->py);

                /* Cardinal neighbor springs (structural) */
                wob_spring(g, forces, i, r-1, c, WOB_K_EDGE);
                wob_spring(g, forces, i, r+1, c, WOB_K_EDGE);
                wob_spring(g, forces, i, r, c-1, WOB_K_EDGE);
                wob_spring(g, forces, i, r, c+1, WOB_K_EDGE);

                /* Diagonal springs (shear resistance — prevents parallelogram) */
                wob_spring(g, forces, i, r-1, c-1, WOB_K_DIAG);
                wob_spring(g, forces, i, r-1, c+1, WOB_K_DIAG);
                wob_spring(g, forces, i, r+1, c-1, WOB_K_DIAG);
                wob_spring(g, forces, i, r+1, c+1, WOB_K_DIAG);

                /* Velocity damping */
                forces[i][0] -= WOB_DAMP * p->vx;
                forces[i][1] -= WOB_DAMP * p->vy;
            }
        }

        /* Semi-implicit Euler (velocity first, then position — more stable) */
        for (int i = 0; i < WOB_GRID_N; i++) {
            wob_point_t *p = &g->pts[i];
            if (p->pinned) { p->px = p->rx; p->py = p->ry; continue; }
            p->vx += forces[i][0] * WOB_DT;
            p->vy += forces[i][1] * WOB_DT;
            p->px += p->vx * WOB_DT;
            p->py += p->vy * WOB_DT;
        }
    }

    /* Check if settled (all velocities and displacements small) */
    int settled = 1;
    for (int i = 0; i < WOB_GRID_N; i++) {
        wob_point_t *p = &g->pts[i];
        float dx = p->px - p->rx, dy = p->py - p->ry;
        if (p->vx > 0.5f || p->vx < -0.5f || p->vy > 0.5f || p->vy < -0.5f ||
            dx > 0.5f || dx < -0.5f || dy > 0.5f || dy < -0.5f)
            settled = 0;
    }
    if (settled) {
        for (int i = 0; i < WOB_GRID_N; i++) {
            g->pts[i].px = g->pts[i].rx;
            g->pts[i].py = g->pts[i].ry;
            g->pts[i].vx = g->pts[i].vy = 0;
        }
        g->active = 0;
    }
}

static void wob_transform(float in_x, float in_y, float *out_x, float *out_y)
{
    if (!wob_warp_grid || wob_warp_w < 1 || wob_warp_h < 1) {
        *out_x = in_x; *out_y = in_y; return;
    }
    float u = (in_x - wob_warp_x) / wob_warp_w;
    float v = (in_y - wob_warp_y) / wob_warp_h;
    if (u < 0) u = 0; if (u > 1) u = 1;
    if (v < 0) v = 0; if (v > 1) v = 1;

    float gx = u * (WOB_GRID_W - 1);
    float gy = v * (WOB_GRID_H - 1);
    int cx = (int)gx; if (cx >= WOB_GRID_W - 1) cx = WOB_GRID_W - 2;
    int cy = (int)gy; if (cy >= WOB_GRID_H - 1) cy = WOB_GRID_H - 2;
    float fx = gx - cx;
    float fy = gy - cy;

    wob_point_t *tl = &wob_warp_grid->pts[cy * WOB_GRID_W + cx];
    wob_point_t *tr = &wob_warp_grid->pts[cy * WOB_GRID_W + cx + 1];
    wob_point_t *bl = &wob_warp_grid->pts[(cy+1) * WOB_GRID_W + cx];
    wob_point_t *br = &wob_warp_grid->pts[(cy+1) * WOB_GRID_W + cx + 1];

    *out_x = tl->px*(1-fx)*(1-fy) + tr->px*fx*(1-fy) + bl->px*(1-fx)*fy + br->px*fx*fy;
    *out_y = tl->py*(1-fx)*(1-fy) + tr->py*fx*(1-fy) + bl->py*(1-fx)*fy + br->py*fx*fy;
}

/* Floating window right-click context menu */
#define VKW_CTX_FONT_UP    0
#define VKW_CTX_FONT_DN    1
#define VKW_CTX_FONT_RST   2
#define VKW_CTX_COUNT      3
static int  vkw_ctx_open = 0;
static int  vkw_ctx_wnd = -1;   /* which window index */
static int  vkw_ctx_x = 0, vkw_ctx_y = 0;
static int  vkw_ctx_hover = -1;

/* Chat channel types */
#define CHAT_GOSSIP    0
#define CHAT_BROADCAST 1
#define CHAT_GANGPATH  2
#define CHAT_TELEPATH  3
#define CHAT_AUCTION   4
#define CHAT_SAY       5
#define CHAT_YELL      6
#define CHAT_NUM       7

static const char *chat_chan_tags[] = {
    "[Gos]", "[Broad]", "[Gang]", "[Tell]", "[Auct]", "[Say]", "[Yell]"
};

/* Per-channel colors: {tag_r, tag_g, tag_b, msg_r, msg_g, msg_b}
 * Raw saturated values — no theme blending at draw time, so channels
 * are visually distinct the same way MegaMUD's native convo window is. */
static const float chat_chan_colors[][6] = {
    /* GOSSIP    — bright cyan  */ { 0.40f, 1.00f, 1.00f,  0.80f, 1.00f, 1.00f },
    /* BROADCAST — bright yellow*/ { 1.00f, 1.00f, 0.30f,  1.00f, 1.00f, 0.80f },
    /* GANGPATH  — tan/brown    */ { 1.00f, 0.75f, 0.35f,  1.00f, 0.88f, 0.65f },
    /* TELEPATH  — magenta/pink */ { 1.00f, 0.45f, 1.00f,  1.00f, 0.80f, 1.00f },
    /* AUCTION   — gold         */ { 1.00f, 0.85f, 0.25f,  1.00f, 0.95f, 0.60f },
    /* SAY       — soft white   */ { 0.85f, 0.90f, 1.00f,  0.95f, 0.95f, 1.00f },
    /* YELL      — bright red   */ { 1.00f, 0.35f, 0.35f,  1.00f, 0.65f, 0.60f },
};

static int convo_out_channel = CHAT_SAY;
static char convo_tell_target[32] = {0};

/* Outgoing telepath tracking — two-part: capture message from input,
 * then match "--- Telepath Sent to FullName ---" within ~20 lines */
static char  tell_pending_msg[256] = {0};  /* buffered message text */
static int   tell_pending = 0;             /* 1 = waiting for confirmation */
static int   tell_lines_waited = 0;        /* lines since we started waiting */
#define TELL_MAX_WAIT 20                   /* discard if no confirm in N lines */

static const char *chat_send_prefix[] = {
    "gossip ", "broadcast ", "gangpath ", "telepath ",
    "auction ", "say ", "yell "
};

/* Forward decls */
static void vkw_print(int idx, const char *text);
static char *bs_get_text(int *out_len);

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

/* Forward declaration — implemented after the SLOP command table */
static int vkt_try_slop_dispatch(const char *input);

static void vkt_eval_python(const char *code, int target_wnd_id)
{
    /* Try SLOP command dispatch first (smoke.help(), bg.set_mode(1), etc.) */
    if (vkt_try_slop_dispatch(code)) return;

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
    win->opacity = 0.96f;
    win->font_scale = ui_scale;
    win->has_input = has_input;

    /* Initialize wobble grid */
    wob_init_grid(&wob_grids[idx], (float)x, (float)y, (float)w, (float)h);

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
    if (vkw_backscroll_idx == idx) vkw_backscroll_idx = -1;

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

static void vkw_print_ex(int idx, const char *text, int chan, int split)
{
    if (idx < 0 || idx >= VKW_MAX_WINDOWS || !vkw_windows[idx].active) return;
    vkw_window_t *w = &vkw_windows[idx];

    /* Split text on newlines, write each as a line */
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len >= VKW_LINE_LEN) len = VKW_LINE_LEN - 1;
        /* Strip ANSI escape sequences (\x1b[...m) since VKW renderer
         * can't display them — they'd show as garbage characters */
        {
            char stripped[VKW_LINE_LEN];
            int si = 0;
            for (int ri = 0; ri < len; ri++) {
                if (p[ri] == '\x1b' && ri + 1 < len && p[ri + 1] == '[') {
                    /* Skip \x1b[ ... until 'm' or end */
                    ri += 2;
                    while (ri < len && p[ri] != 'm') ri++;
                    continue; /* skip the 'm' too */
                }
                if (si < VKW_LINE_LEN - 1) stripped[si++] = p[ri];
            }
            stripped[si] = 0;
            memcpy(w->lines[w->line_head], stripped, si + 1);
        }
        w->line_chan[w->line_head] = chan;
        w->line_split[w->line_head] = split;
        w->line_head = (w->line_head + 1) % VKW_MAX_LINES;
        if (w->line_count < VKW_MAX_LINES) w->line_count++;
        p += len + (nl ? 1 : len); /* advance past newline or to end */
        if (!nl) break;
    }
    w->scroll = 0; /* auto-scroll to bottom */
}

static void vkw_print(int idx, const char *text)
{
    vkw_print_ex(idx, text, -1, 0);
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

    /* Scaled font dimensions */
    int cw = (int)(VKW_CHAR_W * w->font_scale);
    int ch = (int)(VKW_CHAR_H * w->font_scale);
    if (cw < 4) cw = 4;
    if (ch < 8) ch = 8;
    int input_h = ch + 4;
    int titlebar_h = ch + 8;

    /* Drop shadow */
    push_solid(ix + VKW_SHADOW_OFF, iy + VKW_SHADOW_OFF,
               ix + iw + VKW_SHADOW_OFF, iy + ih + VKW_SHADOW_OFF,
               0.0f, 0.0f, 0.0f, 0.5f * op, vp_w, vp_h);

    /* Window background */
    push_solid(ix, iy, ix + iw, iy + ih,
               t->bg[0], t->bg[1], t->bg[2], op, vp_w, vp_h);

    /* Title bar — glossy gradient style matching MUDRadio/Player Stats */
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float tb_r = acr * 0.25f + bgr * 0.5f;
    float tb_g = acg * 0.25f + bgg * 0.5f;
    float tb_b = acb * 0.25f + bgb * 0.5f;
    if (!is_focused) {
        tb_r = bgr + 0.06f; tb_g = bgg + 0.06f; tb_b = bgb + 0.06f;
    }
    int tb_y0 = iy + 2, tb_y1 = iy + titlebar_h;
    /* Title bar background */
    push_solid(ix + 2, tb_y0, ix + iw - 2, tb_y1,
               tb_r, tb_g, tb_b, 0.95f * op, vp_w, vp_h);
    /* Gloss highlight (top half) */
    push_solid(ix + 2, tb_y0, ix + iw - 2, tb_y0 + titlebar_h / 2,
               1.0f, 1.0f, 1.0f, 0.06f * op, vp_w, vp_h);
    /* Bottom edge accent line */
    push_solid(ix + 2, tb_y1 - 1, ix + iw - 2, tb_y1,
               acr, acg, acb, 0.5f * op, vp_w, vp_h);

    /* Title text with drop shadow */
    int ttx = ix + VKW_PAD, tty = tb_y0 + (titlebar_h - 2 - ch) / 2;
    push_text(ttx + 1, tty + 1, w->title, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text(ttx, tty, w->title, t->text[0], t->text[1], t->text[2],
              vp_w, vp_h, cw, ch);

    /* Close button [X] */
    int close_w = cw + 12;
    int cbx = ix + iw - close_w - 2;
    int cby = iy + 2;
    push_solid(cbx, cby, cbx + close_w, cby + titlebar_h - 4,
               0.6f, 0.15f, 0.15f, op, vp_w, vp_h);
    push_text(cbx + (close_w - cw) / 2,
              cby + (titlebar_h - 4 - ch) / 2,
              "X", 1.0f, 0.8f, 0.8f, vp_w, vp_h, cw, ch);

    /* Thick beveled border — 3px with light/dark edges for depth */
    int bw = 3; /* border thickness */
    float br = t->accent[0] * 0.5f, bg_ = t->accent[1] * 0.5f, bb = t->accent[2] * 0.5f;
    if (is_focused) { br = t->accent[0] * 0.8f; bg_ = t->accent[1] * 0.8f; bb = t->accent[2] * 0.8f; }
    /* Outer highlight (top-left = lighter for bevel) */
    float hl = 1.3f, sh = 0.5f;
    float hr = br*hl, hg = bg_*hl, hb = bb*hl;
    float sr = br*sh, sg = bg_*sh, sb = bb*sh;
    if (hr > 1.0f) hr = 1.0f; if (hg > 1.0f) hg = 1.0f; if (hb > 1.0f) hb = 1.0f;
    /* Top edge (highlight) */
    push_solid(ix, iy, ix + iw, iy + bw, hr, hg, hb, op, vp_w, vp_h);
    /* Left edge (highlight) */
    push_solid(ix, iy, ix + bw, iy + ih, hr, hg, hb, op, vp_w, vp_h);
    /* Bottom edge (shadow) */
    push_solid(ix, iy + ih - bw, ix + iw, iy + ih, sr, sg, sb, op, vp_w, vp_h);
    /* Right edge (shadow) */
    push_solid(ix + iw - bw, iy, ix + iw, iy + ih, sr, sg, sb, op, vp_w, vp_h);
    /* Middle border line for definition */
    push_solid(ix + 1, iy + 1, ix + iw - 1, iy + 2, br, bg_, bb, op, vp_w, vp_h);
    push_solid(ix + 1, iy + ih - 2, ix + iw - 1, iy + ih - 1, br, bg_, bb, op, vp_w, vp_h);
    push_solid(ix + 1, iy + 1, ix + 2, iy + ih - 1, br, bg_, bb, op, vp_w, vp_h);
    push_solid(ix + iw - 2, iy + 1, ix + iw - 1, iy + ih - 1, br, bg_, bb, op, vp_w, vp_h);

    /* Resize grip — bottom-right corner: 3 diagonal dots */
    {
        float gr = t->text[0] * 0.4f, gg = t->text[1] * 0.4f, gb = t->text[2] * 0.4f;
        int gx = ix + iw - 3, gy = iy + ih - 3;
        for (int d = 0; d < 3; d++) {
            int dx = gx - d * 4, dy = gy - (2 - d) * 4;
            push_solid(dx - 2, dy - 2, dx, dy, gr, gg, gb, 0.6f, vp_w, vp_h);
        }
        /* Additional dots for fuller grip pattern */
        push_solid(gx - 2 - 4, gy - 2, gx - 4, gy, gr, gg, gb, 0.6f, vp_w, vp_h);
        push_solid(gx - 2, gy - 2 - 4, gx, gy - 4, gr, gg, gb, 0.6f, vp_w, vp_h);
    }

    /* Content area */
    int content_y = iy + titlebar_h + 1;
    int content_h = ih - titlebar_h - 1;
    if (w->has_input) content_h -= input_h;
    int visible_lines = content_h / ch;
    int max_chars = (iw - VKW_PAD * 2) / cw;
    if (max_chars >= VKW_LINE_LEN) max_chars = VKW_LINE_LEN - 1;

    /* Normalize selection range */
    int sel_lo = -1, sel_hi = -1;
    if (w->has_selection) {
        sel_lo = w->sel_start_line < w->sel_end_line ? w->sel_start_line : w->sel_end_line;
        sel_hi = w->sel_start_line > w->sel_end_line ? w->sel_start_line : w->sel_end_line;
    }

    /* Draw text lines (bottom-up from most recent) with word-wrap.
     * Each stored line may span multiple visual rows when it exceeds
     * the window's character width. Walk stored lines from newest to
     * oldest, compute chunks top-down, then blit chunks bottom-up into
     * the remaining visible rows until the viewport is filled. */
    {
        int row_y = content_y + content_h - ch;
        int stored_off = w->scroll;
        int vis_row = 0; /* counts visual rows drawn (for selection mapping) */
        while (row_y >= content_y && stored_off < w->line_count) {
            int line_idx = w->line_head - 1 - stored_off;
            while (line_idx < 0) line_idx += VKW_MAX_LINES;
            line_idx %= VKW_MAX_LINES;

            const char *full = w->lines[line_idx];
            int full_len = (int)strlen(full);
            int chan = w->line_chan[line_idx];
            int split = w->line_split[line_idx];

            /* Compute chunk boundaries: [start_0, start_1, ..., start_N=full_len] */
            #define VKW_MAX_CHUNKS 32
            int chunk_start[VKW_MAX_CHUNKS + 1];
            int n_chunks = 0;
            if (max_chars < 1) max_chars = 1;
            {
                int pos = 0;
                chunk_start[0] = 0;
                while (pos < full_len && n_chunks < VKW_MAX_CHUNKS) {
                    int remaining = full_len - pos;
                    if (remaining <= max_chars) { n_chunks++; break; }
                    int brk = pos + max_chars;
                    /* Try to break at a space, no more than half the width back */
                    int min_brk = pos + max_chars / 2;
                    int probe = brk;
                    while (probe > min_brk && full[probe] != ' ') probe--;
                    if (full[probe] == ' ') brk = probe;
                    n_chunks++;
                    pos = brk;
                    /* Skip the space at the break so the next row doesn't start with " " */
                    while (pos < full_len && full[pos] == ' ') pos++;
                    chunk_start[n_chunks] = pos;
                }
                chunk_start[n_chunks] = full_len;
            }

            /* Pick channel colors — use raw saturated values so channels
             * are visually distinct (MegaMUD-style), no theme blending. */
            const float *cc = (chan >= 0 && chan < CHAT_NUM) ? chat_chan_colors[chan] : NULL;
            float tr0 = cc ? cc[0] : t->text[0] * 0.9f;
            float tg0 = cc ? cc[1] : t->text[1] * 0.9f;
            float tb0 = cc ? cc[2] : t->text[2] * 0.9f;
            float tr1 = cc ? cc[3] : t->text[0] * 0.9f;
            float tg1 = cc ? cc[4] : t->text[1] * 0.9f;
            float tb1 = cc ? cc[5] : t->text[2] * 0.9f;

            /* Draw chunks bottom-up */
            for (int c = n_chunks - 1; c >= 0 && row_y >= content_y; c--) {
                int cs = chunk_start[c];
                int ce = chunk_start[c + 1];
                int clen = ce - cs;
                if (clen > VKW_LINE_LEN - 1) clen = VKW_LINE_LEN - 1;

                /* Selection highlight */
                if (w->has_selection && vis_row >= sel_lo && vis_row <= sel_hi) {
                    push_solid(ix + 1, row_y, ix + iw - 1, row_y + ch,
                               t->accent[0], t->accent[1], t->accent[2], 0.25f, vp_w, vp_h);
                }

                char tmp[VKW_LINE_LEN];
                memcpy(tmp, full + cs, clen);
                tmp[clen] = 0;

                if (cc && split > 0) {
                    if (split <= cs) {
                        /* Whole chunk is past the split — message color */
                        push_text(ix + VKW_PAD, row_y, tmp, tr1, tg1, tb1, vp_w, vp_h, cw, ch);
                    } else if (split >= ce) {
                        /* Whole chunk is before the split — tag color */
                        push_text(ix + VKW_PAD, row_y, tmp, tr0, tg0, tb0, vp_w, vp_h, cw, ch);
                    } else {
                        int local_split = split - cs;
                        char tag_part[VKW_LINE_LEN];
                        memcpy(tag_part, tmp, local_split);
                        tag_part[local_split] = 0;
                        push_text(ix + VKW_PAD, row_y, tag_part, tr0, tg0, tb0, vp_w, vp_h, cw, ch);
                        push_text(ix + VKW_PAD + local_split * cw, row_y,
                                  tmp + local_split, tr1, tg1, tb1, vp_w, vp_h, cw, ch);
                    }
                } else {
                    push_text(ix + VKW_PAD, row_y, tmp, tr0, tg0, tb0, vp_w, vp_h, cw, ch);
                }
                row_y -= ch;
                vis_row++;
            }
            #undef VKW_MAX_CHUNKS
            stored_off++;
        }
    }

    /* Input line */
    if (w->has_input) {
        int iy2 = iy + ih - input_h;
        /* Input bg */
        push_solid(ix + 1, iy2, ix + iw - 1, iy + ih - 1,
                   t->bg[0] + 0.03f, t->bg[1] + 0.03f, t->bg[2] + 0.03f, op, vp_w, vp_h);
        /* Separator */
        push_solid(ix + 1, iy2, ix + iw - 1, iy2 + 1,
                   br * 0.5f, bg_ * 0.5f, bb * 0.5f, op, vp_w, vp_h);
        /* Prompt */
        push_text(ix + VKW_PAD, iy2 + (input_h - ch) / 2,
                  ">>>", t->accent[0], t->accent[1], t->accent[2],
                  vp_w, vp_h, cw, ch);
        /* Input text */
        if (w->input_len > 0) {
            push_text(ix + VKW_PAD + cw * 4,
                      iy2 + (input_h - ch) / 2,
                      w->input, t->text[0], t->text[1], t->text[2],
                      vp_w, vp_h, cw, ch);
        }
        /* Cursor */
        if (is_focused) {
            int cur_x = ix + VKW_PAD + cw * (4 + w->input_cursor);
            push_solid(cur_x, iy2 + 2, cur_x + cw, iy2 + input_h - 2,
                       t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        }
    }
}

static void vkw_draw_all(int vp_w, int vp_h)
{
    /* Simulate all active wobble grids */
    if (fx_wobble_mode) {
        for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
            if (!vkw_windows[i].active) continue;
            wob_grid_t *g = &wob_grids[i];
            /* Update rest positions to match current window rect */
            wob_move_rest(g, vkw_windows[i].x, vkw_windows[i].y,
                          vkw_windows[i].w, vkw_windows[i].h);
            /* Pin top row while dragging */
            if (vkw_windows[i].dragging) {
                wob_pin_top(g);
                g->active = 1;
            } else {
                wob_unpin(g);
            }
            wob_simulate(g);
        }
    }

    /* Draw back-to-front in z-order */
    for (int i = 0; i < vkw_count; i++) {
        int idx = vkw_order[i];
        if (vkw_windows[idx].active) {
            /* Enable wobble warp for this window's draw calls */
            if (fx_wobble_mode && wob_grids[idx].active) {
                wob_warp_active = 1;
                wob_warp_grid = &wob_grids[idx];
                wob_warp_x = vkw_windows[idx].x;
                wob_warp_y = vkw_windows[idx].y;
                wob_warp_w = vkw_windows[idx].w;
                wob_warp_h = vkw_windows[idx].h;
            }
            vkw_draw_one(&vkw_windows[idx], (idx == vkw_focus), vp_w, vp_h);
            wob_warp_active = 0;
            wob_warp_grid = NULL;
        }
    }

    /* Floating window context menu (drawn on top) */
    if (vkw_ctx_open && vkw_ctx_wnd >= 0) {
        const ui_theme_t *t = &ui_themes[current_theme];
        int mcw = 10, mch = 18;
        int item_h = mch + 6;
        int mw = 14 * mcw + 12;  /* width for labels */
        int mh = VKW_CTX_COUNT * item_h + 4;
        int mx0 = vkw_ctx_x, my0 = vkw_ctx_y;

        /* Keep on screen */
        if (mx0 + mw > vp_w) mx0 = vp_w - mw;
        if (my0 + mh > vp_h) my0 = vp_h - mh;

        /* Shadow */
        push_solid(mx0 + 3, my0 + 3, mx0 + mw + 3, my0 + mh + 3,
                   0, 0, 0, 0.4f, vp_w, vp_h);
        /* Background */
        push_solid(mx0, my0, mx0 + mw, my0 + mh,
                   t->bg[0] + 0.08f, t->bg[1] + 0.08f, t->bg[2] + 0.08f, 0.95f,
                   vp_w, vp_h);
        /* Border */
        push_solid(mx0, my0, mx0 + mw, my0 + 1, t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_solid(mx0, my0 + mh - 1, mx0 + mw, my0 + mh, t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_solid(mx0, my0, mx0 + 1, my0 + mh, t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_solid(mx0 + mw - 1, my0, mx0 + mw, my0 + mh, t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);

        const char *labels[VKW_CTX_COUNT] = {
            "Font Bigger  +", "Font Smaller -", "Font Reset"
        };
        for (int i = 0; i < VKW_CTX_COUNT; i++) {
            int iy = my0 + 2 + i * item_h;
            if (i == vkw_ctx_hover) {
                push_solid(mx0 + 1, iy, mx0 + mw - 1, iy + item_h,
                           t->accent[0], t->accent[1], t->accent[2], 0.2f, vp_w, vp_h);
            }
            push_text(mx0 + 6, iy + (item_h - mch) / 2, labels[i],
                      t->text[0], t->text[1], t->text[2], vp_w, vp_h, mcw, mch);
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
    int ch = (int)(VKW_CHAR_H * w->font_scale);
    if (ch < 8) ch = 8;
    int titlebar_h = ch + 8;
    int close_w = (int)(VKW_CHAR_W * w->font_scale) + 12;
    int cx = (int)w->x + (int)w->w - close_w - 2;
    int cy = (int)w->y + 2;
    return (mx >= cx && mx < cx + close_w &&
            my >= cy && my < cy + titlebar_h - 4);
}

static int vkw_hit_titlebar(vkw_window_t *w, int mx, int my)
{
    int ch = (int)(VKW_CHAR_H * w->font_scale);
    if (ch < 8) ch = 8;
    int titlebar_h = ch + 8;
    int close_w = (int)(VKW_CHAR_W * w->font_scale) + 12;
    return (mx >= (int)w->x && mx < (int)(w->x + w->w - close_w - 4) &&
            my >= (int)w->y && my < (int)(w->y + titlebar_h));
}

/* Returns edge bitmask: 1=left, 2=right, 4=top, 8=bottom, 0=none.
 * Corners use a larger grab zone for diagonal resize. Titlebar area
 * is excluded from top-edge resize to prevent drag/resize confusion. */
static int vkw_hit_resize(vkw_window_t *w, int mx, int my)
{
    int ix = (int)w->x, iy = (int)w->y;
    int iw = (int)w->w, ih = (int)w->h;
    int rz = VKW_RESIZE_ZONE;
    int cz = VKW_CORNER_ZONE;
    int edge = 0;

    /* Check if point is even near this window */
    if (mx < ix - 2 || mx > ix + iw + 2 || my < iy - 2 || my > iy + iw + 2) return 0;

    int near_left   = (mx >= ix && mx < ix + rz);
    int near_right  = (mx >= ix + iw - rz && mx < ix + iw);
    int near_top    = (my >= iy && my < iy + rz);
    int near_bottom = (my >= iy + ih - rz && my < iy + ih);

    /* Corner zones: larger grab area at the 4 corners */
    int corner_left   = (mx >= ix && mx < ix + cz);
    int corner_right  = (mx >= ix + iw - cz && mx < ix + iw);
    int corner_top    = (my >= iy && my < iy + cz);
    int corner_bottom = (my >= iy + ih - cz && my < iy + ih);

    /* Bottom-right corner (most common resize) */
    if (corner_bottom && corner_right) return 2 | 8;
    /* Bottom-left corner */
    if (corner_bottom && corner_left) return 1 | 8;
    /* Top-right corner */
    if (corner_top && corner_right) return 2 | 4;
    /* Top-left corner */
    if (corner_top && corner_left) return 1 | 4;

    /* Edges — but exclude top edge in titlebar area to prevent drag confusion */
    if (near_left)   edge |= 1;
    if (near_right)  edge |= 2;
    if (near_bottom) edge |= 8;
    if (near_top) {
        /* Only allow top resize outside the titlebar zone */
        int ch = (int)(VKW_CHAR_H * w->font_scale);
        if (ch < 8) ch = 8;
        int titlebar_h = ch + 8;
        if (my < iy + titlebar_h) {
            /* In titlebar — don't treat as top resize */
        } else {
            edge |= 4;
        }
    }
    return edge;
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
    /* Titlebar drag takes priority over edge resize to prevent confusion */
    if (vkw_hit_titlebar(w, mx, my)) {
        w->dragging = 1;
        w->drag_ox = (float)mx - w->x;
        w->drag_oy = (float)my - w->y;
        return 1;
    }
    /* Edge resize (checked after titlebar so drag wins near top) */
    {
        int edge = vkw_hit_resize(w, mx, my);
        if (edge) {
            w->resizing = 1;
            w->resize_edge = edge;
            w->resize_ox = (float)mx;
            w->resize_oy = (float)my;
            w->resize_sw = w->w;
            w->resize_sh = w->h;
            w->resize_sx = w->x;
            w->resize_sy = w->y;
            return 1;
        }
    }

    /* Click in content area = start text selection */
    {
        int fch = (int)(VKW_CHAR_H * w->font_scale);
        if (fch < 8) fch = 8;
        int ftitlebar_h = fch + 8;
        int finput_h = fch + 4;
        int content_y = (int)w->y + ftitlebar_h + 1;
        int content_h = (int)w->h - ftitlebar_h - 1;
        if (w->has_input) content_h -= finput_h;
        if (my >= content_y && my < content_y + content_h) {
            int from_bottom = (content_y + content_h - my) / fch;
            w->sel_start_line = from_bottom;
            w->sel_end_line = from_bottom;
            w->sel_active = 1;
            w->has_selection = 1;
        }
    }
    return 1;
}

/* Returns the appropriate resize cursor for a VKW window edge, or NULL */
static HCURSOR vkw_resize_cursor(int edge) {
    if (!edge) return NULL;
    int lr = edge & 3;  /* left/right bits */
    int tb = edge & 12; /* top/bottom bits */
    if (lr && tb) {
        /* Corner — diagonal */
        if ((lr == 1 && tb == 4) || (lr == 2 && tb == 8))
            return LoadCursor(NULL, IDC_SIZENWSE); /* NW-SE: top-left or bottom-right */
        else
            return LoadCursor(NULL, IDC_SIZENESW); /* NE-SW: top-right or bottom-left */
    }
    if (lr) return LoadCursor(NULL, IDC_SIZEWE);   /* left or right edge */
    if (tb) return LoadCursor(NULL, IDC_SIZENS);   /* top or bottom edge */
    return NULL;
}

/* Check all VKW windows for hover cursor, returns 1 if cursor was set */
static int vkw_update_cursor(int mx, int my) {
    for (int i = VKW_MAX_WINDOWS - 1; i >= 0; i--) {
        vkw_window_t *w = &vkw_windows[i];
        if (!w->active) continue;
        /* Check if mouse is in this window's bounds at all */
        if (mx < (int)w->x || mx >= (int)(w->x + w->w) ||
            my < (int)w->y || my >= (int)(w->y + w->h)) continue;
        /* Check titlebar first — show normal arrow for drag area */
        if (vkw_hit_titlebar(w, mx, my)) {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 1;
        }
        int edge = vkw_hit_resize(w, mx, my);
        if (edge) {
            HCURSOR c = vkw_resize_cursor(edge);
            if (c) { SetCursor(c); return 1; }
        }
        /* Inside window but not on edge — normal cursor */
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        return 1;
    }
    return 0;
}

static int vkw_mouse_move(int mx, int my)
{
    for (int i = 0; i < VKW_MAX_WINDOWS; i++) {
        vkw_window_t *w = &vkw_windows[i];
        if (!w->active) continue;
        if (w->dragging) {
            float new_x = (float)mx - w->drag_ox;
            float new_y = (float)my - w->drag_oy;
            w->x = new_x;
            w->y = new_y;
            /* Wobble: grid is updated via wob_move_rest in draw */
            if (fx_wobble_mode) wob_grids[i].active = 1;
            return 1;
        }
        if (w->resizing) {
            float dx = (float)mx - w->resize_ox;
            float dy = (float)my - w->resize_oy;
            int e = w->resize_edge;
            /* Keep resize cursor visible while dragging */
            { HCURSOR rc = vkw_resize_cursor(e); if (rc) SetCursor(rc); }
            if (e & 2) { /* right edge */
                w->w = w->resize_sw + dx;
                if (w->w < VKW_MIN_W) w->w = VKW_MIN_W;
            }
            if (e & 8) { /* bottom edge */
                w->h = w->resize_sh + dy;
                if (w->h < VKW_MIN_H) w->h = VKW_MIN_H;
            }
            if (e & 1) { /* left edge */
                float new_w = w->resize_sw - dx;
                if (new_w < VKW_MIN_W) new_w = VKW_MIN_W;
                w->x = w->resize_sx + (w->resize_sw - new_w);
                w->w = new_w;
            }
            if (e & 4) { /* top edge */
                float new_h = w->resize_sh - dy;
                if (new_h < VKW_MIN_H) new_h = VKW_MIN_H;
                w->y = w->resize_sy + (w->resize_sh - new_h);
                w->h = new_h;
            }
            /* Fixed font size — don't scale with window, just show fewer lines */
            return 1;
        }
        if (w->sel_active) {
            int fch = (int)(VKW_CHAR_H * w->font_scale);
            if (fch < 8) fch = 8;
            int ftitlebar_h = fch + 8;
            int finput_h = fch + 4;
            int content_y = (int)w->y + ftitlebar_h + 1;
            int content_h = (int)w->h - ftitlebar_h - 1;
            if (w->has_input) content_h -= finput_h;
            int from_bottom = (content_y + content_h - my) / fch;
            if (from_bottom < 0) from_bottom = 0;
            w->sel_end_line = from_bottom;
            w->has_selection = 1;
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
        vkw_windows[i].sel_active = 0;  /* stop dragging selection, keep has_selection */
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
        /* Console window → Python eval; Convo window → chat; others → callback */
        if (vkw_focus == vkw_console_idx && w->input[0]) {
            vkt_eval_python(w->input, w->id);
        } else if (vkw_focus == vkw_convo_idx && w->input[0]) {
            convo_on_input(w->input);
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

/* Toggle backscroll window (Alt+B) */
static void vkw_toggle_backscroll(int vp_w, int vp_h)
{
    if (vkw_backscroll_idx >= 0 && vkw_windows[vkw_backscroll_idx].active) {
        vkw_close(vkw_backscroll_idx);
        vkw_backscroll_idx = -1;
        return;
    }
    /* Full-ish window, centered */
    int bw = vp_w - 80, bh = vp_h - 60;
    if (bw < 400) bw = 400;
    if (bh < 300) bh = 300;
    int bx = (vp_w - bw) / 2, by = (vp_h - bh) / 2;
    vkw_backscroll_idx = vkw_create("Backscroll (Alt+B)", bx, by, bw, bh, 0);
    if (vkw_backscroll_idx >= 0) {
        vkw_focus = vkw_backscroll_idx;
        /* Fill with backscroll text */
        int text_len = 0;
        char *text = bs_get_text(&text_len);
        if (text && text_len > 0) {
            /* Feed line by line into the window */
            char *p = text;
            while (*p) {
                char *nl = strchr(p, '\n');
                int llen = nl ? (int)(nl - p) : (int)strlen(p);
                char line[VKW_LINE_LEN];
                if (llen >= VKW_LINE_LEN) llen = VKW_LINE_LEN - 1;
                memcpy(line, p, llen);
                line[llen] = 0;
                vkw_print(vkw_backscroll_idx, line);
                if (nl) p = nl + 1; else break;
            }
            free(text);
        } else {
            vkw_print(vkw_backscroll_idx, "(no backscroll data yet)");
        }
        /* Start at bottom (scroll = 0 means newest visible) */
        vkw_windows[vkw_backscroll_idx].scroll = 0;
    }
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
        vkw_print(vkw_console_idx, "\x1b[1;37mMMUDPy Python Console\x1b[0m");
        vkw_print(vkw_console_idx, "Type Python expressions. Commands:");
        vkw_print(vkw_console_idx, "  \x1b[1;36mbg.help()\x1b[0m        - Background plasma commands");
        vkw_print(vkw_console_idx, "  \x1b[1;36msmoke.help()\x1b[0m     - Smoky letters commands");
        vkw_print(vkw_console_idx, "  \x1b[1;36mvk_plugins.list()\x1b[0m - List VK plugins");
        vkw_print(vkw_console_idx, "  \x1b[1;36mmud.help()\x1b[0m       - Script/MUD commands");
        vkw_print(vkw_console_idx, "  \x1b[1;36mmud.list()\x1b[0m       - List loaded scripts");
        vkw_print(vkw_console_idx, "");
    }
}

static void vkw_toggle_convo(int vp_w, int vp_h)
{
    if (vkw_convo_idx >= 0 && vkw_windows[vkw_convo_idx].active) {
        vkw_close(vkw_convo_idx);
        vkw_convo_idx = -1;
        return;
    }
    int cw = 480, ch = 360;
    int cx = vp_w - cw - 20, cy = vp_h - ch - 40;
    vkw_convo_idx = vkw_create("Conversation", cx, cy, cw, ch, 1);
    if (vkw_convo_idx >= 0) {
        vkw_focus = vkw_convo_idx;
        vkw_print(vkw_convo_idx, "");
    }
}

/* ---- Chat line parsing ---- */
/* Detect chat lines from MUD output, forward to conversation window */

static int str_starts(const char *s, const char *prefix)
{
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

/* Case-insensitive check if word at `s` matches `w`, followed by space or ':' */
static int str_word_match(const char *s, const char *w)
{
    while (*w) {
        char a = *s, b = *w;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        s++; w++;
    }
    return 1;
}

static void convo_parse_line(const char *line)
{
    if (vkw_convo_idx < 0 || !vkw_windows[vkw_convo_idx].active) return;
    if (!line || !line[0]) return;

    /* Strip ANSI escape sequences AND any MegaMUD status prompt segments
     * ([HP=.../MA=...]:  or  [HP=...]:  or  [MA=...]:) that may be embedded
     * anywhere in the line. When chat arrives async right after a prompt,
     * MegaMUD renders them on the same line as "[HP=x/MA=y]:gossip..." —
     * scan-and-remove handles that no matter where the prompt lands. */
    char clean[512];
    int ci = 0;
    for (const char *p = line; *p && ci < (int)sizeof(clean) - 2; ) {
        if (*p == '\x1b') {
            p++;
            if (*p == '[') {
                p++;
                while (*p && !((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
                if (*p) p++;
            }
            continue;
        }
        /* Drop [HP=...]: and [MA=...]: prompt segments */
        if (p[0] == '[' && (p[1] == 'H' || p[1] == 'M') &&
            (p[2] == 'P' || p[2] == 'A') && p[3] == '=') {
            const char *rb = strchr(p, ']');
            if (rb && rb[1] == ':') { p = rb + 2; continue; }
        }
        clean[ci++] = *p++;
    }
    clean[ci] = 0;

    char buf[300];
    const char *s = clean;

    /* Get timestamp for display like MegaMUD does */
    SYSTEMTIME st;
    GetLocalTime(&st);
    char ts[16];
    int hr = st.wHour % 12; if (hr == 0) hr = 12;
    wsprintfA(ts, "%d:%02d%s", hr, st.wMinute, st.wHour >= 12 ? "pm" : "am");

    /* Detect channel and find where the message content starts (for coloring).
     * Display the raw cleaned line as-is, just prepend timestamp. */
    int chan = -1;
    int split = 0; /* char offset in output buf where message text begins */

    /* --- Telepath: two-part outgoing tracking --- */
    if (str_starts(s, "--- Telepath Sent to ")) {
        if (tell_pending && tell_pending_msg[0]) {
            const char *name_start = s + 21;
            const char *name_end = strstr(name_start, " ---");
            if (name_end && name_end > name_start) {
                int nlen = (int)(name_end - name_start);
                char name[32]; if (nlen > 31) nlen = 31;
                memcpy(name, name_start, nlen); name[nlen] = 0;
                wsprintfA(buf, "%s You telepath %s: %s", ts, name, tell_pending_msg);
                int sp = (int)strlen(ts) + 1;
                vkw_print_ex(vkw_convo_idx, buf, CHAT_TELEPATH, sp);
            }
        }
        tell_pending = 0;
        tell_pending_msg[0] = 0;
        tell_lines_waited = 0;
        return;
    }

    /* Count lines while waiting for telepath confirmation */
    if (tell_pending) {
        tell_lines_waited++;
        if (tell_lines_waited > TELL_MAX_WAIT) {
            tell_pending = 0;
            tell_pending_msg[0] = 0;
            tell_lines_waited = 0;
        }
    }

    /* Skip leading whitespace (prompt segments were stripped during ANSI pass) */
    while (*s == ' ') s++;

    /* Match channels — just identify, don't reformat */
    if (str_starts(s, "You gossip:") || strstr(s, " gossips:"))
        chan = CHAT_GOSSIP;
    else if (str_starts(s, "Broadcast from "))
        chan = CHAT_BROADCAST;
    else if (strstr(s, " telepaths to you:") || strstr(s, " telepaths:"))
        chan = CHAT_TELEPATH;
    else if (str_starts(s, "You gangpath:") || strstr(s, " gangpaths:"))
        chan = CHAT_GANGPATH;
    else if (str_starts(s, "You auction:") || strstr(s, " auctions:"))
        chan = CHAT_AUCTION;
    else if (str_starts(s, "You say \"") || strstr(s, " says \"") ||
             strstr(s, " says (to "))
        chan = CHAT_SAY;
    else if (str_starts(s, "You yell \"") || strstr(s, " yells \"") ||
             strstr(s, " yells from "))
        chan = CHAT_YELL;
    /* Player online/offline notifications */
    else if (strstr(s, " just entered the realm"))
        chan = CHAT_SAY;  /* show as grey/info */
    else if (strstr(s, " just hung up"))
        chan = CHAT_SAY;

    if (chan < 0) return; /* not a chat line */

    /* Format: "timestamp raw_line" — just like MegaMUD's convo window */
    wsprintfA(buf, "%s %s", ts, s);
    split = (int)strlen(ts) + 1; /* color split after timestamp */
    vkw_print_ex(vkw_convo_idx, buf, chan, split);
}

/* Handle conversation window input — send raw text to MUD via WM_CHAR to MMANSI.
 * No channel system — whatever you type goes straight to the MUD as-is. */
static void convo_on_input(const char *text)
{
    if (!text || !text[0]) return;

    /* Capture outgoing telepath for two-part tracking */
    if (text[0] == '/') {
        const char *p = text + 1;
        if (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        if (*p == ' ' && *(p + 1)) {
            strncpy(tell_pending_msg, p + 1, sizeof(tell_pending_msg) - 1);
            tell_pending_msg[sizeof(tell_pending_msg) - 1] = 0;
            tell_pending = 1;
            tell_lines_waited = 0;
        }
    }

    /* Send exactly what was typed to MMANSI */
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return;
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;
    for (int i = 0; text[i]; i++)
        PostMessageA(target, WM_CHAR, (WPARAM)(unsigned char)text[i], 0);
    PostMessageA(target, WM_CHAR, (WPARAM)'\r', 0);
}

/* ---- Paths & Loops Window ---- */
/* Reads Rooms.md and .mp files directly from MegaMUD directories.
 * Merges Default + Chars/All (All overrides Default).
 * Displays in a VKW window with Goto/Loop tabs, search, hidden toggle, walk/run. */

#define PL_MAX_ROOMS    2000
#define PL_MAX_CATS      100
#define PL_MAX_PATHS     800
#define PL_MAX_NAME       80
#define PL_MAX_CODE        8
#define PL_MAX_CAT        40
#define PL_MAX_FILE       16

typedef struct {
    char name[PL_MAX_NAME];
    char code[PL_MAX_CODE];
    char category[PL_MAX_CAT];
    unsigned int hash;
    unsigned int flags;
    int hidden; /* flags & 0x10 */
} pl_room_t;

typedef struct {
    char name[PL_MAX_NAME];
    char creator[32];
    char file[PL_MAX_FILE];
    char start_code[PL_MAX_CODE];
    char start_category[PL_MAX_CAT];
    char start_room[PL_MAX_NAME];
    unsigned int start_hash, end_hash;
    int steps;
    int is_loop; /* start_hash == end_hash */
} pl_path_t;

static pl_room_t *pl_rooms = NULL;
static int pl_room_count = 0;
static pl_path_t *pl_paths = NULL;
static int pl_path_count = 0;

/* Category index for tree display */
typedef struct {
    char name[PL_MAX_CAT];
    int first_idx;  /* index into pl_rooms or pl_paths */
    int count;
    int expanded;
} pl_cat_t;

static pl_cat_t pl_room_cats[PL_MAX_CATS];
static int pl_room_cat_count = 0;
static pl_cat_t pl_loop_cats[PL_MAX_CATS];
static int pl_loop_cat_count = 0;

/* Window state */
static int pl_wnd_open = 0;
static float pl_x = 100, pl_y = 50, pl_w = 520, pl_h = 480;
static int pl_dragging = 0;
static float pl_drag_ox, pl_drag_oy;
static int pl_tab = 0;       /* 0 = Goto, 1 = Loop */
static int pl_show_hidden = 1;  /* shown by default, like MegaMUD */
static int pl_run_mode = 0;  /* 0 = Walk (combat on), 1 = Run (combat off) */
static char pl_search[64] = {0};
static int pl_search_len = 0;
static int pl_scroll = 0;
static int pl_total_items = 0;   /* set by draw, used to clamp scroll */
static int pl_visible_items = 0; /* set by draw, used to clamp scroll */
static int pl_hover_item = -1;
static int pl_selected_item = -1;  /* currently selected (green) item, -1 = none */
static int pl_focused = 0;   /* input focus for search box */
static int pl_loaded = 0;


/* Get MegaMUD base directory (where the DLL's host exe lives) */
static void pl_get_megamud_dir(char *buf, int bufsz)
{
    GetModuleFileNameA(NULL, buf, bufsz);
    /* Strip exe filename */
    char *last = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '\\' || *p == '/') last = p;
    }
    if (last > buf) *(last + 1) = 0;
    else buf[0] = 0;
}

static unsigned int pl_parse_hex(const char *s)
{
    unsigned int v = 0;
    for (int i = 0; i < 8 && s[i]; i++) {
        char c = s[i];
        int d = 0;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
        v = (v << 4) | d;
    }
    return v;
}

/* Parse one Rooms.md line: HASH:FLAGS:v1:v2:v3:CODE:CATEGORY:NAME */
static int pl_parse_room_line(const char *line, pl_room_t *r)
{
    const char *p = line;
    /* Field 0: hash (8 hex chars) */
    const char *f0 = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0; p++;
    r->hash = pl_parse_hex(f0);

    /* Field 1: flags (8 hex chars) */
    const char *f1 = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0; p++;
    r->flags = pl_parse_hex(f1);
    r->hidden = (r->flags & 0x10) ? 1 : 0;

    /* Fields 2,3,4: skip */
    for (int i = 0; i < 3; i++) {
        while (*p && *p != ':') p++;
        if (*p != ':') return 0; p++;
    }

    /* Field 5: code */
    const char *f5 = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    int len = (int)(p - f5); if (len >= PL_MAX_CODE) len = PL_MAX_CODE - 1;
    memcpy(r->code, f5, len); r->code[len] = 0;
    p++;

    /* Field 6: category */
    const char *f6 = p;
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    len = (int)(p - f6); if (len >= PL_MAX_CAT) len = PL_MAX_CAT - 1;
    memcpy(r->category, f6, len); r->category[len] = 0;
    p++;

    /* Field 7: name (rest of line) */
    const char *f7 = p;
    len = 0;
    while (f7[len] && f7[len] != '\n' && f7[len] != '\r') len++;
    if (len >= PL_MAX_NAME) len = PL_MAX_NAME - 1;
    memcpy(r->name, f7, len); r->name[len] = 0;

    return (r->name[0] && r->code[0] && r->category[0]) ? 1 : 0;
}

/* Parse one .mp file: returns 1 on success */
static int pl_parse_mp(const char *filepath, pl_path_t *path)
{
    FILE *f = fopen(filepath, "r");
    if (!f) return 0;
    char buf[512];
    memset(path, 0, sizeof(*path));

    /* Line 1: [Name][Creator] */
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    {
        char *p = buf;
        if (*p == '[') {
            p++;
            char *end = strchr(p, ']');
            if (end) {
                int len = (int)(end - p);
                if (len >= PL_MAX_NAME) len = PL_MAX_NAME - 1;
                memcpy(path->name, p, len); path->name[len] = 0;
                p = end + 1;
                if (*p == '[') {
                    p++;
                    end = strchr(p, ']');
                    if (end) {
                        len = (int)(end - p);
                        if (len >= 31) len = 31;
                        memcpy(path->creator, p, len); path->creator[len] = 0;
                    }
                }
            }
        }
    }

    /* Line 2: [CODE:Category:RoomName] */
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    {
        char *p = buf;
        if (*p == '[') {
            p++;
            char *end = strchr(p, ':');
            if (end) {
                int len = (int)(end - p);
                if (len >= PL_MAX_CODE) len = PL_MAX_CODE - 1;
                memcpy(path->start_code, p, len); path->start_code[len] = 0;
                p = end + 1;
                end = strchr(p, ':');
                if (end) {
                    len = (int)(end - p);
                    if (len >= PL_MAX_CAT) len = PL_MAX_CAT - 1;
                    memcpy(path->start_category, p, len); path->start_category[len] = 0;
                    p = end + 1;
                    end = strchr(p, ']');
                    if (end) {
                        len = (int)(end - p);
                        if (len >= PL_MAX_NAME) len = PL_MAX_NAME - 1;
                        memcpy(path->start_room, p, len); path->start_room[len] = 0;
                    }
                }
            }
        }
    }

    /* Line 3: start_hash:end_hash:steps:-1:0::: */
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    {
        char *p = buf;
        const char *f0 = p;
        while (*p && *p != ':') p++;
        if (*p == ':') { path->start_hash = pl_parse_hex(f0); p++; }
        const char *f1 = p;
        while (*p && *p != ':') p++;
        if (*p == ':') { path->end_hash = pl_parse_hex(f1); p++; }
        /* steps */
        path->steps = 0;
        while (*p >= '0' && *p <= '9') { path->steps = path->steps * 10 + (*p - '0'); p++; }
    }

    fclose(f);
    path->is_loop = (path->start_hash == path->end_hash) ? 1 : 0;

    /* Extract filename from path */
    const char *fname = filepath;
    for (const char *p = filepath; *p; p++) {
        if (*p == '\\' || *p == '/') fname = p + 1;
    }
    int flen = 0;
    while (fname[flen] && fname[flen] != '.' && flen < PL_MAX_FILE - 1) {
        path->file[flen] = fname[flen]; flen++;
    }
    path->file[flen] = 0;

    /* Skip unnamed/empty paths */
    if (!path->name[0] || !path->start_code[0]) return 0;
    return 1;
}

/* Case-insensitive substring search */
static int pl_stristr(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

/* strcmp for qsort */
static int pl_cat_cmp(const void *a, const void *b)
{
    return strcmp(((const pl_cat_t *)a)->name, ((const pl_cat_t *)b)->name);
}

static int pl_room_cmp(const void *a, const void *b)
{
    const pl_room_t *ra = (const pl_room_t *)a, *rb = (const pl_room_t *)b;
    int c = strcmp(ra->category, rb->category);
    return c ? c : strcmp(ra->name, rb->name);
}

static int pl_path_cmp(const void *a, const void *b)
{
    const pl_path_t *pa = (const pl_path_t *)a, *pb = (const pl_path_t *)b;
    int c = strcmp(pa->start_category, pb->start_category);
    if (c) return c;
    /* Sort loops before non-loops so loop category indices are contiguous */
    if (pa->is_loop != pb->is_loop) return pb->is_loop - pa->is_loop;
    return strcmp(pa->name, pb->name);
}

/* Build category index from rooms or paths */
static void pl_build_room_cats(void)
{
    pl_room_cat_count = 0;
    /* Sort rooms by category then name */
    if (pl_room_count > 1)
        qsort(pl_rooms, pl_room_count, sizeof(pl_room_t), pl_room_cmp);

    for (int i = 0; i < pl_room_count; i++) {
        /* Find or create category */
        int found = -1;
        for (int c = 0; c < pl_room_cat_count; c++) {
            if (strcmp(pl_room_cats[c].name, pl_rooms[i].category) == 0) {
                found = c; break;
            }
        }
        if (found < 0 && pl_room_cat_count < PL_MAX_CATS) {
            found = pl_room_cat_count++;
            strncpy(pl_room_cats[found].name, pl_rooms[i].category, PL_MAX_CAT - 1);
            pl_room_cats[found].first_idx = i;
            pl_room_cats[found].count = 0;
            pl_room_cats[found].expanded = 0;
        }
        if (found >= 0) pl_room_cats[found].count++;
    }
    if (pl_room_cat_count > 1)
        qsort(pl_room_cats, pl_room_cat_count, sizeof(pl_cat_t), pl_cat_cmp);

    /* Recompute first_idx after sort */
    for (int c = 0; c < pl_room_cat_count; c++) {
        pl_room_cats[c].count = 0;
        pl_room_cats[c].first_idx = -1;
    }
    for (int i = 0; i < pl_room_count; i++) {
        for (int c = 0; c < pl_room_cat_count; c++) {
            if (strcmp(pl_room_cats[c].name, pl_rooms[i].category) == 0) {
                if (pl_room_cats[c].first_idx < 0) pl_room_cats[c].first_idx = i;
                pl_room_cats[c].count++;
                break;
            }
        }
    }
}

static void pl_build_loop_cats(void)
{
    pl_loop_cat_count = 0;
    if (pl_path_count > 1)
        qsort(pl_paths, pl_path_count, sizeof(pl_path_t), pl_path_cmp);

    /* Only index loops (start_hash == end_hash) — matches MegaMUD's "Roam area" */
    for (int i = 0; i < pl_path_count; i++) {
        if (!pl_paths[i].is_loop) continue;
        const char *cat = pl_paths[i].start_category;
        int found = -1;
        for (int c = 0; c < pl_loop_cat_count; c++) {
            if (strcmp(pl_loop_cats[c].name, cat) == 0) { found = c; break; }
        }
        if (found < 0 && pl_loop_cat_count < PL_MAX_CATS) {
            found = pl_loop_cat_count++;
            strncpy(pl_loop_cats[found].name, cat, PL_MAX_CAT - 1);
            pl_loop_cats[found].first_idx = i;
            pl_loop_cats[found].count = 0;
            pl_loop_cats[found].expanded = 0;
        }
        if (found >= 0) pl_loop_cats[found].count++;
    }
    if (pl_loop_cat_count > 1)
        qsort(pl_loop_cats, pl_loop_cat_count, sizeof(pl_cat_t), pl_cat_cmp);

    for (int c = 0; c < pl_loop_cat_count; c++) {
        pl_loop_cats[c].count = 0;
        pl_loop_cats[c].first_idx = -1;
    }
    for (int i = 0; i < pl_path_count; i++) {
        if (!pl_paths[i].is_loop) continue;
        for (int c = 0; c < pl_loop_cat_count; c++) {
            if (strcmp(pl_loop_cats[c].name, pl_paths[i].start_category) == 0) {
                if (pl_loop_cats[c].first_idx < 0) pl_loop_cats[c].first_idx = i;
                pl_loop_cats[c].count++;
                break;
            }
        }
    }
}

/* Load all room and path data from MegaMUD files */
static void pl_load_data(void)
{
    if (pl_loaded) return;
    pl_loaded = 1;

    char base[MAX_PATH];
    pl_get_megamud_dir(base, MAX_PATH);

    /* Allocate */
    if (!pl_rooms) pl_rooms = (pl_room_t *)calloc(PL_MAX_ROOMS, sizeof(pl_room_t));
    if (!pl_paths) pl_paths = (pl_path_t *)calloc(PL_MAX_PATHS, sizeof(pl_path_t));
    pl_room_count = 0;
    pl_path_count = 0;

    /* ---- Load Rooms.md ---- */
    /* Load Default first, then Chars/All overrides by hash */
    char path[MAX_PATH];

    /* Default/Rooms.md */
    wsprintfA(path, "%sDefault\\Rooms.md", base);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f) && pl_room_count < PL_MAX_ROOMS) {
            pl_room_t r;
            if (pl_parse_room_line(line, &r))
                pl_rooms[pl_room_count++] = r;
        }
        fclose(f);
    }

    /* Chars/All/Rooms.md — override by hash */
    wsprintfA(path, "%sChars\\All\\Rooms.md", base);
    f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            pl_room_t r;
            if (!pl_parse_room_line(line, &r)) continue;
            /* Check for existing room with same hash */
            int found = 0;
            for (int i = 0; i < pl_room_count; i++) {
                if (pl_rooms[i].hash == r.hash) {
                    pl_rooms[i] = r; found = 1; break;
                }
            }
            if (!found && pl_room_count < PL_MAX_ROOMS)
                pl_rooms[pl_room_count++] = r;
        }
        fclose(f);
    }

    /* ---- Load .mp path files ---- */
    /* Default first, then Chars/All overrides by filename */
    WIN32_FIND_DATAA fd;
    HANDLE hFind;

    wsprintfA(path, "%sDefault\\*.mp", base);
    hFind = FindFirstFileA(path, &fd);
    int mp_total = 0, mp_named = 0, mp_loop = 0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char fpath[MAX_PATH];
            wsprintfA(fpath, "%sDefault\\%s", base, fd.cFileName);
            pl_path_t p;
            mp_total++;
            if (pl_parse_mp(fpath, &p) && pl_path_count < PL_MAX_PATHS) {
                mp_named++;
                if (p.is_loop) mp_loop++;
                if (p.is_loop && api)
                    api->log("[P&L] Default loop: '%s' cat='%s' file='%s' hash=%08X steps=%d\n",
                             p.name, p.start_category, p.file, p.start_hash, p.steps);
                pl_paths[pl_path_count++] = p;
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    if (api) api->log("[P&L] Default: %d total .mp, %d named, %d loops\n", mp_total, mp_named, mp_loop);

    /* Chars/All — override by filename */
    wsprintfA(path, "%sChars\\All\\*.mp", base);
    hFind = FindFirstFileA(path, &fd);
    int ca_total = 0, ca_named = 0, ca_loop = 0;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            char fpath[MAX_PATH];
            wsprintfA(fpath, "%sChars\\All\\%s", base, fd.cFileName);
            pl_path_t p;
            ca_total++;
            if (!pl_parse_mp(fpath, &p)) continue;
            ca_named++;
            if (p.is_loop) ca_loop++;
            if (p.is_loop && api)
                api->log("[P&L] Chars/All loop: '%s' cat='%s' file='%s' hash=%08X steps=%d\n",
                         p.name, p.start_category, p.file, p.start_hash, p.steps);
            /* Override existing by filename */
            int found = 0;
            for (int i = 0; i < pl_path_count; i++) {
                if (strcmp(pl_paths[i].file, p.file) == 0) {
                    pl_paths[i] = p; found = 1; break;
                }
            }
            if (!found && pl_path_count < PL_MAX_PATHS)
                pl_paths[pl_path_count++] = p;
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    if (api) api->log("[P&L] Chars/All: %d total .mp, %d named, %d loops\n", ca_total, ca_named, ca_loop);
    if (api) api->log("[P&L] Total paths loaded: %d\n", pl_path_count);

    /* Build category indices */
    pl_build_room_cats();
    pl_build_loop_cats();

    if (api) api->log("[vk_terminal] Paths&Loops: %d rooms (%d cats), %d paths/loops (%d cats)\n",
                       pl_room_count, pl_room_cat_count, pl_path_count, pl_loop_cat_count);
}

/* Run fake_remote on a throwaway thread (no window = no deadlock).
 * The Vulkan thread owns vkt_hwnd, so SendMessageA from it can deadlock
 * if MMMAIN sends anything back. A bare thread has no window to block on. */
static DWORD WINAPI pl_remote_thread(LPVOID param)
{
    char *cmd = (char *)param;
    if (api && api->fake_remote) api->fake_remote(cmd);
    HeapFree(GetProcessHeap(), 0, cmd);
    return 0;
}

static void pl_fire_remote(const char *cmd)
{
    int len = (int)strlen(cmd) + 1;
    char *buf = (char *)HeapAlloc(GetProcessHeap(), 0, len);
    if (!buf) return;
    memcpy(buf, cmd, len);
    HANDLE h = CreateThread(NULL, 0, pl_remote_thread, buf, 0, NULL);
    if (h) CloseHandle(h);
}

static char vsb_goto_dest[64] = "";   /* name of goto destination for status display */
static char vsb_path_name[128];       /* tentative definition — init copy declared below */

/* Execute goto via fake_remote.
 * fake_remote("goto X") already handles everything:
 * stop_path, clear LOOPING/ON_ENTRY/PATHING/GO_FLAG, start_path, verify, kick. */
static void pl_do_goto(const char *room_name)
{
    if (!api || !api->fake_remote) return;

    /* Save destination name for status display */
    strncpy(vsb_goto_dest, room_name, sizeof(vsb_goto_dest) - 1);
    vsb_goto_dest[sizeof(vsb_goto_dest) - 1] = '\0';

    char cmd[128];
    _snprintf(cmd, sizeof(cmd), "goto %s", room_name);
    cmd[sizeof(cmd) - 1] = '\0';
    pl_fire_remote(cmd);

    vkm_goto_add(room_name);
}

static void pl_do_loop(const char *file, const char *display_name, const char *room_name)
{
    if (!api || !api->fake_remote) return;
    char cmd[128];
    /* fake_remote needs the .mp extension to match */
    if (strstr(file, ".mp") || strstr(file, ".MP"))
        _snprintf(cmd, sizeof(cmd), "loop %s", file);
    else
        _snprintf(cmd, sizeof(cmd), "loop %s.mp", file);
    cmd[sizeof(cmd) - 1] = '\0';
    pl_fire_remote(cmd);

    /* Seed status bar label: prefer the human-readable name from the .mp
     * header, fall back to the filename. Stays until MegaMUD's statusbar
     * overwrites it (or pathing/looping ends). */
    const char *label = (display_name && display_name[0]) ? display_name : file;
    if (label && label[0]) {
        strncpy(vsb_path_name, label, sizeof(vsb_path_name) - 1);
        vsb_path_name[sizeof(vsb_path_name) - 1] = '\0';
    }

    /* Add starting room to recent destinations */
    if (room_name && room_name[0])
        vkm_goto_add(room_name);
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

/* HDR support */
static int hdr_available = 0;       /* 1 = HDR formats detected */
static int hdr_enabled = 0;         /* 1 = user wants HDR */
static int hdr_ext_available = 0;   /* 1 = VK_EXT_swapchain_colorspace supported */
static char hdr_standard[32] = "";  /* e.g. "HDR10", "scRGB", etc. */
static int  vk_vsync = 1;          /* 1 = VSync (FIFO), 0 = uncapped (MAILBOX/IMMEDIATE) */
static int  snd_res_sel = -1;      /* selected resolution index, -1 = auto (native) */
static uint32_t vk_sc_count = 0;
static VkImage *vk_sc_images = NULL;
static VkImageView *vk_sc_views = NULL;
static VkFramebuffer *vk_sc_fbs = NULL;

static VkRenderPass vk_renderpass = VK_NULL_HANDLE;
static VkPipelineLayout vk_pipe_layout = VK_NULL_HANDLE;
static VkPipeline vk_pipeline = VK_NULL_HANDLE;
/* Liquid Letters FX pipeline — uses push constants for time + mode */
static VkPipelineLayout vk_liquid_pipe_layout = VK_NULL_HANDLE;
static VkPipeline vk_liquid_pipeline = VK_NULL_HANDLE;
static int fx_liquid_mode = 0;     /* 0 = off, 1 = liquid letters */
static int fx_waves_mode = 0;     /* 0 = off, 1 = diagonal waves */
static int fx_fbm_mode = 0;       /* 0 = off, 1 = FBM noise warp */
static int fx_sobel_mode = 0;     /* 0 = off, 1 = sobel/sharp/plastic */
static int fx_scanline_mode = 0;  /* 0 = off, 1 = CRT scanlines */
static int fx_smoky_mode = 0;       /* 0 = off, 1 = smoky letters */
static int fx_rain_mode = 0;        /* 0 = off, 1 = raindrop ripple warp */
static int fx_crawl_mode = 0;       /* 0 = off, 1 = SpaceWarZ perspective crawl */
static float rain_size = 0.06f;     /* drop radius in NDC */
static float rain_speed = 1.0f;     /* animation speed multiplier */
static float rain_freq = 3.0f;      /* drop density */
static float rain_warp = 0.02f;     /* displacement strength */
static float fx_time = 0.0f;      /* animation time in seconds */

/* Smoky Letters parameters */
static float smoke_decay = 1.5f;      /* fade speed, 0.5-5.0 */
static float smoke_depth = 0.8f;      /* opacity/strength, 0.0-2.0 */
static float smoke_zoom = 2.0f;       /* noise scale, 0.5-4.0 */
static float smoke_hue = 0.0f;        /* 0-360 degrees */
static float smoke_saturation = 0.0f; /* 0-2 (0 = white smoke) */
static float smoke_value = 1.0f;      /* brightness 0-2 */

/* Drop Shadow parameters */
static int   fx_shadow_mode = 0;        /* 0 = off, 1 = on */
static float shadow_opacity  = 0.6f;    /* 0..1 */
static float shadow_angle    = 135.0f;  /* 0..360 degrees (135=lower-right) */
static float shadow_distance = 2.0f;    /* 0..8 pixels */
static float shadow_blur     = 1.0f;    /* 0..4 blur radius */
static float shadow_r = 0.0f, shadow_g = 0.0f, shadow_b = 0.0f; /* shadow color */

/* ---- Animated Background System ---- */
#define BG_NONE      0
#define BG_PLASMA    1
#define BG_STARFIELD 2
#define BG_ALIENWALL 3
static int bg_mode = BG_NONE;
static int bg_quad_end = 0;

/* Background pipeline */
static VkPipelineLayout vk_bg_pipe_layout = VK_NULL_HANDLE;
static VkPipeline vk_bg_pipeline = VK_NULL_HANDLE;

/* Fractal Plasma parameters */
static float bg_plasma_speed      = 1.0f;
static float bg_plasma_turbulence = 0.5f;
static float bg_plasma_complexity = 4.0f;
static float bg_plasma_hue        = 0.0f;
static float bg_plasma_saturation = 0.8f;
static float bg_plasma_brightness = 0.7f;
static float bg_plasma_alpha      = 0.35f;
/* Sobel/sharpen/edge glow */
static float bg_plasma_sobel      = 0.0f;   /* 0..2 strength */
static float bg_plasma_sobel_angle = 135.0f; /* 0..360 light angle */
static float bg_plasma_edge_glow  = 0.0f;   /* 0..2 edge glow intensity */
static float bg_plasma_sharpen    = 0.0f;   /* 0..2 sharpen strength */
/* Material: 0=glossy,1=wet,2=metallic,3=matte */
#define BGP_MAT_COUNT 4
static const char *bgp_mat_names[BGP_MAT_COUNT] = { "Glossy", "Wet", "Metallic", "Matte" };
static int   bg_plasma_material   = 0;
static float bg_plasma_mat_str    = 1.0f;   /* 0..2 material strength */
static int   bg_tonemap_mode     = 0;      /* 0=none, 1=ACES, 2=Reinhard, 3=Hable, 4=AgX */
static float bg_tonemap_exposure = 1.0f;   /* 0.2..4.0 */
/* Shared beat response enum — each background implements what it can.
 * Plasma wires hue/bright/zoom/warp/jitter; starfield additionally uses
 * radial pulse, twinkle burst, drift boost, and layer wave. */
#define BGP_BEAT_NONE     0
#define BGP_BEAT_HUE      1
#define BGP_BEAT_BRIGHT   2
#define BGP_BEAT_ZOOM     3
#define BGP_BEAT_PULSE    4   /* radial outward (stars: push; plasma: ripple) */
#define BGP_BEAT_JITTER   5   /* position/UV shake */
#define BGP_BEAT_WARP     6   /* large-scale spatial warp */
#define BGP_BEAT_TWINKLE  7   /* stars: twinkle amp; plasma: turbulence spike */
#define BGP_BEAT_DRIFT    8   /* stars: drift/parallax boost; plasma: flow */
#define BGP_BEAT_WAVE     9   /* stars: per-layer sway; plasma: complexity pulse */
#define BGP_BEAT_COUNT   10
static const char *bgp_beat_names[BGP_BEAT_COUNT] = {
    "None", "Hue Shift", "Brightness", "Zoom Pulse",
    "Radial Pulse", "Jitter", "Warp", "Twinkle Burst",
    "Drift Boost", "Layer Wave"
};
static int bg_plasma_beat_bass   = 0;
static int bg_plasma_beat_mid    = 0;
static int bg_plasma_beat_treble = 0;
static int bg_plasma_beat_rms    = 0;

/* ---- Parallax Starfield parameters ---- */
/* Camera modes — keep in sync with the shader's MODE_* defines. */
#define BG_STAR_MODE_WARP       0  /* flying straight at viewer */
#define BG_STAR_MODE_SWERVE     1  /* warp + ship-like swerve */
#define BG_STAR_MODE_HORIZONTAL 2  /* classic horizontal parallax */
#define BG_STAR_MODE_VERTICAL   3  /* vertical parallax (falling) */
#define BG_STAR_MODE_ORBIT      4  /* warp + slow rotation */
#define BG_STAR_MODE_TUNNEL     5  /* radial vortex warp */
#define BG_STAR_MODE_COUNT      6
static const char *bg_star_mode_names[BG_STAR_MODE_COUNT] = {
    "Warp (At Viewer)",
    "Warp + Swerve",
    "Horizontal Parallax",
    "Vertical Parallax",
    "Warp + Orbit",
    "Tunnel Vortex"
};

static int   bg_star_camera_mode   = BG_STAR_MODE_WARP;
static float bg_star_swerve_amt    = 0.35f;  /* 0..1 */
static float bg_star_drift_x       = 0.0f;   /* NDC/sec base drift (warp = 0) */
static float bg_star_drift_y       = 0.0f;
static float bg_star_drift_speed   = 1.0f;   /* overall multiplier */
static float bg_star_parallax      = 0.62f;  /* 0.4..0.95 */
static float bg_star_layer_count   = 8.0f;   /* 2..8 — more = denser field */
static float bg_star_density       = 1.0f;   /* per-cell presence 0.2..1.0 */
static float bg_star_size          = 1.0f;   /* 0.5..2.5 */
static float bg_star_brightness    = 1.0f;   /* 0..2 */
static float bg_star_saturation    = 1.0f;   /* 0..2 */
static float bg_star_hue           = 0.0f;   /* 0..360 */
static float bg_star_bg_glow       = 0.35f;  /* 0..1 sky luminance */
static float bg_star_twinkle_speed = 1.2f;   /* 0..5 */
static float bg_star_twinkle_amt   = 0.55f;  /* 0..1 */
/* Per-band beat actions (uses shared BGP_BEAT_* enum) */
static int   bg_star_beat_bass    = BGP_BEAT_PULSE;
static int   bg_star_beat_mid     = BGP_BEAT_TWINKLE;
static int   bg_star_beat_treble  = BGP_BEAT_JITTER;
static int   bg_star_beat_rms     = BGP_BEAT_BRIGHT;

/* Starfield pipeline */
static VkPipelineLayout vk_star_pipe_layout = VK_NULL_HANDLE;
static VkPipeline       vk_star_pipeline    = VK_NULL_HANDLE;

/* ---- Alien Wall parameters ---- */
static float bg_alien_speed         = 0.8f;
static float bg_alien_cell_scale    = 3.0f;
static float bg_alien_warp          = 1.5f;
static float bg_alien_hue           = 120.0f;
static float bg_alien_saturation    = 0.85f;
static float bg_alien_brightness    = 0.7f;
static float bg_alien_alpha         = 0.35f;
static float bg_alien_vein          = 1.0f;
static float bg_alien_pulse_speed   = 1.0f;
static float bg_alien_glow          = 0.8f;
static float bg_alien_depth_layers  = 2.0f;
static float bg_alien_roughness     = 0.2f;
static float bg_alien_membrane      = 0.4f;
static float bg_alien_wet           = 1.0f;
static float bg_alien_fluid_speed   = 0.8f;
static float bg_alien_specular      = 0.5f;
static float bg_alien_cell_move     = 0.5f;
static float bg_alien_bulge         = 0.6f;
static int   bg_alien_cell_style    = 0;    /* 0=Voronoi 1=Hex 2=Hive 3=Geodesic 4=Mixed */
static const char *bg_alien_style_names[] = { "Voronoi", "Hex", "Hive", "Geodesic", "Mixed" };
#define BG_ALIEN_STYLE_COUNT 5
static int   bg_alien_beat_bass     = BGP_BEAT_BRIGHT;
static int   bg_alien_beat_mid      = BGP_BEAT_HUE;
static int   bg_alien_beat_treble   = BGP_BEAT_ZOOM;
static int   bg_alien_beat_rms      = BGP_BEAT_NONE;

/* Alien Wall pipeline */
static VkPipelineLayout vk_alien_pipe_layout = VK_NULL_HANDLE;
static VkPipeline       vk_alien_pipeline    = VK_NULL_HANDLE;

/* Unified Background Settings dialog state
 * Single tabbed dialog: one tab per registered background plugin.
 * Plasma tab owns the static plasma sliders/dropdowns above; Starfield
 * tab is populated later with starfield sliders. */
#define BGP_TAB_PLASMA    0
#define BGP_TAB_STARFIELD 1
#define BGP_TAB_ALIENWALL 2
#define BGP_TAB_COUNT     3
static const char *bgp_tab_names[BGP_TAB_COUNT] = { "Fractal Plasma", "Parallax Starfield", "Alien Wall" };
static int   bgp_tab = BGP_TAB_PLASMA;     /* active tab index */
static int   bgp_tab_scroll[BGP_TAB_COUNT] = {0, 0, 0}; /* per-tab scroll position */

static int   bgp_visible = 0;
static float bgp_x = 140.0f, bgp_y = 100.0f;
static float bgp_w = 680.0f, bgp_h = 720.0f;
static int   bgp_dragging = 0;
static int   bgp_resizing = 0;
static float bgp_drag_ox, bgp_drag_oy;
static int   bgp_active_slider = -1;
static int   bgp_scroll = 0;
static int   bgp_scroll_dragging = 0;
#define BGP_MIN_W 420.0f
#define BGP_MIN_H 260.0f
#define BGP_RESIZE_ZONE 14
static void  bgp_draw(int vp_w, int vp_h);
static int   bgp_mouse_down(int mx, int my);
static void  bgp_mouse_move(int mx, int my);
static void  bgp_mouse_up(void);

/* Smoke settings panel */
#define SMK_SLIDER_COUNT 6
static int   smk_visible = 0;
static float smk_x = 20, smk_y = 100;
static float smk_w = 280, smk_h = 200;
static int   smk_dragging = 0;
static float smk_drag_ox = 0, smk_drag_oy = 0;
static int   smk_active_slider = -1;
static float *smk_slider_ptr[SMK_SLIDER_COUNT];
static const float smk_slider_min[SMK_SLIDER_COUNT] = { 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f };
static const float smk_slider_max[SMK_SLIDER_COUNT] = { 5.0f, 2.0f, 4.0f, 360.0f, 2.0f, 2.0f };
static const char *smk_labels[SMK_SLIDER_COUNT] = { "Decay", "Depth", "Zoom", "Hue", "Saturation", "Brightness" };
static void  smk_draw(int vp_w, int vp_h);
static int   smk_mouse_down(int mx, int my);
static void  smk_mouse_move(int mx, int my);
static void  smk_mouse_up(void);

/* Rain drops settings panel */
#define RN_SLIDER_COUNT 4
static int   rn_visible = 0;
static int   rn_scaled = 0;
static float rn_x = 40, rn_y = 100;
static float rn_w = 280, rn_h = 200;
static int   rn_dragging = 0;
static float rn_drag_ox = 0, rn_drag_oy = 0;
static int   rn_active_slider = -1;
static float *rn_slider_ptr[RN_SLIDER_COUNT];
static const float rn_slider_min[RN_SLIDER_COUNT] = { 0.02f, 0.5f, 1.0f, 0.005f };
static const float rn_slider_max[RN_SLIDER_COUNT] = { 0.15f, 3.0f, 8.0f, 0.05f };
static const char *rn_labels[RN_SLIDER_COUNT] = { "Drop Size", "Speed", "Frequency", "Warp Str" };
static void  rn_draw(int vp_w, int vp_h);
static int   rn_mouse_down(int mx, int my);
static void  rn_mouse_move(int mx, int my);
static void  rn_mouse_up(void);

/* Drop shadow settings panel */
#define SHD_SLIDER_COUNT 5
static int   shd_visible = 0;
static float shd_x = 60, shd_y = 120;
static float shd_w = 280, shd_h = 200;
static int   shd_dragging = 0;
static float shd_drag_ox = 0, shd_drag_oy = 0;
static int   shd_active_slider = -1;
static float *shd_slider_ptr[SHD_SLIDER_COUNT];
static const float shd_slider_min[SHD_SLIDER_COUNT] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
static const float shd_slider_max[SHD_SLIDER_COUNT] = { 1.0f, 360.0f, 8.0f, 4.0f, 1.0f };
static const char *shd_labels[SHD_SLIDER_COUNT] = { "Opacity", "Angle", "Distance", "Blur", "Brightness" };
static void  shd_draw(int vp_w, int vp_h);
static int   shd_mouse_down(int mx, int my);
static void  shd_mouse_move(int mx, int my);
static void  shd_mouse_up(void);

/* Post-process color/brightness settings */
static float pp_brightness = 0.0f;   /* -1..1 */
static float pp_contrast = 1.0f;     /* 0..2 */
static float pp_hue = 0.0f;          /* -180..180 degrees */
static float pp_saturation = 1.0f;   /* 0..2 */

/* Color panel state */
static int   sp_focus = -1;  /* settings panel focus: 0=clr, 1=bgp, 2=smk, 3=shd, 4=snd */
static int   clr_visible = 0;
static float clr_x = 120.0f, clr_y = 80.0f;
static float clr_w = 280.0f, clr_h = 200.0f;
static int   clr_dragging = 0;
static float clr_drag_ox, clr_drag_oy;
static int   clr_active_slider = -1;  /* 0=brightness, 1=contrast, 2=hue, 3=saturation */
static void  clr_open_window(void);
static void  clr_draw(int vp_w, int vp_h);

/* ---- Fonts & Text Panel ---- */
static int   fnt_visible = 0;
static float fnt_x = 200.0f, fnt_y = 100.0f;
static float fnt_w = 380.0f, fnt_h = 600.0f;
static int   fnt_dragging = 0;
static float fnt_drag_ox, fnt_drag_oy;
static int   fnt_scroll = 0;
static int   fnt_hover_item = -1;
static int   fnt_active_slider = -1; /* 0=font_size, 1=margin */
static void  fnt_toggle(void);
static void  fnt_draw(int vp_w, int vp_h);
static int   fnt_mouse_down(int mx, int my);
static void  fnt_mouse_move(int mx, int my);
static void  fnt_mouse_up(void);
static int   fnt_scroll_wheel(int mx, int my, int delta);
static int   clr_mouse_down(int mx, int my);
static void  clr_mouse_move(int mx, int my);
static void  clr_mouse_up(void);

/* ---- Vulkan Round Timer Widget ---- */
#define VRT_DEFAULT_SIZE 160
#define VRT_MIN_SIZE      80
#define VRT_MAX_SIZE     700
#define VRT_ARC_SEGS      64     /* segments for smooth arc */
#define VRT_PARTICLE_MAX   24

static int  vrt_visible = 0;
static float vrt_x = 50.0f, vrt_y = 50.0f;   /* top-left in pixels */
static float vrt_size = VRT_DEFAULT_SIZE;      /* widget diameter */
static int  vrt_dragging = 0;
static float vrt_drag_ox, vrt_drag_oy;

/* Round timing — mirrors gl_round_timer.c algorithm exactly */
static volatile int    vrt_round_num = 0;
static volatile int    vrt_synced = 0;
static double          vrt_last_tick_ts = 0.0;
static double          vrt_round_period = 5000.0;
static double          vrt_recent_intervals[12];
static int             vrt_interval_count = 0;
static char            vrt_delta_text[32] = "";
static double          vrt_delta_fade = 0.0;
static int             vrt_flash_tick = 0;

/* Manual sync state */
static char vrt_sync_spell[22] = {0};
static volatile int vrt_ms_state = 0;       /* 0=idle, 1=spamming */
static volatile int vrt_ms_already_count = 0;
static volatile int vrt_ms_rounds_detected = 0;

/* Context menu */
static int  vrt_ctx_open = 0;
static int  vrt_ctx_x = 0, vrt_ctx_y = 0;
static int  vrt_ctx_hover = -1;
#define VRT_CTX_W_BASE       220
#define VRT_CTX_ITEM_H_BASE  28
#define VRT_CTX_PAD_BASE      6
static int VRT_CTX_W, VRT_CTX_ITEM_H, VRT_CTX_PAD;
static void vrt_ctx_update_scale(void) {
    VRT_CTX_W = (int)(VRT_CTX_W_BASE * ui_scale);
    VRT_CTX_ITEM_H = (int)(VRT_CTX_ITEM_H_BASE * ui_scale);
    VRT_CTX_PAD = (int)(VRT_CTX_PAD_BASE * ui_scale);
}

/* Menu item IDs */
#define VRT_CTX_SPELL     0
#define VRT_CTX_CAST      1
#define VRT_CTX_SEP1      2
#define VRT_CTX_SMALL     3
#define VRT_CTX_NORMAL    4
#define VRT_CTX_LARGE     5
#define VRT_CTX_XLARGE    6
#define VRT_CTX_XXL       7
#define VRT_CTX_SEP2      8
#define VRT_CTX_ORBITS    9
#define VRT_CTX_SEP3     10
#define VRT_CTX_RESET    11
#define VRT_CTX_HIDE     12
#define VRT_CTX_COUNT    13

/* Spell input dialog state */
static int  vrt_spell_input = 0;   /* 1 = showing spell input */
static char vrt_spell_buf[22] = {0};
static int  vrt_spell_len = 0;
static int  vrt_spell_cursor = 0;

/* Particles (comet trail) */
static struct {
    float x, y, vx, vy, alpha, r, g, b;
    int alive;
} vrt_particles[VRT_PARTICLE_MAX];
static int vrt_particle_idx = 0;

/* Orbital particles — double helix orbiting the center */
#define VRT_ORBIT_MAX  32
static struct {
    float birth_time;    /* fx_time at spawn */
    float phase;         /* position along helix (radians) */
    float helix_id;      /* which strand: 0 or 1 */
    float orbit_r;       /* base orbit radius (fraction of ring R) */
    float speed;         /* orbit speed multiplier */
    float r, g, b;       /* color */
    int   alive;
} vrt_orbits[VRT_ORBIT_MAX];
static int vrt_orbit_idx = 0;
static int vrt_orbits_visible = 1; /* toggle from context menu */
static int vrt_last_spawn_round = -1; /* prevent double-spawning same round */

/* ---- Beat-driven maneuver & pulse state ---- */
/* Forward decls: sin/cos helpers live later in the file (with vrt_draw). */
static float vrt_sinf(float x);
static float vrt_cosf(float x);
#define VRT_MANEUVER_COUNT   8
#define VRT_MANEUVER_DUR     0.85f  /* seconds; quick + smooth */
static int   vrt_bass_beat_last = -1;     /* last observed mr_beat_snap.beat_count */
static int   vrt_beat_mod4 = 0;           /* counts beats 0..3; maneuver fires at 4 */
static float vrt_bass_pulse_time = -10.0f;/* fx_time of last bass hit */
static int   vrt_man_current = -1;        /* -1 = none, else 0..7 */
static float vrt_man_start = 0.0f;        /* fx_time when maneuver began */
static int   vrt_man_recent[2] = {-1,-1}; /* last 2 maneuvers (cooldown) */
static unsigned int vrt_man_rng = 0xA5A5A5A5u;

static unsigned int vrt_man_rand(void) {
    vrt_man_rng = vrt_man_rng * 1664525u + 1013904223u;
    return vrt_man_rng;
}

/* Drive the widget off SuperFlux kick-band onsets (mr_beat_snap.kick_beat_count).
 * The new detector fires reliably on kick drums across psytrance/DnB/breaks,
 * so we no longer need a local fallback detector. */
static void vrt_update_maneuvers(void)
{
    int kc = mr_beat_snap.kick_beat_count;
    if (vrt_bass_beat_last < 0) vrt_bass_beat_last = kc;
    int new_kicks = kc - vrt_bass_beat_last;
    if (new_kicks < 0) new_kicks = 0;
    if (new_kicks > 4) new_kicks = 4;
    vrt_bass_beat_last = kc;

    if (new_kicks > 0) {
        vrt_bass_pulse_time = fx_time;
        vrt_beat_mod4 += new_kicks;
        while (vrt_beat_mod4 >= 4 && vrt_man_current < 0) {
            vrt_beat_mod4 -= 4;
            int pick = -1;
            for (int tries = 0; tries < 16; tries++) {
                int cand = (int)(vrt_man_rand() % (unsigned)VRT_MANEUVER_COUNT);
                if (cand != vrt_man_recent[0] && cand != vrt_man_recent[1]) {
                    pick = cand; break;
                }
            }
            if (pick < 0) pick = (int)(vrt_man_rand() % (unsigned)VRT_MANEUVER_COUNT);
            vrt_man_current = pick;
            vrt_man_start = fx_time;
            vrt_man_recent[1] = vrt_man_recent[0];
            vrt_man_recent[0] = pick;
        }
    }

    if (vrt_man_current >= 0 &&
        (fx_time - vrt_man_start) >= VRT_MANEUVER_DUR) {
        vrt_man_current = -1;
    }
}

/* Compute maneuver offsets for a specific orbital dot.
 *   slot     : per-dot phase index 0..nslots-1 (derived from phase)
 *   nslots   : total dots in ring
 *   *d_ang   : angular offset (radians) added to main_angle
 *   *d_rfrac : radial fractional offset added to or_frac
 *   *g_boost : extra glow/size multiplier (0..1) */
static void vrt_maneuver_offset(int mtype, float t01, int slot, int nslots,
                                float *d_ang, float *d_rfrac, float *g_boost)
{
    *d_ang = 0.0f; *d_rfrac = 0.0f; *g_boost = 0.0f;
    if (mtype < 0) return;
    if (t01 < 0.0f) t01 = 0.0f; if (t01 > 1.0f) t01 = 1.0f;

    float bump = vrt_sinf(t01 * (float)M_PI);              /* 0->1->0 */
    float ease = t01 * t01 * (3.0f - 2.0f * t01);          /* smoothstep */
    float n = (float)((nslots > 0) ? nslots : 12);

    switch (mtype) {
        case 0: {
            /* Swap partners: pairs sweep across the ring in opposite directions,
             * returning home — visually reads as "trading places". */
            float dir = ((slot & 1) == 0) ? 1.0f : -1.0f;
            *d_ang = dir * ease * 2.0f * (float)M_PI;
            *d_rfrac = bump * 0.06f;
            break;
        }
        case 1: {
            /* Burst outward and back */
            *d_rfrac = bump * 0.55f;
            *g_boost = bump * 0.5f;
            break;
        }
        case 2: {
            /* Implode to center, then out */
            *d_rfrac = -bump * 0.25f;
            *g_boost = bump * 0.7f;
            break;
        }
        case 3: {
            /* Spin boost: angular velocity pulse */
            *d_ang = vrt_sinf(t01 * (float)M_PI) * 1.9f;
            *g_boost = bump * 0.25f;
            break;
        }
        case 4: {
            /* Reverse flash: briefly rotate backward then forward home */
            *d_ang = -vrt_sinf(t01 * 2.0f * (float)M_PI) * 1.4f;
            break;
        }
        case 5: {
            /* Ripple: radial bump travels around the ring sequentially */
            float slot_phase = (float)slot / n;
            float wave = t01 - slot_phase * 0.6f;
            float w01 = wave - (float)(int)wave;
            if (w01 < 0.0f) w01 += 1.0f;
            float spike = vrt_sinf(w01 * (float)M_PI);
            if (w01 > 0.5f) spike = 0.0f;                  /* single half-cycle */
            *d_rfrac = spike * 0.42f;
            *g_boost = spike * 0.5f;
            break;
        }
        case 6: {
            /* Figure-8 cross: pairs dive inward + angular sine, different direction per pair */
            float dir = ((slot >> 1) & 1) ? 1.0f : -1.0f;
            *d_rfrac = -vrt_sinf(t01 * (float)M_PI) * 0.32f;
            *d_ang = dir * vrt_sinf(t01 * 2.0f * (float)M_PI) * 1.1f;
            *g_boost = bump * 0.35f;
            break;
        }
        case 7: {
            /* Spiral tighten then release outward */
            if (t01 < 0.5f) {
                float s = t01 * 2.0f;
                float se = s * s * (3.0f - 2.0f * s);
                *d_rfrac = -se * 0.22f;
                *d_ang = se * 1.7f;
            } else {
                float s = (t01 - 0.5f) * 2.0f;
                float se = s * s * (3.0f - 2.0f * s);
                *d_rfrac = -0.22f + se * 0.32f;
                *d_ang = 1.7f + se * 0.8f;
            }
            *g_boost = bump * 0.4f;
            break;
        }
    }
}

static void vrt_spawn_orbits(float cr, float cg, float cb)
{
    /* Two intertwined helix strands, 6 particles each */
    int per_strand = 6;
    int batch = per_strand * 2;
    for (int i = 0; i < batch; i++) {
        int idx = vrt_orbit_idx % VRT_ORBIT_MAX;
        int strand = i / per_strand;
        int si = i % per_strand;
        vrt_orbits[idx].birth_time = fx_time;
        /* Evenly spaced along helix, PI offset between strands */
        vrt_orbits[idx].phase = (float)si * (2.0f * 3.14159f / (float)per_strand);
        vrt_orbits[idx].helix_id = (float)strand;
        vrt_orbits[idx].orbit_r = 0.28f + (float)si * 0.015f;
        vrt_orbits[idx].speed = 1.0f + (float)si * 0.04f;
        /* Strand 0 = base color, strand 1 = complementary shift */
        float hue_var = (float)si * 0.06f;
        if (strand == 0) {
            vrt_orbits[idx].r = cr * (0.8f + hue_var) + 0.15f;
            vrt_orbits[idx].g = cg * (0.9f - hue_var * 0.2f) + 0.1f;
            vrt_orbits[idx].b = cb * (0.85f + hue_var * 0.3f) + 0.1f;
        } else {
            vrt_orbits[idx].r = cb * (0.7f + hue_var) + 0.2f;
            vrt_orbits[idx].g = cr * (0.8f + hue_var * 0.4f) + 0.15f;
            vrt_orbits[idx].b = cg * (0.9f - hue_var * 0.2f) + 0.1f;
        }
        if (vrt_orbits[idx].r > 1) vrt_orbits[idx].r = 1;
        if (vrt_orbits[idx].g > 1) vrt_orbits[idx].g = 1;
        if (vrt_orbits[idx].b > 1) vrt_orbits[idx].b = 1;
        vrt_orbits[idx].alive = 1;
        vrt_orbit_idx++;
    }
}

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

/* UI font texture (dedicated high-quality TTF for P&L window, convo, etc.) */
static VkImage ui_font_img = VK_NULL_HANDLE;
static VkDeviceMemory ui_font_mem = VK_NULL_HANDLE;
static VkImageView ui_font_view = VK_NULL_HANDLE;
static VkSampler ui_font_sampler = VK_NULL_HANDLE;
static VkDescriptorSet ui_desc_set = VK_NULL_HANDLE;
static uint32_t ui_atlas_w = 256, ui_atlas_h = 512;
static int ui_font_ready = 0;

/* VFT (floating text) font atlas — separate high-res atlas with display fonts */
#define VFT_NUM_FONTS 5
static const char *vft_font_files[VFT_NUM_FONTS] = {
    "plugins\\fonts\\VFT_BlackOps.ttf",   /* 0: Heavy military stencil */
    "plugins\\fonts\\VFT_Teko.ttf",       /* 1: Clean condensed numbers */
    "plugins\\fonts\\VFT_Russo.ttf",      /* 2: Geometric bold */
    "plugins\\fonts\\VFT_Orbitron.ttf",   /* 3: Futuristic display */
    "plugins\\fonts\\VFT_Rajdhani.ttf",   /* 4: Sharp angular */
};
static const char *vft_font_names[VFT_NUM_FONTS] = {
    "Black Ops", "Teko", "Russo", "Orbitron", "Rajdhani"
};
static VkImage vft_font_img = VK_NULL_HANDLE;
static VkDeviceMemory vft_font_mem = VK_NULL_HANDLE;
static VkImageView vft_font_view = VK_NULL_HANDLE;
static VkSampler vft_font_sampler = VK_NULL_HANDLE;
static VkDescriptorSet vft_desc_set = VK_NULL_HANDLE;
static uint32_t vft_atlas_w = 0, vft_atlas_h = 0;
static int vft_font_ready = 0;
static int vft_current_font = 0;  /* which VFT font is currently loaded */

/* Icon atlas texture (color emoji from NotoColorEmoji) */
static VkImage vib_icon_img = VK_NULL_HANDLE;
static VkDeviceMemory vib_icon_mem = VK_NULL_HANDLE;
static VkImageView vib_icon_view = VK_NULL_HANDLE;
static VkSampler vib_icon_sampler = VK_NULL_HANDLE;
static VkDescriptorSet vib_desc_set = VK_NULL_HANDLE;
static int vib_icon_ready = 0;
static int vib_quad_start = 0, vib_quad_end = 0;
static int pl_quad_start = 0;  /* index in quad buffer where P&L quads begin */
static int pl_quad_end = 0;    /* index where P&L quads end (menu quads follow) */

/* ---- Status Bar Widget ---- */
#define VSB_BAR_H_BASE 22     /* bar height in pixels at 1080p */
static int VSB_BAR_H = 22;
#define VSB_CHAR_W     10     /* character width for status text */
#define VSB_CHAR_H     18     /* character height for status text */
#define VSB_PAD_BASE    6     /* horizontal padding inside sections */
#define VSB_SEP_W_BASE  2     /* divider width between sections */
static int VSB_PAD = 6;
static int VSB_SEP_W = 2;
#define VSB_STATUSBAR_ID 107  /* MegaMUD status bar control ID */

/* Memory offsets for reading MegaMUD state */
#define VSB_OFF_CUR_HP     0x53D4
#define VSB_OFF_MAX_HP     0x53DC
#define VSB_OFF_CUR_MANA   0x53E0
#define VSB_OFF_MAX_MANA   0x53E8
#define VSB_OFF_IN_COMBAT  0x5698
#define VSB_OFF_IS_RESTING 0x5678
#define VSB_OFF_IS_MEDIT   0x567C
#define VSB_OFF_CUR_STEP   0x5898
#define VSB_OFF_TOTAL_STEPS 0x5894
#define VSB_OFF_PATHING    0x5664
#define VSB_OFF_LOOPING    0x5668
#define VSB_OFF_ROAMING    0x566C
#define VSB_OFF_ON_ENTRY   0x54B4   /* 0=nothing, 1=resume loop, 2=auto-roam */
#define VSB_OFF_MODE       0x54BC   /* 11=idle, 14=walking, 15=looping */
#define VSB_OFF_GO_FLAG    0x564C   /* master go/stop toggle */
#define VSB_OFF_COMBAT_TGT 0x552C
#define VSB_OFF_AUTOCOMBAT 0x573C  /* runtime autocombat flag */

static int  vsb_visible = 0;
static int  vsb_quad_start = 0;  /* quad range for status bar (uses UI font) */
static int  vsb_quad_end = 0;

/* Cached status data — updated periodically */
static char vsb_path_name[128] = "";
static int  vsb_cur_step = 0, vsb_total_steps = 0;
static int  vsb_lap_count = 0;          /* loop lap counter */
static int  vsb_prev_step = -1;         /* previous step for lap detection */
static char vsb_char_name[64] = "";     /* loaded character INI name */
static char vsb_scripts_str[256] = "None"; /* comma-separated loaded script names */
static int  vsb_cur_hp = 0, vsb_max_hp = 0;
static int  vsb_cur_mana = 0, vsb_max_mana = 0;
static int  vsb_in_combat = 0;
static int  vsb_is_resting = 0;
static int  vsb_is_medit = 0;
static int  vsb_pathing = 0;
static int  vsb_looping = 0;
static int  vsb_roaming = 0;
static int  vsb_autocombat = 0;
static char vsb_target[64] = "";
static char vsb_status_text[128] = ""; /* derived status label */
static DWORD vsb_last_read = 0;       /* tick of last status read */
static int  vsb_prev_mana = -1;       /* previous mana for tick detection */
static DWORD vsb_mana_tick_time = 0;  /* GetTickCount of last mana tick */
static int  vsb_mana_tick_show = 0;   /* 1 = flash active */
static int  vsb_mana_tick_val = 0;    /* amount gained on last tick */
static int  vsb_prev_pathing = 0;     /* previous pathing state for 0->1 detection */
static int  vsb_prev_looping = 0;     /* previous looping state for 0->1 detection */
static __int64 vsb_exp_rate = 0;      /* exp/hr from player stats, updated by pst code */

/* ---- Exp Bar Widget ---- */
#define VXB_BAR_H_BASE 20     /* bar height in pixels at 1080p */
#define VXB_SEG_COUNT  20     /* number of segments */
#define VXB_SEG_GAP_BASE 1   /* gap between segments */
#define VXB_PAD_BASE     2   /* horizontal padding */
static int VXB_BAR_H = 20;
static int VXB_SEG_GAP = 1;
static int VXB_PAD = 2;
#define VXB_OFF_EXP_LO  0x53B0
#define VXB_OFF_EXP_HI  0x53B4
#define VXB_OFF_NEED_LO 0x53B8
#define VXB_OFF_NEED_HI 0x53BC
#define VXB_OFF_LEVEL   0x53D0

static int  vxb_visible = 0;
static long long vxb_exp = 0;
static long long vxb_needed = 0;
static int  vxb_level = 0;
static float vxb_percent = 0.0f;
static DWORD vxb_last_read = 0;
static float vxb_flash = 0.0f;   /* flash timer for XP gain */
static long long vxb_prev_exp = 0; /* previous exp for gain detection */

/* Vertex + index buffers */
static VkBuffer vk_vbuf = VK_NULL_HANDLE;
static VkDeviceMemory vk_vmem = VK_NULL_HANDLE;
static VkBuffer vk_ibuf = VK_NULL_HANDLE;
static VkDeviceMemory vk_imem = VK_NULL_HANDLE;
static vertex_t *vk_vdata = NULL;  /* persistently mapped */
static int quad_count = 0;
static int fx_split_quad = 0;     /* quads before this = terminal (FX), after = UI (normal) */
static float last_cw_px = 9.0f, last_ch_px = 17.0f;  /* cell dimensions for smoke shader */

/* ---- Forward declarations ---- */

static void vkt_destroy_swapchain(void);
static int  vkt_create_swapchain(void);
static void vkt_recreate_swapchain(void);
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

/* ---- Backscroll buffer — plain text, ANSI stripped ---- */

#define BACKSCROLL_SIZE  (512 * 1024)   /* 512KB ring buffer */
static char backscroll_buf[BACKSCROLL_SIZE];
static int bs_head = 0;                /* next write position */
static int bs_len = 0;                 /* total bytes stored (max BACKSCROLL_SIZE) */
static CRITICAL_SECTION bs_lock;
static int bs_lock_init = 0;

/* ANSI strip state machine for backscroll */
static int bs_in_esc = 0;     /* inside ESC sequence */
static int bs_in_csi = 0;     /* inside CSI (ESC[...) */

static void bs_append(const char *data, int len)
{
    if (!bs_lock_init) return;
    EnterCriticalSection(&bs_lock);
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        /* ANSI escape stripping */
        if (bs_in_csi) {
            if ((ch >= 0x40 && ch <= 0x7E)) bs_in_csi = 0; /* end of CSI */
            continue;
        }
        if (bs_in_esc) {
            bs_in_esc = 0;
            if (ch == '[') { bs_in_csi = 1; continue; }
            continue; /* skip ESC + one char */
        }
        if (ch == 0x1B) { bs_in_esc = 1; continue; }
        /* Skip \r, keep \n and printable */
        if (ch == '\r') continue;
        backscroll_buf[bs_head] = (char)ch;
        bs_head = (bs_head + 1) % BACKSCROLL_SIZE;
        if (bs_len < BACKSCROLL_SIZE) bs_len++;
    }
    LeaveCriticalSection(&bs_lock);
}

/* Get backscroll as a malloc'd string (caller frees). Returns length. */
static char *bs_get_text(int *out_len)
{
    if (!bs_lock_init || bs_len == 0) { *out_len = 0; return NULL; }
    EnterCriticalSection(&bs_lock);
    char *text = (char *)malloc(bs_len + 1);
    if (text) {
        int start = (bs_head - bs_len + BACKSCROLL_SIZE) % BACKSCROLL_SIZE;
        for (int i = 0; i < bs_len; i++) {
            text[i] = backscroll_buf[(start + i) % BACKSCROLL_SIZE];
        }
        text[bs_len] = 0;
        *out_len = bs_len;
    } else {
        *out_len = 0;
    }
    LeaveCriticalSection(&bs_lock);
    return text;
}


/* ---- Raw ANSI ring buffer — preserves escape sequences ---- */

#define RAW_ANSI_SIZE  (512 * 1024)
static char raw_ansi_buf[RAW_ANSI_SIZE];
static int ra_head = 0, ra_len = 0;

static void ra_append(const char *data, int len)
{
    if (!bs_lock_init) return;
    EnterCriticalSection(&bs_lock);
    for (int i = 0; i < len; i++) {
        raw_ansi_buf[ra_head] = data[i];
        ra_head = (ra_head + 1) % RAW_ANSI_SIZE;
        if (ra_len < RAW_ANSI_SIZE) ra_len++;
    }
    LeaveCriticalSection(&bs_lock);
}

static char *ra_get_text(int *out_len)
{
    if (!bs_lock_init || ra_len == 0) { *out_len = 0; return NULL; }
    EnterCriticalSection(&bs_lock);
    char *text = (char *)malloc(ra_len + 1);
    if (text) {
        int start = (ra_head - ra_len + RAW_ANSI_SIZE) % RAW_ANSI_SIZE;
        for (int i = 0; i < ra_len; i++)
            text[i] = raw_ansi_buf[(start + i) % RAW_ANSI_SIZE];
        text[ra_len] = 0;
        *out_len = ra_len;
    } else {
        *out_len = 0;
    }
    LeaveCriticalSection(&bs_lock);
    return text;
}

/* ---- Colored cell ring buffer — captures scrolled-off lines with attrs ---- */

#define CB_MAX_LINES  8000
#define CB_COLS       AP_COLS

static ap_cell_t cb_lines[CB_MAX_LINES][CB_COLS];
static int cb_head = 0;    /* next write position */
static int cb_count = 0;   /* total lines stored (max CB_MAX_LINES) */

/* Scroll callback — called inside ansi_lock from ap_scroll_up */
static void cb_scroll_cb(const ap_cell_t *row, int cols, void *user)
{
    (void)user;
    int n = (cols < CB_COLS) ? cols : CB_COLS;
    memcpy(cb_lines[cb_head], row, n * sizeof(ap_cell_t));
    /* Zero-fill remainder if cols < CB_COLS */
    if (n < CB_COLS)
        memset(&cb_lines[cb_head][n], 0, (CB_COLS - n) * sizeof(ap_cell_t));
    cb_head = (cb_head + 1) % CB_MAX_LINES;
    if (cb_count < CB_MAX_LINES) cb_count++;
}

/* Get a specific line from the ring (0 = oldest). Returns NULL if out of range. */
static ap_cell_t *cb_get_line(int idx)
{
    if (idx < 0 || idx >= cb_count) return NULL;
    int ring_idx = (cb_head - cb_count + idx + CB_MAX_LINES) % CB_MAX_LINES;
    return cb_lines[ring_idx];
}

/* ---- Clipboard helpers ---- */

static void clipboard_paste_into(char *buf, int *len, int *cursor, int max_len)
{
    if (!OpenClipboard(NULL)) return;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char *text = (const char *)GlobalLock(h);
        if (text) {
            int tlen = (int)strlen(text);
            /* Clamp to available space */
            int avail = max_len - 1 - *len;
            if (tlen > avail) tlen = avail;
            if (tlen > 0) {
                memmove(buf + *cursor + tlen, buf + *cursor, *len - *cursor + 1);
                memcpy(buf + *cursor, text, tlen);
                *len += tlen;
                *cursor += tlen;
                buf[*len] = 0;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

static void clipboard_copy(const char *text, int len)
{
    if (!text || len <= 0) return;
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (h) {
        char *p = (char *)GlobalLock(h);
        memcpy(p, text, len);
        p[len] = 0;
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

/* rm-block filter that operates on the grid AFTER ap_feed has parsed a
 * line. The existing ANSI state machine already handles CSI/ANSI/CR/LF
 * correctly — we just inspect the row contents at \n time and decide
 * whether to blank the row.
 *
 * We only filter rows that came from an *auto* rm injection (triggered by
 * the RM-Every-Step thread in msimg32_proxy). pro_step_thread increments
 * auto_rm_queue_count before typing "rm\r"; we peek it when a row closes,
 * consume one slot on the Location row, and then stay in a "block active"
 * state so the following Regen Time + Room Illu rows get suppressed too.
 *
 * Manual rms (user-typed at any source) are displayed in full — their
 * Location row closes with queue=0, we don't enter block state, nothing
 * is suppressed.
 *
 * The "Show full RM debug text" Extras toggle (rm_show_debug) bypasses
 * filtering entirely for debugging. */
static int rm_show_debug = 0;   /* persisted; bypass filter when 1 */

static int row_contains(ap_term_t *t, int row, const char *s)
{
    int slen = (int)strlen(s);
    if (row < 0 || row >= t->rows || slen <= 0) return 0;
    for (int col = 0; col + slen <= t->cols; col++) {
        int match = 1;
        for (int k = 0; k < slen; k++) {
            if ((char)t->grid[row][col + k].ch != s[k]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

/* True if row's only non-prompt / non-whitespace content is the literal
 * echo "rm" — i.e. the auto-typed command, not a normal prompt where the
 * user typed something. We scan from the last ']' (end of prompt like
 * "[HP=XX]:") to the end and require the trailing chars to be exactly "rm". */
static int row_is_rm_echo(ap_term_t *t, int row)
{
    if (row < 0 || row >= t->rows) return 0;
    int last_bracket = -1;
    for (int c = 0; c < t->cols; c++) {
        if ((char)t->grid[row][c].ch == ']') last_bracket = c;
    }
    int start = (last_bracket >= 0) ? last_bracket + 1 : 0;
    /* skip the ':' right after prompt */
    while (start < t->cols && (char)t->grid[row][start].ch == ':') start++;
    /* skip leading spaces */
    while (start < t->cols && (char)t->grid[row][start].ch == ' ') start++;
    if (start + 1 >= t->cols) return 0;
    if ((char)t->grid[row][start].ch != 'r') return 0;
    if ((char)t->grid[row][start + 1].ch != 'm') return 0;
    /* anything after "rm" must be space (or end) */
    for (int c = start + 2; c < t->cols; c++) {
        char ch = (char)t->grid[row][c].ch;
        if (ch != ' ' && ch != 0) return 0;
    }
    return 1;
}

static int row_has_location(ap_term_t *t, int row)   { return row_contains(t, row, "Location:"); }
static int row_has_regen(ap_term_t *t, int row)      { return row_contains(t, row, "Regen Time:"); }
static int row_has_illu(ap_term_t *t, int row)       { return row_contains(t, row, "Room Illu:"); }

static void row_blank(ap_term_t *t, int row)
{
    for (int c = 0; c < t->cols; c++) {
        t->grid[row][c].ch = ' ';
        t->grid[row][c].attr = (ap_attr_t){ 7, 0, 0, 0, 0, 0, 0 };
    }
}

/* True while we're mid auto-rm response block (Location → Regen → Illu).
 * Set when we consume the queue on a Location row; cleared after the
 * third suppressed row (Room Illu:). */
static int rm_block_remaining = 0;

/* Wrapper for ap_feed that suppresses auto-rm rows at \n time. */
static void ap_feed_rm_filtered(ap_term_t *t, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\n' && !rm_show_debug) {
            int suppress = 0;
            if (rm_block_remaining > 0) {
                /* Inside an auto-rm response — suppress Regen / Illu lines. */
                if (row_has_regen(t, t->cy) || row_has_illu(t, t->cy)) {
                    suppress = 1;
                    rm_block_remaining--;
                } else {
                    /* Unexpected row inside block — bail out of block state
                     * so we don't accidentally swallow real output. */
                    rm_block_remaining = 0;
                }
            } else if (row_has_location(t, t->cy)) {
                /* Location row — consume a queued auto-rm slot. If we got
                 * one, this is ours: suppress and arm the block state. */
                if (auto_rm_queue_consume_safe()) {
                    suppress = 1;
                    rm_block_remaining = 2;  /* Regen + Illu still coming */
                }
            } else if (row_is_rm_echo(t, t->cy) && auto_rm_queue_peek_safe() > 0) {
                /* The "rm" command echo row right before the response block.
                 * Peek (not consume) — we consume on the Location row. */
                suppress = 1;
            }
            if (suppress) {
                row_blank(t, t->cy);
                t->cx = 0;
                continue;
            }
        }
        ap_feed_byte(t, c);
    }
}

/* on_data callback — raw server bytes fed to state machine + backscroll */
static void vkt_on_data(const char *data, int len)
{
    EnterCriticalSection(&ansi_lock);
    ap_feed_rm_filtered(&ansi_term, (const uint8_t *)data, len);
    LeaveCriticalSection(&ansi_lock);
    bs_append(data, len);
    ra_append(data, len);
    pst_feed(data, len);
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

/* Free-form quad: 4 arbitrary corners (clockwise or CCW), solid color.
 * Uses glyph 219 (solid block) UVs so fragment shader outputs solid fill.
 * All positions in NDC [-1,1]. */
static void push_quad_free(float x0, float y0, float x1, float y1,
                           float x2, float y2, float x3, float y3,
                           float r, float g, float b, float a)
{
    if (quad_count >= MAX_QUADS) return;
    int vi = quad_count * 4;
    /* Solid fill UVs — center of glyph 219 */
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    float hp_u = 0.5f / (float)cur_atlas_w, hp_v = 0.5f / (float)cur_atlas_h;
    float su = (219 % 16) * tex_cw + tex_cw * 0.5f;
    float sv = (219 / 16) * tex_ch + tex_ch * 0.5f;
    vk_vdata[vi+0] = (vertex_t){ x0, y0, su, sv, r, g, b, a };
    vk_vdata[vi+1] = (vertex_t){ x1, y1, su, sv, r, g, b, a };
    vk_vdata[vi+2] = (vertex_t){ x2, y2, su, sv, r, g, b, a };
    vk_vdata[vi+3] = (vertex_t){ x3, y3, su, sv, r, g, b, a };
    quad_count++;
}

/* Helper: pixel to NDC with wobble warp */
static inline float wob_px2ndc_x(float px, int vp_w) { return (px / (float)vp_w) * 2.0f - 1.0f; }
static inline float wob_px2ndc_y(float py, int vp_h) { return (py / (float)vp_h) * 2.0f - 1.0f; }

/* Push a warped quad — each corner independently transformed through wobble grid */
static void push_quad_wobbled(float x0, float y0, float x1, float y1,
                               float u0, float v0, float u1, float v1,
                               float r, float g, float b, float a,
                               int vp_w, int vp_h)
{
    float wx0, wy0, wx1, wy1, wx2, wy2, wx3, wy3;
    wob_transform(x0, y0, &wx0, &wy0); /* top-left */
    wob_transform(x1, y0, &wx1, &wy1); /* top-right */
    wob_transform(x1, y1, &wx2, &wy2); /* bottom-right */
    wob_transform(x0, y1, &wx3, &wy3); /* bottom-left */

    if (quad_count >= MAX_QUADS) return;
    int vi = quad_count * 4;
    vk_vdata[vi+0] = (vertex_t){ wob_px2ndc_x(wx0,vp_w), wob_px2ndc_y(wy0,vp_h), u0, v0, r, g, b, a };
    vk_vdata[vi+1] = (vertex_t){ wob_px2ndc_x(wx1,vp_w), wob_px2ndc_y(wy1,vp_h), u1, v0, r, g, b, a };
    vk_vdata[vi+2] = (vertex_t){ wob_px2ndc_x(wx2,vp_w), wob_px2ndc_y(wy2,vp_h), u1, v1, r, g, b, a };
    vk_vdata[vi+3] = (vertex_t){ wob_px2ndc_x(wx3,vp_w), wob_px2ndc_y(wy3,vp_h), u0, v1, r, g, b, a };
    quad_count++;
}

/* Push a solid filled rect (uses glyph 219 █ for solid pixels) */
static void push_solid(float x0, float y0, float x1, float y1,
                       float r, float g, float b, float a,
                       int vp_w, int vp_h)
{
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    float hp_u = 0.5f / (float)cur_atlas_w, hp_v = 0.5f / (float)cur_atlas_h;
    float su0 = (219 % 16) * tex_cw + hp_u, sv0 = (219 / 16) * tex_ch + hp_v;
    float su1 = su0 + tex_cw - 2*hp_u, sv1 = sv0 + tex_ch - 2*hp_v;
    if (wob_warp_active) {
        push_quad_wobbled(x0, y0, x1, y1, su0, sv0, su1, sv1, r, g, b, a, vp_w, vp_h);
        return;
    }
    #define S2X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define S2Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)
    push_quad(S2X(x0), S2Y(y0), S2X(x1), S2Y(y1),
              su0, sv0, su1, sv1, r, g, b, a);
    #undef S2X
    #undef S2Y
}

/* ---- QR Code Pairing Overlay (implementations) ---- */

static void qr_show(void)
{
    ab_resolve();
    if (ab_resolved != 1) return;
    int sz = pfn_ab_gen_qr();
    if (sz <= 0) return;
    const char *cs = pfn_ab_conn_str();
    if (cs) strncpy(qr_conn_str, cs, sizeof(qr_conn_str) - 1);
    qr_visible = 1;
    qr_paired  = 0;
    qr_paired_tick = 0;
    qr_show_tick = GetTickCount();
}

static void qr_draw(int vp_w, int vp_h)
{
    if (!qr_visible) return;
    ab_resolve();
    if (ab_resolved != 1) { qr_visible = 0; return; }

    if (!qr_paired && pfn_ab_client_count && pfn_ab_client_count() > 0) {
        qr_paired = 1;
        qr_paired_tick = GetTickCount();
    }
    if (qr_paired && GetTickCount() - qr_paired_tick > 2500) {
        qr_visible = 0;
        qr_paired  = 0;
        return;
    }

    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int pad = (int)(12 * ui_scale);
    int text_h = ch + 4;

    if (qr_paired) {
        int pw = (int)(320 * ui_scale), ph = text_h * 3 + pad * 2;
        int px0 = (vp_w - pw) / 2, py0 = (vp_h - ph) / 2;

        push_solid(0, 0, vp_w, vp_h, 0.0f, 0.0f, 0.0f, 0.6f, vp_w, vp_h);
        push_solid(px0, py0, px0 + pw, py0 + ph,
                   t->bg[0], t->bg[1], t->bg[2], 0.95f, vp_w, vp_h);
        push_solid(px0, py0, px0 + pw, py0 + 1,
                   0.0f, 0.8f, 0.4f, 1.0f, vp_w, vp_h);
        push_solid(px0, py0 + ph - 1, px0 + pw, py0 + ph,
                   0.0f, 0.8f, 0.4f, 1.0f, vp_w, vp_h);
        push_solid(px0, py0, px0 + 1, py0 + ph,
                   0.0f, 0.8f, 0.4f, 1.0f, vp_w, vp_h);
        push_solid(px0 + pw - 1, py0, px0 + pw, py0 + ph,
                   0.0f, 0.8f, 0.4f, 1.0f, vp_w, vp_h);

        push_text(px0 + pad, py0 + pad, "PAIRING SUCCESSFUL",
                  0.0f, 1.0f, 0.5f, vp_w, vp_h, cw, ch);
        push_text(px0 + pad, py0 + pad + text_h, "Device connected",
                  t->text[0] * 0.7f, t->text[1] * 0.7f, t->text[2] * 0.7f,
                  vp_w, vp_h, cw, ch);
        return;
    }

    int qr_sz = pfn_ab_qr_size();
    if (qr_sz <= 0) { qr_visible = 0; return; }

    int quiet = 2;
    int modules = qr_sz + quiet * 2;

    int max_px = (int)(vp_h * 0.40f);
    int cell_px = max_px / modules;
    if (cell_px < 3) cell_px = 3;
    int qr_px = modules * cell_px;

    int panel_w = qr_px + pad * 2;
    int panel_h = text_h + pad + qr_px + pad + text_h * 2 + pad;
    if (panel_w < 300) panel_w = 300;
    int px0 = (vp_w - panel_w) / 2;
    int py0 = (vp_h - panel_h) / 2;

    push_solid(0, 0, vp_w, vp_h, 0.0f, 0.0f, 0.0f, 0.6f, vp_w, vp_h);

    push_solid(px0, py0, px0 + panel_w, py0 + panel_h,
               t->bg[0], t->bg[1], t->bg[2], 0.95f, vp_w, vp_h);

    push_solid(px0, py0, px0 + panel_w, py0 + 1,
               t->accent[0], t->accent[1], t->accent[2], 0.8f, vp_w, vp_h);
    push_solid(px0, py0 + panel_h - 1, px0 + panel_w, py0 + panel_h,
               t->accent[0], t->accent[1], t->accent[2], 0.8f, vp_w, vp_h);
    push_solid(px0, py0, px0 + 1, py0 + panel_h,
               t->accent[0], t->accent[1], t->accent[2], 0.8f, vp_w, vp_h);
    push_solid(px0 + panel_w - 1, py0, px0 + panel_w, py0 + panel_h,
               t->accent[0], t->accent[1], t->accent[2], 0.8f, vp_w, vp_h);

    int ty = py0 + pad / 2;
    push_text(px0 + pad, ty, "Scan to Pair",
              t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    push_text(px0 + panel_w - pad - cw, ty, "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);

    int qr_x0 = px0 + (panel_w - qr_px) / 2;
    int qr_y0 = ty + text_h + pad / 2;

    push_solid(qr_x0, qr_y0, qr_x0 + qr_px, qr_y0 + qr_px,
               1.0f, 1.0f, 1.0f, 1.0f, vp_w, vp_h);

    for (int my = 0; my < qr_sz; my++) {
        for (int mx = 0; mx < qr_sz; mx++) {
            if (pfn_ab_qr_mod(mx, my)) {
                int cx = qr_x0 + (mx + quiet) * cell_px;
                int cy = qr_y0 + (my + quiet) * cell_px;
                push_solid(cx, cy, cx + cell_px, cy + cell_px,
                           0.0f, 0.0f, 0.0f, 1.0f, vp_w, vp_h);
            }
        }
    }

    int info_y = qr_y0 + qr_px + pad / 2;
    if (qr_conn_str[0]) {
        char addr[128];
        const char *p = qr_conn_str;
        if (strncmp(p, "slop://", 7) == 0) p += 7;
        const char *slash = strchr(p, '/');
        if (slash) {
            int n = (int)(slash - p);
            if (n > (int)sizeof(addr) - 1) n = (int)sizeof(addr) - 1;
            memcpy(addr, p, n);
            addr[n] = 0;
        } else {
            strncpy(addr, p, sizeof(addr) - 1);
        }
        push_text(px0 + pad, info_y, addr,
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    }
    DWORD elapsed = GetTickCount() - qr_show_tick;
    if (elapsed >= QR_EXPIRY_MS) {
        qr_visible = 0;
        return;
    }
    int secs_left = (int)((QR_EXPIRY_MS - elapsed) / 1000);
    char timer_buf[64];
    _snprintf(timer_buf, sizeof(timer_buf), "Expires in %d:%02d - scan now",
              secs_left / 60, secs_left % 60);
    push_text(px0 + pad, info_y + text_h, timer_buf,
              secs_left < 30 ? 0.8f : t->text[0] * 0.5f,
              secs_left < 30 ? 0.3f : t->text[1] * 0.5f,
              secs_left < 30 ? 0.2f : t->text[2] * 0.5f,
              vp_w, vp_h, cw, ch);
}

static int qr_mouse_down(int mx, int my)
{
    if (!qr_visible) return 0;
    (void)mx; (void)my;
    qr_visible = 0;
    return 1;
}

/* Terminal visualizations (boulders, matrix rain, vectroids) */
#include "viz_fx.c"

/* Push a text string at pixel position */
static void push_text(int px, int py, const char *str,
                      float r, float g, float b,
                      int vp_w, int vp_h, int char_w, int char_h)
{
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    float hp_u = 0.5f / (float)cur_atlas_w, hp_v = 0.5f / (float)cur_atlas_h;
    #define T2X(p) (((float)(p) / (float)vp_w) * 2.0f - 1.0f)
    #define T2Y(p) (((float)(p) / (float)vp_h) * 2.0f - 1.0f)
    for (int i = 0; str[i]; i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch <= 32) { px += char_w; continue; }
        float u0 = (ch % 16) * tex_cw + hp_u, v0 = (ch / 16) * tex_ch + hp_v;
        if (wob_warp_active) {
            push_quad_wobbled((float)px, (float)py, (float)(px+char_w), (float)(py+char_h),
                              u0, v0, u0 + tex_cw - 2*hp_u, v0 + tex_ch - 2*hp_v,
                              r, g, b, 1.0f, vp_w, vp_h);
        } else {
            push_quad(T2X(px), T2Y(py), T2X(px + char_w), T2Y(py + char_h),
                      u0, v0, u0 + tex_cw - 2*hp_u, v0 + tex_ch - 2*hp_v, r, g, b, 1.0f);
        }
        px += char_w;
    }
    #undef T2X
    #undef T2Y
}

/* Push solid/text using UI font atlas (for P&L window — always high-quality TTF) */
static void push_solid_ui(float x0, float y0, float x1, float y1,
                           float r, float g, float b, float a,
                           int vp_w, int vp_h)
{
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    float hp_u = 0.5f / (float)ui_atlas_w, hp_v = 0.5f / (float)ui_atlas_h;
    float su0 = (219 % 16) * tex_cw + hp_u, sv0 = (219 / 16) * tex_ch + hp_v;
    float su1 = su0 + tex_cw - 2*hp_u, sv1 = sv0 + tex_ch - 2*hp_v;
    if (wob_warp_active) {
        push_quad_wobbled(x0, y0, x1, y1, su0, sv0, su1, sv1, r, g, b, a, vp_w, vp_h);
        return;
    }
    #define US2X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define US2Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)
    push_quad(US2X(x0), US2Y(y0), US2X(x1), US2Y(y1),
              su0, sv0, su1, sv1, r, g, b, a);
    #undef US2X
    #undef US2Y
}

static void push_text_ui(int px, int py, const char *str,
                          float r, float g, float b,
                          int vp_w, int vp_h, int char_w, int char_h)
{
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    float hp_u = 0.5f / (float)ui_atlas_w, hp_v = 0.5f / (float)ui_atlas_h;
    #define UT2X(p) (((float)(p) / (float)vp_w) * 2.0f - 1.0f)
    #define UT2Y(p) (((float)(p) / (float)vp_h) * 2.0f - 1.0f)
    for (int i = 0; str[i]; i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch <= 32) { px += char_w; continue; }
        float u0 = (ch % 16) * tex_cw + hp_u, v0 = (ch / 16) * tex_ch + hp_v;
        if (wob_warp_active) {
            push_quad_wobbled((float)px, (float)py, (float)(px+char_w), (float)(py+char_h),
                              u0, v0, u0 + tex_cw - 2*hp_u, v0 + tex_ch - 2*hp_v,
                              r, g, b, 1.0f, vp_w, vp_h);
        } else {
            push_quad(UT2X(px), UT2Y(py), UT2X(px + char_w), UT2Y(py + char_h),
                      u0, v0, u0 + tex_cw - 2*hp_u, v0 + tex_ch - 2*hp_v, r, g, b, 1.0f);
        }
        px += char_w;
    }
    #undef UT2X
    #undef UT2Y
}

/* Vulkan-rendered Map Walker panel (defined in vk_mudmap.h). Must be included
 * after ui_font_ready, push_solid_ui, push_text_ui, ui_themes, and VSB_CHAR_*
 * are declared. */
#include "vk_filepicker.h"
#include "vk_mudmap.h"

/* ---- Vulkan menu rendering ---- */

static int vkm_is_sep(int idx) { return idx == VKM_ITEM_SEP || idx == VKM_ITEM_SEP2; }

static void vkm_get_root_item_rect(int idx, int *iy, int *ih)
{
    int y = vkm_y + VKM_PAD;
    for (int i = 0; i < idx; i++)
        y += vkm_is_sep(i) ? VKM_SEP_H : VKM_ITEM_H;
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
    if (vkm_sub == VKM_SUB_VISUAL) return VKM_VIS_COUNT;
    if (vkm_sub == VKM_SUB_WIDGETS) return VKM_WID_COUNT;
    if (vkm_sub == VKM_SUB_RECENT) return vkm_goto_count > 0 ? vkm_goto_count : 1;
    if (vkm_sub == VKM_SUB_THEME) return NUM_THEMES;
    if (vkm_sub == VKM_SUB_FONT) return NUM_TTF_FONTS + 1; /* +1 for bitmap */
    if (vkm_sub == VKM_SUB_FX) return vk_experimental ? VKM_FX_COUNT : (VKM_FX_COUNT - 1);
    if (vkm_sub == VKM_SUB_EXTRAS) return VKM_EXT_COUNT;
    if (vkm_sub == VKM_SUB_XTRA2) return VKM_X2_COUNT;
    if (vkm_sub == VKM_SUB_VIZ) return VKM_VIZ_COUNT;
    if (vkm_sub == VKM_SUB_BG) return VKM_BG_COUNT;
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
static void vkm_get_sub_rect(int *out_sx, int *out_sy)
{
    int parent;
    int is_3rd_level = 0; /* 3rd-level submenus nest to the right of 2nd-level */

    if (vkm_sub == VKM_SUB_VISUAL)
        parent = VKM_ITEM_VISUAL;
    else if (vkm_sub == VKM_SUB_THEME || vkm_sub == VKM_SUB_FONT || vkm_sub == VKM_SUB_FX) {
        parent = VKM_ITEM_VISUAL;
        is_3rd_level = 1;
    } else if (vkm_sub == VKM_SUB_WIDGETS)
        parent = VKM_ITEM_WIDGETS;
    else if (vkm_sub == VKM_SUB_RECENT)
        parent = VKM_ITEM_RECENT;
    else if (vkm_sub == VKM_SUB_EXTRAS)
        parent = VKM_ITEM_EXTRAS;
    else if (vkm_sub == VKM_SUB_XTRA2)
        parent = VKM_ITEM_XTRA2;
    else if (vkm_sub == VKM_SUB_VIZ || vkm_sub == VKM_SUB_BG) {
        parent = VKM_ITEM_XTRA2;
        is_3rd_level = 1;
    } else
        parent = VKM_ITEM_VISUAL;

    int parent_y, parent_h;
    vkm_get_root_item_rect(parent, &parent_y, &parent_h);
    int sx = vkm_x + VKM_ROOT_W - 1;
    int sy = parent_y;
    if (is_3rd_level) sx += VKM_SUB_W - 1; /* offset further right */
    int sh = vkm_sub_height();
    /* Clamp to screen — must match draw code exactly */
    int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
    if (sy + sh > vp_h) sy = vp_h - sh;
    if (sx + VKM_SUB_W > vp_w) {
        if (is_3rd_level)
            sx = vkm_x + VKM_ROOT_W - 1 - VKM_SUB_W + 1; /* flip 3rd-level to left of 2nd-level */
        else
            sx = vkm_x - VKM_SUB_W + 1; /* flip 2nd-level to left of root */
        if (sx < 0) sx = 0;
    }
    *out_sx = sx;
    *out_sy = sy;
}

static int vkm_hit_sub(int mx, int my)
{
    if (vkm_sub == VKM_SUB_NONE) return -1;
    int sx, sy;
    vkm_get_sub_rect(&sx, &sy);

    if (mx < sx || mx >= sx + VKM_SUB_W) return -1;
    int count = vkm_sub_count();
    for (int i = 0; i < count; i++) {
        int sy_item = sy + VKM_PAD + i * VKM_ITEM_H;
        if (my >= sy_item && my < sy_item + VKM_ITEM_H) return i;
    }
    return -1;
}

/* Draw a bordered panel with drop shadow */
static void vkm_draw_panel(int x, int y, int w, int h,
                           const ui_theme_t *t, int vp_w, int vp_h)
{
    /* Drop shadow */
    /* Drop shadow */
    push_solid(x + VKM_SHADOW, y + VKM_SHADOW,
               x + w + VKM_SHADOW, y + h + VKM_SHADOW,
               0.0f, 0.0f, 0.0f, 0.4f, vp_w, vp_h);
    /* Flat background — slightly lighter than theme bg */
    float pbr = t->bg[0] + 0.06f, pbg = t->bg[1] + 0.06f, pbb = t->bg[2] + 0.06f;
    push_solid(x, y, x + w, y + h, pbr, pbg, pbb, 0.95f, vp_w, vp_h);
    /* Thin 1px border — subtle, accent-tinted */
    float br = t->accent[0] * 0.3f + t->bg[0] * 0.3f;
    float bg = t->accent[1] * 0.3f + t->bg[1] * 0.3f;
    float bb = t->accent[2] * 0.3f + t->bg[2] * 0.3f;
    push_solid(x, y, x + w, y + 1, br, bg, bb, 0.6f, vp_w, vp_h);
    push_solid(x, y + h - 1, x + w, y + h, br, bg, bb, 0.6f, vp_w, vp_h);
    push_solid(x, y, x + 1, y + h, br, bg, bb, 0.6f, vp_w, vp_h);
    push_solid(x + w - 1, y, x + w, y + h, br, bg, bb, 0.6f, vp_w, vp_h);
}

/* ---- Vulkan Round Timer Widget ---- */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float vrt_sinf(float x) {
    /* Reduce to [-PI, PI] for Taylor accuracy */
    float pi2 = 2.0f * (float)M_PI;
    while (x > (float)M_PI)  x -= pi2;
    while (x < -(float)M_PI) x += pi2;
    float x2 = x * x;
    float r = x;
    float term = x;
    term *= -x2 / (2.0f * 3.0f); r += term;
    term *= -x2 / (4.0f * 5.0f); r += term;
    term *= -x2 / (6.0f * 7.0f); r += term;
    term *= -x2 / (8.0f * 9.0f); r += term;
    term *= -x2 / (10.0f * 11.0f); r += term;
    term *= -x2 / (12.0f * 13.0f); r += term;
    return r;
}

static float vrt_cosf(float x) {
    return vrt_sinf(x + (float)M_PI / 2.0f);
}

static float vrt_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    /* Newton's method */
    float r = x;
    for (int i = 0; i < 8; i++) r = 0.5f * (r + x / r);
    return r;
}

/* Pixel to NDC helpers for round timer (need viewport dims) */
#define VRT_X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
#define VRT_Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)

/* Push a filled arc (ring wedge) using proper trapezoid quads in NDC.
 * Each segment is 4 vertices: outer0, outer1, inner1, inner0 */
/* DPI scale factor: sizes authored for 1080p, scale proportionally */
static float vrt_dpi_scale(int vp_h) {
    return (float)vp_h / 1080.0f;
}

/* Adaptive segment count based on pixel radius — smooth at any DPI */
static int vrt_auto_segs(float pixel_radius, int base_segs) {
    int s = (int)(pixel_radius * 0.8f);
    if (s < base_segs) s = base_segs;
    if (s > 256) s = 256;
    return s;
}

/* Feather width in pixels for anti-aliased edges */
#define VRT_FEATHER 1.5f

static void vrt_push_arc(float cx, float cy, float r_inner, float r_outer,
                         float angle_start, float angle_end,
                         float r1, float g1, float b1,
                         float r2, float g2, float b2,
                         float alpha, int segs, int vp_w, int vp_h)
{
    if (angle_end <= angle_start) return;
    float step = (angle_end - angle_start) / (float)segs;
    float feather = VRT_FEATHER;
    float fi = r_inner - feather; if (fi < 0) fi = 0;
    float fo = r_outer + feather;
    for (int i = 0; i < segs; i++) {
        float t = ((float)i + 0.5f) / (float)segs;
        float a0 = angle_start + step * (float)i;
        float a1 = angle_start + step * (float)(i + 1);
        float cr = r1 + (r2 - r1) * t;
        float cg = g1 + (g2 - g1) * t;
        float cb = b1 + (b2 - b1) * t;
        float c0 = vrt_cosf(a0), s0 = vrt_sinf(a0);
        float c1 = vrt_cosf(a1), s1 = vrt_sinf(a1);

        /* Inner feather strip (alpha 0 → alpha) */
        {
            float ox0 = cx + r_inner * c0, oy0 = cy - r_inner * s0;
            float ox1 = cx + r_inner * c1, oy1 = cy - r_inner * s1;
            float ix0f = cx + fi * c0,     iy0f = cy - fi * s0;
            float ix1f = cx + fi * c1,     iy1f = cy - fi * s1;
            push_quad_free(VRT_X(ox0), VRT_Y(oy0), VRT_X(ox1), VRT_Y(oy1),
                           VRT_X(ix1f), VRT_Y(iy1f), VRT_X(ix0f), VRT_Y(iy0f),
                           cr, cg, cb, alpha * 0.5f);
        }
        /* Main body */
        {
            float ox0 = cx + r_outer * c0, oy0 = cy - r_outer * s0;
            float ox1 = cx + r_outer * c1, oy1 = cy - r_outer * s1;
            float ix0m = cx + r_inner * c0, iy0m = cy - r_inner * s0;
            float ix1m = cx + r_inner * c1, iy1m = cy - r_inner * s1;
            push_quad_free(VRT_X(ox0), VRT_Y(oy0), VRT_X(ox1), VRT_Y(oy1),
                           VRT_X(ix1m), VRT_Y(iy1m), VRT_X(ix0m), VRT_Y(iy0m),
                           cr, cg, cb, alpha);
        }
        /* Outer feather strip (alpha → 0) */
        {
            float ox0f = cx + fo * c0,     oy0f = cy - fo * s0;
            float ox1f = cx + fo * c1,     oy1f = cy - fo * s1;
            float ox0 = cx + r_outer * c0, oy0 = cy - r_outer * s0;
            float ox1 = cx + r_outer * c1, oy1 = cy - r_outer * s1;
            push_quad_free(VRT_X(ox0f), VRT_Y(oy0f), VRT_X(ox1f), VRT_Y(oy1f),
                           VRT_X(ox1), VRT_Y(oy1), VRT_X(ox0), VRT_Y(oy0),
                           cr, cg, cb, alpha * 0.5f);
        }
    }
}

/* Push a filled circle with soft feathered edge */
static void vrt_push_circle(float cx, float cy, float radius,
                            float r, float g, float b, float a,
                            int segs, int vp_w, int vp_h)
{
    float step = 2.0f * (float)M_PI / (float)segs;
    float feather = VRT_FEATHER;
    float ri = radius - feather; if (ri < 0) ri = 0;
    /* Solid inner disc */
    for (int i = 0; i < segs; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)(i + 1);
        float x0 = cx + ri * vrt_cosf(a0);
        float y0 = cy - ri * vrt_sinf(a0);
        float x1 = cx + ri * vrt_cosf(a1);
        float y1 = cy - ri * vrt_sinf(a1);
        push_quad_free(VRT_X(cx), VRT_Y(cy), VRT_X(x0), VRT_Y(y0),
                       VRT_X(x1), VRT_Y(y1), VRT_X(cx), VRT_Y(cy),
                       r, g, b, a);
    }
    /* Feathered edge ring (alpha → 0) */
    for (int i = 0; i < segs; i++) {
        float a0 = step * (float)i;
        float a1 = step * (float)(i + 1);
        float c0 = vrt_cosf(a0), s0 = vrt_sinf(a0);
        float c1 = vrt_cosf(a1), s1 = vrt_sinf(a1);
        float ix0 = cx + ri * c0,       iy0 = cy - ri * s0;
        float ix1 = cx + ri * c1,       iy1 = cy - ri * s1;
        float ox0 = cx + radius * c0,   oy0 = cy - radius * s0;
        float ox1 = cx + radius * c1,   oy1 = cy - radius * s1;
        push_quad_free(VRT_X(ox0), VRT_Y(oy0), VRT_X(ox1), VRT_Y(oy1),
                       VRT_X(ix1), VRT_Y(iy1), VRT_X(ix0), VRT_Y(iy0),
                       r, g, b, a * 0.4f);
    }
}

/* Push a soft glow circle — multiple concentric layers with decreasing alpha */
static void vrt_push_glow(float cx, float cy, float radius, int layers,
                          float r, float g, float b, float peak_alpha,
                          int segs, int vp_w, int vp_h)
{
    for (int l = 0; l < layers; l++) {
        float t = (float)l / (float)layers;
        float lr = radius * (0.3f + 0.7f * t);
        float la = peak_alpha * (1.0f - t * t); /* quadratic falloff */
        vrt_push_circle(cx, cy, lr, r, g, b, la, segs, vp_w, vp_h);
    }
}

static void vrt_spawn_particle(float x, float y, float r, float g, float b)
{
    int idx = vrt_particle_idx % VRT_PARTICLE_MAX;
    vrt_particles[idx].x = x;
    vrt_particles[idx].y = y;
    vrt_particles[idx].vx = ((float)((int)(fx_time * 1000) % 100) - 50.0f) / 2000.0f;
    vrt_particles[idx].vy = ((float)((int)(fx_time * 1337) % 100) - 50.0f) / 2000.0f;
    vrt_particles[idx].alpha = 0.8f;
    vrt_particles[idx].r = r;
    vrt_particles[idx].g = g;
    vrt_particles[idx].b = b;
    vrt_particles[idx].alive = 1;
    vrt_particle_idx++;
}

static void vrt_draw(int vp_w, int vp_h)
{
    if (!vrt_visible) return;

    const ui_theme_t *t = &ui_themes[current_theme];
    float sz = vrt_size;
    float cx = vrt_x + sz / 2.0f;
    float cy = vrt_y + sz / 2.0f;
    float R = sz / 2.0f;
    int hi_segs = vrt_auto_segs(R, 64);  /* adaptive segment count */
    int lo_segs = hi_segs / 2; if (lo_segs < 24) lo_segs = 24;
    DWORD now = GetTickCount();
    double now_d = (double)GetTickCount64();

    /* Progress */
    float progress = 0.0f;
    if (vrt_synced && vrt_last_tick_ts > 0.0 && vrt_round_period > 0.0) {
        double elapsed = now_d - vrt_last_tick_ts;
        progress = (float)(elapsed / vrt_round_period);
        /* Wrap progress to 0..1 range each 5s cycle */
        if (progress > 1.0f) {
            progress = progress - (float)(int)progress;
        }
    }

    /* ---- Beat maneuvers + pulse envelope (advances once per frame) ---- */
    vrt_update_maneuvers();
    float beat_env = 0.0f;
    {
        float pt = fx_time - vrt_bass_pulse_time;
        if (pt < 0.0f) pt = 0.0f;
        /* Fast attack (~60ms), exponential decay ~0.28s */
        float attack = 1.0f - (pt < 0.06f ? (0.06f - pt) / 0.06f : 0.0f);
        float decay_t = pt * 3.6f;
        float decay = 1.0f / (1.0f + decay_t * decay_t * 1.5f);
        beat_env = attack * decay;
        if (beat_env < 0.0f) beat_env = 0.0f;
        if (beat_env > 1.0f) beat_env = 1.0f;
    }
    float man_t01 = (vrt_man_current >= 0)
                  ? (fx_time - vrt_man_start) / VRT_MANEUVER_DUR : 0.0f;
    if (man_t01 < 0.0f) man_t01 = 0.0f;
    if (man_t01 > 1.0f) man_t01 = 1.0f;

    /* Pre-compute the round-timer progress color (green -> yellow -> red).
     * Used for the outer beat-glow and to flag "danger" on red. */
    float glow_col_r, glow_col_g, glow_col_b, red_danger;
    {
        float p_col = progress; if (p_col > 1.0f) p_col = 1.0f;
        /* Match the main arc gradient: green (low) -> yellow (mid) -> red (high) */
        if (p_col < 0.5f) {
            float tt = p_col * 2.0f;
            glow_col_r = t->accent[0]*0.2f + 0.1f + 0.6f*tt;
            glow_col_g = t->accent[1]*0.2f + 0.7f;
            glow_col_b = t->accent[2]*0.2f + 0.15f - 0.05f*tt;
        } else {
            float tt = (p_col - 0.5f) * 2.0f;
            glow_col_r = t->accent[0]*0.2f + 0.8f;
            glow_col_g = t->accent[1]*0.2f + 0.7f - 0.55f*tt;
            glow_col_b = t->accent[2]*0.2f + 0.10f;
        }
        if (glow_col_r > 1) glow_col_r = 1;
        if (glow_col_g > 1) glow_col_g = 1;
        if (glow_col_b > 1) glow_col_b = 1;
        /* "danger" factor: ramps up as we move into the yellow/red half. */
        red_danger = 0.0f;
        if (progress > 0.5f) red_danger = (progress - 0.5f) * 2.0f;
        if (red_danger > 1.0f) red_danger = 1.0f;
    }

    /* ---- Outer beat-reactive glow halo (sits OUTSIDE the rim) ----
     * Pulses on every bass beat, colored to match the current timer color.
     * Red glows much brighter so it's obvious the round is nearly up. */
    if (beat_env > 0.01f) {
        float danger_boost = 1.0f + red_danger * 2.2f;
        float a_base = beat_env * 0.42f * danger_boost;
        /* Bright inner halo hugging the rim */
        vrt_push_arc(cx, cy, R * 0.96f, R * 1.04f, 0, 2.0f*(float)M_PI,
                     glow_col_r, glow_col_g, glow_col_b,
                     glow_col_r, glow_col_g, glow_col_b,
                     a_base * 0.85f, hi_segs, vp_w, vp_h);
        /* Mid halo */
        vrt_push_arc(cx, cy, R * 1.02f, R * 1.12f, 0, 2.0f*(float)M_PI,
                     glow_col_r, glow_col_g, glow_col_b,
                     glow_col_r*0.7f, glow_col_g*0.7f, glow_col_b*0.7f,
                     a_base * 0.5f, hi_segs, vp_w, vp_h);
        /* Soft outer bloom, wider on red for urgency */
        float outer_r = 1.18f + red_danger * 0.08f;
        vrt_push_arc(cx, cy, R * 1.08f, R * outer_r, 0, 2.0f*(float)M_PI,
                     glow_col_r*0.8f, glow_col_g*0.8f, glow_col_b*0.8f,
                     glow_col_r*0.2f, glow_col_g*0.2f, glow_col_b*0.2f,
                     a_base * 0.28f, hi_segs, vp_w, vp_h);
    }

    /* ---- Glossy acrylic torus background ---- */
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
    float rbg0 = t->bg[0], rbg1 = t->bg[1], rbg2 = t->bg[2];
    /* Layer 1: outermost rim — bright highlight sweeping top-left to dark bottom-right */
    vrt_push_arc(cx, cy, R * 0.90f, R * 0.96f, 0, 2.0f*(float)M_PI,
                 rbg0*1.6f+acr*0.3f+0.10f, rbg1*1.6f+acg*0.3f+0.10f, rbg2*1.6f+acb*0.3f+0.10f,
                 rbg0*0.4f, rbg1*0.4f, rbg2*0.4f,
                 0.8f, hi_segs, vp_w, vp_h);
    /* Layer 2: main ring body — accent-tinted like title bars */
    {
        float mr = acr * 0.20f + rbg0 * 0.55f;
        float mg = acg * 0.20f + rbg1 * 0.55f;
        float mb = acb * 0.20f + rbg2 * 0.55f;
        vrt_push_arc(cx, cy, R * 0.60f, R * 0.91f, 0, 2.0f*(float)M_PI,
                     mr, mg, mb, mr, mg, mb,
                     0.94f, hi_segs, vp_w, vp_h);
    }
    /* Layer 3: gloss highlight — top half of ring gets a white sheen */
    vrt_push_arc(cx, cy, R * 0.60f, R * 0.91f,
                 (float)M_PI * 0.15f, (float)M_PI * 0.85f,
                 1.0f, 1.0f, 1.0f,
                 1.0f, 1.0f, 1.0f,
                 0.07f, lo_segs, vp_w, vp_h);
    /* Layer 4: inner ring shadow (inset depth) */
    vrt_push_arc(cx, cy, R * 0.56f, R * 0.63f, 0, 2.0f*(float)M_PI,
                 rbg0*0.25f, rbg1*0.25f, rbg2*0.25f,
                 rbg0*1.0f+acr*0.15f+0.04f, rbg1*1.0f+acg*0.15f+0.04f, rbg2*1.0f+acb*0.15f+0.04f,
                 0.6f, hi_segs, vp_w, vp_h);
    /* Layer 5: accent edge line on outer rim */
    vrt_push_arc(cx, cy, R * 0.89f, R * 0.91f, 0, 2.0f*(float)M_PI,
                 acr, acg, acb, acr*0.5f, acg*0.5f, acb*0.5f,
                 0.45f, hi_segs, vp_w, vp_h);
    /* Center fill — dark glass */
    vrt_push_circle(cx, cy, R * 0.58f,
                    rbg0*0.30f, rbg1*0.30f, rbg2*0.30f,
                    0.94f, lo_segs, vp_w, vp_h);

    /* ---- Tick marks (thin lines) ---- */
    {
        int num_ticks = (int)(vrt_round_period / 1000.0 + 0.5);
        if (num_ticks < 3) num_ticks = 5;
        if (num_ticks > 12) num_ticks = 12;
        for (int i = 0; i < num_ticks; i++) {
            float theta = (float)M_PI / 2.0f - 2.0f * (float)M_PI * (float)i / (float)num_ticks;
            float tick_w = 0.015f;
            vrt_push_arc(cx, cy, R * 0.72f, R * 0.85f,
                         theta - tick_w, theta + tick_w,
                         t->dim[0]*0.6f, t->dim[1]*0.6f, t->dim[2]*0.6f,
                         t->dim[0]*0.6f, t->dim[1]*0.6f, t->dim[2]*0.6f,
                         0.4f, 2, vp_w, vp_h);
        }
    }

    /* ---- Color gradient: green → yellow → red (themed) ---- */
    float p = progress > 1.0f ? 1.0f : progress;
    float sr, sg, sb, er, eg, eb;
    if (p < 0.5f) {
        float tt = p * 2.0f;
        sr = t->accent[0]*0.2f + 0.1f;
        sg = t->accent[1]*0.2f + 0.7f;
        sb = t->accent[2]*0.2f + 0.15f;
        er = sr + 0.6f * tt;
        eg = sg;
        eb = sb - 0.05f * tt;
    } else {
        float tt = (p - 0.5f) * 2.0f;
        sr = t->accent[0]*0.2f + 0.7f;
        sg = t->accent[1]*0.2f + 0.7f;
        sb = t->accent[2]*0.2f + 0.1f;
        er = sr;
        eg = sg - 0.5f * tt;
        eb = sb;
    }

    /* ---- Main arc — neon tube style, sweeps from 12 o'clock CW ---- */
    if (progress > 0.01f && vrt_synced) {
        float arc_start = (float)M_PI / 2.0f;
        float sweep = progress > 1.0f ? 1.0f : progress;
        float arc_end = arc_start - 2.0f * (float)M_PI * sweep;
        int arc_segs = vrt_auto_segs(R * sweep, 12);

        /* Layer 1: Wide soft glow (bloom) */
        vrt_push_arc(cx, cy, R * 0.62f, R * 0.88f,
                     arc_end, arc_start,
                     er*0.4f, eg*0.4f, eb*0.4f, sr*0.4f, sg*0.4f, sb*0.4f,
                     0.05f, arc_segs, vp_w, vp_h);
        /* Layer 2: Main tube body */
        vrt_push_arc(cx, cy, R * 0.71f, R * 0.80f,
                     arc_end, arc_start,
                     er*0.7f, eg*0.7f, eb*0.7f, sr*0.7f, sg*0.7f, sb*0.7f,
                     0.9f, arc_segs, vp_w, vp_h);
        /* Layer 3: Hot center specular — bright white-ish core */
        {
            float wr = er*0.3f+0.7f, wg = eg*0.3f+0.7f, wb = eb*0.3f+0.7f;
            float ws = sr*0.3f+0.7f, wsg = sg*0.3f+0.7f, wsb = sb*0.3f+0.7f;
            if (wr > 1) wr = 1; if (wg > 1) wg = 1; if (wb > 1) wb = 1;
            if (ws > 1) ws = 1; if (wsg > 1) wsg = 1; if (wsb > 1) wsb = 1;
            vrt_push_arc(cx, cy, R * 0.735f, R * 0.775f,
                         arc_end, arc_start,
                         wr, wg, wb, ws, wsg, wsb,
                         0.55f, arc_segs, vp_w, vp_h);
        }
        /* Layer 4: Top-edge specular glint */
        vrt_push_arc(cx, cy, R * 0.785f, R * 0.80f,
                     arc_end, arc_start,
                     1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                     0.15f, arc_segs, vp_w, vp_h);

        /* ---- Leading edge comet — multi-layer soft glow ---- */
        float mid_r = R * 0.755f;
        float edge_x = cx + mid_r * vrt_cosf(arc_end);
        float edge_y = cy - mid_r * vrt_sinf(arc_end);
        int glow_segs = vrt_auto_segs(R * 0.12f, 16);
        /* Outer bloom */
        vrt_push_glow(edge_x, edge_y, R * 0.12f, 4,
                      er, eg, eb, 0.15f, glow_segs, vp_w, vp_h);
        /* Mid glow */
        vrt_push_glow(edge_x, edge_y, R * 0.06f, 3,
                      er*0.4f+0.6f, eg*0.4f+0.6f, eb*0.4f+0.6f, 0.4f,
                      glow_segs, vp_w, vp_h);
        /* Hot center */
        vrt_push_circle(edge_x, edge_y, R * 0.025f,
                        1.0f, 1.0f, 1.0f, 0.9f, glow_segs, vp_w, vp_h);

        /* Trailing comet tail — fading dots behind leading edge */
        {
            int trail_count = 8;
            float trail_arc = 0.08f; /* radians of trail behind edge */
            for (int ti = 1; ti <= trail_count; ti++) {
                float tt = (float)ti / (float)trail_count;
                float ta = arc_end + trail_arc * tt;
                if (ta > arc_start) break;
                float tx = cx + mid_r * vrt_cosf(ta);
                float ty = cy - mid_r * vrt_sinf(ta);
                float talpha = 0.3f * (1.0f - tt * tt);
                float trad = R * (0.03f - 0.02f * tt);
                if (trad < 1.0f) trad = 1.0f;
                vrt_push_circle(tx, ty, trad,
                                er, eg, eb, talpha, glow_segs / 2, vp_w, vp_h);
            }
        }

        /* Spawn particle at edge occasionally */
        if (((int)(fx_time * 60) % 4) == 0)
            vrt_spawn_particle(edge_x, edge_y, er, eg, eb);
    }

    /* ---- Overdue pulse ---- */
    if (progress > 1.0f) {
        float pulse = 0.15f + 0.1f * vrt_sinf((float)now / 150.0f);
        vrt_push_arc(cx, cy, R * 0.68f, R * 0.82f, 0, 2.0f*(float)M_PI,
                     1.0f, 0.2f, 0.1f, 1.0f, 0.2f, 0.1f,
                     pulse, hi_segs, vp_w, vp_h);
    }

    /* ---- Flash on round tick ---- */
    if (vrt_flash_tick > 0) {
        float a = (float)vrt_flash_tick / 15.0f;
        vrt_push_circle(cx, cy, R * (0.80f + a * 0.1f),
                        1.0f, 1.0f, 0.85f, a * 0.25f, lo_segs, vp_w, vp_h);
        vrt_flash_tick--;
    }

    /* ---- Particles ---- */
    for (int i = 0; i < VRT_PARTICLE_MAX; i++) {
        if (!vrt_particles[i].alive) continue;
        vrt_particles[i].x += vrt_particles[i].vx;
        vrt_particles[i].y += vrt_particles[i].vy;
        vrt_particles[i].alpha -= 0.018f;
        if (vrt_particles[i].alpha <= 0.0f) { vrt_particles[i].alive = 0; continue; }
        float ps = R * 0.025f;
        vrt_push_circle(vrt_particles[i].x, vrt_particles[i].y, ps,
                        vrt_particles[i].r, vrt_particles[i].g, vrt_particles[i].b,
                        vrt_particles[i].alpha, 8, vp_w, vp_h);
    }

    /* ---- Double-helix orbital particles ---- */
    if (vrt_orbits_visible) {
        float orbit_life = 5.0f;
        int orb_segs = vrt_auto_segs(R * 0.02f, 8);
        for (int i = 0; i < VRT_ORBIT_MAX; i++) {
            if (!vrt_orbits[i].alive) continue;
            float age = fx_time - vrt_orbits[i].birth_time;
            if (age > orbit_life) { vrt_orbits[i].alive = 0; continue; }

            /* Alpha envelope: fade in, hold, fade out */
            float alpha;
            if (age < 0.3f) {
                alpha = age / 0.3f * 0.8f;
            } else if (age < 3.5f) {
                alpha = 0.8f;
            } else {
                float fade_t = (age - 3.5f) / 1.5f;
                alpha = 0.8f * (1.0f - fade_t * fade_t);
                if (alpha < 0.01f) { vrt_orbits[i].alive = 0; continue; }
            }

            /* Double-helix: particles orbit around the center in a helix.
             * The "main angle" sweeps around, while a "helix angle" oscillates
             * radially in and out, offset by PI between the two strands. */
            float spd = vrt_orbits[i].speed;
            float t = age * 2.2f * spd + vrt_orbits[i].phase;
            float strand_offset = vrt_orbits[i].helix_id * 3.14159f;

            /* Main orbit angle — all particles sweep around center */
            float main_angle = t * 0.9f;

            /* Derive this dot's slot index (0..11) from its phase for maneuver
             * targeting. Phase was set to si*(2PI/6), so si = round(phase/(PI/3))
             * within a strand; combine with strand to get 0..11. */
            int slot_si = (int)(vrt_orbits[i].phase / ((float)M_PI / 3.0f) + 0.5f) & 7;
            int slot = slot_si + (int)(vrt_orbits[i].helix_id + 0.5f) * 6;

            /* Beat-reactive jitter applied to the main angle (every beat) */
            float jit_dth = 0.0f, jit_dr = 0.0f;
            if (beat_env > 0.01f) {
                float jh1 = vrt_sinf(vrt_orbits[i].phase * 13.7f + strand_offset);
                float jh2 = vrt_sinf(vrt_orbits[i].phase * 7.31f + strand_offset * 2.13f);
                jit_dth = jh1 * beat_env * 0.08f;
                jit_dr  = jh2 * beat_env * 0.04f;
            }

            /* Maneuver offsets (active every 4 beats for ~0.85s) */
            float man_dth = 0.0f, man_dr = 0.0f, man_glow = 0.0f;
            vrt_maneuver_offset(vrt_man_current, man_t01, slot, 12,
                                &man_dth, &man_dr, &man_glow);

            main_angle += jit_dth + man_dth;

            /* Helix modulation — radial breathing creates the double-helix weave */
            float helix_freq = 3.0f; /* how many times the helix wraps per orbit */
            float helix_amp = 0.12f; /* how far in/out the helix breathes */
            float helix_phase = helix_freq * main_angle + strand_offset;
            float or_frac = vrt_orbits[i].orbit_r + vrt_sinf(helix_phase) * helix_amp
                          + jit_dr + man_dr;
            if (or_frac < 0.02f) or_frac = 0.02f;

            /* Slight vertical wobble for depth illusion (mapped to Y offset) */
            float z_wobble = vrt_cosf(helix_phase) * R * 0.03f;

            float orb_r = R * or_frac;
            float ox, oy;
            if (age < 0.3f) {
                /* Burst from center */
                float burst_t = age / 0.3f;
                float bt2 = burst_t * burst_t;
                float fx2 = cx + orb_r * vrt_cosf(main_angle);
                float fy = cy - orb_r * vrt_sinf(main_angle) + z_wobble;
                ox = cx + (fx2 - cx) * bt2;
                oy = cy + (fy - cy) * bt2;
            } else {
                ox = cx + orb_r * vrt_cosf(main_angle);
                oy = cy - orb_r * vrt_sinf(main_angle) + z_wobble;
            }

            /* Size pulses slightly with helix phase for 3D feel, plus a smooth
             * beat-driven glow pulse and any maneuver glow boost. */
            float ps_base = R * 0.018f;
            float beat_pulse_sz = 1.0f + beat_env * 0.45f + man_glow * 0.6f;
            float ps = ps_base * (1.0f + vrt_cosf(helix_phase) * 0.25f) * beat_pulse_sz;

            /* Fading trail: draw 5 ghost particles at past positions */
            #define VRT_TRAIL_LEN 5
            float trail_dt = 0.06f; /* time step between trail dots */
            for (int tr = VRT_TRAIL_LEN; tr >= 1; tr--) {
                float t_past = t - (float)tr * trail_dt * spd * 2.2f;
                float ma_past = t_past * 0.9f;
                float hp_past = helix_freq * ma_past + strand_offset;
                float orf_past = vrt_orbits[i].orbit_r + vrt_sinf(hp_past) * helix_amp;
                float zw_past = vrt_cosf(hp_past) * R * 0.03f;
                float tr_r = R * orf_past;
                float tr_x = cx + tr_r * vrt_cosf(ma_past);
                float tr_y = cy - tr_r * vrt_sinf(ma_past) + zw_past;
                float tr_frac = 1.0f - (float)tr / (float)(VRT_TRAIL_LEN + 1);
                float tr_a = alpha * tr_frac * 0.35f;
                float tr_ps = ps * (0.4f + tr_frac * 0.4f);
                vrt_push_circle(tr_x, tr_y, tr_ps,
                                vrt_orbits[i].r, vrt_orbits[i].g, vrt_orbits[i].b,
                                tr_a, orb_segs, vp_w, vp_h);
            }
            #undef VRT_TRAIL_LEN

            /* Outer glow — alpha swells on every bass beat */
            float glow_a = alpha * (0.12f + beat_env * 0.35f + man_glow * 0.25f);
            vrt_push_circle(ox, oy, ps * 3.0f,
                            vrt_orbits[i].r, vrt_orbits[i].g, vrt_orbits[i].b,
                            glow_a, orb_segs, vp_w, vp_h);
            /* Core — brighter when "in front" (positive helix phase) */
            float front = (vrt_cosf(helix_phase) * 0.5f + 0.5f);
            float core_a = alpha * (0.6f + front * 0.4f);
            vrt_push_circle(ox, oy, ps,
                            vrt_orbits[i].r * 0.4f + 0.6f,
                            vrt_orbits[i].g * 0.4f + 0.6f,
                            vrt_orbits[i].b * 0.4f + 0.6f,
                            core_a, orb_segs, vp_w, vp_h);
        }
    }

    /* ---- Glossy specular highlight (upper crescent) ---- */
    vrt_push_arc(cx, cy - R * 0.05f, R * 0.70f, R * 0.88f,
                 (float)M_PI * 0.15f, (float)M_PI * 0.85f,
                 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                 0.04f, lo_segs, vp_w, vp_h);

    /* ---- Center text: matches msimg32 real round timer exactly ---- */
    {
        int cw = (int)(sz / 12.0f); if (cw < 6) cw = 6;
        int ch = cw * 2;
        int scw = cw * 2 / 3, sch = ch * 2 / 3;

        if (vrt_ms_state == 1) {
            /* Manual sync in progress — "TIMING" with blinking dots */
            int tw = 6 * scw;
            push_text((int)(cx - tw/2), (int)(cy - sch/2),
                      "TIMING", 1.0f, 0.8f, 0.2f, vp_w, vp_h, scw, sch);
        } else if (vrt_synced) {
            /* Synced — big "ROUND" in center with flash on tick */
            double tick_age = (vrt_last_tick_ts > 0) ? (now_d - vrt_last_tick_ts) / 1000.0 : 99.0;
            float flash = 1.0f - (float)(tick_age / 1.5);
            if (flash < 0) flash = 0;
            float cr = t->text[0] * (0.4f + 0.6f * flash);
            float cg = t->text[1] * (0.4f + 0.6f * flash);
            float cb = t->text[2] * (0.4f + 0.6f * flash);
            int tw = 5 * cw;
            /* Shadow */
            push_text((int)(cx - tw/2 + 1), (int)(cy - ch/2 + 2),
                      "ROUND", 0, 0, 0, vp_w, vp_h, cw, ch);
            /* Text */
            push_text((int)(cx - tw/2), (int)(cy - ch/2),
                      "ROUND", cr, cg, cb, vp_w, vp_h, cw, ch);

            /* Elapsed readout below: "3.2s / 5.0s" */
            if (vrt_last_tick_ts > 0.0) {
                double elapsed = now_d - vrt_last_tick_ts;
                double in_cycle = elapsed / 1000.0;
                double period_s = vrt_round_period / 1000.0;
                if (in_cycle > period_s)
                    in_cycle = in_cycle - ((int)(in_cycle / period_s)) * period_s;
                int e10 = (int)(in_cycle * 10.0);
                if (e10 > 50) e10 = 50;
                /* Build "X.Xs / 5.0s" */
                char ebuf[16];
                int ei = 0;
                ebuf[ei++] = '0' + (e10 / 10);
                ebuf[ei++] = '.';
                ebuf[ei++] = '0' + (e10 % 10);
                ebuf[ei++] = 's';
                ebuf[ei++] = ' ';
                ebuf[ei++] = '/';
                ebuf[ei++] = ' ';
                ebuf[ei++] = '5';
                ebuf[ei++] = '.';
                ebuf[ei++] = '0';
                ebuf[ei++] = 's';
                ebuf[ei] = '\0';
                int etw = ei * (scw * 3 / 4);
                push_text((int)(cx - etw/2), (int)(cy + ch/2 + 4),
                          ebuf, t->dim[0], t->dim[1], t->dim[2],
                          vp_w, vp_h, scw * 3 / 4, sch * 3 / 4);
            }
        } else {
            /* Not synced — "WAITING" big + "FOR SYNC" small below */
            int tw = 7 * cw;
            push_text((int)(cx - tw/2), (int)(cy - ch/2 - sch/2),
                      "WAITING", t->accent[0], t->accent[1], t->accent[2],
                      vp_w, vp_h, cw, ch);
            int stw = 8 * scw;
            push_text((int)(cx - stw/2), (int)(cy + ch/2 - sch/2),
                      "FOR SYNC", t->dim[0], t->dim[1], t->dim[2],
                      vp_w, vp_h, scw, sch);
        }

        /* Delta text below circle */
        if (vrt_delta_text[0] && vrt_delta_fade > 0.01) {
            float dr = (vrt_delta_text[0] == '+') ? 1.0f : 0.3f;
            float dg = (vrt_delta_text[0] == '+') ? 0.5f : 1.0f;
            float db = 0.2f;
            int dlen = 0; while (vrt_delta_text[dlen]) dlen++;
            int dtw = dlen * scw;
            push_text((int)(cx - dtw/2), (int)(cy + R * 0.55f),
                      vrt_delta_text, dr, dg, db, vp_w, vp_h, scw, sch);
            vrt_delta_fade -= 0.006;
        }
    }

    /* ---- Context menu ---- */
    if (vrt_ctx_open) {
        int mx = vrt_ctx_x, my = vrt_ctx_y;
        int mw = VRT_CTX_W;
        int s_sep = (int)(8 * ui_scale);
        int mh = VRT_CTX_PAD * 2;
        for (int i = 0; i < VRT_CTX_COUNT; i++)
            mh += (i == VRT_CTX_SEP1 || i == VRT_CTX_SEP2 || i == VRT_CTX_SEP3) ? s_sep : VRT_CTX_ITEM_H;

        /* Panel bg */
        push_solid(mx, my, mx + mw, my + mh,
                   t->bg[0] * 0.9f, t->bg[1] * 0.9f, t->bg[2] * 0.9f, 0.95f,
                   vp_w, vp_h);
        /* Border */
        push_solid(mx, my, mx + mw, my + 1, t->dim[0] * 0.4f, t->dim[1] * 0.4f, t->dim[2] * 0.4f, 0.6f, vp_w, vp_h);
        push_solid(mx, my + mh - 1, mx + mw, my + mh, t->dim[0] * 0.4f, t->dim[1] * 0.4f, t->dim[2] * 0.4f, 0.6f, vp_w, vp_h);
        push_solid(mx, my, mx + 1, my + mh, t->dim[0] * 0.4f, t->dim[1] * 0.4f, t->dim[2] * 0.4f, 0.6f, vp_w, vp_h);
        push_solid(mx + mw - 1, my, mx + mw, my + mh, t->dim[0] * 0.4f, t->dim[1] * 0.4f, t->dim[2] * 0.4f, 0.6f, vp_w, vp_h);

        const char *labels[VRT_CTX_COUNT] = {
            "Set Timing Spell...",
            vrt_ms_state == 1 ? "Stop Timing" : "Cast to Time",
            "", /* sep */
            "Small",
            "Normal",
            "Large",
            "X-Large",
            "XX-Large",
            "", /* sep */
            vrt_orbits_visible ? "Hide Orbits" : "Show Orbits",
            "", /* sep */
            "Reset Timer",
            "Hide Round Timer"
        };

        int iy = my + VRT_CTX_PAD;
        int item_cw = (int)(7 * ui_scale), item_ch = (int)(14 * ui_scale);
        int s4 = (int)(4*ui_scale), s14 = (int)(14*ui_scale), s8 = (int)(8*ui_scale);
        for (int i = 0; i < VRT_CTX_COUNT; i++) {
            if (i == VRT_CTX_SEP1 || i == VRT_CTX_SEP2 || i == VRT_CTX_SEP3) {
                push_solid(mx + s4, iy + 3, mx + mw - s4, iy + 4,
                           t->dim[0] * 0.3f, t->dim[1] * 0.3f, t->dim[2] * 0.3f,
                           0.4f, vp_w, vp_h);
                iy += s8;
                continue;
            }
            /* Hover */
            if (i == vrt_ctx_hover) {
                push_solid(mx + 1, iy, mx + mw - 1, iy + VRT_CTX_ITEM_H,
                           t->accent[0], t->accent[1], t->accent[2],
                           0.15f, vp_w, vp_h);
            }
            /* Check marks for current size */
            int checked = 0;
            if (i == VRT_CTX_SMALL  && vrt_size <= 100) checked = 1;
            if (i == VRT_CTX_NORMAL && vrt_size > 100 && vrt_size <= 200) checked = 1;
            if (i == VRT_CTX_LARGE  && vrt_size > 200 && vrt_size <= 350) checked = 1;
            if (i == VRT_CTX_XLARGE && vrt_size > 350 && vrt_size <= 550) checked = 1;
            if (i == VRT_CTX_XXL    && vrt_size > 550) checked = 1;
            if (i == VRT_CTX_ORBITS && vrt_orbits_visible) checked = 1;

            if (checked) {
                push_text(mx + s4, iy + (VRT_CTX_ITEM_H - item_ch) / 2,
                          "\x10", t->accent[0], t->accent[1], t->accent[2],
                          vp_w, vp_h, item_cw, item_ch);
            }
            /* Grayed out "Cast to Time" if no spell set */
            float lr = t->text[0], lg = t->text[1], lb = t->text[2];
            if (i == VRT_CTX_CAST && !vrt_sync_spell[0]) {
                lr = t->dim[0] * 0.5f; lg = t->dim[1] * 0.5f; lb = t->dim[2] * 0.5f;
            }
            push_text(mx + s14, iy + (VRT_CTX_ITEM_H - item_ch) / 2,
                      labels[i], lr, lg, lb,
                      vp_w, vp_h, item_cw, item_ch);
            iy += VRT_CTX_ITEM_H;
        }
    }

    /* ---- Spell input dialog ---- */
    if (vrt_spell_input) {
        int dw = 240, dh = 80;
        int dx = (int)(cx - dw / 2);
        int dy = (int)(cy + R + 10);
        /* BG */
        push_solid(dx, dy, dx + dw, dy + dh,
                   t->bg[0] * 0.8f + 0.05f, t->bg[1] * 0.8f + 0.05f, t->bg[2] * 0.8f + 0.05f,
                   0.95f, vp_w, vp_h);
        /* Border */
        push_solid(dx, dy, dx + dw, dy + 1, t->accent[0] * 0.6f, t->accent[1] * 0.6f, t->accent[2] * 0.6f, 0.7f, vp_w, vp_h);
        push_solid(dx, dy + dh - 1, dx + dw, dy + dh, t->accent[0] * 0.6f, t->accent[1] * 0.6f, t->accent[2] * 0.6f, 0.7f, vp_w, vp_h);
        push_solid(dx, dy, dx + 1, dy + dh, t->accent[0] * 0.6f, t->accent[1] * 0.6f, t->accent[2] * 0.6f, 0.7f, vp_w, vp_h);
        push_solid(dx + dw - 1, dy, dx + dw, dy + dh, t->accent[0] * 0.6f, t->accent[1] * 0.6f, t->accent[2] * 0.6f, 0.7f, vp_w, vp_h);
        /* Label */
        push_text(dx + 8, dy + 6, "Spell command:", t->dim[0], t->dim[1], t->dim[2],
                  vp_w, vp_h, 7, 14);
        /* Input field bg */
        push_solid(dx + 8, dy + 24, dx + dw - 8, dy + 44,
                   t->bg[0] * 0.3f, t->bg[1] * 0.3f, t->bg[2] * 0.3f, 0.9f,
                   vp_w, vp_h);
        /* Input text */
        push_text(dx + 12, dy + 26, vrt_spell_buf, t->text[0], t->text[1], t->text[2],
                  vp_w, vp_h, 7, 14);
        /* Cursor */
        if ((frame_count / 30) % 2 == 0) {
            int cux = dx + 12 + vrt_spell_cursor * 7;
            push_solid(cux, dy + 26, cux + 7, dy + 40, t->accent[0], t->accent[1], t->accent[2],
                       0.5f, vp_w, vp_h);
        }
        /* Buttons */
        push_solid(dx + 8, dy + 50, dx + 60, dy + 70,
                   t->accent[0] * 0.4f, t->accent[1] * 0.4f, t->accent[2] * 0.4f, 0.7f,
                   vp_w, vp_h);
        push_text(dx + 20, dy + 52, "OK", t->text[0], t->text[1], t->text[2],
                  vp_w, vp_h, 7, 14);
        push_solid(dx + 70, dy + 50, dx + 140, dy + 70,
                   t->bg[0] * 0.5f + 0.1f, t->bg[1] * 0.5f + 0.1f, t->bg[2] * 0.5f + 0.1f, 0.7f,
                   vp_w, vp_h);
        push_text(dx + 80, dy + 52, "Cancel", t->text[0], t->text[1], t->text[2],
                  vp_w, vp_h, 7, 14);
        push_solid(dx + 150, dy + 50, dx + 220, dy + 70,
                   t->bg[0] * 0.5f + 0.1f, t->bg[1] * 0.5f + 0.1f, t->bg[2] * 0.5f + 0.1f, 0.7f,
                   vp_w, vp_h);
        push_text(dx + 162, dy + 52, "Clear", t->text[0], t->text[1], t->text[2],
                  vp_w, vp_h, 7, 14);
    }
}

/* ---- Round timer: on_round callback (mirrors gl_round_timer.c exactly) ---- */
static void vrt_on_round(int round_num)
{
    double now = (double)GetTickCount64();
    double prev = vrt_last_tick_ts;
    vrt_round_num = round_num;
    vrt_flash_tick = 15;

    /* Spawn orbital particles on round tick */
    if (vrt_orbits_visible && round_num != vrt_last_spawn_round) {
        /* Get current arc colors for orbit tinting */
        const ui_theme_t *ot = &ui_themes[current_theme];
        float ocr = ot->accent[0]*0.2f + 0.4f;
        float ocg = ot->accent[1]*0.2f + 0.5f;
        float ocb = ot->accent[2]*0.2f + 0.3f;
        vrt_spawn_orbits(ocr, ocg, ocb);
        vrt_last_spawn_round = round_num;
    }

    /* Delta calculation */
    if (prev > 0.0 && vrt_synced && vrt_round_period > 0.0) {
        double elapsed = now - prev;
        double cycles = (int)(elapsed / vrt_round_period + 0.5);
        if (cycles < 1) cycles = 1;
        double predicted = prev + cycles * vrt_round_period;
        int delta = (int)(now - predicted);
        /* sprintf without libc: manual format */
        int di = 0;
        if (delta >= 0) vrt_delta_text[di++] = '+';
        else { vrt_delta_text[di++] = '-'; delta = -delta; }
        if (delta >= 1000) { vrt_delta_text[di++] = '0' + (delta / 1000) % 10; }
        if (delta >= 100)  { vrt_delta_text[di++] = '0' + (delta / 100) % 10; }
        if (delta >= 10)   { vrt_delta_text[di++] = '0' + (delta / 10) % 10; }
        vrt_delta_text[di++] = '0' + delta % 10;
        vrt_delta_text[di++] = 'm'; vrt_delta_text[di++] = 's';
        vrt_delta_text[di] = '\0';
        vrt_delta_fade = 1.0;
    }

    /* Fixed 5s period — no auto-calibration (lag spikes would corrupt it) */

    vrt_last_tick_ts = now;
    vrt_synced = 1;

    /* Auto-show on first round */
    if (!vrt_visible) { vrt_visible = 1; }

    /* Flush player stats round immediately on round boundary
     * and print round totals to console */
    pst_on_round_tick(round_num);
}

/* ---- Manual sync: on_line callback ---- */
static void vrt_sync_on_line(const char *line)
{
    if (vrt_ms_state != 1) return;

    if (strstr(line, "You have already cast")) {
        vrt_ms_already_count++;
        return;
    }

    if (vrt_ms_already_count >= 2) {
        int boundary = 0;
        if (strncmp(line, "You cast ", 9) == 0) boundary = 1;
        else if (strncmp(line, "You sing ", 9) == 0) boundary = 1;
        else if (strncmp(line, "You invoke ", 11) == 0) boundary = 1;
        else if (strncmp(line, "You attempt to cast", 19) == 0) boundary = 1;
        else if (strncmp(line, "You attempt to sing", 19) == 0) boundary = 1;
        else if (strncmp(line, "You attempt to invoke", 21) == 0) boundary = 1;

        if (boundary) {
            vrt_ms_rounds_detected++;
            vrt_round_num++;
            vrt_on_round(vrt_round_num);
            vrt_ms_already_count = 0;
        }
    }

    if (line[0] && !strstr(line, "You have already cast")) {
        if (vrt_ms_already_count > 0 && vrt_ms_already_count < 2)
            vrt_ms_already_count = 0;
    }
}

/* Combined on_line handler for all features */
static void vkt_on_line(const char *line)
{
    vrt_sync_on_line(line);
    convo_parse_line(line);
}

/* Manual sync spam thread */
static DWORD WINAPI vrt_ms_spam_thread(LPVOID param)
{
    (void)param;
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) { vrt_ms_state = 0; return 0; }
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;

    DWORD start = GetTickCount();
    while (vrt_ms_state == 1) {
        if (GetTickCount() - start > 10000) {
            vrt_ms_state = 0;
            break;
        }
        for (int i = 0; vrt_sync_spell[i]; i++)
            PostMessageA(target, WM_CHAR, (WPARAM)(unsigned char)vrt_sync_spell[i], 0);
        PostMessageA(target, WM_CHAR, (WPARAM)'\r', 0);
        Sleep(150);
    }
    return 0;
}

static void vrt_perform_manual_sync(void)
{
    if (!vrt_sync_spell[0]) {
        /* Show spell input dialog */
        vrt_spell_input = 1;
        vrt_spell_len = 0;
        vrt_spell_buf[0] = '\0';
        vrt_spell_cursor = 0;
        /* Copy current spell if exists */
        for (int i = 0; vrt_sync_spell[i] && i < 21; i++) {
            vrt_spell_buf[i] = vrt_sync_spell[i];
            vrt_spell_len = i + 1;
        }
        vrt_spell_buf[vrt_spell_len] = '\0';
        vrt_spell_cursor = vrt_spell_len;
        return;
    }
    if (vrt_ms_state == 1) {
        vrt_ms_state = 0;
        return;
    }

    vrt_synced = 0;
    vrt_last_tick_ts = 0.0;
    vrt_interval_count = 0;
    vrt_round_period = 5000.0;
    vrt_delta_text[0] = '\0';
    vrt_delta_fade = 0.0;

    vrt_ms_state = 1;
    vrt_ms_already_count = 0;
    vrt_ms_rounds_detected = 0;
    CreateThread(NULL, 0, vrt_ms_spam_thread, NULL, 0, NULL);
}

/* ---- Round timer hit testing ---- */
static float vrt_scaled_size(void) {
    return vrt_size;
}

static int vrt_hit_circle(int mx, int my)
{
    if (!vrt_visible) return 0;
    float ssz = vrt_scaled_size();
    float cx = vrt_x + ssz / 2.0f;
    float cy = vrt_y + ssz / 2.0f;
    float dx = (float)mx - cx;
    float dy = (float)my - cy;
    float r = ssz / 2.0f;
    return (dx * dx + dy * dy) <= (r * r);
}

/* Returns context menu item at mouse position, or -1 */
static int vrt_ctx_hit(int mx, int my)
{
    if (!vrt_ctx_open) return -1;
    if (mx < vrt_ctx_x || mx > vrt_ctx_x + VRT_CTX_W) return -1;
    int iy = vrt_ctx_y + VRT_CTX_PAD;
    for (int i = 0; i < VRT_CTX_COUNT; i++) {
        int ih = (i == VRT_CTX_SEP1 || i == VRT_CTX_SEP2 || i == VRT_CTX_SEP3) ? (int)(8*ui_scale) : VRT_CTX_ITEM_H;
        if (my >= iy && my < iy + ih) {
            if (i == VRT_CTX_SEP1 || i == VRT_CTX_SEP2 || i == VRT_CTX_SEP3) return -1;
            return i;
        }
        iy += ih;
    }
    return -1;
}

/* ---- Paths & Loops window drawing ---- */
#define PL_TITLEBAR_BASE  24
#define PL_TAB_H_BASE     28
#define PL_TOOLBAR_H_BASE 34
#define PL_ITEM_H_BASE    20
#define PL_PAD_BASE        6
#define PL_CW_BASE         8
#define PL_CH_BASE        15
#define PL_GO_H_BASE      28  /* GO button height at bottom */
static int PL_TITLEBAR, PL_TAB_H, PL_TOOLBAR_H, PL_ITEM_H, PL_PAD, PL_GO_H;

static int pl_scaled = 0;
static void pl_draw(int vp_w, int vp_h)
{
    if (!pl_wnd_open) return;
    if (!pl_scaled) { pl_w = 520*ui_scale; pl_h = 480*ui_scale; pl_x = 100*ui_scale; pl_y = 50*ui_scale; pl_scaled = 1; }
    PL_TITLEBAR = (int)(PL_TITLEBAR_BASE*ui_scale); PL_TAB_H = (int)(PL_TAB_H_BASE*ui_scale);
    PL_TOOLBAR_H = (int)(PL_TOOLBAR_H_BASE*ui_scale); PL_ITEM_H = (int)(PL_ITEM_H_BASE*ui_scale);
    PL_PAD = (int)(PL_PAD_BASE*ui_scale); PL_GO_H = (int)(PL_GO_H_BASE*ui_scale);
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(PL_CW_BASE*ui_scale), ch = (int)(PL_CH_BASE*ui_scale);
    int x = (int)pl_x, y = (int)pl_y, w = (int)pl_w, h = (int)pl_h;

    /* Select UI font draw functions when available, else fallback to terminal font */
    void (*psolid)(float,float,float,float,float,float,float,float,int,int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int,int,const char*,float,float,float,int,int,int,int) =
        ui_font_ready ? push_text_ui : push_text;

    /* Drop shadow */
    psolid(x + 4, y + 4, x + w + 4, y + h + 4, 0, 0, 0, 0.4f, vp_w, vp_h);
    /* Background */
    psolid(x, y, x + w, y + h,
           t->bg[0] + 0.04f, t->bg[1] + 0.04f, t->bg[2] + 0.04f, 0.96f, vp_w, vp_h);
    /* Border */
    psolid(x, y, x + w, y + 1, t->accent[0]*0.4f, t->accent[1]*0.4f, t->accent[2]*0.4f, 0.6f, vp_w, vp_h);
    psolid(x, y+h-1, x+w, y+h, t->accent[0]*0.4f, t->accent[1]*0.4f, t->accent[2]*0.4f, 0.6f, vp_w, vp_h);
    psolid(x, y, x+1, y+h, t->accent[0]*0.4f, t->accent[1]*0.4f, t->accent[2]*0.4f, 0.6f, vp_w, vp_h);
    psolid(x+w-1, y, x+w, y+h, t->accent[0]*0.4f, t->accent[1]*0.4f, t->accent[2]*0.4f, 0.6f, vp_w, vp_h);

    /* Titlebar */
    psolid(x, y, x + w, y + PL_TITLEBAR,
           t->bg[0] * 0.6f + 0.08f, t->bg[1] * 0.6f + 0.08f, t->bg[2] * 0.6f + 0.08f,
           0.95f, vp_w, vp_h);
    ptext(x + PL_PAD, y + (PL_TITLEBAR - ch) / 2,
          "Paths & Loops", t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    /* Close button */
    ptext(x + w - 16, y + (PL_TITLEBAR - ch) / 2,
          "X", t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);

    int ty = y + PL_TITLEBAR;

    /* Tab bar */
    int tab_w = w / 2;
    for (int i = 0; i < 2; i++) {
        int tx = x + i * tab_w;
        int active = (pl_tab == i);
        if (active) {
            psolid(tx, ty, tx + tab_w, ty + PL_TAB_H,
                   t->accent[0] * 0.15f, t->accent[1] * 0.15f, t->accent[2] * 0.15f,
                   0.5f, vp_w, vp_h);
        }
        psolid(tx, ty + PL_TAB_H - 2, tx + tab_w, ty + PL_TAB_H,
               active ? t->accent[0] : t->dim[0] * 0.3f,
               active ? t->accent[1] : t->dim[1] * 0.3f,
               active ? t->accent[2] : t->dim[2] * 0.3f,
               active ? 0.8f : 0.3f, vp_w, vp_h);
        const char *tab_label = (i == 0) ? "Goto" : "Loops";
        float tr = active ? t->accent[0] : t->dim[0];
        float tg = active ? t->accent[1] : t->dim[1];
        float tb = active ? t->accent[2] : t->dim[2];
        ptext(tx + (tab_w - (int)strlen(tab_label) * cw) / 2,
              ty + (PL_TAB_H - ch) / 2,
              tab_label, tr, tg, tb, vp_w, vp_h, cw, ch);
    }
    ty += PL_TAB_H;

    /* Toolbar: [Search...] [_] Hidden  [Walk|Run] */
    psolid(x, ty, x + w, ty + PL_TOOLBAR_H,
           t->bg[0] * 0.8f, t->bg[1] * 0.8f, t->bg[2] * 0.8f, 0.6f, vp_w, vp_h);
    /* Search box — prominent with border */
    int sx = x + PL_PAD, sy = ty + 5, sw = w / 2 - PL_PAD, sh = PL_TOOLBAR_H - 10;
    /* Border */
    psolid(sx - 1, sy - 1, sx + sw + 1, sy + sh + 1,
           t->accent[0] * 0.5f, t->accent[1] * 0.5f, t->accent[2] * 0.5f,
           pl_focused ? 0.8f : 0.4f, vp_w, vp_h);
    /* Background */
    psolid(sx, sy, sx + sw, sy + sh,
           t->bg[0] * 0.3f, t->bg[1] * 0.3f, t->bg[2] * 0.3f, 0.95f, vp_w, vp_h);
    /* Magnifying glass icon + text */
    ptext(sx + 4, sy + (sh - ch) / 2,
          "\x0F", t->dim[0] * 0.7f, t->dim[1] * 0.7f, t->dim[2] * 0.7f,
          vp_w, vp_h, cw, ch);
    if (pl_search[0]) {
        ptext(sx + 4 + cw + 3, sy + (sh - ch) / 2,
              pl_search, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    } else {
        ptext(sx + 4 + cw + 3, sy + (sh - ch) / 2,
              "Search...", t->dim[0] * 0.5f, t->dim[1] * 0.5f, t->dim[2] * 0.5f,
              vp_w, vp_h, cw, ch);
    }

    /* Hidden checkbox (goto tab only) */
    if (pl_tab == 0) {
        int hx = x + w / 2 + PL_PAD;
        int hy = ty + (PL_TOOLBAR_H - ch) / 2;
        ptext(hx, hy, pl_show_hidden ? "\xFE" : "\x04",
              t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
        ptext(hx + cw + 2, hy, "Hidden",
              t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
    }

    /* Walk/Run toggle */
    {
        int rx = x + w - 70;
        int ry = ty + (PL_TOOLBAR_H - ch) / 2;
        if (!pl_run_mode) {
            ptext(rx, ry, "Walk", t->accent[0], t->accent[1], t->accent[2],
                  vp_w, vp_h, cw, ch);
        } else {
            ptext(rx, ry, "Run", 1.0f, 0.6f, 0.2f, vp_w, vp_h, cw, ch);
        }
    }
    ty += PL_TOOLBAR_H;

    /* Bottom area: GO button + footer */
    int bottom_h = PL_GO_H + ch + 8;  /* GO button + footer text */

    /* Content area — tree view */
    int content_bottom = y + h - bottom_h;
    int content_h = content_bottom - ty;
    psolid(x + 1, ty, x + w - 1, content_bottom,
           t->bg[0], t->bg[1], t->bg[2], 0.9f, vp_w, vp_h);

    int iy = ty + 2 - pl_scroll * PL_ITEM_H;
    int visible_items = content_h / PL_ITEM_H;
    int item_idx = 0;
    (void)visible_items;

    if (pl_tab == 0) {
        /* Goto tab — rooms by category */
        for (int c = 0; c < pl_room_cat_count && iy < content_bottom; c++) {
            pl_cat_t *cat = &pl_room_cats[c];

            /* Count visible rooms in this category */
            int vis_count = 0;
            for (int r = 0; r < cat->count; r++) {
                int ri = cat->first_idx + r;
                if (ri >= pl_room_count) break;
                if (strcmp(pl_rooms[ri].category, cat->name) != 0) break;
                if (pl_rooms[ri].hidden && !pl_show_hidden) continue;
                if (pl_search[0] && !pl_stristr(pl_rooms[ri].name, pl_search) &&
                    !pl_stristr(pl_rooms[ri].code, pl_search) &&
                    !pl_stristr(pl_rooms[ri].category, pl_search)) continue;
                vis_count++;
            }
            if (vis_count == 0 && pl_search[0]) continue;

            /* Category header — bold, flush left */
            if (iy >= ty && iy < content_bottom) {
                char catbuf[80];
                wsprintfA(catbuf, "%s %s (%d)",
                          cat->expanded ? "\x1F" : "\x10",
                          cat->name, vis_count);
                int is_hover = (pl_hover_item == item_idx);
                if (is_hover) {
                    psolid(x + 2, iy, x + w - 2, iy + PL_ITEM_H,
                           t->accent[0], t->accent[1], t->accent[2],
                           0.1f, vp_w, vp_h);
                }
                /* Categories rendered brighter/bolder */
                ptext(x + PL_PAD, iy + (PL_ITEM_H - ch) / 2,
                      catbuf, t->text[0], t->text[1], t->text[2],
                      vp_w, vp_h, cw, ch);
            }
            iy += PL_ITEM_H;
            item_idx++;

            /* Room items (if expanded) — indented */
            if (cat->expanded) {
                for (int r = 0; r < cat->count; r++) {
                    int ri = cat->first_idx + r;
                    if (ri >= pl_room_count) break;
                    if (strcmp(pl_rooms[ri].category, cat->name) != 0) break;
                    if (pl_rooms[ri].hidden && !pl_show_hidden) continue;
                    if (pl_search[0] && !pl_stristr(pl_rooms[ri].name, pl_search) &&
                        !pl_stristr(pl_rooms[ri].code, pl_search)) continue;

                    if (iy >= ty && iy < content_bottom) {
                        int is_hover = (pl_hover_item == item_idx);
                        int is_selected = (pl_selected_item == item_idx);
                        if (is_selected) {
                            psolid(x + 2, iy, x + w - 2, iy + PL_ITEM_H,
                                   0.1f, 0.5f, 0.15f, 0.35f, vp_w, vp_h);
                        } else if (is_hover) {
                            psolid(x + 2, iy, x + w - 2, iy + PL_ITEM_H,
                                   t->accent[0], t->accent[1], t->accent[2],
                                   0.12f, vp_w, vp_h);
                        }
                        float cr = pl_rooms[ri].hidden ? t->dim[0] * 0.7f : t->text[0] * 0.85f;
                        float cg = pl_rooms[ri].hidden ? t->dim[1] * 0.7f : t->text[1] * 0.85f;
                        float cb = pl_rooms[ri].hidden ? t->dim[2] * 0.7f : t->text[2] * 0.85f;
                        if (is_selected) { cr = 0.4f; cg = 1.0f; cb = 0.5f; }
                        /* Indented from category */
                        ptext(x + PL_PAD + cw * 4, iy + (PL_ITEM_H - ch) / 2,
                              pl_rooms[ri].name, cr, cg, cb,
                              vp_w, vp_h, cw, ch);
                        /* Room code on right */
                        int codelen = (int)strlen(pl_rooms[ri].code);
                        ptext(x + w - PL_PAD - codelen * (cw - 1), iy + (PL_ITEM_H - ch) / 2,
                              pl_rooms[ri].code, t->dim[0] * 0.5f, t->dim[1] * 0.5f, t->dim[2] * 0.5f,
                              vp_w, vp_h, cw - 1, ch);
                    }
                    iy += PL_ITEM_H;
                    item_idx++;
                }
            }
        }
    } else {
        /* Loop tab — only loops (is_loop) by start category */
        for (int c = 0; c < pl_loop_cat_count && iy < content_bottom; c++) {
            pl_cat_t *cat = &pl_loop_cats[c];

            int vis_count = 0;
            for (int p = 0; p < cat->count; p++) {
                int pi = cat->first_idx + p;
                if (pi >= pl_path_count) break;
                if (!pl_paths[pi].is_loop) break;
                if (strcmp(pl_paths[pi].start_category, cat->name) != 0) break;
                if (pl_search[0] && !pl_stristr(pl_paths[pi].name, pl_search) &&
                    !pl_stristr(pl_paths[pi].file, pl_search) &&
                    !pl_stristr(pl_paths[pi].start_category, pl_search)) continue;
                vis_count++;
            }
            if (vis_count == 0 && pl_search[0]) continue;

            if (iy >= ty && iy < content_bottom) {
                char catbuf[80];
                wsprintfA(catbuf, "%s %s (%d)",
                          cat->expanded ? "\x1F" : "\x10",
                          cat->name, vis_count);
                int is_hover = (pl_hover_item == item_idx);
                if (is_hover) {
                    psolid(x + 2, iy, x + w - 2, iy + PL_ITEM_H,
                           t->accent[0], t->accent[1], t->accent[2],
                           0.1f, vp_w, vp_h);
                }
                ptext(x + PL_PAD, iy + (PL_ITEM_H - ch) / 2,
                      catbuf, t->text[0], t->text[1], t->text[2],
                      vp_w, vp_h, cw, ch);
            }
            iy += PL_ITEM_H;
            item_idx++;

            if (cat->expanded) {
                for (int p = 0; p < cat->count; p++) {
                    int pi = cat->first_idx + p;
                    if (pi >= pl_path_count) break;
                    if (!pl_paths[pi].is_loop) break;
                    if (strcmp(pl_paths[pi].start_category, cat->name) != 0) break;
                    if (pl_search[0] && !pl_stristr(pl_paths[pi].name, pl_search) &&
                        !pl_stristr(pl_paths[pi].file, pl_search)) continue;

                    if (iy >= ty && iy < content_bottom) {
                        int is_hover = (pl_hover_item == item_idx);
                        int is_selected = (pl_selected_item == item_idx);
                        if (is_selected) {
                            psolid(x + 2, iy, x + w - 2, iy + PL_ITEM_H,
                                   0.1f, 0.5f, 0.15f, 0.35f, vp_w, vp_h);
                        } else if (is_hover) {
                            psolid(x + 2, iy, x + w - 2, iy + PL_ITEM_H,
                                   t->accent[0], t->accent[1], t->accent[2],
                                   0.12f, vp_w, vp_h);
                        }
                        float ir = t->dim[0], ig = t->dim[1], ib = t->dim[2];
                        float nr = t->text[0] * 0.85f, ng = t->text[1] * 0.85f, nb = t->text[2] * 0.85f;
                        if (is_selected) { nr = 0.4f; ng = 1.0f; nb = 0.5f; }
                        const char *icon = pl_paths[pi].is_loop ? "\x0E" : "\x1A";
                        ptext(x + PL_PAD + cw * 3, iy + (PL_ITEM_H - ch) / 2,
                              icon, ir, ig, ib, vp_w, vp_h, cw, ch);
                        ptext(x + PL_PAD + cw * 5, iy + (PL_ITEM_H - ch) / 2,
                              pl_paths[pi].name, nr, ng, nb,
                              vp_w, vp_h, cw, ch);
                        /* Steps on right */
                        char sbuf[16];
                        wsprintfA(sbuf, "%d steps", pl_paths[pi].steps);
                        int slen = (int)strlen(sbuf);
                        ptext(x + w - PL_PAD - slen * (cw - 1), iy + (PL_ITEM_H - ch) / 2,
                              sbuf, t->dim[0] * 0.5f, t->dim[1] * 0.5f, t->dim[2] * 0.5f,
                              vp_w, vp_h, cw - 1, ch);
                    }
                    iy += PL_ITEM_H;
                    item_idx++;
                }
            }
        }
    }

    /* Track totals for scroll clamping */
    pl_total_items = item_idx;
    pl_visible_items = content_h / PL_ITEM_H;
    if (pl_total_items > pl_visible_items) {
        int max_scroll = pl_total_items - pl_visible_items;
        if (pl_scroll > max_scroll) pl_scroll = max_scroll;
    } else {
        pl_scroll = 0;
    }

    /* GO button */
    {
        int go_y = content_bottom + 4;
        int go_w = 80, go_h = PL_GO_H - 8;
        int go_x = x + (w - go_w) / 2;
        int has_sel = (pl_selected_item >= 0);
        float br = has_sel ? 0.15f : 0.08f;
        float bg = has_sel ? 0.55f : 0.15f;
        float bb = has_sel ? 0.2f : 0.08f;
        float ba = has_sel ? 0.9f : 0.4f;
        psolid(go_x, go_y, go_x + go_w, go_y + go_h, br, bg, bb, ba, vp_w, vp_h);
        /* Button border */
        psolid(go_x, go_y, go_x + go_w, go_y + 1, br*1.5f, bg*1.5f, bb*1.5f, ba, vp_w, vp_h);
        psolid(go_x, go_y+go_h-1, go_x+go_w, go_y+go_h, br*0.5f, bg*0.5f, bb*0.5f, ba, vp_w, vp_h);
        float gr = has_sel ? 1.0f : 0.5f;
        float gg = has_sel ? 1.0f : 0.5f;
        float gb = has_sel ? 1.0f : 0.5f;
        ptext(go_x + (go_w - 2 * cw) / 2, go_y + (go_h - ch) / 2,
              "GO", gr, gg, gb, vp_w, vp_h, cw, ch);
    }

    /* Footer: item count */
    {
        char fbuf[64];
        if (pl_tab == 0)
            wsprintfA(fbuf, "%d rooms in %d areas", pl_room_count, pl_room_cat_count);
        else
            wsprintfA(fbuf, "%d paths/loops in %d areas", pl_path_count, pl_loop_cat_count);
        ptext(x + PL_PAD, y + h - ch - 4,
              fbuf, t->dim[0] * 0.5f, t->dim[1] * 0.5f, t->dim[2] * 0.5f,
              vp_w, vp_h, cw - 1, ch - 2);
    }
}

/* Paths & Loops window hit test — returns 1 if mouse is in window */
static int pl_hit(int mx, int my)
{
    if (!pl_wnd_open) return 0;
    return (mx >= (int)pl_x && mx < (int)(pl_x + pl_w) &&
            my >= (int)pl_y && my < (int)(pl_y + pl_h));
}

/* Map mouse Y to item index in content area */
static int pl_item_at_y(int my)
{
    int ty = (int)pl_y + PL_TITLEBAR + PL_TAB_H + PL_TOOLBAR_H + 2;
    if (my < ty) return -1;
    return (my - ty) / PL_ITEM_H + pl_scroll;
}

/* Execute the goto/loop for the given item_idx. Returns 1 if executed, 0 if category. */
static int pl_exec_item(int item_idx)
{
    int idx = 0;
    if (pl_tab == 0) {
        for (int c = 0; c < pl_room_cat_count; c++) {
            pl_cat_t *cat = &pl_room_cats[c];
            int vis_count = 0;
            for (int r = 0; r < cat->count; r++) {
                int ri = cat->first_idx + r;
                if (ri >= pl_room_count || strcmp(pl_rooms[ri].category, cat->name) != 0) break;
                if (pl_rooms[ri].hidden && !pl_show_hidden) continue;
                if (pl_search[0] && !pl_stristr(pl_rooms[ri].name, pl_search) &&
                    !pl_stristr(pl_rooms[ri].code, pl_search) &&
                    !pl_stristr(pl_rooms[ri].category, pl_search)) continue;
                vis_count++;
            }
            if (vis_count == 0 && pl_search[0]) continue;
            if (idx == item_idx) return 0; /* category header */
            idx++;
            if (cat->expanded) {
                for (int r = 0; r < cat->count; r++) {
                    int ri = cat->first_idx + r;
                    if (ri >= pl_room_count || strcmp(pl_rooms[ri].category, cat->name) != 0) break;
                    if (pl_rooms[ri].hidden && !pl_show_hidden) continue;
                    if (pl_search[0] && !pl_stristr(pl_rooms[ri].name, pl_search) &&
                        !pl_stristr(pl_rooms[ri].code, pl_search)) continue;
                    if (idx == item_idx) {
                        pl_do_goto(pl_rooms[ri].name);
                        pl_wnd_open = 0;
                        return 1;
                    }
                    idx++;
                }
            }
        }
    } else {
        for (int c = 0; c < pl_loop_cat_count; c++) {
            pl_cat_t *cat = &pl_loop_cats[c];
            int vis_count = 0;
            for (int p = 0; p < cat->count; p++) {
                int pi = cat->first_idx + p;
                if (pi >= pl_path_count || !pl_paths[pi].is_loop) break;
                if (strcmp(pl_paths[pi].start_category, cat->name) != 0) break;
                if (pl_search[0] && !pl_stristr(pl_paths[pi].name, pl_search) &&
                    !pl_stristr(pl_paths[pi].file, pl_search) &&
                    !pl_stristr(pl_paths[pi].start_category, pl_search)) continue;
                vis_count++;
            }
            if (vis_count == 0 && pl_search[0]) continue;
            if (idx == item_idx) return 0; /* category header */
            idx++;
            if (cat->expanded) {
                for (int p = 0; p < cat->count; p++) {
                    int pi = cat->first_idx + p;
                    if (pi >= pl_path_count || !pl_paths[pi].is_loop) break;
                    if (strcmp(pl_paths[pi].start_category, cat->name) != 0) break;
                    if (pl_search[0] && !pl_stristr(pl_paths[pi].name, pl_search) &&
                        !pl_stristr(pl_paths[pi].file, pl_search)) continue;
                    if (idx == item_idx) {
                        pl_do_loop(pl_paths[pi].file, pl_paths[pi].name, pl_paths[pi].start_room);
                        pl_wnd_open = 0;
                        return 1;
                    }
                    idx++;
                }
            }
        }
    }
    return 0;
}

/* Single click: categories toggle, items select (green highlight) */
static void pl_click_item(int item_idx)
{
    int idx = 0;
    if (pl_tab == 0) {
        for (int c = 0; c < pl_room_cat_count; c++) {
            pl_cat_t *cat = &pl_room_cats[c];
            int vis_count = 0;
            for (int r = 0; r < cat->count; r++) {
                int ri = cat->first_idx + r;
                if (ri >= pl_room_count || strcmp(pl_rooms[ri].category, cat->name) != 0) break;
                if (pl_rooms[ri].hidden && !pl_show_hidden) continue;
                if (pl_search[0] && !pl_stristr(pl_rooms[ri].name, pl_search) &&
                    !pl_stristr(pl_rooms[ri].code, pl_search) &&
                    !pl_stristr(pl_rooms[ri].category, pl_search)) continue;
                vis_count++;
            }
            if (vis_count == 0 && pl_search[0]) continue;
            if (idx == item_idx) { cat->expanded = !cat->expanded; pl_selected_item = -1; return; }
            idx++;
            if (cat->expanded) {
                for (int r = 0; r < cat->count; r++) {
                    int ri = cat->first_idx + r;
                    if (ri >= pl_room_count || strcmp(pl_rooms[ri].category, cat->name) != 0) break;
                    if (pl_rooms[ri].hidden && !pl_show_hidden) continue;
                    if (pl_search[0] && !pl_stristr(pl_rooms[ri].name, pl_search) &&
                        !pl_stristr(pl_rooms[ri].code, pl_search)) continue;
                    if (idx == item_idx) { pl_selected_item = item_idx; return; }
                    idx++;
                }
            }
        }
    } else {
        for (int c = 0; c < pl_loop_cat_count; c++) {
            pl_cat_t *cat = &pl_loop_cats[c];
            int vis_count = 0;
            for (int p = 0; p < cat->count; p++) {
                int pi = cat->first_idx + p;
                if (pi >= pl_path_count || !pl_paths[pi].is_loop) break;
                if (strcmp(pl_paths[pi].start_category, cat->name) != 0) break;
                if (pl_search[0] && !pl_stristr(pl_paths[pi].name, pl_search) &&
                    !pl_stristr(pl_paths[pi].file, pl_search) &&
                    !pl_stristr(pl_paths[pi].start_category, pl_search)) continue;
                vis_count++;
            }
            if (vis_count == 0 && pl_search[0]) continue;
            if (idx == item_idx) { cat->expanded = !cat->expanded; pl_selected_item = -1; return; }
            idx++;
            if (cat->expanded) {
                for (int p = 0; p < cat->count; p++) {
                    int pi = cat->first_idx + p;
                    if (pi >= pl_path_count || !pl_paths[pi].is_loop) break;
                    if (strcmp(pl_paths[pi].start_category, cat->name) != 0) break;
                    if (pl_search[0] && !pl_stristr(pl_paths[pi].name, pl_search) &&
                        !pl_stristr(pl_paths[pi].file, pl_search)) continue;
                    if (idx == item_idx) { pl_selected_item = item_idx; return; }
                    idx++;
                }
            }
        }
    }
}

/* Execute whatever is currently selected */
static void pl_exec_selected(void)
{
    if (pl_selected_item >= 0) pl_exec_item(pl_selected_item);
}

static void vkm_draw(int vp_w, int vp_h)
{
    if (!vkm_open) return;

    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);

    /* Light scrim behind menu area only — don't dim the whole terminal */

    /* Root menu panel */
    int rh = vkm_root_height();
    vkm_draw_panel(vkm_x, vkm_y, VKM_ROOT_W, rh, t, vp_w, vp_h);

    /* Root menu items */
    const char *root_labels[VKM_ROOT_COUNT] = {
        "Visual  \x10", "Widgets  \x10", "",
        "MegaMUD Settings", "Paths & Loops", "Recent  \x10",
        "Settings  \x10", "Extras  \x10",
        "", "Close  (F11)"
    };

    for (int i = 0; i < VKM_ROOT_COUNT; i++) {
        int iy, ih;
        vkm_get_root_item_rect(i, &iy, &ih);

        if (vkm_is_sep(i)) {
            push_solid(vkm_x + VKM_PAD, iy + ih/2,
                       vkm_x + VKM_ROOT_W - VKM_PAD, iy + ih/2 + 1,
                       t->text[0] * 0.3f, t->text[1] * 0.3f, t->text[2] * 0.3f,
                       0.4f, vp_w, vp_h);
            continue;
        }

        if (i == vkm_hover) {
            push_solid(vkm_x + 1, iy, vkm_x + VKM_ROOT_W - 1, iy + ih,
                       t->accent[0], t->accent[1], t->accent[2],
                       0.15f, vp_w, vp_h);
        }

        float tr = t->text[0], tg = t->text[1], tb = t->text[2];
        if ((i == VKM_ITEM_VISUAL && (vkm_sub == VKM_SUB_VISUAL || vkm_sub == VKM_SUB_THEME || vkm_sub == VKM_SUB_FONT || vkm_sub == VKM_SUB_FX)) ||
            (i == VKM_ITEM_WIDGETS && vkm_sub == VKM_SUB_WIDGETS) ||
            (i == VKM_ITEM_RECENT && vkm_sub == VKM_SUB_RECENT) ||
            (i == VKM_ITEM_EXTRAS && vkm_sub == VKM_SUB_EXTRAS) ||
            (i == VKM_ITEM_XTRA2 && (vkm_sub == VKM_SUB_XTRA2 || vkm_sub == VKM_SUB_VIZ || vkm_sub == VKM_SUB_BG))) {
            tr = t->accent[0]; tg = t->accent[1]; tb = t->accent[2];
        }
        push_text(vkm_x + VKM_PAD, iy + (ih - ch) / 2,
                  root_labels[i], tr, tg, tb, vp_w, vp_h, cw, ch);
    }

    /* Submenu drawing */
    if (vkm_sub != VKM_SUB_NONE) {
        int sx, sy;
        vkm_get_sub_rect(&sx, &sy);
        int count = vkm_sub_count();
        int sh = vkm_sub_height();

        vkm_draw_panel(sx, sy, VKM_SUB_W, sh, t, vp_w, vp_h);

        for (int i = 0; i < count; i++) {
            int iy2 = sy + VKM_PAD + i * VKM_ITEM_H;

            if (i == vkm_sub_hover) {
                push_solid(sx + 1, iy2, sx + VKM_SUB_W - 1, iy2 + VKM_ITEM_H,
                           t->accent[0], t->accent[1], t->accent[2],
                           0.15f, vp_w, vp_h);
            }

            const char *label = NULL;
            int is_active = 0;
            int has_arrow = 0; /* sub-items that open further submenus */

            if (vkm_sub == VKM_SUB_VISUAL) {
                if (i == VKM_VIS_THEME) { label = "Theme  \x10"; has_arrow = 1; }
                else if (i == VKM_VIS_FONT) { label = "Fonts & Text"; }
                else if (i == VKM_VIS_FX) { label = "FX  \x10"; has_arrow = 1; }
                else if (i == VKM_VIS_COLOR) { label = "Color/Brightness"; is_active = clr_visible; }
            } else if (vkm_sub == VKM_SUB_WIDGETS) {
                if (i == VKM_WID_RTIMER) {
                    label = vrt_visible ? "\x04 Round Timer" : "  Round Timer";
                    is_active = vrt_visible;
                } else if (i == VKM_WID_CONVO) {
                    int cv = (vkw_convo_idx >= 0 && vkw_windows[vkw_convo_idx].active);
                    label = cv ? "\x04 Conversation" : "  Conversation";
                    is_active = cv;
                } else if (i == VKM_WID_STATUSBAR) {
                    label = vsb_visible ? "\x04 Status Bar" : "  Status Bar";
                    is_active = vsb_visible;
                } else if (i == VKM_WID_EXPBAR) {
                    label = vxb_visible ? "\x04 Exp Bar" : "  Exp Bar";
                    is_active = vxb_visible;
                } else if (i == VKM_WID_PSTATS) {
                    label = pst_visible ? "\x04 Player Stats" : "  Player Stats";
                    is_active = pst_visible;
                } else if (i == VKM_WID_RADIO) {
                    label = mr_visible ? "\x04 MUDRadio" : "  MUDRadio";
                    is_active = mr_visible;
                } else if (i == VKM_WID_BSCROLL) {
                    label = bsp_visible ? "\x04 Backscroll" : "  Backscroll";
                    is_active = bsp_visible;
                } else if (i == VKM_WID_ICONBAR) {
                    label = vib_visible ? "\x04 Icon Bar" : "  Icon Bar";
                    is_active = vib_visible;
                }
            } else if (vkm_sub == VKM_SUB_RECENT) {
                if (vkm_goto_count == 0) {
                    label = "(empty)";
                } else if (i < vkm_goto_count) {
                    label = vkm_goto_names[i];
                }
            } else if (vkm_sub == VKM_SUB_THEME) {
                label = ui_themes[i].name;
                is_active = (i == current_theme);
            } else if (vkm_sub == VKM_SUB_FONT) {
                if (i == 0) {
                    label = "CP437 Bitmap (VGA)";
                    is_active = (current_font < 0);
                } else if (i - 1 < NUM_TTF_FONTS) {
                    label = ttf_fonts[i - 1].name;
                    is_active = (current_font == i - 1);
                }
            } else if (vkm_sub == VKM_SUB_FX) {
                if (i == VKM_FX_LIQUID) { label = "Liquid Letters"; is_active = fx_liquid_mode; }
                else if (i == VKM_FX_WAVES) { label = "Diagonal Waves"; is_active = fx_waves_mode; }
                else if (i == VKM_FX_FBM) { label = "FBM Currents"; is_active = fx_fbm_mode; }
                else if (i == VKM_FX_SOBEL) { label = "Sobel/Sharp"; is_active = fx_sobel_mode; }
                else if (i == VKM_FX_SCANLINES) { label = "CRT Scanlines"; is_active = fx_scanline_mode; }
                else if (i == VKM_FX_SMOKY) { label = "Smoky Letters"; is_active = fx_smoky_mode; }
                else if (i == VKM_FX_SHADOW) { label = "Drop Shadow"; is_active = fx_shadow_mode; }
                else if (i == VKM_FX_RAIN) { label = "Rain Drops"; is_active = fx_rain_mode; }
                else if (i == VKM_FX_CRAWL) { label = "SpaceWarZ Crawl"; is_active = fx_crawl_mode; }
                else if (i == VKM_FX_WOBBLE && vk_experimental) { label = "Wobbly Widgets \x1b[33m[EXP]"; is_active = fx_wobble_mode; }
            } else if (vkm_sub == VKM_SUB_EXTRAS) {
                if (i == VKM_EXT_SOUND) {
                    label = "Sound & Video";
                    is_active = snd_visible;
                } else if (i == VKM_EXT_COLOR) {
                    label = "Color/Brightness";
                    is_active = clr_visible;
                } else if (i == VKM_EXT_SEP1 || i == VKM_EXT_SEP2 || i == VKM_EXT_SEP3 || i == VKM_EXT_SEP4) {
                    label = "";
                } else if (i == VKM_EXT_CONSOLE) {
                    label = "MMUDPy Console";
                } else if (i == VKM_EXT_SCRIPTMGR) {
                    label = "Script Manager";
                } else if (i == VKM_EXT_HIDEMM) {
                    label = megamud_hidden ? "Show MegaMUD" : "Hide MegaMUD";
                    is_active = megamud_hidden;
                } else if (i == VKM_EXT_RNDRECAP) {
                    label = pst_round_recap ? "\x04 Round Totals" : "  Round Totals";
                    is_active = pst_round_recap;
                } else if (i == VKM_EXT_HDR) {
                    if (hdr_available || hdr_ext_available)
                        label = hdr_enabled ? "\x04 HDR" : "  HDR";
                    else
                        label = "  HDR (N/A)";
                    is_active = hdr_enabled;
                } else if (i == VKM_EXT_SAVE) {
                    label = "Save Settings";
                } else if (i == VKM_EXT_RESET) {
                    label = "Reset to Defaults";
                } else if (i == VKM_EXT_SEP4) {
                    label = "";
                } else if (i == VKM_EXT_PAIRANDROID) {
                    label = "Pair Android Device";
                }
            } else if (vkm_sub == VKM_SUB_XTRA2) {
                if (i == VKM_X2_VIZ) {
                    label = "Visualizations  \x10"; has_arrow = 1;
                } else if (i == VKM_X2_BG) {
                    label = "Backgrounds  \x10"; has_arrow = 1;
                } else if (i == VKM_X2_SEP1) {
                    label = "";
                } else if (i == VKM_X2_RMSTEP) {
                    int on = rm_every_step_get_safe();
                    label = on ? "\x04 RM Every Step" : "  RM Every Step";
                    is_active = on;
                } else if (i == VKM_X2_MAPWALK) {
                    if (!mdw_bin_exists())
                        label = "  Map Walker (no DB)";
                    else
                        label = mdw_visible ? "\x04 Map Walker" : "  Map Walker";
                    is_active = mdw_visible;
                } else if (i == VKM_X2_LOADDB) {
                    label = "Load Map DB...";
                } else if (i == VKM_X2_RMDEBUG) {
                    label = rm_show_debug ? "\x04 Show RM Debug Text"
                                          : "  Show RM Debug Text";
                    is_active = rm_show_debug;
                }
            } else if (vkm_sub == VKM_SUB_BG) {
                if (i == VKM_BG_SETTINGS) {
                    label = "Background Settings...";
                } else if (i == VKM_BG_PLASMA) {
                    label = "Fractal Plasma";
                    is_active = (bg_mode == BG_PLASMA);
                } else if (i == VKM_BG_STARFIELD) {
                    label = "Parallax Starfield";
                    is_active = (bg_mode == BG_STARFIELD);
                } else if (i == VKM_BG_ALIENWALL) {
                    label = "Alien Wall";
                    is_active = (bg_mode == BG_ALIENWALL);
                }
            } else if (vkm_sub == VKM_SUB_VIZ) {
                if (i == VKM_VIZ_BLADES) {
                    label = "Pixel Blades";
                    is_active = (viz_mode == VIZ_BLADES);
                } else if (i == VKM_VIZ_CBALLS) {
                    label = "Cannon Balls";
                    is_active = (viz_mode == VIZ_CANNONBALLS);
                } else if (i == VKM_VIZ_MATRIX) {
                    label = "Matrix Rain";
                    is_active = (viz_mode == VIZ_MATRIX);
                } else if (i == VKM_VIZ_VECTROIDS) {
                    label = "Vectroids";
                    is_active = (viz_mode == VIZ_VECTROIDS);
                }
            }

            float tr, tg, tb;
            if (is_active) {
                tr = t->accent[0]; tg = t->accent[1]; tb = t->accent[2];
            } else {
                tr = t->text[0]; tg = t->text[1]; tb = t->text[2];
            }

            if (label) {
                if (is_active && vkm_sub != VKM_SUB_WIDGETS) {
                    push_text(sx + 3, iy2 + (VKM_ITEM_H - ch) / 2,
                              "\x04", tr, tg, tb, vp_w, vp_h, cw, ch);
                }
                push_text(sx + VKM_PAD + cw + 2, iy2 + (VKM_ITEM_H - ch) / 2,
                          label, tr, tg, tb, vp_w, vp_h, cw, ch);
            }
        }
    }
}

/* ---- Programmatic box-drawing / block elements ----
 * Instead of sampling the font atlas (which causes gaps with bilinear filtering),
 * draw box-drawing and block element characters as solid rectangles.
 * This guarantees gap-free rendering at any resolution, like Kitty/WezTerm do. */

/* Returns 1 if the character was drawn programmatically, 0 if not.
 * cp437 = the CP437 byte value. px0,py0,px1,py1 = cell bounds in pixels.
 * bg_u0..bg_v1 = UV coords for solid glyph (219 = full block).
 * PX macros convert pixel to NDC. */
static int draw_box_char(unsigned char cp437,
                         float px0, float py0, float px1, float py1,
                         float bg_u0, float bg_v0, float bg_u1, float bg_v1,
                         float r, float g, float b,
                         int vp_w, int vp_h)
{
    #define BX2X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define BY2Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)
    #define BSOLID(x0,y0,x1,y1) push_quad(BX2X(x0),BY2Y(y0),BX2X(x1),BY2Y(y1), \
                                           bg_u0,bg_v0,bg_u1,bg_v1, r,g,b, 1.0f)

    float cw = px1 - px0, ch = py1 - py0;
    float cx = px0 + cw * 0.5f, cy = py0 + ch * 0.5f; /* cell center */
    /* Line thickness: ~12% of cell dimension, minimum 1px */
    float lw = cw * 0.12f; if (lw < 1.0f) lw = 1.0f;
    float lh = ch * 0.12f; if (lh < 1.0f) lh = 1.0f;
    /* Double-line offset from center */
    float dw = cw * 0.18f;
    float dh = ch * 0.18f;

    switch (cp437) {
    /* ---- Shade blocks ---- */
    case 0xB0: /* light shade 25% — draw sparse dots */
    case 0xB1: /* medium shade 50% */
    case 0xB2: /* dark shade 75% */ {
        float alpha = (cp437 == 0xB0) ? 0.25f : (cp437 == 0xB1) ? 0.5f : 0.75f;
        push_quad(BX2X(px0),BY2Y(py0),BX2X(px1),BY2Y(py1),
                  bg_u0,bg_v0,bg_u1,bg_v1, r,g,b, alpha);
        return 1;
    }

    /* ---- Single-line box drawing ---- */
    case 0xB3: /* │ vertical */
        BSOLID(cx-lw, py0, cx+lw, py1); return 1;
    case 0xC4: /* ─ horizontal */
        BSOLID(px0, cy-lh, px1, cy+lh); return 1;
    case 0xB4: /* ┤ left+up+down */
        BSOLID(px0, cy-lh, cx+lw, cy+lh);  /* left to center */
        BSOLID(cx-lw, py0, cx+lw, py1);     /* full vertical */
        return 1;
    case 0xC3: /* ├ right+up+down */
        BSOLID(cx-lw, cy-lh, px1, cy+lh);  /* center to right */
        BSOLID(cx-lw, py0, cx+lw, py1);     /* full vertical */
        return 1;
    case 0xC2: /* ┬ left+right+down */
        BSOLID(px0, cy-lh, px1, cy+lh);     /* full horizontal */
        BSOLID(cx-lw, cy, cx+lw, py1);      /* down from center */
        return 1;
    case 0xC1: /* ┴ left+right+up */
        BSOLID(px0, cy-lh, px1, cy+lh);     /* full horizontal */
        BSOLID(cx-lw, py0, cx+lw, cy);      /* up from center */
        return 1;
    case 0xC5: /* ┼ cross */
        BSOLID(px0, cy-lh, px1, cy+lh);     /* full horizontal */
        BSOLID(cx-lw, py0, cx+lw, py1);     /* full vertical */
        return 1;
    case 0xDA: /* ┌ right+down */
        BSOLID(cx-lw, cy-lh, px1, cy+lh);   /* right from center */
        BSOLID(cx-lw, cy, cx+lw, py1);       /* down from center */
        return 1;
    case 0xBF: /* ┐ left+down */
        BSOLID(px0, cy-lh, cx+lw, cy+lh);   /* left to center */
        BSOLID(cx-lw, cy, cx+lw, py1);       /* down from center */
        return 1;
    case 0xC0: /* └ right+up */
        BSOLID(cx-lw, cy-lh, px1, cy+lh);   /* right from center */
        BSOLID(cx-lw, py0, cx+lw, cy+lh);   /* up to center */
        return 1;
    case 0xD9: /* ┘ left+up */
        BSOLID(px0, cy-lh, cx+lw, cy+lh);   /* left to center */
        BSOLID(cx-lw, py0, cx+lw, cy+lh);   /* up to center */
        return 1;

    /* ---- Double-line box drawing ---- */
    case 0xBA: /* ║ double vertical */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, py1);
        return 1;
    case 0xCD: /* ═ double horizontal */
        BSOLID(px0, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, px1, cy+dh+lh);
        return 1;
    case 0xC9: /* ╔ double right+down */
        BSOLID(cx-dw-lw, cy-dh, cx-dw+lw, py1);  /* left vert down */
        BSOLID(cx+dw-lw, cy+dh, cx+dw+lw, py1);  /* right vert down */
        BSOLID(cx-dw, cy-dh-lh, px1, cy-dh+lh);  /* top horiz right */
        BSOLID(cx+dw, cy+dh-lh, px1, cy+dh+lh);  /* bot horiz right */
        return 1;
    case 0xBB: /* ╗ double left+down */
        BSOLID(cx+dw-lw, cy-dh, cx+dw+lw, py1);  /* right vert down */
        BSOLID(cx-dw-lw, cy+dh, cx-dw+lw, py1);  /* left vert down */
        BSOLID(px0, cy-dh-lh, cx+dw, cy-dh+lh);  /* top horiz left */
        BSOLID(px0, cy+dh-lh, cx-dw, cy+dh+lh);  /* bot horiz left */
        return 1;
    case 0xC8: /* ╚ double right+up */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy+dh);  /* left vert up */
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy-dh);  /* right vert up */
        BSOLID(cx-dw, cy+dh-lh, px1, cy+dh+lh);  /* bot horiz right */
        BSOLID(cx+dw, cy-dh-lh, px1, cy-dh+lh);  /* top horiz right */
        return 1;
    case 0xBC: /* ╝ double left+up */
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy+dh);  /* right vert up */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy-dh);  /* left vert up */
        BSOLID(px0, cy+dh-lh, cx+dw, cy+dh+lh);  /* bot horiz left */
        BSOLID(px0, cy-dh-lh, cx-dw, cy-dh+lh);  /* top horiz left */
        return 1;
    case 0xB9: /* ╣ double left+up+down */
        BSOLID(cx+dw-lw, py0, cx+dw+lw, py1);     /* right vert full */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy-dh);   /* left vert up */
        BSOLID(cx-dw-lw, cy+dh, cx-dw+lw, py1);   /* left vert down */
        BSOLID(px0, cy-dh-lh, cx-dw, cy-dh+lh);   /* top horiz left */
        BSOLID(px0, cy+dh-lh, cx-dw, cy+dh+lh);   /* bot horiz left */
        return 1;
    case 0xCC: /* ╠ double right+up+down */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, py1);     /* left vert full */
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy-dh);   /* right vert up */
        BSOLID(cx+dw-lw, cy+dh, cx+dw+lw, py1);   /* right vert down */
        BSOLID(cx+dw, cy-dh-lh, px1, cy-dh+lh);   /* top horiz right */
        BSOLID(cx+dw, cy+dh-lh, px1, cy+dh+lh);   /* bot horiz right */
        return 1;
    case 0xCB: /* ╦ double left+right+down */
        BSOLID(px0, cy-dh-lh, px1, cy-dh+lh);     /* top horiz full */
        BSOLID(px0, cy+dh-lh, cx-dw, cy+dh+lh);   /* bot horiz left */
        BSOLID(cx+dw, cy+dh-lh, px1, cy+dh+lh);   /* bot horiz right */
        BSOLID(cx-dw-lw, cy-dh, cx-dw+lw, py1);   /* left vert down */
        BSOLID(cx+dw-lw, cy-dh, cx+dw+lw, py1);   /* right vert down */
        return 1;
    case 0xCA: /* ╩ double left+right+up */
        BSOLID(px0, cy+dh-lh, px1, cy+dh+lh);     /* bot horiz full */
        BSOLID(px0, cy-dh-lh, cx-dw, cy-dh+lh);   /* top horiz left */
        BSOLID(cx+dw, cy-dh-lh, px1, cy-dh+lh);   /* top horiz right */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy+dh);   /* left vert up */
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy+dh);   /* right vert up */
        return 1;
    case 0xCE: /* ╬ double cross */
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy-dh);   /* left vert up */
        BSOLID(cx-dw-lw, cy+dh, cx-dw+lw, py1);   /* left vert down */
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy-dh);   /* right vert up */
        BSOLID(cx+dw-lw, cy+dh, cx+dw+lw, py1);   /* right vert down */
        BSOLID(px0, cy-dh-lh, cx-dw, cy-dh+lh);   /* top horiz left */
        BSOLID(cx+dw, cy-dh-lh, px1, cy-dh+lh);   /* top horiz right */
        BSOLID(px0, cy+dh-lh, cx-dw, cy+dh+lh);   /* bot horiz left */
        BSOLID(cx+dw, cy+dh-lh, px1, cy+dh+lh);   /* bot horiz right */
        return 1;

    /* ---- Mixed single/double ---- */
    case 0xB5: /* ╡ single-left, double-vert */
        BSOLID(px0, cy-lh, cx, cy+lh);
        BSOLID(cx-dw-lw, py0, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, py1);
        return 1;
    case 0xB6: /* ╢ double-left, single-vert (actually single-vert with double-left) */
        BSOLID(px0, cy-dh-lh, cx, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, cx, cy+dh+lh);
        BSOLID(cx-lw, py0, cx+lw, py1);
        return 1;
    case 0xB7: /* ╖ single-left, double-down */
        BSOLID(px0, cy-lh, cx, cy+lh);
        BSOLID(cx-dw-lw, cy, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, cy, cx+dw+lw, py1);
        return 1;
    case 0xB8: /* ╕ double-left, single-down */
        BSOLID(px0, cy-dh-lh, cx, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, cx, cy+dh+lh);
        BSOLID(cx-lw, cy, cx+lw, py1);
        return 1;
    case 0xC6: /* ╞ single-right, double-vert (═├) */
        BSOLID(cx, cy-lh, px1, cy+lh);
        BSOLID(cx-dw-lw, py0, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, py1);
        return 1;
    case 0xC7: /* ╟ double-right, single-vert */
        BSOLID(cx, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(cx, cy+dh-lh, px1, cy+dh+lh);
        BSOLID(cx-lw, py0, cx+lw, py1);
        return 1;
    case 0xD5: /* ╒ double-right, single-down */
        BSOLID(cx, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(cx, cy+dh-lh, px1, cy+dh+lh);
        BSOLID(cx-lw, cy, cx+lw, py1);
        return 1;
    case 0xD4: /* ╘ double-right, single-up */
        BSOLID(cx, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(cx, cy+dh-lh, px1, cy+dh+lh);
        BSOLID(cx-lw, py0, cx+lw, cy);
        return 1;
    case 0xD6: /* ╓ single-right, double-down */
        BSOLID(cx, cy-lh, px1, cy+lh);
        BSOLID(cx-dw-lw, cy, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, cy, cx+dw+lw, py1);
        return 1;
    case 0xD3: /* ╙ single-right, double-up */
        BSOLID(cx, cy-lh, px1, cy+lh);
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy);
        return 1;
    case 0xBD: /* ╜ single-left, double-up */
        BSOLID(px0, cy-lh, cx, cy+lh);
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy);
        return 1;
    case 0xBE: /* ╛ double-left, single-up */
        BSOLID(px0, cy-dh-lh, cx, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, cx, cy+dh+lh);
        BSOLID(cx-lw, py0, cx+lw, cy);
        return 1;
    case 0xD7: /* ╫ single-cross with double-vert */
        BSOLID(px0, cy-lh, px1, cy+lh);
        BSOLID(cx-dw-lw, py0, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, py1);
        return 1;
    case 0xD8: /* ╪ double-cross with single-vert */
        BSOLID(px0, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, px1, cy+dh+lh);
        BSOLID(cx-lw, py0, cx+lw, py1);
        return 1;
    case 0xD1: /* ╤ double-horiz, single-down */
        BSOLID(px0, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, px1, cy+dh+lh);
        BSOLID(cx-lw, cy, cx+lw, py1);
        return 1;
    case 0xD2: /* ╥ single-horiz, double-down */
        BSOLID(px0, cy-lh, px1, cy+lh);
        BSOLID(cx-dw-lw, cy, cx-dw+lw, py1);
        BSOLID(cx+dw-lw, cy, cx+dw+lw, py1);
        return 1;
    case 0xCF: /* ╧ double-horiz, single-up */
        BSOLID(px0, cy-dh-lh, px1, cy-dh+lh);
        BSOLID(px0, cy+dh-lh, px1, cy+dh+lh);
        BSOLID(cx-lw, py0, cx+lw, cy);
        return 1;
    case 0xD0: /* ╨ single-horiz, double-up */
        BSOLID(px0, cy-lh, px1, cy+lh);
        BSOLID(cx-dw-lw, py0, cx-dw+lw, cy);
        BSOLID(cx+dw-lw, py0, cx+dw+lw, cy);
        return 1;

    /* ---- Block elements ---- */
    case 0xDB: /* █ full block */
        BSOLID(px0, py0, px1, py1); return 1;
    case 0xDC: /* ▄ lower half */
        BSOLID(px0, cy, px1, py1); return 1;
    case 0xDD: /* ▌ left half */
        BSOLID(px0, py0, cx, py1); return 1;
    case 0xDE: /* ▐ right half */
        BSOLID(cx, py0, px1, py1); return 1;
    case 0xDF: /* ▀ upper half */
        BSOLID(px0, py0, px1, cy); return 1;

    default:
        return 0; /* not a box-drawing char — use font atlas */
    }
    #undef BX2X
    #undef BY2Y
    #undef BSOLID
    return 0;
}

/* ---- Status Bar: read MegaMUD state ---- */
#ifndef SB_GETTEXTA
#define SB_GETTEXTA 0x0402
#endif
#ifndef SB_GETPARTS
#define SB_GETPARTS 0x0406
#endif

static void vsb_read_status(void)
{
    DWORD now = GetTickCount();
    if (now - vsb_last_read < 250) return; /* throttle reads to 4/sec */
    vsb_last_read = now;

    if (!api) return;
    unsigned int sbase = api->get_struct_base();
    if (!sbase) return;

    /* Read memory offsets */
    vsb_cur_hp = api->read_struct_i32(VSB_OFF_CUR_HP);
    vsb_max_hp = api->read_struct_i32(VSB_OFF_MAX_HP);
    vsb_cur_mana = api->read_struct_i32(VSB_OFF_CUR_MANA);
    vsb_max_mana = api->read_struct_i32(VSB_OFF_MAX_MANA);
    /* Mana tick detection: mana increased while resting/meditating */
    if (vsb_prev_mana >= 0 && vsb_cur_mana > vsb_prev_mana &&
        (vsb_is_resting || vsb_is_medit) && !vsb_in_combat) {
        vsb_mana_tick_val = vsb_cur_mana - vsb_prev_mana;
        vsb_mana_tick_time = now;
        vsb_mana_tick_show = 1;
    }
    /* Fade out after 2 seconds */
    if (vsb_mana_tick_show && now - vsb_mana_tick_time > 2000)
        vsb_mana_tick_show = 0;
    vsb_prev_mana = vsb_cur_mana;
    vsb_in_combat = api->read_struct_i32(VSB_OFF_IN_COMBAT);
    vsb_is_resting = api->read_struct_i32(VSB_OFF_IS_RESTING);
    vsb_is_medit = api->read_struct_i32(VSB_OFF_IS_MEDIT);
    vsb_cur_step = api->read_struct_i32(VSB_OFF_CUR_STEP);
    vsb_total_steps = api->read_struct_i32(VSB_OFF_TOTAL_STEPS);
    /* Lap detection: step went from high back to 1 = new lap */
    if (vsb_looping && vsb_prev_step > 1 && vsb_cur_step == 1)
        vsb_lap_count++;
    vsb_prev_step = vsb_cur_step;
    {
        int new_pathing = api->read_struct_i32(VSB_OFF_PATHING);
        int new_looping = api->read_struct_i32(VSB_OFF_LOOPING);
        /* Reset exp tracking when pathing or looping starts (0→1 transition) */
        if ((new_pathing && !vsb_prev_pathing) || (new_looping && !vsb_prev_looping)) {
            pst_reset_exp();
            if (new_looping && !vsb_prev_looping) {
                vsb_lap_count = 0;
                vsb_prev_step = -1;
            }
        }
        /* GOTO LOOP PREVENTION: While a goto is active, continuously
         * zero the loop resume triggers at 0x5988/0x598C every tick.
         * VA_VERIFY_PATH checks these on arrival to decide idle vs loop.
         * By zeroing them ~60Hz, we ensure the arrival handler always
         * sees 0 and takes the idle branch — no extra step. */
        if (vsb_goto_dest[0]) {
            unsigned int sbase_addr = api->get_struct_base();
            if (sbase_addr) {
                unsigned char *sb = (unsigned char *)(uintptr_t)sbase_addr;
                *(int *)(sb + 0x5988) = 0;  /* loop resume trigger A */
                *(int *)(sb + 0x598C) = 0;  /* loop resume trigger B */
                *(int *)(sb + VSB_OFF_LOOPING)  = 0;
                *(int *)(sb + VSB_OFF_ON_ENTRY) = 0;
            }
            /* Backup watchdog: if looping STILL resumed somehow, kill it */
            if (new_looping && !vsb_prev_looping) {
                if (sbase_addr) {
                    unsigned char *sb = (unsigned char *)(uintptr_t)sbase_addr;
                    *(int *)(sb + VSB_OFF_PATHING)  = 0;
                    *(int *)(sb + VSB_OFF_GO_FLAG)  = 0;
                    *(int *)(sb + VSB_OFF_MODE)     = 11;
                    new_looping = 0;
                    new_pathing = 0;
                }
                vsb_goto_dest[0] = '\0';
            }
        }
        /* Clear goto destination label when pathing ends normally */
        if (!new_pathing && vsb_prev_pathing) vsb_goto_dest[0] = '\0';
        vsb_prev_pathing = new_pathing;
        vsb_prev_looping = new_looping;
        vsb_pathing = new_pathing;
        vsb_looping = new_looping;
    }
    vsb_roaming = api->read_struct_i32(VSB_OFF_ROAMING);
    vsb_autocombat = api->read_struct_i32(VSB_OFF_AUTOCOMBAT);

    /* Read character INI name from MMMAIN window title (updates if changed) */
    if (mmmain_hwnd) {
        char title[256];
        if (GetWindowTextA(mmmain_hwnd, title, sizeof(title)) > 0) {
            char new_name[64] = {0};
            int k = 0;
            for (int i = 0; title[i] && title[i] != ' ' && title[i] != '[' && k < 63; i++)
                new_name[k++] = title[i];
            new_name[k] = '\0';
            if (strcmp(new_name, vsb_char_name) != 0)
                strncpy(vsb_char_name, new_name, sizeof(vsb_char_name) - 1);
        }
    }

    /* Read combat target string from struct */
    {
        unsigned char *base = (unsigned char *)(uintptr_t)sbase;
        unsigned char *tgt = base + VSB_OFF_COMBAT_TGT;
        /* MegaMUD strings have a size prefix byte, then chars */
        int tlen = tgt[0];
        if (tlen > 0 && tlen < 60) {
            memcpy(vsb_target, tgt + 1, tlen);
            vsb_target[tlen] = '\0';
        } else {
            vsb_target[0] = '\0';
        }
    }

    /* Read status bar part 0 (path name) from MegaMUD's statusbar control */
    {
        HWND mw = api->get_mmmain_hwnd();
        if (mw) {
            HWND sb = GetDlgItem(mw, VSB_STATUSBAR_ID);
            if (sb) {
                char buf[128] = {0};
                SendMessageA(sb, SB_GETTEXTA, 0, (LPARAM)buf);
                buf[127] = '\0';
                /* Strip any " | Map:" suffix we may have added */
                char *pipe = strstr(buf, " | Map:");
                if (pipe) *pipe = '\0';
                /* Only overwrite if statusbar returned something —
                   don't blank out CLI-set path name */
                if (buf[0]) {
                    strncpy(vsb_path_name, buf, sizeof(vsb_path_name) - 1);
                    vsb_path_name[sizeof(vsb_path_name) - 1] = '\0';
                }
            }
        }
        /* Clear path name when no longer pathing or looping */
        if (!vsb_pathing && !vsb_looping)
            vsb_path_name[0] = '\0';
    }

    /* Derive status text */
    if (vsb_in_combat) {
        if (vsb_target[0])
            _snprintf(vsb_status_text, sizeof(vsb_status_text), "Combat: %s", vsb_target);
        else
            _snprintf(vsb_status_text, sizeof(vsb_status_text), "In Combat");
    } else if (vsb_is_resting && vsb_is_medit) {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Meditating");
    } else if (vsb_is_resting) {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Resting");
    } else if (vsb_is_medit) {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Meditating");
    } else if (vsb_roaming) {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Roaming");
    } else if (vsb_goto_dest[0]) {
        /* Goto initiated — always show destination, overrides looping */
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Walking to %s", vsb_goto_dest);
    } else if (vsb_looping) {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Looping");
    } else if (vsb_pathing) {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), vsb_autocombat ? "Walking" : "Running");
    } else {
        _snprintf(vsb_status_text, sizeof(vsb_status_text), "Idle");
    }
    vsb_status_text[sizeof(vsb_status_text) - 1] = '\0';
}

/* ---- Status Bar: draw ---- */
static void vsb_draw(int vp_w, int vp_h)
{
    if (!vsb_visible) return;

    vsb_read_status();

    const ui_theme_t *t = &ui_themes[current_theme];

    /* Use UI font if available, else main font */
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);

    /* Bar background — slightly lighter than theme bg, full width */
    psolid(0, 0, (float)vp_w, (float)VSB_BAR_H,
           t->bg[0] + 0.06f, t->bg[1] + 0.06f, t->bg[2] + 0.06f, 0.95f,
           vp_w, vp_h);

    /* Bottom border line */
    psolid(0, (float)(VSB_BAR_H - 1), (float)vp_w, (float)VSB_BAR_H,
           t->accent[0], t->accent[1], t->accent[2], 0.6f,
           vp_w, vp_h);

    int tx = VSB_PAD;
    int ty = (VSB_BAR_H - ch) / 2;
    char buf[128];

    /* Helper: draw a labeled section and advance tx */
    #define VSB_SECTION(label_str, value_str, label_r, label_g, label_b, val_r, val_g, val_b) \
        do { \
            ptext(tx, ty, label_str, label_r, label_g, label_b, vp_w, vp_h, cw, ch); \
            tx += (int)strlen(label_str) * cw; \
            ptext(tx, ty, value_str, val_r, val_g, val_b, vp_w, vp_h, cw, ch); \
            tx += (int)strlen(value_str) * cw + VSB_PAD; \
            /* divider */ \
            psolid((float)tx, 3.0f*ui_scale, (float)(tx + VSB_SEP_W), (float)(VSB_BAR_H) - 3.0f*ui_scale, \
                   t->text[0], t->text[1], t->text[2], 0.2f, vp_w, vp_h); \
            tx += VSB_SEP_W + VSB_PAD; \
        } while(0)

    /* Dim label color */
    float lr = t->dim[0], lg = t->dim[1], lb = t->dim[2];
    /* Bright value color */
    float vr = t->text[0], vg = t->text[1], vb = t->text[2];

    /* Section: Path/Loop name (always shown as first cell) */
    if (vsb_path_name[0]) {
        const char *plabel = vsb_looping ? "Loop: " : "Path: ";
        if (vsb_looping && vsb_lap_count > 0) {
            _snprintf(buf, sizeof(buf), "%s  Lap %d", vsb_path_name, vsb_lap_count);
        } else {
            _snprintf(buf, sizeof(buf), "%s", vsb_path_name);
        }
        buf[sizeof(buf) - 1] = '\0';
        VSB_SECTION(plabel, buf, lr, lg, lb, vr, vg, vb);
    } else {
        VSB_SECTION("Path: ", "No Path", lr, lg, lb, lr, lg, lb);
    }

    /* Section: Step */
    if (vsb_pathing && vsb_total_steps > 0) {
        _snprintf(buf, sizeof(buf), "%d/%d", vsb_cur_step, vsb_total_steps);
        buf[sizeof(buf) - 1] = '\0';
        VSB_SECTION("Step: ", buf, lr, lg, lb, vr, vg, vb);
    }

    /* Section: Character (loaded INI) */
    if (vsb_char_name[0]) {
        VSB_SECTION("Char: ", vsb_char_name, lr, lg, lb, vr, vg, vb);
    }

    /* Section: HP */
    if (vsb_max_hp > 0) {
        _snprintf(buf, sizeof(buf), "%d/%d", vsb_cur_hp, vsb_max_hp);
        buf[sizeof(buf) - 1] = '\0';
        float hp_ratio = (float)vsb_cur_hp / (float)vsb_max_hp;
        float hr = hp_ratio > 0.5f ? vr : (hp_ratio > 0.25f ? 1.0f : 1.0f);
        float hg = hp_ratio > 0.5f ? vg : (hp_ratio > 0.25f ? 0.8f : 0.2f);
        float hb = hp_ratio > 0.5f ? vb : (hp_ratio > 0.25f ? 0.0f : 0.2f);
        VSB_SECTION("HP: ", buf, lr, lg, lb, hr, hg, hb);
    }

    /* Section: Mana */
    if (vsb_max_mana > 0) {
        _snprintf(buf, sizeof(buf), "%d/%d", vsb_cur_mana, vsb_max_mana);
        buf[sizeof(buf) - 1] = '\0';
        float mn_ratio = (float)vsb_cur_mana / (float)vsb_max_mana;
        float mr = mn_ratio > 0.5f ? 0.4f : (mn_ratio > 0.25f ? 1.0f : 1.0f);
        float mg = mn_ratio > 0.5f ? 0.6f : (mn_ratio > 0.25f ? 0.5f : 0.2f);
        float mb = mn_ratio > 0.5f ? 1.0f : (mn_ratio > 0.25f ? 0.0f : 0.2f);
        VSB_SECTION("Mana: ", buf, lr, lg, lb, mr, mg, mb);

        /* Mana tick indicator — flashes when mana regenerates, shows amount */
        if (vsb_mana_tick_show) {
            DWORD elapsed = GetTickCount() - vsb_mana_tick_time;
            float alpha = (elapsed < 500) ? 1.0f : 1.0f - (float)(elapsed - 500) / 1500.0f;
            if (alpha < 0.0f) alpha = 0.0f;
            /* Pulsing cyan diamond indicator */
            float pulse = 0.7f + 0.3f * (float)sin((double)elapsed * 0.006);
            ptext(tx, ty, "\x04", 0.3f * pulse, 1.0f * pulse, 1.0f * pulse * alpha,
                  vp_w, vp_h, cw, ch);
            tx += cw;
            char tick_buf[16];
            _snprintf(tick_buf, sizeof(tick_buf), "+%d", vsb_mana_tick_val);
            tick_buf[sizeof(tick_buf) - 1] = '\0';
            ptext(tx, ty, tick_buf, 0.3f, 0.9f * alpha, 1.0f * alpha,
                  vp_w, vp_h, cw, ch);
            tx += (int)strlen(tick_buf) * cw + VSB_PAD;
            psolid((float)tx, 3.0f*ui_scale, (float)(tx + VSB_SEP_W), (float)(VSB_BAR_H) - 3.0f*ui_scale,
                   t->text[0], t->text[1], t->text[2], 0.2f, vp_w, vp_h);
            tx += VSB_SEP_W + VSB_PAD;
        }
    }

    /* Section: Status */
    {
        /* Color-code status */
        float sr = vr, sg = vg, sb = vb;
        if (vsb_in_combat)       { sr = 1.0f; sg = 0.3f; sb = 0.3f; }
        else if (vsb_is_resting) { sr = 0.3f; sg = 1.0f; sb = 0.3f; }
        else if (vsb_is_medit)   { sr = 0.5f; sg = 0.5f; sb = 1.0f; }
        else if (vsb_roaming)    { sr = 1.0f; sg = 0.8f; sb = 0.2f; }
        else if (vsb_looping)    { sr = 0.8f; sg = 0.6f; sb = 1.0f; }
        else if (vsb_pathing)    { sr = 0.6f; sg = 0.9f; sb = 1.0f; }
        VSB_SECTION("Status: ", vsb_status_text, lr, lg, lb, sr, sg, sb);
    }

    /* Section: Loaded Scripts */
    {
        int has_scripts = (vsb_scripts_str[0] && strcmp(vsb_scripts_str, "None") != 0);
        if (has_scripts)
            VSB_SECTION("Scripts: ", vsb_scripts_str, lr, lg, lb, 0.3f, 1.0f, 0.3f);
        else
            VSB_SECTION("Scripts: ", "None", lr, lg, lb, lr, lg, lb);
    }

    /* Section: Exp/hr from Player Stats tracker */
    if (vsb_exp_rate > 0) {
        if (vsb_exp_rate >= 1000000LL)
            _snprintf(buf, sizeof(buf), "%lldm/hr", vsb_exp_rate / 1000000LL);
        else if (vsb_exp_rate >= 1000LL)
            _snprintf(buf, sizeof(buf), "%lldk/hr", vsb_exp_rate / 1000LL);
        else
            _snprintf(buf, sizeof(buf), "%lld/hr", vsb_exp_rate);
        buf[sizeof(buf) - 1] = '\0';
        VSB_SECTION("Exp: ", buf, lr, lg, lb, 0.3f, 1.0f, 0.3f);
    }

    #undef VSB_SECTION
}

/* ============================================================
 * MegaMUD Icon Bar — toggle buttons synced with MegaMUD state
 * ============================================================ */

#define VIB_BAR_H_BASE 48    /* icon bar height at 1080p */
static int VIB_BAR_H = 48;
static int vib_visible = 0;

/* Auto-hide slide animation */
static float vib_slide = 0.0f;         /* 0 = fully hidden, 1 = fully revealed */
static DWORD vib_last_interact = 0;    /* tick of last mouse interaction */
static DWORD vib_last_frame = 0;       /* for delta-time animation */
#define VIB_SHOW_MS   3000   /* stay visible 3 seconds after last interaction */
#define VIB_SLIDE_SPEED 5.0f /* slide speed (units per second, 1=full travel) */

/* Button definitions: tooltip, icon char, config offset, runtime offset, is_toggle */
typedef struct {
    const char *tip;         /* hover tooltip */
    const char *icon;        /* CP437 icon character */
    unsigned int cfg_off;    /* 0x4Dxx config offset */
    unsigned int rt_off;     /* 0x57xx runtime offset */
    int is_toggle;           /* 1 = toggle, 0 = action button */
} vib_btn_t;

#define VIB_OFF_GO_FLAG    0x564C
#define VIB_OFF_CONNECTED  0x563C
#define VIB_OFF_CONFIRM_HU 0x8AB8

static const vib_btn_t vib_buttons[] = {
    { "Combat", "\x0F", 0x4D00, 0x573C, 1 },  /* ☼ */
    { "Heal",   "\x03", 0x4D08, 0x5748, 1 },  /* ♥ */
    { "Bless",  "\x0E", 0x4D0C, 0x574C, 1 },  /* ♫ */
    { "Cash",   "$",    0x4D14, 0x5754, 1 },
    { "Items",  "\x04", 0x4D18, 0x5758, 1 },  /* ♦ */
    { "Search", "?",    0x4D1C, 0x575C, 1 },
    { "Sneak",  "\xF0", 0x4D20, 0x5764, 1 },  /* ≡ */
    { "Hide",   "\xB1", 0x4D24, 0x5768, 1 },  /* ▒ */
    { "Go",     "\x10", 0,      VIB_OFF_GO_FLAG, 1 },  /* ► */
    { "Back",   "\x11", 0, 0, 0 },             /* ◄ */
    { "Phone",  "\x0C", 0, 0, 0 },             /* ♀ (handset) */
};
#define VIB_BTN_COUNT (sizeof(vib_buttons) / sizeof(vib_buttons[0]))
#define VIB_BTN_GO    8
#define VIB_BTN_BACK  9
#define VIB_BTN_PHONE 10

static int  vib_hover = -1;    /* button under mouse */
static int  vib_pressed = -1;  /* button being clicked */
static int  vib_pinned = 1;    /* 1 = always visible, 0 = auto-hide */
static int  vib_pin_hover = 0; /* mouse over pin button */
static DWORD vib_last_read = 0;
static int  vib_states[VIB_BTN_COUNT]; /* cached on/off states */
static int  vib_cur_bar_y = -100; /* current animated Y position, updated each frame */

static DWORD vib_last_log = 0;
static void vib_read_states(void)
{
    DWORD now = GetTickCount();
    if (now - vib_last_read < 250) return;
    vib_last_read = now;
    if (!api) return;
    unsigned int sbase = api->get_struct_base();
    if (!sbase) return;
    int do_log = (now - vib_last_log > 5000); /* log every 5s */
    if (do_log) vib_last_log = now;
    for (int i = 0; i < (int)VIB_BTN_COUNT; i++) {
        if (vib_buttons[i].is_toggle && vib_buttons[i].cfg_off) {
            /* Read config offset (0x4Dxx) — runtime (0x57xx) resets to 0 on connect */
            int raw = api->read_struct_i32(vib_buttons[i].cfg_off);
            vib_states[i] = raw != 0;
            if (do_log)
                api->log("[vib] %s: cfg=0x%04X raw=%d state=%d\n",
                         vib_buttons[i].tip, vib_buttons[i].cfg_off, raw, vib_states[i]);
        } else if (i == VIB_BTN_GO) {
            /* Go/Stop has no config offset, use runtime */
            int raw = api->read_struct_i32(VIB_OFF_GO_FLAG);
            vib_states[i] = raw != 0;
            if (do_log)
                api->log("[vib] Go: raw=%d state=%d\n", raw, vib_states[i]);
        } else if (i == VIB_BTN_PHONE) {
            int raw = api->read_struct_i32(VIB_OFF_CONNECTED);
            vib_states[i] = raw != 0;
            if (do_log)
                api->log("[vib] Phone: raw=%d state=%d\n", raw, vib_states[i]);
        }
    }
}

static void vib_click(int idx)
{
    if (idx < 0 || idx >= (int)VIB_BTN_COUNT) return;
    if (!api) return;
    /* Block all buttons except Phone when disconnected */
    if (idx != VIB_BTN_PHONE && !vib_states[VIB_BTN_PHONE]) return;
    unsigned int sbase = api->get_struct_base();
    if (!sbase) return;
    unsigned char *base = (unsigned char *)(uintptr_t)sbase;

    if (vib_buttons[idx].is_toggle) {
        if (idx == VIB_BTN_GO) {
            /* Stop/Go — send MegaMUD's toolbar WM_COMMAND */
            HWND mw = api->get_mmmain_hwnd();
            if (mw) SendMessageA(mw, WM_COMMAND, 0x0803, 0);
        } else {
            /* Toggle: read from config, flip, write to BOTH config and runtime */
            int cur = *(int *)(base + vib_buttons[idx].cfg_off);
            int nv = cur ? 0 : 1;
            *(int *)(base + vib_buttons[idx].cfg_off) = nv;
            if (vib_buttons[idx].rt_off)
                *(int *)(base + vib_buttons[idx].rt_off) = nv;
            vib_states[idx] = nv;
        }
    } else if (idx == VIB_BTN_PHONE) {
        /* Hangup/Connect toggle */
        HWND mw = api->get_mmmain_hwnd();
        if (!mw) return;
        int connected = api->read_struct_i32(VIB_OFF_CONNECTED);
        if (connected) {
            /* Disable confirmation dialog, then send phone button */
            *(int *)(base + VIB_OFF_CONFIRM_HU) = 0;
        }
        SendMessageA(mw, WM_COMMAND, 0x07D4, 0);
    }
    /* VIB_BTN_BACK: not hooked up yet */
}

/* Push a single icon quad from the icon atlas. icon_idx = 0..VIB_ICON_COUNT-1.
 * r,g,b tint the icon (1,1,1 = full color, 0.3,0.3,0.3 = dim). */
static void push_icon_quad(float x0, float y0, float x1, float y1,
                           int icon_idx, float r, float g, float b, float a,
                           int vp_w, int vp_h)
{
    float u0 = (float)(icon_idx * VIB_ICON_CELL) / (float)VIB_ICON_ATLAS_W;
    float u1 = (float)((icon_idx + 1) * VIB_ICON_CELL) / (float)VIB_ICON_ATLAS_W;
    float v0 = 0.0f, v1 = 1.0f;
    #define VI2X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define VI2Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)
    push_quad(VI2X(x0), VI2Y(y0), VI2X(x1), VI2Y(y1),
              u0, v0, u1, v1, r, g, b, a);
    #undef VI2X
    #undef VI2Y
}

/* Icon bar geometry helpers */
static int vib_btn_size(void) { return (int)(40 * ui_scale); } /* square button size */
static int vib_gap(void)      { return (int)(3 * ui_scale); }
static int vib_pin_size(void) { return (int)(18 * ui_scale); } /* small pin button */
static int vib_total_w(void)  {
    return (int)VIB_BTN_COUNT * vib_btn_size() + ((int)VIB_BTN_COUNT - 1) * vib_gap()
           + (int)(12 * ui_scale) + vib_gap() + vib_pin_size() + (int)(4 * ui_scale);
}

/* Anchor: top edge the tab slides down from */
static int vib_anchor_y(void) { return vsb_visible ? VSB_BAR_H : 0; }

/* Update slide animation each frame. Returns current tab top Y. */
static int vib_update_slide(int mouse_y, int vp_w)
{
    if (!vib_visible) { vib_slide = 0.0f; return -VIB_BAR_H; }

    DWORD now = GetTickCount();
    float dt = (vib_last_frame > 0) ? (float)(now - vib_last_frame) / 1000.0f : 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    vib_last_frame = now;

    int anchor = vib_anchor_y();
    int tw = vib_total_w();
    int tab_x0 = (vp_w - tw) / 2;

    /* Trigger zone: mouse near anchor, within tab horizontal bounds (+ margin) */
    int margin = (int)(30 * ui_scale);
    int in_trigger = (mouse_y >= 0 && mouse_y < anchor + (int)(10 * ui_scale) &&
                      vkm_mouse_x >= tab_x0 - margin && vkm_mouse_x < tab_x0 + tw + margin);
    /* Stay open while mouse is over the tab itself */
    float eased = 1.0f - (1.0f - vib_slide) * (1.0f - vib_slide);
    int bar_y = anchor - (int)((1.0f - eased) * VIB_BAR_H);
    int in_tab = (vib_slide > 0.1f && mouse_y >= bar_y && mouse_y < bar_y + VIB_BAR_H &&
                  vkm_mouse_x >= tab_x0 && vkm_mouse_x < tab_x0 + tw);

    if (in_trigger || in_tab || vib_pressed >= 0)
        vib_last_interact = now;

    int want_show;
    if (vib_pinned) {
        want_show = 1;
    } else {
        /* Auto-hide: show only on hover near the top edge / over the bar / recent click.
         * Intentionally does NOT sticky-show on active toggles — that defeats auto-hide
         * since Combat/Heal/etc are almost always on. */
        want_show = in_trigger || in_tab || vib_pressed >= 0 ||
                    (vib_last_interact != 0 && (now - vib_last_interact) < VIB_SHOW_MS);
    }
    float target = want_show ? 1.0f : 0.0f;
    float speed = VIB_SLIDE_SPEED * dt;
    if (vib_slide < target) {
        vib_slide += speed;
        if (vib_slide > target) vib_slide = target;
    } else if (vib_slide > target) {
        vib_slide -= speed * 0.6f;
        if (vib_slide < target) vib_slide = target;
    }

    eased = 1.0f - (1.0f - vib_slide) * (1.0f - vib_slide);
    return anchor - (int)((1.0f - eased) * VIB_BAR_H);
}

static void vib_draw(int vp_w, int vp_h, int bar_y)
{
    if (!vib_visible || vib_slide < 0.01f || !vib_icon_ready) {
        /* No icon quads — collapse range so render loop draws UI quads normally */
        vib_quad_start = vib_quad_end = quad_count;
        return;
    }

    vib_read_states();

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;

    int bs = vib_btn_size();
    int gap = vib_gap();
    int tw = vib_total_w();
    int pad = (int)(6 * ui_scale);
    int tab_x = (vp_w - tw) / 2;
    float alpha = (vib_slide < 0.3f) ? vib_slide / 0.3f : 1.0f;
    int corner = (int)(3 * ui_scale);
    float anim_t = (float)GetTickCount() / 1000.0f; /* seconds for orbit animation */

    /* Tab background */
    psolid((float)tab_x, (float)bar_y, (float)(tab_x + tw), (float)(bar_y + VIB_BAR_H),
           t->bg[0] + 0.04f, t->bg[1] + 0.04f, t->bg[2] + 0.04f, 0.93f * alpha,
           vp_w, vp_h);
    /* Bottom edge */
    psolid((float)(tab_x + corner), (float)(bar_y + VIB_BAR_H - 1),
           (float)(tab_x + tw - corner), (float)(bar_y + VIB_BAR_H),
           t->accent[0], t->accent[1], t->accent[2], 0.5f * alpha,
           vp_w, vp_h);
    /* Side edges */
    psolid((float)tab_x, (float)(bar_y + corner), (float)(tab_x + 1), (float)(bar_y + VIB_BAR_H - corner),
           t->accent[0], t->accent[1], t->accent[2], 0.2f * alpha, vp_w, vp_h);
    psolid((float)(tab_x + tw - 1), (float)(bar_y + corner), (float)(tab_x + tw), (float)(bar_y + VIB_BAR_H - corner),
           t->accent[0], t->accent[1], t->accent[2], 0.2f * alpha, vp_w, vp_h);

    int bx = tab_x + pad;
    int by = bar_y + (VIB_BAR_H - bs) / 2;

    /* Pass 1: button backgrounds, borders (all use UI font atlas / solid quads) */
    for (int i = 0; i < (int)VIB_BTN_COUNT; i++) {
        const vib_btn_t *b = &vib_buttons[i];
        int is_on = vib_states[i];
        int is_hov = (vib_hover == i);
        int is_press = (vib_pressed == i);

        /* Button background */
        float br, bg, bb, ba;
        if (is_press) {
            br = t->accent[0] * 0.5f; bg = t->accent[1] * 0.5f; bb = t->accent[2] * 0.5f; ba = 0.95f;
        } else if (is_on) {
            if (i == VIB_BTN_GO) {
                br = 0.10f; bg = 0.02f; bb = 0.02f; ba = 0.92f; /* dark red for stopped */
            } else {
                br = 0.02f; bg = 0.06f; bb = 0.02f; ba = 0.92f; /* dark green tint */
            }
        } else if (is_hov) {
            br = t->bg[0] + 0.15f; bg = t->bg[1] + 0.15f; bb = t->bg[2] + 0.15f; ba = 0.95f;
        } else {
            br = t->bg[0] + 0.08f; bg = t->bg[1] + 0.08f; bb = t->bg[2] + 0.08f; ba = 0.7f;
        }
        ba *= alpha;
        psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + bs),
               br, bg, bb, ba, vp_w, vp_h);

        /* Border: energy orbs orbiting the square edge when ON, 3D border when off */
        if (is_on) {
            float bdr, bdg, bdb;
            int is_go = (i == VIB_BTN_GO);
            if (is_go) {
                bdr = 1.0f; bdg = 0.2f; bdb = 0.2f; /* red for stopped */
            } else {
                bdr = 0.2f; bdg = 1.0f; bdb = 0.3f; /* green for active */
            }
            /* Dim border base — thin glow along edges */
            int bw = (int)(1.0f * ui_scale); if (bw < 1) bw = 1;
            psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + bw),
                   bdr, bdg, bdb, 0.25f * alpha, vp_w, vp_h);
            psolid((float)bx, (float)(by + bs - bw), (float)(bx + bs), (float)(by + bs),
                   bdr, bdg, bdb, 0.25f * alpha, vp_w, vp_h);
            psolid((float)bx, (float)by, (float)(bx + bw), (float)(by + bs),
                   bdr, bdg, bdb, 0.25f * alpha, vp_w, vp_h);
            psolid((float)(bx + bs - bw), (float)by, (float)(bx + bs), (float)(by + bs),
                   bdr, bdg, bdb, 0.25f * alpha, vp_w, vp_h);

            /* Energy orbs traveling along the square perimeter.
             * Uses CP437 bullet '•' (0x07) from UI TTF atlas for smooth round orbs. */
            if (!is_go) {
                int num_orbs = 2;
                float perim = (float)(bs * 4);
                for (int j = 0; j < num_orbs; j++) {
                    float t_orb = fmodf(anim_t * 0.8f + (float)j * 0.5f + (float)i * 0.3f, 1.0f);
                    float dist = t_orb * perim;
                    float ox, oy;
                    if (dist < (float)bs) {
                        ox = (float)bx + dist; oy = (float)by;
                    } else if (dist < (float)(bs * 2)) {
                        ox = (float)(bx + bs); oy = (float)by + (dist - (float)bs);
                    } else if (dist < (float)(bs * 3)) {
                        ox = (float)(bx + bs) - (dist - (float)(bs * 2)); oy = (float)(by + bs);
                    } else {
                        ox = (float)bx; oy = (float)(by + bs) - (dist - (float)(bs * 3));
                    }
                    float pulse = 0.6f + 0.4f * sinf(anim_t * 5.0f + (float)j * 3.14f);
                    int orb_sz = (int)(10.0f * ui_scale * (0.8f + 0.2f * pulse));
                    /* Glow: larger, dimmer orb behind */
                    int glow_sz = orb_sz * 3;
                    ptext((int)(ox) - glow_sz/2, (int)(oy) - glow_sz/2,
                          "\x07", bdr, bdg, bdb,
                          vp_w, vp_h, glow_sz, glow_sz);
                    /* Bright core orb */
                    ptext((int)(ox) - orb_sz/2, (int)(oy) - orb_sz/2,
                          "\x07",
                          bdr * 0.3f + 0.7f, bdg * 0.3f + 0.7f, bdb * 0.3f + 0.7f,
                          vp_w, vp_h, orb_sz, orb_sz);
                }
            }
        } else {
            /* Raised 3D border */
            psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + 1),
                   1.0f, 1.0f, 1.0f, 0.12f * alpha, vp_w, vp_h);
            psolid((float)bx, (float)by, (float)(bx + 1), (float)(by + bs),
                   1.0f, 1.0f, 1.0f, 0.08f * alpha, vp_w, vp_h);
            psolid((float)(bx + bs - 1), (float)by, (float)(bx + bs), (float)(by + bs),
                   0.0f, 0.0f, 0.0f, 0.2f * alpha, vp_w, vp_h);
            psolid((float)bx, (float)(by + bs - 1), (float)(bx + bs), (float)(by + bs),
                   0.0f, 0.0f, 0.0f, 0.2f * alpha, vp_w, vp_h);
        }

        /* Hover tooltip */
        if (is_hov && vib_slide > 0.8f) {
            const char *tip = b->tip;
            if (i == VIB_BTN_PHONE) tip = is_on ? "Disconnect" : "Connect";
            else if (i == VIB_BTN_GO) tip = is_on ? "Stop" : "Go";
            int tip_len = (int)strlen(tip);
            int tip_cw = (int)(VSB_CHAR_W * ui_scale);
            int tip_ch = (int)(VSB_CHAR_H * ui_scale);
            int tip_w = tip_len * tip_cw + (int)(8 * ui_scale);
            int tip_h = tip_ch + (int)(6 * ui_scale);
            int tip_x = bx + bs / 2 - tip_w / 2;
            int tip_y = bar_y + VIB_BAR_H + (int)(2 * ui_scale);
            if (tip_x < 2) tip_x = 2;
            if (tip_x + tip_w > vp_w - 2) tip_x = vp_w - 2 - tip_w;
            psolid((float)tip_x, (float)tip_y, (float)(tip_x + tip_w), (float)(tip_y + tip_h),
                   t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
            psolid((float)tip_x, (float)tip_y, (float)(tip_x + tip_w), (float)(tip_y + 1),
                   0.3f, 1.0f, 0.4f, 0.5f, vp_w, vp_h);
            ptext(tip_x + (int)(4 * ui_scale), tip_y + (int)(3 * ui_scale),
                  tip, t->text[0], t->text[1], t->text[2], vp_w, vp_h, tip_cw, tip_ch);
        }

        bx += bs + gap;
    }

    /* Pin/unpin button — small pushpin icon drawn with quads.
     * Pinned  = black pushpin upright (stuck in).
     * Unpinned (auto-hide) = dim gray pushpin laying on its side / dimmed. */
    {
        int ps = vib_pin_size();
        int px = bx + gap;
        int py = bar_y + VIB_BAR_H / 2 - ps / 2;
        /* Subtle background only on hover */
        if (vib_pin_hover) {
            psolid((float)px, (float)py, (float)(px + ps), (float)(py + ps),
                   t->bg[0] + 0.15f, t->bg[1] + 0.15f, t->bg[2] + 0.15f,
                   0.6f * alpha, vp_w, vp_h);
        }
        /* Pushpin colors */
        float cr, cg, cb, ca;
        if (vib_pinned) {
            cr = 0.02f; cg = 0.02f; cb = 0.02f; ca = 0.95f; /* black = stuck in */
        } else {
            cr = 0.55f; cg = 0.55f; cb = 0.55f; ca = 0.75f; /* gray = unpinned */
        }
        ca *= alpha;
        /* Draw pushpin shape centered in button:
         *   [head] — round-ish top cap
         *   [shaft] — vertical body
         *   [tip]   — pointy bottom
         * Scaled to fit inside ps x ps. */
        float cx = (float)(px + ps / 2);
        float cy = (float)(py + ps / 2);
        float s  = (float)ps * 0.5f;         /* half-size */
        float head_w = s * 0.95f;
        float head_h = s * 0.35f;
        float shaft_w = s * 0.18f;
        float shaft_h = s * 0.55f;
        float tip_w = s * 0.22f;
        float tip_h = s * 0.20f;
        float top = cy - s * 0.85f;
        /* Head cap (top) */
        psolid(cx - head_w, top,
               cx + head_w, top + head_h,
               cr, cg, cb, ca, vp_w, vp_h);
        /* Head highlight when pinned (tiny sheen) */
        if (vib_pinned) {
            psolid(cx - head_w * 0.6f, top + head_h * 0.15f,
                   cx - head_w * 0.1f, top + head_h * 0.45f,
                   0.9f, 0.9f, 0.9f, 0.35f * alpha, vp_w, vp_h);
        }
        /* Shaft */
        psolid(cx - shaft_w, top + head_h,
               cx + shaft_w, top + head_h + shaft_h,
               cr, cg, cb, ca, vp_w, vp_h);
        /* Tip (narrowing) — two side rects approximate a triangle */
        float ty = top + head_h + shaft_h;
        psolid(cx - tip_w * 0.7f, ty,
               cx + tip_w * 0.7f, ty + tip_h * 0.5f,
               cr, cg, cb, ca, vp_w, vp_h);
        psolid(cx - tip_w * 0.35f, ty + tip_h * 0.5f,
               cx + tip_w * 0.35f, ty + tip_h,
               cr, cg, cb, ca, vp_w, vp_h);
        /* Tooltip */
        if (vib_pin_hover && vib_slide > 0.8f) {
            const char *tip = vib_pinned ? "Auto-hide" : "Pin bar";
            int tip_len = (int)strlen(tip);
            int tip_cw = (int)(VSB_CHAR_W * ui_scale);
            int tip_ch = (int)(VSB_CHAR_H * ui_scale);
            int tip_w = tip_len * tip_cw + (int)(8 * ui_scale);
            int tip_h = tip_ch + (int)(6 * ui_scale);
            int tip_x = px + ps / 2 - tip_w / 2;
            int tip_y = bar_y + VIB_BAR_H + (int)(2 * ui_scale);
            if (tip_x + tip_w > vp_w - 2) tip_x = vp_w - 2 - tip_w;
            psolid((float)tip_x, (float)tip_y, (float)(tip_x + tip_w), (float)(tip_y + tip_h),
                   t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
            ptext(tip_x + (int)(4 * ui_scale), tip_y + (int)(3 * ui_scale),
                  tip, t->text[0], t->text[1], t->text[2], vp_w, vp_h, tip_cw, tip_ch);
        }
    }

    /* Pass 2: icon emoji quads — these use the icon atlas descriptor set */
    vib_quad_start = quad_count;
    bx = tab_x + pad;
    {
        int icon_pad = (int)(4 * ui_scale); /* padding inside button for icon */
        for (int i = 0; i < (int)VIB_BTN_COUNT; i++) {
            int is_on = vib_states[i];
            int is_hov = (vib_hover == i);
            float a;
            if (is_on) a = 1.0f;
            else if (is_hov) a = 0.85f;
            else a = 0.35f; /* dim when off */
            /* Go button: when stopped (is_on), show red stop sign;
             * when going (!is_on), show bright green arrow */
            if (i == VIB_BTN_GO) {
                if (is_on) {
                    /* Stopped — red tinted icon */
                    push_icon_quad((float)(bx + icon_pad), (float)(by + icon_pad),
                                   (float)(bx + bs - icon_pad), (float)(by + bs - icon_pad),
                                   i, 1.0f, 0.1f, 0.1f, 1.0f * alpha, vp_w, vp_h);
                } else {
                    /* Going — bright green arrow */
                    push_icon_quad((float)(bx + icon_pad), (float)(by + icon_pad),
                                   (float)(bx + bs - icon_pad), (float)(by + bs - icon_pad),
                                   i, 0.1f, 1.0f, 0.2f, 1.0f * alpha, vp_w, vp_h);
                }
            } else {
                /* r=-1 signals the shader to use full texture RGB (color emoji mode) */
                push_icon_quad((float)(bx + icon_pad), (float)(by + icon_pad),
                               (float)(bx + bs - icon_pad), (float)(by + bs - icon_pad),
                               i, -1.0f, 0.0f, 0.0f, a * alpha, vp_w, vp_h);
            }
            bx += bs + gap;
        }
    }
    vib_quad_end = quad_count;
}

/* Hit test: returns button index, -2 for pin button, or -1 for miss. */
#define VIB_HIT_PIN -2
static int vib_hit_test(int mx, int my, int bar_y)
{
    if (!vib_visible || vib_slide < 0.5f) return -1;

    int bs = vib_btn_size();
    int gap = vib_gap();
    int tw = vib_total_w();
    int pad = (int)(6 * ui_scale);
    int tab_x = ((int)vk_sc_extent.width - tw) / 2;
    int by = bar_y + (VIB_BAR_H - bs) / 2;

    /* Check main buttons */
    if (my >= by && my < by + bs) {
        int bx = tab_x + pad;
        for (int i = 0; i < (int)VIB_BTN_COUNT; i++) {
            if (mx >= bx && mx < bx + bs) return i;
            bx += bs + gap;
        }
    }

    /* Check pin button */
    int ps = vib_pin_size();
    int bx_end = tab_x + pad + (int)VIB_BTN_COUNT * (bs + gap);
    int px = bx_end + gap;
    int py = bar_y + VIB_BAR_H / 2 - ps / 2;
    if (mx >= px && mx < px + ps && my >= py && my < py + ps)
        return VIB_HIT_PIN;

    return -1;
}

/* ---- Exp Bar: read status ---- */
static void vxb_read_status(void)
{
    DWORD now = GetTickCount();
    if (now - vxb_last_read < 500) return; /* throttle to 2/sec */
    vxb_last_read = now;

    if (!api) return;
    unsigned int sbase = api->get_struct_base();
    if (!sbase) return;

    int exp_lo  = api->read_struct_i32(VXB_OFF_EXP_LO);
    int exp_hi  = api->read_struct_i32(VXB_OFF_EXP_HI);
    int need_lo = api->read_struct_i32(VXB_OFF_NEED_LO);
    int need_hi = api->read_struct_i32(VXB_OFF_NEED_HI);
    vxb_level   = api->read_struct_i32(VXB_OFF_LEVEL);

    vxb_exp    = ((long long)(unsigned int)exp_hi << 32) | (unsigned int)exp_lo;
    vxb_needed = ((long long)(unsigned int)need_hi << 32) | (unsigned int)need_lo;

    /* Detect XP gain for flash */
    if (vxb_exp > vxb_prev_exp && vxb_prev_exp > 0) {
        vxb_flash = 1.0f;
    }
    vxb_prev_exp = vxb_exp;

    /* Decay flash */
    if (vxb_flash > 0.0f) vxb_flash -= 0.03f;
    if (vxb_flash < 0.0f) vxb_flash = 0.0f;

    /* Calculate percentage: exp / (exp + needed) * 100 */
    long long total = vxb_exp + vxb_needed;
    if (total > 0)
        vxb_percent = (float)((double)vxb_exp / (double)total * 100.0);
    else
        vxb_percent = 0.0f;
}

/* ---- Exp Bar: draw ---- */
static void vxb_draw(int vp_w, int vp_h)
{
    if (!vxb_visible) return;

    vxb_read_status();

    const ui_theme_t *t = &ui_themes[current_theme];

    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);

    /* Bar sits just above the input bar */
    int ibh = (int)(INPUT_BAR_H_BASE * ui_scale);
    float bar_top = (float)(vp_h - ibh - VXB_BAR_H);
    float bar_bot = (float)(vp_h - ibh);

    /* Background — darker than theme bg */
    psolid(0, bar_top, (float)vp_w, bar_bot,
           t->bg[0] * 0.6f, t->bg[1] * 0.6f, t->bg[2] * 0.6f, 0.95f,
           vp_w, vp_h);

    /* Top border line — accent color */
    psolid(0, bar_top, (float)vp_w, bar_top + 1.0f,
           t->accent[0], t->accent[1], t->accent[2], 0.5f,
           vp_w, vp_h);

    /* Draw 20 segments */
    int seg_area_w = vp_w - 2 * VXB_PAD;
    float seg_w = (float)(seg_area_w - (VXB_SEG_COUNT - 1) * VXB_SEG_GAP) / (float)VXB_SEG_COUNT;
    int filled = (int)(vxb_percent / 5.0f);
    float partial = (vxb_percent - filled * 5.0f) / 5.0f;
    if (filled > VXB_SEG_COUNT) filled = VXB_SEG_COUNT;
    int overflow = (vxb_percent >= 100.0f);

    float seg_pad = 3.0f * ui_scale;
    float seg_top = bar_top + seg_pad;
    float seg_bot = bar_bot - seg_pad;

    for (int i = 0; i < VXB_SEG_COUNT; i++) {
        float sx0 = VXB_PAD + i * (seg_w + VXB_SEG_GAP);
        float sx1 = sx0 + seg_w;

        /* Segment background (empty) */
        psolid(sx0, seg_top, sx1, seg_bot,
               t->bg[0] + 0.03f, t->bg[1] + 0.03f, t->bg[2] + 0.03f, 0.8f,
               vp_w, vp_h);

        float fill = 0.0f;
        if (i < filled) fill = 1.0f;
        else if (i == filled) fill = partial;

        if (fill > 0.0f) {
            float fx1 = sx0 + seg_w * fill;

            /* Gradient: blend accent toward a brighter version across the bar */
            float grad = (float)i / (float)(VXB_SEG_COUNT - 1);
            float fr, fg, fb;
            if (overflow) {
                /* Gold glow for overflow (ready to train) */
                fr = 1.0f;
                fg = 0.84f + 0.16f * grad;
                fb = 0.0f;
            } else {
                /* Theme accent -> brighter accent */
                fr = t->accent[0] * (1.0f - grad * 0.3f) + grad * 0.3f;
                fg = t->accent[1] * (1.0f - grad * 0.3f) + grad * 0.3f;
                fb = t->accent[2] * (1.0f - grad * 0.3f) + grad * 0.3f;
            }

            /* XP gain flash — brighten all filled segments */
            if (vxb_flash > 0.0f) {
                fr = fr + (1.0f - fr) * vxb_flash * 0.5f;
                fg = fg + (1.0f - fg) * vxb_flash * 0.5f;
                fb = fb + (1.0f - fb) * vxb_flash * 0.5f;
            }

            /* Filled portion */
            psolid(sx0, seg_top, fx1, seg_bot,
                   fr, fg, fb, 0.95f,
                   vp_w, vp_h);

            /* Subtle highlight on top edge of filled segment */
            psolid(sx0, seg_top, fx1, seg_top + ui_scale,
                   fr + 0.15f, fg + 0.15f, fb + 0.15f, 0.6f,
                   vp_w, vp_h);
        }
    }

    /* Info text overlay centered on bar */
    char info[128];
    if (overflow) {
        _snprintf(info, sizeof(info), "Level %d \xC4 READY TO TRAIN!", vxb_level);
    } else if (vxb_needed > 0) {
        /* Format needed with commas */
        char needed_str[32];
        if (vxb_needed >= 1000000000LL)
            _snprintf(needed_str, sizeof(needed_str), "%lld,%03lld,%03lld,%03lld",
                     vxb_needed / 1000000000LL, (vxb_needed / 1000000LL) % 1000,
                     (vxb_needed / 1000LL) % 1000, vxb_needed % 1000);
        else if (vxb_needed >= 1000000LL)
            _snprintf(needed_str, sizeof(needed_str), "%lld,%03lld,%03lld",
                     vxb_needed / 1000000LL, (vxb_needed / 1000LL) % 1000, vxb_needed % 1000);
        else if (vxb_needed >= 1000LL)
            _snprintf(needed_str, sizeof(needed_str), "%lld,%03lld",
                     vxb_needed / 1000LL, vxb_needed % 1000);
        else
            _snprintf(needed_str, sizeof(needed_str), "%lld", vxb_needed);
        _snprintf(info, sizeof(info), "Level %d \xC4 %.1f%% \xC4 %s to next",
                 vxb_level, vxb_percent, needed_str);
    } else {
        _snprintf(info, sizeof(info), "Level %d \xC4 %.1f%%", vxb_level, vxb_percent);
    }
    info[sizeof(info) - 1] = '\0';

    /* Use larger font for exp bar text — slightly bigger than normal UI text */
    int ecw = (int)(VKM_CHAR_W * ui_scale * 1.2f), ech = (int)(VKM_CHAR_H * ui_scale * 1.2f);
    int text_w = (int)strlen(info) * ecw;
    int text_x = (vp_w - text_w) / 2;
    int text_y = (int)bar_top + (VXB_BAR_H - ech) / 2;

    /* Thick shadow outline for readability — 8 directions + center drop */
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy)
                ptext(text_x + dx, text_y + dy, info, 0.0f, 0.0f, 0.0f,
                      vp_w, vp_h, ecw, ech);
    /* Extra drop shadow for depth */
    ptext(text_x + 1, text_y + 2, info, 0.0f, 0.0f, 0.0f, vp_w, vp_h, ecw, ech);
    /* Bold: draw text twice offset by 1px for faux-bold */
    float tr, tg, tb;
    if (overflow) { tr = 1.0f; tg = 0.9f; tb = 0.2f; }
    else { tr = t->text[0]; tg = t->text[1]; tb = t->text[2]; }
    ptext(text_x, text_y, info, tr, tg, tb, vp_w, vp_h, ecw, ech);
    ptext(text_x + 1, text_y, info, tr, tg, tb, vp_w, vp_h, ecw, ech);
}

#include "script_manager_ui.c"

static void vkt_build_vertices(void)
{
    quad_count = 0;
    if (!palette || !vk_vdata) return;

    int vp_w = (int)vk_sc_extent.width;
    int vp_h = (int)vk_sc_extent.height;
    ui_scale = (float)vp_h / 1080.0f;
    if (ui_scale < 1.0f) ui_scale = 1.0f;
    vkm_update_scale();

    /* Check deferred round recap grace period */
    pst_recap_poll();
    vrt_ctx_update_scale();
    VSB_BAR_H = (int)(VSB_BAR_H_BASE * ui_scale);
    VSB_PAD = (int)(VSB_PAD_BASE * ui_scale);
    VSB_SEP_W = (int)(VSB_SEP_W_BASE * ui_scale); if (VSB_SEP_W < 1) VSB_SEP_W = 1;
    VXB_BAR_H = (int)(VXB_BAR_H_BASE * ui_scale);
    VXB_SEG_GAP = (int)(VXB_SEG_GAP_BASE * ui_scale); if (VXB_SEG_GAP < 1) VXB_SEG_GAP = 1;
    VXB_PAD = (int)(VXB_PAD_BASE * ui_scale);
    int input_bar_h = (int)(INPUT_BAR_H_BASE * ui_scale);
    int top_pad = vsb_visible ? VSB_BAR_H : 4;
    int bot_pad = vxb_visible ? VXB_BAR_H + 4 : 4;
    int term_h = vp_h - input_bar_h - top_pad - bot_pad;

    /* Maintain 1:2 (w:h) CP437 aspect ratio per character.
     * Scale uniformly to fit, center horizontally with black bars. */
    int margin_px = (int)(term_margin * ui_scale);
    int usable_w = vp_w - margin_px;
    float cw_fill = (float)usable_w / (float)TERM_COLS;
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
    float x_offset = (float)margin_px;
    last_cw_px = cw; last_ch_px = ch;  /* store for smoke shader push constants */

    /* Precompute integer cell boundaries — eliminates sub-pixel gaps between
     * adjacent character quads (the #1 cause of vertical black lines in
     * tile/terminal renderers). Every quad edge is shared exactly. */
    static int col_x[TERM_COLS + 1];
    static int row_y[TERM_ROWS + 1];
    for (int c = 0; c <= TERM_COLS; c++)
        col_x[c] = (int)(x_offset + c * cw + 0.5f);
    for (int r = 0; r <= TERM_ROWS; r++)
        row_y[r] = (int)(top_pad + r * ch + 0.5f);

    /* UV constants for font atlas (16x16 grid).
     * Half-texel inset prevents LINEAR filtering from bleeding across cell borders. */
    float tex_cw = 1.0f / 16.0f;
    float tex_ch = 1.0f / 16.0f;
    /* Only the built-in CP437 bitmap (font<0) skips inset — it uses NEAREST filter.
     * All TTF-rendered fonts (including Px437/Unscii) need the half-texel inset
     * to prevent LINEAR filter bleed across adjacent atlas cells. */
    float hp_u = (current_font < 0) ? 0.0f : 0.5f / (float)cur_atlas_w;
    float hp_v = (current_font < 0) ? 0.0f : 0.5f / (float)cur_atlas_h;

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
    float bg_u0 = (219 % 16) * tex_cw + hp_u;
    float bg_v0 = (219 / 16) * tex_ch + hp_v;
    float bg_u1 = bg_u0 + tex_cw - 2*hp_u;
    float bg_v1 = bg_v0 + tex_ch - 2*hp_v;

    /* Update visualization physics */
    viz_update(1.0f / 60.0f, cw, ch, x_offset, (float)top_pad,
               (float)vp_w, (float)vp_h, tex_cw, tex_ch, hp_u, hp_v);

    /* Background layer: fullscreen quad for animated backgrounds */
    bg_quad_end = 0;
    if (bg_mode != BG_NONE) {
        push_quad(PX2NDC_X(0), PX2NDC_Y(0),
                  PX2NDC_X(vp_w), PX2NDC_Y(vp_h),
                  bg_u0, bg_v0, bg_u1, bg_v1,
                  1.0f, 1.0f, 1.0f, 1.0f);
        bg_quad_end = quad_count;
    }

    /* Pass 1: backgrounds using solid glyph — merge adjacent same-color cells
     * into single wide quads to eliminate sub-pixel gaps between columns */
    for (int r = 0; r < TERM_ROWS; r++) {
        int c = 0;
        while (c < TERM_COLS) {
            int bg = ansi_term.grid[r][c].attr.bg & 0x07;
            if (bg == 0) { c++; continue; }
            /* Find run of same bg color */
            int run_start = c;
            while (c < TERM_COLS && (ansi_term.grid[r][c].attr.bg & 0x07) == bg)
                c++;
            /* Emit one quad for the entire run — use integer grid for gap-free edges */
            float px0 = (float)col_x[run_start], py0 = (float)row_y[r];
            float px1 = (float)col_x[c],         py1 = (float)row_y[r + 1];
            /* Alpha 0.75 signals shader to skip color post-processing (hue/sat/etc)
             * so recap/highlighted backgrounds keep their true ANSI color */
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      palette[bg].r, palette[bg].g, palette[bg].b, 0.75f);
        }
    }

    /* Pass 1.5: drop shadow quads — emitted before text so they render behind.
     * Each visible character gets a shadow quad at the same glyph UV, offset by
     * shadow_distance pixels in shadow_angle direction, with shadow color+opacity.
     * Alpha=0.65 signals the shader to apply gaussian blur for soft shadows. */
    if (fx_shadow_mode) {
        float rad = shadow_angle * 3.14159265f / 180.0f;
        float sdx = cosf(rad) * shadow_distance;
        float sdy = sinf(rad) * shadow_distance;
        for (int r = 0; r < TERM_ROWS; r++) {
            for (int c = 0; c < TERM_COLS; c++) {
                unsigned char byte = ansi_term.grid[r][c].ch;
                if (byte == 0 || byte == 32) continue;
                /* Skip box-drawing chars (they use programmatic rendering) */
                if (byte >= 0xB0 && byte <= 0xDF) continue;
                float px0 = (float)col_x[c] + sdx;
                float py0 = (float)row_y[r] + sdy;
                float px1 = (float)col_x[c + 1] + sdx;
                float py1 = (float)row_y[r + 1] + sdy;
                /* Apply viz displacement if active */
                if (viz_mode != VIZ_NONE && r < VIZ_ROWS && c < VIZ_COLS) {
                    if (viz_cells[r][c].shattered) continue;
                    px0 += viz_cells[r][c].dx;
                    py0 += viz_cells[r][c].dy;
                    px1 += viz_cells[r][c].dx;
                    py1 += viz_cells[r][c].dy;
                }
                float u0 = (byte % 16) * tex_cw + hp_u;
                float v0 = (byte / 16) * tex_ch + hp_v;
                float u1 = u0 + tex_cw - 2*hp_u;
                float v1 = v0 + tex_ch - 2*hp_v;
                /* alpha=0.65 signals shader: this is a shadow quad */
                push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                          u0, v0, u1, v1,
                          shadow_r, shadow_g, shadow_b, 0.65f);
            }
        }
    }

    /* Pass 2: text characters — box-drawing/block elements drawn programmatically,
     * all other characters sampled from font atlas.
     * Per-word gradient: each word's characters interpolate through the theme's
     * 10-stop color ramp for their ANSI color. */
    for (int r = 0; r < TERM_ROWS; r++) {
        /* Pre-scan row: find word end for each column (right edge of contiguous text) */
        int word_end[TERM_COLS];
        int we = -1;
        for (int c = TERM_COLS - 1; c >= 0; c--) {
            unsigned char ch = ansi_term.grid[r][c].ch;
            if (ch == 0 || ch == 32) { we = -1; word_end[c] = c; }
            else { if (we < 0) we = c; word_end[c] = we; }
        }

        int word_start = -1;
        for (int c = 0; c < TERM_COLS; c++) {
            unsigned char byte = ansi_term.grid[r][c].ch;
            if (byte == 0 || byte == 32) {
                word_start = -1;
                /* When smoke is active, emit wisp quads ABOVE letters in same column.
                 * Each wisp quad encodes: R=proximity, G=rise_cells, B=col_seed, A=0
                 * Skip cells with background color (recap/highlight areas) */
                int cell_bg = ansi_term.grid[r][c].attr.bg & 0x07;
                if (fx_smoky_mode && cell_bg == 0) {
                    /* Look DOWN the same column for nearest letter */
                    int rise = 0;
                    for (int dr = 1; dr <= 6; dr++) {
                        int rr = r + dr;
                        if (rr >= TERM_ROWS) break;
                        unsigned char below = ansi_term.grid[rr][c].ch;
                        if (below != 0 && below != 32) {
                            rise = dr;
                            break;
                        }
                    }
                    if (rise > 0) {
                        float proximity = 1.0f - (float)(rise - 1) / 6.0f;
                        float col_seed = ((float)(c * 73 + 17) / 256.0f);
                        col_seed = col_seed - (int)col_seed; /* fract */
                        /* Overlap smoke quads by 0.5px to eliminate sub-pixel gaps */
                        float px0 = x_offset + c * cw - 0.5f, py0 = top_pad + r * ch - 0.5f;
                        float px1 = px0 + cw + 1.0f, py1 = py0 + ch + 1.0f;
                        float su0 = (32 % 16) * tex_cw + hp_u;
                        float sv0 = (32 / 16) * tex_ch + hp_v;
                        float su1 = su0 + tex_cw - 2*hp_u;
                        float sv1 = sv0 + tex_ch - 2*hp_v;
                        push_quad(PX2NDC_X(px0), PX2NDC_Y(py0),
                                  PX2NDC_X(px1), PX2NDC_Y(py1),
                                  su0, sv0, su1, sv1,
                                  proximity, (float)rise, col_seed, 0.0f);
                    }
                }
                continue;
            }
            if (word_start < 0) word_start = c;

            /* Skip shattered cells (viz_fx renders fragments instead) */
            if (viz_mode != VIZ_NONE && r < VIZ_ROWS && c < VIZ_COLS &&
                viz_cells[r][c].shattered) continue;

            /* Compute gradient position within word */
            int wlen = word_end[c] - word_start + 1;
            float grad_t = (wlen > 1) ? (float)(c - word_start) / (float)(wlen - 1) : 0.0f;

            ap_attr_t *a = &ansi_term.grid[r][c].attr;
            /* fg 0-7 = dim, 8-15 = bright (via SGR 90-97).
             * Bold (SGR 1) also promotes dim to bright. */
            int fg = a->fg256 ? (a->fg & 0x0F) :
                     (a->fg < 8) ? (a->fg | (a->bold ? 0x08 : 0)) : (a->fg & 0x0F);

            /* Sample the theme gradient ramp */
            rgb_t col = sample_ramp(&theme_ramps[fg], grad_t);

            float px0 = (float)col_x[c];
            float py0 = (float)row_y[r];
            float px1 = (float)col_x[c + 1];
            float py1 = (float)row_y[r + 1];

            /* Apply viz displacement */
            if (viz_mode != VIZ_NONE && r < VIZ_ROWS && c < VIZ_COLS) {
                px0 += viz_cells[r][c].dx;
                py0 += viz_cells[r][c].dy;
                px1 += viz_cells[r][c].dx;
                py1 += viz_cells[r][c].dy;
            }

            /* Try programmatic box-drawing first (gap-free at any resolution) */
            if (byte >= 0xB0 && byte <= 0xDF &&
                draw_box_char(byte, px0, py0, px1, py1,
                              bg_u0, bg_v0, bg_u1, bg_v1,
                              col.r, col.g, col.b,
                              vp_w, vp_h))
                continue;
            /* Fall through to font atlas for regular characters */
            float u0 = (byte % 16) * tex_cw + hp_u;
            float v0 = (byte / 16) * tex_ch + hp_v;
            float u1 = u0 + tex_cw - 2*hp_u;
            float v1 = v0 + tex_ch - 2*hp_v;
            /* alpha=0.75 signals "has bg color" → shader adds dark outline
             * and skips hue shift for readability (recap text etc.) */
            int text_bg = a->bg & 0x07;
            float text_alpha = (text_bg != 0) ? 0.75f : 1.0f;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      u0, v0, u1, v1,
                      col.r, col.g, col.b, text_alpha);
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

    /* Pass 4: input bar text — proper 1:2 aspect ratio, centered in bar */
    {
        float ibar_ch = (float)input_bar_h;
        /* Use 1:2 aspect ratio chars that fit the bar height with padding */
        float ich = ibar_ch - 4.0f;
        float icw = ich / 2.0f;
        float bar_y0 = (float)(top_pad + term_h + bot_pad);
        float text_y = bar_y0 + (ibar_ch - ich) / 2.0f;
        for (int i = 0; i < input_len && i < TERM_COLS; i++) {
            unsigned char ch = (unsigned char)input_buf[i];
            if (ch <= 32) continue;
            float u0 = (ch % 16) * tex_cw + hp_u;
            float v0 = (ch / 16) * tex_ch + hp_v;
            float u1 = u0 + tex_cw - 2*hp_u;
            float v1 = v0 + tex_ch - 2*hp_v;
            float px0 = x_offset + i * icw;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(text_y), PX2NDC_X(px0 + icw), PX2NDC_Y(text_y + ich),
                      u0, v0, u1, v1,
                      1.0f, 1.0f, 1.0f, 1.0f);
        }

        /* Blinking cursor in input bar (only in normal mode) */
        if (!input_mode && (frame_count / 30) % 2 == 0) {
            float cx0 = x_offset + input_cursor * icw;
            float cy0 = bar_y0 + ibar_ch - 4.0f * ui_scale;
            float cx1 = cx0 + icw;
            float cy1 = bar_y0 + ibar_ch - 1;
            push_quad(PX2NDC_X(cx0), PX2NDC_Y(cy0), PX2NDC_X(cx1), PX2NDC_Y(cy1),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      0.7f, 0.7f, 0.7f, 1.0f);
        }

        /* Direct mode indicator — right side of input bar */
        {
            const char *mode_label = input_mode ? "[DIRECT]" : "[INPUT]";
            float mr = input_mode ? 0.3f : 0.5f;
            float mg = input_mode ? 1.0f : 0.5f;
            float mb = input_mode ? 0.3f : 0.5f;
            float ma = input_mode ? 1.0f : 0.4f;
            int label_len = (int)strlen(mode_label);
            float lx0 = (float)vp_w - label_len * icw - 8.0f;
            for (int i = 0; mode_label[i]; i++) {
                unsigned char mc = (unsigned char)mode_label[i];
                if (mc <= 32) { continue; }
                float mu0 = (mc % 16) * tex_cw + hp_u;
                float mv0 = (mc / 16) * tex_ch + hp_v;
                float mu1 = mu0 + tex_cw - 2*hp_u;
                float mv1 = mv0 + tex_ch - 2*hp_v;
                float mpx = lx0 + i * icw;
                push_quad(PX2NDC_X(mpx), PX2NDC_Y(text_y), PX2NDC_X(mpx + icw), PX2NDC_Y(text_y + ich),
                          mu0, mv0, mu1, mv1, mr, mg, mb, ma);
            }
        }

        /* Direct mode: big blinking block cursor at terminal cursor position */
        if (input_mode && (frame_count / 20) % 2 == 0) {
            int crow = ansi_term.cy;
            int ccol = ansi_term.cx;
            float bcx = x_offset + ccol * cw;
            float bcy = (float)top_pad + crow * ch;
            push_quad(PX2NDC_X(bcx), PX2NDC_Y(bcy), PX2NDC_X(bcx + cw), PX2NDC_Y(bcy + ch),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      0.3f, 1.0f, 0.3f, 0.7f);
        }
    }

    #undef PX2NDC_X
    #undef PX2NDC_Y

    /* Push visualization quads (boulders, fragments, rain, ship, lasers) */
    viz_push_quads(cw, ch, x_offset, (float)top_pad, vp_w, vp_h,
                    tex_cw, tex_ch, hp_u, hp_v);

    /* Mark where terminal text ends and UI chrome begins.
     * FX pipeline (liquid/waves/etc) only applies to quads before this split. */
    fx_split_quad = quad_count;

    /* Draw floating windows */
    vkw_draw_all(vp_w, vp_h);

    /* Draw round timer widget (with wobble) */
    if (fx_wobble_mode && vrt_visible) {
        { float ssz = vrt_scaled_size();
        wob_move_rest(&wob_grid_rt, vrt_x, vrt_y, ssz, ssz); }
        if (vrt_dragging) {
            wob_pin_top(&wob_grid_rt);
            wob_grid_rt.active = 1;
        } else {
            wob_unpin(&wob_grid_rt);
        }
        wob_simulate(&wob_grid_rt);
        if (wob_grid_rt.active) {
            wob_warp_active = 1;
            wob_warp_grid = &wob_grid_rt;
            { float wssz = vrt_scaled_size();
            wob_warp_x = vrt_x;
            wob_warp_y = vrt_y;
            wob_warp_w = wssz;
            wob_warp_h = wssz; }
        }
    }
    vrt_draw(vp_w, vp_h);

    /* ---- PTT Voice Overlay (mini oscilloscope) ---- */
    {
        vox_resolve();
        int ptt_on = (vox_resolved == 1 && vox_active_ptr && *vox_active_ptr);
        if (ptt_on) vox_fade = 1.0f;
        else if (vox_fade > 0.0f) vox_fade -= 0.012f; /* ~1.3s fade-out */

        if (vox_fade > 0.01f) {
            const ui_theme_t *vt = &ui_themes[current_theme];
            float alpha = vox_fade;
            /* Position: upper-right corner */
            float bw = 200, bh = 60;
            float bx = (float)vp_w - bw - 20;
            float by = 20;

            /* Background */
            push_solid((int)bx, (int)by, (int)(bx + bw), (int)(by + bh),
                       vt->bg[0] * 0.5f, vt->bg[1] * 0.5f, vt->bg[2] * 0.5f,
                       0.8f * alpha, vp_w, vp_h);
            /* Border */
            float bc = ptt_on ? 0.3f : 0.15f;
            push_solid((int)bx, (int)by, (int)(bx + bw), (int)(by + 1),
                       0.2f, 0.8f, 0.3f, bc * alpha, vp_w, vp_h);
            push_solid((int)bx, (int)(by + bh - 1), (int)(bx + bw), (int)(by + bh),
                       0.2f, 0.8f, 0.3f, bc * alpha, vp_w, vp_h);
            push_solid((int)bx, (int)by, (int)(bx + 1), (int)(by + bh),
                       0.2f, 0.8f, 0.3f, bc * alpha, vp_w, vp_h);
            push_solid((int)(bx + bw - 1), (int)by, (int)(bx + bw), (int)(by + bh),
                       0.2f, 0.8f, 0.3f, bc * alpha, vp_w, vp_h);

            /* Label */
            if (ptt_on) {
                push_text((int)(bx + 4), (int)(by + 2), "PTT",
                          0.3f, 1.0f, 0.4f, vp_w, vp_h, 7, 14);
            } else {
                push_text((int)(bx + 4), (int)(by + 2), "PTT",
                          0.5f, 0.5f, 0.5f, vp_w, vp_h, 7, 14);
            }

            /* Audio level bar */
            float level = (vox_level_ptr && ptt_on) ? *vox_level_ptr : 0.0f;
            if (level > 1.0f) level = 1.0f;
            int bar_x = (int)(bx + 30);
            int bar_w = (int)(bw - 36);
            int bar_y = (int)(by + 4);
            int bar_h = 10;
            /* BG */
            push_solid(bar_x, bar_y, bar_x + bar_w, bar_y + bar_h,
                       0.1f, 0.1f, 0.1f, 0.6f * alpha, vp_w, vp_h);
            /* Level fill — green to yellow to red */
            int fill_w = (int)(level * bar_w);
            if (fill_w > 0) {
                float lr = level > 0.7f ? 1.0f : level * 1.4f;
                float lg = level < 0.5f ? 0.9f : 0.9f * (1.0f - (level - 0.5f) * 2.0f);
                push_solid(bar_x, bar_y, bar_x + fill_w, bar_y + bar_h,
                           lr, lg, 0.1f, 0.8f * alpha, vp_w, vp_h);
            }

            /* Waveform oscilloscope */
            if (vox_wave_ptr) {
                float wave_y = by + 18;
                float wave_h = bh - 22;
                float wave_mid = wave_y + wave_h / 2.0f;
                int wsamples = 128;
                float step = bw / (float)wsamples;
                for (int i = 0; i < wsamples - 1; i++) {
                    int wi = (vox_wave_pos_ptr ? (*vox_wave_pos_ptr - wsamples + i) : i);
                    if (wi < 0) wi += 128;
                    wi = wi % 128;
                    int wi2 = (wi + 1) % 128;
                    float v1 = vox_wave_ptr[wi];
                    float v2 = vox_wave_ptr[wi2];
                    float y1 = wave_mid - v1 * wave_h * 0.45f;
                    float y2 = wave_mid - v2 * wave_h * 0.45f;
                    float x1 = bx + 2 + step * i;
                    float x2 = bx + 2 + step * (i + 1);
                    /* Draw as thin solid line (1px height quad) */
                    push_solid((int)x1, (int)y1, (int)x2 + 1, (int)y1 + 1,
                               0.2f, 0.9f, 0.3f, 0.7f * alpha, vp_w, vp_h);
                }
                /* Center line */
                push_solid((int)(bx + 2), (int)wave_mid, (int)(bx + bw - 2), (int)wave_mid + 1,
                           0.3f, 0.3f, 0.3f, 0.3f * alpha, vp_w, vp_h);
            }

            /* Show last recognized text (fades after 3s) */
            if (vox_text_ptr && vox_text_tick_ptr && !ptt_on) {
                DWORD age = GetTickCount() - *vox_text_tick_ptr;
                if (age < 4000 && vox_text_ptr[0]) {
                    float ta = alpha * (age < 3000 ? 1.0f : 1.0f - (float)(age - 3000) / 1000.0f);
                    char tbuf[48];
                    _snprintf(tbuf, sizeof(tbuf), "%.40s", vox_text_ptr);
                    push_text((int)(bx + 4), (int)(by + bh + 2), tbuf,
                              0.8f, 1.0f, 0.8f, vp_w, vp_h, 7, 14);
                }
            }
        }
    }
    wob_warp_active = 0;
    wob_warp_grid = NULL;

    /* Draw UI-font elements (status bar + P&L window) — tracked separately */
    pl_quad_start = quad_count;
    scm_update_vsb_scripts();
    vsb_draw(vp_w, vp_h);
    VIB_BAR_H = (int)(VIB_BAR_H_BASE * ui_scale);
    vib_cur_bar_y = vib_update_slide(vkm_mouse_y, vp_w);
    vib_draw(vp_w, vp_h, vib_cur_bar_y);
    vxb_draw(vp_w, vp_h);
    pl_draw(vp_w, vp_h);
    pst_draw(vp_w, vp_h);
    /* Live current-room tracking — use the map/room parsed from the most
     * recent `rm` command output (msimg32 maintains loc_map / loc_room).
     * Checksums are NOT unique per room, so matching by checksum silently
     * teleported the marker to dup rooms. Gated by [x] use rm. */
    if (mdw_use_rm && !mdw_path_focus) {
        unsigned int packed = get_location_safe();
        if (packed) {
            int map = (int)((packed >> 16) & 0xFFFF);
            int room = (int)(packed & 0xFFFF);
            if (room > 0) mdw_set_current(map, room);
        }
    }
    mdw_draw(vp_w, vp_h);
    stw_draw(vp_w, vp_h, 0, 0, 0);  /* draw only, clicks handled in WM_LBUTTONDOWN */
    bsp_draw(vp_w, vp_h);
    mr_draw(vp_w, vp_h);
    scm_draw(vp_w, vp_h);
    /* Settings panels — draw focused one last (on top) */
    {
        void (*sp_draws[])(int,int) = { clr_draw, bgp_draw, smk_draw, shd_draw, snd_draw, fnt_draw, rn_draw };
        int sp_count = 7;
        for (int i = 0; i < sp_count; i++)
            if (i != sp_focus) sp_draws[i](vp_w, vp_h);
        if (sp_focus >= 0 && sp_focus < sp_count)
            sp_draws[sp_focus](vp_w, vp_h);
    }
    pl_quad_end = quad_count;

    /* Draw context menu on top of UI chrome */
    vkm_draw(vp_w, vp_h);

    /* File picker sits above every panel & context menu — modal */
    fp_draw(vp_w, vp_h);

    /* QR pairing overlay — modal, on top of everything except floating text */
    qr_draw(vp_w, vp_h);

    /* Floating text on top of EVERYTHING (last = topmost, uses ui font atlas) */
    vft_quad_start = quad_count;
    vft_draw(vp_w, vp_h);
    vft_quad_end = quad_count;
}

/* ---- Input handling ---- */

/* Send a string directly to MMANSI (for numpad, direct mode, etc) */
static void input_send_raw(const char *str)
{
    if (!mmansi_hwnd || !str) return;
    for (int i = 0; str[i]; i++)
        PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)(unsigned char)str[i], 0);
    PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)'\r', 0);
}

/* Numpad direction handler — returns 1 if handled.
 * NumLock must be on. Ctrl=pick, Alt=disarm trap, Shift=bash.
 * Mixed modifiers = ignore.
 *
 * When Map Walker is focused, numpad routes to map pan/zoom instead of
 * being sent to the MUD. */
static int numpad_handle(WPARAM vk, int ctrl, int alt, int shift)
{
    /* Map Walker steals numpad while focused (numlock still required). */
    if (mdw_key_down((unsigned int)vk)) { mdw_swallow_char = 1; return 1; }
    /* Check NumLock state */
    if (!(GetKeyState(VK_NUMLOCK) & 1)) return 0;

    const char *dir = NULL;
    switch (vk) {
    case VK_NUMPAD8: dir = "n"; break;
    case VK_NUMPAD2: dir = "s"; break;
    case VK_NUMPAD6: dir = "e"; break;
    case VK_NUMPAD4: dir = "w"; break;
    case VK_NUMPAD9: dir = "ne"; break;
    case VK_NUMPAD7: dir = "nw"; break;
    case VK_NUMPAD3: dir = "se"; break;
    case VK_NUMPAD1: dir = "sw"; break;
    case VK_NUMPAD0: case VK_INSERT: dir = "u"; break;
    case VK_DECIMAL: case VK_DELETE:  dir = "d"; break;
    default: return 0;
    }

    /* Reject mixed modifiers — consume so WM_CHAR eats the digit either way */
    int mods = (ctrl ? 1 : 0) + (alt ? 1 : 0) + (shift ? 1 : 0);
    if (mods > 1) { mdw_swallow_char = 1; return 1; }

    char cmd[64];
    if (ctrl)
        _snprintf(cmd, sizeof(cmd), "pick %s", dir);
    else if (alt)
        _snprintf(cmd, sizeof(cmd), "disarm trap %s", dir);
    else if (shift)
        _snprintf(cmd, sizeof(cmd), "bash %s", dir);
    else
        _snprintf(cmd, sizeof(cmd), "%s", dir);
    cmd[sizeof(cmd) - 1] = '\0';

    input_send_raw(cmd);
    /* If "rm every step" is on AND this is a plain walk (no pick/disarm/bash),
     * chase with "rm" so the Location: line refreshes on every step. */
    if (!ctrl && !alt && !shift && rm_every_step_get_safe()) {
        rm_bridge_resolve();
        if (pfn_auto_rm_queue_increment) pfn_auto_rm_queue_increment();
        input_send_raw("rm");
    }
    /* Swallow the WM_CHAR that TranslateMessage will generate — otherwise
     * "8" also lands in the terminal input box while we're also sending "n"
     * to the MUD. */
    mdw_swallow_char = 1;
    return 1;
}

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

    /* Capture outgoing telepath from main input: /name message or / name message */
    if (input_buf[0] == '/') {
        const char *p = input_buf + 1;
        if (*p == ' ') p++; /* skip optional space after / */
        /* Skip the abbreviated name */
        while (*p && *p != ' ') p++;
        if (*p == ' ') {
            p++; /* skip space between name and message */
            if (*p) {
                strncpy(tell_pending_msg, p, sizeof(tell_pending_msg) - 1);
                tell_pending_msg[sizeof(tell_pending_msg) - 1] = 0;
                tell_pending = 1;
                tell_lines_waited = 0;
            }
        }
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

    /* Instance — probe for VK_EXT_swapchain_colorspace for HDR support */
    {
        typedef VkResult (VKAPI_CALL *PFN_vkEnumInstExtProps)(const char*, uint32_t*, VkExtensionProperties*);
        HMODULE vkmod = GetModuleHandleA("vulkan-1.dll");
        if (!vkmod) vkmod = LoadLibraryA("vulkan-1.dll");
        PFN_vkEnumInstExtProps pfnEnum = vkmod ?
            (PFN_vkEnumInstExtProps)GetProcAddress(vkmod, "vkEnumerateInstanceExtensionProperties") : NULL;
        if (pfnEnum) {
            uint32_t ext_count = 0;
            pfnEnum(NULL, &ext_count, NULL);
            if (ext_count > 0) {
                VkExtensionProperties *exts = (VkExtensionProperties *)malloc(ext_count * sizeof(VkExtensionProperties));
                pfnEnum(NULL, &ext_count, exts);
                for (uint32_t i = 0; i < ext_count; i++) {
                    if (strcmp(exts[i].extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
                        hdr_ext_available = 1;
                        break;
                    }
                }
                free(exts);
            }
        }
    }
    const char *inst_exts_hdr[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                     VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME };
    const char *inst_exts_std[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VkTerminal";
    app.apiVersion = VK_API_VERSION_1_0;
    ici.pApplicationInfo = &app;
    if (hdr_ext_available) {
        ici.enabledExtensionCount = 3;
        ici.ppEnabledExtensionNames = inst_exts_hdr;
        api->log("[vk_terminal] VK_EXT_swapchain_colorspace available — enabling\n");
    } else {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = inst_exts_std;
        api->log("[vk_terminal] VK_EXT_swapchain_colorspace not available\n");
    }
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

/* Load a TTF file from the fonts/ directory next to the DLL.
 * Returns malloc'd buffer, sets *out_size. NULL on failure. */
static unsigned char *vkt_load_ttf_file(int font_idx, int *out_size)
{
    if (font_idx < 0 || font_idx >= NUM_TTF_FONTS) return NULL;
    if (!ttf_fonts[font_idx].filename) return NULL; /* category separator */
    if (ttf_loaded[font_idx]) {
        *out_size = 0; /* size not tracked, stb doesn't need it */
        return ttf_file_data[font_idx];
    }
    /* Build path: C:\MegaMUD\plugins\fonts\<filename> */
    char path[512];
    wsprintfA(path, "C:\\MegaMUD\\plugins\\fonts\\%s", ttf_fonts[font_idx].filename);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (api) api->log("[vk_terminal] TTF not found: %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    int sz = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char *)malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);
    ttf_file_data[font_idx] = data;
    ttf_loaded[font_idx] = 1;
    *out_size = sz;
    if (api) api->log("[vk_terminal] Loaded TTF: %s (%d bytes)\n",
                      ttf_fonts[font_idx].filename, sz);
    return data;
}

/* Build RGBA font atlas pixels.
 * font_idx: -1 = CP437 bitmap, 0+ = TTF font.
 * Caller must free() the returned buffer. Sets *out_w, *out_h. */
static uint8_t *vkt_build_font_pixels(int font_idx, uint32_t *out_w, uint32_t *out_h)
{
    if (font_idx >= 0 && font_idx < NUM_TTF_FONTS) {
        /* ---- TTF font via stb_truetype ---- */
        int ttf_size;
        unsigned char *ttf_data = vkt_load_ttf_file(font_idx, &ttf_size);
        if (!ttf_data) goto fallback_bitmap;

        stbtt_fontinfo font;
        if (!stbtt_InitFont(&font, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
            if (api) api->log("[vk_terminal] stbtt_InitFont failed for %s\n",
                              ttf_fonts[font_idx].filename);
            goto fallback_bitmap;
        }

        /* Compute cell size based on screen resolution.
         * At 4K: cell_h ~= 80px, at 1080p: cell_h ~= 40px.
         * Uses Vulkan surface extent for physical pixels. */
        int ref_h = (vk_sc_extent.width > 0) ? (int)vk_sc_extent.height : fs_height;
        int cell_h = ref_h / (TERM_ROWS + 2) + term_font_adj;
        if (cell_h < 12) cell_h = 12;
        if (cell_h > 128) cell_h = 128;  /* support up to 4K+ */
        int cell_w = (cell_h + 1) / 2; /* 1:2 aspect, round up */

        /* Atlas: 16x16 grid of cells */
        uint32_t aw = (uint32_t)(cell_w * 16);
        uint32_t ah = (uint32_t)(cell_h * 16);
        uint8_t *pixels = (uint8_t *)calloc(aw * ah * 4, 1);
        if (!pixels) goto fallback_bitmap;

        /* Get font metrics for vertical alignment */
        float scale = stbtt_ScaleForPixelHeight(&font, (float)cell_h);
        int ascent, descent, lineGap;
        stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
        int baseline = (int)(ascent * scale);

        for (int cp437 = 0; cp437 < 256; cp437++) {
            /* Skip box-drawing range — those are drawn programmatically.
             * Exception: glyph 219 (█ full block) MUST be in the atlas because
             * push_solid() samples it for all solid fills (window bg, borders, etc) */
            if (cp437 >= 0xB0 && cp437 <= 0xDF && cp437 != 0xDB) continue;

            unsigned int unicode = cp437_to_unicode[cp437];
            if (unicode == 0 && cp437 != 0) continue;
            if (unicode == 0x20) continue; /* skip space */

            int glyph = stbtt_FindGlyphIndex(&font, unicode);
            if (glyph == 0 && unicode != 0) continue; /* glyph not in font */

            /* Render glyph to temp bitmap */
            int gw = 0, gh = 0, xoff = 0, yoff = 0;
            unsigned char *gbmp = stbtt_GetGlyphBitmap(&font, scale, scale,
                                                        glyph, &gw, &gh, &xoff, &yoff);
            if (!gbmp || gw <= 0 || gh <= 0) {
                if (gbmp) stbtt_FreeBitmap(gbmp, NULL);
                continue;
            }
            /* Safety: skip absurdly large glyphs (some Unicode symbols) */
            if (gw > cell_w * 4 || gh > cell_h * 4) {
                stbtt_FreeBitmap(gbmp, NULL);
                continue;
            }

            /* Grid position in atlas */
            int grid_x = (cp437 % 16) * cell_w;
            int grid_y = (cp437 / 16) * cell_h;

            /* Center glyph horizontally in cell */
            int adv, lsb;
            stbtt_GetGlyphHMetrics(&font, glyph, &adv, &lsb);
            int glyph_adv = (int)(adv * scale);
            int x0 = (cell_w - glyph_adv) / 2 + (int)(lsb * scale);
            int y0 = baseline + yoff;

            /* Blit glyph into atlas, clamped to cell AND atlas bounds */
            for (int row = 0; row < gh; row++) {
                for (int col = 0; col < gw; col++) {
                    int px = grid_x + x0 + col;
                    int py = grid_y + y0 + row;
                    /* Clamp to cell bounds */
                    if (px < grid_x || px >= grid_x + cell_w) continue;
                    if (py < grid_y || py >= grid_y + cell_h) continue;
                    /* Clamp to atlas bounds */
                    if (px < 0 || (uint32_t)px >= aw || py < 0 || (uint32_t)py >= ah) continue;
                    uint8_t a = gbmp[row * gw + col];
                    if (a == 0) continue;
                    int idx = (py * (int)aw + px) * 4;
                    pixels[idx+0] = 255;
                    pixels[idx+1] = 255;
                    pixels[idx+2] = 255;
                    pixels[idx+3] = a;
                }
            }
            stbtt_FreeBitmap(gbmp, NULL);
        }

        /* Ensure glyph 219 (█ full block) is solid white — push_solid() depends on it.
         * Some TTF fonts don't have this glyph, so fill the cell manually. */
        {
            int gx = (0xDB % 16) * cell_w;
            int gy = (0xDB / 16) * cell_h;
            /* Check if any alpha was written */
            int has_alpha = 0;
            for (int y = gy; y < gy + cell_h && !has_alpha; y++)
                for (int x = gx; x < gx + cell_w && !has_alpha; x++)
                    if (pixels[(y * (int)aw + x) * 4 + 3] > 0) has_alpha = 1;
            if (!has_alpha) {
                /* Fill entire cell with solid white alpha=255 */
                for (int y = gy; y < gy + cell_h; y++)
                    for (int x = gx; x < gx + cell_w; x++) {
                        int idx = (y * (int)aw + x) * 4;
                        pixels[idx+0] = 255; pixels[idx+1] = 255;
                        pixels[idx+2] = 255; pixels[idx+3] = 255;
                    }
            }
        }

        /* ---- Light antialias on alpha channel ----
         * 3x3 separable kernel (1,2,1) / sigma ~0.5 — just smooths
         * staircase aliasing without blurring glyph detail.
         * Applied per-cell to prevent cross-glyph bleed. */
        {
            uint8_t *tmp = (uint8_t *)calloc(aw * ah, 1);
            if (tmp) {
                /* Extract alpha channel */
                for (uint32_t i = 0; i < aw * ah; i++)
                    tmp[i] = pixels[i * 4 + 3];

                /* 3x3 separable kernel: 1,2,1 (sum=4 per pass, 16 total) */
                /* Separable: horizontal pass */
                uint8_t *h_buf = (uint8_t *)calloc(aw * ah, 1);
                if (h_buf) {
                    for (int cp = 0; cp < 256; cp++) {
                        if (cp >= 0xB0 && cp <= 0xDF) continue; /* skip box-drawing cells */
                        int gx = (cp % 16) * cell_w;
                        int gy = (cp / 16) * cell_h;
                        for (int y = gy + 1; y < gy + cell_h - 1; y++) {
                            for (int x = gx + 1; x < gx + cell_w - 1; x++) {
                                int xl = x - 1 < gx ? gx : x - 1;
                                int xr = x + 1 >= gx + cell_w ? gx + cell_w - 1 : x + 1;
                                int sum = tmp[y * aw + xl] + 2 * tmp[y * aw + x] + tmp[y * aw + xr];
                                h_buf[y * aw + x] = (uint8_t)(sum / 4);
                            }
                        }
                    }
                    /* Vertical pass */
                    for (int cp = 0; cp < 256; cp++) {
                        if (cp >= 0xB0 && cp <= 0xDF) continue;
                        int gx = (cp % 16) * cell_w;
                        int gy = (cp / 16) * cell_h;
                        for (int y = gy + 1; y < gy + cell_h - 1; y++) {
                            for (int x = gx + 1; x < gx + cell_w - 1; x++) {
                                int yt = y - 1 < gy ? gy : y - 1;
                                int yb = y + 1 >= gy + cell_h ? gy + cell_h - 1 : y + 1;
                                int sum = h_buf[yt * aw + x] + 2 * h_buf[y * aw + x] + h_buf[yb * aw + x];
                                int val = sum / 4;
                                if (val > 255) val = 255;
                                pixels[(y * aw + x) * 4 + 3] = (uint8_t)val;
                            }
                        }
                    }
                    free(h_buf);
                }
                free(tmp);
            }
        }

        *out_w = aw; *out_h = ah;
        cur_atlas_w = aw; cur_atlas_h = ah;
        if (api) api->log("[vk_terminal] TTF atlas: %s %dx%d (cell %dx%d, AA)\n",
                          ttf_fonts[font_idx].name, aw, ah, cell_w, cell_h);
        return pixels;
    }

fallback_bitmap:;
    /* ---- Original CP437 bitmap ---- */
    /* 8x16 bitmap → 9x16 VGA-style cells, 4x nearest-neighbor upscale.
     * 9th column blank for most chars, duplicates col 8 for box-drawing. */
    #define FONT_SCALE 4
    #define VGA_CELL_W 9
    uint32_t aw = VGA_CELL_W * 16 * FONT_SCALE, ah = 256 * FONT_SCALE;
    uint8_t *pixels = (uint8_t *)calloc(aw * ah * 4, 1);
    for (int g = 0; g < 256; g++) {
        int gx = (g % 16) * VGA_CELL_W, gy = (g / 16) * 16;
        for (int row = 0; row < 16; row++) {
            uint8_t bits = cp437_bitmap[g][row];
            for (int col = 0; col < 9; col++) {
                int lit = 0;
                if (col < 8) {
                    lit = (bits & (0x80 >> col)) != 0;
                } else {
                    /* 9th column: duplicate col 8 for box-drawing (0xC0-0xDF) */
                    if (g >= 0xC0 && g <= 0xDF)
                        lit = (bits & 0x01) != 0;
                }
                if (lit) {
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
    cur_atlas_w = aw; cur_atlas_h = ah;
    return pixels;
    #undef VGA_CELL_W
    #undef FONT_SCALE
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

    /* LINEAR tiling + host-visible + direct memcpy — NO staging buffer, NO
     * CopyBufferToImage, NO command buffer, NO submit. lavapipe's CopyBufferToImage
     * path corrupts megamud's heap on 32-bit Win11 (~50% crash rate even with
     * every other workaround applied). Linear images are sampled directly from
     * host-mapped memory on software renderers — no transfer op needed. */
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

    VkMemoryRequirements img_mr;
    vkGetImageMemoryRequirements(vk_dev, vk_font_img, &img_mr);
    VkMemoryAllocateInfo img_mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    img_mai.allocationSize = img_mr.size;
    img_mai.memoryTypeIndex = vk_find_memory(vk_pdev, img_mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &img_mai, NULL, &vk_font_mem);
    vkBindImageMemory(vk_dev, vk_font_img, vk_font_mem, 0);

    /* Query row pitch — linear images may pad each row for alignment */
    VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sl;
    vkGetImageSubresourceLayout(vk_dev, vk_font_img, &isr, &sl);

    void *mapped = NULL;
    vkMapMemory(vk_dev, vk_font_mem, 0, VK_WHOLE_SIZE, 0, &mapped);
    uint8_t *dst = (uint8_t *)mapped + (size_t)sl.offset;
    uint32_t src_pitch = atlas_w * 4u;
    if ((VkDeviceSize)src_pitch == sl.rowPitch) {
        memcpy(dst, pixels, (size_t)src_pitch * atlas_h);
    } else {
        for (uint32_t y = 0; y < atlas_h; y++)
            memcpy(dst + (size_t)y * sl.rowPitch, pixels + (size_t)y * src_pitch, src_pitch);
    }
    vkUnmapMemory(vk_dev, vk_font_mem);

    /* View + sampler */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = vk_font_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vk_dev, &ivci, NULL, &vk_font_view);

    VkSamplerCreateInfo saci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    saci.magFilter = VK_FILTER_LINEAR;
    saci.minFilter = VK_FILTER_LINEAR;
    saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_dev, &saci, NULL, &vk_font_sampler);

    /* Image stays at PREINITIALIZED. Descriptor uses VK_IMAGE_LAYOUT_GENERAL
     * which lavapipe's software sampler accepts for linear host-visible
     * images. Strict spec compliance would require a barrier transition, but
     * that re-introduces the submit path we're trying to avoid. */
    if (vk_desc_set != VK_NULL_HANDLE) {
        VkDescriptorImageInfo dii = {0};
        dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        dii.imageView = vk_font_view;
        dii.sampler = vk_font_sampler;
        VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wds.dstSet = vk_desc_set;
        wds.dstBinding = 0;
        wds.descriptorCount = 1;
        wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wds.pImageInfo = &dii;
        vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);
    }

    return 0;
}

static int vkt_create_font_texture(void)
{
    uint32_t aw, ah;
    uint8_t *pixels = vkt_build_font_pixels(current_font, &aw, &ah);
    int ret = vkt_upload_font_texture(pixels, aw, ah);
    free(pixels);
    return ret;
}

/* Build and upload a dedicated UI font atlas (always TTF, independent of terminal font).
 * Tries each TTF font in order until one loads. Uses a fixed 28px cell height for legibility. */
static void vkt_init_ui_font(void)
{
    /* Try each TTF font in order */
    unsigned char *ttf_data = NULL;
    int font_idx = -1, ttf_size;
    for (int i = 0; i < NUM_TTF_FONTS; i++) {
        ttf_data = vkt_load_ttf_file(i, &ttf_size);
        if (ttf_data) { font_idx = i; break; }
    }
    if (!ttf_data) {
        if (api) api->log("[vk_terminal] No TTF fonts found for UI — P&L will use terminal font\n");
        return;
    }

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        if (api) api->log("[vk_terminal] stbtt_InitFont failed for UI font\n");
        return;
    }

    /* UI atlas — moderate res for menus/labels/windows. VFT has its own atlas. */
    int cell_h = 48, cell_w = 24;
    uint32_t aw = (uint32_t)(cell_w * 16), ah = (uint32_t)(cell_h * 16);
    uint8_t *pixels = (uint8_t *)calloc(aw * ah * 4, 1);
    if (!pixels) return;

    float scale = stbtt_ScaleForPixelHeight(&font, (float)cell_h);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scale);

    for (int cp = 0; cp < 256; cp++) {
        unsigned int unicode = cp437_to_unicode[cp];
        if (unicode == 0 && cp != 0) continue;

        int glyph = stbtt_FindGlyphIndex(&font, unicode);
        if (glyph == 0 && unicode != 0x20 && cp != 0) continue;

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&font, glyph, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;

        int bw, bh, bxoff, byoff;
        unsigned char *bmp = stbtt_GetGlyphBitmap(&font, scale, scale, glyph,
                                                   &bw, &bh, &bxoff, &byoff);
        if (!bmp) continue;

        int grid_x = (cp % 16) * cell_w;
        int grid_y = (cp / 16) * cell_h;

        /* Center glyph in cell */
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&font, glyph, &adv, &lsb);
        int x_off = (cell_w - (int)(adv * scale)) / 2 + (int)(lsb * scale);
        int y_off = baseline + y0;

        for (int row = 0; row < bh; row++) {
            for (int col = 0; col < bw; col++) {
                int px = grid_x + x_off + col;
                int py = grid_y + y_off + row;
                if (px <= grid_x || px >= grid_x + cell_w - 1) continue;
                if (py <= grid_y || py >= grid_y + cell_h - 1) continue;
                if (px < 0 || px >= (int)aw || py < 0 || py >= (int)ah) continue;
                pixels[(py * aw + px) * 4 + 3] = bmp[row * bw + col];
            }
        }
        stbtt_FreeBitmap(bmp, NULL);
    }

    /* Ensure glyph 219 (full block) is solid white — needed for push_solid_ui */
    {
        int g_x = (219 % 16) * cell_w, g_y = (219 / 16) * cell_h;
        for (int yy = g_y; yy < g_y + cell_h; yy++)
            for (int xx = g_x; xx < g_x + cell_w; xx++) {
                int idx = (yy * aw + xx) * 4;
                pixels[idx + 0] = 255; pixels[idx + 1] = 255;
                pixels[idx + 2] = 255; pixels[idx + 3] = 255;
            }
    }

    ui_atlas_w = aw; ui_atlas_h = ah;

    /* Create VkImage */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ aw, ah, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    vkCreateImage(vk_dev, &ici, NULL, &ui_font_img);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_dev, ui_font_img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_find_memory(vk_pdev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &mai, NULL, &ui_font_mem);
    vkBindImageMemory(vk_dev, ui_font_img, ui_font_mem, 0);

    /* Copy pixel data */
    void *mapped;
    vkMapMemory(vk_dev, ui_font_mem, 0, mr.size, 0, &mapped);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk_dev, ui_font_img, &sub, &layout);
    for (uint32_t row = 0; row < ah; row++) {
        memcpy((uint8_t *)mapped + layout.offset + row * layout.rowPitch,
               pixels + row * aw * 4, aw * 4);
    }
    vkUnmapMemory(vk_dev, ui_font_mem);
    free(pixels);

    /* NO barrier transition — image stays at PREINITIALIZED, descriptor uses
     * GENERAL. Submitting even a tiny layout-transition cmdbuf corrupts
     * megamud's heap on lavapipe/Win11. lavapipe's software sampler reads
     * linear host-visible images correctly regardless of formal layout. */

    /* Image view */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = ui_font_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vk_dev, &ivci, NULL, &ui_font_view);

    /* Sampler — LINEAR for smooth antialiased TTF */
    VkSamplerCreateInfo saci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    saci.magFilter = VK_FILTER_LINEAR;
    saci.minFilter = VK_FILTER_LINEAR;
    saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_dev, &saci, NULL, &ui_font_sampler);

    /* Update UI descriptor set */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii.imageView = ui_font_view;
    dii.sampler = ui_font_sampler;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet = ui_desc_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);

    ui_font_ready = 1;
    if (api) api->log("[vk_terminal] UI font ready: %s (%dx%d atlas)\n",
                      ttf_fonts[font_idx].name, aw, ah);
}

/* ---- VFT display font atlas — high-res, separate from UI atlas ---- */
static void vkt_init_vft_font(int font_idx)
{
    if (font_idx < 0 || font_idx >= VFT_NUM_FONTS) font_idx = 0;

    /* Load TTF file */
    char path[512];
    wsprintfA(path, "C:\\MegaMUD\\%s", vft_font_files[font_idx]);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (api) api->log("[vk_terminal] VFT font not found: %s\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    int sz = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *ttf_data = (unsigned char *)malloc(sz);
    fread(ttf_data, 1, sz, f);
    fclose(f);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0))) {
        if (api) api->log("[vk_terminal] VFT stbtt_InitFont failed: %s\n", vft_font_names[font_idx]);
        free(ttf_data);
        return;
    }

    /* High-res cells: 48x96 per glyph, 16x16 grid = 768x1536 atlas
     * ~3.5x the UI atlas (14x28) — crisp at large display sizes while
     * staying under 5MB for 32-bit process. */
    int cell_w = 48, cell_h = 96;
    uint32_t aw = (uint32_t)(cell_w * 16), ah = (uint32_t)(cell_h * 16);
    uint8_t *pixels = (uint8_t *)calloc(aw * ah * 4, 1);
    if (!pixels) { free(ttf_data); return; }

    float scale = stbtt_ScaleForPixelHeight(&font, (float)cell_h);
    int ascent, descent, lineGap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scale);

    /* Render printable ASCII (32-126) + a few useful CP437 chars */
    for (int cp = 32; cp < 127; cp++) {
        int glyph = stbtt_FindGlyphIndex(&font, cp);
        if (glyph == 0) continue;

        int x0, y0, x1, y1;
        stbtt_GetGlyphBitmapBox(&font, glyph, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;

        int bw, bh, bxoff, byoff;
        unsigned char *bmp = stbtt_GetGlyphBitmap(&font, scale, scale, glyph,
                                                   &bw, &bh, &bxoff, &byoff);
        if (!bmp) continue;

        int grid_x = (cp % 16) * cell_w;
        int grid_y = (cp / 16) * cell_h;
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&font, glyph, &adv, &lsb);
        int x_off = (cell_w - (int)(adv * scale)) / 2 + (int)(lsb * scale);
        int y_off = baseline + y0;

        for (int row = 0; row < bh; row++) {
            for (int col = 0; col < bw; col++) {
                int px = grid_x + x_off + col;
                int py = grid_y + y_off + row;
                if (px < grid_x || px >= grid_x + cell_w) continue;
                if (py < grid_y || py >= grid_y + cell_h) continue;
                pixels[(py * aw + px) * 4 + 3] = bmp[row * bw + col];
            }
        }
        stbtt_FreeBitmap(bmp, NULL);
    }

    /* Glyph 219 (full block) → solid white for particles/solids */
    {
        int g_x = (219 % 16) * cell_w, g_y = (219 / 16) * cell_h;
        for (int yy = g_y; yy < g_y + cell_h; yy++)
            for (int xx = g_x; xx < g_x + cell_w; xx++) {
                int idx = (yy * aw + xx) * 4;
                pixels[idx+0] = 255; pixels[idx+1] = 255;
                pixels[idx+2] = 255; pixels[idx+3] = 255;
            }
    }
    /* Glyph 7 (bullet •) for particles */
    {
        int g_x = (7 % 16) * cell_w, g_y = (7 / 16) * cell_h;
        float cx = g_x + cell_w * 0.5f, cy = g_y + cell_h * 0.5f;
        float r = cell_w * 0.35f;
        for (int yy = g_y; yy < g_y + cell_h; yy++)
            for (int xx = g_x; xx < g_x + cell_w; xx++) {
                float dx = xx - cx, dy = yy - cy;
                float d = sqrtf(dx*dx + dy*dy);
                if (d < r) {
                    float a = (d < r - 1.5f) ? 255.0f : (r - d) / 1.5f * 255.0f;
                    int idx = (yy * aw + xx) * 4;
                    pixels[idx+0] = 255; pixels[idx+1] = 255;
                    pixels[idx+2] = 255; pixels[idx+3] = (uint8_t)a;
                }
            }
    }

    free(ttf_data);

    /* Destroy previous VFT texture if any */
    if (vft_font_view) { vkDestroyImageView(vk_dev, vft_font_view, NULL); vft_font_view = VK_NULL_HANDLE; }
    if (vft_font_sampler) { vkDestroySampler(vk_dev, vft_font_sampler, NULL); vft_font_sampler = VK_NULL_HANDLE; }
    if (vft_font_img) { vkDestroyImage(vk_dev, vft_font_img, NULL); vft_font_img = VK_NULL_HANDLE; }
    if (vft_font_mem) { vkFreeMemory(vk_dev, vft_font_mem, NULL); vft_font_mem = VK_NULL_HANDLE; }

    vft_atlas_w = aw; vft_atlas_h = ah;

    /* Create VkImage */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ aw, ah, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    if (vkCreateImage(vk_dev, &ici, NULL, &vft_font_img) != VK_SUCCESS) {
        if (api) api->log("[vk_terminal] VFT vkCreateImage failed (%dx%d)\n", aw, ah);
        free(pixels); return;
    }

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_dev, vft_font_img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_find_memory(vk_pdev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(vk_dev, &mai, NULL, &vft_font_mem) != VK_SUCCESS) {
        if (api) api->log("[vk_terminal] VFT vkAllocateMemory failed (%llu bytes)\n",
                          (unsigned long long)mr.size);
        vkDestroyImage(vk_dev, vft_font_img, NULL); vft_font_img = VK_NULL_HANDLE;
        free(pixels); return;
    }
    vkBindImageMemory(vk_dev, vft_font_img, vft_font_mem, 0);

    /* Copy pixels */
    void *mapped;
    vkMapMemory(vk_dev, vft_font_mem, 0, mr.size, 0, &mapped);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk_dev, vft_font_img, &sub, &layout);
    for (uint32_t row = 0; row < ah; row++)
        memcpy((uint8_t *)mapped + layout.offset + row * layout.rowPitch,
               pixels + row * aw * 4, aw * 4);
    vkUnmapMemory(vk_dev, vft_font_mem);
    free(pixels);

    /* NO barrier — stays at PREINITIALIZED, descriptor uses GENERAL. See
     * vkt_init_ui_font for rationale (avoids lavapipe/Win11 heap corruption). */

    /* Image view */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = vft_font_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vk_dev, &ivci, NULL, &vft_font_view);

    /* Sampler — LINEAR for smooth scaling */
    VkSamplerCreateInfo saci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    saci.magFilter = VK_FILTER_LINEAR;
    saci.minFilter = VK_FILTER_LINEAR;
    saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_dev, &saci, NULL, &vft_font_sampler);

    /* Update VFT descriptor set */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii.imageView = vft_font_view;
    dii.sampler = vft_font_sampler;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet = vft_desc_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);

    vft_font_ready = 1;
    vft_current_font = font_idx;
    if (api) api->log("[vk_terminal] VFT font ready: %s (%dx%d atlas)\n",
                      vft_font_names[font_idx], aw, ah);
}

/* ---- Icon atlas init: upload color emoji to Vulkan texture ---- */
static void vib_init_icon_atlas(void)
{
    uint32_t aw = VIB_ICON_ATLAS_W, ah = VIB_ICON_ATLAS_H;

    /* Create VkImage */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ aw, ah, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    vkCreateImage(vk_dev, &ici, NULL, &vib_icon_img);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_dev, vib_icon_img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_find_memory(vk_pdev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &mai, NULL, &vib_icon_mem);
    vkBindImageMemory(vk_dev, vib_icon_img, vib_icon_mem, 0);

    /* Copy pixel data */
    void *mapped;
    vkMapMemory(vk_dev, vib_icon_mem, 0, mr.size, 0, &mapped);
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk_dev, vib_icon_img, &sub, &layout);
    for (uint32_t row = 0; row < ah; row++) {
        memcpy((uint8_t *)mapped + layout.offset + row * layout.rowPitch,
               vib_icon_data + row * aw * 4, aw * 4);
    }
    vkUnmapMemory(vk_dev, vib_icon_mem);

    /* NO barrier — stays at PREINITIALIZED, descriptor uses GENERAL. See
     * vkt_init_ui_font for rationale (avoids lavapipe/Win11 heap corruption). */

    /* Image view */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = vib_icon_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vk_dev, &ivci, NULL, &vib_icon_view);

    /* Sampler — LINEAR for smooth scaled emoji */
    VkSamplerCreateInfo saci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    saci.magFilter = VK_FILTER_LINEAR;
    saci.minFilter = VK_FILTER_LINEAR;
    saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_dev, &saci, NULL, &vib_icon_sampler);

    /* Update icon descriptor set */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    dii.imageView = vib_icon_view;
    dii.sampler = vib_icon_sampler;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet = vib_desc_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);

    vib_icon_ready = 1;
    if (api) api->log("[vk_terminal] Icon atlas ready: %dx%d (%d icons)\n", aw, ah, VIB_ICON_COUNT);
}

/* One-shot barrier submit: transition all 4 linear host-visible texture images
 * from PREINITIALIZED → GENERAL so fragment shader sampling is spec-legal.
 * Pure image memory barrier — no copies, no host writes — safe on lavapipe
 * (the heap corruption was specific to CopyBufferToImage's staging copy). */
static void vkt_transition_textures_to_general(void)
{
    if (!vk_dev || !vk_cmd_pool) return;

    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = vk_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk_dev, &cbai, &cb) != VK_SUCCESS || !cb) return;

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbbi);

    VkImage imgs[4] = { vk_font_img, ui_font_img, vft_font_img, vib_icon_img };
    VkImageMemoryBarrier barriers[4];
    int nb = 0;
    for (int i = 0; i < 4; i++) {
        if (!imgs[i]) continue;
        VkImageMemoryBarrier *b = &barriers[nb++];
        memset(b, 0, sizeof(*b));
        b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b->srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        b->dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b->oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
        b->newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b->image = imgs[i];
        b->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b->subresourceRange.levelCount = 1;
        b->subresourceRange.layerCount = 1;
    }
    if (nb > 0) {
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, nb, barriers);
    }
    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(vk_dev, &fci, NULL, &fence);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(vk_queue, 1, &si, fence);
    vkWaitForFences(vk_dev, 1, &fence, VK_TRUE, 1000000000ULL);
    vkDestroyFence(vk_dev, fence, NULL);
    vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &cb);
    if (api) api->log("[vk_terminal] Texture layouts transitioned to GENERAL (%d images)\n", nb);
}

/* Switch font at runtime — called ONLY from inside vkt_render_frame
 * after the fence wait, so previous frame is guaranteed complete.
 * No vkDeviceWaitIdle needed (which can deadlock under Wine FIFO present
 * when the message pump isn't running). */
/* Switch font at runtime — called from render loop after fence wait.
 * Also used for font size changes (same font_idx, different term_font_adj). */
static void vkt_switch_font(int font_idx)
{
    int size_only = (font_idx == current_font);
    current_font = font_idx;
    if (!vk_dev) return;

    uint32_t aw, ah;
    uint8_t *pixels = vkt_build_font_pixels(current_font, &aw, &ah);
    vkt_upload_font_texture(pixels, aw, ah);
    free(pixels);

    /* Clear terminal so old glyph shapes don't linger (skip for size-only changes) */
    if (!size_only) {
        EnterCriticalSection(&ansi_lock);
        ap_init(&ansi_term, TERM_ROWS, TERM_COLS);
        ap_set_scroll_cb(&ansi_term, cb_scroll_cb, NULL);
        LeaveCriticalSection(&ansi_lock);
    }

    const char *fname = (current_font < 0) ? "CP437 Bitmap (VGA)"
                       : ttf_fonts[current_font].name;
    if (api) api->log("[vk_terminal] Font: %s (adj=%d)\n", fname, term_font_adj);
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
    /* lavapipe/Win32: ensure prior submits drained before allocating descriptors. */
    if (vk_dev) vkDeviceWaitIdle(vk_dev);

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

    /* Pool — 4 sets: main font + UI font + icon atlas + VFT font */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 4;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    vkCreateDescriptorPool(vk_dev, &dpci, NULL, &vk_desc_pool);

    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = vk_desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &vk_desc_layout;
    vkAllocateDescriptorSets(vk_dev, &dsai, &vk_desc_set);
    vkAllocateDescriptorSets(vk_dev, &dsai, &ui_desc_set);
    vkAllocateDescriptorSets(vk_dev, &dsai, &vib_desc_set);
    vkAllocateDescriptorSets(vk_dev, &dsai, &vft_desc_set);

    /* Update with font texture. GENERAL matches the linear-host-visible
     * image that vkt_upload_font_texture creates (no barrier transition). */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
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

    /* Dump all surface formats for HDR debugging */
    api->log("[vk_terminal] Surface formats (%u total):\n", fmtc);
    for (uint32_t i = 0; i < fmtc; i++) {
        api->log("[vk_terminal]   [%u] format=%d colorSpace=%d\n", i, fmts[i].format, fmts[i].colorSpace);
    }

    /* Probe for HDR formats */
    hdr_available = 0;
    hdr_standard[0] = 0;
    int hdr10_idx = -1, scrgb_idx = -1, p3_idx = -1;
    for (uint32_t i = 0; i < fmtc; i++) {
        if (fmts[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            hdr10_idx = (int)i;
            if (!hdr_available) { hdr_available = 1; strncpy(hdr_standard, "HDR10 (ST2084)", sizeof(hdr_standard)); }
        }
        if (fmts[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
            scrgb_idx = (int)i;
            if (!hdr_available) { hdr_available = 1; strncpy(hdr_standard, "scRGB Linear", sizeof(hdr_standard)); }
        }
        if (fmts[i].colorSpace == VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT) {
            p3_idx = (int)i;
            if (!hdr_available && hdr10_idx < 0 && scrgb_idx < 0) { hdr_available = 1; strncpy(hdr_standard, "Display P3", sizeof(hdr_standard)); }
        }
    }
    api->log("[vk_terminal] HDR probe: hdr10=%d scrgb=%d p3=%d available=%d\n",
             hdr10_idx, scrgb_idx, p3_idx, hdr_available);

    if (hdr_enabled && hdr_available) {
        /* Select best HDR format: prefer HDR10 > scRGB > P3 */
        int best = (hdr10_idx >= 0) ? hdr10_idx : (scrgb_idx >= 0) ? scrgb_idx : p3_idx;
        if (best >= 0) {
            vk_sc_fmt = fmts[best].format;
            cs = fmts[best].colorSpace;
            api->log("[vk_terminal] HDR enabled: format=%d colorspace=%d (%s)\n",
                     vk_sc_fmt, cs, hdr_standard);
        }
    } else {
        /* Standard SDR: prefer B8G8R8A8_UNORM */
        for (uint32_t i = 0; i < fmtc; i++) {
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
                fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                vk_sc_fmt = fmts[i].format;
                cs = fmts[i].colorSpace;
                break;
            }
        }
    }
    free(fmts);

    vk_sc_extent = caps.currentExtent;
    if (vk_sc_extent.width == UINT32_MAX) {
        /* Surface doesn't have a fixed extent — use our target */
        vk_sc_extent.width = (uint32_t)fs_width;
        vk_sc_extent.height = (uint32_t)fs_height;
        /* Clamp to surface limits */
        if (vk_sc_extent.width > caps.maxImageExtent.width)
            vk_sc_extent.width = caps.maxImageExtent.width;
        if (vk_sc_extent.height > caps.maxImageExtent.height)
            vk_sc_extent.height = caps.maxImageExtent.height;
        if (vk_sc_extent.width < caps.minImageExtent.width)
            vk_sc_extent.width = caps.minImageExtent.width;
        if (vk_sc_extent.height < caps.minImageExtent.height)
            vk_sc_extent.height = caps.minImageExtent.height;
    }
    /* Update fs_width/fs_height to match actual render resolution */
    fs_width = (int)vk_sc_extent.width;
    fs_height = (int)vk_sc_extent.height;
    api->log("[vk_terminal] Surface extent: %dx%d (max: %dx%d, min: %dx%d)\n",
             vk_sc_extent.width, vk_sc_extent.height,
             caps.maxImageExtent.width, caps.maxImageExtent.height,
             caps.minImageExtent.width, caps.minImageExtent.height);

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
    sci.presentMode = vk_vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    sci.clipped = VK_TRUE;
    res = vkCreateSwapchainKHR(vk_dev, &sci, NULL, &vk_swapchain);
    if (res != VK_SUCCESS && hdr_enabled && hdr_available) {
        /* HDR swapchain failed — fall back to SDR */
        api->log("[vk_terminal] HDR swapchain failed (%d) — falling back to SDR\n", res);
        vk_sc_fmt = VK_FORMAT_B8G8R8A8_UNORM;
        cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        sci.imageFormat = vk_sc_fmt;
        sci.imageColorSpace = cs;
        hdr_available = 0;
        hdr_enabled = 0;
        strncpy(hdr_standard, "", sizeof(hdr_standard));
        res = vkCreateSwapchainKHR(vk_dev, &sci, NULL, &vk_swapchain);
    }
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

    /* ---- Liquid Letters FX pipeline (push constants for time + mode) ---- */
    {
        VkPushConstantRange pcr = {0};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(float) * 27;

        VkPipelineLayoutCreateInfo lplci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        lplci.setLayoutCount = 1;
        lplci.pSetLayouts = &vk_desc_layout;
        lplci.pushConstantRangeCount = 1;
        lplci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(vk_dev, &lplci, NULL, &vk_liquid_pipe_layout);

        VkShaderModuleCreateInfo lvsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        lvsci.codeSize = liquid_vert_spv_size;
        lvsci.pCode = liquid_vert_spv;
        VkShaderModule lvs;
        vkCreateShaderModule(vk_dev, &lvsci, NULL, &lvs);

        VkShaderModuleCreateInfo lfsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        lfsci.codeSize = liquid_frag_spv_size;
        lfsci.pCode = (const uint32_t *)liquid_frag_spv;
        VkShaderModule lfs;
        vkCreateShaderModule(vk_dev, &lfsci, NULL, &lfs);

        VkPipelineShaderStageCreateInfo lstages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, lvs, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, lfs, "main", NULL },
        };

        VkGraphicsPipelineCreateInfo lgpci = gpci; /* copy most state */
        lgpci.pStages = lstages;
        lgpci.layout = vk_liquid_pipe_layout;
        res = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &lgpci, NULL, &vk_liquid_pipeline);

        vkDestroyShaderModule(vk_dev, lvs, NULL);
        vkDestroyShaderModule(vk_dev, lfs, NULL);

        if (res != VK_SUCCESS)
            api->log("[vk_terminal] Liquid pipeline failed: %d\n", res);
        else
            api->log("[vk_terminal] Liquid Letters pipeline OK\n");
    }

    /* ---- Background Plasma pipeline ---- */
    {
        VkPushConstantRange pcr = {0};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(float) * 27; /* BG shader push constants */

        VkPipelineLayoutCreateInfo bplci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        bplci.setLayoutCount = 1;
        bplci.pSetLayouts = &vk_desc_layout;
        bplci.pushConstantRangeCount = 1;
        bplci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(vk_dev, &bplci, NULL, &vk_bg_pipe_layout);

        VkShaderModuleCreateInfo bvsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        bvsci.codeSize = bg_plasma_vert_spv_size;
        bvsci.pCode = bg_plasma_vert_spv;
        VkShaderModule bvs;
        vkCreateShaderModule(vk_dev, &bvsci, NULL, &bvs);

        VkShaderModuleCreateInfo bfsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        bfsci.codeSize = bg_plasma_frag_spv_size;
        bfsci.pCode = (const uint32_t *)bg_plasma_frag_spv;
        VkShaderModule bfs;
        vkCreateShaderModule(vk_dev, &bfsci, NULL, &bfs);

        VkPipelineShaderStageCreateInfo bstages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, bvs, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, bfs, "main", NULL },
        };

        VkGraphicsPipelineCreateInfo bgpci = gpci;
        bgpci.pStages = bstages;
        bgpci.layout = vk_bg_pipe_layout;
        res = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &bgpci, NULL, &vk_bg_pipeline);

        vkDestroyShaderModule(vk_dev, bvs, NULL);
        vkDestroyShaderModule(vk_dev, bfs, NULL);

        if (res != VK_SUCCESS)
            api->log("[vk_terminal] Background pipeline failed: %d\n", res);
        else
            api->log("[vk_terminal] Background pipeline OK\n");
    }

    /* ---- Parallax Starfield pipeline ---- */
    {
        VkPushConstantRange pcr = {0};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(float) * 29; /* starfield push constants */

        VkPipelineLayoutCreateInfo splci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        splci.setLayoutCount = 1;
        splci.pSetLayouts = &vk_desc_layout;
        splci.pushConstantRangeCount = 1;
        splci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(vk_dev, &splci, NULL, &vk_star_pipe_layout);

        VkShaderModuleCreateInfo svsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        svsci.codeSize = bg_starfield_vert_spv_size;
        svsci.pCode = bg_starfield_vert_spv;
        VkShaderModule svs;
        vkCreateShaderModule(vk_dev, &svsci, NULL, &svs);

        VkShaderModuleCreateInfo sfsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        sfsci.codeSize = bg_starfield_frag_spv_size;
        sfsci.pCode = bg_starfield_frag_spv;
        VkShaderModule sfs;
        vkCreateShaderModule(vk_dev, &sfsci, NULL, &sfs);

        VkPipelineShaderStageCreateInfo sstages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, svs, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, sfs, "main", NULL },
        };

        VkGraphicsPipelineCreateInfo sgpci = gpci;
        sgpci.pStages = sstages;
        sgpci.layout = vk_star_pipe_layout;
        res = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &sgpci, NULL, &vk_star_pipeline);

        vkDestroyShaderModule(vk_dev, svs, NULL);
        vkDestroyShaderModule(vk_dev, sfs, NULL);

        if (res != VK_SUCCESS)
            api->log("[vk_terminal] Starfield pipeline failed: %d\n", res);
        else
            api->log("[vk_terminal] Starfield pipeline OK\n");
    }

    /* ---- Alien Wall pipeline ---- */
    {
        VkPushConstantRange pcr = {0};
        pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(float) * 27;

        VkPipelineLayoutCreateInfo aplci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        aplci.setLayoutCount = 1;
        aplci.pSetLayouts = &vk_desc_layout;
        aplci.pushConstantRangeCount = 1;
        aplci.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(vk_dev, &aplci, NULL, &vk_alien_pipe_layout);

        VkShaderModuleCreateInfo avsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        avsci.codeSize = bg_plasma_vert_spv_size;
        avsci.pCode = bg_plasma_vert_spv;
        VkShaderModule avs;
        vkCreateShaderModule(vk_dev, &avsci, NULL, &avs);

        VkShaderModuleCreateInfo afsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        afsci.codeSize = bg_alienwall_frag_spv_size;
        afsci.pCode = (const uint32_t *)bg_alienwall_frag_spv;
        VkShaderModule afs;
        vkCreateShaderModule(vk_dev, &afsci, NULL, &afs);

        VkPipelineShaderStageCreateInfo astages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_VERTEX_BIT, avs, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
              VK_SHADER_STAGE_FRAGMENT_BIT, afs, "main", NULL },
        };

        VkGraphicsPipelineCreateInfo agpci = gpci;
        agpci.pStages = astages;
        agpci.layout = vk_alien_pipe_layout;
        res = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &agpci, NULL, &vk_alien_pipeline);

        vkDestroyShaderModule(vk_dev, avs, NULL);
        vkDestroyShaderModule(vk_dev, afs, NULL);

        if (res != VK_SUCCESS)
            api->log("[vk_terminal] Alien Wall pipeline failed: %d\n", res);
        else
            api->log("[vk_terminal] Alien Wall pipeline OK\n");
    }
    api->log("[vk_terminal] Swapchain %dx%d, %d images\n",
             vk_sc_extent.width, vk_sc_extent.height, vk_sc_count);
    return 0;
}

static void vkt_destroy_swapchain(void)
{
    vkDeviceWaitIdle(vk_dev);
    if (vk_pipeline) { vkDestroyPipeline(vk_dev, vk_pipeline, NULL); vk_pipeline = VK_NULL_HANDLE; }
    if (vk_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_pipe_layout, NULL); vk_pipe_layout = VK_NULL_HANDLE; }
    if (vk_liquid_pipeline) { vkDestroyPipeline(vk_dev, vk_liquid_pipeline, NULL); vk_liquid_pipeline = VK_NULL_HANDLE; }
    if (vk_liquid_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_liquid_pipe_layout, NULL); vk_liquid_pipe_layout = VK_NULL_HANDLE; }
    if (vk_bg_pipeline) { vkDestroyPipeline(vk_dev, vk_bg_pipeline, NULL); vk_bg_pipeline = VK_NULL_HANDLE; }
    if (vk_bg_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_bg_pipe_layout, NULL); vk_bg_pipe_layout = VK_NULL_HANDLE; }
    if (vk_star_pipeline) { vkDestroyPipeline(vk_dev, vk_star_pipeline, NULL); vk_star_pipeline = VK_NULL_HANDLE; }
    if (vk_star_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_star_pipe_layout, NULL); vk_star_pipe_layout = VK_NULL_HANDLE; }
    if (vk_alien_pipeline) { vkDestroyPipeline(vk_dev, vk_alien_pipeline, NULL); vk_alien_pipeline = VK_NULL_HANDLE; }
    if (vk_alien_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_alien_pipe_layout, NULL); vk_alien_pipe_layout = VK_NULL_HANDLE; }
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

static void vkt_recreate_swapchain(void) {
    vkt_destroy_swapchain();
    vkt_create_swapchain();
    /* Font atlas must be rebuilt at the new resolution's cell size.
     * Without this, switching to 4K stretches 1080p-sized glyphs → blurry zoom. */
    pending_font_idx = current_font;
    font_change_pending = 1;
}

/* ---- Render frame ---- */

static void vkt_render_frame(void)
{
    vkWaitForFences(vk_dev, 1, &vk_fence, VK_TRUE, UINT64_MAX);

    /* Font switch here — fence waited, previous frame fully complete */
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

    vkt_read_buffer();
    vkt_build_vertices();

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

    int pp_active = (pp_brightness != 0.0f || pp_contrast != 1.0f || pp_hue != 0.0f || pp_saturation != 1.0f);
    int fx_active = (fx_liquid_mode || fx_waves_mode || fx_scanline_mode || fx_fbm_mode || fx_sobel_mode || fx_smoky_mode || fx_shadow_mode || fx_rain_mode || fx_crawl_mode || pp_active) && vk_liquid_pipeline;
    /* Bitmap-style fonts use the liquid pipeline but the shader guards
     * neighbor-sampling passes (shadow blur, liquid letters, dark outline,
     * sobel) via the is_pixel_font push constant to prevent artifact lines. */

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(vk_cmd_buf, 0, 1, &vk_vbuf, &offset);
    vkCmdBindIndexBuffer(vk_cmd_buf, vk_ibuf, 0, VK_INDEX_TYPE_UINT16);

    /* Draw 0: Animated background (if active) */
    int term_start = 0;
    if (bg_mode == BG_PLASMA && bg_quad_end > 0 && vk_bg_pipeline) {
        vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_bg_pipeline);
        vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_bg_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
        /* Compute beat-reactive modifiers from mr_beat_snap */
        float beat_hue = 0.0f, beat_bright = 0.0f, beat_zoom = 0.0f;
        {
            EnterCriticalSection(&mr_beat_lock);
            float bass = mr_beat_snap.bass_energy;
            float mid  = mr_beat_snap.mid_energy;
            float treb = mr_beat_snap.treble_energy;
            float rms  = (bass + mid + treb) * 0.33f;
            LeaveCriticalSection(&mr_beat_lock);
            struct { int mode; float energy; } bands[4] = {
                { bg_plasma_beat_bass, bass }, { bg_plasma_beat_mid, mid },
                { bg_plasma_beat_treble, treb }, { bg_plasma_beat_rms, rms }
            };
            for (int i = 0; i < 4; i++) {
                float e = bands[i].energy;
                /* Retuned coefficients: hue/bright stronger, zoom gentler */
                if (bands[i].mode == BGP_BEAT_HUE)      beat_hue    += e * 140.0f;
                if (bands[i].mode == BGP_BEAT_BRIGHT)   beat_bright += e * 1.1f;
                if (bands[i].mode == BGP_BEAT_ZOOM)     beat_zoom   += e * 0.12f;
                /* Additional modes folded into existing push constants so
                 * plasma still reacts meaningfully without a shader change. */
                if (bands[i].mode == BGP_BEAT_PULSE)    beat_zoom   += e * 0.08f;
                if (bands[i].mode == BGP_BEAT_JITTER)   beat_hue    += e * 40.0f;
                if (bands[i].mode == BGP_BEAT_WARP)     beat_bright += e * 0.4f;
                if (bands[i].mode == BGP_BEAT_TWINKLE)  beat_bright += e * 0.6f;
                if (bands[i].mode == BGP_BEAT_DRIFT)    beat_hue    += e * 80.0f;
                if (bands[i].mode == BGP_BEAT_WAVE)     beat_zoom   += e * 0.05f;
            }
        }
        float bg_pc[21] = { fx_time, bg_plasma_speed, bg_plasma_turbulence,
                            bg_plasma_complexity, bg_plasma_hue, bg_plasma_saturation,
                            bg_plasma_brightness, bg_plasma_alpha,
                            (float)vk_sc_extent.width, (float)vk_sc_extent.height,
                            bg_plasma_sobel, bg_plasma_sobel_angle,
                            bg_plasma_edge_glow, bg_plasma_sharpen,
                            beat_hue, beat_bright, beat_zoom,
                            (float)bg_plasma_material, bg_plasma_mat_str,
                            (float)bg_tonemap_mode, bg_tonemap_exposure };
        vkCmdPushConstants(vk_cmd_buf, vk_bg_pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(bg_pc), bg_pc);
        vkCmdDrawIndexed(vk_cmd_buf, bg_quad_end * 6, 1, 0, 0, 0);
        term_start = bg_quad_end;
    } else if (bg_mode == BG_STARFIELD && bg_quad_end > 0 && vk_star_pipeline) {
        vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_star_pipeline);
        vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_star_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
        /* Evaluate per-band beat modes into 9 starfield effect channels. */
        float bp_pulse=0, bp_jitter=0, bp_bright=0, bp_hue=0, bp_zoom=0,
              bp_warp=0, bp_twinkle=0, bp_drift=0, bp_wave=0;
        {
            EnterCriticalSection(&mr_beat_lock);
            float bass = mr_beat_snap.bass_energy;
            float mid  = mr_beat_snap.mid_energy;
            float treb = mr_beat_snap.treble_energy;
            float rms  = (bass + mid + treb) * 0.33f;
            LeaveCriticalSection(&mr_beat_lock);
            struct { int mode; float energy; } bands[4] = {
                { bg_star_beat_bass, bass }, { bg_star_beat_mid, mid },
                { bg_star_beat_treble, treb }, { bg_star_beat_rms, rms }
            };
            for (int i = 0; i < 4; i++) {
                float e = bands[i].energy;
                switch (bands[i].mode) {
                    case BGP_BEAT_HUE:     bp_hue     += e * 1.0f; break;
                    case BGP_BEAT_BRIGHT:  bp_bright  += e * 1.2f; break;
                    case BGP_BEAT_ZOOM:    bp_zoom    += e * 1.0f; break;
                    case BGP_BEAT_PULSE:   bp_pulse   += e * 1.3f; break;
                    case BGP_BEAT_JITTER:  bp_jitter  += e * 1.0f; break;
                    case BGP_BEAT_WARP:    bp_warp    += e * 1.0f; break;
                    case BGP_BEAT_TWINKLE: bp_twinkle += e * 1.1f; break;
                    case BGP_BEAT_DRIFT:   bp_drift   += e * 1.0f; break;
                    case BGP_BEAT_WAVE:    bp_wave    += e * 1.0f; break;
                    default: break;
                }
            }
        }
        float sp_pc[29] = {
            fx_time,
            (float)vk_sc_extent.width, (float)vk_sc_extent.height,
            bg_star_layer_count, bg_star_parallax,
            bg_star_drift_x, bg_star_drift_y, bg_star_drift_speed,
            bg_star_density, bg_star_size, bg_star_brightness,
            bg_star_saturation, bg_star_hue, bg_star_bg_glow,
            bg_star_twinkle_speed, bg_star_twinkle_amt,
            bp_pulse, bp_jitter, bp_bright, bp_hue,
            bp_zoom, bp_warp, bp_twinkle, bp_drift, bp_wave,
            (float)bg_star_camera_mode, bg_star_swerve_amt,
            0.0f, 0.0f
        };
        vkCmdPushConstants(vk_cmd_buf, vk_star_pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(sp_pc), sp_pc);
        vkCmdDrawIndexed(vk_cmd_buf, bg_quad_end * 6, 1, 0, 0, 0);
        term_start = bg_quad_end;
    } else if (bg_mode == BG_ALIENWALL && bg_quad_end > 0 && vk_alien_pipeline) {
        vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_alien_pipeline);
        vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_alien_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
        float ab_hue = 0.0f, ab_bright = 0.0f, ab_zoom = 0.0f;
        {
            EnterCriticalSection(&mr_beat_lock);
            float bass = mr_beat_snap.bass_energy;
            float mid  = mr_beat_snap.mid_energy;
            float treb = mr_beat_snap.treble_energy;
            float rms  = (bass + mid + treb) * 0.33f;
            LeaveCriticalSection(&mr_beat_lock);
            struct { int mode; float energy; } bands[4] = {
                { bg_alien_beat_bass, bass }, { bg_alien_beat_mid, mid },
                { bg_alien_beat_treble, treb }, { bg_alien_beat_rms, rms }
            };
            for (int i = 0; i < 4; i++) {
                float e = bands[i].energy;
                if (bands[i].mode == BGP_BEAT_HUE)      ab_hue    += e * 140.0f;
                if (bands[i].mode == BGP_BEAT_BRIGHT)   ab_bright += e * 1.1f;
                if (bands[i].mode == BGP_BEAT_ZOOM)     ab_zoom   += e * 0.12f;
                if (bands[i].mode == BGP_BEAT_PULSE)    ab_zoom   += e * 0.08f;
                if (bands[i].mode == BGP_BEAT_JITTER)   ab_hue    += e * 40.0f;
                if (bands[i].mode == BGP_BEAT_WARP)     ab_bright += e * 0.4f;
                if (bands[i].mode == BGP_BEAT_TWINKLE)  ab_bright += e * 0.6f;
                if (bands[i].mode == BGP_BEAT_DRIFT)    ab_hue    += e * 80.0f;
                if (bands[i].mode == BGP_BEAT_WAVE)     ab_zoom   += e * 0.05f;
            }
        }
        float aw_pc[27] = {
            fx_time, bg_alien_speed, bg_alien_cell_scale, bg_alien_warp,
            bg_alien_hue, bg_alien_saturation, bg_alien_brightness, bg_alien_alpha,
            (float)vk_sc_extent.width, (float)vk_sc_extent.height,
            bg_alien_vein, bg_alien_pulse_speed, bg_alien_glow, bg_alien_depth_layers,
            ab_hue, ab_bright, ab_zoom,
            bg_alien_roughness, bg_alien_membrane,
            (float)bg_tonemap_mode, bg_tonemap_exposure,
            bg_alien_wet, bg_alien_fluid_speed, bg_alien_specular,
            bg_alien_cell_move, bg_alien_bulge, (float)bg_alien_cell_style
        };
        vkCmdPushConstants(vk_cmd_buf, vk_alien_pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(aw_pc), aw_pc);
        vkCmdDrawIndexed(vk_cmd_buf, bg_quad_end * 6, 1, 0, 0, 0);
        term_start = bg_quad_end;
    }

    if (fx_active && fx_split_quad > term_start) {
        /* Draw 1: Terminal text with FX pipeline */
        vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_liquid_pipeline);
        vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_liquid_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
        float pc_data[28] = { fx_time,
                              fx_liquid_mode ? 1.0f : 0.0f,
                              fx_waves_mode ? 1.0f : 0.0f,
                              fx_scanline_mode ? 1.0f : 0.0f,
                              fx_fbm_mode ? 1.0f : 0.0f,
                              fx_sobel_mode ? 1.0f : 0.0f,
                              pp_brightness, pp_contrast, pp_hue, pp_saturation,
                              fx_smoky_mode ? 1.0f : 0.0f,
                              smoke_decay, smoke_depth, smoke_zoom,
                              smoke_hue, smoke_saturation, smoke_value,
                              last_cw_px, last_ch_px,
                              fx_shadow_mode ? shadow_opacity : 0.0f,
                              shadow_blur,
                              is_pixel_font(current_font) ? 1.0f : 0.0f,
                              fx_rain_mode ? 1.0f : 0.0f,
                              rain_size, rain_speed, rain_freq, rain_warp,
                              fx_crawl_mode ? 1.0f : 0.0f };
        vkCmdPushConstants(vk_cmd_buf, vk_liquid_pipe_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc_data), pc_data);
        vkCmdDrawIndexed(vk_cmd_buf, (fx_split_quad - term_start) * 6, 1, term_start * 6, 0, 0);

        /* Draw 2: UI chrome (windows, timer) with normal pipeline + main font */
        vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
        vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                vk_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
        {
            int chrome_quads = pl_quad_start - fx_split_quad;
            if (chrome_quads > 0)
                vkCmdDrawIndexed(vk_cmd_buf, chrome_quads * 6, 1, fx_split_quad * 6, 0, 0);
        }
        /* Draw 3: UI chrome with UI font atlas — split around icon bar quads */
        {
            if (ui_font_ready)
                vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipe_layout, 0, 1, &ui_desc_set, 0, NULL);
            /* 3a: UI quads before icon bar */
            int pre_icon = vib_quad_start - pl_quad_start;
            if (pre_icon > 0)
                vkCmdDrawIndexed(vk_cmd_buf, pre_icon * 6, 1, pl_quad_start * 6, 0, 0);
            /* 3b: Icon bar emoji quads with icon atlas */
            int icon_quads = vib_quad_end - vib_quad_start;
            if (icon_quads > 0 && vib_icon_ready) {
                vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipe_layout, 0, 1, &vib_desc_set, 0, NULL);
                vkCmdDrawIndexed(vk_cmd_buf, icon_quads * 6, 1, vib_quad_start * 6, 0, 0);
            }
            /* 3c: UI quads after icon bar (P&L, settings, etc.) */
            int post_icon = pl_quad_end - vib_quad_end;
            if (post_icon > 0) {
                if (icon_quads > 0 && vib_icon_ready)
                    vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipe_layout, 0, 1, &ui_desc_set, 0, NULL);
                vkCmdDrawIndexed(vk_cmd_buf, post_icon * 6, 1, vib_quad_end * 6, 0, 0);
            }
        }
        /* Draw 4: Context menu (back to main font) */
        {
            int menu_quads = vft_quad_start - pl_quad_end;
            if (menu_quads > 0) {
                vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
                vkCmdDrawIndexed(vk_cmd_buf, menu_quads * 6, 1, pl_quad_end * 6, 0, 0);
            }
        }
        /* Draw 5: Floating text (VFT font atlas, or ui atlas fallback) */
        {
            int vft_quads = vft_quad_end - vft_quad_start;
            if (vft_quads > 0) {
                VkDescriptorSet ds = vft_font_ready ? vft_desc_set : ui_desc_set;
                if (ds) {
                    vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipe_layout, 0, 1, &ds, 0, NULL);
                    vkCmdDrawIndexed(vk_cmd_buf, vft_quads * 6, 1, vft_quad_start * 6, 0, 0);
                }
            }
        }
    } else {
        /* No FX or no split — single draw with appropriate pipeline */
        if (fx_active) {
            vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_liquid_pipeline);
            vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk_liquid_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
            float pc_data[28] = { fx_time,
                                  fx_liquid_mode ? 1.0f : 0.0f,
                                  fx_waves_mode ? 1.0f : 0.0f,
                                  fx_scanline_mode ? 1.0f : 0.0f,
                                  fx_fbm_mode ? 1.0f : 0.0f,
                                  fx_sobel_mode ? 1.0f : 0.0f,
                                  pp_brightness, pp_contrast, pp_hue, pp_saturation,
                                  fx_smoky_mode ? 1.0f : 0.0f,
                                  smoke_decay, smoke_depth, smoke_zoom,
                                  smoke_hue, smoke_saturation, smoke_value,
                                  0.0f, 0.0f, /* cw/ch not needed */
                                  fx_shadow_mode ? shadow_opacity : 0.0f,
                                  shadow_blur,
                                  is_pixel_font(current_font) ? 1.0f : 0.0f,
                                  fx_rain_mode ? 1.0f : 0.0f,
                                  rain_size, rain_speed, rain_freq, rain_warp,
                                  fx_crawl_mode ? 1.0f : 0.0f };
            vkCmdPushConstants(vk_cmd_buf, vk_liquid_pipe_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc_data), pc_data);
        } else {
            vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
            vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    vk_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
        }
        /* Draw terminal + UI chrome up to P&L (skip bg quads) */
        if (pl_quad_start > term_start)
            vkCmdDrawIndexed(vk_cmd_buf, (pl_quad_start - term_start) * 6, 1, term_start * 6, 0, 0);
        /* UI chrome with UI font — split around icon bar quads */
        {
            if (ui_font_ready) {
                vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
                vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipe_layout, 0, 1, &ui_desc_set, 0, NULL);
            }
            /* Pre-icon UI quads */
            int pre_icon = vib_quad_start - pl_quad_start;
            if (pre_icon > 0)
                vkCmdDrawIndexed(vk_cmd_buf, pre_icon * 6, 1, pl_quad_start * 6, 0, 0);
            /* Icon bar emoji quads with icon atlas */
            int icon_quads = vib_quad_end - vib_quad_start;
            if (icon_quads > 0 && vib_icon_ready) {
                vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipe_layout, 0, 1, &vib_desc_set, 0, NULL);
                vkCmdDrawIndexed(vk_cmd_buf, icon_quads * 6, 1, vib_quad_start * 6, 0, 0);
            }
            /* Post-icon UI quads */
            int post_icon = pl_quad_end - vib_quad_end;
            if (post_icon > 0) {
                if (icon_quads > 0 && vib_icon_ready)
                    vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipe_layout, 0, 1, &ui_desc_set, 0, NULL);
                vkCmdDrawIndexed(vk_cmd_buf, post_icon * 6, 1, vib_quad_end * 6, 0, 0);
            }
        }
        /* Menu quads (back to main font) */
        {
            int menu_quads = vft_quad_start - pl_quad_end;
            if (menu_quads > 0) {
                vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        vk_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);
                vkCmdDrawIndexed(vk_cmd_buf, menu_quads * 6, 1, pl_quad_end * 6, 0, 0);
            }
        }
        /* Floating text (VFT font atlas, or ui atlas fallback) */
        {
            int vft_quads = vft_quad_end - vft_quad_start;
            if (vft_quads > 0) {
                VkDescriptorSet ds = vft_font_ready ? vft_desc_set : ui_desc_set;
                if (ds) {
                    vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            vk_pipe_layout, 0, 1, &ds, 0, NULL);
                    vkCmdDrawIndexed(vk_cmd_buf, vft_quads * 6, 1, vft_quad_start * 6, 0, 0);
                }
            }
        }
    }

    vkCmdEndRenderPass(vk_cmd_buf);
    vkEndCommandBuffer(vk_cmd_buf);

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
    fx_time += 1.0f / 60.0f; /* ~60fps, good enough for animation */
}

/* ---- Window procedure ---- */

static LRESULT CALLBACK vkt_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_KEYUP:
        /* F9 PTT release — call voice.dll directly */
        if (wParam == VK_F9) {
            typedef int (*voice_wndproc_fn)(HWND, UINT, WPARAM, LPARAM);
            static voice_wndproc_fn vwp = NULL;
            if (!vwp) {
                HMODULE vm = GetModuleHandleA("voice.dll");
                if (!vm) vm = LoadLibraryA("voice.dll");
                if (vm) vwp = (voice_wndproc_fn)GetProcAddress(vm, "voice_on_wndproc");
                if (api) api->log("[vk_terminal] F9 PTT: voice.dll=%p wndproc=%p\n", vm, vwp);
            }
            if (vwp) vwp(hwnd, WM_KEYUP, wParam, lParam);
        }
        return 0;

    case WM_KEYDOWN: {
        /* Reset swallow-char at the start of every keydown so it can't get
         * stuck (e.g., when a prior keydown set it but no WM_CHAR followed,
         * which happens with modified/dead keys). Only the current keystroke
         * is allowed to set it, which is consumed by the immediately-
         * following WM_CHAR. Without this, a single stuck flag silently
         * eats all typed input forever. */
        mdw_swallow_char = 0;
        /* File picker absorbs all keys when modal */
        if (fp_visible() && fp_key_down((unsigned int)wParam)) return 0;
        int ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        int alt  = GetKeyState(VK_MENU) & 0x8000;
        int shift = GetKeyState(VK_SHIFT) & 0x8000;

        /* Ctrl+T = toggle direct terminal input mode */
        if (ctrl && wParam == 'T' && !alt && !shift) {
            input_mode = !input_mode;
            return 0;
        }

        /* Numpad directions (NumLock must be on) */
        if (numpad_handle(wParam, ctrl, alt, shift)) return 0;

        /* Direct mode: send ANSI escape sequences for arrow/nav keys */
        if (input_mode && mmansi_hwnd) {
            const char *seq = NULL;
            switch (wParam) {
            case VK_UP:    seq = "\x1b[A"; break;
            case VK_DOWN:  seq = "\x1b[B"; break;
            case VK_RIGHT: seq = "\x1b[C"; break;
            case VK_LEFT:  seq = "\x1b[D"; break;
            case VK_HOME:  seq = "\x1b[H"; break;
            case VK_END:   seq = "\x1b[F"; break;
            case VK_PRIOR: seq = "\x1b[5~"; break; /* Page Up */
            case VK_NEXT:  seq = "\x1b[6~"; break; /* Page Down */
            }
            if (seq) {
                for (int i = 0; seq[i]; i++)
                    PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)(unsigned char)seq[i], 0);
                return 0;
            }
        }

        /* F9 PTT press — call voice.dll directly */
        if (wParam == VK_F9) {
            typedef int (*voice_wndproc_fn)(HWND, UINT, WPARAM, LPARAM);
            static voice_wndproc_fn vwp = NULL;
            if (!vwp) {
                HMODULE vm = GetModuleHandleA("voice.dll");
                if (!vm) vm = LoadLibraryA("voice.dll");
                if (vm) vwp = (voice_wndproc_fn)GetProcAddress(vm, "voice_on_wndproc");
                if (api) api->log("[vk_terminal] F9 PTT: voice.dll=%p wndproc=%p\n", vm, vwp);
            }
            if (vwp) vwp(hwnd, WM_KEYDOWN, wParam, lParam);
            return 0;
        }

        if (wParam == VK_ESCAPE) {
            if (vkm_open) { vkm_open = 0; vkm_sub = VKM_SUB_NONE; return 0; }
            if (vkw_focus >= 0) { vkw_focus = -1; return 0; } /* Esc = back to terminal */
        }

        /* Alt+B = toggle backscroll */
        if (alt && (wParam == 'B' || wParam == 'b')) {
            bsp_toggle((int)vk_sc_extent.width, (int)vk_sc_extent.height);
            return 0;
        }

        /* Ctrl+V = paste into active input */
        if (ctrl && wParam == 'V') {
            if (scm_visible && scm_focused) { scm_paste(); return 0; }
            if (vkw_focus >= 0) {
                vkw_window_t *fw = &vkw_windows[vkw_focus];
                if (fw->active && fw->has_input) {
                    clipboard_paste_into(fw->input, &fw->input_len, &fw->input_cursor, VKW_LINE_LEN);
                }
            } else {
                clipboard_paste_into(input_buf, &input_len, &input_cursor, INPUT_BUF_SZ);
            }
            return 0;
        }

        /* Ctrl+C/X/A — route to Script Manager if focused */
        if (ctrl && wParam == 'C' && scm_visible && scm_focused) { scm_copy_selection(); return 0; }
        if (ctrl && wParam == 'X' && scm_visible && scm_focused) { scm_cut_selection(); return 0; }
        if (ctrl && wParam == 'A' && scm_visible && scm_focused) { scm_select_all(); return 0; }

        /* Ctrl+C = copy selected text from focused window, or backscroll */
        if (ctrl && wParam == 'C') {
            if (vkw_focus >= 0) {
                vkw_window_t *fw = &vkw_windows[vkw_focus];
                if (fw->active && fw->has_selection && fw->line_count > 0) {
                    /* Copy selected lines */
                    int lo = fw->sel_start_line < fw->sel_end_line ? fw->sel_start_line : fw->sel_end_line;
                    int hi = fw->sel_start_line > fw->sel_end_line ? fw->sel_start_line : fw->sel_end_line;
                    if (hi >= fw->line_count) hi = fw->line_count - 1;
                    int total = 0;
                    for (int i = lo; i <= hi; i++) {
                        int idx = fw->line_head - 1 - i - fw->scroll;
                        while (idx < 0) idx += VKW_MAX_LINES;
                        idx %= VKW_MAX_LINES;
                        total += (int)strlen(fw->lines[idx]) + 1;
                    }
                    char *buf = (char *)malloc(total + 1);
                    if (buf) {
                        int off = 0;
                        /* Copy top-to-bottom (hi = topmost visually) */
                        for (int i = hi; i >= lo; i--) {
                            int idx = fw->line_head - 1 - i - fw->scroll;
                            while (idx < 0) idx += VKW_MAX_LINES;
                            idx %= VKW_MAX_LINES;
                            int slen = (int)strlen(fw->lines[idx]);
                            memcpy(buf + off, fw->lines[idx], slen);
                            off += slen;
                            buf[off++] = '\n';
                        }
                        buf[off] = 0;
                        clipboard_copy(buf, off);
                        free(buf);
                    }
                    fw->has_selection = 0;
                } else if (fw->active && fw->line_count > 0) {
                    /* No selection — copy all */
                    int total = 0;
                    for (int i = 0; i < fw->line_count; i++) total += (int)strlen(fw->lines[i]) + 1;
                    char *buf = (char *)malloc(total + 1);
                    if (buf) {
                        int off = 0;
                        for (int i = 0; i < fw->line_count; i++) {
                            int idx = (fw->line_head - fw->line_count + i + VKW_MAX_LINES) % VKW_MAX_LINES;
                            int slen = (int)strlen(fw->lines[idx]);
                            memcpy(buf + off, fw->lines[idx], slen);
                            off += slen;
                            buf[off++] = '\n';
                        }
                        buf[off] = 0;
                        clipboard_copy(buf, off);
                        free(buf);
                    }
                }
            } else {
                /* Terminal focused — copy entire backscroll */
                int blen = 0;
                char *bt = bs_get_text(&blen);
                if (bt) { clipboard_copy(bt, blen); free(bt); }
            }
            return 0;
        }

        /* Ctrl+A = select all in input (move cursor to end) */
        if (ctrl && wParam == 'A') {
            if (vkw_focus >= 0) {
                vkw_window_t *fw = &vkw_windows[vkw_focus];
                if (fw->active && fw->has_input) fw->input_cursor = fw->input_len;
            } else {
                input_cursor = input_len;
            }
            return 0;
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
        /* Route to Script Manager editor if focused */
        if (scm_key_down((unsigned int)wParam)) return 0;
        /* Route to focused window first */
        if (vkw_key_down((unsigned int)wParam)) return 0;
        input_handle_key(wParam, 0);
        return 0;
    }
    case WM_SYSKEYDOWN:
        /* Alt+F4 — kill MegaMUD instantly, no dialogs */
        if (wParam == VK_F4) {
            ExitProcess(0);
            return 0;
        }
        /* Alt+B when system key (alt held) */
        if (wParam == 'B' || wParam == 'b') {
            bsp_toggle((int)vk_sc_extent.width, (int)vk_sc_extent.height);
            return 0;
        }
        break;
    case WM_CHAR:
        /* Map Walker consumed the preceding WM_KEYDOWN — eat the WM_CHAR
         * that TranslateMessage generates so numpad walking doesn't also
         * type "4" into the terminal input. */
        if (mdw_swallow_char) { mdw_swallow_char = 0; return 0; }
        /* Map Walker input boxes (Map/Room fields) take priority */
        if (mdw_char_input((unsigned int)wParam)) return 0;
        /* File picker absorbs character input when modal */
        if (fp_visible()) return 0;
        /* Suppress Ctrl+chars (already handled in WM_KEYDOWN) */
        if (wParam < 32 && wParam != '\r' && wParam != '\b') return 0;
        /* Direct terminal mode: forward all chars to MMANSI */
        if (input_mode && mmansi_hwnd && !vrt_spell_input &&
            !(pl_wnd_open && pl_focused) && !scm_focused) {
            PostMessageA(mmansi_hwnd, WM_CHAR, wParam, lParam);
            return 0;
        }
        /* Route to spell input dialog if open */
        if (vrt_spell_input) {
            if (wParam == '\r') {
                /* Confirm */
                for (int i = 0; i < 21 && vrt_spell_buf[i]; i++) vrt_sync_spell[i] = vrt_spell_buf[i];
                vrt_sync_spell[vrt_spell_len] = '\0';
                vrt_spell_input = 0;
            } else if (wParam == '\b') {
                if (vrt_spell_cursor > 0) {
                    for (int i = vrt_spell_cursor - 1; i < vrt_spell_len; i++)
                        vrt_spell_buf[i] = vrt_spell_buf[i + 1];
                    vrt_spell_len--;
                    vrt_spell_cursor--;
                }
            } else if (wParam == 27) {
                vrt_spell_input = 0;
            } else if (vrt_spell_len < 21) {
                for (int i = vrt_spell_len; i > vrt_spell_cursor; i--)
                    vrt_spell_buf[i] = vrt_spell_buf[i - 1];
                vrt_spell_buf[vrt_spell_cursor] = (char)wParam;
                vrt_spell_len++;
                vrt_spell_cursor++;
                vrt_spell_buf[vrt_spell_len] = '\0';
            }
            return 0;
        }
        /* Route to P&L search if focused */
        if (pl_wnd_open && pl_focused) {
            if (wParam == 27) { pl_focused = 0; }
            else if (wParam == '\r') { pl_focused = 0; }
            else if (wParam == '\b') {
                if (pl_search_len > 0) pl_search[--pl_search_len] = 0;
            } else if (pl_search_len < 62) {
                pl_search[pl_search_len++] = (char)wParam;
                pl_search[pl_search_len] = 0;
                pl_scroll = 0;
            }
            return 0;
        }
        /* Route to Script Manager editor if focused */
        if (scm_key_char((unsigned int)wParam)) return 0;
        /* Route to MUDRadio search if focused */
        if (mr_search_focus && mr_key_char((int)wParam)) return 0;
        /* Route to focused window first */
        if (vkw_key_char((unsigned int)wParam)) return 0;
        input_handle_key(wParam, 1);
        return 0;
    case WM_MOUSEMOVE: {
        int mx2 = mouse_tx((short)LOWORD(lParam));
        int my2 = mouse_ty((short)HIWORD(lParam));
        if (fp_visible()) { fp_mouse_move(mx2, my2); return 0; }
        vkm_mouse_x = mx2;
        vkm_mouse_y = my2;
        /* Paths & Loops window drag */
        if (pl_dragging) {
            pl_x = (float)mx2 - pl_drag_ox;
            pl_y = (float)my2 - pl_drag_oy;
            return 0;
        }
        /* Paths & Loops hover tracking */
        if (pl_wnd_open && pl_hit(mx2, my2)) {
            pl_hover_item = pl_item_at_y(my2);
        } else {
            pl_hover_item = -1;
        }
        /* Script Manager drag/resize */
        if (scm_dragging || scm_resizing) {
            scm_mouse_move(mx2, my2);
            return 0;
        }
        /* MUDRadio drag/resize */
        if (mr_dragging || mr_resizing || mr_vol_dragging) {
            mr_mouse_move(mx2, my2);
            return 0;
        }
        /* Color panel drag/slider */
        if (clr_dragging || clr_active_slider >= 0) {
            clr_mouse_move(mx2, my2);
            return 0;
        }
        /* Background settings panel drag/slider/resize */
        if (bgp_dragging || bgp_resizing || bgp_active_slider >= 0) {
            bgp_mouse_move(mx2, my2);
            return 0;
        }
        if (smk_dragging || smk_active_slider >= 0) {
            smk_mouse_move(mx2, my2);
            return 0;
        }
        if (rn_dragging || rn_active_slider >= 0) {
            rn_mouse_move(mx2, my2);
            return 0;
        }
        if (shd_dragging || shd_active_slider >= 0) {
            shd_mouse_move(mx2, my2);
            return 0;
        }
        if (snd_dragging || snd_active_slider >= 0) {
            snd_mouse_move(mx2, my2);
            return 0;
        }
        if (fnt_dragging || fnt_active_slider >= 0) {
            fnt_mouse_move(mx2, my2);
            return 0;
        }
        /* Settings window drag */
        if (stw_dragging) {
            stw_x = (float)mx2 - stw_drag_ox;
            stw_y = (float)my2 - stw_drag_oy;
            return 0;
        }
        /* Player Stats drag */
        if (pst_dragging) {
            pst_x = (float)mx2 - pst_drag_ox;
            pst_y = (float)my2 - pst_drag_oy;
            return 0;
        }
        /* Map Walker drag/resize/pan */
        if (mdw_dragging || mdw_resizing || mdw_panning) {
            mdw_mouse_move(mx2, my2);
            return 0;
        }
        /* Backscroll panel drag/resize */
        if (bsp_dragging || bsp_resizing || bsp_selecting) {
            bsp_mouse_move(mx2, my2);
            return 0;
        }
        /* Round timer drag */
        if (vrt_dragging) {
            vrt_x = (float)mx2 - vrt_drag_ox;
            vrt_y = (float)my2 - vrt_drag_oy;
            if (fx_wobble_mode) wob_grid_rt.active = 1;
            return 0;
        }
        /* Floating window context menu hover */
        if (vkw_ctx_open) {
            int mcw2 = 10, mch2 = 18;
            int item_h2 = mch2 + 6;
            int cmw2 = 14 * mcw2 + 12;
            int cmh2 = VKW_CTX_COUNT * item_h2 + 4;
            int cx2 = vkw_ctx_x, cy2 = vkw_ctx_y;
            if (cx2 + cmw2 > (int)vk_sc_extent.width) cx2 = (int)vk_sc_extent.width - cmw2;
            if (cy2 + cmh2 > (int)vk_sc_extent.height) cy2 = (int)vk_sc_extent.height - cmh2;
            if (mx2 >= cx2 && mx2 < cx2 + cmw2 && my2 >= cy2 && my2 < cy2 + cmh2)
                vkw_ctx_hover = (my2 - cy2 - 2) / item_h2;
            else
                vkw_ctx_hover = -1;
        }
        /* Icon bar hover */
        {
            int vh = vib_hit_test(mx2, my2, vib_cur_bar_y);
            vib_pin_hover = (vh == VIB_HIT_PIN);
            vib_hover = (vh >= 0) ? vh : -1;
        }
        /* Round timer context menu hover */
        if (vrt_ctx_open) {
            vrt_ctx_hover = vrt_ctx_hit(mx2, my2);
            return 0;
        }
        /* When a context menu is open, it has top z-order priority —
         * skip all window interaction so windows underneath don't
         * steal hover/click events from the menu */
        if (!vkm_open) {
            /* Window drag/resize takes priority */
            if (vkw_mouse_move(mx2, my2)) return 0;
            /* Update cursor for VKW window edge hover */
            if (!vkw_update_cursor(mx2, my2)) {
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            }
        }
        if (vkm_open) {
            int rh = vkm_hit_root(mx2, my2);
            int sh = vkm_hit_sub(mx2, my2);

            /* Check if mouse is inside the 3rd-level submenu panel */
            int in_3rd = 0;
            if (vkm_sub == VKM_SUB_THEME || vkm_sub == VKM_SUB_FONT ||
                vkm_sub == VKM_SUB_FX || vkm_sub == VKM_SUB_VIZ ||
                vkm_sub == VKM_SUB_BG) {
                int sx3, sy3;
                vkm_get_sub_rect(&sx3, &sy3);
                int sh3 = vkm_sub_height();
                if (mx2 >= sx3 && mx2 < sx3 + VKM_SUB_W &&
                    my2 >= sy3 && my2 < sy3 + sh3) {
                    in_3rd = 1;
                }
            }

            if (rh >= 0 && !in_3rd) {
                vkm_hover = rh;
                /* Determine target sub for this root item */
                int target_sub = VKM_SUB_NONE;
                if (rh == VKM_ITEM_VISUAL) target_sub = VKM_SUB_VISUAL;
                else if (rh == VKM_ITEM_WIDGETS) target_sub = VKM_SUB_WIDGETS;
                else if (rh == VKM_ITEM_RECENT) target_sub = VKM_SUB_RECENT;
                else if (rh == VKM_ITEM_EXTRAS) target_sub = VKM_SUB_EXTRAS;
                else if (rh == VKM_ITEM_XTRA2) target_sub = VKM_SUB_XTRA2;

                /* If a 3rd-level sub is open and we're hovering its parent root,
                 * don't close/switch — treat it as "already on this sub" */
                int is_3rd_parent = 0;
                if ((vkm_sub == VKM_SUB_THEME || vkm_sub == VKM_SUB_FONT || vkm_sub == VKM_SUB_FX)
                    && rh == VKM_ITEM_VISUAL) is_3rd_parent = 1;
                if ((vkm_sub == VKM_SUB_VIZ || vkm_sub == VKM_SUB_BG) && rh == VKM_ITEM_XTRA2) is_3rd_parent = 1;

                if (is_3rd_parent) {
                    vkm_hover_root = rh;
                } else if (target_sub == VKM_SUB_NONE) {
                    /* Non-submenu item: close sub immediately */
                    vkm_sub = VKM_SUB_NONE;
                    vkm_hover_root = -1;
                } else if (vkm_sub == target_sub) {
                    /* Already on this sub, nothing to do */
                    vkm_hover_root = rh;
                } else {
                    /* Delayed hover: wait before switching submenu */
                    if (vkm_hover_root != rh) {
                        vkm_hover_root = rh;
                        vkm_hover_root_tick = GetTickCount();
                    } else if ((GetTickCount() - vkm_hover_root_tick) >= VKM_HOVER_DELAY_MS) {
                        vkm_sub = target_sub;
                        vkm_sub_hover = -1;
                    }
                }
            } else if (!in_3rd) {
                vkm_hover_root = -1;
            }
            if (sh >= 0) {
                vkm_sub_hover = sh;
                /* Delayed hover-to-open: track which arrow item is hovered, drill after delay */
                int is_drill_item = 0;
                if (vkm_sub == VKM_SUB_VISUAL && (sh == VKM_VIS_THEME || sh == VKM_VIS_FX))
                    is_drill_item = 1;
                if (vkm_sub == VKM_SUB_XTRA2 && sh == VKM_X2_VIZ)
                    is_drill_item = 1;

                if (is_drill_item) {
                    if (vkm_hover_drill != sh) {
                        vkm_hover_drill = sh;
                        vkm_hover_tick = GetTickCount();
                    } else if ((GetTickCount() - vkm_hover_tick) >= VKM_HOVER_DELAY_MS) {
                        /* Sustained hover — drill in */
                        if (vkm_sub == VKM_SUB_VISUAL) {
                            if (sh == VKM_VIS_THEME) vkm_sub = VKM_SUB_THEME;
                            else if (sh == VKM_VIS_FX) vkm_sub = VKM_SUB_FX;
                        } else if (vkm_sub == VKM_SUB_XTRA2) {
                            if (sh == VKM_X2_VIZ) vkm_sub = VKM_SUB_VIZ;
                            else if (sh == VKM_X2_BG) vkm_sub = VKM_SUB_BG;
                        }
                        vkm_hover_drill = -1;
                    }
                } else {
                    vkm_hover_drill = -1;
                }
            } else if (!in_3rd) {
                vkm_hover_drill = -1;
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = mouse_tx((short)LOWORD(lParam));
        int my = mouse_ty((short)HIWORD(lParam));
        /* FILE PICKER IS MODAL — nothing else can grab this click. */
        if (fp_visible()) { fp_mouse_down(mx, my); return 0; }
        /* QR OVERLAY IS MODAL — any click dismisses it */
        if (qr_mouse_down(mx, my)) return 0;
        /* Z-ORDER GATE: when the root context menu is open, swallow the
         * LBUTTONDOWN so it can't bleed through to MudAMP / panels beneath.
         * The actual menu item selection fires on LBUTTONUP. */
        if (vkm_open) return 0;
        /* Floating window context menu click */
        if (vkw_ctx_open) {
            int mcw = 10, mch = 18;
            int item_h = mch + 6;
            int cmw = 14 * mcw + 12;
            int cmh = VKW_CTX_COUNT * item_h + 4;
            int cx0 = vkw_ctx_x, cy0 = vkw_ctx_y;
            if (cx0 + cmw > (int)vk_sc_extent.width) cx0 = (int)vk_sc_extent.width - cmw;
            if (cy0 + cmh > (int)vk_sc_extent.height) cy0 = (int)vk_sc_extent.height - cmh;
            if (mx >= cx0 && mx < cx0 + cmw && my >= cy0 && my < cy0 + cmh) {
                int ci = (my - cy0 - 2) / item_h;
                if (ci >= 0 && ci < VKW_CTX_COUNT && vkw_ctx_wnd >= 0) {
                    vkw_window_t *tw = &vkw_windows[vkw_ctx_wnd];
                    if (ci == VKW_CTX_FONT_UP) {
                        tw->font_scale += 0.15f;
                        if (tw->font_scale > 3.0f) tw->font_scale = 3.0f;
                    } else if (ci == VKW_CTX_FONT_DN) {
                        tw->font_scale -= 0.15f;
                        if (tw->font_scale < 0.5f) tw->font_scale = 0.5f;
                    } else if (ci == VKW_CTX_FONT_RST) {
                        tw->font_scale = 1.0f;
                    }
                }
            }
            vkw_ctx_open = 0;
            return 0;
        }
        /* Round timer context menu click */
        if (vrt_ctx_open) {
            int ci = vrt_ctx_hit(mx, my);
            vrt_ctx_open = 0;
            if (ci == VRT_CTX_SPELL) { vrt_spell_input = 1; vrt_spell_len = 0; vrt_spell_buf[0] = '\0'; vrt_spell_cursor = 0;
                for (int i = 0; vrt_sync_spell[i] && i < 21; i++) { vrt_spell_buf[i] = vrt_sync_spell[i]; vrt_spell_len = i + 1; }
                vrt_spell_buf[vrt_spell_len] = '\0'; vrt_spell_cursor = vrt_spell_len; }
            else if (ci == VRT_CTX_CAST) { vrt_perform_manual_sync(); }
            else if (ci == VRT_CTX_SMALL) { vrt_size = 80; }
            else if (ci == VRT_CTX_NORMAL) { vrt_size = 160; }
            else if (ci == VRT_CTX_LARGE) { vrt_size = 280; }
            else if (ci == VRT_CTX_XLARGE) { vrt_size = 420; }
            else if (ci == VRT_CTX_XXL) { vrt_size = 640; }
            else if (ci == VRT_CTX_ORBITS) { vrt_orbits_visible = !vrt_orbits_visible; }
            else if (ci == VRT_CTX_RESET) { vrt_synced = 0; vrt_last_tick_ts = 0; vrt_round_num = 0; vrt_delta_text[0] = '\0'; }
            else if (ci == VRT_CTX_HIDE) { vrt_visible = 0; }
            return 0;
        }
        /* Spell input dialog clicks */
        if (vrt_spell_input) {
            float vssz = vrt_scaled_size();
            float scx = vrt_x + vssz / 2.0f;
            float scy = vrt_y + vssz / 2.0f + vssz / 2.0f + 10;
            int dx = (int)(scx - 120), dy = (int)scy;
            if (mx >= dx + 8 && mx <= dx + 60 && my >= dy + 50 && my <= dy + 70) {
                /* OK */
                for (int i = 0; i < 21 && vrt_spell_buf[i]; i++) vrt_sync_spell[i] = vrt_spell_buf[i];
                vrt_sync_spell[vrt_spell_len] = '\0';
                vrt_spell_input = 0;
            } else if (mx >= dx + 70 && mx <= dx + 140 && my >= dy + 50 && my <= dy + 70) {
                /* Cancel */
                vrt_spell_input = 0;
            } else if (mx >= dx + 150 && mx <= dx + 220 && my >= dy + 50 && my <= dy + 70) {
                /* Clear */
                vrt_spell_buf[0] = '\0'; vrt_spell_len = 0; vrt_spell_cursor = 0;
                vrt_sync_spell[0] = '\0';
            }
            return 0;
        }
        /* Paths & Loops window interaction */
        if (pl_wnd_open && pl_hit(mx, my)) {
            int lx = mx - (int)pl_x, ly = my - (int)pl_y;
            /* Close button */
            if (lx >= (int)pl_w - 20 && ly < PL_TITLEBAR) {
                pl_wnd_open = 0; return 0;
            }
            /* Titlebar drag */
            if (ly < PL_TITLEBAR) {
                pl_dragging = 1;
                pl_drag_ox = (float)mx - pl_x;
                pl_drag_oy = (float)my - pl_y;
                return 0;
            }
            /* Tab click */
            if (ly >= PL_TITLEBAR && ly < PL_TITLEBAR + PL_TAB_H) {
                pl_tab = (lx < (int)pl_w / 2) ? 0 : 1;
                pl_scroll = 0;
                return 0;
            }
            /* Toolbar clicks */
            if (ly >= PL_TITLEBAR + PL_TAB_H && ly < PL_TITLEBAR + PL_TAB_H + PL_TOOLBAR_H) {
                /* Hidden checkbox (goto tab, right half) */
                if (pl_tab == 0 && lx >= (int)pl_w / 2) {
                    pl_show_hidden = !pl_show_hidden;
                    return 0;
                }
                /* Walk/Run toggle (right edge) */
                if (lx >= (int)pl_w - 70) {
                    pl_run_mode = !pl_run_mode;
                    return 0;
                }
                /* Search box focus */
                pl_focused = 1;
                return 0;
            }
            /* GO button click */
            {
                int bottom_h = PL_GO_H + (int)(PL_CH_BASE * ui_scale) + (int)(8 * ui_scale);
                int content_bottom = (int)(pl_y + pl_h) - bottom_h;
                int go_y = content_bottom + 4;
                int go_h = PL_GO_H - 8;
                int go_w = 80;
                int go_x = (int)pl_x + ((int)pl_w - go_w) / 2;
                if (mx >= go_x && mx < go_x + go_w && my >= go_y && my < go_y + go_h) {
                    pl_exec_selected();
                    return 0;
                }
            }
            /* Content area — item click (select, don't execute) */
            {
                int bottom_h = PL_GO_H + (int)(PL_CH_BASE * ui_scale) + (int)(8 * ui_scale);
                int content_bottom = (int)(pl_y + pl_h) - bottom_h;
                if (my < content_bottom) {
                    int item = pl_item_at_y(my);
                    if (item >= 0) pl_click_item(item);
                }
            }
            return 0;
        }
        /* Settings panels — check focused one FIRST (drawn on top) */
        {
            struct { int (*fn)(int,int); int idx; } sp[] = {
                { clr_mouse_down, 0 }, { bgp_mouse_down, 1 }, { smk_mouse_down, 2 }, { shd_mouse_down, 3 }, { snd_mouse_down, 4 }, { fnt_mouse_down, 5 }, { rn_mouse_down, 6 }
            };
            /* Check focused panel first */
            if (sp_focus >= 0 && sp_focus < 7 && sp[sp_focus].fn(mx, my)) {
                return 0;
            }
            /* Then check others */
            for (int i = 0; i < 7; i++) {
                if (i != sp_focus && sp[i].fn(mx, my)) {
                    sp_focus = i;
                    return 0;
                }
            }
        }
        /* Script Manager click */
        if (scm_mouse_down(mx, my)) return 0;
        /* Backscroll panel click */
        if (bsp_mouse_down(mx, my)) return 0;
        /* Settings window click (MegaMUD Settings — on top of MudAMP) */
        if (stw_visible && mx >= (int)stw_x && mx < (int)(stw_x + stw_w) &&
            my >= (int)stw_y && my < (int)(stw_y + stw_h)) {
            int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);
            int titlebar_h = ch + 10;
            int ly = my - (int)stw_y;
            /* Close button */
            if (ly < titlebar_h && mx >= (int)(stw_x + stw_w - 24)) {
                stw_visible = 0;
                return 0;
            }
            /* Title bar drag */
            if (ly < titlebar_h) {
                stw_dragging = 1;
                stw_drag_ox = (float)mx - stw_x;
                stw_drag_oy = (float)my - stw_y;
                return 0;
            }
            /* Content clicks — re-call draw with click=1 to let widgets handle */
            int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
            stw_draw(vp_w, vp_h, mx, my, 1);
            return 0;
        }
        /* Map Walker — claims focus before MudAMP so it can't bleed clicks */
        if (mdw_visible && mdw_mouse_down(mx, my)) return 0;
        /* MUDRadio panel click — AFTER all settings/child windows */
        if (mr_mouse_down(mx, my)) return 0;
        /* Icon bar click (includes pin button at -2) */
        {
            int bi = vib_hit_test(mx, my, vib_cur_bar_y);
            if (bi >= 0 || bi == VIB_HIT_PIN) {
                vib_pressed = bi;
                vib_last_interact = GetTickCount();
                return 0;
            }
        }
        /* Player Stats panel click */
        if (pst_visible && mx >= (int)pst_x && mx < (int)(pst_x + pst_w) &&
            my >= (int)pst_y && my < (int)(pst_y + pst_h)) {
            int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);
            int titlebar_h = ch + 10;
            int ly = my - (int)pst_y;
            /* Close button (top-right corner, 20x titlebar_h area) */
            if (ly < titlebar_h && mx >= (int)(pst_x + pst_w - 24)) {
                pst_visible = 0;
                return 0;
            }
            /* Title bar drag */
            if (ly < titlebar_h) {
                pst_dragging = 1;
                pst_drag_ox = (float)mx - pst_x;
                pst_drag_oy = (float)my - pst_y;
                return 0;
            }
            /* Section header clicks for reset */
            int section_h = ch + 6;
            int row_h = ch + 4;
            int exp_hdr_y = titlebar_h + 2;
            int acc_hdr_y = exp_hdr_y + section_h + row_h * 6 + 4;
            if (ly >= exp_hdr_y && ly < exp_hdr_y + section_h) {
                pst_reset_exp();
                return 0;
            }
            if (ly >= acc_hdr_y && ly < acc_hdr_y + section_h) {
                pst_reset_combat();
                return 0;
            }
            return 0;
        }
        /* Round timer drag start */
        if (vrt_hit_circle(mx, my)) {
            vrt_dragging = 1;
            vrt_drag_ox = (float)mx - vrt_x;
            vrt_drag_oy = (float)my - vrt_y;
            return 0;
        }
        if (!vkm_open) {
            vkw_mouse_down(mx, my); /* start drag/resize or focus window */
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int mx = mouse_tx((short)LOWORD(lParam));
        int my = mouse_ty((short)HIWORD(lParam));
        /* Double-click on map room — walk to it */
        if (mdw_dblclick(mx, my)) return 0;
        /* Double-click in P&L content area — execute immediately */
        if (pl_wnd_open && pl_hit(mx, my)) {
            int lx = mx - (int)pl_x, ly = my - (int)pl_y;
            int toolbar_bottom = PL_TITLEBAR + PL_TAB_H + PL_TOOLBAR_H;
            int bottom_h = PL_GO_H + (int)(PL_CH_BASE * ui_scale) + (int)(8 * ui_scale);
            int content_bottom = (int)pl_h - bottom_h;
            if (ly >= toolbar_bottom && ly < content_bottom) {
                int item = pl_item_at_y(my);
                if (item >= 0) {
                    pl_selected_item = item;
                    pl_exec_item(item);
                }
            }
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP: {
        int mx = mouse_tx((short)LOWORD(lParam));
        int my = mouse_ty((short)HIWORD(lParam));
        if (fp_visible()) { fp_mouse_up(); return 0; }
        if (pl_dragging) { pl_dragging = 0; return 0; }
        if (scm_dragging || scm_resizing) { scm_mouse_up(); return 0; }
        if (mr_dragging || mr_resizing || mr_vol_dragging) { mr_mouse_up(); return 0; }
        if (clr_dragging || clr_active_slider >= 0) { clr_mouse_up(); return 0; }
        if (bgp_dragging || bgp_resizing || bgp_active_slider >= 0) { bgp_mouse_up(); return 0; }
        if (smk_dragging || smk_active_slider >= 0) { smk_mouse_up(); return 0; }
        if (rn_dragging || rn_active_slider >= 0) { rn_mouse_up(); return 0; }
        if (shd_dragging || shd_active_slider >= 0) { shd_mouse_up(); return 0; }
        if (snd_dragging || snd_active_slider >= 0) { snd_mouse_up(); return 0; }
        if (fnt_dragging || fnt_active_slider >= 0) { fnt_mouse_up(); return 0; }
        if (stw_dragging) { stw_dragging = 0; return 0; }
        if (pst_dragging) { pst_dragging = 0; return 0; }
        if (mdw_dragging || mdw_resizing || mdw_panning) { mdw_mouse_up(); return 0; }
        if (bsp_dragging || bsp_resizing || bsp_selecting) { bsp_mouse_up(); return 0; }
        if (vrt_dragging) { vrt_dragging = 0; return 0; }
        /* Icon bar: fire click on button release */
        if (vib_pressed >= 0 || vib_pressed == VIB_HIT_PIN) {
            int bi = vib_hit_test(mx, my, vib_cur_bar_y);
            if (bi == VIB_HIT_PIN && vib_pressed == VIB_HIT_PIN) {
                vib_pinned = !vib_pinned;
            } else if (bi >= 0 && bi == vib_pressed) {
                vib_click(bi);
            }
            vib_pressed = -1;
            return 0;
        }
        vkw_mouse_up(); /* end any drag/resize */

vkm_click_handler:
        if (!vkm_open) return 0;

        /* Check submenu click first */
        int si = vkm_hit_sub(mx, my);

        /* Visual > submenu clicks — open 2nd-level submenus */
        if (si >= 0 && vkm_sub == VKM_SUB_VISUAL) {
            if (si == VKM_VIS_THEME) { vkm_sub = VKM_SUB_THEME; vkm_sub_hover = -1; }
            else if (si == VKM_VIS_FONT) { fnt_toggle(); vkm_open = 0; vkm_sub = VKM_SUB_NONE; return 0; }
            else if (si == VKM_VIS_FX) { vkm_sub = VKM_SUB_FX; vkm_sub_hover = -1; }
            else if (si == VKM_VIS_COLOR) { clr_open_window(); vkm_open = 0; vkm_sub = VKM_SUB_NONE; return 0; }
            return 0;
        }
        /* Widgets > submenu clicks — toggle widgets */
        if (si >= 0 && vkm_sub == VKM_SUB_WIDGETS) {
            if (si == VKM_WID_RTIMER) {
                vrt_visible = !vrt_visible;
                if (vrt_visible && vrt_x < 1 && vrt_y < 1) {
                    vrt_x = (float)vk_sc_extent.width - vrt_scaled_size() - 20;
                    vrt_y = 60;
                }
            } else if (si == VKM_WID_CONVO) {
                vkw_toggle_convo((int)vk_sc_extent.width, (int)vk_sc_extent.height);
            } else if (si == VKM_WID_STATUSBAR) {
                vsb_visible = !vsb_visible;
            } else if (si == VKM_WID_EXPBAR) {
                vxb_visible = !vxb_visible;
            } else if (si == VKM_WID_PSTATS) {
                pst_toggle();
            } else if (si == VKM_WID_RADIO) {
                mr_toggle();
            } else if (si == VKM_WID_BSCROLL) {
                bsp_toggle((int)vk_sc_extent.width, (int)vk_sc_extent.height);
            } else if (si == VKM_WID_ICONBAR) {
                vib_visible = !vib_visible;
            }
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }
        /* Recent > submenu clicks — goto that room */
        if (si >= 0 && vkm_sub == VKM_SUB_RECENT) {
            if (si < vkm_goto_count && vkm_goto_names[si][0]) {
                pl_do_goto(vkm_goto_names[si]);
            }
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }
        /* Theme picker */
        if (si >= 0 && vkm_sub == VKM_SUB_THEME) {
            current_theme = si;
            apply_theme_palette(si);
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }
        /* Font picker */
        if (si >= 0 && vkm_sub == VKM_SUB_FONT) {
            pending_font_idx = (si == 0) ? -1 : si - 1;
            font_change_pending = 1;
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }
        /* FX toggles */
        if (si >= 0 && vkm_sub == VKM_SUB_FX) {
            if (si == VKM_FX_LIQUID) {
                fx_liquid_mode = !fx_liquid_mode;
                if (api) api->log("[vk_terminal] Liquid Letters: %s\n",
                                  fx_liquid_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_WAVES) {
                fx_waves_mode = !fx_waves_mode;
                if (fx_waves_mode) { fx_rain_mode = 0; rn_visible = 0; }
                if (api) api->log("[vk_terminal] Diagonal Waves: %s\n",
                                  fx_waves_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_FBM) {
                fx_fbm_mode = !fx_fbm_mode;
                if (api) api->log("[vk_terminal] FBM Currents: %s\n",
                                  fx_fbm_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_SOBEL) {
                fx_sobel_mode = !fx_sobel_mode;
                if (api) api->log("[vk_terminal] Sobel/Sharp: %s\n",
                                  fx_sobel_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_SCANLINES) {
                fx_scanline_mode = !fx_scanline_mode;
                if (api) api->log("[vk_terminal] CRT Scanlines: %s\n",
                                  fx_scanline_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_SMOKY) {
                fx_smoky_mode = !fx_smoky_mode;
                if (fx_smoky_mode && !smk_visible) smk_visible = 1;
                if (!fx_smoky_mode) smk_visible = 0;
                if (api) api->log("[vk_terminal] Smoky Letters: %s\n",
                                  fx_smoky_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_RAIN) {
                fx_rain_mode = !fx_rain_mode;
                if (fx_rain_mode) {
                    fx_waves_mode = 0; /* mutually exclusive */
                    if (!rn_visible) { rn_visible = 1; sp_focus = 6; }
                }
                if (!fx_rain_mode) rn_visible = 0;
                if (api) api->log("[vk_terminal] Rain Drops: %s\n",
                                  fx_rain_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_CRAWL) {
                fx_crawl_mode = !fx_crawl_mode;
                if (api) api->log("[vk_terminal] SpaceWarZ Crawl: %s\n",
                                  fx_crawl_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_SHADOW) {
                fx_shadow_mode = !fx_shadow_mode;
                if (fx_shadow_mode && !shd_visible) { shd_visible = 1; sp_focus = 3; }
                if (!fx_shadow_mode) shd_visible = 0;
                if (api) api->log("[vk_terminal] Drop Shadow: %s\n",
                                  fx_shadow_mode ? "ON" : "OFF");
            } else if (si == VKM_FX_WOBBLE && vk_experimental) {
                fx_wobble_mode = !fx_wobble_mode;
                if (api) api->log("[vk_terminal] Wobbly Widgets: %s\n",
                                  fx_wobble_mode ? "ON" : "OFF");
            }
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }

        /* Settings submenu click */
        if (si >= 0 && vkm_sub == VKM_SUB_EXTRAS) {
            if (si == VKM_EXT_SOUND) {
                snd_open_window();
            } else if (si == VKM_EXT_COLOR) {
                clr_open_window();
            } else if (si == VKM_EXT_HIDEMM) {
                megamud_hidden = !megamud_hidden;
                if (mmmain_hwnd) {
                    if (megamud_hidden) {
                        ShowWindow(mmmain_hwnd, SW_HIDE);
                        SetForegroundWindow(vkt_hwnd);
                        SetFocus(vkt_hwnd);
                    } else {
                        ShowWindow(mmmain_hwnd, SW_SHOW);
                        RedrawWindow(mmmain_hwnd, NULL, NULL,
                                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
                    }
                }
                if (api) api->log("[vk_terminal] MegaMUD %s\n",
                                  megamud_hidden ? "HIDDEN" : "SHOWN");
            } else if (si == VKM_EXT_RNDRECAP) {
                pst_round_recap = !pst_round_recap;
                if (api) api->log("[vk_terminal] Round Totals: %s\n",
                                  pst_round_recap ? "ON" : "OFF");
            } else if (si == VKM_EXT_HDR) {
                if (hdr_available || hdr_ext_available) {
                    hdr_enabled = !hdr_enabled;
                    /* Swapchain needs recreation for format change — trigger resize */
                    vkt_recreate_swapchain();
                    if (api) api->log("[vk_terminal] HDR: %s (%s)\n",
                                      hdr_enabled ? "ON" : "OFF", hdr_standard);
                }
            } else if (si == VKM_EXT_CONSOLE) {
                vkw_toggle_console((int)vk_sc_extent.width, (int)vk_sc_extent.height);
            } else if (si == VKM_EXT_SCRIPTMGR) {
                scm_toggle();
            } else if (si == VKM_EXT_SAVE) {
                vkt_save_settings();
            } else if (si == VKM_EXT_RESET) {
                vkt_reset_settings();
            } else if (si == VKM_EXT_PAIRANDROID) {
                qr_show();
            }
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }

        /* Extras > submenu click — opens 3rd-level submenus / direct toggles */
        if (si >= 0 && vkm_sub == VKM_SUB_XTRA2) {
            if (si == VKM_X2_VIZ) { vkm_sub = VKM_SUB_VIZ; vkm_sub_hover = -1; return 0; }
            if (si == VKM_X2_BG)  { vkm_sub = VKM_SUB_BG;  vkm_sub_hover = -1; return 0; }
            if (si == VKM_X2_RMSTEP) {
                rm_every_step_toggle_safe();
                if (api) api->log("[vk_terminal] RM Every Step: %s\n",
                                  rm_every_step_get_safe() ? "ON" : "OFF");
                vkm_open = 0;
                vkm_sub = VKM_SUB_NONE;
                return 0;
            }
            if (si == VKM_X2_MAPWALK) {
                if (mdw_bin_exists()) mdw_toggle();
                else if (api) api->log("[vk_terminal] Map Walker: no mmud.bin — use Load Map DB first.\n");
                vkm_open = 0;
                vkm_sub = VKM_SUB_NONE;
                return 0;
            }
            if (si == VKM_X2_LOADDB) {
                mdw_pick_and_load_mdb();
                vkm_open = 0;
                vkm_sub = VKM_SUB_NONE;
                return 0;
            }
            if (si == VKM_X2_RMDEBUG) {
                rm_show_debug = !rm_show_debug;
                if (api) api->log("[vk_terminal] Show RM Debug: %s\n",
                                  rm_show_debug ? "ON" : "OFF");
                vkt_save_settings();
                vkm_open = 0;
                vkm_sub = VKM_SUB_NONE;
                return 0;
            }
            return 0;
        }

        /* Backgrounds submenu click */
        if (si >= 0 && vkm_sub == VKM_SUB_BG) {
            if (si == VKM_BG_SETTINGS) {
                /* First item opens the unified Background Settings dialog.
                 * Dialog is tabbed; users pick which bg plugin to configure there. */
                bgp_visible = 1;
            } else if (si == VKM_BG_PLASMA) {
                /* Plain toggle — mutually exclusive with other backgrounds.
                 * Does NOT auto-open settings; use "Background Settings..." for that. */
                bg_mode = (bg_mode == BG_PLASMA) ? BG_NONE : BG_PLASMA;
                if (api) api->log("[vk_terminal] Fractal Plasma: %s\n",
                                  bg_mode == BG_PLASMA ? "ON" : "OFF");
            } else if (si == VKM_BG_STARFIELD) {
                bg_mode = (bg_mode == BG_STARFIELD) ? BG_NONE : BG_STARFIELD;
                if (api) api->log("[vk_terminal] Parallax Starfield: %s\n",
                                  bg_mode == BG_STARFIELD ? "ON" : "OFF");
            } else if (si == VKM_BG_ALIENWALL) {
                bg_mode = (bg_mode == BG_ALIENWALL) ? BG_NONE : BG_ALIENWALL;
                if (api) api->log("[vk_terminal] Alien Wall: %s\n",
                                  bg_mode == BG_ALIENWALL ? "ON" : "OFF");
            }
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }

        /* Visualizations submenu click */
        if (si >= 0 && vkm_sub == VKM_SUB_VIZ) {
            if (si == VKM_VIZ_BLADES) {
                viz_mode = (viz_mode == VIZ_BLADES) ? VIZ_NONE : VIZ_BLADES;
                if (viz_mode == VIZ_BLADES) viz_init();
            } else if (si == VKM_VIZ_CBALLS) {
                viz_mode = (viz_mode == VIZ_CANNONBALLS) ? VIZ_NONE : VIZ_CANNONBALLS;
                if (viz_mode == VIZ_CANNONBALLS) viz_init();
            } else if (si == VKM_VIZ_MATRIX) {
                viz_mode = (viz_mode == VIZ_MATRIX) ? VIZ_NONE : VIZ_MATRIX;
                if (viz_mode == VIZ_MATRIX) viz_init();
            } else if (si == VKM_VIZ_VECTROIDS) {
                viz_mode = (viz_mode == VIZ_VECTROIDS) ? VIZ_NONE : VIZ_VECTROIDS;
                if (viz_mode == VIZ_VECTROIDS) viz_init();
            }
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            return 0;
        }

        /* Check root click */
        int ri = vkm_hit_root(mx, my);
        if (ri == VKM_ITEM_MEGASETTINGS) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            stw_toggle();
            return 0;
        }
        if (ri == VKM_ITEM_PATHS) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            pl_load_data();
            pl_wnd_open = !pl_wnd_open;
            pl_scroll = 0;
            return 0;
        }
        if (ri == VKM_ITEM_CLOSE) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
            vkt_hide();
            return 0;
        }
        if (ri == VKM_ITEM_VISUAL || ri == VKM_ITEM_WIDGETS || ri == VKM_ITEM_RECENT || ri == VKM_ITEM_EXTRAS || ri == VKM_ITEM_XTRA2) {
            /* Click instantly opens submenu (bypass hover delay) */
            if (ri == VKM_ITEM_VISUAL) vkm_sub = VKM_SUB_VISUAL;
            else if (ri == VKM_ITEM_WIDGETS) vkm_sub = VKM_SUB_WIDGETS;
            else if (ri == VKM_ITEM_RECENT) vkm_sub = VKM_SUB_RECENT;
            else if (ri == VKM_ITEM_EXTRAS) vkm_sub = VKM_SUB_EXTRAS;
            else if (ri == VKM_ITEM_XTRA2) vkm_sub = VKM_SUB_XTRA2;
            vkm_sub_hover = -1;
            vkm_hover_root = ri;
            return 0;
        }

        /* Click outside menu = close */
        vkm_open = 0;
        vkm_sub = VKM_SUB_NONE;
        return 0;
    }
    case WM_RBUTTONUP: {
        int rmx = mouse_tx((short)LOWORD(lParam));
        int rmy = mouse_ty((short)HIWORD(lParam));
        /* File picker is modal — swallow right-clicks too */
        if (fp_visible()) return 0;
        /* Map walker: right-click a room to center the view on it */
        if (mdw_rbutton_down(rmx, rmy)) return 0;
        /* Right-click on floating window → font size context menu */
        {
            int wnd = vkw_hit_test(rmx, rmy);
            if (wnd >= 0) {
                if (vkw_ctx_open && vkw_ctx_wnd == wnd) {
                    vkw_ctx_open = 0;
                } else {
                    vkw_ctx_open = 1;
                    vkw_ctx_wnd = wnd;
                    vkw_ctx_x = rmx;
                    vkw_ctx_y = rmy;
                    vkw_ctx_hover = -1;
                }
                return 0;
            }
        }
        /* Close window context menu if clicking elsewhere */
        if (vkw_ctx_open) { vkw_ctx_open = 0; }
        /* Right-click on round timer → its own context menu */
        if (vrt_hit_circle(rmx, rmy)) {
            vrt_ctx_open = !vrt_ctx_open;
            vrt_ctx_x = rmx;
            vrt_ctx_y = rmy;
            vrt_ctx_hover = -1;
            return 0;
        }
        /* Close round timer context menu if open */
        if (vrt_ctx_open) { vrt_ctx_open = 0; return 0; }
        /* Toggle Vulkan-rendered context menu at mouse position */
        if (vkm_open) {
            vkm_open = 0;
            vkm_sub = VKM_SUB_NONE;
        } else {
            vkm_x = rmx;
            vkm_y = rmy;
            vkm_open = 1;
            vkm_hover = -1;
            vkm_sub = VKM_SUB_NONE;
            vkm_sub_hover = -1;
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        /* File picker scroll has absolute priority when open */
        if (fp_visible()) {
            int fmx = mouse_tx((short)LOWORD(lParam));
            int fmy = mouse_ty((short)HIWORD(lParam));
            int fd = GET_WHEEL_DELTA_WPARAM(wParam);
            fp_wheel(fmx, fmy, fd);
            return 0;
        }
        /* Map Walker zoom — check BEFORE any panel so it wins over MudAMP */
        {
            int wmx = mouse_tx((short)LOWORD(lParam));
            int wmy = mouse_ty((short)HIWORD(lParam));
            int wd = (short)HIWORD(wParam);
            if (mdw_wheel(wmx, wmy, wd)) return 0;
        }
        /* Scroll plasma settings panel */
        if (bgp_visible) {
            int wmx = mouse_tx((short)LOWORD(lParam));
            int wmy = mouse_ty((short)HIWORD(lParam));
            if (wmx >= (int)bgp_x && wmx < (int)(bgp_x + bgp_w) &&
                wmy >= (int)bgp_y && wmy < (int)(bgp_y + bgp_h)) {
                int wd = (short)HIWORD(wParam);
                bgp_scroll += (wd > 0) ? -2 : 2;
                if (bgp_scroll < 0) bgp_scroll = 0;
                return 0;
            }
        }
        /* Scroll Script Manager */
        if (scm_visible) {
            int wmx = mouse_tx((short)LOWORD(lParam));
            int wmy = mouse_ty((short)HIWORD(lParam));
            int wd = (short)HIWORD(wParam);
            if (scm_scroll_wheel(wmx, wmy, wd)) return 0;
        }
        /* Scroll MudAMP station list */
        if (mr_visible) {
            int wmx = mouse_tx((short)LOWORD(lParam));
            int wmy = mouse_ty((short)HIWORD(lParam));
            int wd = (short)HIWORD(wParam);
            if (mr_scroll_wheel(wmx, wmy, wd)) return 0;
        }
        /* Scroll font panel */
        if (fnt_visible) {
            int wmx = mouse_tx((short)LOWORD(lParam));
            int wmy = mouse_ty((short)HIWORD(lParam));
            int wd = (short)HIWORD(wParam);
            if (fnt_scroll_wheel(wmx, wmy, wd > 0 ? 1 : -1)) return 0;
        }
        /* Scroll backscroll panel */
        if (bsp_visible) {
            int bmx = mouse_tx((short)LOWORD(lParam));
            int bmy = mouse_ty((short)HIWORD(lParam));
            if (bmx >= (int)bsp_x && bmx < (int)(bsp_x + bsp_w) &&
                bmy >= (int)bsp_y && bmy < (int)(bsp_y + bsp_h)) {
                int wd = (short)HIWORD(wParam);
                bsp_scroll += (wd > 0) ? 3 : -3;
                if (bsp_scroll < 0) bsp_scroll = 0;
                return 0;
            }
        }
        /* Scroll Paths & Loops window */
        if (pl_wnd_open) {
            int pmx = mouse_tx((short)LOWORD(lParam));
            int pmy = mouse_ty((short)HIWORD(lParam));
            if (pl_hit(pmx, pmy)) {
                int wd = (short)HIWORD(wParam);
                pl_scroll += (wd > 0) ? -3 : 3;
                if (pl_scroll < 0) pl_scroll = 0;
                if (pl_total_items > pl_visible_items) {
                    int max_scroll = pl_total_items - pl_visible_items;
                    if (pl_scroll > max_scroll) pl_scroll = max_scroll;
                } else {
                    pl_scroll = 0;
                }
                return 0;
            }
        }
        /* Scroll wheel on round timer = resize */
        if (vrt_visible) {
            int wmx = mouse_tx((short)LOWORD(lParam));
            int wmy = mouse_ty((short)HIWORD(lParam));
            if (vrt_hit_circle(wmx, wmy)) {
                int wd = (short)HIWORD(wParam);
                float newsize = vrt_size + (wd > 0 ? 20.0f : -20.0f);
                if (newsize < VRT_MIN_SIZE) newsize = VRT_MIN_SIZE;
                if (newsize > VRT_MAX_SIZE) newsize = VRT_MAX_SIZE;
                /* Recenter on resize using scaled size */
                float old_ssz = vrt_scaled_size();
                float old_cx = vrt_x + old_ssz / 2.0f;
                float old_cy = vrt_y + old_ssz / 2.0f;
                vrt_size = newsize;
                float new_ssz = vrt_scaled_size();
                vrt_x = old_cx - new_ssz / 2.0f;
                vrt_y = old_cy - new_ssz / 2.0f;
                return 0;
            }
        }
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
    case WM_SYSCOMMAND:
        /* Alt+F4 generates SC_CLOSE — kill instantly */
        if ((wParam & 0xFFF0) == SC_CLOSE) {
            ExitProcess(0);
            return 0;
        }
        break;
    case WM_CLOSE:
        /* Alt+F4 fallback — kill instantly */
        ExitProcess(0);
        return 0;
    case WM_DESTROY:
        vkt_visible = 0;
        return 0;
    case WM_SETCURSOR:
        /* Let our custom cursor logic handle it — don't let Windows reset to IDC_ARROW */
        if (LOWORD(lParam) == HTCLIENT) return TRUE;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Background thread for --autoconnect / --loop (non-blocking) */
static int vkt_wait_for_ingame(int timeout_ms)
{
    /* Poll MegaMUD mode offset until character is in-game (mode >= 11).
     * Mode 11=idle, 14=walking, 15=looping. Before login it's 0. */
    if (!api) return 0;
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 250) {
        int mode = api->read_struct_i32(VSB_OFF_MODE);
        if (mode >= 11) return 1;
        Sleep(250);
    }
    return 0;
}

static void vkt_do_loadscripts(void)
{
    /* Load scripts from comma-separated list in vkt_loadscripts[] */
    if (!vkt_loadscripts[0]) return;

    /* Ensure script list is populated so we can mark loaded state */
    scm_scan_scripts();

    char buf[512];
    strncpy(buf, vkt_loadscripts, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *p = buf;
    while (*p) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;

        /* Extract script name */
        char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        char saved = *p;
        *p = '\0';

        if (start[0]) {
            /* Strip .py extension if the user included it */
            int slen = (int)strlen(start);
            if (slen > 3 && strcmp(start + slen - 3, ".py") == 0)
                start[slen - 3] = '\0';

            char cmd[256];
            _snprintf(cmd, sizeof(cmd), "mud.load('%s')", start);
            vkt_eval_python(cmd, 0);
            if (api) api->log("[vk_terminal] --loadscripts: loaded \"%s\"\n", start);
            /* Mark as loaded in script manager UI */
            for (int si = 0; si < scm_script_count; si++) {
                if (_stricmp(scm_script_names[si], start) == 0) {
                    scm_script_loaded[si] = 1;
                    break;
                }
            }
            Sleep(500); /* let script initialize before loading next */
        }

        if (saved) p++;
    }
    vkt_loadscripts[0] = '\0';
    /* Update status bar scripts display */
    scm_update_vsb_scripts();
}

static DWORD WINAPI vkt_autoconnect_thread(LPVOID param)
{
    (void)param;
    int scripts_only = (vkt_autoconnect == 2);

    if (!scripts_only) {
        /* Auto-connect to BBS */
        Sleep(2000);
        HMODULE hMmudpy = GetModuleHandleA("mmudpy.dll");
        if (!hMmudpy) hMmudpy = GetModuleHandleA("MMUDPy.dll");
        if (hMmudpy) {
            typedef int (*mmudpy_connect_fn)(void);
            mmudpy_connect_fn pfn = (mmudpy_connect_fn)GetProcAddress(hMmudpy, "mmudpy_connect");
            if (pfn) {
                pfn();
                if (api) api->log("[vk_terminal] --autoconnect: connecting\n");
            }
        }
    }

    /* Wait for character to be in-game (mode >= 11) before loading scripts/loops */
    if (api) api->log("[vk_terminal] Waiting for in-game state...\n");
    if (vkt_wait_for_ingame(60000)) {
        if (api) api->log("[vk_terminal] Character is in-game\n");
        Sleep(5000); /* let MegaMUD fully load paths, scripts, etc */

        /* Load scripts first */
        vkt_do_loadscripts();

        /* Reset player stats exp tracking after scripts are loaded */
        pst_reset_exp();

        /* Then start loop if requested */
        if (vkt_loop_name[0]) {
            HMODULE hMmudpy = GetModuleHandleA("mmudpy.dll");
            if (!hMmudpy) hMmudpy = GetModuleHandleA("MMUDPy.dll");
            if (hMmudpy) {
                typedef int (*mmudpy_fake_remote_fn)(const char *cmd);
                mmudpy_fake_remote_fn pRemote = (mmudpy_fake_remote_fn)GetProcAddress(hMmudpy, "mmudpy_fake_remote");
                if (pRemote) {
                    /* Stop any auto-resumed path from previous session first */
                    pRemote("stop");
                    if (api) api->log("[vk_terminal] --loop: stopped auto-resumed path\n");
                    Sleep(1000); /* let MegaMUD fully clear old path state */

                    char loop_cmd[160];
                    _snprintf(loop_cmd, sizeof(loop_cmd), "loop %s", vkt_loop_name);
                    pRemote(loop_cmd);
                    if (api) api->log("[vk_terminal] --loop: starting \"%s\"\n", vkt_loop_name);
                }
            }
            /* Preserve loop name for status bar display */
            strncpy(vsb_path_name, vkt_loop_name, sizeof(vsb_path_name) - 1);
            vsb_path_name[sizeof(vsb_path_name) - 1] = '\0';
            vkt_loop_name[0] = '\0';
        }
    } else {
        if (api) api->log("[vk_terminal] Timed out waiting for in-game state\n");
    }
    vkt_autoconnect = 0;
    return 0;
}

/* ---- Thread ---- */

/* EnumWindows callback: dismiss any MessageBox/dialog (#32770) belonging to
 * our process by clicking IDCANCEL / WM_CLOSE. Used to kill MegaMUD's
 * "TRC/LOG file too large, clear?" popups that race with first queueSubmit
 * on --startvulkan. */
static BOOL CALLBACK vkt_dismiss_dialog_cb(HWND hwnd, LPARAM lp)
{
    DWORD target_pid = (DWORD)lp;
    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);
    if (wnd_pid != target_pid) return TRUE;
    char cls[32] = {0};
    GetClassNameA(hwnd, cls, sizeof(cls));
    /* #32770 is the standard Win32 dialog class (MessageBox, DialogBox, etc.) */
    if (strcmp(cls, "#32770") != 0) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (api) api->log("[vk_terminal] Dismissing MegaMUD dialog hwnd=0x%p\n", hwnd);
    /* IDCANCEL = 2. Standard Win32 "No"/"Cancel" / Escape equivalent. */
    PostMessageA(hwnd, WM_COMMAND, 2 /*IDCANCEL*/, 0);
    PostMessageA(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}

static DWORD WINAPI vkt_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    /* Wine DPI is set to 192 in DllMain so MegaMUD's Win32 UI scales properly.
     * With KDE "Apply scaling themselves", XWayland gives us full 4K. */

    /* --startvulkan: find MMMAIN but DON'T hide it yet — hiding it too early
     * causes the app to exit silently on Win11 (MegaMUD appears to treat
     * early-hidden main window as a quit signal). We hide it later once the
     * Vulkan fullscreen window is up and ready to take over. */
    if (vkt_visible) {
        for (int i = 0; i < 200 && !mmmain_hwnd; i++) {
            HWND mw = FindWindowA("MMMAIN", NULL);
            if (mw) { mmmain_hwnd = mw; break; }
            Sleep(50);
        }
    }

    /* Splash progress helper */
    #define SPLASH_PROGRESS(pct, msg) do { \
        SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", #pct); \
        SetEnvironmentVariableA("SLOP_LOAD_STATUS", msg); \
    } while(0)

    /* Dismiss any MegaMUD startup dialogs (e.g. "TRC/LOG file too large, clear?")
     * that appear during --startvulkan. They grab message queue focus and race
     * with our first Vulkan queueSubmit, killing the process silently. */
    #define DISMISS_MEGAMUD_DIALOGS() do { \
        DWORD my_pid = GetCurrentProcessId(); \
        EnumWindows(vkt_dismiss_dialog_cb, (LPARAM)my_pid); \
    } while(0)

    SPLASH_PROGRESS(5, "Waiting for MegaMUD...");

    /* Wait for MMANSI */
    for (int i = 0; i < 100 && !mmansi_hwnd; i++) {
        if (!mmmain_hwnd) {
            HWND mw = FindWindowA("MMMAIN", NULL);
            if (mw) mmmain_hwnd = mw;
        }
        if (mmmain_hwnd)
            mmansi_hwnd = FindWindowExA(mmmain_hwnd, NULL, "MMANSI", NULL);
        /* Dismiss any MegaMUD startup dialogs that block further init */
        if (vkt_visible && (i % 5) == 0) DISMISS_MEGAMUD_DIALOGS();
        if (!mmansi_hwnd) Sleep(100);
    }
    if (!mmansi_hwnd) {
        api->log("[vk_terminal] MMANSI not found\n");
        /* Kill the splash so it doesn't linger as a TOPMOST popup blocking input */
        HWND sp = FindWindowA("SLOP_SPLASH", NULL);
        if (sp) PostMessageA(sp, WM_CLOSE, 0, 0);
        return 1;
    }

    /* Vulkan/font/buffer/descriptor/icon init already ran on main thread in vkt_init.
     * Kept here as a fallback in case vkt_mt_init_done was not set (e.g. early error). */
    if (!vkt_mt_init_done) {
        SPLASH_PROGRESS(15, "Creating Vulkan instance...");
        if (vkt_init_vulkan() != 0) return 1;
        SPLASH_PROGRESS(40, "Loading font textures...");
        if (vkt_create_font_texture() != 0) return 1;
        SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "55");
        SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Creating buffers...");
        if (vkt_create_buffers() != 0) return 1;
        SPLASH_PROGRESS(65, "Creating descriptors...");
        if (vkt_create_descriptors() != 0) return 1;
        SPLASH_PROGRESS(75, "Initializing fonts...");
        vkt_init_ui_font();
        vkt_init_vft_font(0);
        SPLASH_PROGRESS(85, "Loading icon atlas...");
        vib_init_icon_atlas();
    }

    SPLASH_PROGRESS(95, "Starting terminal...");

    /* Register window class */
    WNDCLASSA wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = vkt_wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SlopVkTerminal";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    apply_theme_palette(current_theme);
    vkt_running = 1;
    SPLASH_PROGRESS(100, "Ready!");
    api->log("[vk_terminal] Ready (F11 to toggle fullscreen)\n");

    /* Bulletproof splash close — env-var polling races with splash WM_TIMER;
     * directly post WM_CLOSE so the splash disappears the moment we're up.
     * Was getting stuck at 45% when plugin-loop finished but MMANSI detection
     * took >10s on Win11 (then WM_TIMER never saw >=100 and splash lingered,
     * forced KDE taskbar visible on every terminal click). */
    {
        HWND sp = FindWindowA("SLOP_SPLASH", NULL);
        if (sp) {
            PostMessageA(sp, WM_CLOSE, 0, 0);
            api->log("[vk_terminal] Posted WM_CLOSE to splash hwnd=%p\n", sp);
        }
    }


    /* --autoconnect/--loop: run in background so Vulkan window opens immediately */
    if (vkt_autoconnect) {
        CreateThread(NULL, 0, vkt_autoconnect_thread, NULL, 0, NULL);
    }

    /* Main loop — wait for F11 toggle, then render */
    while (vkt_running) {
        if (!vkt_visible) {
            /* Tear down presentation if window exists */
            if (vkt_hwnd) {
                /* Restore MegaMUD BEFORE destroying Vulkan window so
                 * the OS gives focus to MegaMUD instead of desktop */
                if (mmmain_hwnd) {
                    if (megamud_hidden) {
                        ShowWindow(mmmain_hwnd, SW_SHOW);
                        megamud_hidden = 0;
                        RedrawWindow(mmmain_hwnd, NULL, NULL,
                                     RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASE | RDW_FRAME);
                    }
                    SetForegroundWindow(mmmain_hwnd);
                    BringWindowToTop(mmmain_hwnd);
                    if (mmansi_hwnd) SetFocus(mmansi_hwnd);
                }
                if (vk_dev) vkDeviceWaitIdle(vk_dev);
                vkt_destroy_swapchain();
                if (vk_surface) { vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE; }
                DestroyWindow(vkt_hwnd);
                vkt_hwnd = NULL;
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
            /* Use user-selected resolution, or screen size for auto */
            if (snd_res_sel >= 0 && snd_res_sel < (int)NUM_RES) {
                fs_width = resolutions[snd_res_sel].w;
                fs_height = resolutions[snd_res_sel].h;
            } else {
                int sm_w = GetSystemMetrics(SM_CXSCREEN);
                int sm_h = GetSystemMetrics(SM_CYSCREEN);
                if (sm_w > 0) fs_width = sm_w;
                if (sm_h > 0) fs_height = sm_h;
            }
            api->log("[vk_terminal] Fullscreen: %dx%d\n", fs_width, fs_height);

            vkt_hwnd = CreateWindowExA(
                WS_EX_APPWINDOW,
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

            /* Kill splash — safety net. If WM_CLOSE from vkt_thread startup
             * didn't land (splash thread not pumping yet), this catches it
             * when the Vulkan window actually appears. */
            {
                HWND sp = FindWindowA("SLOP_SPLASH", NULL);
                if (sp) PostMessageA(sp, WM_CLOSE, 0, 0);
            }

            /* Close MegaMUD owned popup windows (Conversations etc.) to prevent focus stealing */
            if (mmmain_hwnd) {
                HWND popup = GetWindow(GetDesktopWindow(), GW_CHILD);
                while (popup) {
                    HWND next = GetWindow(popup, GW_HWNDNEXT);
                    if (GetWindow(popup, GW_OWNER) == mmmain_hwnd && IsWindowVisible(popup)) {
                        SendMessageA(popup, WM_CLOSE, 0, 0);
                    }
                    popup = next;
                }
                /* Hide MegaMUD main window */
                ShowWindow(mmmain_hwnd, SW_HIDE);
                megamud_hidden = 1;
            }
            ShowWindow(vkt_hwnd, SW_SHOW);
            SetForegroundWindow(vkt_hwnd);
            SetFocus(vkt_hwnd);
            api->log("[vk_terminal] Fullscreen %dx%d\n", fs_width, fs_height);
            mouse_update_scale((int)vk_sc_extent.width, (int)vk_sc_extent.height);

            /* Post-create settle: pump messages + dismiss MegaMUD popups for
             * ~2s before the first render submit. The "TRC/LOG file too large,
             * clear?" dialog in particular races with first queueSubmit and
             * silently terminates the process on Win11+lavapipe. */
            for (int s = 0; s < 80; s++) {
                MSG m2;
                while (PeekMessageA(&m2, NULL, 0, 0, PM_REMOVE)) {
                    if (m2.message == WM_QUIT) { vkt_running = 0; break; }
                    TranslateMessage(&m2);
                    DispatchMessageA(&m2);
                }
                if (!vkt_running) break;
                /* Scan for and dismiss any MegaMUD dialog popups every ~100ms */
                if ((s % 4) == 0) DISMISS_MEGAMUD_DIALOGS();
                Sleep(25);
            }
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

    /* UI font cleanup */
    if (ui_font_sampler) { vkDestroySampler(vk_dev, ui_font_sampler, NULL); ui_font_sampler = VK_NULL_HANDLE; }
    if (ui_font_view) { vkDestroyImageView(vk_dev, ui_font_view, NULL); ui_font_view = VK_NULL_HANDLE; }
    if (ui_font_img) { vkDestroyImage(vk_dev, ui_font_img, NULL); ui_font_img = VK_NULL_HANDLE; }
    if (ui_font_mem) { vkFreeMemory(vk_dev, ui_font_mem, NULL); ui_font_mem = VK_NULL_HANDLE; }
    ui_font_ready = 0;

    /* Icon atlas cleanup */
    if (vib_icon_sampler) { vkDestroySampler(vk_dev, vib_icon_sampler, NULL); vib_icon_sampler = VK_NULL_HANDLE; }
    if (vib_icon_view) { vkDestroyImageView(vk_dev, vib_icon_view, NULL); vib_icon_view = VK_NULL_HANDLE; }
    if (vib_icon_img) { vkDestroyImage(vk_dev, vib_icon_img, NULL); vib_icon_img = VK_NULL_HANDLE; }
    if (vib_icon_mem) { vkFreeMemory(vk_dev, vib_icon_mem, NULL); vib_icon_mem = VK_NULL_HANDLE; }
    vib_icon_ready = 0;

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

/* ---- Player Statistics — Self-Contained Combat Parser ---- */

/* Attack type stats: count, min damage, max damage, total damage */
typedef struct { int n, min, max; __int64 total; } pst_atk_t;

static struct {
    /* Experience tracking */
    DWORD   exp_start_tick;     /* GetTickCount at exp reset */
    __int64 exp_gained;         /* accumulated from "You gain X experience" */

    /* Combat accuracy */
    int     miss;               /* player attack missed */
    pst_atk_t hit;              /* normal hits */
    pst_atk_t crit;             /* critical hits */
    pst_atk_t extra;            /* extra attacks */
    pst_atk_t backstab;         /* backstab hits */
    pst_atk_t spell;            /* spell/cast damage */

    /* Defense */
    int     dodge;              /* player dodged */

    /* Round tracking — includes misses as 0-damage swings */
    pst_atk_t rnd;              /* per-round damage totals */
    int     cur_round_dmg;      /* accumulating current round */
    int     cur_round_swings;   /* swings this round (hits + misses) */
    DWORD   last_swing_tick;    /* timestamp of last swing for round separation */

    /* Per-round breakdown (cleared after each round recap) */
    int     cr_hits;            /* normal hits this round */
    int     cr_crits;           /* crits this round */
    int     cr_extra;           /* extra attacks this round */
    int     cr_spell;           /* spell hits this round */
    int     cr_bs;              /* backstabs this round */
    int     cr_miss;            /* misses this round */
    int     cr_glance;          /* glancing blows this round (weapon ineffective) */
    int     cr_dodge;           /* enemy dodged player's attack this round */
    int     cr_dmg;             /* total damage this round (all sources incl BS) */

    /* Misc */
    int     kills;              /* monster kills */

    /* Combat section start */
    DWORD   combat_start_tick;
} pst_s;

/* ANSI line extraction buffer */
static char pst_lbuf[1024];
/* Line-buffered state machine. Replaces the hand-rolled loop that lived
 * here; preserves behavior exactly (byte-for-byte validated in
 * /tmp/test_line_fsm.c across chunk sizes 1..1M). */
static line_fsm_t pst_fsm;
static int        pst_fsm_inited = 0;

static DWORD pst_last_poll = 0;
#define PST_POLL_MS 1000

/* Deferred round recap: wait for late combat messages before displaying.
 * BBS lag can cause messages from the same round to arrive after the
 * round timer fires, splitting one round's output across two recaps.
 * Grace period lets late messages accumulate before we flush. */
static DWORD pst_last_recap_tick = 0;  /* GetTickCount of last recap emission */

/* ---- helpers ---- */

static int pst_contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

/* Parse damage number from "for X damage!" — scan backwards from " damage!" */
static int pst_parse_dmg(const char *line) {
    const char *p = strstr(line, " damage!");
    if (!p || p == line) return 0;
    p--; /* char before ' damage!' */
    while (p > line && *p >= '0' && *p <= '9') p--;
    if (*p < '0' || *p > '9') p++;
    return atoi(p);
}

/* Record a hit into an atk_t bucket */
static void pst_record_hit(pst_atk_t *a, int dmg) {
    a->n++;
    a->total += dmg;
    if (a->min < 0 || dmg < a->min) a->min = dmg;
    if (dmg > a->max) a->max = dmg;
}

static void pst_atk_clear(pst_atk_t *a) { a->n = 0; a->min = -1; a->max = 0; a->total = 0; }

/* Flush current round if any swings accumulated */
static void pst_flush_round(void) {
    if (pst_s.cur_round_swings > 0) {
        pst_record_hit(&pst_s.rnd, pst_s.cur_round_dmg);
        pst_s.cur_round_dmg = 0;
        pst_s.cur_round_swings = 0;
    }
}

/* Actually display the round recap and clear counters.
 * Called after grace period has elapsed. */
static void pst_emit_recap(void) {
    pst_flush_round();

    /* Only show recap if there was actual combat this round */
    int had_combat = pst_s.cr_hits || pst_s.cr_crits || pst_s.cr_extra ||
                     pst_s.cr_spell || pst_s.cr_bs || pst_s.cr_miss ||
                     pst_s.cr_glance || pst_s.cr_dodge;
    int round_dmg = pst_s.cr_dmg;

    /* Always store recap data for MMUDPy access */
    if (had_combat) {
        pst_recap_dmg = round_dmg;
        pst_recap_hits = pst_s.cr_hits;
        pst_recap_crits = pst_s.cr_crits;
        pst_recap_extra = pst_s.cr_extra;
        pst_recap_spell = pst_s.cr_spell;
        pst_recap_bs = pst_s.cr_bs;
        pst_recap_miss = pst_s.cr_miss;
        pst_recap_glance = pst_s.cr_glance;
        pst_recap_dodge = pst_s.cr_dodge;
        pst_recap_seq++;
        _snprintf(pst_last_recap, sizeof(pst_last_recap),
            "%d dmg, %d hit, %d crit, %d xtra, %d spell, %d bs, %d miss, %d glance, %d dodge",
            round_dmg, pst_s.cr_hits, pst_s.cr_crits, pst_s.cr_extra,
            pst_s.cr_spell, pst_s.cr_bs, pst_s.cr_miss, pst_s.cr_glance, pst_s.cr_dodge);
        pst_last_recap[sizeof(pst_last_recap) - 1] = 0;
    }

    if (pst_round_recap && had_combat) {
        char buf[256];
        int pos = 0;

        pos += _snprintf(buf + pos, sizeof(buf) - pos,
            "\x1b[44;1;37m %d Damage",
            round_dmg);

        /* Build parenthetical: BS, Crits, Hits, Xtra, Spell, then misses/dodges */
        {
            int first = 1;
            #define RECAP_ADD(cnt, label) \
                if ((cnt) > 0) { \
                    pos += _snprintf(buf + pos, sizeof(buf) - pos, \
                        "%s%dx %s", first ? " (" : ", ", (cnt), label); \
                    first = 0; \
                }
            RECAP_ADD(pst_s.cr_bs,     "BS");
            RECAP_ADD(pst_s.cr_crits,  "Crits");
            RECAP_ADD(pst_s.cr_hits,   "Hits");
            RECAP_ADD(pst_s.cr_extra,  "Xtra");
            RECAP_ADD(pst_s.cr_spell,  "Spell");
            RECAP_ADD(pst_s.cr_miss,   "Misses");
            RECAP_ADD(pst_s.cr_glance, "Glances");
            RECAP_ADD(pst_s.cr_dodge,  "Dodges");
            #undef RECAP_ADD
            if (!first) pos += _snprintf(buf + pos, sizeof(buf) - pos, ")");
        }

        pos += _snprintf(buf + pos, sizeof(buf) - pos, " \x1b[0m");
        buf[sizeof(buf) - 1] = 0;

        char inject[280];
        /* No leading CRLF — combat swings from MegaMUD always end with CRLF,
         * so cursor is already at col 0. Adding another CRLF inserts a blank
         * line that shows up as an extra empty slat in SpaceWarZ crawl mode.
         * Use bare \r + clear-to-EOL as a belt-and-suspenders guard in case
         * we ever get injected mid-line. */
        _snprintf(inject, sizeof(inject), "\r\x1b[K%s\r\n", buf);
        inject[sizeof(inject) - 1] = 0;
        EnterCriticalSection(&ansi_lock);
        ap_feed(&ansi_term, (const uint8_t *)inject, (int)strlen(inject));
        LeaveCriticalSection(&ansi_lock);
    }

    /* Clear per-round breakdown counters */
    pst_s.cr_hits = 0; pst_s.cr_crits = 0; pst_s.cr_extra = 0;
    pst_s.cr_spell = 0; pst_s.cr_bs = 0; pst_s.cr_miss = 0;
    pst_s.cr_glance = 0; pst_s.cr_dodge = 0; pst_s.cr_dmg = 0;
}

/* Called from vrt_on_round. Recap emission is now driven by
 * pst_check_round_gap (inside pst_parse_line), which fires on cluster
 * boundaries BEFORE the next round's first swing is counted. This tick
 * stays as a belt-and-suspenders safety net for rounds where no further
 * combat events arrive (e.g. combat ended); it's a no-op otherwise
 * because the gap check already drained the counters. */
static void pst_on_round_tick(int round_num) {
    pst_flush_round();
    pst_last_recap_tick = GetTickCount();
}

/* Called every frame — if combat events have accumulated and no swing has
 * arrived for >PST_IDLE_EMIT_MS, the round is over. Emit the recap now,
 * during the idle gap, so the user sees the recap promptly.
 *
 * MegaMUD rounds fire every ~5s and all events in a burst land within a
 * few hundred ms of each other. 250ms is tight enough to feel responsive
 * while still clearing any normal intra-burst jitter. Cluster-gap (500ms)
 * stays as a belt-and-suspenders fallback. */
#define PST_IDLE_EMIT_MS 100
static void pst_recap_poll(void) {
    if (pst_s.last_swing_tick == 0) return;
    DWORD elapsed = GetTickCount() - pst_s.last_swing_tick;
    if (elapsed < PST_IDLE_EMIT_MS) return;
    int has_data = pst_s.cr_hits || pst_s.cr_crits || pst_s.cr_extra ||
                   pst_s.cr_spell || pst_s.cr_bs || pst_s.cr_miss ||
                   pst_s.cr_glance || pst_s.cr_dodge;
    if (has_data) {
        pst_flush_round();
        pst_emit_recap();
        pst_last_recap_tick = GetTickCount();
    }
}

/* Cluster-boundary check — runs at the TOP of every combat-event branch in
 * pst_parse_line, BEFORE the event is accumulated into this round's counters.
 *
 * If >CLUSTER_GAP_MS has passed since the last swing and we have combat data
 * buffered, this event belongs to a NEW round — emit the recap of the
 * previous round now, then clear counters. The event will then accumulate
 * cleanly into the fresh round.
 *
 * This fixes round splits that happen when msimg32's round-tick fires AFTER
 * pst_feed has already added round N+1's first swing to round N's counters
 * (both triggered synchronously from the same hooked_process_incoming chunk,
 * pst_feed ran first). By making pst_parse_line itself the state-machine
 * driver for recap emission, ordering becomes deterministic. */
#define PST_CLUSTER_GAP_MS 500

static void pst_check_round_gap(void) {
    DWORD now = GetTickCount();
    if (pst_s.last_swing_tick > 0) {
        DWORD elapsed = now - pst_s.last_swing_tick;
        if (elapsed > PST_CLUSTER_GAP_MS) {
            /* New cluster — emit recap of what we've accumulated. */
            int has_data = pst_s.cr_hits || pst_s.cr_crits || pst_s.cr_extra ||
                           pst_s.cr_spell || pst_s.cr_bs || pst_s.cr_miss ||
                           pst_s.cr_glance || pst_s.cr_dodge;
            if (has_data) {
                pst_flush_round();
                pst_emit_recap();
            } else if (pst_s.cur_round_swings > 0) {
                /* per-round rnd stats still want flushing */
                pst_flush_round();
            }
        }
    }
    pst_s.last_swing_tick = now;
}

/* ---- Combat line parser ---- */

static void pst_parse_line(const char *line)
{
    /* Strip HP/MA prompt prefix if glued on: "[HP=197/MA=159]:You slash..."
     * MUD sometimes sends prompt + combat line in same packet with no newline */
    if (line[0] == '[' && line[1] == 'H' && line[2] == 'P' && line[3] == '=') {
        const char *colon = strchr(line, ':');
        if (colon && colon[1]) line = colon + 1;
    }

    /* --- Experience --- */
    {
        const char *yg = strstr(line, "You gain ");
        if (yg && strstr(yg, " experience")) {
            int val = atoi(yg + 9); /* "You gain " = 9 chars */
            if (val > 0) {
                pst_s.exp_gained += val;
                /* Update status bar exp rate */
                DWORD ems = GetTickCount() - pst_s.exp_start_tick;
                if (ems > 5000)
                    vsb_exp_rate = pst_s.exp_gained * 3600000LL / (__int64)ems;
            }
            return;
        }
    }

    /* --- Monster killed --- */
    if (pst_contains(line, " drops to the ground!")) {
        pst_s.kills++;
        pst_flush_round();
        return;
    }

    /* --- Player Dodge (you dodged monster attack) --- */
    if (pst_contains(line, "but you dodge") ||
        pst_contains(line, "but you sidestep") ||
        pst_contains(line, "but you easily dodge") ||
        pst_contains(line, "but you barely dodge") ||
        pst_contains(line, "but you quickly roll") ||
        pst_contains(line, "but you leap out of") ||
        pst_contains(line, "but you duck the attack") ||
        pst_contains(line, "You quickly backpedal")) {
        pst_s.dodge++;
        return;
    }

    /* --- Player Hit (line contains " damage!") --- */
    if (pst_contains(line, " damage!")) {
        /* Is this us hitting a monster, or a monster hitting us?
         * Enemy lines contain " you " before " damage!" (e.g. "impales you with") */
        const char *dmg_pos = strstr(line, " damage!");
        if (dmg_pos) {
            const char *yu = strstr(line, " you ");
            if (yu && yu < dmg_pos) return; /* enemy hit on us */
            const char *yf = strstr(line, " you!");
            if (yf) return;
        }

        /* Player hit — parse damage and categorize */
        int dmg = pst_parse_dmg(line);

        if (pst_contains(line, "backstab") || pst_contains(line, "You surprise")) {
            /* BS starts a new round. Emit previous recap BEFORE counting. */
            pst_check_round_gap();
            pst_record_hit(&pst_s.backstab, dmg);
            pst_s.cr_bs++;
            pst_s.cr_dmg += dmg;
            pst_flush_round();
            pst_record_hit(&pst_s.rnd, dmg);
        } else if (pst_contains(line, " critically ")) {
            pst_check_round_gap();
            pst_record_hit(&pst_s.crit, dmg);
            pst_s.cr_crits++;
            pst_s.cr_dmg += dmg;
            pst_s.cur_round_dmg += dmg;
            pst_s.cur_round_swings++;
        } else if (pst_contains(line, "Your ") && (pst_contains(line, " hits ") || pst_contains(line, " blasts ") ||
                   pst_contains(line, " burns ") || pst_contains(line, " freezes ") || pst_contains(line, " zaps "))) {
            pst_check_round_gap();
            pst_record_hit(&pst_s.spell, dmg);
            pst_s.cr_spell++;
            pst_s.cr_dmg += dmg;
            pst_s.cur_round_dmg += dmg;
            pst_s.cur_round_swings++;
        } else if (pst_contains(line, "extra attack")) {
            pst_check_round_gap();
            pst_record_hit(&pst_s.extra, dmg);
            pst_s.cr_extra++;
            pst_s.cr_dmg += dmg;
            pst_s.cur_round_dmg += dmg;
            pst_s.cur_round_swings++;
        } else {
            pst_check_round_gap();
            pst_record_hit(&pst_s.hit, dmg);
            pst_s.cr_hits++;
            pst_s.cr_dmg += dmg;
            pst_s.cur_round_dmg += dmg;
            pst_s.cur_round_swings++;
        }
        return;
    }

    /* --- Glancing Blow (weapon ineffective, "Your cut glances off ...") --- */
    if (strncmp(line, "Your ", 5) == 0 && pst_contains(line, " glances off")) {
        pst_check_round_gap();
        pst_s.miss++;
        pst_s.cr_glance++;
        pst_s.cur_round_swings++;
        return;
    }

    /* --- Enemy Dodge (monster dodged player's attack) --- */
    if (pst_contains(line, "dodges your ") ||
        pst_contains(line, "sidesteps your ") ||
        pst_contains(line, "evades your ")) {
        pst_check_round_gap();
        pst_s.miss++;
        pst_s.cr_dodge++;
        pst_s.cur_round_swings++;
        return;
    }

    /* --- Player Miss --- */
    if (pst_contains(line, "You miss ") ||
        pst_contains(line, "You fire at ") ||
        pst_contains(line, "You flail at ") ||
        pst_contains(line, "You hurl at ") ||
        pst_contains(line, "You lash at ") ||
        pst_contains(line, "You lunge at ") ||
        pst_contains(line, "You shoot at ") ||
        pst_contains(line, "You swing at ") ||
        pst_contains(line, "You swipe at ") ||
        pst_contains(line, "You thrust at ") ||
        pst_contains(line, "You snap at ") ||
        pst_contains(line, "You spray at ") ||
        pst_contains(line, "You double-shoot at ") ||
        pst_contains(line, " and barely miss ") ||
        pst_contains(line, "Your inaccurate ") ||
        pst_contains(line, "You stumble and ") ||
        pst_contains(line, "You slip and scramble") ||
        pst_contains(line, "You circle and ") ||
        pst_contains(line, "You advance, looking") ||
        pst_contains(line, "You jumpkick") ||
        pst_contains(line, "You punch") ||
        pst_contains(line, "You kick")) {
        /* Only count as miss if the line does NOT also contain " damage!" (already handled above) */
        if (!pst_contains(line, " damage!")) {
            pst_check_round_gap();
            pst_s.miss++;
            pst_s.cr_miss++;
            pst_s.cur_round_swings++;
        }
        return;
    }

}

/* ---- ANSI stripper + line extraction ---- */

static void pst_fsm_cb(const char *line, void *user)
{
    (void)user;
    pst_parse_line(line);
}

static void pst_feed(const char *data, int len)
{
    if (!pst_fsm_inited) {
        line_fsm_init(&pst_fsm, pst_lbuf, (int)sizeof(pst_lbuf),
                      LINE_FSM_STRIP_INLINE, LINE_FSM_TERM_CRLF,
                      pst_fsm_cb, NULL);
        pst_fsm_inited = 1;
    }
    line_fsm_feed(&pst_fsm, data, len);
}

/* ---- Formatting helpers ---- */

static void pst_fmt_num(char *out, int sz, __int64 v) {
    if (v >= 1000000) _snprintf(out, sz, "%lld,%03lld,%03lld", v / 1000000, (v / 1000) % 1000, v % 1000);
    else if (v >= 1000) _snprintf(out, sz, "%lld,%03lld", v / 1000, v % 1000);
    else _snprintf(out, sz, "%lld", v);
    out[sz - 1] = 0;
}

static void pst_fmt_time(char *out, int sz, DWORD elapsed_ms) {
    int s = (int)(elapsed_ms / 1000);
    int h = s / 3600; s %= 3600;
    int m = s / 60; s %= 60;
    _snprintf(out, sz, "%02d:%02d:%02d", h, m, s);
    out[sz - 1] = 0;
}

/* ---- Reset functions ---- */

static void pst_reset_exp(void) {
    pst_s.exp_start_tick = GetTickCount();
    pst_s.exp_gained = 0;
    pst_s.kills = 0;
    vsb_exp_rate = 0;
}

static void pst_reset_combat(void) {
    pst_s.miss = 0;
    pst_atk_clear(&pst_s.hit);
    pst_atk_clear(&pst_s.crit);
    pst_atk_clear(&pst_s.extra);
    pst_atk_clear(&pst_s.backstab);
    pst_atk_clear(&pst_s.spell);
    pst_atk_clear(&pst_s.rnd);
    pst_s.cur_round_dmg = 0;
    pst_s.cur_round_swings = 0;
    pst_s.dodge = 0;
    pst_s.combat_start_tick = GetTickCount();
}

/* ---- Toggle ---- */

static void pst_toggle(void)
{
    pst_visible = !pst_visible;
    if (pst_visible) {
        int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
        if (pst_x < 1 && pst_y < 1) {
            pst_x = (float)(vp_w - (int)pst_w) / 2.0f;
            pst_y = (float)(vp_h - (int)pst_h) / 2.0f;
        }
        if (pst_s.exp_start_tick == 0) {
            pst_s.exp_start_tick = GetTickCount();
            pst_s.combat_start_tick = GetTickCount();
            pst_atk_clear(&pst_s.hit);
            pst_atk_clear(&pst_s.crit);
            pst_atk_clear(&pst_s.extra);
            pst_atk_clear(&pst_s.backstab);
            pst_atk_clear(&pst_s.spell);
            pst_atk_clear(&pst_s.rnd);
        }
    }
}

/* ---- Vulkan-rendered Player Stats Panel ---- */

static int pst_scaled = 0;
static void pst_draw(int vp_w, int vp_h)
{
    if (!pst_visible) return;
    if (!pst_scaled) { pst_w = 340*ui_scale; pst_h = 390*ui_scale; pst_scaled = 1; }

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);

    float x0 = pst_x, y0 = pst_y;
    float pw = pst_w, ph = pst_h;
    float x1 = x0 + pw, y1 = y0 + ph;

    /* Theme colors */
    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];
    float dmr = t->dim[0], dmg = t->dim[1], dmb = t->dim[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];

    /* ---- Panel background ---- */
    psolid(x0, y0, x1, y1,
           bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.96f,
           vp_w, vp_h);

    /* ---- Outer bevel: light top/left, dark bottom/right ---- */
    psolid(x0, y0, x1, y0 + 1.0f,
           bgr + 0.18f, bgg + 0.18f, bgb + 0.18f, 0.8f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1.0f, y1,
           bgr + 0.14f, bgg + 0.14f, bgb + 0.14f, 0.7f, vp_w, vp_h);
    psolid(x0, y1 - 1.0f, x1, y1,
           bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.9f, vp_w, vp_h);
    psolid(x1 - 1.0f, y0, x1, y1,
           bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.85f, vp_w, vp_h);

    /* Inner bevel (second line for depth) */
    psolid(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f,
           bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.5f, vp_w, vp_h);
    psolid(x0 + 1.0f, y0 + 1.0f, x0 + 2.0f, y1 - 1.0f,
           bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.4f, vp_w, vp_h);

    int titlebar_h = ch + 10;
    int section_h = ch + 6;
    int row_h = ch + 4;
    int pad = 8;
    int val_x = (int)x0 + pad + 11 * cw; /* value column start */

    /* ---- Title bar ---- */
    float tb_y0 = y0 + 2.0f, tb_y1 = y0 + (float)titlebar_h;
    /* Title bar background — accent tinted */
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y1,
           acr * 0.25f + bgr * 0.5f, acg * 0.25f + bgg * 0.5f, acb * 0.25f + bgb * 0.5f, 0.95f,
           vp_w, vp_h);
    /* Title bar gloss highlight (top half lighter) */
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y0 + (float)(titlebar_h / 2),
           1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
    /* Title bar bottom edge */
    psolid(x0 + 2.0f, tb_y1 - 1.0f, x1 - 2.0f, tb_y1,
           acr, acg, acb, 0.5f, vp_w, vp_h);

    /* Title text */
    int title_tx = (int)x0 + pad;
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    ptext(title_tx + 1, title_ty + 1, "Player Statistics", 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(title_tx, title_ty, "Player Statistics", txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Close button [X] */
    int close_tx = (int)x1 - pad - cw;
    ptext(close_tx, title_ty, "X", 1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);

    /* ---- Content area ---- */
    int cy = (int)tb_y1 + 2;
    char buf[64], line[64];

    /* ==== Experience Section Header ==== */
    float sh_y0 = (float)cy, sh_y1 = (float)(cy + section_h);
    /* Section header background — subtle accent band */
    psolid(x0 + 3.0f, sh_y0, x1 - 3.0f, sh_y1,
           acr * 0.15f + bgr * 0.6f, acg * 0.15f + bgg * 0.6f, acb * 0.15f + bgb * 0.6f, 0.9f,
           vp_w, vp_h);
    /* Top highlight for plastic/gloss look */
    psolid(x0 + 3.0f, sh_y0, x1 - 3.0f, sh_y0 + 1.0f,
           1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
    /* Bottom shadow */
    psolid(x0 + 3.0f, sh_y1 - 1.0f, x1 - 3.0f, sh_y1,
           0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);

    int sh_ty = cy + (section_h - ch) / 2;
    ptext((int)x0 + pad, sh_ty, "\xC4\xC4 Experience \xC4\xC4\xC4\xC4\xC4\xC4", acr, acg, acb, vp_w, vp_h, cw, ch);
    ptext((int)x1 - pad - 7 * cw, sh_ty, "[Reset]", acr * 0.7f + 0.3f, acg * 0.7f + 0.3f, acb * 0.7f + 0.3f, vp_w, vp_h, cw, ch);
    cy += section_h;

    /* Duration */
    DWORD now = GetTickCount();
    DWORD elapsed = now - pst_s.exp_start_tick;
    pst_fmt_time(buf, sizeof(buf), elapsed);
    ptext((int)x0 + pad, cy + 2, "Duration:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(val_x, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
    cy += row_h;

    /* Exp Made */
    pst_fmt_num(buf, sizeof(buf), pst_s.exp_gained);
    ptext((int)x0 + pad, cy + 2, "Exp Made:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(val_x, cy + 2, buf, 0.3f, 1.0f, 0.3f, vp_w, vp_h, cw, ch);
    cy += row_h;

    /* Exp Rate */
    __int64 exp_rate = 0;
    if (elapsed > 5000) {
        exp_rate = pst_s.exp_gained * 3600000LL / (__int64)elapsed;
        if (exp_rate >= 1000000LL) {
            _snprintf(buf, sizeof(buf), "%lld m/hr", exp_rate / 1000000LL);
        } else {
            pst_fmt_num(buf, sizeof(buf), exp_rate);
            strcat(buf, " /hr");
        }
    } else { strcpy(buf, "---"); }
    ptext((int)x0 + pad, cy + 2, "Exp Rate:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(val_x, cy + 2, buf, 0.3f, 0.9f, 1.0f, vp_w, vp_h, cw, ch);
    cy += row_h;

    /* Exp Needed (from the exp bar's memory read) */
    if (vxb_needed > 0) {
        pst_fmt_num(buf, sizeof(buf), vxb_needed);
    } else { strcpy(buf, "---"); }
    ptext((int)x0 + pad, cy + 2, "Exp Need:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(val_x, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
    cy += row_h;

    /* Time to Level */
    if (exp_rate > 0 && vxb_needed > 0) {
        __int64 secs = vxb_needed * 3600LL / exp_rate;
        if (secs < 0) secs = 0;
        __int64 yrs = secs / (365LL * 24 * 3600);
        __int64 rem = secs % (365LL * 24 * 3600);
        __int64 mos = rem / (30LL * 24 * 3600);
        rem %= (30LL * 24 * 3600);
        __int64 days = rem / (24 * 3600);
        rem %= (24 * 3600);
        __int64 hrs = rem / 3600;
        rem %= 3600;
        __int64 mins = rem / 60;
        __int64 sec = rem % 60;

        if (yrs > 20) {
            strcpy(buf, "a millennium");
        } else if (yrs > 0) {
            if (mos > 0)
                _snprintf(buf, sizeof(buf), "%lldy %lldmo", yrs, mos);
            else
                _snprintf(buf, sizeof(buf), "%lld years", yrs);
        } else if (mos > 0) {
            if (days > 0)
                _snprintf(buf, sizeof(buf), "%lldmo %lldd", mos, days);
            else
                _snprintf(buf, sizeof(buf), "%lld months", mos);
        } else if (days > 0) {
            _snprintf(buf, sizeof(buf), "%lldd %lldh", days, hrs);
        } else if (hrs > 0) {
            _snprintf(buf, sizeof(buf), "%lldh %lldm", hrs, mins);
        } else if (mins > 0) {
            _snprintf(buf, sizeof(buf), "%lldm %llds", mins, sec);
        } else {
            _snprintf(buf, sizeof(buf), "%lld seconds", sec);
        }
        buf[sizeof(buf) - 1] = 0;
    } else { strcpy(buf, "---"); }
    ptext((int)x0 + pad, cy + 2, "To Level:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    /* Color: green if under 1hr, yellow if under 1day, dim otherwise */
    {
        float tlr = dmr, tlg = dmg, tlb = dmb;
        if (exp_rate > 0 && vxb_needed > 0) {
            __int64 secs = vxb_needed * 3600LL / exp_rate;
            if (secs < 3600) { tlr = 0.3f; tlg = 1.0f; tlb = 0.3f; }
            else if (secs < 86400) { tlr = 1.0f; tlg = 0.85f; tlb = 0.2f; }
        }
        ptext(val_x, cy + 2, buf, tlr, tlg, tlb, vp_w, vp_h, cw, ch);
    }
    cy += row_h;

    /* Kills */
    _snprintf(buf, sizeof(buf), "%d", pst_s.kills);
    ptext((int)x0 + pad, cy + 2, "Kills:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(val_x, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
    cy += row_h + 4;

    /* ==== Accuracy Section Header ==== */
    sh_y0 = (float)cy; sh_y1 = (float)(cy + section_h);
    psolid(x0 + 3.0f, sh_y0, x1 - 3.0f, sh_y1,
           acr * 0.15f + bgr * 0.6f, acg * 0.15f + bgg * 0.6f, acb * 0.15f + bgb * 0.6f, 0.9f,
           vp_w, vp_h);
    psolid(x0 + 3.0f, sh_y0, x1 - 3.0f, sh_y0 + 1.0f,
           1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
    psolid(x0 + 3.0f, sh_y1 - 1.0f, x1 - 3.0f, sh_y1,
           0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);

    sh_ty = cy + (section_h - ch) / 2;
    ptext((int)x0 + pad, sh_ty, "\xC4\xC4 Accuracy \xC4\xC4\xC4\xC4\xC4\xC4\xC4", acr, acg, acb, vp_w, vp_h, cw, ch);
    ptext((int)x1 - pad - 7 * cw, sh_ty, "[Reset]", acr * 0.7f + 0.3f, acg * 0.7f + 0.3f, acb * 0.7f + 0.3f, vp_w, vp_h, cw, ch);
    cy += section_h;

    /* Column headers */
    int col_pct = (int)x0 + pad + 5 * cw;
    int col_rng = (int)x0 + pad + 11 * cw;
    int col_avg = (int)x0 + pad + 24 * cw;
    ptext(col_pct, cy + 2, "%", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(col_rng, cy + 2, "Range", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    ptext(col_avg, cy + 2, "Avg", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    cy += row_h;

    /* Combat accuracy rows — BS separated from regular hit/miss */
    int melee_swings = pst_s.miss + pst_s.hit.n + pst_s.crit.n + pst_s.extra.n + pst_s.spell.n;
    if (melee_swings == 0) melee_swings = 1;

    struct { const char *name; int count; pst_atk_t *atk; float r, g, b; } arows[] = {
        {"Miss", pst_s.miss,       NULL,             1.0f, 0.35f, 0.35f},
        {"Hit",  pst_s.hit.n,      &pst_s.hit,       txr,  txg,   txb},
        {"Crit", pst_s.crit.n,     &pst_s.crit,      1.0f, 0.85f, 0.2f},
        {"Xtra", pst_s.extra.n,    &pst_s.extra,      0.3f, 0.9f,  1.0f},
        {"Spel", pst_s.spell.n,    &pst_s.spell,      0.85f, 0.4f, 1.0f},
    };

    for (int i = 0; i < 5; i++) {
        int pct = arows[i].count * 100 / melee_swings;
        float rr = arows[i].r, rg = arows[i].g, rb = arows[i].b;

        if (i & 1) {
            psolid(x0 + 3.0f, (float)cy, x1 - 3.0f, (float)(cy + row_h),
                   1.0f, 1.0f, 1.0f, 0.02f, vp_w, vp_h);
        }

        ptext((int)x0 + pad, cy + 2, arows[i].name, rr, rg, rb, vp_w, vp_h, cw, ch);
        _snprintf(buf, sizeof(buf), "%3d%%", pct);
        ptext(col_pct, cy + 2, buf, dmr, dmg, dmb, vp_w, vp_h, cw, ch);

        if (arows[i].atk && arows[i].atk->n > 0) {
            int mn = arows[i].atk->min < 0 ? 0 : arows[i].atk->min;
            int avg = (int)(arows[i].atk->total / arows[i].atk->n);
            _snprintf(buf, sizeof(buf), "%d-%d", mn, arows[i].atk->max);
            ptext(col_rng, cy + 2, buf, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
            _snprintf(buf, sizeof(buf), "%d", avg);
            ptext(col_avg, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
        }
        cy += row_h;
    }

    /* Round row */
    ptext((int)x0 + pad, cy + 2, "Rnd", 0.3f, 1.0f, 0.5f, vp_w, vp_h, cw, ch);
    if (pst_s.rnd.n > 0) {
        int mn = pst_s.rnd.min < 0 ? 0 : pst_s.rnd.min;
        int avg = (int)(pst_s.rnd.total / pst_s.rnd.n);
        _snprintf(buf, sizeof(buf), "%d-%d", mn, pst_s.rnd.max);
        ptext(col_rng, cy + 2, buf, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        _snprintf(buf, sizeof(buf), "%d", avg);
        ptext(col_avg, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
    } else {
        ptext(col_rng, cy + 2, "---", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    }
    cy += row_h + 4;

    /* ==== Backstab Section ==== */
    if (pst_s.backstab.n > 0) {
        sh_y0 = (float)cy; sh_y1 = (float)(cy + section_h);
        psolid(x0 + 3.0f, sh_y0, x1 - 3.0f, sh_y1,
               acr * 0.15f + bgr * 0.6f, acg * 0.15f + bgg * 0.6f, acb * 0.15f + bgb * 0.6f, 0.9f,
               vp_w, vp_h);
        psolid(x0 + 3.0f, sh_y0, x1 - 3.0f, sh_y0 + 1.0f,
               1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
        psolid(x0 + 3.0f, sh_y1 - 1.0f, x1 - 3.0f, sh_y1,
               0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        sh_ty = cy + (section_h - ch) / 2;
        ptext((int)x0 + pad, sh_ty, "\xC4\xC4 Backstab \xC4\xC4\xC4\xC4\xC4\xC4\xC4", acr, acg, acb, vp_w, vp_h, cw, ch);
        cy += section_h;

        /* BS count */
        _snprintf(buf, sizeof(buf), "%d", pst_s.backstab.n);
        ptext((int)x0 + pad, cy + 2, "Landed:", 1.0f, 0.6f, 0.1f, vp_w, vp_h, cw, ch);
        ptext(val_x, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
        cy += row_h;

        /* BS damage range + avg */
        {
            int mn = pst_s.backstab.min < 0 ? 0 : pst_s.backstab.min;
            int avg = (int)(pst_s.backstab.total / pst_s.backstab.n);
            _snprintf(buf, sizeof(buf), "%d-%d", mn, pst_s.backstab.max);
            ptext((int)x0 + pad, cy + 2, "Range:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
            ptext(val_x, cy + 2, buf, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
            _snprintf(buf, sizeof(buf), "Avg: %d", avg);
            ptext(val_x + 10 * cw, cy + 2, buf, txr, txg, txb, vp_w, vp_h, cw, ch);
        }
        cy += row_h;
    }

    /* Auto-size panel height to content */
    pst_h = (float)(cy - (int)y0 + 6);
}

/* ---- MegaMUD Settings Window ---- */
/* Full settings dialog rendered in Vulkan, themed, with tabs + widgets.
 * Reads/writes MegaMUD struct offsets directly. */


/* Tab system — two rows */
#define STW_TAB_COUNT 16
static int stw_tab = 0;  /* active tab index */

static const char *stw_tab_names[STW_TAB_COUNT] = {
    "General", "Display", "Toolbar", "BBS", "Events", "Spells", "Combat", "PvP",
    "Party", "Cash", "Sounds", "Auto-roam", "Talk", "Statline", "Other", "MajorSLOP"
};

/* Scroll offset for scrollable sections */
static int stw_manual_scroll = 0;
static int stw_auto_scroll = 0;

/* ---- Widget drawing primitives ---- */

/* Draw a themed section header with label */
static void stw_section(float x0, float x1, int *cy, const char *label,
                        const ui_theme_t *t, int vp_w, int vp_h, int cw, int ch,
                        void (*psolid)(float,float,float,float,float,float,float,float,int,int),
                        void (*ptext)(int,int,const char*,float,float,float,int,int,int,int))
{
    float y = (float)*cy;
    /* Label */
    ptext((int)x0, (int)y + 2, label, t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    /* Underline */
    int lw = (int)strlen(label) * cw;
    psolid(x0, y + ch + 3, x1, y + ch + 4,
           t->accent[0] * 0.5f, t->accent[1] * 0.5f, t->accent[2] * 0.5f, 0.6f, vp_w, vp_h);
    *cy += ch + 8;
}

/* Draw a checkbox. Returns 1 if clicked. */
static int stw_checkbox(float x0, int *cy, const char *label, int checked,
                        int mx, int my, int clicked,
                        const ui_theme_t *t, int vp_w, int vp_h, int cw, int ch,
                        void (*psolid)(float,float,float,float,float,float,float,float,int,int),
                        void (*ptext)(int,int,const char*,float,float,float,int,int,int,int))
{
    int bx = (int)x0;
    int by = *cy + 1;
    int bs = ch;
    int hit = 0;

    /* Box background */
    psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + bs),
           t->bg[0] + 0.08f, t->bg[1] + 0.08f, t->bg[2] + 0.08f, 1.0f, vp_w, vp_h);
    /* Box border */
    psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + 1),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);
    psolid((float)bx, (float)(by + bs - 1), (float)(bx + bs), (float)(by + bs),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);
    psolid((float)bx, (float)by, (float)(bx + 1), (float)(by + bs),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);
    psolid((float)(bx + bs - 1), (float)by, (float)(bx + bs), (float)(by + bs),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);

    /* Check mark */
    if (checked) {
        int m = 3;
        psolid((float)(bx + m), (float)(by + m), (float)(bx + bs - m), (float)(by + bs - m),
               t->accent[0], t->accent[1], t->accent[2], 0.9f, vp_w, vp_h);
    }

    /* Label */
    ptext(bx + bs + 6, *cy + 2, label, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);

    /* Hit test */
    int label_w = (int)strlen(label) * cw + bs + 6;
    if (clicked && mx >= bx && mx < bx + label_w && my >= by && my < by + bs + 2)
        hit = 1;

    *cy += ch + 5;
    return hit;
}

/* Draw a radio button. Returns 1 if clicked. */
static int stw_radio(float x0, int *cy, const char *label, int selected,
                     int mx, int my, int clicked,
                     const ui_theme_t *t, int vp_w, int vp_h, int cw, int ch,
                     void (*psolid)(float,float,float,float,float,float,float,float,int,int),
                     void (*ptext)(int,int,const char*,float,float,float,int,int,int,int))
{
    int bx = (int)x0;
    int by = *cy + 1;
    int bs = ch;
    int hit = 0;

    /* Outer circle (approximated with filled square + smaller inner) */
    psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + bs),
           t->bg[0] + 0.08f, t->bg[1] + 0.08f, t->bg[2] + 0.08f, 1.0f, vp_w, vp_h);
    /* Border */
    psolid((float)bx, (float)by, (float)(bx + bs), (float)(by + 1),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);
    psolid((float)bx, (float)(by + bs - 1), (float)(bx + bs), (float)(by + bs),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);
    psolid((float)bx, (float)by, (float)(bx + 1), (float)(by + bs),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);
    psolid((float)(bx + bs - 1), (float)by, (float)(bx + bs), (float)(by + bs),
           t->dim[0], t->dim[1], t->dim[2], 0.6f, vp_w, vp_h);

    /* Filled dot if selected */
    if (selected) {
        int m = 3;
        psolid((float)(bx + m), (float)(by + m), (float)(bx + bs - m), (float)(by + bs - m),
               t->accent[0], t->accent[1], t->accent[2], 0.95f, vp_w, vp_h);
    }

    /* Label */
    ptext(bx + bs + 6, *cy + 2, label, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);

    /* Hit test */
    int label_w = (int)strlen(label) * cw + bs + 6;
    if (clicked && mx >= bx && mx < bx + label_w && my >= by && my < by + bs + 2)
        hit = 1;

    *cy += ch + 5;
    return hit;
}

/* Draw a label: value pair */
static void stw_label_val(float x0, int *cy, const char *label, const char *value,
                          const ui_theme_t *t, int vp_w, int vp_h, int cw, int ch,
                          void (*ptext)(int,int,const char*,float,float,float,int,int,int,int))
{
    ptext((int)x0, *cy + 2, label, t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
    int vx = (int)x0 + (int)strlen(label) * cw + cw;
    ptext(vx, *cy + 2, value, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    *cy += ch + 5;
}

/* ---- Settings toggle offsets — Manual and Auto mode defaults ---- */
#define STW_TOGGLE_COUNT 11
static const struct { const char *name; int off; } stw_toggles[STW_TOGGLE_COUNT] = {
    { "Auto-Combat",    0x4D00 },
    { "Auto-Nuke",      0x4D04 },
    { "Auto-Heal/Rest", 0x4D08 },
    { "Auto-Bless",     0x4D0C },
    { "Auto-Light",     0x4D10 },
    { "Auto-Cash",      0x4D14 },
    { "Auto-Get",       0x4D18 },
    { "Auto-Search",    0x4D1C },
    { "Auto-Sneak",     0x4D20 },
    { "Auto-Hide",      0x4D24 },
    { "Auto-Track",     0x4D28 },
};

/* Manual defaults at 0x4D2C, Auto defaults at 0x4D58 (same 11 slots) */
#define STW_DEF_MANUAL_BASE 0x4D2C
#define STW_DEF_AUTO_BASE   0x4D58

/* Other General tab offsets */
#define STW_OFF_START_TASK   0x4D84
#define STW_OFF_AUTO_CONNECT 0x8AA0
#define STW_OFF_NO_MODE_DEFS 0x4D84  /* shares with START_TASK? need to verify */

static void stw_toggle(void)
{
    stw_visible = !stw_visible;
    if (stw_visible && !stw_scaled) {
        stw_w = 620 * ui_scale;
        stw_h = 480 * ui_scale;
        int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
        stw_x = (float)(vp_w - (int)stw_w) / 2.0f;
        stw_y = (float)(vp_h - (int)stw_h) / 2.0f;
        stw_scaled = 1;
    }
}

/* ---- Settings Window Draw ---- */

static void stw_draw(int vp_w, int vp_h, int mx, int my, int clicked)
{
    if (!stw_visible) return;

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);

    float x0 = stw_x, y0 = stw_y;
    float x1 = x0 + stw_w, y1 = y0 + stw_h;

    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];

    int pad = (int)(10 * ui_scale);
    int titlebar_h = ch + 10;
    int tab_h = ch + 8;

    /* ---- Panel background ---- */
    psolid(x0, y0, x1, y1,
           bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.97f, vp_w, vp_h);

    /* ---- Outer bevel ---- */
    psolid(x0, y0, x1, y0 + 1, bgr + 0.18f, bgg + 0.18f, bgb + 0.18f, 0.8f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1, y1, bgr + 0.14f, bgg + 0.14f, bgb + 0.14f, 0.7f, vp_w, vp_h);
    psolid(x0, y1 - 1, x1, y1, bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.9f, vp_w, vp_h);
    psolid(x1 - 1, y0, x1, y1, bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.85f, vp_w, vp_h);
    /* Inner bevel */
    psolid(x0 + 1, y0 + 1, x1 - 1, y0 + 2, bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.5f, vp_w, vp_h);
    psolid(x0 + 1, y0 + 1, x0 + 2, y1 - 1, bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.4f, vp_w, vp_h);

    /* ---- Title bar ---- */
    float tb_y0 = y0 + 2, tb_y1 = y0 + titlebar_h;
    psolid(x0 + 2, tb_y0, x1 - 2, tb_y1,
           acr * 0.25f + bgr * 0.5f, acg * 0.25f + bgg * 0.5f, acb * 0.25f + bgb * 0.5f, 0.95f,
           vp_w, vp_h);
    psolid(x0 + 2, tb_y0, x1 - 2, tb_y0 + titlebar_h / 2,
           1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
    psolid(x0 + 2, tb_y1 - 1, x1 - 2, tb_y1,
           acr, acg, acb, 0.5f, vp_w, vp_h);

    /* Title text */
    int title_tx = (int)x0 + pad;
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    ptext(title_tx + 1, title_ty + 1, "MegaMUD Settings", 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(title_tx, title_ty, "MegaMUD Settings", txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Close [X] */
    int close_tx = (int)x1 - pad - cw;
    ptext(close_tx, title_ty, "X", 1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);

    /* ---- Tab bars (two rows of 8) ---- */
    int tabs_y = (int)tb_y1 + 2;
    int tab_w = ((int)stw_w - 4) / 8;

    for (int row = 0; row < 2; row++) {
        int ry = tabs_y + row * tab_h;
        for (int col = 0; col < 8; col++) {
            int ti = row * 8 + col;
            if (ti >= STW_TAB_COUNT) break;
            int tx = (int)x0 + 2 + col * tab_w;
            int tw = (col == 7) ? (int)x1 - 2 - tx : tab_w; /* last tab fills remainder */
            int active = (ti == stw_tab);

            /* Tab background */
            if (active) {
                psolid((float)tx, (float)ry, (float)(tx + tw), (float)(ry + tab_h),
                       acr * 0.3f + bgr * 0.5f, acg * 0.3f + bgg * 0.5f, acb * 0.3f + bgb * 0.5f, 0.95f,
                       vp_w, vp_h);
                /* Active indicator line at bottom */
                psolid((float)tx, (float)(ry + tab_h - 2), (float)(tx + tw), (float)(ry + tab_h),
                       acr, acg, acb, 0.9f, vp_w, vp_h);
            } else {
                psolid((float)tx, (float)ry, (float)(tx + tw), (float)(ry + tab_h),
                       bgr + 0.02f, bgg + 0.02f, bgb + 0.02f, 0.85f, vp_w, vp_h);
            }

            /* Tab border (right edge) */
            psolid((float)(tx + tw - 1), (float)ry, (float)(tx + tw), (float)(ry + tab_h),
                   bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.4f, vp_w, vp_h);

            /* Tab label — centered */
            int lbl_len = (int)strlen(stw_tab_names[ti]);
            int lbl_x = tx + (tw - lbl_len * cw) / 2;
            int lbl_y = ry + (tab_h - ch) / 2;
            ptext(lbl_x, lbl_y, stw_tab_names[ti],
                  active ? txr : t->dim[0], active ? txg : t->dim[1], active ? txb : t->dim[2],
                  vp_w, vp_h, cw, ch);

            /* Tab click */
            if (clicked && mx >= tx && mx < tx + tw && my >= ry && my < ry + tab_h)
                stw_tab = ti;
        }
    }

    /* ---- Content area ---- */
    int content_y = tabs_y + 2 * tab_h + 4;
    int content_x = (int)x0 + pad;
    int content_x1 = (int)x1 - pad;
    int cy = content_y;

    /* Get struct base for reading/writing */
    unsigned int sbase = api ? api->get_struct_base() : 0;
    unsigned char *base = sbase ? (unsigned char *)(uintptr_t)sbase : NULL;

    if (stw_tab == 0) {
        /* ==== General Tab ==== */

        /* ---- Directories ---- */
        stw_section((float)content_x, (float)content_x1, &cy, "Directories",
                    t, vp_w, vp_h, cw, ch, psolid, ptext);

        stw_label_val((float)content_x, &cy, "Data Files: ", "C:\\MegaMUD",
                      t, vp_w, vp_h, cw, ch, ptext);
        cy += 4;

        /* ---- Initial ---- */
        stw_section((float)content_x, (float)content_x1, &cy, "Initial",
                    t, vp_w, vp_h, cw, ch, psolid, ptext);

        if (base) {
            int start_task = *(int *)(base + STW_OFF_START_TASK);
            /* Radio: Do nothing (0), Begin looping (1), Begin auto-roaming (2) */
            if (stw_radio((float)content_x, &cy, "Do nothing", start_task == 0,
                          mx, my, clicked, t, vp_w, vp_h, cw, ch, psolid, ptext))
                *(int *)(base + STW_OFF_START_TASK) = 0;
            if (stw_radio((float)content_x, &cy, "Begin looping at...", start_task == 1,
                          mx, my, clicked, t, vp_w, vp_h, cw, ch, psolid, ptext))
                *(int *)(base + STW_OFF_START_TASK) = 1;
            if (stw_radio((float)content_x, &cy, "Begin auto-roaming", start_task == 2,
                          mx, my, clicked, t, vp_w, vp_h, cw, ch, psolid, ptext))
                *(int *)(base + STW_OFF_START_TASK) = 2;

            cy += 4;

            int auto_conn = *(int *)(base + STW_OFF_AUTO_CONNECT);
            if (stw_checkbox((float)content_x, &cy, "Auto-connect when INI loaded", auto_conn,
                             mx, my, clicked, t, vp_w, vp_h, cw, ch, psolid, ptext))
                *(int *)(base + STW_OFF_AUTO_CONNECT) = auto_conn ? 0 : 1;
        }

        cy += 8;

        /* ---- Manual-Mode Defaults / Auto-Mode Defaults side by side ---- */
        int half_w = ((int)stw_w - pad * 3) / 2;
        int left_x = content_x;
        int right_x = content_x + half_w + pad;

        /* Section headers */
        int sec_y = cy;
        stw_section((float)left_x, (float)(left_x + half_w), &cy, "Manual-Mode Defaults",
                    t, vp_w, vp_h, cw, ch, psolid, ptext);
        int cy_right = sec_y;
        stw_section((float)right_x, (float)(right_x + half_w), &cy_right, "Auto-Mode Defaults",
                    t, vp_w, vp_h, cw, ch, psolid, ptext);

        if (base) {
            int cy_left = cy;
            cy_right = cy;

            for (int i = 0; i < STW_TOGGLE_COUNT; i++) {
                /* Manual default (profile 1) */
                int m_val = *(int *)(base + STW_DEF_MANUAL_BASE + i * 4);
                if (stw_checkbox((float)left_x, &cy_left, stw_toggles[i].name, m_val,
                                 mx, my, clicked, t, vp_w, vp_h, cw, ch, psolid, ptext))
                    *(int *)(base + STW_DEF_MANUAL_BASE + i * 4) = m_val ? 0 : 1;

                /* Auto default (profile 2) */
                int a_val = *(int *)(base + STW_DEF_AUTO_BASE + i * 4);
                if (stw_checkbox((float)right_x, &cy_right, stw_toggles[i].name, a_val,
                                 mx, my, clicked, t, vp_w, vp_h, cw, ch, psolid, ptext))
                    *(int *)(base + STW_DEF_AUTO_BASE + i * 4) = a_val ? 0 : 1;
            }

            cy = cy_left > cy_right ? cy_left : cy_right;
        }
    } else {
        /* Placeholder for other tabs */
        ptext(content_x, cy + 20, "(coming soon)",
              t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
    }

    /* Auto-size height */
    float min_h = (float)(cy - (int)y0 + pad + 4);
    if (min_h > stw_h) stw_h = min_h;
}

/* ---- Settings window hit test + interaction ---- */

static int stw_hit_test(int mx, int my)
{
    if (!stw_visible) return 0;
    return mx >= (int)stw_x && mx < (int)(stw_x + stw_w) &&
           my >= (int)stw_y && my < (int)(stw_y + stw_h);
}

/* ---- Vulkan Floating Text (VFT) plugin ---- */
#include "vft_text.c"

/* ---- Backscroll Panel — glossy panel with 4 mode tabs + save ---- */

static void bsp_toggle(int vp_w, int vp_h)
{
    bsp_visible = !bsp_visible;
    if (bsp_visible) {
        bsp_w = (float)(vp_w - 80);
        bsp_h = (float)(vp_h - 60);
        if (bsp_w < 400) bsp_w = 400;
        if (bsp_h < 300) bsp_h = 300;
        bsp_x = (float)(vp_w - (int)bsp_w) / 2.0f;
        bsp_y = (float)(vp_h - (int)bsp_h) / 2.0f;
        bsp_scroll = 0; /* 0 = bottom (most recent) */
        bsp_sel_active = 0;
        bsp_selecting = 0;
        vkw_focus = -1;
        /* Freeze snapshot of all buffers */
        bsp_snap_cb_count = cb_count;
        bsp_snap_cb_head = cb_head;
        bsp_snap_bs_len = bs_len;
        bsp_snap_bs_head = bs_head;
        bsp_snap_ra_len = ra_len;
        bsp_snap_ra_head = ra_head;
        /* Count plain/raw lines from snapshot */
        bsp_snap_plain_lines = 0;
        if (bsp_snap_bs_len > 0) {
            bsp_snap_plain_lines = 1;
            int start = (bsp_snap_bs_head - bsp_snap_bs_len + BACKSCROLL_SIZE) % BACKSCROLL_SIZE;
            for (int i = 0; i < bsp_snap_bs_len; i++)
                if (backscroll_buf[(start + i) % BACKSCROLL_SIZE] == '\n') bsp_snap_plain_lines++;
        }
        bsp_snap_raw_lines = 0;
        if (bsp_snap_ra_len > 0) {
            bsp_snap_raw_lines = 1;
            int start = (bsp_snap_ra_head - bsp_snap_ra_len + RAW_ANSI_SIZE) % RAW_ANSI_SIZE;
            for (int i = 0; i < bsp_snap_ra_len; i++)
                if (raw_ansi_buf[(start + i) % RAW_ANSI_SIZE] == '\n') bsp_snap_raw_lines++;
        }
    }
}

/* Get a specific plain-text line from snapshot. Writes into buf, returns length. */
static int bsp_get_plain_line(int line_idx, char *buf, int max_len)
{
    if (!bs_lock_init || bsp_snap_bs_len == 0) return 0;
    int start = (bsp_snap_bs_head - bsp_snap_bs_len + BACKSCROLL_SIZE) % BACKSCROLL_SIZE;
    int cur_line = 0, pos = 0;
    for (int i = 0; i < bsp_snap_bs_len; i++) {
        char ch = backscroll_buf[(start + i) % BACKSCROLL_SIZE];
        if (ch == '\n') {
            if (cur_line == line_idx) { buf[pos] = 0; return pos; }
            cur_line++;
            pos = 0;
            continue;
        }
        if (cur_line == line_idx && pos < max_len - 1) buf[pos++] = ch;
    }
    buf[pos] = 0;
    return pos;
}

static int bsp_get_raw_line(int line_idx, char *buf, int max_len)
{
    if (!bs_lock_init || bsp_snap_ra_len == 0) return 0;
    int start = (bsp_snap_ra_head - bsp_snap_ra_len + RAW_ANSI_SIZE) % RAW_ANSI_SIZE;
    int cur_line = 0, pos = 0;
    for (int i = 0; i < bsp_snap_ra_len; i++) {
        char ch = raw_ansi_buf[(start + i) % RAW_ANSI_SIZE];
        if (ch == '\n') {
            if (cur_line == line_idx) { buf[pos] = 0; return pos; }
            cur_line++;
            pos = 0;
            continue;
        }
        if (cur_line == line_idx && pos < max_len - 1) {
            if ((unsigned char)ch == 0x1B) {
                if (pos < max_len - 4) { buf[pos++] = 'E'; buf[pos++] = 'S'; buf[pos++] = 'C'; }
            } else if (ch >= 0x20 || ch == '\t') {
                buf[pos++] = ch;
            }
        }
    }
    buf[pos] = 0;
    return pos;
}

/* Get a colored cell line from snapshot */
static ap_cell_t *bsp_get_cb_line(int idx)
{
    if (idx < 0 || idx >= bsp_snap_cb_count) return NULL;
    int ring_idx = (bsp_snap_cb_head - bsp_snap_cb_count + idx + CB_MAX_LINES) % CB_MAX_LINES;
    return cb_lines[ring_idx];
}

/* ---- Save helpers ---- */

static void bsp_save_plain_file(FILE *fp)
{
    int text_len = 0;
    char *text = bs_get_text(&text_len);
    if (text) { fwrite(text, 1, text_len, fp); free(text); }
}

static void bsp_save_raw_file(FILE *fp)
{
    int text_len = 0;
    char *text = ra_get_text(&text_len);
    if (text) { fwrite(text, 1, text_len, fp); free(text); }
}

static void bsp_write_html_char(FILE *fp, unsigned char ch)
{
    if (ch == '<') fprintf(fp, "&lt;");
    else if (ch == '>') fprintf(fp, "&gt;");
    else if (ch == '&') fprintf(fp, "&amp;");
    else if (ch == '"') fprintf(fp, "&quot;");
    else if (ch >= 0x20 && ch < 0x7F) fputc(ch, fp);
    else if (ch == 0) fputc(' ', fp);
    else fprintf(fp, "&#x%04X;", cp437_to_unicode[ch]);
}

static void bsp_save_html_file(FILE *fp, rgb_t *pal)
{
    /* HTML header with embedded styling */
    fprintf(fp,
        "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n"
        "<style>\n"
        "body{background:#000;margin:0;padding:8px;}\n"
        "pre{font-family:'IBM Plex Mono','Cascadia Code','Consolas','Courier New',monospace;"
        "font-size:14px;line-height:1.3;margin:0;}\n"
        ".s{}\n");
    /* CRT scanlines if enabled */
    if (fx_scanline_mode) {
        fprintf(fp,
            ".scanlines{background:repeating-linear-gradient("
            "transparent,transparent 1px,rgba(0,0,0,0.15) 1px,rgba(0,0,0,0.15) 2px);"
            "position:fixed;top:0;left:0;width:100%%;height:100%%;pointer-events:none;z-index:99;}\n");
    }
    fprintf(fp, "</style></head><body>\n");
    if (fx_scanline_mode) fprintf(fp, "<div class=\"scanlines\"></div>\n");
    fprintf(fp, "<pre>");

    /* Iterate colored cell ring buffer */
    int prev_fg = -1, prev_bg = -1;
    int span_open = 0;
    for (int i = 0; i < bsp_snap_cb_count; i++) {
        ap_cell_t *row = bsp_get_cb_line(i);
        if (!row) continue;
        /* Find last non-space char to trim trailing spaces */
        int last_char = CB_COLS - 1;
        while (last_char >= 0 && row[last_char].ch <= 0x20 &&
               row[last_char].attr.bg == 0) last_char--;
        for (int c = 0; c <= last_char; c++) {
            ap_cell_t *cell = &row[c];
            int fg_idx = cell->attr.fg;
            if (cell->attr.bold && !cell->attr.fg256 && fg_idx < 8) fg_idx += 8;
            fg_idx &= 15;
            int bg_idx = cell->attr.bg & 15;
            if (cell->attr.reverse) { int tmp = fg_idx; fg_idx = bg_idx; bg_idx = tmp; }
            /* Check if color changed */
            if (fg_idx != prev_fg || bg_idx != prev_bg) {
                if (span_open) fprintf(fp, "</span>");
                rgb_t fc = pal[fg_idx], bc = pal[bg_idx];
                fprintf(fp, "<span style=\"color:#%02x%02x%02x",
                    (int)(fc.r*255.0f), (int)(fc.g*255.0f), (int)(fc.b*255.0f));
                if (bg_idx != 0)
                    fprintf(fp, ";background:#%02x%02x%02x",
                        (int)(bc.r*255.0f), (int)(bc.g*255.0f), (int)(bc.b*255.0f));
                fprintf(fp, "\">");
                span_open = 1;
                prev_fg = fg_idx;
                prev_bg = bg_idx;
            }
            bsp_write_html_char(fp, cell->ch);
        }
        if (span_open) { fprintf(fp, "</span>"); span_open = 0; prev_fg = -1; prev_bg = -1; }
        fprintf(fp, "\n");
    }
    if (span_open) fprintf(fp, "</span>");
    fprintf(fp, "</pre></body></html>\n");
}

static void bsp_save(void)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "backscroll";
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = vkt_hwnd;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;

    if (bsp_mode == BSP_MODE_PLAIN || bsp_mode == BSP_MODE_RAW_ANSI) {
        ofn.lpstrFilter = "Text Files\0*.txt\0All Files\0*.*\0";
        ofn.lpstrDefExt = "txt";
    } else {
        ofn.lpstrFilter = "HTML Files\0*.html\0All Files\0*.*\0";
        ofn.lpstrDefExt = "html";
    }
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameA(&ofn)) return;

    FILE *fp = fopen(path, "wb");
    if (!fp) return;

    switch (bsp_mode) {
        case BSP_MODE_PLAIN:      bsp_save_plain_file(fp); break;
        case BSP_MODE_ANSI_HTML:  bsp_save_html_file(fp, pal_classic); break;
        case BSP_MODE_THEME_HTML: bsp_save_html_file(fp, tinted_palette); break;
        case BSP_MODE_RAW_ANSI:   bsp_save_raw_file(fp); break;
    }
    fclose(fp);
    if (api) api->log("[vk_terminal] Backscroll saved to %s\n", path);
}

/* ---- Backscroll Panel: draw ---- */

static int bsp_scaled = 0;
static void bsp_draw(int vp_w, int vp_h)
{
    if (!bsp_visible) return;
    if (!bsp_scaled) { bsp_w = 680*ui_scale; bsp_h = 500*ui_scale; bsp_x = 80*ui_scale; bsp_y = 40*ui_scale; bsp_scaled = 1; }

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);

    float x0 = bsp_x, y0 = bsp_y;
    float x1 = bsp_x + bsp_w, y1 = bsp_y + bsp_h;
    int titlebar_h = ch + 10;
    int tab_h = ch + 8;
    int save_h = ch + 12;

    /* Drop shadow */
    psolid(x0 + 4, y0 + 4, x1 + 4, y1 + 4,
           0.0f, 0.0f, 0.0f, 0.4f, vp_w, vp_h);

    /* Panel background */
    psolid(x0, y0, x1, y1,
           t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.97f,
           vp_w, vp_h);

    /* Border */
    psolid(x0, y0, x1, y0 + 1, t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    psolid(x0, y1 - 1, x1, y1, t->accent[0], t->accent[1], t->accent[2], 0.4f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1, y1, t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
    psolid(x1 - 1, y0, x1, y1, t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);

    /* ---- Glossy title bar ---- */
    float tb_y0 = y0 + 1, tb_y1 = y0 + titlebar_h;
    float ar = t->accent[0], ag = t->accent[1], ab = t->accent[2];

    /* Accent-tinted background */
    psolid(x0 + 1, tb_y0, x1 - 1, tb_y1,
           ar * 0.25f + t->bg[0] * 0.75f, ag * 0.25f + t->bg[1] * 0.75f,
           ab * 0.25f + t->bg[2] * 0.75f, 0.95f, vp_w, vp_h);
    /* Gloss highlight — top half */
    float gloss_mid = tb_y0 + (tb_y1 - tb_y0) * 0.45f;
    psolid(x0 + 1, tb_y0, x1 - 1, gloss_mid,
           1.0f, 1.0f, 1.0f, 0.10f, vp_w, vp_h);
    /* Accent bottom edge */
    psolid(x0 + 1, tb_y1 - 1, x1 - 1, tb_y1,
           ar, ag, ab, 0.5f, vp_w, vp_h);

    /* Title text with drop shadow */
    int ttx = (int)x0 + 8, tty = (int)tb_y0 + (titlebar_h - ch) / 2;
    ptext(ttx + 1, tty + 1, "Backscroll", 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(ttx, tty, "Backscroll", t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);

    /* Close button X */
    int close_w = 20;
    int cbx = (int)(x1 - close_w - 4);
    ptext(cbx + 1, tty + 1, "X", 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(cbx, tty, "X", 1.0f, 0.4f, 0.4f, vp_w, vp_h, cw, ch);

    /* ---- Radio button tab bar ---- */
    float tab_y0 = tb_y1;
    float tab_y1 = tab_y0 + tab_h;

    /* Tab bar background */
    psolid(x0 + 1, tab_y0, x1 - 1, tab_y1,
           t->bg[0], t->bg[1], t->bg[2], 0.95f, vp_w, vp_h);

    static const char *tab_labels[] = { "Plain", "ANSI HTML", "Theme HTML", "Raw ANSI" };
    float tab_w = (bsp_w - 2) / 4.0f;
    for (int i = 0; i < 4; i++) {
        float tx0 = x0 + 1 + i * tab_w;
        float tx1 = tx0 + tab_w;
        if (i == bsp_mode) {
            /* Active tab — accent background + underline */
            psolid(tx0, tab_y0, tx1, tab_y1,
                   ar * 0.2f, ag * 0.2f, ab * 0.2f, 0.8f, vp_w, vp_h);
            psolid(tx0, tab_y1 - 2, tx1, tab_y1,
                   ar, ag, ab, 0.9f, vp_w, vp_h);
        }
        /* Tab label centered */
        int lw = (int)strlen(tab_labels[i]) * cw;
        int lx = (int)(tx0 + (tab_w - lw) / 2);
        int ly = (int)tab_y0 + (tab_h - ch) / 2;
        if (i == bsp_mode) {
            ptext(lx, ly, tab_labels[i], ar, ag, ab, vp_w, vp_h, cw, ch);
        } else {
            ptext(lx, ly, tab_labels[i], t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
        }
        /* Tab separator */
        if (i < 3) {
            psolid(tx1, tab_y0 + 3, tx1 + 1, tab_y1 - 3,
                   t->text[0], t->text[1], t->text[2], 0.15f, vp_w, vp_h);
        }
    }

    /* ---- Display area ---- */
    float disp_y0 = tab_y1 + 2;
    float disp_y1 = y1 - save_h - 2;
    int disp_h = (int)(disp_y1 - disp_y0);
    int vis_lines = disp_h / ch;
    if (vis_lines < 1) vis_lines = 1;

    /* Dark inset background */
    psolid(x0 + 4, disp_y0, x1 - 4, disp_y1,
           0.0f, 0.0f, 0.0f, 0.85f, vp_w, vp_h);

    int total_lines = 0;
    if (bsp_mode == BSP_MODE_PLAIN) total_lines = bsp_snap_plain_lines;
    else if (bsp_mode == BSP_MODE_RAW_ANSI) total_lines = bsp_snap_raw_lines;
    else total_lines = bsp_snap_cb_count; /* colored modes use snapshot */

    /* Clamp scroll */
    int max_scroll = total_lines - vis_lines;
    if (max_scroll < 0) max_scroll = 0;
    if (bsp_scroll > max_scroll) bsp_scroll = max_scroll;
    if (bsp_scroll < 0) bsp_scroll = 0;

    int start_line = total_lines - vis_lines - bsp_scroll;
    if (start_line < 0) start_line = 0;

    /* Cache layout for mouse handlers */
    bsp_disp_y0 = disp_y0;
    bsp_disp_y1 = disp_y1;
    bsp_disp_lx = (int)x0 + 8;
    bsp_disp_start = start_line;
    bsp_disp_vis = vis_lines;
    bsp_disp_total = total_lines;

    /* Render visible lines */
    for (int v = 0; v < vis_lines && (start_line + v) < total_lines; v++) {
        int line_idx = start_line + v;
        int ly = (int)disp_y0 + v * ch;
        int lx = (int)x0 + 8;

        if (bsp_mode == BSP_MODE_PLAIN) {
            char line[256];
            bsp_get_plain_line(line_idx, line, 256);
            ptext(lx, ly, line, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
        } else if (bsp_mode == BSP_MODE_RAW_ANSI) {
            char line[256];
            bsp_get_raw_line(line_idx, line, 256);
            ptext(lx, ly, line, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
        } else {
            /* Colored cell rendering (ANSI HTML / Theme HTML) */
            ap_cell_t *row = bsp_get_cb_line(line_idx);
            if (!row) continue;
            rgb_t *cpal = (bsp_mode == BSP_MODE_ANSI_HTML) ? pal_classic : tinted_palette;
            /* Find last non-space */
            int last = CB_COLS - 1;
            while (last >= 0 && row[last].ch <= 0x20 && row[last].attr.bg == 0) last--;
            /* Background color runs — render colored bg rects before text */
            {
                int bg_run_start = -1, prev_bg = 0;
                for (int c = 0; c <= last; c++) {
                    int bg_idx = row[c].attr.bg & 15;
                    if (row[c].attr.reverse) bg_idx = row[c].attr.fg & 15;
                    if (bg_idx != prev_bg || c == 0) {
                        if (bg_run_start >= 0 && prev_bg != 0) {
                            rgb_t bc = cpal[prev_bg];
                            psolid((float)(lx + bg_run_start * cw), (float)ly,
                                   (float)(lx + c * cw), (float)(ly + ch),
                                   bc.r, bc.g, bc.b, 0.9f, vp_w, vp_h);
                        }
                        bg_run_start = c;
                        prev_bg = bg_idx;
                    }
                }
                /* Flush last bg run */
                if (bg_run_start >= 0 && prev_bg != 0) {
                    rgb_t bc = cpal[prev_bg];
                    psolid((float)(lx + bg_run_start * cw), (float)ly,
                           (float)(lx + (last + 1) * cw), (float)(ly + ch),
                           bc.r, bc.g, bc.b, 0.9f, vp_w, vp_h);
                }
            }
            /* Batch by color runs */
            int run_start = 0;
            char run_buf[CB_COLS + 1];
            int run_len = 0;
            int prev_fg_idx = -1;
            for (int c = 0; c <= last; c++) {
                int fg_idx = row[c].attr.fg;
                if (row[c].attr.bold && !row[c].attr.fg256 && fg_idx < 8) fg_idx += 8;
                fg_idx &= 15;
                if (row[c].attr.reverse) fg_idx = row[c].attr.bg & 15;
                if (fg_idx != prev_fg_idx) {
                    /* Flush previous run */
                    if (run_len > 0) {
                        run_buf[run_len] = 0;
                        rgb_t fc = cpal[prev_fg_idx];
                        ptext(lx + run_start * cw, ly, run_buf,
                              fc.r, fc.g, fc.b, vp_w, vp_h, cw, ch);
                    }
                    run_start = c;
                    run_len = 0;
                    prev_fg_idx = fg_idx;
                }
                unsigned char byte = row[c].ch;
                run_buf[run_len++] = (byte >= 0x20) ? (char)byte : ' ';
            }
            /* Flush last run */
            if (run_len > 0 && prev_fg_idx >= 0) {
                run_buf[run_len] = 0;
                rgb_t fc = cpal[prev_fg_idx];
                ptext(lx + run_start * cw, ly, run_buf,
                      fc.r, fc.g, fc.b, vp_w, vp_h, cw, ch);
            }
        }
    }

    /* ---- Selection highlight overlay ---- */
    if (bsp_sel_active || bsp_selecting) {
        /* Normalize selection so s_line <= e_line */
        int s_line = bsp_sel_start_line, s_col = bsp_sel_start_col;
        int e_line = bsp_sel_end_line, e_col = bsp_sel_end_col;
        if (s_line > e_line || (s_line == e_line && s_col > e_col)) {
            int tmp; tmp = s_line; s_line = e_line; e_line = tmp;
            tmp = s_col; s_col = e_col; e_col = tmp;
        }
        for (int v = 0; v < vis_lines && (start_line + v) < total_lines; v++) {
            int abs_line = start_line + v;
            if (abs_line < s_line || abs_line > e_line) continue;
            int hy = (int)disp_y0 + v * ch;
            int hlx, hrx;
            if (abs_line == s_line && abs_line == e_line) {
                hlx = bsp_disp_lx + s_col * cw;
                hrx = bsp_disp_lx + e_col * cw;
            } else if (abs_line == s_line) {
                hlx = bsp_disp_lx + s_col * cw;
                hrx = (int)(x1 - 14);
            } else if (abs_line == e_line) {
                hlx = bsp_disp_lx;
                hrx = bsp_disp_lx + e_col * cw;
            } else {
                hlx = bsp_disp_lx;
                hrx = (int)(x1 - 14);
            }
            if (hrx > hlx) {
                psolid((float)hlx, (float)hy, (float)hrx, (float)(hy + ch),
                       ar, ag, ab, 0.30f, vp_w, vp_h);
            }
        }
    }

    /* Scrollbar track */
    if (total_lines > vis_lines) {
        float sb_x = x1 - 12;
        float sb_w = 6;
        psolid(sb_x, disp_y0, sb_x + sb_w, disp_y1,
               t->bg[0], t->bg[1], t->bg[2], 0.5f, vp_w, vp_h);
        /* Thumb */
        float ratio = (float)vis_lines / (float)total_lines;
        float thumb_h = disp_h * ratio;
        if (thumb_h < 20) thumb_h = 20;
        float scroll_ratio = (max_scroll > 0) ? (float)(max_scroll - bsp_scroll) / (float)max_scroll : 0;
        float thumb_y = disp_y0 + scroll_ratio * (disp_h - thumb_h);
        psolid(sb_x, thumb_y, sb_x + sb_w, thumb_y + thumb_h,
               ar, ag, ab, 0.6f, vp_w, vp_h);
    }

    /* ---- Save button bar ---- */
    float save_y0 = y1 - save_h;
    /* Subtle separator */
    psolid(x0 + 8, save_y0, x1 - 8, save_y0 + 1,
           t->text[0], t->text[1], t->text[2], 0.15f, vp_w, vp_h);

    /* Save button — centered, glossy */
    int btn_w = 12 * cw + 16;
    float btn_x0 = x0 + (bsp_w - btn_w) / 2;
    float btn_x1 = btn_x0 + btn_w;
    float btn_y0 = save_y0 + 4;
    float btn_y1 = y1 - 4;
    /* Button background */
    psolid(btn_x0, btn_y0, btn_x1, btn_y1,
           ar * 0.3f + t->bg[0] * 0.7f, ag * 0.3f + t->bg[1] * 0.7f,
           ab * 0.3f + t->bg[2] * 0.7f, 0.95f, vp_w, vp_h);
    /* Gloss top half */
    psolid(btn_x0, btn_y0, btn_x1, btn_y0 + (btn_y1 - btn_y0) * 0.45f,
           1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
    /* Accent bottom edge */
    psolid(btn_x0, btn_y1 - 1, btn_x1, btn_y1,
           ar, ag, ab, 0.5f, vp_w, vp_h);
    /* Button text */
    const char *save_label = "[ Save File ]";
    int slw = (int)strlen(save_label) * cw;
    int slx = (int)(btn_x0 + (btn_w - slw) / 2);
    int sly = (int)(btn_y0 + (btn_y1 - btn_y0 - ch) / 2);
    ptext(slx + 1, sly + 1, save_label, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(slx, sly, save_label, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);

    /* Resize grip (bottom-right corner) */
    psolid(x1 - 14, y1 - 14, x1 - 2, y1 - 2,
           ar, ag, ab, 0.3f, vp_w, vp_h);
}

/* ---- Backscroll Panel: copy selection to clipboard ---- */

static void bsp_copy_selection(void)
{
    /* Normalize so s <= e */
    int s_line = bsp_sel_start_line, s_col = bsp_sel_start_col;
    int e_line = bsp_sel_end_line, e_col = bsp_sel_end_col;
    if (s_line > e_line || (s_line == e_line && s_col > e_col)) {
        int tmp; tmp = s_line; s_line = e_line; e_line = tmp;
        tmp = s_col; s_col = e_col; e_col = tmp;
    }

    int num_lines = e_line - s_line + 1;
    if (num_lines <= 0) return;

    if (bsp_mode == BSP_MODE_PLAIN || bsp_mode == BSP_MODE_RAW_ANSI) {
        /* Plain text copy */
        int buf_size = num_lines * (CB_COLS + 2) + 1;
        char *buf = (char *)malloc(buf_size);
        if (!buf) return;
        int pos = 0;
        for (int i = s_line; i <= e_line; i++) {
            char line[256];
            int len;
            if (bsp_mode == BSP_MODE_PLAIN)
                len = bsp_get_plain_line(i, line, 256);
            else
                len = bsp_get_raw_line(i, line, 256);
            int c0 = (i == s_line) ? s_col : 0;
            int c1 = (i == e_line) ? e_col : len;
            if (c0 > len) c0 = len;
            if (c1 > len) c1 = len;
            for (int c = c0; c < c1 && pos < buf_size - 2; c++)
                buf[pos++] = line[c];
            /* Trim trailing spaces on each line */
            while (pos > 0 && buf[pos - 1] == ' ' &&
                   (i == e_line || buf[pos - 1] == ' ')) {
                /* Only trim if it's trailing, not if mid-line */
                break;
            }
            if (i < e_line) { buf[pos++] = '\r'; buf[pos++] = '\n'; }
        }
        buf[pos] = 0;
        /* Put on clipboard */
        if (OpenClipboard(vkt_hwnd)) {
            EmptyClipboard();
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, pos + 1);
            if (hg) {
                char *dst = (char *)GlobalLock(hg);
                memcpy(dst, buf, pos + 1);
                GlobalUnlock(hg);
                SetClipboardData(CF_TEXT, hg);
            }
            CloseClipboard();
        }
        free(buf);
    } else {
        /* HTML mode — build a self-contained HTML snippet */
        rgb_t *cpal = (bsp_mode == BSP_MODE_ANSI_HTML) ? pal_classic : tinted_palette;
        /* Estimate size: ~100 bytes per cell worst case */
        int est = num_lines * CB_COLS * 80 + 1024;
        char *buf = (char *)malloc(est);
        if (!buf) return;
        int pos = 0;

        #define BSP_APPENDF(...) do { pos += _snprintf(buf + pos, est - pos, __VA_ARGS__); } while(0)
        #define BSP_APPEND(s) do { int _l = (int)strlen(s); if (pos + _l < est) { memcpy(buf+pos,s,_l); pos+=_l; } } while(0)

        BSP_APPENDF("<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\">\n<style>\n"
            "body{background:#000;margin:0;padding:8px;}\n"
            "pre{font-family:'IBM Plex Mono','Cascadia Code','Consolas','Courier New',monospace;"
            "font-size:14px;line-height:1.3;margin:0;}\n");
        if (fx_scanline_mode) {
            BSP_APPEND(".scanlines{background:repeating-linear-gradient("
                "transparent,transparent 1px,rgba(0,0,0,0.15) 1px,rgba(0,0,0,0.15) 2px);"
                "position:fixed;top:0;left:0;width:100%;height:100%;pointer-events:none;z-index:99;}\n");
        }
        BSP_APPEND("</style></head><body>\n");
        if (fx_scanline_mode) BSP_APPEND("<div class=\"scanlines\"></div>\n");
        BSP_APPEND("<pre>");

        int prev_fg = -1, prev_bg = -1;
        int span_open = 0;
        for (int i = s_line; i <= e_line; i++) {
            ap_cell_t *row = bsp_get_cb_line(i);
            if (!row) { BSP_APPEND("\n"); continue; }
            int c0 = (i == s_line) ? s_col : 0;
            int c1 = (i == e_line) ? e_col : CB_COLS;
            /* Trim trailing spaces */
            int last = c1 - 1;
            while (last >= c0 && row[last].ch <= 0x20 && row[last].attr.bg == 0) last--;
            for (int c = c0; c <= last; c++) {
                ap_cell_t *cell = &row[c];
                int fg_idx = cell->attr.fg;
                if (cell->attr.bold && !cell->attr.fg256 && fg_idx < 8) fg_idx += 8;
                fg_idx &= 15;
                int bg_idx = cell->attr.bg & 15;
                if (cell->attr.reverse) { int tmp2 = fg_idx; fg_idx = bg_idx; bg_idx = tmp2; }
                if (fg_idx != prev_fg || bg_idx != prev_bg) {
                    if (span_open) BSP_APPEND("</span>");
                    rgb_t fc = cpal[fg_idx], bc = cpal[bg_idx];
                    BSP_APPENDF("<span style=\"color:#%02x%02x%02x",
                        (int)(fc.r*255), (int)(fc.g*255), (int)(fc.b*255));
                    if (bg_idx != 0)
                        BSP_APPENDF(";background:#%02x%02x%02x",
                            (int)(bc.r*255), (int)(bc.g*255), (int)(bc.b*255));
                    BSP_APPEND("\">");
                    span_open = 1;
                    prev_fg = fg_idx; prev_bg = bg_idx;
                }
                unsigned char ch = cell->ch;
                if (ch == '<') BSP_APPEND("&lt;");
                else if (ch == '>') BSP_APPEND("&gt;");
                else if (ch == '&') BSP_APPEND("&amp;");
                else if (ch >= 0x20 && ch < 0x7F) { buf[pos++] = (char)ch; }
                else if (ch == 0) { buf[pos++] = ' '; }
                else BSP_APPENDF("&#x%04X;", cp437_to_unicode[ch]);
            }
            if (span_open) { BSP_APPEND("</span>"); span_open = 0; prev_fg = -1; prev_bg = -1; }
            if (i < e_line) BSP_APPEND("\n");
        }
        if (span_open) BSP_APPEND("</span>");
        BSP_APPEND("</pre></body></html>\n");
        buf[pos] = 0;

        #undef BSP_APPENDF
        #undef BSP_APPEND

        /* Put on clipboard as text (the HTML is the text content) */
        if (OpenClipboard(vkt_hwnd)) {
            EmptyClipboard();
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, pos + 1);
            if (hg) {
                char *dst = (char *)GlobalLock(hg);
                memcpy(dst, buf, pos + 1);
                GlobalUnlock(hg);
                SetClipboardData(CF_TEXT, hg);
            }
            CloseClipboard();
        }
        free(buf);
    }
}

/* ---- Backscroll Panel: mouse handling ---- */

static int bsp_mouse_down(int mx, int my)
{
    if (!bsp_visible) return 0;
    if (mx < (int)bsp_x || mx >= (int)(bsp_x + bsp_w) ||
        my < (int)bsp_y || my >= (int)(bsp_y + bsp_h)) return 0;

    vkw_focus = -1; /* steal focus from floating windows */

    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_h = ch + 8;
    int save_h = ch + 12;
    int ly = my - (int)bsp_y;
    int lx = mx - (int)bsp_x;

    /* Close button */
    if (ly < titlebar_h && mx >= (int)(bsp_x + bsp_w - 24)) {
        bsp_visible = 0;
        return 1;
    }
    /* Title bar drag */
    if (ly < titlebar_h) {
        bsp_dragging = 1;
        bsp_drag_ox = (float)mx - bsp_x;
        bsp_drag_oy = (float)my - bsp_y;
        return 1;
    }
    /* Tab bar click */
    if (ly >= titlebar_h && ly < titlebar_h + tab_h) {
        int tab = (int)((float)lx / ((bsp_w) / 4.0f));
        if (tab > 3) tab = 3;
        if (tab < 0) tab = 0;
        bsp_mode = tab;
        bsp_scroll = 0;
        return 1;
    }
    /* Resize grip (check BEFORE save button — grip is in bottom-right corner) */
    if (mx >= (int)(bsp_x + bsp_w - 14) && my >= (int)(bsp_y + bsp_h - 14)) {
        bsp_resizing = 1;
        bsp_drag_ox = (float)mx - bsp_w;
        bsp_drag_oy = (float)my - bsp_h;
        return 1;
    }
    /* Save button click */
    if (ly >= (int)bsp_h - save_h) {
        bsp_save();
        return 1;
    }
    /* Display area — start text selection */
    if (my >= (int)bsp_disp_y0 && my < (int)bsp_disp_y1) {
        int cw2 = (int)(VKM_CHAR_W * ui_scale), ch2 = (int)(VKM_CHAR_H * ui_scale);
        int row = (my - (int)bsp_disp_y0) / ch2;
        int col = (mx - bsp_disp_lx) / cw2;
        if (col < 0) col = 0;
        if (col > CB_COLS) col = CB_COLS;
        int abs_line = bsp_disp_start + row;
        if (abs_line >= bsp_disp_total) abs_line = bsp_disp_total - 1;
        if (abs_line < 0) abs_line = 0;
        bsp_sel_start_line = abs_line;
        bsp_sel_start_col = col;
        bsp_sel_end_line = abs_line;
        bsp_sel_end_col = col;
        bsp_selecting = 1;
        bsp_sel_active = 0;
        return 1;
    }
    return 1; /* consume click inside panel */
}

static void bsp_mouse_move(int mx, int my)
{
    if (bsp_dragging) {
        bsp_x = (float)mx - bsp_drag_ox;
        bsp_y = (float)my - bsp_drag_oy;
        return;
    }
    if (bsp_resizing) {
        bsp_w = (float)mx - bsp_drag_ox;
        bsp_h = (float)my - bsp_drag_oy;
        if (bsp_w < 400) bsp_w = 400;
        if (bsp_h < 200) bsp_h = 200;
        return;
    }
    if (bsp_selecting) {
        int cw2 = (int)(VKM_CHAR_W * ui_scale), ch2 = (int)(VKM_CHAR_H * ui_scale);
        int row = (my - (int)bsp_disp_y0) / ch2;
        int col = (mx - bsp_disp_lx) / cw2;
        if (col < 0) col = 0;
        if (col > CB_COLS) col = CB_COLS;
        int abs_line = bsp_disp_start + row;
        /* Auto-scroll if dragging past top/bottom */
        if (my < (int)bsp_disp_y0 && bsp_scroll < bsp_disp_total - bsp_disp_vis) {
            bsp_scroll++;
            abs_line = bsp_disp_start;
        } else if (my >= (int)bsp_disp_y1 && bsp_scroll > 0) {
            bsp_scroll--;
            abs_line = bsp_disp_start + bsp_disp_vis - 1;
        }
        if (abs_line >= bsp_disp_total) abs_line = bsp_disp_total - 1;
        if (abs_line < 0) abs_line = 0;
        bsp_sel_end_line = abs_line;
        bsp_sel_end_col = col;
        bsp_sel_active = 1;
    }
}

static void bsp_mouse_up(void)
{
    if (bsp_selecting) {
        bsp_selecting = 0;
        /* If we have an actual selection (not just a click), copy to clipboard */
        if (bsp_sel_active &&
            (bsp_sel_start_line != bsp_sel_end_line ||
             bsp_sel_start_col != bsp_sel_end_col)) {
            bsp_copy_selection();
        } else {
            bsp_sel_active = 0;
        }
    }
    bsp_dragging = 0;
    bsp_resizing = 0;
}

/* ---- MUDRadio: Audio Player + Internet Radio + Beat Viz ---- */
#include "mudradio.c"
#include "mudradio_ui.c"
/* viz_fx.c moved earlier — after push_quad helpers, before menu/vertex code */

/* ---- Sound Settings Window ---- */

/* winmm function pointers (dynamically loaded) */
typedef UINT (WINAPI *waveInGetNumDevs_fn)(void);
typedef UINT (WINAPI *waveOutGetNumDevs_fn)(void);
typedef struct { WORD wMid; WORD wPid; UINT vDriverVersion; char szPname[32]; DWORD dwFormats; WORD wChannels; WORD wReserved1; } WAVEINCAPSA_t;
typedef struct { WORD wMid; WORD wPid; UINT vDriverVersion; char szPname[32]; DWORD dwFormats; WORD wChannels; WORD wReserved1; DWORD dwSupport; } WAVEOUTCAPSA_t;
typedef UINT (WINAPI *waveInGetDevCapsA_fn)(UINT, WAVEINCAPSA_t*, UINT);
typedef UINT (WINAPI *waveOutGetDevCapsA_fn)(UINT, WAVEOUTCAPSA_t*, UINT);

static HMODULE snd_winmm = NULL;
static waveInGetNumDevs_fn   snd_waveInGetNumDevs = NULL;
static waveOutGetNumDevs_fn  snd_waveOutGetNumDevs = NULL;
static waveInGetDevCapsA_fn  snd_waveInGetDevCapsA = NULL;
static waveOutGetDevCapsA_fn snd_waveOutGetDevCapsA = NULL;

#define SND_MAX_DEVS 16
static char snd_in_names[SND_MAX_DEVS][32];
static int  snd_in_count = 0;
static int  snd_in_sel = 0;
static char snd_out_names[SND_MAX_DEVS][32];
static int  snd_out_count = 0;
static int  snd_out_sel = 0;
static int  snd_master_vol = 80;  /* 0-100 */

static void snd_load_winmm(void)
{
    if (snd_winmm) return;
    snd_winmm = LoadLibraryA("winmm.dll");
    if (!snd_winmm) return;
    snd_waveInGetNumDevs = (waveInGetNumDevs_fn)GetProcAddress(snd_winmm, "waveInGetNumDevs");
    snd_waveOutGetNumDevs = (waveOutGetNumDevs_fn)GetProcAddress(snd_winmm, "waveOutGetNumDevs");
    snd_waveInGetDevCapsA = (waveInGetDevCapsA_fn)GetProcAddress(snd_winmm, "waveInGetDevCapsA");
    snd_waveOutGetDevCapsA = (waveOutGetDevCapsA_fn)GetProcAddress(snd_winmm, "waveOutGetDevCapsA");
}

static void snd_enumerate(void)
{
    snd_load_winmm();
    snd_in_count = 0;
    snd_out_count = 0;
    if (snd_waveInGetNumDevs && snd_waveInGetDevCapsA) {
        int n = (int)snd_waveInGetNumDevs();
        if (n > SND_MAX_DEVS) n = SND_MAX_DEVS;
        for (int i = 0; i < n; i++) {
            WAVEINCAPSA_t caps;
            if (snd_waveInGetDevCapsA(i, &caps, sizeof(caps)) == 0) {
                strncpy(snd_in_names[snd_in_count], caps.szPname, 31);
                snd_in_names[snd_in_count][31] = 0;
                snd_in_count++;
            }
        }
    }
    if (snd_waveOutGetNumDevs && snd_waveOutGetDevCapsA) {
        int n = (int)snd_waveOutGetNumDevs();
        if (n > SND_MAX_DEVS) n = SND_MAX_DEVS;
        for (int i = 0; i < n; i++) {
            WAVEOUTCAPSA_t caps;
            if (snd_waveOutGetDevCapsA(i, &caps, sizeof(caps)) == 0) {
                strncpy(snd_out_names[snd_out_count], caps.szPname, 31);
                snd_out_names[snd_out_count][31] = 0;
                snd_out_count++;
            }
        }
    }
    if (api) api->log("[sound] Found %d input, %d output devices\n", snd_in_count, snd_out_count);
}

static void snd_open_window(void)
{
    if (snd_visible) { sp_focus = 4; return; }
    snd_enumerate();
    snd_w = 440 * ui_scale; snd_h = 520 * ui_scale;
    snd_x = 80 * ui_scale;  snd_y = 60 * ui_scale;
    snd_visible = 1;
    sp_focus = 4;
}

static void snd_draw(int vp_w, int vp_h)
{
    if (!snd_visible) return;
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = snd_x, y0 = snd_y;
    float pw = snd_w;
    int titlebar_h = ch + 8;

    /* Panel background */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + snd_h),
               t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
    /* Border */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)(y0 + snd_h - 1), (int)(x0 + pw), (int)(y0 + snd_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + 1), (int)(y0 + snd_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)(x0 + pw - 1), (int)y0, (int)(x0 + pw), (int)(y0 + snd_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    /* Titlebar */
    {
        float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
        float tbr = acr * 0.25f + t->bg[0] * 0.5f;
        float tbg = acg * 0.25f + t->bg[1] * 0.5f;
        float tbb = acb * 0.25f + t->bg[2] * 0.5f;
        int ty0 = (int)y0 + 2, ty1 = (int)y0 + titlebar_h;
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty1,
                   tbr, tbg, tbb, 0.95f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty1 - 1, (int)(x0 + pw) - 2, ty1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
    }
    push_text((int)(x0 + 6) + 1, (int)(y0 + 4) + 1, "Sound & Video",
              0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + 6), (int)(y0 + 4), "Sound & Video",
              t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 16), (int)(y0 + 4), "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);

    float cy = y0 + titlebar_h + 10;
    float row_h = ch + 6;
    /* Max chars that fit in the panel (with margins) */
    int max_label = (int)((pw - 30) / cw);
    if (max_label > 54) max_label = 54;

    /* ======== VIDEO SECTION ======== */
    /* Section divider line */
    push_solid((int)(x0 + 8), (int)cy + ch/2, (int)(x0 + pw - 8), (int)cy + ch/2 + 1,
               t->accent[0] * 0.5f, t->accent[1] * 0.5f, t->accent[2] * 0.5f, 0.4f, vp_w, vp_h);
    push_text((int)(x0 + 12), (int)cy, "VIDEO",
              t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    cy += row_h + 2;

    /* Current render resolution (info) */
    {
        char res_label[64];
        _snprintf(res_label, sizeof(res_label), "Render: %dx%d",
                  (int)vk_sc_extent.width, (int)vk_sc_extent.height);
        push_text((int)(x0 + 14), (int)cy, res_label,
                  t->text[0] * 0.9f, t->text[1] * 0.9f, t->text[2] * 0.9f, vp_w, vp_h, cw, ch);
    }
    cy += row_h;

    /* Resolution selector */
    push_text((int)(x0 + 14), (int)cy, "Resolution:",
              t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
    cy += row_h;
    /* "Auto (native)" option */
    {
        int sel = (snd_res_sel < 0);
        if (sel) {
            push_solid((int)(x0 + 10), (int)cy - 1, (int)(x0 + pw - 10), (int)(cy + row_h - 2),
                       t->accent[0], t->accent[1], t->accent[2], 0.15f, vp_w, vp_h);
        }
        push_text((int)(x0 + 18), (int)cy,
                  sel ? "\x10 Auto (native)" : "  Auto (native)",
                  sel ? t->accent[0] : t->text[0] * 0.7f,
                  sel ? t->accent[1] : t->text[1] * 0.7f,
                  sel ? t->accent[2] : t->text[2] * 0.7f,
                  vp_w, vp_h, cw, ch);
        cy += row_h;
    }
    for (int i = 0; i < (int)NUM_RES; i++) {
        int sel = (snd_res_sel == i);
        if (sel) {
            push_solid((int)(x0 + 10), (int)cy - 1, (int)(x0 + pw - 10), (int)(cy + row_h - 2),
                       t->accent[0], t->accent[1], t->accent[2], 0.15f, vp_w, vp_h);
        }
        char rl[48];
        _snprintf(rl, sizeof(rl), "%s %s", sel ? "\x10" : " ", resolutions[i].name);
        push_text((int)(x0 + 18), (int)cy, rl,
                  sel ? t->accent[0] : t->text[0] * 0.7f,
                  sel ? t->accent[1] : t->text[1] * 0.7f,
                  sel ? t->accent[2] : t->text[2] * 0.7f,
                  vp_w, vp_h, cw, ch);
        cy += row_h;
    }

    cy += 4;

    /* VSync toggle */
    push_text((int)(x0 + 14), (int)cy,
              vk_vsync ? "\x04 VSync (60 FPS)" : "  VSync (uncapped)",
              vk_vsync ? 0.3f : t->text[0] * 0.7f,
              vk_vsync ? 0.9f : t->text[1] * 0.7f,
              vk_vsync ? 0.3f : t->text[2] * 0.7f,
              vp_w, vp_h, cw, ch);
    cy += row_h;

    /* HDR toggle */
    if (hdr_available) {
        char hdr_label[64];
        _snprintf(hdr_label, sizeof(hdr_label), "%s HDR (%s)",
                  hdr_enabled ? "\x04" : " ", hdr_standard);
        push_text((int)(x0 + 14), (int)cy, hdr_label,
                  hdr_enabled ? 0.3f : t->text[0] * 0.7f,
                  hdr_enabled ? 0.9f : t->text[1] * 0.7f,
                  hdr_enabled ? 0.3f : t->text[2] * 0.7f,
                  vp_w, vp_h, cw, ch);
    } else {
        push_text((int)(x0 + 14), (int)cy, "  HDR: Not Detected",
                  0.9f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);
    }
    cy += row_h;

    cy += 6;

    /* ======== SOUND SECTION ======== */
    push_solid((int)(x0 + 8), (int)cy + ch/2, (int)(x0 + pw - 8), (int)cy + ch/2 + 1,
               t->accent[0] * 0.5f, t->accent[1] * 0.5f, t->accent[2] * 0.5f, 0.4f, vp_w, vp_h);
    push_text((int)(x0 + 12), (int)cy, "SOUND",
              t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    cy += row_h + 2;

    /* ---- Volume Slider ---- */
    {
        char vbuf[16];
        _snprintf(vbuf, sizeof(vbuf), "%d%%", snd_master_vol);
        float vol_f = (float)snd_master_vol;
        clr_draw_slider(x0 + 8, cy, pw - 16, ch + 14, vol_f, 0.0f, 100.0f,
                        "Volume", vbuf, vp_w, vp_h, t);
        cy += ch + 14;
    }

    cy += 6;

    /* ---- Recording Devices ---- */
    push_text((int)(x0 + 14), (int)cy, "Recording Device",
              t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
    cy += row_h;

    if (snd_in_count == 0) {
        push_text((int)(x0 + 18), (int)cy, "(none found)",
                  t->text[0] * 0.4f, t->text[1] * 0.4f, t->text[2] * 0.4f, vp_w, vp_h, cw, ch);
        cy += row_h;
    } else {
        for (int i = 0; i < snd_in_count; i++) {
            int selected = (i == snd_in_sel);
            if (selected) {
                push_solid((int)(x0 + 10), (int)cy - 1, (int)(x0 + pw - 10), (int)(cy + row_h - 2),
                           t->accent[0], t->accent[1], t->accent[2], 0.15f, vp_w, vp_h);
            }
            char label[64];
            _snprintf(label, sizeof(label), "%s %.*s", selected ? "\x10" : " ", max_label, snd_in_names[i]);
            push_text((int)(x0 + 18), (int)cy, label,
                      selected ? t->accent[0] : t->text[0] * 0.7f,
                      selected ? t->accent[1] : t->text[1] * 0.7f,
                      selected ? t->accent[2] : t->text[2] * 0.7f,
                      vp_w, vp_h, cw, ch);
            cy += row_h;
        }
    }

    cy += 4;

    /* ---- Playback Devices ---- */
    push_text((int)(x0 + 14), (int)cy, "Playback Device",
              t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
    cy += row_h;

    if (snd_out_count == 0) {
        push_text((int)(x0 + 18), (int)cy, "(none found)",
                  t->text[0] * 0.4f, t->text[1] * 0.4f, t->text[2] * 0.4f, vp_w, vp_h, cw, ch);
        cy += row_h;
    } else {
        for (int i = 0; i < snd_out_count; i++) {
            int selected = (i == snd_out_sel);
            if (selected) {
                push_solid((int)(x0 + 10), (int)cy - 1, (int)(x0 + pw - 10), (int)(cy + row_h - 2),
                           t->accent[0], t->accent[1], t->accent[2], 0.15f, vp_w, vp_h);
            }
            char label[64];
            _snprintf(label, sizeof(label), "%s %.*s", selected ? "\x10" : " ", max_label, snd_out_names[i]);
            push_text((int)(x0 + 18), (int)cy, label,
                      selected ? t->accent[0] : t->text[0] * 0.7f,
                      selected ? t->accent[1] : t->text[1] * 0.7f,
                      selected ? t->accent[2] : t->text[2] * 0.7f,
                      vp_w, vp_h, cw, ch);
            cy += row_h;
        }
    }

    cy += 6;

    /* ---- Voice Info ---- */
    {
        HMODULE vm = GetModuleHandleA("voice.dll");
        push_text((int)(x0 + 14), (int)cy, "F9 Push-to-Talk",
                  t->dim[0], t->dim[1], t->dim[2], vp_w, vp_h, cw, ch);
        cy += row_h;
        if (vm) {
            push_text((int)(x0 + 18), (int)cy, "voice.dll: loaded",
                      0.3f, 0.9f, 0.3f, vp_w, vp_h, cw, ch);
        } else {
            push_text((int)(x0 + 18), (int)cy, "voice.dll: NOT loaded",
                      0.9f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);
        }
        cy += row_h;
    }

    snd_h = cy - y0 + 10;
}

/* Hit test: Returns:
 * -1=none, 0..N=input dev, 100..100+N=output dev, 200=volume slider,
 * 300=auto res, 301..307=resolution, 400=vsync, 401=HDR */
static int snd_hit_item(int mx, int my) {
    int ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    float row_h = ch + 6;
    int hit_x = (mx >= (int)(snd_x + 10) && mx < (int)(snd_x + snd_w - 10));

    /* Match layout of snd_draw exactly */
    float cy = snd_y + titlebar_h + 10;

    /* VIDEO section header */
    cy += row_h + 2;
    /* Render info line */
    cy += row_h;
    /* "Resolution:" label */
    cy += row_h;
    /* Auto (native) */
    if (my >= (int)cy && my < (int)(cy + row_h) && hit_x) return 300;
    cy += row_h;
    /* Resolution entries */
    for (int i = 0; i < (int)NUM_RES; i++) {
        if (my >= (int)cy && my < (int)(cy + row_h) && hit_x) return 301 + i;
        cy += row_h;
    }
    cy += 4;
    /* VSync */
    if (my >= (int)cy && my < (int)(cy + row_h) && hit_x) return 400;
    cy += row_h;
    /* HDR */
    if (my >= (int)cy && my < (int)(cy + row_h) && hit_x) return 401;
    cy += row_h;

    cy += 6;

    /* SOUND section header */
    cy += row_h + 2;

    /* Volume slider */
    {
        float sx0 = snd_x + 8 + 110;
        float sx1 = snd_x + snd_w - 16 - 50 + 8;
        float vol_row_h = ch + 14;
        if (my >= (int)cy && my < (int)(cy + vol_row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4))
            return 200;
        cy += vol_row_h;
    }

    cy += 6;

    /* Recording Device header */
    cy += row_h;
    /* Input devices */
    for (int i = 0; i < snd_in_count; i++) {
        if (my >= (int)cy && my < (int)(cy + row_h) && hit_x) return i;
        cy += row_h;
    }
    if (snd_in_count == 0) cy += row_h;

    cy += 4 + row_h; /* gap + Playback Device header */

    /* Output devices */
    for (int i = 0; i < snd_out_count; i++) {
        if (my >= (int)cy && my < (int)(cy + row_h) && hit_x) return 100 + i;
        cy += row_h;
    }

    return -1;
}

static int snd_mouse_down(int mx, int my) {
    if (!snd_visible) return 0;
    if (mx < (int)snd_x || mx >= (int)(snd_x + snd_w) ||
        my < (int)snd_y || my >= (int)(snd_y + snd_h)) return 0;
    int ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int ly = my - (int)snd_y;
    /* Close button */
    if (ly < titlebar_h && mx >= (int)(snd_x + snd_w - 20)) {
        snd_visible = 0;
        return 1;
    }
    /* Titlebar drag */
    if (ly < titlebar_h) {
        snd_dragging = 1;
        snd_drag_ox = (float)mx - snd_x;
        snd_drag_oy = (float)my - snd_y;
        return 1;
    }
    int hit = snd_hit_item(mx, my);
    /* Audio device selection */
    if (hit >= 0 && hit < 100) {
        snd_in_sel = hit;
        if (api) api->log("[sound] Selected input device %d: %s\n", hit, snd_in_names[hit]);
        return 1;
    }
    if (hit >= 100 && hit < 200) {
        snd_out_sel = hit - 100;
        if (api) api->log("[sound] Selected output device %d: %s\n", snd_out_sel, snd_out_names[snd_out_sel]);
        return 1;
    }
    /* Volume slider */
    if (hit == 200) {
        float sx0 = snd_x + 8 + 110;
        float sx1 = snd_x + snd_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        snd_master_vol = (int)(pct * 100.0f);
        snd_active_slider = 0;
        return 1;
    }
    /* Resolution: auto */
    if (hit == 300) {
        snd_res_sel = -1;
        int sm_w = GetSystemMetrics(SM_CXSCREEN);
        int sm_h = GetSystemMetrics(SM_CYSCREEN);
        if (sm_w > 0) fs_width = sm_w;
        if (sm_h > 0) fs_height = sm_h;
        if (api) api->log("[video] Auto-native: %dx%d\n", fs_width, fs_height);
        if (vkt_hwnd) {
            SetWindowPos(vkt_hwnd, HWND_TOP, 0, 0, fs_width, fs_height,
                         SWP_FRAMECHANGED);
        }
        vkt_recreate_swapchain();
        return 1;
    }
    /* Resolution: specific */
    if (hit >= 301 && hit < 301 + (int)NUM_RES) {
        int idx = hit - 301;
        snd_res_sel = idx;
        fs_width = resolutions[idx].w;
        fs_height = resolutions[idx].h;
        if (api) api->log("[video] Resolution: %s (%dx%d)\n", resolutions[idx].name, fs_width, fs_height);
        if (vkt_hwnd) {
            SetWindowPos(vkt_hwnd, HWND_TOP, 0, 0, fs_width, fs_height,
                         SWP_FRAMECHANGED);
        }
        vkt_recreate_swapchain();
        return 1;
    }
    /* VSync toggle */
    if (hit == 400) {
        vk_vsync = !vk_vsync;
        if (api) api->log("[video] VSync: %s\n", vk_vsync ? "ON" : "OFF");
        vkt_recreate_swapchain();
        return 1;
    }
    /* HDR toggle */
    if (hit == 401 && hdr_available) {
        hdr_enabled = !hdr_enabled;
        if (api) api->log("[video] HDR: %s\n", hdr_enabled ? "ON" : "OFF");
        vkt_recreate_swapchain();
        return 1;
    }
    return 1; /* consume click inside panel */
}

static void snd_mouse_move(int mx, int my) {
    if (snd_dragging) {
        snd_x = (float)mx - snd_drag_ox;
        snd_y = (float)my - snd_drag_oy;
        return;
    }
    if (snd_active_slider == 0) {
        float sx0 = snd_x + 8 + 110;
        float sx1 = snd_x + snd_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        snd_master_vol = (int)(pct * 100.0f);
    }
}

static void snd_mouse_up(void) {
    snd_dragging = 0;
    snd_active_slider = -1;
}

/* ---- Color/Brightness Settings Panel ---- */

static void clr_open_window(void) {
    clr_visible = !clr_visible;
    if (clr_visible) {
        clr_w = 280 * ui_scale; clr_h = 200 * ui_scale;
        int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
        if (clr_x < 1 && clr_y < 1) {
            clr_x = (float)(vp_w - (int)clr_w) / 2.0f;
            clr_y = (float)(vp_h - (int)clr_h) / 2.0f;
        }
    }
}

static void clr_draw_slider(float x0, float y0, float w, float h,
                             float val, float vmin, float vmax,
                             const char *label, const char *fmt_val,
                             int vp_w, int vp_h, const ui_theme_t *t)
{
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    /* Label on the left */
    push_text((int)x0, (int)y0, label, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);

    /* Slider track — label area and value area scale with font */
    float sx0 = x0 + (float)(12 * cw);
    float sx1 = x0 + w - (float)(7 * cw);
    float sy = y0 + ch * 0.5f - 3;
    float sh = 6.0f;

    /* Track background */
    push_solid((int)sx0, (int)sy, (int)sx1, (int)(sy + sh),
               t->text[0] * 0.2f, t->text[1] * 0.2f, t->text[2] * 0.2f,
               0.5f, vp_w, vp_h);

    /* Fill */
    float pct = (val - vmin) / (vmax - vmin);
    if (pct < 0) pct = 0; if (pct > 1) pct = 1;
    float fill_x = sx0 + pct * (sx1 - sx0);
    push_solid((int)sx0, (int)sy, (int)fill_x, (int)(sy + sh),
               t->accent[0], t->accent[1], t->accent[2],
               0.6f, vp_w, vp_h);

    /* Thumb */
    push_solid((int)(fill_x - 3), (int)(sy - 2), (int)(fill_x + 3), (int)(sy + sh + 2),
               t->text[0], t->text[1], t->text[2],
               0.9f, vp_w, vp_h);

    /* Value text */
    push_text((int)(sx1 + 6), (int)y0, fmt_val, t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
}

static void clr_draw(int vp_w, int vp_h) {
    if (!clr_visible) return;
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = clr_x, y0 = clr_y;
    float pw = clr_w;
    int titlebar_h = ch + 8;

    /* Panel background */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + clr_h),
               t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
    /* Border */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)(y0 + clr_h - 1), (int)(x0 + pw), (int)(y0 + clr_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + 1), (int)(y0 + clr_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)(x0 + pw - 1), (int)y0, (int)(x0 + pw), (int)(y0 + clr_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);

    /* Titlebar — glossy gradient */
    {
        float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
        float tbr = acr * 0.25f + t->bg[0] * 0.5f;
        float tbg = acg * 0.25f + t->bg[1] * 0.5f;
        float tbb = acb * 0.25f + t->bg[2] * 0.5f;
        int ty0 = (int)y0 + 2, ty1 = (int)y0 + titlebar_h;
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty1,
                   tbr, tbg, tbb, 0.95f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty1 - 1, (int)(x0 + pw) - 2, ty1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
    }
    push_text((int)(x0 + 6) + 1, (int)(y0 + 4) + 1, "Color / Brightness",
              0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + 6), (int)(y0 + 4), "Color / Brightness",
              t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    /* Close X */
    push_text((int)(x0 + pw - 16), (int)(y0 + 4), "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);

    /* Reset button */
    push_text((int)(x0 + pw - 55), (int)(y0 + 4), "Reset",
              t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);

    float cy = y0 + titlebar_h + 12;
    float row_h = ch + 14;
    char vbuf[32];

    _snprintf(vbuf, sizeof(vbuf), "%+.0f%%", pp_brightness * 100);
    clr_draw_slider(x0 + 8, cy, pw - 16, row_h, pp_brightness, -1.0f, 1.0f,
                    "Brightness", vbuf, vp_w, vp_h, t);
    cy += row_h;

    _snprintf(vbuf, sizeof(vbuf), "%.0f%%", pp_contrast * 100);
    clr_draw_slider(x0 + 8, cy, pw - 16, row_h, pp_contrast, 0.0f, 2.0f,
                    "Contrast", vbuf, vp_w, vp_h, t);
    cy += row_h;

    _snprintf(vbuf, sizeof(vbuf), "%+.0f\xF8", pp_hue); /* degree symbol */
    clr_draw_slider(x0 + 8, cy, pw - 16, row_h, pp_hue, -180.0f, 180.0f,
                    "Hue Shift", vbuf, vp_w, vp_h, t);
    cy += row_h;

    _snprintf(vbuf, sizeof(vbuf), "%.0f%%", pp_saturation * 100);
    clr_draw_slider(x0 + 8, cy, pw - 16, row_h, pp_saturation, 0.0f, 2.0f,
                    "Saturation", vbuf, vp_w, vp_h, t);
    cy += row_h;

    clr_h = cy - y0 + 8;
}

static int clr_hit_slider(int mx, int my, float *out_pct) {
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    float row_h = ch + 14;

    float sx0 = clr_x + 8 + 110;
    float sx1 = clr_x + clr_w - 16 - 50 + 8;

    for (int i = 0; i < 4; i++) {
        float sy = clr_y + titlebar_h + 12 + i * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    return -1;
}

static void clr_set_from_pct(int slider, float pct) {
    switch (slider) {
        case 0: pp_brightness = -1.0f + pct * 2.0f; break;
        case 1: pp_contrast = pct * 2.0f; break;
        case 2: pp_hue = -180.0f + pct * 360.0f; break;
        case 3: pp_saturation = pct * 2.0f; break;
    }
}

static int clr_mouse_down(int mx, int my) {
    if (!clr_visible) return 0;
    if (mx < (int)clr_x || mx >= (int)(clr_x + clr_w) ||
        my < (int)clr_y || my >= (int)(clr_y + clr_h)) return 0;

    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int ly = my - (int)clr_y;

    /* Close button */
    if (ly < titlebar_h && mx >= (int)(clr_x + clr_w - 20)) {
        clr_visible = 0;
        return 1;
    }
    /* Reset button */
    if (ly < titlebar_h && mx >= (int)(clr_x + clr_w - 60) && mx < (int)(clr_x + clr_w - 20)) {
        pp_brightness = 0.0f; pp_contrast = 1.0f;
        pp_hue = 0.0f; pp_saturation = 1.0f;
        return 1;
    }
    /* Titlebar drag */
    if (ly < titlebar_h) {
        clr_dragging = 1;
        clr_drag_ox = (float)mx - clr_x;
        clr_drag_oy = (float)my - clr_y;
        return 1;
    }

    /* Slider click */
    float pct;
    int s = clr_hit_slider(mx, my, &pct);
    if (s >= 0) {
        clr_active_slider = s;
        clr_set_from_pct(s, pct);
        return 1;
    }

    return 1; /* consume click inside panel */
}

static void clr_mouse_move(int mx, int my) {
    if (clr_dragging) {
        clr_x = (float)mx - clr_drag_ox;
        clr_y = (float)my - clr_drag_oy;
        return;
    }
    if (clr_active_slider >= 0) {
        float sx0 = clr_x + 8 + 110;
        float sx1 = clr_x + clr_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        clr_set_from_pct(clr_active_slider, pct);
    }
}

static void clr_mouse_up(void) {
    clr_dragging = 0;
    clr_active_slider = -1;
}

/* ---- Fonts & Text Panel (fnt_) ---- */

static void fnt_toggle(void) {
    fnt_visible = !fnt_visible;
    if (fnt_visible) {
        fnt_w = 380 * ui_scale; fnt_h = 600 * ui_scale;
        int vp_w = (int)vk_sc_extent.width, vp_h = (int)vk_sc_extent.height;
        fnt_x = (float)(vp_w - (int)fnt_w) / 2.0f;
        fnt_y = (float)(vp_h - (int)fnt_h) / 2.0f;
        fnt_scroll = 0;
    }
}

static void fnt_draw(int vp_w, int vp_h) {
    if (!fnt_visible) return;
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = fnt_x, y0 = fnt_y, pw = fnt_w;
    int titlebar_h = ch + 8;
    int item_h = ch + 6;
    int total_fonts = NUM_TTF_FONTS + 1; /* +1 for CP437 bitmap */

    /* Panel background */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + fnt_h),
               t->bg[0], t->bg[1], t->bg[2], 0.94f, vp_w, vp_h);
    /* Border */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)(y0 + fnt_h - 1), (int)(x0 + pw), (int)(y0 + fnt_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + 1), (int)(y0 + fnt_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)(x0 + pw - 1), (int)y0, (int)(x0 + pw), (int)(y0 + fnt_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);

    /* Titlebar */
    {
        float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
        float tbr = acr * 0.25f + t->bg[0] * 0.5f;
        float tbg = acg * 0.25f + t->bg[1] * 0.5f;
        float tbb = acb * 0.25f + t->bg[2] * 0.5f;
        int ty0 = (int)y0 + 2, ty1 = (int)y0 + titlebar_h;
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty1,
                   tbr, tbg, tbb, 0.95f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty1 - 1, (int)(x0 + pw) - 2, ty1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
    }
    push_text((int)(x0 + 6) + 1, (int)(y0 + 4) + 1, "Fonts & Text",
              0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + 6), (int)(y0 + 4), "Fonts & Text",
              t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 16), (int)(y0 + 4), "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);

    /* --- Font list section --- */
    float cy = y0 + titlebar_h + 4;
    push_text((int)(x0 + 8), (int)cy, "FONT",
              t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    cy += ch + 4;

    /* Separator line */
    push_solid((int)(x0 + 4), (int)cy, (int)(x0 + pw - 4), (int)(cy + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
    cy += 4;

    /* Calculate how many font items fit in the list area */
    float slider_area_h = (ch + 14) * 2 + 24; /* 2 sliders + padding */
    float list_area_h = fnt_h - (cy - y0) - slider_area_h - 8;
    int visible_items = (int)(list_area_h / (float)item_h);
    if (visible_items < 3) visible_items = 3;
    if (fnt_scroll > total_fonts - visible_items) fnt_scroll = total_fonts - visible_items;
    if (fnt_scroll < 0) fnt_scroll = 0;

    fnt_hover_item = -1;
    for (int i = 0; i < visible_items && (i + fnt_scroll) < total_fonts; i++) {
        int fi = i + fnt_scroll;
        const char *label;
        int is_active;
        int is_separator = 0;
        if (fi == 0) {
            label = "CP437 Bitmap (VGA)";
            is_active = (current_font < 0);
        } else {
            int ti = fi - 1;
            label = ttf_fonts[ti].name;
            is_separator = (ttf_fonts[ti].filename == NULL);
            is_active = !is_separator && (current_font == ti);
        }

        float iy = cy + i * item_h;

        if (is_separator) {
            /* Category header: centered, accent color, subtle underline */
            push_solid((int)(x0 + 8), (int)(iy + item_h - 2), (int)(x0 + pw - 8), (int)(iy + item_h - 1),
                       t->accent[0], t->accent[1], t->accent[2], 0.35f, vp_w, vp_h);
            /* Center the label */
            int label_w = (int)strlen(label) * cw;
            int cx = (int)(x0 + (pw - label_w) / 2);
            push_text(cx, (int)(iy + 2), label,
                      t->accent[0] * 0.8f, t->accent[1] * 0.8f, t->accent[2] * 0.8f,
                      vp_w, vp_h, cw, ch);
            continue;
        }

        /* Highlight active font */
        if (is_active) {
            push_solid((int)(x0 + 2), (int)iy, (int)(x0 + pw - 2), (int)(iy + item_h),
                       t->accent[0], t->accent[1], t->accent[2], 0.25f, vp_w, vp_h);
        }

        /* Font name */
        float tr = is_active ? t->accent[0] : t->text[0] * 0.85f;
        float tg = is_active ? t->accent[1] : t->text[1] * 0.85f;
        float tb = is_active ? t->accent[2] : t->text[2] * 0.85f;
        push_text((int)(x0 + 10), (int)(iy + 2), label, tr, tg, tb, vp_w, vp_h, cw, ch);
    }

    /* Scrollbar if needed */
    if (total_fonts > visible_items) {
        float sb_x = x0 + pw - 8;
        float sb_y0 = cy;
        float sb_h = (float)(visible_items * item_h);
        float thumb_h = sb_h * ((float)visible_items / (float)total_fonts);
        if (thumb_h < 12) thumb_h = 12;
        float thumb_y = sb_y0 + ((float)fnt_scroll / (float)(total_fonts - visible_items)) * (sb_h - thumb_h);
        push_solid((int)sb_x, (int)sb_y0, (int)(sb_x + 4), (int)(sb_y0 + sb_h),
                   t->text[0] * 0.15f, t->text[1] * 0.15f, t->text[2] * 0.15f, 0.4f, vp_w, vp_h);
        push_solid((int)sb_x, (int)thumb_y, (int)(sb_x + 4), (int)(thumb_y + thumb_h),
                   t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    }

    /* --- Sliders section --- */
    float slider_y = cy + visible_items * item_h + 8;

    /* Separator */
    push_solid((int)(x0 + 4), (int)slider_y, (int)(x0 + pw - 4), (int)(slider_y + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
    slider_y += 8;

    float row_h = ch + 14;
    char vbuf[32];

    /* Font Size slider (term_font_adj, -5..+5) */
    _snprintf(vbuf, sizeof(vbuf), "%+d", term_font_adj);
    clr_draw_slider(x0 + 8, slider_y, pw - 16, row_h,
                    (float)term_font_adj, -5.0f, 5.0f,
                    "Font Size", vbuf, vp_w, vp_h, t);
    slider_y += row_h;

    /* Left Margin slider (term_margin, 0..40) */
    _snprintf(vbuf, sizeof(vbuf), "%dpx", term_margin);
    clr_draw_slider(x0 + 8, slider_y, pw - 16, row_h,
                    (float)term_margin, 0.0f, 40.0f,
                    "Left Margin", vbuf, vp_w, vp_h, t);
    slider_y += row_h;

}

static int fnt_mouse_down(int mx, int my) {
    if (!fnt_visible) return 0;
    if (mx < (int)fnt_x || mx >= (int)(fnt_x + fnt_w) ||
        my < (int)fnt_y || my >= (int)(fnt_y + fnt_h)) return 0;

    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int item_h = ch + 6;
    int total_fonts = NUM_TTF_FONTS + 1;
    int ly = my - (int)fnt_y;

    /* Close button */
    if (ly < titlebar_h && mx >= (int)(fnt_x + fnt_w - 20)) {
        fnt_visible = 0;
        return 1;
    }
    /* Titlebar drag */
    if (ly < titlebar_h) {
        fnt_dragging = 1;
        fnt_drag_ox = (float)mx - fnt_x;
        fnt_drag_oy = (float)my - fnt_y;
        return 1;
    }

    /* Font list area */
    float list_y0 = fnt_y + titlebar_h + 4 + ch + 4 + 4; /* after "FONT" label + separator */
    float slider_area_h = (ch + 14) * 2 + 24;
    float list_area_h = fnt_h - (list_y0 - fnt_y) - slider_area_h - 8;
    int visible_items = (int)(list_area_h / (float)item_h);
    if (visible_items < 3) visible_items = 3;

    if ((float)my >= list_y0 && (float)my < list_y0 + visible_items * item_h) {
        int clicked_row = (int)((float)(my - (int)list_y0) / (float)item_h);
        int fi = clicked_row + fnt_scroll;
        if (fi >= 0 && fi < total_fonts) {
            /* Skip category separator clicks */
            if (fi > 0 && ttf_fonts[fi - 1].filename == NULL) return 1;
            pending_font_idx = (fi == 0) ? -1 : fi - 1;
            font_change_pending = 1;
            return 1;
        }
    }

    /* Slider area — check both sliders */
    float slider_y = list_y0 + visible_items * item_h + 8 + 8; /* after separator */
    float row_h = ch + 14;

    /* Font Size slider */
    {
        float sx0 = fnt_x + 8 + 110;
        float sx1 = fnt_x + fnt_w - 16 - 50 + 8;
        float sy = slider_y;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            fnt_active_slider = 0;
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            term_font_adj = (int)(-5.0f + pct * 10.0f);
            font_change_pending = 1;
            pending_font_idx = current_font;
            return 1;
        }
    }
    /* Left Margin slider */
    {
        float sx0 = fnt_x + 8 + 110;
        float sx1 = fnt_x + fnt_w - 16 - 50 + 8;
        float sy = slider_y + row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            fnt_active_slider = 1;
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            term_margin = (int)(pct * 40.0f);
            return 1;
        }
    }

    return 1; /* consume click inside panel */
}

static void fnt_mouse_move(int mx, int my) {
    if (fnt_dragging) {
        fnt_x = (float)mx - fnt_drag_ox;
        fnt_y = (float)my - fnt_drag_oy;
        return;
    }
    if (fnt_active_slider >= 0) {
        float sx0 = fnt_x + 8 + 110;
        float sx1 = fnt_x + fnt_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        if (fnt_active_slider == 0) {
            term_font_adj = (int)(-5.0f + pct * 10.0f);
            font_change_pending = 1;
            pending_font_idx = current_font;
        } else if (fnt_active_slider == 1) {
            term_margin = (int)(pct * 40.0f);
        }
    }
}

static void fnt_mouse_up(void) {
    fnt_dragging = 0;
    fnt_active_slider = -1;
}

static int fnt_scroll_wheel(int mx, int my, int delta) {
    if (!fnt_visible) return 0;
    if (mx < (int)fnt_x || mx >= (int)(fnt_x + fnt_w) ||
        my < (int)fnt_y || my >= (int)(fnt_y + fnt_h)) return 0;
    fnt_scroll -= delta;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int item_h = ch + 6;
    int total_fonts = NUM_TTF_FONTS + 1;
    float slider_area_h = (ch + 14) * 2 + 24;
    float list_y0 = fnt_y + titlebar_h + 4 + ch + 4 + 4;
    float list_area_h = fnt_h - (list_y0 - fnt_y) - slider_area_h - 8;
    int visible_items = (int)(list_area_h / (float)item_h);
    if (visible_items < 3) visible_items = 3;
    int max_scroll = total_fonts - visible_items;
    if (max_scroll < 0) max_scroll = 0;
    if (fnt_scroll < 0) fnt_scroll = 0;
    if (fnt_scroll > max_scroll) fnt_scroll = max_scroll;
    return 1;
}

/* ---- Background Settings Panel (bgp_) ---- */

#define BGP_SLIDER_COUNT 12

static const char *bgp_labels[BGP_SLIDER_COUNT] = {
    "Speed", "Turbulence", "Complexity", "Hue", "Saturation", "Brightness", "Opacity",
    "Sobel", "Sobel Angle", "Edge Glow", "Sharpen", "Mat. Str."
};

static float bgp_slider_min[BGP_SLIDER_COUNT] = {
    0.1f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f
};
static float bgp_slider_max[BGP_SLIDER_COUNT] = {
    5.0f, 2.0f, 8.0f, 360.0f, 1.0f, 2.0f, 1.0f,
    2.0f, 360.0f, 2.0f, 2.0f, 2.0f
};

static float *bgp_slider_ptr[BGP_SLIDER_COUNT];

/* Tonemap mode names */
#define BGP_TONEMAP_COUNT 5
static const char *bgp_tonemap_names[BGP_TONEMAP_COUNT] = {
    "None", "ACES Filmic", "Reinhard", "Hable", "AgX"
};

/* Total virtual rows = sliders + section labels + dropdown rows + defaults block */
#define BGP_TOTAL_ROWS (BGP_SLIDER_COUNT + 13)  /* 12 sliders + 1 material dd + 1 sep + 4 beat dds + 2 section labels + 1 tonemap dd + 1 exposure slider + 1 defaults header + 1 defaults button */

/* Forward decl for starfield tab rendering (populated in step 4) */
static void bgp_draw_starfield_tab(float x0, float content_top, float content_bot,
                                   float pw, float sb_w, int vp_w, int vp_h,
                                   const ui_theme_t *t, int cw, int ch);
static int  bgp_starfield_total_rows(void);
static void bgp_draw_alienwall_tab(float x0, float content_top, float content_bot,
                                   float pw, float sb_w, int vp_w, int vp_h,
                                   const ui_theme_t *t, int cw, int ch);

static int bgp_scaled = 0;
static void bgp_draw(int vp_w, int vp_h) {
    if (!bgp_visible) return;
    if (!bgp_scaled) { bgp_w = 680*ui_scale; bgp_h = 720*ui_scale; bgp_x = 100*ui_scale; bgp_y = 60*ui_scale; bgp_scaled = 1; }
    if (bgp_w < BGP_MIN_W) bgp_w = BGP_MIN_W;
    if (bgp_h < BGP_MIN_H) bgp_h = BGP_MIN_H;
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = bgp_x, y0 = bgp_y;
    float pw = bgp_w;
    float x1 = x0 + pw, y1 = y0 + bgp_h;
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float sb_w = 10.0f;
    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];

    /* Clamp tab index */
    if (bgp_tab < 0 || bgp_tab >= BGP_TAB_COUNT) bgp_tab = 0;
    /* Pull per-tab scroll position into the shared var used by hit tests */
    bgp_scroll = bgp_tab_scroll[bgp_tab];

    bgp_slider_ptr[0] = &bg_plasma_speed;
    bgp_slider_ptr[1] = &bg_plasma_turbulence;
    bgp_slider_ptr[2] = &bg_plasma_complexity;
    bgp_slider_ptr[3] = &bg_plasma_hue;
    bgp_slider_ptr[4] = &bg_plasma_saturation;
    bgp_slider_ptr[5] = &bg_plasma_brightness;
    bgp_slider_ptr[6] = &bg_plasma_alpha;
    bgp_slider_ptr[7] = &bg_plasma_sobel;
    bgp_slider_ptr[8] = &bg_plasma_sobel_angle;
    bgp_slider_ptr[9] = &bg_plasma_edge_glow;
    bgp_slider_ptr[10] = &bg_plasma_sharpen;
    bgp_slider_ptr[11] = &bg_plasma_mat_str;

    /* ---- Panel body (slightly lifted from pure bg) ---- */
    push_solid((int)x0, (int)y0, (int)x1, (int)y1,
               bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.96f, vp_w, vp_h);

    /* Outer bevel: light top/left, dark bottom/right */
    push_solid((int)x0, (int)y0, (int)x1, (int)y0 + 1,
               bgr + 0.18f, bgg + 0.18f, bgb + 0.18f, 0.8f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)x0 + 1, (int)y1,
               bgr + 0.14f, bgg + 0.14f, bgb + 0.14f, 0.7f, vp_w, vp_h);
    push_solid((int)x0, (int)y1 - 1, (int)x1, (int)y1,
               bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.9f, vp_w, vp_h);
    push_solid((int)x1 - 1, (int)y0, (int)x1, (int)y1,
               bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.85f, vp_w, vp_h);
    /* Inner bevel for depth */
    push_solid((int)x0 + 1, (int)y0 + 1, (int)x1 - 1, (int)y0 + 2,
               bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.5f, vp_w, vp_h);
    push_solid((int)x0 + 1, (int)y0 + 1, (int)x0 + 2, (int)y1 - 1,
               bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.4f, vp_w, vp_h);

    /* ---- Decorated titlebar (matches Player Stats) ---- */
    {
        float tb_y0 = y0 + 2.0f, tb_y1 = y0 + (float)titlebar_h;
        /* Accent-tinted bg */
        push_solid((int)x0 + 2, (int)tb_y0, (int)x1 - 2, (int)tb_y1,
                   acr * 0.25f + bgr * 0.5f, acg * 0.25f + bgg * 0.5f, acb * 0.25f + bgb * 0.5f,
                   0.95f, vp_w, vp_h);
        /* Gloss highlight on top half */
        push_solid((int)x0 + 2, (int)tb_y0, (int)x1 - 2, (int)tb_y0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        /* Bottom accent edge */
        push_solid((int)x0 + 2, (int)tb_y1 - 1, (int)x1 - 2, (int)tb_y1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
        int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
        int pad_x = 14;
        push_text((int)x0 + pad_x + 1, title_ty + 1, "Background Settings",
                  0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
        push_text((int)x0 + pad_x, title_ty, "Background Settings",
                  txr, txg, txb, vp_w, vp_h, cw, ch);
        /* Close [X] — red-tinted, inset from right edge to keep margin */
        push_text((int)x1 - pad_x - cw, title_ty, "X",
                  1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);
    }

    /* --- Tab bar --- */
    {
        float tab_y0 = y0 + titlebar_h;
        float tab_y1 = tab_y0 + tab_bar_h;
        /* Tab bar strip background (slightly darker than panel) */
        push_solid((int)(x0 + 1), (int)tab_y0, (int)(x0 + pw - 1), (int)tab_y1,
                   t->bg[0] * 0.85f, t->bg[1] * 0.85f, t->bg[2] * 0.85f,
                   0.95f, vp_w, vp_h);
        /* Draw each tab button */
        float tx = x0 + 4;
        for (int i = 0; i < BGP_TAB_COUNT; i++) {
            int tlen = (int)strlen(bgp_tab_names[i]);
            float tw = (float)(tlen * cw) + 16.0f;
            int is_active = (i == bgp_tab);
            float br, bg, bb, ba;
            if (is_active) {
                br = t->bg[0]; bg = t->bg[1]; bb = t->bg[2]; ba = 0.95f;
            } else {
                br = t->bg[0] * 0.6f; bg = t->bg[1] * 0.6f; bb = t->bg[2] * 0.6f; ba = 0.7f;
            }
            push_solid((int)tx, (int)(tab_y0 + 2), (int)(tx + tw), (int)tab_y1,
                       br, bg, bb, ba, vp_w, vp_h);
            if (is_active) {
                /* Underline in accent color to indicate active tab */
                push_solid((int)tx, (int)(tab_y1 - 2), (int)(tx + tw), (int)tab_y1,
                           t->accent[0], t->accent[1], t->accent[2], 0.9f, vp_w, vp_h);
            }
            float tr = is_active ? t->text[0] : t->text[0] * 0.6f;
            float tg = is_active ? t->text[1] : t->text[1] * 0.6f;
            float tb = is_active ? t->text[2] : t->text[2] * 0.6f;
            push_text((int)(tx + 8), (int)(tab_y0 + 5),
                      bgp_tab_names[i], tr, tg, tb, vp_w, vp_h, cw, ch);
            tx += tw + 2;
        }
        /* Bottom border of tab bar */
        push_solid((int)(x0 + 1), (int)tab_y1, (int)(x0 + pw - 1), (int)(tab_y1 + 1),
                   t->accent[0], t->accent[1], t->accent[2], 0.4f, vp_w, vp_h);
    }

    /* --- Resize grip (bottom-right corner, diagonal hatch) --- */
    {
        int gx1 = (int)x1 - 2, gy1 = (int)y1 - 2;
        int gz = BGP_RESIZE_ZONE;
        for (int i = 2; i <= gz; i += 3) {
            push_solid(gx1 - i, gy1 - 1, gx1, gy1,
                       acr * 0.8f + 0.1f, acg * 0.8f + 0.1f, acb * 0.8f + 0.1f,
                       0.6f, vp_w, vp_h);
            push_solid(gx1 - 1, gy1 - i, gx1, gy1,
                       acr * 0.8f + 0.1f, acg * 0.8f + 0.1f, acb * 0.8f + 0.1f,
                       0.6f, vp_w, vp_h);
            /* Diagonal hatch */
            for (int j = 0; j < i; j++) {
                push_solid(gx1 - j - 1, gy1 - (i - j) - 1,
                           gx1 - j, gy1 - (i - j),
                           1.0f, 1.0f, 1.0f, 0.22f, vp_w, vp_h);
            }
        }
    }

    /* Content area with scrolling */
    float content_top = y0 + titlebar_h + tab_bar_h + 4;
    float content_bot = y0 + bgp_h - 4;
    float content_h = content_bot - content_top;
    float row_h = ch + 14;
    float slider_w = pw - 40 - sb_w;
    char vbuf[32];

    /* If Starfield or Alien Wall tab is active, delegate and return early. */
    if (bgp_tab == BGP_TAB_STARFIELD) {
        bgp_draw_starfield_tab(x0, content_top, content_bot, pw, sb_w,
                               vp_w, vp_h, t, cw, ch);
        bgp_tab_scroll[bgp_tab] = bgp_scroll;
        return;
    }
    if (bgp_tab == BGP_TAB_ALIENWALL) {
        bgp_draw_alienwall_tab(x0, content_top, content_bot, pw, sb_w,
                               vp_w, vp_h, t, cw, ch);
        bgp_tab_scroll[bgp_tab] = bgp_scroll;
        return;
    }

    /* Virtual row index (for scroll) — each slider is a row, plus section headers and dropdowns */
    int vrow = 0;
    int max_visible = (int)(content_h / row_h);
    if (max_visible < 3) max_visible = 3;
    int max_scroll = BGP_TOTAL_ROWS - max_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (bgp_scroll > max_scroll) bgp_scroll = max_scroll;
    if (bgp_scroll < 0) bgp_scroll = 0;

    float cy = content_top + 4;
    int drawn = 0;

    /* Helper: draw a row only if it's in the visible scroll window */
    #define BGP_ROW_VISIBLE (vrow >= bgp_scroll && drawn < max_visible)
    #define BGP_NEXT_ROW do { if (vrow >= bgp_scroll && drawn < max_visible) { cy += row_h; drawn++; } vrow++; } while(0)

    /* --- Sliders section --- */
    for (int i = 0; i < BGP_SLIDER_COUNT; i++) {
        if (BGP_ROW_VISIBLE) {
            float val = *bgp_slider_ptr[i];
            if (i == 3 || i == 8)  /* Hue, Sobel Angle */
                _snprintf(vbuf, sizeof(vbuf), "%.0f\xF8", val);
            else if (i == 2)       /* Complexity */
                _snprintf(vbuf, sizeof(vbuf), "%.0f", val);
            else
                _snprintf(vbuf, sizeof(vbuf), "%.2f", val);

            clr_draw_slider(x0 + 20, cy, slider_w, row_h, val,
                            bgp_slider_min[i], bgp_slider_max[i],
                            bgp_labels[i], vbuf, vp_w, vp_h, t);
        }
        BGP_NEXT_ROW;
    }

    /* --- Material dropdown --- */
    if (BGP_ROW_VISIBLE) {
        push_text((int)(x0 + 20), (int)(cy + 2), "Material:",
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
        int dd_x = (int)(x0 + 20 + 11 * cw);
        int dd_w = (int)(slider_w - 11 * cw - 2);
        int dd_y = (int)cy;
        int dd_h = ch + 4;
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                   t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                   1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
        push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        int mat = bg_plasma_material;
        if (mat < 0 || mat >= BGP_MAT_COUNT) mat = 0;
        push_text(dd_x + 4, dd_y + 2, bgp_mat_names[mat],
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
        push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                  t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
    }
    BGP_NEXT_ROW;

    /* --- Beat Response section header --- */
    if (BGP_ROW_VISIBLE) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Beat Response",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    BGP_NEXT_ROW;

    /* --- Beat dropdown rows --- */
    {
        struct { const char *label; int *val; } beats[4] = {
            { "Bass/Kick:", &bg_plasma_beat_bass },
            { "Mid:",       &bg_plasma_beat_mid },
            { "Treble:",    &bg_plasma_beat_treble },
            { "RMS:",       &bg_plasma_beat_rms }
        };
        for (int b = 0; b < 4; b++) {
            if (BGP_ROW_VISIBLE) {
                push_text((int)(x0 + 20), (int)(cy + 2), beats[b].label,
                          t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
                /* Dropdown button */
                int dd_x = (int)(x0 + 20 + 11 * cw);
                int dd_w = (int)(slider_w - 11 * cw - 2);
                int dd_y = (int)cy;
                int dd_h = ch + 4;
                /* Button bg */
                push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                           t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
                /* Top highlight */
                push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                           1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
                /* Bottom shadow */
                push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                           0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
                /* Current value text */
                int mode = *beats[b].val;
                if (mode < 0 || mode >= BGP_BEAT_COUNT) mode = 0;
                push_text(dd_x + 4, dd_y + 2, bgp_beat_names[mode],
                          t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
                /* Arrow */
                push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                          t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
            }
            BGP_NEXT_ROW;
        }
    }

    /* --- Tonemapping section header --- */
    if (BGP_ROW_VISIBLE) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Tonemapping",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    BGP_NEXT_ROW;

    /* Tonemap mode dropdown */
    if (BGP_ROW_VISIBLE) {
        push_text((int)(x0 + 20), (int)(cy + 2), "Mode:",
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
        int dd_x = (int)(x0 + 20 + 11 * cw);
        int dd_w = (int)(slider_w - 11 * cw - 2);
        int dd_y = (int)cy;
        int dd_h = ch + 4;
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                   t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                   1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
        push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        int tm = bg_tonemap_mode;
        if (tm < 0 || tm >= BGP_TONEMAP_COUNT) tm = 0;
        push_text(dd_x + 4, dd_y + 2, bgp_tonemap_names[tm],
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
        push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                  t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
    }
    BGP_NEXT_ROW;

    /* Exposure slider */
    if (BGP_ROW_VISIBLE) {
        _snprintf(vbuf, sizeof(vbuf), "%.1f", bg_tonemap_exposure);
        clr_draw_slider(x0 + 20, cy, slider_w, row_h, bg_tonemap_exposure,
                        0.2f, 4.0f, "Exposure", vbuf, vp_w, vp_h, t);
    }
    BGP_NEXT_ROW;

    /* --- Defaults section header --- */
    if (BGP_ROW_VISIBLE) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Defaults",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    BGP_NEXT_ROW;

    /* --- Reset to Defaults button --- */
    if (BGP_ROW_VISIBLE) {
        const char *btn = "[ Reset to Defaults ]";
        int blen = (int)strlen(btn);
        int bw = blen * cw + 16;
        int bx = (int)(x0 + (pw - sb_w - bw) * 0.5f);
        int by = (int)cy;
        int bh = ch + 6;
        /* Button bg */
        push_solid(bx, by, bx + bw, by + bh,
                   t->bg[0] + 0.08f, t->bg[1] + 0.08f, t->bg[2] + 0.08f, 0.9f, vp_w, vp_h);
        /* Top highlight */
        push_solid(bx, by, bx + bw, by + 1, 1.0f, 1.0f, 1.0f, 0.15f, vp_w, vp_h);
        /* Bottom shadow */
        push_solid(bx, by + bh - 1, bx + bw, by + bh, 0.0f, 0.0f, 0.0f, 0.25f, vp_w, vp_h);
        /* Accent border */
        push_solid(bx, by, bx + 1, by + bh,
                   t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_solid(bx + bw - 1, by, bx + bw, by + bh,
                   t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_text(bx + 8, by + 3, btn,
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    }
    BGP_NEXT_ROW;

    #undef BGP_ROW_VISIBLE
    #undef BGP_NEXT_ROW

    /* --- Scrollbar --- */
    {
        float sb_x0 = x0 + pw - sb_w - 2;
        float sb_y0 = content_top;
        float sb_y1 = content_bot;
        float track_h = sb_y1 - sb_y0;

        /* Track bg */
        push_solid((int)sb_x0, (int)sb_y0, (int)(sb_x0 + sb_w), (int)sb_y1,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);

        if (max_scroll > 0) {
            float thumb_frac = (float)max_visible / (float)BGP_TOTAL_ROWS;
            float thumb_h = track_h * thumb_frac;
            if (thumb_h < 16.0f) thumb_h = 16.0f;
            float thumb_pos = ((float)bgp_scroll / (float)max_scroll) * (track_h - thumb_h);
            float ty0 = sb_y0 + thumb_pos;
            float ty1 = ty0 + thumb_h;
            push_solid((int)(sb_x0 + 1), (int)ty0, (int)(sb_x0 + sb_w - 1), (int)ty1,
                       t->accent[0], t->accent[1], t->accent[2], 0.4f, vp_w, vp_h);
            push_solid((int)(sb_x0 + 1), (int)ty0, (int)(sb_x0 + sb_w - 1), (int)(ty0 + 1),
                       1.0f, 1.0f, 1.0f, 0.1f, vp_w, vp_h);
        }
    }

    /* Save this tab's scroll position so switching tabs preserves each one's view */
    bgp_tab_scroll[bgp_tab] = bgp_scroll;
}

/* ---- Starfield tab ----
 * Sliders mirror plasma's layout pattern. Per-band beat dropdowns use the
 * shared BGP_BEAT_* enum. Rows route through the same virtual-scroll. */

#define STAR_SLIDER_COUNT 14
static const char *bg_star_labels[STAR_SLIDER_COUNT] = {
    "Swerve",
    "Drift X", "Drift Y", "Drift Speed",
    "Parallax", "Layers",
    "Density", "Star Size", "Brightness",
    "Saturation", "Hue", "Sky Glow",
    "Twinkle Speed", "Twinkle Amt"
};
static float bg_star_slider_min[STAR_SLIDER_COUNT] = {
    0.0f,
    -0.10f, -0.10f, 0.0f,
    0.40f, 2.0f,
    0.20f, 0.15f, 0.0f,        /* Star Size min = 0.15 (can go tiny) */
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f
};
static float bg_star_slider_max[STAR_SLIDER_COUNT] = {
    1.0f,
    0.10f, 0.10f, 3.0f,
    0.95f, 8.0f,
    1.0f, 1.3f, 2.0f,          /* Star Size max = 1.3 (no giants) */
    2.0f, 360.0f, 1.0f,
    5.0f, 1.0f
};
static float *bg_star_slider_ptr[STAR_SLIDER_COUNT];

/* Virtual rows: [camera section hdr, camera dropdown], sliders, [beat hdr, 4 dds], [defaults hdr, reset btn] */
#define STAR_TOTAL_ROWS (STAR_SLIDER_COUNT + 9)

static int bgp_starfield_total_rows(void) { return STAR_TOTAL_ROWS; }

/* Helpers shared w/ plasma path */
static int star_hit_slider(int mx, int my, float *out_pct);
static int star_hit_dropdown(int mx, int my);
static int star_hit_defaults_button(int mx, int my);
static void star_reset_defaults(void);

static void bgp_draw_starfield_tab(float x0, float content_top, float content_bot,
                                   float pw, float sb_w, int vp_w, int vp_h,
                                   const ui_theme_t *t, int cw, int ch)
{
    bg_star_slider_ptr[0]  = &bg_star_swerve_amt;
    bg_star_slider_ptr[1]  = &bg_star_drift_x;
    bg_star_slider_ptr[2]  = &bg_star_drift_y;
    bg_star_slider_ptr[3]  = &bg_star_drift_speed;
    bg_star_slider_ptr[4]  = &bg_star_parallax;
    bg_star_slider_ptr[5]  = &bg_star_layer_count;
    bg_star_slider_ptr[6]  = &bg_star_density;
    bg_star_slider_ptr[7]  = &bg_star_size;
    bg_star_slider_ptr[8]  = &bg_star_brightness;
    bg_star_slider_ptr[9]  = &bg_star_saturation;
    bg_star_slider_ptr[10] = &bg_star_hue;
    bg_star_slider_ptr[11] = &bg_star_bg_glow;
    bg_star_slider_ptr[12] = &bg_star_twinkle_speed;
    bg_star_slider_ptr[13] = &bg_star_twinkle_amt;

    float row_h = ch + 14;
    float content_h = content_bot - content_top;
    float slider_w = pw - 40 - sb_w;
    int max_visible = (int)(content_h / row_h);
    if (max_visible < 3) max_visible = 3;
    int max_scroll = STAR_TOTAL_ROWS - max_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (bgp_scroll > max_scroll) bgp_scroll = max_scroll;
    if (bgp_scroll < 0) bgp_scroll = 0;

    float cy = content_top + 4;
    int drawn = 0;
    int vrow = 0;
    char vbuf[32];

    #define STAR_VIS (vrow >= bgp_scroll && drawn < max_visible)
    #define STAR_NEXT do { if (STAR_VIS) { cy += row_h; drawn++; } vrow++; } while(0)

    /* --- Camera section header --- */
    if (STAR_VIS) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Camera",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    STAR_NEXT;

    /* --- Camera mode dropdown --- */
    if (STAR_VIS) {
        push_text((int)(x0 + 20), (int)(cy + 2), "Mode:",
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
        int dd_x = (int)(x0 + 20 + 11 * cw);
        int dd_w = (int)(slider_w - 11 * cw - 2);
        int dd_y = (int)cy;
        int dd_h = ch + 4;
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                   t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                   1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
        push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        int mm = bg_star_camera_mode;
        if (mm < 0 || mm >= BG_STAR_MODE_COUNT) mm = 0;
        push_text(dd_x + 4, dd_y + 2, bg_star_mode_names[mm],
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
        push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                  t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
    }
    STAR_NEXT;

    /* --- Sliders --- */
    for (int i = 0; i < STAR_SLIDER_COUNT; i++) {
        if (STAR_VIS) {
            float val = *bg_star_slider_ptr[i];
            if (i == 10)        /* Hue degrees (was 9, +1 for Swerve) */
                _snprintf(vbuf, sizeof(vbuf), "%.0f\xF8", val);
            else if (i == 5)    /* Layers integer-ish (was 4, +1 for Swerve) */
                _snprintf(vbuf, sizeof(vbuf), "%.0f", val);
            else
                _snprintf(vbuf, sizeof(vbuf), "%.2f", val);
            clr_draw_slider(x0 + 20, cy, slider_w, row_h, val,
                            bg_star_slider_min[i], bg_star_slider_max[i],
                            bg_star_labels[i], vbuf, vp_w, vp_h, t);
        }
        STAR_NEXT;
    }

    /* --- Beat Response section header --- */
    if (STAR_VIS) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Beat Response",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    STAR_NEXT;

    /* --- Beat dropdowns --- */
    {
        struct { const char *label; int *val; } beats[4] = {
            { "Bass/Kick:", &bg_star_beat_bass },
            { "Mid:",       &bg_star_beat_mid },
            { "Treble:",    &bg_star_beat_treble },
            { "RMS:",       &bg_star_beat_rms }
        };
        for (int b = 0; b < 4; b++) {
            if (STAR_VIS) {
                push_text((int)(x0 + 20), (int)(cy + 2), beats[b].label,
                          t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
                int dd_x = (int)(x0 + 20 + 11 * cw);
                int dd_w = (int)(slider_w - 11 * cw - 2);
                int dd_y = (int)cy;
                int dd_h = ch + 4;
                push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                           t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
                push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                           1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
                push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                           0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
                int mode = *beats[b].val;
                if (mode < 0 || mode >= BGP_BEAT_COUNT) mode = 0;
                push_text(dd_x + 4, dd_y + 2, bgp_beat_names[mode],
                          t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
                push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                          t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
            }
            STAR_NEXT;
        }
    }

    /* --- Defaults section header --- */
    if (STAR_VIS) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Defaults",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    STAR_NEXT;

    /* --- Reset button --- */
    if (STAR_VIS) {
        const char *btn = "[ Reset to Defaults ]";
        int blen = (int)strlen(btn);
        int bw = blen * cw + 16;
        int bx = (int)(x0 + (pw - sb_w - bw) * 0.5f);
        int by = (int)cy;
        int bh = ch + 6;
        push_solid(bx, by, bx + bw, by + bh,
                   t->bg[0] + 0.08f, t->bg[1] + 0.08f, t->bg[2] + 0.08f, 0.9f, vp_w, vp_h);
        push_solid(bx, by, bx + bw, by + 1, 1.0f, 1.0f, 1.0f, 0.15f, vp_w, vp_h);
        push_solid(bx, by + bh - 1, bx + bw, by + bh, 0.0f, 0.0f, 0.0f, 0.25f, vp_w, vp_h);
        push_solid(bx, by, bx + 1, by + bh,
                   t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_solid(bx + bw - 1, by, bx + bw, by + bh,
                   t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_text(bx + 8, by + 3, btn,
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    }
    STAR_NEXT;

    #undef STAR_VIS
    #undef STAR_NEXT

    /* Scrollbar */
    {
        float sb_x0 = x0 + pw - sb_w - 2;
        push_solid((int)sb_x0, (int)content_top, (int)(sb_x0 + sb_w), (int)content_bot,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        if (max_scroll > 0) {
            float track_h = content_bot - content_top;
            float thumb_frac = (float)max_visible / (float)STAR_TOTAL_ROWS;
            float thumb_h = track_h * thumb_frac;
            if (thumb_h < 16.0f) thumb_h = 16.0f;
            float thumb_pos = ((float)bgp_scroll / (float)max_scroll) * (track_h - thumb_h);
            float ty0 = content_top + thumb_pos;
            float ty1 = ty0 + thumb_h;
            push_solid((int)(sb_x0 + 1), (int)ty0, (int)(sb_x0 + sb_w - 1), (int)ty1,
                       t->accent[0], t->accent[1], t->accent[2], 0.4f, vp_w, vp_h);
            push_solid((int)(sb_x0 + 1), (int)ty0, (int)(sb_x0 + sb_w - 1), (int)(ty0 + 1),
                       1.0f, 1.0f, 1.0f, 0.1f, vp_w, vp_h);
        }
    }
}

/* Starfield-tab hit tests
 * Layout: row 0 = camera hdr, row 1 = camera dropdown,
 *         rows 2..STAR_SLIDER_COUNT+1 = sliders,
 *         row +2 = beat hdr, +3..+6 = beat dropdowns,
 *         row +7 = defaults hdr, row +8 = reset button */
#define STAR_SLIDER_BASE_ROW      2
#define STAR_BEAT_DD_BASE_ROW     (STAR_SLIDER_COUNT + STAR_SLIDER_BASE_ROW + 1)
#define STAR_DEFAULTS_BTN_ROW     (STAR_SLIDER_COUNT + STAR_SLIDER_BASE_ROW + 6)
#define STAR_CAMERA_DD_ROW        1

static int star_hit_slider(int mx, int my, float *out_pct) {
    if (bgp_tab != BGP_TAB_STARFIELD) return -1;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float sx0 = bgp_x + 20 + (float)(12 * cw);
    float sx1 = bgp_x + 20 + slider_w - (float)(7 * cw) + 8;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    for (int i = 0; i < STAR_SLIDER_COUNT; i++) {
        int vrow = STAR_SLIDER_BASE_ROW + i - bgp_scroll;
        if (vrow < 0) continue;
        float sy = content_top + 4 + vrow * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    return -1;
}

/* Returns -1 none, 0..3 for bass/mid/treble/rms beat dropdowns,
 * 100 for the camera mode dropdown. */
static int star_hit_dropdown(int mx, int my) {
    if (bgp_tab != BGP_TAB_STARFIELD) return -1;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    int dd_x = (int)(bgp_x + 20 + 11 * cw);
    int dd_w = (int)(slider_w - 11 * cw - 2);
    if (mx < dd_x || mx >= dd_x + dd_w) return -1;

    /* Camera mode dropdown */
    {
        int vrow = STAR_CAMERA_DD_ROW - bgp_scroll;
        if (vrow >= 0) {
            float ry = content_top + 4 + vrow * row_h;
            if (my >= (int)ry && my < (int)(ry + row_h)) return 100;
        }
    }
    /* Beat dropdowns */
    for (int b = 0; b < 4; b++) {
        int vrow = STAR_BEAT_DD_BASE_ROW + b - bgp_scroll;
        if (vrow < 0) continue;
        float ry = content_top + 4 + vrow * row_h;
        if (my >= (int)ry && my < (int)(ry + row_h)) return b;
    }
    return -1;
}

static int star_hit_defaults_button(int mx, int my) {
    if (bgp_tab != BGP_TAB_STARFIELD) return 0;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    int vrow = STAR_DEFAULTS_BTN_ROW - bgp_scroll;
    if (vrow < 0) return 0;
    float ry = content_top + 4 + vrow * row_h;
    const char *btn = "[ Reset to Defaults ]";
    int blen = (int)strlen(btn);
    int bw = blen * cw + 16;
    int bx = (int)(bgp_x + (bgp_w - sb_w - bw) * 0.5f);
    int bh = ch + 6;
    return (mx >= bx && mx < bx + bw && my >= (int)ry && my < (int)ry + bh);
}

static void star_reset_defaults(void) {
    bg_star_camera_mode = BG_STAR_MODE_WARP;
    bg_star_swerve_amt = 0.35f;
    bg_star_drift_x = 0.0f; bg_star_drift_y = 0.0f;
    bg_star_drift_speed = 1.0f;
    bg_star_parallax = 0.62f; bg_star_layer_count = 8.0f;
    bg_star_density = 1.0f; bg_star_size = 1.0f;
    bg_star_brightness = 1.0f; bg_star_saturation = 1.0f;
    bg_star_hue = 0.0f; bg_star_bg_glow = 0.35f;
    bg_star_twinkle_speed = 1.2f; bg_star_twinkle_amt = 0.55f;
    bg_star_beat_bass = BGP_BEAT_PULSE;
    bg_star_beat_mid = BGP_BEAT_TWINKLE;
    bg_star_beat_treble = BGP_BEAT_JITTER;
    bg_star_beat_rms = BGP_BEAT_BRIGHT;
}

/* ---- Alien Wall tab ---- */

#define ALIEN_SLIDER_COUNT 18
static const char *bg_alien_labels[ALIEN_SLIDER_COUNT] = {
    "Speed", "Cell Scale", "Warp Strength", "Hue Shift",
    "Saturation", "Brightness", "Opacity",
    "Vein Strength", "Pulse Speed", "Glow Intensity",
    "Depth Layers", "Roughness", "Membrane Mix",
    "Wet/Glossy", "Fluid Speed", "Specular Tight",
    "Cell Move Speed", "Bulge Strength"
};
static float bg_alien_slider_min[ALIEN_SLIDER_COUNT] = {
    0.1f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
    1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.0f,
    0.0f, 0.0f
};
static float bg_alien_slider_max[ALIEN_SLIDER_COUNT] = {
    5.0f, 8.0f, 4.0f, 360.0f,
    2.0f, 2.0f, 1.0f,
    2.0f, 4.0f, 2.0f,
    4.0f, 1.0f, 1.0f,
    2.0f, 2.0f, 1.0f,
    2.0f, 2.0f
};
static float *bg_alien_slider_ptr[ALIEN_SLIDER_COUNT];

#define ALIEN_TOTAL_ROWS (ALIEN_SLIDER_COUNT + 9)

static void bgp_draw_alienwall_tab(float x0, float content_top, float content_bot,
                                   float pw, float sb_w, int vp_w, int vp_h,
                                   const ui_theme_t *t, int cw, int ch)
{
    bg_alien_slider_ptr[0]  = &bg_alien_speed;
    bg_alien_slider_ptr[1]  = &bg_alien_cell_scale;
    bg_alien_slider_ptr[2]  = &bg_alien_warp;
    bg_alien_slider_ptr[3]  = &bg_alien_hue;
    bg_alien_slider_ptr[4]  = &bg_alien_saturation;
    bg_alien_slider_ptr[5]  = &bg_alien_brightness;
    bg_alien_slider_ptr[6]  = &bg_alien_alpha;
    bg_alien_slider_ptr[7]  = &bg_alien_vein;
    bg_alien_slider_ptr[8]  = &bg_alien_pulse_speed;
    bg_alien_slider_ptr[9]  = &bg_alien_glow;
    bg_alien_slider_ptr[10] = &bg_alien_depth_layers;
    bg_alien_slider_ptr[11] = &bg_alien_roughness;
    bg_alien_slider_ptr[12] = &bg_alien_membrane;
    bg_alien_slider_ptr[13] = &bg_alien_wet;
    bg_alien_slider_ptr[14] = &bg_alien_fluid_speed;
    bg_alien_slider_ptr[15] = &bg_alien_specular;
    bg_alien_slider_ptr[16] = &bg_alien_cell_move;
    bg_alien_slider_ptr[17] = &bg_alien_bulge;

    float row_h = ch + 14;
    float content_h = content_bot - content_top;
    float slider_w = pw - 40 - sb_w;
    int max_visible = (int)(content_h / row_h);
    if (max_visible < 3) max_visible = 3;
    int max_scroll = ALIEN_TOTAL_ROWS - max_visible;
    if (max_scroll < 0) max_scroll = 0;
    if (bgp_scroll > max_scroll) bgp_scroll = max_scroll;
    if (bgp_scroll < 0) bgp_scroll = 0;

    float cy = content_top + 4;
    int drawn = 0;
    int vrow = 0;
    char vbuf[32];

    #define ALIEN_VIS (vrow >= bgp_scroll && drawn < max_visible)
    #define ALIEN_NEXT do { if (ALIEN_VIS) { cy += row_h; drawn++; } vrow++; } while(0)

    /* --- Sliders --- */
    for (int i = 0; i < ALIEN_SLIDER_COUNT; i++) {
        if (ALIEN_VIS) {
            float val = *bg_alien_slider_ptr[i];
            if (i == 3)
                _snprintf(vbuf, sizeof(vbuf), "%.0f\xF8", val);
            else if (i == 10)
                _snprintf(vbuf, sizeof(vbuf), "%.0f", val);
            else
                _snprintf(vbuf, sizeof(vbuf), "%.2f", val);
            clr_draw_slider(x0 + 20, cy, slider_w, row_h, val,
                            bg_alien_slider_min[i], bg_alien_slider_max[i],
                            bg_alien_labels[i], vbuf, vp_w, vp_h, t);
        }
        ALIEN_NEXT;
    }

    /* --- Cell Style section header --- */
    if (ALIEN_VIS) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Cell Geometry",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    ALIEN_NEXT;

    /* --- Cell Style dropdown --- */
    if (ALIEN_VIS) {
        push_text((int)(x0 + 20), (int)(cy + 2), "Cell Style:",
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
        int dd_x = (int)(x0 + 20 + 11 * cw);
        int dd_w = (int)(slider_w - 11 * cw - 2);
        int dd_y = (int)cy;
        int dd_h = ch + 4;
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                   t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
        push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                   1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
        push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        int sty = bg_alien_cell_style;
        if (sty < 0 || sty >= BG_ALIEN_STYLE_COUNT) sty = 0;
        push_text(dd_x + 4, dd_y + 2, bg_alien_style_names[sty],
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
        push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                  t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
    }
    ALIEN_NEXT;

    /* --- Beat Response section header --- */
    if (ALIEN_VIS) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Beat Response",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    ALIEN_NEXT;

    /* --- Beat dropdowns --- */
    {
        struct { const char *label; int *val; } beats[4] = {
            { "Bass/Kick:", &bg_alien_beat_bass },
            { "Mid:",       &bg_alien_beat_mid },
            { "Treble:",    &bg_alien_beat_treble },
            { "RMS:",       &bg_alien_beat_rms }
        };
        for (int b = 0; b < 4; b++) {
            if (ALIEN_VIS) {
                push_text((int)(x0 + 20), (int)(cy + 2), beats[b].label,
                          t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
                int dd_x = (int)(x0 + 20 + 11 * cw);
                int dd_w = (int)(slider_w - 11 * cw - 2);
                int dd_y = (int)cy;
                int dd_h = ch + 4;
                push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + dd_h,
                           t->bg[0] + 0.05f, t->bg[1] + 0.05f, t->bg[2] + 0.05f, 0.9f, vp_w, vp_h);
                push_solid(dd_x, dd_y, dd_x + dd_w, dd_y + 1,
                           1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
                push_solid(dd_x, dd_y + dd_h - 1, dd_x + dd_w, dd_y + dd_h,
                           0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
                int mode = *beats[b].val;
                if (mode < 0 || mode >= BGP_BEAT_COUNT) mode = 0;
                push_text(dd_x + 4, dd_y + 2, bgp_beat_names[mode],
                          t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
                push_text(dd_x + dd_w - cw - 2, dd_y + 2, "\x1F",
                          t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);
            }
            ALIEN_NEXT;
        }
    }

    /* --- Defaults section header --- */
    if (ALIEN_VIS) {
        push_solid((int)(x0 + 20), (int)(cy + row_h * 0.3f),
                   (int)(x0 + pw - sb_w - 20), (int)(cy + row_h * 0.35f),
                   t->accent[0], t->accent[1], t->accent[2], 0.3f, vp_w, vp_h);
        push_text((int)(x0 + 20), (int)(cy + 2), "Defaults",
                  t->accent[0], t->accent[1], t->accent[2], vp_w, vp_h, cw, ch);
    }
    ALIEN_NEXT;

    /* --- Reset button --- */
    if (ALIEN_VIS) {
        const char *btn = "[ Reset to Defaults ]";
        int blen = (int)strlen(btn);
        int bw = blen * cw + 16;
        int bx = (int)(x0 + (pw - sb_w - bw) * 0.5f);
        int by = (int)cy;
        int bh = ch + 6;
        push_solid(bx, by, bx + bw, by + bh,
                   t->bg[0] + 0.08f, t->bg[1] + 0.08f, t->bg[2] + 0.08f, 0.9f, vp_w, vp_h);
        push_solid(bx, by, bx + bw, by + 1, 1.0f, 1.0f, 1.0f, 0.15f, vp_w, vp_h);
        push_solid(bx, by + bh - 1, bx + bw, by + bh, 0.0f, 0.0f, 0.0f, 0.25f, vp_w, vp_h);
        push_solid(bx, by, bx + 1, by + bh,
                   t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_solid(bx + bw - 1, by, bx + bw, by + bh,
                   t->accent[0], t->accent[1], t->accent[2], 0.5f, vp_w, vp_h);
        push_text(bx + 8, by + 3, btn,
                  t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    }
    ALIEN_NEXT;

    #undef ALIEN_VIS
    #undef ALIEN_NEXT

    /* Scrollbar */
    {
        float sb_x0 = x0 + pw - sb_w - 2;
        push_solid((int)sb_x0, (int)content_top, (int)(sb_x0 + sb_w), (int)content_bot,
                   0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        if (max_scroll > 0) {
            float track_h = content_bot - content_top;
            float thumb_frac = (float)max_visible / (float)ALIEN_TOTAL_ROWS;
            float thumb_h = track_h * thumb_frac;
            if (thumb_h < 16.0f) thumb_h = 16.0f;
            float thumb_pos = ((float)bgp_scroll / (float)max_scroll) * (track_h - thumb_h);
            float ty0 = content_top + thumb_pos;
            float ty1 = ty0 + thumb_h;
            push_solid((int)(sb_x0 + 1), (int)ty0, (int)(sb_x0 + sb_w - 1), (int)ty1,
                       t->accent[0], t->accent[1], t->accent[2], 0.4f, vp_w, vp_h);
            push_solid((int)(sb_x0 + 1), (int)ty0, (int)(sb_x0 + sb_w - 1), (int)(ty0 + 1),
                       1.0f, 1.0f, 1.0f, 0.1f, vp_w, vp_h);
        }
    }
}

/* Alien Wall hit tests */
#define ALIEN_SLIDER_BASE_ROW      0
#define ALIEN_STYLE_DD_ROW         (ALIEN_SLIDER_COUNT + 1)
#define ALIEN_BEAT_DD_BASE_ROW     (ALIEN_SLIDER_COUNT + 3)
#define ALIEN_DEFAULTS_BTN_ROW     (ALIEN_SLIDER_COUNT + 8)

static int alien_hit_slider(int mx, int my, float *out_pct) {
    if (bgp_tab != BGP_TAB_ALIENWALL) return -1;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float sx0 = bgp_x + 20 + (float)(12 * cw);
    float sx1 = bgp_x + 20 + slider_w - (float)(7 * cw) + 8;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    for (int i = 0; i < ALIEN_SLIDER_COUNT; i++) {
        int vr = ALIEN_SLIDER_BASE_ROW + i - bgp_scroll;
        if (vr < 0) continue;
        float sy = content_top + 4 + vr * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    return -1;
}

static int alien_hit_dropdown(int mx, int my) {
    if (bgp_tab != BGP_TAB_ALIENWALL) return -1;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    int dd_x = (int)(bgp_x + 20 + 11 * cw);
    int dd_w = (int)(slider_w - 11 * cw - 2);
    if (mx < dd_x || mx >= dd_x + dd_w) return -1;
    for (int b = 0; b < 4; b++) {
        int vr = ALIEN_BEAT_DD_BASE_ROW + b - bgp_scroll;
        if (vr < 0) continue;
        float ry = content_top + 4 + vr * row_h;
        if (my >= (int)ry && my < (int)(ry + row_h)) return b;
    }
    return -1;
}

static int alien_hit_style_dropdown(int mx, int my) {
    if (bgp_tab != BGP_TAB_ALIENWALL) return 0;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    int dd_x = (int)(bgp_x + 20 + 11 * cw);
    int dd_w = (int)(slider_w - 11 * cw - 2);
    if (mx < dd_x || mx >= dd_x + dd_w) return 0;
    int vr = ALIEN_STYLE_DD_ROW - bgp_scroll;
    if (vr < 0) return 0;
    float ry = content_top + 4 + vr * row_h;
    return (my >= (int)ry && my < (int)(ry + row_h));
}

static int alien_hit_defaults_button(int mx, int my) {
    if (bgp_tab != BGP_TAB_ALIENWALL) return 0;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    int vr = ALIEN_DEFAULTS_BTN_ROW - bgp_scroll;
    if (vr < 0) return 0;
    float ry = content_top + 4 + vr * row_h;
    const char *btn = "[ Reset to Defaults ]";
    int blen = (int)strlen(btn);
    int bw = blen * cw + 16;
    int bx = (int)(bgp_x + (bgp_w - sb_w - bw) * 0.5f);
    int bh = ch + 6;
    return (mx >= bx && mx < bx + bw && my >= (int)ry && my < (int)ry + bh);
}

static void alien_reset_defaults(void) {
    bg_alien_speed = 0.8f; bg_alien_cell_scale = 3.0f;
    bg_alien_warp = 1.5f; bg_alien_hue = 120.0f;
    bg_alien_saturation = 0.85f; bg_alien_brightness = 0.7f;
    bg_alien_alpha = 0.35f; bg_alien_vein = 1.0f;
    bg_alien_pulse_speed = 1.0f; bg_alien_glow = 0.8f;
    bg_alien_depth_layers = 2.0f; bg_alien_roughness = 0.2f;
    bg_alien_membrane = 0.4f; bg_alien_wet = 1.0f;
    bg_alien_fluid_speed = 0.8f; bg_alien_specular = 0.5f;
    bg_alien_cell_move = 0.5f; bg_alien_bulge = 0.6f;
    bg_alien_cell_style = 0;
    bg_alien_beat_bass = BGP_BEAT_BRIGHT;
    bg_alien_beat_mid = BGP_BEAT_HUE;
    bg_alien_beat_treble = BGP_BEAT_ZOOM;
    bg_alien_beat_rms = BGP_BEAT_NONE;
}

static int bgp_hit_slider(int mx, int my, float *out_pct) {
    if (bgp_tab != BGP_TAB_PLASMA) return -1;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float sx0 = bgp_x + 20 + (float)(12 * cw);
    float sx1 = bgp_x + 20 + slider_w - (float)(7 * cw) + 8;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    for (int i = 0; i < BGP_SLIDER_COUNT; i++) {
        int vrow = i - bgp_scroll;
        if (vrow < 0) continue;
        float sy = content_top + 4 + vrow * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    /* Exposure slider at virtual row BGP_SLIDER_COUNT + 8 */
    {
        int vrow = BGP_SLIDER_COUNT + 8 - bgp_scroll;
        if (vrow >= 0) {
            float sy = content_top + 4 + vrow * row_h;
            if (my >= (int)sy && my < (int)(sy + row_h) &&
                mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
                float pct = ((float)mx - sx0) / (sx1 - sx0);
                if (pct < 0) pct = 0; if (pct > 1) pct = 1;
                *out_pct = pct;
                return BGP_SLIDER_COUNT; /* special index for exposure */
            }
        }
    }
    return -1;
}

static void bgp_set_from_pct(int slider, float pct) {
    if (slider == BGP_SLIDER_COUNT) {
        /* Exposure slider */
        bg_tonemap_exposure = 0.2f + pct * 3.8f;
        return;
    }
    if (slider < 0 || slider >= BGP_SLIDER_COUNT) return;
    float val = bgp_slider_min[slider] + pct * (bgp_slider_max[slider] - bgp_slider_min[slider]);
    *bgp_slider_ptr[slider] = val;
}

/* Returns dropdown index: -1=none, 0=material, 1-4=beat bands */
static int bgp_hit_dropdown(int mx, int my) {
    if (bgp_tab != BGP_TAB_PLASMA) return -1;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float slider_w = bgp_w - 40 - sb_w;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    int dd_x = (int)(bgp_x + 20 + 11 * cw);
    int dd_w = (int)(slider_w - 11 * cw - 2);
    if (mx < dd_x || mx >= dd_x + dd_w) return -1;
    /* Material dropdown at virtual row BGP_SLIDER_COUNT */
    {
        int vrow = BGP_SLIDER_COUNT - bgp_scroll;
        if (vrow >= 0) {
            float ry = content_top + 4 + vrow * row_h;
            if (my >= (int)ry && my < (int)(ry + row_h)) return 0;
        }
    }
    /* Beat dropdowns at virtual rows BGP_SLIDER_COUNT + 2..5 */
    for (int b = 0; b < 4; b++) {
        int vrow = BGP_SLIDER_COUNT + 2 + b - bgp_scroll;
        if (vrow < 0) continue;
        float ry = content_top + 4 + vrow * row_h;
        if (my >= (int)ry && my < (int)(ry + row_h)) return b + 1;
    }
    /* Tonemap dropdown at virtual row BGP_SLIDER_COUNT + 7 */
    {
        int vrow = BGP_SLIDER_COUNT + 7 - bgp_scroll;
        if (vrow >= 0) {
            float ry = content_top + 4 + vrow * row_h;
            if (my >= (int)ry && my < (int)(ry + row_h)) return 5;
        }
    }
    return -1;
}

/* Returns 1 if mouse is over the Reset-to-Defaults button in the plasma tab */
static int bgp_hit_defaults_button(int mx, int my) {
    if (bgp_tab != BGP_TAB_PLASMA) return 0;
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    float row_h = ch + 14;
    float sb_w = 10.0f;
    float content_top = bgp_y + titlebar_h + tab_bar_h + 4;
    /* Defaults button is at virtual row BGP_SLIDER_COUNT + 10 */
    int vrow = BGP_SLIDER_COUNT + 10 - bgp_scroll;
    if (vrow < 0) return 0;
    float ry = content_top + 4 + vrow * row_h;
    const char *btn = "[ Reset to Defaults ]";
    int blen = (int)strlen(btn);
    int bw = blen * cw + 16;
    int bx = (int)(bgp_x + (bgp_w - sb_w - bw) * 0.5f);
    int bh = ch + 6;
    return (mx >= bx && mx < bx + bw && my >= (int)ry && my < (int)ry + bh);
}

/* Reset only the currently active tab's settings to defaults. */
static void bgp_reset_current_tab_defaults(void) {
    if (bgp_tab == BGP_TAB_PLASMA) {
        bg_plasma_speed = 1.0f; bg_plasma_turbulence = 0.5f;
        bg_plasma_complexity = 4.0f; bg_plasma_hue = 0.0f;
        bg_plasma_saturation = 0.8f; bg_plasma_brightness = 0.7f;
        bg_plasma_alpha = 0.35f;
        bg_plasma_sobel = 0.0f; bg_plasma_sobel_angle = 135.0f;
        bg_plasma_edge_glow = 0.0f; bg_plasma_sharpen = 0.0f;
        bg_plasma_material = 0; bg_plasma_mat_str = 1.0f;
        bg_plasma_beat_bass = 0; bg_plasma_beat_mid = 0;
        bg_plasma_beat_treble = 0; bg_plasma_beat_rms = 0;
        bg_tonemap_mode = 0; bg_tonemap_exposure = 1.0f;
    }
    /* Starfield defaults will be wired when its controls land. */
}

static int bgp_mouse_down(int mx, int my) {
    if (!bgp_visible) return 0;
    if (mx < (int)bgp_x || mx >= (int)(bgp_x + bgp_w) ||
        my < (int)bgp_y || my >= (int)(bgp_y + bgp_h)) return 0;
    int cw = (int)(VKM_CHAR_W * ui_scale);
    int ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int tab_bar_h = ch + 10;
    int ly = my - (int)bgp_y;

    /* Resize grip: bottom-right corner. Catch BEFORE other hit tests. */
    if (mx >= (int)(bgp_x + bgp_w - BGP_RESIZE_ZONE) &&
        my >= (int)(bgp_y + bgp_h - BGP_RESIZE_ZONE)) {
        bgp_resizing = 1;
        bgp_drag_ox = (float)mx - (bgp_x + bgp_w);  /* offset from right edge */
        bgp_drag_oy = (float)my - (bgp_y + bgp_h);  /* offset from bottom edge */
        return 1;
    }

    /* Close X: titlebar right side, with padding margin */
    if (ly < titlebar_h && mx >= (int)(bgp_x + bgp_w - 14 - cw - 4) &&
        mx < (int)(bgp_x + bgp_w - 4)) {
        bgp_visible = 0;
        return 1;
    }
    if (ly < titlebar_h) {
        bgp_dragging = 1;
        bgp_drag_ox = (float)mx - bgp_x;
        bgp_drag_oy = (float)my - bgp_y;
        return 1;
    }
    /* Tab bar click — switch active tab */
    if (ly >= titlebar_h && ly < titlebar_h + tab_bar_h) {
        float tx = bgp_x + 4;
        for (int i = 0; i < BGP_TAB_COUNT; i++) {
            int tlen = (int)strlen(bgp_tab_names[i]);
            float tw = (float)(tlen * cw) + 16.0f;
            if (mx >= (int)tx && mx < (int)(tx + tw)) {
                if (bgp_tab != i) {
                    bgp_tab = i;
                    bgp_active_slider = -1;
                    bgp_scroll = bgp_tab_scroll[i];
                }
                return 1;
            }
            tx += tw + 2;
        }
        return 1;
    }
    /* Reset-to-Defaults button (any tab) */
    if (bgp_hit_defaults_button(mx, my)) {
        bgp_reset_current_tab_defaults();
        return 1;
    }
    if (star_hit_defaults_button(mx, my)) {
        star_reset_defaults();
        return 1;
    }
    if (alien_hit_defaults_button(mx, my)) {
        alien_reset_defaults();
        return 1;
    }
    /* Alien Wall tab: its own slider + dropdown hit paths */
    if (bgp_tab == BGP_TAB_ALIENWALL) {
        float apct;
        int as = alien_hit_slider(mx, my, &apct);
        if (as >= 0) {
            bgp_active_slider = 200 + as;
            float val = bg_alien_slider_min[as] + apct *
                        (bg_alien_slider_max[as] - bg_alien_slider_min[as]);
            *bg_alien_slider_ptr[as] = val;
            return 1;
        }
        if (alien_hit_style_dropdown(mx, my)) {
            bg_alien_cell_style = (bg_alien_cell_style + 1) % BG_ALIEN_STYLE_COUNT;
            return 1;
        }
        int add = alien_hit_dropdown(mx, my);
        if (add >= 0) {
            int *val = NULL;
            if (add == 0) val = &bg_alien_beat_bass;
            else if (add == 1) val = &bg_alien_beat_mid;
            else if (add == 2) val = &bg_alien_beat_treble;
            else if (add == 3) val = &bg_alien_beat_rms;
            if (val) *val = (*val + 1) % BGP_BEAT_COUNT;
            return 1;
        }
        return 1;
    }
    /* Starfield tab: its own slider + dropdown hit paths */
    if (bgp_tab == BGP_TAB_STARFIELD) {
        float spct;
        int ss = star_hit_slider(mx, my, &spct);
        if (ss >= 0) {
            /* Active slider index space is shared with plasma — use high bits
             * to distinguish: STAR starts at 100. */
            bgp_active_slider = 100 + ss;
            float val = bg_star_slider_min[ss] + spct *
                        (bg_star_slider_max[ss] - bg_star_slider_min[ss]);
            *bg_star_slider_ptr[ss] = val;
            return 1;
        }
        int sdd = star_hit_dropdown(mx, my);
        if (sdd >= 0) {
            if (sdd == 100) {
                /* Camera mode dropdown — cycle through modes */
                bg_star_camera_mode = (bg_star_camera_mode + 1) % BG_STAR_MODE_COUNT;
            } else {
                int *val = NULL;
                if (sdd == 0) val = &bg_star_beat_bass;
                else if (sdd == 1) val = &bg_star_beat_mid;
                else if (sdd == 2) val = &bg_star_beat_treble;
                else if (sdd == 3) val = &bg_star_beat_rms;
                if (val) *val = (*val + 1) % BGP_BEAT_COUNT;
            }
            return 1;
        }
        return 1;
    }
    /* Slider click (plasma tab) */
    float pct;
    int s = bgp_hit_slider(mx, my, &pct);
    if (s >= 0) {
        bgp_active_slider = s;
        bgp_set_from_pct(s, pct);
        return 1;
    }
    /* Dropdown click — cycle to next mode */
    int dd = bgp_hit_dropdown(mx, my);
    if (dd >= 0) {
        if (dd == 0) {
            /* Material dropdown */
            bg_plasma_material = (bg_plasma_material + 1) % BGP_MAT_COUNT;
        } else if (dd == 5) {
            /* Tonemap dropdown */
            bg_tonemap_mode = (bg_tonemap_mode + 1) % BGP_TONEMAP_COUNT;
        } else {
            /* Beat dropdowns (1-4 maps to bass/mid/treble/rms) */
            int *val = NULL;
            if (dd == 1) val = &bg_plasma_beat_bass;
            else if (dd == 2) val = &bg_plasma_beat_mid;
            else if (dd == 3) val = &bg_plasma_beat_treble;
            else if (dd == 4) val = &bg_plasma_beat_rms;
            if (val) *val = (*val + 1) % BGP_BEAT_COUNT;
        }
        return 1;
    }
    return 1;
}

static void bgp_mouse_move(int mx, int my) {
    if (bgp_dragging) {
        bgp_x = (float)mx - bgp_drag_ox;
        bgp_y = (float)my - bgp_drag_oy;
        return;
    }
    if (bgp_resizing) {
        /* drag_ox/oy are offsets from the right/bottom edges at click time */
        float new_w = (float)mx - bgp_x - bgp_drag_ox;
        float new_h = (float)my - bgp_y - bgp_drag_oy;
        if (new_w < BGP_MIN_W) new_w = BGP_MIN_W;
        if (new_h < BGP_MIN_H) new_h = BGP_MIN_H;
        bgp_w = new_w;
        bgp_h = new_h;
        return;
    }
    if (bgp_active_slider >= 0) {
        int cw = (int)(VKM_CHAR_W * ui_scale);
        float sb_w = 10.0f;
        float slider_w = bgp_w - 40 - sb_w;
        float sx0 = bgp_x + 20 + (float)(12 * cw);
        float sx1 = bgp_x + 20 + slider_w - (float)(7 * cw) + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        if (bgp_active_slider >= 200 && bgp_active_slider < 200 + ALIEN_SLIDER_COUNT) {
            int ai = bgp_active_slider - 200;
            float val = bg_alien_slider_min[ai] + pct *
                        (bg_alien_slider_max[ai] - bg_alien_slider_min[ai]);
            *bg_alien_slider_ptr[ai] = val;
        } else if (bgp_active_slider >= 100 && bgp_active_slider < 100 + STAR_SLIDER_COUNT) {
            int si = bgp_active_slider - 100;
            float val = bg_star_slider_min[si] + pct *
                        (bg_star_slider_max[si] - bg_star_slider_min[si]);
            *bg_star_slider_ptr[si] = val;
        } else {
            bgp_set_from_pct(bgp_active_slider, pct);
        }
    }
}

static void bgp_mouse_up(void) {
    bgp_dragging = 0;
    bgp_resizing = 0;
    bgp_active_slider = -1;
    bgp_scroll_dragging = 0;
}

/* ---- Smoky Letters Settings Panel ---- */

static int smk_scaled = 0;
static void smk_draw(int vp_w, int vp_h) {
    if (!smk_visible) return;
    if (!smk_scaled) { smk_w = 280*ui_scale; smk_h = 200*ui_scale; smk_x = 20*ui_scale; smk_y = 100*ui_scale; smk_scaled = 1; }
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = smk_x, y0 = smk_y;
    float pw = smk_w;
    int titlebar_h = ch + 8;

    smk_slider_ptr[0] = &smoke_decay;
    smk_slider_ptr[1] = &smoke_depth;
    smk_slider_ptr[2] = &smoke_zoom;
    smk_slider_ptr[3] = &smoke_hue;
    smk_slider_ptr[4] = &smoke_saturation;
    smk_slider_ptr[5] = &smoke_value;

    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + smk_h),
               t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)(y0 + smk_h - 1), (int)(x0 + pw), (int)(y0 + smk_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + 1), (int)(y0 + smk_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)(x0 + pw - 1), (int)y0, (int)(x0 + pw), (int)(y0 + smk_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);

    {
        float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
        float tbr = acr * 0.25f + t->bg[0] * 0.5f;
        float tbg = acg * 0.25f + t->bg[1] * 0.5f;
        float tbb = acb * 0.25f + t->bg[2] * 0.5f;
        int ty0 = (int)y0 + 2, ty1 = (int)y0 + titlebar_h;
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty1,
                   tbr, tbg, tbb, 0.95f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty1 - 1, (int)(x0 + pw) - 2, ty1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
    }
    push_text((int)(x0 + 6) + 1, (int)(y0 + 4) + 1, "Smoky Letters",
              0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + 6), (int)(y0 + 4), "Smoky Letters",
              t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 16), (int)(y0 + 4), "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 55), (int)(y0 + 4), "Reset",
              t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);

    float cy = y0 + titlebar_h + 12;
    float row_h = ch + 14;
    char vbuf[32];

    for (int i = 0; i < SMK_SLIDER_COUNT; i++) {
        float val = *smk_slider_ptr[i];
        if (i == 3)
            _snprintf(vbuf, sizeof(vbuf), "%.0f\xF8", val);
        else
            _snprintf(vbuf, sizeof(vbuf), "%.2f", val);

        clr_draw_slider(x0 + 8, cy, pw - 16, row_h, val,
                        smk_slider_min[i], smk_slider_max[i],
                        smk_labels[i], vbuf, vp_w, vp_h, t);
        cy += row_h;
    }

    smk_h = cy - y0 + 8;
}

static int smk_hit_slider(int mx, int my, float *out_pct) {
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    float row_h = ch + 14;
    float sx0 = smk_x + 8 + 110;
    float sx1 = smk_x + smk_w - 16 - 50 + 8;
    for (int i = 0; i < SMK_SLIDER_COUNT; i++) {
        float sy = smk_y + titlebar_h + 12 + i * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    return -1;
}

static void smk_set_from_pct(int slider, float pct) {
    if (slider < 0 || slider >= SMK_SLIDER_COUNT) return;
    float val = smk_slider_min[slider] + pct * (smk_slider_max[slider] - smk_slider_min[slider]);
    *smk_slider_ptr[slider] = val;
}

static int smk_mouse_down(int mx, int my) {
    if (!smk_visible) return 0;
    if (mx < (int)smk_x || mx >= (int)(smk_x + smk_w) ||
        my < (int)smk_y || my >= (int)(smk_y + smk_h)) return 0;
    int ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int ly = my - (int)smk_y;
    if (ly < titlebar_h && mx >= (int)(smk_x + smk_w - 20)) {
        smk_visible = 0;
        return 1;
    }
    if (ly < titlebar_h && mx >= (int)(smk_x + smk_w - 60) && mx < (int)(smk_x + smk_w - 20)) {
        smoke_decay = 1.5f; smoke_depth = 0.8f;
        smoke_zoom = 2.0f; smoke_hue = 0.0f;
        smoke_saturation = 0.0f; smoke_value = 1.0f;
        return 1;
    }
    if (ly < titlebar_h) {
        smk_dragging = 1;
        smk_drag_ox = (float)mx - smk_x;
        smk_drag_oy = (float)my - smk_y;
        return 1;
    }
    float pct;
    int s = smk_hit_slider(mx, my, &pct);
    if (s >= 0) {
        smk_active_slider = s;
        smk_set_from_pct(s, pct);
        return 1;
    }
    return 1;
}

static void smk_mouse_move(int mx, int my) {
    if (smk_dragging) {
        smk_x = (float)mx - smk_drag_ox;
        smk_y = (float)my - smk_drag_oy;
        return;
    }
    if (smk_active_slider >= 0) {
        float sx0 = smk_x + 8 + 110;
        float sx1 = smk_x + smk_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        smk_set_from_pct(smk_active_slider, pct);
    }
}

static void smk_mouse_up(void) {
    smk_dragging = 0;
    smk_active_slider = -1;
}

/* ============ Rain Drops Settings Panel ============ */

static void rn_draw(int vp_w, int vp_h) {
    if (!rn_visible) return;
    if (!rn_scaled) { rn_w = 280*ui_scale; rn_h = 200*ui_scale; rn_x = 40*ui_scale; rn_y = 100*ui_scale; rn_scaled = 1; }
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = rn_x, y0 = rn_y;
    float pw = rn_w;
    int titlebar_h = ch + 8;

    rn_slider_ptr[0] = &rain_size;
    rn_slider_ptr[1] = &rain_speed;
    rn_slider_ptr[2] = &rain_freq;
    rn_slider_ptr[3] = &rain_warp;

    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + rn_h),
               t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)(y0 + rn_h - 1), (int)(x0 + pw), (int)(y0 + rn_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + 1), (int)(y0 + rn_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)(x0 + pw - 1), (int)y0, (int)(x0 + pw), (int)(y0 + rn_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);

    {
        float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
        float tbr = acr * 0.25f + t->bg[0] * 0.5f;
        float tbg = acg * 0.25f + t->bg[1] * 0.5f;
        float tbb = acb * 0.25f + t->bg[2] * 0.5f;
        int ty0 = (int)y0 + 2, ty1 = (int)y0 + titlebar_h;
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty1,
                   tbr, tbg, tbb, 0.95f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty1 - 1, (int)(x0 + pw) - 2, ty1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
    }
    push_text((int)(x0 + 6) + 1, (int)(y0 + 4) + 1, "Rain Drops",
              0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + 6), (int)(y0 + 4), "Rain Drops",
              t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 16), (int)(y0 + 4), "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 55), (int)(y0 + 4), "Reset",
              t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);

    float cy = y0 + titlebar_h + 12;
    float row_h = ch + 14;
    char vbuf[32];

    for (int i = 0; i < RN_SLIDER_COUNT; i++) {
        float val = *rn_slider_ptr[i];
        _snprintf(vbuf, sizeof(vbuf), "%.3f", val);
        clr_draw_slider(x0 + 8, cy, pw - 16, row_h, val,
                        rn_slider_min[i], rn_slider_max[i],
                        rn_labels[i], vbuf, vp_w, vp_h, t);
        cy += row_h;
    }

    rn_h = cy - y0 + 8;
}

static int rn_hit_slider(int mx, int my, float *out_pct) {
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    float row_h = ch + 14;
    float sx0 = rn_x + 8 + 110;
    float sx1 = rn_x + rn_w - 16 - 50 + 8;
    for (int i = 0; i < RN_SLIDER_COUNT; i++) {
        float sy = rn_y + titlebar_h + 12 + i * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    return -1;
}

static void rn_set_from_pct(int slider, float pct) {
    if (slider < 0 || slider >= RN_SLIDER_COUNT) return;
    float val = rn_slider_min[slider] + pct * (rn_slider_max[slider] - rn_slider_min[slider]);
    *rn_slider_ptr[slider] = val;
}

static int rn_mouse_down(int mx, int my) {
    if (!rn_visible) return 0;
    if (mx < (int)rn_x || mx >= (int)(rn_x + rn_w) ||
        my < (int)rn_y || my >= (int)(rn_y + rn_h)) return 0;
    int ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int ly = my - (int)rn_y;
    if (ly < titlebar_h && mx >= (int)(rn_x + rn_w - 20)) {
        rn_visible = 0;
        return 1;
    }
    if (ly < titlebar_h && mx >= (int)(rn_x + rn_w - 60) && mx < (int)(rn_x + rn_w - 20)) {
        rain_size = 0.06f; rain_speed = 1.0f;
        rain_freq = 3.0f; rain_warp = 0.02f;
        return 1;
    }
    if (ly < titlebar_h) {
        rn_dragging = 1;
        rn_drag_ox = (float)mx - rn_x;
        rn_drag_oy = (float)my - rn_y;
        sp_focus = 6;
        return 1;
    }
    float pct;
    int s = rn_hit_slider(mx, my, &pct);
    if (s >= 0) {
        rn_active_slider = s;
        rn_set_from_pct(s, pct);
        sp_focus = 6;
        return 1;
    }
    return 1;
}

static void rn_mouse_move(int mx, int my) {
    if (rn_dragging) {
        rn_x = (float)mx - rn_drag_ox;
        rn_y = (float)my - rn_drag_oy;
        return;
    }
    if (rn_active_slider >= 0) {
        float sx0 = rn_x + 8 + 110;
        float sx1 = rn_x + rn_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        rn_set_from_pct(rn_active_slider, pct);
    }
}

static void rn_mouse_up(void) {
    rn_dragging = 0;
    rn_active_slider = -1;
}

/* ============ Drop Shadow Settings Panel ============ */

static int shd_scaled = 0;
static void shd_draw(int vp_w, int vp_h) {
    if (!shd_visible) return;
    if (!shd_scaled) { shd_w = 280*ui_scale; shd_h = 200*ui_scale; shd_x = 60*ui_scale; shd_y = 120*ui_scale; shd_scaled = 1; }
    const ui_theme_t *t = &ui_themes[current_theme];
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    float x0 = shd_x, y0 = shd_y;
    float pw = shd_w;
    int titlebar_h = ch + 8;

    /* The 5th slider controls shadow brightness (combined R=G=B) */
    float shd_bright = (shadow_r + shadow_g + shadow_b) / 3.0f;
    shd_slider_ptr[0] = &shadow_opacity;
    shd_slider_ptr[1] = &shadow_angle;
    shd_slider_ptr[2] = &shadow_distance;
    shd_slider_ptr[3] = &shadow_blur;
    shd_slider_ptr[4] = &shd_bright; /* temporary, applied in set */

    /* Panel background */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + shd_h),
               t->bg[0], t->bg[1], t->bg[2], 0.92f, vp_w, vp_h);
    /* Border */
    push_solid((int)x0, (int)y0, (int)(x0 + pw), (int)(y0 + 1),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)(y0 + shd_h - 1), (int)(x0 + pw), (int)(y0 + shd_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)x0, (int)y0, (int)(x0 + 1), (int)(y0 + shd_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    push_solid((int)(x0 + pw - 1), (int)y0, (int)(x0 + pw), (int)(y0 + shd_h),
               t->accent[0], t->accent[1], t->accent[2], 0.6f, vp_w, vp_h);
    /* Titlebar */
    {
        float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];
        float tbr = acr * 0.25f + t->bg[0] * 0.5f;
        float tbg = acg * 0.25f + t->bg[1] * 0.5f;
        float tbb = acb * 0.25f + t->bg[2] * 0.5f;
        int ty0 = (int)y0 + 2, ty1 = (int)y0 + titlebar_h;
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty1,
                   tbr, tbg, tbb, 0.95f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty0, (int)(x0 + pw) - 2, ty0 + titlebar_h / 2,
                   1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
        push_solid((int)x0 + 2, ty1 - 1, (int)(x0 + pw) - 2, ty1,
                   acr, acg, acb, 0.5f, vp_w, vp_h);
    }
    push_text((int)(x0 + 6) + 1, (int)(y0 + 4) + 1, "Drop Shadow",
              0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + 6), (int)(y0 + 4), "Drop Shadow",
              t->text[0], t->text[1], t->text[2], vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 16), (int)(y0 + 4), "X",
              t->text[0] * 0.6f, t->text[1] * 0.6f, t->text[2] * 0.6f, vp_w, vp_h, cw, ch);
    push_text((int)(x0 + pw - 55), (int)(y0 + 4), "Reset",
              t->text[0] * 0.5f, t->text[1] * 0.5f, t->text[2] * 0.5f, vp_w, vp_h, cw, ch);

    float cy = y0 + titlebar_h + 12;
    float row_h = ch + 14;

    /* Color preview box */
    {
        push_text((int)(x0 + 8), (int)cy + 2, "Color",
                  t->text[0] * 0.8f, t->text[1] * 0.8f, t->text[2] * 0.8f, vp_w, vp_h, cw, ch);
        int bx0 = (int)(x0 + 60), by0 = (int)cy;
        int bx1 = bx0 + 24, by1 = by0 + (int)row_h - 4;
        push_solid(bx0, by0, bx1, by1, shadow_r, shadow_g, shadow_b, 1.0f, vp_w, vp_h);
        /* Border around color box */
        push_solid(bx0 - 1, by0 - 1, bx1 + 1, by0, 0.5f, 0.5f, 0.5f, 0.8f, vp_w, vp_h);
        push_solid(bx0 - 1, by1, bx1 + 1, by1 + 1, 0.5f, 0.5f, 0.5f, 0.8f, vp_w, vp_h);
        push_solid(bx0 - 1, by0, bx0, by1, 0.5f, 0.5f, 0.5f, 0.8f, vp_w, vp_h);
        push_solid(bx1, by0, bx1 + 1, by1, 0.5f, 0.5f, 0.5f, 0.8f, vp_w, vp_h);
        cy += row_h;
    }

    /* Sliders */
    char vbuf[32];
    for (int i = 0; i < SHD_SLIDER_COUNT; i++) {
        float val;
        if (i == 4)
            val = (shadow_r + shadow_g + shadow_b) / 3.0f;
        else
            val = *shd_slider_ptr[i];

        if (i == 1)
            _snprintf(vbuf, sizeof(vbuf), "%.0f\xF8", val);
        else if (i == 2)
            _snprintf(vbuf, sizeof(vbuf), "%.1fpx", val);
        else
            _snprintf(vbuf, sizeof(vbuf), "%.2f", val);

        clr_draw_slider(x0 + 8, cy, pw - 16, row_h, val,
                        shd_slider_min[i], shd_slider_max[i],
                        shd_labels[i], vbuf, vp_w, vp_h, t);
        cy += row_h;
    }

    shd_h = cy - y0 + 8;
}

static int shd_hit_slider(int mx, int my, float *out_pct) {
    int cw = (int)(VKM_CHAR_W * ui_scale), ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    float row_h = ch + 14;
    float color_row_h = row_h; /* skip color preview row */
    float sx0 = shd_x + 8 + 110;
    float sx1 = shd_x + shd_w - 16 - 50 + 8;
    for (int i = 0; i < SHD_SLIDER_COUNT; i++) {
        float sy = shd_y + titlebar_h + 12 + color_row_h + i * row_h;
        if (my >= (int)sy && my < (int)(sy + row_h) &&
            mx >= (int)(sx0 - 4) && mx <= (int)(sx1 + 4)) {
            float pct = ((float)mx - sx0) / (sx1 - sx0);
            if (pct < 0) pct = 0; if (pct > 1) pct = 1;
            *out_pct = pct;
            return i;
        }
    }
    return -1;
}

static void shd_set_from_pct(int slider, float pct) {
    if (slider < 0 || slider >= SHD_SLIDER_COUNT) return;
    float val = shd_slider_min[slider] + pct * (shd_slider_max[slider] - shd_slider_min[slider]);
    if (slider == 4) {
        /* Brightness: set R=G=B uniformly */
        shadow_r = shadow_g = shadow_b = val;
    } else {
        *shd_slider_ptr[slider] = val;
    }
}

static int shd_mouse_down(int mx, int my) {
    if (!shd_visible) return 0;
    if (mx < (int)shd_x || mx >= (int)(shd_x + shd_w) ||
        my < (int)shd_y || my >= (int)(shd_y + shd_h)) return 0;
    int ch = (int)(VKM_CHAR_H * ui_scale);
    int titlebar_h = ch + 8;
    int ly = my - (int)shd_y;
    /* Close button */
    if (ly < titlebar_h && mx >= (int)(shd_x + shd_w - 20)) {
        shd_visible = 0;
        return 1;
    }
    /* Reset button */
    if (ly < titlebar_h && mx >= (int)(shd_x + shd_w - 60) && mx < (int)(shd_x + shd_w - 20)) {
        shadow_opacity = 0.6f; shadow_angle = 135.0f;
        shadow_distance = 2.0f; shadow_blur = 1.0f;
        shadow_r = shadow_g = shadow_b = 0.0f;
        return 1;
    }
    /* Titlebar drag */
    if (ly < titlebar_h) {
        shd_dragging = 1;
        shd_drag_ox = (float)mx - shd_x;
        shd_drag_oy = (float)my - shd_y;
        return 1;
    }
    float pct;
    int s = shd_hit_slider(mx, my, &pct);
    if (s >= 0) {
        shd_active_slider = s;
        shd_set_from_pct(s, pct);
        return 1;
    }
    return 1;
}

static void shd_mouse_move(int mx, int my) {
    if (shd_dragging) {
        shd_x = (float)mx - shd_drag_ox;
        shd_y = (float)my - shd_drag_oy;
        return;
    }
    if (shd_active_slider >= 0) {
        float sx0 = shd_x + 8 + 110;
        float sx1 = shd_x + shd_w - 16 - 50 + 8;
        float pct = ((float)mx - sx0) / (sx1 - sx0);
        if (pct < 0) pct = 0; if (pct > 1) pct = 1;
        shd_set_from_pct(shd_active_slider, pct);
    }
}

static void shd_mouse_up(void) {
    shd_dragging = 0;
    shd_active_slider = -1;
}

/* ---- Background MMUDPy Exports ---- */

__declspec(dllexport) void vkt_bg_set_mode(int mode)
{
    /* No auto-open of settings dialog — users open it explicitly via the
     * "Background Settings..." menu item or bg.settings.open() command. */
    bg_mode = mode;
    if (api) api->log("[vk_terminal] bg_mode=%d\n", bg_mode);
}

__declspec(dllexport) int vkt_bg_get_mode(void) { return bg_mode; }

__declspec(dllexport) void vkt_bg_plasma_set_speed(float v) { bg_plasma_speed = v; }
__declspec(dllexport) void vkt_bg_plasma_set_turbulence(float v) { bg_plasma_turbulence = v; }
__declspec(dllexport) void vkt_bg_plasma_set_complexity(float v) { bg_plasma_complexity = v; }
__declspec(dllexport) void vkt_bg_plasma_set_hue(float v) { bg_plasma_hue = v; }
__declspec(dllexport) void vkt_bg_plasma_set_saturation(float v) { bg_plasma_saturation = v; }
__declspec(dllexport) void vkt_bg_plasma_set_brightness(float v) { bg_plasma_brightness = v; }
__declspec(dllexport) void vkt_bg_plasma_set_alpha(float v) { bg_plasma_alpha = v; }

__declspec(dllexport) float vkt_bg_plasma_get_speed(void) { return bg_plasma_speed; }
__declspec(dllexport) float vkt_bg_plasma_get_turbulence(void) { return bg_plasma_turbulence; }
__declspec(dllexport) float vkt_bg_plasma_get_complexity(void) { return bg_plasma_complexity; }
__declspec(dllexport) float vkt_bg_plasma_get_hue(void) { return bg_plasma_hue; }
__declspec(dllexport) float vkt_bg_plasma_get_saturation(void) { return bg_plasma_saturation; }
__declspec(dllexport) float vkt_bg_plasma_get_brightness(void) { return bg_plasma_brightness; }
__declspec(dllexport) float vkt_bg_plasma_get_alpha(void) { return bg_plasma_alpha; }

/* Sobel / Emboss / Edge / Sharpen */
__declspec(dllexport) void  vkt_bg_plasma_set_sobel(float v)       { bg_plasma_sobel = v; }
__declspec(dllexport) float vkt_bg_plasma_get_sobel(void)          { return bg_plasma_sobel; }
__declspec(dllexport) void  vkt_bg_plasma_set_sobel_angle(float v) { bg_plasma_sobel_angle = v; }
__declspec(dllexport) float vkt_bg_plasma_get_sobel_angle(void)    { return bg_plasma_sobel_angle; }
__declspec(dllexport) void  vkt_bg_plasma_set_edge_glow(float v)   { bg_plasma_edge_glow = v; }
__declspec(dllexport) float vkt_bg_plasma_get_edge_glow(void)      { return bg_plasma_edge_glow; }
__declspec(dllexport) void  vkt_bg_plasma_set_sharpen(float v)     { bg_plasma_sharpen = v; }
__declspec(dllexport) float vkt_bg_plasma_get_sharpen(void)        { return bg_plasma_sharpen; }

/* Material */
__declspec(dllexport) void  vkt_bg_plasma_set_material(int t)      { bg_plasma_material = t; }
__declspec(dllexport) int   vkt_bg_plasma_get_material(void)       { return bg_plasma_material; }
__declspec(dllexport) void  vkt_bg_plasma_set_material_str(float v){ bg_plasma_mat_str = v; }
__declspec(dllexport) float vkt_bg_plasma_get_material_str(void)   { return bg_plasma_mat_str; }

/* Tonemap */
__declspec(dllexport) void  vkt_bg_plasma_set_tonemap(int m)       { bg_tonemap_mode = m; }
__declspec(dllexport) int   vkt_bg_plasma_get_tonemap(void)        { return bg_tonemap_mode; }
__declspec(dllexport) void  vkt_bg_plasma_set_tonemap_exp(float v) { bg_tonemap_exposure = v; }
__declspec(dllexport) float vkt_bg_plasma_get_tonemap_exp(void)    { return bg_tonemap_exposure; }

/* Beat response toggles */
__declspec(dllexport) void vkt_bg_plasma_set_beat_bass(int v)      { bg_plasma_beat_bass = v; }
__declspec(dllexport) int  vkt_bg_plasma_get_beat_bass(void)       { return bg_plasma_beat_bass; }
__declspec(dllexport) void vkt_bg_plasma_set_beat_mid(int v)       { bg_plasma_beat_mid = v; }
__declspec(dllexport) int  vkt_bg_plasma_get_beat_mid(void)        { return bg_plasma_beat_mid; }
__declspec(dllexport) void vkt_bg_plasma_set_beat_treble(int v)    { bg_plasma_beat_treble = v; }
__declspec(dllexport) int  vkt_bg_plasma_get_beat_treble(void)     { return bg_plasma_beat_treble; }
__declspec(dllexport) void vkt_bg_plasma_set_beat_rms(int v)       { bg_plasma_beat_rms = v; }
__declspec(dllexport) int  vkt_bg_plasma_get_beat_rms(void)        { return bg_plasma_beat_rms; }

__declspec(dllexport) void vkt_bg_plasma_set_all(float speed, float turbulence, float complexity,
                                                  float hue, float saturation, float brightness, float alpha)
{
    bg_plasma_speed = speed; bg_plasma_turbulence = turbulence;
    bg_plasma_complexity = complexity; bg_plasma_hue = hue;
    bg_plasma_saturation = saturation; bg_plasma_brightness = brightness;
    bg_plasma_alpha = alpha;
}

/* Alien Wall setters */
__declspec(dllexport) void vkt_bg_alien_set_speed(float v)      { bg_alien_speed = v; }
__declspec(dllexport) void vkt_bg_alien_set_cell_scale(float v) { bg_alien_cell_scale = v; }
__declspec(dllexport) void vkt_bg_alien_set_warp(float v)       { bg_alien_warp = v; }
__declspec(dllexport) void vkt_bg_alien_set_hue(float v)        { bg_alien_hue = v; }
__declspec(dllexport) void vkt_bg_alien_set_saturation(float v) { bg_alien_saturation = v; }
__declspec(dllexport) void vkt_bg_alien_set_brightness(float v) { bg_alien_brightness = v; }
__declspec(dllexport) void vkt_bg_alien_set_alpha(float v)      { bg_alien_alpha = v; }
__declspec(dllexport) void vkt_bg_alien_set_vein(float v)       { bg_alien_vein = v; }
__declspec(dllexport) void vkt_bg_alien_set_pulse(float v)      { bg_alien_pulse_speed = v; }
__declspec(dllexport) void vkt_bg_alien_set_glow(float v)       { bg_alien_glow = v; }
__declspec(dllexport) void vkt_bg_alien_set_depth(float v)      { bg_alien_depth_layers = v; }
__declspec(dllexport) void vkt_bg_alien_set_roughness(float v)  { bg_alien_roughness = v; }
__declspec(dllexport) void vkt_bg_alien_set_membrane(float v)   { bg_alien_membrane = v; }
__declspec(dllexport) void vkt_bg_alien_set_wet(float v)        { bg_alien_wet = v; }
__declspec(dllexport) void vkt_bg_alien_set_fluid(float v)      { bg_alien_fluid_speed = v; }
__declspec(dllexport) void vkt_bg_alien_set_specular(float v)   { bg_alien_specular = v; }
__declspec(dllexport) void vkt_bg_alien_set_cell_move(float v)  { bg_alien_cell_move = v; }
__declspec(dllexport) void vkt_bg_alien_set_bulge(float v)      { bg_alien_bulge = v; }
__declspec(dllexport) void vkt_bg_alien_set_cell_style(int v)   { bg_alien_cell_style = v % BG_ALIEN_STYLE_COUNT; }

__declspec(dllexport) void vkt_bg_show_settings(void) { bgp_visible = 1; }
__declspec(dllexport) void vkt_bg_hide_settings(void) { bgp_visible = 0; }

__declspec(dllexport) void vkt_bg_help(void)
{
    if (vkw_console_idx < 0) return;
    vkw_print(vkw_console_idx, "\x1b[1;37m---- Backgrounds ----\x1b[0m");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m Fractal Plasma");
    vkw_print(vkw_console_idx, "");
    vkw_print(vkw_console_idx, "\x1b[1;37mCommands:\x1b[0m");
    vkw_print(vkw_console_idx, "  bg.set_mode(0|1)     - Off / Plasma");
    vkw_print(vkw_console_idx, "  bg.get_mode()        - Get current mode");
    vkw_print(vkw_console_idx, "  bg.plasma.speed(f)   - Speed (0.1-5.0)");
    vkw_print(vkw_console_idx, "  bg.plasma.turbulence(f)");
    vkw_print(vkw_console_idx, "  bg.plasma.complexity(f) - Octaves (1-8)");
    vkw_print(vkw_console_idx, "  bg.plasma.hue(f)     - Hue (0-360)");
    vkw_print(vkw_console_idx, "  bg.plasma.saturation(f)");
    vkw_print(vkw_console_idx, "  bg.plasma.brightness(f)");
    vkw_print(vkw_console_idx, "  bg.plasma.alpha(f)   - Opacity (0-1)");
    vkw_print(vkw_console_idx, "  bg.plasma.set_all(speed,turb,cmplx,hue,sat,brt,alpha)");
    vkw_print(vkw_console_idx, "  bg.plasma.sobel(f)        - Emboss (0-2)");
    vkw_print(vkw_console_idx, "  bg.plasma.sobel_angle(f)  - Light angle (0-360)");
    vkw_print(vkw_console_idx, "  bg.plasma.edge_glow(f)    - Edge glow (0-2)");
    vkw_print(vkw_console_idx, "  bg.plasma.sharpen(f)      - Sharpen (0-2)");
    vkw_print(vkw_console_idx, "  bg.plasma.material(0-3)   - 0=glossy,1=wet,2=metal,3=matte");
    vkw_print(vkw_console_idx, "  bg.plasma.material_str(f) - Material strength (0-2)");
    vkw_print(vkw_console_idx, "  bg.plasma.tonemap(0-4)    - 0=none,1=ACES,2=Reinhard,3=Hable,4=AgX");
    vkw_print(vkw_console_idx, "  bg.plasma.tonemap_exp(f)  - Exposure (0.2-4.0)");
    vkw_print(vkw_console_idx, "  bg.plasma.beat_bass(0|1)  - Bass beat response");
    vkw_print(vkw_console_idx, "  bg.plasma.beat_mid(0|1)   - Mid beat response");
    vkw_print(vkw_console_idx, "  bg.plasma.beat_treble(0|1)- Treble beat response");
    vkw_print(vkw_console_idx, "  bg.plasma.beat_rms(0|1)   - RMS beat response");
    vkw_print(vkw_console_idx, "  All setters have get_ variants (e.g. bg.plasma.get_sobel())");
    vkw_print(vkw_console_idx, "  bg.show_settings()   - Show settings panel");
    vkw_print(vkw_console_idx, "  bg.hide_settings()   - Hide settings panel");
}

__declspec(dllexport) void vkt_vk_plugins_show_experimental(int val)
{
    vk_experimental = val ? 1 : 0;
    if (vkw_console_idx >= 0) {
        vkw_print(vkw_console_idx, vk_experimental
            ? "[vk_plugins] Experimental plugins: \x1b[1;32mENABLED\x1b[0m"
            : "[vk_plugins] Experimental plugins: \x1b[1;31mDISABLED\x1b[0m");
    }
    if (api) api->log("[vk_terminal] vk_plugins.show_experimental = %s\n",
                      vk_experimental ? "True" : "False");
}

__declspec(dllexport) void vkt_vk_plugins_list(void)
{
    if (vkw_console_idx < 0) return;
    vkw_print(vkw_console_idx, "\x1b[1;37m---- VK Plugins ----\x1b[0m");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m Liquid Letters");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m Diagonal Waves");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m FBM Currents");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m Sobel/Sharp");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m CRT Scanlines");
    vkw_print(vkw_console_idx, fx_smoky_mode
        ? " \x1b[1;32m*\x1b[0m Smoky Letters  [\x1b[1;32mACTIVE\x1b[0m]"
        : " \x1b[1;32m*\x1b[0m Smoky Letters");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m Round Timer");
    vkw_print(vkw_console_idx, " \x1b[1;32m*\x1b[0m Exp Bar");
    vkw_print(vkw_console_idx, "\x1b[1;37m---- Backgrounds ----\x1b[0m");
    vkw_print(vkw_console_idx, bg_mode == BG_PLASMA
        ? " \x1b[1;32m*\x1b[0m Fractal Plasma  [\x1b[1;32mACTIVE\x1b[0m]"
        : " \x1b[1;32m*\x1b[0m Fractal Plasma");
    vkw_print(vkw_console_idx, bg_mode == BG_STARFIELD
        ? " \x1b[1;32m*\x1b[0m Parallax Starfield  [\x1b[1;32mACTIVE\x1b[0m]"
        : " \x1b[1;32m*\x1b[0m Parallax Starfield");
    vkw_print(vkw_console_idx, bg_mode == BG_ALIENWALL
        ? " \x1b[1;32m*\x1b[0m Alien Wall  [\x1b[1;32mACTIVE\x1b[0m]"
        : " \x1b[1;32m*\x1b[0m Alien Wall");
    vkw_print(vkw_console_idx, "\x1b[1;37m---- Experimental ----\x1b[0m");
    vkw_print(vkw_console_idx, vk_experimental
        ? " \x1b[1;33m!\x1b[0m Wobbly Widgets  [\x1b[1;33mEXPERIMENTAL\x1b[0m]"
        : " \x1b[1;30m-\x1b[0m Wobbly Widgets  [\x1b[1;30mHIDDEN\x1b[0m]");
    vkw_print(vkw_console_idx, "");
    vkw_print(vkw_console_idx, vk_experimental
        ? "  show_experimental = \x1b[1;32mTrue\x1b[0m"
        : "  show_experimental = \x1b[1;31mFalse\x1b[0m");
}

/* ---- Smoky Letters MMUDPy Exports ---- */
__declspec(dllexport) void vkt_smoke_set_mode(int mode) {
    fx_smoky_mode = mode ? 1 : 0;
    if (fx_smoky_mode && !smk_visible) smk_visible = 1;
    if (!fx_smoky_mode) smk_visible = 0;
    if (api) api->log("[vk_terminal] Smoky Letters: %s\n", fx_smoky_mode ? "ON" : "OFF");
}
__declspec(dllexport) int vkt_smoke_get_mode(void) { return fx_smoky_mode; }
__declspec(dllexport) void vkt_smoke_set_decay(float v) { smoke_decay = v; }
__declspec(dllexport) void vkt_smoke_set_depth(float v) { smoke_depth = v; }
__declspec(dllexport) void vkt_smoke_set_zoom(float v) { smoke_zoom = v; }
__declspec(dllexport) void vkt_smoke_set_hue(float v) { smoke_hue = v; }
__declspec(dllexport) void vkt_smoke_set_saturation(float v) { smoke_saturation = v; }
__declspec(dllexport) void vkt_smoke_set_value(float v) { smoke_value = v; }
__declspec(dllexport) float vkt_smoke_get_decay(void) { return smoke_decay; }
__declspec(dllexport) float vkt_smoke_get_depth(void) { return smoke_depth; }
__declspec(dllexport) float vkt_smoke_get_zoom(void) { return smoke_zoom; }
__declspec(dllexport) float vkt_smoke_get_hue(void) { return smoke_hue; }
__declspec(dllexport) float vkt_smoke_get_saturation(void) { return smoke_saturation; }
__declspec(dllexport) float vkt_smoke_get_value(void) { return smoke_value; }
__declspec(dllexport) void vkt_smoke_set_all(float decay, float depth, float zoom, float hue, float sat, float val) {
    smoke_decay = decay; smoke_depth = depth; smoke_zoom = zoom;
    smoke_hue = hue; smoke_saturation = sat; smoke_value = val;
}
__declspec(dllexport) void vkt_smoke_show_settings(void) { smk_visible = 1; }
__declspec(dllexport) void vkt_smoke_hide_settings(void) { smk_visible = 0; }
__declspec(dllexport) void vkt_smoke_help(void) {
    if (vkw_console_idx < 0) return;
    vkw_print(vkw_console_idx, "\x1b[1;37m---- Smoky Letters ----\x1b[0m");
    vkw_print(vkw_console_idx, "Smoke emitting from letter edges");
    vkw_print(vkw_console_idx, "");
    vkw_print(vkw_console_idx, "\x1b[1;37mCommands:\x1b[0m");
    vkw_print(vkw_console_idx, "  smoke.set_mode(0|1)  - Off / On");
    vkw_print(vkw_console_idx, "  smoke.get_mode()     - Get current mode");
    vkw_print(vkw_console_idx, "  smoke.decay(f)       - Decay speed (0.5-5.0)");
    vkw_print(vkw_console_idx, "  smoke.depth(f)       - Strength (0.0-2.0)");
    vkw_print(vkw_console_idx, "  smoke.zoom(f)        - Noise scale (0.5-4.0)");
    vkw_print(vkw_console_idx, "  smoke.hue(f)         - Hue (0-360)");
    vkw_print(vkw_console_idx, "  smoke.saturation(f)  - Saturation (0-2)");
    vkw_print(vkw_console_idx, "  smoke.value(f)       - Brightness (0-2)");
    vkw_print(vkw_console_idx, "  smoke.set_all(decay,depth,zoom,hue,sat,val)");
    vkw_print(vkw_console_idx, "  smoke.show_settings() - Show settings panel");
    vkw_print(vkw_console_idx, "  smoke.hide_settings() - Hide settings panel");
}

/* ---- Round Recap MMUDPy exports ---- */
__declspec(dllexport) const char *vkt_recap_get(void) { return pst_last_recap; }
__declspec(dllexport) int vkt_recap_seq(void) { return pst_recap_seq; }
__declspec(dllexport) int vkt_recap_dmg(void) { return pst_recap_dmg; }
__declspec(dllexport) int vkt_recap_hits(void) { return pst_recap_hits; }
__declspec(dllexport) int vkt_recap_crits(void) { return pst_recap_crits; }
__declspec(dllexport) int vkt_recap_extra(void) { return pst_recap_extra; }
__declspec(dllexport) int vkt_recap_spell(void) { return pst_recap_spell; }
__declspec(dllexport) int vkt_recap_bs(void) { return pst_recap_bs; }
__declspec(dllexport) int vkt_recap_miss(void) { return pst_recap_miss; }
__declspec(dllexport) int vkt_recap_glance(void) { return pst_recap_glance; }
__declspec(dllexport) int vkt_recap_dodge(void) { return pst_recap_dodge; }
__declspec(dllexport) void vkt_recap_set_show(int v) {
    pst_round_recap = v ? 1 : 0;
    if (api) api->log("[vk_terminal] Round Totals: %s\n", pst_round_recap ? "ON" : "OFF");
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
    { "vk_plugins.show_experimental", "vkt_vk_plugins_show_experimental", "i", "v", "Enable/disable experimental plugins (1=True, 0=False)" },
    { "vk_plugins.list", "vkt_vk_plugins_list", "", "v", "List all VK plugins and their status" },
    /* Backgrounds */
    { "bg.set_mode",          "vkt_bg_set_mode",          "i",       "v", "Set background mode (0=off, 1=plasma, 2=starfield, 3=alienwall)" },
    { "bg.get_mode",          "vkt_bg_get_mode",          "",        "i", "Get current background mode" },
    { "bg.plasma.speed",      "vkt_bg_plasma_set_speed",  "f",       "v", "Set plasma speed (0.1-5.0)" },
    { "bg.plasma.turbulence", "vkt_bg_plasma_set_turbulence","f",    "v", "Set plasma turbulence (0.0-2.0)" },
    { "bg.plasma.complexity", "vkt_bg_plasma_set_complexity","f",    "v", "Set plasma complexity/octaves (1-8)" },
    { "bg.plasma.hue",        "vkt_bg_plasma_set_hue",    "f",       "v", "Set plasma hue shift (0-360)" },
    { "bg.plasma.saturation", "vkt_bg_plasma_set_saturation","f",    "v", "Set plasma saturation (0-1)" },
    { "bg.plasma.brightness", "vkt_bg_plasma_set_brightness","f",    "v", "Set plasma brightness (0-2)" },
    { "bg.plasma.alpha",      "vkt_bg_plasma_set_alpha",  "f",       "v", "Set plasma opacity (0-1)" },
    { "bg.plasma.set_all",    "vkt_bg_plasma_set_all",    "fffffff", "v", "Set all plasma params (speed,turb,cmplx,hue,sat,brt,alpha)" },
    /* Plasma sobel/emboss/edge/sharpen */
    { "bg.plasma.sobel",          "vkt_bg_plasma_set_sobel",       "f", "v", "Set emboss strength (0-2)" },
    { "bg.plasma.get_sobel",      "vkt_bg_plasma_get_sobel",       "",  "f", "Get emboss strength" },
    { "bg.plasma.sobel_angle",    "vkt_bg_plasma_set_sobel_angle", "f", "v", "Set emboss light angle (0-360)" },
    { "bg.plasma.get_sobel_angle","vkt_bg_plasma_get_sobel_angle", "",  "f", "Get emboss light angle" },
    { "bg.plasma.edge_glow",      "vkt_bg_plasma_set_edge_glow",  "f", "v", "Set edge glow intensity (0-2)" },
    { "bg.plasma.get_edge_glow",  "vkt_bg_plasma_get_edge_glow",  "",  "f", "Get edge glow intensity" },
    { "bg.plasma.sharpen",        "vkt_bg_plasma_set_sharpen",     "f", "v", "Set sharpen strength (0-2)" },
    { "bg.plasma.get_sharpen",    "vkt_bg_plasma_get_sharpen",     "",  "f", "Get sharpen strength" },
    /* Plasma material */
    { "bg.plasma.material",       "vkt_bg_plasma_set_material",    "i", "v", "Set material (0=glossy,1=wet,2=metallic,3=matte)" },
    { "bg.plasma.get_material",   "vkt_bg_plasma_get_material",    "",  "i", "Get material type" },
    { "bg.plasma.material_str",   "vkt_bg_plasma_set_material_str","f", "v", "Set material strength (0-2)" },
    { "bg.plasma.get_material_str","vkt_bg_plasma_get_material_str","", "f", "Get material strength" },
    /* Plasma tonemap */
    { "bg.plasma.tonemap",        "vkt_bg_plasma_set_tonemap",     "i", "v", "Set tonemap (0=none,1=ACES,2=Reinhard,3=Hable,4=AgX)" },
    { "bg.plasma.get_tonemap",    "vkt_bg_plasma_get_tonemap",     "",  "i", "Get tonemap mode" },
    { "bg.plasma.tonemap_exp",    "vkt_bg_plasma_set_tonemap_exp", "f", "v", "Set tonemap exposure (0.2-4.0)" },
    { "bg.plasma.get_tonemap_exp","vkt_bg_plasma_get_tonemap_exp", "",  "f", "Get tonemap exposure" },
    /* Plasma beat response */
    { "bg.plasma.beat_bass",      "vkt_bg_plasma_set_beat_bass",   "i", "v", "Set bass beat response (0/1)" },
    { "bg.plasma.get_beat_bass",  "vkt_bg_plasma_get_beat_bass",   "",  "i", "Get bass beat response" },
    { "bg.plasma.beat_mid",       "vkt_bg_plasma_set_beat_mid",    "i", "v", "Set mid beat response (0/1)" },
    { "bg.plasma.get_beat_mid",   "vkt_bg_plasma_get_beat_mid",    "",  "i", "Get mid beat response" },
    { "bg.plasma.beat_treble",    "vkt_bg_plasma_set_beat_treble", "i", "v", "Set treble beat response (0/1)" },
    { "bg.plasma.get_beat_treble","vkt_bg_plasma_get_beat_treble", "",  "i", "Get treble beat response" },
    { "bg.plasma.beat_rms",       "vkt_bg_plasma_set_beat_rms",    "i", "v", "Set RMS beat response (0/1)" },
    { "bg.plasma.get_beat_rms",   "vkt_bg_plasma_get_beat_rms",    "",  "i", "Get RMS beat response" },
    /* Plasma getters for base params */
    { "bg.plasma.get_speed",      "vkt_bg_plasma_get_speed",       "",  "f", "Get plasma speed" },
    { "bg.plasma.get_turbulence", "vkt_bg_plasma_get_turbulence",  "",  "f", "Get plasma turbulence" },
    { "bg.plasma.get_complexity", "vkt_bg_plasma_get_complexity",  "",  "f", "Get plasma complexity" },
    { "bg.plasma.get_hue",        "vkt_bg_plasma_get_hue",        "",  "f", "Get plasma hue shift" },
    { "bg.plasma.get_saturation", "vkt_bg_plasma_get_saturation",  "",  "f", "Get plasma saturation" },
    { "bg.plasma.get_brightness", "vkt_bg_plasma_get_brightness",  "",  "f", "Get plasma brightness" },
    { "bg.plasma.get_alpha",      "vkt_bg_plasma_get_alpha",       "",  "f", "Get plasma opacity" },
    /* Alien Wall */
    { "bg.alien.speed",       "vkt_bg_alien_set_speed",       "f", "v", "Set alien wall speed (0.1-5.0)" },
    { "bg.alien.cell_scale",  "vkt_bg_alien_set_cell_scale",  "f", "v", "Set cell scale (1-8)" },
    { "bg.alien.warp",        "vkt_bg_alien_set_warp",        "f", "v", "Set warp strength (0-4)" },
    { "bg.alien.hue",         "vkt_bg_alien_set_hue",         "f", "v", "Set hue shift (0-360)" },
    { "bg.alien.saturation",  "vkt_bg_alien_set_saturation",  "f", "v", "Set saturation (0-2)" },
    { "bg.alien.brightness",  "vkt_bg_alien_set_brightness",  "f", "v", "Set brightness (0-2)" },
    { "bg.alien.alpha",       "vkt_bg_alien_set_alpha",       "f", "v", "Set opacity (0-1)" },
    { "bg.alien.vein",        "vkt_bg_alien_set_vein",        "f", "v", "Set vein strength (0-2)" },
    { "bg.alien.pulse",       "vkt_bg_alien_set_pulse",       "f", "v", "Set pulse speed (0-4)" },
    { "bg.alien.glow",        "vkt_bg_alien_set_glow",        "f", "v", "Set glow intensity (0-2)" },
    { "bg.alien.depth",       "vkt_bg_alien_set_depth",       "f", "v", "Set depth layers (1-4)" },
    { "bg.alien.roughness",   "vkt_bg_alien_set_roughness",   "f", "v", "Set roughness (0-1, 0=wet)" },
    { "bg.alien.membrane",    "vkt_bg_alien_set_membrane",    "f", "v", "Set membrane mix (0-1)" },
    { "bg.alien.wet",         "vkt_bg_alien_set_wet",         "f", "v", "Set wet/glossy intensity (0-2)" },
    { "bg.alien.fluid",       "vkt_bg_alien_set_fluid",       "f", "v", "Set fluid flow speed (0-2)" },
    { "bg.alien.specular",    "vkt_bg_alien_set_specular",    "f", "v", "Set specular tightness (0-1)" },
    { "bg.alien.cell_move",   "vkt_bg_alien_set_cell_move",   "f", "v", "Set cell crawl speed (0-2)" },
    { "bg.alien.bulge",       "vkt_bg_alien_set_bulge",       "f", "v", "Set per-cell bulge variance (0-2)" },
    { "bg.alien.cell_style",  "vkt_bg_alien_set_cell_style",  "i", "v", "Set cell geometry (0=Voronoi 1=Hex 2=Hive 3=Geodesic 4=Mixed)" },
    { "bg.show_settings",     "vkt_bg_show_settings",     "",        "v", "Show background settings panel" },
    { "bg.hide_settings",     "vkt_bg_hide_settings",     "",        "v", "Hide background settings panel" },
    { "bg.help",              "vkt_bg_help",              "",        "v", "Show background commands help" },
    /* Smoky Letters */
    { "smoke.set_mode",       "vkt_smoke_set_mode",       "i",      "v", "Set smoky letters mode (0=off, 1=on)" },
    { "smoke.get_mode",       "vkt_smoke_get_mode",       "",       "i", "Get smoky letters mode" },
    { "smoke.decay",          "vkt_smoke_set_decay",      "f",      "v", "Set smoke decay speed (0.5-5.0)" },
    { "smoke.depth",          "vkt_smoke_set_depth",      "f",      "v", "Set smoke strength (0.0-2.0)" },
    { "smoke.zoom",           "vkt_smoke_set_zoom",       "f",      "v", "Set smoke noise scale (0.5-4.0)" },
    { "smoke.hue",            "vkt_smoke_set_hue",        "f",      "v", "Set smoke hue (0-360)" },
    { "smoke.saturation",     "vkt_smoke_set_saturation", "f",      "v", "Set smoke saturation (0-2)" },
    { "smoke.value",          "vkt_smoke_set_value",      "f",      "v", "Set smoke brightness (0-2)" },
    { "smoke.set_all",        "vkt_smoke_set_all",        "ffffff", "v", "Set all smoke params (decay,depth,zoom,hue,sat,val)" },
    { "smoke.show_settings",  "vkt_smoke_show_settings",  "",       "v", "Show smoke settings panel" },
    { "smoke.hide_settings",  "vkt_smoke_hide_settings",  "",       "v", "Hide smoke settings panel" },
    { "smoke.help",           "vkt_smoke_help",           "",       "v", "Show smoke commands help" },
    /* Round Recap */
    { "recap.get",     "vkt_recap_get",      "", "s", "Get last round recap string" },
    { "recap.seq",     "vkt_recap_seq",      "", "i", "Get recap sequence number" },
    { "recap.dmg",     "vkt_recap_dmg",      "", "i", "Get last round damage" },
    { "recap.hits",    "vkt_recap_hits",     "", "i", "Get last round hit count" },
    { "recap.crits",   "vkt_recap_crits",    "", "i", "Get last round crit count" },
    { "recap.extra",   "vkt_recap_extra",    "", "i", "Get last round extra attack count" },
    { "recap.spell",   "vkt_recap_spell",    "", "i", "Get last round spell count" },
    { "recap.bs",      "vkt_recap_bs",       "", "i", "Get last round backstab count" },
    { "recap.miss",    "vkt_recap_miss",     "", "i", "Get last round miss count" },
    { "recap.glance",  "vkt_recap_glance",   "", "i", "Get last round glancing blow count" },
    { "recap.dodge",   "vkt_recap_dodge",    "", "i", "Get last round enemy dodge count" },
    { "recap.show",    "vkt_recap_set_show", "i","v", "Set round recap display (0=off, 1=on)" },
    /* MUDRadio */
    { "radio.show",        "mr_cmd_show",         "",  "v", "Show MUDRadio panel" },
    { "radio.hide",        "mr_cmd_hide",         "",  "v", "Hide MUDRadio panel" },
    { "radio.toggle",      "mr_cmd_toggle",       "",  "v", "Toggle MUDRadio panel" },
    { "radio.play",        "mr_cmd_play",         "",  "v", "Play/resume audio" },
    { "radio.pause",       "mr_cmd_pause",        "",  "v", "Pause audio" },
    { "radio.stop",        "mr_cmd_stop",         "",  "v", "Stop audio" },
    { "radio.next",        "mr_cmd_next",         "",  "v", "Next track" },
    { "radio.prev",        "mr_cmd_prev",         "",  "v", "Previous track" },
    { "radio.volume",      "mr_cmd_volume",       "f", "v", "Set volume (0.0-1.0)" },
    { "radio.get_volume",  "mr_cmd_get_volume",   "",  "f", "Get current volume" },
    { "radio.shuffle",     "mr_cmd_shuffle",      "i", "v", "Set shuffle (0=off, 1=on)" },
    { "radio.repeat",      "mr_cmd_repeat",       "i", "v", "Set repeat (0=off, 1=one, 2=all)" },
    { "radio.play_file",   "mr_cmd_play_file",    "s", "v", "Play audio file (path, Linux or Windows)" },
    { "radio.play_stream", "mr_cmd_play_stream",  "s", "v", "Play internet radio stream (URL)" },
    { "radio.search",      "mr_cmd_search",       "s", "v", "Search Radio Browser for stations" },
    { "radio.play_station","mr_cmd_play_station",  "i", "v", "Play station by index from search results" },
    { "radio.fav_toggle",  "mr_cmd_fav_toggle",   "i", "v", "Toggle favorite for station index" },
    { "radio.load_dir",    "mr_cmd_load_dir",     "s", "v", "Load audio files from directory into playlist" },
    { "radio.playlist_count","mr_cmd_playlist_count","","i", "Get number of tracks in playlist" },
    { "radio.now_playing", "mr_cmd_now_playing",   "",  "s", "Get now playing text" },
    { "radio.status",      "mr_cmd_status",        "",  "s", "Get transport status (playing/paused/stopped/etc)" },
    /* Radio beat/waveform analysis */
    { "radio.bpm",            "mr_cmd_get_bpm",            "", "f", "Get estimated BPM" },
    { "radio.bass_energy",    "mr_cmd_get_bass_energy",    "", "f", "Get bass energy (0.0-1.0)" },
    { "radio.mid_energy",     "mr_cmd_get_mid_energy",     "", "f", "Get mid energy (0.0-1.0)" },
    { "radio.treble_energy",  "mr_cmd_get_treble_energy",  "", "f", "Get treble energy (0.0-1.0)" },
    { "radio.onset_strength", "mr_cmd_get_onset_strength", "", "f", "Get onset/kick strength (0.0-1.0)" },
    { "radio.onset",          "mr_cmd_get_onset",          "", "i", "Get beat onset (1=detected this frame)" },
    { "radio.beat_count",     "mr_cmd_get_beat_count",     "", "i", "Get monotonic beat counter" },
    /* Floating Text (VFT) */
    { "vft",                  "vft_command",               "s", "i", "Spawn floating text: \"text|x|y|param=val|...\"" },
    { "vft_help",             "vft_help",                  "",  "v", "Show full VFT command reference" },
    { "vft_fonts",            "vft_list_fonts",            "",  "v", "List available VFT display fonts" },
    { "vft_set_font",         "vft_set_font",              "i", "i", "Set VFT font (0-4)" },
    /* Android Pairing */
    { "pair",                 "vkt_pair_android",          "",  "v", "Show QR code to pair Android device" },
};

__declspec(dllexport) void vkt_pair_android(void) { qr_show(); }

/* ---- Map Walker API for cross-DLL access (android_bridge) ---- */

__declspec(dllexport) int vkt_mdw_loaded(void) { return mdw_loaded; }
__declspec(dllexport) int vkt_mdw_cur_map(void) { return mdw_loaded ? (int)mdw_cur_map_view : -1; }
__declspec(dllexport) int vkt_mdw_cur_room_map(void) {
    return (mdw_loaded && mdw_cur_ri < mdw_room_count) ? (int)mdw_rooms[mdw_cur_ri].map : -1;
}
__declspec(dllexport) int vkt_mdw_cur_room_num(void) {
    return (mdw_loaded && mdw_cur_ri < mdw_room_count) ? (int)mdw_rooms[mdw_cur_ri].room : -1;
}

#define MDW_SERBUF (1024*1024)
static char mdw_serbuf[MDW_SERBUF];

__declspec(dllexport) const char *vkt_mdw_serialize_map(void)
{
    if (!mdw_loaded) return NULL;

    int pos = 0, left = MDW_SERBUF;
    #define SER(...) do { \
        int _n = _snprintf(mdw_serbuf + pos, left, __VA_ARGS__); \
        if (_n < 0 || _n >= left) goto ser_overflow; \
        pos += _n; left -= _n; \
    } while(0)

    int *ri_seq = (int *)calloc(mdw_room_count, sizeof(int));
    if (!ri_seq) return NULL;
    for (uint32_t i = 0; i < mdw_room_count; i++) ri_seq[i] = -1;

    int cur_seq = -1, seq = 0;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->map != mdw_cur_map_view || r->gx == INT16_MIN) continue;
        ri_seq[i] = seq;
        if (i == mdw_cur_ri) cur_seq = seq;
        seq++;
    }

    SER("{\"map\":%u,\"cur\":%d,\"rooms\":[", (unsigned)mdw_cur_map_view, cur_seq);

    int first = 1;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->map != mdw_cur_map_view || r->gx == INT16_MIN) continue;
        int known = 0;
        char code[8] = "";
        if (api && api->room_is_known)
            known = api->room_is_known(r->checksum, code, sizeof(code));
        char esc[128];
        int j = 0;
        for (int k = 0; r->name[k] && j < 120; k++) {
            if (r->name[k] == '"' || r->name[k] == '\\') esc[j++] = '\\';
            esc[j++] = r->name[k];
        }
        esc[j] = 0;
        if (!first) SER(",");
        first = 0;
        SER("{\"m\":%u,\"r\":%u,\"x\":%d,\"y\":%d,\"n\":%u,\"s\":%u,\"p\":%u,\"l\":%u,\"nm\":\"%s\",\"k\":%d,\"c\":\"%s\"}",
            (unsigned)r->map, (unsigned)r->room, (int)r->gx, (int)r->gy,
            (unsigned)r->npc, (unsigned)r->shop, (unsigned)r->spell,
            (unsigned)r->lair_count, esc, known, known ? code : "");
    }

    SER("],\"exits\":[");
    first = 1;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->map != mdw_cur_map_view || r->gx == INT16_MIN) continue;
        int si = ri_seq[i];
        for (uint32_t e = 0; e < r->exit_count; e++) {
            MdwExit *ex = &mdw_exits[r->exit_ofs + e];
            int ti = -1;
            if (ex->tmap == mdw_cur_map_view) {
                uint32_t ni = mdw_room_lookup(ex->tmap, ex->troom);
                if (ni != 0xFFFFFFFF && ni < mdw_room_count) ti = ri_seq[ni];
            }
            if (!first) SER(",");
            first = 0;
            SER("{\"s\":%d,\"d\":%u,\"f\":%u,\"ti\":%d}", si, (unsigned)ex->dir, (unsigned)ex->flags, ti);
        }
    }

    SER("]}");
    free(ri_seq);
    mdw_serbuf[pos] = 0;
    return mdw_serbuf;

ser_overflow:
    free(ri_seq);
    return NULL;
    #undef SER
}

/* ---- Icon Bar API for cross-DLL access (android_bridge) ---- */

__declspec(dllexport) const char *vkt_icon_states(void)
{
    static char buf[256];
    vib_read_states();
    int pos = 0;
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "{\"count\":%d,\"names\":[", (int)VIB_BTN_COUNT);
    for (int i = 0; i < (int)VIB_BTN_COUNT; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += _snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", vib_buttons[i].tip);
    }
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "],\"states\":[");
    for (int i = 0; i < (int)VIB_BTN_COUNT; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += _snprintf(buf + pos, sizeof(buf) - pos, "%d", vib_states[i]);
    }
    pos += _snprintf(buf + pos, sizeof(buf) - pos, "]}");
    buf[pos] = 0;
    return buf;
}

__declspec(dllexport) void vkt_icon_toggle(int idx)
{
    vib_click(idx);
}

__declspec(dllexport) slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(vkt_commands) / sizeof(vkt_commands[0]);
    return vkt_commands;
}

/* ---- SLOP Command Dispatcher for Console ---- */
/* Dispatches "cmd.name(args)" typed in the VKW console directly to C exports,
 * bypassing Python eval. This makes smoke.help(), bg.set_mode(1) etc. work. */
static int vkt_try_slop_dispatch(const char *input)
{
    const char *paren = strchr(input, '(');
    if (!paren) return 0;
    const char *close = strrchr(input, ')');
    if (!close || close < paren) return 0;

    char cmd_name[128];
    int cmd_len = (int)(paren - input);
    if (cmd_len <= 0 || cmd_len >= (int)sizeof(cmd_name)) return 0;
    memcpy(cmd_name, input, cmd_len);
    cmd_name[cmd_len] = 0;

    /* Look up in SLOP command table */
    int count = sizeof(vkt_commands) / sizeof(vkt_commands[0]);
    slop_command_t *match = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(vkt_commands[i].py_name, cmd_name) == 0) {
            match = &vkt_commands[i];
            break;
        }
    }
    if (!match) return 0;

    /* Resolve the DLL export function */
    static HMODULE self_mod = NULL;
    if (!self_mod) self_mod = GetModuleHandleA("vk_terminal.dll");
    if (!self_mod) return 0;
    FARPROC fn = GetProcAddress(self_mod, match->c_func);
    if (!fn) return 0;

    /* Parse arguments from between parens */
    char args_str[512];
    int args_len = (int)(close - paren - 1);
    if (args_len < 0) args_len = 0;
    if (args_len >= (int)sizeof(args_str)) args_len = (int)sizeof(args_str) - 1;
    memcpy(args_str, paren + 1, args_len);
    args_str[args_len] = 0;

    const char *atypes = match->arg_types;
    int argc = (int)strlen(atypes);
    int iargs[8] = {0};
    float fargs[8] = {0};
    char sarg_bufs[8][512];
    const char *sargs[8] = {NULL};

    if (argc > 0) {
        char *tok = args_str;
        for (int i = 0; i < argc && i < 8; i++) {
            while (*tok == ' ') tok++;
            char *comma = strchr(tok, ',');
            if (comma && i < argc - 1) *comma = 0;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = 0;

            if (atypes[i] == 'i') {
                iargs[i] = atoi(tok);
            } else if (atypes[i] == 'f') {
                fargs[i] = (float)atof(tok);
            } else if (atypes[i] == 's') {
                char *s = tok;
                int slen = (int)strlen(s);
                if (slen >= 2 && ((s[0] == '\'' && s[slen-1] == '\'') ||
                                  (s[0] == '"' && s[slen-1] == '"'))) {
                    s++; slen -= 2;
                }
                if (slen >= (int)sizeof(sarg_bufs[i])) slen = (int)sizeof(sarg_bufs[i]) - 1;
                memcpy(sarg_bufs[i], s, slen);
                sarg_bufs[i][slen] = 0;
                sargs[i] = sarg_bufs[i];
            }
            if (comma && i < argc - 1) tok = comma + 1;
        }
    }

    char result_buf[256] = "";
    const char *rtype = match->ret_type;

    /* All exports are __cdecl (mingw default), NOT __stdcall */
    if (argc == 0) {
        if (rtype[0] == 'v') ((void (__cdecl *)(void))fn)();
        else if (rtype[0] == 'i') {
            int r = ((int (__cdecl *)(void))fn)();
            _snprintf(result_buf, sizeof(result_buf), "%d", r);
        } else if (rtype[0] == 'f') {
            float r = ((float (__cdecl *)(void))fn)();
            _snprintf(result_buf, sizeof(result_buf), "%.4f", r);
        } else if (rtype[0] == 's') {
            const char *r = ((const char *(__cdecl *)(void))fn)();
            _snprintf(result_buf, sizeof(result_buf), "%s", r ? r : "(null)");
        }
    } else if (argc == 1 && atypes[0] == 'i') {
        if (rtype[0] == 'v') ((void (__cdecl *)(int))fn)(iargs[0]);
        else if (rtype[0] == 'i') {
            int r = ((int (__cdecl *)(int))fn)(iargs[0]);
            _snprintf(result_buf, sizeof(result_buf), "%d", r);
        }
    } else if (argc == 1 && atypes[0] == 'f') {
        if (rtype[0] == 'v') ((void (__cdecl *)(float))fn)(fargs[0]);
        else if (rtype[0] == 'f') {
            float r = ((float (__cdecl *)(float))fn)(fargs[0]);
            _snprintf(result_buf, sizeof(result_buf), "%.4f", r);
        }
    } else if (argc == 1 && atypes[0] == 's') {
        if (!sargs[0]) sargs[0] = "";
        if (rtype[0] == 'v') {
            ((void (__cdecl *)(const char *))fn)(sargs[0]);
        } else if (rtype[0] == 'i') {
            int r = ((int (__cdecl *)(const char *))fn)(sargs[0]);
            _snprintf(result_buf, sizeof(result_buf), "%d", r);
        } else if (rtype[0] == 'f') {
            float r = ((float (__cdecl *)(const char *))fn)(sargs[0]);
            _snprintf(result_buf, sizeof(result_buf), "%.4f", r);
        } else if (rtype[0] == 's') {
            const char *r = ((const char *(__cdecl *)(const char *))fn)(sargs[0]);
            _snprintf(result_buf, sizeof(result_buf), "%s", r ? r : "(null)");
        }
    } else if (argc == 2 && atypes[0] == 'i' && atypes[1] == 's') {
        if (rtype[0] == 'v') ((void (__cdecl *)(int, const char *))fn)(iargs[0], sargs[1]);
    } else if (argc == 2 && atypes[0] == 'i' && atypes[1] == 'f') {
        if (rtype[0] == 'v') ((void (__cdecl *)(int, float))fn)(iargs[0], fargs[1]);
    } else if (argc == 6) {
        if (rtype[0] == 'v') ((void (__cdecl *)(float,float,float,float,float,float))fn)(
            fargs[0], fargs[1], fargs[2], fargs[3], fargs[4], fargs[5]);
    } else if (argc == 7) {
        if (rtype[0] == 'v') ((void (__cdecl *)(float,float,float,float,float,float,float))fn)(
            fargs[0], fargs[1], fargs[2], fargs[3], fargs[4], fargs[5], fargs[6]);
    } else {
        return 0;
    }

    if (result_buf[0] && vkw_console_idx >= 0) {
        vkw_print(vkw_console_idx, result_buf);
    }
    return 1;
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
    /* Alt+F4 from MegaMUD window — kill instantly, no dialogs */
    if (msg == WM_SYSKEYDOWN && wParam == VK_F4) {
        ExitProcess(0);
        return 1;
    }
    return 0;
}

/* ---- Settings Save/Load ---- */
#define VKT_INI_SECTION "VulkanTerminal"

static void vkt_get_ini_path(char *out, int maxlen)
{
    char mod[MAX_PATH];
    GetModuleFileNameA(NULL, mod, MAX_PATH);
    /* Strip exe name, append plugins\vk_terminal.ini */
    char *slash = strrchr(mod, '\\');
    if (slash) *(slash + 1) = '\0';
    _snprintf(out, maxlen, "%splugins\\vk_terminal.ini", mod);
    out[maxlen - 1] = '\0';
}

static void vkt_save_settings(void)
{
    char ini[MAX_PATH];
    vkt_get_ini_path(ini, MAX_PATH);
    char buf[32];
    #define SAVE_INT(key, val) do { _snprintf(buf, 32, "%d", val); \
        WritePrivateProfileStringA(VKT_INI_SECTION, key, buf, ini); } while(0)
    #define SAVE_FLOAT(key, val) do { _snprintf(buf, 32, "%.4f", val); \
        WritePrivateProfileStringA(VKT_INI_SECTION, key, buf, ini); } while(0)

    SAVE_INT("theme", current_theme);
    SAVE_INT("font", current_font);
    SAVE_INT("fx_liquid", fx_liquid_mode);
    SAVE_INT("fx_waves", fx_waves_mode);
    SAVE_INT("fx_fbm", fx_fbm_mode);
    SAVE_INT("fx_sobel", fx_sobel_mode);
    SAVE_INT("fx_scanlines", fx_scanline_mode);
    SAVE_INT("fx_smoky", fx_smoky_mode);
    SAVE_INT("fx_shadow", fx_shadow_mode);
    SAVE_INT("fx_wobble", fx_wobble_mode);
    SAVE_INT("fx_rain", fx_rain_mode);
    SAVE_INT("fx_crawl", fx_crawl_mode);
    SAVE_FLOAT("rain_size", rain_size);
    SAVE_FLOAT("rain_speed", rain_speed);
    SAVE_FLOAT("rain_freq", rain_freq);
    SAVE_FLOAT("rain_warp", rain_warp);
    SAVE_INT("round_timer", vrt_visible);
    SAVE_INT("vrt_x", (int)vrt_x); SAVE_INT("vrt_y", (int)vrt_y);
    SAVE_FLOAT("vrt_size", vrt_size);
    SAVE_INT("vrt_orbits", vrt_orbits_visible);
    SAVE_INT("status_bar", vsb_visible);
    SAVE_INT("icon_bar", vib_visible);
    SAVE_INT("icon_bar_pin", vib_pinned);
    SAVE_INT("exp_bar", vxb_visible);
    SAVE_INT("player_stats", pst_visible);
    SAVE_INT("round_recap", pst_round_recap);
    SAVE_INT("mudradio", mr_visible);
    SAVE_INT("rm_show_debug", rm_show_debug);
    SAVE_INT("font_adj", term_font_adj);
    SAVE_INT("term_margin", term_margin);
    SAVE_FLOAT("brightness", pp_brightness);
    SAVE_FLOAT("contrast", pp_contrast);
    SAVE_FLOAT("hue", pp_hue);
    SAVE_FLOAT("saturation", pp_saturation);
    /* Background settings */
    SAVE_INT("bg_mode", bg_mode);
    SAVE_FLOAT("bg_plasma_speed", bg_plasma_speed);
    SAVE_FLOAT("bg_plasma_turbulence", bg_plasma_turbulence);
    SAVE_FLOAT("bg_plasma_complexity", bg_plasma_complexity);
    SAVE_FLOAT("bg_plasma_hue", bg_plasma_hue);
    SAVE_FLOAT("bg_plasma_saturation", bg_plasma_saturation);
    SAVE_FLOAT("bg_plasma_brightness", bg_plasma_brightness);
    SAVE_FLOAT("bg_plasma_alpha", bg_plasma_alpha);
    SAVE_FLOAT("bg_plasma_sobel", bg_plasma_sobel);
    SAVE_FLOAT("bg_plasma_sobel_angle", bg_plasma_sobel_angle);
    SAVE_FLOAT("bg_plasma_edge_glow", bg_plasma_edge_glow);
    SAVE_FLOAT("bg_plasma_sharpen", bg_plasma_sharpen);
    SAVE_INT("bg_material", bg_plasma_material);
    SAVE_FLOAT("bg_mat_str", bg_plasma_mat_str);
    SAVE_INT("bg_tonemap", bg_tonemap_mode);
    SAVE_FLOAT("bg_tonemap_exp", bg_tonemap_exposure);
    SAVE_INT("bg_beat_bass", bg_plasma_beat_bass);
    SAVE_INT("bg_beat_mid", bg_plasma_beat_mid);
    SAVE_INT("bg_beat_treble", bg_plasma_beat_treble);
    SAVE_INT("bg_beat_rms", bg_plasma_beat_rms);
    /* Background Settings dialog */
    SAVE_INT("bgp_tab", bgp_tab);
    SAVE_INT("bgp_x", (int)bgp_x); SAVE_INT("bgp_y", (int)bgp_y);
    SAVE_INT("bgp_w", (int)bgp_w); SAVE_INT("bgp_h", (int)bgp_h);
    /* Parallax Starfield */
    SAVE_INT("star_camera_mode", bg_star_camera_mode);
    SAVE_FLOAT("star_swerve", bg_star_swerve_amt);
    SAVE_FLOAT("star_drift_x", bg_star_drift_x);
    SAVE_FLOAT("star_drift_y", bg_star_drift_y);
    SAVE_FLOAT("star_drift_speed", bg_star_drift_speed);
    SAVE_FLOAT("star_parallax", bg_star_parallax);
    SAVE_FLOAT("star_layers", bg_star_layer_count);
    SAVE_FLOAT("star_density", bg_star_density);
    SAVE_FLOAT("star_size", bg_star_size);
    SAVE_FLOAT("star_bright", bg_star_brightness);
    SAVE_FLOAT("star_sat", bg_star_saturation);
    SAVE_FLOAT("star_hue", bg_star_hue);
    SAVE_FLOAT("star_bg_glow", bg_star_bg_glow);
    SAVE_FLOAT("star_twinkle_speed", bg_star_twinkle_speed);
    SAVE_FLOAT("star_twinkle_amt", bg_star_twinkle_amt);
    SAVE_INT("star_beat_bass", bg_star_beat_bass);
    SAVE_INT("star_beat_mid", bg_star_beat_mid);
    SAVE_INT("star_beat_treb", bg_star_beat_treble);
    SAVE_INT("star_beat_rms", bg_star_beat_rms);
    /* Alien Wall */
    SAVE_FLOAT("alien_speed", bg_alien_speed);
    SAVE_FLOAT("alien_cell_scale", bg_alien_cell_scale);
    SAVE_FLOAT("alien_warp", bg_alien_warp);
    SAVE_FLOAT("alien_hue", bg_alien_hue);
    SAVE_FLOAT("alien_sat", bg_alien_saturation);
    SAVE_FLOAT("alien_bright", bg_alien_brightness);
    SAVE_FLOAT("alien_alpha", bg_alien_alpha);
    SAVE_FLOAT("alien_vein", bg_alien_vein);
    SAVE_FLOAT("alien_pulse", bg_alien_pulse_speed);
    SAVE_FLOAT("alien_glow", bg_alien_glow);
    SAVE_FLOAT("alien_depth", bg_alien_depth_layers);
    SAVE_FLOAT("alien_rough", bg_alien_roughness);
    SAVE_FLOAT("alien_membrane", bg_alien_membrane);
    SAVE_FLOAT("alien_wet", bg_alien_wet);
    SAVE_FLOAT("alien_fluid", bg_alien_fluid_speed);
    SAVE_FLOAT("alien_specular", bg_alien_specular);
    SAVE_FLOAT("alien_cell_move", bg_alien_cell_move);
    SAVE_FLOAT("alien_bulge", bg_alien_bulge);
    SAVE_INT("alien_cell_style", bg_alien_cell_style);
    SAVE_INT("alien_beat_bass", bg_alien_beat_bass);
    SAVE_INT("alien_beat_mid", bg_alien_beat_mid);
    SAVE_INT("alien_beat_treb", bg_alien_beat_treble);
    SAVE_INT("alien_beat_rms", bg_alien_beat_rms);
    /* Display / HDR */
    SAVE_INT("hdr_enabled", hdr_enabled);
    SAVE_INT("vk_vsync", vk_vsync);
    SAVE_INT("snd_res_sel", snd_res_sel);
    /* Sound settings */
    SAVE_INT("snd_in_sel", snd_in_sel);
    SAVE_INT("snd_out_sel", snd_out_sel);
    SAVE_INT("snd_master_vol", snd_master_vol);
    /* Drop shadow settings */
    SAVE_FLOAT("shadow_opacity", shadow_opacity);
    SAVE_FLOAT("shadow_angle", shadow_angle);
    SAVE_FLOAT("shadow_distance", shadow_distance);
    SAVE_FLOAT("shadow_blur", shadow_blur);
    SAVE_FLOAT("shadow_r", shadow_r);
    SAVE_FLOAT("shadow_g", shadow_g);
    SAVE_FLOAT("shadow_b", shadow_b);
    /* Smoke settings */
    SAVE_FLOAT("smoke_decay", smoke_decay);
    SAVE_FLOAT("smoke_depth", smoke_depth);
    SAVE_FLOAT("smoke_zoom", smoke_zoom);
    SAVE_FLOAT("smoke_hue", smoke_hue);
    SAVE_FLOAT("smoke_saturation", smoke_saturation);
    SAVE_FLOAT("smoke_value", smoke_value);
    /* Window positions */
    SAVE_INT("pst_x", (int)pst_x); SAVE_INT("pst_y", (int)pst_y);
    SAVE_INT("mr_x", (int)mr_x); SAVE_INT("mr_y", (int)mr_y);
    SAVE_INT("mr_w", (int)mr_w); SAVE_INT("mr_h", (int)mr_h);
    /* Map Walker state */
    SAVE_INT("mdw_visible", mdw_visible);
    SAVE_INT("mdw_x", (int)mdw_x); SAVE_INT("mdw_y", (int)mdw_y);
    SAVE_INT("mdw_w", (int)mdw_w); SAVE_INT("mdw_h", (int)mdw_h);
    SAVE_FLOAT("mdw_zoom", mdw_zoom);
    SAVE_FLOAT("mdw_view_x", mdw_view_x);
    SAVE_FLOAT("mdw_view_y", mdw_view_y);
    SAVE_INT("mdw_cur_map_view", (int)mdw_cur_map_view);
    SAVE_INT("mdw_auto_recenter", mdw_auto_recenter);
    SAVE_INT("mdw_use_rm", mdw_use_rm);
    /* Recent destinations */
    SAVE_INT("recent_count", vkm_goto_count);
    for (int i = 0; i < vkm_goto_count; i++) {
        char key[32];
        _snprintf(key, sizeof(key), "recent_%d", i);
        WritePrivateProfileStringA(VKT_INI_SECTION, key, vkm_goto_names[i], ini);
    }

    #undef SAVE_INT
    #undef SAVE_FLOAT
    if (api) api->log("[vk_terminal] Settings saved to %s\n", ini);
}

static void vkt_load_settings(void)
{
    char ini[MAX_PATH];
    vkt_get_ini_path(ini, MAX_PATH);
    /* Check if file exists */
    DWORD attr = GetFileAttributesA(ini);
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    #define LOAD_INT(key, var, def) var = (int)GetPrivateProfileIntA(VKT_INI_SECTION, key, def, ini)
    #define LOAD_FLOAT(key, var, def) do { char _b[32]; \
        GetPrivateProfileStringA(VKT_INI_SECTION, key, #def, _b, 32, ini); \
        var = (float)atof(_b); } while(0)

    LOAD_INT("theme", current_theme, 0);
    LOAD_INT("font", current_font, -1);
    LOAD_INT("fx_liquid", fx_liquid_mode, 0);
    LOAD_INT("fx_waves", fx_waves_mode, 0);
    LOAD_INT("fx_fbm", fx_fbm_mode, 0);
    LOAD_INT("fx_sobel", fx_sobel_mode, 0);
    LOAD_INT("fx_scanlines", fx_scanline_mode, 0);
    LOAD_INT("fx_smoky", fx_smoky_mode, 0);
    LOAD_INT("fx_wobble", fx_wobble_mode, 0);
    LOAD_INT("fx_rain", fx_rain_mode, 0);
    LOAD_INT("fx_crawl", fx_crawl_mode, 0);
    LOAD_FLOAT("rain_size", rain_size, 0.06);
    LOAD_FLOAT("rain_speed", rain_speed, 1.0);
    LOAD_FLOAT("rain_freq", rain_freq, 3.0);
    LOAD_FLOAT("rain_warp", rain_warp, 0.02);
    if (fx_rain_mode && fx_waves_mode) fx_waves_mode = 0; /* enforce exclusivity */
    LOAD_INT("round_timer", vrt_visible, 0);
    { int v; LOAD_INT("vrt_x", v, (int)vrt_x); vrt_x = (float)v; }
    { int v; LOAD_INT("vrt_y", v, (int)vrt_y); vrt_y = (float)v; }
    LOAD_FLOAT("vrt_size", vrt_size, VRT_DEFAULT_SIZE);
    LOAD_INT("vrt_orbits", vrt_orbits_visible, 1);
    LOAD_INT("status_bar", vsb_visible, 1);
    LOAD_INT("icon_bar", vib_visible, 1);
    LOAD_INT("icon_bar_pin", vib_pinned, 1);
    LOAD_INT("exp_bar", vxb_visible, 1);
    LOAD_INT("player_stats", pst_visible, 0);
    LOAD_INT("round_recap", pst_round_recap, 1);
    LOAD_INT("mudradio", mr_visible, 0);
    LOAD_INT("rm_show_debug", rm_show_debug, 0);
    LOAD_INT("font_adj", term_font_adj, 0);
    LOAD_INT("term_margin", term_margin, 0);
    LOAD_FLOAT("brightness", pp_brightness, 0.0);
    LOAD_FLOAT("contrast", pp_contrast, 1.0);
    LOAD_FLOAT("hue", pp_hue, 0.0);
    LOAD_FLOAT("saturation", pp_saturation, 1.0);
    /* Background settings */
    LOAD_INT("bg_mode", bg_mode, 0);
    LOAD_FLOAT("bg_plasma_speed", bg_plasma_speed, 1.0);
    LOAD_FLOAT("bg_plasma_turbulence", bg_plasma_turbulence, 0.5);
    LOAD_FLOAT("bg_plasma_complexity", bg_plasma_complexity, 4.0);
    LOAD_FLOAT("bg_plasma_hue", bg_plasma_hue, 0.0);
    LOAD_FLOAT("bg_plasma_saturation", bg_plasma_saturation, 0.8);
    LOAD_FLOAT("bg_plasma_brightness", bg_plasma_brightness, 0.7);
    LOAD_FLOAT("bg_plasma_alpha", bg_plasma_alpha, 0.35);
    LOAD_FLOAT("bg_plasma_sobel", bg_plasma_sobel, 0.0);
    LOAD_FLOAT("bg_plasma_sobel_angle", bg_plasma_sobel_angle, 135.0);
    LOAD_FLOAT("bg_plasma_edge_glow", bg_plasma_edge_glow, 0.0);
    LOAD_FLOAT("bg_plasma_sharpen", bg_plasma_sharpen, 0.0);
    LOAD_INT("bg_material", bg_plasma_material, 0);
    LOAD_FLOAT("bg_mat_str", bg_plasma_mat_str, 1.0);
    LOAD_INT("bg_tonemap", bg_tonemap_mode, 0);
    LOAD_FLOAT("bg_tonemap_exp", bg_tonemap_exposure, 1.0);
    LOAD_INT("bg_beat_bass", bg_plasma_beat_bass, 0);
    LOAD_INT("bg_beat_mid", bg_plasma_beat_mid, 0);
    LOAD_INT("bg_beat_treble", bg_plasma_beat_treble, 0);
    LOAD_INT("bg_beat_rms", bg_plasma_beat_rms, 0);
    /* Background Settings dialog */
    LOAD_INT("bgp_tab", bgp_tab, 0);
    { int v; LOAD_INT("bgp_x", v, (int)bgp_x); bgp_x = (float)v; }
    { int v; LOAD_INT("bgp_y", v, (int)bgp_y); bgp_y = (float)v; }
    { int v; LOAD_INT("bgp_w", v, (int)bgp_w); bgp_w = (float)v; bgp_scaled = 1; }
    { int v; LOAD_INT("bgp_h", v, (int)bgp_h); bgp_h = (float)v; }
    if (bgp_tab < 0 || bgp_tab >= BGP_TAB_COUNT) bgp_tab = 0;
    if (bgp_w < BGP_MIN_W) bgp_w = BGP_MIN_W;
    if (bgp_h < BGP_MIN_H) bgp_h = BGP_MIN_H;
    /* Parallax Starfield */
    LOAD_INT("star_camera_mode", bg_star_camera_mode, BG_STAR_MODE_WARP);
    if (bg_star_camera_mode < 0 || bg_star_camera_mode >= BG_STAR_MODE_COUNT)
        bg_star_camera_mode = BG_STAR_MODE_WARP;
    LOAD_FLOAT("star_swerve", bg_star_swerve_amt, 0.35);
    LOAD_FLOAT("star_drift_x", bg_star_drift_x, 0.0);
    LOAD_FLOAT("star_drift_y", bg_star_drift_y, 0.0);
    LOAD_FLOAT("star_drift_speed", bg_star_drift_speed, 1.0);
    LOAD_FLOAT("star_parallax", bg_star_parallax, 0.62);
    LOAD_FLOAT("star_layers", bg_star_layer_count, 8.0);
    LOAD_FLOAT("star_density", bg_star_density, 1.0);
    LOAD_FLOAT("star_size", bg_star_size, 1.0);
    LOAD_FLOAT("star_bright", bg_star_brightness, 1.0);
    LOAD_FLOAT("star_sat", bg_star_saturation, 1.0);
    LOAD_FLOAT("star_hue", bg_star_hue, 0.0);
    LOAD_FLOAT("star_bg_glow", bg_star_bg_glow, 0.35);
    LOAD_FLOAT("star_twinkle_speed", bg_star_twinkle_speed, 1.2);
    LOAD_FLOAT("star_twinkle_amt", bg_star_twinkle_amt, 0.55);
    LOAD_INT("star_beat_bass", bg_star_beat_bass, BGP_BEAT_PULSE);
    LOAD_INT("star_beat_mid", bg_star_beat_mid, BGP_BEAT_TWINKLE);
    LOAD_INT("star_beat_treb", bg_star_beat_treble, BGP_BEAT_JITTER);
    LOAD_INT("star_beat_rms", bg_star_beat_rms, BGP_BEAT_BRIGHT);
    /* Alien Wall */
    LOAD_FLOAT("alien_speed", bg_alien_speed, 0.8);
    LOAD_FLOAT("alien_cell_scale", bg_alien_cell_scale, 3.0);
    LOAD_FLOAT("alien_warp", bg_alien_warp, 1.5);
    LOAD_FLOAT("alien_hue", bg_alien_hue, 120.0);
    LOAD_FLOAT("alien_sat", bg_alien_saturation, 0.85);
    LOAD_FLOAT("alien_bright", bg_alien_brightness, 0.7);
    LOAD_FLOAT("alien_alpha", bg_alien_alpha, 0.35);
    LOAD_FLOAT("alien_vein", bg_alien_vein, 1.0);
    LOAD_FLOAT("alien_pulse", bg_alien_pulse_speed, 1.0);
    LOAD_FLOAT("alien_glow", bg_alien_glow, 0.8);
    LOAD_FLOAT("alien_depth", bg_alien_depth_layers, 2.0);
    LOAD_FLOAT("alien_rough", bg_alien_roughness, 0.2);
    LOAD_FLOAT("alien_membrane", bg_alien_membrane, 0.4);
    LOAD_FLOAT("alien_wet", bg_alien_wet, 1.0);
    LOAD_FLOAT("alien_fluid", bg_alien_fluid_speed, 0.8);
    LOAD_FLOAT("alien_specular", bg_alien_specular, 0.5);
    LOAD_FLOAT("alien_cell_move", bg_alien_cell_move, 0.5);
    LOAD_FLOAT("alien_bulge", bg_alien_bulge, 0.6);
    LOAD_INT("alien_cell_style", bg_alien_cell_style, 0);
    LOAD_INT("alien_beat_bass", bg_alien_beat_bass, BGP_BEAT_BRIGHT);
    LOAD_INT("alien_beat_mid", bg_alien_beat_mid, BGP_BEAT_HUE);
    LOAD_INT("alien_beat_treb", bg_alien_beat_treble, BGP_BEAT_ZOOM);
    LOAD_INT("alien_beat_rms", bg_alien_beat_rms, BGP_BEAT_NONE);
    /* Display / HDR */
    LOAD_INT("hdr_enabled", hdr_enabled, 0);
    LOAD_INT("vk_vsync", vk_vsync, 1);
    LOAD_INT("snd_res_sel", snd_res_sel, -1);
    /* Apply saved resolution */
    if (snd_res_sel >= 0 && snd_res_sel < (int)NUM_RES) {
        fs_width = resolutions[snd_res_sel].w;
        fs_height = resolutions[snd_res_sel].h;
    }
    /* Sound settings */
    LOAD_INT("snd_in_sel", snd_in_sel, 0);
    LOAD_INT("snd_out_sel", snd_out_sel, 0);
    LOAD_INT("snd_master_vol", snd_master_vol, 80);
    /* Drop shadow settings */
    LOAD_INT("fx_shadow", fx_shadow_mode, 0);
    LOAD_FLOAT("shadow_opacity", shadow_opacity, 0.6);
    LOAD_FLOAT("shadow_angle", shadow_angle, 135.0);
    LOAD_FLOAT("shadow_distance", shadow_distance, 2.0);
    LOAD_FLOAT("shadow_blur", shadow_blur, 1.0);
    LOAD_FLOAT("shadow_r", shadow_r, 0.0);
    LOAD_FLOAT("shadow_g", shadow_g, 0.0);
    LOAD_FLOAT("shadow_b", shadow_b, 0.0);
    LOAD_FLOAT("smoke_decay", smoke_decay, 1.5);
    LOAD_FLOAT("smoke_depth", smoke_depth, 0.8);
    LOAD_FLOAT("smoke_zoom", smoke_zoom, 2.0);
    LOAD_FLOAT("smoke_hue", smoke_hue, 0.0);
    LOAD_FLOAT("smoke_saturation", smoke_saturation, 0.0);
    LOAD_FLOAT("smoke_value", smoke_value, 1.0);
    { int v; LOAD_INT("pst_x", v, (int)pst_x); pst_x = (float)v; }
    { int v; LOAD_INT("pst_y", v, (int)pst_y); pst_y = (float)v; }
    { int v; LOAD_INT("mr_x", v, (int)mr_x); mr_x = (float)v; }
    { int v; LOAD_INT("mr_y", v, (int)mr_y); mr_y = (float)v; }
    { int v; LOAD_INT("mr_w", v, (int)mr_w); mr_w = (float)v; }
    { int v; LOAD_INT("mr_h", v, (int)mr_h); mr_h = (float)v; }
    /* Map Walker state */
    LOAD_INT("mdw_visible", mdw_visible, 0);
    { int v; LOAD_INT("mdw_x", v, (int)mdw_x); mdw_x = (float)v; }
    { int v; LOAD_INT("mdw_y", v, (int)mdw_y); mdw_y = (float)v; }
    { int v; LOAD_INT("mdw_w", v, (int)mdw_w); mdw_w = (float)v; }
    { int v; LOAD_INT("mdw_h", v, (int)mdw_h); mdw_h = (float)v; }
    LOAD_FLOAT("mdw_zoom", mdw_zoom, 4.0);
    LOAD_FLOAT("mdw_view_x", mdw_view_x, 0.0);
    LOAD_FLOAT("mdw_view_y", mdw_view_y, 0.0);
    { int v; LOAD_INT("mdw_cur_map_view", v, 1); mdw_cur_map_view = (uint16_t)v; }
    LOAD_INT("mdw_auto_recenter", mdw_auto_recenter, 1);
    LOAD_INT("mdw_use_rm", mdw_use_rm, 1);
    /* Recent destinations */
    LOAD_INT("recent_count", vkm_goto_count, 0);
    if (vkm_goto_count > VKM_GOTO_MAX) vkm_goto_count = VKM_GOTO_MAX;
    for (int i = 0; i < vkm_goto_count; i++) {
        char key[32];
        _snprintf(key, sizeof(key), "recent_%d", i);
        GetPrivateProfileStringA(VKT_INI_SECTION, key, "", vkm_goto_names[i], 64, ini);
    }

    #undef LOAD_INT
    #undef LOAD_FLOAT
    if (api) api->log("[vk_terminal] Settings loaded from %s\n", ini);
}

static void vkt_reset_settings(void)
{
    current_theme = 0;
    current_font = -1;
    fx_liquid_mode = 0;
    fx_waves_mode = 0;
    fx_fbm_mode = 0;
    fx_sobel_mode = 0;
    fx_scanline_mode = 0;
    fx_smoky_mode = 0;
    smoke_decay = 1.5f;
    smoke_depth = 0.8f;
    smoke_zoom = 2.0f;
    smoke_hue = 0.0f;
    smoke_saturation = 0.0f;
    smoke_value = 1.0f;
    smk_visible = 0;
    fx_shadow_mode = 0;
    shadow_opacity = 0.6f;
    shadow_angle = 135.0f;
    shadow_distance = 2.0f;
    shadow_blur = 1.0f;
    shadow_r = shadow_g = shadow_b = 0.0f;
    shd_visible = 0;
    fx_rain_mode = 0;
    rain_size = 0.06f; rain_speed = 1.0f;
    rain_freq = 3.0f; rain_warp = 0.02f;
    rn_visible = 0;
    fx_wobble_mode = 0;
    vrt_visible = 0;
    vsb_visible = 0;
    vib_visible = 0;
    vxb_visible = 0;
    pst_visible = 0;
    pst_round_recap = 1;
    mr_visible = 0;
    if (term_font_adj != 0) {
        term_font_adj = 0;
        pending_font_idx = current_font;
        font_change_pending = 1;
    }
    pp_brightness = 0.0f;
    pp_contrast = 1.0f;
    pp_hue = 0.0f;
    pp_saturation = 1.0f;
    bg_mode = BG_NONE;
    bg_plasma_speed = 1.0f;
    bg_plasma_turbulence = 0.5f;
    bg_plasma_complexity = 4.0f;
    bg_plasma_hue = 0.0f;
    bg_plasma_saturation = 0.8f;
    bg_plasma_brightness = 0.7f;
    bg_plasma_alpha = 0.35f;
    bgp_visible = 0;
    apply_theme_palette(current_theme);
    if (api) api->log("[vk_terminal] Settings reset to defaults\n");
}

static int vkt_init(const slop_api_t *a)
{
    api = a;
    api->log("[vk_terminal] Initializing Vulkan Terminal...\n");
    api->add_menu_item("Vulkan Terminal (F11)", IDM_VKT_TOGGLE);

    /* Initialize ANSI terminal emulator */
    InitializeCriticalSection(&ansi_lock);
    InitializeCriticalSection(&bs_lock);
    bs_lock_init = 1;
    ap_init(&ansi_term, TERM_ROWS, TERM_COLS);
    ap_set_scroll_cb(&ansi_term, cb_scroll_cb, NULL);

    vkt_load_settings();
    apply_theme_palette(current_theme);
    input_buf[0] = '\0';
    /* Always load map data so Android remote can access it even when the
     * Map Walker panel is not visible on desktop */
    mdw_try_load();

    mr_init();

    /* Check for launch flags (saved to env vars by msimg32 proxy) */
    {
        char buf[8];
        if (GetEnvironmentVariableA("SLOP_STARTVULKAN", buf, sizeof(buf)) > 0) {
            vkt_visible = 1;
            api->log("[vk_terminal] --startvulkan: auto-launching Vulkan mode\n");
        }
        if (GetEnvironmentVariableA("SLOP_AUTOCONNECT", buf, sizeof(buf)) > 0) {
            vkt_autoconnect = 1;
            api->log("[vk_terminal] --autoconnect: will connect after init\n");
        }
        if (GetEnvironmentVariableA("SLOP_LOOP", vkt_loop_name, sizeof(vkt_loop_name)) > 0) {
            vkt_autoconnect = 1; /* --loop implies --autoconnect */
            api->log("[vk_terminal] --loop: will start loop \"%s\" after in-game\n", vkt_loop_name);
        }
        if (GetEnvironmentVariableA("SLOP_LOADSCRIPTS", vkt_loadscripts, sizeof(vkt_loadscripts)) > 0) {
            api->log("[vk_terminal] --loadscripts: will load \"%s\" after in-game\n", vkt_loadscripts);
            /* If no autoconnect, we still need a thread to wait for in-game */
            if (!vkt_autoconnect)
                vkt_autoconnect = 2; /* 2 = scripts-only, don't auto-connect */
        }
    }

    /* Run all Vulkan/font/buffer/descriptor/icon init synchronously on the
     * main thread while the plugin loader is blocked. Doing this in the
     * worker thread (where it used to run) races with autoroam/mmudpy/
     * script_manager/TTS_SAM init and with megamud's own startup, which
     * corrupted megamud's heap non-deterministically on native Win11.
     * vk_starfield.exe (standalone, main-thread everything) is rock solid
     * on the same lavapipe driver, which pointed at thread-ordering as
     * the real culprit, not Vulkan itself. */
    SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "15");
    SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Creating Vulkan instance...");
    if (vkt_init_vulkan() == 0) {
        SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "40");
        SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Loading font textures...");
        if (vkt_create_font_texture() == 0) {
            SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "55");
            SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Creating buffers...");
            if (vkt_create_buffers() == 0) {
                SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "65");
                SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Creating descriptors...");
                if (vkt_create_descriptors() == 0) {
                    SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "75");
                    SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Initializing fonts...");
                    vkt_init_ui_font();
                    vkt_init_vft_font(0);
                    SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "85");
                    SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Loading icon atlas...");
                    vib_init_icon_atlas();
                    vkt_transition_textures_to_general();
                    vkt_mt_init_done = 1;
                    api->log("[vk_terminal] Main-thread Vulkan init complete\n");
                }
            }
        }
    }
    if (!vkt_mt_init_done) {
        api->log("[vk_terminal] Main-thread Vulkan init FAILED — worker thread will retry\n");
    }

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
    mr_shutdown();
    DeleteCriticalSection(&ansi_lock);
    if (bs_lock_init) { DeleteCriticalSection(&bs_lock); bs_lock_init = 0; }
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
    .on_line     = vkt_on_line,
    .on_round    = vrt_on_round,
    .on_wndproc  = vkt_on_wndproc,
    .on_data     = vkt_on_data,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &vkt_plugin; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)p; (void)r;
    return TRUE;
}
