/* vk_mudmap.h — Vulkan-rendered Map Walker panel for vk_terminal.
 *
 * Free-standing floating panel (same style as pst_draw). Owns its own bin
 * data and pan/zoom state. 3D rounded-corner room tiles with pulse-glow
 * animation on current-room change. Numpad routes here when focused.
 *
 * Public surface (all prefixed mdw_):
 *   mdw_draw(vp_w, vp_h)          — render at end of frame
 *   mdw_toggle()                  — show/hide
 *   mdw_mouse_down(mx, my)        — returns 1 if consumed
 *   mdw_mouse_move(mx, my)        — update drag/resize
 *   mdw_mouse_up()                — release drag/resize
 *   mdw_wheel(mx, my, delta)      — zoom in/out
 *   mdw_key_down(vk)              — numpad walking; returns 1 if consumed
 *   mdw_has_focus()               — true when numpad should route here
 *   mdw_bin_exists()              — 1 if mmud.bin present (menu enable)
 *   mdw_set_current(map, room)    — external trigger: parsed from `rm` output
 *   mdw_pulse_on_room(cksum)      — legacy checksum path (kept as no-op shim)
 */

#ifndef VK_MUDMAP_H
#define VK_MUDMAP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <windows.h>

/* ---- Data structures (mirror mudmap/mudmap_test.c) ---- */

typedef struct {
    uint16_t map, room;
    uint16_t npc, shop, spell, cmd;
    uint8_t  lair_count;
    int8_t   light;
    uint8_t  exit_count;
    char     name[64];
    uint32_t checksum;
    uint32_t exit_ofs;
    int16_t  gx, gy;
    char    *lair_raw;  /* v4+: raw MDB Lair field text, NULL if empty */
} MdwRoom;

typedef struct {
    uint8_t  dir;
    uint16_t flags;
    uint16_t tmap, troom;
    uint16_t key_item;
    uint16_t pick_req;
    uint16_t bash_req;
    char     text_cmd[64];
} MdwExit;

typedef struct { uint16_t map; uint32_t count; uint32_t *indices; } MdwMapInfo;

typedef struct {
    uint16_t  len;
    char     *data;
} MdwAuxCell;

typedef struct {
    char      name[64];
    uint8_t   col_count;
    char    **col_names;
    uint32_t  row_count;
    MdwAuxCell *cells;
    char     *blob;
} MdwAuxTable;

typedef struct { uint32_t key, val; } MdwHkv;

/* ---- State ---- */

static MdwRoom     *mdw_rooms = NULL;
static uint32_t     mdw_room_count = 0;
static MdwExit     *mdw_exits = NULL;
static uint32_t     mdw_exit_count = 0;
static MdwMapInfo  *mdw_maps = NULL;
static uint32_t     mdw_map_count = 0;
static MdwAuxTable *mdw_aux = NULL;
static uint32_t     mdw_aux_count = 0;
static MdwHkv      *mdw_hash = NULL;
static uint32_t     mdw_hash_cap = 0;
static int          mdw_loaded = 0;

/* Window state */
static int   mdw_visible = 0;
static int   mdw_focused = 0;
/* When we consume a WM_KEYDOWN (e.g. numpad walking), set this so the
 * matching WM_CHAR gets eaten — otherwise TranslateMessage still types
 * "4" into the terminal input when the user walks west. */
static int   mdw_swallow_char = 0;
static float mdw_x = 120.0f, mdw_y = 80.0f;
static float mdw_w = 640.0f, mdw_h = 480.0f;
static int   mdw_scaled = 0;
static int   mdw_dragging = 0;
static float mdw_drag_ox = 0, mdw_drag_oy = 0;
static int   mdw_resizing = 0;
static int   mdw_resize_edge = 0;  /* bitmask: 1=L,2=R,4=T,8=B */
static float mdw_resize_ox = 0, mdw_resize_oy = 0;
static float mdw_resize_sw = 0, mdw_resize_sh = 0;
static float mdw_resize_sx = 0, mdw_resize_sy = 0;
static int   mdw_panning = 0;
static float mdw_pan_anchor_mx = 0, mdw_pan_anchor_my = 0;

/* View */
static uint16_t mdw_cur_map_view = 1;     /* which map is being displayed */
static float    mdw_view_x = 0, mdw_view_y = 0;  /* world-space pan center */
static float    mdw_zoom = 4.0f;          /* 1.0 = default CELL px */

/* Current room + glow pulse */
static uint32_t mdw_cur_ri = 0xFFFFFFFF;
static DWORD    mdw_pulse_tick = 0;       /* GetTickCount() of most recent change */

/* Cached room code from MegaMUD's Rooms.md (updated on room change) */
static char     mdw_cur_room_code[8] = "";
static int      mdw_cur_room_known = 0;   /* 1 = known in MegaMUD */
static uint32_t mdw_cur_room_code_ri = 0xFFFFFFFF; /* which ri we last checked */

/* Per-room known cache — refreshed on room change, avoids per-frame API calls */
#define MDW_KNOWN_MAX 4096
static int      mdw_known_flags[MDW_KNOWN_MAX];
static char     mdw_known_codes[MDW_KNOWN_MAX][8];
static DWORD    mdw_known_tick = 0;
static uint16_t mdw_known_map = 0xFFFF;

/* Hover state — updated inside mdw_draw from vkm_mouse_x/y (always live,
 * not just during drag). The tooltip draws for mdw_hover_ri if valid. */
static uint32_t mdw_hover_ri = 0xFFFFFFFF;

/* Deferred recenter: while the user is hovering a tile (reading a tooltip),
 * player movement must not jerk the camera off under the cursor. Pending
 * target ri is stashed here and applied on the first frame hover clears. */
static uint32_t mdw_pending_recenter_ri = 0xFFFFFFFF;

/* Auto-recenter the view on every room change (default). When 0, the view
 * stays put — useful for AFK loop scripting where you set the framing once
 * and just watch the player marker move around the visible map. */
static int mdw_auto_recenter = 1;

/* ---- Path recorder (ported from mudmap_test.c) ----
 * Records each room transition as an MME-compatible step so we can write a
 * .mp file and its enhanced .mpx sidecar. The sidebar panel on the right of
 * the map window lists every recorded step live. */
typedef struct {
    uint32_t cksum;     /* room the player is IN before this step */
    uint16_t flags;     /* MegaMUD step flags (bits from EX_* / MdwExit.flags) */
    char     dir[64];   /* compass letter or composed "open door, n" etc. */
    uint16_t map, room; /* for .mpx sidecar */
    uint8_t  lair;      /* lair_count (0 = not a lair) */
    int8_t   illu;      /* light value */
    uint16_t cmd;       /* CMD field (TB index) */
} MdwRecStep;

static int          mdw_rec_active  = 0;
static MdwRecStep  *mdw_rec_path    = NULL;
static uint32_t     mdw_rec_n       = 0;
static uint32_t     mdw_rec_cap     = 0;
static uint32_t     mdw_rec_start_ri = 0xFFFFFFFF;
static int          mdw_rec_gen_mpx = 1;   /* checkbox — sidecar by default */
static int          mdw_show_path_panel = 1;
static int          mdw_path_focus = 0;

/* Button hit rects (filled during draw, read during click). */
static float mdw_btn_rec_x0 = 0, mdw_btn_rec_x1 = 0;
static float mdw_btn_save_x0 = 0, mdw_btn_save_x1 = 0;
static float mdw_btn_clear_x0 = 0, mdw_btn_clear_x1 = 0;
static float mdw_btn_undo_x0 = 0, mdw_btn_undo_x1 = 0;
static float mdw_btn_code_x0 = 0, mdw_btn_code_x1 = 0;

/* ---- Code Room dialog state ---- */
static int  mdw_code_showing = 0;
static char mdw_code_input[5] = "";       /* 4-char code being typed */
static char mdw_code_room_name[80] = "";  /* room name from mmud.bin */
static char mdw_code_area[48] = "";       /* area from mmud.bin lookup */
static uint32_t mdw_code_cksum = 0;       /* checksum of room being coded */
static char mdw_code_status[80] = "";     /* status feedback */
static DWORD mdw_code_status_tick = 0;
static float mdw_cd_btn_ok_x0 = 0, mdw_cd_btn_ok_x1 = 0;
static float mdw_cd_btn_cancel_x0 = 0, mdw_cd_btn_cancel_x1 = 0;
static float mdw_btn_mpx_x0 = 0, mdw_btn_mpx_x1 = 0;
static float mdw_btn_panel_x0 = 0, mdw_btn_panel_x1 = 0;

/* Path panel scroll offset (how many steps scrolled up from bottom) */
static int mdw_path_scroll = 0;

/* Path panel button y range (for click routing) */
static float mdw_pp_y0 = 0, mdw_pp_y1 = 0;
static float mdw_pp_x0 = 0, mdw_pp_x1 = 0;

/* ---- Save dialog state ---- */
static int  mdw_save_showing = 0;
static char mdw_save_title[128]  = "";
static char mdw_save_name[128]   = "";
static char mdw_save_author[64]  = "Custom";
static char mdw_save_area[64]    = "";
static int  mdw_save_field = 0;         /* 0=title, 1=filename, 2=author, 3=area */
static int  mdw_save_is_loop = 0;       /* auto-detected: start==end room */
static char mdw_save_warn[128]  = "";   /* warning if rooms unknown */
static char mdw_save_status[128] = "";  /* feedback message after save */
static DWORD mdw_save_status_tick = 0;
static int  mdw_import_prompt = 0;     /* 1 = showing import dialog after save */
static char mdw_import_mp_path[MAX_PATH] = ""; /* full path of saved .mp for import */
static char mdw_import_title[128] = "";  /* path title for fake_remote("loop <title>") */
static int  mdw_import_is_loop = 0;      /* 1 = saved path is a loop, show [Loop] btn */
static float mdw_sd_btn_import_x0 = 0, mdw_sd_btn_import_x1 = 0;
static float mdw_sd_btn_loop_x0 = 0, mdw_sd_btn_loop_x1 = 0;
static float mdw_sd_btn_icancel_x0 = 0, mdw_sd_btn_icancel_x1 = 0;

/* Area list (unique areas from _RoomsMD aux table) */
static char  mdw_area_names[512][48];
static int   mdw_area_count = 0;
static int   mdw_area_scroll = 0;
static int   mdw_area_sel = -1;  /* -1 = custom text, >=0 = index into mdw_area_names */

/* Save dialog hit rects */
static float mdw_sd_x0 = 0, mdw_sd_y0 = 0, mdw_sd_x1 = 0, mdw_sd_y1 = 0;
static float mdw_sd_btn_save_x0 = 0, mdw_sd_btn_save_x1 = 0;
static float mdw_sd_btn_cancel_x0 = 0, mdw_sd_btn_cancel_x1 = 0;
static float mdw_sd_area_list_y0 = 0, mdw_sd_area_list_y1 = 0;
static float mdw_sd_title_x0 = 0, mdw_sd_title_x1 = 0;
static float mdw_sd_name_x0 = 0, mdw_sd_name_x1 = 0;
static float mdw_sd_author_x0 = 0, mdw_sd_author_x1 = 0;
static float mdw_sd_area_x0 = 0, mdw_sd_area_x1 = 0;

/* ---- MPX-from-path verification ---- */
typedef struct {
    uint32_t cksum;
    uint16_t flags;
    char     dir[64];
} MdwMpParsedStep;

typedef struct {
    uint16_t map, room;
} MdwMpxCapture;

static int   mdw_mpx_verify_active = 0;  /* 0=off, 1=capturing runs */
static int   mdw_mpx_verify_run = 0;     /* current run 0..2 */
static int   mdw_mpx_verify_step = 0;    /* step within current run */
static MdwMpParsedStep *mdw_mpx_parsed = NULL;
static int   mdw_mpx_parsed_n = 0;
static MdwMpxCapture *mdw_mpx_caps[3] = {NULL, NULL, NULL};
static char  mdw_mpx_mp_path[MAX_PATH] = "";
static char  mdw_mpx_start_cksum[12] = "";
static char  mdw_mpx_end_cksum[12]   = "";
static char  mdw_mpx_header_lines[4][256];  /* first 3-4 lines of .mp for re-emit */
static int   mdw_mpx_header_count = 0;
static char  mdw_mpx_status[128] = "";
static DWORD mdw_mpx_status_tick = 0;

/* MegaMUD base directory (auto-detected from working dir) */
static char mdw_megamud_dir[MAX_PATH] = "";

/* Forward declarations for functions defined later in the file */
static uint32_t mdw_room_lookup(uint16_t map, uint16_t room);
static void mdw_send_rm(void);
static int  mdw_verify_room(void);

/* ---- Auto-MPX: passive background .mpx generation ----
 * Scans Default/ for .mp files without .mpx sidecar, watches room
 * transitions, and after 3 identical runs writes the .mpx automatically.
 * Works regardless of what causes movement — MegaMUD paths, loops, gotos,
 * or mmudpy injected commands. Active when mdw_rec_gen_mpx == 1. */

#define MDW_AUTO_MPX_MAX 256
#define MDW_AUTO_MPX_RUNS 3

typedef struct {
    char     filename[64];
    uint32_t *step_cksums;    /* checksum per step (room you're IN) */
    uint16_t *step_flags;
    char     (*step_dirs)[64];
    int      step_count;
    char     start_cksum[12];
    char     end_cksum[12];
    char     header_lines[4][256];
    int      header_count;
    char     mp_fullpath[MAX_PATH];

    /* Per-run capture */
    int      active;          /* currently tracking this path */
    int      run;             /* current run 0..2 */
    int      step;            /* current step within run */
    MdwMpxCapture *caps[MDW_AUTO_MPX_RUNS];
} MdwAutoMpx;

static MdwAutoMpx  mdw_auto_mpx[MDW_AUTO_MPX_MAX];
static int          mdw_auto_mpx_count = 0;
static int          mdw_auto_mpx_scanned = 0;

/* Cached shop Number → ShopType lookup. MME enum: 0=General, 1=Weapons,
 * 2=Armour, 3=Items, 4=Spells, 5=Hospital, 6=Tavern, 7=Bank, 8=Training,
 * 9=Inn, 10=Specific, 11=Gang, 12=Deed. Size 256 covers v1.11p (max ~182). */
#define MDW_SHOP_CACHE_N 256
static signed char mdw_shop_type[MDW_SHOP_CACHE_N];  /* -1 = unknown */
static int         mdw_shop_type_ready = 0;

static void mdw_shop_type_build(void)
{
    mdw_shop_type_ready = 1;
    for (int i = 0; i < MDW_SHOP_CACHE_N; i++) mdw_shop_type[i] = -1;
    for (uint32_t a = 0; a < mdw_aux_count; a++) {
        MdwAuxTable *t = &mdw_aux[a];
        if (strcmp(t->name, "Shops") != 0) continue;
        int c_num = -1, c_type = -1;
        for (int c = 0; c < t->col_count; c++) {
            if (strcmp(t->col_names[c], "Number") == 0)   c_num  = c;
            if (strcmp(t->col_names[c], "ShopType") == 0) c_type = c;
        }
        if (c_num < 0 || c_type < 0) return;
        for (uint32_t rr = 0; rr < t->row_count; rr++) {
            MdwAuxCell *nc = &t->cells[rr * t->col_count + c_num];
            MdwAuxCell *tc = &t->cells[rr * t->col_count + c_type];
            int num = 0, ty = 0;
            for (int ci = 0; ci < nc->len; ci++) {
                char c2 = nc->data[ci];
                if (c2 < '0' || c2 > '9') break;
                num = num * 10 + (c2 - '0');
            }
            for (int ci = 0; ci < tc->len; ci++) {
                char c2 = tc->data[ci];
                if (c2 < '0' || c2 > '9') break;
                ty = ty * 10 + (c2 - '0');
            }
            if (num > 0 && num < MDW_SHOP_CACHE_N) mdw_shop_type[num] = (signed char)ty;
        }
        return;
    }
}

static int mdw_shop_type_get(unsigned shop_num)
{
    if (!mdw_shop_type_ready) mdw_shop_type_build();
    if (shop_num == 0 || shop_num >= MDW_SHOP_CACHE_N) return -1;
    return mdw_shop_type[shop_num];
}
static int mdw_shop_is_trainer(unsigned shop_num) { return mdw_shop_type_get(shop_num) == 8; }
static int mdw_shop_is_healer (unsigned shop_num) { return mdw_shop_type_get(shop_num) == 5; }

/* ---- Path-recorder helpers ---- */

/* Find the exit in `src_ri` whose destination is (dst_map, dst_room).
 * Returns NULL if no such exit is present (ri mismatch, teleport, etc.). */
static MdwExit *mdw_exit_from_to(uint32_t src_ri,
                                 uint16_t dst_map, uint16_t dst_room)
{
    if (src_ri >= mdw_room_count) return NULL;
    MdwRoom *sr = &mdw_rooms[src_ri];
    for (int i = 0; i < sr->exit_count; i++) {
        MdwExit *ex = &mdw_exits[sr->exit_ofs + i];
        if (ex->tmap == dst_map && ex->troom == dst_room) return ex;
    }
    return NULL;
}

/* Direction codes are packed 0=N .. 9=D matching DIR_CODE in mudmap_api.py. */
static const char *mdw_dir_lo[10] = {
    "n","s","e","w","ne","nw","se","sw","u","d"
};

#define MDW_EX_DOOR    0x0001
#define MDW_EX_HIDDEN  0x0002
#define MDW_EX_TRAP    0x0004
#define MDW_EX_KEY     0x0008
#define MDW_EX_CAST    0x0400
#define MDW_EX_TEXTCMD 0x1000

/* Copy cell contents into dst (NUL-terminated). Shared with tooltip code. */
static const char *mdw_aux_cell_str(MdwAuxTable *t, int row, int col,
                                    char *dst, int dstcap)
{
    dst[0] = 0;
    if (!t || row < 0 || col < 0 || row >= (int)t->row_count || col >= t->col_count)
        return dst;
    MdwAuxCell *c = &t->cells[row * t->col_count + col];
    if (c->len == 0xFFFF) return dst;
    int n = c->len < dstcap - 1 ? c->len : dstcap - 1;
    if (c->data && n > 0) memcpy(dst, c->data, n);
    dst[n] = 0;
    return dst;
}

static int mdw_aux_row_by_number(MdwAuxTable *t, uint32_t num)
{
    if (!t || t->row_count == 0) return -1;
    char buf[16]; int n = wsprintfA(buf, "%u", num);
    for (uint32_t r = 0; r < t->row_count; r++) {
        MdwAuxCell *c = &t->cells[r * t->col_count + 0];
        if (c->len == (uint16_t)n && !memcmp(c->data, buf, n)) return (int)r;
    }
    return -1;
}

/* Lookup name of an item or spell by Number in the aux tables. */
static const char *mdw_item_name(uint32_t num, char *dst, int dstcap)
{
    static MdwAuxTable *items = NULL;
    if (!items) for (uint32_t i = 0; i < mdw_aux_count; i++)
        if (!strcmp(mdw_aux[i].name, "Items")) { items = &mdw_aux[i]; break; }
    dst[0] = 0;
    if (!items || num == 0) return dst;
    int r = mdw_aux_row_by_number(items, num);
    int c = -1;
    for (int i = 0; i < items->col_count; i++)
        if (!strcmp(items->col_names[i], "Name")) { c = i; break; }
    return mdw_aux_cell_str(items, r, c, dst, dstcap);
}

static const char *mdw_spell_name(uint32_t num, char *dst, int dstcap)
{
    static MdwAuxTable *spells = NULL;
    if (!spells) for (uint32_t i = 0; i < mdw_aux_count; i++)
        if (!strcmp(mdw_aux[i].name, "Spells")) { spells = &mdw_aux[i]; break; }
    dst[0] = 0;
    if (!spells || num == 0) return dst;
    int r = mdw_aux_row_by_number(spells, num);
    int c = -1;
    for (int i = 0; i < spells->col_count; i++)
        if (!strcmp(spells->col_names[i], "Name")) { c = i; break; }
    return mdw_aux_cell_str(spells, r, c, dst, dstcap);
}

/* Compose step text per MME's recorder logic. */
static void mdw_compose_step_dir(MdwExit *ex, char *out, int outcap)
{
    const char *d = (ex->dir <= 9) ? mdw_dir_lo[ex->dir] : "look";
    char pre[128] = {0};
    int  pn = 0;
    char tmp[64];
    if (ex->flags & MDW_EX_CAST) {
        const char *sn = mdw_spell_name(ex->key_item, tmp, sizeof tmp);
        if (sn[0]) pn += wsprintfA(pre + pn, "cast %s, ", sn);
    }
    if (ex->flags & MDW_EX_KEY) {
        const char *in = mdw_item_name(ex->key_item, tmp, sizeof tmp);
        if (in[0]) pn += wsprintfA(pre + pn, "use %s, ", in);
    }
    if (ex->flags & MDW_EX_HIDDEN) {
        lstrcpyA(pre + pn, "search, "); pn += 8;
    }
    if (ex->flags & MDW_EX_DOOR) {
        lstrcpyA(pre + pn, "open door, "); pn += 11;
    }
    wsprintfA(out, "%s%s", pre, d);
    (void)outcap;
}

/* Append a step to the recorded path for the room we just left (src_ri)
 * via exit `ex`. Grows the array as needed. */
static void mdw_rec_append(uint32_t src_ri, MdwExit *ex)
{
    if (src_ri >= mdw_room_count) return;
    if (mdw_rec_n >= mdw_rec_cap) {
        mdw_rec_cap = mdw_rec_cap ? mdw_rec_cap * 2 : 64;
        mdw_rec_path = (MdwRecStep *)realloc(mdw_rec_path,
                                             mdw_rec_cap * sizeof(MdwRecStep));
    }
    MdwRoom *sr = &mdw_rooms[src_ri];
    MdwRecStep *s = &mdw_rec_path[mdw_rec_n++];
    s->cksum = sr->checksum;
    s->flags = ex ? (ex->flags & 0x0FFF) : 0;
    s->map   = sr->map;
    s->room  = sr->room;
    s->lair  = sr->lair_count;
    s->illu  = sr->light;
    s->cmd   = sr->cmd;
    if (ex) mdw_compose_step_dir(ex, s->dir, sizeof s->dir);
    else    lstrcpyA(s->dir, "look");
}

static void mdw_rec_clear(void)
{
    mdw_rec_n = 0;
    mdw_rec_start_ri = 0xFFFFFFFF;
    mdw_path_scroll = 0;
}

static void mdw_log(const char *fmt, ...);

static void mdw_rec_undo(void)
{
    if (mdw_rec_n == 0) return;
    mdw_rec_n--;
    /* Move map view back to the room of the last remaining step
     * (or the start room if we undid everything) */
    if (mdw_rec_n > 0) {
        MdwRecStep *prev = &mdw_rec_path[mdw_rec_n - 1];
        uint32_t ri = mdw_room_lookup(prev->map, prev->room);
        if (ri < mdw_room_count) {
            mdw_cur_ri = ri;
            mdw_pulse_tick = GetTickCount();
            if (mdw_auto_recenter && mdw_rooms[ri].gx != INT16_MIN) {
                mdw_view_x = mdw_rooms[ri].gx * 20.0f;
                mdw_view_y = mdw_rooms[ri].gy * 20.0f;
            }
        }
    } else if (mdw_rec_start_ri != 0xFFFFFFFF && mdw_rec_start_ri < mdw_room_count) {
        mdw_cur_ri = mdw_rec_start_ri;
        mdw_pulse_tick = GetTickCount();
        if (mdw_auto_recenter && mdw_rooms[mdw_rec_start_ri].gx != INT16_MIN) {
            mdw_view_x = mdw_rooms[mdw_rec_start_ri].gx * 20.0f;
            mdw_view_y = mdw_rooms[mdw_rec_start_ri].gy * 20.0f;
        }
        mdw_rec_start_ri = 0xFFFFFFFF;
    }
    mdw_log("[PATH] Undo step -> %u steps remain\n", mdw_rec_n);
}

/* ---- Logging (MPX operations) ---- */

static void mdw_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    OutputDebugStringA(buf);
    FILE *f = fopen("mudplugin.log", "a");
    if (f) { fwrite(buf, 1, n, f); fflush(f); fclose(f); }
}

/* ---- MegaMUD directory detection ---- */

static void mdw_find_megamud_dir(void)
{
    if (mdw_megamud_dir[0]) return;
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    lstrcpynA(mdw_megamud_dir, cwd, MAX_PATH);
    int len = (int)lstrlenA(mdw_megamud_dir);
    if (len > 0 && mdw_megamud_dir[len-1] != '\\')
        lstrcatA(mdw_megamud_dir, "\\");
}

/* ---- _RoomsMD aux table lookup ---- */

static MdwAuxTable *mdw_roomsmd_table(void)
{
    static MdwAuxTable *cached = NULL;
    if (cached) return cached;
    for (uint32_t i = 0; i < mdw_aux_count; i++) {
        if (strcmp(mdw_aux[i].name, "_RoomsMD") == 0) {
            cached = &mdw_aux[i];
            return cached;
        }
    }
    return NULL;
}

static int mdw_roomsmd_lookup(uint32_t cksum,
                              char *code, int code_sz,
                              char *area, int area_sz,
                              char *name, int name_sz)
{
    MdwAuxTable *t = mdw_roomsmd_table();
    if (!t) return 0;
    char hex[12];
    wsprintfA(hex, "%08X", cksum);
    int c_cksum = -1, c_code = -1, c_area = -1, c_name = -1;
    for (int c = 0; c < t->col_count; c++) {
        if (strcmp(t->col_names[c], "cksum") == 0) c_cksum = c;
        if (strcmp(t->col_names[c], "code")  == 0) c_code  = c;
        if (strcmp(t->col_names[c], "area")  == 0) c_area  = c;
        if (strcmp(t->col_names[c], "name")  == 0) c_name  = c;
    }
    if (c_cksum < 0) return 0;
    for (uint32_t r = 0; r < t->row_count; r++) {
        MdwAuxCell *cc = &t->cells[r * t->col_count + c_cksum];
        if (cc->len != 8) continue;
        if (_strnicmp(cc->data, hex, 8) != 0) continue;
        if (c_code >= 0 && code)
            mdw_aux_cell_str(t, r, c_code, code, code_sz);
        if (c_area >= 0 && area)
            mdw_aux_cell_str(t, r, c_area, area, area_sz);
        if (c_name >= 0 && name)
            mdw_aux_cell_str(t, r, c_name, name, name_sz);
        return 1;
    }
    return 0;
}

/* ---- Area list builder (unique areas from _RoomsMD) ---- */

static void mdw_build_area_list(void)
{
    mdw_area_count = 0;
    MdwAuxTable *t = mdw_roomsmd_table();
    if (!t) return;
    int c_area = -1;
    for (int c = 0; c < t->col_count; c++)
        if (strcmp(t->col_names[c], "area") == 0) { c_area = c; break; }
    if (c_area < 0) return;
    for (uint32_t r = 0; r < t->row_count && mdw_area_count < 512; r++) {
        char buf[48];
        mdw_aux_cell_str(t, r, c_area, buf, sizeof buf);
        if (!buf[0]) continue;
        int dup = 0;
        for (int i = 0; i < mdw_area_count; i++) {
            if (lstrcmpiA(mdw_area_names[i], buf) == 0) { dup = 1; break; }
        }
        if (!dup) lstrcpynA(mdw_area_names[mdw_area_count++], buf, 48);
    }
    /* Sort alphabetically */
    for (int i = 0; i < mdw_area_count - 1; i++)
        for (int j = i + 1; j < mdw_area_count; j++)
            if (lstrcmpiA(mdw_area_names[i], mdw_area_names[j]) > 0) {
                char tmp[48];
                lstrcpynA(tmp, mdw_area_names[i], 48);
                lstrcpynA(mdw_area_names[i], mdw_area_names[j], 48);
                lstrcpynA(mdw_area_names[j], tmp, 48);
            }
}

/* ---- Monster name lookup for MPX lair info ---- */

static void mdw_lair_monsters_str(MdwRoom *room, char *out, int outsz)
{
    out[0] = 0;
    if (!room->lair_raw) return;
    MdwAuxTable *mt = NULL;
    int mcol_num = -1, mcol_name = -1;
    for (uint32_t a = 0; a < mdw_aux_count; a++) {
        if (strcmp(mdw_aux[a].name, "Monsters") == 0) {
            mt = &mdw_aux[a];
            for (int c = 0; c < mt->col_count; c++) {
                if (strcmp(mt->col_names[c], "Number") == 0) mcol_num  = c;
                if (strcmp(mt->col_names[c], "Name")   == 0) mcol_name = c;
            }
            break;
        }
    }
    if (!mt || mcol_num < 0 || mcol_name < 0) return;
    int olen = 0;
    const char *p = strchr(room->lair_raw, ':');
    if (p) p++;
    while (p && *p) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == '[' || *p == 0) break;
        int id = 0;
        const char *q = p;
        while (*q >= '0' && *q <= '9') { id = id * 10 + (*q - '0'); q++; }
        if (q == p) break;
        char nm[64];
        nm[0] = 0;
        int row = mdw_aux_row_by_number(mt, (uint32_t)id);
        if (row >= 0) mdw_aux_cell_str(mt, row, mcol_name, nm, sizeof nm);
        if (!nm[0]) wsprintfA(nm, "#%d", id);
        int nl = (int)lstrlenA(nm);
        if (olen + nl + 2 < outsz) {
            if (olen > 0) { out[olen++] = ','; }
            lstrcpynA(out + olen, nm, outsz - olen);
            olen += nl;
        }
        p = q;
    }
}

/* ---- NPC/Shop name lookup ---- */

static void mdw_npc_name(uint16_t npc_id, char *out, int outsz)
{
    out[0] = 0;
    if (!npc_id) return;
    for (uint32_t a = 0; a < mdw_aux_count; a++) {
        MdwAuxTable *t = &mdw_aux[a];
        if (strcmp(t->name, "Monsters") != 0) continue;
        int row = mdw_aux_row_by_number(t, npc_id);
        if (row < 0) return;
        for (int c = 0; c < t->col_count; c++)
            if (strcmp(t->col_names[c], "Name") == 0)
                { mdw_aux_cell_str(t, row, c, out, outsz); return; }
    }
}

static void mdw_shop_name(uint16_t shop_id, char *out, int outsz)
{
    out[0] = 0;
    if (!shop_id) return;
    for (uint32_t a = 0; a < mdw_aux_count; a++) {
        MdwAuxTable *t = &mdw_aux[a];
        if (strcmp(t->name, "Shops") != 0) continue;
        int row = mdw_aux_row_by_number(t, shop_id);
        if (row < 0) return;
        for (int c = 0; c < t->col_count; c++)
            if (strcmp(t->col_names[c], "Name") == 0)
                { mdw_aux_cell_str(t, row, c, out, outsz); return; }
    }
}

/* ---- Save .mp file ---- */

static int mdw_do_save_mp(const char *dir, const char *filename,
                          const char *path_name, const char *author,
                          const char *area_name)
{
    if (mdw_rec_n == 0 || mdw_rec_start_ri == 0xFFFFFFFF) return 0;

    MdwRoom *start_room = &mdw_rooms[mdw_rec_start_ri];
    MdwRecStep *last = &mdw_rec_path[mdw_rec_n - 1];
    uint32_t end_ri = mdw_cur_ri;
    MdwRoom *end_room = (end_ri < mdw_room_count) ? &mdw_rooms[end_ri] : NULL;

    char start_code[8] = "FFFF", start_area[64] = "", start_name[80] = "";
    char end_code[8]   = "FFFF", end_area[64]   = "", end_name[80]   = "";

    mdw_roomsmd_lookup(start_room->checksum, start_code, 8, start_area, 64, start_name, 80);
    if (end_room)
        mdw_roomsmd_lookup(end_room->checksum, end_code, 8, end_area, 64, end_name, 80);

    if (!start_name[0]) lstrcpynA(start_name, start_room->name, 80);
    if (end_room && !end_name[0]) lstrcpynA(end_name, end_room->name, 80);
    if (!start_area[0] && area_name[0]) lstrcpynA(start_area, area_name, 64);
    if (!end_area[0] && area_name[0]) lstrcpynA(end_area, area_name, 64);

    int is_loop = (end_room && start_room->checksum == end_room->checksum);

    char fullpath[MAX_PATH];
    wsprintfA(fullpath, "%s%s", dir, filename);

    FILE *f = fopen(fullpath, "wb");
    if (!f) {
        mdw_log("[MPX] ERROR: cannot open %s for writing\n", fullpath);
        return 0;
    }

    /* Header line 0: [loop_name][author] */
    if (is_loop && path_name[0])
        fprintf(f, "[%s][%s]\r\n", path_name, author);
    else
        fprintf(f, "[][%s]\r\n", author);

    /* Header line 1: start room */
    fprintf(f, "[%s:%s:%s]\r\n", start_code, start_area, start_name);

    /* Header line 2: end room (absent for loops) */
    if (!is_loop && end_room)
        fprintf(f, "[%s:%s:%s]\r\n", end_code, end_area, end_name);

    /* Header line 3: checksums + step count */
    fprintf(f, "%08X:%08X:%u:-1:0:::\r\n",
            start_room->checksum,
            end_room ? end_room->checksum : start_room->checksum,
            mdw_rec_n);

    /* Steps */
    for (uint32_t i = 0; i < mdw_rec_n; i++) {
        MdwRecStep *s = &mdw_rec_path[i];
        char dir_up[64];
        for (int j = 0; j < 64 && s->dir[j]; j++)
            dir_up[j] = (char)toupper((unsigned char)s->dir[j]);
        dir_up[lstrlenA(s->dir)] = 0;
        fprintf(f, "%08X:%04X:%s\r\n", s->cksum, s->flags, dir_up);
    }
    fclose(f);
    mdw_log("[PATH] Saved .mp: %s (%u steps)\n", fullpath, mdw_rec_n);
    return 1;
}

/* ---- Save .mpx sidecar ---- */

static int mdw_do_save_mpx(const char *dir, const char *filename,
                           const char *path_name, const char *author,
                           const char *area_name)
{
    if (mdw_rec_n == 0 || mdw_rec_start_ri == 0xFFFFFFFF) return 0;

    MdwRoom *start_room = &mdw_rooms[mdw_rec_start_ri];
    uint32_t end_ri = mdw_cur_ri;
    MdwRoom *end_room = (end_ri < mdw_room_count) ? &mdw_rooms[end_ri] : NULL;

    char start_code[8] = "FFFF", start_area[64] = "", start_name[80] = "";
    char end_code[8]   = "FFFF", end_area[64]   = "", end_name[80]   = "";
    mdw_roomsmd_lookup(start_room->checksum, start_code, 8, start_area, 64, start_name, 80);
    if (end_room)
        mdw_roomsmd_lookup(end_room->checksum, end_code, 8, end_area, 64, end_name, 80);
    if (!start_name[0]) lstrcpynA(start_name, start_room->name, 80);
    if (end_room && !end_name[0]) lstrcpynA(end_name, end_room->name, 80);
    if (!start_area[0] && area_name[0]) lstrcpynA(start_area, area_name, 64);
    if (!end_area[0] && area_name[0]) lstrcpynA(end_area, area_name, 64);
    int is_loop = (end_room && start_room->checksum == end_room->checksum);

    char fullpath[MAX_PATH];
    wsprintfA(fullpath, "%s%s", dir, filename);

    FILE *f = fopen(fullpath, "wb");
    if (!f) {
        mdw_log("[MPX] ERROR: cannot open %s for writing\n", fullpath);
        return 0;
    }

    fprintf(f, "MPX1\r\n");
    if (is_loop && path_name[0])
        fprintf(f, "[%s][%s]\r\n", path_name, author);
    else
        fprintf(f, "[][%s]\r\n", author);
    fprintf(f, "[%s:%s:%s]\r\n", start_code, start_area, start_name);
    if (!is_loop && end_room)
        fprintf(f, "[%s:%s:%s]\r\n", end_code, end_area, end_name);
    fprintf(f, "%08X:%08X:%u:-1:0:::\r\n",
            start_room->checksum,
            end_room ? end_room->checksum : start_room->checksum,
            mdw_rec_n);
    fprintf(f, "---\r\n");

    for (uint32_t i = 0; i < mdw_rec_n; i++) {
        MdwRecStep *s = &mdw_rec_path[i];
        char dir_up[64];
        for (int j = 0; j < 64 && s->dir[j]; j++)
            dir_up[j] = (char)toupper((unsigned char)s->dir[j]);
        dir_up[lstrlenA(s->dir)] = 0;

        /* Look up the full room from bin by map/room */
        uint32_t ri = mdw_room_lookup(s->map, s->room);
        MdwRoom *rm = (ri < mdw_room_count) ? &mdw_rooms[ri] : NULL;

        fprintf(f, "%08X:%04X:%s", s->cksum, s->flags, dir_up);
        fprintf(f, "|map=%u|room=%u", (unsigned)s->map, (unsigned)s->room);

        if (rm) {
            fprintf(f, "|name=%s|illu=%d", rm->name, (int)rm->light);
            if (rm->lair_count) {
                fprintf(f, "|lair=%u", (unsigned)rm->lair_count);
                char mobs[256];
                mdw_lair_monsters_str(rm, mobs, sizeof mobs);
                if (mobs[0]) fprintf(f, "|mobs=%s", mobs);
            }
            if (rm->npc) {
                char nm[80]; mdw_npc_name(rm->npc, nm, sizeof nm);
                fprintf(f, "|npc=%s", nm[0] ? nm : "?");
            }
            if (rm->shop) {
                char nm[80]; mdw_shop_name(rm->shop, nm, sizeof nm);
                fprintf(f, "|shop=%s", nm[0] ? nm : "?");
                fprintf(f, "|shoptype=%d", mdw_shop_type_get(rm->shop));
            }
            if (rm->spell) fprintf(f, "|spell=%u", (unsigned)rm->spell);
            if (rm->cmd)   fprintf(f, "|cmd=%u", (unsigned)rm->cmd);
        }
        fprintf(f, "\r\n");
    }

    /* End room info (the room you arrive at after the last step) */
    if (end_room) {
        fprintf(f, "---END\r\n");
        fprintf(f, "map=%u|room=%u|name=%s|illu=%d",
                (unsigned)end_room->map, (unsigned)end_room->room,
                end_room->name, (int)end_room->light);
        if (end_room->lair_count) {
            fprintf(f, "|lair=%u", (unsigned)end_room->lair_count);
            char mobs[256];
            mdw_lair_monsters_str(end_room, mobs, sizeof mobs);
            if (mobs[0]) fprintf(f, "|mobs=%s", mobs);
        }
        if (end_room->npc) {
            char nm[80]; mdw_npc_name(end_room->npc, nm, sizeof nm);
            fprintf(f, "|npc=%s", nm[0] ? nm : "?");
        }
        if (end_room->shop) {
            char nm[80]; mdw_shop_name(end_room->shop, nm, sizeof nm);
            fprintf(f, "|shop=%s", nm[0] ? nm : "?");
        }
        fprintf(f, "\r\n");
    }

    fclose(f);
    mdw_log("[MPX] Saved .mpx: %s (%u steps, from recording)\n", fullpath, mdw_rec_n);
    return 1;
}

/* ---- Open save dialog with auto-detection ---- */

static void mdw_save_dialog_open(void)
{
    if (mdw_rec_n == 0 || mdw_rec_start_ri == 0xFFFFFFFF) return;
    if (mdw_rec_start_ri >= mdw_room_count) return;

    MdwRoom *start_room = &mdw_rooms[mdw_rec_start_ri];
    MdwRoom *end_room = (mdw_cur_ri < mdw_room_count) ? &mdw_rooms[mdw_cur_ri] : NULL;

    mdw_save_is_loop = (end_room && start_room->checksum == end_room->checksum);

    char start_code[8] = "", end_code[8] = "";
    char start_area[64] = "";
    int start_known = mdw_roomsmd_lookup(start_room->checksum, start_code, 8,
                                          start_area, 64, NULL, 0);
    int end_known = end_room ?
        mdw_roomsmd_lookup(end_room->checksum, end_code, 8, NULL, 0, NULL, 0) : 0;

    mdw_save_title[0] = 0;
    mdw_save_name[0] = 0;
    mdw_save_warn[0] = 0;
    mdw_save_area[0] = 0;
    mdw_area_sel = -1;
    mdw_save_field = 0;

    if (mdw_save_is_loop && start_known) {
        lstrcpynA(mdw_save_name, start_code, 5);
    } else if (start_known && end_known) {
        wsprintfA(mdw_save_name, "%.4s%.4s", start_code, end_code);
    }

    if (start_known && start_area[0]) {
        lstrcpynA(mdw_save_area, start_area, sizeof(mdw_save_area));
        for (int i = 0; i < mdw_area_count; i++) {
            if (lstrcmpiA(mdw_area_names[i], start_area) == 0) {
                mdw_area_sel = i;
                break;
            }
        }
    }

    if (!start_known && !end_known)
        wsprintfA(mdw_save_warn, "Start and end rooms are unknown");
    else if (!start_known)
        wsprintfA(mdw_save_warn, "Start room is unknown");
    else if (!end_known && !mdw_save_is_loop)
        wsprintfA(mdw_save_warn, "End room is unknown");

    if (mdw_save_is_loop)
        mdw_save_field = 0;
    else if (mdw_save_name[0])
        mdw_save_field = 0;

    mdw_build_area_list();
    mdw_save_showing = 1;
}

/* ---- Save entry point (called from save dialog) ---- */

static void mdw_save_path(void)
{
    mdw_find_megamud_dir();

    MdwRoom *start_room = &mdw_rooms[mdw_rec_start_ri];
    MdwRoom *end_room = (mdw_cur_ri < mdw_room_count) ? &mdw_rooms[mdw_cur_ri] : NULL;

    char start_code[8] = "FFFF", end_code[8] = "FFFF";
    mdw_roomsmd_lookup(start_room->checksum, start_code, 8, NULL, 0, NULL, 0);
    if (end_room)
        mdw_roomsmd_lookup(end_room->checksum, end_code, 8, NULL, 0, NULL, 0);

    int is_loop = (end_room && start_room->checksum == end_room->checksum);
    char mp_name[64], mpx_name[64];
    if (mdw_save_name[0]) {
        /* User-chosen filename (8 chars max) */
        char base[12] = "";
        lstrcpynA(base, mdw_save_name, 9);
        wsprintfA(mp_name, "%s.mp", base);
    } else if (is_loop) {
        wsprintfA(mp_name, "%sLOOP.mp", start_code);
    } else {
        wsprintfA(mp_name, "%s%s.mp", start_code, end_code);
    }

    lstrcpynA(mpx_name, mp_name, 60);
    char *dot = strrchr(mpx_name, '.');
    if (dot) lstrcpyA(dot, ".mpx");

    char dir[MAX_PATH];
    wsprintfA(dir, "%sPathsToImport\\", mdw_megamud_dir);
    CreateDirectoryA(dir, NULL);

    const char *area = mdw_save_area;
    if (mdw_area_sel >= 0 && mdw_area_sel < mdw_area_count)
        area = mdw_area_names[mdw_area_sel];

    int ok = mdw_do_save_mp(dir, mp_name, mdw_save_title, mdw_save_author, area);
    if (ok && mdw_rec_gen_mpx) {
        char mpx_dir[MAX_PATH];
        wsprintfA(mpx_dir, "%smpx\\", mdw_megamud_dir);
        CreateDirectoryA(mpx_dir, NULL);
        mdw_do_save_mpx(mpx_dir, mpx_name, mdw_save_title, mdw_save_author, area);
    }

    if (ok) {
        wsprintfA(mdw_save_status, "Saved %s%s", mp_name,
                  mdw_rec_gen_mpx ? " + .mpx" : "");
        mdw_log("[PATH] Save complete: %s%s (%s, by %s, area %s)\n",
                dir, mp_name, mdw_save_name, mdw_save_author, area);
        wsprintfA(mdw_import_mp_path, "%s%s", dir, mp_name);
        lstrcpynA(mdw_import_title, mdw_save_title, sizeof(mdw_import_title));
        mdw_import_is_loop = is_loop;
        mdw_import_prompt = 1;
    } else {
        wsprintfA(mdw_save_status, "SAVE FAILED — check log");
    }
    mdw_save_status_tick = GetTickCount();
    mdw_save_showing = 0;
}

/* ---- MPX-from-path: parse .mp file ---- */

static int mdw_mpx_parse_mp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char line[512];
    int header_lines = 0;
    mdw_mpx_header_count = 0;
    mdw_mpx_start_cksum[0] = 0;
    mdw_mpx_end_cksum[0] = 0;

    if (mdw_mpx_parsed) { free(mdw_mpx_parsed); mdw_mpx_parsed = NULL; }
    mdw_mpx_parsed_n = 0;
    int cap = 0;

    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\r'); if (nl) *nl = 0;
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;

        if (line[0] == '[' || header_lines < 3) {
            if (mdw_mpx_header_count < 4)
                lstrcpynA(mdw_mpx_header_lines[mdw_mpx_header_count++], line, 256);
            header_lines++;
            /* Parse the summary line: STARTCKSUM:ENDCKSUM:N:... */
            if (strlen(line) >= 17 && line[8] == ':' && line[17] == ':') {
                memcpy(mdw_mpx_start_cksum, line, 8);
                mdw_mpx_start_cksum[8] = 0;
                memcpy(mdw_mpx_end_cksum, line + 9, 8);
                mdw_mpx_end_cksum[8] = 0;
            }
            continue;
        }

        /* Step line: CHECKSUM:FLAGS:DIRECTION */
        if (strlen(line) < 14 || line[8] != ':') continue;
        if (mdw_mpx_parsed_n >= cap) {
            cap = cap ? cap * 2 : 64;
            mdw_mpx_parsed = (MdwMpParsedStep *)realloc(mdw_mpx_parsed,
                              cap * sizeof(MdwMpParsedStep));
        }
        MdwMpParsedStep *s = &mdw_mpx_parsed[mdw_mpx_parsed_n++];
        char hex[9]; memcpy(hex, line, 8); hex[8] = 0;
        s->cksum = (uint32_t)strtoul(hex, NULL, 16);
        memcpy(hex, line + 9, 4); hex[4] = 0;
        s->flags = (uint16_t)strtoul(hex, NULL, 16);
        lstrcpynA(s->dir, line + 14, 64);
    }
    fclose(f);
    mdw_log("[MPX] Parsed %s: %d steps, start=%s end=%s\n",
            path, mdw_mpx_parsed_n, mdw_mpx_start_cksum, mdw_mpx_end_cksum);
    return mdw_mpx_parsed_n > 0;
}

/* ---- MPX-from-path: start verification ---- */

static void mdw_mpx_verify_start(const char *mp_path)
{
    if (!mdw_mpx_parse_mp(mp_path)) {
        wsprintfA(mdw_mpx_status, "Failed to parse .mp file");
        mdw_mpx_status_tick = GetTickCount();
        return;
    }
    lstrcpynA(mdw_mpx_mp_path, mp_path, MAX_PATH);
    for (int r = 0; r < 3; r++) {
        if (mdw_mpx_caps[r]) { free(mdw_mpx_caps[r]); mdw_mpx_caps[r] = NULL; }
        mdw_mpx_caps[r] = (MdwMpxCapture *)calloc(mdw_mpx_parsed_n, sizeof(MdwMpxCapture));
    }
    mdw_mpx_verify_active = 1;
    mdw_mpx_verify_run = 0;
    mdw_mpx_verify_step = 0;
    wsprintfA(mdw_mpx_status, "MPX verify: waiting for path start (run 1/3)");
    mdw_mpx_status_tick = GetTickCount();
    mdw_log("[MPX] Verify started for %s (%d steps, 3 runs needed)\n",
            mp_path, mdw_mpx_parsed_n);
}

/* ---- MPX-from-path: feed room transition ---- */

static void mdw_mpx_verify_feed(uint16_t map, uint16_t room, uint32_t cksum)
{
    if (!mdw_mpx_verify_active || mdw_mpx_parsed_n == 0) return;

    int step = mdw_mpx_verify_step;
    int run  = mdw_mpx_verify_run;

    if (step < mdw_mpx_parsed_n) {
        if (cksum == mdw_mpx_parsed[step].cksum) {
            mdw_mpx_caps[run][step].map  = map;
            mdw_mpx_caps[run][step].room = room;
            mdw_mpx_verify_step++;
            mdw_log("[MPX] Run %d step %d/%d: map=%u room=%u cksum=%08X OK\n",
                    run + 1, step + 1, mdw_mpx_parsed_n, map, room, cksum);

            if (mdw_mpx_verify_step >= mdw_mpx_parsed_n) {
                mdw_log("[MPX] Run %d/%d complete\n", run + 1, 3);
                mdw_mpx_verify_run++;
                mdw_mpx_verify_step = 0;

                if (mdw_mpx_verify_run >= 3) {
                    /* Check all 3 runs match */
                    int match = 1;
                    for (int s = 0; s < mdw_mpx_parsed_n && match; s++) {
                        for (int r = 1; r < 3; r++) {
                            if (mdw_mpx_caps[r][s].map  != mdw_mpx_caps[0][s].map ||
                                mdw_mpx_caps[r][s].room != mdw_mpx_caps[0][s].room) {
                                mdw_log("[MPX] MISMATCH at step %d: run1=(%u,%u) run%d=(%u,%u)\n",
                                        s, mdw_mpx_caps[0][s].map, mdw_mpx_caps[0][s].room,
                                        r + 1, mdw_mpx_caps[r][s].map, mdw_mpx_caps[r][s].room);
                                match = 0;
                            }
                        }
                    }
                    if (match) {
                        mdw_log("[MPX] All 3 runs MATCH — generating .mpx\n");
                        /* Build mpx path in mpx\ folder */
                        char mpx_dir[MAX_PATH];
                        wsprintfA(mpx_dir, "%smpx\\", mdw_megamud_dir);
                        CreateDirectoryA(mpx_dir, NULL);
                        const char *mp_base = strrchr(mdw_mpx_mp_path, '\\');
                        if (!mp_base) mp_base = mdw_mpx_mp_path; else mp_base++;
                        char mpx_path[MAX_PATH];
                        wsprintfA(mpx_path, "%s%s", mpx_dir, mp_base);
                        char *dot = strrchr(mpx_path, '.');
                        if (dot) lstrcpyA(dot, ".mpx");
                        else lstrcatA(mpx_path, ".mpx");

                        FILE *fp = fopen(mpx_path, "wb");
                        if (fp) {
                            fprintf(fp, "MPX1\r\n");
                            for (int h = 0; h < mdw_mpx_header_count; h++)
                                fprintf(fp, "%s\r\n", mdw_mpx_header_lines[h]);
                            fprintf(fp, "---\r\n");
                            for (int s = 0; s < mdw_mpx_parsed_n; s++) {
                                MdwMpParsedStep *ps = &mdw_mpx_parsed[s];
                                MdwMpxCapture *cap = &mdw_mpx_caps[0][s];
                                fprintf(fp, "%08X:%04X:%s", ps->cksum, ps->flags, ps->dir);
                                fprintf(fp, "|map=%u|room=%u", cap->map, cap->room);
                                uint32_t ri = mdw_room_lookup(cap->map, cap->room);
                                if (ri < mdw_room_count) {
                                    MdwRoom *rm = &mdw_rooms[ri];
                                    fprintf(fp, "|name=%s|illu=%d", rm->name, (int)rm->light);
                                    if (rm->lair_count) {
                                        fprintf(fp, "|lair=%u", (unsigned)rm->lair_count);
                                        char mobs[256];
                                        mdw_lair_monsters_str(rm, mobs, sizeof mobs);
                                        if (mobs[0]) fprintf(fp, "|mobs=%s", mobs);
                                    }
                                    if (rm->npc) {
                                        char nm[80]; mdw_npc_name(rm->npc, nm, 80);
                                        fprintf(fp, "|npc=%s", nm[0] ? nm : "?");
                                    }
                                    if (rm->shop) {
                                        char nm[80]; mdw_shop_name(rm->shop, nm, 80);
                                        fprintf(fp, "|shop=%s", nm[0] ? nm : "?");
                                        fprintf(fp, "|shoptype=%d", mdw_shop_type_get(rm->shop));
                                    }
                                    if (rm->spell) fprintf(fp, "|spell=%u", (unsigned)rm->spell);
                                    if (rm->cmd) fprintf(fp, "|cmd=%u", (unsigned)rm->cmd);
                                }
                                fprintf(fp, "\r\n");
                            }
                            fclose(fp);
                            mdw_log("[MPX] Written: %s\n", mpx_path);
                            wsprintfA(mdw_mpx_status, "MPX verified and saved!");
                        } else {
                            wsprintfA(mdw_mpx_status, "MPX verify OK but write failed");
                        }
                        mdw_mpx_verify_active = 0;
                    } else {
                        mdw_log("[MPX] Runs did NOT match — restarting 3 runs\n");
                        mdw_mpx_verify_run = 0;
                        mdw_mpx_verify_step = 0;
                        wsprintfA(mdw_mpx_status, "MPX runs mismatched — restarting");
                    }
                    mdw_mpx_status_tick = GetTickCount();
                } else {
                    wsprintfA(mdw_mpx_status, "MPX verify: run %d/3 starting...", mdw_mpx_verify_run + 1);
                    mdw_mpx_status_tick = GetTickCount();
                }
            } else {
                wsprintfA(mdw_mpx_status, "MPX run %d/3: step %d/%d",
                          run + 1, mdw_mpx_verify_step, mdw_mpx_parsed_n);
                mdw_mpx_status_tick = GetTickCount();
            }
        }
    }
}

static void mdw_mpx_verify_stop(void)
{
    mdw_mpx_verify_active = 0;
    for (int r = 0; r < 3; r++) {
        if (mdw_mpx_caps[r]) { free(mdw_mpx_caps[r]); mdw_mpx_caps[r] = NULL; }
    }
    if (mdw_mpx_parsed) { free(mdw_mpx_parsed); mdw_mpx_parsed = NULL; }
    mdw_mpx_parsed_n = 0;
    wsprintfA(mdw_mpx_status, "MPX verify cancelled");
    mdw_mpx_status_tick = GetTickCount();
    mdw_log("[MPX] Verify cancelled by user\n");
}

/* ---- Auto-MPX: scan + feed ---- */

static int mdw_auto_mpx_parse_one(const char *fullpath, MdwAutoMpx *out)
{
    FILE *f = fopen(fullpath, "rb");
    if (!f) return 0;
    char line[512];
    int header_lines = 0;
    out->header_count = 0;
    out->start_cksum[0] = 0;
    out->end_cksum[0] = 0;
    out->step_count = 0;
    int cap = 0;
    out->step_cksums = NULL;
    out->step_flags = NULL;
    out->step_dirs = NULL;

    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\r'); if (nl) *nl = 0;
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0]) continue;

        if (line[0] == '[' || header_lines < 3) {
            if (out->header_count < 4)
                lstrcpynA(out->header_lines[out->header_count++], line, 256);
            header_lines++;
            if (strlen(line) >= 17 && line[8] == ':' && line[17] == ':') {
                memcpy(out->start_cksum, line, 8); out->start_cksum[8] = 0;
                memcpy(out->end_cksum, line + 9, 8); out->end_cksum[8] = 0;
            }
            continue;
        }
        if (strlen(line) < 14 || line[8] != ':') continue;
        if (out->step_count >= cap) {
            cap = cap ? cap * 2 : 64;
            out->step_cksums = (uint32_t *)realloc(out->step_cksums, cap * sizeof(uint32_t));
            out->step_flags  = (uint16_t *)realloc(out->step_flags, cap * sizeof(uint16_t));
            out->step_dirs   = realloc(out->step_dirs, cap * 64);
        }
        char hex[9]; memcpy(hex, line, 8); hex[8] = 0;
        out->step_cksums[out->step_count] = (uint32_t)strtoul(hex, NULL, 16);
        memcpy(hex, line + 9, 4); hex[4] = 0;
        out->step_flags[out->step_count] = (uint16_t)strtoul(hex, NULL, 16);
        lstrcpynA(out->step_dirs[out->step_count], line + 14, 64);
        out->step_count++;
    }
    fclose(f);
    return out->step_count > 0;
}

static void mdw_auto_mpx_scan(void)
{
    if (mdw_auto_mpx_scanned) return;
    mdw_auto_mpx_scanned = 1;
    mdw_auto_mpx_count = 0;
    mdw_find_megamud_dir();

    char search[MAX_PATH];
    wsprintfA(search, "%sDefault\\*.mp", mdw_megamud_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA(search, &fd);
    if (hf == INVALID_HANDLE_VALUE) return;

    int loaded = 0, skipped = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int nlen = (int)lstrlenA(fd.cFileName);
        if (nlen < 4) continue;
        /* Skip non-.mp or .mpx files */
        if (lstrcmpiA(fd.cFileName + nlen - 3, ".mp") != 0) continue;
        if (lstrcmpiA(fd.cFileName + nlen - 4, ".mpx") == 0) continue;

        /* Check if .mpx already exists */
        char mpx_name[MAX_PATH];
        wsprintfA(mpx_name, "%sDefault\\%s", mdw_megamud_dir, fd.cFileName);
        char *dot = strrchr(mpx_name, '.');
        if (dot) lstrcpyA(dot, ".mpx");
        DWORD attr = GetFileAttributesA(mpx_name);
        if (attr != INVALID_FILE_ATTRIBUTES) { skipped++; continue; }

        if (mdw_auto_mpx_count >= MDW_AUTO_MPX_MAX) break;

        MdwAutoMpx *am = &mdw_auto_mpx[mdw_auto_mpx_count];
        memset(am, 0, sizeof(*am));
        lstrcpynA(am->filename, fd.cFileName, 64);
        wsprintfA(am->mp_fullpath, "%sDefault\\%s", mdw_megamud_dir, fd.cFileName);

        if (mdw_auto_mpx_parse_one(am->mp_fullpath, am)) {
            for (int r = 0; r < MDW_AUTO_MPX_RUNS; r++)
                am->caps[r] = (MdwMpxCapture *)calloc(am->step_count, sizeof(MdwMpxCapture));
            am->active = 0;
            am->run = 0;
            am->step = 0;
            mdw_auto_mpx_count++;
            loaded++;
        }
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    mdw_log("[MPX-AUTO] Scanned Default/: %d .mp files loaded, %d skipped (have .mpx)\n",
            loaded, skipped);
}

static void mdw_auto_mpx_write(MdwAutoMpx *am)
{
    char mpx_path[MAX_PATH];
    lstrcpynA(mpx_path, am->mp_fullpath, MAX_PATH);
    char *dot = strrchr(mpx_path, '.');
    if (dot) lstrcpyA(dot, ".mpx");
    else lstrcatA(mpx_path, ".mpx");

    FILE *fp = fopen(mpx_path, "wb");
    if (!fp) {
        mdw_log("[MPX-AUTO] ERROR: cannot write %s\n", mpx_path);
        return;
    }
    fprintf(fp, "MPX1\r\n");
    for (int h = 0; h < am->header_count; h++)
        fprintf(fp, "%s\r\n", am->header_lines[h]);
    fprintf(fp, "---\r\n");

    for (int s = 0; s < am->step_count; s++) {
        MdwMpxCapture *cap = &am->caps[0][s];
        fprintf(fp, "%08X:%04X:%s", am->step_cksums[s], am->step_flags[s], am->step_dirs[s]);
        fprintf(fp, "|map=%u|room=%u", cap->map, cap->room);
        uint32_t ri = mdw_room_lookup(cap->map, cap->room);
        if (ri < mdw_room_count) {
            MdwRoom *rm = &mdw_rooms[ri];
            fprintf(fp, "|name=%s|illu=%d", rm->name, (int)rm->light);
            if (rm->lair_count) {
                fprintf(fp, "|lair=%u", (unsigned)rm->lair_count);
                char mobs[256];
                mdw_lair_monsters_str(rm, mobs, sizeof mobs);
                if (mobs[0]) fprintf(fp, "|mobs=%s", mobs);
            }
            if (rm->npc) {
                char nm[80]; mdw_npc_name(rm->npc, nm, 80);
                fprintf(fp, "|npc=%s", nm[0] ? nm : "?");
            }
            if (rm->shop) {
                char nm[80]; mdw_shop_name(rm->shop, nm, 80);
                fprintf(fp, "|shop=%s", nm[0] ? nm : "?");
                fprintf(fp, "|shoptype=%d", mdw_shop_type_get(rm->shop));
            }
            if (rm->spell) fprintf(fp, "|spell=%u", (unsigned)rm->spell);
            if (rm->cmd) fprintf(fp, "|cmd=%u", (unsigned)rm->cmd);
        }
        fprintf(fp, "\r\n");
    }
    fclose(fp);
    mdw_log("[MPX-AUTO] Written: %s (%d steps verified over 3 runs)\n",
            mpx_path, am->step_count);
}

static void mdw_auto_mpx_feed(uint16_t map, uint16_t room, uint32_t cksum)
{
    if (!mdw_rec_gen_mpx || !mdw_auto_mpx_scanned) return;

    for (int i = 0; i < mdw_auto_mpx_count; i++) {
        MdwAutoMpx *am = &mdw_auto_mpx[i];
        if (am->step_count == 0) continue;

        if (!am->active) {
            /* Check if this room matches step 0 — path might be starting */
            if (cksum == am->step_cksums[0]) {
                am->active = 1;
                am->step = 0;
                am->caps[am->run][0].map  = map;
                am->caps[am->run][0].room = room;
                am->step = 1;
                mdw_log("[MPX-AUTO] %s: run %d started (step 1/%d, map=%u room=%u)\n",
                        am->filename, am->run + 1, am->step_count, map, room);
            }
            continue;
        }

        /* Active — check if current room matches expected step */
        int s = am->step;
        if (s < am->step_count && cksum == am->step_cksums[s]) {
            am->caps[am->run][s].map  = map;
            am->caps[am->run][s].room = room;
            am->step++;
            mdw_log("[MPX-AUTO] %s: run %d step %d/%d (map=%u room=%u)\n",
                    am->filename, am->run + 1, am->step, am->step_count, map, room);

            if (am->step >= am->step_count) {
                mdw_log("[MPX-AUTO] %s: run %d/%d complete\n",
                        am->filename, am->run + 1, MDW_AUTO_MPX_RUNS);
                am->run++;
                am->step = 0;
                am->active = 0;  /* wait for next path start */

                if (am->run >= MDW_AUTO_MPX_RUNS) {
                    /* Compare all runs */
                    int match = 1;
                    for (int ss = 0; ss < am->step_count && match; ss++) {
                        for (int r = 1; r < MDW_AUTO_MPX_RUNS; r++) {
                            if (am->caps[r][ss].map  != am->caps[0][ss].map ||
                                am->caps[r][ss].room != am->caps[0][ss].room) {
                                mdw_log("[MPX-AUTO] %s: MISMATCH step %d run1=(%u,%u) run%d=(%u,%u)\n",
                                        am->filename, ss,
                                        am->caps[0][ss].map, am->caps[0][ss].room,
                                        r + 1, am->caps[r][ss].map, am->caps[r][ss].room);
                                match = 0;
                            }
                        }
                    }
                    if (match) {
                        mdw_log("[MPX-AUTO] %s: ALL %d RUNS MATCH — writing .mpx\n",
                                am->filename, MDW_AUTO_MPX_RUNS);
                        mdw_auto_mpx_write(am);
                        wsprintfA(mdw_mpx_status, "Auto-MPX: %s done!", am->filename);
                        mdw_mpx_status_tick = GetTickCount();
                        /* Remove from list (swap with last) */
                        free(am->step_cksums); free(am->step_flags); free(am->step_dirs);
                        for (int r = 0; r < MDW_AUTO_MPX_RUNS; r++) free(am->caps[r]);
                        if (i < mdw_auto_mpx_count - 1)
                            *am = mdw_auto_mpx[mdw_auto_mpx_count - 1];
                        mdw_auto_mpx_count--;
                        i--;  /* re-check this slot */
                    } else {
                        mdw_log("[MPX-AUTO] %s: runs mismatched, restarting\n", am->filename);
                        am->run = 0;
                        am->step = 0;
                    }
                }
            }
        } else if (am->active) {
            /* Checksum didn't match — path broken (got lost, teleported, etc.) */
            mdw_log("[MPX-AUTO] %s: step %d expected %08X got %08X — resetting run %d\n",
                    am->filename, s, am->step_cksums[s], cksum, am->run + 1);
            am->active = 0;
            am->step = 0;
            /* Don't reset run counter — partial progress on earlier runs is still valid.
             * Just restart this run from scratch. */
        }
    }
}

/* ---- Bottom-bar controls ---- */
static int mdw_use_rm = 1;                /* auto-track current room from OFF_ROOM_CHECKSUM */
static char mdw_map_buf[8]  = "";
static char mdw_room_buf[8] = "";
static int  mdw_input_focus = 0;          /* 0=none, 1=map, 2=room */
/* Hit rects for the bottom control strip (filled every frame by mdw_draw) */
static float mdw_ctrl_y0 = 0, mdw_ctrl_y1 = 0;
static float mdw_cb_use_rm_x0 = 0,  mdw_cb_use_rm_x1 = 0;
static float mdw_cb_auto_x0   = 0,  mdw_cb_auto_x1   = 0;
static float mdw_cb_noev_x0   = 0,  mdw_cb_noev_x1   = 0;
static char  mdw_walk_status[256] = "";
static DWORD mdw_walk_status_tick = 0;

static int      mdw_confirm_showing = 0;
static uint32_t mdw_confirm_dest_ri = 0xFFFFFFFF;
static float    mdw_confirm_x0, mdw_confirm_y0, mdw_confirm_x1, mdw_confirm_y1;
static float    mdw_confirm_btn_run_x0, mdw_confirm_btn_run_x1;
static float    mdw_confirm_btn_cancel_x0, mdw_confirm_btn_cancel_x1;
static float    mdw_confirm_btn_y0, mdw_confirm_btn_y1;
static float mdw_btn_zoom_out_x0 = 0, mdw_btn_zoom_out_x1 = 0;
static float mdw_btn_zoom_in_x0  = 0, mdw_btn_zoom_in_x1  = 0;
static float mdw_in_map_x0  = 0,  mdw_in_map_x1  = 0;
static float mdw_in_room_x0 = 0,  mdw_in_room_x1 = 0;
static float mdw_btn_go_x0  = 0,  mdw_btn_go_x1  = 0;


/* Directional vectors for layout/numpad */
static const int MDW_DX[10] = { 0, 0, 1,-1, 1,-1, 1,-1, 0, 0 };
static const int MDW_DY[10] = {-1, 1, 0, 0,-1,-1, 1, 1, 0, 0 };

/* ---- Helpers ---- */

static void mdw_hash_build(void)
{
    free(mdw_hash); mdw_hash = NULL;
    mdw_hash_cap = 1;
    while (mdw_hash_cap < mdw_room_count * 3) mdw_hash_cap <<= 1;
    if (mdw_hash_cap < 8) mdw_hash_cap = 8;
    mdw_hash = (MdwHkv *)calloc(mdw_hash_cap, sizeof(MdwHkv));
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        uint32_t k = ((uint32_t)mdw_rooms[i].map << 16) | mdw_rooms[i].room;
        if (k == 0) k = 0xFFFFFFFF;
        uint32_t h = (k * 2654435761u) & (mdw_hash_cap - 1);
        while (mdw_hash[h].key) h = (h + 1) & (mdw_hash_cap - 1);
        mdw_hash[h].key = k;
        mdw_hash[h].val = i + 1;
    }
}

static uint32_t mdw_room_lookup(uint16_t map, uint16_t room)
{
    if (!mdw_hash) return 0xFFFFFFFF;
    uint32_t k = ((uint32_t)map << 16) | room;
    if (k == 0) k = 0xFFFFFFFF;
    uint32_t h = (k * 2654435761u) & (mdw_hash_cap - 1);
    while (mdw_hash[h].key) {
        if (mdw_hash[h].key == k) return mdw_hash[h].val - 1;
        h = (h + 1) & (mdw_hash_cap - 1);
    }
    return 0xFFFFFFFF;
}

static void mdw_free(void)
{
    for (uint32_t i = 0; i < mdw_aux_count; i++) {
        MdwAuxTable *t = &mdw_aux[i];
        for (int c = 0; c < t->col_count; c++) free(t->col_names[c]);
        free(t->col_names);
        free(t->cells);
        free(t->blob);
    }
    free(mdw_aux); mdw_aux = NULL; mdw_aux_count = 0;
    mdw_shop_type_ready = 0;
    for (uint32_t i = 0; i < mdw_map_count; i++) free(mdw_maps[i].indices);
    free(mdw_maps); mdw_maps = NULL; mdw_map_count = 0;
    free(mdw_exits); mdw_exits = NULL; mdw_exit_count = 0;
    for (uint32_t i = 0; i < mdw_room_count; i++) free(mdw_rooms[i].lair_raw);
    free(mdw_rooms); mdw_rooms = NULL; mdw_room_count = 0;
    free(mdw_hash); mdw_hash = NULL; mdw_hash_cap = 0;
    mdw_loaded = 0;
    mdw_cur_ri = 0xFFFFFFFF;
}

static int mdw_load_bin(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    mdw_free();
    uint32_t magic, version, rc, mc;
    if (fread(&magic, 4, 1, f) != 1 ||
        fread(&version, 4, 1, f) != 1 ||
        fread(&rc, 4, 1, f) != 1 ||
        fread(&mc, 4, 1, f) != 1 ||
        magic != 0x44554D4D ||
        version < 1 || version > 5)
    { fclose(f); return -1; }
    mdw_room_count = rc;
    mdw_map_count = mc;
    mdw_rooms = (MdwRoom *)calloc(rc, sizeof(MdwRoom));

    long rooms_start = ftell(f);
    uint32_t total_exits = 0;
    for (uint32_t i = 0; i < rc; i++) {
        uint16_t hdr[6]; fread(hdr, 2, 6, f);
        uint8_t lair, ec, nl; int8_t light;
        fread(&lair, 1, 1, f); fread(&light, 1, 1, f);
        fread(&ec, 1, 1, f);   fread(&nl, 1, 1, f);
        fseek(f, nl, SEEK_CUR);
        fseek(f, 4, SEEK_CUR); /* checksum */
        if (version >= 5) {
            for (uint8_t e = 0; e < ec; e++) {
                fseek(f, 13, SEEK_CUR); /* dir+flags+tmap+troom+key+pick+bash */
                uint8_t tcl = 0; fread(&tcl, 1, 1, f);
                fseek(f, tcl, SEEK_CUR);
            }
        } else {
            fseek(f, ec * 9, SEEK_CUR);
        }
        total_exits += ec;
        if (version >= 4) {
            uint16_t lrl = 0;
            fread(&lrl, 2, 1, f);
            fseek(f, lrl, SEEK_CUR);
        }
    }
    mdw_exit_count = total_exits;
    mdw_exits = (MdwExit *)calloc(total_exits, sizeof(MdwExit));

    fseek(f, rooms_start, SEEK_SET);
    uint32_t eo = 0;
    for (uint32_t i = 0; i < rc; i++) {
        MdwRoom *r = &mdw_rooms[i];
        uint16_t hdr[6]; fread(hdr, 2, 6, f);
        r->map = hdr[0]; r->room = hdr[1];
        r->npc = hdr[2]; r->shop = hdr[3];
        r->spell = hdr[4]; r->cmd = hdr[5];
        uint8_t nl;
        fread(&r->lair_count, 1, 1, f);
        fread(&r->light, 1, 1, f);
        fread(&r->exit_count, 1, 1, f);
        fread(&nl, 1, 1, f);
        int n = nl < 63 ? nl : 63;
        fread(r->name, 1, nl, f); r->name[n] = 0;
        fread(&r->checksum, 4, 1, f);
        r->exit_ofs = eo;
        r->gx = r->gy = INT16_MIN;
        for (uint32_t e = 0; e < r->exit_count; e++) {
            MdwExit *ex = &mdw_exits[eo++];
            fread(&ex->dir, 1, 1, f);
            fread(&ex->flags, 2, 1, f);
            fread(&ex->tmap, 2, 1, f);
            fread(&ex->troom, 2, 1, f);
            fread(&ex->key_item, 2, 1, f);
            ex->pick_req = 0; ex->bash_req = 0;
            ex->text_cmd[0] = 0;
            if (version >= 5) {
                fread(&ex->pick_req, 2, 1, f);
                fread(&ex->bash_req, 2, 1, f);
                uint8_t tcl = 0; fread(&tcl, 1, 1, f);
                if (tcl > 0) {
                    int tn = tcl < 63 ? tcl : 63;
                    fread(ex->text_cmd, 1, tcl, f);
                    ex->text_cmd[tn] = 0;
                }
            }
        }
        r->lair_raw = NULL;
        if (version >= 4) {
            uint16_t lrl = 0;
            fread(&lrl, 2, 1, f);
            if (lrl > 0) {
                r->lair_raw = (char *)malloc(lrl + 1);
                if (r->lair_raw) {
                    fread(r->lair_raw, 1, lrl, f);
                    r->lair_raw[lrl] = 0;
                } else {
                    fseek(f, lrl, SEEK_CUR);
                }
            }
        }
    }
    mdw_maps = (MdwMapInfo *)calloc(mc, sizeof(MdwMapInfo));
    for (uint32_t i = 0; i < mc; i++) {
        uint16_t mnum; uint32_t rcnt;
        fread(&mnum, 2, 1, f); fread(&rcnt, 4, 1, f);
        mdw_maps[i].map = mnum; mdw_maps[i].count = rcnt;
        mdw_maps[i].indices = (uint32_t *)malloc(rcnt * sizeof(uint32_t));
        for (uint32_t j = 0; j < rcnt; j++) {
            uint16_t rn, idx;
            fread(&rn, 2, 1, f); fread(&idx, 2, 1, f);
            mdw_maps[i].indices[j] = idx;
        }
    }
    if (version >= 2) {
        uint32_t tc = 0;
        if (fread(&tc, 4, 1, f) == 1 && tc > 0 && tc < 1024) {
            mdw_aux_count = tc;
            mdw_aux = (MdwAuxTable *)calloc(tc, sizeof(MdwAuxTable));
            for (uint32_t ti = 0; ti < tc; ti++) {
                MdwAuxTable *t = &mdw_aux[ti];
                uint8_t nl; fread(&nl, 1, 1, f);
                int nn = nl < 63 ? nl : 63;
                fread(t->name, 1, nl, f); t->name[nn] = 0;
                fread(&t->col_count, 1, 1, f);
                t->col_names = (char **)calloc(t->col_count, sizeof(char *));
                for (int ci = 0; ci < t->col_count; ci++) {
                    uint8_t cl; fread(&cl, 1, 1, f);
                    char *s = (char *)malloc(cl + 1);
                    fread(s, 1, cl, f); s[cl] = 0;
                    t->col_names[ci] = s;
                }
                fread(&t->row_count, 4, 1, f);
                size_t total_cells = (size_t)t->row_count * t->col_count;
                t->cells = (MdwAuxCell *)calloc(total_cells, sizeof(MdwAuxCell));
                long cells_start = ftell(f);
                size_t blob_bytes = 0;
                for (size_t k = 0; k < total_cells; k++) {
                    uint16_t len; fread(&len, 2, 1, f);
                    if (len != 0xFFFF) { blob_bytes += len; fseek(f, len, SEEK_CUR); }
                }
                fseek(f, cells_start, SEEK_SET);
                t->blob = (char *)malloc(blob_bytes + 1);
                size_t off = 0;
                for (size_t k = 0; k < total_cells; k++) {
                    uint16_t len; fread(&len, 2, 1, f);
                    t->cells[k].len = len;
                    if (len == 0xFFFF) { t->cells[k].data = NULL; }
                    else { t->cells[k].data = t->blob + off;
                           fread(t->blob + off, 1, len, f); off += len; }
                }
            }
        }
    }
    fclose(f);
    mdw_hash_build();
    mdw_loaded = 1;
    /* Trigger auto-MPX scan now that bin data is loaded */
    mdw_auto_mpx_scanned = 0;
    if (mdw_rec_gen_mpx) mdw_auto_mpx_scan();
    return 0;
}

/* BFS layout on the current view map */
#define MDW_GRID_DIM 50
static void mdw_layout_bfs(uint16_t start_map, uint32_t start)
{
    if (start >= mdw_room_count) return;
    uint8_t *used = (uint8_t *)calloc(MDW_GRID_DIM * MDW_GRID_DIM, 1);
    uint32_t *queue = (uint32_t *)malloc(mdw_room_count * sizeof(uint32_t));
    uint32_t qh = 0, qt = 0;
    int center = MDW_GRID_DIM / 2;
    mdw_rooms[start].gx = (int16_t)center;
    mdw_rooms[start].gy = (int16_t)center;
    used[center * MDW_GRID_DIM + center] = 1;
    queue[qt++] = start;
    while (qh < qt) {
        uint32_t ri = queue[qh++];
        MdwRoom *r = &mdw_rooms[ri];
        for (uint32_t e = 0; e < r->exit_count; e++) {
            MdwExit *ex = &mdw_exits[r->exit_ofs + e];
            if (ex->dir > 7) continue;
            if (ex->tmap != start_map) continue;
            uint32_t ni = mdw_room_lookup(ex->tmap, ex->troom);
            if (ni == 0xFFFFFFFF) continue;
            if (mdw_rooms[ni].gx != INT16_MIN) continue;
            int nx = r->gx + MDW_DX[ex->dir];
            int ny = r->gy + MDW_DY[ex->dir];
            if (nx < 0 || nx >= MDW_GRID_DIM || ny < 0 || ny >= MDW_GRID_DIM) continue;
            if (used[ny * MDW_GRID_DIM + nx]) {
                int placed = 0;
                for (int rr = 1; rr <= 3 && !placed; rr++) {
                    for (int oy = -rr; oy <= rr && !placed; oy++) {
                        for (int ox = -rr; ox <= rr && !placed; ox++) {
                            int tx = nx + ox, ty = ny + oy;
                            if (tx < 0 || tx >= MDW_GRID_DIM) continue;
                            if (ty < 0 || ty >= MDW_GRID_DIM) continue;
                            if (used[ty * MDW_GRID_DIM + tx]) continue;
                            nx = tx; ny = ty; placed = 1;
                        }
                    }
                }
                if (!placed) continue;
            }
            mdw_rooms[ni].gx = (int16_t)nx;
            mdw_rooms[ni].gy = (int16_t)ny;
            used[ny * MDW_GRID_DIM + nx] = 1;
            queue[qt++] = ni;
        }
    }
    free(queue); free(used);
}

static void mdw_relayout(uint16_t map_num)
{
    if (!mdw_loaded) return;
    for (uint32_t i = 0; i < mdw_room_count; i++)
        mdw_rooms[i].gx = mdw_rooms[i].gy = INT16_MIN;
    /* Find start room = first room listed in this map's index */
    uint32_t start = 0xFFFFFFFF;
    for (uint32_t i = 0; i < mdw_map_count; i++)
        if (mdw_maps[i].map == map_num && mdw_maps[i].count > 0) {
            start = mdw_maps[i].indices[0];
            break;
        }
    if (start == 0xFFFFFFFF) return;
    mdw_layout_bfs(map_num, start);
    mdw_cur_map_view = map_num;
    /* Center on the start room */
    mdw_view_x = mdw_rooms[start].gx * 20.0f;
    mdw_view_y = mdw_rooms[start].gy * 20.0f;
}

/* Jump view/current-room to (map, room). Returns 1 if that room exists in
 * the loaded bin. Does NOT change use_rm or touch input state — callers
 * wrap this for manual vs auto behavior. */
static int mdw_set_current(int map_num, int room_num)
{
    if (!mdw_loaded) return 0;
    uint32_t ri = mdw_room_lookup((uint16_t)map_num, (uint16_t)room_num);
    if (ri == 0xFFFFFFFF) return 0;
    int changed = (mdw_cur_ri != ri);
    /* Feed MPX verifiers on every room change */
    if (changed) {
        uint32_t ck = mdw_rooms[ri].checksum;
        if (mdw_mpx_verify_active)
            mdw_mpx_verify_feed((uint16_t)map_num, (uint16_t)room_num, ck);
        /* Auto-MPX: passively watch all room transitions */
        mdw_auto_mpx_feed((uint16_t)map_num, (uint16_t)room_num, ck);
    }
    int map_switched = 0;
    if (mdw_rooms[ri].map != mdw_cur_map_view || mdw_rooms[ri].gx == INT16_MIN) {
        for (uint32_t k = 0; k < mdw_room_count; k++)
            mdw_rooms[k].gx = mdw_rooms[k].gy = INT16_MIN;
        mdw_layout_bfs(mdw_rooms[ri].map, ri);
        mdw_cur_map_view = mdw_rooms[ri].map;
        changed = 1;
        map_switched = 1;
    }
    mdw_cur_ri = ri;
    if (changed) {
        mdw_pulse_tick = GetTickCount();
        /* Recenter on same-map moves only if auto is on; on a map switch
         * recenter unconditionally so the user isn't staring at blank
         * space in coords that belong to a different map. */
        if (mdw_rooms[ri].gx != INT16_MIN && (mdw_auto_recenter || map_switched)) {
            if (mdw_hover_ri != 0xFFFFFFFF) {
                /* Hovering — queue it, apply on the frame hover clears. */
                mdw_pending_recenter_ri = ri;
            } else {
                mdw_view_x = mdw_rooms[ri].gx * 20.0f;
                mdw_view_y = mdw_rooms[ri].gy * 20.0f;
                mdw_pending_recenter_ri = 0xFFFFFFFF;
            }
        }
    }
    return 1;
}

/* Manual "Set Room" button target: jump and pin the view (turns off RM
 * auto-tracking so the manual setting isn't clobbered next frame). */
static int mdw_goto_room(int map_num, int room_num)
{
    if (!mdw_set_current(map_num, room_num)) return 0;
    mdw_use_rm = 0;
    mdw_map_buf[0] = 0;
    mdw_room_buf[0] = 0;
    mdw_input_focus = 0;
    return 1;
}

/* ================================================================
 * Dynamic pathfinding — BFS across the room/exit graph
 * ================================================================ */

#define MDW_DYNPATH_MAX_STEPS 500
#define MDW_DYNPATH_MAX_ROOMS 65536
#define MDW_DYNPATH_MAX_REQS  16

typedef struct {
    uint32_t cksum;
    uint16_t flags;
    char     dir[32];
    uint16_t map, room;
} MdwDynStep;

typedef struct {
    uint16_t id;
    char     name[64];
} MdwDynReq;

typedef struct {
    MdwDynStep *steps;
    int         count;
    int         error;
    char        errmsg[256];
    int         req_count;
    MdwDynReq   reqs[MDW_DYNPATH_MAX_REQS];
} MdwDynPath;

#define MDW_DPERR_NONE      0
#define MDW_DPERR_NO_PATH   1
#define MDW_DPERR_BLOCKED   2
#define MDW_DPERR_NOT_LOADED 3
#define MDW_DPERR_SAME_ROOM 4
#define MDW_DPERR_BAD_ROOM  5

static MdwDynPath mdw_dynpath_result;

static void mdw_dynpath_free(void)
{
    if (mdw_dynpath_result.steps) {
        free(mdw_dynpath_result.steps);
        mdw_dynpath_result.steps = NULL;
    }
    mdw_dynpath_result.count = 0;
    mdw_dynpath_result.error = MDW_DPERR_NONE;
    mdw_dynpath_result.errmsg[0] = 0;
    mdw_dynpath_result.req_count = 0;
}

static MdwDynPath *mdw_find_path(uint32_t from_ri, uint32_t to_ri)
{
    mdw_dynpath_free();
    MdwDynPath *dp = &mdw_dynpath_result;

    if (!mdw_loaded || !mdw_rooms || !mdw_exits) {
        dp->error = MDW_DPERR_NOT_LOADED;
        lstrcpyA(dp->errmsg, "Map data not loaded");
        return dp;
    }
    if (from_ri >= mdw_room_count || to_ri >= mdw_room_count) {
        dp->error = MDW_DPERR_BAD_ROOM;
        wsprintfA(dp->errmsg, "Invalid room index (from=%u to=%u, max=%u)",
                  from_ri, to_ri, mdw_room_count);
        return dp;
    }
    if (from_ri == to_ri) {
        dp->error = MDW_DPERR_SAME_ROOM;
        lstrcpyA(dp->errmsg, "Already in that room");
        return dp;
    }

    uint32_t cap = mdw_room_count;
    if (cap > MDW_DYNPATH_MAX_ROOMS) cap = MDW_DYNPATH_MAX_ROOMS;

    uint32_t *prev = (uint32_t *)calloc(cap, sizeof(uint32_t));
    uint8_t  *exit_idx = (uint8_t *)calloc(cap, sizeof(uint8_t));
    uint8_t  *visited = (uint8_t *)calloc(cap, sizeof(uint8_t));
    uint32_t *queue = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!prev || !exit_idx || !visited || !queue) {
        dp->error = MDW_DPERR_NO_PATH;
        lstrcpyA(dp->errmsg, "Out of memory for pathfinding");
        goto cleanup;
    }

    memset(prev, 0xFF, cap * sizeof(uint32_t));
    uint32_t qh = 0, qt = 0;

    if (from_ri < cap) {
        visited[from_ri] = 1;
        queue[qt++] = from_ri;
    }

    uint16_t target_map = mdw_rooms[to_ri].map;
    int found = 0;

    int player_str = pfn_get_player_strength ? pfn_get_player_strength() : 0;
    int player_plk = pfn_get_player_picklocks ? pfn_get_player_picklocks() : 0;

    while (qh < qt && !found) {
        uint32_t cur = queue[qh++];
        MdwRoom *cr = &mdw_rooms[cur];

        for (int i = 0; i < cr->exit_count; i++) {
            MdwExit *ex = &mdw_exits[cr->exit_ofs + i];
            uint32_t ni = mdw_room_lookup(ex->tmap, ex->troom);
            if (ni >= cap || ni == 0xFFFFFFFF) continue;
            if (visited[ni]) continue;

            /* CAST exits remain blocked */
            if (ex->flags & MDW_EX_CAST) {
                char spl[64];
                mdw_spell_name(ex->key_item, spl, sizeof spl);
                if (spl[0]) continue;
            }

            /* DOOR exits: check if player can bash or pick.
               If pick_req/bash_req are set and player doesn't meet either, skip. */
            if ((ex->flags & MDW_EX_DOOR) && !(ex->flags & MDW_EX_KEY)) {
                if (ex->pick_req > 1 || ex->bash_req > 1) {
                    int can_pick = (ex->pick_req > 0 && player_plk >= ex->pick_req);
                    int can_bash = (ex->bash_req > 0 && player_str >= ex->bash_req);
                    if (!can_pick && !can_bash) continue;
                }
            }

            visited[ni] = 1;
            prev[ni] = cur;
            exit_idx[ni] = (uint8_t)i;

            if (ni == to_ri) { found = 1; break; }
            if (qt < cap) queue[qt++] = ni;
        }
    }

    if (!found) {
        /* Try to find why — is destination on a different map? */
        MdwRoom *fr = &mdw_rooms[from_ri];
        MdwRoom *tr = &mdw_rooms[to_ri];
        int blocked = 0;
        if (fr->map != tr->map) {
            wsprintfA(dp->errmsg, "Cannot path across maps (Map %d -> Map %d). "
                      "No connecting exit found.", fr->map, tr->map);
        } else {
            /* Check if any exit leads toward it but is blocked */
            for (uint32_t ri = 0; ri < cap; ri++) {
                if (!visited[ri]) continue;
                MdwRoom *cr = &mdw_rooms[ri];
                for (int i = 0; i < cr->exit_count; i++) {
                    MdwExit *ex = &mdw_exits[cr->exit_ofs + i];
                    uint32_t ni = mdw_room_lookup(ex->tmap, ex->troom);
                    if (ni == 0xFFFFFFFF || ni >= cap) continue;
                    if (visited[ni]) continue;

                    if (ex->flags & MDW_EX_CAST) {
                        char spl[64];
                        mdw_spell_name(ex->key_item, spl, sizeof spl);
                        wsprintfA(dp->errmsg,
                                  "Cannot advance past Map %d Room %d (%s) — "
                                  "requires spell: %s",
                                  cr->map, cr->room, cr->name,
                                  spl[0] ? spl : "unknown spell");
                        blocked = 1; break;
                    }
                }
                if (blocked) break;
            }
            if (!blocked) {
                wsprintfA(dp->errmsg, "No path from Map %d Room %d (%s) to "
                          "Map %d Room %d (%s) — rooms are not connected",
                          fr->map, fr->room, fr->name,
                          tr->map, tr->room, tr->name);
            }
        }
        dp->error = blocked ? MDW_DPERR_BLOCKED : MDW_DPERR_NO_PATH;
        goto cleanup;
    }

    /* Trace back from destination to source */
    int path_len = 0;
    {
        uint32_t ri = to_ri;
        while (ri != from_ri && ri != 0xFFFFFFFF && path_len < MDW_DYNPATH_MAX_STEPS) {
            path_len++;
            ri = prev[ri];
        }
    }
    if (path_len <= 0 || path_len > MDW_DYNPATH_MAX_STEPS) {
        dp->error = MDW_DPERR_NO_PATH;
        wsprintfA(dp->errmsg, "Path too long (%d steps, max %d)",
                  path_len, MDW_DYNPATH_MAX_STEPS);
        goto cleanup;
    }

    dp->steps = (MdwDynStep *)calloc(path_len, sizeof(MdwDynStep));
    if (!dp->steps) {
        dp->error = MDW_DPERR_NO_PATH;
        lstrcpyA(dp->errmsg, "Out of memory allocating path steps");
        goto cleanup;
    }
    dp->count = path_len;

    /* Fill steps in reverse (from dest back to src) and collect required items */
    dp->req_count = 0;
    {
        uint32_t ri = to_ri;
        for (int si = path_len - 1; si >= 0; si--) {
            uint32_t pri = prev[ri];
            MdwRoom *pr = &mdw_rooms[pri];
            MdwExit *ex = &mdw_exits[pr->exit_ofs + exit_idx[ri]];
            dp->steps[si].cksum = pr->checksum;
            dp->steps[si].flags = ex->flags & 0x1FFF;
            dp->steps[si].map   = pr->map;
            dp->steps[si].room  = pr->room;
            const char *stepdir = (ex->text_cmd[0])
                ? ex->text_cmd
                : ((ex->dir <= 9) ? mdw_dir_lo[ex->dir] : "look");
            strncpy(dp->steps[si].dir, stepdir, sizeof dp->steps[si].dir - 1);
            dp->steps[si].dir[sizeof dp->steps[si].dir - 1] = 0;

            if ((ex->flags & MDW_EX_KEY) && ex->key_item &&
                dp->req_count < MDW_DYNPATH_MAX_REQS) {
                int dup = 0;
                for (int k = 0; k < dp->req_count; k++)
                    if (dp->reqs[k].id == ex->key_item) { dup = 1; break; }
                if (!dup) {
                    dp->reqs[dp->req_count].id = ex->key_item;
                    mdw_item_name(ex->key_item,
                                  dp->reqs[dp->req_count].name,
                                  sizeof dp->reqs[dp->req_count].name);
                    dp->req_count++;
                }
            }
            ri = pri;
        }
    }

cleanup:
    if (prev) free(prev);
    if (exit_idx) free(exit_idx);
    if (visited) free(visited);
    if (queue) free(queue);
    return dp;
}

static int mdw_bin_exists(void)
{
    FILE *f = fopen("mudmap/mmud.bin", "rb");
    if (f) { fclose(f); return 1; }
    f = fopen("mmud.bin", "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static int mdw_try_load(void)
{
    if (mdw_load_bin("mudmap/mmud.bin") == 0) return 0;
    if (mdw_load_bin("mmud.bin") == 0) return 0;
    return -1;
}

/* Legacy checksum-based tracking. DO NOT USE — Rooms.md checksums are NOT
 * unique across the map (identical name + identical exit signature = same
 * checksum), so matching by checksum silently teleports to the first dup
 * in bin order. Use mdw_set_current(map, room) instead, driven by the
 * actual map/room parsed from the "rm" command output. Kept as a no-op
 * shim for source compatibility. */
static void mdw_pulse_on_room(uint32_t cksum) { (void)cksum; }

static int mdw_has_focus(void) { return mdw_visible && mdw_focused && mdw_loaded; }

/* ---- Rendering ---- */

/* Draw a 3D rounded-corner tile centered at (cx,cy) with half-size hs (in
 * window pixels). The tile has:
 *   - beveled body (darker base + highlight diagonal)
 *   - corner rounding via dark-bg overdraw in the 4 corners
 *   - top gloss highlight
 *   - bottom shadow
 *   - bright 1-pixel outer border
 *   - optional glow halo controlled by `glow_alpha` (outer pulsing ring)
 *
 * Colors `r,g,b` are the base fill. `or,og,ob` is the outer ring color.
 * Clips against [cx0..cx1, cy0..cy1]. */
static void mdw_tile_3d(float cx, float cy, float hs,
                        float r, float g, float b,
                        float or_, float og, float ob,
                        float bgr, float bgg, float bgb,
                        float glow_alpha,
                        float cx0, float cy0, float cx1, float cy1,
                        int vp_w, int vp_h)
{
    (void)bgr; (void)bgg; (void)bgb;
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;

    /* Trivial clip */
    float x0 = cx - hs, y0 = cy - hs, x1 = cx + hs, y1 = cy + hs;
    if (x1 < cx0 || y1 < cy0 || x0 > cx1 || y0 > cy1) return;

    /* Soft pulsing glow for current room ONLY — single quad, no bloom rings
     * (they caused shimmer/flicker when overlapping adjacent tiles). */
    if (glow_alpha > 0.01f) {
        float ring = hs + 3.0f;
        float gx0 = cx - ring, gy0 = cy - ring;
        float gx1 = cx + ring, gy1 = cy + ring;
        if (gx0 < cx0) gx0 = cx0; if (gy0 < cy0) gy0 = cy0;
        if (gx1 > cx1) gx1 = cx1; if (gy1 > cy1) gy1 = cy1;
        psolid(gx0, gy0, gx1, gy1,
               r * 1.5f > 1.0f ? 1.0f : r * 1.5f,
               g * 1.5f > 1.0f ? 1.0f : g * 1.5f,
               b * 1.5f > 1.0f ? 1.0f : b * 1.5f,
               glow_alpha * 0.45f, vp_w, vp_h);
    }

    /* Clip box itself */
    if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return;

    /* Base fill */
    psolid(x0, y0, x1, y1, r, g, b, 1.0f, vp_w, vp_h);

    /* Smooth/beveled look — only applied once the tile is large enough
     * that the overdraw can't flicker at the sub-pixel level. At tiny
     * zoom the tile stays a flat fill (keeps framerate and avoids the
     * shimmer the old bevel pass had). */
    if (hs >= 6.0f) {
        float h = y1 - y0;
        float w = x1 - x0;
        /* Top gloss: horizontal band covering top ~35% with a lighter tint */
        float gloss = h * 0.35f;
        float gx0 = x0, gy0 = y0, gx1 = x1, gy1 = y0 + gloss;
        if (gy1 > y1) gy1 = y1;
        float glr = r + (1.0f - r) * 0.35f;
        float glg = g + (1.0f - g) * 0.35f;
        float glb = b + (1.0f - b) * 0.35f;
        psolid(gx0, gy0, gx1, gy1, glr, glg, glb, 0.45f, vp_w, vp_h);

        /* Bottom shadow band */
        float shad = h * 0.28f;
        float sy0 = y1 - shad;
        if (sy0 < y0) sy0 = y0;
        psolid(x0, sy0, x1, y1, r * 0.35f, g * 0.35f, b * 0.35f, 0.40f, vp_w, vp_h);

        /* 1-px top bright edge */
        psolid(x0, y0, x1, y0 + 1.0f,
               r + (1.0f - r) * 0.7f, g + (1.0f - g) * 0.7f, b + (1.0f - b) * 0.7f,
               0.9f, vp_w, vp_h);
        /* 1-px bottom dark edge */
        psolid(x0, y1 - 1.0f, x1, y1, r * 0.25f, g * 0.25f, b * 0.25f, 0.9f, vp_w, vp_h);
        /* 1-px left light / right shadow — skip if tile is narrow */
        if (w >= 10.0f) {
            psolid(x0, y0, x0 + 1.0f, y1,
                   r + (1.0f - r) * 0.45f, g + (1.0f - g) * 0.45f, b + (1.0f - b) * 0.45f,
                   0.55f, vp_w, vp_h);
            psolid(x1 - 1.0f, y0, x1, y1, r * 0.4f, g * 0.4f, b * 0.4f, 0.55f, vp_w, vp_h);
        }

        /* Corner chamfers (fake rounded corners): punch the panel bg into
         * each corner. Size scales with tile so it reads at all zooms. */
        float cn = hs * 0.18f;
        if (cn < 1.5f) cn = 1.5f;
        if (cn > 4.0f) cn = 4.0f;
        if (cn * 2.0f < w && cn * 2.0f < h) {
            psolid(x0, y0, x0 + cn, y0 + cn, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
            psolid(x1 - cn, y0, x1, y0 + cn, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
            psolid(x0, y1 - cn, x0 + cn, y1, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
            psolid(x1 - cn, y1 - cn, x1, y1, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
        }
    }

    /* 1-px outline only for the current room (bright accent). */
    if (glow_alpha > 0.01f) {
        psolid(x0, y0, x1, y0 + 1, or_, og, ob, 1.0f, vp_w, vp_h);
        psolid(x0, y1 - 1, x1, y1, or_, og, ob, 1.0f, vp_w, vp_h);
        psolid(x0, y0, x0 + 1, y1, or_, og, ob, 1.0f, vp_w, vp_h);
        psolid(x1 - 1, y0, x1, y1, or_, og, ob, 1.0f, vp_w, vp_h);
    }
}

/* Pixel skull: 4-6 quads, draws a recognizable skull even at tile sizes ~12px.
 * cx,cy is the center. size is the effective half-width for scaling. */
static void mdw_draw_skull(float cx, float cy, float size,
                           int vp_w, int vp_h)
{
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    float s = size * 0.85f;
    if (s < 3.0f) return;  /* too small to read */
    /* White cranium */
    psolid(cx - s * 0.75f, cy - s * 0.85f, cx + s * 0.75f, cy + s * 0.10f,
           1.0f, 1.0f, 1.0f, 1.0f, vp_w, vp_h);
    /* Jaw (slightly narrower) */
    psolid(cx - s * 0.55f, cy + s * 0.10f, cx + s * 0.55f, cy + s * 0.55f,
           1.0f, 1.0f, 1.0f, 1.0f, vp_w, vp_h);
    /* Eye sockets (black) */
    float ew = s * 0.28f, eh = s * 0.32f;
    psolid(cx - s * 0.45f, cy - s * 0.45f,
           cx - s * 0.45f + ew, cy - s * 0.45f + eh,
           0.0f, 0.0f, 0.0f, 1.0f, vp_w, vp_h);
    psolid(cx + s * 0.45f - ew, cy - s * 0.45f,
           cx + s * 0.45f,       cy - s * 0.45f + eh,
           0.0f, 0.0f, 0.0f, 1.0f, vp_w, vp_h);
    /* Teeth gap (dark band across jaw) */
    psolid(cx - s * 0.45f, cy + s * 0.20f, cx + s * 0.45f, cy + s * 0.45f,
           0.0f, 0.0f, 0.0f, 1.0f, vp_w, vp_h);
    /* Two white tooth slivers */
    psolid(cx - s * 0.15f, cy + s * 0.20f, cx - s * 0.08f, cy + s * 0.45f,
           1.0f, 1.0f, 1.0f, 1.0f, vp_w, vp_h);
    psolid(cx + s * 0.08f, cy + s * 0.20f, cx + s * 0.15f, cy + s * 0.45f,
           1.0f, 1.0f, 1.0f, 1.0f, vp_w, vp_h);
}

/* Healer / Temple (ShopType=5 Hospital) — white square with a red cross. */
static void mdw_draw_cross(float cx, float cy, float size,
                           int vp_w, int vp_h)
{
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    float s = size * 0.80f;
    if (s < 3.0f) return;
    /* White square backing */
    psolid(cx - s * 0.80f, cy - s * 0.80f, cx + s * 0.80f, cy + s * 0.80f,
           1.0f, 1.0f, 1.0f, 1.0f, vp_w, vp_h);
    /* Red vertical bar */
    psolid(cx - s * 0.20f, cy - s * 0.65f, cx + s * 0.20f, cy + s * 0.65f,
           0.95f, 0.10f, 0.10f, 1.0f, vp_w, vp_h);
    /* Red horizontal bar */
    psolid(cx - s * 0.65f, cy - s * 0.20f, cx + s * 0.65f, cy + s * 0.20f,
           0.95f, 0.10f, 0.10f, 1.0f, vp_w, vp_h);
}

/* Line via 1 quad (orthogonal) or max 4 stepped quads (diagonal). Clipped.
 * Caps total quads/line to keep mdw_draw well under MAX_QUADS even with
 * thousands of exits on a 56K-room map. */
static void mdw_line_h(float x0, float y0, float x1, float y1, float thick,
                       float r, float g, float b, float a,
                       float cx0, float cy0, float cx1, float cy1, int vp_w, int vp_h)
{
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    float dx = x1 - x0, dy = y1 - y0;
    float adx = dx < 0 ? -dx : dx;
    float ady = dy < 0 ? -dy : dy;
    if (adx < 0.5f && ady < 0.5f) return;

    /* Endpoint AABB cull against content rect (cheap early-out) */
    float bx0 = x0 < x1 ? x0 : x1, bx1 = x0 > x1 ? x0 : x1;
    float by0 = y0 < y1 ? y0 : y1, by1 = y0 > y1 ? y0 : y1;
    if (bx1 < cx0 || bx0 > cx1 || by1 < cy0 || by0 > cy1) return;

    /* Orthogonal lines → single axis-aligned quad, centered on the line */
    float ht = thick * 0.5f;
    if (ady < 0.5f) {
        float qx0 = bx0, qx1 = bx1, qy0 = y0 - ht, qy1 = y0 + ht;
        if (qx0 < cx0) qx0 = cx0; if (qx1 > cx1) qx1 = cx1;
        if (qy0 < cy0) qy0 = cy0; if (qy1 > cy1) qy1 = cy1;
        if (qx1 > qx0 && qy1 > qy0) psolid(qx0, qy0, qx1, qy1, r, g, b, a, vp_w, vp_h);
        return;
    }
    if (adx < 0.5f) {
        float qx0 = x0 - ht, qx1 = x0 + ht, qy0 = by0, qy1 = by1;
        if (qx0 < cx0) qx0 = cx0; if (qx1 > cx1) qx1 = cx1;
        if (qy0 < cy0) qy0 = cy0; if (qy1 > cy1) qy1 = cy1;
        if (qx1 > qx0 && qy1 > qy0) psolid(qx0, qy0, qx1, qy1, r, g, b, a, vp_w, vp_h);
        return;
    }

    /* Diagonal → single rotated quad via push_quad_free (true 45° line
     * instead of staircase — rooms visibly connected at any zoom). */
    {
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f) return;
        float nx = -dy / len, ny = dx / len;   /* perpendicular unit vec */
        float ht = thick * 0.5f;
        float ox = nx * ht, oy = ny * ht;
        float px0 = x0 - ox, py0 = y0 - oy;
        float px1 = x1 - ox, py1 = y1 - oy;
        float px2 = x1 + ox, py2 = y1 + oy;
        float px3 = x0 + ox, py3 = y0 + oy;
        /* Convert pixels → NDC and push a free-form quad. */
        float nx0 = (px0 / (float)vp_w) * 2.0f - 1.0f;
        float ny0 = (py0 / (float)vp_h) * 2.0f - 1.0f;
        float nx1 = (px1 / (float)vp_w) * 2.0f - 1.0f;
        float ny1 = (py1 / (float)vp_h) * 2.0f - 1.0f;
        float nx2 = (px2 / (float)vp_w) * 2.0f - 1.0f;
        float ny2 = (py2 / (float)vp_h) * 2.0f - 1.0f;
        float nx3 = (px3 / (float)vp_w) * 2.0f - 1.0f;
        float ny3 = (py3 / (float)vp_h) * 2.0f - 1.0f;
        push_quad_free(nx0, ny0, nx1, ny1, nx2, ny2, nx3, ny3, r, g, b, a);
    }
}

/* Outlined text via a 4-direction halo pass over CP437 bitmap. */
static void mdw_text_outlined(int px, int py, const char *s,
                              float r, float g, float b,
                              float or_, float og, float ob,
                              int vp_w, int vp_h, int cw, int ch)
{
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    ptext(px - 1, py, s, or_, og, ob, vp_w, vp_h, cw, ch);
    ptext(px + 1, py, s, or_, og, ob, vp_w, vp_h, cw, ch);
    ptext(px, py - 1, s, or_, og, ob, vp_w, vp_h, cw, ch);
    ptext(px, py + 1, s, or_, og, ob, vp_w, vp_h, cw, ch);
    ptext(px, py, s, r, g, b, vp_w, vp_h, cw, ch);
}

static int mdw_walk_to(int map_num, int room_num);

static void mdw_draw(int vp_w, int vp_h)
{
    if (!mdw_visible) return;

    if (pfn_dynpath_needs_repath) {
        int rp_map, rp_room;
        if (pfn_dynpath_needs_repath(&rp_map, &rp_room)) {
            mdw_log("[MDW] repath requested to %d,%d\n", rp_map, rp_room);
            int rv = mdw_walk_to(rp_map, rp_room);
            if (rv != 0)
                mdw_log("[MDW] repath failed: %d\n", rv);
        }
    }

    if (!mdw_scaled) { mdw_w = 640 * ui_scale; mdw_h = 480 * ui_scale; mdw_scaled = 1; }

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;
    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);

    float x0 = mdw_x, y0 = mdw_y;
    float pw = mdw_w, ph = mdw_h;
    float x1 = x0 + pw, y1 = y0 + ph;

    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];
    float dmr = t->dim[0], dmg = t->dim[1], dmb = t->dim[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];

    /* ---- Drop shadow ---- */
    psolid(x0 + 6, y0 + 6, x1 + 6, y1 + 6, 0.0f, 0.0f, 0.0f, 0.40f, vp_w, vp_h);

    /* ---- Panel background — opaque like pl_/vkw windows ---- */
    psolid(x0, y0, x1, y1,
           bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.96f, vp_w, vp_h);

    /* ---- Outer bevel ---- */
    psolid(x0, y0, x1, y0 + 1, bgr + 0.18f, bgg + 0.18f, bgb + 0.18f, 0.80f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1, y1, bgr + 0.14f, bgg + 0.14f, bgb + 0.14f, 0.70f, vp_w, vp_h);
    psolid(x0, y1 - 1, x1, y1, bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.90f, vp_w, vp_h);
    psolid(x1 - 1, y0, x1, y1, bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.85f, vp_w, vp_h);
    psolid(x0 + 1, y0 + 1, x1 - 1, y0 + 2, bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.50f, vp_w, vp_h);
    psolid(x0 + 1, y0 + 1, x0 + 2, y1 - 1, bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.40f, vp_w, vp_h);

    int titlebar_h = ch + 10;
    int pad = 8;

    /* ---- Title bar (accent-tinted, gloss) ---- */
    float tb_y0 = y0 + 2.0f, tb_y1 = y0 + (float)titlebar_h;
    float tb_r = mdw_focused ? (acr * 0.28f + bgr * 0.5f) : (acr * 0.18f + bgr * 0.55f);
    float tb_g = mdw_focused ? (acg * 0.28f + bgg * 0.5f) : (acg * 0.18f + bgg * 0.55f);
    float tb_b = mdw_focused ? (acb * 0.28f + bgb * 0.5f) : (acb * 0.18f + bgb * 0.55f);
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y1, tb_r, tb_g, tb_b, 0.95f, vp_w, vp_h);
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y0 + (float)(titlebar_h / 2),
           1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
    psolid(x0 + 2.0f, tb_y1 - 1.0f, x1 - 2.0f, tb_y1, acr, acg, acb, 0.5f, vp_w, vp_h);

    /* Update cached room code when current room changes */
    if (mdw_cur_ri != mdw_cur_room_code_ri) {
        mdw_cur_room_code_ri = mdw_cur_ri;
        mdw_cur_room_code[0] = 0;
        mdw_cur_room_known = 0;
        if (mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count && api && api->room_is_known) {
            mdw_cur_room_known = api->room_is_known(
                mdw_rooms[mdw_cur_ri].checksum, mdw_cur_room_code, sizeof(mdw_cur_room_code));
        }
    }

    /* Refresh per-room known cache on room/map change (once, not per frame) */
    DWORD ktick = GetTickCount();
    if (mdw_cur_map_view != mdw_known_map || ktick - mdw_known_tick > 5000) {
        mdw_known_map = mdw_cur_map_view;
        mdw_known_tick = ktick;
        if (api && api->room_is_known) {
            uint32_t cap = mdw_room_count < MDW_KNOWN_MAX ? mdw_room_count : MDW_KNOWN_MAX;
            for (uint32_t ki = 0; ki < cap; ki++) {
                mdw_known_codes[ki][0] = 0;
                mdw_known_flags[ki] = api->room_is_known(
                    mdw_rooms[ki].checksum, mdw_known_codes[ki], sizeof(mdw_known_codes[ki]));
            }
        }
    }

    /* Title text — "Map Walker │ Map N  Room M │ <room name>" */
    int title_tx = (int)x0 + pad;
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    char tbuf[200];
    if (mdw_loaded && mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count) {
        MdwRoom *cr = &mdw_rooms[mdw_cur_ri];
        _snprintf(tbuf, sizeof(tbuf), "Map Walker  \xB3  Map %u  Room %u  \xB3  %s",
                  (unsigned)cr->map, (unsigned)cr->room, cr->name);
    } else if (mdw_loaded) {
        _snprintf(tbuf, sizeof(tbuf), "Map Walker  \xB3  Map %u", (unsigned)mdw_cur_map_view);
    } else {
        _snprintf(tbuf, sizeof(tbuf), "Map Walker  \xB3  (no map loaded)");
    }
    tbuf[sizeof(tbuf) - 1] = 0;
    /* Clip title to not overlap divider + room code + close [X] */
    int code_len = mdw_cur_room_known ? (int)strlen(mdw_cur_room_code) + 3 : 7;
    int title_max_cols = ((int)x1 - pad - cw - 4 - title_tx - (5 + 18 + code_len) * cw - cw * 2) / cw;
    if (title_max_cols > 0 && (int)strlen(tbuf) > title_max_cols)
        tbuf[title_max_cols] = 0;
    ptext(title_tx + 1, title_ty + 1, tbuf, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(title_tx, title_ty, tbuf, txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Room code indicator: " │ (STON)" green or " │ (????)" red */
    if (mdw_loaded && mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count) {
        int code_tx = title_tx + ((int)strlen(tbuf)) * cw;
        /* Divider + label */
        ptext(code_tx, title_ty, "  \xB3  ", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        code_tx += 5 * cw;
        ptext(code_tx, title_ty, "MegaMUD Room Code ", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        code_tx += 18 * cw;
        char codebuf[12];
        if (mdw_cur_room_known)
            wsprintfA(codebuf, "(%s)", mdw_cur_room_code);
        else
            wsprintfA(codebuf, "(%s)", "????");
        float cr2 = mdw_cur_room_known ? 0.2f : 1.0f;
        float cg2 = mdw_cur_room_known ? 1.0f : 0.3f;
        float cb2 = mdw_cur_room_known ? 0.3f : 0.3f;
        ptext(code_tx, title_ty, codebuf, cr2, cg2, cb2, vp_w, vp_h, cw, ch);
    }

    /* Close [X] */
    int close_tx = (int)x1 - pad - cw;
    ptext(close_tx, title_ty, "X", 1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);

    /* Reserve a fixed-height controls strip at the bottom for the checkbox,
     * map/room inputs, Set Room button, and zoom buttons. */
    int ctrl_h = ch + 10;
    float ctrl_y0 = y1 - 6 - (float)ctrl_h;
    float ctrl_y1 = y1 - 6;

    /* ---- Content area (shrink for path panel if visible) ---- */
    int pp_cols = 30;  /* path panel width in characters */
    int pp_w_px = mdw_show_path_panel ? (pp_cols * cw + 16) : 0;
    float c_x0 = x0 + 6, c_y0 = (float)tb_y1 + 3;
    float c_x1 = x1 - 6 - (float)pp_w_px, c_y1 = ctrl_y0 - 2;
    float pp_x0f = c_x1, pp_x1f = x1 - 6;
    mdw_pp_x0 = pp_x0f; mdw_pp_x1 = pp_x1f;
    mdw_pp_y0 = c_y0;   mdw_pp_y1 = c_y1;
    /* Content bg — solid dark canvas like a terminal window */
    psolid(c_x0, c_y0, c_x1, c_y1,
           bgr * 0.35f, bgg * 0.35f, bgb * 0.35f, 0.98f, vp_w, vp_h);
    /* Inner bevel for content area */
    psolid(c_x0, c_y0, c_x1, c_y0 + 1, 0.0f, 0.0f, 0.0f, 0.5f, vp_w, vp_h);
    psolid(c_x0, c_y0, c_x0 + 1, c_y1, 0.0f, 0.0f, 0.0f, 0.4f, vp_w, vp_h);
    psolid(c_x0, c_y1 - 1, c_x1, c_y1, bgr + 0.15f, bgg + 0.15f, bgb + 0.15f, 0.4f, vp_w, vp_h);
    psolid(c_x1 - 1, c_y0, c_x1, c_y1, bgr + 0.12f, bgg + 0.12f, bgb + 0.12f, 0.4f, vp_w, vp_h);

    if (!mdw_loaded) {
        const char *msg = "Load a map database via Extras > Load Map DB...";
        int mw = (int)strlen(msg) * cw;
        mdw_text_outlined((int)((c_x0 + c_x1 - mw) * 0.5f),
                          (int)((c_y0 + c_y1 - ch) * 0.5f),
                          msg, dmr, dmg, dmb, 0.0f, 0.0f, 0.0f,
                          vp_w, vp_h, cw, ch);
        return;
    }

    /* ---- Map rendering ---- */
    /* World-to-window transform */
    float cell = 20.0f * mdw_zoom;
    float tile = 7.0f * mdw_zoom;  /* half-size */
    if (tile < 3.5f) tile = 3.5f;
    float cview_x = (c_x0 + c_x1) * 0.5f;
    float cview_y = (c_y0 + c_y1) * 0.5f;
    /* screen = cview + (world - mdw_view) * zoom */
    float zfac = cell / 20.0f;

    /* Glow pulse factor for current room (1.0 → 0.0 over 1200ms) */
    DWORD now = GetTickCount();
    float pulse = 0.0f;
    if (mdw_cur_ri != 0xFFFFFFFF) {
        DWORD age = now - mdw_pulse_tick;
        if (age < 1200) pulse = 1.0f - (float)age / 1200.0f;
        /* Steady soft breathing for current room */
        float breathe = 0.3f + 0.2f * sinf((float)(now % 2000) / 2000.0f * 6.28318f);
        if (pulse < breathe) pulse = breathe;
    }

    /* Exits first — aggressive viewport cull. An exit line from a room fully
     * off-screen can still clip through the viewport only if its neighbor is
     * on-screen (rooms are adjacent on the BFS grid, ≤1 cell apart, so a
     * margin of one cell is sufficient). */
    float margin = cell + 4.0f;
    float cull_x0 = c_x0 - margin, cull_x1 = c_x1 + margin;
    float cull_y0 = c_y0 - margin, cull_y1 = c_y1 + margin;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->gx == INT16_MIN) continue;
        if (r->map != mdw_cur_map_view) continue;
        float sx0 = cview_x + (r->gx * 20.0f - mdw_view_x) * zfac;
        float sy0 = cview_y + (r->gy * 20.0f - mdw_view_y) * zfac;
        if (sx0 < cull_x0 || sx0 > cull_x1 || sy0 < cull_y0 || sy0 > cull_y1) continue;
        for (uint32_t e = 0; e < r->exit_count; e++) {
            MdwExit *ex = &mdw_exits[r->exit_ofs + e];
            if (ex->dir > 7) continue;
            if (ex->tmap != mdw_cur_map_view) continue;
            uint32_t ni = mdw_room_lookup(ex->tmap, ex->troom);
            if (ni == 0xFFFFFFFF) continue;
            if (mdw_rooms[ni].gx == INT16_MIN) continue;
            float sx1 = cview_x + (mdw_rooms[ni].gx * 20.0f - mdw_view_x) * zfac;
            float sy1 = cview_y + (mdw_rooms[ni].gy * 20.0f - mdw_view_y) * zfac;
            /* Skip lines where both endpoints are off-screen on the same side */
            if ((sx0 < c_x0 && sx1 < c_x0) || (sx0 > c_x1 && sx1 > c_x1)) continue;
            if ((sy0 < c_y0 && sy1 < c_y0) || (sy0 > c_y1 && sy1 > c_y1)) continue;
            float lr = 0.45f, lg = 0.45f, lb = 0.50f;
            if (ex->flags & 0x0001) { lr = 0.85f; lg = 0.55f; lb = 0.20f; }
            if (ex->flags & 0x0002) { lr = 0.35f; lg = 0.35f; lb = 0.35f; }
            if (ex->flags & 0x0004) { lr = 0.90f; lg = 0.15f; lb = 0.15f; }
            /* Thickness scales mildly with zoom so the line doesn't vanish
             * when zoomed out and doesn't overwhelm rooms when zoomed in. */
            float lthick = 3.0f * mdw_zoom;
            if (lthick < 2.0f) lthick = 2.0f;
            if (lthick > 6.0f) lthick = 6.0f;
            mdw_line_h(sx0, sy0, sx1, sy1, lthick, lr, lg, lb, 0.85f,
                       c_x0, c_y0, c_x1, c_y1, vp_w, vp_h);
        }
    }

    /* Rooms */
    float bgdark_r = bgr * 0.6f, bgdark_g = bgg * 0.6f, bgdark_b = bgb * 0.6f;
    uint32_t hover_ri = 0xFFFFFFFF;
    float fmx = (float)vkm_mouse_x, fmy = (float)vkm_mouse_y;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->gx == INT16_MIN) continue;
        if (r->map != mdw_cur_map_view) continue;
        float sx = cview_x + (r->gx * 20.0f - mdw_view_x) * zfac;
        float sy = cview_y + (r->gy * 20.0f - mdw_view_y) * zfac;
        if (sx + tile < c_x0 || sx - tile > c_x1) continue;
        if (sy + tile < c_y0 || sy - tile > c_y1) continue;

        /* Hover test (only inside the content rect) */
        if (fmx >= c_x0 && fmx <= c_x1 && fmy >= c_y0 && fmy <= c_y1 &&
            fmx >= sx - tile && fmx <= sx + tile &&
            fmy >= sy - tile && fmy <= sy + tile) {
            hover_ri = i;
        }

        float rc = 0.30f, gc = 0.55f, bc = 0.85f;
        if (r->shop)           { rc = 0.90f; gc = 0.80f; bc = 0.15f; }
        else if (r->npc)       { rc = 0.85f; gc = 0.20f; bc = 0.20f; }
        else if (r->spell)     { rc = 0.70f; gc = 0.30f; bc = 0.90f; }
        else if (r->lair_count){ rc = 0.90f; gc = 0.40f; bc = 0.10f; }

        /* Lair-number text color (yellow normally; on current tile we
         * override below to bright green on the violet player color). */
        float tr = 1.0f, tg = 0.95f, tb = 0.40f;

        float glow = 0.0f;
        if (i == mdw_cur_ri) {
            glow = pulse;
            /* Player's current room: bright violet regardless of room type,
             * so "you are here" is unmistakable. If the current room is a
             * lair, its number is drawn in bright green (see tr/tg/tb) with
             * the black drop shadow already applied below. */
            rc = 0.85f; gc = 0.25f; bc = 1.00f;
            tr = 0.30f; tg = 1.00f; tb = 0.35f;
        }

        /* Outer ring: accent color on current; theme dim otherwise */
        float orng_r = acr, orng_g = acg, orng_b = acb;
        if (i != mdw_cur_ri) { orng_r = dmr; orng_g = dmg; orng_b = dmb; }

        mdw_tile_3d(sx, sy, tile, rc, gc, bc,
                    orng_r, orng_g, orng_b,
                    bgdark_r, bgdark_g, bgdark_b, glow,
                    c_x0, c_y0, c_x1, c_y1, vp_w, vp_h);

        /* Clip gate for all text/glyphs: the tile itself is shader-clipped
         * to the content rect, but ptext/draw_skull/draw_cross aren't.
         * Without this check, labels on tiles that straddle the top edge
         * bleed over the window title bar. */
        int text_ok = (sy - tile >= c_y0 && sy + tile <= c_y1 &&
                       sx - tile >= c_x0 && sx + tile <= c_x1);

        /* Zoom-scaled glyph sizes for room icons and lair numbers */
        int gcw = (int)(tile * 0.55f);
        if (gcw < cw) gcw = cw;
        int gch = gcw * 2;

        /* Special-room glyphs — skip for current-room tile to avoid clashing
         * with the cyan player highlight. */
        if (text_ok && i != mdw_cur_ri) {
            int is_trainer = r->shop && mdw_shop_is_trainer(r->shop);
            int is_healer  = r->shop && mdw_shop_is_healer(r->shop);
            if (r->npc && !r->shop && tile >= 5.0f) {
                /* Boss/monster room → skull */
                mdw_draw_skull(sx, sy, tile, vp_w, vp_h);
            } else if (is_healer && tile >= 5.0f) {
                /* Temple / Hospital (healer, ShopType=5) → red cross */
                mdw_draw_cross(sx, sy, tile, vp_w, vp_h);
            } else if (is_trainer && tile >= 5.0f) {
                /* Class trainer (ShopType=8) → bold bright T with halo */
                const char *g = "T";
                int gx = (int)(sx - gcw * 0.5f);
                int gy = (int)(sy - gch * 0.5f);
                ptext(gx - 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx + 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy - 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy + 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy, g, 0.4f, 1.0f, 0.6f, vp_w, vp_h, gcw, gch);
                ptext(gx + 1, gy, g, 0.4f, 1.0f, 0.6f, vp_w, vp_h, gcw, gch);
            } else if (r->shop && tile >= 5.0f) {
                /* Shop → $ glyph, bold with halo */
                const char *g = "$";
                int gx = (int)(sx - gcw * 0.5f);
                int gy = (int)(sy - gch * 0.5f);
                ptext(gx - 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx + 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy - 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy + 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy, g, 1.0f, 1.0f, 0.2f, vp_w, vp_h, gcw, gch);
                ptext(gx + 1, gy, g, 1.0f, 1.0f, 0.2f, vp_w, vp_h, gcw, gch);
            } else if (r->spell && !r->lair_count && tile >= 5.0f) {
                /* Spell/trainer room → "+" */
                const char *g = "+";
                int gx = (int)(sx - gcw * 0.5f);
                int gy = (int)(sy - gch * 0.5f);
                ptext(gx - 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx + 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy - 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy + 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
                ptext(gx, gy, g, 1.0f, 1.0f, 1.0f, vp_w, vp_h, gcw, gch);
                ptext(gx + 1, gy, g, 1.0f, 1.0f, 1.0f, vp_w, vp_h, gcw, gch);
            }
        }

        /* Lair size (monster cap) — bold, drop-shadowed, centered on tile */
        if (text_ok && r->lair_count) {
            char nb[8]; wsprintfA(nb, "%u", r->lair_count);
            int nlen = (int)strlen(nb);
            int nw = nlen * gcw + 1;
            int tx = (int)(sx - nw * 0.5f);
            int ty = (int)(sy - gch * 0.5f);
            /* Drop shadow (2px down/right, black) */
            ptext(tx + 2, ty + 2, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
            ptext(tx + 3, ty + 2, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
            /* Black halo — 4 directions */
            ptext(tx - 1, ty,     nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
            ptext(tx + 1, ty,     nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
            ptext(tx,     ty - 1, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
            ptext(tx,     ty + 1, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, gcw, gch);
            /* Main glyph */
            ptext(tx,     ty, nb, tr, tg, tb, vp_w, vp_h, gcw, gch);
            ptext(tx + 1, ty, nb, tr, tg, tb, vp_w, vp_h, gcw, gch);
        }

        /* Up/down exit corner triangles — full corner, pulsing colors
         * UP = upper-left corner, DOWN = lower-right corner */
        if (tile >= 4.0f) {
            int has_up = 0, has_down = 0;
            for (uint32_t e = 0; e < r->exit_count; e++) {
                MdwExit *ex = &mdw_exits[r->exit_ofs + e];
                if (ex->dir == 8) has_up = 1;
                if (ex->dir == 9) has_down = 1;
            }
            if (has_up || has_down) {
                float tp = (float)(now % 800) / 800.0f;
                float alt = 0.5f + 0.5f * sinf(tp * 6.28318f);
                float tri = tile * 0.95f;
                if (tri < 4.0f) tri = 4.0f;
                int nsteps = 8;
                float stp = tri / nsteps;
                if (has_up) {
                    float ur = alt * 0.0f + (1.0f - alt) * 1.0f;
                    float ug = alt * 1.0f + (1.0f - alt) * 0.85f;
                    float ub = alt * 1.0f + (1.0f - alt) * 0.0f;
                    for (int s = 0; s < nsteps; s++) {
                        float w = tri - s * stp;
                        psolid(sx - tile, sy - tile + s * stp,
                               sx - tile + w, sy - tile + (s+1) * stp,
                               ur, ug, ub, 0.85f, vp_w, vp_h);
                    }
                }
                if (has_down) {
                    float dr = (1.0f - alt) * 0.0f + alt * 1.0f;
                    float dg = (1.0f - alt) * 1.0f + alt * 0.85f;
                    float db = (1.0f - alt) * 1.0f + alt * 0.0f;
                    for (int s = 0; s < nsteps; s++) {
                        float w = tri - s * stp;
                        psolid(sx + tile - w, sy + tile - (s+1) * stp,
                               sx + tile,     sy + tile - s * stp,
                               dr, dg, db, 0.85f, vp_w, vp_h);
                    }
                }
            }
        }

        /* Known room: animated clockwise green dots border + room code */
        if (i != mdw_cur_ri && i < MDW_KNOWN_MAX && mdw_known_flags[i]) {
                char *kcode = mdw_known_codes[i];
                float boff = tile + 2.5f;
                float dot_sz = tile * 0.15f;
                if (dot_sz < 1.2f) dot_sz = 1.2f;
                if (dot_sz > 4.0f) dot_sz = 4.0f;
                int ndots = 12;
                float side = 2.0f * boff;
                float perim = 4.0f * side;
                float dphase = (float)(now % 4000) / 4000.0f;
                float da = 0.6f + 0.35f * sinf((float)(now % 1500) / 1500.0f * 6.28318f);

                for (int d = 0; d < ndots; d++) {
                    float frac = dphase + (float)d / ndots;
                    frac -= (float)(int)frac;
                    float pos = frac * perim;
                    float ddx, ddy;
                    if (pos < side) {
                        ddx = -boff + pos; ddy = -boff;
                    } else if (pos < 2 * side) {
                        ddx = boff; ddy = -boff + (pos - side);
                    } else if (pos < 3 * side) {
                        ddx = boff - (pos - 2 * side); ddy = boff;
                    } else {
                        ddx = -boff; ddy = boff - (pos - 3 * side);
                    }
                    float dx0 = sx + ddx - dot_sz, dy0 = sy + ddy - dot_sz;
                    float dx1 = sx + ddx + dot_sz, dy1 = sy + ddy + dot_sz;
                    if (dx1 >= c_x0 && dx0 <= c_x1 && dy1 >= c_y0 && dy0 <= c_y1)
                        psolid(dx0, dy0, dx1, dy1, 0.2f, 0.9f, 0.3f, da, vp_w, vp_h);
                }

                if (text_ok && kcode[0] && tile >= 5.0f) {
                    int kcw = (int)(tile * 0.45f);
                    if (kcw < 4) kcw = 4;
                    int kch = kcw * 2;
                    int klen = (int)strlen(kcode);
                    int kw = klen * kcw + 1;
                    int kx = (int)(sx - kw * 0.5f);
                    int ky = (int)(sy + tile + 2.0f);
                    ptext(kx + 1, ky + 1, kcode, 0.0f, 0.0f, 0.0f, vp_w, vp_h, kcw, kch);
                    ptext(kx, ky, kcode, 0.3f, 1.0f, 0.4f, vp_w, vp_h, kcw, kch);
                    ptext(kx + 1, ky, kcode, 0.3f, 1.0f, 0.4f, vp_w, vp_h, kcw, kch);
                }
        }
    }
    mdw_hover_ri = hover_ri;

    /* Hover just cleared? Flush any queued recenter so the camera catches
     * up with the player's current room (only when auto-recenter is on —
     * if the user flipped it off during a hover, drop the pending target). */
    if (mdw_hover_ri == 0xFFFFFFFF && mdw_pending_recenter_ri != 0xFFFFFFFF
        && mdw_pending_recenter_ri < mdw_room_count) {
        if (mdw_auto_recenter) {
            MdwRoom *pr = &mdw_rooms[mdw_pending_recenter_ri];
            if (pr->gx != INT16_MIN) {
                mdw_view_x = pr->gx * 20.0f;
                mdw_view_y = pr->gy * 20.0f;
            }
        }
        mdw_pending_recenter_ri = 0xFFFFFFFF;
    }

    /* ---- Animated rainbow glow border for the current room ----
     * 4 concentric outlines rotating through the HSV wheel, with a
     * breathing alpha pulse so it's unmistakable which room you're in. */
    if (mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count &&
        mdw_rooms[mdw_cur_ri].gx != INT16_MIN &&
        mdw_rooms[mdw_cur_ri].map == mdw_cur_map_view) {
        MdwRoom *cr = &mdw_rooms[mdw_cur_ri];
        float csx = cview_x + (cr->gx * 20.0f - mdw_view_x) * zfac;
        float csy = cview_y + (cr->gy * 20.0f - mdw_view_y) * zfac;
        if (csx + tile + 16 >= c_x0 && csx - tile - 16 <= c_x1 &&
            csy + tile + 16 >= c_y0 && csy - tile - 16 <= c_y1) {
            DWORD tms = now;
            float phase = (float)(tms % 3000) / 3000.0f;   /* 3s full cycle */
            float breathe = 0.55f + 0.45f * sinf((float)(tms % 1400) / 1400.0f * 6.28318f);

            const int RINGS = 4;
            for (int ring = 0; ring < RINGS; ring++) {
                float tprog = (float)ring / (float)RINGS;
                /* Hue rotates over time and per-ring (rainbow chase) */
                float hue = phase + tprog;
                hue -= (float)(int)hue;
                float hr = 0, hg = 0, hb = 0;
                float h6 = hue * 6.0f;
                int seg = (int)h6;
                float f = h6 - (float)seg;
                switch (seg) {
                case 0: hr = 1.0f; hg = f;    hb = 0.0f; break;
                case 1: hr = 1.0f - f; hg = 1.0f; hb = 0.0f; break;
                case 2: hr = 0.0f; hg = 1.0f; hb = f;    break;
                case 3: hr = 0.0f; hg = 1.0f - f; hb = 1.0f; break;
                case 4: hr = f;    hg = 0.0f; hb = 1.0f; break;
                default:hr = 1.0f; hg = 0.0f; hb = 1.0f - f; break;
                }
                /* Outer rings fade out */
                float alpha = (1.0f - tprog) * breathe * 0.9f;
                float off = tile + 1.0f + (float)ring * 2.5f;
                float gx0 = csx - off, gy0 = csy - off;
                float gx1 = csx + off, gy1 = csy + off;
                /* Clip to content rect */
                float qx0 = gx0 < c_x0 ? c_x0 : gx0;
                float qy0 = gy0 < c_y0 ? c_y0 : gy0;
                float qx1 = gx1 > c_x1 ? c_x1 : gx1;
                float qy1 = gy1 > c_y1 ? c_y1 : gy1;
                if (qx1 <= qx0 || qy1 <= qy0) continue;
                float th = 1.5f;
                /* Top */
                if (gy0 >= c_y0) psolid(qx0, qy0, qx1, qy0 + th, hr, hg, hb, alpha, vp_w, vp_h);
                /* Bottom */
                if (gy1 <= c_y1) psolid(qx0, qy1 - th, qx1, qy1, hr, hg, hb, alpha, vp_w, vp_h);
                /* Left */
                if (gx0 >= c_x0) psolid(qx0, qy0, qx0 + th, qy1, hr, hg, hb, alpha, vp_w, vp_h);
                /* Right */
                if (gx1 <= c_x1) psolid(qx1 - th, qy0, qx1, qy1, hr, hg, hb, alpha, vp_w, vp_h);
            }
            /* Inner white hot-spot that fades fast for extra pop */
            float hot = 0.35f + 0.65f * breathe;
            psolid(csx - tile - 0.5f, csy - tile - 0.5f,
                   csx + tile + 0.5f, csy - tile + 1.0f,
                   1.0f, 1.0f, 1.0f, hot * 0.25f, vp_w, vp_h);
            psolid(csx - tile - 0.5f, csy + tile - 1.0f,
                   csx + tile + 0.5f, csy + tile + 0.5f,
                   1.0f, 1.0f, 1.0f, hot * 0.25f, vp_w, vp_h);
        }
    }

    /* ---- Hover tooltip (suppressed when save dialog is open) ---- */
    if (mdw_hover_ri != 0xFFFFFFFF && mdw_hover_ri < mdw_room_count && !mdw_save_showing) {
        MdwRoom *hr = &mdw_rooms[mdw_hover_ri];
        /* Collect up to ~9 lines of content */
        char lines[12][96];
        int nlines = 0;
        _snprintf(lines[nlines++], 96, "Map %u  Room %u",
                  (unsigned)hr->map, (unsigned)hr->room);
        _snprintf(lines[nlines++], 96, "%.*s", 90, hr->name[0] ? hr->name : "(unnamed)");
        /* Type flags */
        int hr_is_trainer = hr->shop && mdw_shop_is_trainer(hr->shop);
        char tflags[128]; tflags[0] = 0;
        int tlen = 0;
        if (hr_is_trainer)     tlen += _snprintf(tflags + tlen, 128 - tlen, "Trainer ");
        else if (hr->shop)     tlen += _snprintf(tflags + tlen, 128 - tlen, "Shop ");
        if (hr->npc)           tlen += _snprintf(tflags + tlen, 128 - tlen, "NPC ");
        if (hr->spell)         tlen += _snprintf(tflags + tlen, 128 - tlen, "Spell ");
        if (hr->lair_count)    tlen += _snprintf(tflags + tlen, 128 - tlen, "Lair ");
        if (tlen > 0) _snprintf(lines[nlines++], 96, "Type: %s", tflags);
        /* Illu */
        _snprintf(lines[nlines++], 96, "Illu: %d", (int)hr->light);

        /* Locate aux tables once (Monsters by Number→Name, Shops by Number→Name) */
        MdwAuxTable *mt = NULL;
        int mcol_num = -1, mcol_name = -1;
        MdwAuxTable *st = NULL;
        int scol_num = -1, scol_name = -1;
        for (uint32_t a = 0; a < mdw_aux_count; a++) {
            MdwAuxTable *t = &mdw_aux[a];
            if (!mt && strcmp(t->name, "Monsters") == 0) {
                mt = t;
                for (int c = 0; c < t->col_count; c++) {
                    if (strcmp(t->col_names[c], "Number") == 0) mcol_num  = c;
                    if (strcmp(t->col_names[c], "Name")   == 0) mcol_name = c;
                }
            } else if (!st && strcmp(t->name, "Shops") == 0) {
                st = t;
                for (int c = 0; c < t->col_count; c++) {
                    if (strcmp(t->col_names[c], "Number") == 0) scol_num  = c;
                    if (strcmp(t->col_names[c], "Name")   == 0) scol_name = c;
                }
            }
        }
        /* Cells aren't null-terminated — copy exactly cell.len bytes and cap.
         * Returns 1 if a match was found; BUF always ends null-terminated. */
        #define MDW_LOOKUP_NAME_INTO(TBL, CNUM, CNAME, ID, BUF, BUF_SZ) do { \
            (BUF)[0] = 0; \
            if ((TBL) && (CNUM) >= 0 && (CNAME) >= 0) { \
                for (uint32_t rr = 0; rr < (TBL)->row_count; rr++) { \
                    MdwAuxCell *nc = &(TBL)->cells[rr * (TBL)->col_count + (CNUM)]; \
                    int nid = 0; \
                    for (int ci = 0; ci < nc->len; ci++) { \
                        char c2 = nc->data[ci]; \
                        if (c2 < '0' || c2 > '9') break; \
                        nid = nid * 10 + (c2 - '0'); \
                    } \
                    if (nid == (int)(ID)) { \
                        MdwAuxCell *nmc = &(TBL)->cells[rr * (TBL)->col_count + (CNAME)]; \
                        int cn = nmc->len; \
                        if (cn > (int)((BUF_SZ) - 1)) cn = (int)((BUF_SZ) - 1); \
                        if (nmc->data && cn > 0) memcpy((BUF), nmc->data, cn); \
                        (BUF)[cn] = 0; \
                        break; \
                    } \
                } \
            } \
        } while (0)

        /* Unique NPC (boss) name */
        if (hr->npc) {
            char nm[80];
            MDW_LOOKUP_NAME_INTO(mt, mcol_num, mcol_name, hr->npc, nm, sizeof(nm));
            _snprintf(lines[nlines++], 96, "NPC: %s", nm[0] ? nm : "?");
        }
        /* Shop name */
        if (hr->shop) {
            char nm[80];
            MDW_LOOKUP_NAME_INTO(st, scol_num, scol_name, hr->shop, nm, sizeof(nm));
            _snprintf(lines[nlines++], 96, "%s: %s",
                      hr_is_trainer ? "Trainer" : "Shop",
                      nm[0] ? nm : "?");
        }

        /* Lair details: size + monster list abbreviated */
        if (hr->lair_count && hr->lair_raw) {
            _snprintf(lines[nlines++], 96, "Lair size: %u", (unsigned)hr->lair_count);

            /* Parse lair_raw: "(Max N): id1,id2,...,[a-b-c-d]".
             * Skip to ':', then collect comma-separated ints until '[' or end. */
            char mbuf[128]; mbuf[0] = 0;
            int mb_len = 0;
            int added = 0, total = 0;
            const char *p = strchr(hr->lair_raw, ':');
            if (p) p++;
            while (p && *p) {
                while (*p == ' ' || *p == ',') p++;
                if (*p == '[' || *p == 0) break;
                int id = 0;
                const char *q = p;
                while (*q >= '0' && *q <= '9') { id = id * 10 + (*q - '0'); q++; }
                if (q == p) break;
                total++;
                char nm[64];
                MDW_LOOKUP_NAME_INTO(mt, mcol_num, mcol_name, id, nm, sizeof(nm));
                if (!nm[0]) { nm[0] = '?'; nm[1] = 0; }
                if (added < 3) {
                    int left = (int)sizeof(mbuf) - mb_len - 1;
                    if (left > 0) {
                        int w = _snprintf(mbuf + mb_len, left, "%s%s",
                                          mb_len ? ", " : "", nm);
                        if (w > 0) mb_len += w;
                    }
                    added++;
                }
                p = q;
            }
            if (total > added) {
                int left = (int)sizeof(mbuf) - mb_len - 1;
                if (left > 0) _snprintf(mbuf + mb_len, left, ", +%d more", total - added);
            }
            if (mbuf[0]) _snprintf(lines[nlines++], 96, "Mobs: %s", mbuf);
        }

        /* CMD index (special trigger/command hook on this room) */
        if (hr->cmd) _snprintf(lines[nlines++], 96, "CMD: %u", (unsigned)hr->cmd);
        #undef MDW_LOOKUP_NAME_INTO

        /* Compute tooltip box size from longest line */
        int maxw = 0;
        for (int i = 0; i < nlines; i++) {
            int ln = (int)strlen(lines[i]);
            if (ln > maxw) maxw = ln;
        }
        int pad = 6;
        int tw = maxw * cw + pad * 2;
        int th = nlines * (ch + 2) + pad * 2;
        float tx = (float)vkm_mouse_x + 14.0f;
        float ty = (float)vkm_mouse_y + 14.0f;
        /* Clamp inside panel's content rect; flip if it would spill */
        if (tx + tw > c_x1) tx = (float)vkm_mouse_x - 6.0f - tw;
        if (tx < c_x0) tx = c_x0 + 2.0f;
        if (ty + th > c_y1) ty = (float)vkm_mouse_y - 6.0f - th;
        if (ty < c_y0) ty = c_y0 + 2.0f;
        float tx1 = tx + tw, ty1 = ty + th;
        /* Drop shadow */
        psolid(tx + 3, ty + 3, tx1 + 3, ty1 + 3, 0.0f, 0.0f, 0.0f, 0.45f, vp_w, vp_h);
        /* Panel body (slightly darker than content bg) */
        psolid(tx, ty, tx1, ty1, bgr * 0.75f, bgg * 0.75f, bgb * 0.75f, 0.97f, vp_w, vp_h);
        /* 1-px accent border */
        psolid(tx, ty, tx1, ty + 1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid(tx, ty1 - 1, tx1, ty1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid(tx, ty, tx + 1, ty1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid(tx1 - 1, ty, tx1, ty1, acr, acg, acb, 0.9f, vp_w, vp_h);
        /* Text lines */
        int ly = (int)ty + pad;
        for (int i = 0; i < nlines; i++) {
            float tr = txr, tg = txg, tb = txb;
            if (i == 0) { tr = acr; tg = acg; tb = acb; }  /* header in accent color */
            ptext((int)tx + pad, ly, lines[i], tr, tg, tb, vp_w, vp_h, cw, ch);
            ly += ch + 2;
        }
    }

    /* ---- Path panel (right sidebar) ---- */
    if (mdw_show_path_panel && pp_w_px > 0) {
        /* Panel background */
        psolid(pp_x0f, c_y0, pp_x1f, c_y1,
               bgr * 0.25f, bgg * 0.25f, bgb * 0.28f, 0.95f, vp_w, vp_h);
        /* Left accent stripe */
        psolid(pp_x0f, c_y0, pp_x0f + 2, c_y1,
               0.20f, 0.85f, 0.35f, 0.9f, vp_w, vp_h);
        /* Separator from map area */
        psolid(pp_x0f, c_y0, pp_x0f + 1, c_y1,
               0.0f, 0.0f, 0.0f, 0.5f, vp_w, vp_h);

        int pp_pad = 6;
        int pp_tx = (int)pp_x0f + pp_pad + 3;
        int pp_ty = (int)c_y0 + pp_pad;
        int pp_max_cols = ((int)pp_x1f - pp_tx - pp_pad) / cw;
        if (pp_max_cols < 8) pp_max_cols = 8;

        /* Header: PATH N step(s) */
        char pp_hdr[64];
        if (mdw_rec_n > 0)
            wsprintfA(pp_hdr, "PATH  %u step%s", mdw_rec_n, mdw_rec_n == 1 ? "" : "s");
        else
            wsprintfA(pp_hdr, "PATH  (empty)");
        ptext(pp_tx, pp_ty, pp_hdr, txr, txg, txb, vp_w, vp_h, cw, ch);

        /* REC indicator (blinking when active) */
        if (mdw_rec_active) {
            int blink = (GetTickCount() / 400) & 1;
            if (blink)
                ptext(pp_tx + (int)strlen(pp_hdr) * cw + cw, pp_ty,
                      "REC", 1.0f, 0.15f, 0.15f, vp_w, vp_h, cw, ch);
            /* Red dot */
            float rdx = pp_x1f - pp_pad - (float)cw;
            float rdy = (float)pp_ty + (float)ch * 0.3f;
            psolid(rdx, rdy, rdx + (float)ch * 0.4f, rdy + (float)ch * 0.4f,
                   1.0f, 0.1f, 0.1f, blink ? 1.0f : 0.4f, vp_w, vp_h);
        }
        pp_ty += ch + 4;

        /* Buttons row: [REC] [CLR] [SAVE] [x]MPX */
        {
            int bh = ch + 4;
            int bx = pp_tx;

            /* [REC] / [STOP] */
            const char *rec_lbl = mdw_rec_active ? "STOP" : "REC";
            int rec_w = (int)strlen(rec_lbl) * cw + 8;
            float br = mdw_rec_active ? 0.7f : 0.15f;
            float bg2 = mdw_rec_active ? 0.1f : 0.55f;
            float bb = mdw_rec_active ? 0.1f : 0.15f;
            psolid((float)bx, (float)pp_ty, (float)(bx + rec_w), (float)(pp_ty + bh),
                   br * 0.5f + bgr * 0.3f, bg2 * 0.5f + bgg * 0.3f, bb * 0.5f + bgb * 0.3f,
                   1.0f, vp_w, vp_h);
            psolid((float)bx, (float)pp_ty, (float)(bx + rec_w), (float)pp_ty + 1,
                   br, bg2, bb, 0.9f, vp_w, vp_h);
            ptext(bx + 4, pp_ty + 2, rec_lbl, txr, txg, txb, vp_w, vp_h, cw, ch);
            mdw_btn_rec_x0 = (float)bx; mdw_btn_rec_x1 = (float)(bx + rec_w);
            bx += rec_w + 4;

            /* [CLR] */
            int clr_w = 3 * cw + 8;
            psolid((float)bx, (float)pp_ty, (float)(bx + clr_w), (float)(pp_ty + bh),
                   bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 1.0f, vp_w, vp_h);
            psolid((float)bx, (float)pp_ty, (float)(bx + clr_w), (float)pp_ty + 1,
                   dmr, dmg, dmb, 0.9f, vp_w, vp_h);
            ptext(bx + 4, pp_ty + 2, "CLR", txr, txg, txb, vp_w, vp_h, cw, ch);
            mdw_btn_clear_x0 = (float)bx; mdw_btn_clear_x1 = (float)(bx + clr_w);
            bx += clr_w + 4;

            /* [UNDO] */
            {
                int undo_w = 4 * cw + 8;
                float ua = (mdw_rec_n > 0) ? 1.0f : 0.4f;
                psolid((float)bx, (float)pp_ty, (float)(bx + undo_w), (float)(pp_ty + bh),
                       0.6f * 0.5f + bgr * 0.3f, 0.5f * 0.5f + bgg * 0.3f, 0.1f * 0.5f + bgb * 0.3f,
                       ua, vp_w, vp_h);
                psolid((float)bx, (float)pp_ty, (float)(bx + undo_w), (float)pp_ty + 1,
                       0.6f, 0.5f, 0.1f, 0.9f * ua, vp_w, vp_h);
                ptext(bx + 4, pp_ty + 2, "UNDO", txr, txg, txb, vp_w, vp_h, cw, ch);
                mdw_btn_undo_x0 = (float)bx; mdw_btn_undo_x1 = (float)(bx + undo_w);
                bx += undo_w + 4;
            }

            /* [SAVE] */
            int sav_w = 4 * cw + 8;
            float sav_a = (mdw_rec_n > 0) ? 1.0f : 0.4f;
            psolid((float)bx, (float)pp_ty, (float)(bx + sav_w), (float)(pp_ty + bh),
                   acr * 0.4f + bgr * 0.3f, acg * 0.4f + bgg * 0.3f, acb * 0.4f + bgb * 0.3f,
                   sav_a, vp_w, vp_h);
            psolid((float)bx, (float)pp_ty, (float)(bx + sav_w), (float)pp_ty + 1,
                   acr, acg, acb, 0.9f * sav_a, vp_w, vp_h);
            ptext(bx + 4, pp_ty + 2, "SAVE", txr, txg, txb, vp_w, vp_h, cw, ch);
            mdw_btn_save_x0 = (float)bx; mdw_btn_save_x1 = (float)(bx + sav_w);
            bx += sav_w + 6;

            /* [x] MPX checkbox */
            int cb_sz = ch;
            psolid((float)bx, (float)pp_ty + 2, (float)(bx + cb_sz), (float)(pp_ty + 2 + cb_sz),
                   bgr * 0.2f, bgg * 0.2f, bgb * 0.2f, 1.0f, vp_w, vp_h);
            psolid((float)bx, (float)pp_ty + 2, (float)(bx + cb_sz), (float)pp_ty + 3,
                   acr, acg, acb, 0.8f, vp_w, vp_h);
            psolid((float)bx, (float)(pp_ty + 2 + cb_sz - 1), (float)(bx + cb_sz), (float)(pp_ty + 2 + cb_sz),
                   acr, acg, acb, 0.8f, vp_w, vp_h);
            psolid((float)bx, (float)pp_ty + 2, (float)bx + 1, (float)(pp_ty + 2 + cb_sz),
                   acr, acg, acb, 0.8f, vp_w, vp_h);
            psolid((float)(bx + cb_sz - 1), (float)pp_ty + 2, (float)(bx + cb_sz), (float)(pp_ty + 2 + cb_sz),
                   acr, acg, acb, 0.8f, vp_w, vp_h);
            if (mdw_rec_gen_mpx) {
                psolid((float)bx + 3, (float)pp_ty + 5,
                       (float)(bx + cb_sz - 3), (float)(pp_ty + 2 + cb_sz - 3),
                       0.25f, 0.95f, 0.35f, 1.0f, vp_w, vp_h);
            }
            mdw_btn_mpx_x0 = (float)bx; mdw_btn_mpx_x1 = (float)(bx + cb_sz);
            ptext(bx + cb_sz + 3, pp_ty + 2, "MPX", txr, txg, txb, vp_w, vp_h, cw, ch);
        }
        pp_ty += ch + 8;

        /* Divider line */
        psolid(pp_x0f + 4, (float)pp_ty, pp_x1f - 4, (float)pp_ty + 1,
               dmr * 0.5f, dmg * 0.5f, dmb * 0.5f, 0.6f, vp_w, vp_h);
        pp_ty += 4;

        /* Step list area */
        int pp_list_y0 = pp_ty;
        int pp_list_y1 = (int)c_y1 - pp_pad - ch - 4; /* leave room for status */
        int visible_rows = (pp_list_y1 - pp_list_y0) / (ch + 2);
        if (visible_rows < 1) visible_rows = 1;

        if (mdw_rec_n == 0) {
            ptext(pp_tx, pp_ty + (pp_list_y1 - pp_list_y0) / 2 - ch,
                  "C to record", dmr * 0.7f, dmg * 0.7f, dmb * 0.7f,
                  vp_w, vp_h, cw, ch);
            ptext(pp_tx, pp_ty + (pp_list_y1 - pp_list_y0) / 2,
                  "walk to record", dmr * 0.7f, dmg * 0.7f, dmb * 0.7f,
                  vp_w, vp_h, cw, ch);
        } else {
            /* Show most recent steps (auto-scroll to bottom) */
            int start_idx = 0;
            if ((int)mdw_rec_n > visible_rows) {
                start_idx = (int)mdw_rec_n - visible_rows - mdw_path_scroll;
                if (start_idx < 0) start_idx = 0;
            }
            int ly = pp_list_y0;
            for (int i = start_idx; i < (int)mdw_rec_n && ly + ch < pp_list_y1; i++) {
                MdwRecStep *s = &mdw_rec_path[i];
                char line[80];
                char dir_up[64];
                for (int j = 0; j < 64 && s->dir[j]; j++)
                    dir_up[j] = (char)toupper((unsigned char)s->dir[j]);
                dir_up[lstrlenA(s->dir)] = 0;
                wsprintfA(line, "%08X:%04X:%s", s->cksum, s->flags, dir_up);
                if ((int)strlen(line) > pp_max_cols) line[pp_max_cols] = 0;

                float sr = 0.65f, sg = 0.70f, sb = 0.75f;
                if (i == (int)mdw_rec_n - 1) { sr = 0.30f; sg = 1.00f; sb = 0.45f; }
                ptext(pp_tx, ly, line, sr, sg, sb, vp_w, vp_h, cw, ch);
                ly += ch + 2;
            }
        }

        /* Status / MPX verify status at bottom of panel */
        {
            int st_y = (int)c_y1 - pp_pad - ch;
            const char *st = "";
            float st_r = dmr, st_g = dmg, st_b = dmb;
            DWORD now2 = GetTickCount();
            if (mdw_mpx_verify_active) {
                st = mdw_mpx_status;
                st_r = 0.9f; st_g = 0.8f; st_b = 0.2f;
            } else if (mdw_save_status[0] && (now2 - mdw_save_status_tick) < 5000) {
                st = mdw_save_status;
                st_r = 0.3f; st_g = 1.0f; st_b = 0.4f;
            } else if (mdw_mpx_status[0] && (now2 - mdw_mpx_status_tick) < 5000) {
                st = mdw_mpx_status;
                st_r = 0.9f; st_g = 0.8f; st_b = 0.2f;
            }
            if (st[0]) {
                char stbuf[64];
                lstrcpynA(stbuf, st, pp_max_cols < 63 ? pp_max_cols + 1 : 64);
                ptext(pp_tx, st_y, stbuf, st_r, st_g, st_b, vp_w, vp_h, cw, ch);
            }
        }
    }

    /* ---- Save dialog overlay ---- */
    if (mdw_save_showing) {
        float sd_w = 320.0f * ui_scale;
        float sd_h = 300.0f * ui_scale;
        float sd_x = (x0 + x1 - sd_w) * 0.5f;
        float sd_y = (y0 + y1 - sd_h) * 0.5f;
        float sd_x1 = sd_x + sd_w, sd_y1 = sd_y + sd_h;
        mdw_sd_x0 = sd_x; mdw_sd_y0 = sd_y; mdw_sd_x1 = sd_x1; mdw_sd_y1 = sd_y1;

        /* Dim background */
        psolid(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.5f, vp_w, vp_h);
        /* Dialog panel */
        psolid(sd_x, sd_y, sd_x1, sd_y1,
               bgr + 0.06f, bgg + 0.06f, bgb + 0.06f, 0.98f, vp_w, vp_h);
        /* Border */
        psolid(sd_x, sd_y, sd_x1, sd_y + 1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid(sd_x, sd_y1 - 1, sd_x1, sd_y1, acr * 0.4f, acg * 0.4f, acb * 0.4f, 0.9f, vp_w, vp_h);
        psolid(sd_x, sd_y, sd_x + 1, sd_y1, acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid(sd_x1 - 1, sd_y, sd_x1, sd_y1, acr * 0.4f, acg * 0.4f, acb * 0.4f, 0.8f, vp_w, vp_h);

        int sd_pad = 10;
        int sd_tx = (int)sd_x + sd_pad;
        int sd_ty = (int)sd_y + sd_pad;

        ptext(sd_tx, sd_ty, mdw_save_is_loop ? "Save Loop" : "Save Path",
              acr, acg, acb, vp_w, vp_h, cw, ch);
        sd_ty += ch + 4;
        if (mdw_save_warn[0]) {
            ptext(sd_tx, sd_ty, mdw_save_warn, 1.0f, 0.6f, 0.2f, vp_w, vp_h, cw, ch);
            sd_ty += ch + 4;
        }

        int fw = (int)sd_w - sd_pad * 2;
        float fb;

        /* Title field (loop/path display name) */
        ptext(sd_tx, sd_ty, "Title:", txr, txg, txb, vp_w, vp_h, cw, ch);
        sd_ty += ch + 2;
        fb = (mdw_save_field == 0) ? 0.25f : 0.12f;
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)(sd_ty + ch + 4),
               fb, fb, fb, 1.0f, vp_w, vp_h);
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)sd_ty + 1,
               dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        mdw_sd_title_x0 = (float)sd_tx; mdw_sd_title_x1 = (float)(sd_tx + fw);
        ptext(sd_tx + 3, sd_ty + 2, mdw_save_title, txr, txg, txb, vp_w, vp_h, cw, ch);
        if (mdw_save_field == 0 && (GetTickCount() / 500) & 1) {
            int cx2 = sd_tx + 3 + (int)strlen(mdw_save_title) * cw;
            psolid((float)cx2, (float)sd_ty + 2, (float)cx2 + 1, (float)(sd_ty + ch + 2),
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        sd_ty += ch + 8;

        /* Filename field (8 chars max, becomes .mp name) */
        ptext(sd_tx, sd_ty, "Filename (8 chars):", txr, txg, txb, vp_w, vp_h, cw, ch);
        sd_ty += ch + 2;
        fb = (mdw_save_field == 1) ? 0.25f : 0.12f;
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)(sd_ty + ch + 4),
               fb, fb, fb, 1.0f, vp_w, vp_h);
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)sd_ty + 1,
               dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        mdw_sd_name_x0 = (float)sd_tx; mdw_sd_name_x1 = (float)(sd_tx + fw);
        ptext(sd_tx + 3, sd_ty + 2, mdw_save_name, txr, txg, txb, vp_w, vp_h, cw, ch);
        if (mdw_save_field == 1 && (GetTickCount() / 500) & 1) {
            int cx2 = sd_tx + 3 + (int)strlen(mdw_save_name) * cw;
            psolid((float)cx2, (float)sd_ty + 2, (float)cx2 + 1, (float)(sd_ty + ch + 2),
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        sd_ty += ch + 8;

        /* Author field */
        ptext(sd_tx, sd_ty, "Author:", txr, txg, txb, vp_w, vp_h, cw, ch);
        sd_ty += ch + 2;
        fb = (mdw_save_field == 2) ? 0.25f : 0.12f;
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)(sd_ty + ch + 4),
               fb, fb, fb, 1.0f, vp_w, vp_h);
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)sd_ty + 1,
               dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        mdw_sd_author_x0 = (float)sd_tx; mdw_sd_author_x1 = (float)(sd_tx + fw);
        ptext(sd_tx + 3, sd_ty + 2, mdw_save_author, txr, txg, txb, vp_w, vp_h, cw, ch);
        if (mdw_save_field == 2 && (GetTickCount() / 500) & 1) {
            int cx2 = sd_tx + 3 + (int)strlen(mdw_save_author) * cw;
            psolid((float)cx2, (float)sd_ty + 2, (float)cx2 + 1, (float)(sd_ty + ch + 2),
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        sd_ty += ch + 8;

        /* Area field + dropdown list */
        ptext(sd_tx, sd_ty, "Area:", txr, txg, txb, vp_w, vp_h, cw, ch);
        sd_ty += ch + 2;
        fb = (mdw_save_field == 3) ? 0.25f : 0.12f;
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)(sd_ty + ch + 4),
               fb, fb, fb, 1.0f, vp_w, vp_h);
        psolid((float)sd_tx, (float)sd_ty, (float)(sd_tx + fw), (float)sd_ty + 1,
               dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        mdw_sd_area_x0 = (float)sd_tx; mdw_sd_area_x1 = (float)(sd_tx + fw);
        const char *area_disp = mdw_save_area;
        if (mdw_area_sel >= 0 && mdw_area_sel < mdw_area_count)
            area_disp = mdw_area_names[mdw_area_sel];
        ptext(sd_tx + 3, sd_ty + 2, area_disp, txr, txg, txb, vp_w, vp_h, cw, ch);
        if (mdw_save_field == 3 && (GetTickCount() / 500) & 1 && mdw_area_sel < 0) {
            int cx2 = sd_tx + 3 + (int)strlen(mdw_save_area) * cw;
            psolid((float)cx2, (float)sd_ty + 2, (float)cx2 + 1, (float)(sd_ty + ch + 2),
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        sd_ty += ch + 6;

        /* Area dropdown (show up to 6 rows when field 3 focused) */
        if (mdw_save_field == 3 && mdw_area_count > 0) {
            int max_vis = 6;
            if (max_vis > mdw_area_count) max_vis = mdw_area_count;
            int list_h = max_vis * (ch + 2) + 4;
            float lx0 = (float)sd_tx, ly0 = (float)sd_ty;
            float lx1 = (float)(sd_tx + fw), ly1 = ly0 + (float)list_h;
            mdw_sd_area_list_y0 = ly0; mdw_sd_area_list_y1 = ly1;
            psolid(lx0, ly0, lx1, ly1, bgr * 0.3f, bgg * 0.3f, bgb * 0.3f, 0.98f, vp_w, vp_h);
            psolid(lx0, ly0, lx1, ly0 + 1, dmr, dmg, dmb, 0.5f, vp_w, vp_h);
            int aly = (int)ly0 + 2;
            int start_a = mdw_area_scroll;
            if (start_a > mdw_area_count - max_vis) start_a = mdw_area_count - max_vis;
            if (start_a < 0) start_a = 0;
            for (int a = start_a; a < mdw_area_count && a < start_a + max_vis; a++) {
                float ar = dmr, ag = dmg, ab = dmb;
                if (a == mdw_area_sel) {
                    psolid(lx0 + 1, (float)aly, lx1 - 1, (float)(aly + ch + 1),
                           acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.8f, vp_w, vp_h);
                    ar = txr; ag = txg; ab = txb;
                }
                char abuf[48];
                lstrcpynA(abuf, mdw_area_names[a], 44);
                ptext(sd_tx + 4, aly, abuf, ar, ag, ab, vp_w, vp_h, cw, ch);
                aly += ch + 2;
            }
            sd_ty += list_h + 2;
        }

        /* Buttons: [Save] [Cancel] */
        int btn_y = (int)sd_y1 - sd_pad - ch - 6;
        int btn_w = 7 * cw + 10;
        int btn_x0 = (int)sd_x + sd_pad;
        int btn_x1 = btn_x0 + btn_w;
        psolid((float)btn_x0, (float)btn_y, (float)btn_x1, (float)(btn_y + ch + 4),
               acr * 0.4f + bgr * 0.3f, acg * 0.4f + bgg * 0.3f, acb * 0.4f + bgb * 0.3f,
               1.0f, vp_w, vp_h);
        psolid((float)btn_x0, (float)btn_y, (float)btn_x1, (float)btn_y + 1,
               acr, acg, acb, 0.9f, vp_w, vp_h);
        ptext(btn_x0 + 5, btn_y + 2, "Save", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_sd_btn_save_x0 = (float)btn_x0; mdw_sd_btn_save_x1 = (float)btn_x1;

        btn_x0 = btn_x1 + 8;
        btn_x1 = btn_x0 + btn_w;
        psolid((float)btn_x0, (float)btn_y, (float)btn_x1, (float)(btn_y + ch + 4),
               bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 1.0f, vp_w, vp_h);
        psolid((float)btn_x0, (float)btn_y, (float)btn_x1, (float)btn_y + 1,
               dmr, dmg, dmb, 0.9f, vp_w, vp_h);
        ptext(btn_x0 + 5, btn_y + 2, "Cancel", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_sd_btn_cancel_x0 = (float)btn_x0; mdw_sd_btn_cancel_x1 = (float)btn_x1;
    }

    /* ---- Import prompt overlay ---- */
    if (mdw_import_prompt && !mdw_save_showing) {
        float ip_w = 320.0f * ui_scale;
        float ip_h = 90.0f * ui_scale;
        float ip_x = (x0 + x1 - ip_w) * 0.5f;
        float ip_y = (y0 + y1 - ip_h) * 0.5f;
        float ip_x1g = ip_x + ip_w, ip_y1g = ip_y + ip_h;
        int ip_pad = (int)(8 * ui_scale);

        psolid(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.4f, vp_w, vp_h);
        psolid(ip_x, ip_y, ip_x1g, ip_y1g, bgr * 0.6f, bgg * 0.6f, bgb * 0.6f, 0.95f, vp_w, vp_h);
        psolid(ip_x, ip_y, ip_x1g, ip_y + 1, acr, acg, acb, 1.0f, vp_w, vp_h);

        int ip_tx = (int)ip_x + ip_pad;
        int ip_ty = (int)ip_y + ip_pad;
        ptext(ip_tx, ip_ty, "Import to MegaMUD?", txr, txg, txb, vp_w, vp_h, cw, ch);
        ip_ty += ch + 4;
        ptext(ip_tx, ip_ty, mdw_save_status, 0.3f, 1.0f, 0.4f, vp_w, vp_h, cw, ch);
        ip_ty += ch + 8;

        /* [Import] button */
        int btn_w = 8 * cw + 10;
        int bx0 = ip_tx;
        int bx1 = bx0 + btn_w;
        psolid((float)bx0, (float)ip_ty, (float)bx1, (float)(ip_ty + ch + 4),
               0.1f, 0.4f, 0.15f, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)ip_ty, (float)bx1, (float)ip_ty + 1,
               0.3f, 1.0f, 0.4f, 0.9f, vp_w, vp_h);
        ptext(bx0 + 5, ip_ty + 2, "Import", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_sd_btn_import_x0 = (float)bx0; mdw_sd_btn_import_x1 = (float)bx1;

        /* [Loop] button — only for loops */
        mdw_sd_btn_loop_x0 = 0; mdw_sd_btn_loop_x1 = 0;
        if (mdw_import_is_loop) {
            bx0 = bx1 + 8;
            btn_w = 6 * cw + 10;
            bx1 = bx0 + btn_w;
            psolid((float)bx0, (float)ip_ty, (float)bx1, (float)(ip_ty + ch + 4),
                   0.15f, 0.2f, 0.45f, 1.0f, vp_w, vp_h);
            psolid((float)bx0, (float)ip_ty, (float)bx1, (float)ip_ty + 1,
                   0.4f, 0.5f, 1.0f, 0.9f, vp_w, vp_h);
            ptext(bx0 + 5, ip_ty + 2, "Loop", txr, txg, txb, vp_w, vp_h, cw, ch);
            mdw_sd_btn_loop_x0 = (float)bx0; mdw_sd_btn_loop_x1 = (float)bx1;
        }

        /* [Cancel] button */
        bx0 = bx1 + 8;
        btn_w = 8 * cw + 10;
        bx1 = bx0 + btn_w;
        psolid((float)bx0, (float)ip_ty, (float)bx1, (float)(ip_ty + ch + 4),
               bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)ip_ty, (float)bx1, (float)ip_ty + 1,
               dmr, dmg, dmb, 0.9f, vp_w, vp_h);
        ptext(bx0 + 5, ip_ty + 2, "Cancel", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_sd_btn_icancel_x0 = (float)bx0; mdw_sd_btn_icancel_x1 = (float)bx1;

        mdw_sd_x0 = ip_x; mdw_sd_y0 = ip_y; mdw_sd_x1 = ip_x1g; mdw_sd_y1 = ip_y1g;
    }

    /* ---- Walk confirmation dialog (path requires items) ---- */
    if (mdw_confirm_showing && mdw_dynpath_result.req_count > 0) {
        MdwDynPath *cdp = &mdw_dynpath_result;
        int nreqs = cdp->req_count;
        float cf_w = 320.0f * ui_scale;
        float cf_h = (float)(60 + nreqs * (ch + 2) + ch + 20) * ui_scale;
        if (cf_h < 120.0f * ui_scale) cf_h = 120.0f * ui_scale;
        float cf_x = (x0 + x1 - cf_w) * 0.5f;
        float cf_y = (y0 + y1 - cf_h) * 0.5f;
        float cf_x1g = cf_x + cf_w, cf_y1g = cf_y + cf_h;
        int cf_pad = (int)(10 * ui_scale);
        mdw_confirm_x0 = cf_x; mdw_confirm_y0 = cf_y;
        mdw_confirm_x1 = cf_x1g; mdw_confirm_y1 = cf_y1g;

        psolid(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.5f, vp_w, vp_h);
        psolid(cf_x, cf_y, cf_x1g, cf_y1g,
               bgr + 0.06f, bgg + 0.06f, bgb + 0.06f, 0.98f, vp_w, vp_h);
        psolid(cf_x, cf_y, cf_x1g, cf_y + 1, 1.0f, 0.7f, 0.2f, 0.9f, vp_w, vp_h);
        psolid(cf_x, cf_y1g - 1, cf_x1g, cf_y1g, 0.4f, 0.28f, 0.08f, 0.9f, vp_w, vp_h);
        psolid(cf_x, cf_y, cf_x + 1, cf_y1g, 1.0f, 0.7f, 0.2f, 0.8f, vp_w, vp_h);
        psolid(cf_x1g - 1, cf_y, cf_x1g, cf_y1g, 0.4f, 0.28f, 0.08f, 0.8f, vp_w, vp_h);

        int cf_tx = (int)cf_x + cf_pad;
        int cf_ty = (int)cf_y + cf_pad;

        ptext(cf_tx, cf_ty, "This path requires:", 1.0f, 0.7f, 0.2f, vp_w, vp_h, cw, ch);
        cf_ty += ch + 6;

        for (int i = 0; i < nreqs; i++) {
            char rline[80];
            wsprintfA(rline, "  %s", cdp->reqs[i].name[0] ? cdp->reqs[i].name : "(unknown item)");
            ptext(cf_tx, cf_ty, rline, txr, txg, txb, vp_w, vp_h, cw, ch);
            cf_ty += ch + 2;
        }
        cf_ty += 4;

        char stepinfo[64];
        wsprintfA(stepinfo, "(%d steps to %s)",
                  cdp->count,
                  (mdw_confirm_dest_ri < mdw_room_count)
                      ? mdw_rooms[mdw_confirm_dest_ri].name : "?");
        ptext(cf_tx, cf_ty, stepinfo, dmr, dmg, dmb, vp_w, vp_h, cw, ch);

        int btn_y2 = (int)cf_y1g - cf_pad - ch - 6;
        int btn_w2 = 10 * cw + 10;

        int rbx0 = cf_tx;
        int rbx1 = rbx0 + btn_w2;
        psolid((float)rbx0, (float)btn_y2, (float)rbx1, (float)(btn_y2 + ch + 4),
               0.15f, 0.4f, 0.15f, 1.0f, vp_w, vp_h);
        psolid((float)rbx0, (float)btn_y2, (float)rbx1, (float)btn_y2 + 1,
               0.3f, 1.0f, 0.4f, 0.9f, vp_w, vp_h);
        ptext(rbx0 + 5, btn_y2 + 2, "Run path", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_confirm_btn_run_x0 = (float)rbx0;
        mdw_confirm_btn_run_x1 = (float)rbx1;

        rbx0 = rbx1 + 8;
        rbx1 = rbx0 + btn_w2;
        psolid((float)rbx0, (float)btn_y2, (float)rbx1, (float)(btn_y2 + ch + 4),
               bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 1.0f, vp_w, vp_h);
        psolid((float)rbx0, (float)btn_y2, (float)rbx1, (float)btn_y2 + 1,
               dmr, dmg, dmb, 0.9f, vp_w, vp_h);
        ptext(rbx0 + 5, btn_y2 + 2, "Cancel", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_confirm_btn_cancel_x0 = (float)rbx0;
        mdw_confirm_btn_cancel_x1 = (float)rbx1;

        mdw_confirm_btn_y0 = (float)btn_y2;
        mdw_confirm_btn_y1 = (float)(btn_y2 + ch + 4);
    }

    /* ---- Code Room dialog overlay ---- */
    if (mdw_code_showing) {
        float cd_w = 300.0f * ui_scale;
        float cd_h = 140.0f * ui_scale;
        float cd_x = (x0 + x1 - cd_w) * 0.5f;
        float cd_y = (y0 + y1 - cd_h) * 0.5f;
        float cd_x1g = cd_x + cd_w, cd_y1g = cd_y + cd_h;
        int cd_pad = (int)(8 * ui_scale);

        psolid(x0, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.4f, vp_w, vp_h);
        psolid(cd_x, cd_y, cd_x1g, cd_y1g, bgr * 0.6f, bgg * 0.6f, bgb * 0.6f, 0.95f, vp_w, vp_h);
        psolid(cd_x, cd_y, cd_x1g, cd_y + 1, acr, acg, acb, 1.0f, vp_w, vp_h);

        int cd_tx = (int)cd_x + cd_pad;
        int cd_ty = (int)cd_y + cd_pad;

        ptext(cd_tx, cd_ty, "Code Room", acr, acg, acb, vp_w, vp_h, cw, ch);
        cd_ty += ch + 6;

        /* Room name + area */
        ptext(cd_tx, cd_ty, mdw_code_room_name, txr, txg, txb, vp_w, vp_h, cw, ch);
        cd_ty += ch + 2;
        if (mdw_code_area[0]) {
            ptext(cd_tx, cd_ty, mdw_code_area, dmr, dmg, dmb, vp_w, vp_h, cw, ch);
            cd_ty += ch + 2;
        }
        cd_ty += 4;

        /* 4-char code input */
        ptext(cd_tx, cd_ty, "Code (4 chars):", txr, txg, txb, vp_w, vp_h, cw, ch);
        int inp_x = cd_tx + 16 * cw;
        int inp_w = 6 * cw + 6;
        psolid((float)inp_x, (float)(cd_ty - 2), (float)(inp_x + inp_w), (float)(cd_ty + ch + 2),
               0.25f, 0.25f, 0.25f, 1.0f, vp_w, vp_h);
        psolid((float)inp_x, (float)(cd_ty - 2), (float)(inp_x + inp_w), (float)(cd_ty - 1),
               dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        ptext(inp_x + 3, cd_ty, mdw_code_input, txr, txg, txb, vp_w, vp_h, cw, ch);
        if ((GetTickCount() / 500) & 1) {
            int cxp = inp_x + 3 + (int)strlen(mdw_code_input) * cw;
            psolid((float)cxp, (float)cd_ty, (float)cxp + 1, (float)(cd_ty + ch),
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        cd_ty += ch + 8;

        /* Status line */
        if (mdw_code_status[0]) {
            ptext(cd_tx, cd_ty, mdw_code_status, 1.0f, 0.6f, 0.2f, vp_w, vp_h, cw, ch);
            cd_ty += ch + 4;
        }

        /* [OK] [Cancel] buttons */
        int btn_w = 6 * cw + 10;
        int bx0 = cd_tx, bx1 = bx0 + btn_w;
        int btn_y = (int)cd_y1g - cd_pad - ch - 6;
        float ok_r = (strlen(mdw_code_input) == 4) ? 0.1f : 0.2f;
        float ok_g = (strlen(mdw_code_input) == 4) ? 0.4f : 0.2f;
        float ok_b = (strlen(mdw_code_input) == 4) ? 0.15f : 0.2f;
        psolid((float)bx0, (float)btn_y, (float)bx1, (float)(btn_y + ch + 4),
               ok_r, ok_g, ok_b, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)btn_y, (float)bx1, (float)btn_y + 1,
               ok_r + 0.2f, ok_g + 0.2f, ok_b + 0.2f, 0.9f, vp_w, vp_h);
        ptext(bx0 + 5, btn_y + 2, "OK", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_cd_btn_ok_x0 = (float)bx0; mdw_cd_btn_ok_x1 = (float)bx1;

        bx0 = bx1 + 8; bx1 = bx0 + btn_w;
        psolid((float)bx0, (float)btn_y, (float)bx1, (float)(btn_y + ch + 4),
               bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)btn_y, (float)bx1, (float)btn_y + 1,
               dmr, dmg, dmb, 0.9f, vp_w, vp_h);
        ptext(bx0 + 5, btn_y + 2, "Cancel", txr, txg, txb, vp_w, vp_h, cw, ch);
        mdw_cd_btn_cancel_x0 = (float)bx0; mdw_cd_btn_cancel_x1 = (float)bx1;

        mdw_sd_x0 = cd_x; mdw_sd_y0 = cd_y; mdw_sd_x1 = cd_x1g; mdw_sd_y1 = cd_y1g;
    }

    /* Live dynpath progress (persistent while walking) */
    {
        int dp_step = 0, dp_count = 0, dp_on = 0;
        if (pfn_dynpath_get_progress)
            pfn_dynpath_get_progress(&dp_step, &dp_count, &dp_on);

        if (dp_on && dp_count > 0) {
            char prog[128];
            wsprintfA(prog, "DynPath  %d / %d", dp_step, dp_count);
            int sy = (int)(ctrl_y0 - ch - 4);
            psolid(c_x0, (float)sy - 1, c_x1, (float)sy + ch + 2,
                   0.0f, 0.0f, 0.0f, 0.7f, vp_w, vp_h);
            ptext((int)c_x0 + 6, sy, prog,
                  0.3f, 1.0f, 0.5f, vp_w, vp_h, cw, ch);

            mdw_walk_status[0] = 0;
        } else if (mdw_walk_status[0]) {
            DWORD elapsed = GetTickCount() - mdw_walk_status_tick;
            if (elapsed > 5000) {
                mdw_walk_status[0] = 0;
            } else {
                float alpha = elapsed > 4000 ? (5000 - elapsed) / 1000.0f : 1.0f;
                int sy = (int)(ctrl_y0 - ch - 4);
                psolid(c_x0, (float)sy - 1, c_x1, (float)sy + ch + 2,
                       0.0f, 0.0f, 0.0f, 0.7f * alpha, vp_w, vp_h);
                ptext((int)c_x0 + 6, sy, mdw_walk_status,
                      1.0f, 0.9f, 0.3f, vp_w, vp_h, cw, ch);
            }
        }
    }

    /* ---- Bottom controls strip ---- */
    mdw_ctrl_y0 = ctrl_y0;
    mdw_ctrl_y1 = ctrl_y1;
    /* Strip background (slightly lighter than content) */
    psolid(c_x0, ctrl_y0, c_x1, ctrl_y1,
           bgr + 0.05f, bgg + 0.05f, bgb + 0.05f, 0.98f, vp_w, vp_h);
    psolid(c_x0, ctrl_y0, c_x1, ctrl_y0 + 1, acr, acg, acb, 0.5f, vp_w, vp_h);

    int cy = (int)(ctrl_y0 + (ctrl_h - ch) / 2);
    int cx = (int)c_x0 + 6;

    /* [X] use rm checkbox */
    {
        int bx0 = cx, by0 = cy, bx1 = cx + ch, by1 = cy + ch;
        mdw_cb_use_rm_x0 = (float)bx0; mdw_cb_use_rm_x1 = (float)bx1;
        /* Box */
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               bgr * 0.2f, bgg * 0.2f, bgb * 0.2f, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx0, (float)by1 - 1, (float)bx1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx0 + 1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx1 - 1, (float)by0, (float)bx1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        /* Check */
        if (mdw_use_rm) {
            psolid((float)bx0 + 3, (float)by0 + 3,
                   (float)bx1 - 3, (float)by1 - 3,
                   0.25f, 0.95f, 0.35f, 1.0f, vp_w, vp_h);
        }
        ptext(bx1 + 4, cy, "use rm", txr, txg, txb, vp_w, vp_h, cw, ch);
        cx = bx1 + 4 + 6 * cw + 10;
    }

    /* [X] auto checkbox — auto-recenter map on room change vs manual pan */
    {
        int bx0 = cx, by0 = cy, bx1 = cx + ch, by1 = cy + ch;
        mdw_cb_auto_x0 = (float)bx0; mdw_cb_auto_x1 = (float)bx1;
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               bgr * 0.2f, bgg * 0.2f, bgb * 0.2f, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx0, (float)by1 - 1, (float)bx1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx0 + 1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx1 - 1, (float)by0, (float)bx1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        if (mdw_auto_recenter) {
            psolid((float)bx0 + 3, (float)by0 + 3,
                   (float)bx1 - 3, (float)by1 - 3,
                   0.25f, 0.95f, 0.35f, 1.0f, vp_w, vp_h);
        }
        ptext(bx1 + 4, cy, "auto", txr, txg, txb, vp_w, vp_h, cw, ch);
        cx = bx1 + 4 + 4 * cw + 10;
    }

    /* [X] no events — suspend timed events during map walk */
    {
        int noev = pfn_dynpath_get_suspend_events ? pfn_dynpath_get_suspend_events() : 0;
        int bx0 = cx, by0 = cy, bx1 = cx + ch, by1 = cy + ch;
        mdw_cb_noev_x0 = (float)bx0; mdw_cb_noev_x1 = (float)bx1;
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               bgr * 0.2f, bgg * 0.2f, bgb * 0.2f, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx0, (float)by1 - 1, (float)bx1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx0 + 1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx1 - 1, (float)by0, (float)bx1, (float)by1,
               acr, acg, acb, 0.8f, vp_w, vp_h);
        if (noev) {
            psolid((float)bx0 + 3, (float)by0 + 3,
                   (float)bx1 - 3, (float)by1 - 3,
                   0.95f, 0.6f, 0.2f, 1.0f, vp_w, vp_h);
        }
        ptext(bx1 + 4, cy, "no events", txr, txg, txb, vp_w, vp_h, cw, ch);
        cx = bx1 + 4 + 9 * cw + 10;
    }

    /* Map: [  ]  Room: [    ] */
    {
        int mw = 4 * cw + 6;
        int rw = 6 * cw + 6;
        ptext(cx, cy, "Map:", txr, txg, txb, vp_w, vp_h, cw, ch);
        cx += 4 * cw + 4;
        int mx0 = cx, mx1 = cx + mw;
        int my0 = cy - 2, my1 = cy + ch + 2;
        mdw_in_map_x0 = (float)mx0; mdw_in_map_x1 = (float)mx1;
        float bg_shade = mdw_input_focus == 1 ? 0.25f : 0.12f;
        psolid((float)mx0, (float)my0, (float)mx1, (float)my1,
               bg_shade, bg_shade, bg_shade, 1.0f, vp_w, vp_h);
        psolid((float)mx0, (float)my0, (float)mx1, (float)my0 + 1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        psolid((float)mx0, (float)my1 - 1, (float)mx1, (float)my1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        psolid((float)mx0, (float)my0, (float)mx0 + 1, (float)my1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        psolid((float)mx1 - 1, (float)my0, (float)mx1, (float)my1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        ptext(mx0 + 3, cy, mdw_map_buf, txr, txg, txb, vp_w, vp_h, cw, ch);
        if (mdw_input_focus == 1 && (GetTickCount() / 500) & 1) {
            int cxp = mx0 + 3 + (int)strlen(mdw_map_buf) * cw;
            psolid((float)cxp, (float)cy, (float)cxp + 1, (float)cy + ch,
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        cx = mx1 + 8;

        ptext(cx, cy, "Room:", txr, txg, txb, vp_w, vp_h, cw, ch);
        cx += 5 * cw + 4;
        int rx0 = cx, rx1 = cx + rw;
        mdw_in_room_x0 = (float)rx0; mdw_in_room_x1 = (float)rx1;
        bg_shade = mdw_input_focus == 2 ? 0.25f : 0.12f;
        psolid((float)rx0, (float)my0, (float)rx1, (float)my1,
               bg_shade, bg_shade, bg_shade, 1.0f, vp_w, vp_h);
        psolid((float)rx0, (float)my0, (float)rx1, (float)my0 + 1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        psolid((float)rx0, (float)my1 - 1, (float)rx1, (float)my1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        psolid((float)rx0, (float)my0, (float)rx0 + 1, (float)my1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        psolid((float)rx1 - 1, (float)my0, (float)rx1, (float)my1, dmr, dmg, dmb, 0.8f, vp_w, vp_h);
        ptext(rx0 + 3, cy, mdw_room_buf, txr, txg, txb, vp_w, vp_h, cw, ch);
        if (mdw_input_focus == 2 && (GetTickCount() / 500) & 1) {
            int cxp = rx0 + 3 + (int)strlen(mdw_room_buf) * cw;
            psolid((float)cxp, (float)cy, (float)cxp + 1, (float)cy + ch,
                   txr, txg, txb, 1.0f, vp_w, vp_h);
        }
        cx = rx1 + 8;
    }

    /* [Set Room] button */
    {
        const char *lbl = "Set Room";
        int bw = (int)strlen(lbl) * cw + 10;
        int bx0 = cx, bx1 = cx + bw;
        int by0 = cy - 2, by1 = cy + ch + 2;
        mdw_btn_go_x0 = (float)bx0; mdw_btn_go_x1 = (float)bx1;
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               acr * 0.4f + bgr * 0.4f, acg * 0.4f + bgg * 0.4f, acb * 0.4f + bgb * 0.4f,
               1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid((float)bx0, (float)by1 - 1, (float)bx1, (float)by1, acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.9f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx0 + 1, (float)by1, acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx1 - 1, (float)by0, (float)bx1, (float)by1, acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.8f, vp_w, vp_h);
        ptext(bx0 + 5, cy, lbl, txr, txg, txb, vp_w, vp_h, cw, ch);
        cx = bx1 + 8;
    }

    /* [Code Room] button — color changes based on known status */
    {
        const char *lbl = mdw_cur_room_known ? mdw_cur_room_code : "Code";
        float cbr, cbg, cbb;
        if (mdw_cur_room_known) {
            cbr = 0.1f; cbg = 0.35f; cbb = 0.15f;
        } else {
            cbr = 0.4f; cbg = 0.12f; cbb = 0.12f;
        }
        int bw = (int)strlen(lbl) * cw + 10;
        if (bw < 6 * cw + 10) bw = 6 * cw + 10;
        int bx0 = cx, bx1 = cx + bw;
        int by0 = cy - 2, by1 = cy + ch + 2;
        mdw_btn_code_x0 = (float)bx0; mdw_btn_code_x1 = (float)bx1;
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               cbr, cbg, cbb, 1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1,
               cbr + 0.2f, cbg + 0.2f, cbb + 0.2f, 0.9f, vp_w, vp_h);
        ptext(bx0 + 5, cy, lbl, txr, txg, txb, vp_w, vp_h, cw, ch);
    }

    /* [-] [+] zoom buttons, right-aligned */
    {
        int bw = 2 * cw + 8;
        int bx1 = (int)c_x1 - 6;
        int bx0 = bx1 - bw;
        int by0 = cy - 2, by1 = cy + ch + 2;
        mdw_btn_zoom_in_x0 = (float)bx0; mdw_btn_zoom_in_x1 = (float)bx1;
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               acr * 0.4f + bgr * 0.4f, acg * 0.4f + bgg * 0.4f, acb * 0.4f + bgb * 0.4f,
               1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx0 + 1, (float)by1, acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx1 - 1, (float)by0, (float)bx1, (float)by1, acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.8f, vp_w, vp_h);
        ptext(bx0 + 4, cy, "+", txr, txg, txb, vp_w, vp_h, cw, ch);

        bx1 = bx0 - 4;
        bx0 = bx1 - bw;
        mdw_btn_zoom_out_x0 = (float)bx0; mdw_btn_zoom_out_x1 = (float)bx1;
        psolid((float)bx0, (float)by0, (float)bx1, (float)by1,
               acr * 0.4f + bgr * 0.4f, acg * 0.4f + bgg * 0.4f, acb * 0.4f + bgb * 0.4f,
               1.0f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx1, (float)by0 + 1, acr, acg, acb, 0.9f, vp_w, vp_h);
        psolid((float)bx0, (float)by0, (float)bx0 + 1, (float)by1, acr, acg, acb, 0.8f, vp_w, vp_h);
        psolid((float)bx1 - 1, (float)by0, (float)bx1, (float)by1, acr * 0.3f, acg * 0.3f, acb * 0.3f, 0.8f, vp_w, vp_h);
        ptext(bx0 + 4, cy, "-", txr, txg, txb, vp_w, vp_h, cw, ch);
    }
}

/* ---- Interaction ---- */

/* Returns bitmask of edges grabbed at (mx,my): 1=L, 2=R, 4=T, 8=B */
static int mdw_hit_resize(int mx, int my)
{
    int rz = 6;
    float fx = (float)mx, fy = (float)my;
    int edge = 0;
    if (fx >= mdw_x - rz && fx <= mdw_x + rz && fy >= mdw_y && fy <= mdw_y + mdw_h) edge |= 1;
    if (fx >= mdw_x + mdw_w - rz && fx <= mdw_x + mdw_w + rz && fy >= mdw_y && fy <= mdw_y + mdw_h) edge |= 2;
    if (fy >= mdw_y - rz && fy <= mdw_y + rz && fx >= mdw_x && fx <= mdw_x + mdw_w) edge |= 4;
    if (fy >= mdw_y + mdw_h - rz && fy <= mdw_y + mdw_h + rz && fx >= mdw_x && fx <= mdw_x + mdw_w) edge |= 8;
    return edge;
}

static int mdw_hit_window(int mx, int my)
{
    return (mx >= (int)mdw_x && mx < (int)(mdw_x + mdw_w) &&
            my >= (int)mdw_y && my < (int)(mdw_y + mdw_h));
}

static int mdw_mouse_down(int mx, int my)
{
    if (!mdw_visible) return 0;
    /* Resize edges first (generous zone around window) */
    int edge = mdw_hit_resize(mx, my);
    if (edge) {
        mdw_focused = 1;
        mdw_resizing = 1;
        mdw_resize_edge = edge;
        mdw_resize_ox = (float)mx;
        mdw_resize_oy = (float)my;
        mdw_resize_sw = mdw_w; mdw_resize_sh = mdw_h;
        mdw_resize_sx = mdw_x; mdw_resize_sy = mdw_y;
        return 1;
    }
    if (!mdw_hit_window(mx, my)) {
        /* Clicked outside → lose focus, don't consume */
        if (mdw_focused) mdw_focused = 0;
        return 0;
    }
    mdw_focused = 1;

    int ch = (int)(VSB_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    int cw = (int)(VSB_CHAR_W * ui_scale);
    int ly = my - (int)mdw_y;

    /* Close [X] */
    if (ly < titlebar_h && mx >= (int)(mdw_x + mdw_w) - cw - 14) {
        mdw_visible = 0; mdw_focused = 0;
        return 1;
    }
    /* Titlebar drag */
    if (ly < titlebar_h) {
        mdw_dragging = 1;
        mdw_drag_ox = (float)mx - mdw_x;
        mdw_drag_oy = (float)my - mdw_y;
        return 1;
    }
    /* Bottom control strip */
    if ((float)my >= mdw_ctrl_y0 && (float)my <= mdw_ctrl_y1) {
        float fmx = (float)mx;
        if (fmx >= mdw_cb_use_rm_x0 && fmx <= mdw_cb_use_rm_x1) {
            mdw_use_rm = !mdw_use_rm;
            mdw_input_focus = 0;
            return 1;
        }
        if (fmx >= mdw_cb_noev_x0 && fmx <= mdw_cb_noev_x1) {
            if (pfn_dynpath_get_suspend_events && pfn_dynpath_set_suspend_events) {
                pfn_dynpath_set_suspend_events(!pfn_dynpath_get_suspend_events());
            }
            mdw_input_focus = 0;
            return 1;
        }
        if (fmx >= mdw_cb_auto_x0 && fmx <= mdw_cb_auto_x1) {
            mdw_auto_recenter = !mdw_auto_recenter;
            /* Flipping auto back ON while a room is known snaps view to it
             * so the user doesn't have to wait for the next movement. */
            if (mdw_auto_recenter && mdw_cur_ri != 0xFFFFFFFF &&
                mdw_cur_ri < mdw_room_count &&
                mdw_rooms[mdw_cur_ri].gx != INT16_MIN) {
                mdw_view_x = mdw_rooms[mdw_cur_ri].gx * 20.0f;
                mdw_view_y = mdw_rooms[mdw_cur_ri].gy * 20.0f;
            }
            mdw_input_focus = 0;
            return 1;
        }
        if (fmx >= mdw_in_map_x0 && fmx <= mdw_in_map_x1) {
            mdw_input_focus = 1;
            return 1;
        }
        if (fmx >= mdw_in_room_x0 && fmx <= mdw_in_room_x1) {
            mdw_input_focus = 2;
            return 1;
        }
        if (fmx >= mdw_btn_go_x0 && fmx <= mdw_btn_go_x1) {
            int mn = mdw_map_buf[0]  ? atoi(mdw_map_buf)  : (int)mdw_cur_map_view;
            int rn = mdw_room_buf[0] ? atoi(mdw_room_buf) : 0;
            if (rn > 0) mdw_goto_room(mn, rn);
            mdw_input_focus = 0;
            return 1;
        }
        if (fmx >= mdw_btn_code_x0 && fmx <= mdw_btn_code_x1 &&
            mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count) {
            MdwRoom *cr = &mdw_rooms[mdw_cur_ri];
            if (mdw_cur_room_known) {
                wsprintfA(mdw_code_status, "Already known: %s", mdw_cur_room_code);
                mdw_code_status_tick = GetTickCount();
                wsprintfA(mdw_save_status, "Already known: %s", mdw_cur_room_code);
                mdw_save_status_tick = GetTickCount();
            } else {
                mdw_code_showing = 1;
                mdw_code_input[0] = 0;
                mdw_code_cksum = cr->checksum;
                lstrcpynA(mdw_code_room_name, cr->name, sizeof(mdw_code_room_name));
                mdw_code_area[0] = 0;
                mdw_roomsmd_lookup(cr->checksum, NULL, 0, mdw_code_area, sizeof(mdw_code_area), NULL, 0);
                mdw_code_status[0] = 0;
            }
            mdw_input_focus = 0;
            return 1;
        }
        if (fmx >= mdw_btn_zoom_in_x0 && fmx <= mdw_btn_zoom_in_x1) {
            mdw_zoom *= 1.2f; if (mdw_zoom > 10.0f) mdw_zoom = 10.0f;
            mdw_input_focus = 0;
            return 1;
        }
        if (fmx >= mdw_btn_zoom_out_x0 && fmx <= mdw_btn_zoom_out_x1) {
            mdw_zoom /= 1.2f; if (mdw_zoom < 0.25f) mdw_zoom = 0.25f;
            mdw_input_focus = 0;
            return 1;
        }
        /* Clicked on empty strip area — clear input focus */
        mdw_input_focus = 0;
        return 1;
    }
    /* ---- Code Room dialog click handling ---- */
    if (mdw_code_showing) {
        float fmx = (float)mx, fmy = (float)my;
        if (fmx >= mdw_sd_x0 && fmx <= mdw_sd_x1 &&
            fmy >= mdw_sd_y0 && fmy <= mdw_sd_y1) {
            /* [OK] */
            if (fmx >= mdw_cd_btn_ok_x0 && fmx <= mdw_cd_btn_ok_x1) {
                if (strlen(mdw_code_input) != 4) {
                    lstrcpyA(mdw_code_status, "Code must be 4 characters");
                } else if (api && api->code_room) {
                    int r = api->code_room(mdw_code_cksum, mdw_code_input,
                                           mdw_code_area[0] ? mdw_code_area : "Unknown",
                                           mdw_code_room_name);
                    if (r == 0) {
                        wsprintfA(mdw_save_status, "Coded: %s", mdw_code_input);
                        mdw_save_status_tick = GetTickCount();
                        mdw_cur_room_code_ri = 0xFFFFFFFF; /* force re-check */
                        mdw_code_showing = 0;
                    } else if (r == 1) {
                        lstrcpyA(mdw_code_status, "Room already coded");
                    } else {
                        wsprintfA(mdw_code_status, "Failed (%d)", r);
                    }
                } else {
                    lstrcpyA(mdw_code_status, "API not available");
                }
                return 1;
            }
            /* [Cancel] */
            if (fmx >= mdw_cd_btn_cancel_x0 && fmx <= mdw_cd_btn_cancel_x1) {
                mdw_code_showing = 0;
                return 1;
            }
        }
        return 1;
    }
    /* ---- Save dialog click handling ---- */
    if (mdw_save_showing) {
        float fmx = (float)mx, fmy = (float)my;
        /* Click in name field */
        if (fmy >= mdw_sd_y0 && fmy <= mdw_sd_y1) {
            if (fmx >= mdw_sd_btn_save_x0 && fmx <= mdw_sd_btn_save_x1 &&
                fmy >= mdw_sd_y1 - 30 * ui_scale) {
                mdw_save_path();
                return 1;
            }
            if (fmx >= mdw_sd_btn_cancel_x0 && fmx <= mdw_sd_btn_cancel_x1 &&
                fmy >= mdw_sd_y1 - 30 * ui_scale) {
                mdw_save_showing = 0;
                return 1;
            }
            /* Field focus by click */
            if (fmx >= mdw_sd_title_x0 && fmx <= mdw_sd_title_x1 && fmy < mdw_sd_name_x0)
                mdw_save_field = 0;
            else if (fmx >= mdw_sd_name_x0 && fmx <= mdw_sd_name_x1 && fmy < mdw_sd_author_x0)
                mdw_save_field = 1;
            else if (fmx >= mdw_sd_author_x0 && fmx <= mdw_sd_author_x1)
                mdw_save_field = 2;
            else if (fmx >= mdw_sd_area_x0 && fmx <= mdw_sd_area_x1)
                mdw_save_field = 3;
            /* Area list click */
            if (mdw_save_field == 3 && fmy >= mdw_sd_area_list_y0 && fmy <= mdw_sd_area_list_y1) {
                int ch2 = (int)(VSB_CHAR_H * ui_scale);
                int row = (int)(fmy - mdw_sd_area_list_y0 - 2) / (ch2 + 2);
                int idx = mdw_area_scroll + row;
                if (idx >= 0 && idx < mdw_area_count) mdw_area_sel = idx;
            }
            return 1;
        }
        return 1; /* swallow all clicks while dialog is up */
    }
    /* ---- Import prompt click handling ---- */
    if (mdw_import_prompt) {
        float fmx = (float)mx, fmy = (float)my;
        if (fmx >= mdw_sd_x0 && fmx <= mdw_sd_x1 &&
            fmy >= mdw_sd_y0 && fmy <= mdw_sd_y1) {
            /* [Import] — import only */
            if (fmx >= mdw_sd_btn_import_x0 && fmx <= mdw_sd_btn_import_x1) {
                if (api && api->import_path && mdw_import_mp_path[0]) {
                    int r = api->import_path(mdw_import_mp_path);
                    if (r == 0)
                        wsprintfA(mdw_save_status, "Imported!");
                    else
                        wsprintfA(mdw_save_status, "Import failed (%d)", r);
                } else {
                    wsprintfA(mdw_save_status, "Import not available");
                }
                mdw_save_status_tick = GetTickCount();
                mdw_import_prompt = 0;
                return 1;
            }
            /* [Loop] — import + start looping */
            if (mdw_import_is_loop &&
                fmx >= mdw_sd_btn_loop_x0 && fmx <= mdw_sd_btn_loop_x1) {
                if (api && api->import_path && mdw_import_mp_path[0]) {
                    int r = api->import_path(mdw_import_mp_path);
                    if (r == 0 && api->fake_remote && mdw_import_title[0]) {
                        char cmd[160];
                        wsprintfA(cmd, "loop %s", mdw_import_title);
                        int lr = api->fake_remote(cmd);
                        if (lr == 0)
                            wsprintfA(mdw_save_status, "Imported + looping!");
                        else
                            wsprintfA(mdw_save_status, "Imported, loop start failed (%d)", lr);
                    } else if (r == 0) {
                        wsprintfA(mdw_save_status, "Imported (loop unavailable)");
                    } else {
                        wsprintfA(mdw_save_status, "Import failed (%d)", r);
                    }
                } else {
                    wsprintfA(mdw_save_status, "Import not available");
                }
                mdw_save_status_tick = GetTickCount();
                mdw_import_prompt = 0;
                return 1;
            }
            /* [Cancel] */
            if (fmx >= mdw_sd_btn_icancel_x0 && fmx <= mdw_sd_btn_icancel_x1) {
                mdw_import_prompt = 0;
                return 1;
            }
        }
        return 1; /* swallow clicks while prompt is up */
    }

    /* ---- Walk confirmation dialog click handling ---- */
    if (mdw_confirm_showing) {
        float fmx = (float)mx, fmy = (float)my;
        if (fmy >= mdw_confirm_btn_y0 && fmy <= mdw_confirm_btn_y1) {
            if (fmx >= mdw_confirm_btn_run_x0 && fmx <= mdw_confirm_btn_run_x1) {
                if (!mdw_verify_room()) {
                    mdw_confirm_showing = 0;
                    return 1;
                }
                MdwDynPath *cdp = &mdw_dynpath_result;
                if (cdp->steps && cdp->count > 0 && pfn_dynpath_inject &&
                    mdw_confirm_dest_ri < mdw_room_count) {
                    if (pfn_dynpath_set_label) {
                        char lbl[128];
                        wsprintfA(lbl, "%d,%d to %d,%d",
                                  mdw_rooms[mdw_cur_ri].map, mdw_rooms[mdw_cur_ri].room,
                                  mdw_rooms[mdw_confirm_dest_ri].map, mdw_rooms[mdw_confirm_dest_ri].room);
                        pfn_dynpath_set_label(lbl);
                    }
                    if (pfn_dynpath_set_dest_room)
                        pfn_dynpath_set_dest_room(
                            mdw_rooms[mdw_confirm_dest_ri].map,
                            mdw_rooms[mdw_confirm_dest_ri].room);
                    int rv = pfn_dynpath_inject(
                        (dynpath_step_t *)cdp->steps, cdp->count,
                        mdw_rooms[mdw_cur_ri].checksum,
                        mdw_rooms[mdw_confirm_dest_ri].checksum);
                    if (rv == 0)
                        wsprintfA(mdw_walk_status, "Walking to %s (%d steps)",
                                  mdw_rooms[mdw_confirm_dest_ri].name, cdp->count);
                    else
                        wsprintfA(mdw_walk_status, "Inject failed (%d)", rv);
                    mdw_walk_status_tick = GetTickCount();
                }
                mdw_confirm_showing = 0;
                return 1;
            }
            if (fmx >= mdw_confirm_btn_cancel_x0 && fmx <= mdw_confirm_btn_cancel_x1) {
                mdw_confirm_showing = 0;
                return 1;
            }
        }
        return 1;
    }

    /* ---- Path panel button clicks ---- */
    if (mdw_show_path_panel) {
        float fmx = (float)mx, fmy = (float)my;
        if (fmx >= mdw_pp_x0 && fmx <= mdw_pp_x1 &&
            fmy >= mdw_pp_y0 && fmy <= mdw_pp_y1) {
            /* REC button */
            if (fmx >= mdw_btn_rec_x0 && fmx <= mdw_btn_rec_x1 &&
                fmy >= mdw_pp_y0 && fmy <= mdw_pp_y0 + 60) {
                mdw_rec_active = !mdw_rec_active;
                if (mdw_rec_active)
                    mdw_log("[PATH] Recording started (button)\n");
                else
                    mdw_log("[PATH] Recording stopped (%u steps)\n", mdw_rec_n);
                return 1;
            }
            /* CLR button */
            if (fmx >= mdw_btn_clear_x0 && fmx <= mdw_btn_clear_x1 &&
                fmy >= mdw_pp_y0 && fmy <= mdw_pp_y0 + 60) {
                mdw_rec_clear();
                mdw_rec_active = 0;
                mdw_log("[PATH] Path cleared\n");
                return 1;
            }
            /* UNDO button */
            if (fmx >= mdw_btn_undo_x0 && fmx <= mdw_btn_undo_x1 &&
                fmy >= mdw_pp_y0 && fmy <= mdw_pp_y0 + 60 && mdw_rec_n > 0) {
                mdw_rec_undo();
                return 1;
            }
            /* SAVE button */
            if (fmx >= mdw_btn_save_x0 && fmx <= mdw_btn_save_x1 &&
                fmy >= mdw_pp_y0 && fmy <= mdw_pp_y0 + 60 && mdw_rec_n > 0) {
                mdw_save_dialog_open();
                return 1;
            }
            /* MPX checkbox */
            if (fmx >= mdw_btn_mpx_x0 && fmx <= mdw_btn_mpx_x1 + 30 &&
                fmy >= mdw_pp_y0 && fmy <= mdw_pp_y0 + 60) {
                mdw_rec_gen_mpx = !mdw_rec_gen_mpx;
                return 1;
            }
            /* Clicked in panel area — take focus, consume */
            mdw_path_focus = 1;
            mdw_input_focus = 0;
            return 1;
        }
    }

    /* Content area = start pan (and drop path/input focus) */
    mdw_path_focus = 0;
    mdw_input_focus = 0;
    mdw_panning = 1;
    mdw_pan_anchor_mx = (float)mx;
    mdw_pan_anchor_my = (float)my;
    return 1;
}

static void mdw_mouse_move(int mx, int my)
{
    if (!mdw_visible) return;
    if (mdw_dragging) {
        mdw_x = (float)mx - mdw_drag_ox;
        mdw_y = (float)my - mdw_drag_oy;
        return;
    }
    if (mdw_resizing) {
        float dx = (float)mx - mdw_resize_ox;
        float dy = (float)my - mdw_resize_oy;
        if (mdw_resize_edge & 1) { mdw_x = mdw_resize_sx + dx; mdw_w = mdw_resize_sw - dx; }
        if (mdw_resize_edge & 2) { mdw_w = mdw_resize_sw + dx; }
        if (mdw_resize_edge & 4) { mdw_y = mdw_resize_sy + dy; mdw_h = mdw_resize_sh - dy; }
        if (mdw_resize_edge & 8) { mdw_h = mdw_resize_sh + dy; }
        if (mdw_w < 200) mdw_w = 200;
        if (mdw_h < 150) mdw_h = 150;
        return;
    }
    if (mdw_panning) {
        float dx = (float)mx - mdw_pan_anchor_mx;
        float dy = (float)my - mdw_pan_anchor_my;
        mdw_pan_anchor_mx = (float)mx;
        mdw_pan_anchor_my = (float)my;
        float zfac = mdw_zoom;
        if (zfac < 0.01f) zfac = 0.01f;
        mdw_view_x -= dx / zfac;
        mdw_view_y -= dy / zfac;
    }
}

static void mdw_mouse_up(void)
{
    mdw_dragging = 0;
    mdw_resizing = 0;
    mdw_resize_edge = 0;
    mdw_panning = 0;
}

/* Right-click on a room = center the view on it. Returns 1 if the click
 * landed on a room (so the outer wndproc can skip its right-click menu). */
static int mdw_rbutton_down(int mx, int my)
{
    if (!mdw_visible || !mdw_loaded) return 0;
    if (!mdw_hit_window(mx, my)) return 0;

    int ch = (int)(VSB_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    if (my - (int)mdw_y < titlebar_h) return 0;

    /* Reproduce the transform from mdw_draw */
    float x0 = mdw_x, y0 = mdw_y;
    float x1 = x0 + mdw_w, y1 = y0 + mdw_h;
    float tb_y1 = y0 + (float)titlebar_h;
    int cw = (int)(VSB_CHAR_W * ui_scale);
    int ctrl_h_r = ch + 10;
    float ctrl_y0_r = y1 - 6 - (float)ctrl_h_r;
    int pp_cols_r = 30;
    int pp_w_px_r = mdw_show_path_panel ? (pp_cols_r * cw + 16) : 0;
    float c_x0 = x0 + 6, c_y0 = tb_y1 + 3;
    float c_x1 = x1 - 6 - (float)pp_w_px_r, c_y1 = ctrl_y0_r - 2;
    if ((float)mx < c_x0 || (float)mx > c_x1 ||
        (float)my < c_y0 || (float)my > c_y1) return 0;

    float cell = 20.0f * mdw_zoom;
    float tile = 7.0f * mdw_zoom;
    if (tile < 3.5f) tile = 3.5f;
    float cview_x = (c_x0 + c_x1) * 0.5f;
    float cview_y = (c_y0 + c_y1) * 0.5f;
    float zfac = cell / 20.0f;

    /* Find closest room to cursor (within tile radius) */
    float best_d2 = tile * tile * 4.0f;
    uint32_t best_i = 0xFFFFFFFF;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->gx == INT16_MIN) continue;
        if (r->map != mdw_cur_map_view) continue;
        float sx = cview_x + (r->gx * 20.0f - mdw_view_x) * zfac;
        float sy = cview_y + (r->gy * 20.0f - mdw_view_y) * zfac;
        if (sx < c_x0 - tile || sx > c_x1 + tile) continue;
        if (sy < c_y0 - tile || sy > c_y1 + tile) continue;
        float ddx = sx - (float)mx, ddy = sy - (float)my;
        float d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2) { best_d2 = d2; best_i = i; }
    }
    if (best_i == 0xFFFFFFFF) return 0;

    /* Center view on that room (current-room marker stays tied to
     * the real character via RM polling). */
    mdw_view_x = mdw_rooms[best_i].gx * 20.0f;
    mdw_view_y = mdw_rooms[best_i].gy * 20.0f;
    return 1;
}

static void mdw_send_rm(void)
{
    if (!api || !api->inject_command) return;
    if (pfn_auto_rm_queue_increment) pfn_auto_rm_queue_increment();
    api->inject_command("rm");
}

static int mdw_verify_room(void)
{
    if (mdw_cur_ri >= mdw_room_count) {
        wsprintfA(mdw_walk_status, "Current room unknown — refreshing...");
        mdw_walk_status_tick = GetTickCount();
        mdw_send_rm();
        return 0;
    }
    uint32_t mega_ck = pfn_get_room_checksum ? pfn_get_room_checksum() : 0;
    if (!mega_ck) return 1;
    if (mdw_rooms[mdw_cur_ri].checksum == mega_ck) return 1;
    wsprintfA(mdw_walk_status,
              "Room mismatch — refreshing position...");
    mdw_walk_status_tick = GetTickCount();
    mdw_send_rm();
    return 0;
}

static int mdw_dblclick(int mx, int my)
{
    if (!mdw_visible || !mdw_loaded) return 0;
    if (!mdw_hit_window(mx, my)) return 0;

    int ch = (int)(VSB_CHAR_H * ui_scale);
    int titlebar_h = ch + 10;
    if (my - (int)mdw_y < titlebar_h) return 0;

    float x0 = mdw_x, y0 = mdw_y;
    float x1 = x0 + mdw_w, y1 = y0 + mdw_h;
    float tb_y1 = y0 + (float)titlebar_h;
    int ctrl_h = ch + 10;
    float ctrl_y0 = y1 - 6 - (float)ctrl_h;
    int pp_cols = 30;
    int pp_w_px = mdw_show_path_panel ? (pp_cols * (int)(VSB_CHAR_W * ui_scale) + 16) : 0;
    float c_x0 = x0 + 6, c_y0 = tb_y1 + 3;
    float c_x1 = x1 - 6 - (float)pp_w_px, c_y1 = ctrl_y0 - 2;
    if ((float)mx < c_x0 || (float)mx > c_x1 ||
        (float)my < c_y0 || (float)my > c_y1) {
        return 0;
    }

    float cell = 20.0f * mdw_zoom;
    float tile = 7.0f * mdw_zoom;
    if (tile < 3.5f) tile = 3.5f;
    float cview_x = (c_x0 + c_x1) * 0.5f;
    float cview_y = (c_y0 + c_y1) * 0.5f;
    float zfac = cell / 20.0f;

    float best_d2 = tile * tile * 4.0f;
    uint32_t best_i = 0xFFFFFFFF;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->gx == INT16_MIN) continue;
        if (r->map != mdw_cur_map_view) continue;
        float sx = cview_x + (r->gx * 20.0f - mdw_view_x) * zfac;
        float sy = cview_y + (r->gy * 20.0f - mdw_view_y) * zfac;
        if (sx < c_x0 - tile || sx > c_x1 + tile) continue;
        if (sy < c_y0 - tile || sy > c_y1 + tile) continue;
        float ddx = sx - (float)mx, ddy = sy - (float)my;
        float d2 = ddx * ddx + ddy * ddy;
        if (d2 < best_d2) { best_d2 = d2; best_i = i; }
    }
    if (best_i == 0xFFFFFFFF) return 0;
    if (mdw_cur_ri == 0xFFFFFFFF || mdw_cur_ri >= mdw_room_count) {
        wsprintfA(mdw_walk_status, "Current room unknown — can't pathfind");
        mdw_walk_status_tick = GetTickCount();
        return 1;
    }
    if (best_i == mdw_cur_ri) {
        wsprintfA(mdw_walk_status, "Already in that room");
        mdw_walk_status_tick = GetTickCount();
        return 1;
    }

    rm_bridge_resolve();
    if (!pfn_dynpath_inject) {
        wsprintfA(mdw_walk_status, "Dynamic pathing not available");
        mdw_walk_status_tick = GetTickCount();
        return 1;
    }

    if (!mdw_verify_room()) return 1;

    MdwDynPath *dp = mdw_find_path(mdw_cur_ri, best_i);
    if (dp->error != MDW_DPERR_NONE) {
        strncpy(mdw_walk_status, dp->errmsg, sizeof(mdw_walk_status) - 1);
        mdw_walk_status[sizeof(mdw_walk_status) - 1] = 0;
        mdw_walk_status_tick = GetTickCount();
        return 1;
    }

    if (dp->req_count > 0) {
        mdw_confirm_dest_ri = best_i;
        mdw_confirm_showing = 1;
        return 1;
    }

    if (pfn_dynpath_set_label) {
        char lbl[128];
        wsprintfA(lbl, "%d,%d to %d,%d",
                  mdw_rooms[mdw_cur_ri].map, mdw_rooms[mdw_cur_ri].room,
                  mdw_rooms[best_i].map, mdw_rooms[best_i].room);
        pfn_dynpath_set_label(lbl);
    }
    if (pfn_dynpath_set_dest_room)
        pfn_dynpath_set_dest_room(mdw_rooms[best_i].map, mdw_rooms[best_i].room);
    int rv = pfn_dynpath_inject((dynpath_step_t *)dp->steps, dp->count,
                                mdw_rooms[mdw_cur_ri].checksum,
                                mdw_rooms[best_i].checksum);
    if (rv == 0) {
        wsprintfA(mdw_walk_status, "Walking to %s (%d steps)",
                  mdw_rooms[best_i].name, dp->count);
    } else {
        wsprintfA(mdw_walk_status, "Inject failed (%d)", rv);
    }
    mdw_walk_status_tick = GetTickCount();
    return 1;
}

/* Programmatic walk-to: same as double-click but by map/room number.
 * Returns 0=ok, 1=no map data, 2=room not found, 3=cur unknown,
 * 4=same room, 5=no path, 6=inject unavailable, 7=inject failed. */
static int mdw_walk_to(int map_num, int room_num)
{
    if (!mdw_loaded || !mdw_rooms || !mdw_exits) return 1;
    uint32_t dest_ri = mdw_room_lookup((uint16_t)map_num, (uint16_t)room_num);
    if (dest_ri == 0xFFFFFFFF) return 2;
    if (mdw_cur_ri == 0xFFFFFFFF || mdw_cur_ri >= mdw_room_count) return 3;
    if (dest_ri == mdw_cur_ri) return 4;

    rm_bridge_resolve();
    if (!pfn_dynpath_inject) return 6;

    MdwDynPath *dp = mdw_find_path(mdw_cur_ri, dest_ri);
    if (dp->error != MDW_DPERR_NONE) return 5;

    if (pfn_dynpath_set_label) {
        char lbl[128];
        wsprintfA(lbl, "%d,%d to %d,%d",
                  mdw_rooms[mdw_cur_ri].map, mdw_rooms[mdw_cur_ri].room,
                  mdw_rooms[dest_ri].map, mdw_rooms[dest_ri].room);
        pfn_dynpath_set_label(lbl);
    }
    if (pfn_dynpath_set_dest_room)
        pfn_dynpath_set_dest_room(mdw_rooms[dest_ri].map, mdw_rooms[dest_ri].room);
    int rv = pfn_dynpath_inject((dynpath_step_t *)dp->steps, dp->count,
                                mdw_rooms[mdw_cur_ri].checksum,
                                mdw_rooms[dest_ri].checksum);
    if (rv != 0) return 7;

    wsprintfA(mdw_walk_status, "Walking to %s (%d steps)",
              mdw_rooms[dest_ri].name, dp->count);
    mdw_walk_status_tick = GetTickCount();
    return 0;
}

static int mdw_wheel(int mx, int my, int delta)
{
    if (!mdw_visible) return 0;
    if (!mdw_hit_window(mx, my)) return 0;
    /* Save dialog area list scroll */
    if (mdw_save_showing && mdw_save_field == 2) {
        if (delta > 0) mdw_area_scroll -= 2;
        else mdw_area_scroll += 2;
        if (mdw_area_scroll < 0) mdw_area_scroll = 0;
        if (mdw_area_scroll > mdw_area_count - 6) mdw_area_scroll = mdw_area_count - 6;
        if (mdw_area_scroll < 0) mdw_area_scroll = 0;
        return 1;
    }
    /* Path panel scroll */
    if (mdw_show_path_panel && (float)mx >= mdw_pp_x0 && (float)mx <= mdw_pp_x1) {
        if (delta > 0) mdw_path_scroll += 3;
        else mdw_path_scroll -= 3;
        if (mdw_path_scroll < 0) mdw_path_scroll = 0;
        int max_scroll = (int)mdw_rec_n - 5;
        if (mdw_path_scroll > max_scroll) mdw_path_scroll = max_scroll;
        if (mdw_path_scroll < 0) mdw_path_scroll = 0;
        return 1;
    }
    float old = mdw_zoom;
    if (delta > 0) mdw_zoom *= 1.2f;
    else mdw_zoom /= 1.2f;
    if (mdw_zoom < 0.25f) mdw_zoom = 0.25f;
    if (mdw_zoom > 10.0f) mdw_zoom = 10.0f;
    /* Zoom about cursor: adjust view so the room under the cursor stays put. */
    float cview_x = mdw_x + mdw_w * 0.5f;
    float cview_y = mdw_y + mdw_h * 0.5f;
    float wx = mdw_view_x + ((float)mx - cview_x) / old;
    float wy = mdw_view_y + ((float)my - cview_y) / old;
    mdw_view_x = wx - ((float)mx - cview_x) / mdw_zoom;
    mdw_view_y = wy - ((float)my - cview_y) / mdw_zoom;
    return 1;
}

/* Numpad key handler while the map window is focused.
 * Numpad directional keys (1-9, 0, .) are NOT consumed here — they fall
 * through so numpad_handle can send walk commands to the MUD (MMExplorer
 * behavior: numpad = actual character movement, NOT map panning).
 * View auto-centers on current room via mdw_pulse_on_room when the
 * character moves. Zoom is via mouse wheel or +/-. */
static int mdw_key_down(unsigned int vk)
{
    if (!mdw_has_focus()) return 0;

    /* Text field focus: Enter/Backspace/Esc while typing in map/room boxes */
    if (mdw_input_focus == 1 || mdw_input_focus == 2) {
        char *buf = mdw_input_focus == 1 ? mdw_map_buf : mdw_room_buf;
        int cap = mdw_input_focus == 1 ? (int)sizeof(mdw_map_buf) : (int)sizeof(mdw_room_buf);
        if (vk == VK_BACK) {
            int n = (int)strlen(buf);
            if (n > 0) buf[n - 1] = 0;
            mdw_swallow_char = 1;
            return 1;
        }
        if (vk == VK_RETURN) {
            int mn = mdw_map_buf[0]  ? atoi(mdw_map_buf)  : (int)mdw_cur_map_view;
            int rn = mdw_room_buf[0] ? atoi(mdw_room_buf) : 0;
            if (rn > 0) mdw_goto_room(mn, rn);
            mdw_input_focus = 0;
            mdw_swallow_char = 1;
            return 1;
        }
        if (vk == VK_ESCAPE) {
            mdw_input_focus = 0;
            mdw_swallow_char = 1;
            return 1;
        }
        if (vk == VK_TAB) {
            mdw_input_focus = (mdw_input_focus == 1) ? 2 : 1;
            mdw_swallow_char = 1;
            return 1;
        }
        (void)cap;
        /* Let WM_CHAR do the digit insertion */
        return 0;
    }

    /* Save dialog captures all keys when visible */
    if (mdw_save_showing) {
        char *buf = NULL;
        int cap = 0;
        if (mdw_save_field == 0) { buf = mdw_save_title;  cap = (int)sizeof(mdw_save_title); }
        if (mdw_save_field == 1) { buf = mdw_save_name;   cap = 9; }
        if (mdw_save_field == 2) { buf = mdw_save_author; cap = (int)sizeof(mdw_save_author); }
        if (mdw_save_field == 3 && mdw_area_sel < 0) { buf = mdw_save_area; cap = (int)sizeof(mdw_save_area); }

        if (vk == VK_BACK && buf) {
            int n = (int)strlen(buf);
            if (n > 0) buf[n - 1] = 0;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_TAB) {
            mdw_save_field = (mdw_save_field + 1) % 4;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_RETURN) {
            mdw_save_path();
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_ESCAPE) {
            mdw_save_showing = 0;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_UP && mdw_save_field == 3) {
            if (mdw_area_sel > 0) mdw_area_sel--;
            else if (mdw_area_sel < 0 && mdw_area_count > 0) mdw_area_sel = mdw_area_count - 1;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_DOWN && mdw_save_field == 3) {
            if (mdw_area_sel < mdw_area_count - 1) mdw_area_sel++;
            else mdw_area_sel = -1;  /* back to custom */
            mdw_swallow_char = 1; return 1;
        }
        /* Let WM_CHAR handle text insertion */
        return 0;
    }
    /* Code Room dialog keyboard */
    if (mdw_code_showing) {
        if (vk == VK_RETURN) {
            if (strlen(mdw_code_input) == 4 && api && api->code_room) {
                int r = api->code_room(mdw_code_cksum, mdw_code_input,
                                       mdw_code_area[0] ? mdw_code_area : "Unknown",
                                       mdw_code_room_name);
                if (r == 0) {
                    wsprintfA(mdw_save_status, "Coded: %s", mdw_code_input);
                    mdw_save_status_tick = GetTickCount();
                    mdw_cur_room_code_ri = 0xFFFFFFFF;
                    mdw_code_showing = 0;
                } else if (r == 1) {
                    lstrcpyA(mdw_code_status, "Room already coded");
                } else {
                    wsprintfA(mdw_code_status, "Failed (%d)", r);
                }
            } else if (strlen(mdw_code_input) != 4) {
                lstrcpyA(mdw_code_status, "Code must be 4 characters");
            }
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_ESCAPE) {
            mdw_code_showing = 0;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_BACK) {
            int l = (int)strlen(mdw_code_input);
            if (l > 0) mdw_code_input[l - 1] = '\0';
            mdw_swallow_char = 1; return 1;
        }
        /* Let WM_CHAR handle text input */
        return 0;
    }
    /* Walk confirmation: Enter/R=Run, Esc=Cancel */
    if (mdw_confirm_showing) {
        if (vk == VK_RETURN || vk == 'R') {
            if (!mdw_verify_room()) {
                mdw_confirm_showing = 0;
                mdw_swallow_char = 1; return 1;
            }
            MdwDynPath *cdp = &mdw_dynpath_result;
            if (cdp->steps && cdp->count > 0 && pfn_dynpath_inject &&
                mdw_confirm_dest_ri < mdw_room_count) {
                if (api && api->inject_command) api->inject_command("stand");
                if (pfn_dynpath_set_label) {
                    char lbl[128];
                    wsprintfA(lbl, "%d,%d to %d,%d",
                              mdw_rooms[mdw_cur_ri].map, mdw_rooms[mdw_cur_ri].room,
                              mdw_rooms[mdw_confirm_dest_ri].map, mdw_rooms[mdw_confirm_dest_ri].room);
                    pfn_dynpath_set_label(lbl);
                }
                int rv = pfn_dynpath_inject(
                    (dynpath_step_t *)cdp->steps, cdp->count,
                    mdw_rooms[mdw_cur_ri].checksum,
                    mdw_rooms[mdw_confirm_dest_ri].checksum);
                if (rv == 0)
                    wsprintfA(mdw_walk_status, "Walking to %s (%d steps)",
                              mdw_rooms[mdw_confirm_dest_ri].name, cdp->count);
                else
                    wsprintfA(mdw_walk_status, "Inject failed (%d)", rv);
                mdw_walk_status_tick = GetTickCount();
            }
            mdw_confirm_showing = 0;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_ESCAPE) {
            mdw_confirm_showing = 0;
            mdw_swallow_char = 1; return 1;
        }
        mdw_swallow_char = 1; return 1;
    }
    /* Import prompt: I/Enter=Import, L=Loop, Esc=Cancel */
    if (mdw_import_prompt) {
        if (vk == 'I' || vk == VK_RETURN) {
            if (api && api->import_path && mdw_import_mp_path[0]) {
                int r = api->import_path(mdw_import_mp_path);
                if (r == 0)
                    wsprintfA(mdw_save_status, "Imported!");
                else
                    wsprintfA(mdw_save_status, "Import failed (%d)", r);
            } else {
                wsprintfA(mdw_save_status, "Import not available");
            }
            mdw_save_status_tick = GetTickCount();
            mdw_import_prompt = 0;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == 'L' && mdw_import_is_loop) {
            if (api && api->import_path && mdw_import_mp_path[0]) {
                int r = api->import_path(mdw_import_mp_path);
                if (r == 0 && api->fake_remote && mdw_import_title[0]) {
                    char cmd[160];
                    wsprintfA(cmd, "loop %s", mdw_import_title);
                    int lr = api->fake_remote(cmd);
                    if (lr == 0)
                        wsprintfA(mdw_save_status, "Imported + looping!");
                    else
                        wsprintfA(mdw_save_status, "Imported, loop start failed (%d)", lr);
                } else if (r == 0) {
                    wsprintfA(mdw_save_status, "Imported (loop unavailable)");
                } else {
                    wsprintfA(mdw_save_status, "Import failed (%d)", r);
                }
            } else {
                wsprintfA(mdw_save_status, "Import not available");
            }
            mdw_save_status_tick = GetTickCount();
            mdw_import_prompt = 0;
            mdw_swallow_char = 1; return 1;
        }
        if (vk == VK_ESCAPE) {
            mdw_import_prompt = 0;
            mdw_swallow_char = 1; return 1;
        }
        mdw_swallow_char = 1; return 1;
    }

    /* C = toggle recording */
    if (vk == 'C' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        mdw_rec_active = !mdw_rec_active;
        if (mdw_rec_active) {
            mdw_log("[PATH] Recording started\n");
        } else {
            mdw_log("[PATH] Recording stopped (%u steps)\n", mdw_rec_n);
        }
        mdw_swallow_char = 1;
        return 1;
    }

    /* S = open save dialog (if we have steps) */
    if (vk == 'S' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (mdw_rec_n > 0 && !mdw_save_showing) {
            mdw_save_dialog_open();
        }
        mdw_swallow_char = 1;
        return 1;
    }

    /* U = undo last step */
    if (vk == 'U' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        if (mdw_rec_n > 0) mdw_rec_undo();
        mdw_swallow_char = 1;
        return 1;
    }

    /* P = toggle path panel */
    if (vk == 'P' && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        mdw_show_path_panel = !mdw_show_path_panel;
        mdw_swallow_char = 1;
        return 1;
    }

    if (!(GetKeyState(VK_NUMLOCK) & 1)) return 0;

    /* Numpad walks the MAP when path panel has focus. If recording, also capture steps. */
    if (mdw_path_focus && mdw_cur_ri < mdw_room_count) {
        int dir = -1;
        switch (vk) {
        case VK_NUMPAD8: dir = 0; break; /* N  */
        case VK_NUMPAD2: dir = 1; break; /* S  */
        case VK_NUMPAD6: dir = 2; break; /* E  */
        case VK_NUMPAD4: dir = 3; break; /* W  */
        case VK_NUMPAD9: dir = 4; break; /* NE */
        case VK_NUMPAD7: dir = 5; break; /* NW */
        case VK_NUMPAD3: dir = 6; break; /* SE */
        case VK_NUMPAD1: dir = 7; break; /* SW */
        case VK_NUMPAD0: dir = 8; break; /* U  */
        case VK_DECIMAL: dir = 9; break; /* D  */
        }
        if (dir >= 0) {
            MdwRoom *r = &mdw_rooms[mdw_cur_ri];
            MdwExit *found = NULL;
            for (int i = 0; i < r->exit_count; i++) {
                MdwExit *ex = &mdw_exits[r->exit_ofs + i];
                if (ex->dir == dir) { found = ex; break; }
            }
            if (found) {
                uint32_t tri = mdw_room_lookup(found->tmap, found->troom);
                if (tri < mdw_room_count && tri != mdw_cur_ri) {
                    if (mdw_rec_active) {
                        if (mdw_rec_start_ri == 0xFFFFFFFF)
                            mdw_rec_start_ri = mdw_cur_ri;
                        mdw_rec_append(mdw_cur_ri, found);
                    }
                    if (mdw_rooms[tri].map != mdw_cur_map_view || mdw_rooms[tri].gx == INT16_MIN) {
                        for (uint32_t k = 0; k < mdw_room_count; k++)
                            mdw_rooms[k].gx = mdw_rooms[k].gy = INT16_MIN;
                        mdw_layout_bfs(mdw_rooms[tri].map, tri);
                        mdw_cur_map_view = mdw_rooms[tri].map;
                    }
                    mdw_cur_ri = tri;
                    mdw_pulse_tick = GetTickCount();
                    if (mdw_rooms[tri].gx != INT16_MIN) {
                        mdw_view_x = mdw_rooms[tri].gx * 20.0f;
                        mdw_view_y = mdw_rooms[tri].gy * 20.0f;
                    }
                }
            }
            mdw_swallow_char = 1;
            return 1;
        }
    }

    switch (vk) {
    case VK_ADD:      mdw_zoom *= 1.2f; if (mdw_zoom > 10.0f) mdw_zoom = 10.0f; mdw_swallow_char = 1; return 1;
    case VK_SUBTRACT: mdw_zoom /= 1.2f; if (mdw_zoom < 0.25f) mdw_zoom = 0.25f; mdw_swallow_char = 1; return 1;
    default: return 0;
    }
}

/* WM_CHAR router for the map-panel input boxes. Returns 1 if consumed. */
static int mdw_char_input(unsigned int ch)
{
    /* Code Room dialog text input — uppercase alpha only, 4 chars max */
    if (mdw_code_showing) {
        if (ch >= 32 && ch < 127) {
            char c = (char)ch;
            if (c >= 'a' && c <= 'z') c -= 32; /* uppercase */
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                int n = (int)strlen(mdw_code_input);
                if (n < 4) { mdw_code_input[n] = c; mdw_code_input[n + 1] = 0; }
            }
            return 1;
        }
        if (ch == '\b') {
            int n = (int)strlen(mdw_code_input);
            if (n > 0) mdw_code_input[n - 1] = 0;
            return 1;
        }
        return 1;
    }
    /* Save dialog text input */
    if (mdw_save_showing) {
        char *buf = NULL;
        int cap = 0;
        if (mdw_save_field == 0) { buf = mdw_save_title;  cap = (int)sizeof(mdw_save_title); }
        if (mdw_save_field == 1) { buf = mdw_save_name;   cap = 9; }
        if (mdw_save_field == 2) { buf = mdw_save_author; cap = (int)sizeof(mdw_save_author); }
        if (mdw_save_field == 3 && mdw_area_sel < 0) { buf = mdw_save_area; cap = (int)sizeof(mdw_save_area); }
        if (buf && ch >= 32 && ch < 127) {
            int n = (int)strlen(buf);
            if (n < cap - 1) { buf[n] = (char)ch; buf[n + 1] = 0; }
            return 1;
        }
        if (ch == '\b' && buf) {
            int n = (int)strlen(buf);
            if (n > 0) buf[n - 1] = 0;
            return 1;
        }
        return 1;  /* swallow all chars while dialog open */
    }

    if (mdw_input_focus != 1 && mdw_input_focus != 2) return 0;
    char *buf = mdw_input_focus == 1 ? mdw_map_buf : mdw_room_buf;
    int cap = mdw_input_focus == 1 ? (int)sizeof(mdw_map_buf) : (int)sizeof(mdw_room_buf);
    if (ch >= '0' && ch <= '9') {
        int n = (int)strlen(buf);
        if (n < cap - 1) { buf[n] = (char)ch; buf[n + 1] = 0; }
        return 1;
    }
    if (ch == '\b') {
        int n = (int)strlen(buf);
        if (n > 0) buf[n - 1] = 0;
        return 1;
    }
    /* Swallow anything else so typing stays inside the box */
    return 1;
}

static void mdw_toggle(void)
{
    mdw_visible = !mdw_visible;
    if (mdw_visible) {
        if (!mdw_loaded) mdw_try_load();
        if (mdw_loaded && mdw_map_count > 0)
            mdw_relayout(mdw_maps[0].map);
        mdw_focused = 1;
        mdw_send_rm();
        /* Center view on current room if we have one, otherwise fall back
         * to the centroid of the visible map so the user sees rooms
         * immediately instead of a blank canvas requiring pan-to-find.
         * In manual (auto-recenter off) mode we trust the saved view. */
        int centered = !mdw_auto_recenter;
        if (mdw_auto_recenter && mdw_cur_ri != 0xFFFFFFFF &&
            mdw_cur_ri < mdw_room_count &&
            mdw_rooms[mdw_cur_ri].gx != INT16_MIN) {
            mdw_view_x = mdw_rooms[mdw_cur_ri].gx * 20.0f;
            mdw_view_y = mdw_rooms[mdw_cur_ri].gy * 20.0f;
            centered = 1;
        }
        if (!centered && mdw_loaded) {
            double sx = 0, sy = 0; int n = 0;
            for (uint32_t i = 0; i < mdw_room_count; i++) {
                MdwRoom *rr = &mdw_rooms[i];
                if (rr->gx == INT16_MIN) continue;
                if (rr->map != mdw_cur_map_view) continue;
                sx += rr->gx; sy += rr->gy; n++;
            }
            if (n > 0) {
                mdw_view_x = (float)(sx / n) * 20.0f;
                mdw_view_y = (float)(sy / n) * 20.0f;
            }
        }
    } else {
        mdw_focused = 0;
    }
}

/* Worker thread: runs python mudmap_api.py export_bin, then reloads
 * mmud.bin. Runs off the vkt_wndproc thread so the Vulkan message loop
 * never blocks on the 30s wait — previously this killed the swapchain
 * and closed the terminal. */
static char mdw_worker_mdb[MAX_PATH];
static volatile int mdw_worker_busy = 0;

static DWORD WINAPI mdw_mdb_worker(LPVOID param)
{
    (void)param;
    char rooms_md[MAX_PATH];
    lstrcpynA(rooms_md, mdw_worker_mdb, MAX_PATH);
    char *last_sep = NULL;
    for (char *p = rooms_md; *p; p++) if (*p == '\\' || *p == '/') last_sep = p;
    if (last_sep) {
        *(last_sep + 1) = 0;
        lstrcatA(rooms_md, "Rooms.md");
        DWORD attr = GetFileAttributesA(rooms_md);
        if (attr == INVALID_FILE_ATTRIBUTES) rooms_md[0] = 0;
    } else {
        rooms_md[0] = 0;
    }

    CreateDirectoryA("mudmap", NULL);

    char cmd[2048];
    if (rooms_md[0])
        _snprintf(cmd, sizeof(cmd),
            "python mudmap\\mudmap_api.py export_bin \"%s\" mudmap\\mmud.bin \"%s\"",
            mdw_worker_mdb, rooms_md);
    else
        _snprintf(cmd, sizeof(cmd),
            "python mudmap\\mudmap_api.py export_bin \"%s\" mudmap\\mmud.bin",
            mdw_worker_mdb);
    cmd[sizeof(cmd) - 1] = 0;

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (api) api->log("[mudmap] running: %s\n", cmd);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        if (api) api->log("[mudmap] CreateProcess failed (err=%lu) — "
                          "python on PATH? mudmap\\mudmap_api.py present?\n",
                          GetLastError());
        mdw_worker_busy = 0;
        return 1;
    }
    DWORD wait_res = WaitForSingleObject(pi.hProcess, 60000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    if (wait_res == WAIT_TIMEOUT) {
        if (api) api->log("[mudmap] export timed out after 60s — killing python\n");
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exit_code != 0) {
        if (api) api->log("[mudmap] python export failed: exit=%lu. "
                          "Check that mudmap/*.py is deployed and "
                          "access_parser is installed.\n", exit_code);
        mdw_worker_busy = 0;
        return 1;
    }

    if (mdw_try_load() == 0) {
        if (mdw_map_count > 0) mdw_relayout(mdw_maps[0].map);
        mdw_visible = 1;
        mdw_focused = 1;
        mdw_send_rm();
        if (api) api->log("[mudmap] loaded %u rooms, %u maps\n",
                          mdw_room_count, mdw_map_count);
    } else {
        if (api) api->log("[mudmap] export succeeded but mmud.bin failed to load\n");
    }
    mdw_worker_busy = 0;
    return 0;
}

/* Callback invoked by the Vulkan file picker with the chosen MDB path.
 * Fires off a worker thread — returns immediately so the Vulkan message
 * loop keeps pumping and the swapchain stays alive. NULL = cancel. */
static void mdw_on_mdb_picked(const char *mdb_path)
{
    if (!mdb_path || !mdb_path[0]) return;
    if (mdw_worker_busy) return;
    lstrcpynA(mdw_worker_mdb, mdb_path, MAX_PATH);
    mdw_worker_busy = 1;
    HANDLE h = CreateThread(NULL, 0, mdw_mdb_worker, NULL, 0, NULL);
    if (h) CloseHandle(h);
    else   mdw_worker_busy = 0;
}

/* Opens the themed Vulkan file picker. Selection is handled async by
 * mdw_on_mdb_picked once the user hits OK (or cancels). */
static void mdw_pick_and_load_mdb(void)
{
    static const char *mdb_exts[] = {".mdb", ".accdb"};
    /* Start dir: MegaMUD install dir if discoverable, else CWD. */
    char start[MAX_PATH] = {0};
    GetCurrentDirectoryA(sizeof(start), start);
    fp_open("Select MegaMUD MDB (MDB2)", start, mdb_exts, 2, mdw_on_mdb_picked);
}

#endif /* VK_MUDMAP_H */
