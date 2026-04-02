/*
 * gl_terminal.c — GL Terminal Plugin for MajorSLOP
 * ==================================================
 *
 * Replaces MMANSI's rendering with a GPU-accelerated OpenGL terminal.
 * Reads the same terminal buffer and renders with smooth text + themes.
 *
 * Features: OEM charset (CP437 box-drawing), theme palettes, input box
 * with command history (up/down arrows), clean shutdown.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o gl_terminal.dll gl_terminal.c \
 *       -lgdi32 -luser32 -lopengl32 -mwindows
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <GL/gl.h>

/* ---- Constants ---- */

#define TERM_ROWS        60
#define TERM_COLS        132
#define TERM_STRIDE      0x84
#define MMANSI_TEXT_OFF  0x62
#define MMANSI_ATTR_OFF  0x1F52

/* Font atlas: 16x16 grid of chars, each cell sized dynamically */
#define FONT_GRID        16
#define FONT_CELL_W      24    /* px per char in atlas (high-res for quality) */
#define FONT_CELL_H      36
#define FONT_TEX_W       (FONT_GRID * FONT_CELL_W)
#define FONT_TEX_H       (FONT_GRID * FONT_CELL_H)

#define IDM_TERM_TOGGLE    50900
#define IDM_TERM_THEME_BASE 50910
#define IDM_TERM_HIDE       50935

/* Input history */
#define MAX_HISTORY 64
#define INPUT_HEIGHT 24  /* px for input box area */

/* ---- Color palettes ---- */

typedef struct { float r, g, b; } rgb_t;

static rgb_t pal_classic[16] = {
    {0.00f,0.00f,0.00f}, {0.00f,0.00f,0.67f}, {0.00f,0.67f,0.00f}, {0.00f,0.67f,0.67f},
    {0.67f,0.00f,0.00f}, {0.67f,0.00f,0.67f}, {0.67f,0.33f,0.00f}, {0.67f,0.67f,0.67f},
    {0.33f,0.33f,0.33f}, {0.33f,0.33f,1.00f}, {0.33f,1.00f,0.33f}, {0.33f,1.00f,1.00f},
    {1.00f,0.33f,0.33f}, {1.00f,0.33f,1.00f}, {1.00f,1.00f,0.33f}, {1.00f,1.00f,1.00f},
};
static rgb_t pal_dracula[16] = {
    {0.16f,0.16f,0.21f}, {0.38f,0.45f,0.94f}, {0.31f,0.98f,0.48f}, {0.55f,0.91f,0.99f},
    {1.00f,0.33f,0.40f}, {0.74f,0.47f,0.95f}, {0.95f,0.71f,0.24f}, {0.73f,0.75f,0.82f},
    {0.38f,0.40f,0.50f}, {0.51f,0.59f,0.97f}, {0.44f,0.99f,0.58f}, {0.65f,0.94f,0.99f},
    {1.00f,0.47f,0.53f}, {0.84f,0.60f,0.98f}, {0.98f,0.80f,0.40f}, {0.97f,0.97f,0.95f},
};
static rgb_t pal_solarized[16] = {
    {0.00f,0.17f,0.21f}, {0.15f,0.55f,0.82f}, {0.52f,0.60f,0.00f}, {0.16f,0.63f,0.60f},
    {0.86f,0.20f,0.18f}, {0.83f,0.21f,0.51f}, {0.71f,0.54f,0.00f}, {0.58f,0.63f,0.63f},
    {0.40f,0.48f,0.51f}, {0.26f,0.65f,0.90f}, {0.63f,0.72f,0.14f}, {0.29f,0.74f,0.73f},
    {0.93f,0.33f,0.31f}, {0.91f,0.34f,0.60f}, {0.80f,0.67f,0.14f}, {0.93f,0.91f,0.84f},
};
static rgb_t pal_amber[16] = {
    {0.05f,0.03f,0.00f}, {0.40f,0.25f,0.00f}, {0.60f,0.40f,0.00f}, {0.50f,0.35f,0.00f},
    {0.70f,0.30f,0.00f}, {0.55f,0.25f,0.00f}, {0.80f,0.55f,0.00f}, {0.85f,0.60f,0.00f},
    {0.45f,0.30f,0.00f}, {0.50f,0.35f,0.00f}, {0.75f,0.55f,0.00f}, {0.65f,0.48f,0.00f},
    {0.90f,0.45f,0.00f}, {0.70f,0.40f,0.00f}, {1.00f,0.75f,0.00f}, {1.00f,0.80f,0.10f},
};
static rgb_t pal_green[16] = {
    {0.00f,0.04f,0.00f}, {0.00f,0.25f,0.00f}, {0.00f,0.50f,0.00f}, {0.00f,0.40f,0.10f},
    {0.10f,0.30f,0.00f}, {0.05f,0.25f,0.05f}, {0.00f,0.55f,0.00f}, {0.00f,0.65f,0.00f},
    {0.00f,0.30f,0.00f}, {0.00f,0.35f,0.05f}, {0.00f,0.75f,0.00f}, {0.00f,0.60f,0.10f},
    {0.10f,0.45f,0.00f}, {0.05f,0.40f,0.05f}, {0.10f,0.85f,0.00f}, {0.15f,1.00f,0.10f},
};

#define NUM_THEMES 5
static const char *theme_names[NUM_THEMES] = {
    "Classic VGA", "Dracula", "Solarized Dark", "Amber CRT", "Green Phosphor"
};
static rgb_t *theme_palettes[NUM_THEMES] = {
    pal_classic, pal_dracula, pal_solarized, pal_amber, pal_green
};

/* ---- State ---- */

static const slop_api_t *api = NULL;
static HWND term_hwnd = NULL;     /* main container (holds GL + input) */
static HWND gl_panel = NULL;      /* GL rendering child window */
static HWND input_box = NULL;     /* EDIT control for command input */
static HDC  term_hdc  = NULL;
static HGLRC term_ctx = NULL;
static volatile int term_running = 0;
static volatile int term_visible = 0;
static HWND mmansi_hwnd = NULL;
static HANDLE term_thread_handle = NULL;

static GLuint font_tex = 0;
static int current_theme = 0;
static rgb_t *palette = NULL;

static int term_px_w = 0, term_px_h = 0;  /* GL panel size */
static float cell_w = 0, cell_h = 0;

/* Terminal buffer cache */
static unsigned char term_text[TERM_ROWS][TERM_COLS];
static unsigned char term_attr[TERM_ROWS][TERM_COLS];

/* Command history */
static char *cmd_history[MAX_HISTORY];
static int history_count = 0;
static int history_idx = 0;

/* Input subclass */
static WNDPROC orig_input_proc = NULL;
static HBRUSH input_bg_brush = NULL;

/* Forward decls */
void gl_term_show(void);
void gl_term_hide(void);
void gl_term_toggle(void);

/* ---- Font texture generation ---- */

static void gen_font_texture(void)
{
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = FONT_TEX_W;
    bmi.bmiHeader.biHeight = -FONT_TEX_H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    unsigned char *pixels = NULL;
    HDC memDC = CreateCompatibleDC(NULL);
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, dib);
    memset(pixels, 0, FONT_TEX_W * FONT_TEX_H * 4);

    /* Use OEM_CHARSET for CP437 box-drawing characters.
     * "Terminal" is the classic DOS font; "Lucida Console" is fallback. */
    HFONT font = CreateFontA(
        FONT_CELL_H - 2, 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE,
        OEM_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN, "Terminal");
    if (!font) {
        font = CreateFontA(FONT_CELL_H - 2, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                           OEM_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, "Lucida Console");
    }
    HFONT oldFont = (HFONT)SelectObject(memDC, font);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));

    /* Render ALL 256 characters (including extended/box-drawing) */
    for (int c = 1; c < 256; c++) {
        int gx = (c % FONT_GRID) * FONT_CELL_W;
        int gy = (c / FONT_GRID) * FONT_CELL_H;
        char ch = (char)c;
        RECT rc = { gx, gy + 1, gx + FONT_CELL_W, gy + FONT_CELL_H };
        DrawTextA(memDC, &ch, 1, &rc, DT_LEFT | DT_TOP | DT_NOCLIP | DT_NOPREFIX);
    }

    SelectObject(memDC, oldFont);
    DeleteObject(font);

    /* Convert BGRA → RGBA, alpha from luminance */
    for (int i = 0; i < FONT_TEX_W * FONT_TEX_H; i++) {
        unsigned char b = pixels[i * 4 + 0];
        unsigned char g = pixels[i * 4 + 1];
        unsigned char r = pixels[i * 4 + 2];
        unsigned char a = r > g ? (r > b ? r : b) : (g > b ? g : b);
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 255;
        pixels[i * 4 + 2] = 255;
        pixels[i * 4 + 3] = a;
    }

    glGenTextures(1, &font_tex);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FONT_TEX_W, FONT_TEX_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    if (api) api->log("[gl_terminal] Font texture OK (%dx%d, OEM)\n", FONT_TEX_W, FONT_TEX_H);
}

/* ---- Terminal buffer reading ---- */

static void term_read_buffer(void)
{
    if (!mmansi_hwnd) return;
    LONG wdata = GetWindowLongA(mmansi_hwnd, 4);
    if (!wdata) return;

    for (int r = 0; r < TERM_ROWS; r++) {
        memcpy(term_text[r], (const void *)(wdata + MMANSI_TEXT_OFF + r * TERM_STRIDE), TERM_COLS);
        memcpy(term_attr[r], (const void *)(wdata + MMANSI_ATTR_OFF + r * TERM_STRIDE), TERM_COLS);
    }
}

/* ---- Rendering ---- */

static void term_render(void)
{
    if (!font_tex || !palette) return;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, term_px_w, term_px_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glClearColor(palette[0].r, palette[0].g, palette[0].b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    float cw = cell_w;
    float ch = cell_h;
    float tex_cw = 1.0f / (float)FONT_GRID;
    float tex_ch = 1.0f / (float)FONT_GRID;

    /* Pass 1: background colors (skip black/default) */
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    for (int r = 0; r < TERM_ROWS; r++) {
        float y = r * ch;
        for (int c = 0; c < TERM_COLS; c++) {
            int bg_idx = (term_attr[r][c] >> 4) & 0x07;
            if (bg_idx == 0) continue;
            rgb_t *bg = &palette[bg_idx];
            float x = c * cw;
            glColor3f(bg->r, bg->g, bg->b);
            glVertex2f(x,      y);
            glVertex2f(x + cw, y);
            glVertex2f(x + cw, y + ch);
            glVertex2f(x,      y + ch);
        }
    }
    glEnd();

    /* Pass 2: text characters */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, font_tex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBegin(GL_QUADS);
    for (int r = 0; r < TERM_ROWS; r++) {
        float y = r * ch;
        for (int c = 0; c < TERM_COLS; c++) {
            unsigned char byte = term_text[r][c];
            if (byte <= 32) continue;

            int fg_idx = term_attr[r][c] & 0x0F;
            rgb_t *fg = &palette[fg_idx];

            float u0 = (float)(byte % FONT_GRID) * tex_cw;
            float v0 = (float)(byte / FONT_GRID) * tex_ch;
            float u1 = u0 + tex_cw;
            float v1 = v0 + tex_ch;

            float x = c * cw;

            glColor4f(fg->r, fg->g, fg->b, 1.0f);
            glTexCoord2f(u0, v0); glVertex2f(x,      y);
            glTexCoord2f(u1, v0); glVertex2f(x + cw, y);
            glTexCoord2f(u1, v1); glVertex2f(x + cw, y + ch);
            glTexCoord2f(u0, v1); glVertex2f(x,      y + ch);
        }
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

/* ---- Input box with history ---- */

static void input_send_command(void)
{
    char buf[256];
    GetWindowTextA(input_box, buf, 255);
    if (!buf[0]) return;

    /* Add to history */
    if (history_count < MAX_HISTORY) {
        cmd_history[history_count++] = _strdup(buf);
    } else {
        free(cmd_history[0]);
        memmove(cmd_history, cmd_history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        cmd_history[MAX_HISTORY - 1] = _strdup(buf);
    }
    history_idx = history_count;

    /* Send to MMANSI as keystrokes */
    if (mmansi_hwnd) {
        for (int i = 0; buf[i]; i++)
            PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)(unsigned char)buf[i], 0);
        PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)'\r', 0);
    }

    SetWindowTextA(input_box, "");
}

static LRESULT CALLBACK input_subclass(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            input_send_command();
            return 0;
        }
        if (wParam == VK_UP) {
            if (history_idx > 0) {
                history_idx--;
                SetWindowTextA(input_box, cmd_history[history_idx]);
                SendMessageA(input_box, EM_SETSEL, -1, -1);
            }
            return 0;
        }
        if (wParam == VK_DOWN) {
            if (history_idx < history_count - 1) {
                history_idx++;
                SetWindowTextA(input_box, cmd_history[history_idx]);
                SendMessageA(input_box, EM_SETSEL, -1, -1);
            } else {
                history_idx = history_count;
                SetWindowTextA(input_box, "");
            }
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SetWindowTextA(input_box, "");
            history_idx = history_count;
            return 0;
        }
    }
    /* Forward other keys to MMANSI too (for MegaMUD hotkeys etc) */
    if (msg == WM_CHAR && wParam == '\r') return 0; /* already handled */

    return CallWindowProcA(orig_input_proc, hwnd, msg, wParam, lParam);
}

/* ---- Context menu ---- */

static void term_show_context_menu(HWND hwnd, int x, int y)
{
    HMENU menu = CreatePopupMenu();
    HMENU theme_sub = CreatePopupMenu();
    for (int i = 0; i < NUM_THEMES; i++)
        AppendMenuA(theme_sub, MF_STRING | (i == current_theme ? MF_CHECKED : 0),
                    IDM_TERM_THEME_BASE + i, theme_names[i]);
    AppendMenuA(menu, MF_POPUP, (UINT_PTR)theme_sub, "Theme");
    AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(menu, MF_STRING, IDM_TERM_HIDE, "Disable GL Terminal");
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

/* ---- Window procedures ---- */

static LRESULT CALLBACK gl_panel_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        term_px_w = LOWORD(lParam);
        term_px_h = HIWORD(lParam);
        if (term_px_w > 0 && term_px_h > 0) {
            cell_w = (float)term_px_w / (float)TERM_COLS;
            cell_h = (float)term_px_h / (float)TERM_ROWS;
            if (term_ctx) glViewport(0, 0, term_px_w, term_px_h);
        }
        return 0;

    /* Forward keyboard to MMANSI */
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (mmansi_hwnd)
            PostMessageA(mmansi_hwnd, msg, wParam, lParam);
        return 0;

    case WM_RBUTTONUP: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ClientToScreen(hwnd, &pt);
        term_show_context_menu(hwnd, pt.x, pt.y);
        return 0;
    }

    /* Click focuses input box */
    case WM_LBUTTONDOWN:
        if (input_box) SetFocus(input_box);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= IDM_TERM_THEME_BASE && id < IDM_TERM_THEME_BASE + NUM_THEMES) {
            current_theme = id - IDM_TERM_THEME_BASE;
            palette = theme_palettes[current_theme];
            return 0;
        }
        if (id == IDM_TERM_HIDE) { gl_term_hide(); return 0; }
        return 0;
    }

    case WM_DESTROY:
        term_running = 0;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK term_container_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_SIZE: {
        int w = LOWORD(lParam), h = HIWORD(lParam);
        if (gl_panel)
            MoveWindow(gl_panel, 0, 0, w, h - INPUT_HEIGHT, TRUE);
        if (input_box)
            MoveWindow(input_box, 0, h - INPUT_HEIGHT, w, INPUT_HEIGHT, TRUE);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wParam, RGB(255, 255, 255));
        SetBkColor((HDC)wParam, RGB(0, 0, 0));
        if (!input_bg_brush) input_bg_brush = CreateSolidBrush(RGB(0, 0, 0));
        return (LRESULT)input_bg_brush;
    case WM_COMMAND:
        /* Forward menu commands to GL panel */
        if (gl_panel) SendMessageA(gl_panel, msg, wParam, lParam);
        return 0;
    case WM_SETFOCUS:
        if (input_box) SetFocus(input_box);
        return 0;
    case WM_DESTROY:
        term_running = 0;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---- GL init ---- */

static int term_init_gl(void)
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;

    term_hdc = GetDC(gl_panel);
    if (!term_hdc) return -1;
    int pf = ChoosePixelFormat(term_hdc, &pfd);
    if (!pf || !SetPixelFormat(term_hdc, pf, &pfd)) return -1;
    term_ctx = wglCreateContext(term_hdc);
    if (!term_ctx || !wglMakeCurrent(term_hdc, term_ctx)) return -1;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    gen_font_texture();
    return 0;
}

/* ---- Render thread ---- */

static DWORD WINAPI term_thread_fn(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    /* Wait for MMANSI */
    HWND mw = NULL;
    for (int i = 0; i < 100 && !mmansi_hwnd; i++) {
        mw = FindWindowA("MMMAIN", NULL);
        if (mw) mmansi_hwnd = FindWindowExA(mw, NULL, "MMANSI", NULL);
        if (!mmansi_hwnd) Sleep(100);
    }
    if (!mmansi_hwnd) {
        api->log("[gl_terminal] MMANSI not found\n");
        return 1;
    }

    RECT ar;
    GetWindowRect(mmansi_hwnd, &ar);
    int wx = ar.left, wy = ar.top;
    int ww = ar.right - ar.left, wh = ar.bottom - ar.top;
    api->log("[gl_terminal] MMANSI at %d,%d %dx%d\n", wx, wy, ww, wh);

    /* Register classes */
    WNDCLASSA wc1 = {0};
    wc1.lpfnWndProc = term_container_proc;
    wc1.hInstance = hInst;
    wc1.lpszClassName = "SlopGLTermCont";
    wc1.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc1.hCursor = LoadCursor(NULL, IDC_IBEAM);
    RegisterClassA(&wc1);

    WNDCLASSA wc2 = {0};
    wc2.style = CS_OWNDC;
    wc2.lpfnWndProc = gl_panel_proc;
    wc2.hInstance = hInst;
    wc2.lpszClassName = "SlopGLTermPanel";
    wc2.hCursor = LoadCursor(NULL, IDC_IBEAM);
    RegisterClassA(&wc2);

    /* Container: tool window popup over MMANSI area */
    term_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        "SlopGLTermCont", NULL, WS_POPUP | WS_CLIPCHILDREN,
        wx, wy, ww, wh,
        mw, NULL, hInst, NULL);
    if (!term_hwnd) {
        api->log("[gl_terminal] CreateWindow failed\n");
        return 1;
    }

    /* GL panel: fills container minus input height */
    gl_panel = CreateWindowExA(
        0, "SlopGLTermPanel", NULL, WS_CHILD | WS_VISIBLE,
        0, 0, ww, wh - INPUT_HEIGHT,
        term_hwnd, NULL, hInst, NULL);

    /* Input box: EDIT control at bottom (black bg, white text via WM_CTLCOLOREDIT) */
    input_box = CreateWindowExA(
        0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, wh - INPUT_HEIGHT, ww, INPUT_HEIGHT,
        term_hwnd, (HMENU)200, hInst, NULL);

    /* Style the input box */
    HFONT ifont = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              ANSI_CHARSET, 0, 0, DEFAULT_QUALITY,
                              FIXED_PITCH | FF_MODERN, "Consolas");
    SendMessageA(input_box, WM_SETFONT, (WPARAM)ifont, TRUE);
    SendMessageA(input_box, EM_LIMITTEXT, 250, 0);

    /* Subclass input for history and enter handling */
    orig_input_proc = (WNDPROC)SetWindowLongA(input_box, GWL_WNDPROC, (LONG)input_subclass);

    term_px_w = ww;
    term_px_h = wh - INPUT_HEIGHT;
    cell_w = (float)ww / (float)TERM_COLS;
    cell_h = (float)(wh - INPUT_HEIGHT) / (float)TERM_ROWS;

    if (term_init_gl() != 0) {
        api->log("[gl_terminal] GL init failed\n");
        DestroyWindow(term_hwnd);
        term_hwnd = NULL;
        return 1;
    }

    glViewport(0, 0, ww, wh - INPUT_HEIGHT);
    palette = theme_palettes[current_theme];

    api->log("[gl_terminal] Running (cell %.1fx%.1f)\n", cell_w, cell_h);
    term_running = 1;
    term_visible = 1;
    ShowWindow(term_hwnd, SW_SHOWNOACTIVATE);
    SetFocus(input_box);

    while (term_running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { term_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!term_running) break;

        if (term_visible) {
            /* Track MMANSI position */
            RECT cur;
            if (mmansi_hwnd && GetWindowRect(mmansi_hwnd, &cur)) {
                int nw = cur.right - cur.left, nh = cur.bottom - cur.top;
                if (cur.left != wx || cur.top != wy || nw != ww || nh != wh) {
                    wx = cur.left; wy = cur.top; ww = nw; wh = nh;
                    SetWindowPos(term_hwnd, NULL, wx, wy, ww, wh,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }

            term_read_buffer();
            wglMakeCurrent(term_hdc, term_ctx);
            term_render();
            SwapBuffers(term_hdc);
            Sleep(16);
        } else {
            Sleep(100);
        }
    }

    wglMakeCurrent(NULL, NULL);
    if (term_ctx) { wglDeleteContext(term_ctx); term_ctx = NULL; }
    if (term_hdc) { ReleaseDC(gl_panel, term_hdc); term_hdc = NULL; }
    if (font_tex) { glDeleteTextures(1, &font_tex); font_tex = 0; }
    return 0;
}

/* ---- Exported commands ---- */

__declspec(dllexport) void gl_term_show(void)
{
    if (term_hwnd) { term_visible = 1; ShowWindow(term_hwnd, SW_SHOWNOACTIVATE); }
}
__declspec(dllexport) void gl_term_hide(void)
{
    if (term_hwnd) { term_visible = 0; ShowWindow(term_hwnd, SW_HIDE); }
}
__declspec(dllexport) void gl_term_toggle(void)
{
    if (term_visible) gl_term_hide(); else gl_term_show();
}
__declspec(dllexport) void gl_term_set_theme(int idx)
{
    if (idx >= 0 && idx < NUM_THEMES) {
        current_theme = idx;
        palette = theme_palettes[idx];
    }
}
__declspec(dllexport) int gl_term_get_theme(void) { return current_theme; }
__declspec(dllexport) void gl_term_set_color(int idx, int r, int g, int b)
{
    if (idx >= 0 && idx < 16 && palette) {
        palette[idx].r = (float)r / 255.0f;
        palette[idx].g = (float)g / 255.0f;
        palette[idx].b = (float)b / 255.0f;
    }
}

static slop_command_t term_commands[] = {
    { "gl_term_show",      "gl_term_show",      "",    "v", "Show GL terminal" },
    { "gl_term_hide",      "gl_term_hide",      "",    "v", "Hide GL terminal" },
    { "gl_term_toggle",    "gl_term_toggle",     "",    "v", "Toggle GL terminal" },
    { "gl_term_set_theme", "gl_term_set_theme",  "i",   "v", "Set theme (0-4)" },
    { "gl_term_get_theme", "gl_term_get_theme",  "",    "i", "Get theme index" },
    { "gl_term_set_color", "gl_term_set_color",  "iiii","v", "Set palette color" },
};

__declspec(dllexport) slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(term_commands) / sizeof(term_commands[0]);
    return term_commands;
}

/* ---- Plugin interface ---- */

static int term_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_TERM_TOGGLE) {
        gl_term_toggle();
        return 1;
    }
    return 0;
}

static int term_init(const slop_api_t *a)
{
    api = a;
    api->log("[gl_terminal] DISABLED — not launching (WIP)\n");
    /* Disabled until rendering quality / z-order issues are resolved */
    return 0;
}

static void term_shutdown(void)
{
    term_running = 0;
    /* Post quit to break message loop */
    if (term_hwnd) PostMessageA(term_hwnd, WM_QUIT, 0, 0);
    if (gl_panel) PostMessageA(gl_panel, WM_QUIT, 0, 0);

    /* Wait for thread to exit with timeout */
    if (term_thread_handle) {
        WaitForSingleObject(term_thread_handle, 500);
        CloseHandle(term_thread_handle);
        term_thread_handle = NULL;
    }

    /* Free history */
    for (int i = 0; i < history_count; i++) free(cmd_history[i]);
    history_count = 0;

    /* Restore MMANSI */
    if (mmansi_hwnd) ShowWindow(mmansi_hwnd, SW_SHOW);
    if (input_bg_brush) { DeleteObject(input_bg_brush); input_bg_brush = NULL; }
    term_hwnd = NULL;
    gl_panel = NULL;
    input_box = NULL;
    if (api) api->log("[gl_terminal] Shutdown\n");
}

static slop_plugin_t term_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "GL Terminal",
    .author      = "Tripmunk",
    .description = "OpenGL terminal replacement — themes, smooth text, input box",
    .version     = "0.2.0",
    .init        = term_init,
    .shutdown    = term_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = term_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &term_plugin; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
