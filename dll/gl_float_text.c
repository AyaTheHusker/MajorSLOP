/*
 * gl_float_text.c — GL Floating Combat Text Plugin
 * ==================================================
 *
 * GPU-accelerated floating text with glow, outlines, physics, and particles.
 * Matches the same trigger format and effects as mmudpy's GDI float_text,
 * but rendered via OpenGL for smooth animation.
 *
 * Transparent layered window overlays the MMANSI terminal area.
 * Text entries float upward, fade, and can have glow/particle effects.
 *
 * Exports MMUDPy commands: mud.gl_float(), mud.gl_float_clear()
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o gl_float_text.dll gl_float_text.c \
 *       -lgdi32 -luser32 -lopengl32 -mwindows
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <GL/gl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- State ---- */

static const slop_api_t *api = NULL;
static HWND ft_hwnd = NULL;
static HDC  ft_hdc  = NULL;
static HGLRC ft_ctx = NULL;
static volatile int ft_running = 0;
static volatile int ft_visible = 1;
static int ft_width = 800;
static int ft_height = 600;

/* Color key */
#define KEY_R 2
#define KEY_G 0
#define KEY_B 2

#define IDM_FT_TOGGLE 50800

/* ---- Float entries ---- */

#define FT_MAX 32

/* Motion modes (matching mmudpy) */
#define FT_RISE      0
#define FT_SLIDE_L   1
#define FT_SLIDE_R   2
#define FT_ARC_L     3
#define FT_ARC_R     4
#define FT_SHAKE     5
#define FT_WAVE      6
#define FT_ZOOM      7
#define FT_BOUNCE    8
#define FT_SCATTER   9

typedef struct {
    char text[256];
    float x, y;           /* normalized position 0-1 */
    float vx, vy;
    float r, g, b;        /* top color */
    float r2, g2, b2;     /* bottom color (gradient) */
    float alpha;
    float scale;
    float glow_r, glow_g, glow_b;
    int   glow_size;
    int   shadow;
    int   bold;
    int   motion;
    int   tick, max_tick;
    float orig_x, orig_y;
    float target_x;
    float angle;          /* rotation for spin effects */
    int   alive;
} ft_entry_t;

static ft_entry_t ft_entries[FT_MAX];
static int ft_count = 0;
static CRITICAL_SECTION ft_lock;
static int ft_lock_init = 0;

/* Bitmap font display lists — multiple sizes */
#define FT_FONT_SMALL  0
#define FT_FONT_MED    1
#define FT_FONT_LARGE  2
#define FT_FONT_HUGE   3
#define FT_FONT_COUNT  4

static UINT ft_font_base[FT_FONT_COUNT];
static int ft_font_sizes[FT_FONT_COUNT] = { 16, 24, 36, 48 };

/* ---- Particle system ---- */

#define FT_PART_MAX 128

static struct {
    float x, y, vx, vy;
    float r, g, b, alpha;
    int   alive;
} ft_parts[FT_PART_MAX];
static int ft_part_idx = 0;

static void ft_spawn_particles(float x, float y, float r, float g, float b, int count)
{
    for (int i = 0; i < count; i++) {
        int idx = ft_part_idx % FT_PART_MAX;
        ft_parts[idx].x = x + ((float)(rand() % 40) - 20.0f) / (float)ft_width;
        ft_parts[idx].y = y + ((float)(rand() % 20) - 10.0f) / (float)ft_height;
        float angle = (float)(rand() % 360) * (float)M_PI / 180.0f;
        float speed = 0.002f + (float)(rand() % 30) / 10000.0f;
        ft_parts[idx].vx = cosf(angle) * speed;
        ft_parts[idx].vy = sinf(angle) * speed;
        ft_parts[idx].r = r;
        ft_parts[idx].g = g;
        ft_parts[idx].b = b;
        ft_parts[idx].alpha = 0.8f;
        ft_parts[idx].alive = 1;
        ft_part_idx++;
    }
}

/* ---- Core API ---- */

static void ft_add(const char *text, float x, float y, int font_size,
                   int r, int g, int b, int r2, int g2, int b2,
                   int shadow, int glow_sz, int glow_r, int glow_g, int glow_b,
                   int rise_px, int fade_ms, int duration, int bold,
                   int motion, int exit_mode)
{
    if (!ft_lock_init) return;
    (void)rise_px; (void)fade_ms; (void)exit_mode;

    EnterCriticalSection(&ft_lock);

    if (ft_count >= FT_MAX) {
        /* Kill oldest */
        memmove(&ft_entries[0], &ft_entries[1], (FT_MAX - 1) * sizeof(ft_entry_t));
        ft_count = FT_MAX - 1;
    }

    ft_entry_t *e = &ft_entries[ft_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->text, text, 255);
    e->text[255] = '\0';

    /* Normalize position to 0-1 range */
    e->x = (x < 0) ? 0.5f : x / (float)ft_width;
    e->y = (y < 0) ? 0.5f : y / (float)ft_height;
    e->orig_x = e->x;
    e->orig_y = e->y;
    e->target_x = e->x;

    e->r = (float)r / 255.0f;
    e->g = (float)g / 255.0f;
    e->b = (float)b / 255.0f;
    e->r2 = (r2 < 0) ? e->r : (float)r2 / 255.0f;
    e->g2 = (g2 < 0) ? e->g : (float)g2 / 255.0f;
    e->b2 = (b2 < 0) ? e->b : (float)b2 / 255.0f;
    e->alpha = 1.0f;
    e->scale = (float)font_size / 36.0f;
    e->shadow = shadow;
    e->bold = bold;
    e->motion = motion;
    e->glow_size = glow_sz;
    e->glow_r = (glow_r < 0) ? e->r * 0.5f : (float)glow_r / 255.0f;
    e->glow_g = (glow_g < 0) ? e->g * 0.5f : (float)glow_g / 255.0f;
    e->glow_b = (glow_b < 0) ? e->b * 0.5f : (float)glow_b / 255.0f;
    e->tick = 0;
    e->max_tick = duration > 0 ? duration : 80;
    e->angle = 0.0f;

    /* Initial velocity based on motion */
    e->vx = 0; e->vy = 0;
    switch (motion) {
    case FT_RISE:    e->vy = 0.004f; break;
    case FT_SLIDE_L: e->x = -0.3f; e->vx = 0; break;
    case FT_SLIDE_R: e->x = 1.3f; e->vx = 0; break;
    case FT_BOUNCE:  e->vy = 0.012f; break;
    case FT_SCATTER: {
        float a = (float)(rand() % 360) * (float)M_PI / 180.0f;
        float spd = 0.005f + (float)(rand() % 40) / 5000.0f;
        e->vx = cosf(a) * spd;
        e->vy = sinf(a) * spd;
        break;
    }
    case FT_ZOOM:    e->scale = 0.1f; break;
    default: e->vy = 0.003f; break;
    }

    /* Stack offset for rise/wave/shake */
    if (motion == FT_RISE || motion == FT_WAVE || motion == FT_SHAKE || motion == FT_ZOOM)
        e->y += (float)ft_count * 0.05f;

    e->alive = 1;
    ft_count++;

    LeaveCriticalSection(&ft_lock);

    /* Auto-show */
    if (!ft_visible && ft_hwnd) {
        ft_visible = 1;
        ShowWindow(ft_hwnd, SW_SHOWNOACTIVATE);
    }
}

static void ft_render_text(const char *text, float x, float y, float r, float g, float b, float a, float scale)
{
    /* Pick best font size */
    int fi = FT_FONT_MED;
    if (scale < 0.6f) fi = FT_FONT_SMALL;
    else if (scale > 1.2f) fi = FT_FONT_LARGE;
    else if (scale > 1.8f) fi = FT_FONT_HUGE;

    int len = (int)strlen(text);
    /* Approximate text width in GL coords */
    float char_w = (float)ft_font_sizes[fi] / (float)ft_width * 0.6f;
    float tx = x - (float)len * char_w / 2.0f;

    /* Convert y from 0-1 (top=0) to GL coords (-1 to 1, bottom=-1) */
    float gx = tx * 2.0f - 1.0f;
    float gy = 1.0f - y * 2.0f;

    glColor4f(r, g, b, a);
    glRasterPos2f(gx, gy);
    glListBase(ft_font_base[fi]);
    glCallLists(len, GL_UNSIGNED_BYTE, text);
}

static void ft_render(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    EnterCriticalSection(&ft_lock);

    for (int i = 0; i < ft_count; i++) {
        ft_entry_t *e = &ft_entries[i];
        if (!e->alive) continue;

        e->tick++;
        float progress = (float)e->tick / (float)e->max_tick;

        /* Update position based on motion */
        switch (e->motion) {
        case FT_RISE:
            e->y -= e->vy;
            break;
        case FT_SLIDE_L:
        case FT_SLIDE_R: {
            float dx = e->target_x - e->x;
            e->x += dx * 0.08f;
            break;
        }
        case FT_BOUNCE:
            e->y -= e->vy;
            e->vy -= 0.0004f; /* gravity */
            if (e->y > e->orig_y) {
                e->y = e->orig_y;
                e->vy = -e->vy * 0.6f;
            }
            break;
        case FT_SCATTER:
            e->x += e->vx;
            e->y += e->vy;
            e->vy += 0.0002f; /* slight gravity */
            break;
        case FT_SHAKE:
            e->x = e->orig_x + ((float)(rand() % 10) - 5.0f) / (float)ft_width * 4.0f;
            e->y -= 0.002f;
            break;
        case FT_WAVE:
            e->y -= 0.002f;
            e->x = e->orig_x + sinf((float)e->tick * 0.15f) * 0.05f;
            break;
        case FT_ZOOM:
            if (progress < 0.3f)
                e->scale += 0.06f;
            e->y -= 0.001f;
            break;
        case FT_ARC_L:
        case FT_ARC_R: {
            float t = progress;
            float dir = (e->motion == FT_ARC_L) ? -1.0f : 1.0f;
            e->x = e->orig_x + dir * sinf(t * (float)M_PI) * 0.15f;
            e->y = e->orig_y - t * 0.3f;
            break;
        }
        default:
            e->y -= 0.003f;
            break;
        }

        /* Fade alpha in final 40% */
        if (progress > 0.6f)
            e->alpha = 1.0f - (progress - 0.6f) / 0.4f;

        /* Kill if done */
        if (e->tick >= e->max_tick) {
            /* Spawn shatter particles on death */
            ft_spawn_particles(e->x, e->y, e->r, e->g, e->b, 8);
            e->alive = 0;
            continue;
        }

        /* Draw glow (multiple offset passes) */
        if (e->glow_size > 0 && e->alpha > 0.1f) {
            float gs = (float)e->glow_size / (float)ft_width;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    ft_render_text(e->text,
                        e->x + (float)dx * gs,
                        e->y + (float)dy * gs,
                        e->glow_r, e->glow_g, e->glow_b,
                        e->alpha * 0.3f, e->scale);
                }
            }
        }

        /* Shadow */
        if (e->shadow) {
            float so = 2.0f / (float)ft_width;
            ft_render_text(e->text, e->x + so, e->y + so,
                           0.0f, 0.0f, 0.0f, e->alpha * 0.5f, e->scale);
        }

        /* Main text */
        ft_render_text(e->text, e->x, e->y,
                       e->r, e->g, e->b, e->alpha, e->scale);
    }

    /* Compact dead entries */
    int write = 0;
    for (int read = 0; read < ft_count; read++) {
        if (ft_entries[read].alive) {
            if (write != read)
                ft_entries[write] = ft_entries[read];
            write++;
        }
    }
    ft_count = write;

    LeaveCriticalSection(&ft_lock);

    /* Render particles */
    glBegin(GL_QUADS);
    for (int i = 0; i < FT_PART_MAX; i++) {
        if (!ft_parts[i].alive) continue;
        ft_parts[i].x += ft_parts[i].vx;
        ft_parts[i].y += ft_parts[i].vy;
        ft_parts[i].vy += 0.0001f;
        ft_parts[i].alpha -= 0.02f;
        if (ft_parts[i].alpha <= 0.0f) { ft_parts[i].alive = 0; continue; }

        float s = 0.006f;
        float px = ft_parts[i].x * 2.0f - 1.0f;
        float py = 1.0f - ft_parts[i].y * 2.0f;
        float a = ft_parts[i].alpha;
        glColor4f(ft_parts[i].r, ft_parts[i].g, ft_parts[i].b, a);
        glVertex2f(px - s, py - s);
        glVertex2f(px + s, py - s);
        glVertex2f(px + s, py + s);
        glVertex2f(px - s, py + s);
    }
    glEnd();
}

/* Forward decl */
static int ft_has_particles(void);

/* ---- Window & GL ---- */

static LRESULT CALLBACK ft_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        ft_width = LOWORD(lParam);
        ft_height = HIWORD(lParam);
        if (ft_ctx) glViewport(0, 0, ft_width, ft_height);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: ft_running = 0; return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static int ft_init_gl(void)
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;

    ft_hdc = GetDC(ft_hwnd);
    if (!ft_hdc) return -1;

    int pf = ChoosePixelFormat(ft_hdc, &pfd);
    if (!pf || !SetPixelFormat(ft_hdc, pf, &pfd)) return -1;

    ft_ctx = wglCreateContext(ft_hdc);
    if (!ft_ctx || !wglMakeCurrent(ft_hdc, ft_ctx)) return -1;

    glClearColor((float)KEY_R / 255.0f, (float)KEY_G / 255.0f,
                 (float)KEY_B / 255.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Create bitmap fonts at multiple sizes */
    for (int i = 0; i < FT_FONT_COUNT; i++) {
        ft_font_base[i] = glGenLists(128);
        HFONT font = CreateFontA(ft_font_sizes[i], 0, 0, 0,
                                 FW_BOLD, FALSE, FALSE, FALSE,
                                 ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, "Consolas");
        HFONT oldf = (HFONT)SelectObject(ft_hdc, font);
        wglUseFontBitmapsA(ft_hdc, 0, 128, ft_font_base[i]);
        SelectObject(ft_hdc, oldf);
        DeleteObject(font);
    }

    return 0;
}

static DWORD WINAPI ft_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = ft_wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SlopGLFloatText";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    /* Position over MMANSI area */
    HWND mw = api->get_mmmain_hwnd();
    HWND ansi = api->get_mmansi_hwnd();
    RECT ar = {0};
    int wx = 0, wy = 0, ww = 800, wh = 600;
    if (ansi) {
        GetWindowRect(ansi, &ar);
        wx = ar.left;
        wy = ar.top;
        ww = ar.right - ar.left;
        wh = ar.bottom - ar.top;
    } else if (mw) {
        GetWindowRect(mw, &ar);
        wx = ar.left;
        wy = ar.top;
        ww = ar.right - ar.left;
        wh = ar.bottom - ar.top;
    }

    ft_hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "SlopGLFloatText", NULL,
        WS_POPUP,
        wx, wy, ww, wh,
        mw, NULL, hInst, NULL);

    if (!ft_hwnd) {
        api->log("[gl_float_text] CreateWindow failed (err=%lu)\n", GetLastError());
        return 1;
    }

    SetLayeredWindowAttributes(ft_hwnd, RGB(KEY_R, KEY_G, KEY_B), 0, LWA_COLORKEY);
    ft_width = ww;
    ft_height = wh;

    if (ft_init_gl() != 0) {
        api->log("[gl_float_text] GL init failed\n");
        DestroyWindow(ft_hwnd);
        ft_hwnd = NULL;
        return 1;
    }

    glViewport(0, 0, ft_width, ft_height);
    api->log("[gl_float_text] GL float text running (%dx%d)\n", ww, wh);
    ft_running = 1;

    /* Start hidden — shows on first float text */
    ShowWindow(ft_hwnd, SW_SHOWNOACTIVATE);

    while (ft_running) {
        MSG msg;
        while (PeekMessageA(&msg, ft_hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        /* Reposition over MMANSI each frame in case it moved */
        if (ansi && ft_visible) {
            RECT cur;
            GetWindowRect(ansi, &cur);
            int nw = cur.right - cur.left;
            int nh = cur.bottom - cur.top;
            if (cur.left != ar.left || cur.top != ar.top || nw != ww || nh != wh) {
                ar = cur;
                ww = nw; wh = nh;
                SetWindowPos(ft_hwnd, NULL, cur.left, cur.top, ww, wh,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                ft_width = ww;
                ft_height = wh;
                glViewport(0, 0, ww, wh);
            }
        }

        if (ft_visible && (ft_count > 0 || ft_has_particles())) {
            glClear(GL_COLOR_BUFFER_BIT);
            ft_render();
            SwapBuffers(ft_hdc);
            Sleep(16);
        } else {
            Sleep(50);
        }
    }

    wglMakeCurrent(NULL, NULL);
    if (ft_ctx) { wglDeleteContext(ft_ctx); ft_ctx = NULL; }
    if (ft_hdc) { ReleaseDC(ft_hwnd, ft_hdc); ft_hdc = NULL; }
    return 0;
}

static int ft_has_particles(void)
{
    for (int i = 0; i < FT_PART_MAX; i++)
        if (ft_parts[i].alive) return 1;
    return 0;
}

/* ---- Exported commands for MMUDPy ---- */

__declspec(dllexport) void gl_ft_spawn(const char *text,
    int x, int y, int font_size,
    int r, int g, int b, int r2, int g2, int b2,
    int shadow, int glow_sz, int glow_r, int glow_g, int glow_b,
    int rise_px, int fade_ms, int duration, int bold,
    int motion, int exit_mode)
{
    ft_add(text, (float)x, (float)y, font_size,
           r, g, b, r2, g2, b2,
           shadow, glow_sz, glow_r, glow_g, glow_b,
           rise_px, fade_ms, duration, bold,
           motion, exit_mode);
}

__declspec(dllexport) void gl_ft_simple(const char *text, int r, int g, int b, int duration)
{
    ft_add(text, -1.0f, -1.0f, 36,
           r, g, b, -1, -1, -1,
           1, 2, -1, -1, -1,
           1, 30, duration > 0 ? duration : 80, 1,
           FT_RISE, 0);
}

__declspec(dllexport) void gl_ft_clear(void)
{
    if (!ft_lock_init) return;
    EnterCriticalSection(&ft_lock);
    ft_count = 0;
    for (int i = 0; i < FT_PART_MAX; i++) ft_parts[i].alive = 0;
    LeaveCriticalSection(&ft_lock);
}

__declspec(dllexport) void gl_ft_show(void)
{
    if (ft_hwnd) { ft_visible = 1; ShowWindow(ft_hwnd, SW_SHOWNOACTIVATE); }
}

__declspec(dllexport) void gl_ft_hide(void)
{
    if (ft_hwnd) { ft_visible = 0; ShowWindow(ft_hwnd, SW_HIDE); }
}

__declspec(dllexport) void gl_ft_toggle(void)
{
    if (ft_visible) gl_ft_hide(); else gl_ft_show();
}

/* Command table for MMUDPy discovery */
static slop_command_t ft_commands[] = {
    { "gl_float",  "gl_ft_simple", "siiii",                  "v", "GL float text (text, r, g, b, duration)" },
    { "gl_float_ex", "gl_ft_spawn", "siiiiiiiiiiiiiiiiiiiii", "v", "GL float text full (same args as float_text_ex)" },
    { "gl_float_clear", "gl_ft_clear", "",                   "v", "Clear all GL floating text" },
    { "gl_float_show",  "gl_ft_show",  "",                   "v", "Show GL float text overlay" },
    { "gl_float_hide",  "gl_ft_hide",  "",                   "v", "Hide GL float text overlay" },
    { "gl_float_toggle","gl_ft_toggle","",                   "v", "Toggle GL float text visibility" },
};

__declspec(dllexport) slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(ft_commands) / sizeof(ft_commands[0]);
    return ft_commands;
}

/* ---- Plugin callbacks ---- */

static int ft_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_FT_TOGGLE) {
        gl_ft_toggle();
        return 1;
    }
    return 0;
}

static int ft_init(const slop_api_t *a)
{
    api = a;
    api->log("[gl_float_text] DISABLED — not launching (WIP)\n");
    return 0;
}

static void ft_shutdown(void)
{
    ft_running = 0;
    Sleep(100);
    if (ft_hwnd) { DestroyWindow(ft_hwnd); ft_hwnd = NULL; }
    if (ft_lock_init) DeleteCriticalSection(&ft_lock);
    if (api) api->log("[gl_float_text] Shutdown\n");
}

static slop_plugin_t ft_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "GL Float Text",
    .author      = "Tripmunk",
    .description = "GPU-accelerated floating combat text with glow and particles",
    .version     = "0.1.0",
    .init        = ft_init,
    .shutdown    = ft_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = ft_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &ft_plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
