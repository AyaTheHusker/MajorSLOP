/*
 * mmudpy.dll - MajorSLOP Plugin: Embedded Python (MMUDPy)
 * ========================================================
 *
 * Embeds 32-bit Python 3.12 directly inside MegaMUD via the plugin system.
 * Provides a GDI console window for interactive Python scripting with
 * direct access to the game API through ctypes.
 *
 * Python runtime lives in plugins/python/ (embeddable distribution).
 * Scripts live in plugins/scripts/.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o mmudpy.dll mmudpy.c \
 *       -lgdi32 -luser32 -lcomdlg32 -mwindows
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <setjmp.h>  /* for VEH-based exception catching around Py_Initialize */

#define WM_PYEXEC       (WM_USER + 100)
#define WM_FLOAT        (WM_USER + 101)
#define WM_DEFERRED_DIE (WM_USER + 102)  /* posted to defer DestroyWindow by 1 cycle */

/* Struct for marshaling float_text_ex params to console thread */
typedef struct {
    char text[256];
    int x, y, font_size;
    int r, g, b, r2, g2, b2;
    int shadow, glow_size, glow_r, glow_g, glow_b;
    int rise_px, fade_ms, duration, bold;
    int motion, exit_mode;
} float_text_params_t;

/* ---- Python C API function pointers (loaded dynamically) ---- */

typedef struct _object PyObject;
typedef ssize_t Py_ssize_t;
typedef int (*Py_IsInitialized_t)(void);
typedef void (*Py_Initialize_t)(void);
typedef void (*Py_Finalize_t)(void);
typedef void (*Py_SetPythonHome_t)(const wchar_t *);
typedef void (*Py_SetPath_t)(const wchar_t *);
typedef int (*PyRun_SimpleString_t)(const char *);
typedef PyObject *(*PyRun_String_t)(const char *, int, PyObject *, PyObject *);
typedef PyObject *(*PyImport_AddModule_t)(const char *);
typedef PyObject *(*PyModule_GetDict_t)(PyObject *);
typedef PyObject *(*PyObject_Repr_t)(PyObject *);
typedef const char *(*PyUnicode_AsUTF8_t)(PyObject *);
typedef void (*Py_DecRef_t)(PyObject *);
typedef PyObject *(*PyErr_Occurred_t)(void);
typedef void (*PyErr_Print_t)(void);
typedef void (*PyErr_Clear_t)(void);
typedef void (*PyErr_Fetch_t)(PyObject **, PyObject **, PyObject **);
typedef void (*PyErr_Restore_t)(PyObject *, PyObject *, PyObject *);
typedef int (*Py_CompileString_t)(const char *, const char *, int);
/* Actually returns PyCodeObject* but we only care about NULL/non-NULL */
typedef PyObject *(*Py_CompileString_p_t)(const char *, const char *, int);
typedef int (*PyGILState_Ensure_t)(void);
typedef void (*PyGILState_Release_t)(int);
typedef void *(*PyEval_SaveThread_t)(void);
typedef void (*PyEval_RestoreThread_t)(void *);
/* PyInterpreterState / PyThreadState are opaque to us */
typedef void *PyInterpreterState;
typedef void *PyThreadState;
typedef PyInterpreterState *(*PyInterpreterState_Main_t)(void);
typedef PyThreadState *(*PyThreadState_New_t)(PyInterpreterState *);
typedef void (*PyEval_AcquireThread_t)(PyThreadState *);

/* Function pointers */
static HMODULE hPython = NULL;
static Py_Initialize_t       pPy_Initialize = NULL;
static Py_Finalize_t         pPy_Finalize = NULL;
static Py_IsInitialized_t    pPy_IsInitialized = NULL;
static Py_SetPythonHome_t    pPy_SetPythonHome = NULL;
static Py_SetPath_t          pPy_SetPath = NULL;
static PyRun_SimpleString_t  pPyRun_SimpleString = NULL;
static PyImport_AddModule_t  pPyImport_AddModule = NULL;
static PyModule_GetDict_t    pPyModule_GetDict = NULL;
static PyObject_Repr_t       pPyObject_Repr = NULL;
static PyUnicode_AsUTF8_t    pPyUnicode_AsUTF8 = NULL;
static Py_DecRef_t           pPy_DecRef = NULL;
static PyErr_Occurred_t      pPyErr_Occurred = NULL;
static PyErr_Print_t         pPyErr_Print = NULL;
static PyErr_Clear_t         pPyErr_Clear = NULL;
static PyErr_Fetch_t         pPyErr_Fetch = NULL;
static PyErr_Restore_t       pPyErr_Restore = NULL;
static Py_CompileString_p_t  pPy_CompileString = NULL;
static PyRun_String_t        pPyRun_String = NULL;
static PyGILState_Ensure_t   pPyGILState_Ensure = NULL;
static PyGILState_Release_t  pPyGILState_Release = NULL;
static PyEval_SaveThread_t   pPyEval_SaveThread = NULL;
static PyEval_RestoreThread_t pPyEval_RestoreThread = NULL;
static PyInterpreterState_Main_t pPyInterpreterState_Main = NULL;
static PyThreadState_New_t   pPyThreadState_New = NULL;
static PyEval_AcquireThread_t pPyEval_AcquireThread = NULL;
static void *saved_gil_state = NULL;  /* For releasing GIL from main thread */

/* Python compile modes */
#define Py_eval_input   258
#define Py_file_input   257
#define Py_single_input 256

/* ---- Plugin state ---- */

static const slop_api_t *api = NULL;
static int menu_base = 41000;  /* set from api->menu_base_id in init */
static int py_ready = 0;
static HWND con_hwnd = NULL;        /* Console window */
static HWND con_output = NULL;      /* Output edit control */
static HWND con_input = NULL;       /* Input edit control */
static WNDPROC orig_input_proc = NULL;
static CRITICAL_SECTION py_lock;
static HBRUSH pie_brush = NULL;    /* Tiled pie pattern for console bg */

/* Command history */
#define MAX_HISTORY 100
static char *cmd_history[MAX_HISTORY];
static int history_count = 0;
static int history_idx = 0;

/* Line callback buffer - multiple readers, each gets all lines */
#define MAX_LINE_BUF 256
#define MAX_LINE_READERS 8
static char *line_buffer[MAX_LINE_BUF];
static int line_buf_head = 0;
static int line_buf_tail[MAX_LINE_READERS];  /* per-reader tail — raw stream */
static int line_reader_active[MAX_LINE_READERS];
/* Parallel clean (ANSI-stripped) line stream. Same capacity, same reader
 * slots — a reader that registers gets tails into BOTH streams and can
 * read from whichever it prefers via mmudpy_get_line / _get_clean_line. */
static char *clean_line_buffer[MAX_LINE_BUF];
static int clean_line_buf_head = 0;
static int clean_line_buf_tail[MAX_LINE_READERS];
static CRITICAL_SECTION line_lock;

/* Debug counters */
static volatile int dbg_on_line_count = 0;   /* how many times mmudpy_on_line called */
static volatile int dbg_get_line_count = 0;  /* how many times mmudpy_get_line returned a line */
static volatile int dbg_get_line_empty = 0;  /* how many times mmudpy_get_line returned empty */

/* Debug log file */
static FILE *dbglog = NULL;
static void dbg(const char *fmt, ...) {
    if (!dbglog) dbglog = fopen("mmudpy_debug.log", "w");
    if (!dbglog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(dbglog, fmt, ap);
    va_end(ap);
    fflush(dbglog);
}

/* Round callback buffer */
static volatile int last_round_num = 0;

/* ---- Async console output buffer ---- */

#define CON_BUF_COUNT 512
static char *con_ring[CON_BUF_COUNT];
static volatile int con_ring_head = 0;
static volatile int con_ring_tail = 0;
static CRITICAL_SECTION con_ring_lock;
static int con_ring_lock_init = 0;
#define CON_FLUSH_TIMER_ID 99

/* Pending float text queue (script threads write, console thread creates) */
#define FLOAT_QUEUE_MAX 16
static float_text_params_t float_queue[FLOAT_QUEUE_MAX];
static volatile int float_queue_head = 0;
static volatile int float_queue_tail = 0;
static CRITICAL_SECTION float_queue_lock;
static int float_queue_lock_init = 0;

/* Append directly to edit control (must be called on console thread) */
static void con_append_direct(const char *text)
{
    if (!con_output) return;
    int len = GetWindowTextLengthA(con_output);
    SendMessageA(con_output, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageA(con_output, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageA(con_output, EM_SCROLLCARET, 0, 0);
}

/* Flush pending messages from the ring buffer (called on console thread) */
static void con_flush(void)
{
    if (!con_ring_lock_init) return;
    EnterCriticalSection(&con_ring_lock);
    while (con_ring_tail != con_ring_head) {
        char *msg = con_ring[con_ring_tail];
        con_ring_tail = (con_ring_tail + 1) % CON_BUF_COUNT;
        if (msg) {
            /* Append outside lock to avoid holding it during SendMessage */
            LeaveCriticalSection(&con_ring_lock);
            con_append_direct(msg);
            free(msg);
            EnterCriticalSection(&con_ring_lock);
        }
    }
    LeaveCriticalSection(&con_ring_lock);
}

/* Thread-safe console append - queues if called from another thread */
static void con_append(const char *text)
{
    if (!con_output) return;

    /* If we're on the console thread, write directly */
    DWORD con_tid = GetWindowThreadProcessId(con_hwnd, NULL);
    if (GetCurrentThreadId() == con_tid) {
        con_flush();  /* drain any pending first */
        con_append_direct(text);
        return;
    }

    /* Otherwise queue it for the console thread to pick up */
    if (!con_ring_lock_init) return;
    EnterCriticalSection(&con_ring_lock);
    int next = (con_ring_head + 1) % CON_BUF_COUNT;
    if (next == con_ring_tail) {
        /* Buffer full, drop oldest */
        free(con_ring[con_ring_tail]);
        con_ring[con_ring_tail] = NULL;
        con_ring_tail = (con_ring_tail + 1) % CON_BUF_COUNT;
    }
    con_ring[con_ring_head] = _strdup(text);
    con_ring_head = next;
    LeaveCriticalSection(&con_ring_lock);
}

/* ---- Floating text overlay system ---- */

#define FLOAT_MAX 16
#define FLOAT_TIMER_ID 7777

/* Motion modes for floating text */
#define FLOAT_MOTION_RISE       0  /* straight up (default) */
#define FLOAT_MOTION_SLIDE_L    1  /* enter from left */
#define FLOAT_MOTION_SLIDE_R    2  /* enter from right */
#define FLOAT_MOTION_ARC_L      3  /* parabolic arc drifting left */
#define FLOAT_MOTION_ARC_R      4  /* parabolic arc drifting right */
#define FLOAT_MOTION_SHAKE      5  /* jitter side-to-side while rising */
#define FLOAT_MOTION_WAVE       6  /* sine wave while rising */
#define FLOAT_MOTION_ZOOM       7  /* scale up from tiny */
#define FLOAT_MOTION_BOUNCE     8  /* rise, bounce down, settle */
#define FLOAT_MOTION_SCATTER    9  /* random angle outward */
#define FLOAT_MOTION_COUNT     10

/* Exit modes for floating text */
#define FLOAT_EXIT_FADE         0  /* default: alpha fade out */
#define FLOAT_EXIT_SHATTER      1  /* explode into bouncing particles */
#define FLOAT_EXIT_SPIN_SHRINK  2  /* spin and shrink to nothing */
#define FLOAT_EXIT_SPIN_ZOOM    3  /* spin, zoom big, pixelate, fade */
#define FLOAT_EXIT_COUNT        4

static const char *float_wnd_class = "MMUDPyFloat";
static int float_class_registered = 0;

typedef struct {
    HWND hwnd;
    int tick;
    int max_tick;
    int font_size;
    int rise_px;          /* pixels to rise per tick */
    int fade_ms;          /* timer interval ms */
    /* Colors */
    COLORREF color_top;   /* gradient top color */
    COLORREF color_bot;   /* gradient bottom color (same = solid) */
    COLORREF shadow_col;  /* drop shadow color */
    int shadow_ox, shadow_oy;  /* shadow offset */
    COLORREF glow_col;    /* outer glow color */
    int glow_size;        /* glow spread in px (0 = no glow) */
    int bold;
    char text[256];
    char font_face[64];
    /* Motion & exit */
    int motion;           /* FLOAT_MOTION_* */
    int exit_mode;        /* FLOAT_EXIT_* */
    int orig_font_size;   /* for zoom effects */
    /* Physics state (floats for sub-pixel precision) */
    float fx, fy;         /* current position as float */
    float target_x;       /* target x for slides */
    float vel_x, vel_y;   /* velocity for scatter/bounce */
    float angle;          /* rotation angle for spin effects (degrees) */
    int orig_x, orig_y;   /* starting position */
    /* Window bounds for bounce */
    int bounds_l, bounds_t, bounds_r, bounds_b;
} float_entry_t;

static float_entry_t floats[FLOAT_MAX];
static int float_count = 0;
static CRITICAL_SECTION float_lock;
static int float_lock_init = 0;

/* ---- Particle system (for shatter exit) ---- */

#define PARTICLE_MAX 128
#define PARTICLE_SIZE 6     /* px per particle block */
#define PARTICLE_TIMER_ID 7778
#define PARTICLE_GRAVITY 0.8f

static const char *particle_wnd_class = "MMUDPyParticle";
static int particle_class_registered = 0;

typedef struct {
    HWND hwnd;
    float x, y;           /* position */
    float vx, vy;         /* velocity */
    COLORREF color;
    int size;             /* pixel size */
    int tick, max_tick;
    int bounds_l, bounds_t, bounds_r, bounds_b;
} particle_t;

static particle_t particles[PARTICLE_MAX];
static int particle_count = 0;
static UINT_PTR particle_timer = 0;

static LRESULT CALLBACK particle_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        /* Find this particle's color */
        COLORREF col = RGB(255, 100, 0); /* fallback */
        for (int i = 0; i < particle_count; i++) {
            if (particles[i].hwnd == hwnd) { col = particles[i].color; break; }
        }
        HBRUSH br = CreateSolidBrush(col);
        FillRect(hdc, &rc, br);
        DeleteObject(br);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DEFERRED_DIE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void particle_remove(int idx)
{
    if (idx >= 0 && idx < particle_count) {
        memmove(&particles[idx], &particles[idx + 1],
                (particle_count - idx - 1) * sizeof(particle_t));
        particle_count--;
    }
}

/* Tick all active particles - called from console thread timer */
static void particles_tick(void)
{
    for (int i = particle_count - 1; i >= 0; i--) {
        particle_t *p = &particles[i];
        p->tick++;

        /* Physics */
        p->vy += PARTICLE_GRAVITY;
        p->x += p->vx;
        p->y += p->vy;

        /* Bounce off bounds */
        if (p->x < p->bounds_l) { p->x = (float)p->bounds_l; p->vx = -p->vx * 0.7f; }
        if (p->x > p->bounds_r) { p->x = (float)p->bounds_r; p->vx = -p->vx * 0.7f; }
        if (p->y > p->bounds_b) { p->y = (float)p->bounds_b; p->vy = -p->vy * 0.6f; }
        if (p->y < p->bounds_t) { p->y = (float)p->bounds_t; p->vy = -p->vy * 0.6f; }

        /* Friction */
        p->vx *= 0.98f;

        /* Fade alpha */
        int alpha = 255 - (p->tick * 255 / p->max_tick);
        if (alpha < 0) alpha = 0;

        SetWindowPos(p->hwnd, NULL, (int)p->x, (int)p->y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        SetLayeredWindowAttributes(p->hwnd, 0, (BYTE)alpha, LWA_ALPHA);

        if (alpha < 15 || p->tick >= p->max_tick) {
            SetWindowPos(p->hwnd, NULL, -32000, -32000, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            PostMessage(p->hwnd, WM_DEFERRED_DIE, 0, 0);
            particle_remove(i);
        }
    }

    /* Kill timer when no particles left */
    if (particle_count == 0 && particle_timer) {
        if (con_hwnd) KillTimer(con_hwnd, PARTICLE_TIMER_ID);
        particle_timer = 0;
    }
}

/* Spawn particles from a dying float text */
static void spawn_particles(float_entry_t *fe)
{
    if (!particle_class_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = particle_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = particle_wnd_class;
        RegisterClassA(&wc);
        particle_class_registered = 1;
    }

    /* Get float text center in screen coords */
    RECT wr;
    GetWindowRect(fe->hwnd, &wr);
    int cx = (wr.left + wr.right) / 2;
    int cy = (wr.top + wr.bottom) / 2;

    /* Get MMMAIN bounds for bouncing */
    HWND mw = FindWindowA("MMMAIN", NULL);
    RECT mwr = {0};
    if (mw) GetWindowRect(mw, &mwr);

    /* Particle colors: mix of text top, bottom, and glow colors */
    COLORREF colors[3] = { fe->color_top, fe->color_bot, fe->glow_col };

    int num = 12 + (rand() % 8); /* 12-20 particles */
    if (num > PARTICLE_MAX - particle_count)
        num = PARTICLE_MAX - particle_count;

    for (int i = 0; i < num; i++) {
        if (particle_count >= PARTICLE_MAX) break;

        float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
        float speed = 3.0f + (float)(rand() % 80) / 10.0f; /* 3-11 px/tick */

        particle_t *p = &particles[particle_count];
        p->x = (float)cx + (float)(rand() % 40 - 20);
        p->y = (float)cy + (float)(rand() % 20 - 10);
        p->vx = cosf(angle) * speed;
        p->vy = sinf(angle) * speed - 4.0f; /* bias upward */
        p->color = colors[rand() % 3];
        p->size = PARTICLE_SIZE + (rand() % 4) - 2; /* 4-8 px */
        if (p->size < 3) p->size = 3;
        p->tick = 0;
        p->max_tick = 30 + (rand() % 20); /* 30-50 ticks */
        p->bounds_l = mwr.left;
        p->bounds_t = mwr.top;
        p->bounds_r = mwr.right - p->size;
        p->bounds_b = mwr.bottom - p->size;

        p->hwnd = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            particle_wnd_class, NULL,
            WS_POPUP,
            (int)p->x, (int)p->y, p->size, p->size,
            mw, NULL, GetModuleHandleA(NULL), NULL);

        SetLayeredWindowAttributes(p->hwnd, 0, 255, LWA_ALPHA);
        ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
        particle_count++;
    }

    /* Start particle timer on console window if not running */
    if (!particle_timer && particle_count > 0 && con_hwnd) {
        particle_timer = SetTimer(con_hwnd, PARTICLE_TIMER_ID, 30, NULL);
    }
}

static float_entry_t *float_find(HWND hwnd, int *out_idx)
{
    for (int i = 0; i < float_count; i++) {
        if (floats[i].hwnd == hwnd) {
            if (out_idx) *out_idx = i;
            return &floats[i];
        }
    }
    return NULL;
}

static void float_remove(int idx)
{
    if (idx >= 0 && idx < float_count) {
        memmove(&floats[idx], &floats[idx + 1],
                (float_count - idx - 1) * sizeof(float_entry_t));
        float_count--;
    }
}

static LRESULT CALLBACK float_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  /* we handle erase in WM_PAINT (double-buffered) */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc_win = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        /* Double-buffer: paint to memory DC, then blit in one shot.
           Prevents Wine compositor from catching partial renders. */
        HDC hdc = CreateCompatibleDC(hdc_win);
        HBITMAP bmp = CreateCompatibleBitmap(hdc_win, w, h);
        HBITMAP old_bmp = (HBITMAP)SelectObject(hdc, bmp);

        /* Fill background with the color key - this becomes transparent */
        HBRUSH bg = CreateSolidBrush(RGB(1, 1, 1));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);

        int idx = -1;
        float_entry_t *fe = float_find(hwnd, &idx);
        if (!fe || !fe->text[0]) {
            BitBlt(hdc_win, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, old_bmp);
            DeleteObject(bmp);
            DeleteDC(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        /* Apply rotation transform for spin effects */
        if (fe->angle != 0.0f) {
            SetGraphicsMode(hdc, GM_ADVANCED);
            float rad = fe->angle * 3.14159f / 180.0f;
            float cx = (float)(rc.right - rc.left) / 2.0f;
            float cy = (float)(rc.bottom - rc.top) / 2.0f;
            XFORM xf;
            xf.eM11 = cosf(rad);
            xf.eM12 = sinf(rad);
            xf.eM21 = -sinf(rad);
            xf.eM22 = cosf(rad);
            xf.eDx = cx - cx * cosf(rad) + cy * sinf(rad);
            xf.eDy = cy - cx * sinf(rad) - cy * cosf(rad);
            SetWorldTransform(hdc, &xf);
        }

        HFONT font = CreateFontA(
            fe->font_size, 0, 0, 0,
            fe->bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH,
            fe->font_face[0] ? fe->font_face : "Consolas");
        HFONT old = (HFONT)SelectObject(hdc, font);

        /* Outer glow - draw text at multiple offsets in glow color */
        if (fe->glow_size > 0) {
            SetTextColor(hdc, fe->glow_col);
            for (int dy = -fe->glow_size; dy <= fe->glow_size; dy++) {
                for (int dx = -fe->glow_size; dx <= fe->glow_size; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx*dx + dy*dy > fe->glow_size * fe->glow_size) continue;
                    RECT gr = {rc.left + dx, rc.top + dy, rc.right + dx, rc.bottom + dy};
                    DrawTextA(hdc, fe->text, -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }

        /* Drop shadow */
        if (fe->shadow_ox || fe->shadow_oy) {
            SetTextColor(hdc, fe->shadow_col);
            RECT sr = {rc.left + fe->shadow_ox, rc.top + fe->shadow_oy,
                       rc.right + fe->shadow_ox, rc.bottom + fe->shadow_oy};
            DrawTextA(hdc, fe->text, -1, &sr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        /* Main text - gradient if colors differ */
        if (fe->color_top == fe->color_bot) {
            /* Solid color */
            SetTextColor(hdc, fe->color_top);
            DrawTextA(hdc, fe->text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            /* Vertical gradient: draw in horizontal bands with clipping */
            int h = rc.bottom - rc.top;
            int bands = h > 0 ? h : 1;
            int rt = GetRValue(fe->color_top), gt2 = GetGValue(fe->color_top), bt = GetBValue(fe->color_top);
            int rb = GetRValue(fe->color_bot), gb = GetGValue(fe->color_bot), bb = GetBValue(fe->color_bot);

            for (int y = 0; y < bands; y++) {
                float t = (float)y / (float)(bands > 1 ? bands - 1 : 1);
                int cr = rt + (int)((rb - rt) * t);
                int cg = gt2 + (int)((gb - gt2) * t);
                int cb = bt + (int)((bb - bt) * t);
                SetTextColor(hdc, RGB(cr, cg, cb));

                HRGN clip = CreateRectRgn(rc.left, rc.top + y, rc.right, rc.top + y + 1);
                SelectClipRgn(hdc, clip);
                DrawTextA(hdc, fe->text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectClipRgn(hdc, NULL);
                DeleteObject(clip);
            }
        }

        SelectObject(hdc, old);
        DeleteObject(font);

        /* Blit double-buffer to window in one atomic operation */
        BitBlt(hdc_win, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, old_bmp);
        DeleteObject(bmp);
        DeleteDC(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER: {
        if (wParam != FLOAT_TIMER_ID) break;

        int idx = -1;
        float_entry_t *fe = float_find(hwnd, &idx);
        if (!fe) {
            KillTimer(hwnd, FLOAT_TIMER_ID);
            SetWindowPos(hwnd, NULL, -32000, -32000, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            PostMessage(hwnd, WM_DEFERRED_DIE, 0, 0);
            return 0;
        }

        fe->tick++;
        float progress = (float)fe->tick / (float)fe->max_tick;

        /* ---- Motion ---- */
        {
            RECT wr;
            GetWindowRect(hwnd, &wr);
            int new_x = wr.left;
            int new_y = wr.top;
            int need_move = 1;
            int need_resize = 0;
            int new_w = wr.right - wr.left;
            int new_h = wr.bottom - wr.top;

            switch (fe->motion) {
            case FLOAT_MOTION_RISE:
            default:
                new_y = new_y - fe->rise_px;
                break;

            case FLOAT_MOTION_SLIDE_L: {
                /* Slide from left toward target_x, ease out */
                float t = progress < 0.3f ? progress / 0.3f : 1.0f;
                t = 1.0f - (1.0f - t) * (1.0f - t); /* ease out quad */
                fe->fx = fe->orig_x + (fe->target_x - fe->orig_x) * t;
                fe->fy -= (float)fe->rise_px * 0.3f;
                new_x = (int)fe->fx;
                new_y = (int)fe->fy;
                break;
            }
            case FLOAT_MOTION_SLIDE_R: {
                float t = progress < 0.3f ? progress / 0.3f : 1.0f;
                t = 1.0f - (1.0f - t) * (1.0f - t);
                fe->fx = fe->orig_x + (fe->target_x - fe->orig_x) * t;
                fe->fy -= (float)fe->rise_px * 0.3f;
                new_x = (int)fe->fx;
                new_y = (int)fe->fy;
                break;
            }
            case FLOAT_MOTION_ARC_L: {
                /* Parabolic arc: drift left, rise fast then slow */
                fe->fx -= 2.0f;
                float arc_p = progress * 2.0f;
                if (arc_p > 1.0f) arc_p = 1.0f;
                fe->fy = fe->orig_y - (float)fe->rise_px * 30.0f * arc_p * (2.0f - arc_p);
                new_x = (int)fe->fx;
                new_y = (int)fe->fy;
                break;
            }
            case FLOAT_MOTION_ARC_R: {
                fe->fx += 2.0f;
                float arc_p = progress * 2.0f;
                if (arc_p > 1.0f) arc_p = 1.0f;
                fe->fy = fe->orig_y - (float)fe->rise_px * 30.0f * arc_p * (2.0f - arc_p);
                new_x = (int)fe->fx;
                new_y = (int)fe->fy;
                break;
            }
            case FLOAT_MOTION_SHAKE:
                new_x = fe->orig_x + ((fe->tick % 2) ? 4 : -4);
                new_y = new_y - fe->rise_px;
                break;

            case FLOAT_MOTION_WAVE: {
                double s = sin((double)fe->tick * 0.35);
                new_x = fe->orig_x + (int)(s * 25.0);
                new_y = new_y - fe->rise_px;
                break;
            }
            case FLOAT_MOTION_ZOOM: {
                /* Scale font from 30% to 100% over first third, then rise */
                float zoom_p = progress * 3.0f;
                if (zoom_p > 1.0f) zoom_p = 1.0f;
                zoom_p = zoom_p * (2.0f - zoom_p); /* ease out */
                int new_size = (int)(fe->orig_font_size * (0.3f + 0.7f * zoom_p));
                if (new_size != fe->font_size) {
                    fe->font_size = new_size;
                    /* Resize window to fit new font */
                    int text_len = (int)strlen(fe->text);
                    new_w = new_size * text_len / 2 + 60;
                    if (new_w < 200) new_w = 200;
                    if (new_w > 1200) new_w = 1200;
                    new_h = new_size + 30;
                    need_resize = 1;
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                if (progress > 0.33f) new_y = new_y - fe->rise_px;
                else need_move = 0;
                break;
            }
            case FLOAT_MOTION_BOUNCE: {
                fe->vel_y += 0.6f; /* gravity */
                fe->fy += fe->vel_y;
                if (fe->fy > (float)fe->orig_y) {
                    fe->fy = (float)fe->orig_y;
                    fe->vel_y = -fe->vel_y * 0.55f;
                    if (fe->vel_y > -1.0f) fe->vel_y = 0; /* stop bouncing */
                }
                new_y = (int)fe->fy;
                new_x = fe->orig_x;
                break;
            }
            case FLOAT_MOTION_SCATTER: {
                fe->vel_y += 0.4f; /* gravity */
                fe->fx += fe->vel_x;
                fe->fy += fe->vel_y;
                new_x = (int)fe->fx;
                new_y = (int)fe->fy;
                break;
            }
            } /* switch motion */

            if (need_move || need_resize) {
                SetWindowPos(hwnd, NULL, new_x, new_y,
                             need_resize ? new_w : 0, need_resize ? new_h : 0,
                             (need_resize ? 0 : SWP_NOSIZE) |
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }

        /* ---- Exit handling ---- */
        int exit_start = fe->max_tick * 3 / 4; /* last 25% is exit phase */
        int dying = (fe->tick >= exit_start);
        int done = (fe->tick >= fe->max_tick);

        switch (fe->exit_mode) {
        case FLOAT_EXIT_FADE:
        default: {
            /* Standard alpha fade */
            int alpha = 255 - (fe->tick * 255 / fe->max_tick);
            if (alpha < 15) { done = 1; break; }
            SetLayeredWindowAttributes(hwnd, RGB(1, 1, 1), (BYTE)alpha, LWA_COLORKEY | LWA_ALPHA);
            break;
        }
        case FLOAT_EXIT_SHATTER: {
            /* Full opacity until exit phase, then shatter */
            if (dying && fe->tick == exit_start) {
                /* Trigger shatter - spawn particles, kill immediately */
                spawn_particles(fe);
                done = 1;
            }
            break;
        }
        case FLOAT_EXIT_SPIN_SHRINK: {
            /* Spin + shrink + fade in exit phase */
            if (dying) {
                float exit_p = (float)(fe->tick - exit_start) / (float)(fe->max_tick - exit_start);
                fe->angle += 20.0f;
                int new_size = (int)(fe->orig_font_size * (1.0f - exit_p * 0.9f));
                if (new_size < 4) new_size = 4;
                if (new_size != fe->font_size) {
                    fe->font_size = new_size;
                    RECT wr2;
                    GetWindowRect(hwnd, &wr2);
                    int text_len = (int)strlen(fe->text);
                    int nw = new_size * text_len / 2 + 60;
                    if (nw < 100) nw = 100;
                    int nh = new_size + 30;
                    int cx = (wr2.left + wr2.right) / 2;
                    int cy = (wr2.top + wr2.bottom) / 2;
                    SetWindowPos(hwnd, NULL, cx - nw/2, cy - nh/2, nw, nh,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                int alpha = (int)(255 * (1.0f - exit_p));
                if (alpha < 15) { done = 1; break; }
                SetLayeredWindowAttributes(hwnd, RGB(1, 1, 1), (BYTE)alpha, LWA_COLORKEY | LWA_ALPHA);
            }
            break;
        }
        case FLOAT_EXIT_SPIN_ZOOM: {
            /* Spin + grow + fade in exit phase */
            if (dying) {
                float exit_p = (float)(fe->tick - exit_start) / (float)(fe->max_tick - exit_start);
                fe->angle += 15.0f;
                int new_size = (int)(fe->orig_font_size * (1.0f + exit_p * 1.5f));
                if (new_size > 200) new_size = 200;
                if (new_size != fe->font_size) {
                    fe->font_size = new_size;
                    RECT wr2;
                    GetWindowRect(hwnd, &wr2);
                    int text_len = (int)strlen(fe->text);
                    int nw = new_size * text_len / 2 + 60;
                    if (nw > 1600) nw = 1600;
                    int nh = new_size + 30;
                    int cx = (wr2.left + wr2.right) / 2;
                    int cy = (wr2.top + wr2.bottom) / 2;
                    SetWindowPos(hwnd, NULL, cx - nw/2, cy - nh/2, nw, nh,
                                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    InvalidateRect(hwnd, NULL, TRUE);
                }
                int alpha = (int)(255 * (1.0f - exit_p));
                if (alpha < 15) { done = 1; break; }
                SetLayeredWindowAttributes(hwnd, RGB(1, 1, 1), (BYTE)alpha, LWA_COLORKEY | LWA_ALPHA);
            }
            break;
        }
        } /* switch exit_mode */

        if (done) {
            KillTimer(hwnd, FLOAT_TIMER_ID);
            /* Wine flicker fix: move off-screen so the compositor can't
               flash it at full opacity, then defer destroy by one cycle */
            SetWindowPos(hwnd, NULL, -32000, -32000, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            if (float_lock_init) EnterCriticalSection(&float_lock);
            float_remove(idx);
            if (float_lock_init) LeaveCriticalSection(&float_lock);
            PostMessage(hwnd, WM_DEFERRED_DIE, 0, 0);
        }
        return 0;
    }
    case WM_DEFERRED_DIE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/*
 * float_text_ex - full-featured floating text
 *
 * x,y: position relative to MMMAIN window (-1,-1 = center)
 * font_size: height in pixels (0 = default 36)
 * r,g,b: top color (or solid color)
 * r2,g2,b2: bottom color for gradient (-1 = same as top = solid)
 * shadow: 1 = drop shadow, 0 = none
 * glow_size: outer glow radius in px (0 = none)
 * glow_r/g/b: glow color
 * rise_px: pixels to rise per tick (0 = no rise)
 * fade_ms: timer interval in ms (0 = default 30)
 * duration: total ticks before gone (0 = default 80)
 * bold: 1 = bold, 0 = normal
 */
static void float_text_ex(const char *text,
    int x, int y, int font_size,
    int r, int g, int b,
    int r2, int g2, int b2,
    int shadow,
    int glow_size, int glow_r, int glow_g, int glow_b,
    int rise_px, int fade_ms, int duration, int bold,
    int motion, int exit_mode)
{
    if (!float_class_registered) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = float_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = float_wnd_class;
        wc.hbrBackground = NULL;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        float_class_registered = 1;
    }

    if (!float_lock_init) {
        InitializeCriticalSection(&float_lock);
        float_lock_init = 1;
    }

    if (font_size <= 0) font_size = 36;
    if (fade_ms <= 0) fade_ms = 30;
    if (duration <= 0) duration = 80;
    if (r2 < 0) { r2 = r; g2 = g; b2 = b; }
    if (motion < 0 || motion >= FLOAT_MOTION_COUNT) motion = FLOAT_MOTION_RISE;
    if (exit_mode < 0 || exit_mode >= FLOAT_EXIT_COUNT) exit_mode = FLOAT_EXIT_FADE;

    /* Calculate window size based on font + text length */
    int text_len = (int)strlen(text);
    int fw = font_size * text_len / 2 + 60;
    if (fw < 200) fw = 200;
    if (fw > 1200) fw = 1200;
    int fh = font_size + 30;

    /* Position relative to MMMAIN (screen coords, popup with no owner) */
    HWND mw = FindWindowA("MMMAIN", NULL);
    RECT mwr = {0};
    if (mw) GetWindowRect(mw, &mwr);
    int mw_w = mwr.right - mwr.left;
    int mw_h = mwr.bottom - mwr.top;

    /* Default center position (screen coords) */
    int center_x, center_y;
    if (x < 0) center_x = mwr.left + (mw_w - fw) / 2;
    else center_x = mwr.left + x;
    if (y < 0) center_y = mwr.top + (mw_h - fh) / 2 - 30;
    else center_y = mwr.top + y;

    /* Starting position depends on motion mode */
    int fx = center_x, fy = center_y;
    float init_vx = 0, init_vy = 0;
    float tgt_x = (float)center_x;

    switch (motion) {
    case FLOAT_MOTION_SLIDE_L:
        fx = mwr.left - fw - 20;     /* start offscreen left */
        tgt_x = (float)center_x;
        break;
    case FLOAT_MOTION_SLIDE_R:
        fx = mwr.right + 20;         /* start offscreen right */
        tgt_x = (float)center_x;
        break;
    case FLOAT_MOTION_BOUNCE:
        fy = center_y - 80;          /* start above, fall down */
        init_vy = -8.0f;
        break;
    case FLOAT_MOTION_SCATTER: {
        float angle = (float)(rand() % 360) * 3.14159f / 180.0f;
        float speed = 4.0f + (float)(rand() % 40) / 10.0f;
        init_vx = cosf(angle) * speed;
        init_vy = sinf(angle) * speed - 3.0f;
        break;
    }
    default:
        break;
    }

    EnterCriticalSection(&float_lock);

    /* Stack offset for multiple active floats (only for rise/wave/shake) */
    if (motion == FLOAT_MOTION_RISE || motion == FLOAT_MOTION_SHAKE ||
        motion == FLOAT_MOTION_WAVE || motion == FLOAT_MOTION_ZOOM)
        fy += float_count * (fh + 5);

    if (float_count >= FLOAT_MAX) {
        DestroyWindow(floats[0].hwnd);
        float_remove(0);
    }

    HWND hw = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        float_wnd_class, NULL,
        WS_POPUP,
        fx, fy, fw, fh,
        mw, NULL, GetModuleHandleA(NULL), NULL);

    SetLayeredWindowAttributes(hw, RGB(1, 1, 1), 255, LWA_COLORKEY | LWA_ALPHA);

    float_entry_t *fe = &floats[float_count];
    memset(fe, 0, sizeof(*fe));
    fe->hwnd = hw;
    fe->tick = 0;
    fe->max_tick = duration;
    fe->font_size = font_size;
    fe->rise_px = rise_px;
    fe->fade_ms = fade_ms;
    fe->color_top = RGB(r, g, b);
    fe->color_bot = RGB(r2, g2, b2);
    fe->bold = bold;
    if (shadow) {
        fe->shadow_col = RGB(0, 0, 0);
        fe->shadow_ox = 2;
        fe->shadow_oy = 2;
    }
    fe->glow_size = glow_size;
    fe->glow_col = RGB(glow_r, glow_g, glow_b);
    strncpy(fe->text, text, 255);
    fe->text[255] = '\0';

    /* Motion & exit state */
    fe->motion = motion;
    fe->exit_mode = exit_mode;
    fe->orig_font_size = font_size;
    fe->orig_x = fx;
    fe->orig_y = fy;
    fe->fx = (float)fx;
    fe->fy = (float)fy;
    fe->target_x = tgt_x;
    fe->vel_x = init_vx;
    fe->vel_y = init_vy;
    fe->angle = 0.0f;
    fe->bounds_l = mwr.left;
    fe->bounds_t = mwr.top;
    fe->bounds_r = mwr.right;
    fe->bounds_b = mwr.bottom;

    float_count++;

    LeaveCriticalSection(&float_lock);

    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);
    SetTimer(hw, FLOAT_TIMER_ID, fade_ms, NULL);
}

/* Simple wrapper for backward compat */
static void float_text_show(const char *text, int r, int g, int b, int duration)
{
    float_text_ex(text, -1, -1, 36, r, g, b, -1, -1, -1, 1, 2, r/2, g/2, b/2, 1, 30, duration, 1, 0, 0);
}

/* Forward declarations */
static void create_pie_brush(void);

/* ---- Exported C functions for ctypes ---- */

__declspec(dllexport) int mmudpy_read_i32(int offset)
{
    if (!api) return 0;
    return api->read_struct_i32((unsigned int)offset);
}

__declspec(dllexport) int mmudpy_read_i16(int offset)
{
    if (!api) return 0;
    return api->read_struct_i16((unsigned int)offset);
}

__declspec(dllexport) int mmudpy_read_string(int offset, char *out, int max_len)
{
    if (!api || !out || max_len <= 0) return 0;
    unsigned int base = api->get_struct_base();
    if (!base) { out[0] = '\0'; return 0; }
    int copy = max_len - 1;
    if (copy > 512) copy = 512;
    memcpy(out, (const char *)(base + offset), copy);
    out[copy] = '\0';
    /* Find actual null terminator */
    int slen = (int)strlen(out);
    return slen;
}

__declspec(dllexport) int mmudpy_write_i32(int offset, int value)
{
    if (!api) return -1;
    unsigned int base = api->get_struct_base();
    if (!base) return -1;
    *(int *)(base + offset) = value;
    return 0;
}

__declspec(dllexport) int mmudpy_write_i16(int offset, int value)
{
    if (!api) return -1;
    unsigned int base = api->get_struct_base();
    if (!base) return -1;
    *(short *)(base + offset) = (short)value;
    return 0;
}

__declspec(dllexport) int mmudpy_write_string(int offset, const char *text, int max_len)
{
    if (!api || !text) return -1;
    unsigned int base = api->get_struct_base();
    if (!base) return -1;
    if (max_len > 512) max_len = 512;
    strncpy((char *)(base + offset), text, max_len);
    ((char *)(base + offset))[max_len - 1] = '\0';
    return 0;
}

__declspec(dllexport) void mmudpy_command(const char *cmd)
{
    if (api) api->inject_command(cmd);
}

__declspec(dllexport) void mmudpy_text(const char *txt)
{
    if (api) api->inject_text(txt);
}

__declspec(dllexport) void mmudpy_con_write(const char *text)
{
    con_append(text);
}

__declspec(dllexport) unsigned int mmudpy_get_struct_base(void)
{
    if (!api) return 0;
    return api->get_struct_base();
}

__declspec(dllexport) int mmudpy_read_terminal_row(int row, char *out, int out_sz)
{
    if (!api || !out || out_sz <= 0) return 0;
    return api->read_terminal_row(row, out, out_sz);
}

/* MMANSI terminal layout (mirrors msimg32_proxy.c defines) */
#define MMANSI_TEXT_OFF    0x62
#define MMANSI_ATTR_OFF    0x1F52
#define MMANSI_ROW_STRIDE  0x84
#define MMANSI_MAX_ROWS    60

__declspec(dllexport) int mmudpy_read_terminal_attrs(int row, char *out, int out_sz)
{
    /* Read MMANSI attribute bytes for a terminal row.
     * Returns raw DOS color attribute bytes (1 byte per column). */
    if (!out || out_sz <= 0 || row < 0 || row >= MMANSI_MAX_ROWS) return 0;
    HWND mw = FindWindowA("MMMAIN", NULL);
    if (!mw) return 0;
    HWND ansi = FindWindowExA(mw, NULL, "MMANSI", NULL);
    if (!ansi) return 0;
    LONG wdata = GetWindowLongA(ansi, 4);
    if (!wdata) return 0;
    const unsigned char *attr = (const unsigned char *)(wdata + MMANSI_ATTR_OFF + row * MMANSI_ROW_STRIDE);
    int max = (out_sz < MMANSI_ROW_STRIDE) ? out_sz : MMANSI_ROW_STRIDE;
    memcpy(out, attr, max);
    return max;
}

__declspec(dllexport) void mmudpy_inject_server(const char *data, int len)
{
    if (api && api->inject_server_data && data && len > 0)
        api->inject_server_data(data, len);
}

__declspec(dllexport) int mmudpy_fake_remote(const char *cmd)
{
    if (!api || !api->fake_remote || !cmd) return -1;
    return api->fake_remote(cmd);
}

/* Connect/hangup — sends WM_COMMAND 0x07D4 (phone button) to MMMAIN.
 * For hangup: first disable confirmation by setting CONFIRM_HANGUP=0 (0x8AB8). */
/* Inject a MegaMUD-style blue status line into the Vulkan terminal */
static void inject_status(const char *msg)
{
    if (!api || !api->inject_server_data) return;
    /* MegaMUD style: bright white on blue background */
    char buf[512];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int len = wsprintfA(buf,
        "\r\n\x1b[1;37;44m[%s (%04d-%02d-%02d %02d:%02d:%02d)]\x1b[0m\r\n",
        msg, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    api->inject_server_data(buf, len);
}

__declspec(dllexport) int mmudpy_connect(void)
{
    if (!api || !api->get_mmmain_hwnd || !api->get_struct_base) return -1;
    unsigned int base = api->get_struct_base();
    if (!base) return -1;
    int connected = api->read_struct_i32(0x563C);
    if (connected) return 0;
    HWND mmmain = api->get_mmmain_hwnd();
    if (!mmmain) return -1;
    SendMessageA(mmmain, WM_COMMAND, 0x07D4, 0);
    inject_status("Connecting...");
    return 0;
}

__declspec(dllexport) int mmudpy_hangup(void)
{
    if (!api || !api->get_mmmain_hwnd || !api->get_struct_base) return -1;
    unsigned int base = api->get_struct_base();
    if (!base) return -1;
    int connected = api->read_struct_i32(0x563C);
    if (!connected) return 0;
    *(int *)(base + 0x8AB8) = 0;
    HWND mmmain = api->get_mmmain_hwnd();
    if (!mmmain) return -1;
    SendMessageA(mmmain, WM_COMMAND, 0x07D4, 0);
    inject_status("Session ended");
    return 0;
}

__declspec(dllexport) int mmudpy_is_connected(void)
{
    if (!api || !api->read_struct_i32) return 0;
    return api->read_struct_i32(0x563C) != 0;
}

__declspec(dllexport) int mmudpy_is_in_game(void)
{
    if (!api || !api->read_struct_i32) return 0;
    return api->read_struct_i32(0x5644) != 0;
}

/* ---- Eval queue: vk_terminal submits Python code, Python thread processes ---- */
#define EVAL_BUF_SZ 8192
static CRITICAL_SECTION eval_lock;
static char eval_code[EVAL_BUF_SZ];
static int eval_target = 0;    /* 0=terminal, >0=vkw window id */
static volatile int eval_pending = 0;
static void eval_pump(void);  /* forward decl - processes queued eval from vk_terminal */

__declspec(dllexport) void mmudpy_queue_eval(const char *code, int target)
{
    if (!code) return;
    EnterCriticalSection(&eval_lock);
    strncpy(eval_code, code, EVAL_BUF_SZ - 1);
    eval_code[EVAL_BUF_SZ - 1] = 0;
    eval_target = target;
    eval_pending = 1;
    LeaveCriticalSection(&eval_lock);
}

__declspec(dllexport) int mmudpy_line_register(void)
{
    /* Allocate a reader slot, returns reader_id (0-7) or -1 */
    EnterCriticalSection(&line_lock);
    for (int i = 0; i < MAX_LINE_READERS; i++) {
        if (!line_reader_active[i]) {
            line_reader_active[i] = 1;
            line_buf_tail[i]        = line_buf_head;        /* raw stream */
            clean_line_buf_tail[i]  = clean_line_buf_head;  /* clean stream */
            LeaveCriticalSection(&line_lock);
            return i;
        }
    }
    LeaveCriticalSection(&line_lock);
    return -1;
}

__declspec(dllexport) void mmudpy_line_unregister(int reader_id)
{
    if (reader_id < 0 || reader_id >= MAX_LINE_READERS) return;
    EnterCriticalSection(&line_lock);
    line_reader_active[reader_id] = 0;
    LeaveCriticalSection(&line_lock);
}

__declspec(dllexport) int mmudpy_get_line(int reader_id, char *out, int out_sz)
{
    /* Returns next buffered line for this reader, or 0 if empty */
    if (reader_id < 0 || reader_id >= MAX_LINE_READERS) return 0;
    EnterCriticalSection(&line_lock);
    if (!line_reader_active[reader_id] || line_buf_head == line_buf_tail[reader_id]) {
        dbg_get_line_empty++;
        LeaveCriticalSection(&line_lock);
        return 0;
    }
    int idx = line_buf_tail[reader_id];
    char *line = line_buffer[idx];
    line_buf_tail[reader_id] = (idx + 1) % MAX_LINE_BUF;
    LeaveCriticalSection(&line_lock);

    if (line) {
        dbg_get_line_count++;
        if (dbg_get_line_count <= 5)
            dbg("[get_line] #%d reader=%d: %.60s\n", dbg_get_line_count, reader_id, line);
        strncpy(out, line, out_sz - 1);
        out[out_sz - 1] = '\0';
        return 1;
    }
    return 0;
}

__declspec(dllexport) int mmudpy_get_clean_line(int reader_id, char *out, int out_sz)
{
    /* Like mmudpy_get_line but returns the ANSI-stripped variant.
     * Shares the reader slot with mmudpy_get_line — both streams advance
     * independently, so a script can consume either or both. */
    if (reader_id < 0 || reader_id >= MAX_LINE_READERS) return 0;
    EnterCriticalSection(&line_lock);
    if (!line_reader_active[reader_id] || clean_line_buf_head == clean_line_buf_tail[reader_id]) {
        LeaveCriticalSection(&line_lock);
        return 0;
    }
    int idx = clean_line_buf_tail[reader_id];
    char *line = clean_line_buffer[idx];
    clean_line_buf_tail[reader_id] = (idx + 1) % MAX_LINE_BUF;
    LeaveCriticalSection(&line_lock);

    if (line) {
        strncpy(out, line, out_sz - 1);
        out[out_sz - 1] = '\0';
        return 1;
    }
    return 0;
}

__declspec(dllexport) int mmudpy_get_round(void)
{
    return last_round_num;
}

/* Debug: returns on_line_count, get_line_count, get_line_empty via out params */
__declspec(dllexport) void mmudpy_debug_counts(int *on_line, int *got, int *empty)
{
    if (on_line) *on_line = dbg_on_line_count;
    if (got) *got = dbg_get_line_count;
    if (empty) *empty = dbg_get_line_empty;
}

/*
 * mmudpy_exec - Run arbitrary Python code in the MMUDPy interpreter.
 * Other plugin DLLs can call this to inject Python objects, help topics,
 * commands, or anything else into the console environment.
 *
 * Usage from another plugin:
 *   HMODULE m = GetModuleHandleA("mmudpy.dll");
 *   typedef int (*exec_fn)(const char *);
 *   exec_fn py = (exec_fn)GetProcAddress(m, "mmudpy_exec");
 *   if (py) py("my_var = 42\nprint('injected!')");
 *
 * Returns 0 on success, -1 if Python not ready.
 */
__declspec(dllexport) int mmudpy_exec(const char *code)
{
    if (!py_ready || !pPyRun_SimpleString) return -1;
    EnterCriticalSection(&py_lock);
    int gil = 0;
    if (pPyGILState_Ensure)
        gil = pPyGILState_Ensure();
    int rc = pPyRun_SimpleString(code);
    if (pPyGILState_Release)
        pPyGILState_Release(gil);
    LeaveCriticalSection(&py_lock);
    return rc;
}

__declspec(dllexport) void mmudpy_toggle_console(void)
{
    if (!con_hwnd) return;
    if (IsWindowVisible(con_hwnd)) {
        ShowWindow(con_hwnd, SW_HIDE);
    } else {
        ShowWindow(con_hwnd, SW_SHOW);
        SetForegroundWindow(con_hwnd);
        SetFocus(con_input);
    }
}

/* Forward declaration */
__declspec(dllexport) void mmudpy_float_text_ex(const char *text,
    int x, int y, int font_size,
    int r, int g, int b, int r2, int g2, int b2,
    int shadow, int glow_size, int glow_r, int glow_g, int glow_b,
    int rise_px, int fade_ms, int duration, int bold,
    int motion, int exit_mode);

__declspec(dllexport) void mmudpy_float_text(const char *text, int r, int g, int b, int duration)
{
    mmudpy_float_text_ex(text, -1, -1, 36, r, g, b, -1, -1, -1, 1, 2, r/2, g/2, b/2, 1, 30, duration, 1, 0, 0);
}

__declspec(dllexport) void mmudpy_float_text_ex(const char *text,
    int x, int y, int font_size,
    int r, int g, int b,
    int r2, int g2, int b2,
    int shadow,
    int glow_size, int glow_r, int glow_g, int glow_b,
    int rise_px, int fade_ms, int duration, int bold,
    int motion, int exit_mode)
{
    /* If on console thread, create directly. Otherwise queue for timer. */
    if (con_hwnd) {
        DWORD con_tid = GetWindowThreadProcessId(con_hwnd, NULL);
        if (GetCurrentThreadId() == con_tid) {
            float_text_ex(text, x, y, font_size, r, g, b, r2, g2, b2,
                          shadow, glow_size, glow_r, glow_g, glow_b,
                          rise_px, fade_ms, duration, bold, motion, exit_mode);
            return;
        }
    }

    /* Queue for the console thread's flush timer to pick up */
    if (!float_queue_lock_init) return;
    EnterCriticalSection(&float_queue_lock);
    int next = (float_queue_head + 1) % FLOAT_QUEUE_MAX;
    if (next == float_queue_tail) {
        /* Full - drop oldest */
        float_queue_tail = (float_queue_tail + 1) % FLOAT_QUEUE_MAX;
    }
    float_text_params_t *p = &float_queue[float_queue_head];
    strncpy(p->text, text, 255); p->text[255] = '\0';
    p->x = x; p->y = y; p->font_size = font_size;
    p->r = r; p->g = g; p->b = b;
    p->r2 = r2; p->g2 = g2; p->b2 = b2;
    p->shadow = shadow; p->glow_size = glow_size;
    p->glow_r = glow_r; p->glow_g = glow_g; p->glow_b = glow_b;
    p->rise_px = rise_px; p->fade_ms = fade_ms;
    p->duration = duration; p->bold = bold;
    p->motion = motion; p->exit_mode = exit_mode;
    float_queue_head = next;
    LeaveCriticalSection(&float_queue_lock);
    dbg("[float_q] queued: \"%s\" size=%d motion=%d exit=%d\n", p->text, p->font_size, motion, exit_mode);
}

/* Forward declaration */
static void py_exec(const char *code);

/* ---- Script state query (shared between C and Python) ---- */

/* Python writes script-is-loaded status here via ctypes, C reads it */
static char script_query_name[64] = {0};
static volatile int script_query_result = 0;

__declspec(dllexport) void mmudpy_set_query_result(int val)
{
    script_query_result = val;
}

/* Ask Python if a script is loaded - posts to console thread for safety */
/* (WM_PYEXEC and WM_FLOAT defined near top of file) */

static int mmudpy_is_script_loaded(const char *name)
{
    if (!py_ready || !con_hwnd) return 0;
    char cmd[256];
    sprintf(cmd, "_dll.mmudpy_set_query_result(1 if '%s' in _loaded_scripts else 0)", name);
    script_query_result = 0;
    /* Execute on the console thread which owns Python */
    char *cmd_copy = _strdup(cmd);
    SendMessageA(con_hwnd, WM_PYEXEC, 0, (LPARAM)cmd_copy);
    /* SendMessage blocks until handled, so result is ready */
    return script_query_result;
}

/* Execute Python code on the console thread (safe, blocking) */
static void py_exec_safe(const char *code)
{
    if (!py_ready || !con_hwnd) return;
    char *cmd_copy = _strdup(code);
    SendMessageA(con_hwnd, WM_PYEXEC, 0, (LPARAM)cmd_copy);
}

/* ---- Python execution ---- */

static void py_exec(const char *code)
{
    if (!py_ready || !pPyRun_SimpleString) return;

    /* Acquire the GIL - we released it before the message loop */
    int gil = 0;
    if (pPyGILState_Ensure) {
        gil = pPyGILState_Ensure();
    }

    EnterCriticalSection(&py_lock);

    /* Try as eval first (expression that returns a value) */
    PyObject *main_mod = pPyImport_AddModule("__main__");
    PyObject *main_dict = pPyModule_GetDict(main_mod);

    /* Try to compile as eval */
    pPyErr_Clear();
    PyObject *compiled = pPy_CompileString(code, "<console>", Py_eval_input);
    if (compiled) {
        /* It's an expression - eval it */
        PyObject *result = pPyRun_String(code, Py_eval_input, main_dict, main_dict);
        if (result) {
            /* Print repr if not None */
            PyObject *repr = pPyObject_Repr(result);
            if (repr) {
                const char *s = pPyUnicode_AsUTF8(repr);
                if (s && strcmp(s, "None") != 0) {
                    con_append(s);
                    con_append("\r\n");
                }
                pPy_DecRef(repr);
            }
            pPy_DecRef(result);
        } else {
            pPyErr_Print();
        }
        pPy_DecRef(compiled);
    } else {
        /* Not an expression - clear the error and try as exec */
        pPyErr_Clear();
        pPyRun_SimpleString(code);
    }

    LeaveCriticalSection(&py_lock);

    /* Release the GIL so script threads can run */
    if (pPyGILState_Release) {
        pPyGILState_Release(gil);
    }
}

/* ---- Console window ---- */

static void history_add(const char *cmd)
{
    if (history_count >= MAX_HISTORY) {
        free(cmd_history[0]);
        memmove(cmd_history, cmd_history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        history_count--;
    }
    cmd_history[history_count++] = _strdup(cmd);
    history_idx = history_count;
}

static LRESULT CALLBACK input_subclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            char buf[4096];
            GetWindowTextA(hwnd, buf, sizeof(buf));
            if (buf[0]) {
                /* Show in output */
                con_append(">>> ");
                con_append(buf);
                con_append("\r\n");

                /* Add to history */
                history_add(buf);

                /* Clear input */
                SetWindowTextA(hwnd, "");

                /* Execute */
                py_exec(buf);
            }
            return 0;
        }
        else if (wParam == VK_UP) {
            if (history_idx > 0) {
                history_idx--;
                SetWindowTextA(hwnd, cmd_history[history_idx]);
                SendMessageA(hwnd, EM_SETSEL, 9999, 9999);
            }
            return 0;
        }
        else if (wParam == VK_DOWN) {
            if (history_idx < history_count - 1) {
                history_idx++;
                SetWindowTextA(hwnd, cmd_history[history_idx]);
                SendMessageA(hwnd, EM_SETSEL, 9999, 9999);
            } else {
                history_idx = history_count;
                SetWindowTextA(hwnd, "");
            }
            return 0;
        }
    }
    return CallWindowProcA(orig_input_proc, hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK con_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        int input_h = 24;
        int pad = 4;
        MoveWindow(con_output, pad, pad, w - pad * 2, h - input_h - pad * 3, TRUE);
        MoveWindow(con_input, pad, h - input_h - pad, w - pad * 2, input_h, TRUE);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        if (hCtl == con_output) {
            SetTextColor(hdcEdit, RGB(200, 200, 200));
            SetBkColor(hdcEdit, RGB(26, 26, 28));
            static HBRUSH output_bg = NULL;
            if (!output_bg) output_bg = CreateSolidBrush(RGB(26, 26, 28));
            return (LRESULT)output_bg;
        }
        if (hCtl == con_input) {
            SetTextColor(hdcEdit, RGB(220, 220, 220));
            SetBkColor(hdcEdit, RGB(20, 20, 22));
            static HBRUSH input_bg = NULL;
            if (!input_bg) input_bg = CreateSolidBrush(RGB(20, 20, 22));
            return (LRESULT)input_bg;
        }
        break;
    }
    case WM_PYEXEC: {
        /* Execute Python code on this thread (which owns the GIL) */
        char *code = (char *)lParam;
        if (code) {
            py_exec(code);
            free(code);
        }
        return 0;
    }
    case WM_FLOAT: {
        /* Create floating text on console thread (has message pump) */
        float_text_params_t *p = (float_text_params_t *)lParam;
        if (p) {
            float_text_ex(p->text, p->x, p->y, p->font_size,
                p->r, p->g, p->b, p->r2, p->g2, p->b2,
                p->shadow, p->glow_size, p->glow_r, p->glow_g, p->glow_b,
                p->rise_px, p->fade_ms, p->duration, p->bold,
                p->motion, p->exit_mode);
            free(p);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == CON_FLUSH_TIMER_ID) {
            con_flush();
            /* Process any pending Python eval from Vulkan terminal */
            eval_pump();
            /* Also drain pending float text requests */
            if (float_queue_lock_init) {
                EnterCriticalSection(&float_queue_lock);
                while (float_queue_tail != float_queue_head) {
                    float_text_params_t q = float_queue[float_queue_tail];
                    float_queue_tail = (float_queue_tail + 1) % FLOAT_QUEUE_MAX;
                    LeaveCriticalSection(&float_queue_lock);
                    dbg("[float_drain] creating: \"%s\" size=%d\n", q.text, q.font_size);
                    float_text_ex(q.text, q.x, q.y, q.font_size,
                        q.r, q.g, q.b, q.r2, q.g2, q.b2,
                        q.shadow, q.glow_size, q.glow_r, q.glow_g, q.glow_b,
                        q.rise_px, q.fade_ms, q.duration, q.bold,
                        q.motion, q.exit_mode);
                    dbg("[float_drain] created OK\n");
                    EnterCriticalSection(&float_queue_lock);
                }
                LeaveCriticalSection(&float_queue_lock);
            }
            return 0;
        }
        if (wParam == PARTICLE_TIMER_ID) {
            particles_tick();
            return 0;
        }
        break;
    case WM_SETFOCUS:
        SetFocus(con_input);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, CON_FLUSH_TIMER_ID);
        con_hwnd = NULL;
        con_output = NULL;
        con_input = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void create_console_window(HINSTANCE hInst)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = con_wndproc;
    wc.hInstance = hInst;
    wc.hbrBackground = CreateSolidBrush(RGB(26, 26, 28));
    wc.lpszClassName = "MMUDPyConsole";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    /* Make console a child of MMMAIN so it stays within MegaMUD's window space */
    HWND parent = FindWindowA("MMMAIN", NULL);

    con_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW, "MMUDPyConsole", "MMUDPy Console",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 620,
        parent, NULL, hInst, NULL);

    /* Output - multiline read-only edit */
    con_output = CreateWindowExA(
        0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        4, 4, 888, 556,
        con_hwnd, NULL, hInst, NULL);

    /* Dark background for output */
    /* We'll set colors in WM_CTLCOLORSTATIC/WM_CTLCOLOREDIT via subclass */

    /* Input - single line */
    con_input = CreateWindowExA(
        0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        4, 564, 888, 26,
        con_hwnd, NULL, hInst, NULL);

    /* Set monospace font */
    HFONT mono = CreateFontA(
        18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SendMessageA(con_output, WM_SETFONT, (WPARAM)mono, TRUE);
    SendMessageA(con_input, WM_SETFONT, (WPARAM)mono, TRUE);

    /* Set text limits */
    SendMessageA(con_output, EM_SETLIMITTEXT, (WPARAM)1048576, 0);

    /* Subclass input for Enter/Up/Down */
    orig_input_proc = (WNDPROC)SetWindowLongA(con_input, GWL_WNDPROC, (LONG)input_subclass);

    /* Start hidden - user opens via Plugins > MMUDPy > Show/Hide Console */
    ShowWindow(con_hwnd, SW_HIDE);

    /* Create pie pattern brush for edit background */
    create_pie_brush();
}

/* ---- Mud Pie pattern brush (tiled behind console text) ---- */

static void draw_pie_slice(HDC hdc, int cx, int cy, int radius)
{
    /* Chocolate pie slice - subtle on dark background */
    int r = radius;

    /* Pie filling (dark chocolate, just slightly lighter than bg) */
    HBRUSH choc = CreateSolidBrush(RGB(38, 32, 30));
    HPEN choc_pen = CreatePen(PS_SOLID, 1, RGB(35, 28, 26));
    SelectObject(hdc, choc);
    SelectObject(hdc, choc_pen);
    Pie(hdc, cx - r, cy - r, cx + r, cy + r,
        cx + r, cy - r/3,
        cx - r/5, cy - r);
    DeleteObject(choc);
    DeleteObject(choc_pen);

    /* Mousse layer */
    HBRUSH mousse = CreateSolidBrush(RGB(42, 34, 30));
    HPEN mousse_pen = CreatePen(PS_SOLID, 1, RGB(40, 32, 28));
    SelectObject(hdc, mousse);
    SelectObject(hdc, mousse_pen);
    int r2 = radius - 6;
    Pie(hdc, cx - r2, cy - r2, cx + r2, cy + r2,
        cx + r2, cy - r2/3,
        cx - r2/5, cy - r2);
    DeleteObject(mousse);
    DeleteObject(mousse_pen);

    /* Crust edge (subtle golden) */
    HPEN crust = CreatePen(PS_SOLID, 2, RGB(55, 42, 25));
    SelectObject(hdc, crust);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Arc(hdc, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1,
        cx + r, cy - r/3,
        cx - r/5, cy - r);
    DeleteObject(crust);

    /* Whipped cream dollop (subtle light) */
    HBRUSH cream = CreateSolidBrush(RGB(55, 52, 48));
    HPEN cream_pen = CreatePen(PS_SOLID, 1, RGB(50, 48, 44));
    SelectObject(hdc, cream);
    SelectObject(hdc, cream_pen);
    Ellipse(hdc, cx + r/4 - 7, cy - r/3 - 5,
                 cx + r/4 + 7, cy - r/3 + 5);
    Ellipse(hdc, cx + r/4 - 4, cy - r/3 - 8,
                 cx + r/4 + 4, cy - r/3 - 1);
    DeleteObject(cream);
    DeleteObject(cream_pen);

    /* Drizzle */
    HPEN drizzle = CreatePen(PS_SOLID, 1, RGB(32, 24, 20));
    SelectObject(hdc, drizzle);
    MoveToEx(hdc, cx + 2, cy - r/2 + 5, NULL);
    LineTo(hdc, cx + r/3, cy - r/4 + 3);
    DeleteObject(drizzle);
}

static void create_pie_brush(void)
{
    /* Draw a small tile with a pie slice, create pattern brush */
    int tile_sz = 64;
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, tile_sz, tile_sz);
    SelectObject(memDC, bmp);

    /* Fill with console background color */
    RECT rc = {0, 0, tile_sz, tile_sz};
    HBRUSH bg = CreateSolidBrush(RGB(26, 26, 28));
    FillRect(memDC, &rc, bg);
    DeleteObject(bg);

    /* Draw a small pie slice centered in the tile */
    draw_pie_slice(memDC, tile_sz / 2, tile_sz / 2 + 2, tile_sz / 2 - 6);

    pie_brush = CreatePatternBrush(bmp);

    DeleteDC(memDC);
    DeleteObject(bmp);
    ReleaseDC(NULL, screenDC);
}

/* ---- Python bootstrap code ---- */

static const char *py_bootstrap =
    "import ctypes, sys, os\n"
    "\n"
    "# Load our DLL for ctypes calls\n"
    "_dll = None\n"
    "for name in ['mmudpy', 'mmudpy.dll']:\n"
    "    try:\n"
    "        _dll = ctypes.CDLL(name)\n"
    "        break\n"
    "    except:\n"
    "        pass\n"
    "\n"
    "if _dll is None:\n"
    "    # Try to find it by path\n"
    "    import glob\n"
    "    for p in glob.glob(r'C:\\MegaMUD\\plugins\\mmudpy.dll'):\n"
    "        try:\n"
    "            _dll = ctypes.CDLL(p)\n"
    "            break\n"
    "        except:\n"
    "            pass\n"
    "\n"
    "# Setup function signatures\n"
    "if _dll:\n"
    "    _dll.mmudpy_read_i32.restype = ctypes.c_int\n"
    "    _dll.mmudpy_read_i32.argtypes = [ctypes.c_int]\n"
    "    _dll.mmudpy_read_i16.restype = ctypes.c_int\n"
    "    _dll.mmudpy_read_i16.argtypes = [ctypes.c_int]\n"
    "    _dll.mmudpy_read_string.restype = ctypes.c_int\n"
    "    _dll.mmudpy_read_string.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_write_i32.restype = ctypes.c_int\n"
    "    _dll.mmudpy_write_i32.argtypes = [ctypes.c_int, ctypes.c_int]\n"
    "    _dll.mmudpy_write_i16.restype = ctypes.c_int\n"
    "    _dll.mmudpy_write_i16.argtypes = [ctypes.c_int, ctypes.c_int]\n"
    "    _dll.mmudpy_write_string.restype = ctypes.c_int\n"
    "    _dll.mmudpy_write_string.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_command.restype = None\n"
    "    _dll.mmudpy_command.argtypes = [ctypes.c_char_p]\n"
    "    _dll.mmudpy_text.restype = None\n"
    "    _dll.mmudpy_text.argtypes = [ctypes.c_char_p]\n"
    "    _dll.mmudpy_con_write.restype = None\n"
    "    _dll.mmudpy_con_write.argtypes = [ctypes.c_char_p]\n"
    "    _dll.mmudpy_get_struct_base.restype = ctypes.c_uint\n"
    "    _dll.mmudpy_get_struct_base.argtypes = []\n"
    "    _dll.mmudpy_read_terminal_row.restype = ctypes.c_int\n"
    "    _dll.mmudpy_read_terminal_row.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_read_terminal_attrs.restype = ctypes.c_int\n"
    "    _dll.mmudpy_read_terminal_attrs.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_inject_server.restype = None\n"
    "    _dll.mmudpy_inject_server.argtypes = [ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_fake_remote.restype = ctypes.c_int\n"
    "    _dll.mmudpy_fake_remote.argtypes = [ctypes.c_char_p]\n"
    "    _dll.mmudpy_connect.restype = ctypes.c_int\n"
    "    _dll.mmudpy_connect.argtypes = []\n"
    "    _dll.mmudpy_hangup.restype = ctypes.c_int\n"
    "    _dll.mmudpy_hangup.argtypes = []\n"
    "    _dll.mmudpy_is_connected.restype = ctypes.c_int\n"
    "    _dll.mmudpy_is_connected.argtypes = []\n"
    "    _dll.mmudpy_is_in_game.restype = ctypes.c_int\n"
    "    _dll.mmudpy_is_in_game.argtypes = []\n"
    "    _dll.mmudpy_line_register.restype = ctypes.c_int\n"
    "    _dll.mmudpy_line_register.argtypes = []\n"
    "    _dll.mmudpy_line_unregister.restype = None\n"
    "    _dll.mmudpy_line_unregister.argtypes = [ctypes.c_int]\n"
    "    _dll.mmudpy_get_line.restype = ctypes.c_int\n"
    "    _dll.mmudpy_get_line.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_get_clean_line.restype = ctypes.c_int\n"
    "    _dll.mmudpy_get_clean_line.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]\n"
    "    _dll.mmudpy_get_round.restype = ctypes.c_int\n"
    "    _dll.mmudpy_get_round.argtypes = []\n"
    "    _dll.mmudpy_toggle_console.restype = None\n"
    "    _dll.mmudpy_toggle_console.argtypes = []\n"
    "    _dll.mmudpy_float_text.restype = None\n"
    "    _dll.mmudpy_float_text.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int]\n"
    "    _dll.mmudpy_float_text_ex.restype = None\n"
    "    _dll.mmudpy_float_text_ex.argtypes = [ctypes.c_char_p] + [ctypes.c_int] * 20\n"
    "    _dll.mmudpy_set_query_result.restype = None\n"
    "    _dll.mmudpy_set_query_result.argtypes = [ctypes.c_int]\n"
    "    _dll.mmudpy_debug_counts.restype = None\n"
    "    _dll.mmudpy_debug_counts.argtypes = [ctypes.POINTER(ctypes.c_int)] * 3\n"
    "\n"
    "# Redirect stdout/stderr to console\n"
    "class _ConWriter:\n"
    "    def __init__(self):\n"
    "        self._buf = ''\n"
    "    def write(self, s):\n"
    "        if _dll and s:\n"
    "            text = s.replace('\\n', '\\r\\n')\n"
    "            _dll.mmudpy_con_write(text.encode('utf-8', errors='replace'))\n"
    "    def flush(self):\n"
    "        pass\n"
    "\n"
    "sys.stdout = _ConWriter()\n"
    "sys.stderr = _ConWriter()\n"
    "\n"
    "# Game struct offsets\n"
    "class OFF:\n"
    "    PLAYER_NAME     = 0x537C\n"
    "    PLAYER_SURNAME  = 0x5387\n"
    "    PLAYER_GANG     = 0x539A\n"
    "    PLAYER_LEVEL    = 0x53D0\n"
    "    PLAYER_RACE     = 0x53C4\n"
    "    PLAYER_CLASS    = 0x53C8\n"
    "    CUR_HP          = 0x53D4\n"
    "    MAX_HP          = 0x53DC\n"
    "    CUR_MANA        = 0x53E0\n"
    "    MAX_MANA        = 0x53E8\n"
    "    EXP_LO          = 0x53B0\n"
    "    EXP_HI          = 0x53B4\n"
    "    EXP_NEED_LO     = 0x53B8\n"
    "    EXP_NEED_HI     = 0x53BC\n"
    "    STRENGTH        = 0x53F4\n"
    "    AGILITY         = 0x53F8\n"
    "    INTELLECT       = 0x53FC\n"
    "    WISDOM          = 0x5400\n"
    "    HEALTH          = 0x5404\n"
    "    CHARM           = 0x5408\n"
    "    PERCEPTION      = 0x540C\n"
    "    STEALTH         = 0x5410\n"
    "    THIEVERY        = 0x5414\n"
    "    TRAPS           = 0x5418\n"
    "    PICKLOCKS       = 0x541C\n"
    "    TRACKING        = 0x5420\n"
    "    MARTIAL_ARTS    = 0x5424\n"
    "    MAGIC_RES       = 0x5428\n"
    "    SPELL_CASTING   = 0x542C\n"
    "    CONNECTED       = 0x563C\n"
    "    IN_GAME         = 0x5644\n"
    "    IN_COMBAT       = 0x5698\n"
    "    IS_RESTING      = 0x5678\n"
    "    IS_MEDITATING   = 0x567C\n"
    "    IS_SNEAKING     = 0x5688\n"
    "    IS_HIDING       = 0x5690\n"
    "    IS_LOST         = 0x5670\n"
    "    IS_FLEEING      = 0x5674\n"
    "    PATHING_ACTIVE  = 0x5664\n"
    "    LOOPING         = 0x5668\n"
    "    GO_FLAG         = 0x564C\n"
    "    MODE            = 0x54BC\n"
    "    IS_BLINDED      = 0x56AC\n"
    "    IS_CONFUSED     = 0x56B0\n"
    "    IS_POISONED     = 0x56B4\n"
    "    POISON_AMOUNT   = 0x56B8\n"
    "    IS_DISEASED     = 0x56BC\n"
    "    IS_HELD         = 0x56CC\n"
    "    IS_STUNNED      = 0x56D0\n"
    "    ROOM_NAME       = 0x2D9C\n"
    "    ROOM_CHECKSUM   = 0x2E70\n"
    "    ROOM_EXITS      = 0x2E78\n"
    "    ROOM_DARKNESS   = 0x5528\n"
    "    ENTITY_COUNT    = 0x1ED4\n"
    "    COMBAT_TARGET   = 0x552C\n"
    "    ATTACK_CMD      = 0x51D9\n"
    "    AUTO_COMBAT     = 0x4D00\n"
    "    AUTO_NUKE       = 0x4D04\n"
    "    AUTO_HEAL       = 0x4D08\n"
    "    AUTO_BLESS      = 0x4D0C\n"
    "    AUTO_LIGHT      = 0x4D10\n"
    "    AUTO_CASH       = 0x4D14\n"
    "    AUTO_GET        = 0x4D18\n"
    "    AUTO_SNEAK      = 0x4D20\n"
    "    AUTO_HIDE       = 0x4D24\n"
    "    CUR_PATH_STEP   = 0x5898\n"
    "    PATH_TOTAL      = 0x5894\n"
    "    PATH_FILENAME   = 0x5834\n"
    "    HEAL_CMD        = 0x4E40\n"
    "    HP_REST_PCT     = 0x378C\n"
    "    HP_HEAL_PCT     = 0x3790\n"
    "    HP_RUN_PCT      = 0x3798\n"
    "    MANA_REST_PCT   = 0x37A8\n"
    "    MANA_HEAL_PCT   = 0x37AC\n"
    "    MANA_BLESS_PCT  = 0x37B8\n"
    "    PARTY_SIZE      = 0x35F0\n"
    "    PARTY_LEADER    = 0x3748\n"
    "    PARTY_MEMBERS   = 0x35F8\n"
    "\n"
    "# Scripts directory: plugins/MMUDPy/scripts relative to MegaMUD exe\n"
    "_scripts_dir = os.path.join(os.path.dirname(os.path.abspath(sys.executable or '.')), 'plugins', 'MMUDPy', 'scripts')\n"
    "os.makedirs(_scripts_dir, exist_ok=True)\n"
    "\n"
    "# Convenience API object\n"
    "class _MudAPI:\n"
    "    '''MegaMUD Game API - type `mud` to see all commands.'''\n"
    "\n"
    "    # --- Low-level memory access ---\n"
    "    def read_i32(self, offset):\n"
    "        '''Read 32-bit int at struct offset.'''\n"
    "        return _dll.mmudpy_read_i32(offset)\n"
    "    def read_i16(self, offset):\n"
    "        '''Read 16-bit int at struct offset.'''\n"
    "        return _dll.mmudpy_read_i16(offset)\n"
    "    def read_string(self, offset, maxlen=64):\n"
    "        '''Read null-terminated string at struct offset.'''\n"
    "        buf = ctypes.create_string_buffer(maxlen)\n"
    "        _dll.mmudpy_read_string(offset, buf, maxlen)\n"
    "        return buf.value.decode('utf-8', errors='replace')\n"
    "    def write_i32(self, offset, value):\n"
    "        '''Write 32-bit int to struct offset.'''\n"
    "        return _dll.mmudpy_write_i32(offset, value)\n"
    "    def write_i16(self, offset, value):\n"
    "        '''Write 16-bit int to struct offset.'''\n"
    "        return _dll.mmudpy_write_i16(offset, value)\n"
    "    def write_string(self, offset, text, maxlen=64):\n"
    "        '''Write string to struct offset.'''\n"
    "        return _dll.mmudpy_write_string(offset, text.encode('utf-8'), maxlen)\n"
    "\n"
    "    # --- Commands ---\n"
    "    def command(self, cmd):\n"
    "        '''Send a command to the MUD (types it + Enter).'''\n"
    "        _dll.mmudpy_command(cmd.encode('utf-8'))\n"
    "    def text(self, txt):\n"
    "        '''Type text into terminal (no Enter).'''\n"
    "        _dll.mmudpy_text(txt.encode('utf-8'))\n"
    "\n"
    "    # --- Info ---\n"
    "    def struct_base(self):\n"
    "        '''Get base address of game struct (0 = not found).'''\n"
    "        return _dll.mmudpy_get_struct_base()\n"
    "    def terminal_row(self, row):\n"
    "        '''Read terminal row text (0-59).'''\n"
    "        buf = ctypes.create_string_buffer(140)\n"
    "        _dll.mmudpy_read_terminal_row(row, buf, 140)\n"
    "        return buf.value.decode('utf-8', errors='replace')\n"
    "    def terminal_attrs(self, row):\n"
    "        '''Read terminal row color attributes (0-59). Returns list of DOS color bytes.'''\n"
    "        buf = ctypes.create_string_buffer(140)\n"
    "        n = _dll.mmudpy_read_terminal_attrs(row, buf, 140)\n"
    "        return list(buf.raw[:n])\n"
    "    def terminal_row_full(self, row):\n"
    "        '''Read terminal row text + attrs. Returns (text, attrs) tuple.\n"
    "        attrs is a list of DOS color bytes, one per character.\n"
    "        Color mapping: 0x02=green, 0x06=cyan, 0x07=gray, 0x09=blue, 0x0E=bold yellow.'''\n"
    "        return (self.terminal_row(row), self.terminal_attrs(row))\n"
    "    def terminal_dump(self, start=0, end=60):\n"
    "        '''Dump terminal rows with color info. Prints non-empty rows.'''\n"
    "        for r in range(start, end):\n"
    "            text = self.terminal_row(r)\n"
    "            if text.strip():\n"
    "                attrs = self.terminal_attrs(r)\n"
    "                # Show unique colors in this row\n"
    "                colors = set(attrs[:len(text)])\n"
    "                color_str = ','.join(f'0x{c:02X}' for c in sorted(colors))\n"
    "                print(f'[{r:2d}] {text}  ({color_str})')\n"
    "    def inject_server(self, data):\n"
    "        '''Inject fake server data through MegaMUD\\'s real pipeline.\n"
    "        Data appears in MMANSI terminal and goes through the line parser.\n"
    "        Use ANSI escape codes for colors, \\\\r\\\\n for newlines.\n"
    "        Example: mud.inject_server(\\\"\\\\r\\\\n\\\\033[1;33mRoom Name\\\\033[0m\\\\r\\\\n\\\")'''\n"
    "        if isinstance(data, str):\n"
    "            data = data.encode('utf-8')\n"
    "        _dll.mmudpy_inject_server(data, len(data))\n"
    "\n"
    "    # --- Player ---\n"
    "    def player_name(self):\n"
    "        '''Current player name.'''\n"
    "        return self.read_string(OFF.PLAYER_NAME, 32)\n"
    "    def level(self):\n"
    "        '''Player level.'''\n"
    "        return self.read_i32(OFF.PLAYER_LEVEL)\n"
    "    def hp(self):\n"
    "        '''Returns (current_hp, max_hp).'''\n"
    "        return (self.read_i32(OFF.CUR_HP), self.read_i32(OFF.MAX_HP))\n"
    "    def mana(self):\n"
    "        '''Returns (current_mana, max_mana).'''\n"
    "        return (self.read_i32(OFF.CUR_MANA), self.read_i32(OFF.MAX_MANA))\n"
    "    def hp_pct(self):\n"
    "        '''HP as percentage (0-100).'''\n"
    "        cur, mx = self.hp()\n"
    "        return (cur * 100 // mx) if mx > 0 else 0\n"
    "    def mana_pct(self):\n"
    "        '''Mana as percentage (0-100).'''\n"
    "        cur, mx = self.mana()\n"
    "        return (cur * 100 // mx) if mx > 0 else 0\n"
    "\n"
    "    # --- Room ---\n"
    "    def room_name(self):\n"
    "        '''Current room name.'''\n"
    "        return self.read_string(OFF.ROOM_NAME, 80)\n"
    "\n"
    "    # --- Connection ---\n"
    "    def connect(self):\n"
    "        '''Connect to BBS. No-op if already connected.'''\n"
    "        return _dll.mmudpy_connect() == 0\n"
    "    def hangup(self):\n"
    "        '''Hangup (disconnect). Bypasses confirmation. No-op if not connected.'''\n"
    "        return _dll.mmudpy_hangup() == 0\n"
    "    def is_connected(self):\n"
    "        '''True if connected to BBS.'''\n"
    "        return _dll.mmudpy_is_connected() != 0\n"
    "    def is_in_game(self):\n"
    "        '''True if in the game world.'''\n"
    "        return _dll.mmudpy_is_in_game() != 0\n"
    "\n"
    "    # --- State flags ---\n"
    "    def in_combat(self):\n"
    "        '''True if in combat.'''\n"
    "        return self.read_i32(OFF.IN_COMBAT) != 0\n"
    "    def is_resting(self):\n"
    "        '''True if resting.'''\n"
    "        return self.read_i32(OFF.IS_RESTING) != 0\n"
    "    def is_meditating(self):\n"
    "        '''True if meditating.'''\n"
    "        return self.read_i32(OFF.IS_MEDITATING) != 0\n"
    "    def is_sneaking(self):\n"
    "        '''True if sneaking.'''\n"
    "        return self.read_i32(OFF.IS_SNEAKING) != 0\n"
    "    def is_hiding(self):\n"
    "        '''True if hiding.'''\n"
    "        return self.read_i32(OFF.IS_HIDING) != 0\n"
    "    def is_poisoned(self):\n"
    "        '''True if poisoned.'''\n"
    "        return self.read_i32(OFF.IS_POISONED) != 0\n"
    "    def is_held(self):\n"
    "        '''True if held/paralyzed.'''\n"
    "        return self.read_i32(OFF.IS_HELD) != 0\n"
    "    def is_stunned(self):\n"
    "        '''True if stunned.'''\n"
    "        return self.read_i32(OFF.IS_STUNNED) != 0\n"
    "    def is_blinded(self):\n"
    "        '''True if blinded.'''\n"
    "        return self.read_i32(OFF.IS_BLINDED) != 0\n"
    "    def is_confused(self):\n"
    "        '''True if confused.'''\n"
    "        return self.read_i32(OFF.IS_CONFUSED) != 0\n"
    "    def is_fleeing(self):\n"
    "        '''True if fleeing.'''\n"
    "        return self.read_i32(OFF.IS_FLEEING) != 0\n"
    "    def is_looping(self):\n"
    "        '''True if loop is active.'''\n"
    "        return self.read_i32(OFF.LOOPING) != 0\n"
    "\n"
    "    # --- Stats ---\n"
    "    def strength(self): return self.read_i32(OFF.STRENGTH)\n"
    "    def agility(self): return self.read_i32(OFF.AGILITY)\n"
    "    def intellect(self): return self.read_i32(OFF.INTELLECT)\n"
    "    def wisdom(self): return self.read_i32(OFF.WISDOM)\n"
    "    def health(self): return self.read_i32(OFF.HEALTH)\n"
    "    def charm(self): return self.read_i32(OFF.CHARM)\n"
    "    def perception(self): return self.read_i32(OFF.PERCEPTION)\n"
    "\n"
    "    # --- Round ---\n"
    "    def get_round(self):\n"
    "        '''Current round number.'''\n"
    "        return _dll.mmudpy_get_round()\n"
    "\n"
    "    # --- Automation Toggles ---\n"
    "    # Each returns the new state (True/False). Call with no arg to toggle,\n"
    "    # or pass True/False to set explicitly.\n"
    "    # Runtime toggle offsets (what MegaMUD actually checks at runtime)\n"
    "    _RT_OFFSETS = {\n"
    "        0x4D00: 0x573C,  # Auto Combat\n"
    "        0x4D04: 0x5744,  # Auto Nuke\n"
    "        0x4D08: 0x5748,  # Auto Heal\n"
    "        0x4D0C: 0x574C,  # Auto Bless\n"
    "        0x4D10: 0x5750,  # Auto Light\n"
    "        0x4D14: 0x5754,  # Auto Cash\n"
    "        0x4D18: 0x5758,  # Auto Get\n"
    "        0x4D1C: 0x575C,  # Auto Search\n"
    "        0x4D20: 0x5764,  # Auto Sneak\n"
    "        0x4D24: 0x5768,  # Auto Hide\n"
    "        0x4D28: 0x576C,  # Auto Track\n"
    "    }\n"
    "    def _toggle(self, off, val=None):\n"
    "        rt = self._RT_OFFSETS.get(off)\n"
    "        read_off = rt if rt else off\n"
    "        if val is None:\n"
    "            val = not bool(self.read_i32(read_off))\n"
    "        v = 1 if val else 0\n"
    "        self.write_i32(off, v)\n"
    "        if rt:\n"
    "            self.write_i32(rt, v)\n"
    "        return bool(val)\n"
    "    def autocombat(self, on=None):\n"
    "        '''Toggle or set auto-combat. mud.autocombat() / mud.autocombat(True)'''\n"
    "        return self._toggle(0x4D00, on)\n"
    "    def autonuke(self, on=None):\n"
    "        '''Toggle or set auto-nuke (offensive spells).'''\n"
    "        return self._toggle(0x4D04, on)\n"
    "    def autoheal(self, on=None):\n"
    "        '''Toggle or set auto-heal.'''\n"
    "        return self._toggle(0x4D08, on)\n"
    "    def autobless(self, on=None):\n"
    "        '''Toggle or set auto-bless (buff spells).'''\n"
    "        return self._toggle(0x4D0C, on)\n"
    "    def autolight(self, on=None):\n"
    "        '''Toggle or set auto-light in dark rooms.'''\n"
    "        return self._toggle(0x4D10, on)\n"
    "    def autocash(self, on=None):\n"
    "        '''Toggle or set auto-pick up cash.'''\n"
    "        return self._toggle(0x4D14, on)\n"
    "    def autoget(self, on=None):\n"
    "        '''Toggle or set auto-pick up items.'''\n"
    "        return self._toggle(0x4D18, on)\n"
    "    def autosearch(self, on=None):\n"
    "        '''Toggle or set auto-search rooms.'''\n"
    "        return self._toggle(0x4D1C, on)\n"
    "    def autosneak(self, on=None):\n"
    "        '''Toggle or set auto-sneak movement.'''\n"
    "        return self._toggle(0x4D20, on)\n"
    "    def autohide(self, on=None):\n"
    "        '''Toggle or set auto-hide after combat.'''\n"
    "        return self._toggle(0x4D24, on)\n"
    "    def autotrack(self, on=None):\n"
    "        '''Toggle or set auto-track monsters.'''\n"
    "        return self._toggle(0x4D28, on)\n"
    "    def autotrain(self, on=None):\n"
    "        '''Toggle or set auto-train on level up.'''\n"
    "        return self._toggle(0x3780, on)\n"
    "\n"
    "    # --- Console ---\n"
    "    def console(self):\n"
    "        '''Toggle the MMUDPy console window show/hide.'''\n"
    "        _dll.mmudpy_toggle_console()\n"
    "\n"
    "    # --- Visual effects ---\n"
    "    # Motion modes\n"
    "    RISE = 0; SLIDE_L = 1; SLIDE_R = 2; ARC_L = 3; ARC_R = 4\n"
    "    SHAKE = 5; WAVE = 6; ZOOM = 7; BOUNCE = 8; SCATTER = 9\n"
    "    # Exit modes\n"
    "    FADE = 0; SHATTER = 1; SPIN_SHRINK = 2; SPIN_ZOOM = 3\n"
    "\n"
    "    def float_text(self, text, r=255, g=255, b=255, duration=80,\n"
    "                   x=-1, y=-1, size=36, r2=-1, g2=-1, b2=-1,\n"
    "                   shadow=1, glow=2, glow_r=-1, glow_g=-1, glow_b=-1,\n"
    "                   rise=1, fade_ms=30, bold=1, motion=0, exit=0):\n"
    "        '''Show floating text overlay.\n"
    "        Motion: RISE=0, SLIDE_L=1, SLIDE_R=2, ARC_L=3, ARC_R=4,\n"
    "                SHAKE=5, WAVE=6, ZOOM=7, BOUNCE=8, SCATTER=9\n"
    "        Exit:   FADE=0, SHATTER=1, SPIN_SHRINK=2, SPIN_ZOOM=3\n"
    "        '''\n"
    "        if glow_r < 0: glow_r = r // 2\n"
    "        if glow_g < 0: glow_g = g // 2\n"
    "        if glow_b < 0: glow_b = b // 2\n"
    "        _dll.mmudpy_float_text_ex(text.encode('utf-8'),\n"
    "            x, y, size, r, g, b, r2, g2, b2,\n"
    "            shadow, glow, glow_r, glow_g, glow_b,\n"
    "            rise, fade_ms, duration, bold, motion, exit)\n"
    "\n"
    "    # --- Script management ---\n"
    "    def save(self, name, code=None):\n"
    "        '''Save code to plugins/scripts/<name>.py. If no code given, saves last input.'''\n"
    "        if not name.endswith('.py'):\n"
    "            name += '.py'\n"
    "        path = os.path.join(_scripts_dir, name)\n"
    "        if code is None:\n"
    "            print(f'Usage: mud.save(\"name\", \"\"\"your code here\"\"\")')\n"
    "            return\n"
    "        os.makedirs(_scripts_dir, exist_ok=True)\n"
    "        with open(path, 'w') as f:\n"
    "            f.write(code)\n"
    "        print(f'Saved: {path}')\n"
    "\n"
    "    def load(self, name):\n"
    "        '''Load and run a script. Scripts with start() are auto-started.'''\n"
    "        if not name.endswith('.py'):\n"
    "            name += '.py'\n"
    "        base = name[:-3]\n"
    "        path = os.path.join(_scripts_dir, name)\n"
    "        if not os.path.exists(path):\n"
    "            print(f'Script not found: {path}')\n"
    "            return\n"
    "        with open(path, 'r') as f:\n"
    "            code = f.read()\n"
    "        ns = {'mud': self, 'OFF': OFF, '_dll': _dll, '_scripts_dir': _scripts_dir,\n"
    "              'ctypes': ctypes, 'os': os, 'sys': sys, 're': __import__('re'),\n"
    "              'threading': __import__('threading'), 'time': __import__('time')}\n"
    "        exec(code, ns)\n"
    "        _loaded_scripts[base] = ns\n"
    "        if 'start' in ns and callable(ns['start']):\n"
    "            ns['start']()\n"
    "        else:\n"
    "            print(f'Loaded: {name}')\n"
    "\n"
    "    def stop(self, name):\n"
    "        '''Stop a running script (calls its stop() function).'''\n"
    "        if name in _loaded_scripts:\n"
    "            ns = _loaded_scripts[name]\n"
    "            if 'stop' in ns and callable(ns['stop']):\n"
    "                ns['stop']()\n"
    "            del _loaded_scripts[name]\n"
    "        else:\n"
    "            print(f'Script not loaded: {name}')\n"
    "            if _loaded_scripts:\n"
    "                print('Loaded: ' + ', '.join(sorted(_loaded_scripts.keys())))\n"
    "\n"
    "    def call(self, script_name, func_name, *args):\n"
    "        '''Call a function in a loaded script. E.g.: mud.call('crit_tracker', 'settings')'''\n"
    "        if script_name not in _loaded_scripts:\n"
    "            print(f'Script not loaded: {script_name}')\n"
    "            return\n"
    "        ns = _loaded_scripts[script_name]\n"
    "        if func_name not in ns or not callable(ns[func_name]):\n"
    "            print(f'No callable {func_name}() in {script_name}')\n"
    "            return\n"
    "        return ns[func_name](*args)\n"
    "\n"
    "    def debug(self):\n"
    "        '''Show line buffer debug counters.'''\n"
    "        a, b, c = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()\n"
    "        _dll.mmudpy_debug_counts(ctypes.byref(a), ctypes.byref(b), ctypes.byref(c))\n"
    "        print(f'on_line called: {a.value}')\n"
    "        print(f'get_line returned data: {b.value}')\n"
    "        print(f'get_line returned empty: {c.value}')\n"
    "        print(f'Loaded scripts: {list(_loaded_scripts.keys())}')\n"
    "\n"
    "    def running(self):\n"
    "        '''Show currently running scripts.'''\n"
    "        if not _loaded_scripts:\n"
    "            print('No scripts running.')\n"
    "            return\n"
    "        print(f'Running scripts ({len(_loaded_scripts)}):')\n"
    "        for name in sorted(_loaded_scripts.keys()):\n"
    "            print(f'  {name}')\n"
    "\n"
    "    def list(self):\n"
    "        '''Alias for running() — list loaded scripts.'''\n"
    "        self.running()\n"
    "\n"
    "    def help(self, topic=None):\n"
    "        '''Show help. Delegates to global help().'''\n"
    "        help(topic)\n"
    "\n"
    "    def scripts(self):\n"
    "        '''List all saved scripts.'''\n"
    "        if not os.path.exists(_scripts_dir):\n"
    "            print('No scripts directory.')\n"
    "            return\n"
    "        files = [f for f in os.listdir(_scripts_dir) if f.endswith('.py')]\n"
    "        if not files:\n"
    "            print('No scripts saved yet.')\n"
    "            print(f'Save one: mud.save(\"name\", \"\"\"code here\"\"\")')\n"
    "            return\n"
    "        print(f'Scripts ({len(files)}):')\n"
    "        for f in sorted(files):\n"
    "            path = os.path.join(_scripts_dir, f)\n"
    "            size = os.path.getsize(path)\n"
    "            print(f'  {f}  ({size} bytes)')\n"
    "\n"
    "    def edit(self, name):\n"
    "        '''Print contents of a script so you can copy/modify it.'''\n"
    "        if not name.endswith('.py'):\n"
    "            name += '.py'\n"
    "        path = os.path.join(_scripts_dir, name)\n"
    "        if not os.path.exists(path):\n"
    "            print(f'Script not found: {path}')\n"
    "            return\n"
    "        with open(path, 'r') as f:\n"
    "            print(f.read())\n"
    "\n"
    "    def delete(self, name):\n"
    "        '''Delete a script.'''\n"
    "        if not name.endswith('.py'):\n"
    "            name += '.py'\n"
    "        path = os.path.join(_scripts_dir, name)\n"
    "        if not os.path.exists(path):\n"
    "            print(f'Script not found: {path}')\n"
    "            return\n"
    "        os.remove(path)\n"
    "        print(f'Deleted: {name}')\n"
    "\n"
    "    def __repr__(self):\n"
    "        lines = []\n"
    "        lines.append('=== MegaMUD Python API ===')\n"
    "        lines.append('')\n"
    "        lines.append('--- Player ---')\n"
    "        lines.append('  mud.player_name()    mud.level()')\n"
    "        lines.append('  mud.hp()             mud.mana()')\n"
    "        lines.append('  mud.hp_pct()         mud.mana_pct()')\n"
    "        lines.append('')\n"
    "        lines.append('--- Stats ---')\n"
    "        lines.append('  mud.strength()  mud.agility()   mud.intellect()')\n"
    "        lines.append('  mud.wisdom()    mud.health()    mud.charm()')\n"
    "        lines.append('  mud.perception()')\n"
    "        lines.append('')\n"
    "        lines.append('--- Room ---')\n"
    "        lines.append('  mud.room_name()')\n"
    "        lines.append('')\n"
    "        lines.append('--- State ---')\n"
    "        lines.append('  mud.in_combat()    mud.is_resting()    mud.is_meditating()')\n"
    "        lines.append('  mud.is_sneaking()  mud.is_hiding()     mud.is_fleeing()')\n"
    "        lines.append('  mud.is_poisoned()  mud.is_held()       mud.is_stunned()')\n"
    "        lines.append('  mud.is_blinded()   mud.is_confused()   mud.is_looping()')\n"
    "        lines.append('')\n"
    "        lines.append('--- Connection ---')\n"
    "        lines.append('  mud.connect()      mud.hangup()         mud.is_connected()')\n"
    "        lines.append('  mud.is_in_game()')\n"
    "        lines.append('')\n"
    "        lines.append('--- Commands ---')\n"
    "        lines.append('  mud.command(\"say hello\")   mud.text(\"partial\")')\n"
    "        lines.append('')\n"
    "        lines.append('--- Memory ---')\n"
    "        lines.append('  mud.read_i32(OFF.X)          mud.write_i32(OFF.X, val)')\n"
    "        lines.append('  mud.read_i16(OFF.X)          mud.write_i16(OFF.X, val)')\n"
    "        lines.append('  mud.read_string(OFF.X, len)  mud.write_string(OFF.X, s, len)')\n"
    "        lines.append('  mud.struct_base()            mud.terminal_row(n)')\n"
    "        lines.append('  mud.get_round()')\n"
    "        lines.append('')\n"
    "        lines.append('--- Effects ---')\n"
    "        lines.append('  mud.float_text(\"text\", r, g, b)         Basic floating text')\n"
    "        lines.append('  mud.float_text(\"text\", r, g, b,         Full options:')\n"
    "        lines.append('    size=48, bold=1, shadow=1,             font size + style')\n"
    "        lines.append('    r2=R, g2=G, b2=B,                     gradient bottom color')\n"
    "        lines.append('    glow=3, glow_r=R, glow_g=G, glow_b=B, outer glow')\n"
    "        lines.append('    rise=1, fade_ms=30, duration=80)       animation')\n"
    "        lines.append('')\n"
    "        lines.append('--- Vulkan Floating Text (VFT) ---')\n"
    "        lines.append('  vft(\"text|x|y|params...\")               GPU floating text')\n"
    "        lines.append('  vft(\"clear\")                            Clear all VFT')\n"
    "        lines.append('  vft_help()                              Full VFT reference')\n"
    "        lines.append('  vft_fonts()                             List VFT fonts (0-4)')\n"
    "        lines.append('  Type help(\"vft\") for full syntax + examples')\n"
    "        lines.append('')\n"
    "        lines.append('--- Scripts ---')\n"
    "        lines.append('  mud.scripts()              List saved scripts')\n"
    "        lines.append('  mud.save(\"name\", code)     Save a script')\n"
    "        lines.append('  mud.load(\"name\")           Load and run a script')\n"
    "        lines.append('  mud.edit(\"name\")           Print script contents')\n"
    "        lines.append('  mud.delete(\"name\")         Delete a script')\n"
    "        lines.append('')\n"
    "        lines.append('--- Toggles (no arg=toggle, True/False=set) ---')\n"
    "        lines.append('  mud.autocombat()   mud.autonuke()    mud.autoheal()')\n"
    "        lines.append('  mud.autobless()    mud.autolight()   mud.autocash()')\n"
    "        lines.append('  mud.autoget()      mud.autosearch()  mud.autosneak()')\n"
    "        lines.append('  mud.autohide()     mud.autotrack()   mud.autotrain()')\n"
    "        lines.append('')\n"
    "        lines.append('--- Remote (@commands, no telepath) ---')\n"
    "        lines.append('  mud.remote.stop()              Stop movement')\n"
    "        lines.append('  mud.remote.rego()              Resume movement')\n"
    "        lines.append('  mud.remote.loop(\"name\")        Start loop by name')\n"
    "        lines.append('  mud.remote.goto(\"room\")        Go to room by name')\n"
    "        lines.append('  mud.remote.roam(True/False)    Auto-roam on/off')\n"
    "        lines.append('  mud.remote.reset()             Reset flags/stats')\n"
    "        lines.append('')\n"
    "        lines.append('--- Offsets ---')\n"
    "        lines.append('  Type OFF to see all game struct offsets.')\n"
    "        return '\\n'.join(lines)\n"
    "\n"
    "class _Remote:\n"
    "    '''Execute MegaMUD @commands directly - no telepath, no response.\n"
    "    Calls the exact same internal functions as the real @commands.\n"
    "    Usage: mud.remote.loop(\"ancient crypt\")'''\n"
    "    def _call(self, cmd):\n"
    "        rv = _dll.mmudpy_fake_remote(cmd.encode('utf-8'))\n"
    "        if rv != 0:\n"
    "            print(f'remote: {cmd} - failed')\n"
    "        return rv == 0\n"
    "    def stop(self):\n"
    "        '''Stop current path/movement.'''\n"
    "        return self._call('stop')\n"
    "    def rego(self):\n"
    "        '''Resume path movement.'''\n"
    "        return self._call('rego')\n"
    "    def loop(self, name):\n"
    "        '''Start a loop by name or filename. Case-insensitive.'''\n"
    "        return self._call(f'loop {name}')\n"
    "    def goto(self, room):\n"
    "        '''Go to a room by name. Case-insensitive.'''\n"
    "        return self._call(f'goto {room}')\n"
    "    def roam(self, on=True):\n"
    "        '''Set auto-roaming on or off.'''\n"
    "        return self._call(f'roam {\"on\" if on else \"off\"}')\n"
    "    def reset(self):\n"
    "        '''Reset all MegaMUD internal flags and statistics.'''\n"
    "        return self._call('reset')\n"
    "    def __repr__(self):\n"
    "        return ('MegaMUD Remote Commands (no telepath):\\n'\n"
    "                '  mud.remote.stop()              - stop movement\\n'\n"
    "                '  mud.remote.rego()              - resume movement\\n'\n"
    "                '  mud.remote.loop(\"name\")        - start loop\\n'\n"
    "                '  mud.remote.goto(\"room\")        - go to room\\n'\n"
    "                '  mud.remote.roam(True/False)    - auto-roam on/off\\n'\n"
    "                '  mud.remote.reset()             - reset flags/stats')\n"
    "\n"
    "mud = _MudAPI()\n"
    "mud.remote = _Remote()\n"
    "\n"
    "# Track loaded scripts for unloading\n"
    "_loaded_scripts = {}\n"
    "\n"
    "def help(topic=None):\n"
    "    '''Show help. Usage: help() or help(\"hp\") or help(\"scripts\").'''\n"
    "    topics = {\n"
    "        'hp': 'mud.hp() -> (cur, max)\\nmud.hp_pct() -> 0-100\\nmud.read_i32(OFF.CUR_HP) for raw value',\n"
    "        'mana': 'mud.mana() -> (cur, max)\\nmud.mana_pct() -> 0-100\\nmud.read_i32(OFF.CUR_MANA) for raw value',\n"
    "        'combat': 'mud.in_combat() -> True/False\\nmud.is_held() / mud.is_stunned() / mud.is_blinded()\\nmud.command(\"attack target\") to send commands',\n"
    "        'room': 'mud.room_name() -> current room name\\nmud.read_i32(OFF.ROOM_EXITS) for exit flags\\nmud.read_i32(OFF.ROOM_DARKNESS) for darkness flag',\n"
    "        'player': 'mud.player_name() / mud.level()\\nmud.strength() / mud.agility() / etc.\\nmud.read_string(OFF.PLAYER_GANG, 32) for gang name',\n"
    "        'state': 'mud.is_resting() / mud.is_meditating() / mud.is_sneaking()\\nmud.is_hiding() / mud.is_fleeing() / mud.is_looping()\\nmud.is_poisoned() / mud.is_held() / mud.is_stunned()',\n"
    "        'connection': 'mud.connect()       - connect to BBS\\nmud.hangup()        - disconnect (bypasses confirmation)\\nmud.is_connected()  - True if connected\\nmud.is_in_game()    - True if in game world',\n"
    "        'commands': 'mud.command(\"say hello\") -> types + Enter\\nmud.text(\"partial\") -> types without Enter\\nWorks just like typing in MegaMUD.',\n"
    "        'memory': 'mud.read_i32(OFF.X) / mud.write_i32(OFF.X, val)\\nmud.read_i16() / mud.write_i16()\\nmud.read_string(OFF.X, maxlen) / mud.write_string(OFF.X, text, maxlen)\\nAll offsets in OFF class, type OFF to see them.',\n"
    "        'scripts': 'mud.scripts() -> list all scripts\\nmud.load(\"name\") -> load and run\\nmud.stop(\"name\") -> stop a running script\\nmud.edit(\"name\") -> show script source\\nmud.save(\"name\", code) -> save new script\\nmud.delete(\"name\") -> remove script\\n\\nScripts have start()/stop() functions.\\nLoad from menu: Plugins > MMUDPy > script name',\n"
    "        'offsets': 'Type OFF to see all game struct offsets.\\nUse with mud.read_i32(OFF.X) / mud.write_i32(OFF.X, val).\\nAll offsets are read/write.',\n"
    "        'remote': 'mud.remote - direct MegaMUD @commands (no telepath):\\n  mud.remote.stop()              - stop movement\\n  mud.remote.rego()              - resume movement\\n  mud.remote.loop(\"name\")        - start loop\\n  mud.remote.goto(\"room\")        - go to room\\n  mud.remote.roam(True/False)    - auto-roam on/off\\n  mud.remote.reset()             - reset flags/stats\\n\\nType mud.remote to see the list.',\n"
    "        'toggles': 'Automation toggles - call with no arg to toggle, True/False to set:\\n  mud.autocombat()  mud.autonuke()   mud.autoheal()\\n  mud.autobless()   mud.autolight()  mud.autocash()\\n  mud.autoget()     mud.autosearch() mud.autosneak()\\n  mud.autohide()    mud.autotrack()  mud.autotrain()\\n\\nReturns the new state (True/False).\\nExample: mud.autocombat(False)  - turn off auto-combat',\n"
    "        'vft': 'Vulkan Floating Text (VFT) - GPU-rendered floating text FX\\n\\n"
    "SYNTAX: vft(\"text|x|y|param=val|...\")\\n\\n"
    "POSITION:\\n"
    "  x,y = 0.0-1.0 or: center, left, right, top, bottom\\n\\n"
    "SIZE & COLOR:\\n"
    "  size=N        Scale multiplier (default 2.0, try 1-8)\\n"
    "  color=NAME    270+ named colors: red, gold, crimson, cyan,\\n"
    "                emerald, cobalt, plasma, lava, frost, neonpink,\\n"
    "                periwinkle, burntsienna, obsidian, mithril...\\n"
    "  color=RRGGBB  Hex color: ff0000, 00ff00, etc.\\n"
    "  alpha=N       Opacity 0.0-1.0\\n\\n"
    "FONT (font=N, 0-4):\\n"
    "  0=Black Ops  1=Teko  2=Russo  3=Orbitron  4=Rajdhani\\n\\n"
    "INTRO EFFECTS (intro=name or intro=name:duration):\\n"
    "  fade, zoom_in, zoom_out, slide_l, slide_r, slide_u,\\n"
    "  slide_d, bounce, pop, typewriter, wave_in, spin\\n\\n"
    "OUTRO EFFECTS (outro=name or outro=name:duration):\\n"
    "  fade, zoom_out, shatter, explode, melt, dissolve,\\n"
    "  evaporate, spin\\n\\n"
    "TIMING:\\n"
    "  hold=N        Hold seconds (default 2.0)\\n"
    "  intro_dur=N   Intro seconds (default 0.3)\\n"
    "  outro_dur=N   Outro seconds (default 0.5)\\n\\n"
    "MODIFIERS (combine any):\\n"
    "  glow=1         Outer glow       shadow=1    Drop shadow\\n"
    "  outline=1      Text outline     shake=N     Shake intensity\\n"
    "  wave=N         Wave motion      pulse=1     Pulsing scale\\n"
    "  rainbow=1      Rainbow cycle    trail=1     Motion trail\\n"
    "  sparks=1       Spark particles  helix=1     Helix particles\\n"
    "  chromatic=1    Chromatic shift  fire=1      Fire effect\\n"
    "  breathe=1      Breathing scale  orbit=1     Orbiting dots\\n\\n"
    "COMMANDS:\\n"
    "  vft(\"clear\")       Clear all floating text\\n"
    "  vft(\"clear:ID\")    Clear specific text by ID\\n"
    "  vft_help()         Full reference (in console)\\n"
    "  vft_fonts()        List available fonts\\n\\n"
    "EXAMPLES:\\n"
    "  vft(\"CRITICAL HIT!|0.5|0.3|size=5|color=red\")\\n"
    "  vft(\"CRITICAL HIT!|center|center|size=5|color=crimson\"\\n"
    "      \"|intro=zoom_in|outro=shatter|glow=1\")\\n"
    "  vft(\"LEVEL UP!|center|0.3|size=6|color=gold\"\\n"
    "      \"|intro=bounce|outro=explode|rainbow=1|sparks=1\")\\n"
    "  vft(\"+9999 HP|0.5|0.5|size=4|color=emerald\"\\n"
    "      \"|intro=pop|outro=fade|pulse=1|glow=1\")\\n"
    "  vft(\"BACKSTAB!|0.7|0.2|size=5|color=purple\"\\n"
    "      \"|intro=slide_l|outro=dissolve|shadow=1|trail=1\")\\n"
    "  vft(\"GODLIKE!|center|center|size=8|color=gold\"\\n"
    "      \"|font=3|intro=spin|outro=explode|fire=1|chromatic=1\")\\n"
    "  vft(\"Healing...|0.5|0.8|size=3|color=cyan\"\\n"
    "      \"|intro=fade|outro=evaporate|breathe=1|glow=1\")',\n"
    "    }\n"
    "    # Plugins can register topics via: help.__wrapped_topics__['name'] = 'text'\n"
    "    if hasattr(help, '__wrapped_topics__'):\n"
    "        topics.update(help.__wrapped_topics__)\n"
    "    if topic is None:\n"
    "        print('=== MMUDPy Help ===')\n"
    "        print('Type help(\"topic\") for details on:')\n"
    "        # Show core topics first, then any plugin-registered ones\n"
    "        core = ['hp', 'mana', 'combat', 'room', 'player', 'state', 'connection', 'commands', 'memory', 'scripts', 'offsets', 'toggles', 'remote', 'vft']\n"
    "        plugin_topics = [k for k in sorted(topics.keys()) if k not in core]\n"
    "        print('  ' + ', '.join(core))\n"
    "        if plugin_topics:\n"
    "            print('  Plugins: ' + ', '.join(plugin_topics))\n"
    "        print()\n"
    "        print('Type `mud` to see all API methods.')\n"
    "        print('Type `OFF` to see all struct offsets.')\n"
    "        return\n"
    "    t = topic.lower().strip()\n"
    "    if t in topics:\n"
    "        print(topics[t])\n"
    "    else:\n"
    "        print(f'Unknown topic: {topic}')\n"
    "        print('Available: ' + ', '.join(sorted(topics.keys())))\n"
    "\n"
    "# Plugin topic registry - plugins inject via mmudpy_exec:\n"
    "#   help.__wrapped_topics__['name'] = 'help text'\n"
    "help.__wrapped_topics__ = {}\n"
    "\n"
    "print('MMUDPy Console ready. Type `mud` for API, `help()` for help.')\n"
    "print()\n"
;

/* ---- Load Python runtime ---- */

/* VEH + longjmp exception catcher — MinGW gcc (dwarf2 eh) does not support
 * MS-style __try/__except, so we use a Vectored Exception Handler combined
 * with setjmp/longjmp to unwind back to a safe point if Python's init
 * crashes (missing VCRuntime, corrupted zip, bad stdlib, etc.). This lets
 * us disable the Python bridge gracefully instead of killing MegaMUD —
 * exactly the failure mode seen on Win11 with python312.dll @ 0x00244045. */
static jmp_buf  seh_jmpbuf;
static volatile LONG seh_armed = 0;
static volatile DWORD seh_caught_code = 0;

static LONG CALLBACK seh_veh_handler(EXCEPTION_POINTERS *ep)
{
    if (!seh_armed || !ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    /* Only catch hard faults — let debug breakpoints, C++ exceptions, etc. through. */
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    seh_armed = 0;
    seh_caught_code = code;
    longjmp(seh_jmpbuf, 1);
    /* unreachable */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Returns 0 on success, or the SEH exception code on crash. */
static int seh_py_initialize(void)
{
    PVOID handler = AddVectoredExceptionHandler(1, seh_veh_handler);
    int rc;
    seh_caught_code = 0;
    if (setjmp(seh_jmpbuf) == 0) {
        seh_armed = 1;
        pPy_Initialize();
        seh_armed = 0;
        rc = 0;
    } else {
        /* longjmp landed here — Python crashed */
        seh_armed = 0;
        rc = (int)seh_caught_code;
    }
    if (handler) RemoveVectoredExceptionHandler(handler);
    return rc;
}

static int seh_py_run_bootstrap(const char *code)
{
    PVOID handler = AddVectoredExceptionHandler(1, seh_veh_handler);
    int rc;
    seh_caught_code = 0;
    if (setjmp(seh_jmpbuf) == 0) {
        seh_armed = 1;
        rc = pPyRun_SimpleString(code);
        seh_armed = 0;
    } else {
        seh_armed = 0;
        rc = -0x10000 | ((int)seh_caught_code & 0xFFFF);
    }
    if (handler) RemoveVectoredExceptionHandler(handler);
    return rc;
}

static int load_python(void)
{
    /* Build path to python312.dll relative to MegaMUD exe */
    char py_dir[MAX_PATH];
    GetModuleFileNameA(NULL, py_dir, MAX_PATH);
    /* Strip exe name */
    char *slash = strrchr(py_dir, '\\');
    if (slash) *slash = '\0';
    strcat(py_dir, "\\plugins\\python");

    char py_dll[MAX_PATH];
    sprintf(py_dll, "%s\\python312.dll", py_dir);

    /* Suppress Windows' error dialogs — if python312.dll is missing deps
     * (vcruntime, CRT version mismatch, etc.) we want silent failure, not
     * a modal popup that blocks the whole app. */
    UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    /* Temporarily add python dir to DLL search path, then restore */
    char old_dll_dir[MAX_PATH] = {0};
    DWORD old_len = GetDllDirectoryA(MAX_PATH, old_dll_dir);
    SetDllDirectoryA(py_dir);

    hPython = LoadLibraryA(py_dll);
    if (!hPython) {
        /* Try just the name in case it's on PATH */
        hPython = LoadLibraryA("python312.dll");
    }

    /* Restore original DLL search path and error mode */
    SetDllDirectoryA(old_len > 0 ? old_dll_dir : NULL);
    SetErrorMode(old_mode);
    if (!hPython) {
        api->log("[mmudpy] Failed to load python312.dll from %s (err=%lu) — Python bridge disabled\n",
                 py_dll, GetLastError());
        return -1;
    }

    api->log("[mmudpy] Loaded python312.dll from %s\n", py_dll);

    /* Load function pointers */
    #define LOAD_PY(name) p##name = (name##_t)GetProcAddress(hPython, #name); \
        if (!p##name) { api->log("[mmudpy] Missing: %s\n", #name); return -1; }

    LOAD_PY(Py_Initialize);
    LOAD_PY(Py_Finalize);
    LOAD_PY(Py_IsInitialized);
    LOAD_PY(PyRun_SimpleString);
    LOAD_PY(PyImport_AddModule);
    LOAD_PY(PyModule_GetDict);
    LOAD_PY(PyObject_Repr);
    LOAD_PY(PyUnicode_AsUTF8);
    LOAD_PY(PyErr_Print);
    LOAD_PY(PyErr_Clear);
    LOAD_PY(PyErr_Fetch);
    LOAD_PY(PyErr_Restore);

    #undef LOAD_PY

    /* These might have different names */
    pPy_DecRef = (Py_DecRef_t)GetProcAddress(hPython, "Py_DecRef");
    pPyErr_Occurred = (PyErr_Occurred_t)GetProcAddress(hPython, "PyErr_Occurred");
    pPy_CompileString = (Py_CompileString_p_t)GetProcAddress(hPython, "Py_CompileString");
    pPyRun_String = (PyRun_String_t)GetProcAddress(hPython, "PyRun_String");
    pPyGILState_Ensure = (PyGILState_Ensure_t)GetProcAddress(hPython, "PyGILState_Ensure");
    pPyGILState_Release = (PyGILState_Release_t)GetProcAddress(hPython, "PyGILState_Release");
    pPyEval_SaveThread = (PyEval_SaveThread_t)GetProcAddress(hPython, "PyEval_SaveThread");
    pPyEval_RestoreThread = (PyEval_RestoreThread_t)GetProcAddress(hPython, "PyEval_RestoreThread");
    /* Needed for cross-thread Py_Finalize — see mmudpy_shutdown */
    pPyInterpreterState_Main = (PyInterpreterState_Main_t)GetProcAddress(hPython, "PyInterpreterState_Main");
    pPyThreadState_New       = (PyThreadState_New_t)GetProcAddress(hPython, "PyThreadState_New");
    pPyEval_AcquireThread    = (PyEval_AcquireThread_t)GetProcAddress(hPython, "PyEval_AcquireThread");

    /* ---------- Python 3.12 embeddable init (the "just works" recipe) --------
     * The embeddable distribution ships a python312._pth file next to the DLL.
     * When Python sees that file, it configures sys.path from it and ignores
     * PYTHONPATH/PYTHONHOME env vars — this is "isolated mode" and is the
     * supported way to embed Python 3.12.
     *
     * What was breaking us on Win11 (python312.dll!+0x244045 AV crash):
     *   - We were calling Py_SetPythonHome() AND Py_SetPath() in addition to
     *     having a _pth file. These legacy calls are deprecated in 3.12 and
     *     create inconsistent state with the _pth-driven path config — Python
     *     crashes during encodings-module import with a NULL deref.
     *   - Inherited PYTHONHOME/PYTHONPATH env vars (from a system-installed
     *     Python) can also poison the init.
     *
     * Fix: clear the env vars, DO NOT call the deprecated setters, cd into
     * the python dir so _pth gets picked up deterministically, let Python
     * auto-configure itself from the _pth.
     * ------------------------------------------------------------------------- */

    /* Clear hostile env vars that override _pth / confuse init */
    _putenv("PYTHONHOME=");
    _putenv("PYTHONPATH=");
    _putenv("PYTHONSTARTUP=");
    _putenv("PYTHONDONTWRITEBYTECODE=1");  /* embed = no .pyc writing */
    _putenv("PYTHONNOUSERSITE=1");
    _putenv("PYTHONIOENCODING=utf-8");

    /* Temporarily chdir to python dir so Python finds python312._pth
     * reliably regardless of where MegaMUD was launched from.  We restore
     * CWD after Py_Initialize returns. */
    char saved_cwd[MAX_PATH] = {0};
    GetCurrentDirectoryA(MAX_PATH, saved_cwd);
    SetCurrentDirectoryA(py_dir);
    api->log("[mmudpy] Python init: cwd -> %s (was %s)\n", py_dir, saved_cwd);

    /* Initialize Python — wrapped in SEH so a crash inside libpython (e.g.
     * missing MSVC runtime, corrupted embedded stdlib) disables the Python
     * bridge gracefully instead of crashing the whole process. This is
     * exactly what bit us on Win11 with python312.dll @ offset 0x00244045. */
    api->log("[mmudpy] Calling Py_Initialize (under SEH guard)...\n");
    int init_ex = seh_py_initialize();

    /* Restore original CWD regardless of init outcome */
    if (saved_cwd[0]) SetCurrentDirectoryA(saved_cwd);
    api->log("[mmudpy] Python init: cwd restored to %s\n", saved_cwd);

    if (init_ex != 0) {
        api->log("[mmudpy] Py_Initialize crashed with SEH exception 0x%08X — Python bridge disabled\n",
                 init_ex);
        FreeLibrary(hPython);
        hPython = NULL;
        return -1;
    }

    if (!pPy_IsInitialized()) {
        api->log("[mmudpy] Py_Initialize returned but Py_IsInitialized is false — Python bridge disabled\n");
        return -1;
    }

    api->log("[mmudpy] Python initialized\n");

    /* Run bootstrap — also SEH-wrapped */
    int rc = seh_py_run_bootstrap(py_bootstrap);
    if (rc != 0) {
        api->log("[mmudpy] Bootstrap failed (rc=%d)\n", rc);
    }

    /* ---- Scan for extension commands from other plugins ---- */
    {
        char plugins_dir[MAX_PATH];
        GetModuleFileNameA(NULL, plugins_dir, MAX_PATH);
        char *sl2 = strrchr(plugins_dir, '\\');
        if (sl2) *sl2 = '\0';
        strcat(plugins_dir, "\\plugins\\");

        char search_pat[MAX_PATH];
        sprintf(search_pat, "%s*.dll", plugins_dir);

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search_pat, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                /* Skip mmudpy.dll itself */
                if (_stricmp(fd.cFileName, "mmudpy.dll") == 0) continue;

                /* Use full path - GetModuleHandleA(filename) fails for
                   DLLs loaded from subdirectories on Wine */
                char full_dll[MAX_PATH];
                sprintf(full_dll, "%s%s", plugins_dir, fd.cFileName);
                HMODULE hMod = GetModuleHandleA(full_dll);
                if (!hMod) {
                    /* Try LoadLibrary - returns existing handle if already loaded */
                    hMod = LoadLibraryA(full_dll);
                }
                if (!hMod) continue;

                typedef slop_command_t *(*get_cmds_fn)(int *);
                get_cmds_fn get_cmds = (get_cmds_fn)GetProcAddress(hMod, "slop_get_commands");
                if (!get_cmds) continue;

                int cmd_count = 0;
                slop_command_t *cmds = get_cmds(&cmd_count);
                if (!cmds || cmd_count <= 0) continue;

                api->log("[mmudpy] Found %d ext commands in %s\n", cmd_count, fd.cFileName);

                /* Generate Python wrapper: load DLL via ctypes, wrap each command */
                char py_buf[32768];
                int pos = 0;

                /* Load the DLL in Python */
                pos += sprintf(py_buf + pos,
                    "_ext_%.*s = ctypes.CDLL(r'%s%s')\n",
                    (int)(strchr(fd.cFileName, '.') ? strchr(fd.cFileName, '.') - fd.cFileName : strlen(fd.cFileName)),
                    fd.cFileName,
                    plugins_dir, fd.cFileName);

                char dll_var[64];
                int namelen = (int)(strchr(fd.cFileName, '.') ? strchr(fd.cFileName, '.') - fd.cFileName : strlen(fd.cFileName));
                sprintf(dll_var, "_ext_%.*s", namelen, fd.cFileName);

                /* Pre-scan: create SimpleNamespace objects for dotted prefixes
                 * e.g. "radio.play" needs "mud.radio = types.SimpleNamespace()" */
                {
                    char seen_ns[32][64];
                    int num_ns = 0;
                    for (int ci2 = 0; ci2 < cmd_count; ci2++) {
                        const char *dot = strchr(cmds[ci2].py_name, '.');
                        if (!dot) continue;
                        int plen = (int)(dot - cmds[ci2].py_name);
                        if (plen <= 0 || plen >= 63) continue;
                        char prefix[64];
                        memcpy(prefix, cmds[ci2].py_name, plen);
                        prefix[plen] = '\0';
                        /* Check if already seen */
                        int found = 0;
                        for (int k = 0; k < num_ns; k++)
                            if (strcmp(seen_ns[k], prefix) == 0) { found = 1; break; }
                        if (!found && num_ns < 32) {
                            strcpy(seen_ns[num_ns++], prefix);
                            pos += sprintf(py_buf + pos,
                                "if not hasattr(mud, '%s'): mud.%s = type('', (), {})()\n",
                                prefix, prefix);
                        }
                    }
                }

                for (int ci = 0; ci < cmd_count; ci++) {
                    slop_command_t *c = &cmds[ci];

                    /* Build argtypes list */
                    char argtypes[256] = "[";
                    int alen = 1;
                    for (const char *a = c->arg_types; *a; a++) {
                        if (alen > 1) { argtypes[alen++] = ','; argtypes[alen++] = ' '; }
                        switch (*a) {
                        case 'i': alen += sprintf(argtypes + alen, "ctypes.c_int"); break;
                        case 's': alen += sprintf(argtypes + alen, "ctypes.c_char_p"); break;
                        case 'f': alen += sprintf(argtypes + alen, "ctypes.c_float"); break;
                        default:  alen += sprintf(argtypes + alen, "ctypes.c_int"); break;
                        }
                    }
                    argtypes[alen++] = ']';
                    argtypes[alen] = '\0';

                    /* restype */
                    const char *restype = "None";
                    if (c->ret_type && c->ret_type[0] == 'i') restype = "ctypes.c_int";
                    else if (c->ret_type && c->ret_type[0] == 's') restype = "ctypes.c_char_p";
                    else if (c->ret_type && c->ret_type[0] == 'f') restype = "ctypes.c_float";

                    pos += sprintf(py_buf + pos,
                        "%s.%s.restype = %s\n"
                        "%s.%s.argtypes = %s\n",
                        dll_var, c->c_func, restype,
                        dll_var, c->c_func, argtypes);

                    /* Build lambda - handle string encoding for 's' args */
                    int has_str = (strchr(c->arg_types, 's') != NULL);
                    if (c->arg_types[0] == '\0') {
                        /* No args */
                        pos += sprintf(py_buf + pos,
                            "mud.%s = lambda: %s.%s()\n",
                            c->py_name, dll_var, c->c_func);
                    } else if (!has_str) {
                        /* All numeric - simple passthrough */
                        char params[128] = "";
                        int plen = 0;
                        int argn = 0;
                        for (const char *a = c->arg_types; *a; a++, argn++) {
                            if (plen > 0) plen += sprintf(params + plen, ", ");
                            plen += sprintf(params + plen, "a%d", argn);
                        }
                        pos += sprintf(py_buf + pos,
                            "mud.%s = lambda %s: %s.%s(%s)\n",
                            c->py_name, params, dll_var, c->c_func, params);
                    } else {
                        /* Has string args - need encoding */
                        char params[128] = "";
                        char call[256] = "";
                        int plen = 0, clen = 0, argn = 0;
                        for (const char *a = c->arg_types; *a; a++, argn++) {
                            if (plen > 0) plen += sprintf(params + plen, ", ");
                            plen += sprintf(params + plen, "a%d", argn);
                            if (clen > 0) clen += sprintf(call + clen, ", ");
                            if (*a == 's')
                                clen += sprintf(call + clen, "a%d.encode('utf-8') if isinstance(a%d, str) else a%d", argn, argn, argn);
                            else
                                clen += sprintf(call + clen, "a%d", argn);
                        }
                        pos += sprintf(py_buf + pos,
                            "mud.%s = lambda %s: %s.%s(%s)\n",
                            c->py_name, params, dll_var, c->c_func, call);
                    }

                    /* Add docstring as attribute (escape quotes) */
                    if (c->doc && c->doc[0]) {
                        char edoc[256];
                        int el = 0;
                        for (const char *dp = c->doc; *dp && el < 250; dp++) {
                            if (*dp == '\'') { edoc[el++] = '\\'; edoc[el++] = '\''; }
                            else edoc[el++] = *dp;
                        }
                        edoc[el] = '\0';
                        pos += sprintf(py_buf + pos,
                            "mud.%s.__doc__ = '%s'\n",
                            c->py_name, edoc);
                    }

                    if (pos > 30000) break;  /* safety */
                }

                api->log("[mmudpy] Running ext wrapper:\n%s\n", py_buf);
                pPyRun_SimpleString(py_buf);

            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }

    /* Top-level convenience aliases for VFT (matches help docs) */
    pPyRun_SimpleString(
        "if hasattr(mud, 'vft'):\n"
        "    import builtins\n"
        "    builtins.vft = mud.vft\n"
        "    builtins.vft_help = mud.vft_help\n"
        "    builtins.vft_fonts = mud.vft_fonts\n"
        "    builtins.vft_set_font = mud.vft_set_font\n"
    );

    py_ready = 1;
    return 0;
}

/* ---- Console thread ---- */

static DWORD WINAPI console_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    create_console_window(hInst);

    /* Init async console output buffer */
    InitializeCriticalSection(&con_ring_lock);
    con_ring_lock_init = 1;
    InitializeCriticalSection(&float_queue_lock);
    float_queue_lock_init = 1;
    SetTimer(con_hwnd, CON_FLUSH_TIMER_ID, 50, NULL);  /* flush every 50ms */

    /* Load Python after console window exists (so bootstrap output goes there) */
    if (load_python() == 0) {
        api->log("[mmudpy] Python ready, console open\n");
    } else {
        con_append("ERROR: Failed to load Python.\r\n");
        con_append("Make sure python312.dll is in plugins/python/\r\n");
    }

    /* Release the GIL so Python threads (scripts) can run */
    if (pPyEval_SaveThread) {
        saved_gil_state = pPyEval_SaveThread();
        dbg("[gil] Released GIL before message loop\n");
    }

    /* Message loop for the console window */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}

/* ---- Eval pump - processes queued Python from vk_terminal ---- */
/* Called from the console window's 50ms timer so it works even offline */

static void eval_pump(void)
{
    if (!eval_pending || !py_ready || !pPyRun_SimpleString) return;

    char code_copy[EVAL_BUF_SZ];
    int target;
    EnterCriticalSection(&eval_lock);
    strncpy(code_copy, eval_code, EVAL_BUF_SZ);
    target = eval_target;
    eval_pending = 0;
    LeaveCriticalSection(&eval_lock);

    int gil = 0;
    if (pPyGILState_Ensure) gil = pPyGILState_Ensure();
    EnterCriticalSection(&py_lock);

    /* Write code to temp file to avoid escaping hell */
    {
        FILE *fp = fopen("C:\\MegaMUD\\_vkt_eval.tmp", "w");
        if (fp) { fputs(code_copy, fp); fclose(fp); }
    }

    /* Redirect stdout+stderr, eval/exec, capture all output including errors */
    pPyRun_SimpleString(
        "import io as _io, sys as _sys, traceback as _tb\n"
        "_vkt_buf = _io.StringIO()\n"
        "_vkt_oldout = _sys.stdout\n"
        "_vkt_olderr = _sys.stderr\n"
        "_sys.stdout = _vkt_buf\n"
        "_sys.stderr = _vkt_buf\n"
        "try:\n"
        "    with open(r'C:\\MegaMUD\\_vkt_eval.tmp', 'r') as _f:\n"
        "        _vkt_code = _f.read()\n"
        "    try:\n"
        "        _vkt_r = eval(compile(_vkt_code, '<console>', 'eval'))\n"
        "        if _vkt_r is not None: print(repr(_vkt_r))\n"
        "    except SyntaxError:\n"
        "        exec(compile(_vkt_code, '<console>', 'exec'))\n"
        "except Exception:\n"
        "    _tb.print_exc()\n"
        "_sys.stdout = _vkt_oldout\n"
        "_sys.stderr = _vkt_olderr\n"
        "_vkt_out = _vkt_buf.getvalue()\n"
        "del _vkt_buf, _vkt_oldout, _vkt_olderr\n"
    );

    /* Route output to the right target */
    if (target == 0) {
        /* `` prefix in terminal: inject as server data (ASCII safe for CP437) */
        pPyRun_SimpleString(
            "if '_vkt_out' in dir() and _vkt_out:\n"
            "    import ctypes as _ct\n"
            "    _api = _ct.WinDLL('msimg32.dll')\n"
            "    if hasattr(_api, 'slop_inject_server_data'):\n"
            "        _d = ('\\r\\n' + _vkt_out).encode('latin-1', 'replace')\n"
            "        _api.slop_inject_server_data(_d, len(_d))\n"
            "if '_vkt_out' in dir(): del _vkt_out\n"
        );
    } else {
        /* Console window: print each line to the Vulkan window (ASCII safe) */
        char target_script[512];
        wsprintfA(target_script,
            "if '_vkt_out' in dir() and _vkt_out:\n"
            "    import ctypes as _ct\n"
            "    _vkt = _ct.WinDLL('vk_terminal.dll')\n"
            "    for _ln in _vkt_out.rstrip('\\n').split('\\n'):\n"
            "        _vkt.vkt_wnd_print(%d, _ln.encode('latin-1', 'replace'))\n"
            "if '_vkt_out' in dir(): del _vkt_out\n",
            target);
        pPyRun_SimpleString(target_script);
    }

    LeaveCriticalSection(&py_lock);
    if (pPyGILState_Release) pPyGILState_Release(gil);
}

/* ---- Plugin callbacks ---- */

static void mmudpy_on_line(const char *line)
{
    dbg_on_line_count++;
    if (dbg_on_line_count <= 5)
        dbg("[on_line] #%d: %.60s\n", dbg_on_line_count, line);
    /* Buffer lines for Python scripts to poll - all readers share the buffer */
    EnterCriticalSection(&line_lock);
    int next = (line_buf_head + 1) % MAX_LINE_BUF;

    /* Free oldest line if buffer wraps */
    if (line_buffer[line_buf_head]) {
        free(line_buffer[line_buf_head]);
        line_buffer[line_buf_head] = NULL;
    }

    line_buffer[line_buf_head] = _strdup(line);
    line_buf_head = next;

    /* Advance any reader tails that fell behind (buffer wrapped past them) */
    for (int i = 0; i < MAX_LINE_READERS; i++) {
        if (line_reader_active[i] && line_buf_tail[i] == next) {
            line_buf_tail[i] = (next + 1) % MAX_LINE_BUF;
        }
    }

    LeaveCriticalSection(&line_lock);
}

static void mmudpy_on_clean_line(const char *line)
{
    /* Parallel buffer for ANSI-stripped lines. Reuses the same wrap /
     * reader-tail-catchup logic as mmudpy_on_line but against the clean
     * stream. */
    EnterCriticalSection(&line_lock);
    int next = (clean_line_buf_head + 1) % MAX_LINE_BUF;

    if (clean_line_buffer[clean_line_buf_head]) {
        free(clean_line_buffer[clean_line_buf_head]);
        clean_line_buffer[clean_line_buf_head] = NULL;
    }

    clean_line_buffer[clean_line_buf_head] = _strdup(line);
    clean_line_buf_head = next;

    for (int i = 0; i < MAX_LINE_READERS; i++) {
        if (line_reader_active[i] && clean_line_buf_tail[i] == next) {
            clean_line_buf_tail[i] = (next + 1) % MAX_LINE_BUF;
        }
    }

    LeaveCriticalSection(&line_lock);
}

static void mmudpy_on_round(int round_num)
{
    last_round_num = round_num;
}

/* ---- Plugin lifecycle ---- */

static int mmudpy_init(const slop_api_t *a)
{
    api = a;
    menu_base = a->menu_base_id;
    InitializeCriticalSection(&py_lock);
    InitializeCriticalSection(&line_lock);
    InitializeCriticalSection(&eval_lock);

    /* Ensure scripts directory exists */
    char scripts_dir[MAX_PATH];
    GetModuleFileNameA(NULL, scripts_dir, MAX_PATH);
    char *sl = strrchr(scripts_dir, '\\');
    if (sl) *sl = '\0';
    strcat(scripts_dir, "\\plugins\\MMUDPy\\scripts");
    CreateDirectoryA(scripts_dir, NULL);

    api->log("[mmudpy] MMUDPy plugin initializing...\n");

    /* Start console + Python on its own thread */
    HINSTANCE hInst = GetModuleHandleA(NULL);
    CreateThread(NULL, 0, console_thread, (LPVOID)hInst, 0, NULL);

    return 0;
}

static void mmudpy_shutdown(void)
{
    api->log("[mmudpy] Shutting down...\n");

    py_ready = 0;

    /* Finalize Python.
     *
     * CRASH FIX (Win11 python312.dll+0x244045 = Py_FinalizeEx+0x35):
     *   Py_Finalize MUST be called with a Python thread state attached to
     *   the *calling* thread's platform TLS slot.  We init Python on
     *   console_thread, but shutdown runs on the main thread (plugin
     *   unloader) — which has no entry in Python's TLS.  Py_FinalizeEx's
     *   very first action (mov edi,[tls[index]] then mov eax,[edi+8])
     *   AVs on the NULL edi.
     *
     *   First attempt used PyEval_AcquireThread(PyThreadState_New(...))
     *   but in Python 3.12 AcquireThread updates the runtime's
     *   current-tstate tracker WITHOUT writing the platform TLS slot that
     *   Py_FinalizeEx reads — same crash.
     *
     *   Correct fix: PyGILState_Ensure().  It's specifically designed for
     *   "foreign thread wants to call into Python" — auto-creates a
     *   tstate, writes it into TLS, and acquires the GIL, atomically.
     *   After Py_Finalize we don't Release: the state is invalidated by
     *   finalization and the process is exiting anyway.
     */
    if (hPython && pPy_IsInitialized && pPy_IsInitialized()) {
        if (pPy_Finalize && pPyGILState_Ensure) {
            (void)pPyGILState_Ensure();  /* writes TLS + acquires GIL */
            pPy_Finalize();
        } else if (api) {
            api->log("[mmudpy] PyGILState_Ensure unavailable — skipping Py_Finalize (process exit will clean up)\n");
        }
    }

    /* Close console */
    if (con_hwnd) {
        DestroyWindow(con_hwnd);
        con_hwnd = NULL;
    }

    /* Free history */
    for (int i = 0; i < history_count; i++) {
        free(cmd_history[i]);
    }

    /* Free line buffer */
    EnterCriticalSection(&line_lock);
    for (int i = 0; i < MAX_LINE_BUF; i++) {
        if (line_buffer[i]) { free(line_buffer[i]); line_buffer[i] = NULL; }
    }
    LeaveCriticalSection(&line_lock);

    DeleteCriticalSection(&py_lock);
    DeleteCriticalSection(&line_lock);

    if (hPython) {
        FreeLibrary(hPython);
        hPython = NULL;
    }
}

/* ---- Script Manager (rack-based UI) ---- */

#define SCRIPT_MENU_MAX   32
#define SM_RACK_H         40      /* Height of each script rack row */
#define SM_BTN_W          70      /* Button width */
#define SM_BTN_H          26      /* Button height */
#define SM_LOAD_BTN_ID    43000
#define SM_TOGGLE_BASE    43100   /* Toggle buttons: 43100..43131 */
#define SM_REMOVE_BASE    43200   /* Remove buttons: 43200..43231 */

static HWND sm_hwnd = NULL;       /* Script Manager window */
static HWND sm_scroll = NULL;     /* Scrollable panel (child) */
static int sm_rack_count = 0;

typedef struct {
    char name[64];
    int  active;       /* 1 = running, 0 = stopped */
    HWND lbl;          /* Script name label */
    HWND status_lbl;   /* Status indicator: "Running" / "Stopped" */
    HWND btn_toggle;   /* Load/Unload button */
    HWND btn_remove;   /* Remove (-) button */
} sm_rack_t;

static sm_rack_t sm_racks[SCRIPT_MENU_MAX];

static char sm_script_names[SCRIPT_MENU_MAX][64];
static int sm_script_count = 0;

static void sm_scan_scripts(void)
{
    char search[MAX_PATH];
    GetModuleFileNameA(NULL, search, MAX_PATH);
    char *sl = strrchr(search, '\\');
    if (sl) *sl = '\0';
    strcat(search, "\\plugins\\MMUDPy\\scripts\\*.py");

    sm_script_count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (sm_script_count >= SCRIPT_MENU_MAX) break;
        strncpy(sm_script_names[sm_script_count], fd.cFileName, 63);
        sm_script_names[sm_script_count][63] = '\0';
        char *dot = strstr(sm_script_names[sm_script_count], ".py");
        if (dot) *dot = '\0';
        sm_script_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static HFONT sm_font = NULL;

static void sm_rebuild_racks(void)
{
    if (!sm_scroll) return;

    /* Destroy old rack controls */
    for (int i = 0; i < sm_rack_count; i++) {
        if (sm_racks[i].btn_toggle) DestroyWindow(sm_racks[i].btn_toggle);
        if (sm_racks[i].btn_remove) DestroyWindow(sm_racks[i].btn_remove);
    }
    sm_rack_count = 0;

    sm_scan_scripts();
    HINSTANCE hInst = GetModuleHandleA(NULL);
    RECT cr;
    GetClientRect(sm_hwnd, &cr);
    int panel_w = cr.right - cr.left;

    if (panel_w < 100) panel_w = 360;  /* fallback before first WM_SIZE */

    for (int i = 0; i < sm_script_count; i++) {
        int y = i * SM_RACK_H + 4;
        sm_rack_t *r = &sm_racks[i];
        memset(r, 0, sizeof(*r));
        strncpy(r->name, sm_script_names[i], 63);

        /* Query Python for actual loaded state */
        r->active = mmudpy_is_script_loaded(r->name);

        /* Single toggle button with script name on it */
        char label[128];
        sprintf(label, "%s: %s", r->active ? "Unload" : "Load", r->name);
        r->btn_toggle = CreateWindowExA(0, "BUTTON", label,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            8, y + 6, panel_w - 50, SM_BTN_H,
            sm_hwnd, (HMENU)(LONG_PTR)(SM_TOGGLE_BASE + i), hInst, NULL);
        if (sm_font) SendMessageA(r->btn_toggle, WM_SETFONT, (WPARAM)sm_font, TRUE);

        /* Remove button */
        r->btn_remove = CreateWindowExA(0, "BUTTON", " X ",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            panel_w - 38, y + 6, 26, SM_BTN_H,
            sm_hwnd, (HMENU)(LONG_PTR)(SM_REMOVE_BASE + i), hInst, NULL);
        if (sm_font) SendMessageA(r->btn_remove, WM_SETFONT, (WPARAM)sm_font, TRUE);

        sm_rack_count++;
    }

    /* Update scroll range */
    int total_h = sm_script_count * SM_RACK_H + 8;
    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE;
    si.nMin = 0;
    si.nMax = total_h;
    si.nPage = cr.bottom - cr.top;
    SetScrollInfo(sm_scroll, SB_VERT, &si, TRUE);
}

static LRESULT CALLBACK sm_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        MoveWindow(sm_scroll, 0, 0, w, h, TRUE);
        sm_rebuild_racks();
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = {0};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(sm_scroll, SB_VERT, &si);
        int old_pos = si.nPos;
        switch (LOWORD(wParam)) {
            case SB_LINEUP:   si.nPos -= 20; break;
            case SB_LINEDOWN: si.nPos += 20; break;
            case SB_PAGEUP:   si.nPos -= si.nPage; break;
            case SB_PAGEDOWN: si.nPos += si.nPage; break;
            case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(sm_scroll, SB_VERT, &si, TRUE);
        GetScrollInfo(sm_scroll, SB_VERT, &si);
        if (si.nPos != old_pos) {
            ScrollWindow(sm_scroll, 0, old_pos - si.nPos, NULL, NULL);
            UpdateWindow(sm_scroll);
        }
        return 0;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wParam);

        /* Toggle (Load/Unload) a script */
        if (id >= SM_TOGGLE_BASE && id < SM_TOGGLE_BASE + SCRIPT_MENU_MAX) {
            int idx = id - SM_TOGGLE_BASE;
            if (idx < sm_rack_count && py_ready) {
                char cmd[256];
                if (sm_racks[idx].active) {
                    sprintf(cmd, "mud.stop('%s')", sm_racks[idx].name);
                } else {
                    sprintf(cmd, "mud.load('%s')", sm_racks[idx].name);
                }
                py_exec_safe(cmd);

                /* Query real state after the action */
                int now_loaded = mmudpy_is_script_loaded(sm_racks[idx].name);
                sm_racks[idx].active = now_loaded;
                char new_label[128];
                sprintf(new_label, "%s: %s",
                    now_loaded ? "Unload" : "Load", sm_racks[idx].name);
                SetWindowTextA(sm_racks[idx].btn_toggle, new_label);
            }
            return 0;
        }

        /* Remove a script */
        if (id >= SM_REMOVE_BASE && id < SM_REMOVE_BASE + SCRIPT_MENU_MAX) {
            int idx = id - SM_REMOVE_BASE;
            if (idx < sm_rack_count) {
                char msg_buf[256];
                sprintf(msg_buf, "Remove MMUDPy Script '%s'?\n\n"
                        "This will stop the script (if running) and delete the file.",
                        sm_racks[idx].name);
                int ret = MessageBoxA(hwnd, msg_buf, "Remove Script",
                                      MB_YESNO | MB_ICONQUESTION);
                if (ret == IDYES) {
                    /* Stop if running */
                    if (py_ready) {
                        char cmd[256];
                        sprintf(cmd,
                            "if '%s' in _loaded_scripts:\n"
                            "    mud.stop('%s')\n",
                            sm_racks[idx].name, sm_racks[idx].name);
                        py_exec_safe(cmd);
                    }
                    /* Delete file */
                    if (py_ready) {
                        char cmd[256];
                        sprintf(cmd, "mud.delete('%s')", sm_racks[idx].name);
                        py_exec_safe(cmd);
                    }
                    /* Rebuild racks */
                    sm_rebuild_racks();
                }
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        sm_hwnd = NULL;
        sm_scroll = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void show_script_manager(HWND parent)
{
    if (sm_hwnd) {
        ShowWindow(sm_hwnd, SW_SHOW);
        SetForegroundWindow(sm_hwnd);
        sm_rebuild_racks();
        return;
    }

    static int sm_class_reg = 0;
    HINSTANCE hInst = GetModuleHandleA(NULL);

    if (!sm_class_reg) {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = sm_wndproc;
        wc.hInstance = hInst;
        wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 32));
        wc.lpszClassName = "MMUDPyScriptMgr";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassA(&wc);
        sm_class_reg = 1;
    }

    if (!sm_font) {
        sm_font = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH, "Segoe UI");
    }

    sm_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW, "MMUDPyScriptMgr", "MMUDPy Script Manager",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 400,
        parent, NULL, hInst, NULL);

    /* Scrollable panel */
    sm_scroll = CreateWindowExA(
        0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | SS_LEFT,
        0, 0, 380, 340,
        sm_hwnd, NULL, hInst, NULL);

    sm_rebuild_racks();

    ShowWindow(sm_hwnd, SW_SHOW);
    UpdateWindow(sm_hwnd);
}

/* ---- Menu click handler ---- */

static int mmudpy_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    if (msg == WM_COMMAND) {
        WORD id = LOWORD(wParam);

        /* Show/Hide Console - IDM_PLUGIN_BASE + plugin_idx*10 + 0
         * We respond to any ID in our plugin range for show/hide */
        if (id == menu_base) {
            if (con_hwnd) {
                if (IsWindowVisible(con_hwnd)) {
                    ShowWindow(con_hwnd, SW_HIDE);
                } else {
                    ShowWindow(con_hwnd, SW_SHOW);
                    SetForegroundWindow(con_hwnd);
                    SetFocus(con_input);
                }
            }
            return 1;
        }

        /* Script Manager - IDM_PLUGIN_BASE + plugin_idx*10 + 1 */
        if (id == menu_base + 1) {
            show_script_manager(hwnd);
            return 1;
        }
    }
    return 0;
}

/* ---- Plugin descriptor ---- */

static slop_plugin_t plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "MMUDPy",
    .author      = "Tripmunk",
    .description = "Embedded Python console with direct game API access",
    .version     = "1.0.0",
    .init        = mmudpy_init,
    .shutdown    = mmudpy_shutdown,
    .on_line     = mmudpy_on_line,
    .on_clean_line = mmudpy_on_clean_line,
    .on_round    = mmudpy_on_round,
    .on_wndproc  = mmudpy_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
