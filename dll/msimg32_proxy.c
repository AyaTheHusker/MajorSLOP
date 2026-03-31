/*
 * MSIMG32.dll proxy — drops into MegaMUD folder, forwards AlphaBlend to the
 * real system DLL, and runs a TCP control server on DLL_PROCESS_ATTACH.
 *
 * Build (32-bit):
 *   i686-w64-mingw32-gcc -shared -o MSIMG32.dll msimg32_proxy.c msimg32.def -lgdi32 -lws2_32 -luser32 -mwindows
 *
 * Install:
 *   cp MSIMG32.dll /path/to/MegaMUD/
 *
 * Protocol (line-based, \n terminated):
 *   PING                            → PONG
 *   BASE                            → struct base address as hex
 *   FIND <name>                     → scan heap for game state struct, lock base
 *   READ <hex_offset> <size>        → returns hex bytes (relative to struct base)
 *   WRITE <hex_offset> <hex_bytes>  → writes bytes, returns OK
 *   READ32 <hex_offset>             → returns u32 as decimal
 *   WRITE32 <hex_offset> <decimal>  → writes u32, returns OK
 *   READS <hex_offset> <maxlen>     → reads null-terminated string
 *   WRITES <hex_offset> <string>    → writes string + null terminator
 *   READABS <hex_addr> <size>       → read raw bytes at absolute address
 *   STATUS                          → read all 8 statusbar parts (tab-separated)
 *   DLGTEXT <class> <ctrl_id>       → read text from dialog control
 *   CLICK <class> <ctrl_id>         → send BM_CLICK to dialog button
 *   SETTHEME <idx_or_name>          → set widget theme (0-15 or name prefix)
 *   ROUNDTICK <round_num>           → notify DLL of a combat round tick
 *   COMBATEND                       → notify DLL combat has ended
 *   ENUMWIN                         → dump all MMMAIN child windows (class, id, size, style)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "slop_plugin.h"

#pragma comment(lib, "ws2_32.lib")

#include <commctrl.h>

/* Status bar messages — define if missing */
#ifndef SB_GETTEXTA
#define SB_GETTEXTA 0x0402
#endif
#ifndef SB_SETTEXTA
#define SB_SETTEXTA 0x0401
#endif
#ifndef SB_GETPARTS
#define SB_GETPARTS 0x0406
#endif
#ifndef SB_SETPARTS
#define SB_SETPARTS 0x0404
#endif

#define PLUGIN_PORT 9901
#define CMD_BUF 4096
#define MEGAMUD_STATUSBAR_ID 107

/* Known struct offsets (from megamud_offsets.py) */
#define OFF_PLAYER_NAME   0x537C
#define OFF_PLAYER_CUR_HP 0x53D4
#define OFF_PLAYER_MAX_HP 0x53DC
#define OFF_PLAYER_CUR_MANA 0x53E0
#define OFF_PLAYER_MAX_MANA 0x53E8
#define OFF_PLAYER_LEVEL  0x53D0
#define OFF_CONNECTED     0x563C
#define OFF_IN_COMBAT     0x5698
#define OFF_IS_RESTING    0x5678
#define OFF_SHOW_ROUNDS   0x3804
#define OFF_COMBAT_TARGET 0x552C  /* char[] - current combat target name (full, with size prefix) */
#define OFF_ROOM_CHECKSUM 0x2E70  /* uint32 - current room checksum/hash */
#define OFF_CUR_PATH_STEP 0x5898  /* int32 - current path step (1-based) */
#define OFF_PATH_TOTAL    0x5894  /* int32 - total steps in path */
#define OFF_PATHING_ACTIVE 0x5664 /* int32 - non-zero if currently pathing */
#define STRUCT_MIN_SIZE   0x9700

/* MMANSI terminal buffer layout (from Ghidra RE)
 * GetWindowLongA(mmansi_hwnd, 4) returns ptr to window data struct (0x4058 bytes)
 * +0x62  = text buffer (chars), stride 0x84 per row, filled with 0x20 on init
 * +0x1F52 = attribute/color buffer, same stride
 * Rows/cols come from parent struct at +0x1A48 (rows) and +0x1A4C (cols) */
#define MMANSI_TEXT_OFF    0x62
#define MMANSI_ATTR_OFF    0x1F52
#define MMANSI_ROW_STRIDE  0x84   /* 132 bytes per row */
#define MMANSI_MAX_ROWS    60

/* Menu & about window IDs */
#define IDM_SLOP_ABOUT       40901
#define SLOP_WINDOW_CLASS    "MajorSLOPAbout"
#define SLOP_TIMER_ID        1
#define SLOP_TIMER_MS        33   /* ~30 fps */
#define SLOP_WIN_W           420
#define SLOP_WIN_H           270

/* Global state */
static HWND slop_menu_owner = NULL;  /* MMMAIN handle for menu updates */
static HMENU slop_menu = NULL;       /* Our top-level popup menu handle */

/* ---- Forward all msimg32.dll exports to real DLL ---- */

typedef BOOL (WINAPI *AlphaBlend_t)(HDC,int,int,int,int,HDC,int,int,int,int,BLENDFUNCTION);
typedef BOOL (WINAPI *GradientFill_t)(HDC,PTRIVERTEX,ULONG,PVOID,ULONG,ULONG);
typedef BOOL (WINAPI *TransparentBlt_t)(HDC,int,int,int,int,HDC,int,int,int,int,UINT);
typedef BOOL (WINAPI *DllInitialize_t)(void);
typedef void (WINAPI *vSetDdrawflag_t)(void);

static HMODULE real_msimg32 = NULL;
static AlphaBlend_t real_AlphaBlend = NULL;
static GradientFill_t real_GradientFill = NULL;
static TransparentBlt_t real_TransparentBlt = NULL;
static DllInitialize_t real_DllInitialize = NULL;
static vSetDdrawflag_t real_vSetDdrawflag = NULL;

BOOL WINAPI proxy_AlphaBlend(HDC hd,int xd,int yd,int wd,int hd2,
    HDC hs,int xs,int ys,int ws,int hs2,BLENDFUNCTION bf)
{
    if (real_AlphaBlend) return real_AlphaBlend(hd,xd,yd,wd,hd2,hs,xs,ys,ws,hs2,bf);
    return FALSE;
}

BOOL WINAPI proxy_GradientFill(HDC hdc,PTRIVERTEX vert,ULONG nvert,PVOID mesh,ULONG nmesh,ULONG mode)
{
    if (real_GradientFill) return real_GradientFill(hdc,vert,nvert,mesh,nmesh,mode);
    return FALSE;
}

BOOL WINAPI proxy_TransparentBlt(HDC hd,int xd,int yd,int wd,int hd2,
    HDC hs,int xs,int ys,int ws,int hs2,UINT crTransparent)
{
    if (real_TransparentBlt) return real_TransparentBlt(hd,xd,yd,wd,hd2,hs,xs,ys,ws,hs2,crTransparent);
    return FALSE;
}

BOOL WINAPI proxy_DllInitialize(void)
{
    if (real_DllInitialize) return real_DllInitialize();
    return TRUE;
}

void WINAPI proxy_vSetDdrawflag(void)
{
    if (real_vSetDdrawflag) real_vSetDdrawflag();
}

/* ---- Logging ---- */

static FILE *logfile = NULL;

static void logmsg(const char *fmt, ...)
{
    if (!logfile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fflush(logfile);
}

/* ---- Hex helpers ---- */

static int hex2byte(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *hex, unsigned char *out, int max)
{
    int len = 0;
    while (*hex && *(hex+1) && len < max) {
        int hi = hex2byte(*hex);
        int lo = hex2byte(*(hex+1));
        if (hi < 0 || lo < 0) break;
        out[len++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return len;
}

/* ---- Game state struct finder ---- */

static DWORD struct_base = 0;

/*
 * Scan process memory for the game state struct using VirtualQuery.
 * Signature: player name at +0x5360, reasonable HP/level values.
 */
static DWORD find_struct(const char *player_name)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD addr = 0;
    int namelen = (int)strlen(player_name);

    int regions_scanned = 0;
    int regions_total = 0;
    int names_found = 0;
    while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        regions_total++;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)))
        {
            DWORD size_check = (DWORD)mbi.RegionSize;
            if (size_check < 0x100 || size_check >= 200000000) {
                addr = (DWORD)mbi.BaseAddress + mbi.RegionSize;
                if (addr < (DWORD)mbi.BaseAddress) break;
                continue;
            }
            regions_scanned++;
            unsigned char *region = (unsigned char *)mbi.BaseAddress;
            DWORD size = (DWORD)mbi.RegionSize;

            /* Scan for player name */
            for (DWORD off = 0; off + namelen < size; off += 4) {
                unsigned char *candidate = region + off + OFF_PLAYER_NAME;

                /* Check name match */
                if (memcmp(candidate, player_name, namelen) != 0)
                    continue;
                if (candidate[namelen] != '\0')
                    continue;

                names_found++;

                /* Log every candidate with surrounding values */
                DWORD cand_addr = (DWORD)(region + off);
                int hp = *(int *)(region + off + OFF_PLAYER_CUR_HP);
                int max_hp = *(int *)(region + off + OFF_PLAYER_MAX_HP);
                int level = *(int *)(region + off + OFF_PLAYER_LEVEL);
                logmsg("[mudplugin] Candidate #%d at 0x%08X: hp=%d max_hp=%d level=%d\n",
                       names_found, cand_addr, hp, max_hp, level);

                /* Also try shifted offsets: maybe stats are at -0x1C from expected */
                int hp2 = *(int *)(region + off + 0x53B8);
                int maxhp2 = *(int *)(region + off + 0x53BC);
                int level2 = *(int *)(region + off + 0x53B4);
                logmsg("[mudplugin]   Alt offsets: hp=%d max_hp=%d level=%d\n",
                       hp2, maxhp2, level2);

                if (hp < 1 || hp > 100000 || max_hp < 1 || max_hp > 100000)
                {
                    /* Try alt offsets */
                    if (hp2 >= 1 && hp2 <= 100000 && maxhp2 >= 1 && maxhp2 <= 100000 &&
                        level2 >= 1 && level2 <= 1000) {
                        logmsg("[mudplugin] Alt offsets matched! Using this candidate.\n");
                        return cand_addr;
                    }
                    continue;
                }
                if (max_hp < hp)
                    continue;

                if (level < 1 || level > 1000)
                    continue;

                /* Found it */
                DWORD base = (DWORD)(region + off);
                logmsg("[mudplugin] Found struct at 0x%08X (name=%s hp=%d/%d lvl=%d)\n",
                       base, player_name, hp, max_hp, level);
                return base;
            }
        }

        /* Move to next region */
        addr = (DWORD)mbi.BaseAddress + mbi.RegionSize;
        if (addr < (DWORD)mbi.BaseAddress)
            break; /* overflow */
    }

    logmsg("[mudplugin] FIND scanned %d/%d regions, found name %d times, no valid struct\n",
           regions_scanned, regions_total, names_found);
    return 0;
}

/* ---- Proxy connection state ---- */

static volatile int proxy_connected = 0;
static int proxy_port = 9999;
static int web_port   = 8000;
static int dll_port   = PLUGIN_PORT;  /* 9901 — DLL bridge / API port */

/* ================================================================
 * MajorSLOP About Window — plasma fractal animation + status text
 * ================================================================ */

static HWND slop_about_hwnd = NULL;
static HBITMAP slop_dib = NULL;
static unsigned char *slop_pixels = NULL;
static int slop_frame = 0;
static HFONT slop_font_big = NULL;
static HFONT slop_font_small = NULL;

/* Sine table for fast plasma (256 entries, range 0-255) */
static unsigned char slop_sin[256];

static void slop_init_sin(void)
{
    for (int i = 0; i < 256; i++)
        slop_sin[i] = (unsigned char)(128.0 + 127.0 * sin(i * 3.14159265 * 2.0 / 256.0));
}

/* HSV to RGB (h=0-255, s=0-255, v=0-255) */
static void hsv2rgb(int h, int s, int v, unsigned char *r, unsigned char *g, unsigned char *b)
{
    if (s == 0) { *r = *g = *b = (unsigned char)v; return; }
    int region = h / 43;
    int remainder = (h - region * 43) * 6;
    int p = (v * (255 - s)) >> 8;
    int q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    int t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
    switch (region) {
        case 0:  *r=v; *g=t; *b=p; break;
        case 1:  *r=q; *g=v; *b=p; break;
        case 2:  *r=p; *g=v; *b=t; break;
        case 3:  *r=p; *g=q; *b=v; break;
        case 4:  *r=t; *g=p; *b=v; break;
        default: *r=v; *g=p; *b=q; break;
    }
}

static void slop_render_plasma(int w, int h, int frame)
{
    if (!slop_pixels) return;
    int t = frame;
    for (int y = 0; y < h; y++) {
        unsigned char *row = slop_pixels + (h - 1 - y) * w * 3; /* DIB is bottom-up */
        for (int x = 0; x < w; x++) {
            /* Multi-layered plasma */
            int v1 = slop_sin[(x * 3 + t * 2) & 255];
            int v2 = slop_sin[(y * 4 + t * 3) & 255];
            int v3 = slop_sin[((x + y) * 2 + t) & 255];
            int v4 = slop_sin[((x * x + y * y) / (w + 1) + t * 4) & 255];
            int hue = (v1 + v2 + v3 + v4) / 4;
            /* Shift hue over time for flowing color */
            hue = (hue + t * 2) & 255;
            unsigned char r, g, b;
            hsv2rgb(hue, 220, 200 + (v3 >> 3), &r, &g, &b);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }
}

static void slop_draw_text_shadow(HDC hdc, HFONT font, const char *text,
                                  int cx, int cy, COLORREF color, int shadow_off)
{
    SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
    int x = cx - sz.cx / 2;
    int y = cy - sz.cy / 2;
    /* Shadow */
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, x + shadow_off, y + shadow_off, text, (int)strlen(text));
    /* Glow layer */
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutA(hdc, x + shadow_off - 1, y + shadow_off - 1, text, (int)strlen(text));
    /* Main text */
    SetTextColor(hdc, color);
    TextOutA(hdc, x, y, text, (int)strlen(text));
}

static LRESULT CALLBACK slop_about_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        /* Create DIB for plasma */
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = SLOP_WIN_W;
        bmi.bmiHeader.biHeight = SLOP_WIN_H;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC hdc = GetDC(hwnd);
        slop_dib = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, (void**)&slop_pixels, NULL, 0);
        ReleaseDC(hwnd, hdc);

        /* Fonts */
        slop_font_big = CreateFontA(
            -28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");
        slop_font_small = CreateFontA(
            -16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial");

        slop_frame = 0;
        SetTimer(hwnd, SLOP_TIMER_ID, SLOP_TIMER_MS, NULL);
        return 0;
    }
    case WM_TIMER:
        if (wParam == SLOP_TIMER_ID) {
            slop_frame++;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        /* Render plasma into DIB */
        slop_render_plasma(SLOP_WIN_W, SLOP_WIN_H, slop_frame);

        /* Blit plasma */
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP old_bmp = (HBITMAP)SelectObject(mem, slop_dib);
        BitBlt(hdc, 0, 0, SLOP_WIN_W, SLOP_WIN_H, mem, 0, 0, SRCCOPY);

        /* Text floats with gentle sine bob */
        int bob = (int)(6.0 * sin(slop_frame * 0.05));
        int cx = SLOP_WIN_W / 2;

        if (proxy_connected) {
            /* Connected state — bright white/cyan text */
            slop_draw_text_shadow(hdc, slop_font_big, "MajorSLOP Proxy",
                                  cx, 40 + bob, RGB(255, 255, 255), 3);
            slop_draw_text_shadow(hdc, slop_font_big, "Connected",
                                  cx, 72 + bob, RGB(100, 255, 200), 3);

            char line[128];
            sprintf(line, "Proxy Port: %d", proxy_port);
            slop_draw_text_shadow(hdc, slop_font_small, line,
                                  cx, 115 - bob, RGB(200, 220, 255), 2);
            sprintf(line, "Web Client: http://localhost:%d", web_port);
            slop_draw_text_shadow(hdc, slop_font_small, line,
                                  cx, 138 - bob, RGB(200, 220, 255), 2);
            sprintf(line, "DLL API: %d", dll_port);
            slop_draw_text_shadow(hdc, slop_font_small, line,
                                  cx, 161 - bob, RGB(160, 170, 200), 2);
        } else {
            /* Disconnected state — warning colors */
            slop_draw_text_shadow(hdc, slop_font_big, "Proxy Not Connected",
                                  cx, 50 + bob, RGB(255, 100, 100), 3);
            slop_draw_text_shadow(hdc, slop_font_small, "Run MajorSLOP.exe",
                                  cx, 100 - bob, RGB(255, 220, 100), 2);
            char line[64];
            sprintf(line, "DLL Bridge listening on port %d", dll_port);
            slop_draw_text_shadow(hdc, slop_font_small, line,
                                  cx, 135 - bob, RGB(180, 180, 200), 2);
        }

        /* Version tag */
        slop_draw_text_shadow(hdc, slop_font_small, "MajorSLOP v0.1.0 by Tripmunk",
                              cx, SLOP_WIN_H - 30, RGB(180, 180, 220), 2);

        SelectObject(mem, old_bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, SLOP_TIMER_ID);
        if (slop_dib) { DeleteObject(slop_dib); slop_dib = NULL; slop_pixels = NULL; }
        if (slop_font_big) { DeleteObject(slop_font_big); slop_font_big = NULL; }
        if (slop_font_small) { DeleteObject(slop_font_small); slop_font_small = NULL; }
        slop_about_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void slop_show_about(HWND parent)
{
    /* If already open, bring to front */
    if (slop_about_hwnd && IsWindow(slop_about_hwnd)) {
        SetForegroundWindow(slop_about_hwnd);
        return;
    }

    /* Register class (once) */
    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = slop_about_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = SLOP_WINDOW_CLASS;
        RegisterClassA(&wc);
        registered = 1;
    }

    /* Center on parent */
    RECT pr;
    GetWindowRect(parent, &pr);
    int px = (pr.left + pr.right) / 2 - SLOP_WIN_W / 2;
    int py = (pr.top + pr.bottom) / 2 - SLOP_WIN_H / 2;

    slop_about_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        SLOP_WINDOW_CLASS,
        "MajorSLOP Proxy by Tripmunk 2026",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        px, py, SLOP_WIN_W, SLOP_WIN_H,
        parent, NULL, GetModuleHandle(NULL), NULL);

    if (slop_about_hwnd) {
        SetForegroundWindow(slop_about_hwnd);
        ShowWindow(slop_about_hwnd, SW_SHOW);
        UpdateWindow(slop_about_hwnd);
    }
}

/* ================================================================
 * Theme & Round Timer — native Win32 widgets for MegaMUD
 * ================================================================ */

#define RT_TIMER_ID        3
#define RT_TIMER_MS        33      /* ~30 fps */
#define RT_CLASS           "MajorSLOPRoundTimer"
#define RT_WIN_W           190
#define RT_WIN_H           220
#define THEME_SETTINGS_CLASS "MajorSLOPThemes"
#define IDM_SLOP_THEMES     40902
#define IDM_SLOP_ROUNDTIMER 40903
#define IDM_SLOP_PRO_STEP   40904
#define IDM_SLOP_PLUGINS    40905
#define IDM_PLUGIN_BASE     41000  /* plugin menu items: 41000..41099 */
#define IDM_THEME_COMBO     70030

/* ---- Theme definitions (mirrors web client themes.js) ---- */
typedef struct {
    const char *key;     /* theme ID string */
    const char *name;    /* display name */
    unsigned char accent_r, accent_g, accent_b;  /* --accent color */
    unsigned char text_r, text_g, text_b;         /* --text color */
    unsigned char dim_r, dim_g, dim_b;            /* --text-dim color */
    unsigned char hp_full_r, hp_full_g, hp_full_b;   /* HP bar at full */
    unsigned char mana_r, mana_g, mana_b;            /* mana bar color */
    unsigned char bg_r, bg_g, bg_b;              /* --bg (darkest) */
    unsigned char panel_r, panel_g, panel_b;     /* --bg-panel (dialog bg) */
} slop_theme_t;

static const slop_theme_t SLOP_THEMES[] = {
    /*                                    accent          text            dim             hp_full         mana            bg              panel */
    { "greylord",     "Grey Lord",      0x44,0x44,0xCC, 0xC8,0xC8,0xE0, 0x66,0x66,0xAA, 0x22,0xCC,0x44, 0x22,0x66,0xEE, 0x0A,0x0A,0x12, 0x10,0x10,0x1C },
    { "blackfort",    "Black Fort",     0x33,0x33,0x66, 0x99,0x9A,0xB0, 0x44,0x44,0x66, 0x22,0xCC,0x44, 0x22,0x66,0xEE, 0x03,0x03,0x06, 0x04,0x04,0x0A },
    { "khazarad",     "Khazarad",       0xAA,0x88,0x44, 0xD4,0xC4,0xA0, 0x8A,0x7A,0x56, 0xCC,0xAA,0x44, 0x44,0x88,0xCC, 0x0C,0x0A,0x06, 0x18,0x12,0x0A },
    { "silvermere",   "Silvermere",     0x77,0x88,0xBB, 0xD8,0xD8,0xE8, 0x88,0x88,0xAA, 0x22,0xCC,0x44, 0x44,0xAA,0xDD, 0x0E,0x0E,0x12, 0x1C,0x1C,0x24 },
    { "annora",       "Annora",         0x99,0xAA,0xDD, 0xEE,0xEE,0xF4, 0xAA,0xAA,0xCC, 0x22,0xCC,0x44, 0x66,0x88,0xEE, 0x14,0x14,0x18, 0x28,0x28,0x32 },
    { "jorah",        "Jorah",          0x33,0x66,0xCC, 0xB0,0xC0,0xE8, 0x55,0x66,0xAA, 0x22,0xCC,0x44, 0x33,0x66,0xCC, 0x06,0x08,0x10, 0x0A,0x10,0x24 },
    { "putakwa",      "Putakwa",        0x22,0xAA,0x88, 0xA0,0xD8,0xC8, 0x44,0x88,0x66, 0x22,0xCC,0x44, 0x22,0x88,0xAA, 0x04,0x0C,0x0A, 0x08,0x16,0x12 },
    { "void",         "Void",           0x88,0x44,0xCC, 0xD0,0xB0,0xE8, 0x77,0x44,0xAA, 0x22,0xCC,0x44, 0x88,0x44,0xCC, 0x0A,0x04,0x10, 0x14,0x08,0x20 },
    { "ozzrinom",     "Ozzrinom",       0xCC,0x33,0x44, 0xE0,0xB0,0xB0, 0x88,0x44,0x44, 0xCC,0x33,0x44, 0x44,0x66,0xDD, 0x0C,0x04,0x06, 0x1C,0x08,0x0C },
    { "phoenix",      "Phoenix",        0xDD,0x66,0x22, 0xE8,0xD0,0xA8, 0x88,0x66,0x40, 0xDD,0x88,0x22, 0x44,0x88,0xCC, 0x0C,0x08,0x04, 0x1A,0x0E,0x06 },
    { "madwizard",    "Mad Wizard",     0x00,0xEE,0xFF, 0xE0,0xF0,0xFF, 0x66,0xAA,0xCC, 0x00,0xEE,0xFF, 0xFF,0x00,0xCC, 0x06,0x06,0x0C, 0x0C,0x08,0x18 },
    { "tasloi",       "Tasloi",         0x44,0xAA,0x22, 0xC0,0xD8,0xA0, 0x66,0x88,0x44, 0x44,0xAA,0x22, 0x22,0x66,0xEE, 0x06,0x0A,0x04, 0x0E,0x16,0x0A },
    { "frostborn",    "Frostborn",      0x44,0xAA,0xDD, 0xC8,0xE0,0xF0, 0x55,0x88,0xAA, 0x44,0xCC,0xEE, 0x22,0x77,0xBB, 0x06,0x0A,0x0E, 0x0C,0x14,0x1E },
    { "sandstorm",    "Sandstorm",      0xCC,0xAA,0x44, 0xE0,0xD4,0xB8, 0x8A,0x7A,0x56, 0xCC,0xAA,0x44, 0x44,0x88,0xCC, 0x0C,0x0A,0x06, 0x1A,0x14,0x0C },
    { "crystal",      "Crystal Cavern", 0x99,0x66,0xEE, 0xD0,0xC0,0xE8, 0x77,0x66,0xAA, 0x99,0x66,0xEE, 0x44,0x66,0xCC, 0x08,0x06,0x0C, 0x10,0x0C,0x1A },
    { "afroman",      "Afroman",        0xCC,0x22,0x44, 0xEE,0xEE,0xF4, 0xAA,0xAA,0xCC, 0xCC,0x22,0x44, 0x44,0x66,0xDD, 0x06,0x06,0x08, 0x0C,0x0C,0x18 },
};
#define SLOP_THEME_COUNT (sizeof(SLOP_THEMES) / sizeof(SLOP_THEMES[0]))

/* Forward declarations */
static volatile int rt_visible;
static DWORD WINAPI theme_scanner_thread(LPVOID param);
static DWORD WINAPI pro_step_thread(LPVOID param);
static DWORD WINAPI auto_struct_thread(LPVOID param);
static DWORD WINAPI mmansi_scanner_thread(LPVOID param);
static int mmansi_read_row(HWND mmansi, int row, char *out, int out_sz);
static int scan_mmansi_for_location(void);
static void update_statusbar_location(void);
static int loc_map;
static int loc_room;

/* Plugin system forward declarations */
static int plugin_count;
static void plugins_on_line(const char *line);
static void plugins_on_round(int round_num);
static int plugins_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void show_plugins_dialog(HWND parent);
static void ensure_plugins_dir(void);
static void load_plugins(void);
static void unload_plugins(void);

/* ---- Configuration file (majorslop.cfg) ---- */

#define CFG_FILENAME "majorslop.cfg"

static int theme_idx = -1;       /* -1 = no theme (MegaMUD default), 0+ = theme index */
static int rt_show_on_start = 0; /* 1 = show round timer on launch */
static char cfg_player_name[64] = {0}; /* player name for auto-struct-find */
static volatile int pro_every_step = 0; /* 1 = inject "pro" on every path step */
static int pro_last_step = 0;           /* last path step seen */
static DWORD pro_last_checksum = 0;     /* last room checksum seen */

static void cfg_get_path(char *out, int maxlen)
{
    /* Config lives next to the DLL (MegaMUD folder) */
    GetModuleFileNameA(NULL, out, maxlen);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
    else out[0] = '\0';
    strncat(out, CFG_FILENAME, maxlen - (int)strlen(out) - 1);
}

static void cfg_load(void)
{
    char path[MAX_PATH];
    cfg_get_path(path, MAX_PATH);
    FILE *f = fopen(path, "r");
    if (!f) {
        logmsg("[mudplugin] No config file, using defaults\n");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* Skip comments and blank lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        /* Remove trailing newline */
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        nl = strchr(line, '\r'); if (nl) *nl = '\0';

        if (strncmp(line, "theme=", 6) == 0) {
            char *val = line + 6;
            if (strcmp(val, "none") == 0 || strcmp(val, "") == 0) {
                theme_idx = -1;
            } else {
                /* Try numeric */
                int idx = atoi(val);
                if (val[0] >= '0' && val[0] <= '9' && idx >= 0 && idx < (int)SLOP_THEME_COUNT) {
                    theme_idx = idx;
                } else {
                    /* Match by key name */
                    for (int i = 0; i < (int)SLOP_THEME_COUNT; i++) {
                        if (_stricmp(val, SLOP_THEMES[i].key) == 0) {
                            theme_idx = i;
                            break;
                        }
                    }
                }
            }
        } else if (strncmp(line, "round_timer=", 12) == 0) {
            char *val = line + 12;
            rt_show_on_start = (strcmp(val, "shown") == 0 || strcmp(val, "1") == 0);
        } else if (strncmp(line, "player_name=", 12) == 0) {
            strncpy(cfg_player_name, line + 12, sizeof(cfg_player_name) - 1);
            cfg_player_name[sizeof(cfg_player_name) - 1] = '\0';
        } else if (strncmp(line, "pro_every_step=", 15) == 0) {
            pro_every_step = (strcmp(line + 15, "1") == 0 || strcmp(line + 15, "on") == 0);
        }
    }
    fclose(f);
    logmsg("[mudplugin] Config loaded: theme=%s round_timer=%s\n",
           theme_idx >= 0 ? SLOP_THEMES[theme_idx].key : "none",
           rt_show_on_start ? "shown" : "hidden");
}

static void cfg_save(void)
{
    char path[MAX_PATH];
    cfg_get_path(path, MAX_PATH);
    FILE *f = fopen(path, "w");
    if (!f) {
        logmsg("[mudplugin] Failed to save config to %s\n", path);
        return;
    }
    fprintf(f, "# MajorSLOP Configuration\n");
    fprintf(f, "# Edit while MegaMUD is closed, or use the in-app menus.\n");
    fprintf(f, "#\n");
    fprintf(f, "# theme: none, or a theme name (greylord, blackfort, khazarad, etc.)\n");
    fprintf(f, "# round_timer: shown or hidden\n");
    fprintf(f, "\n");
    fprintf(f, "theme=%s\n", theme_idx >= 0 ? SLOP_THEMES[theme_idx].key : "none");
    fprintf(f, "round_timer=%s\n", rt_visible ? "shown" : "hidden");
    fprintf(f, "pro_every_step=%s\n", pro_every_step ? "on" : "off");
    if (cfg_player_name[0])
        fprintf(f, "player_name=%s\n", cfg_player_name);
    fclose(f);
}

/* ---- Window theming engine ---- */

static HBRUSH theme_bg_brush = NULL;    /* bg color brush */
static HBRUSH theme_panel_brush = NULL; /* panel bg brush */
static COLORREF theme_text_color = RGB(0xC8, 0xC8, 0xE0);
static COLORREF theme_bg_color = RGB(0x0A, 0x0A, 0x12);
static COLORREF theme_panel_color = RGB(0x10, 0x10, 0x1C);
static COLORREF theme_accent_color = RGB(0x44, 0x44, 0xCC);
static COLORREF theme_dim_color = RGB(0x66, 0x66, 0xAA);

/* Track subclassed dialogs */
#define MAX_SUBCLASSED 32
static struct { HWND hwnd; WNDPROC orig; } subclassed_dlgs[MAX_SUBCLASSED];
static int num_subclassed = 0;

static int theme_active = 0;  /* 1 when a theme is applied */

static void theme_update_brushes(void)
{
    if (theme_bg_brush) { DeleteObject(theme_bg_brush); theme_bg_brush = NULL; }
    if (theme_panel_brush) { DeleteObject(theme_panel_brush); theme_panel_brush = NULL; }

    if (theme_idx < 0 || theme_idx >= (int)SLOP_THEME_COUNT) {
        theme_active = 0;
        return;
    }

    const slop_theme_t *th = &SLOP_THEMES[theme_idx];
    theme_bg_color = RGB(th->bg_r, th->bg_g, th->bg_b);
    theme_panel_color = RGB(th->panel_r, th->panel_g, th->panel_b);
    theme_text_color = RGB(th->text_r, th->text_g, th->text_b);
    theme_accent_color = RGB(th->accent_r, th->accent_g, th->accent_b);
    theme_dim_color = RGB(th->dim_r, th->dim_g, th->dim_b);
    theme_bg_brush = CreateSolidBrush(theme_bg_color);
    theme_panel_brush = CreateSolidBrush(theme_panel_color);
    theme_active = 1;
}

/* Subclass proc for #32770, MMTALK, MMPRTY dialogs */
static LRESULT CALLBACK theme_dlg_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* Find original proc */
    WNDPROC orig = DefWindowProcA;
    for (int i = 0; i < num_subclassed; i++) {
        if (subclassed_dlgs[i].hwnd == hwnd) { orig = subclassed_dlgs[i].orig; break; }
    }

    if (theme_active && theme_panel_brush) {
        switch (msg) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            return (LRESULT)theme_panel_brush;
        }
        case WM_CTLCOLORDLG:
            return (LRESULT)theme_panel_brush;
        case WM_CTLCOLORBTN:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            return (LRESULT)theme_panel_brush;
        }
        case WM_ERASEBKGND:
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, theme_panel_brush);
            return 1;
        }
        }
    }

    if (msg == WM_NCDESTROY) {
        /* Clean up subclass entry */
        LRESULT ret = CallWindowProcA(orig, hwnd, msg, wParam, lParam);
        for (int i = 0; i < num_subclassed; i++) {
            if (subclassed_dlgs[i].hwnd == hwnd) {
                subclassed_dlgs[i] = subclassed_dlgs[--num_subclassed];
                break;
            }
        }
        return ret;
    }
    return CallWindowProcA(orig, hwnd, msg, wParam, lParam);
}

static void theme_subclass_window(HWND hwnd)
{
    /* Check if already subclassed */
    for (int i = 0; i < num_subclassed; i++) {
        if (subclassed_dlgs[i].hwnd == hwnd) return;
    }
    if (num_subclassed >= MAX_SUBCLASSED) return;

    WNDPROC old = (WNDPROC)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)theme_dlg_proc);
    if (old && old != theme_dlg_proc) {
        subclassed_dlgs[num_subclassed].hwnd = hwnd;
        subclassed_dlgs[num_subclassed].orig = old;
        num_subclassed++;
    }
}

static void theme_apply_all(void)
{
    theme_update_brushes();

    DWORD our_pid = GetCurrentProcessId();
    HWND mw = FindWindowA("MMMAIN", NULL);

    /* Theme the status bar */
    if (mw) {
        HWND sb = FindWindowExA(mw, NULL, "msctls_statusbar32", NULL);
        if (sb) {
            /* CLR_DEFAULT (-1) restores the default color */
            SendMessageA(sb, 0x0401 /* SB_SETBKCOLOR */, 0,
                         theme_active ? (LPARAM)theme_bg_color : (LPARAM)0xFFFFFFFF);
            InvalidateRect(sb, NULL, TRUE);
        }
    }

    /* Find and subclass all top-level windows belonging to our process */
    HWND top = NULL;
    while ((top = FindWindowExA(NULL, top, NULL, NULL)) != NULL) {
        DWORD win_pid = 0;
        GetWindowThreadProcessId(top, &win_pid);
        if (win_pid != our_pid) continue;

        char cls[64] = {0};
        GetClassNameA(top, cls, sizeof(cls));

        /* Subclass dialogs and custom MegaMUD windows */
        if (strcmp(cls, "#32770") == 0 || strcmp(cls, "MMTALK") == 0 ||
            strcmp(cls, "MMPRTY") == 0) {
            theme_subclass_window(top);
            /* Force repaint on all children */
            InvalidateRect(top, NULL, TRUE);
            HWND child = GetWindow(top, GW_CHILD);
            while (child) {
                InvalidateRect(child, NULL, TRUE);
                child = GetWindow(child, GW_HWNDNEXT);
            }
        }
    }

    /* Redraw MMMAIN and menu bar */
    if (mw) {
        InvalidateRect(mw, NULL, TRUE);
        DrawMenuBar(mw);
    }
}

static HWND theme_settings_hwnd = NULL;

/* Read an i32 from struct (used by round timer for combat state) */
static int rt_read_i32(DWORD offset)
{
    if (!struct_base) return 0;
    return *(int *)(struct_base + offset);
}

/* ---- Round Timer State ---- */
static HWND rt_hwnd = NULL;
/* rt_visible forward-declared above cfg section */
static int rt_synced = 0;
static int rt_in_combat = 0;
static int rt_round_num = 0;
static double rt_last_tick_ts = 0.0;   /* GetTickCount64-based ms */
static double rt_round_period = 5000.0;
static double rt_recent_intervals[12];
static int rt_interval_count = 0;
static char rt_delta_text[32] = "";
static double rt_delta_fade = 0.0;

/* Called from protocol handler when proxy sends ROUNDTICK */
static void rt_on_round_tick(int round_num)
{
    double now = (double)GetTickCount64();
    double prev = rt_last_tick_ts;
    rt_round_num = round_num;
    rt_in_combat = 1;

    if (prev > 0.0 && rt_synced && rt_round_period > 0.0) {
        double elapsed = now - prev;
        double cycles = (int)(elapsed / rt_round_period + 0.5);
        if (cycles < 1) cycles = 1;
        double predicted = prev + cycles * rt_round_period;
        int delta = (int)(now - predicted);
        sprintf(rt_delta_text, "%s%dms", delta >= 0 ? "+" : "", delta);
        rt_delta_fade = 1.0;
    }

    if (prev > 0.0) {
        double interval = now - prev;
        double single = interval;
        if (interval > 6000.0 && rt_round_period > 0.0) {
            int rounds = (int)(interval / rt_round_period + 0.5);
            if (rounds > 0) single = interval / rounds;
        }
        if (single > 2000.0 && single < 8000.0) {
            if (rt_interval_count < 12)
                rt_recent_intervals[rt_interval_count++] = single;
            else {
                memmove(rt_recent_intervals, rt_recent_intervals + 1, 11 * sizeof(double));
                rt_recent_intervals[11] = single;
            }
            double ws = 0, vs = 0;
            for (int i = 0; i < rt_interval_count; i++) {
                double w = 1.0 + i;
                ws += w; vs += rt_recent_intervals[i] * w;
            }
            rt_round_period = vs / ws;
        }
    }

    rt_last_tick_ts = now;
    if (rt_round_period > 0.0) rt_synced = 1;
}

static void rt_on_combat_end(void)
{
    rt_in_combat = 0;
}

/* ---- Round Timer GDI Rendering ---- */

static void rt_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    int W = cr.right, H = cr.bottom;

    /* Double buffer */
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    /* Background — dark grey */
    HBRUSH bg = CreateSolidBrush(RGB(20, 20, 28));
    FillRect(mem, &cr, bg);
    DeleteObject(bg);

    /* Use selected theme, or default to Grey Lord colors for the timer */
    const slop_theme_t *th = (theme_idx >= 0 && theme_idx < (int)SLOP_THEME_COUNT)
        ? &SLOP_THEMES[theme_idx] : &SLOP_THEMES[0];
    int cx = W / 2;
    int R = W / 2 - 25;
    if (R > (H / 2 - 40)) R = H / 2 - 40;
    if (R < 20) R = 20;
    int cy = R + 20;
    double now = (double)GetTickCount64();
    double period = rt_round_period;

    /* Outer ring track — bright enough to see on dark bg */
    int penW = R * 8 / 62; if (penW < 2) penW = 2;
    int arcW = R * 6 / 62; if (arcW < 2) arcW = 2;
    int ballR = R * 5 / 62; if (ballR < 3) ballR = 3;
    HPEN dimPen = CreatePen(PS_SOLID, penW, RGB(th->dim_r * 2 / 3, th->dim_g * 2 / 3, th->dim_b * 2 / 3));
    HPEN oldPen = (HPEN)SelectObject(mem, dimPen);
    HBRUSH hollow = (HBRUSH)GetStockObject(HOLLOW_BRUSH);
    HBRUSH oldBr = (HBRUSH)SelectObject(mem, hollow);
    Ellipse(mem, cx - R, cy - R, cx + R, cy + R);
    SelectObject(mem, oldPen);
    DeleteObject(dimPen);

    /* Tick marks — 5 ticks around the ring */
    int ticks = (period > 0) ? (int)(period / 1000.0 + 0.5) : 5;
    if (ticks < 2) ticks = 2;
    for (int i = 0; i < ticks; i++) {
        double a = -3.14159265 / 2.0 + ((double)i / ticks) * 3.14159265 * 2.0;
        int x1 = cx + (int)(cos(a) * (R - 6));
        int y1 = cy + (int)(sin(a) * (R - 6));
        int x2 = cx + (int)(cos(a) * (R + 6));
        int y2 = cy + (int)(sin(a) * (R + 6));
        HPEN tp = CreatePen(PS_SOLID, (i == 0) ? (penW / 4 + 1) : (penW / 8 + 1),
                            (i == 0) ? RGB(220, 220, 240) : RGB(th->dim_r, th->dim_g, th->dim_b));
        SelectObject(mem, tp);
        MoveToEx(mem, x1, y1, NULL);
        LineTo(mem, x2, y2);
        SelectObject(mem, oldPen);
        DeleteObject(tp);
    }

    /* Scaled font sizes */
    int fontBig = R * 18 / 62; if (fontBig < 10) fontBig = 10;
    int fontSmall = R * 13 / 62; if (fontSmall < 8) fontSmall = 8;
    int fontTiny = R * 11 / 62; if (fontTiny < 7) fontTiny = 7;

    /* Progress arc + ball */
    if (rt_synced && rt_last_tick_ts > 0.0) {
        double elapsed = now - rt_last_tick_ts;
        double phase = fmod(elapsed, period) / period;

        /* Draw progress arc as series of line segments — green→yellow→red */
        int steps = 48;
        double startA = -3.14159265 / 2.0;
        int prev_x = 0, prev_y = 0;
        for (int i = 0; i <= steps; i++) {
            double t = (double)i / steps;
            if (t > phase) break;
            double a = startA + t * 3.14159265 * 2.0;
            int px = cx + (int)(cos(a) * R);
            int py = cy + (int)(sin(a) * R);
            /* Color: green → yellow at 50%, yellow → red at 67%+ */
            int cr2, cg, cb;
            if (t < 0.5) {
                double p = t / 0.5;
                cr2 = (int)(40 + 200 * p); cg = (int)(220 - 40 * p); cb = 40;
            } else if (t < 0.67) {
                double p = (t - 0.5) / 0.17;
                cr2 = (int)(240 + 15 * p); cg = (int)(180 - 80 * p); cb = 30;
            } else {
                double p = (t - 0.67) / 0.33;
                cr2 = 255; cg = (int)(100 - 80 * p); cb = (int)(30 - 20 * p);
            }
            if (i > 0) {
                HPEN seg = CreatePen(PS_SOLID, arcW, RGB(cr2, cg, cb));
                SelectObject(mem, seg);
                MoveToEx(mem, prev_x, prev_y, NULL);
                LineTo(mem, px, py);
                SelectObject(mem, oldPen);
                DeleteObject(seg);
            }
            prev_x = px; prev_y = py;
        }

        /* Ball at current phase */
        double ball_a = startA + phase * 3.14159265 * 2.0;
        int bx = cx + (int)(cos(ball_a) * R);
        int by = cy + (int)(sin(ball_a) * R);
        /* Ball color matches phase */
        int br2, bg2, bb;
        if (phase < 0.5) { br2 = 80; bg2 = 220; bb = 40; }
        else if (phase < 0.67) { br2 = 240; bg2 = 180; bb = 30; }
        else { br2 = 255; bg2 = 60; bb = 10; }
        HBRUSH ball = CreateSolidBrush(RGB(br2, bg2, bb));
        SelectObject(mem, ball);
        HPEN ballPen = CreatePen(PS_SOLID, 1, RGB(br2, bg2, bb));
        SelectObject(mem, ballPen);
        Ellipse(mem, bx - ballR, by - ballR, bx + ballR, by + ballR);
        SelectObject(mem, oldBr);
        SelectObject(mem, oldPen);
        DeleteObject(ball);
        DeleteObject(ballPen);

        /* Elapsed text */
        char elapsed_txt[32];
        sprintf(elapsed_txt, "%.1fs / %.1fs", fmod(elapsed, period) / 1000.0, period / 1000.0);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(th->dim_r, th->dim_g, th->dim_b));
        HFONT small_font = CreateFontA(fontSmall, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                        0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        HFONT old_font = (HFONT)SelectObject(mem, small_font);
        SIZE sz;
        GetTextExtentPoint32A(mem, elapsed_txt, (int)strlen(elapsed_txt), &sz);
        TextOutA(mem, cx - sz.cx / 2, cy + 22, elapsed_txt, (int)strlen(elapsed_txt));
        SelectObject(mem, old_font);
        DeleteObject(small_font);
    }

    /* Center text: "ROUND" or "WAITING" */
    SetBkMode(mem, TRANSPARENT);
    HFONT big_font = CreateFontA(fontBig, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
                                  0, 0, ANTIALIASED_QUALITY, 0, "Arial");
    HFONT old_font = (HFONT)SelectObject(mem, big_font);

    if (rt_synced) {
        /* Flash color on recent tick */
        double tick_age = (rt_last_tick_ts > 0) ? (now - rt_last_tick_ts) / 1000.0 : 99.0;
        double flash = (1.0 - tick_age / 1.5);
        if (flash < 0) flash = 0;
        int gr = (int)(80 + 175 * flash);
        int gg = (int)(90 + 165 * flash);
        int gb = (int)(100 + 50 * flash);
        SetTextColor(mem, RGB(gr, gg, gb));
        SIZE sz;
        GetTextExtentPoint32A(mem, "ROUND", 5, &sz);
        TextOutA(mem, cx - sz.cx / 2, cy - sz.cy / 2 - 2, "ROUND", 5);
    } else {
        /* Unsynced — show bright "WAITING FOR SYNC" */
        SetTextColor(mem, RGB(th->accent_r, th->accent_g, th->accent_b));
        SIZE sz;
        GetTextExtentPoint32A(mem, "WAITING", 7, &sz);
        TextOutA(mem, cx - sz.cx / 2, cy - sz.cy / 2 - 8, "WAITING", 7);
        HFONT sub_font = CreateFontA(fontSmall, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                      0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        SelectObject(mem, sub_font);
        SetTextColor(mem, RGB(th->dim_r, th->dim_g, th->dim_b));
        GetTextExtentPoint32A(mem, "FOR SYNC", 8, &sz);
        TextOutA(mem, cx - sz.cx / 2, cy + 6, "FOR SYNC", 8);
        SelectObject(mem, old_font);
        DeleteObject(sub_font);
    }
    SelectObject(mem, old_font);
    DeleteObject(big_font);

    /* Delta text at bottom */
    if (rt_delta_fade > 0.01 && rt_delta_text[0]) {
        int isLate = (rt_delta_text[0] == '+');
        COLORREF dc = isLate ? RGB(255, 180, 60) : RGB(60, 220, 120);
        /* Fade via font alpha isn't easy in GDI, just show it solid until it fades */
        SetTextColor(mem, dc);
        HFONT df = CreateFontA(fontSmall, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET,
                                0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        HFONT odf = (HFONT)SelectObject(mem, df);
        SIZE sz;
        GetTextExtentPoint32A(mem, rt_delta_text, (int)strlen(rt_delta_text), &sz);
        TextOutA(mem, cx - sz.cx / 2, H - 30, rt_delta_text, (int)strlen(rt_delta_text));
        SelectObject(mem, odf);
        DeleteObject(df);
    }

    /* Period label */
    {
        char period_txt[48];
        sprintf(period_txt, "Period: %.0fms", rt_round_period);
        HFONT pf = CreateFontA(fontTiny, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        HFONT opf = (HFONT)SelectObject(mem, pf);
        SetTextColor(mem, RGB(th->dim_r * 3 / 4, th->dim_g * 3 / 4, th->dim_b * 3 / 4));
        SIZE sz;
        GetTextExtentPoint32A(mem, period_txt, (int)strlen(period_txt), &sz);
        TextOutA(mem, cx - sz.cx / 2, H - 16, period_txt, (int)strlen(period_txt));
        SelectObject(mem, opf);
        DeleteObject(pf);
    }

    /* Blit to screen */
    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBr);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK slop_rt_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, RT_TIMER_ID, RT_TIMER_MS, NULL);
        return 0;
    case WM_TIMER:
        if (wParam == RT_TIMER_ID) {
            if (rt_delta_fade > 0.01) rt_delta_fade -= 0.008;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_PAINT:
        rt_paint(hwnd);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        rt_visible = 0;
        ShowWindow(hwnd, SW_HIDE);
        KillTimer(hwnd, RT_TIMER_ID);
        /* Update menu text */
        if (slop_menu)
            ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                        IDM_SLOP_ROUNDTIMER, "Show Round Timer");
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, RT_TIMER_ID);
        rt_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void slop_show_round_timer(HWND parent)
{
    if (rt_hwnd && IsWindow(rt_hwnd)) {
        if (rt_visible) {
            /* Hide it */
            rt_visible = 0;
            ShowWindow(rt_hwnd, SW_HIDE);
            KillTimer(rt_hwnd, RT_TIMER_ID);
            if (slop_menu)
                ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                            IDM_SLOP_ROUNDTIMER, "Show Round Timer");
        } else {
            /* Show it */
            rt_visible = 1;
            ShowWindow(rt_hwnd, SW_SHOWNOACTIVATE);
            SetTimer(rt_hwnd, RT_TIMER_ID, RT_TIMER_MS, NULL);
            if (slop_menu)
                ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                            IDM_SLOP_ROUNDTIMER, "Hide Round Timer");
        }
        return;
    }

    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = slop_rt_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = RT_CLASS;
        RegisterClassA(&wc);
        registered = 1;
    }

    RECT pr;
    GetWindowRect(parent, &pr);
    /* Position inside MegaMUD's window, top-right corner */
    int px = pr.right - RT_WIN_W - 8;
    int py = pr.top + 60;  /* below menu/toolbar */
    /* Clamp within MegaMUD bounds */
    if (px < pr.left) px = pr.left;
    if (py < pr.top) py = pr.top;
    if (py + RT_WIN_H > pr.bottom) py = pr.bottom - RT_WIN_H;

    logmsg("[mudplugin] Creating round timer at %d,%d (%dx%d)\n", px, py, RT_WIN_W, RT_WIN_H);

    rt_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        RT_CLASS,
        "Round Timer",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_THICKFRAME,
        px, py, RT_WIN_W, RT_WIN_H,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    logmsg("[mudplugin] CreateWindowExA returned %p\n", rt_hwnd);

    if (rt_hwnd) {
        rt_visible = 1;
        SetWindowPos(rt_hwnd, HWND_TOPMOST, px, py, RT_WIN_W, RT_WIN_H,
                     SWP_SHOWWINDOW);
        if (slop_menu)
            ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                        IDM_SLOP_ROUNDTIMER, "Hide Round Timer");
        logmsg("[mudplugin] Round timer created and shown\n");
    }
}

/* ================================================================
 * Theme Settings Dialog
 * ================================================================ */

static LRESULT CALLBACK slop_theme_settings_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        int y = 15, x = 15;

        CreateWindowExA(0, "STATIC", "Theme:",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        x, y + 2, 50, 18, hwnd, NULL, GetModuleHandle(NULL), NULL);

        HWND combo = CreateWindowExA(0, "COMBOBOX", "",
                        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                        x + 55, y, 160, 300, hwnd, (HMENU)(INT_PTR)IDM_THEME_COMBO,
                        GetModuleHandle(NULL), NULL);
        SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"None (MegaMUD Default)");
        for (int i = 0; i < (int)SLOP_THEME_COUNT; i++) {
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)SLOP_THEMES[i].name);
        }
        /* combo index 0 = "None", 1+ = theme indices */
        SendMessageA(combo, CB_SETCURSEL, theme_idx + 1, 0);

        /* Apply button */
        y += 30;
        CreateWindowExA(0, "BUTTON", "Apply Theme",
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                        x + 55, y, 120, 28, hwnd, (HMENU)(INT_PTR)70031,
                        GetModuleHandle(NULL), NULL);

        return 0;
    }
    case WM_COMMAND:
    {
        WORD id = LOWORD(wParam);
        WORD notif = HIWORD(wParam);
        if (id == IDM_THEME_COMBO && notif == CBN_SELCHANGE) {
            /* Just track the selection, apply on button press */
            int sel = (int)SendMessageA((HWND)lParam, CB_GETCURSEL, 0, 0);
            theme_idx = sel - 1;
            logmsg("[mudplugin] Theme selected: %d (%s)\n", theme_idx,
                   theme_idx >= 0 ? SLOP_THEMES[theme_idx].name : "none");
        }
        if (id == 70031 && notif == BN_CLICKED) {
            /* Apply button */
            logmsg("[mudplugin] Applying theme %d\n", theme_idx);
            theme_apply_all();
            cfg_save();
            logmsg("[mudplugin] Theme applied and saved\n");
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        theme_settings_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void slop_show_themes(HWND parent)
{
    if (theme_settings_hwnd && IsWindow(theme_settings_hwnd)) {
        SetForegroundWindow(theme_settings_hwnd);
        return;
    }

    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = slop_theme_settings_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = THEME_SETTINGS_CLASS;
        RegisterClassA(&wc);
        registered = 1;
    }

    RECT pr;
    GetWindowRect(parent, &pr);
    int px = (pr.left + pr.right) / 2 - 130;
    int py = (pr.top + pr.bottom) / 2 - 60;

    theme_settings_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        THEME_SETTINGS_CLASS,
        "MajorSLOP Themes",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        px, py, 260, 110,
        parent, NULL, GetModuleHandle(NULL), NULL);

    if (theme_settings_hwnd) {
        SetForegroundWindow(theme_settings_hwnd);
        ShowWindow(theme_settings_hwnd, SW_SHOW);
    }
}

/* (Old overlay code removed — themes and round timer above) */
static WNDPROC orig_mmmain_proc = NULL;
static volatile int slop_in_menu = 0;  /* set while our menu is open */

static void slop_update_menu_state(void)
{
    if (!slop_menu) return;
    /* Update round timer toggle text */
    ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                IDM_SLOP_ROUNDTIMER, rt_visible ? "Hide Round Timer" : "Show Round Timer");
}

static LRESULT CALLBACK slop_mmmain_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* Track when menu loop is active — suppress MegaMUD's timers/focus stealing
       that would otherwise dismiss the dropdown after ~1 second */
    if (msg == WM_ENTERMENULOOP) {
        slop_in_menu = 1;
    } else if (msg == WM_EXITMENULOOP) {
        slop_in_menu = 0;
    }
    /* While menu is open, eat WM_TIMER so MegaMUD doesn't steal focus */
    if (slop_in_menu && msg == WM_TIMER) {
        return 0;
    }

    if (msg == WM_COMMAND) {
        WORD id = LOWORD(wParam);
        if (id == IDM_SLOP_ABOUT) {
            slop_show_about(hwnd);
            return 0;
        }
        if (id == IDM_SLOP_THEMES) {
            slop_show_themes(hwnd);
            return 0;
        }
        if (id == IDM_SLOP_ROUNDTIMER) {
            slop_show_round_timer(hwnd);
            slop_update_menu_state();
            return 0;
        }
        if (id == IDM_SLOP_PRO_STEP) {
            pro_every_step = !pro_every_step;
            /* Initialize to current state so we don't fire on toggle */
            if (pro_every_step && struct_base) {
                pro_last_checksum = *(DWORD *)(struct_base + OFF_ROOM_CHECKSUM);
                pro_last_step = *(int *)(struct_base + OFF_CUR_PATH_STEP);
            }
            ModifyMenuA(slop_menu, IDM_SLOP_PRO_STEP, MF_BYCOMMAND | MF_STRING,
                        IDM_SLOP_PRO_STEP,
                        pro_every_step ? "RM Every Step  [ON]" : "RM Every Step  [OFF]");
            cfg_save();
            logmsg("[mudplugin] RM Every Step: %s\n", pro_every_step ? "ON" : "OFF");
            return 0;
        }
        if (id == IDM_SLOP_PLUGINS) {
            show_plugins_dialog(hwnd);
            return 0;
        }
        /* Forward to plugin WndProc handlers */
        if (id >= IDM_PLUGIN_BASE && id < IDM_PLUGIN_BASE + 100) {
            plugins_on_wndproc(hwnd, msg, wParam, lParam);
            return 0;
        }
    }
    /* Theme: handle WM_CTLCOLOR for MMMAIN's own children */
    if (theme_active && theme_panel_brush) {
        switch (msg) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            return (LRESULT)theme_panel_brush;
        }
        case WM_CTLCOLORDLG:
            return (LRESULT)theme_panel_brush;
        }
    }
    return CallWindowProcA(orig_mmmain_proc, hwnd, msg, wParam, lParam);
}

static void inject_menu(HWND main_wnd)
{
    HMENU menu_bar = GetMenu(main_wnd);
    if (!menu_bar) {
        logmsg("[mudplugin] No menu bar found on MMMAIN\n");
        return;
    }

    /* Check if we already injected (look for our menu) */
    int count = GetMenuItemCount(menu_bar);
    for (int i = 0; i < count; i++) {
        char buf[64] = {0};
        GetMenuStringA(menu_bar, i, buf, sizeof(buf), MF_BYPOSITION);
        if (strcmp(buf, "MajorSLOP") == 0) {
            logmsg("[mudplugin] MajorSLOP menu already present\n");
            return;
        }
    }

    /* Create our top-level popup menu */
    slop_menu = CreatePopupMenu();
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_ABOUT, "Proxy Info...");
    AppendMenuA(slop_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_THEMES, "Themes...");
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_ROUNDTIMER, "Show Round Timer");
    AppendMenuA(slop_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_PRO_STEP,
                pro_every_step ? "RM Every Step  [ON]" : "RM Every Step  [OFF]");
    AppendMenuA(slop_menu, MF_SEPARATOR, 0, NULL);
    {
        char plabel[64];
        sprintf(plabel, "Plugins... (%d)", plugin_count);
        AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_PLUGINS, plabel);
    }

    /* Append to menu bar after the last item (right of Help) */
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)slop_menu, "MajorSLOP");
    DrawMenuBar(main_wnd);

    slop_menu_owner = main_wnd;
    slop_update_menu_state();

    /* Subclass to catch our menu commands */
    if (!orig_mmmain_proc) {
        orig_mmmain_proc = (WNDPROC)SetWindowLongA(main_wnd, GWL_WNDPROC, (LONG)slop_mmmain_proc);
    }

    logmsg("[mudplugin] MajorSLOP menu injected to menu bar\n");

    /* Apply initial theme (if one was set in config) */
    theme_update_brushes();
    if (theme_active) theme_apply_all();

    /* Start background thread to theme newly opened dialogs */
    CreateThread(NULL, 0, theme_scanner_thread, NULL, 0, NULL);

    /* Start Pro Every Step watcher thread */
    CreateThread(NULL, 0, pro_step_thread, NULL, 0, NULL);

    /* Start auto struct finder so DLL features work without proxy */
    CreateThread(NULL, 0, auto_struct_thread, NULL, 0, NULL);

    /* Start MMANSI terminal scanner for round detection without proxy */
    CreateThread(NULL, 0, mmansi_scanner_thread, NULL, 0, NULL);

    /* Plugin system — create dir and load plugins */
    ensure_plugins_dir();
    load_plugins();
}

/* ================================================================
 * Winsock recv() hook — intercept MUD traffic for round detection
 * ================================================================
 *
 * IAT-patches ws2_32!recv so we see every byte from the MUD server.
 * We buffer partial lines, strip ANSI escapes, and feed clean lines
 * into the round detection state machine.
 */

/* Original recv function pointer */
typedef int (WSAAPI *recv_fn)(SOCKET s, char *buf, int len, int flags);
static recv_fn real_recv = NULL;

/* Track which socket is the MUD connection (first large recv) */
static SOCKET mud_socket = INVALID_SOCKET;

/* Line buffer for accumulating partial recv data */
#define RECV_LINE_BUF 4096
static char recv_line_buf[RECV_LINE_BUF];
static int recv_line_pos = 0;

/* Round detection state — passive swing-based detection.
 * Every round, swings happen (hits/misses/damage/glances) right at onset.
 * Multiple swings can happen in the same round (multi-hit, party members, etc).
 * We cluster events within a short window — first event in a new cluster = round tick.
 * Rounds are always 5s on ParaMUD, so any gap > ~3s between clusters = new round. */
static int rd_round_num = 0;            /* our own round counter */
static double rd_last_event_ts = 0.0;   /* timestamp of last combat event (for clustering) */
#define RD_CLUSTER_GAP 500.0            /* ms — events within this are same round (burst is ~200ms, next round ~5s away) */

/* Strip ANSI escape sequences from a line in-place.
 * ESC [ ... final_byte  where final_byte is 0x40-0x7E */
static void strip_ansi(char *line)
{
    char *r = line, *w = line;
    while (*r) {
        if (*r == '\x1b') {
            r++;
            if (*r == '[') {
                r++;
                /* Skip parameters and intermediate bytes (0x20-0x3F) */
                while (*r && (unsigned char)*r >= 0x20 && (unsigned char)*r <= 0x3F) r++;
                /* Skip final byte (0x40-0x7E) */
                if (*r && (unsigned char)*r >= 0x40 && (unsigned char)*r <= 0x7E) r++;
            } else {
                /* Non-CSI escape — skip one more char */
                if (*r) r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Check if line starts with a prefix (case-insensitive) */
static int starts_with_i(const char *line, const char *prefix)
{
    while (*prefix) {
        if ((*line | 0x20) != (*prefix | 0x20)) return 0;
        line++; prefix++;
    }
    return 1;
}

/* Get current combat target name from struct memory.
 * Returns pointer to static string, empty if not in combat or no base. */
static const char *get_combat_target(void)
{
    if (!struct_base) return "";
    const char *name = (const char *)(struct_base + OFF_COMBAT_TARGET);
    /* Sanity check — should be printable ASCII */
    if (name[0] < 0x20 || name[0] > 0x7E) return "";
    return name;
}

/* Case-insensitive strstr */
static const char *stristr(const char *haystack, const char *needle)
{
    if (!needle[0]) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && ((*h | 0x20) == (*n | 0x20))) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

/* Check if a line is a combat event (hit, miss, dodge, fumble, etc.)
 * These all fire at round onset. Matches the EXACT same patterns as
 * parser.py: RE_DAMAGE, RE_COMBAT_MISS, RE_PLAYER_SWING, RE_GLANCES_OFF */
static int is_combat_event(const char *line)
{
    int len = (int)strlen(line);
    if (len < 5) return 0;

    /* RE_DAMAGE: "for N damage!" */
    if (strstr(line, " damage!")) return 1;

    /* RE_COMBAT_MISS: miss, dodge, fumble, block, bounce off, parry */
    if (stristr(line, "misses ")) return 1;
    if (stristr(line, "miss ")) return 1;
    if (stristr(line, "dodges ")) return 1;
    if (stristr(line, "dodge ")) return 1;
    if (stristr(line, "fumbles ")) return 1;
    if (stristr(line, "fumble ")) return 1;
    if (stristr(line, "blocks ")) return 1;
    if (stristr(line, "block ")) return 1;
    if (stristr(line, "bounces off")) return 1;
    if (stristr(line, "bounce off")) return 1;
    if (stristr(line, "parries ")) return 1;
    if (stristr(line, "parry ")) return 1;

    /* RE_PLAYER_SWING: "You swing at ...", "You swipe at ..." */
    if (starts_with_i(line, "You ") && strstr(line, " at ")) return 1;

    /* RE_GLANCES_OFF: "Your sword glances off ..." */
    if (starts_with_i(line, "Your ") && strstr(line, " glances off")) return 1;

    return 0;
}

/* Process one clean (ANSI-stripped) line from MUD traffic.
 * Round detection is ONLY based on actual swings (hits/misses/damage).
 * Combat Engaged/Off are IRRELEVANT — they fire 100x per round via macros. */
static void rd_process_line(const char *line)
{
    if (!line[0]) return;

    if (is_combat_event(line)) {
        double now = (double)GetTickCount64();
        /* First event, or >500ms since last cluster = NEW ROUND */
        if (rd_last_event_ts == 0.0 || (now - rd_last_event_ts) > RD_CLUSTER_GAP) {
            rd_round_num++;
            rt_on_round_tick(rd_round_num);
            plugins_on_round(rd_round_num);
        }
        rd_last_event_ts = now;
    }

    /* Notify plugins of every line */
    plugins_on_line(line);
}

/* Feed raw recv data into line buffer, process complete lines */
static void rd_feed_data(const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            /* Complete line — process it */
            recv_line_buf[recv_line_pos] = '\0';
            /* Strip trailing \r */
            if (recv_line_pos > 0 && recv_line_buf[recv_line_pos - 1] == '\r')
                recv_line_buf[recv_line_pos - 1] = '\0';
            /* Strip ANSI */
            strip_ansi(recv_line_buf);
            /* Process */
            rd_process_line(recv_line_buf);
            recv_line_pos = 0;
        } else if (recv_line_pos < RECV_LINE_BUF - 1) {
            recv_line_buf[recv_line_pos++] = c;
        }
        /* else: line too long, silently truncate */
    }
}

/* Track sockets to skip (our own DLL server connections) */
static SOCKET skip_sockets[8];
static int num_skip_sockets = 0;

static void mark_skip_socket(SOCKET s)
{
    if (num_skip_sockets < 8)
        skip_sockets[num_skip_sockets++] = s;
}

static int is_skip_socket(SOCKET s)
{
    for (int i = 0; i < num_skip_sockets; i++)
        if (skip_sockets[i] == s) return 1;
    return 0;
}

/* Hooked recv — intercepts all winsock recv calls in this process */
static int WSAAPI hooked_recv(SOCKET s, char *buf, int len, int flags)
{
    int ret = real_recv(s, buf, len, flags);

    /* Skip our own DLL server's client socket */
    if (is_skip_socket(s)) return ret;

    if (ret <= 0 && s == mud_socket) {
        /* MUD socket closed/errored — reset so we re-detect on reconnect */
        logmsg("[recvhook] MUD socket %d closed (ret=%d)\n", (int)s, ret);
        mud_socket = INVALID_SOCKET;
        recv_line_pos = 0;
        rd_last_event_ts = 0.0;
    }
    if (ret > 0) {
        /* Heuristic: the MUD socket is the one receiving ANSI/text data.
         * If we haven't identified it yet, check for ESC[ or printable text. */
        if (mud_socket == INVALID_SOCKET) {
            /* Look for ANSI escape or substantial text content */
            int text_chars = 0;
            for (int i = 0; i < ret && i < 128; i++) {
                unsigned char c = (unsigned char)buf[i];
                if (c == 0x1b || (c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n')
                    text_chars++;
            }
            /* If >70% looks like text/ANSI, it's probably the MUD socket */
            if (ret >= 8 && text_chars * 100 / ret > 70) {
                mud_socket = s;
                logmsg("[recvhook] MUD socket identified: %d\n", (int)s);
            }
        }
        if (s == mud_socket) {
            rd_feed_data(buf, ret);
        }
    }
    return ret;
}

/* IAT patching: replace recv in the import table of the main EXE module */
static int hook_recv(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;

    /* Get the PE headers */
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    /* Find import directory */
    DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!import_rva) return 0;

    IMAGE_IMPORT_DESCRIPTOR *imports = (IMAGE_IMPORT_DESCRIPTOR *)((char *)exe + import_rva);

    /* Walk import descriptors looking for ws2_32.dll or wsock32.dll */
    for (; imports->Name; imports++) {
        const char *dll_name = (const char *)((char *)exe + imports->Name);
        if (_stricmp(dll_name, "ws2_32.dll") != 0 &&
            _stricmp(dll_name, "wsock32.dll") != 0 &&
            _stricmp(dll_name, "WSOCK32.dll") != 0 &&
            _stricmp(dll_name, "WS2_32.dll") != 0)
            continue;

        logmsg("[recvhook] Found import for %s\n", dll_name);

        /* Walk the IAT (Import Address Table) */
        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)((char *)exe + imports->FirstThunk);
        IMAGE_THUNK_DATA *orig_thunk = NULL;
        if (imports->OriginalFirstThunk)
            orig_thunk = (IMAGE_THUNK_DATA *)((char *)exe + imports->OriginalFirstThunk);

        for (int i = 0; thunk->u1.Function; thunk++, i++) {
            /* Check if this is recv by comparing the function pointer */
            FARPROC *func_ptr = (FARPROC *)&thunk->u1.Function;

            /* Try to identify recv by name from original thunk */
            if (orig_thunk) {
                IMAGE_THUNK_DATA *ot = orig_thunk + i;
                if (!(ot->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME *name_entry =
                        (IMAGE_IMPORT_BY_NAME *)((char *)exe + ot->u1.AddressOfData);
                    if (strcmp((char *)name_entry->Name, "recv") == 0) {
                        /* Found recv! Patch it. */
                        real_recv = (recv_fn)*func_ptr;
                        DWORD old_protect;
                        VirtualProtect(func_ptr, sizeof(FARPROC), PAGE_READWRITE, &old_protect);
                        *func_ptr = (FARPROC)hooked_recv;
                        VirtualProtect(func_ptr, sizeof(FARPROC), old_protect, &old_protect);
                        logmsg("[recvhook] recv() hooked successfully (was 0x%08X -> 0x%08X)\n",
                               (DWORD)real_recv, (DWORD)hooked_recv);
                        return 1;
                    }
                }
            } else {
                /* No original thunk — try matching by address */
                HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
                if (!ws2) ws2 = GetModuleHandleA("wsock32.dll");
                if (ws2) {
                    FARPROC real = GetProcAddress(ws2, "recv");
                    if (real && *func_ptr == real) {
                        real_recv = (recv_fn)*func_ptr;
                        DWORD old_protect;
                        VirtualProtect(func_ptr, sizeof(FARPROC), PAGE_READWRITE, &old_protect);
                        *func_ptr = (FARPROC)hooked_recv;
                        VirtualProtect(func_ptr, sizeof(FARPROC), old_protect, &old_protect);
                        logmsg("[recvhook] recv() hooked by address match\n");
                        return 1;
                    }
                }
            }
        }
    }

    /* Fallback: MegaMUD might not import recv directly from ws2_32.
     * Try hooking via the loaded ws2_32 module's IAT instead,
     * or just get the address and scan all thunks. */
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) ws2 = LoadLibraryA("ws2_32.dll");
    if (ws2) {
        FARPROC target = GetProcAddress(ws2, "recv");
        if (target) {
            /* Scan ALL import descriptors for any thunk pointing to recv */
            IMAGE_IMPORT_DESCRIPTOR *imp2 = (IMAGE_IMPORT_DESCRIPTOR *)((char *)exe + import_rva);
            for (; imp2->Name; imp2++) {
                IMAGE_THUNK_DATA *th = (IMAGE_THUNK_DATA *)((char *)exe + imp2->FirstThunk);
                for (; th->u1.Function; th++) {
                    if ((FARPROC)th->u1.Function == target) {
                        real_recv = (recv_fn)target;
                        FARPROC *fp = (FARPROC *)&th->u1.Function;
                        DWORD old_protect;
                        VirtualProtect(fp, sizeof(FARPROC), PAGE_READWRITE, &old_protect);
                        *fp = (FARPROC)hooked_recv;
                        VirtualProtect(fp, sizeof(FARPROC), old_protect, &old_protect);
                        logmsg("[recvhook] recv() hooked via fallback scan\n");
                        return 1;
                    }
                }
            }
        }
    }

    logmsg("[recvhook] FAILED to hook recv()\n");
    return 0;
}

/* ---- Client handler ---- */

static void handle_client(SOCKET client)
{
    char buf[CMD_BUF];
    int bufpos = 0;

    proxy_connected = 1;
    mark_skip_socket(client);  /* Don't parse our own control traffic */
    logmsg("[mudplugin] Client connected, struct_base=0x%08X\n", struct_base);

    /* Send banner with current state */
    char banner[128];
    sprintf(banner, "MUDPLUGIN v2.0 BASE=0x%08X\n", struct_base);
    send(client, banner, (int)strlen(banner), 0);

    while (1) {
        int n = recv(client, buf + bufpos, CMD_BUF - bufpos - 1, 0);
        if (n <= 0) break;
        bufpos += n;
        buf[bufpos] = '\0';

        char *line_start = buf;
        char *newline;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            if (newline > line_start && *(newline-1) == '\r')
                *(newline-1) = '\0';

            char resp[CMD_BUF];
            resp[0] = '\0';

            if (strncmp(line_start, "PING", 4) == 0) {
                strcpy(resp, "PONG\n");

            } else if (strncmp(line_start, "FIND ", 5) == 0) {
                char *name = line_start + 5;
                while (*name == ' ') name++;
                struct_base = find_struct(name);
                if (struct_base) {
                    sprintf(resp, "OK 0x%08X\n", struct_base);
                    /* Save player name so auto-find works without proxy */
                    strncpy(cfg_player_name, name, sizeof(cfg_player_name) - 1);
                    cfg_player_name[sizeof(cfg_player_name) - 1] = '\0';
                    cfg_save();
                } else
                    strcpy(resp, "ERR struct not found\n");

            } else if (strncmp(line_start, "SETBASE ", 8) == 0) {
                struct_base = (DWORD)strtoul(line_start + 8, NULL, 16);
                logmsg("[mudplugin] Base set to 0x%08X\n", struct_base);
                sprintf(resp, "OK 0x%08X\n", struct_base);

            } else if (strncmp(line_start, "BASE", 4) == 0) {
                sprintf(resp, "0x%08X\n", struct_base);

            } else if (strncmp(line_start, "LOCATION", 8) == 0) {
                /* Return current map,room from rm parsing */
                sprintf(resp, "%d,%d\n", loc_map, loc_room);

            } else if (strncmp(line_start, "SETLOC ", 7) == 0) {
                /* Allow proxy to set location: SETLOC map room */
                sscanf(line_start + 7, "%d %d", &loc_map, &loc_room);
                update_statusbar_location();
                sprintf(resp, "OK %d,%d\n", loc_map, loc_room);

            } else if (strncmp(line_start, "SCANLOC", 7) == 0) {
                /* Force scan MMANSI for Location line */
                if (scan_mmansi_for_location()) {
                    update_statusbar_location();
                    sprintf(resp, "OK %d,%d\n", loc_map, loc_room);
                } else {
                    strcpy(resp, "ERR not found\n");
                }

            } else if (strncmp(line_start, "SETPORTS ", 9) == 0) {
                /* SETPORTS <proxy_port> <web_port> */
                char *p = line_start + 9;
                proxy_port = atoi(p);
                while (*p && *p != ' ') p++;
                while (*p == ' ') p++;
                if (*p) web_port = atoi(p);
                sprintf(resp, "OK %d %d\n", proxy_port, web_port);

            } else if (strncmp(line_start, "READABS ", 8) == 0) {
                char *p = line_start + 8;
                DWORD addr = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                int size = atoi(p);
                if (size <= 0) size = 4;
                if (size > (CMD_BUF / 2) - 2) size = (CMD_BUF / 2) - 2;
                unsigned char *src = (unsigned char *)addr;
                char *rp = resp;
                for (int i = 0; i < size; i++) {
                    sprintf(rp, "%02X", src[i]);
                    rp += 2;
                }
                *rp++ = '\n';
                *rp = '\0';

            } else if (strncmp(line_start, "ENUMWIN", 7) == 0) {
                /* Enumerate ALL windows belonging to this process.
                 * Includes MMMAIN children AND top-level dialogs (Player Stats, etc) */
                HWND mw = FindWindowA("MMMAIN", NULL);
                if (!mw) {
                    strcpy(resp, "ERR no MMMAIN\n");
                } else {
                    char hdr[128];
                    sprintf(hdr, "ENUMWIN MMMAIN=0x%08X\n", (DWORD)mw);
                    send(client, hdr, (int)strlen(hdr), 0);

                    /* Enumerate MMMAIN children (recursive) */
                    HWND stack[256];
                    int sp = 0;
                    HWND child = GetWindow(mw, GW_CHILD);
                    while (child) {
                        if (sp < 256) stack[sp++] = child;
                        child = GetWindow(child, GW_HWNDNEXT);
                    }
                    while (sp > 0) {
                        HWND h = stack[--sp];
                        char cls[128] = {0};
                        char txt[256] = {0};
                        GetClassNameA(h, cls, sizeof(cls));
                        GetWindowTextA(h, txt, sizeof(txt));
                        RECT r;
                        GetWindowRect(h, &r);
                        int w = r.right - r.left, ht = r.bottom - r.top;
                        LONG style = GetWindowLongA(h, GWL_STYLE);
                        LONG exstyle = GetWindowLongA(h, GWL_EXSTYLE);
                        int ctrlid = GetDlgCtrlID(h);
                        int visible = IsWindowVisible(h);
                        char line[512];
                        sprintf(line, "  HWND=0x%08X class=%-24s id=%-5d %4dx%-4d style=0x%08X ex=0x%08X vis=%d text=\"%.60s\"\n",
                                (DWORD)h, cls, ctrlid, w, ht, style, exstyle, visible, txt);
                        send(client, line, (int)strlen(line), 0);
                        HWND gc = GetWindow(h, GW_CHILD);
                        while (gc) {
                            if (sp < 256) stack[sp++] = gc;
                            gc = GetWindow(gc, GW_HWNDNEXT);
                        }
                    }
                    strcpy(resp, "END\n");
                }

            } else if (struct_base == 0) {
                strcpy(resp, "ERR no base - send FIND <playername> first\n");

            } else if (strncmp(line_start, "READ32 ", 7) == 0) {
                DWORD offset = (DWORD)strtoul(line_start + 7, NULL, 16);
                DWORD *ptr = (DWORD *)(struct_base + offset);
                sprintf(resp, "%u\n", *ptr);

            } else if (strncmp(line_start, "WRITE32 ", 8) == 0) {
                char *p = line_start + 8;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                DWORD val = (DWORD)strtoul(p, NULL, 10);
                DWORD *ptr = (DWORD *)(struct_base + offset);
                *ptr = val;
                strcpy(resp, "OK\n");

            } else if (strncmp(line_start, "READS ", 6) == 0) {
                char *p = line_start + 6;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                int maxlen = atoi(p);
                if (maxlen <= 0) maxlen = 256;
                if (maxlen > CMD_BUF - 2) maxlen = CMD_BUF - 2;
                char *src = (char *)(struct_base + offset);
                int slen = (int)strnlen(src, maxlen);
                memcpy(resp, src, slen);
                resp[slen] = '\n';
                resp[slen+1] = '\0';

            } else if (strncmp(line_start, "WRITES ", 7) == 0) {
                char *p = line_start + 7;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                char *dst = (char *)(struct_base + offset);
                strcpy(dst, p);
                strcpy(resp, "OK\n");

            } else if (strncmp(line_start, "READ ", 5) == 0) {
                char *p = line_start + 5;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                int size = atoi(p);
                if (size <= 0) size = 4;
                if (size > (CMD_BUF / 2) - 2) size = (CMD_BUF / 2) - 2;
                unsigned char *src = (unsigned char *)(struct_base + offset);
                char *rp = resp;
                for (int i = 0; i < size; i++) {
                    sprintf(rp, "%02X", src[i]);
                    rp += 2;
                }
                *rp++ = '\n';
                *rp = '\0';

            } else if (strncmp(line_start, "WRITE ", 6) == 0) {
                char *p = line_start + 6;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                unsigned char bytes[CMD_BUF/2];
                int nbytes = parse_hex_bytes(p, bytes, sizeof(bytes));
                unsigned char *dst = (unsigned char *)(struct_base + offset);
                memcpy(dst, bytes, nbytes);
                sprintf(resp, "OK %d\n", nbytes);

            } else if (strncmp(line_start, "STATUS", 6) == 0) {
                /* Read MegaMUD's status bar (msctls_statusbar32, ctrl_id=107).
                 * Returns all 8 parts tab-separated on one line. */
                HWND main_wnd = FindWindowA("MMMAIN", NULL);
                if (!main_wnd) {
                    strcpy(resp, "ERR no MMMAIN window\n");
                } else {
                    HWND sb = GetDlgItem(main_wnd, MEGAMUD_STATUSBAR_ID);
                    if (!sb) {
                        strcpy(resp, "ERR no statusbar control\n");
                    } else {
                        char *rp = resp;
                        int parts = (int)SendMessageA(sb, SB_GETPARTS, 0, 0);
                        if (parts <= 0) parts = 8;
                        if (parts > 8) parts = 8;
                        for (int i = 0; i < parts; i++) {
                            char part_buf[256] = {0};
                            SendMessageA(sb, SB_GETTEXTA, (WPARAM)i, (LPARAM)part_buf);
                            part_buf[255] = '\0';
                            if (i > 0) *rp++ = '\t';
                            int plen = (int)strlen(part_buf);
                            memcpy(rp, part_buf, plen);
                            rp += plen;
                        }
                        *rp++ = '\n';
                        *rp = '\0';
                    }
                }

            } else if (strncmp(line_start, "DLGTEXT ", 8) == 0) {
                /* Read text from a control in a dialog/window.
                 * DLGTEXT <parent_class_or_title> <ctrl_id>
                 * Finds the parent window, gets child by ctrl_id, reads text.
                 * Used for Player Statistics dialog controls. */
                char *p = line_start + 8;
                /* Parse parent: could be class name like "#32770" or window title */
                char parent_class[128] = {0};
                int pi = 0;
                while (*p && *p != ' ' && pi < 126) parent_class[pi++] = *p++;
                parent_class[pi] = '\0';
                while (*p == ' ') p++;
                int ctrl_id = atoi(p);

                /* Find the parent window */
                HWND parent = NULL;
                if (parent_class[0] == '#') {
                    /* Class name like #32770 — enumerate to find MegaMUD's dialog */
                    HWND candidate = NULL;
                    DWORD our_pid = GetCurrentProcessId();
                    while ((candidate = FindWindowExA(NULL, candidate, parent_class, NULL)) != NULL) {
                        DWORD win_pid = 0;
                        GetWindowThreadProcessId(candidate, &win_pid);
                        if (win_pid == our_pid) { parent = candidate; break; }
                    }
                } else {
                    parent = FindWindowA(parent_class, NULL);
                }

                if (!parent) {
                    strcpy(resp, "ERR parent window not found\n");
                } else {
                    HWND ctrl = GetDlgItem(parent, ctrl_id);
                    if (!ctrl) {
                        sprintf(resp, "ERR control %d not found\n", ctrl_id);
                    } else {
                        char text_buf[512] = {0};
                        GetWindowTextA(ctrl, text_buf, 511);
                        sprintf(resp, "%s\n", text_buf);
                    }
                }

            } else if (strncmp(line_start, "CLICK ", 6) == 0) {
                /* Send BM_CLICK to a button in a dialog.
                 * CLICK <parent_class> <ctrl_id>
                 * Used for resetting exp meter (ctrl 1721). */
                char *p = line_start + 6;
                char parent_class[128] = {0};
                int pi = 0;
                while (*p && *p != ' ' && pi < 126) parent_class[pi++] = *p++;
                parent_class[pi] = '\0';
                while (*p == ' ') p++;
                int ctrl_id = atoi(p);

                HWND parent = NULL;
                if (parent_class[0] == '#') {
                    HWND candidate = NULL;
                    DWORD our_pid = GetCurrentProcessId();
                    while ((candidate = FindWindowExA(NULL, candidate, parent_class, NULL)) != NULL) {
                        DWORD win_pid = 0;
                        GetWindowThreadProcessId(candidate, &win_pid);
                        if (win_pid == our_pid) { parent = candidate; break; }
                    }
                } else {
                    parent = FindWindowA(parent_class, NULL);
                }

                if (!parent) {
                    strcpy(resp, "ERR parent window not found\n");
                } else {
                    HWND ctrl = GetDlgItem(parent, ctrl_id);
                    if (!ctrl) {
                        sprintf(resp, "ERR control %d not found\n", ctrl_id);
                    } else {
                        SendMessageA(ctrl, BM_CLICK, 0, 0);
                        strcpy(resp, "OK\n");
                    }
                }

            } else if (strncmp(line_start, "SETTHEME ", 9) == 0) {
                /* SETTHEME <index_or_name>
                 * Set theme by index (0-15), name prefix, or "none" */
                char *arg = line_start + 9;
                if (_stricmp(arg, "none") == 0) {
                    theme_idx = -1;
                    theme_apply_all();
                    cfg_save();
                    strcpy(resp, "OK theme=none\n");
                } else {
                int idx = atoi(arg);
                /* Try numeric index first */
                if (idx >= 0 && idx < (int)SLOP_THEME_COUNT && (arg[0] >= '0' && arg[0] <= '9')) {
                    theme_idx = idx;
                    theme_apply_all();
                    cfg_save();
                    sprintf(resp, "OK theme=%s\n", SLOP_THEMES[idx].name);
                } else {
                    /* Match by name prefix (case-insensitive) */
                    int found = -1;
                    int alen = (int)strlen(arg);
                    for (int i = 0; i < (int)SLOP_THEME_COUNT; i++) {
                        if (_strnicmp(arg, SLOP_THEMES[i].name, alen) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) {
                        theme_idx = found;
                        theme_apply_all();
                        cfg_save();
                        sprintf(resp, "OK theme=%s\n", SLOP_THEMES[found].name);
                    } else {
                        strcpy(resp, "ERR unknown theme\n");
                    }
                }
                } /* close "none" else */

            } else if (strncmp(line_start, "ROUNDTICK ", 10) == 0) {
                /* ROUNDTICK <round_num>
                 * Proxy sends this on each detected combat round */
                int rnum = atoi(line_start + 10);
                rt_on_round_tick(rnum);
                sprintf(resp, "OK round=%d\n", rnum);

            } else if (strncmp(line_start, "SNAP", 4) == 0) {
                /* SNAP <hex_offset> <size>
                 * Take a memory snapshot. Next SNAP with same args shows diffs. */
                static unsigned char snap_buf[8192];
                static DWORD snap_off = 0, snap_size = 0;
                static int snap_taken = 0;

                char *p = line_start + 5;
                DWORD off = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                DWORD sz = (DWORD)strtoul(p, NULL, 10);
                if (sz > sizeof(snap_buf)) sz = sizeof(snap_buf);
                if (sz == 0) sz = 256;

                unsigned char *src = (unsigned char *)(struct_base + off);

                if (!snap_taken || off != snap_off || sz != snap_size) {
                    /* First snapshot */
                    memcpy(snap_buf, src, sz);
                    snap_off = off;
                    snap_size = sz;
                    snap_taken = 1;
                    sprintf(resp, "OK snapshot %u bytes at +0x%X\n", sz, off);
                } else {
                    /* Diff against snapshot */
                    char hdr[64];
                    sprintf(hdr, "DIFF +0x%X (%u bytes)\n", off, sz);
                    send(client, hdr, (int)strlen(hdr), 0);
                    int diffs = 0;
                    for (DWORD i = 0; i < sz; i += 4) {
                        DWORD old_val = *(DWORD *)(snap_buf + i);
                        DWORD new_val = *(DWORD *)(src + i);
                        if (old_val != new_val) {
                            char line[128];
                            /* Show as both unsigned and signed */
                            int old_s = (int)old_val, new_s = (int)new_val;
                            sprintf(line, "  +0x%04X: %u -> %u (0x%08X -> 0x%08X) [i32: %d -> %d]\n",
                                    off + i, old_val, new_val, old_val, new_val, old_s, new_s);
                            send(client, line, (int)strlen(line), 0);
                            diffs++;
                        }
                    }
                    if (diffs == 0) {
                        send(client, "  (no changes)\n", 15, 0);
                    }
                    /* Update snapshot */
                    memcpy(snap_buf, src, sz);
                    strcpy(resp, "END\n");
                }

            } else if (strncmp(line_start, "COMBATEND", 9) == 0) {
                rt_on_combat_end();
                strcpy(resp, "OK\n");

            } else {
                strcpy(resp, "ERR unknown command\n");
            }

            if (resp[0])
                send(client, resp, (int)strlen(resp), 0);

            line_start = newline + 1;
        }

        if (line_start > buf) {
            int remaining = bufpos - (int)(line_start - buf);
            if (remaining > 0)
                memmove(buf, line_start, remaining);
            bufpos = remaining;
        }
    }

    proxy_connected = 0;
    logmsg("[mudplugin] Client disconnected\n");
    closesocket(client);
}

/* ---- Theme scanner thread (catches newly opened dialogs) ---- */

static DWORD WINAPI theme_scanner_thread(LPVOID param)
{
    (void)param;
    while (1) {
        Sleep(2000);
        if (theme_active) {
            theme_apply_all();
        }
    }
    return 0;
}

/* ---- MMANSI terminal buffer reader & Location parser ---- */

/* loc_map and loc_room forward-declared above */

/* ---- MMANSI terminal scanner for round detection ---- */
/* Polls the MMANSI text buffer for new lines, feeds them to rd_process_line.
 * This replaces the failed recv() hook — reads directly from terminal memory.
 *
 * The terminal scrolls up when new lines arrive, so ALL rows change position.
 * We track the bottom-most line's content. When it changes, we find where the
 * old bottom line moved to (scrolled up), and process only the lines below it
 * — those are the genuinely new lines. */

/* Simple hash for a string */
static unsigned int line_hash(const char *s)
{
    unsigned int h = 5381;
    while (*s) { h = h * 33 + (unsigned char)*s; s++; }
    return h;
}

#define SEEN_SIZE 256
static unsigned int seen_hashes[SEEN_SIZE];
static int seen_head = 0;
static int seen_count = 0;

static int was_seen(unsigned int h)
{
    for (int i = 0; i < seen_count; i++) {
        if (seen_hashes[i] == h) return 1;
    }
    return 0;
}

static void mark_seen(unsigned int h)
{
    if (seen_count < SEEN_SIZE) {
        seen_hashes[seen_count++] = h;
    } else {
        seen_hashes[seen_head] = h;
        seen_head = (seen_head + 1) % SEEN_SIZE;
    }
}

static DWORD WINAPI mmansi_scanner_thread(LPVOID param)
{
    (void)param;
    int initialized = 0;
    char prev_rows[MMANSI_MAX_ROWS][140];
    memset(prev_rows, 0, sizeof(prev_rows));

    logmsg("[mmansi_scan] Terminal scanner started\n");

    while (1) {
        Sleep(80);

        HWND mw = FindWindowA("MMMAIN", NULL);
        if (!mw) continue;
        HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
        if (!ansi) continue;
        LONG wdata = GetWindowLongA(ansi, 4);
        if (!wdata) continue;

        char rows[MMANSI_MAX_ROWS][140];
        for (int r = 0; r < MMANSI_MAX_ROWS; r++) {
            mmansi_read_row(ansi, r, rows[r], 140);
        }

        if (!initialized) {
            /* Seed the seen set with everything currently on screen */
            for (int r = 0; r < MMANSI_MAX_ROWS; r++) {
                if (rows[r][0]) mark_seen(line_hash(rows[r]));
            }
            memcpy(prev_rows, rows, sizeof(prev_rows));
            initialized = 1;
            logmsg("[mmansi_scan] Initialized with %d lines\n", seen_count);
            continue;
        }

        /* Check every row. If content changed AND we haven't seen this
         * line content before, it's genuinely new — process it. */
        for (int r = 0; r < MMANSI_MAX_ROWS; r++) {
            if (!rows[r][0]) continue;
            if (strcmp(rows[r], prev_rows[r]) == 0) continue;
            unsigned int h = line_hash(rows[r]);
            if (was_seen(h)) continue;
            mark_seen(h);
            rd_process_line(rows[r]);
        }

        memcpy(prev_rows, rows, sizeof(prev_rows));
    }
    return 0;
}

/* Read a row from MMANSI's text buffer. Returns trimmed length. */
static int mmansi_read_row(HWND mmansi, int row, char *out, int out_sz)
{
    LONG wdata = GetWindowLongA(mmansi, 4);
    if (!wdata) return 0;
    int offset = MMANSI_TEXT_OFF + row * MMANSI_ROW_STRIDE;
    const char *src = (const char *)(wdata + offset);
    int max = (out_sz - 1 < MMANSI_ROW_STRIDE) ? out_sz - 1 : MMANSI_ROW_STRIDE;
    memcpy(out, src, max);
    out[max] = '\0';
    /* Trim trailing spaces */
    for (int i = max - 1; i >= 0 && out[i] == ' '; i--)
        out[i] = '\0';
    return (int)strlen(out);
}

/* Scan MMANSI buffer for most recent "Location:" line, extract map,room */
static int scan_mmansi_for_location(void)
{
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return 0;
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    if (!ansi) return 0;

    char row_buf[140];
    /* Scan from bottom up to find the most recent Location: line */
    for (int r = MMANSI_MAX_ROWS - 1; r >= 0; r--) {
        int len = mmansi_read_row(ansi, r, row_buf, sizeof(row_buf));
        if (len < 10) continue;
        /* Look for "Location:" */
        char *loc = strstr(row_buf, "Location:");
        if (!loc) continue;
        /* Parse "Location:            X,Y" */
        char *p = loc + 9;
        while (*p == ' ') p++;
        int map = 0, room = 0;
        if (sscanf(p, "%d,%d", &map, &room) == 2) {
            loc_map = map;
            loc_room = room;
            return 1;
        }
    }
    return 0;
}

/* Update MegaMUD's status bar — append Map/Room to the path name (part 0) */
static void update_statusbar_location(void)
{
    if (loc_map == 0 && loc_room == 0) return;

    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return;
    HWND sb = GetDlgItem(mw, MEGAMUD_STATUSBAR_ID);
    if (!sb) return;

    /* Read current path name from part 0 */
    char part0[256] = {0};
    SendMessageA(sb, SB_GETTEXTA, 0, (LPARAM)part0);
    part0[255] = '\0';

    /* Strip any existing " | Map:" suffix we may have added before */
    char *existing = strstr(part0, " | Map:");
    if (existing) *existing = '\0';

    /* Build new text: "MUSTFULL | Map:1 Room:9472" */
    char text[256];
    sprintf(text, "%s | Map:%d Room:%d", part0, loc_map, loc_room);
    SendMessageA(sb, SB_SETTEXTA, 0, (LPARAM)text);
}

/* ---- Pro Every Step thread ---- */
/* Watches ROOM_CHECKSUM for changes and types "pro<Enter>" into MegaMUD's
 * MMANSI terminal. We're in-process, so PostMessage works directly. */

/* Type a string + Enter into MegaMUD's terminal */
static void type_into_megamud(const char *cmd)
{
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return;

    /* Find the MMANSI child (the terminal where you type commands) */
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;

    /* Type each character */
    for (int i = 0; cmd[i]; i++) {
        PostMessageA(target, WM_CHAR, (WPARAM)(unsigned char)cmd[i], 0);
    }
    /* Press Enter */
    PostMessageA(target, WM_CHAR, (WPARAM)'\r', 0);
}

static DWORD WINAPI pro_step_thread(LPVOID param)
{
    (void)param;
    logmsg("[prostep] Thread started\n");

    while (1) {
        Sleep(150);  /* Poll ~7x/sec */

        if (!pro_every_step) continue;
        if (!struct_base) continue;

        int step = *(int *)(struct_base + OFF_CUR_PATH_STEP);

        /* Initialize on first read */
        if (pro_last_step == 0) {
            pro_last_step = step;
            continue;
        }

        /* Path step changed — type rm */
        if (step != pro_last_step && step > 0) {
            logmsg("[prostep] Step %d->%d\n", pro_last_step, step);
            pro_last_step = step;
            type_into_megamud("rm");
            /* Wait for rm output to appear in terminal, then parse Location */
            Sleep(500);
            if (scan_mmansi_for_location()) {
                update_statusbar_location();
                logmsg("[prostep] Location: Map %d, Room %d\n", loc_map, loc_room);
            }
        } else {
            pro_last_step = step;
        }
    }
    return 0;
}

/* ---- Auto struct finder thread ---- */
/* Periodically tries to find struct_base using cfg_player_name so DLL
   features work without the proxy ever connecting. */
static DWORD WINAPI auto_struct_thread(LPVOID param)
{
    (void)param;
    while (1) {
        Sleep(3000);
        if (struct_base) continue;  /* already found */
        if (!cfg_player_name[0]) continue;  /* no name configured */
        DWORD base = find_struct(cfg_player_name);
        if (base) {
            struct_base = base;
            logmsg("[mudplugin] Auto-found struct at 0x%08lX for '%s'\n", base, cfg_player_name);
        }
    }
    return 0;
}

/* ---- TCP Server ---- */

static DWORD WINAPI payload_main(LPVOID param)
{
    Sleep(2000);

    logfile = fopen("mudplugin.log", "a");
    logmsg("[mudplugin] DLL loaded into PID %lu\n", GetCurrentProcessId());
    logmsg("[mudplugin] Module base: 0x%08X\n", (DWORD)GetModuleHandle(NULL));

    /* Init plasma lookup tables */
    slop_init_sin();

    /* Load config before anything else */
    cfg_load();

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logmsg("[mudplugin] WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    /* Hook winsock recv() for MUD traffic parsing (round detection) */
    if (hook_recv()) {
        logmsg("[mudplugin] recv() hook installed — round detection active\n");
    } else {
        logmsg("[mudplugin] recv() hook FAILED — round detection will need proxy\n");
    }

    /* Inject menu into MegaMUD */
    HWND main_wnd = FindWindowA("MMMAIN", NULL);
    if (main_wnd) {
        inject_menu(main_wnd);
    } else {
        logmsg("[mudplugin] MMMAIN not found yet, will retry...\n");
        for (int i = 0; i < 10; i++) {
            Sleep(1000);
            main_wnd = FindWindowA("MMMAIN", NULL);
            if (main_wnd) {
                inject_menu(main_wnd);
                break;
            }
        }
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        logmsg("[mudplugin] socket() failed: %d\n", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* Try ports 9901-9910 so multiple MegaMUD instances can coexist */
    int bound = 0;
    for (int port = PLUGIN_PORT; port < PLUGIN_PORT + 10; port++) {
        addr.sin_port = htons(port);
        if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
            dll_port = port;
            bound = 1;
            break;
        }
        logmsg("[mudplugin] Port %d in use, trying next...\n", port);
    }
    if (!bound) {
        logmsg("[mudplugin] bind() failed on all ports 9901-9910\n");
        closesocket(srv);
        return 1;
    }

    if (listen(srv, 2) == SOCKET_ERROR) {
        logmsg("[mudplugin] listen() failed: %d\n", WSAGetLastError());
        closesocket(srv);
        return 1;
    }

    logmsg("[mudplugin] TCP server listening on 127.0.0.1:%d\n", dll_port);

    while (1) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client = accept(srv, (struct sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) {
            logmsg("[mudplugin] accept() failed: %d\n", WSAGetLastError());
            continue;
        }
        handle_client(client);
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}

/* ================================================================
 * Plugin System
 * ================================================================ */

#define SLOP_MAX_PLUGINS 16

typedef struct {
    HMODULE            dll;
    slop_plugin_t     *desc;
    char               path[MAX_PATH];
    int                loaded;
} loaded_plugin_t;

static loaded_plugin_t plugins[SLOP_MAX_PLUGINS];
static int plugin_count = 0;

/* Callbacks registered by plugins */
static void (*plugin_round_cbs[SLOP_MAX_PLUGINS])(int round_num);
static int plugin_round_cb_count = 0;
static void (*plugin_line_cbs[SLOP_MAX_PLUGINS])(const char *line);
static int plugin_line_cb_count = 0;

/* ---- API wrapper functions ---- */

static HWND api_get_mmansi_hwnd(void)
{
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return NULL;
    return FindWindowExA(mw, NULL, "MMANSI", NULL);
}

static HWND api_get_mmmain_hwnd(void)
{
    return FindWindowA("MMMAIN", NULL);
}

static int api_read_terminal_row(int row, char *out, int out_sz)
{
    HWND mmansi = api_get_mmansi_hwnd();
    if (!mmansi) return 0;
    return mmansi_read_row(mmansi, row, out, out_sz);
}

static unsigned int api_get_struct_base(void)
{
    return (unsigned int)struct_base;
}

static int api_read_struct_i32(unsigned int offset)
{
    if (!struct_base) return 0;
    return *(int *)(struct_base + offset);
}

static int api_read_struct_i16(unsigned int offset)
{
    if (!struct_base) return 0;
    return *(short *)(struct_base + offset);
}

static void api_inject_text(const char *text)
{
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return;
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;
    for (int i = 0; text[i]; i++)
        PostMessageA(target, WM_CHAR, (WPARAM)(unsigned char)text[i], 0);
}

static void api_inject_command(const char *cmd)
{
    api_inject_text(cmd);
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return;
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;
    PostMessageA(target, WM_CHAR, (WPARAM)'\r', 0);
}

static int api_add_menu_item(const char *label, unsigned int id)
{
    if (!slop_menu) return 0;
    AppendMenuA(slop_menu, MF_STRING, id, label);
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (mw) DrawMenuBar(mw);
    return (int)id;
}

static int api_add_menu_separator(void)
{
    if (!slop_menu) return 0;
    AppendMenuA(slop_menu, MF_SEPARATOR, 0, NULL);
    return 1;
}

static void api_on_round_tick(void (*callback)(int round_num))
{
    if (callback && plugin_round_cb_count < SLOP_MAX_PLUGINS) {
        plugin_round_cbs[plugin_round_cb_count++] = callback;
    }
}

static void api_on_terminal_line(void (*callback)(const char *line))
{
    if (callback && plugin_line_cb_count < SLOP_MAX_PLUGINS) {
        plugin_line_cbs[plugin_line_cb_count++] = callback;
    }
}

/* The API struct that gets passed to every plugin */
static slop_api_t slop_api = {0};

static void slop_api_init(void)
{
    slop_api.api_version       = SLOP_API_VERSION;
    slop_api.log               = logmsg;
    slop_api.get_mmansi_hwnd   = api_get_mmansi_hwnd;
    slop_api.read_terminal_row = api_read_terminal_row;
    slop_api.terminal_rows     = MMANSI_MAX_ROWS;
    slop_api.terminal_cols     = MMANSI_ROW_STRIDE;
    slop_api.get_mmmain_hwnd   = api_get_mmmain_hwnd;
    slop_api.get_struct_base   = api_get_struct_base;
    slop_api.read_struct_i32   = api_read_struct_i32;
    slop_api.read_struct_i16   = api_read_struct_i16;
    slop_api.inject_text       = api_inject_text;
    slop_api.inject_command    = api_inject_command;
    slop_api.add_menu_item     = api_add_menu_item;
    slop_api.add_menu_separator = api_add_menu_separator;
    slop_api.on_round_tick     = api_on_round_tick;
    slop_api.on_terminal_line  = api_on_terminal_line;
}

/* Notify all plugins + registered callbacks of a terminal line */
static void plugins_on_line(const char *line)
{
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].loaded && plugins[i].desc->on_line)
            plugins[i].desc->on_line(line);
    }
    for (int i = 0; i < plugin_line_cb_count; i++) {
        if (plugin_line_cbs[i]) plugin_line_cbs[i](line);
    }
}

/* Notify all plugins + registered callbacks of a round tick */
static void plugins_on_round(int round_num)
{
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].loaded && plugins[i].desc->on_round)
            plugins[i].desc->on_round(round_num);
    }
    for (int i = 0; i < plugin_round_cb_count; i++) {
        if (plugin_round_cbs[i]) plugin_round_cbs[i](round_num);
    }
}

/* Pass WndProc messages to plugins — return non-zero if any plugin handled it */
static int plugins_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].loaded && plugins[i].desc->on_wndproc) {
            if (plugins[i].desc->on_wndproc(hwnd, msg, wParam, lParam))
                return 1;
        }
    }
    return 0;
}

/* Get MegaMUD install directory (where the DLL lives) */
static void get_megamud_dir(char *out, int maxlen)
{
    GetModuleFileNameA(NULL, out, maxlen);
    char *slash = strrchr(out, '\\');
    if (slash) *(slash + 1) = '\0';
    else out[0] = '\0';
}

/* Create plugins directory if it doesn't exist */
static void ensure_plugins_dir(void)
{
    char dir[MAX_PATH];
    get_megamud_dir(dir, MAX_PATH);
    strcat(dir, "plugins");
    CreateDirectoryA(dir, NULL);  /* no-op if already exists */
    logmsg("[plugins] Ensured plugins directory: %s\n", dir);
}

/* Load all plugin DLLs from plugins/ directory */
static void load_plugins(void)
{
    char search[MAX_PATH];
    get_megamud_dir(search, MAX_PATH);
    strcat(search, "plugins\\*.dll");

    slop_api_init();

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logmsg("[plugins] No plugins found in plugins/\n");
        return;
    }

    do {
        if (plugin_count >= SLOP_MAX_PLUGINS) {
            logmsg("[plugins] Max plugins (%d) reached, skipping %s\n",
                   SLOP_MAX_PLUGINS, fd.cFileName);
            break;
        }

        char full_path[MAX_PATH];
        get_megamud_dir(full_path, MAX_PATH);
        strcat(full_path, "plugins\\");
        strcat(full_path, fd.cFileName);

        logmsg("[plugins] Loading %s...\n", fd.cFileName);

        HMODULE dll = LoadLibraryA(full_path);
        if (!dll) {
            logmsg("[plugins] FAILED to load %s (error %lu)\n", fd.cFileName, GetLastError());
            continue;
        }

        /* Look for the required export */
        slop_get_plugin_fn get_plugin = (slop_get_plugin_fn)GetProcAddress(dll, "slop_get_plugin");
        if (!get_plugin) {
            logmsg("[plugins] %s has no slop_get_plugin export — not a MajorSLOP plugin\n", fd.cFileName);
            FreeLibrary(dll);
            continue;
        }

        slop_plugin_t *desc = get_plugin();
        if (!desc) {
            logmsg("[plugins] %s: slop_get_plugin() returned NULL\n", fd.cFileName);
            FreeLibrary(dll);
            continue;
        }

        /* Validate magic */
        if (desc->magic != SLOP_PLUGIN_MAGIC) {
            logmsg("[plugins] %s: bad magic 0x%08X (expected 0x%08X) — not a MajorSLOP plugin\n",
                   fd.cFileName, desc->magic, SLOP_PLUGIN_MAGIC);
            FreeLibrary(dll);
            continue;
        }

        /* Validate API version */
        if (desc->api_version != SLOP_API_VERSION) {
            logmsg("[plugins] %s: API version %u (expected %u) — incompatible\n",
                   fd.cFileName, desc->api_version, SLOP_API_VERSION);
            FreeLibrary(dll);
            continue;
        }

        /* Call init */
        if (desc->init) {
            int ret = desc->init(&slop_api);
            if (ret != 0) {
                logmsg("[plugins] %s: init() returned %d — plugin refused to load\n",
                       fd.cFileName, ret);
                FreeLibrary(dll);
                continue;
            }
        }

        /* Success! */
        loaded_plugin_t *lp = &plugins[plugin_count];
        lp->dll = dll;
        lp->desc = desc;
        strncpy(lp->path, full_path, MAX_PATH - 1);
        lp->loaded = 1;
        plugin_count++;

        logmsg("[plugins] Loaded: %s v%s by %s — %s\n",
               desc->name ? desc->name : fd.cFileName,
               desc->version ? desc->version : "?",
               desc->author ? desc->author : "?",
               desc->description ? desc->description : "");

    } while (FindNextFileA(h, &fd));

    FindClose(h);
    logmsg("[plugins] %d plugin(s) loaded\n", plugin_count);
}

/* Unload all plugins (called on DLL_PROCESS_DETACH) */
static void unload_plugins(void)
{
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].loaded) {
            if (plugins[i].desc && plugins[i].desc->shutdown)
                plugins[i].desc->shutdown();
            FreeLibrary(plugins[i].dll);
            plugins[i].loaded = 0;
            logmsg("[plugins] Unloaded plugin %d\n", i);
        }
    }
    plugin_count = 0;
    plugin_round_cb_count = 0;
    plugin_line_cb_count = 0;
}

/* Plugins dialog — shows loaded plugins */
static void show_plugins_dialog(HWND parent)
{
    char msg[2048];
    int pos = 0;

    if (plugin_count == 0) {
        pos += sprintf(msg + pos, "No plugins loaded.\n\n");
        pos += sprintf(msg + pos, "To install plugins, place .dll files in the\n");
        pos += sprintf(msg + pos, "plugins/ folder inside your MegaMUD directory.\n\n");
        pos += sprintf(msg + pos, "Plugins must be built with slop_plugin.h\n");
        pos += sprintf(msg + pos, "and export the slop_get_plugin() function.\n");
    } else {
        pos += sprintf(msg + pos, "Loaded Plugins (%d):\n", plugin_count);
        pos += sprintf(msg + pos, "========================\n\n");
        for (int i = 0; i < plugin_count; i++) {
            slop_plugin_t *p = plugins[i].desc;
            pos += sprintf(msg + pos, "  %s v%s\n",
                           p->name ? p->name : "Unknown",
                           p->version ? p->version : "?");
            pos += sprintf(msg + pos, "  by %s\n", p->author ? p->author : "?");
            if (p->description)
                pos += sprintf(msg + pos, "  %s\n", p->description);
            pos += sprintf(msg + pos, "\n");
        }
    }

    char plugins_dir[MAX_PATH];
    get_megamud_dir(plugins_dir, MAX_PATH);
    strcat(plugins_dir, "plugins\\");
    pos += sprintf(msg + pos, "Plugin directory:\n%s", plugins_dir);

    MessageBoxA(parent, msg, "MajorSLOP Plugins", MB_OK | MB_ICONINFORMATION);
}

/* ---- DllMain ---- */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        {
            char sys_path[MAX_PATH];
            GetSystemDirectory(sys_path, MAX_PATH);
            strcat(sys_path, "\\msimg32.dll");
            real_msimg32 = LoadLibraryA(sys_path);
            if (real_msimg32) {
                real_AlphaBlend = (AlphaBlend_t)GetProcAddress(real_msimg32, "AlphaBlend");
                real_GradientFill = (GradientFill_t)GetProcAddress(real_msimg32, "GradientFill");
                real_TransparentBlt = (TransparentBlt_t)GetProcAddress(real_msimg32, "TransparentBlt");
                real_DllInitialize = (DllInitialize_t)GetProcAddress(real_msimg32, "DllInitialize");
                real_vSetDdrawflag = (vSetDdrawflag_t)GetProcAddress(real_msimg32, "vSetDdrawflag");
            }
        }
        CreateThread(NULL, 0, payload_main, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        unload_plugins();
        if (real_msimg32) FreeLibrary(real_msimg32);
        if (logfile) fclose(logfile);
        break;
    }
    return TRUE;
}
