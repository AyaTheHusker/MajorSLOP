/*
 * gl_overlay.c — OpenGL Overlay Plugin for MajorSLOP
 * ===================================================
 *
 * Creates a transparent OpenGL child window inside MegaMUD's MMANSI area.
 * Other plugins can register draw callbacks via the exported API.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o gl_overlay.dll gl_overlay.c \
 *       -lgdi32 -luser32 -lopengl32 -mwindows
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <string.h>
#include <GL/gl.h>

/* ---- State ---- */

static const slop_api_t *api = NULL;
static HWND gl_hwnd = NULL;       /* GL child window */
static HDC  gl_hdc  = NULL;
static HGLRC gl_ctx = NULL;
static int gl_width = 800;
static int gl_height = 600;
static volatile int gl_running = 0;
static volatile int gl_visible = 0;

#define IDM_GL_TOGGLE  50600

/* ---- Draw callback registry ---- */

#define MAX_DRAW_CBS 16

typedef void (*gl_draw_fn)(int width, int height);

static gl_draw_fn draw_cbs[MAX_DRAW_CBS];
static int draw_cb_count = 0;

/* Export: register a draw callback */
__declspec(dllexport) int gl_register_draw(gl_draw_fn fn)
{
    if (!fn || draw_cb_count >= MAX_DRAW_CBS) return -1;
    draw_cbs[draw_cb_count++] = fn;
    return draw_cb_count - 1;
}

/* Export: unregister a draw callback */
__declspec(dllexport) void gl_unregister_draw(int id)
{
    if (id >= 0 && id < draw_cb_count)
        draw_cbs[id] = NULL;
}

/* Export: get GL window dimensions */
__declspec(dllexport) void gl_get_size(int *w, int *h)
{
    if (w) *w = gl_width;
    if (h) *h = gl_height;
}

/* ---- GL Window ---- */

static LRESULT CALLBACK gl_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE:
        gl_width = LOWORD(lParam);
        gl_height = HIWORD(lParam);
        if (gl_ctx) {
            glViewport(0, 0, gl_width, gl_height);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;  /* prevent flicker */
    case WM_DESTROY:
        gl_running = 0;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static int init_gl(void)
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    gl_hdc = GetDC(gl_hwnd);
    if (!gl_hdc) {
        api->log("[gl_overlay] GetDC failed\n");
        return -1;
    }

    int pf = ChoosePixelFormat(gl_hdc, &pfd);
    if (!pf) {
        api->log("[gl_overlay] ChoosePixelFormat failed\n");
        return -1;
    }

    if (!SetPixelFormat(gl_hdc, pf, &pfd)) {
        api->log("[gl_overlay] SetPixelFormat failed\n");
        return -1;
    }

    gl_ctx = wglCreateContext(gl_hdc);
    if (!gl_ctx) {
        api->log("[gl_overlay] wglCreateContext failed\n");
        return -1;
    }

    if (!wglMakeCurrent(gl_hdc, gl_ctx)) {
        api->log("[gl_overlay] wglMakeCurrent failed\n");
        return -1;
    }

    /* Basic GL setup */
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    api->log("[gl_overlay] GL vendor: %s\n", vendor ? vendor : "?");
    api->log("[gl_overlay] GL renderer: %s\n", renderer ? renderer : "?");
    api->log("[gl_overlay] GL version: %s\n", version ? version : "?");

    return 0;
}

/* Demo: draw a spinning triangle */
static float demo_angle = 0.0f;

static void demo_draw(int w, int h)
{
    (void)w; (void)h;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glRotatef(demo_angle, 0.0f, 0.0f, 1.0f);

    glBegin(GL_TRIANGLES);
    glColor4f(1.0f, 0.2f, 0.2f, 0.8f);
    glVertex2f(0.0f, 0.6f);
    glColor4f(0.2f, 1.0f, 0.2f, 0.8f);
    glVertex2f(-0.5f, -0.3f);
    glColor4f(0.2f, 0.2f, 1.0f, 0.8f);
    glVertex2f(0.5f, -0.3f);
    glEnd();

    demo_angle += 1.0f;
    if (demo_angle >= 360.0f) demo_angle -= 360.0f;
}

/* ---- Render thread ---- */

static DWORD WINAPI gl_render_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    /* Register window class */
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = gl_wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SlopGLOverlay";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    /* Create as a standalone tool window, owned by MMMAIN */
    HWND mw = api->get_mmmain_hwnd();
    int w = 640, h = 480;

    gl_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        "SlopGLOverlay", "GL Overlay",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        mw, NULL, hInst, NULL);

    if (!gl_hwnd) {
        api->log("[gl_overlay] CreateWindow failed (err=%lu)\n", GetLastError());
        return 1;
    }

    gl_width = w;
    gl_height = h;

    if (init_gl() != 0) {
        api->log("[gl_overlay] GL init failed\n");
        DestroyWindow(gl_hwnd);
        gl_hwnd = NULL;
        return 1;
    }

    /* Register demo draw callback */
    gl_register_draw(demo_draw);

    api->log("[gl_overlay] GL overlay running (%dx%d)\n", w, h);
    gl_running = 1;

    /* Render loop ~60fps */
    while (gl_running) {
        /* Handle messages */
        MSG msg;
        while (PeekMessageA(&msg, gl_hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        /* Only render when visible */
        if (gl_visible) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            for (int i = 0; i < draw_cb_count; i++) {
                if (draw_cbs[i])
                    draw_cbs[i](gl_width, gl_height);
            }

            SwapBuffers(gl_hdc);
            Sleep(16);  /* ~60fps */
        } else {
            Sleep(100);  /* idle when hidden */
        }
    }

    /* Cleanup */
    wglMakeCurrent(NULL, NULL);
    if (gl_ctx) { wglDeleteContext(gl_ctx); gl_ctx = NULL; }
    if (gl_hdc) { ReleaseDC(gl_hwnd, gl_hdc); gl_hdc = NULL; }

    return 0;
}

/* ---- Plugin interface ---- */

static int gl_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_GL_TOGGLE) {
        if (gl_hwnd) {
            gl_visible = !gl_visible;
            ShowWindow(gl_hwnd, gl_visible ? SW_SHOW : SW_HIDE);
            if (api) api->log("[gl_overlay] %s\n", gl_visible ? "shown" : "hidden");
        }
        return 1;
    }
    return 0;
}

static int gl_init(const slop_api_t *a)
{
    api = a;
    api->log("[gl_overlay] DISABLED — not launching (WIP)\n");
    return 0;
}

static void gl_shutdown(void)
{
    gl_running = 0;
    Sleep(100);  /* let render thread exit */
    if (gl_hwnd) {
        DestroyWindow(gl_hwnd);
        gl_hwnd = NULL;
    }
    if (api) api->log("[gl_overlay] Shutdown\n");
}

static slop_plugin_t gl_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "GL Overlay",
    .author      = "Tripmunk",
    .description = "OpenGL overlay inside MegaMUD window",
    .version     = "0.1.0",
    .init        = gl_init,
    .shutdown    = gl_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = gl_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &gl_plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
