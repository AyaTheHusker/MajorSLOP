/* vk_filepicker.h — themed Vulkan file picker, modeled on pl_ window.
 *
 * Replaces the flicker-prone Win32 GetOpenFileNameA when overlayed on a
 * fullscreen Vulkan surface. Self-contained: uses the host's push_solid_ui /
 * push_text_ui, ui_themes[current_theme], and ui_scale. Interactions mirror
 * the Paths & Loops window: titlebar drag, scrollwheel, click + double-click.
 *
 * Usage (from caller):
 *     static void my_cb(const char *path) {
 *         if (!path) return;          // cancel
 *         // path is a normalized Windows-style absolute path
 *     }
 *     const char *exts[] = {".mdb", ".accdb"};
 *     fp_open("Select MegaMUD MDB", NULL, exts, 2, my_cb);
 *
 * Then at the host level, call fp_draw/fp_mouse_down/fp_mouse_up/fp_key_down
 * in the usual places. fp_visible() gates the picker being active.
 */
#ifndef VK_FILEPICKER_H
#define VK_FILEPICKER_H

#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

typedef void (*fp_callback_t)(const char *path_or_null);

#define FP_MAX_ENTRIES  2048
#define FP_MAX_EXTS     8
#define FP_EXT_LEN      16

typedef struct {
    char name[MAX_PATH];
    int  is_dir;
    int  is_drive;   /* root-level drive entry (e.g. "C:\\") */
} fp_entry_t;

static int   fp_visible_flag = 0;
static char  fp_title[128] = "";
static char  fp_cur_dir[MAX_PATH] = "";
static char  fp_exts[FP_MAX_EXTS][FP_EXT_LEN];
static int   fp_ext_count = 0;
static fp_callback_t fp_cb = NULL;

static fp_entry_t fp_entries[FP_MAX_ENTRIES];
static int fp_entry_count = 0;
static int fp_sel = 0;
static int fp_scroll = 0;

static float fp_x = 140, fp_y = 80, fp_w = 640, fp_h = 440;
static int   fp_scaled = 0;
static int   fp_dragging = 0;
static float fp_drag_dx = 0, fp_drag_dy = 0;

/* double-click detection */
static int   fp_last_click_idx = -1;
static DWORD fp_last_click_ms = 0;

static int fp_visible(void) { return fp_visible_flag; }

/* ---- Path helpers ---- */

static void fp_normalize(char *p)
{
    for (char *s = p; *s; s++) if (*s == '/') *s = '\\';
}

static int fp_is_drive_root(const char *p)
{
    /* "X:\\" (len 3) or "X:" (len 2) */
    if (!p || !p[0]) return 0;
    if (p[1] != ':') return 0;
    if (p[2] == 0) return 1;
    if (p[2] == '\\' && p[3] == 0) return 1;
    return 0;
}

static int fp_is_computer_root(const char *p)
{
    return (!p || !p[0]);
}

/* qsort comparator: dirs first, then alphabetical (case-insensitive) */
static int fp_cmp(const void *a, const void *b)
{
    const fp_entry_t *ea = (const fp_entry_t *)a;
    const fp_entry_t *eb = (const fp_entry_t *)b;
    if (ea->is_dir != eb->is_dir) return eb->is_dir - ea->is_dir;
    return lstrcmpiA(ea->name, eb->name);
}

/* ---- Directory scanning ---- */

static void fp_scan_drives(void)
{
    fp_entry_count = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        if (fp_entry_count >= FP_MAX_ENTRIES) break;
        fp_entry_t *e = &fp_entries[fp_entry_count++];
        e->name[0] = 'A' + i;
        e->name[1] = ':';
        e->name[2] = '\\';
        e->name[3] = 0;
        e->is_dir = 1;
        e->is_drive = 1;
    }
}

static void fp_rescan(void)
{
    fp_entry_count = 0;
    fp_sel = 0;
    fp_scroll = 0;

    if (fp_is_computer_root(fp_cur_dir)) {
        fp_scan_drives();
        return;
    }

    /* ".." unless we're at a drive root with no parent */
    if (fp_entry_count < FP_MAX_ENTRIES) {
        fp_entry_t *e = &fp_entries[fp_entry_count++];
        lstrcpynA(e->name, "..", sizeof(e->name));
        e->is_dir = 1;
        e->is_drive = 0;
    }

    char pattern[MAX_PATH + 4];
    size_t dlen = strlen(fp_cur_dir);
    if (dlen > 0 && fp_cur_dir[dlen - 1] == '\\')
        _snprintf(pattern, sizeof(pattern), "%s*", fp_cur_dir);
    else
        _snprintf(pattern, sizeof(pattern), "%s\\*", fp_cur_dir);
    pattern[sizeof(pattern) - 1] = 0;

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    int had_err = (h == INVALID_HANDLE_VALUE);
    int first_entry_start = fp_entry_count;
    if (!had_err) {
        do {
            if (fp_entry_count >= FP_MAX_ENTRIES) break;
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            int is_dir = !!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            if (!is_dir && fp_ext_count > 0) {
                const char *dot = strrchr(fd.cFileName, '.');
                int ok = 0;
                if (dot) {
                    for (int i = 0; i < fp_ext_count; i++) {
                        if (lstrcmpiA(dot, fp_exts[i]) == 0) { ok = 1; break; }
                    }
                }
                if (!ok) continue;
            }
            fp_entry_t *e = &fp_entries[fp_entry_count++];
            lstrcpynA(e->name, fd.cFileName, sizeof(e->name));
            e->is_dir = is_dir;
            e->is_drive = 0;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* sort only the scanned portion (".." stays on top) */
    if (fp_entry_count > first_entry_start) {
        qsort(&fp_entries[first_entry_start],
              fp_entry_count - first_entry_start,
              sizeof(fp_entry_t), fp_cmp);
    }
}

/* ---- Navigation ---- */

static void fp_go_up(void)
{
    if (fp_is_computer_root(fp_cur_dir)) return;
    if (fp_is_drive_root(fp_cur_dir)) {
        fp_cur_dir[0] = 0;      /* jump to Computer (drive list) */
        fp_rescan();
        return;
    }
    size_t n = strlen(fp_cur_dir);
    /* strip trailing slash */
    while (n > 0 && fp_cur_dir[n - 1] == '\\') { fp_cur_dir[n - 1] = 0; n--; }
    char *last = strrchr(fp_cur_dir, '\\');
    if (last) {
        /* keep the trailing slash on drive roots so "C:\foo" -> "C:\" */
        if (last == fp_cur_dir + 2 && fp_cur_dir[1] == ':') {
            fp_cur_dir[3] = 0;  /* "C:\" */
        } else {
            *last = 0;
        }
    } else {
        fp_cur_dir[0] = 0;
    }
    fp_rescan();
}

static void fp_enter_dir(const fp_entry_t *e)
{
    if (e->is_drive) {
        lstrcpynA(fp_cur_dir, e->name, sizeof(fp_cur_dir));
        fp_rescan();
        return;
    }
    if (!strcmp(e->name, "..")) {
        fp_go_up();
        return;
    }
    /* append */
    size_t n = strlen(fp_cur_dir);
    if (n + strlen(e->name) + 2 >= sizeof(fp_cur_dir)) return;
    if (n > 0 && fp_cur_dir[n - 1] != '\\') { fp_cur_dir[n++] = '\\'; fp_cur_dir[n] = 0; }
    strcat(fp_cur_dir, e->name);
    fp_rescan();
}

static void fp_commit_file(const fp_entry_t *e)
{
    char path[MAX_PATH];
    size_t n = strlen(fp_cur_dir);
    if (n > 0 && fp_cur_dir[n - 1] == '\\')
        _snprintf(path, sizeof(path), "%s%s", fp_cur_dir, e->name);
    else
        _snprintf(path, sizeof(path), "%s\\%s", fp_cur_dir, e->name);
    path[sizeof(path) - 1] = 0;
    fp_visible_flag = 0;
    if (fp_cb) fp_cb(path);
}

static void fp_activate(int idx)
{
    if (idx < 0 || idx >= fp_entry_count) return;
    fp_entry_t *e = &fp_entries[idx];
    if (e->is_dir) fp_enter_dir(e);
    else           fp_commit_file(e);
}

/* ---- Public API ---- */

static void fp_open(const char *title, const char *start_dir,
                    const char *const *exts, int ext_count,
                    fp_callback_t cb)
{
    lstrcpynA(fp_title, title ? title : "Select File", sizeof(fp_title));
    if (start_dir && start_dir[0]) {
        lstrcpynA(fp_cur_dir, start_dir, sizeof(fp_cur_dir));
    } else {
        GetCurrentDirectoryA(sizeof(fp_cur_dir), fp_cur_dir);
    }
    fp_normalize(fp_cur_dir);

    fp_ext_count = 0;
    for (int i = 0; i < ext_count && i < FP_MAX_EXTS; i++) {
        lstrcpynA(fp_exts[i], exts[i], FP_EXT_LEN);
        fp_ext_count++;
    }
    fp_cb = cb;
    fp_visible_flag = 1;
    fp_last_click_idx = -1;
    fp_rescan();
}

static void fp_cancel(void)
{
    if (!fp_visible_flag) return;
    fp_visible_flag = 0;
    if (fp_cb) fp_cb(NULL);
}

/* ---- Geometry ---- */

#define FP_TITLEBAR  22
#define FP_PATHBAR   24
#define FP_BUTTONBAR 34
#define FP_ITEM_H    20
#define FP_PAD       8

static int fp_hit(int mx, int my)
{
    if (!fp_visible_flag) return 0;
    return (mx >= (int)fp_x && mx < (int)(fp_x + fp_w) &&
            my >= (int)fp_y && my < (int)(fp_y + fp_h));
}

static int fp_item_at(int mx, int my)
{
    int list_y = (int)fp_y + FP_TITLEBAR + FP_PATHBAR + 2;
    int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
    if (mx < (int)fp_x + FP_PAD || mx >= (int)(fp_x + fp_w) - FP_PAD) return -1;
    if (my < list_y || my >= list_y + list_h) return -1;
    int idx = (my - list_y) / FP_ITEM_H + fp_scroll;
    if (idx < 0 || idx >= fp_entry_count) return -1;
    return idx;
}

/* ---- Rendering ---- */

static void fp_draw(int vp_w, int vp_h)
{
    if (!fp_visible_flag) return;
    if (!fp_scaled) {
        fp_w = 640 * ui_scale;
        fp_h = 440 * ui_scale;
        fp_x = (vp_w - fp_w) / 2.0f;
        fp_y = (vp_h - fp_h) / 2.0f;
        fp_scaled = 1;
    }

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float,float,float,float,float,float,float,float,int,int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int,int,const char*,float,float,float,int,int,int,int) =
        ui_font_ready ? push_text_ui : push_text;

    int cw = (int)(VSB_CHAR_W * ui_scale);
    int ch = (int)(VSB_CHAR_H * ui_scale);
    int x = (int)fp_x, y = (int)fp_y, w = (int)fp_w, h = (int)fp_h;

    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];
    float dmr = t->dim[0], dmg = t->dim[1], dmb = t->dim[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];

    /* Modal backdrop dims the whole screen */
    psolid(0, 0, vp_w, vp_h, 0, 0, 0, 0.35f, vp_w, vp_h);

    /* Drop shadow */
    psolid(x + 6, y + 6, x + w + 6, y + h + 6, 0, 0, 0, 0.5f, vp_w, vp_h);

    /* Background */
    psolid(x, y, x + w, y + h, bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.98f, vp_w, vp_h);

    /* Border */
    psolid(x, y, x + w, y + 1, acr * 0.5f, acg * 0.5f, acb * 0.5f, 0.8f, vp_w, vp_h);
    psolid(x, y + h - 1, x + w, y + h, acr * 0.4f, acg * 0.4f, acb * 0.4f, 0.8f, vp_w, vp_h);
    psolid(x, y, x + 1, y + h, acr * 0.4f, acg * 0.4f, acb * 0.4f, 0.8f, vp_w, vp_h);
    psolid(x + w - 1, y, x + w, y + h, acr * 0.4f, acg * 0.4f, acb * 0.4f, 0.8f, vp_w, vp_h);

    /* ---- Titlebar ---- */
    psolid(x, y, x + w, y + FP_TITLEBAR,
           acr * 0.25f + bgr * 0.5f, acg * 0.25f + bgg * 0.5f, acb * 0.25f + bgb * 0.5f,
           0.95f, vp_w, vp_h);
    /* Gloss */
    psolid(x + 1, y + 1, x + w - 1, y + FP_TITLEBAR / 2,
           1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
    /* Accent underline */
    psolid(x, y + FP_TITLEBAR - 1, x + w, y + FP_TITLEBAR,
           acr, acg, acb, 0.6f, vp_w, vp_h);
    ptext(x + FP_PAD, y + (FP_TITLEBAR - ch) / 2,
          fp_title, txr, txg, txb, vp_w, vp_h, cw, ch);
    /* Close X */
    ptext(x + w - cw - 6, y + (FP_TITLEBAR - ch) / 2,
          "X", dmr, dmg, dmb, vp_w, vp_h, cw, ch);

    /* ---- Path bar ---- */
    int py = y + FP_TITLEBAR;
    psolid(x, py, x + w, py + FP_PATHBAR,
           bgr * 0.6f, bgg * 0.6f, bgb * 0.6f, 0.85f, vp_w, vp_h);
    psolid(x + FP_PAD, py + 3, x + w - FP_PAD, py + FP_PATHBAR - 3,
           bgr * 0.3f, bgg * 0.3f, bgb * 0.3f, 0.8f, vp_w, vp_h);
    const char *label = fp_is_computer_root(fp_cur_dir) ? "[Computer]" : fp_cur_dir;
    /* Truncate visually if too long */
    int max_chars = (w - 2 * FP_PAD - 8) / cw;
    char disp[MAX_PATH];
    if ((int)strlen(label) <= max_chars) {
        lstrcpynA(disp, label, sizeof(disp));
    } else {
        int keep = max_chars - 4;
        if (keep < 8) keep = 8;
        _snprintf(disp, sizeof(disp), "...%s",
                  label + (int)strlen(label) - keep);
        disp[sizeof(disp) - 1] = 0;
    }
    ptext(x + FP_PAD + 4, py + (FP_PATHBAR - ch) / 2,
          disp, txr, txg, txb, vp_w, vp_h, cw, ch);

    /* ---- List area ---- */
    int list_y = py + FP_PATHBAR + 2;
    int list_h = h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
    psolid(x + FP_PAD - 1, list_y - 1, x + w - FP_PAD + 1, list_y + list_h + 1,
           acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.4f, vp_w, vp_h);
    psolid(x + FP_PAD, list_y, x + w - FP_PAD, list_y + list_h,
           bgr * 0.2f, bgg * 0.2f, bgb * 0.2f, 0.95f, vp_w, vp_h);

    int visible_rows = list_h / FP_ITEM_H;
    for (int row = 0; row < visible_rows; row++) {
        int idx = fp_scroll + row;
        if (idx >= fp_entry_count) break;
        fp_entry_t *e = &fp_entries[idx];
        int iy = list_y + row * FP_ITEM_H;
        int ix = x + FP_PAD + 4;

        /* Selection highlight */
        if (idx == fp_sel) {
            psolid(x + FP_PAD + 1, iy, x + w - FP_PAD - 1, iy + FP_ITEM_H,
                   acr * 0.35f, acg * 0.35f, acb * 0.35f, 0.55f, vp_w, vp_h);
            psolid(x + FP_PAD + 1, iy, x + FP_PAD + 3, iy + FP_ITEM_H,
                   acr, acg, acb, 0.9f, vp_w, vp_h);
        }

        /* Icon: folder CP437 (0xB1) or file */
        const char *icon = e->is_drive ? "\xFE" : (e->is_dir ? "\x10" : "\x07");
        float ir = e->is_dir ? acr : txr;
        float ig = e->is_dir ? acg : txg;
        float ib = e->is_dir ? acb : txb;
        ptext(ix, iy + (FP_ITEM_H - ch) / 2, icon, ir, ig, ib, vp_w, vp_h, cw, ch);

        /* Name */
        float nr = txr, ng = txg, nb = txb;
        if (e->is_dir) { nr = txr; ng = txg; nb = txb; }
        const char *nm = e->name;
        /* Truncate long names */
        char trunc[MAX_PATH];
        int max_nm = (w - 2 * FP_PAD - 8 - cw * 3) / cw;
        if ((int)strlen(nm) > max_nm && max_nm > 8) {
            _snprintf(trunc, sizeof(trunc), "%.*s...",
                      max_nm - 3, nm);
            trunc[sizeof(trunc) - 1] = 0;
            nm = trunc;
        }
        ptext(ix + cw * 2, iy + (FP_ITEM_H - ch) / 2, nm, nr, ng, nb, vp_w, vp_h, cw, ch);
    }

    /* Scrollbar */
    if (fp_entry_count > visible_rows) {
        int sb_x = x + w - FP_PAD - 3;
        int sb_y = list_y;
        int sb_h = list_h;
        psolid(sb_x, sb_y, sb_x + 3, sb_y + sb_h,
               bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.6f, vp_w, vp_h);
        float ratio = (float)visible_rows / (float)fp_entry_count;
        int thumb_h = (int)(sb_h * ratio);
        if (thumb_h < 10) thumb_h = 10;
        int max_scroll = fp_entry_count - visible_rows;
        float pos = (max_scroll > 0) ? (float)fp_scroll / (float)max_scroll : 0.0f;
        int thumb_y = sb_y + (int)((sb_h - thumb_h) * pos);
        psolid(sb_x, thumb_y, sb_x + 3, thumb_y + thumb_h,
               acr * 0.8f, acg * 0.8f, acb * 0.8f, 0.85f, vp_w, vp_h);
    }

    /* ---- Button bar ---- */
    int by = y + h - FP_BUTTONBAR;
    psolid(x, by, x + w, y + h,
           bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.9f, vp_w, vp_h);
    psolid(x, by, x + w, by + 1, acr * 0.4f, acg * 0.4f, acb * 0.4f, 0.6f, vp_w, vp_h);

    /* OK / Cancel buttons */
    int btn_w = 80 * ui_scale;
    int btn_h = FP_BUTTONBAR - 12;
    int btn_ok_x = x + w - 2 * btn_w - FP_PAD * 2;
    int btn_cancel_x = x + w - btn_w - FP_PAD;
    int btn_y = by + 6;
    int can_ok = (fp_sel >= 0 && fp_sel < fp_entry_count &&
                  !fp_entries[fp_sel].is_dir);
    /* OK */
    psolid(btn_ok_x, btn_y, btn_ok_x + btn_w, btn_y + btn_h,
           can_ok ? acr * 0.4f : bgr * 0.3f,
           can_ok ? acg * 0.4f : bgg * 0.3f,
           can_ok ? acb * 0.4f : bgb * 0.3f,
           0.9f, vp_w, vp_h);
    psolid(btn_ok_x, btn_y, btn_ok_x + btn_w, btn_y + 1,
           acr, acg, acb, can_ok ? 0.9f : 0.3f, vp_w, vp_h);
    const char *ok_lbl = "OK";
    ptext(btn_ok_x + (btn_w - (int)strlen(ok_lbl) * cw) / 2,
          btn_y + (btn_h - ch) / 2,
          ok_lbl,
          can_ok ? txr : dmr * 0.5f,
          can_ok ? txg : dmg * 0.5f,
          can_ok ? txb : dmb * 0.5f,
          vp_w, vp_h, cw, ch);
    /* Cancel */
    psolid(btn_cancel_x, btn_y, btn_cancel_x + btn_w, btn_y + btn_h,
           bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.9f, vp_w, vp_h);
    psolid(btn_cancel_x, btn_y, btn_cancel_x + btn_w, btn_y + 1,
           dmr, dmg, dmb, 0.5f, vp_w, vp_h);
    const char *cn_lbl = "Cancel";
    ptext(btn_cancel_x + (btn_w - (int)strlen(cn_lbl) * cw) / 2,
          btn_y + (btn_h - ch) / 2,
          cn_lbl, txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Hint */
    const char *hint = "Enter=open  Backspace=up  Esc=cancel";
    ptext(x + FP_PAD, btn_y + (btn_h - ch) / 2,
          hint, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
}

/* ---- Input ---- */

/* Returns 1 if click was consumed (picker visible + hit its rect). */
static int fp_mouse_down(int mx, int my)
{
    if (!fp_visible_flag) return 0;
    if (!fp_hit(mx, my)) {
        /* Click outside = cancel */
        fp_cancel();
        return 1;
    }
    int x = (int)fp_x, y = (int)fp_y, w = (int)fp_w;

    /* Close X */
    if (mx >= x + w - 20 && mx < x + w - 4 &&
        my >= y && my < y + FP_TITLEBAR) {
        fp_cancel();
        return 1;
    }
    /* Titlebar drag */
    if (my < y + FP_TITLEBAR) {
        fp_dragging = 1;
        fp_drag_dx = mx - fp_x;
        fp_drag_dy = my - fp_y;
        return 1;
    }
    /* Button bar */
    int by = y + (int)fp_h - FP_BUTTONBAR;
    if (my >= by) {
        int btn_w = 80 * ui_scale;
        int btn_h = FP_BUTTONBAR - 12;
        int btn_ok_x = x + w - 2 * btn_w - FP_PAD * 2;
        int btn_cancel_x = x + w - btn_w - FP_PAD;
        int btn_y = by + 6;
        if (my >= btn_y && my < btn_y + btn_h) {
            if (mx >= btn_cancel_x && mx < btn_cancel_x + btn_w) {
                fp_cancel();
                return 1;
            }
            if (mx >= btn_ok_x && mx < btn_ok_x + btn_w) {
                if (fp_sel >= 0 && fp_sel < fp_entry_count &&
                    !fp_entries[fp_sel].is_dir) {
                    fp_commit_file(&fp_entries[fp_sel]);
                }
                return 1;
            }
        }
        return 1;
    }
    /* List item */
    int idx = fp_item_at(mx, my);
    if (idx >= 0) {
        DWORD now = GetTickCount();
        int is_dbl = (idx == fp_last_click_idx && (now - fp_last_click_ms) < 400);
        fp_sel = idx;
        fp_last_click_idx = idx;
        fp_last_click_ms = now;
        if (is_dbl) fp_activate(idx);
        return 1;
    }
    return 1; /* consumed — block bleed-through */
}

static void fp_mouse_move(int mx, int my)
{
    if (!fp_visible_flag || !fp_dragging) return;
    fp_x = mx - fp_drag_dx;
    fp_y = my - fp_drag_dy;
}

static void fp_mouse_up(void)
{
    fp_dragging = 0;
}

/* Returns 1 if wheel was consumed. */
static int fp_wheel(int mx, int my, int delta)
{
    if (!fp_visible_flag || !fp_hit(mx, my)) return 0;
    int step = (delta > 0) ? -3 : 3;
    fp_scroll += step;
    int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
    int visible_rows = list_h / FP_ITEM_H;
    int max_scroll = fp_entry_count - visible_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (fp_scroll < 0) fp_scroll = 0;
    if (fp_scroll > max_scroll) fp_scroll = max_scroll;
    return 1;
}

/* Returns 1 if key consumed. */
static int fp_key_down(unsigned int vk)
{
    if (!fp_visible_flag) return 0;
    switch (vk) {
        case VK_ESCAPE: fp_cancel(); return 1;
        case VK_RETURN: fp_activate(fp_sel); return 1;
        case VK_BACK:   fp_go_up(); return 1;
        case VK_UP: {
            if (fp_sel > 0) fp_sel--;
            int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
            int rows = list_h / FP_ITEM_H;
            if (fp_sel < fp_scroll) fp_scroll = fp_sel;
            if (fp_sel >= fp_scroll + rows) fp_scroll = fp_sel - rows + 1;
            return 1;
        }
        case VK_DOWN: {
            if (fp_sel < fp_entry_count - 1) fp_sel++;
            int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
            int rows = list_h / FP_ITEM_H;
            if (fp_sel < fp_scroll) fp_scroll = fp_sel;
            if (fp_sel >= fp_scroll + rows) fp_scroll = fp_sel - rows + 1;
            return 1;
        }
        case VK_PRIOR: { /* PgUp */
            int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
            int rows = list_h / FP_ITEM_H;
            fp_sel -= rows;
            if (fp_sel < 0) fp_sel = 0;
            fp_scroll = fp_sel;
            return 1;
        }
        case VK_NEXT: { /* PgDn */
            int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
            int rows = list_h / FP_ITEM_H;
            fp_sel += rows;
            if (fp_sel >= fp_entry_count) fp_sel = fp_entry_count - 1;
            if (fp_sel >= fp_scroll + rows) fp_scroll = fp_sel - rows + 1;
            return 1;
        }
        case VK_HOME: fp_sel = 0; fp_scroll = 0; return 1;
        case VK_END: {
            fp_sel = fp_entry_count - 1;
            int list_h = (int)fp_h - FP_TITLEBAR - FP_PATHBAR - FP_BUTTONBAR - 4;
            int rows = list_h / FP_ITEM_H;
            fp_scroll = fp_sel - rows + 1;
            if (fp_scroll < 0) fp_scroll = 0;
            return 1;
        }
    }
    return 0;
}

#endif /* VK_FILEPICKER_H */
