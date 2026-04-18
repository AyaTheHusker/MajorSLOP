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
 *   SAMSAY <text> [pitch] [speed]   → speak text using SAM TTS
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
 *   ROUNDTICK <round_num>           → notify DLL of a combat round tick
 *   COMBATEND                       → notify DLL combat has ended
 *   ENUMWIN                         → dump all MMMAIN child windows (class, id, size, style)
 *   INJECT <text>                   → feed raw text through MegaMUD as fake server data (\r\n for newlines)
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
#include <io.h>      /* _commit, _fileno — for force-flushing log file */

#include "slop_plugin.h"
#include "line_fsm.h"

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

/* Bless command slots (10 × char[21], stride 0x15) */
#define OFF_BLESS_CMD1    0x4F1F
#define OFF_BLESS_STRIDE  0x15
#define OFF_BLESS_COUNT   10
/* Party bless command slots (4 × char[21]) */
#define OFF_PARTY_BLESS1  0x4FF1
#define OFF_PARTY_BLESS_COUNT 4
/* Heal/Regen/Flux command slots (char[21] each) */
#define OFF_HEAL_CMD      0x4E40
#define OFF_REGEN_CMD     0x4E55
#define OFF_FLUX_CMD      0x4E6A
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
#define SLOP_WINDOW_CLASS    "MegaMudPlusAbout"
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
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    if (logfile) {
        fwrite(buf, 1, n, logfile);
        fflush(logfile);
        /* On crash-prone systems the C stdlib flush isn't enough — force the OS
         * to commit the bytes to disk so the last log line survives a crash. */
        _commit(_fileno(logfile));
    }
    /* Mirror to debugger (DebugView / VS output) for live tracing */
    OutputDebugStringA(buf);
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
 * MegaMud+ About Window — plasma fractal animation + status text
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
            slop_draw_text_shadow(hdc, slop_font_big, "MegaMud+ Proxy",
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
        slop_draw_text_shadow(hdc, slop_font_small, "MegaMud+ v0.1.0 by Tripmunk",
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
        "MegaMud+ Proxy by Tripmunk 2026",
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
#define RT_CLASS           "MegaMudPlusRoundTimer"
#define RT_WIN_W           190
#define RT_WIN_H           220
#define IDM_SLOP_ROUNDTIMER 40903
#define IDM_SLOP_PRO_STEP   40904
#define IDM_SLOP_PLUGINS    40905
#define IDM_PLUGIN_BASE     41000  /* plugin menu items: 41000..41099 */

/* Round timer right-click context menu IDs */
#define IDM_RT_SYNC_SPELL   40910
#define IDM_RT_PERFORM_SYNC 40911
#define IDM_RT_HIDE         40912

/* Manual sync spell picker window */
#define RT_SPELL_CLASS      "MegaMudPlusSpellPicker"
#define IDC_SPELL_LIST      50001
#define IDC_SPELL_EDIT      50002
#define IDC_SPELL_OK        50003
#define IDC_SPELL_CANCEL    50004
#define IDC_SPELL_ROUNDS    50005

/* Forward declarations */
static volatile int rt_visible;
static void subclass_toolbar_for_il_dedup(HWND tb);
static void rt_show_context_menu(HWND hwnd, int x, int y);
static void rt_show_spell_picker(HWND parent);
static void rt_perform_manual_sync(void);
static int starts_with_i(const char *line, const char *prefix);
static const char *stristr(const char *haystack, const char *needle);
static void plugins_on_round(int round_num);
static int rd_round_num = 0;       /* round counter — shared between auto and manual sync */
static double rd_last_event_ts;    /* timestamp of last combat event (for clustering) */
static DWORD WINAPI pro_step_thread(LPVOID param);
static DWORD WINAPI auto_struct_thread(LPVOID param);
static DWORD WINAPI mmansi_scanner_thread(LPVOID param);
static int mmansi_read_row(HWND mmansi, int row, char *out, int out_sz);
static int scan_mmansi_for_location(void);
static void update_statusbar_location(void);
static int loc_map;
static int loc_room;

/* Plugin system forward declarations */
#define SLOP_MAX_PLUGINS 16

typedef struct {
    HMODULE            dll;
    slop_plugin_t     *desc;
    char               path[MAX_PATH];
    int                loaded;
} loaded_plugin_t;

static loaded_plugin_t plugins[SLOP_MAX_PLUGINS];
static int plugin_count;
static void plugins_on_line(const char *line);
static void plugins_on_clean_line(const char *line);
static void plugins_on_data(const char *data, int len);
static void plugins_on_round(int round_num);
static int plugins_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void show_plugins_dialog(HWND parent);
static void ensure_plugins_dir(void);
static void load_plugins(void);
static void unload_plugins(void);

/* TTS plugin discovery — plugins can export tts_speak(const char *text, int pitch, int speed) */
typedef void (*tts_speak_fn)(const char *text, int pitch, int speed);
static struct {
    tts_speak_fn speak;
    const char  *name;     /* from plugin descriptor */
    int          plugin_idx;
} tts_engines[SLOP_MAX_PLUGINS];
static int tts_engine_count = 0;

/* ---- Configuration file (majorslop.cfg) ---- */

#define CFG_FILENAME "majorslop.cfg"

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

        if (strncmp(line, "round_timer=", 12) == 0) {
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
    logmsg("[mudplugin] Config loaded: round_timer=%s\n",
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
    fprintf(f, "# MegaMud+ Configuration\n");
    fprintf(f, "# Edit while MegaMUD is closed, or use the in-app menus.\n");
    fprintf(f, "#\n");
    fprintf(f, "# round_timer: shown or hidden\n");
    fprintf(f, "\n");
    fprintf(f, "round_timer=%s\n", rt_visible ? "shown" : "hidden");
    fprintf(f, "pro_every_step=%s\n", pro_every_step ? "on" : "off");
    if (cfg_player_name[0])
        fprintf(f, "player_name=%s\n", cfg_player_name);
    fclose(f);
}

/* ---- Manual Sync Spell state ---- */
static char rt_sync_spell[22] = {0};  /* selected spell command (4-letter abbrev) */
static HWND rt_spell_picker_hwnd = NULL;

/* Read a null-terminated string from MegaMUD struct */
static const char *rt_read_str(DWORD offset, int maxlen)
{
    static char buf[64];
    if (!struct_base) { buf[0] = '\0'; return buf; }
    const char *src = (const char *)(struct_base + offset);
    int i;
    for (i = 0; i < maxlen && i < 63 && src[i] && src[i] != '\0'; i++)
        buf[i] = src[i];
    buf[i] = '\0';
    /* trim trailing spaces */
    while (i > 0 && buf[i-1] == ' ') buf[--i] = '\0';
    return buf;
}

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

    /* Round timer colors — hardcoded for now */
    #define RT_DIM   RGB(0x66, 0x66, 0xAA)
    #define RT_DIM2  RGB(0x44, 0x44, 0x71)  /* dim * 2/3 */
    #define RT_ACCENT RGB(0x44, 0x44, 0xCC)
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
    HPEN dimPen = CreatePen(PS_SOLID, penW, RT_DIM2);
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
                            (i == 0) ? RGB(220, 220, 240) : RT_DIM);
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
        sprintf(elapsed_txt, "%.1fs / 5.0s", fmod(elapsed, period) / 1000.0);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RT_DIM);
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
        SetTextColor(mem, RT_ACCENT);
        SIZE sz;
        GetTextExtentPoint32A(mem, "WAITING", 7, &sz);
        TextOutA(mem, cx - sz.cx / 2, cy - sz.cy / 2 - 8, "WAITING", 7);
        HFONT sub_font = CreateFontA(fontSmall, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                      0, 0, ANTIALIASED_QUALITY, 0, "Arial");
        SelectObject(mem, sub_font);
        SetTextColor(mem, RT_DIM);
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
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDBLCLK:
        rt_perform_manual_sync();
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
    case WM_RBUTTONUP:
    {
        POINT pt;
        pt.x = (short)LOWORD(lParam);
        pt.y = (short)HIWORD(lParam);
        ClientToScreen(hwnd, &pt);
        rt_show_context_menu(hwnd, pt.x, pt.y);
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == IDM_RT_SYNC_SPELL) {
            HWND mw = FindWindowA("MMMAIN", NULL);
            rt_show_spell_picker(mw ? mw : hwnd);
            return 0;
        }
        if (id == IDM_RT_PERFORM_SYNC) {
            rt_perform_manual_sync();
            return 0;
        }
        if (id == IDM_RT_HIDE) {
            SendMessageA(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0;
    }
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
        wc.style = CS_DBLCLKS;
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
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        RT_CLASS,
        "Round Timer",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_THICKFRAME,
        px, py, RT_WIN_W, RT_WIN_H,
        FindWindowA("MMMAIN", NULL), NULL, GetModuleHandle(NULL), NULL);

    logmsg("[mudplugin] CreateWindowExA returned %p\n", rt_hwnd);

    if (rt_hwnd) {
        rt_visible = 1;
        SetWindowPos(rt_hwnd, NULL, px, py, RT_WIN_W, RT_WIN_H,
                     SWP_SHOWWINDOW | SWP_NOZORDER);
        if (slop_menu)
            ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                        IDM_SLOP_ROUNDTIMER, "Hide Round Timer");
        logmsg("[mudplugin] Round timer created and shown\n");
    }
}

/* ================================================================
 * Manual Sync Spell Picker
 * ================================================================ */

/* ---- Read buff/heal spells from Default/Spells.md (MDB2) ---- */

#define RT_MAX_SPELLS 300
#define OFF_PLAYER_CLASS 0x53C8

typedef struct {
    char name[32];   /* spell name */
    char cmd[10];    /* command abbreviation */
    int  level;      /* level required */
} rt_spell_entry_t;

static rt_spell_entry_t rt_spell_bank[RT_MAX_SPELLS];
static int rt_spell_bank_count = 0;

/* Map class ID → allowed spell schools.
 * Class 3=Cleric→school 1, Class 5=Mage→school 3,4,
 * Class 7=Bard→school 10, Class 8=Missionary→school 1,
 * Class 9=Ranger→school 7, Class 10=Paladin→school 1,
 * Class 11=Warlock→school 5, Class 12=Ninja→school 11,
 * Class 13=Druid→school 7 */
static int rt_class_allows_school(int class_id, int school)
{
    switch (class_id) {
    case 3: case 8: case 10: return school == 1;        /* Cleric/Missionary/Paladin */
    case 5:                  return school == 3 || school == 4; /* Mage */
    case 7:                  return school == 10;        /* Bard */
    case 9: case 13:         return school == 7;         /* Ranger/Druid */
    case 11:                 return school == 5;         /* Warlock */
    case 12:                 return school == 11;        /* Ninja */
    default:                 return 0;                   /* Warrior/Thief/Ganbusher etc. */
    }
}

static void rt_collect_spells(void)
{
    rt_spell_bank_count = 0;

    /* Get player class and level from memory */
    int player_class = struct_base ? *(int *)(struct_base + OFF_PLAYER_CLASS) : 0;
    int player_level = struct_base ? *(int *)(struct_base + OFF_PLAYER_LEVEL) : 0;

    /* Build path to Default/Spells.md next to the exe */
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0'; else path[0] = '\0';
    strcat(path, "Default\\Spells.md");

    /* Read file */
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        logmsg("[spells] Cannot open %s\n", path);
        return;
    }
    DWORD file_size = GetFileSize(hf, NULL);
    if (file_size < 0x400) { CloseHandle(hf); return; }

    unsigned char *data = (unsigned char *)malloc(file_size);
    if (!data) { CloseHandle(hf); return; }
    DWORD bytes_read;
    ReadFile(hf, data, file_size, &bytes_read, NULL);
    CloseHandle(hf);

    int page_size = 0x400;
    int num_pages = (int)(file_size / page_size);

    for (int pg = 1; pg < num_pages && rt_spell_bank_count < RT_MAX_SPELLS; pg++) {
        unsigned char *page = data + pg * page_size;
        /* Data leaf pages have type 0x0000 */
        unsigned short page_type = *(unsigned short *)page;
        if (page_type != 0) continue;

        int pos = 0x0C;  /* skip page header */
        while (pos < page_size - 10) {
            int rec_len_byte = page[pos];
            if (rec_len_byte == 0 || rec_len_byte == 0xFF) break;
            int total = rec_len_byte + 1;
            if (pos + total > page_size) break;

            /* Find 0x80 marker after key */
            int marker = -1;
            for (int i = 2; i < 20 && i < total; i++) {
                if (page[pos + i] == 0x80) { marker = i; break; }
            }
            if (marker < 0 || marker + 3 + 156 > total) {
                pos += total;
                continue;
            }

            unsigned char *payload = &page[pos + marker + 3];

            /* Spell name at +0x00 (30 bytes) */
            char name[32] = {0};
            memcpy(name, payload, 30);
            name[30] = '\0';
            /* Null-terminate at first \0 */
            for (int i = 0; i < 30; i++) if (name[i] == '\0') break;

            /* Command at +0x1E (7 bytes) */
            char cmd[10] = {0};
            memcpy(cmd, payload + 0x1E, 7);
            cmd[7] = '\0';

            /* Level required at +0x52 */
            int spell_level = payload[0x52];
            /* Target type at +0x60 */
            int target_type = payload[0x60];
            /* Spell school at +0x61 */
            int school = payload[0x61];

            /* Filter: must have name and command */
            if (!name[0] || !cmd[0]) { pos += total; continue; }
            /* Filter: skip item-granted spells (id >= 10000, cmd starts with #) */
            if (cmd[0] == '#') { pos += total; continue; }

            /* Filter: buff/heal/utility only — NOT attacks
             * 1=self buff, 2=heal/cure, 7=utility, 13=party buff */
            if (target_type != 1 && target_type != 2 &&
                target_type != 7 && target_type != 13) {
                pos += total;
                continue;
            }

            /* Filter by class (only if we know it — if 0, show all) */
            if (player_class > 0 && school > 0 && !rt_class_allows_school(player_class, school)) {
                pos += total;
                continue;
            }

            /* Filter by level (only if we know it — if 0, show all) */
            if (player_level > 0 && spell_level > 0 && spell_level > player_level) {
                pos += total;
                continue;
            }

            /* Add to bank */
            rt_spell_entry_t *e = &rt_spell_bank[rt_spell_bank_count];
            strncpy(e->name, name, 31); e->name[31] = '\0';
            strncpy(e->cmd, cmd, 9); e->cmd[9] = '\0';
            e->level = spell_level;
            rt_spell_bank_count++;

            pos += total;
        }
    }

    free(data);
    logmsg("[spells] Loaded %d buff/heal spells for class=%d level=%d\n",
           rt_spell_bank_count, player_class, player_level);
}

/* Populate a ComboBox with "name (cmd)" entries */
static void rt_populate_spell_combo(HWND cb)
{
    SendMessageA(cb, CB_RESETCONTENT, 0, 0);
    rt_collect_spells();
    if (rt_spell_bank_count == 0) {
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"(no spells available)");
        SendMessageA(cb, CB_SETCURSEL, 0, 0);
        return;
    }
    int sel_idx = -1;
    for (int i = 0; i < rt_spell_bank_count; i++) {
        char display[48];
        sprintf(display, "%s (%s)", rt_spell_bank[i].name, rt_spell_bank[i].cmd);
        int idx = (int)SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)display);
        /* Store the spell bank index as item data */
        SendMessageA(cb, CB_SETITEMDATA, idx, (LPARAM)i);
        if (rt_sync_spell[0] && _stricmp(rt_spell_bank[i].cmd, rt_sync_spell) == 0)
            sel_idx = idx;
    }
    if (sel_idx >= 0)
        SendMessageA(cb, CB_SETCURSEL, sel_idx, 0);
}

/* Manual sync rounds setting */
/* Forward declaration */
static void rt_perform_manual_sync(void);

static LRESULT CALLBACK slop_spell_picker_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        HINSTANCE hinst = GetModuleHandle(NULL);
        int y = 10;

        /* Spell command entry */
        CreateWindowExA(0, "STATIC", "Spell cmd:",
                        WS_CHILD | WS_VISIBLE,
                        10, y + 2, 70, 18, hwnd, NULL, hinst, NULL);
        HWND ed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", rt_sync_spell,
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LOWERCASE,
                        82, y, 120, 22, hwnd,
                        (HMENU)(UINT_PTR)IDC_SPELL_EDIT,
                        hinst, NULL);
        SendMessageA(ed, EM_SETLIMITTEXT, 20, 0);
        y += 32;

        /* OK / Cancel */
        CreateWindowExA(0, "BUTTON", "OK",
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        40, y, 70, 26, hwnd, (HMENU)(UINT_PTR)IDC_SPELL_OK,
                        hinst, NULL);
        CreateWindowExA(0, "BUTTON", "Cancel",
                        WS_CHILD | WS_VISIBLE,
                        120, y, 70, 26, hwnd, (HMENU)(UINT_PTR)IDC_SPELL_CANCEL,
                        hinst, NULL);

        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        if (id == IDC_SPELL_OK) {
            HWND ed = GetDlgItem(hwnd, IDC_SPELL_EDIT);
            if (ed) {
                char manual[32] = {0};
                GetWindowTextA(ed, manual, sizeof(manual));
                char *p = manual; while (*p == ' ') p++;
                if (p[0]) {
                    strncpy(rt_sync_spell, p, 21);
                    rt_sync_spell[21] = '\0';
                    int l = (int)strlen(rt_sync_spell);
                    while (l > 0 && rt_sync_spell[l-1] == ' ') rt_sync_spell[--l] = '\0';
                    logmsg("[roundtimer] Sync spell set: '%s'\n", rt_sync_spell);
                }
            }
            DestroyWindow(hwnd);
            return 0;
        }

        if (id == IDC_SPELL_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        rt_spell_picker_hwnd = NULL;
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void rt_show_spell_picker(HWND parent)
{
    if (rt_spell_picker_hwnd) {
        SetForegroundWindow(rt_spell_picker_hwnd);
        return;
    }

    static int registered = 0;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = slop_spell_picker_proc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = RT_SPELL_CLASS;
        RegisterClassA(&wc);
        registered = 1;
    }

    /* Position near the round timer */
    RECT pr;
    if (rt_hwnd) GetWindowRect(rt_hwnd, &pr);
    else GetWindowRect(parent, &pr);
    int px = pr.left;
    int py = pr.bottom + 4;

    /* Clamp to screen */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (px + 230 > sw) px = sw - 230;
    if (py + 100 > sh) py = pr.top - 104;
    if (px < 0) px = 0;
    if (py < 0) py = 0;

    rt_spell_picker_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        RT_SPELL_CLASS,
        "Manual Sync Spell",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        px, py, 230, 100,
        parent, NULL, GetModuleHandle(NULL), NULL);
}

/* ---- Manual Sync State Machine ---- */

/* States */
#define MS_IDLE        0
#define MS_SPAMMING    1  /* actively spamming spell, waiting for round boundaries */
#define MS_DONE        2

static volatile int ms_state = MS_IDLE;
static volatile int ms_already_count = 0;   /* consecutive "already cast" lines */
static volatile int ms_rounds_detected = 0; /* rounds detected during 15s window */

/* Spam thread: sends the spell command rapidly until ms_state != MS_SPAMMING */
static DWORD WINAPI ms_spam_thread(LPVOID param)
{
    (void)param;
    logmsg("[mansync] Spam thread started: spell='%s' (15s window)\n",
           rt_sync_spell);

    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) { ms_state = MS_IDLE; return 0; }
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;

    DWORD start_time = GetTickCount();

    while (ms_state == MS_SPAMMING) {
        /* 10 second sync window — enough for 2 round ticks */
        if (GetTickCount() - start_time > 10000) {
            logmsg("[mansync] 10s sync complete — detected %d rounds\n", ms_rounds_detected);
            ms_state = MS_DONE;
            break;
        }
        /* Send spell command + enter */
        for (int i = 0; rt_sync_spell[i]; i++)
            PostMessageA(target, WM_CHAR, (WPARAM)(unsigned char)rt_sync_spell[i], 0);
        PostMessageA(target, WM_CHAR, (WPARAM)'\r', 0);

        /* ~150ms between spam attempts — fast enough to catch the round boundary
         * but not so fast we flood the terminal */
        Sleep(150);
    }

    logmsg("[mansync] Spam thread ended: detected %d rounds\n", ms_rounds_detected);
    return 0;
}

/* Called from rd_process_line when manual sync is active.
 * Watches for:
 *   "You have already cast a spell this round" → increment already_count
 *   "You cast"/"You sing"/"You invoke"/"You attempt" → round boundary if already_count >= 2
 */
static void ms_process_line(const char *line)
{
    if (ms_state != MS_SPAMMING) return;

    /* "You have already cast a spell this round" */
    if (stristr(line, "You have already cast")) {
        ms_already_count++;
        return;
    }

    /* Round boundary: cast/sing/invoke/attempt — but only if we had 2+ "already cast" first */
    if (ms_already_count >= 2) {
        int is_boundary = 0;
        if (starts_with_i(line, "You cast ")) is_boundary = 1;
        else if (starts_with_i(line, "You sing ")) is_boundary = 1;
        else if (starts_with_i(line, "You invoke ")) is_boundary = 1;
        else if (starts_with_i(line, "You attempt to cast")) is_boundary = 1;
        else if (starts_with_i(line, "You attempt to sing")) is_boundary = 1;
        else if (starts_with_i(line, "You attempt to invoke")) is_boundary = 1;

        if (is_boundary) {
            ms_rounds_detected++;
            logmsg("[mansync] Round boundary #%d detected (already_count=%d)\n",
                   ms_rounds_detected, ms_already_count);

            /* Fire the round tick */
            rd_round_num++;
            rt_on_round_tick(rd_round_num);
            plugins_on_round(rd_round_num);

            ms_already_count = 0;
        }
    }

    /* Any line that isn't "already cast" resets the counter
     * (unless it's empty or a prompt line) */
    if (line[0] && !stristr(line, "You have already cast")) {
        /* Don't reset on the boundary line itself — already handled above.
         * Only reset on unrelated lines (monster says something, etc.) */
        if (ms_already_count > 0 && ms_already_count < 2) {
            /* Had <2 already-casts, then got an unrelated line — false start, reset */
            ms_already_count = 0;
        }
    }
}

static void rt_perform_manual_sync(void)
{
    if (!rt_sync_spell[0]) return;
    if (ms_state == MS_SPAMMING) {
        /* Already running — cancel it */
        ms_state = MS_IDLE;
        logmsg("[mansync] Cancelled by user\n");
        return;
    }

    /* Reset round timer state — throw away old ticks, start fresh */
    rt_synced = 0;
    rt_last_tick_ts = 0.0;
    rt_interval_count = 0;
    rt_round_period = 5000.0;
    rt_delta_text[0] = '\0';
    rt_delta_fade = 0.0;
    rd_last_event_ts = 0.0;

    ms_state = MS_SPAMMING;
    ms_already_count = 0;
    ms_rounds_detected = 0;

    logmsg("[mansync] Starting 15s manual sync: spell='%s'\n", rt_sync_spell);

    CreateThread(NULL, 0, ms_spam_thread, NULL, 0, NULL);
}

/* Round timer right-click context menu */
static void rt_show_context_menu(HWND hwnd, int x, int y)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, IDM_RT_SYNC_SPELL, "Manual Sync Spell...");
    AppendMenuA(menu, MF_STRING | (rt_sync_spell[0] ? 0 : MF_GRAYED),
                IDM_RT_PERFORM_SYNC, "Perform Manual Sync");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_RT_HIDE, "Hide Round Timer");
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static WNDPROC orig_mmmain_proc = NULL;
static WNDPROC orig_mmansi_proc = NULL;
static volatile int slop_in_menu = 0;  /* set while our menu is open */

static void slop_update_menu_state(void)
{
    if (!slop_menu) return;
    /* Update round timer toggle text */
    ModifyMenuA(slop_menu, IDM_SLOP_ROUNDTIMER, MF_BYCOMMAND | MF_STRING,
                IDM_SLOP_ROUNDTIMER, rt_visible ? "Hide Round Timer" : "Show Round Timer");
}

static LRESULT CALLBACK slop_mmansi_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* Alt+F4 — instant kill at every level, no dialogs */
    if (msg == WM_SYSKEYDOWN && wParam == VK_F4) { ExitProcess(0); return 0; }
    if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE) { ExitProcess(0); return 0; }
    if (msg == WM_CLOSE) { ExitProcess(0); return 0; }
    /* Forward WM_KEYDOWN to plugins for hotkeys (F11 etc.) */
    if (msg == WM_KEYDOWN) {
        if (plugins_on_wndproc(hwnd, msg, wParam, lParam))
            return 0;
    }
    return CallWindowProcA(orig_mmansi_proc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK slop_mmmain_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* Alt+F4 — instant kill at every level, no dialogs */
    if (msg == WM_SYSKEYDOWN && wParam == VK_F4) { ExitProcess(0); return 0; }
    if (msg == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE) { ExitProcess(0); return 0; }
    if (msg == WM_CLOSE) { ExitProcess(0); return 0; }

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
        if (id >= IDM_PLUGIN_BASE) {
            logmsg("[wndproc] WM_COMMAND id=%u forwarding to plugins\n", (unsigned)id);
            if (plugins_on_wndproc(hwnd, msg, wParam, lParam))
                return 0;
        }
    }
    /* Forward WM_TIMER to plugins so they can use SetTimer */
    if (msg == WM_TIMER) {
        plugins_on_wndproc(hwnd, msg, wParam, lParam);
    }
    /* Forward WM_KEYDOWN to plugins for hotkeys (e.g. F11) */
    if (msg == WM_KEYDOWN) {
        if (plugins_on_wndproc(hwnd, msg, wParam, lParam))
            return 0;
    }
    return CallWindowProcA(orig_mmmain_proc, hwnd, msg, wParam, lParam);
}

static void inject_menu(HWND main_wnd)
{
    logmsg("[trace] inject_menu: enter hwnd=0x%08X\n", (DWORD)main_wnd);
    HMENU menu_bar = GetMenu(main_wnd);
    logmsg("[trace] inject_menu: menu_bar=0x%08X\n", (DWORD)menu_bar);
    if (!menu_bar) {
        logmsg("[mudplugin] No menu bar found on MMMAIN\n");
        return;
    }

    /* Check if we already injected (look for our menu) */
    int count = GetMenuItemCount(menu_bar);
    logmsg("[trace] inject_menu: existing menu count=%d\n", count);
    for (int i = 0; i < count; i++) {
        char buf[64] = {0};
        GetMenuStringA(menu_bar, i, buf, sizeof(buf), MF_BYPOSITION);
        if (strcmp(buf, "MegaMud+") == 0) {
            logmsg("[mudplugin] MegaMud+ menu already present\n");
            return;
        }
    }

    /* Create our top-level popup menu */
    slop_menu = CreatePopupMenu();
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_ABOUT, "Proxy Info...");
    AppendMenuA(slop_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_ROUNDTIMER, "Show Round Timer");
    AppendMenuA(slop_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(slop_menu, MF_STRING, IDM_SLOP_PRO_STEP,
                pro_every_step ? "RM Every Step  [ON]" : "RM Every Step  [OFF]");
    /* Append MegaMud+ to menu bar */
    AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)slop_menu, "MegaMud+");

    /* Subclass to catch our menu commands (before plugin load so callbacks work) */
    logmsg("[trace] inject_menu: subclassing MMMAIN\n");
    if (!orig_mmmain_proc) {
        orig_mmmain_proc = (WNDPROC)SetWindowLongA(main_wnd, GWL_WNDPROC, (LONG)slop_mmmain_proc);
    }
    logmsg("[trace] inject_menu: MMMAIN subclassed, orig_proc=0x%08X\n", (DWORD)orig_mmmain_proc);
    /* Subclass MMANSI for keyboard hotkeys (F11 etc.) — keys go here, not MMMAIN */
    {
        HWND ansi = FindWindowExA(main_wnd, NULL, "MMANSI", NULL);
        logmsg("[trace] inject_menu: MMANSI hwnd=0x%08X\n", (DWORD)ansi);
        if (ansi && !orig_mmansi_proc) {
            orig_mmansi_proc = (WNDPROC)SetWindowLongA(ansi, GWL_WNDPROC, (LONG)slop_mmansi_proc);
            logmsg("[mudplugin] MMANSI subclassed for hotkeys\n");
        }
    }

    slop_menu_owner = main_wnd;
    slop_update_menu_state();

    logmsg("[mudplugin] MegaMud+ menu injected to menu bar\n");

    /* Start background threads */
    logmsg("[trace] inject_menu: spawning background threads\n");
    CreateThread(NULL, 0, pro_step_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, auto_struct_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, mmansi_scanner_thread, NULL, 0, NULL);
    logmsg("[trace] inject_menu: threads spawned\n");

    /* Plugin system — load plugins FIRST, then build Plugins menu */
    logmsg("[trace] inject_menu: ensure_plugins_dir()\n");
    ensure_plugins_dir();
    logmsg("[trace] inject_menu: load_plugins()\n");
    load_plugins();
    logmsg("[trace] inject_menu: plugins loaded\n");

    /* Create "Plugins" top-level menu with per-plugin submenus */
    {
        HMENU plugins_menu = CreatePopupMenu();

        if (plugin_count == 0) {
            AppendMenuA(plugins_menu, MF_STRING | MF_GRAYED, 0, "(no plugins loaded)");
        } else {
            for (int i = 0; i < plugin_count; i++) {
                slop_plugin_t *p = plugins[i].desc;
                const char *pname = (p && p->name) ? p->name : "Unknown";

                HMENU sub = CreatePopupMenu();

                char info[128];
                sprintf(info, "%s v%s", pname, (p && p->version) ? p->version : "?");
                AppendMenuA(sub, MF_STRING | MF_GRAYED, 0, info);
                AppendMenuA(sub, MF_SEPARATOR, 0, NULL);

                int base_id = IDM_PLUGIN_BASE + i * 10;
                if (p && p->name && _stricmp(p->name, "MMUDPy") == 0) {
                    AppendMenuA(sub, MF_STRING, base_id + 0, "Show/Hide Console");
                    AppendMenuA(sub, MF_STRING, base_id + 1, "Script Manager...");
                }

                AppendMenuA(plugins_menu, MF_POPUP, (UINT_PTR)sub, pname);
            }
        }

        AppendMenuA(plugins_menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(plugins_menu, MF_STRING, IDM_SLOP_PLUGINS, "Plugin Info...");

        AppendMenuA(menu_bar, MF_POPUP, (UINT_PTR)plugins_menu, "Plugins");
    }

    DrawMenuBar(main_wnd);

    /* Force frame recalculation — adding menu items can change menu bar height,
       which shifts the client area. Without this, toolbar/MMANSI stay at stale positions. */
    SetWindowPos(main_wnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    /* Log child window positions for debugging */
    {
        HWND tb = FindWindowExA(main_wnd, NULL, "ToolbarWindow32", NULL);
        HWND ansi = FindWindowExA(main_wnd, NULL, "MMANSI", NULL);
        HWND sb = FindWindowExA(main_wnd, NULL, "msctls_statusbar32", NULL);
        RECT r;
        if (tb) {
            GetWindowRect(tb, &r);
            POINT pt = {r.left, r.top};
            ScreenToClient(main_wnd, &pt);
            logmsg("[mudplugin] Toolbar pos: client(%d,%d) size(%dx%d) vis=%d\n",
                   pt.x, pt.y, r.right - r.left, r.bottom - r.top, IsWindowVisible(tb));
            subclass_toolbar_for_il_dedup(tb);
        }
        if (ansi) {
            GetWindowRect(ansi, &r);
            POINT pt = {r.left, r.top};
            ScreenToClient(main_wnd, &pt);
            logmsg("[mudplugin] MMANSI pos: client(%d,%d) size(%dx%d)\n",
                   pt.x, pt.y, r.right - r.left, r.bottom - r.top);
        }
    }
}

/* ================================================================
 * Winsock recv() hook — intercept MUD traffic for round detection
 * ================================================================
 *
 * IAT-patches ws2_32!recv so we see every byte from the MUD server.
 * We buffer partial lines, strip ANSI escapes, and feed clean lines
 * into the round detection state machine.
 */

/* Forward declarations for functions defined later */
static void strip_ansi(char *line);
static void rd_process_line(const char *line);

/* FUN_0041C8D0: process incoming server data (also used by inject) */
typedef void (__cdecl *process_incoming_fn)(int, void *, int);

/* ---- ASLR-safe VA resolver --------------------------------------
 * All hardcoded VAs below are from Ghidra RE of MegaMUD.exe assuming
 * its default PE image base 0x00400000. On Windows 11 (or any system
 * with system-wide ASLR / mandatory ASLR), the EXE gets relocated to
 * some other base — 0x00400000 is then garbage memory and every
 * VirtualProtect/call goes boom. mega_va() converts a design-time VA
 * into the runtime address by rebasing against the actual loaded
 * MegaMUD.exe module. Every VA_* macro routes through this. */
static DWORD mega_base_cached = 0;
static inline DWORD mega_va(DWORD va)
{
    if (!mega_base_cached)
        mega_base_cached = (DWORD)GetModuleHandleA(NULL);
    return va - 0x00400000 + mega_base_cached;
}

#define VA_PROCESS_INCOMING  mega_va(0x0041C8D0)

/* ---- Inline hook for FUN_0041C8D0 (incoming data processor) ----
 * Ghidra RE: void __cdecl FUN_0041C8D0(int struct, void *data, int len)
 * This is where ALL incoming BBS data flows before reaching MMANSI.
 * Data contains raw ANSI escape codes — perfect for color parsing.
 * We patch the first 5 bytes with a JMP to our hook, then call original. */

static unsigned char orig_bytes_0041C8D0[5];  /* saved original bytes */
static int hook_0041C8D0_installed = 0;

/* Line buffer for accumulating incoming data into lines.
 * The state machine is shared between the FUN_0041C8D0 hook path
 * (incoming_feed_data) and the legacy recv-hook path (rd_feed_data) —
 * both feed the same byte stream so a single FSM is correct and
 * matches the old behavior where recv_line_pos/recv_line_buf were
 * module-level globals shared across both entry points. */
#define RECV_LINE_BUF 4096
static char recv_line_buf[RECV_LINE_BUF];
static line_fsm_t recv_fsm;
static int recv_fsm_inited = 0;

/* Dispatch a completed line: raw → plugins → strip → clean → plugins → round.
 * `line` points into recv_line_buf (writable), so we strip ANSI in place. */
static void recv_on_line_cb(const char *line, void *user)
{
    (void)user;
    plugins_on_line(line);               /* raw, ANSI intact */
    char *mut = (char *)line;            /* owned by recv_fsm's backing store */
    strip_ansi(mut);
    plugins_on_clean_line(mut);
    rd_process_line(mut);
}

static void recv_fsm_ensure(void)
{
    if (!recv_fsm_inited) {
        line_fsm_init(&recv_fsm, recv_line_buf, RECV_LINE_BUF,
                      LINE_FSM_STRIP_NONE, LINE_FSM_TERM_LF,
                      recv_on_line_cb, NULL);
        recv_fsm_inited = 1;
    }
}

/* Feed raw incoming data (with ANSI) into line buffer, dispatch complete lines */
static void incoming_feed_data(const char *data, int len)
{
    recv_fsm_ensure();
    line_fsm_feed(&recv_fsm, data, len);
}

static void dynpath_check_pending(void);  /* defined below — drains pending + checks arrival */

/* Our hook — called instead of FUN_0041C8D0 */
static volatile int hook_0041C8D0_call_count = 0;
static void __cdecl hooked_process_incoming(int sbase, void *data, int len)
{
    /* Log the first few calls so we can confirm the hook is reached cleanly */
    if (hook_0041C8D0_call_count < 3) {
        logmsg("[trace] hooked_process_incoming #%d sbase=0x%08X data=0x%08X len=%d\n",
               hook_0041C8D0_call_count, (DWORD)sbase, (DWORD)data, len);
        hook_0041C8D0_call_count++;
    }
    /* Tap the raw ANSI data for plugins */
    if (len > 0 && data) {
        /* Raw byte stream for terminal emulators (byte-by-byte processing) */
        plugins_on_data((const char *)data, len);
        /* Line-delimited for other plugins (round detection, triggers, etc.) */
        incoming_feed_data((const char *)data, len);
    }

    /* Restore original bytes, call real function, re-patch */
    unsigned char *target = (unsigned char *)VA_PROCESS_INCOMING;
    DWORD old_protect;
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect);
    memcpy(target, orig_bytes_0041C8D0, 5);
    VirtualProtect(target, 5, old_protect, &old_protect);

    /* Call original */
    process_incoming_fn real_fn = (process_incoming_fn)VA_PROCESS_INCOMING;
    real_fn(sbase, data, len);

    dynpath_check_pending();

    /* Re-install hook */
    VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect);
    target[0] = 0xE9;  /* JMP rel32 */
    *(int *)(target + 1) = (int)((unsigned char *)hooked_process_incoming - target - 5);
    VirtualProtect(target, 5, old_protect, &old_protect);
}

static int hook_process_incoming(void)
{
    unsigned char *target = (unsigned char *)VA_PROCESS_INCOMING;

    /* Save original 5 bytes */
    DWORD old_protect;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old_protect)) {
        logmsg("[comhook] VirtualProtect failed on runtime=0x%08X (design VA=0x0041C8D0, mega_base=0x%08X)\n",
               (DWORD)target, mega_base_cached);
        return 0;
    }
    memcpy(orig_bytes_0041C8D0, target, 5);

    /* Write JMP to our hook */
    target[0] = 0xE9;
    *(int *)(target + 1) = (int)((unsigned char *)hooked_process_incoming - target - 5);

    VirtualProtect(target, 5, old_protect, &old_protect);
    hook_0041C8D0_installed = 1;
    logmsg("[comhook] FUN_0041C8D0 hooked — raw ANSI interception active\n");
    return 1;
}

/* Original recv function pointer (legacy — recv hook never worked, kept for reference) */
typedef int (WSAAPI *recv_fn)(SOCKET s, char *buf, int len, int flags);
static recv_fn real_recv = NULL;

/* Track which socket is the MUD connection (first large recv) */
static SOCKET mud_socket = INVALID_SOCKET;

/* Round detection state — passive swing-based detection.
 * Every round, swings happen (hits/misses/damage/glances) right at onset.
 * Multiple swings can happen in the same round (multi-hit, party members, etc).
 * We cluster events within a short window — first event in a new cluster = round tick.
 * Rounds are always 5s on ParaMUD, so any gap > ~3s between clusters = new round. */
/* rd_round_num declared earlier (forward decl section) */
/* rd_last_event_ts declared earlier (forward decl section) */
#define RD_CLUSTER_GAP 500.0            /* ms — events within this are same round (only "You " lines now, so burst is tight) */

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

    /* "Your sword glances off ..." — weapon ineffective (before "You " guard) */
    if (starts_with_i(line, "Your ") && strstr(line, " glances off")) return 1;

    /* ONLY track YOUR combat actions — not monster attacks, not spells.
     * "You cast", "You sing", "You invoke", "You use" are NOT combat rounds,
     * those are buff/heal spells — reserved for a future manual-set system. */
    if (!starts_with_i(line, "You ")) return 0;

    /* "You hit/slash/crush/hurl/etc ... for N damage!" */
    if (strstr(line, " damage!")) return 1;

    /* "You ... who dodges your attack!" — miss via dodge (melee AND ranged) */
    if (strstr(line, "dodges your attack")) return 1;

    /* "You miss" — straight miss */
    if (stristr(line, "You miss ")) return 1;

    /* "You fumble" */
    if (stristr(line, "You fumble")) return 1;

    /* Exclude ALL spells/songs/invocations — not on the melee round timer.
     * Future manual mode will handle spell round detection separately. */
    if (starts_with_i(line, "You cast ") ||
        starts_with_i(line, "You sing ") ||
        starts_with_i(line, "You invoke ")) return 0;

    /* Generic melee/ranged miss: "You [verb] at [target]!"
     * Format from WCC: You %s at %s!  (msg #8446)
     * Only match if exactly ONE word between "You " and " at " to avoid
     * false positives from item use ("You point your wand at X!"),
     * exploration ("You step into the water at..."), etc.
     * Also match ranged: "You hurl/fire/throw a X at Y!" (multiple words ok) */
    {
        const char *at_ptr = strstr(line + 4, " at ");
        if (at_ptr && len > 0 && line[len - 1] == '!') {
            /* Check if single word between "You " and " at " (melee miss) */
            const char *first_space = strchr(line + 4, ' ');
            if (first_space == at_ptr) return 1;  /* "You [verb] at X!" */
            /* Ranged miss: "You hurl/fire/throw ..." with " at " and no damage */
            if (starts_with_i(line, "You hurl ") ||
                starts_with_i(line, "You fire ") ||
                starts_with_i(line, "You throw ")) return 1;
        }
    }

    return 0;
}

/* Process one clean (ANSI-stripped) line from MUD traffic.
 * Round detection is ONLY based on actual swings (hits/misses/damage).
 * Combat Engaged/Off are IRRELEVANT — they fire 100x per round via macros. */
static void rd_process_line(const char *line)
{
    if (!line[0]) return;

    /* Manual sync: takes over completely — skip normal combat detection */
    if (ms_state == MS_SPAMMING) {
        ms_process_line(line);
        return;
    }

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

    /* plugins already notified with raw ANSI in rd_feed_data */
}

/* Feed raw recv data into line buffer, process complete lines.
 * Legacy path (recv hook) — MegaMUD uses ReadFile in practice, so this is
 * almost never called. Shares the FSM with incoming_feed_data to guarantee
 * the two paths can't desync. */
static void rd_feed_data(const char *data, int len)
{
    recv_fsm_ensure();
    line_fsm_feed(&recv_fsm, data, len);
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
        if (recv_fsm_inited) line_fsm_reset(&recv_fsm);
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

/* ================================================================
 * HIMAGELIST double-free compat shim (native Win11)
 * ================================================================
 *
 * megamud.exe has a latent double-free on an HIMAGELIST in its init /
 * toolbar-recreate path — documented in MEGAMUD_RE.md under "HIMAGELIST
 * Double-Free — Code Map". Wine's comctl32 silently tolerates it, but
 * native Win11's comctl32 validates the "HIML" magic via `cmpxchg` and
 * AVs if the block has been freed. Without this shim the process dies
 * ~2–3s after launch whenever vk_terminal.dll is loaded.
 *
 * This shim IAT-patches COMCTL32!ImageList_Destroy. It tracks handles
 * seen by Destroy and drops duplicate Destroys silently, mirroring Wine.
 * Every call is logged so we can learn the actual handle-flow after the
 * fact. Thread-safe via a CRITICAL_SECTION — destroys can come from the
 * UI thread or comctl32 worker threads.
 */

typedef BOOL (WINAPI *imagelist_destroy_fn)(HIMAGELIST);
static imagelist_destroy_fn real_imagelist_destroy = NULL;

#define IL_SEEN_CAP 64
static HIMAGELIST il_seen[IL_SEEN_CAP];
static int il_seen_count = 0;
static CRITICAL_SECTION il_seen_lock;
static int il_seen_lock_inited = 0;

static int il_seen_contains(HIMAGELIST h)
{
    for (int i = 0; i < il_seen_count; i++) {
        if (il_seen[i] == h) return 1;
    }
    return 0;
}

/* Pre-mark a handle as freed so a subsequent ImageList_Destroy call gets
 * deduped (skipped) by the hook. Used by the toolbar WM_DESTROY subclass:
 * comctl32 frees the toolbar's imagelists internally on WM_DESTROY without
 * going through the public ImageList_Destroy export, so our IAT shim never
 * sees that first free. If the app then calls ImageList_Destroy on the
 * same handle (as megamud does at MEGAMUD+0x889d3), comctl32's IsValid
 * cmpxchg AVs on the freed memory. Pre-marking lets us swallow that call. */
static void il_mark_freed(HIMAGELIST h)
{
    if (!h) return;
    if (!il_seen_lock_inited) {
        InitializeCriticalSection(&il_seen_lock);
        il_seen_lock_inited = 1;
    }
    EnterCriticalSection(&il_seen_lock);
    if (!il_seen_contains(h) && il_seen_count < IL_SEEN_CAP) {
        il_seen[il_seen_count++] = h;
        logmsg("[ildedup] pre-marked 0x%08X freed (toolbar WM_DESTROY)\n", (DWORD)h);
    }
    LeaveCriticalSection(&il_seen_lock);
}

static WNDPROC orig_toolbar_wndproc = NULL;

static LRESULT CALLBACK toolbar_subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        HIMAGELIST il_n = (HIMAGELIST)SendMessageA(hwnd, TB_GETIMAGELIST,         0, 0);
        HIMAGELIST il_h = (HIMAGELIST)SendMessageA(hwnd, TB_GETHOTIMAGELIST,      0, 0);
        HIMAGELIST il_d = (HIMAGELIST)SendMessageA(hwnd, TB_GETDISABLEDIMAGELIST, 0, 0);
        il_mark_freed(il_n);
        il_mark_freed(il_h);
        il_mark_freed(il_d);
    }
    return CallWindowProcA(orig_toolbar_wndproc, hwnd, msg, wp, lp);
}

static void subclass_toolbar_for_il_dedup(HWND tb)
{
    if (!tb || orig_toolbar_wndproc) return;
    LONG_PTR old = SetWindowLongPtrA(tb, GWLP_WNDPROC, (LONG_PTR)toolbar_subclass_proc);
    if (old) {
        orig_toolbar_wndproc = (WNDPROC)old;
        logmsg("[ildedup] toolbar 0x%08X subclassed for WM_DESTROY pre-mark\n", (DWORD)tb);
    } else {
        logmsg("[ildedup] toolbar subclass FAILED on 0x%08X (err=%lu)\n",
               (DWORD)tb, GetLastError());
    }
}

static BOOL WINAPI hooked_imagelist_destroy(HIMAGELIST h)
{
    if (!h) {
        /* comctl32 treats NULL as a no-op returning TRUE; keep Wine-compat */
        return TRUE;
    }

    EnterCriticalSection(&il_seen_lock);
    int already = il_seen_contains(h);
    if (!already && il_seen_count < IL_SEEN_CAP) {
        il_seen[il_seen_count++] = h;
    }
    LeaveCriticalSection(&il_seen_lock);

    if (already) {
        logmsg("[ildedup] SKIP duplicate ImageList_Destroy(0x%08X) — would crash native comctl32\n",
               (DWORD)h);
        return TRUE;
    }

    logmsg("[ildedup] ImageList_Destroy(0x%08X) real\n", (DWORD)h);
    if (real_imagelist_destroy) {
        return real_imagelist_destroy(h);
    }
    return FALSE;
}

/* IAT-patch COMCTL32!ImageList_Destroy in megamud.exe */
static int hook_imagelist_destroy(void)
{
    if (!il_seen_lock_inited) {
        InitializeCriticalSection(&il_seen_lock);
        il_seen_lock_inited = 1;
    }

    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!import_rva) return 0;

    HMODULE comctl = GetModuleHandleA("comctl32.dll");
    if (!comctl) comctl = LoadLibraryA("comctl32.dll");
    FARPROC target = comctl ? GetProcAddress(comctl, "ImageList_Destroy") : NULL;
    if (!target) {
        logmsg("[ildedup] cannot resolve comctl32!ImageList_Destroy\n");
        return 0;
    }
    real_imagelist_destroy = (imagelist_destroy_fn)target;

    IMAGE_IMPORT_DESCRIPTOR *imports = (IMAGE_IMPORT_DESCRIPTOR *)((char *)exe + import_rva);
    int patched = 0;
    for (; imports->Name; imports++) {
        const char *dll_name = (const char *)((char *)exe + imports->Name);
        if (_stricmp(dll_name, "comctl32.dll") != 0) continue;

        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)((char *)exe + imports->FirstThunk);
        IMAGE_THUNK_DATA *orig_thunk = NULL;
        if (imports->OriginalFirstThunk)
            orig_thunk = (IMAGE_THUNK_DATA *)((char *)exe + imports->OriginalFirstThunk);

        for (int i = 0; thunk->u1.Function; thunk++, i++) {
            FARPROC *func_ptr = (FARPROC *)&thunk->u1.Function;
            int match = 0;

            if (orig_thunk) {
                IMAGE_THUNK_DATA *ot = orig_thunk + i;
                if (!(ot->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME *name_entry =
                        (IMAGE_IMPORT_BY_NAME *)((char *)exe + ot->u1.AddressOfData);
                    if (strcmp((char *)name_entry->Name, "ImageList_Destroy") == 0) {
                        match = 1;
                    }
                }
            }
            if (!match && *func_ptr == target) match = 1;

            if (match) {
                DWORD old_protect;
                VirtualProtect(func_ptr, sizeof(FARPROC), PAGE_READWRITE, &old_protect);
                *func_ptr = (FARPROC)hooked_imagelist_destroy;
                VirtualProtect(func_ptr, sizeof(FARPROC), old_protect, &old_protect);
                patched++;
                logmsg("[ildedup] IAT slot 0x%08X patched (was 0x%08X -> 0x%08X)\n",
                       (DWORD)func_ptr, (DWORD)target, (DWORD)hooked_imagelist_destroy);
            }
        }
    }

    if (patched == 0) {
        logmsg("[ildedup] no ImageList_Destroy IAT slot found in megamud.exe imports\n");
        return 0;
    }
    return 1;
}

/* ================================================================
 * Exit logging — hook ExitProcess/TerminateProcess/abort so any
 * silent process death tells us exactly who pulled the trigger.
 * ================================================================ */

typedef VOID (WINAPI *ExitProcess_fn)(UINT);
typedef BOOL (WINAPI *TerminateProcess_fn)(HANDLE, UINT);
static ExitProcess_fn      real_ExitProcess      = NULL;
static TerminateProcess_fn real_TerminateProcess = NULL;

static void log_caller_module(const char *tag, DWORD caller, UINT code, HANDLE proc)
{
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)caller, &mod);
    char modname[MAX_PATH] = {0};
    if (mod) GetModuleFileNameA(mod, modname, sizeof(modname));
    DWORD offset = mod ? (caller - (DWORD)mod) : 0;
    if (proc) {
        logmsg("[EXIT] %s(hProc=0x%p, code=%u) from 0x%08X %s+0x%X (tid=%lu)\n",
               tag, proc, code, caller, modname[0] ? modname : "?", offset,
               GetCurrentThreadId());
    } else {
        logmsg("[EXIT] %s(code=%u) from 0x%08X %s+0x%X (tid=%lu)\n",
               tag, code, caller, modname[0] ? modname : "?", offset,
               GetCurrentThreadId());
    }
    if (logfile) { fflush(logfile); _commit(_fileno(logfile)); }
}

static VOID WINAPI hooked_ExitProcess(UINT code)
{
    DWORD caller = (DWORD)__builtin_return_address(0);
    log_caller_module("ExitProcess", caller, code, NULL);
    if (real_ExitProcess) real_ExitProcess(code);
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(1000);
}

static BOOL WINAPI hooked_TerminateProcess(HANDLE hProc, UINT code)
{
    DWORD caller = (DWORD)__builtin_return_address(0);
    /* Only log kills of *our own* process; skip child-process kills */
    if (hProc == GetCurrentProcess() || hProc == (HANDLE)-1 ||
        GetProcessId(hProc) == GetCurrentProcessId()) {
        log_caller_module("TerminateProcess", caller, code, hProc);
    }
    if (real_TerminateProcess) return real_TerminateProcess(hProc, code);
    return FALSE;
}

/* IAT-patch kernel32!ExitProcess + kernel32!TerminateProcess in megamud.exe */
static int hook_exit_logging(void)
{
    HMODULE exe = GetModuleHandleA(NULL);
    if (!exe) return 0;

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)exe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    DWORD import_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!import_rva) return 0;

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) k32 = LoadLibraryA("kernel32.dll");
    if (!k32) return 0;
    FARPROC t_exit = GetProcAddress(k32, "ExitProcess");
    FARPROC t_term = GetProcAddress(k32, "TerminateProcess");
    real_ExitProcess      = (ExitProcess_fn)t_exit;
    real_TerminateProcess = (TerminateProcess_fn)t_term;

    IMAGE_IMPORT_DESCRIPTOR *imports = (IMAGE_IMPORT_DESCRIPTOR *)((char *)exe + import_rva);
    int patched = 0;
    for (; imports->Name; imports++) {
        const char *dll_name = (const char *)((char *)exe + imports->Name);
        if (_stricmp(dll_name, "kernel32.dll") != 0) continue;

        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)((char *)exe + imports->FirstThunk);
        IMAGE_THUNK_DATA *orig_thunk = NULL;
        if (imports->OriginalFirstThunk)
            orig_thunk = (IMAGE_THUNK_DATA *)((char *)exe + imports->OriginalFirstThunk);

        for (int i = 0; thunk->u1.Function; thunk++, i++) {
            FARPROC *func_ptr = (FARPROC *)&thunk->u1.Function;
            const char *name = NULL;

            if (orig_thunk) {
                IMAGE_THUNK_DATA *ot = orig_thunk + i;
                if (!(ot->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    IMAGE_IMPORT_BY_NAME *ne =
                        (IMAGE_IMPORT_BY_NAME *)((char *)exe + ot->u1.AddressOfData);
                    name = (const char *)ne->Name;
                }
            }

            FARPROC replacement = NULL;
            if ((name && strcmp(name, "ExitProcess") == 0) ||
                (!name && t_exit && *func_ptr == t_exit)) {
                replacement = (FARPROC)hooked_ExitProcess;
            } else if ((name && strcmp(name, "TerminateProcess") == 0) ||
                       (!name && t_term && *func_ptr == t_term)) {
                replacement = (FARPROC)hooked_TerminateProcess;
            }

            if (replacement) {
                DWORD old_protect;
                VirtualProtect(func_ptr, sizeof(FARPROC), PAGE_READWRITE, &old_protect);
                *func_ptr = replacement;
                VirtualProtect(func_ptr, sizeof(FARPROC), old_protect, &old_protect);
                patched++;
                logmsg("[exitlog] IAT slot patched: %s -> 0x%08X\n",
                       name ? name : "(by addr)", (DWORD)replacement);
            }
        }
    }

    if (patched == 0) {
        logmsg("[exitlog] no ExitProcess/TerminateProcess IAT slots found\n");
        return 0;
    }
    return 1;
}

/* ================================================================
 * Server data injection — call MegaMUD's incoming data processor
 * ================================================================
 *
 * Ghidra RE of FUN_0041BAC0 (main comm thread) shows the pipeline:
 *   BBS → ReadFile (FUN_0040F9B0) → buffer → FUN_0041C8D0(struct, data, len)
 *
 * FUN_0041C8D0 is the incoming data processor — it feeds raw bytes
 * into MMANSI for display and through MegaMUD's line parser.
 * We call it directly with our fake data.
 *
 * Note: FUN_0041CF40 is the OUTGOING processor (keyboard → BBS).
 *       Buffer at +0x863D is OUTGOING. Don't write there for injection.
 */

#define COMM_BUFFER_SIZE  0x50   /* 80 bytes — matches MegaMUD's read cap */

/* Inject fake server data through MegaMUD's real incoming data processor.
 * Data appears in MMANSI terminal and goes through the full line parser. */
static void inject_server_data_impl(const char *data, int len)
{
    if (!struct_base || len <= 0) return;

    process_incoming_fn process_incoming = (process_incoming_fn)VA_PROCESS_INCOMING;

    /* Feed data in chunks matching MegaMUD's normal read size (80 bytes) */
    int offset = 0;
    while (offset < len) {
        int chunk = len - offset;
        if (chunk > COMM_BUFFER_SIZE) chunk = COMM_BUFFER_SIZE;

        process_incoming((int)struct_base, (void *)(data + offset), chunk);
        offset += chunk;
    }

    logmsg("[inject] Injected %d bytes via FUN_0041C8D0\n", len);
}

/* ================================================================
 * Fake Remote — call MegaMUD's internal @command functions directly
 * without telepath overhead or response messages.
 * Ghidra RE of FUN_0047cf70 (the master @command dispatcher, 17KB).
 * ================================================================ */

/* Internal function VAs from Ghidra RE */
/* All routed through mega_va() — ASLR-safe at runtime */
#define VA_STOP_PATH     mega_va(0x00428970)  /* void(int struct) — stop current path/movement */
#define VA_PREP_LOOP     mega_va(0x0045fee0)  /* void(void *struct, int dest_buf) — prepare loop dest */
#define VA_LOAD_PATH     mega_va(0x0045f860)  /* void(int struct, char *dest, void *entry, int 0) */
#define VA_START_PATH    mega_va(0x0042b510)  /* int(int struct, void *dest, int entry) — start path */
#define VA_VERIFY_PATH   mega_va(0x00428fa0)  /* uint(int *struct, int 0) — 0 = success */
#define VA_UPDATE_ROAM   mega_va(0x004455b0)  /* void(int *struct) — update roaming state */
#define VA_MM_STRICMP    mega_va(0x004a31f0)  /* int(void *s1, void *s2) — 0 = match */
#define VA_NORMALIZE_STR mega_va(0x0047c870)  /* void(void *str) — normalize (lowercase/trim) */
/* Path database management (Ghidra RE 2026-04-17) */
#define VA_PATH_ALLOC    mega_va(0x0045f100)  /* void*(int struct) — alloc 0x84-byte path record slot */
#define VA_PATH_EXISTS   mega_va(0x0045fe20)  /* uint(int struct, byte *filename) — 0 = not found */
#define VA_PARSE_MP      mega_va(0x00460050)  /* uint(int struct, char *record, int 0) — parse .mp file */
#define VA_UPDATE_PATHS  mega_va(0x00427d20)  /* void(int *struct, int flag) — rebuild Paths.md/menu */
#define VA_INI_WRITE_INT mega_va(0x00435c30)  /* void(int struct, LPCSTR sect, LPCSTR key, uint val) */
#define VA_LOAD_ALL_DATA mega_va(0x00427120)  /* uint(int *struct, uint flags) — master data reload */
/* Reset update functions */
#define VA_RESET_FN1     mega_va(0x00478a70)  /* void(int struct) */
#define VA_RESET_FN2     mega_va(0x00478920)  /* void(int struct) */
#define VA_RESET_FN3     mega_va(0x00478b10)  /* void(int struct) */
#define VA_RESET_FN4     mega_va(0x0040e8d0)  /* void(int struct, int 1) */

/* Struct offsets for @command state (Ghidra RE of FUN_0047cf70) */
#define OFF_FR_GO_FLAG    0x564C   /* int32: master go/stop toggle. 1=go, 0=stop */
#define OFF_FR_PATHING    0x5664   /* int32: non-zero if currently pathing */
#define OFF_FR_LOOPING    0x5668   /* int32: 1 = loop mode */
#define OFF_FR_ROAMING    0x566C   /* int32: auto-roam enabled */
#define OFF_FR_IS_RESTING 0x5678   /* int32: player is resting */
#define OFF_FR_IS_MEDIT   0x567C   /* int32: player is meditating */
#define OFF_FR_LOOP_COUNT 0x2C58   /* int32: number of loop/path entries */
#define OFF_FR_LOOP_ARRAY 0x2C60   /* ptr: array of loop entry pointers */
#define OFF_FR_ROOM_COUNT 0x2C18   /* int32: number of room entries */
#define OFF_FR_ROOM_ARRAY 0x2C20   /* ptr: array of room entry pointers */
#define OFF_FR_LOOP_DEST  0x5930   /* char[]: loop destination buffer */
#define OFF_FR_ON_ENTRY   0x54B4   /* int32: 0=nothing, 1=resume loop, 2=auto-roam */
#define OFF_FR_MODE       0x54BC   /* int32: 11=idle, 14=walking, 15=looping, 16=roaming */
#define OFF_FR_STEPS_REM  0x54D8   /* int32: steps remaining in current path */
#define OFF_FR_MMMAIN_HWND 0x0C   /* HWND: MMMAIN window handle in struct */
#define OFF_FR_PATH_FILE   0x5834  /* char[76]: current .mp filename */
#define OFF_FR_PATH_WP     0x5880  /* 32 bytes: waypoints + sentinel + total_steps + cur_step */
#define OFF_FR_SEL_PATH    0x593C  /* char[14]: selected/queued path filename */
#define OFF_FR_LOOP_RESUME_A 0x5988 /* int32: loop resume trigger A */
#define OFF_FR_LOOP_RESUME_B 0x598C /* int32: loop resume trigger B */
#define OFF_FR_PATH_DIRTY  0x571C  /* int32: path database dirty flag */
#define OFF_FR_BASEDIR     0x06F7  /* char[257]: MegaMUD base directory */
#define MEGAMUD_CMD_STOPGO 0x0803  /* WM_COMMAND ID for stop/go toggle */

/* Loop entry struct (from Ghidra):
 *   +0x04  char* — pointer to filename string
 *   +0x0C  char[] — inline display name
 * Room entry struct:
 *   +0x01  char[] — short code/name
 *   +0x06  char[] — full room name
 *   +0x44  void* — destination data pointer */

typedef void (__cdecl *fn_vi)(int);
typedef void (__cdecl *fn_vpi)(void *, int);
typedef void (__cdecl *fn_vicpi)(int, char *, void *, int);
typedef int  (__cdecl *fn_iippi)(int, void *, int);
typedef unsigned int (__cdecl *fn_uipi)(int *, int);
typedef void (__cdecl *fn_vip)(int *);
typedef int  (__cdecl *fn_ipp)(void *, void *);
typedef void (__cdecl *fn_vp)(void *);
typedef void (__cdecl *fn_vii)(int, int);

static int fake_remote_impl(const char *cmd)
{
    if (!struct_base || !cmd || !cmd[0]) return -1;
    int sb = (int)struct_base;
    unsigned char *sbp = (unsigned char *)struct_base;  /* pointer form */

    /* --- stop --- */
    if (_stricmp(cmd, "stop") == 0) {
        if (*(int *)(sbp + OFF_FR_PATHING) != 0) {
            HWND mw = FindWindowA("MMMAIN", NULL);
            if (mw) SendMessageA(mw, WM_COMMAND, MEGAMUD_CMD_STOPGO, 0);
        }
        logmsg("[fake_remote] stop\n");
        return 0;
    }

    /* --- rego --- */
    if (_stricmp(cmd, "rego") == 0) {
        if (*(int *)(sbp + OFF_FR_PATHING) != 0) {
            logmsg("[fake_remote] rego — already pathing\n");
            return 0;
        }
        HWND mw = FindWindowA("MMMAIN", NULL);
        if (mw) SendMessageA(mw, WM_COMMAND, MEGAMUD_CMD_STOPGO, 0);
        int ok = (*(int *)(sbp + OFF_FR_PATHING) != 0);
        logmsg("[fake_remote] rego — %s\n", ok ? "resumed" : "can't");
        return ok ? 0 : -1;
    }

    /* --- roam on|off --- */
    if (_strnicmp(cmd, "roam", 4) == 0 &&
        (cmd[4] == ' ' || cmd[4] == '\0')) {
        const char *arg = cmd + 4;
        while (*arg == ' ') arg++;
        if (_stricmp(arg, "on") == 0) {
            *(int *)(sbp + OFF_FR_ROAMING) = 1;
            *(int *)(sbp + OFF_FR_PATHING) = 1;
        } else if (_stricmp(arg, "off") == 0) {
            *(int *)(sbp + OFF_FR_ROAMING) = 0;
        } else {
            logmsg("[fake_remote] roam — need 'on' or 'off'\n");
            return -1;
        }
        ((fn_vip)VA_UPDATE_ROAM)((int *)sbp);
        logmsg("[fake_remote] roam %s\n", arg);
        return 0;
    }

    /* --- loop <name> --- */
    if (_strnicmp(cmd, "loop ", 5) == 0) {
        const char *name = cmd + 5;
        while (*name == ' ') name++;
        if (!*name) return -1;

        char nb[0x29];
        strncpy(nb, name, 0x28);
        nb[0x28] = '\0';
        ((fn_vp)VA_NORMALIZE_STR)(nb);

        int count = *(int *)(sbp + OFF_FR_LOOP_COUNT);
        int arr   = *(int *)(sbp + OFF_FR_LOOP_ARRAY);
        fn_ipp cmp = (fn_ipp)VA_MM_STRICMP;

        for (int i = 0; i < count; i++) {
            int entry = *(int *)(arr + i * 4);
            if (!entry) continue;

            char *fname = *(char **)(entry + 4);
            int fm = (fname && cmp(fname, nb) == 0);
            int dm = (cmp((char *)(entry + 0xC), nb) == 0);

            if (fm || dm) {
                ((fn_vi)VA_STOP_PATH)(sb);
                /* Fully clear old state before starting new loop */
                *(int *)(sbp + OFF_FR_LOOPING) = 0;
                *(int *)(sbp + OFF_FR_ON_ENTRY) = 0;
                *(int *)(sbp + OFF_FR_PATHING) = 0;
                *(int *)(sbp + OFF_FR_GO_FLAG) = 0;
                ((fn_vpi)VA_PREP_LOOP)(sbp, (int)(sbp + OFF_FR_LOOP_DEST));
                ((fn_vicpi)VA_LOAD_PATH)(sb, (char *)(sbp + OFF_FR_LOOP_DEST),
                                         (void *)entry, 0);
                int rv = ((fn_iippi)VA_START_PATH)(sb, NULL, entry);
                if (rv == 0) {
                    logmsg("[fake_remote] loop '%s' — start failed\n", name);
                    return -1;
                }
                unsigned int vr = ((fn_uipi)VA_VERIFY_PATH)((int *)sbp, 0);
                if (vr != 0) {
                    logmsg("[fake_remote] loop '%s' — verify failed\n", name);
                    return -1;
                }
                *(int *)(sbp + 0x54dc) = 0;
                /* Kick movement: if GO_FLAG is not set, send StopGo to start.
                 * Real @loop assumes you're already going; we may be resting. */
                if (*(int *)(sbp + OFF_FR_GO_FLAG) == 0) {
                    HWND mw = *(HWND *)(sbp + OFF_FR_MMMAIN_HWND);
                    if (mw) SendMessageA(mw, WM_COMMAND, MEGAMUD_CMD_STOPGO, 0);
                    logmsg("[fake_remote] loop — sent StopGo to kick movement\n");
                }
                logmsg("[fake_remote] loop '%s' — started (entry %d)\n", name, i);
                return 0;
            }
        }
        logmsg("[fake_remote] loop '%s' — not found (%d entries)\n", name, count);
        return -1;
    }

    /* --- goto <name> --- */
    if (_strnicmp(cmd, "goto ", 5) == 0) {
        const char *name = cmd + 5;
        while (*name == ' ') name++;
        if (!*name) return -1;

        char nb[0x29];
        strncpy(nb, name, 0x28);
        nb[0x28] = '\0';
        ((fn_vp)VA_NORMALIZE_STR)(nb);

        int count = *(int *)(sbp + OFF_FR_ROOM_COUNT);
        int arr   = *(int *)(sbp + OFF_FR_ROOM_ARRAY);
        fn_ipp cmp = (fn_ipp)VA_MM_STRICMP;

        for (int i = 0; i < count; i++) {
            int entry = *(int *)(arr + i * 4);
            if (!entry) continue;

            int m1 = (cmp((char *)(entry + 6), nb) == 0);
            int m2 = (cmp((char *)(entry + 1), nb) == 0);

            if (m1 || m2) {
                /* VA-based goto with loop-resume prevention.
                 * Layer 1: zero all known loop-resume fields before/after VA calls.
                 * Layer 2: vk_terminal.c watchdog catches resume if this doesn't stick. */
                ((fn_vi)VA_STOP_PATH)(sb);

                /* Clear loop state before starting goto path */
                *(int *)(sbp + OFF_FR_LOOPING) = 0;
                *(int *)(sbp + OFF_FR_ON_ENTRY) = 0;
                *(int *)(sbp + 0x5988) = 0;  /* loop resume trigger A */
                *(int *)(sbp + 0x598C) = 0;  /* loop resume trigger B */
                memset(sbp + 0x5834, 0, 76); /* PATH_FILE_NAME */

                void *dest = *(void **)(entry + 0x44);
                int rv = ((fn_iippi)VA_START_PATH)(sb, dest, 0);
                if (rv == 0) {
                    logmsg("[fake_remote] goto '%s' — start failed\n", name);
                    return -1;
                }
                unsigned int vr = ((fn_uipi)VA_VERIFY_PATH)((int *)sbp, 0);
                if (vr != 0) {
                    logmsg("[fake_remote] goto '%s' — verify failed\n", name);
                    return -1;
                }

                /* Re-assert after VA calls — they may restore loop state */
                *(int *)(sbp + OFF_FR_LOOPING) = 0;
                *(int *)(sbp + OFF_FR_ON_ENTRY) = 0;
                *(int *)(sbp + OFF_FR_MODE) = 14;       /* walking, not looping */
                *(int *)(sbp + 0x5988) = 0;
                *(int *)(sbp + 0x598C) = 0;
                memset(sbp + 0x5834, 0, 76);            /* PATH_FILE_NAME */

                /* Kick movement — StopGo is a TOGGLE, only send if stopped */
                if (*(int *)(sbp + OFF_FR_GO_FLAG) == 0) {
                    HWND mw = *(HWND *)(sbp + OFF_FR_MMMAIN_HWND);
                    if (mw) SendMessageA(mw, WM_COMMAND, MEGAMUD_CMD_STOPGO, 0);
                    logmsg("[fake_remote] goto — sent StopGo to kick movement\n");
                }
                logmsg("[fake_remote] goto '%s' — started (room %d)\n", name, i);
                return 0;
            }
        }
        logmsg("[fake_remote] goto '%s' — not found (%d rooms)\n", name, count);
        return -1;
    }

    /* --- reset --- */
    if (_stricmp(cmd, "reset") == 0) {
        /* Zero all internal flags/statistics (from Ghidra RE of @reset handler) */
        *(int *)(sbp + 0x3194) = 0;
        *(int *)(sbp + 0x358c) = 0;
        *(int *)(sbp + 0x4d98) = 0;
        *(int *)(sbp + 0x4d9c) = 0;
        *(int *)(sbp + 0x4da0) = 0;
        *(int *)(sbp + 0x5448) = 0;
        *(int *)(sbp + 0x4e3c) = 0;
        *(int *)(sbp + 0x54d0) = -1;  /* 0xFFFFFFFF */
        *(int *)(sbp + 0x5508) = 0;
        *(int *)(sbp + 0x3784) = 0;
        *(int *)(sbp + 0x5674) = 0;
        *(int *)(sbp + 0x5694) = 0;
        *(int *)(sbp + 0x56a4) = 0;
        *(int *)(sbp + 0x3558) = 0;
        *(int *)(sbp + 0x56ac) = 0;
        *(int *)(sbp + 0x56b0) = 0;
        *(int *)(sbp + 0x56b4) = 0;
        *(int *)(sbp + 0x56b8) = 0;
        *(int *)(sbp + 0x56bc) = 0;
        *(int *)(sbp + 0x56c0) = 0;
        *(int *)(sbp + 0x56c4) = 0;
        *(int *)(sbp + 0x56c8) = 0;
        *(int *)(sbp + 0x56cc) = 0;
        *(int *)(sbp + 0x56d0) = 0;
        *(int *)(sbp + 0x56d4) = 0;
        *(int *)(sbp + 0x56e0) = 0;
        *(int *)(sbp + 0x56e4) = 0;
        *(int *)(sbp + 0x56f0) = 0;
        *(int *)(sbp + 0x5730) = 0;
        *(int *)(sbp + 0x5734) = 0;
        *(int *)(sbp + 0x5738) = 0;
        *(int *)(sbp + 0x573c) = 1;  /* auto-combat default ON */
        *(int *)(sbp + 0x5744) = 1;
        *(int *)(sbp + 0x5748) = 1;
        *(int *)(sbp + 0x574c) = 1;
        *(int *)(sbp + 0x5750) = 1;
        *(int *)(sbp + 0x5754) = 0;
        *(int *)(sbp + 0x5758) = 0;
        *(int *)(sbp + 0x575c) = 0;
        *(int *)(sbp + 0x5764) = 0;
        *(int *)(sbp + 0x5768) = 0;
        *(int *)(sbp + 0x576c) = 0;
        *(int *)(sbp + 0x5770) = 0;
        *(int *)(sbp + 0x5778) = 0;
        *(int *)(sbp + 0x577c) = 0;
        *(int *)(sbp + 0x5788) = 0;
        *(int *)(sbp + 0x578c) = 0;
        *(int *)(sbp + 0x5790) = 0;
        *(int *)(sbp + 0x5794) = 0;
        *(int *)(sbp + 0x5a0c) = 0;
        *(int *)(sbp + 0x5510) = 0;

        /* Clear flags in spell/room entry arrays */
        int n1 = *(int *)(sbp + 0x1f14);
        int a1 = *(int *)(sbp + 0x1f1c);
        for (int i = 0; i < n1; i++) {
            unsigned short *p = (unsigned short *)(*(int *)(a1 + i * 4) + 0x116);
            *p &= 0xfffc;
        }

        /* Copy exp/time stamps for rate calculation reset */
        *(int *)(sbp + 0x95d8) = *(int *)(sbp + 0x9468);
        *(int *)(sbp + 0x95dc) = *(int *)(sbp + 0x946c);
        *(int *)(sbp + 0x95e0) = *(int *)(sbp + 0x9468);
        *(int *)(sbp + 0x95e4) = *(int *)(sbp + 0x946c);
        memset(sbp + 0x95f0, 0, 0x28);

        /* Reset HP/mana baseline */
        *(int *)(sbp + 0x9618) = *(int *)(sbp + 0x53d4);
        *(int *)(sbp + 0x961c) = *(int *)(sbp + 0x53e0);

        /* Clear status strings */
        memset(sbp + 0x319c, 0, 0x1f);
        memset(sbp + 0x5590, 0, 0x0b);
        memset(sbp + 0x559b, 0, 0x0b);
        memset(sbp + 0x55a6, 0, 0x0b);
        memset(sbp + 0x55b1, 0, 100);

        /* Clear item flags */
        int n2 = *(int *)(sbp + 0x1e28);
        int a2 = *(int *)(sbp + 0x1e30);
        for (int i = 0; i < n2; i++) {
            int e = *(int *)(a2 + i * 4);
            *(unsigned int *)(e + 0x54) &= 0xffff3fff;
            *(int *)(e + 0x78) = 0;
        }
        *(int *)(sbp + 0x9610) = 0;
        *(int *)(sbp + 0x9614) = 0;

        /* Call internal update functions */
        ((fn_vi)VA_RESET_FN1)(sb);
        ((fn_vi)VA_RESET_FN2)(sb);
        ((fn_vi)VA_RESET_FN3)(sb);
        ((fn_vii)VA_RESET_FN4)(sb, 1);

        logmsg("[fake_remote] reset — all flags/stats cleared\n");
        return 0;
    }

    logmsg("[fake_remote] unknown: '%s'\n", cmd);
    return -1;
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

            } else if (strncmp(line_start, "TERMROW ", 8) == 0) {
                /* TERMROW <row>
                 * Read MMANSI terminal row text + attribute bytes.
                 * Returns: TEXT<tab>ATTRHEX\n */
                int row = atoi(line_start + 8);
                HWND mw = FindWindowA("MMMAIN", NULL);
                HWND ansi = mw ? FindWindowExA(mw, NULL, "MMANSI", NULL) : NULL;
                if (!ansi) {
                    strcpy(resp, "ERR no MMANSI\n");
                } else {
                    LONG wdata = GetWindowLongA(ansi, 4);
                    if (!wdata || row < 0 || row >= MMANSI_MAX_ROWS) {
                        strcpy(resp, "ERR invalid\n");
                    } else {
                        /* Read text */
                        const char *txt = (const char *)(wdata + MMANSI_TEXT_OFF + row * MMANSI_ROW_STRIDE);
                        /* Read attrs */
                        const unsigned char *attr = (const unsigned char *)(wdata + MMANSI_ATTR_OFF + row * MMANSI_ROW_STRIDE);
                        /* Find trimmed length */
                        int len = MMANSI_ROW_STRIDE;
                        while (len > 0 && txt[len-1] == ' ') len--;
                        if (len == 0) {
                            strcpy(resp, "\n");
                        } else {
                            char *rp = resp;
                            memcpy(rp, txt, len); rp += len;
                            *rp++ = '\t';
                            for (int i = 0; i < len; i++) {
                                sprintf(rp, "%02X", attr[i]);
                                rp += 2;
                            }
                            *rp++ = '\n'; *rp = '\0';
                        }
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

            } else if (strncmp(line_start, "TTSAY ", 6) == 0) {
                /* TTSAY <engine_name> <text> [pitch] [speed]
                 * Speak text using a named TTS engine plugin.
                 * Engine name matches plugin name (case-insensitive prefix).
                 * Examples: TTSAY SAM Hello world 64 72
                 *           TTSAY espeak Hello world */
                char *arg = line_start + 6;
                /* Parse engine name (first word) */
                char engine[32] = {0};
                int ei = 0;
                while (*arg && *arg != ' ' && ei < 31) engine[ei++] = *arg++;
                engine[ei] = '\0';
                while (*arg == ' ') arg++;

                /* Parse text (everything up to optional trailing numbers) */
                char tts_text[256] = {0};
                int tts_pitch = 0, tts_speed = 0;

                /* Simple: take the rest as text. If last 1-2 tokens are numbers, use them as pitch/speed */
                strncpy(tts_text, arg, 255);

                /* Find matching TTS engine */
                tts_speak_fn found_fn = NULL;
                const char *found_name = NULL;
                int elen = (int)strlen(engine);
                for (int t = 0; t < tts_engine_count; t++) {
                    if (_strnicmp(engine, tts_engines[t].name, elen) == 0) {
                        found_fn = tts_engines[t].speak;
                        found_name = tts_engines[t].name;
                        break;
                    }
                }
                if (found_fn) {
                    found_fn(tts_text, tts_pitch, tts_speed);
                    sprintf(resp, "OK tts=%s\n", found_name);
                } else if (tts_engine_count == 0) {
                    strcpy(resp, "ERR no TTS engines loaded\n");
                } else {
                    sprintf(resp, "ERR unknown TTS engine '%s'\n", engine);
                }

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

            } else if (strncmp(line_start, "INJECT ", 7) == 0) {
                /* INJECT <text>
                 * Feeds raw text through MegaMUD's ReadFile hook as fake server data.
                 * Use \\r\\n for line breaks. Shows in MMANSI and goes through parser.
                 * Example: INJECT \r\nObvious exits: NONE\r\n */
                const char *text = line_start + 7;
                /* Unescape \r and \n sequences */
                char inject_decoded[COMM_BUFFER_SIZE];
                int dlen = 0;
                for (int i = 0; text[i] && dlen < COMM_BUFFER_SIZE - 1; i++) {
                    if (text[i] == '\\' && text[i+1] == 'r') {
                        inject_decoded[dlen++] = '\r';
                        i++;
                    } else if (text[i] == '\\' && text[i+1] == 'n') {
                        inject_decoded[dlen++] = '\n';
                        i++;
                    } else if (text[i] == '\\' && text[i+1] == '\\') {
                        inject_decoded[dlen++] = '\\';
                        i++;
                    } else {
                        inject_decoded[dlen++] = text[i];
                    }
                }
                if (struct_base && dlen > 0) {
                    inject_server_data_impl(inject_decoded, dlen);
                    sprintf(resp, "OK injected %d bytes\n", dlen);
                } else if (!struct_base) {
                    strcpy(resp, "ERR no struct base\n");
                } else {
                    strcpy(resp, "ERR empty inject data\n");
                }

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
            /* During manual sync, skip dedup — we need to count repeated
             * "You have already cast" lines which have identical hashes */
            if (ms_state != MS_SPAMMING) {
                if (was_seen(h)) continue;
            }
            mark_seen(h);
            /* Only send via MMANSI poll if inline hook isn't active
             * (hook sends raw ANSI; MMANSI rows are stripped text). These
             * rows are already ANSI-free (MMANSI strips at render), so
             * both on_line and on_clean_line receive identical content. */
            if (!hook_0041C8D0_installed) {
                plugins_on_line(rows[r]);
                plugins_on_clean_line(rows[r]);
            }
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

/* Scan MMANSI buffer for most recent "Location:" line, extract map,room.
 * Rejects chat spoofs (gossips/telepaths/say/yell/...) by requiring the
 * line to START with "Location:" after whitespace. Legitimate rm output
 * has no speaker prefix; every chat variant does. */
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
        /* Anchor to start of line. MegaMUD prefixes output lines with one
         * or more "[HP=.../MA=...]:" prompts (stackable), e.g.:
         *   [HP=238/MA=131]:[HP=238/MA=131]:Location:            6,2327
         * Strip any number of leading prompts + whitespace, then require
         * "Location:" to be the next token. Chat ("Name gossips: ...",
         * "You say: ...", etc.) still fails this check because after the
         * optional prompt a speaker name remains, not "Location:". */
        char *p = row_buf;
        for (;;) {
            while (*p == ' ' || *p == '\t') p++;
            if (p[0] == '[' && p[1] == 'H' && p[2] == 'P' && p[3] == '=') {
                char *end = strchr(p, ']');
                if (!end || end[1] != ':') break;
                p = end + 2;
                continue;
            }
            break;
        }
        if (strncmp(p, "Location:", 9) != 0) continue;
        p += 9;
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

/* Queue of "pending auto-rm injections" the vk_terminal filter should
 * swallow. Incremented right before PostMessage(WM_CHAR) types "rm\r";
 * vk_terminal peeks this to decide if the next "rm"/"Location:" line
 * came from us (hide it) vs. a manual user/script "rm" (show it). */
static volatile LONG auto_rm_queue_count = 0;

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

        /* Path step changed — type rm. Location parsing is handled by
         * get_location() which self-scans MMANSI ~20Hz, so we don't block
         * here waiting for the response (combat output used to push the
         * Location: line past a fixed 500ms sleep, leaving the cache stale). */
        if (step != pro_last_step && step > 0) {
            logmsg("[prostep] Step %d->%d\n", pro_last_step, step);
            pro_last_step = step;
            InterlockedIncrement(&auto_rm_queue_count);
            type_into_megamud("rm");
        } else {
            pro_last_step = step;
        }
    }
    return 0;
}

/* ---- Public exports so vk_terminal can show/flip the toggle from its own
 * right-click menu (without knowing the internal IDM/struct layout). */
int WINAPI rm_every_step_get(void)
{
    return pro_every_step ? 1 : 0;
}

void WINAPI rm_every_step_set(int on)
{
    on = on ? 1 : 0;
    if (pro_every_step == on) return;
    pro_every_step = on;
    if (on && struct_base) {
        pro_last_checksum = *(DWORD *)(struct_base + OFF_ROOM_CHECKSUM);
        pro_last_step     = *(int   *)(struct_base + OFF_CUR_PATH_STEP);
    }
    /* Keep msimg32's own menu text in sync. */
    if (slop_menu) {
        ModifyMenuA(slop_menu, IDM_SLOP_PRO_STEP, MF_BYCOMMAND | MF_STRING,
                    IDM_SLOP_PRO_STEP,
                    on ? "RM Every Step  [ON]" : "RM Every Step  [OFF]");
    }
    cfg_save();
    logmsg("[mudplugin] RM Every Step: %s (via vk_terminal)\n", on ? "ON" : "OFF");
}

/* Increment the auto-rm queue from outside msimg32 (numpad walk, android walk).
 * Same atomic pattern as pro_step_thread uses internally. */
void WINAPI auto_rm_queue_increment(void)
{
    InterlockedIncrement(&auto_rm_queue_count);
}

/* Returns current auto-rm queue depth without modifying it. vk_terminal uses
 * this to peek whether the next "rm" echo / Location block is ours. */
int WINAPI auto_rm_queue_peek(void)
{
    return (int)auto_rm_queue_count;
}

/* Decrement the auto-rm queue (floor at 0) and return 1 if we consumed a
 * slot, 0 if queue was already empty. vk_terminal calls this on the
 * Location: row match, once per auto-rm response. */
int WINAPI auto_rm_queue_consume(void)
{
    for (;;) {
        LONG cur = auto_rm_queue_count;
        if (cur <= 0) return 0;
        if (InterlockedCompareExchange(&auto_rm_queue_count, cur - 1, cur) == cur)
            return 1;
    }
}

/* Live current room checksum. Returns 0 when struct not resolved yet.
 * Safe to call every frame — just a memory read. */
unsigned int WINAPI get_room_checksum(void)
{
    if (!struct_base) return 0;
    return *(DWORD *)(struct_base + OFF_ROOM_CHECKSUM);
}

/* Live current location as parsed from the "rm" command output (Map, Room).
 * Returns packed value: (map << 16) | (room & 0xFFFF), or 0 if not yet known.
 * Map/room can each be 0..65535. MajorMUD map numbers are small (<256), room
 * numbers fit in 16 bits. Safe to call every frame.
 *
 * Rescans MMANSI itself (rate-limited ~20Hz) so the value is always fresh —
 * the prostep "type rm; Sleep(500); scan" pattern was missing fresh Location
 * lines when combat output delayed them past the sleep window. */
unsigned int WINAPI get_location(void)
{
    static DWORD last_scan = 0;
    DWORD now = GetTickCount();
    if ((DWORD)(now - last_scan) >= 50) {
        last_scan = now;
        scan_mmansi_for_location();
    }
    if (loc_map == 0 && loc_room == 0) return 0;
    unsigned int m = (unsigned int)loc_map & 0xFFFF;
    unsigned int r = (unsigned int)loc_room & 0xFFFF;
    return (m << 16) | r;
}

/* ---- Dynamic path injection ----
 * Builds a step buffer in MegaMUD's exact format and writes it into
 * the path entry at struct+0x5930, then sets control flags so the
 * native path walker executes it.  Called from vk_terminal when the
 * user clicks a room on the map and BFS produces a route.
 *
 * Step buffer format (from Ghidra RE of STEP_ALLOC 0x45f240):
 *   [4B checksum][2B flags][dir_string\0]  — stride = strlen(dir)+8
 *
 * Path entry offsets (relative to entry base at struct+0x5930):
 *   +0x58 src_cksum, +0x5C dst_cksum, +0x6C step_count,
 *   +0x70 cur_step, +0x74 byte_ofs, +0x7C capacity, +0x80 buf_ptr
 */

#define VA_MM_ALLOC  mega_va(0x004a4b80)
#define VA_MM_FREE   mega_va(0x004dd8b0)

typedef void *(__cdecl *fn_mm_alloc_t)(int size);
typedef void  (__cdecl *fn_mm_free_t)(void *ptr);

typedef struct {
    unsigned int   cksum;
    unsigned short flags;
    char           dir[32];
} dynpath_step_t;

static void api_inject_command(const char *cmd);

/* ---- Dynamic Path Walker ----
 * Replicates FUN_00404ed0 logic from Ghidra decompilation (2026-04-18).
 * MegaMUD stays in MODE=11 (idle). We send one direction at a time.
 * Between steps we check the EXACT same flags MegaMUD checks:
 *   dead, held, blinded, in-combat, HP/mana thresholds, sneak, hide.
 * MegaMUD's own subsystems handle auto-combat, auto-rest, auto-heal.
 * Our job: wait for those to finish, then send next direction.
 * Direction comes from BFS path, not .mp file. No room DB needed. */

enum {
    DPW_DONE = 0,
    DPW_READY,
    DPW_ARRIVE_SETTLE,
    DPW_SNEAK_WAIT,
    DPW_HIDE_WAIT,
    DPW_DIR_SENT,
    DPW_COMBAT_WAIT,
    DPW_REST_WAIT
};

#define DPW_MOVE_TIMEOUT_MS   15000
#define DPW_SNEAK_TIMEOUT_MS   5000
#define DPW_ARRIVE_SETTLE_MS    600
#define DPW_HIDE_TIMEOUT_MS    5000
#define DPW_REST_CHECK_MS      1000
#define DPW_COMBAT_SETTLE_MS    500

/* Shared state (cross-thread handoff from vk_mudmap → main thread) */
static volatile LONG   dp_pending = 0;
static dynpath_step_t *dp_steps   = NULL;
static int             dp_count   = 0;
static unsigned int    dp_src     = 0;
static unsigned int    dp_dst     = 0;

static volatile LONG   dp_active  = 0;
static unsigned int    dp_active_dst = 0;
static volatile LONG   dp_suspend_events = 0;

/* Walker state (main thread only) */
static int              dpw_state = DPW_DONE;
static int              dpw_step  = 0;
static int              dpw_count = 0;
static dynpath_step_t  *dpw_steps = NULL;
static unsigned int     dpw_dst   = 0;
static unsigned int     dpw_last_cksum = 0;
static DWORD            dpw_timeout = 0;
static DWORD            dpw_combat_end_tick = 0;

static void dpw_tick(void)
{
    if (dpw_state == DPW_DONE || !struct_base) return;

    unsigned char *sbp = (unsigned char *)struct_base;

    /* --- Read flags exactly matching FUN_00404ed0 checks --- */
    int cur_hp      = *(int *)(sbp + 0x53D4);
    int max_hp      = *(int *)(sbp + 0x53DC);
    int cur_mana    = *(int *)(sbp + 0x53E0);
    int max_mana    = *(int *)(sbp + 0x53E8);
    int hp_full_pct = *(int *)(sbp + 0x3788);
    int hp_rest_pct = *(int *)(sbp + 0x378C);
    int mn_full_pct = *(int *)(sbp + 0x37A4);
    int mn_rest_pct = *(int *)(sbp + 0x37A8);
    int in_combat   = *(int *)(sbp + 0x5698);
    int is_resting  = *(int *)(sbp + 0x5678);
    int is_medit    = *(int *)(sbp + 0x567C);
    int is_held     = *(int *)(sbp + 0x56CC);
    int is_blinded  = *(int *)(sbp + 0x56AC);
    int ign_blind   = *(int *)(sbp + 0x3A38);
    int auto_combat = *(int *)(sbp + 0x4D00);
    int auto_sneak  = *(int *)(sbp + 0x4D20);
    int auto_hide   = *(int *)(sbp + 0x4D24);
    int is_sneaking = *(int *)(sbp + 0x5688);
    int is_hiding   = *(int *)(sbp + 0x5690);
    int busy_lock   = *(int *)(sbp + 0x4CFC);
    unsigned int room_ck = *(unsigned int *)(sbp + OFF_ROOM_CHECKSUM);

    switch (dpw_state) {
    case DPW_READY:
        /* -- Path complete? -- */
        if (dpw_step >= dpw_count) {
            dpw_state = DPW_DONE;
            InterlockedExchange(&dp_active, 0);
            logmsg("[dynpath] arrived at %08X (%d steps)\n", room_ck, dpw_count);
            break;
        }

        /* -- Exact MegaMUD early-exit checks from FUN_00404ed0 -- */
        if (busy_lock) break;
        if (cur_hp < 1) {
            logmsg("[dynpath] dead (HP=%d), aborting\n", cur_hp);
            dpw_state = DPW_DONE;
            InterlockedExchange(&dp_active, 0);
            break;
        }
        if (is_held) break;
        if (is_blinded && !ign_blind) break;

        /* -- Combat check (mirrors FUN_004066b0 + main flow) -- */
        if (in_combat) {
            if (auto_combat) {
                dpw_state = DPW_COMBAT_WAIT;
            } else {
                api_inject_command(dpw_steps[dpw_step].dir);
                dpw_last_cksum = room_ck;
                dpw_timeout = GetTickCount();
                dpw_step++;
                dpw_state = DPW_DIR_SENT;
            }
            break;
        }

        /* -- Post-combat rest check ONLY --
         * FUN_00404ed0 does NOT check IS_RESTING — sending a direction
         * breaks rest automatically. We only wait for rest recovery
         * after combat ends with low HP, so MegaMUD can auto-heal. */
        if (dpw_combat_end_tick) {
            int hp_rest_thresh = max_hp > 0 ? (max_hp * hp_rest_pct) / 100 : 0;
            if (cur_hp < hp_rest_thresh) {
                dpw_state = DPW_REST_WAIT;
                dpw_timeout = GetTickCount();
                dpw_combat_end_tick = 0;
                break;
            }
            if (GetTickCount() - dpw_combat_end_tick > DPW_COMBAT_SETTLE_MS) {
                dpw_combat_end_tick = 0;
            }
        }

        /* -- Sneak check (mirrors FUN_004066b0 lines 176-186) --
         * FUN_004066b0 checks: if monsters present, don't try to sneak.
         * We can't call FUN_00453960 but IN_COMBAT covers the main case. */
        if (auto_sneak && !is_sneaking) {
            api_inject_command("sneak");
            dpw_timeout = GetTickCount();
            dpw_state = DPW_SNEAK_WAIT;
            break;
        }

        /* -- Hide check -- */
        if (auto_hide && !is_hiding) {
            api_inject_command("hide");
            dpw_timeout = GetTickCount();
            dpw_state = DPW_HIDE_WAIT;
            break;
        }

        /* -- All clear: send direction (mirrors FUN_00404ed0 line 1199) -- */
        api_inject_command(dpw_steps[dpw_step].dir);
        dpw_last_cksum = room_ck;
        dpw_timeout = GetTickCount();
        dpw_step++;
        dpw_state = DPW_DIR_SENT;
        break;

    case DPW_SNEAK_WAIT:
        if (in_combat) {
            dpw_state = auto_combat ? DPW_COMBAT_WAIT : DPW_READY;
            break;
        }
        if (is_sneaking) {
            if (auto_hide && !is_hiding) {
                api_inject_command("hide");
                dpw_timeout = GetTickCount();
                dpw_state = DPW_HIDE_WAIT;
            } else {
                dpw_state = DPW_READY;
            }
            break;
        }
        if (GetTickCount() - dpw_timeout > DPW_SNEAK_TIMEOUT_MS) {
            dpw_state = DPW_READY;
        }
        break;

    case DPW_HIDE_WAIT:
        if (in_combat) {
            dpw_state = auto_combat ? DPW_COMBAT_WAIT : DPW_READY;
            break;
        }
        if (is_hiding) {
            dpw_state = DPW_READY;
            break;
        }
        if (GetTickCount() - dpw_timeout > DPW_HIDE_TIMEOUT_MS) {
            dpw_state = DPW_READY;
        }
        break;

    case DPW_ARRIVE_SETTLE:
        if (in_combat) {
            dpw_state = auto_combat ? DPW_COMBAT_WAIT : DPW_READY;
            break;
        }
        if (GetTickCount() - dpw_timeout >= DPW_ARRIVE_SETTLE_MS) {
            dpw_state = DPW_READY;
        }
        break;

    case DPW_DIR_SENT:
        if (room_ck != dpw_last_cksum && room_ck != 0) {
            InterlockedIncrement(&auto_rm_queue_count);
            type_into_megamud("rm");
            dpw_timeout = GetTickCount();
            dpw_state = DPW_ARRIVE_SETTLE;
            break;
        }
        if (in_combat) {
            if (auto_combat) {
                dpw_state = DPW_COMBAT_WAIT;
            }
            break;
        }
        if (GetTickCount() - dpw_timeout > DPW_MOVE_TIMEOUT_MS) {
            logmsg("[dynpath] move timeout at step %d, aborting\n", dpw_step);
            dpw_state = DPW_DONE;
            InterlockedExchange(&dp_active, 0);
        }
        break;

    case DPW_COMBAT_WAIT:
        if (!in_combat) {
            dpw_combat_end_tick = GetTickCount();
            dpw_state = DPW_READY;
        }
        break;

    case DPW_REST_WAIT: {
        /* Wait for HP >= HP_FULL_PCT and MANA >= MANA_FULL_PCT.
         * Mirrors FUN_00479dc0 rest-completion check exactly. */
        int hp_full_thresh = max_hp > 0 ? (max_hp * hp_full_pct) / 100 : 0;
        int mn_full_thresh = max_mana > 0 ? (max_mana * mn_full_pct) / 100 : 0;
        int hp_done = (cur_hp >= hp_full_thresh);
        int mn_done = (max_mana <= 0) || (cur_mana >= mn_full_thresh);

        if (in_combat) {
            dpw_state = auto_combat ? DPW_COMBAT_WAIT : DPW_READY;
            break;
        }

        if (hp_done && mn_done) {
            dpw_state = DPW_READY;
            break;
        }

        if (!is_resting && !is_medit && hp_done) {
            dpw_state = DPW_READY;
            break;
        }
        break;
    }
    }
}

static void dynpath_do_inject(void)
{
    if (!struct_base || !dp_steps || dp_count <= 0) return;

    dpw_state = DPW_DONE;
    if (dpw_steps) {
        HeapFree(GetProcessHeap(), 0, dpw_steps);
        dpw_steps = NULL;
    }

    int sz = dp_count * (int)sizeof(dynpath_step_t);
    dpw_steps = (dynpath_step_t *)HeapAlloc(GetProcessHeap(), 0, sz);
    if (!dpw_steps) return;
    memcpy(dpw_steps, dp_steps, sz);

    dpw_count = dp_count;
    dpw_dst   = dp_dst;
    dpw_step  = 0;
    dpw_last_cksum = *(unsigned int *)((unsigned char *)struct_base + OFF_ROOM_CHECKSUM);
    dpw_timeout = GetTickCount();

    dp_active_dst = dp_dst;
    InterlockedExchange(&dp_active, 1);
    dpw_state = DPW_READY;

    logmsg("[dynpath] walk started: %d steps, src=%08X dst=%08X\n",
           dpw_count, dp_src, dp_dst);
}

static void dynpath_check_arrival(void)
{
    dpw_tick();
}

int WINAPI dynpath_inject(dynpath_step_t *steps, int count,
                          unsigned int src_cksum, unsigned int dst_cksum)
{
    if (!struct_base || !steps || count <= 0) return -1;

    dynpath_step_t *copy = (dynpath_step_t *)HeapAlloc(
        GetProcessHeap(), 0, count * (int)sizeof(dynpath_step_t));
    if (!copy) return -2;
    memcpy(copy, steps, count * sizeof(dynpath_step_t));

    dynpath_step_t *old = (dynpath_step_t *)InterlockedExchangePointer(
        (void *volatile *)&dp_steps, copy);
    if (old) HeapFree(GetProcessHeap(), 0, old);

    dp_count = count;
    dp_src   = src_cksum;
    dp_dst   = dst_cksum;
    InterlockedExchange(&dp_pending, 1);

    return 0;
}

void WINAPI dynpath_set_suspend_events(int val)
{
    InterlockedExchange(&dp_suspend_events, val ? 1 : 0);
}

int WINAPI dynpath_get_suspend_events(void)
{
    return (int)dp_suspend_events;
}

int WINAPI get_player_strength(void)
{
    if (!struct_base) return 0;
    return *(int *)(struct_base + 0x53F4);
}

int WINAPI get_player_picklocks(void)
{
    if (!struct_base) return 0;
    return *(int *)(struct_base + 0x541C);
}

/* Called from the ReadFile hook (main thread) to drain pending injections
 * and detect arrival at the dynamic path destination. */
static void dynpath_check_pending(void)
{
    if (InterlockedCompareExchange(&dp_pending, 0, 1) == 1) {
        dynpath_do_inject();
        dynpath_step_t *old = (dynpath_step_t *)InterlockedExchangePointer(
            (void *volatile *)&dp_steps, NULL);
        if (old) HeapFree(GetProcessHeap(), 0, old);
    }
    dynpath_check_arrival();
}

/* ---- Auto struct finder thread ---- */
/* Gets struct base directly from MMMAIN window extra data (offset 4),
   as documented in MEGAMUD_RE.md: GetWindowLongA(hWnd, 4) */

static DWORD WINAPI auto_struct_thread(LPVOID param)
{
    (void)param;
    while (1) {
        Sleep(1000);
        if (struct_base) continue;  /* already found */

        HWND mw = FindWindowA("MMMAIN", NULL);
        if (!mw) continue;

        DWORD base = (DWORD)GetWindowLongA(mw, 4);
        if (base) {
            struct_base = base;
            /* Read player name from the struct */
            char *name = (char *)(base + 0x537C);
            if (name[0] >= 'A' && name[0] <= 'Z') {
                strncpy(cfg_player_name, name, sizeof(cfg_player_name) - 1);
                cfg_player_name[sizeof(cfg_player_name) - 1] = '\0';
            }
            logmsg("[mudplugin] Struct base from MMMAIN window data: 0x%08lX (player: %s)\n",
                   base, cfg_player_name);
        }
    }
    return 0;
}

/* ---- Unhandled exception filter — captures crashes on ANY thread ---- */
static LONG WINAPI slop_crash_filter(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *cx = ep->ContextRecord;
    logmsg("[CRASH] code=0x%08X addr=0x%08X thread=%lu\n",
           (DWORD)er->ExceptionCode, (DWORD)er->ExceptionAddress, GetCurrentThreadId());
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        logmsg("[CRASH] access violation: %s 0x%08X\n",
               er->ExceptionInformation[0] == 0 ? "read" :
               er->ExceptionInformation[0] == 1 ? "write" : "exec",
               (DWORD)er->ExceptionInformation[1]);
    }
    if (cx) {
        logmsg("[CRASH] eip=0x%08X eax=0x%08X ebx=0x%08X ecx=0x%08X edx=0x%08X\n",
               (DWORD)cx->Eip, (DWORD)cx->Eax, (DWORD)cx->Ebx, (DWORD)cx->Ecx, (DWORD)cx->Edx);
        logmsg("[CRASH] esi=0x%08X edi=0x%08X esp=0x%08X ebp=0x%08X\n",
               (DWORD)cx->Esi, (DWORD)cx->Edi, (DWORD)cx->Esp, (DWORD)cx->Ebp);
        /* Walk a few frames of the stack to give us a poor-man's backtrace */
        DWORD *sp = (DWORD *)cx->Esp;
        for (int i = 0; i < 16 && sp; i++) {
            logmsg("[CRASH] stack[%02d] @0x%08X = 0x%08X\n", i, (DWORD)(sp + i), sp[i]);
        }
    }
    /* Identify which module the crash address lives in */
    HMODULE mods[64];
    DWORD needed = 0;
    /* Can't easily enumerate modules without psapi, but we can check a few known ones */
    HMODULE our_dll = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)er->ExceptionAddress, &our_dll);
    if (our_dll) {
        char modname[MAX_PATH] = {0};
        GetModuleFileNameA(our_dll, modname, sizeof(modname));
        logmsg("[CRASH] faulting module: %s @ base 0x%08X (offset 0x%08X)\n",
               modname, (DWORD)our_dll, (DWORD)er->ExceptionAddress - (DWORD)our_dll);
    }
    (void)mods; (void)needed;
    if (logfile) { fflush(logfile); _commit(_fileno(logfile)); }
    return EXCEPTION_CONTINUE_SEARCH;  /* let normal crash handling run after logging */
}

/* First-chance exception handler — runs BEFORE any __try/__except,
 * so it sees exceptions that get swallowed silently too. We filter
 * out the common noise codes (debugger strings, C++ throws, etc). */
static LONG WINAPI slop_vectored_handler(EXCEPTION_POINTERS *ep)
{
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Skip noise */
    if (code == 0x40010006 /* DBG_PRINTEXCEPTION_C */ ||
        code == 0x4001000A /* DBG_PRINTEXCEPTION_WIDE_C */ ||
        code == 0x406D1388 /* MS_VC_EXCEPTION (thread name) */ ||
        code == 0x40010005 /* DBG_CONTROL_C */ ||
        code == 0xE06D7363 /* C++ throw */ ||
        code == 0x80000003 /* breakpoint */ ||
        code == 0x4000001F /* WOW64 single-step */) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE mod = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ep->ExceptionRecord->ExceptionAddress, &mod);
    char modname[MAX_PATH] = {0};
    if (mod) GetModuleFileNameA(mod, modname, sizeof(modname));
    DWORD offset = mod ? ((DWORD)ep->ExceptionRecord->ExceptionAddress - (DWORD)mod) : 0;
    logmsg("[VEH] first-chance exc=0x%08X addr=0x%08X %s+0x%X tid=%lu\n",
           code, (DWORD)ep->ExceptionRecord->ExceptionAddress,
           modname[0] ? modname : "?", offset, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        logmsg("[VEH] AV %s @ 0x%08X\n",
               ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" :
               ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "exec",
               (DWORD)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    if (logfile) { fflush(logfile); _commit(_fileno(logfile)); }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void slop_atexit_log(void)
{
    logmsg("[EXIT] atexit handler fired — process returning normally (tid=%lu)\n",
           GetCurrentThreadId());
    if (logfile) { fflush(logfile); _commit(_fileno(logfile)); }
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

    /* Hook FUN_0041C8D0 (incoming data processor) for raw ANSI interception.
     * MegaMUD uses ReadFile/WriteFile on pipes, NOT winsock recv().
     * This inline hook taps data before it reaches MMANSI. */
    if (hook_process_incoming()) {
        logmsg("[mudplugin] Incoming data hook installed — raw ANSI active\n");
    } else {
        logmsg("[mudplugin] Incoming data hook FAILED — falling back to MMANSI polling\n");
    }

    /* Compat shim: dedupe HIMAGELIST double-destroy (native Win11 vs Wine).
     * megamud.exe's toolbar init path double-destroys an HIMAGELIST; Wine
     * is silent, native Win11 AVs. See MEGAMUD_RE.md "HIMAGELIST Double-
     * Free — Code Map". Must run before megamud touches the toolbar. */
    if (hook_imagelist_destroy()) {
        logmsg("[mudplugin] ImageList_Destroy dedupe shim installed\n");
    } else {
        logmsg("[mudplugin] ImageList_Destroy shim install FAILED — native Win11 may crash\n");
    }

    /* Log every ExitProcess/TerminateProcess so silent deaths tell us who/why */
    if (hook_exit_logging()) {
        logmsg("[mudplugin] Exit logging hooks installed (ExitProcess + TerminateProcess)\n");
    } else {
        logmsg("[mudplugin] Exit logging hook install FAILED\n");
    }

    /* Legacy recv hook — DISABLED. MegaMUD uses ReadFile, not winsock recv(),
     * so this hook was always useless. On Windows 11 with ASLR it also
     * crashes inside the PE/IAT walker. The FUN_0041C8D0 hook above gives
     * us the same data (raw ANSI stream) without any of the hazard. */
    logmsg("[mudplugin] recv() hook skipped — using FUN_0041C8D0 for incoming data\n");
    logmsg("[trace] post-recv checkpoint A\n");

    /* Server data injection uses direct buffer write (no hooks needed) */
    logmsg("[mudplugin] Server injection ready (direct buffer at +0x863D)\n");

    /* Inject menu into MegaMUD */
    logmsg("[trace] FindWindowA(MMMAIN)...\n");
    HWND main_wnd = FindWindowA("MMMAIN", NULL);
    logmsg("[trace] MMMAIN hwnd=0x%08X\n", (DWORD)main_wnd);
    if (main_wnd) {
        logmsg("[trace] calling inject_menu()\n");
        inject_menu(main_wnd);
        logmsg("[trace] inject_menu() returned\n");
    } else {
        logmsg("[mudplugin] MMMAIN not found yet, will retry...\n");
        for (int i = 0; i < 10; i++) {
            Sleep(1000);
            main_wnd = FindWindowA("MMMAIN", NULL);
            if (main_wnd) {
                logmsg("[trace] MMMAIN found on retry %d, calling inject_menu()\n", i);
                inject_menu(main_wnd);
                logmsg("[trace] inject_menu() returned (retry path)\n");
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
    if (!slop_menu) { logmsg("[menu] add_menu_item('%s', %u) — no menu!\n", label, id); return 0; }
    AppendMenuA(slop_menu, MF_STRING, id, label);
    logmsg("[menu] Added '%s' (id=%u) to MegaMud+ menu\n", label, id);
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

/* Server data injection API — writes directly to MegaMUD's data buffer */
static void api_inject_server_data(const char *data, int len)
{
    inject_server_data_impl(data, len);
}

/* Fake remote — call @command handlers without telepath */
static int api_fake_remote(const char *cmd)
{
    return fake_remote_impl(cmd);
}

/* ================================================================
 * Path Auto-Import — add .mp file to MegaMUD's path database
 * without triggering dialogs, with full pathing state preservation.
 * Ghidra RE of FUN_00464c50 (scanner), FUN_0045f100 (alloc),
 * FUN_0045fe20 (exists), FUN_00460050 (parse), FUN_00427d20 (menu).
 * ================================================================ */

typedef void  *(__cdecl *fn_path_alloc)(int);
typedef unsigned int (__cdecl *fn_path_exists)(int, unsigned char *);
typedef unsigned int (__cdecl *fn_parse_mp)(int, char *, int);
typedef void  (__cdecl *fn_update_paths)(int *, int);
typedef void  (__cdecl *fn_ini_write_int)(int, const char *, const char *, unsigned int);

#define PATH_RECORD_SIZE 0x84
#define PATH_RECORD_DWORDS 0x21

struct path_state_snapshot {
    int   on_entry;
    int   mode;
    int   steps_remaining;
    int   pathing_active;
    int   looping;
    char  path_file[76];
    char  path_wp[32];
    char  loop_entry_buf[PATH_RECORD_SIZE];
    int   loop_resume_a;
    int   loop_resume_b;
    char  sel_path[14];
    int   cur_path_step;
};

static void save_path_state(unsigned char *sbp, struct path_state_snapshot *snap)
{
    snap->on_entry       = *(int *)(sbp + OFF_FR_ON_ENTRY);
    snap->mode           = *(int *)(sbp + OFF_FR_MODE);
    snap->steps_remaining= *(int *)(sbp + OFF_FR_STEPS_REM);
    snap->pathing_active = *(int *)(sbp + OFF_FR_PATHING);
    snap->looping        = *(int *)(sbp + OFF_FR_LOOPING);
    memcpy(snap->path_file, sbp + OFF_FR_PATH_FILE, 76);
    memcpy(snap->path_wp,   sbp + OFF_FR_PATH_WP, 32);
    memcpy(snap->loop_entry_buf, sbp + OFF_FR_LOOP_DEST, PATH_RECORD_SIZE);
    snap->loop_resume_a  = *(int *)(sbp + OFF_FR_LOOP_RESUME_A);
    snap->loop_resume_b  = *(int *)(sbp + OFF_FR_LOOP_RESUME_B);
    memcpy(snap->sel_path, sbp + OFF_FR_SEL_PATH, 14);
    snap->cur_path_step  = *(int *)(sbp + OFF_CUR_PATH_STEP);
}

static void restore_path_state(unsigned char *sbp, const struct path_state_snapshot *snap)
{
    *(int *)(sbp + OFF_FR_ON_ENTRY)      = snap->on_entry;
    *(int *)(sbp + OFF_FR_MODE)          = snap->mode;
    *(int *)(sbp + OFF_FR_STEPS_REM)     = snap->steps_remaining;
    *(int *)(sbp + OFF_FR_PATHING)       = snap->pathing_active;
    *(int *)(sbp + OFF_FR_LOOPING)       = snap->looping;
    memcpy(sbp + OFF_FR_PATH_FILE, snap->path_file, 76);
    memcpy(sbp + OFF_FR_PATH_WP,   snap->path_wp, 32);
    memcpy(sbp + OFF_FR_LOOP_DEST, snap->loop_entry_buf, PATH_RECORD_SIZE);
    *(int *)(sbp + OFF_FR_LOOP_RESUME_A) = snap->loop_resume_a;
    *(int *)(sbp + OFF_FR_LOOP_RESUME_B) = snap->loop_resume_b;
    memcpy(sbp + OFF_FR_SEL_PATH, snap->sel_path, 14);
    *(int *)(sbp + OFF_CUR_PATH_STEP)    = snap->cur_path_step;
}

static int api_import_path(const char *mp_filepath)
{
    if (!struct_base || !mp_filepath || !mp_filepath[0]) return -1;

    int sb = (int)struct_base;
    unsigned char *sbp = (unsigned char *)struct_base;

    const char *basedir = (const char *)(sbp + OFF_FR_BASEDIR);
    if (!basedir[0]) {
        logmsg("[import] No MegaMUD base directory\n");
        return -2;
    }

    /* Extract just the filename from the full path */
    const char *fname = mp_filepath;
    for (const char *p = mp_filepath; *p; p++) {
        if (*p == '\\' || *p == '/') fname = p + 1;
    }
    if (!fname[0] || strlen(fname) > 13) {
        logmsg("[import] Invalid filename: %s\n", fname);
        return -3;
    }

    /* Copy .mp to Default\ so VA_PARSE_MP can find it */
    char dest_path[MAX_PATH];
    wsprintfA(dest_path, "%s\\Default\\%s", basedir, fname);
    if (!CopyFileA(mp_filepath, dest_path, FALSE)) {
        logmsg("[import] Failed to copy %s -> %s (err=%lu)\n",
               mp_filepath, dest_path, GetLastError());
        return -4;
    }
    logmsg("[import] Copied %s -> %s\n", mp_filepath, dest_path);

    /* Check if already registered */
    fn_path_exists path_exists = (fn_path_exists)VA_PATH_EXISTS;
    unsigned int existing = path_exists(sb, (unsigned char *)fname);
    if (existing) {
        logmsg("[import] Path %s already registered, re-parsing\n", fname);
        /* Re-parse into existing record */
        fn_parse_mp parse_mp = (fn_parse_mp)VA_PARSE_MP;
        parse_mp(sb, (char *)existing, 0);
        fn_update_paths update = (fn_update_paths)VA_UPDATE_PATHS;
        update((int *)sbp, 1);
        return 0;
    }

    /* Save pathing state */
    struct path_state_snapshot snap;
    save_path_state(sbp, &snap);
    logmsg("[import] Saved pathing state (mode=%d step=%d path=%s)\n",
           snap.mode, snap.cur_path_step, snap.path_file);

    /* Build local 0x84-byte record and parse */
    char record[PATH_RECORD_SIZE];
    memset(record, 0, PATH_RECORD_SIZE);
    record[0] = 0;  /* type = Default */
    strncpy(record + 0x0C, fname, 14);
    record[0x0C + 13] = '\0';

    fn_parse_mp parse_mp = (fn_parse_mp)VA_PARSE_MP;
    unsigned int parse_ok = parse_mp(sb, record, 0);
    if (!parse_ok) {
        logmsg("[import] VA_PARSE_MP failed for %s\n", fname);
        restore_path_state(sbp, &snap);
        return -5;
    }

    /* Set imported flag */
    *(unsigned int *)(record + 0x60) |= 0x10000000;

    /* Allocate slot and copy record */
    fn_path_alloc path_alloc = (fn_path_alloc)VA_PATH_ALLOC;
    void *slot = path_alloc(sb);
    if (!slot) {
        logmsg("[import] VA_PATH_ALLOC failed\n");
        restore_path_state(sbp, &snap);
        return -6;
    }

    unsigned int *dst = (unsigned int *)slot;
    unsigned int *src = (unsigned int *)record;
    for (int i = 0; i < PATH_RECORD_DWORDS; i++)
        dst[i] = src[i];

    *(int *)(sbp + OFF_FR_PATH_DIRTY) = 1;

    /* Restore pathing state before menu update */
    restore_path_state(sbp, &snap);
    logmsg("[import] Restored pathing state\n");

    /* Refresh Paths menu (skip VA_PATH_FINALIZE to avoid room dialogs) */
    fn_update_paths update = (fn_update_paths)VA_UPDATE_PATHS;
    update((int *)sbp, 1);

    logmsg("[import] Path %s imported successfully\n", fname);
    return 0;
}

/* ---- In-memory Rooms.md lookup table ---- */

typedef struct {
    unsigned int cksum;
    char code[5];
    char area[48];
    char name[64];
} room_entry_t;

static room_entry_t *room_table = NULL;
static int room_table_count = 0;
static int room_table_cap = 0;
static int room_table_loaded = 0;

static void room_table_load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strlen(line) < 10 || line[8] != ':') continue;
        unsigned int ck = 0;
        for (int i = 0; i < 8; i++) {
            char c = line[i];
            int v = 0;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else goto skip;
            ck = (ck << 4) | v;
        }
        {
            /* Parse colon fields: cksum:flags:0:0:0:CODE:AREA:NAME */
            char *fields[8] = {0};
            char buf[512];
            lstrcpynA(buf, line, sizeof(buf));
            char *p = buf;
            int fi = 0;
            fields[0] = p;
            while (*p && fi < 7) {
                if (*p == ':') { *p = '\0'; fi++; fields[fi] = p + 1; }
                p++;
            }
            /* Trim trailing newline from last field */
            if (fields[fi]) {
                char *nl = strchr(fields[fi], '\n');
                if (nl) *nl = '\0';
                nl = strchr(fields[fi], '\r');
                if (nl) *nl = '\0';
            }

            if (room_table_count >= room_table_cap) {
                room_table_cap = room_table_cap ? room_table_cap * 2 : 1024;
                room_table = realloc(room_table, room_table_cap * sizeof(room_entry_t));
            }
            room_entry_t *e = &room_table[room_table_count++];
            e->cksum = ck;
            e->code[0] = 0;
            e->area[0] = 0;
            e->name[0] = 0;
            if (fields[5]) lstrcpynA(e->code, fields[5], 5);
            if (fields[6]) lstrcpynA(e->area, fields[6], sizeof(e->area));
            if (fields[7]) lstrcpynA(e->name, fields[7], sizeof(e->name));
        }
        skip:;
    }
    fclose(f);
}

static void room_table_reload(void)
{
    room_table_count = 0;
    unsigned int sb = struct_base;
    if (!sb) return;
    unsigned char *sbp = (unsigned char *)sb;
    const char *basedir = (const char *)(sbp + OFF_FR_BASEDIR);

    char path[MAX_PATH];
    wsprintfA(path, "%s\\Default\\Rooms.md", basedir);
    room_table_load_file(path);
    wsprintfA(path, "%s\\Chars\\All\\Rooms.md", basedir);
    room_table_load_file(path);
    logmsg("[rooms] Loaded %d room entries into RAM\n", room_table_count);
    room_table_loaded = 1;
}

static int room_table_find(unsigned int cksum, char *out_code, int out_sz)
{
    if (!room_table_loaded) room_table_reload();
    for (int i = 0; i < room_table_count; i++) {
        if (room_table[i].cksum == cksum) {
            if (out_code) lstrcpynA(out_code, room_table[i].code, out_sz);
            return 1;
        }
    }
    return 0;
}

/* ---- Code Room: add room to MegaMUD's Rooms.md and reload ---- */

typedef unsigned int (__attribute__((stdcall)) *fn_load_all)(int *st, unsigned int flags);

static int api_code_room(unsigned int cksum, const char *code,
                         const char *area, const char *room_name)
{
    unsigned int sb = struct_base;
    if (!sb) return -1;
    unsigned char *sbp = (unsigned char *)sb;
    const char *basedir = (const char *)(sbp + OFF_FR_BASEDIR);

    if (!code || strlen(code) < 1 || strlen(code) > 4) {
        logmsg("[code_room] Invalid code '%s'\n", code ? code : "NULL");
        return -2;
    }

    char existing[8] = "";
    if (room_table_find(cksum, existing, sizeof(existing))) {
        logmsg("[code_room] Room %08X already known as '%s'\n", cksum, existing);
        return 1;
    }

    /* Append to Chars/All/Rooms.md */
    char chars_dir[MAX_PATH], custom_rooms[MAX_PATH];
    wsprintfA(chars_dir, "%s\\Chars", basedir);
    CreateDirectoryA(chars_dir, NULL);
    wsprintfA(chars_dir, "%s\\Chars\\All", basedir);
    CreateDirectoryA(chars_dir, NULL);
    wsprintfA(custom_rooms, "%s\\Chars\\All\\Rooms.md", basedir);

    FILE *f = fopen(custom_rooms, "a");
    if (!f) {
        logmsg("[code_room] Failed to open %s for append\n", custom_rooms);
        return -3;
    }
    fprintf(f, "%08X:00000000:0:0:0:%s:%s:%s\n",
            cksum,
            code,
            area ? area : "Unknown",
            room_name ? room_name : "Unknown Room");
    fclose(f);
    logmsg("[code_room] Added %08X:%s:%s:%s\n", cksum, code,
           area ? area : "", room_name ? room_name : "");

    /* Save pathing state, reload all data, restore state */
    struct path_state_snapshot snap;
    save_path_state(sbp, &snap);

    fn_load_all loader = (fn_load_all)VA_LOAD_ALL_DATA;
    loader((int *)sbp, 1);

    restore_path_state(sbp, &snap);

    /* Refresh our in-memory table too */
    room_table_reload();

    logmsg("[code_room] Done — state restored, %d rooms in table\n", room_table_count);
    return 0;
}

static int api_room_is_known(unsigned int cksum, char *out_code, int out_sz)
{
    return room_table_find(cksum, out_code, out_sz);
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
    slop_api.inject_server_data = api_inject_server_data;
    slop_api.fake_remote        = api_fake_remote;
    slop_api.strip_ansi         = strip_ansi;
    slop_api.import_path        = api_import_path;
    slop_api.code_room          = api_code_room;
    slop_api.room_is_known      = api_room_is_known;
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

/* Notify plugins that implement on_clean_line with ANSI already stripped.
 * Plugins doing substring/prefix matching should prefer this over on_line. */
static void plugins_on_clean_line(const char *line)
{
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].loaded && plugins[i].desc->on_clean_line)
            plugins[i].desc->on_clean_line(line);
    }
}

/* Notify plugins that implement on_data with raw server bytes */
static void plugins_on_data(const char *data, int len)
{
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].loaded && plugins[i].desc->on_data)
            plugins[i].desc->on_data(data, len);
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

    /* First pass: count plugin files so splash progress can report N/total */
    int plugin_total = 0;
    {
        WIN32_FIND_DATAA cfd;
        HANDLE ch = FindFirstFileA(search, &cfd);
        if (ch != INVALID_HANDLE_VALUE) {
            do { plugin_total++; } while (FindNextFileA(ch, &cfd));
            FindClose(ch);
        }
    }
    int plugin_idx = 0;
    SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "10");
    SetEnvironmentVariableA("SLOP_LOAD_STATUS", "Loading plugins...");

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

        /* Splash: announce plugin-by-plugin so any fatal load tells us which one */
        {
            char st[192];
            _snprintf(st, sizeof(st) - 1, "Loading plugin: %s", fd.cFileName);
            st[sizeof(st) - 1] = 0;
            SetEnvironmentVariableA("SLOP_LOAD_STATUS", st);
            char pct[16];
            int p = 10;
            if (plugin_total > 0) p = 10 + (plugin_idx * 35) / plugin_total;
            _snprintf(pct, sizeof(pct) - 1, "%d", p);
            pct[sizeof(pct) - 1] = 0;
            SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", pct);
        }

        HMODULE dll = LoadLibraryA(full_path);
        if (!dll) {
            logmsg("[plugins] FAILED to load %s (error %lu)\n", fd.cFileName, GetLastError());
            plugin_idx++;
            continue;
        }

        /* Look for the required export */
        slop_get_plugin_fn get_plugin = (slop_get_plugin_fn)GetProcAddress(dll, "slop_get_plugin");
        if (!get_plugin) {
            logmsg("[plugins] %s has no slop_get_plugin export — not a MegaMud+ plugin\n", fd.cFileName);
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
            logmsg("[plugins] %s: bad magic 0x%08X (expected 0x%08X) — not a MegaMud+ plugin\n",
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

        /* Call init — tell plugin its menu base ID */
        slop_api.menu_base_id = IDM_PLUGIN_BASE + plugin_count * 10;
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

        /* Check for TTS export: tts_speak(const char*, int, int) */
        tts_speak_fn speak_fn = (tts_speak_fn)GetProcAddress(dll, "tts_speak");
        if (speak_fn) {
            tts_engines[tts_engine_count].speak = speak_fn;
            tts_engines[tts_engine_count].name = desc->name;
            tts_engines[tts_engine_count].plugin_idx = plugin_count;
            tts_engine_count++;
            logmsg("[plugins] TTS engine found: %s\n", desc->name);
        }

        plugin_count++;
        plugin_idx++;

        logmsg("[plugins] Loaded: %s v%s by %s — %s\n",
               desc->name ? desc->name : fd.cFileName,
               desc->version ? desc->version : "?",
               desc->author ? desc->author : "?",
               desc->description ? desc->description : "");

        /* Splash: mark plugin as done */
        {
            char st[192];
            _snprintf(st, sizeof(st) - 1, "Loaded: %s",
                      desc->name ? desc->name : fd.cFileName);
            st[sizeof(st) - 1] = 0;
            SetEnvironmentVariableA("SLOP_LOAD_STATUS", st);
        }

    } while (FindNextFileA(h, &fd));
    SetEnvironmentVariableA("SLOP_LOAD_PROGRESS", "45");
    SetEnvironmentVariableA("SLOP_LOAD_STATUS", "All plugins loaded");

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

    MessageBoxA(parent, msg, "MegaMud+ Plugins", MB_OK | MB_ICONINFORMATION);
}

/* Case-insensitive substring search */
static char *slop_stristr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (_strnicmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
    }
    return NULL;
}

/* ---- Splash loading screen for --startvulkan ---- */

static volatile int splash_progress = 0;
static const char *splash_status = "Initializing...";

static LRESULT CALLBACK splash_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;

        /* Dark background */
        HBRUSH bgBrush = CreateSolidBrush(RGB(18, 18, 24));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        /* Scale factor based on window height */
        float sc = (float)h / 160.0f;
        if (sc < 1.0f) sc = 1.0f;
        int title_sz = (int)(28 * sc);
        int status_sz = (int)(16 * sc);
        int pct_sz = (int)(14 * sc);

        /* Title text */
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(200, 200, 220));
        HFONT titleFont = CreateFontA(title_sz, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(hdc, titleFont);
        RECT titleRc = {0, (int)(h * 0.12f), w, (int)(h * 0.40f)};
        DrawTextA(hdc, "MajorSLOP", -1, &titleRc, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(titleFont);

        /* Progress bar */
        int bar_margin = (int)(40 * sc);
        int bar_w = w - bar_margin * 2;
        int bar_h = (int)(18 * sc);
        int bar_x = bar_margin;
        int bar_y = (int)(h * 0.50f);
        int pen_w = (int)(2 * sc);
        if (pen_w < 1) pen_w = 1;

        /* Bar outline */
        HPEN borderPen = CreatePen(PS_SOLID, pen_w, RGB(80, 80, 120));
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, nullBrush);
        Rectangle(hdc, bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        /* Filled portion */
        int pct = splash_progress;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int fill_w = (bar_w - pen_w * 2) * pct / 100;
        if (fill_w > 0) {
            RECT fillRc = {bar_x + pen_w, bar_y + pen_w,
                           bar_x + pen_w + fill_w, bar_y + bar_h - pen_w};
            HBRUSH fillBr = CreateSolidBrush(RGB(60, 120, 220));
            FillRect(hdc, &fillRc, fillBr);
            DeleteObject(fillBr);
        }

        /* Percentage text */
        char pct_str[16];
        _snprintf(pct_str, sizeof(pct_str), "%d%%", pct);
        SetTextColor(hdc, RGB(160, 160, 200));
        HFONT pctFont = CreateFontA(pct_sz, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        oldFont = (HFONT)SelectObject(hdc, pctFont);
        RECT pctRc = {0, bar_y + bar_h + (int)(4 * sc), w, bar_y + bar_h + (int)(24 * sc)};
        DrawTextA(hdc, pct_str, -1, &pctRc, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(pctFont);

        /* Status text */
        SetTextColor(hdc, RGB(130, 130, 170));
        HFONT statusFont = CreateFontA(status_sz, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
        oldFont = (HFONT)SelectObject(hdc, statusFont);
        RECT statusRc = {0, (int)(h * 0.75f), w, h};
        DrawTextA(hdc, splash_status, -1, &statusRc, DT_CENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(statusFont);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        /* Poll env vars for progress updates from vk_terminal */
        char buf[64];
        if (GetEnvironmentVariableA("SLOP_LOAD_PROGRESS", buf, sizeof(buf)) > 0)
            splash_progress = atoi(buf);
        static char status_buf[128];
        if (GetEnvironmentVariableA("SLOP_LOAD_STATUS", status_buf, sizeof(status_buf)) > 0)
            splash_status = status_buf;
        InvalidateRect(lParam ? (HWND)lParam : wParam ? NULL : NULL, NULL, FALSE);
        InvalidateRect(FindWindowA("SLOP_SPLASH", NULL), NULL, FALSE);
        /* Auto-close when done */
        if (splash_progress >= 100) {
            DestroyWindow(FindWindowA("SLOP_SPLASH", NULL));
            return 0;
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Hide MegaMUD window ASAP for --startvulkan, show splash */
static DWORD WINAPI startvulkan_hide_thread(LPVOID param)
{
    (void)param;

    /* Hide MegaMUD window */
    for (int i = 0; i < 600; i++) {
        HWND mw = FindWindowA("MMMAIN", NULL);
        if (mw) {
            ShowWindow(mw, SW_HIDE);
            break;
        }
        Sleep(50);
    }

    /* Create splash window */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = splash_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "SLOP_SPLASH";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    /* Size: 480x160 base, scaled for DPI */
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int splash_w = sw / 3;  if (splash_w < 480) splash_w = 480;
    int splash_h = splash_w * 160 / 480;
    int splash_x = (sw - splash_w) / 2;
    int splash_y = (sh - splash_h) / 2;

    HWND splash = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        "SLOP_SPLASH", "Loading...",
        WS_POPUP | WS_VISIBLE,
        splash_x, splash_y, splash_w, splash_h,
        NULL, NULL, GetModuleHandle(NULL), NULL);

    if (splash) {
        SetTimer(splash, 1, 100, NULL);
        /* Message loop for splash */
        MSG msg;
        while (GetMessageA(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            /* Exit if splash was destroyed */
            if (!IsWindow(splash)) break;
        }
    }
    return 0;
}

/* ---- DllMain ---- */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        /* Install crash handler immediately — catches crashes on any thread
         * even before payload_main opens the logfile. slop_crash_filter
         * calls logmsg which tolerates NULL logfile (mirrors to OutputDebugString). */
        SetUnhandledExceptionFilter(slop_crash_filter);
        /* First-chance handler — runs before SEH, catches exceptions that
         * would otherwise be swallowed by __try/__except in megamud.exe */
        AddVectoredExceptionHandler(1 /*FIRST*/, slop_vectored_handler);
        /* Normal-exit log so we can distinguish "returned cleanly" from "crashed" */
        atexit(slop_atexit_log);
        /* Open logfile early so crash logs go to disk even before payload_main runs */
        if (!logfile) logfile = fopen("mudplugin.log", "a");
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
        /* Parse custom flags, show help or save to env vars.
         * Case-insensitive matching for all flags. */
        {
            /* Case-insensitive strstr helper (local to this block) */
            #define SLOP_STRISTR(hay, needle) slop_stristr(hay, needle)

            char *cmdline = GetCommandLineA();
            if (cmdline) {
                /* Help — print usage and kill the process */
                if (SLOP_STRISTR(cmdline, "--help") || SLOP_STRISTR(cmdline, "-help") ||
                    strstr(cmdline, "--?")           || strstr(cmdline, "-?")) {
                    const char *help =
                        "\nMajorSLOP - MegaMUD Plugin Framework\n"
                        "=====================================\n\n"
                        "Usage: MegaMud.exe [options]\n\n"
                        "Options:\n"
                        "  --startvulkan          Launch directly into Vulkan terminal\n"
                        "                         (hides the original MegaMUD window)\n"
                        "  --autoconnect          Auto-connect to BBS after startup\n"
                        "  --loop \"name.mp\"        Start a loop after connecting\n"
                        "                         (implies --autoconnect)\n"
                        "  --help, -help, --?, -?\n"
                        "                         Show this help and exit\n\n"
                        "Examples:\n"
                        "  MegaMud.exe --startvulkan --autoconnect\n"
                        "  MegaMud.exe --startvulkan --loop \"stontiny.mp\"\n\n";
                    DWORD written;
                    int printed = 0;
                    /* Attach to parent console (the terminal that launched wine) */
                    AttachConsole((DWORD)-1); /* ATTACH_PARENT_PROCESS */
                    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                    if (hOut && hOut != INVALID_HANDLE_VALUE) {
                        WriteFile(hOut, help, (DWORD)strlen(help), &written, NULL);
                        FlushFileBuffers(hOut);
                        printed = 1;
                    }
                    if (!printed) {
                        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
                        if (hErr && hErr != INVALID_HANDLE_VALUE) {
                            WriteFile(hErr, help, (DWORD)strlen(help), &written, NULL);
                            FlushFileBuffers(hErr);
                            printed = 1;
                        }
                    }
                    /* Last resort: MessageBox */
                    if (!printed)
                        MessageBoxA(NULL, help + 1, "MajorSLOP", MB_OK | MB_ICONINFORMATION);
                    ExitProcess(0);
                }
                if (SLOP_STRISTR(cmdline, "--startvulkan")) {
                    SetEnvironmentVariableA("SLOP_STARTVULKAN", "1");
                    /* Start a thread that hides MMMAIN the instant it appears */
                    CreateThread(NULL, 0, startvulkan_hide_thread, NULL, 0, NULL);
                }
                if (SLOP_STRISTR(cmdline, "--autoconnect") ||
                    SLOP_STRISTR(cmdline, "--c") ||
                    SLOP_STRISTR(cmdline, " -c"))
                    SetEnvironmentVariableA("SLOP_AUTOCONNECT", "1");

                /* --loop "name" or --loop 'name' — extract the loop name */
                {
                    char *lpos = SLOP_STRISTR(cmdline, "--loop");
                    if (lpos) {
                        /* --loop implies --autoconnect */
                        SetEnvironmentVariableA("SLOP_AUTOCONNECT", "1");
                        char *arg = lpos + 6; /* skip "--loop" */
                        while (*arg == ' ') arg++;
                        char loop_name[128] = {0};
                        if (*arg == '"' || *arg == '\'') {
                            char quote = *arg++;
                            int k = 0;
                            while (*arg && *arg != quote && k < 127)
                                loop_name[k++] = *arg++;
                        } else {
                            /* Unquoted — take until space or end */
                            int k = 0;
                            while (*arg && *arg != ' ' && k < 127)
                                loop_name[k++] = *arg++;
                        }
                        if (loop_name[0])
                            SetEnvironmentVariableA("SLOP_LOOP", loop_name);
                    }
                }

                /* --loadscripts name,name — extract comma-separated script list */
                {
                    char *lpos = SLOP_STRISTR(cmdline, "--loadscript");
                    if (lpos) {
                        char *arg = lpos + 12; /* skip "--loadscript" */
                        if (*arg == 's' || *arg == 'S') arg++; /* skip optional 's' */
                        /* Skip optional opening paren/quote/space */
                        while (*arg == ' ' || *arg == '(') arg++;
                        char scripts[512] = {0};
                        if (*arg == '"' || *arg == '\'') {
                            char quote = *arg++;
                            int k = 0;
                            while (*arg && *arg != quote && k < 511)
                                scripts[k++] = *arg++;
                        } else {
                            /* Unquoted — take until space, closing paren, or end */
                            int k = 0;
                            while (*arg && *arg != ' ' && *arg != ')' && k < 511)
                                scripts[k++] = *arg++;
                        }
                        /* Strip internal quotes from script names */
                        {
                            char clean[512] = {0};
                            int j = 0;
                            for (int i = 0; scripts[i] && j < 511; i++)
                                if (scripts[i] != '\'' && scripts[i] != '"')
                                    clean[j++] = scripts[i];
                            memcpy(scripts, clean, 512);
                        }
                        if (scripts[0])
                            SetEnvironmentVariableA("SLOP_LOADSCRIPTS", scripts);
                    }
                }

                /* Strip all custom flags from the PEB command line in-place.
                 * --loop and --loadscripts must be stripped with their arguments. */
                {
                    char *lpos = SLOP_STRISTR(cmdline, "--loop");
                    if (lpos) {
                        char *end = lpos + 6;
                        while (*end == ' ') end++;
                        if (*end == '"' || *end == '\'') {
                            char q = *end++;
                            while (*end && *end != q) end++;
                            if (*end == q) end++;
                        } else {
                            while (*end && *end != ' ') end++;
                        }
                        char *dst = lpos;
                        if (dst > cmdline && *(dst - 1) == ' ') dst--;
                        memmove(dst, end, strlen(end) + 1);
                    }
                }
                /* Strip --loadscripts and its argument (handles parens, quotes, bare) */
                {
                    char *lpos = SLOP_STRISTR(cmdline, "--loadscript");
                    if (lpos) {
                        char *end = lpos + 12;
                        if (*end == 's' || *end == 'S') end++;
                        /* Skip optional paren/space */
                        if (*end == '(') {
                            end++;
                            while (*end && *end != ')') end++;
                            if (*end == ')') end++;
                        } else {
                            while (*end == ' ') end++;
                            if (*end == '"' || *end == '\'') {
                                char q = *end++;
                                while (*end && *end != q) end++;
                                if (*end == q) end++;
                            } else {
                                while (*end && *end != ' ') end++;
                            }
                        }
                        char *dst = lpos;
                        if (dst > cmdline && *(dst - 1) == ' ') dst--;
                        memmove(dst, end, strlen(end) + 1);
                    }
                }
                {
                    static const char *strip_flags[] = {
                        "--startvulkan", "--autoconnect", "--c", "-c", NULL
                    };
                    for (int i = 0; strip_flags[i]; i++) {
                        char *pos;
                        while ((pos = SLOP_STRISTR(cmdline, strip_flags[i])) != NULL) {
                            int flen = (int)strlen(strip_flags[i]);
                            char *src = pos + flen;
                            char *dst = pos;
                            if (dst > cmdline && *(dst - 1) == ' ') dst--;
                            memmove(dst, src, strlen(src) + 1);
                        }
                    }
                }
            }
            #undef SLOP_STRISTR
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
