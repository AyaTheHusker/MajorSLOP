/*
 * gl_round_timer.c — GL Round Timer Plugin
 * =========================================
 *
 * Full-featured OpenGL combat round timer: sweeping arc, color gradient,
 * glow, particle trail, pulse-on-tick. Draggable, resizable, right-click
 * context menu with spell picker, double-click for manual sync.
 *
 * Measures round duration from live combat data (weighted rolling average).
 * Same features as GDI round timer but with GL effects.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o gl_round_timer.dll gl_round_timer.c \
 *       -lgdi32 -luser32 -lopengl32 -mwindows
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <GL/gl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Config ---- */

#define RT_DEFAULT_SIZE 160
#define RT_MIN_SIZE      80
#define RT_MAX_SIZE     400
#define RT_TIMER_MS      16    /* ~60fps */
#define IDM_RT_TOGGLE    50700

/* Context menu IDs */
#define IDM_RT_SYNC_SPELL   50710
#define IDM_RT_PERFORM_SYNC 50711
#define IDM_RT_HIDE         50712
#define IDM_RT_SCALE_HALF   50720
#define IDM_RT_SCALE_1X     50721
#define IDM_RT_SCALE_2X     50722
#define IDM_RT_PERIOD_40    50730
#define IDM_RT_PERIOD_45    50731
#define IDM_RT_PERIOD_50    50732
#define IDM_RT_PERIOD_55    50733
#define IDM_RT_PERIOD_60    50734

/* Color key for transparency */
#define KEY_R 2
#define KEY_G 0
#define KEY_B 2

/* ---- State ---- */

static const slop_api_t *api = NULL;
static HWND rt_hwnd = NULL;
static HDC  rt_hdc  = NULL;
static HGLRC rt_ctx = NULL;
static volatile int rt_running = 0;
static volatile int rt_visible = 0;
static int rt_win_size = RT_DEFAULT_SIZE;

/* Round timing — matches GDI timer's algorithm */
static volatile int    rt_round_num = 0;
static volatile int    rt_synced = 0;
static volatile int    rt_in_combat = 0;
static double          rt_last_tick_ts = 0.0;
static double          rt_round_period = 5000.0;
static double          rt_recent_intervals[12];
static int             rt_interval_count = 0;
static char            rt_delta_text[32] = "";
static double          rt_delta_fade = 0.0;
static volatile int    rt_flash_tick = 0;

/* Drag state */
static int rt_dragging = 0;
static POINT rt_drag_offset;

/* Manual sync */
static char rt_sync_spell[22] = {0};
static volatile int ms_state = 0;      /* 0=idle, 1=spamming, 2=done */
static volatile int ms_already_count = 0;
static volatile int ms_rounds_detected = 0;

/* Spell picker */
static HWND rt_spell_hwnd = NULL;
static const char *RT_SPELL_CLASS = "SlopGLSpellPicker";

/* Particle trail */
#define RT_MAX_PARTICLES 32
static struct {
    float x, y, vx, vy, alpha, r, g, b;
    int alive;
} rt_particles[RT_MAX_PARTICLES];
static int rt_particle_idx = 0;

/* Font display lists */
static UINT rt_font_big = 0;    /* round number */
static UINT rt_font_small = 0;  /* labels */

/* Forward declarations */
void gl_rt_show(void);
void gl_rt_hide(void);
void gl_rt_toggle(void);

/* ---- GL helpers ---- */

static void rt_draw_circle(float cx, float cy, float radius,
                           float r, float g, float b, float a, int segs)
{
    glBegin(GL_TRIANGLE_FAN);
    glColor4f(r, g, b, a * 0.3f);
    glVertex2f(cx, cy);
    glColor4f(r, g, b, a);
    for (int i = 0; i <= segs; i++) {
        float theta = 2.0f * (float)M_PI * (float)i / (float)segs;
        glVertex2f(cx + radius * cosf(theta), cy + radius * sinf(theta));
    }
    glEnd();
}

static void rt_draw_circle_solid(float cx, float cy, float radius,
                                 float r, float g, float b, float a, int segs)
{
    glBegin(GL_TRIANGLE_FAN);
    glColor4f(r, g, b, a);
    glVertex2f(cx, cy);
    for (int i = 0; i <= segs; i++) {
        float theta = 2.0f * (float)M_PI * (float)i / (float)segs;
        glVertex2f(cx + radius * cosf(theta), cy + radius * sinf(theta));
    }
    glEnd();
}

static void rt_draw_arc(float cx, float cy, float r_inner, float r_outer,
                        float start, float end,
                        float r1, float g1, float b1,
                        float r2, float g2, float b2,
                        float alpha, int segs)
{
    if (end <= start) return;
    float step = (end - start) / (float)segs;
    glBegin(GL_QUAD_STRIP);
    for (int i = 0; i <= segs; i++) {
        float t = (float)i / (float)segs;
        float theta = start + step * (float)i;
        float cr = r1 + (r2 - r1) * t;
        float cg = g1 + (g2 - g1) * t;
        float cb = b1 + (b2 - b1) * t;
        glColor4f(cr, cg, cb, alpha);
        glVertex2f(cx + r_outer * cosf(theta), cy + r_outer * sinf(theta));
        glColor4f(cr * 0.6f, cg * 0.6f, cb * 0.6f, alpha * 0.5f);
        glVertex2f(cx + r_inner * cosf(theta), cy + r_inner * sinf(theta));
    }
    glEnd();
}

static void rt_spawn_particle(float x, float y, float r, float g, float b)
{
    int idx = rt_particle_idx % RT_MAX_PARTICLES;
    rt_particles[idx].x = x;
    rt_particles[idx].y = y;
    rt_particles[idx].vx = ((float)(rand() % 100) - 50.0f) / 2000.0f;
    rt_particles[idx].vy = ((float)(rand() % 100) - 50.0f) / 2000.0f;
    rt_particles[idx].alpha = 0.8f;
    rt_particles[idx].r = r;
    rt_particles[idx].g = g;
    rt_particles[idx].b = b;
    rt_particles[idx].alive = 1;
    rt_particle_idx++;
}

static void rt_draw_text(UINT font_base, const char *text,
                         float x, float y, float r, float g, float b, float a)
{
    if (!font_base) return;
    int len = (int)strlen(text);
    glColor4f(r, g, b, a);
    glRasterPos2f(x, y);
    glListBase(font_base);
    glCallLists(len, GL_UNSIGNED_BYTE, text);
}

/* ---- Rendering ---- */

static void rt_render(void)
{
    float cx = 0.0f, cy = 0.0f;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    DWORD now = GetTickCount();
    double now_d = (double)GetTickCount64();

    /* Progress calculation */
    float progress = 0.0f;
    if (rt_synced && rt_last_tick_ts > 0.0 && rt_round_period > 0.0) {
        double elapsed = now_d - rt_last_tick_ts;
        progress = (float)(elapsed / rt_round_period);
        if (progress > 1.5f) progress = 1.5f;
    }

    /* Background dark circle */
    rt_draw_circle_solid(cx, cy, 0.88f, 0.08f, 0.08f, 0.10f, 0.85f, 48);

    /* Tick marks — number based on period */
    int num_ticks = (int)(rt_round_period / 1000.0 + 0.5);
    if (num_ticks < 3) num_ticks = 5;
    if (num_ticks > 12) num_ticks = 12;
    glBegin(GL_LINES);
    glColor4f(0.3f, 0.3f, 0.38f, 0.5f);
    for (int i = 0; i < num_ticks; i++) {
        float theta = (float)M_PI / 2.0f - 2.0f * (float)M_PI * (float)i / (float)num_ticks;
        glVertex2f(cx + 0.72f * cosf(theta), cy + 0.72f * sinf(theta));
        glVertex2f(cx + 0.83f * cosf(theta), cy + 0.83f * sinf(theta));
    }
    glEnd();

    /* Color gradient: green → yellow → red */
    float p = progress > 1.0f ? 1.0f : progress;
    float sr, sg, sb, er, eg, eb;
    if (p < 0.5f) {
        float t = p * 2.0f;
        sr = 0.1f; sg = 0.9f; sb = 0.2f;
        er = 0.1f + 0.9f * t; eg = 0.9f; eb = 0.2f - 0.1f * t;
    } else {
        float t = (p - 0.5f) * 2.0f;
        sr = 1.0f; sg = 0.9f; sb = 0.1f;
        er = 1.0f; eg = 0.9f - 0.7f * t; eb = 0.1f;
    }

    /* Main arc — sweeps from 12 o'clock CW */
    if (progress > 0.01f && rt_synced) {
        float arc_start = (float)M_PI / 2.0f;
        float sweep_frac = progress > 1.0f ? 1.0f : progress;
        float arc_end = arc_start - 2.0f * (float)M_PI * sweep_frac;
        int segs = (int)(sweep_frac * 64.0f);
        if (segs < 4) segs = 4;

        /* Main arc */
        rt_draw_arc(cx, cy, 0.55f, 0.78f, arc_end, arc_start,
                    er, eg, eb, sr, sg, sb, 0.9f, segs);
        /* Glow (larger, dimmer) */
        rt_draw_arc(cx, cy, 0.50f, 0.83f, arc_end, arc_start,
                    er, eg, eb, sr, sg, sb, 0.15f, segs);

        /* Leading edge ball */
        float edge_x = cx + 0.66f * cosf(arc_end);
        float edge_y = cy + 0.66f * sinf(arc_end);
        rt_draw_circle_solid(edge_x, edge_y, 0.06f, er, eg, eb, 0.8f, 12);
        /* Edge glow */
        rt_draw_circle(edge_x, edge_y, 0.10f, er, eg, eb, 0.5f, 12);

        /* Particle spawn */
        if (rand() % 3 == 0)
            rt_spawn_particle(edge_x, edge_y, er, eg, eb);
    }

    /* Overdue pulse */
    if (progress > 1.0f) {
        float pulse = 0.3f + 0.2f * sinf((float)now / 150.0f);
        rt_draw_circle(cx, cy, 0.85f, 1.0f, 0.2f, 0.1f, pulse, 48);
    }

    /* Flash on round tick */
    if (rt_flash_tick > 0) {
        float a = (float)rt_flash_tick / 15.0f;
        rt_draw_circle_solid(cx, cy, 0.9f + a * 0.15f, 1.0f, 1.0f, 0.8f, a * 0.35f, 48);
        rt_flash_tick--;
    }

    /* Particles */
    glBegin(GL_QUADS);
    for (int i = 0; i < RT_MAX_PARTICLES; i++) {
        if (!rt_particles[i].alive) continue;
        rt_particles[i].x += rt_particles[i].vx;
        rt_particles[i].y += rt_particles[i].vy;
        rt_particles[i].alpha -= 0.015f;
        if (rt_particles[i].alpha <= 0.0f) { rt_particles[i].alive = 0; continue; }
        float s = 0.02f;
        float px = rt_particles[i].x, py = rt_particles[i].y;
        glColor4f(rt_particles[i].r, rt_particles[i].g, rt_particles[i].b,
                  rt_particles[i].alpha);
        glVertex2f(px - s, py - s);
        glVertex2f(px + s, py - s);
        glVertex2f(px + s, py + s);
        glVertex2f(px - s, py + s);
    }
    glEnd();

    /* Outer ring */
    glBegin(GL_LINE_LOOP);
    glColor4f(0.4f, 0.4f, 0.45f, 0.4f);
    for (int i = 0; i < 64; i++) {
        float theta = 2.0f * (float)M_PI * (float)i / 64.0f;
        glVertex2f(cx + 0.84f * cosf(theta), cy + 0.84f * sinf(theta));
    }
    glEnd();

    /* Center text */
    if (rt_font_big) {
        if (rt_synced && rt_round_num > 0) {
            char buf[16];
            sprintf(buf, "%d", rt_round_num);
            int len = (int)strlen(buf);
            float cw = 0.15f;
            float tx = cx - (float)len * cw / 2.0f;
            /* Shadow */
            rt_draw_text(rt_font_big, buf, tx + 0.02f, cy - 0.06f, 0,0,0, 0.6f);
            /* Text */
            rt_draw_text(rt_font_big, buf, tx, cy - 0.04f, 0.9f, 0.9f, 0.95f, 0.9f);
        } else if (ms_state == 1) {
            rt_draw_text(rt_font_small, "TIMING", cx - 0.28f, cy, 1.0f, 0.8f, 0.2f, 0.9f);
        } else {
            rt_draw_text(rt_font_small, "SYNC", cx - 0.18f, cy, 0.5f, 0.5f, 0.55f, 0.6f);
        }
    }

    /* Label above: "ROUND" */
    if (rt_synced && rt_font_small) {
        rt_draw_text(rt_font_small, "ROUND", cx - 0.22f, cy + 0.28f, 0.5f, 0.5f, 0.55f, 0.5f);
    }

    /* Delta text below */
    if (rt_delta_text[0] && rt_delta_fade > 0.01 && rt_font_small) {
        float dr = (rt_delta_text[0] == '+') ? 1.0f : 0.3f;
        float dg = (rt_delta_text[0] == '+') ? 0.5f : 1.0f;
        float db = 0.2f;
        int dlen = (int)strlen(rt_delta_text);
        float dtx = cx - (float)dlen * 0.06f / 2.0f;
        rt_draw_text(rt_font_small, rt_delta_text, dtx, cy - 0.35f,
                     dr, dg, db, (float)rt_delta_fade * 0.8f);
        rt_delta_fade -= 0.006;
    }

    /* Manual sync indicator */
    if (ms_state == 1 && rt_font_small) {
        float blink = 0.5f + 0.5f * sinf((float)now / 200.0f);
        rt_draw_text(rt_font_small, "...", cx - 0.08f, cy - 0.20f,
                     1.0f, 0.8f, 0.2f, blink);
    }

    /* Elapsed time readout */
    if (rt_synced && rt_last_tick_ts > 0.0 && rt_font_small) {
        double elapsed = now_d - rt_last_tick_ts;
        char ebuf[32];
        sprintf(ebuf, "%.1fs / %.1fs", elapsed / 1000.0, rt_round_period / 1000.0);
        int elen = (int)strlen(ebuf);
        float etx = cx - (float)elen * 0.05f / 2.0f;
        rt_draw_text(rt_font_small, ebuf, etx, cy - 0.50f, 0.4f, 0.4f, 0.45f, 0.4f);
    }
}

/* ---- Spell picker dialog ---- */

static LRESULT CALLBACK rt_spell_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HWND ed = NULL;
    switch (msg) {
    case WM_CREATE: {
        CreateWindowExA(0, "STATIC", "Spell command:",
                        WS_CHILD | WS_VISIBLE, 10, 10, 120, 20,
                        hwnd, NULL, GetModuleHandleA(NULL), NULL);
        ed = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", rt_sync_spell,
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             10, 32, 200, 22,
                             hwnd, (HMENU)100, GetModuleHandleA(NULL), NULL);
        SendMessageA(ed, EM_LIMITTEXT, 21, 0);
        CreateWindowExA(0, "BUTTON", "OK",
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        10, 62, 60, 26, hwnd, (HMENU)IDOK, GetModuleHandleA(NULL), NULL);
        CreateWindowExA(0, "BUTTON", "Cancel",
                        WS_CHILD | WS_VISIBLE,
                        80, 62, 60, 26, hwnd, (HMENU)IDCANCEL, GetModuleHandleA(NULL), NULL);
        CreateWindowExA(0, "BUTTON", "Clear",
                        WS_CHILD | WS_VISIBLE,
                        150, 62, 60, 26, hwnd, (HMENU)101, GetModuleHandleA(NULL), NULL);
        SetFocus(ed);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            char buf[32];
            GetWindowTextA(ed, buf, 22);
            /* Trim trailing spaces */
            int l = (int)strlen(buf);
            while (l > 0 && buf[l-1] == ' ') buf[--l] = '\0';
            strncpy(rt_sync_spell, buf, 21);
            rt_sync_spell[21] = '\0';
            if (api) api->log("[gl_round_timer] Sync spell set: '%s'\n", rt_sync_spell);
            DestroyWindow(hwnd);
            rt_spell_hwnd = NULL;
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hwnd);
            rt_spell_hwnd = NULL;
            return 0;
        }
        if (LOWORD(wParam) == 101) { /* Clear */
            SetWindowTextA(ed, "");
            rt_sync_spell[0] = '\0';
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        rt_spell_hwnd = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void rt_show_spell_picker(void)
{
    if (rt_spell_hwnd) { SetForegroundWindow(rt_spell_hwnd); return; }

    static int reg = 0;
    if (!reg) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = rt_spell_proc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = RT_SPELL_CLASS;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        reg = 1;
    }

    RECT wr;
    GetWindowRect(rt_hwnd, &wr);
    rt_spell_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        RT_SPELL_CLASS, "Manual Sync Spell",
        WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
        wr.left, wr.bottom + 5, 230, 100,
        rt_hwnd, NULL, GetModuleHandleA(NULL), NULL);
}

/* ---- Manual sync ---- */

static DWORD WINAPI ms_spam_thread(LPVOID param)
{
    (void)param;
    if (api) api->log("[gl_rt_mansync] Spam started: '%s'\n", rt_sync_spell);

    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) { ms_state = 0; return 0; }
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    HWND target = ansi ? ansi : mw;

    DWORD start = GetTickCount();
    while (ms_state == 1) {
        if (GetTickCount() - start > 10000) {
            if (api) api->log("[gl_rt_mansync] 10s complete, %d rounds\n", ms_rounds_detected);
            ms_state = 2;
            break;
        }
        for (int i = 0; rt_sync_spell[i]; i++)
            PostMessageA(target, WM_CHAR, (WPARAM)(unsigned char)rt_sync_spell[i], 0);
        PostMessageA(target, WM_CHAR, (WPARAM)'\r', 0);
        Sleep(150);
    }
    return 0;
}

static void rt_perform_manual_sync(void)
{
    if (!rt_sync_spell[0]) {
        rt_show_spell_picker();
        return;
    }
    if (ms_state == 1) {
        ms_state = 0;
        if (api) api->log("[gl_rt_mansync] Cancelled\n");
        return;
    }

    rt_synced = 0;
    rt_last_tick_ts = 0.0;
    rt_interval_count = 0;
    rt_round_period = 5000.0;
    rt_delta_text[0] = '\0';
    rt_delta_fade = 0.0;

    ms_state = 1;
    ms_already_count = 0;
    ms_rounds_detected = 0;
    CreateThread(NULL, 0, ms_spam_thread, NULL, 0, NULL);
}

/* ---- Context menu ---- */

static void rt_show_context_menu(HWND hwnd, int x, int y)
{
    HMENU menu = CreatePopupMenu();
    HMENU scale_sub = CreatePopupMenu();
    AppendMenuA(scale_sub, MF_STRING | (rt_win_size <= 100 ? MF_CHECKED : 0),
                IDM_RT_SCALE_HALF, "Small (80)");
    AppendMenuA(scale_sub, MF_STRING | (rt_win_size > 100 && rt_win_size <= 200 ? MF_CHECKED : 0),
                IDM_RT_SCALE_1X, "Normal (160)");
    AppendMenuA(scale_sub, MF_STRING | (rt_win_size > 200 ? MF_CHECKED : 0),
                IDM_RT_SCALE_2X, "Large (280)");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)scale_sub, "Size");

    HMENU period_sub = CreatePopupMenu();
    int cur_p = (int)(rt_round_period / 100.0 + 0.5);
    AppendMenuA(period_sub, MF_STRING | (cur_p == 40 ? MF_CHECKED : 0), IDM_RT_PERIOD_40, "4.0s");
    AppendMenuA(period_sub, MF_STRING | (cur_p == 45 ? MF_CHECKED : 0), IDM_RT_PERIOD_45, "4.5s");
    AppendMenuA(period_sub, MF_STRING | (cur_p == 50 ? MF_CHECKED : 0), IDM_RT_PERIOD_50, "5.0s");
    AppendMenuA(period_sub, MF_STRING | (cur_p == 55 ? MF_CHECKED : 0), IDM_RT_PERIOD_55, "5.5s");
    AppendMenuA(period_sub, MF_STRING | (cur_p == 60 ? MF_CHECKED : 0), IDM_RT_PERIOD_60, "6.0s");
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)period_sub, "Round Period");

    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_RT_SYNC_SPELL, "Set Timing Spell...");
    AppendMenuA(menu, MF_STRING | (rt_sync_spell[0] ? 0 : MF_GRAYED),
                IDM_RT_PERFORM_SYNC,
                ms_state == 1 ? "Stop Timing" : "Cast to Time");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_RT_HIDE, "Hide Round Timer");

    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static void rt_resize(int new_size)
{
    if (new_size < RT_MIN_SIZE) new_size = RT_MIN_SIZE;
    if (new_size > RT_MAX_SIZE) new_size = RT_MAX_SIZE;
    rt_win_size = new_size;
    if (rt_hwnd) {
        SetWindowPos(rt_hwnd, NULL, 0, 0, new_size, new_size,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        glViewport(0, 0, new_size, new_size);
    }
}

/* ---- Window procedure ---- */

static LRESULT CALLBACK rt_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;

    case WM_LBUTTONDOWN:
        /* Start drag */
        rt_dragging = 1;
        SetCapture(hwnd);
        POINT pt;
        GetCursorPos(&pt);
        RECT wr;
        GetWindowRect(hwnd, &wr);
        rt_drag_offset.x = pt.x - wr.left;
        rt_drag_offset.y = pt.y - wr.top;
        return 0;

    case WM_MOUSEMOVE:
        if (rt_dragging) {
            POINT cur;
            GetCursorPos(&cur);
            SetWindowPos(hwnd, NULL,
                         cur.x - rt_drag_offset.x,
                         cur.y - rt_drag_offset.y,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (rt_dragging) {
            rt_dragging = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
        rt_perform_manual_sync();
        return 0;

    case WM_RBUTTONUP: {
        POINT rpt;
        rpt.x = (short)LOWORD(lParam);
        rpt.y = (short)HIWORD(lParam);
        ClientToScreen(hwnd, &rpt);
        rt_show_context_menu(hwnd, rpt.x, rpt.y);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        /* Scroll wheel = resize */
        short delta = (short)HIWORD(wParam);
        int step = (delta > 0) ? 20 : -20;
        rt_resize(rt_win_size + step);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDM_RT_SYNC_SPELL)    { rt_show_spell_picker(); return 0; }
        if (id == IDM_RT_PERFORM_SYNC)  { rt_perform_manual_sync(); return 0; }
        if (id == IDM_RT_HIDE)          { gl_rt_hide(); return 0; }
        if (id == IDM_RT_SCALE_HALF)    { rt_resize(80); return 0; }
        if (id == IDM_RT_SCALE_1X)      { rt_resize(160); return 0; }
        if (id == IDM_RT_SCALE_2X)      { rt_resize(280); return 0; }
        if (id == IDM_RT_PERIOD_40)     { rt_round_period = 4000.0; rt_interval_count = 0; return 0; }
        if (id == IDM_RT_PERIOD_45)     { rt_round_period = 4500.0; rt_interval_count = 0; return 0; }
        if (id == IDM_RT_PERIOD_50)     { rt_round_period = 5000.0; rt_interval_count = 0; return 0; }
        if (id == IDM_RT_PERIOD_55)     { rt_round_period = 5500.0; rt_interval_count = 0; return 0; }
        if (id == IDM_RT_PERIOD_60)     { rt_round_period = 6000.0; rt_interval_count = 0; return 0; }
        return 0;
    }

    case WM_DESTROY:
        rt_running = 0;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---- GL init ---- */

static int rt_init_gl(void)
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;

    rt_hdc = GetDC(rt_hwnd);
    if (!rt_hdc) return -1;
    int pf = ChoosePixelFormat(rt_hdc, &pfd);
    if (!pf || !SetPixelFormat(rt_hdc, pf, &pfd)) return -1;
    rt_ctx = wglCreateContext(rt_hdc);
    if (!rt_ctx || !wglMakeCurrent(rt_hdc, rt_ctx)) return -1;

    glClearColor((float)KEY_R / 255.0f, (float)KEY_G / 255.0f,
                 (float)KEY_B / 255.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Big font for round number */
    rt_font_big = glGenLists(128);
    HFONT f1 = CreateFontA(32, 0, 0, 0, FW_BOLD, 0, 0, 0,
                           ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    HFONT of1 = (HFONT)SelectObject(rt_hdc, f1);
    wglUseFontBitmapsA(rt_hdc, 0, 128, rt_font_big);
    SelectObject(rt_hdc, of1);
    DeleteObject(f1);

    /* Small font for labels */
    rt_font_small = glGenLists(128);
    HFONT f2 = CreateFontA(14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    HFONT of2 = (HFONT)SelectObject(rt_hdc, f2);
    wglUseFontBitmapsA(rt_hdc, 0, 128, rt_font_small);
    SelectObject(rt_hdc, of2);
    DeleteObject(f2);

    return 0;
}

/* ---- Render thread ---- */

static DWORD WINAPI rt_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = rt_wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SlopGLRoundTimer";
    wc.hCursor = LoadCursor(NULL, IDC_SIZEALL);
    RegisterClassA(&wc);

    HWND mw = api->get_mmmain_hwnd();
    int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
    if (mw) {
        RECT mr;
        GetWindowRect(mw, &mr);
        wx = mr.right - rt_win_size - 20;
        wy = mr.top + 60;
    }

    rt_hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        "SlopGLRoundTimer", "Round Timer",
        WS_POPUP,
        wx, wy, rt_win_size, rt_win_size,
        mw, NULL, hInst, NULL);

    if (!rt_hwnd) {
        api->log("[gl_round_timer] CreateWindow failed (err=%lu)\n", GetLastError());
        return 1;
    }

    SetLayeredWindowAttributes(rt_hwnd, RGB(KEY_R, KEY_G, KEY_B), 0, LWA_COLORKEY);

    if (rt_init_gl() != 0) {
        api->log("[gl_round_timer] GL init failed\n");
        DestroyWindow(rt_hwnd);
        rt_hwnd = NULL;
        return 1;
    }

    glViewport(0, 0, rt_win_size, rt_win_size);
    api->log("[gl_round_timer] Running (%dx%d)\n", rt_win_size, rt_win_size);
    rt_running = 1;

    while (rt_running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { rt_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!rt_running) break;

        if (rt_visible) {
            glViewport(0, 0, rt_win_size, rt_win_size);
            glClear(GL_COLOR_BUFFER_BIT);
            rt_render();
            SwapBuffers(rt_hdc);
            Sleep(RT_TIMER_MS);
        } else {
            Sleep(100);
        }
    }

    wglMakeCurrent(NULL, NULL);
    if (rt_ctx) { wglDeleteContext(rt_ctx); rt_ctx = NULL; }
    if (rt_hdc) { ReleaseDC(rt_hwnd, rt_hdc); rt_hdc = NULL; }
    return 0;
}

/* ---- Exported commands for MMUDPy ---- */

__declspec(dllexport) void gl_rt_show(void)
{
    if (rt_hwnd) { rt_visible = 1; ShowWindow(rt_hwnd, SW_SHOWNOACTIVATE); }
}

__declspec(dllexport) void gl_rt_hide(void)
{
    if (rt_hwnd) { rt_visible = 0; ShowWindow(rt_hwnd, SW_HIDE); }
}

__declspec(dllexport) void gl_rt_toggle(void)
{
    if (rt_visible) gl_rt_hide(); else gl_rt_show();
}

__declspec(dllexport) void gl_rt_set_pos(int x, int y)
{
    if (rt_hwnd)
        SetWindowPos(rt_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

__declspec(dllexport) int gl_rt_get_round(void) { return rt_round_num; }

__declspec(dllexport) void gl_rt_reset(void)
{
    rt_round_num = 0; rt_synced = 0; rt_last_tick_ts = 0.0;
    rt_interval_count = 0; rt_round_period = 5000.0;
    rt_delta_text[0] = '\0'; rt_delta_fade = 0.0;
}

__declspec(dllexport) void gl_rt_set_size(int sz) { rt_resize(sz); }

__declspec(dllexport) void gl_rt_set_spell(const char *spell)
{
    if (spell) { strncpy(rt_sync_spell, spell, 21); rt_sync_spell[21] = '\0'; }
}

__declspec(dllexport) void gl_rt_cast_to_time(void) { rt_perform_manual_sync(); }

__declspec(dllexport) void gl_rt_set_period(int ms)
{
    if (ms >= 2000 && ms <= 10000) {
        rt_round_period = (double)ms;
        rt_interval_count = 0;
    }
}

static slop_command_t rt_commands[] = {
    { "rt_show",          "gl_rt_show",          "",  "v", "Show GL round timer" },
    { "rt_hide",          "gl_rt_hide",          "",  "v", "Hide GL round timer" },
    { "rt_toggle",        "gl_rt_toggle",        "",  "v", "Toggle round timer" },
    { "rt_set_pos",       "gl_rt_set_pos",       "ii","v", "Set position (x, y)" },
    { "rt_get_round",     "gl_rt_get_round",     "",  "i", "Get current round number" },
    { "rt_reset",         "gl_rt_reset",         "",  "v", "Reset round timer" },
    { "rt_set_size",      "gl_rt_set_size",      "i", "v", "Set timer size in pixels" },
    { "rt_set_spell",     "gl_rt_set_spell",     "s", "v", "Set timing spell command" },
    { "rt_cast_to_time",  "gl_rt_cast_to_time",  "",  "v", "Start Cast to Time sync" },
    { "rt_set_period",    "gl_rt_set_period",    "i", "v", "Set round period in ms" },
};

__declspec(dllexport) slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(rt_commands) / sizeof(rt_commands[0]);
    return rt_commands;
}

/* ---- Plugin callbacks ---- */

/* Called from on_round — same algorithm as GDI timer */
static void rt_on_round(int round_num)
{
    double now = (double)GetTickCount64();
    double prev = rt_last_tick_ts;
    rt_round_num = round_num;
    rt_in_combat = 1;
    rt_flash_tick = 15;

    /* Delta calculation */
    if (prev > 0.0 && rt_synced && rt_round_period > 0.0) {
        double elapsed = now - prev;
        double cycles = (int)(elapsed / rt_round_period + 0.5);
        if (cycles < 1) cycles = 1;
        double predicted = prev + cycles * rt_round_period;
        int delta = (int)(now - predicted);
        sprintf(rt_delta_text, "%s%dms", delta >= 0 ? "+" : "", delta);
        rt_delta_fade = 1.0;
    }

    /* Period calibration — weighted rolling average */
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

    /* Auto-show */
    if (!rt_visible && rt_hwnd) gl_rt_show();
}

/* Manual sync line processing */
static void rt_on_line(const char *line)
{
    if (ms_state != 1) return;

    if (strstr(line, "You have already cast")) {
        ms_already_count++;
        return;
    }

    if (ms_already_count >= 2) {
        int boundary = 0;
        if (strncmp(line, "You cast ", 9) == 0) boundary = 1;
        else if (strncmp(line, "You sing ", 9) == 0) boundary = 1;
        else if (strncmp(line, "You invoke ", 11) == 0) boundary = 1;
        else if (strncmp(line, "You attempt to cast", 19) == 0) boundary = 1;
        else if (strncmp(line, "You attempt to sing", 19) == 0) boundary = 1;
        else if (strncmp(line, "You attempt to invoke", 21) == 0) boundary = 1;

        if (boundary) {
            ms_rounds_detected++;
            rt_round_num++;
            rt_on_round(rt_round_num);
            ms_already_count = 0;
            if (api) api->log("[gl_rt_mansync] Round boundary #%d\n", ms_rounds_detected);
        }
    }

    /* Reset false start */
    if (line[0] && !strstr(line, "You have already cast")) {
        if (ms_already_count > 0 && ms_already_count < 2)
            ms_already_count = 0;
    }
}

static int rt_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_RT_TOGGLE) {
        gl_rt_toggle();
        return 1;
    }
    return 0;
}

static int rt_init(const slop_api_t *a)
{
    api = a;
    api->log("[gl_round_timer] DISABLED — not launching (WIP)\n");
    /* Disabled until rendering / shutdown issues are resolved */
    return 0;
}

static void rt_shutdown(void)
{
    rt_running = 0;
    ms_state = 0;
    /* Post WM_QUIT to break render thread's message loop */
    if (rt_hwnd) PostMessageA(rt_hwnd, WM_QUIT, 0, 0);
    if (rt_spell_hwnd) PostMessageA(rt_spell_hwnd, WM_QUIT, 0, 0);
    Sleep(200);
    rt_hwnd = NULL;
    rt_spell_hwnd = NULL;
    if (api) api->log("[gl_round_timer] Shutdown\n");
}

static slop_plugin_t rt_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "GL Round Timer",
    .author      = "Tripmunk",
    .description = "OpenGL combat round timer — drag, resize, right-click, Cast to Time",
    .version     = "0.2.0",
    .init        = rt_init,
    .shutdown    = rt_shutdown,
    .on_line     = rt_on_line,
    .on_round    = rt_on_round,
    .on_wndproc  = rt_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &rt_plugin; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
