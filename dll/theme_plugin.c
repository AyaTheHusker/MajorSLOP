/*
 * theme_plugin.c — MajorSLOP Theme Engine Plugin
 *
 * Window theming engine for MegaMUD. Provides dark/colored themes
 * for all MegaMUD windows, dialogs, menus, and controls.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o theme_plugin.dll theme_plugin.c \
 *       -lgdi32 -luser32 -lcomctl32 -mwindows
 */

#include "slop_plugin.h"
#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- Constants ---- */

#define THEME_SETTINGS_CLASS "MajorSLOPThemes"
#define IDM_THEME_MENU       40902
#define IDM_THEME_COMBO      40920
#define IDM_THEME_APPLY      40921

/* Statusbar messages */
#ifndef SB_SETBKCOLOR
#define SB_SETBKCOLOR  0x2001
#endif

#ifndef TB_SETBKCOLOR
#define TB_SETBKCOLOR  (WM_USER + 25)
#endif

/* ---- Theme definitions ---- */

typedef struct {
    const char *key;
    const char *name;
    unsigned char accent_r, accent_g, accent_b;
    unsigned char text_r, text_g, text_b;
    unsigned char dim_r, dim_g, dim_b;
    unsigned char hp_full_r, hp_full_g, hp_full_b;
    unsigned char mana_r, mana_g, mana_b;
    unsigned char bg_r, bg_g, bg_b;
    unsigned char panel_r, panel_g, panel_b;
} theme_def_t;

static const theme_def_t THEMES[] = {
    { "default",      "Default",        0x00,0x00,0x80, 0x00,0x00,0x00, 0x80,0x80,0x80, 0x00,0x80,0x00, 0x00,0x00,0x80, 0xD4,0xD0,0xC8, 0xD4,0xD0,0xC8 },
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
#define THEME_COUNT (sizeof(THEMES) / sizeof(THEMES[0]))

/* ---- State ---- */

static const slop_api_t *g_api = NULL;
static int theme_idx = -1;
static int theme_active = 0;
static HBRUSH theme_bg_brush = NULL;
static HBRUSH theme_panel_brush = NULL;
static HBRUSH theme_menubar_brush = NULL;
static COLORREF theme_text_color, theme_bg_color, theme_panel_color;
static COLORREF theme_accent_color, theme_dim_color, theme_menubar_color;
static HWND theme_settings_hwnd = NULL;
static HHOOK theme_cbt_hook = NULL;
static WNDPROC orig_mmmain_proc = NULL;

/* Track subclassed dialogs */
#define MAX_SUBCLASSED 32
static struct { HWND hwnd; WNDPROC orig; } subclassed_dlgs[MAX_SUBCLASSED];
static int num_subclassed = 0;

/* ---- Helpers ---- */

static int clamp255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

/* ---- Config ---- */

static void cfg_get_path(char *buf, int sz) {
    GetModuleFileNameA(NULL, buf, sz);
    /* strip exe name, append theme.cfg */
    char *last = strrchr(buf, '\\');
    if (last) *(last + 1) = '\0';
    else buf[0] = '\0';
    strncat(buf, "theme.cfg", sz - (int)strlen(buf) - 1);
}

static void cfg_load(void) {
    char path[MAX_PATH];
    cfg_get_path(path, MAX_PATH);

    FILE *f = fopen(path, "r");
    if (!f) {
        theme_idx = -1;
        return;
    }

    char line[256];
    theme_idx = -1;
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        if (strncmp(line, "theme=", 6) == 0) {
            const char *val = line + 6;
            if (strcmp(val, "none") == 0 || val[0] == '\0') {
                theme_idx = -1;
            } else {
                for (unsigned int i = 0; i < THEME_COUNT; i++) {
                    if (strcmp(val, THEMES[i].key) == 0) {
                        theme_idx = (int)i;
                        break;
                    }
                }
            }
        }
    }
    fclose(f);
}

static void cfg_save(void) {
    char path[MAX_PATH];
    cfg_get_path(path, MAX_PATH);

    FILE *f = fopen(path, "w");
    if (!f) return;

    if (theme_idx >= 0 && theme_idx < (int)THEME_COUNT) {
        fprintf(f, "theme=%s\n", THEMES[theme_idx].key);
    } else {
        fprintf(f, "theme=none\n");
    }
    fclose(f);
}

/* ---- Brush management ---- */

static void theme_update_brushes(void) {
    /* Delete old brushes */
    if (theme_bg_brush) { DeleteObject(theme_bg_brush); theme_bg_brush = NULL; }
    if (theme_panel_brush) { DeleteObject(theme_panel_brush); theme_panel_brush = NULL; }
    if (theme_menubar_brush) { DeleteObject(theme_menubar_brush); theme_menubar_brush = NULL; }

    if (theme_idx < 0 || theme_idx >= (int)THEME_COUNT) {
        theme_active = 0;
        return;
    }

    const theme_def_t *t = &THEMES[theme_idx];

    theme_bg_color    = RGB(t->bg_r, t->bg_g, t->bg_b);
    theme_panel_color = RGB(t->panel_r, t->panel_g, t->panel_b);
    theme_text_color  = RGB(t->text_r, t->text_g, t->text_b);
    theme_accent_color = RGB(t->accent_r, t->accent_g, t->accent_b);
    theme_dim_color   = RGB(t->dim_r, t->dim_g, t->dim_b);

    /* Menu bar is slightly lighter than panel */
    theme_menubar_color = RGB(
        clamp255(t->panel_r + 12),
        clamp255(t->panel_g + 12),
        clamp255(t->panel_b + 12)
    );

    theme_bg_brush      = CreateSolidBrush(theme_bg_color);
    theme_panel_brush   = CreateSolidBrush(theme_panel_color);
    theme_menubar_brush = CreateSolidBrush(theme_menubar_color);

    theme_active = 1;
}

/* ---- Dialog subclassing ---- */

static LRESULT CALLBACK theme_dlg_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

static WNDPROC theme_find_orig_proc(HWND hwnd) {
    for (int i = 0; i < num_subclassed; i++) {
        if (subclassed_dlgs[i].hwnd == hwnd)
            return subclassed_dlgs[i].orig;
    }
    return DefWindowProcA;
}

static LRESULT CALLBACK theme_dlg_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = theme_find_orig_proc(hwnd);

    if (!theme_active)
        return CallWindowProcA(orig, hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)theme_panel_brush;
        }

        case WM_CTLCOLORDLG:
            return (LRESULT)theme_panel_brush;

        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            return (LRESULT)theme_panel_brush;
        }

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, theme_panel_brush);
            return 1;
        }

        case WM_NCDESTROY: {
            /* Remove from subclassed array and restore */
            for (int i = 0; i < num_subclassed; i++) {
                if (subclassed_dlgs[i].hwnd == hwnd) {
                    WNDPROC saved = subclassed_dlgs[i].orig;
                    /* Shift remaining entries down */
                    for (int j = i; j < num_subclassed - 1; j++)
                        subclassed_dlgs[j] = subclassed_dlgs[j + 1];
                    num_subclassed--;
                    return CallWindowProcA(saved, hwnd, msg, wParam, lParam);
                }
            }
            return CallWindowProcA(orig, hwnd, msg, wParam, lParam);
        }
    }

    return CallWindowProcA(orig, hwnd, msg, wParam, lParam);
}

static void theme_subclass_window(HWND hwnd) {
    if (!hwnd) return;

    /* Already subclassed? */
    for (int i = 0; i < num_subclassed; i++) {
        if (subclassed_dlgs[i].hwnd == hwnd)
            return;
    }

    if (num_subclassed >= MAX_SUBCLASSED)
        return;

    WNDPROC old = (WNDPROC)SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)theme_dlg_proc);
    subclassed_dlgs[num_subclassed].hwnd = hwnd;
    subclassed_dlgs[num_subclassed].orig = old;
    num_subclassed++;
}

/* ---- Theme application ---- */

static BOOL CALLBACK theme_enum_children(HWND hwnd, LPARAM lParam) {
    char cls[64];
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (strcmp(cls, "MMANSI") == 0) return TRUE; /* skip terminal */
    theme_subclass_window(hwnd);
    return TRUE;
}

static BOOL CALLBACK theme_enum_toplevel(HWND hwnd, LPARAM lParam) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;

    char cls[64];
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (strcmp(cls, "MMMAIN") == 0) return TRUE;
    if (strcmp(cls, "MMANSI") == 0) return TRUE;

    theme_subclass_window(hwnd);
    return TRUE;
}

static void theme_apply_menus(HWND mw) {
    HMENU menubar = GetMenu(mw);
    if (!menubar) return;

    /* Set menu bar background */
    MENUINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = theme_menubar_brush;
    SetMenuInfo(menubar, &mi);

    /* Convert menu bar items to owner-draw */
    int count = GetMenuItemCount(menubar);
    for (int i = 0; i < count; i++) {
        MENUITEMINFOA mii;
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_FTYPE | MIIM_STRING;
        mii.dwTypeData = NULL;
        mii.cch = 0;
        GetMenuItemInfoA(menubar, i, TRUE, &mii);

        /* Get the text length */
        int textlen = mii.cch;
        if (textlen <= 0) continue;

        char *text = (char *)malloc(textlen + 1);
        if (!text) continue;

        mii.dwTypeData = text;
        mii.cch = textlen + 1;
        mii.fMask = MIIM_FTYPE | MIIM_STRING;
        GetMenuItemInfoA(menubar, i, TRUE, &mii);

        /* Set as owner-draw, store text pointer in dwItemData */
        mii.fMask = MIIM_FTYPE | MIIM_DATA;
        mii.fType |= MFT_OWNERDRAW;
        mii.dwItemData = (ULONG_PTR)text;
        SetMenuItemInfoA(menubar, i, TRUE, &mii);
    }

    /* Process submenus */
    for (int i = 0; i < count; i++) {
        HMENU sub = GetSubMenu(menubar, i);
        if (!sub) continue;

        /* Set submenu background */
        MENUINFO smi;
        memset(&smi, 0, sizeof(smi));
        smi.cbSize = sizeof(smi);
        smi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
        smi.hbrBack = theme_panel_brush;
        SetMenuInfo(sub, &smi);

        /* Convert submenu items to owner-draw */
        int sc = GetMenuItemCount(sub);
        for (int j = 0; j < sc; j++) {
            MENUITEMINFOA sii;
            memset(&sii, 0, sizeof(sii));
            sii.cbSize = sizeof(sii);
            sii.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
            sii.dwTypeData = NULL;
            sii.cch = 0;
            GetMenuItemInfoA(sub, j, TRUE, &sii);

            /* Skip separators and already owner-draw items */
            if (sii.fType & MFT_SEPARATOR) continue;
            if (sii.fType & MFT_OWNERDRAW) continue;

            int slen = sii.cch;
            if (slen <= 0) continue;

            char *stext = (char *)malloc(slen + 1);
            if (!stext) continue;

            sii.dwTypeData = stext;
            sii.cch = slen + 1;
            sii.fMask = MIIM_FTYPE | MIIM_STRING;
            GetMenuItemInfoA(sub, j, TRUE, &sii);

            sii.fMask = MIIM_FTYPE | MIIM_DATA;
            sii.fType |= MFT_OWNERDRAW;
            sii.dwItemData = (ULONG_PTR)stext;
            SetMenuItemInfoA(sub, j, TRUE, &sii);

            /* Recurse into nested submenus */
            HMENU nested = GetSubMenu(sub, j);
            if (nested) {
                MENUINFO nmi;
                memset(&nmi, 0, sizeof(nmi));
                nmi.cbSize = sizeof(nmi);
                nmi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
                nmi.hbrBack = theme_panel_brush;
                SetMenuInfo(nested, &nmi);

                int nc = GetMenuItemCount(nested);
                for (int k = 0; k < nc; k++) {
                    MENUITEMINFOA nii;
                    memset(&nii, 0, sizeof(nii));
                    nii.cbSize = sizeof(nii);
                    nii.fMask = MIIM_FTYPE | MIIM_STRING;
                    nii.dwTypeData = NULL;
                    nii.cch = 0;
                    GetMenuItemInfoA(nested, k, TRUE, &nii);

                    if (nii.fType & MFT_SEPARATOR) continue;
                    if (nii.fType & MFT_OWNERDRAW) continue;

                    int nlen = nii.cch;
                    if (nlen <= 0) continue;

                    char *ntext = (char *)malloc(nlen + 1);
                    if (!ntext) continue;

                    nii.dwTypeData = ntext;
                    nii.cch = nlen + 1;
                    nii.fMask = MIIM_FTYPE | MIIM_STRING;
                    GetMenuItemInfoA(nested, k, TRUE, &nii);

                    nii.fMask = MIIM_FTYPE | MIIM_DATA;
                    nii.fType |= MFT_OWNERDRAW;
                    nii.dwItemData = (ULONG_PTR)ntext;
                    SetMenuItemInfoA(nested, k, TRUE, &nii);
                }
            }
        }
    }
}

static void theme_apply_toolbar(HWND mw) {
    /* Find rebar/toolbar children and theme them */
    HWND child = GetWindow(mw, GW_CHILD);
    while (child) {
        char cls[64];
        GetClassNameA(child, cls, sizeof(cls));

        if (strcmp(cls, "ReBarWindow32") == 0 || strcmp(cls, "ToolbarWindow32") == 0) {
            /* Set toolbar background color via custom draw in WM_NOTIFY,
               but we can at least set the rebar band colors */
            SendMessageA(child, RB_SETBKCOLOR, 0, (LPARAM)theme_bg_color);
            SendMessageA(child, RB_SETTEXTCOLOR, 0, (LPARAM)theme_text_color);

            /* Theme toolbar band children */
            HWND band = GetWindow(child, GW_CHILD);
            while (band) {
                char bcls[64];
                GetClassNameA(band, bcls, sizeof(bcls));
                if (strcmp(bcls, "ToolbarWindow32") == 0) {
                    SendMessageA(band, TB_SETBKCOLOR, 0, (LPARAM)theme_bg_color);
                }
                band = GetWindow(band, GW_HWNDNEXT);
            }
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static void theme_apply_all(void) {
    theme_update_brushes();
    if (!theme_active) return;

    HWND mw = g_api ? g_api->get_mmmain_hwnd() : NULL;
    if (!mw) return;

    /* Theme statusbar */
    HWND sb = FindWindowExA(mw, NULL, "msctls_statusbar32", NULL);
    if (sb) {
        SendMessageA(sb, SB_SETBKCOLOR, 0, (LPARAM)theme_bg_color);
        InvalidateRect(sb, NULL, TRUE);
    }

    /* Subclass top-level windows in process (skip MMMAIN, MMANSI) */
    EnumWindows(theme_enum_toplevel, 0);

    /* Subclass MMMAIN's children (skip MMANSI) */
    EnumChildWindows(mw, theme_enum_children, 0);

    /* Theme menus */
    theme_apply_menus(mw);

    /* Theme toolbar/rebar */
    theme_apply_toolbar(mw);

    /* Force redraw */
    InvalidateRect(mw, NULL, TRUE);
    DrawMenuBar(mw);
}

/* ---- Theme settings dialog ---- */

static LRESULT CALLBACK theme_settings_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            /* Create label */
            CreateWindowA("STATIC", "Theme:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                10, 12, 50, 20, hwnd, NULL,
                GetModuleHandleA(NULL), NULL);

            /* Create combo box */
            HWND combo = CreateWindowA("COMBOBOX", "",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                65, 10, 180, 300, hwnd, (HMENU)(UINT_PTR)IDM_THEME_COMBO,
                GetModuleHandleA(NULL), NULL);

            /* Add "None" entry */
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"(None)");

            /* Add all themes */
            for (unsigned int i = 0; i < THEME_COUNT; i++) {
                SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)THEMES[i].name);
            }

            /* Select current */
            if (theme_idx >= 0 && theme_idx < (int)THEME_COUNT)
                SendMessageA(combo, CB_SETCURSEL, theme_idx + 1, 0);
            else
                SendMessageA(combo, CB_SETCURSEL, 0, 0);

            /* Create Apply button */
            CreateWindowA("BUTTON", "Apply",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                255, 9, 65, 24, hwnd, (HMENU)(UINT_PTR)IDM_THEME_APPLY,
                GetModuleHandleA(NULL), NULL);

            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == IDM_THEME_COMBO && code == CBN_SELCHANGE) {
                HWND combo = GetDlgItem(hwnd, IDM_THEME_COMBO);
                int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
                if (sel == 0)
                    theme_idx = -1;
                else
                    theme_idx = sel - 1;
                theme_apply_all();
                cfg_save();
                if (g_api) g_api->log("[theme_plugin] Theme changed to: %s\n",
                    theme_idx >= 0 ? THEMES[theme_idx].name : "None");
            }
            if (id == IDM_THEME_APPLY) {
                HWND combo = GetDlgItem(hwnd, IDM_THEME_COMBO);
                int sel = (int)SendMessageA(combo, CB_GETCURSEL, 0, 0);
                if (sel == 0)
                    theme_idx = -1;
                else
                    theme_idx = sel - 1;
                theme_apply_all();
                cfg_save();
                if (g_api) g_api->log("[theme_plugin] Theme applied: %s\n",
                    theme_idx >= 0 ? THEMES[theme_idx].name : "None");
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (theme_active) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, theme_text_color);
                SetBkColor(hdc, theme_panel_color);
                SetBkMode(hdc, OPAQUE);
                return (LRESULT)theme_panel_brush;
            }
            break;
        }

        case WM_CTLCOLORDLG:
            if (theme_active)
                return (LRESULT)theme_panel_brush;
            break;

        case WM_ERASEBKGND:
            if (theme_active) {
                HDC hdc = (HDC)wParam;
                RECT rc;
                GetClientRect(hwnd, &rc);
                FillRect(hdc, &rc, theme_panel_brush);
                return 1;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            theme_settings_hwnd = NULL;
            return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void show_themes(HWND parent) {
    if (theme_settings_hwnd) {
        SetForegroundWindow(theme_settings_hwnd);
        return;
    }

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = theme_settings_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = THEME_SETTINGS_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    theme_settings_hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        THEME_SETTINGS_CLASS,
        "MajorSLOP Themes",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 340, 55,
        parent, NULL, GetModuleHandleA(NULL), NULL);

    if (theme_settings_hwnd) {
        ShowWindow(theme_settings_hwnd, SW_SHOW);
        UpdateWindow(theme_settings_hwnd);
    }
}

/* ---- CBT hook ---- */

static LRESULT CALLBACK theme_cbt_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_ACTIVATE && theme_active) {
        HWND hwnd = (HWND)wParam;
        char cls[64];
        GetClassNameA(hwnd, cls, sizeof(cls));

        if (strcmp(cls, "MMANSI") != 0 && strcmp(cls, "MMMAIN") != 0) {
            DWORD pid;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId()) {
                theme_subclass_window(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
            }
        }
    }
    return CallNextHookEx(theme_cbt_hook, nCode, wParam, lParam);
}

/* ---- Scanner thread ---- */

static DWORD WINAPI theme_scanner_thread(LPVOID param) {
    (void)param;
    for (;;) {
        Sleep(2000);
        if (theme_active) {
            theme_apply_all();
        }
    }
    return 0;
}

/* ---- MMMAIN sub-subclass ---- */

static LRESULT CALLBACK theme_mmmain_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!theme_active)
        return CallWindowProcA(orig_mmmain_proc, hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDM_THEME_MENU) {
                show_themes(hwnd);
                return 0;
            }
            break;
        }

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            SetBkMode(hdc, OPAQUE);
            return (LRESULT)theme_panel_brush;
        }

        case WM_CTLCOLORDLG:
            return (LRESULT)theme_panel_brush;

        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, theme_text_color);
            SetBkColor(hdc, theme_panel_color);
            return (LRESULT)theme_panel_brush;
        }

        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, theme_bg_brush);
            return 1;
        }

        case WM_NCPAINT: {
            /* Let the default draw the non-client area first */
            LRESULT lr = CallWindowProcA(orig_mmmain_proc, hwnd, msg, wParam, lParam);
            /* Then paint menu bar background */
            HDC hdc = GetWindowDC(hwnd);
            if (hdc) {
                MENUBARINFO mbi;
                memset(&mbi, 0, sizeof(mbi));
                mbi.cbSize = sizeof(mbi);
                if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
                    RECT bar = mbi.rcBar;
                    /* Convert screen coords to window coords */
                    POINT pt = { 0, 0 };
                    MapWindowPoints(hwnd, NULL, &pt, 1);
                    /* Actually we need window-relative coords */
                    RECT wr;
                    GetWindowRect(hwnd, &wr);
                    OffsetRect(&bar, -wr.left, -wr.top);
                    FillRect(hdc, &bar, theme_menubar_brush);

                    /* Redraw menu bar text */
                    HMENU menu = GetMenu(hwnd);
                    if (menu) {
                        HFONT font = (HFONT)SendMessageA(hwnd, WM_GETFONT, 0, 0);
                        if (!font) font = (HFONT)GetStockObject(SYSTEM_FONT);
                        HFONT oldfont = (HFONT)SelectObject(hdc, font);
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, theme_text_color);

                        int mc = GetMenuItemCount(menu);
                        for (int i = 0; i < mc; i++) {
                            RECT ir;
                            if (GetMenuItemRect(hwnd, menu, i, &ir)) {
                                OffsetRect(&ir, -wr.left, -wr.top);
                                FillRect(hdc, &ir, theme_menubar_brush);

                                char text[128];
                                MENUITEMINFOA mii;
                                memset(&mii, 0, sizeof(mii));
                                mii.cbSize = sizeof(mii);
                                mii.fMask = MIIM_STRING | MIIM_DATA;
                                mii.dwTypeData = text;
                                mii.cch = sizeof(text);
                                if (GetMenuItemInfoA(menu, i, TRUE, &mii) && mii.dwTypeData) {
                                    /* Use dwItemData text if available (ownerdraw) */
                                    const char *label = mii.dwItemData ? (const char *)mii.dwItemData : text;
                                    DrawTextA(hdc, label, -1, &ir,
                                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                                }
                            }
                        }
                        SelectObject(hdc, oldfont);
                    }
                }
                ReleaseDC(hwnd, hdc);
            }
            return lr;
        }

        case WM_NCACTIVATE: {
            LRESULT lr = CallWindowProcA(orig_mmmain_proc, hwnd, msg, wParam, lParam);
            /* Repaint menu bar after activation change */
            SendMessageA(hwnd, WM_NCPAINT, 1, 0);
            return lr;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT *mis = (MEASUREITEMSTRUCT *)lParam;
            if (mis->CtlType == ODT_MENU) {
                const char *text = (const char *)mis->itemData;
                if (text) {
                    HDC hdc = GetDC(hwnd);
                    SIZE sz;
                    GetTextExtentPoint32A(hdc, text, (int)strlen(text), &sz);
                    mis->itemWidth = sz.cx + 12;
                    mis->itemHeight = sz.cy + 6;
                    ReleaseDC(hwnd, hdc);
                } else {
                    mis->itemWidth = 60;
                    mis->itemHeight = 20;
                }
                return TRUE;
            }
            break;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lParam;
            if (dis->CtlType == ODT_MENU) {
                const char *text = (const char *)dis->itemData;
                BOOL selected = (dis->itemState & ODS_SELECTED) != 0;
                BOOL is_menubar = (dis->itemState & ODS_NOACCEL) != 0
                    || (GetMenu(hwnd) != NULL);

                /* Determine if this is a top-level menu bar item */
                HMENU menubar = GetMenu(hwnd);
                BOOL is_toplevel = FALSE;
                if (menubar) {
                    int mc = GetMenuItemCount(menubar);
                    for (int i = 0; i < mc; i++) {
                        if (GetMenuItemID(menubar, i) == dis->itemID ||
                            GetSubMenu(menubar, i) != NULL) {
                            /* Check by matching itemData */
                            MENUITEMINFOA mii;
                            memset(&mii, 0, sizeof(mii));
                            mii.cbSize = sizeof(mii);
                            mii.fMask = MIIM_DATA;
                            GetMenuItemInfoA(menubar, i, TRUE, &mii);
                            if (mii.dwItemData == (ULONG_PTR)text) {
                                is_toplevel = TRUE;
                                break;
                            }
                        }
                    }
                }

                HBRUSH bg;
                COLORREF fg;
                if (selected) {
                    bg = CreateSolidBrush(theme_accent_color);
                    fg = RGB(0xFF, 0xFF, 0xFF);
                } else if (is_toplevel) {
                    bg = theme_menubar_brush;
                    fg = theme_text_color;
                } else {
                    bg = theme_panel_brush;
                    fg = theme_text_color;
                }

                FillRect(dis->hDC, &dis->rcItem, bg);

                if (selected && bg != theme_menubar_brush && bg != theme_panel_brush)
                    DeleteObject(bg);

                if (text) {
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, fg);

                    RECT tr = dis->rcItem;
                    if (!is_toplevel) {
                        tr.left += 20; /* indent submenu items */
                    }

                    /* Handle tab-separated text (item\tshortcut) */
                    char *tab = strchr(text, '\t');
                    if (tab && !is_toplevel) {
                        int mainlen = (int)(tab - text);
                        DrawTextA(dis->hDC, text, mainlen, &tr,
                            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                        RECT sr = dis->rcItem;
                        sr.right -= 8;
                        DrawTextA(dis->hDC, tab + 1, -1, &sr,
                            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                    } else {
                        DrawTextA(dis->hDC, text, -1, &tr,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                }

                if (dis->itemState & ODS_GRAYED) {
                    /* Draw grayed text over */
                    SetTextColor(dis->hDC, theme_dim_color);
                    if (text) {
                        RECT tr = dis->rcItem;
                        if (!is_toplevel) tr.left += 20;
                        DrawTextA(dis->hDC, text, -1, &tr,
                            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                }

                return TRUE;
            }
            break;
        }
    }

    /* WM_COMMAND for theme menu when theme is NOT active */
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_THEME_MENU) {
        show_themes(hwnd);
        return 0;
    }

    return CallWindowProcA(orig_mmmain_proc, hwnd, msg, wParam, lParam);
}

/* ---- Plugin lifecycle ---- */

static int theme_init(const slop_api_t *api) {
    g_api = api;
    cfg_load();
    theme_update_brushes();

    /* Sub-subclass MMMAIN for full WM_CTLCOLOR control */
    HWND mw = api->get_mmmain_hwnd();
    if (mw) {
        orig_mmmain_proc = (WNDPROC)SetWindowLongA(mw, GWL_WNDPROC, (LONG)theme_mmmain_proc);
        api->log("[theme_plugin] Subclassed MMMAIN\n");
    }

    /* Add menu item */
    api->add_menu_item("Themes...", IDM_THEME_MENU);

    /* Apply initial theme */
    if (theme_active) theme_apply_all();

    /* Install CBT hook for new dialogs */
    DWORD tid = GetCurrentThreadId();
    theme_cbt_hook = SetWindowsHookExA(WH_CBT, theme_cbt_proc, NULL, tid);

    /* Start scanner thread for periodic re-theming */
    CreateThread(NULL, 0, theme_scanner_thread, NULL, 0, NULL);

    api->log("[theme_plugin] Initialized (theme=%s)\n",
        theme_idx >= 0 ? THEMES[theme_idx].name : "None");

    return 0;
}

static void theme_shutdown(void) {
    if (theme_cbt_hook) {
        UnhookWindowsHookEx(theme_cbt_hook);
        theme_cbt_hook = NULL;
    }

    /* Restore MMMAIN proc */
    HWND mw = g_api ? g_api->get_mmmain_hwnd() : NULL;
    if (mw && orig_mmmain_proc) {
        SetWindowLongA(mw, GWL_WNDPROC, (LONG)orig_mmmain_proc);
        orig_mmmain_proc = NULL;
    }

    /* Delete brushes */
    if (theme_bg_brush) { DeleteObject(theme_bg_brush); theme_bg_brush = NULL; }
    if (theme_panel_brush) { DeleteObject(theme_panel_brush); theme_panel_brush = NULL; }
    if (theme_menubar_brush) { DeleteObject(theme_menubar_brush); theme_menubar_brush = NULL; }

    if (g_api) g_api->log("[theme_plugin] Shutdown\n");
}

/* ---- Plugin descriptor ---- */

static slop_plugin_t theme_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Themes",
    .author      = "Tripmunk",
    .description = "Window theming engine for MegaMUD",
    .version     = "1.0.0",
    .init        = theme_init,
    .shutdown    = theme_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = NULL,  /* we sub-subclass MMMAIN directly */
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) {
    return &theme_plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) {
    (void)h; (void)r; (void)p;
    return TRUE;
}
