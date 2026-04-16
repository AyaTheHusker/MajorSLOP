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

/* Hover state — updated inside mdw_draw from vkm_mouse_x/y (always live,
 * not just during drag). The tooltip draws for mdw_hover_ri if valid. */
static uint32_t mdw_hover_ri = 0xFFFFFFFF;

/* Deferred recenter: while the user is hovering a tile (reading a tooltip),
 * player movement must not jerk the camera off under the cursor. Pending
 * target ri is stashed here and applied on the first frame hover clears. */
static uint32_t mdw_pending_recenter_ri = 0xFFFFFFFF;

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

/* Button hit rects (filled during draw, read during click). */
static float mdw_btn_rec_x0 = 0, mdw_btn_rec_x1 = 0;
static float mdw_btn_save_x0 = 0, mdw_btn_save_x1 = 0;
static float mdw_btn_clear_x0 = 0, mdw_btn_clear_x1 = 0;
static float mdw_btn_mpx_x0 = 0, mdw_btn_mpx_x1 = 0;
static float mdw_btn_panel_x0 = 0, mdw_btn_panel_x1 = 0;

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

#define MDW_EX_DOOR   0x0001
#define MDW_EX_HIDDEN 0x0002
#define MDW_EX_KEY    0x0008
#define MDW_EX_CAST   0x0400

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
}

/* ---- Bottom-bar controls ---- */
static int mdw_use_rm = 1;                /* auto-track current room from OFF_ROOM_CHECKSUM */
static char mdw_map_buf[8]  = "";
static char mdw_room_buf[8] = "";
static int  mdw_input_focus = 0;          /* 0=none, 1=map, 2=room */
/* Hit rects for the bottom control strip (filled every frame by mdw_draw) */
static float mdw_ctrl_y0 = 0, mdw_ctrl_y1 = 0;
static float mdw_cb_use_rm_x0 = 0,  mdw_cb_use_rm_x1 = 0;
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
        (version != 1 && version != 2 && version != 3 && version != 4))
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
        fseek(f, 4 + ec * 9, SEEK_CUR);
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
    /* Recorder: capture the transition FROM the previous room (mdw_cur_ri)
     * VIA the matching exit to this (map_num, room_num). If recording just
     * started, store the first room as the start anchor. */
    if (mdw_rec_active && changed) {
        if (mdw_rec_start_ri == 0xFFFFFFFF) mdw_rec_start_ri = mdw_cur_ri;
        if (mdw_cur_ri != 0xFFFFFFFF) {
            MdwExit *ex = mdw_exit_from_to(mdw_cur_ri,
                                           (uint16_t)map_num, (uint16_t)room_num);
            if (ex) mdw_rec_append(mdw_cur_ri, ex);
        }
    }
    if (mdw_rooms[ri].map != mdw_cur_map_view || mdw_rooms[ri].gx == INT16_MIN) {
        for (uint32_t k = 0; k < mdw_room_count; k++)
            mdw_rooms[k].gx = mdw_rooms[k].gy = INT16_MIN;
        mdw_layout_bfs(mdw_rooms[ri].map, ri);
        mdw_cur_map_view = mdw_rooms[ri].map;
        changed = 1;
    }
    mdw_cur_ri = ri;
    if (changed) {
        mdw_pulse_tick = GetTickCount();
        if (mdw_rooms[ri].gx != INT16_MIN) {
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

static void mdw_draw(int vp_w, int vp_h)
{
    if (!mdw_visible) return;
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

    /* Title text — "Map Walker │ Map N  Room M │ <room name>" */
    int title_tx = (int)x0 + pad;
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    char tbuf[160];
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
    /* Clip title to not overlap the close [X] */
    int title_max_cols = ((int)x1 - pad - cw - 4 - title_tx) / cw;
    if (title_max_cols > 0 && (int)strlen(tbuf) > title_max_cols)
        tbuf[title_max_cols] = 0;
    ptext(title_tx + 1, title_ty + 1, tbuf, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(title_tx, title_ty, tbuf, txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Close [X] */
    int close_tx = (int)x1 - pad - cw;
    ptext(close_tx, title_ty, "X", 1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);

    /* Reserve a fixed-height controls strip at the bottom for the checkbox,
     * map/room inputs, Set Room button, and zoom buttons. */
    int ctrl_h = ch + 10;
    float ctrl_y0 = y1 - 6 - (float)ctrl_h;
    float ctrl_y1 = y1 - 6;

    /* ---- Content area ---- */
    float c_x0 = x0 + 6, c_y0 = (float)tb_y1 + 3;
    float c_x1 = x1 - 6, c_y1 = ctrl_y0 - 2;
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
         * override below so it reads on the cyan player color). */
        float tr = 1.0f, tg = 0.95f, tb = 0.40f;

        float glow = 0.0f;
        if (i == mdw_cur_ri) {
            glow = pulse;
            /* Player's current room uses a fixed bright cyan regardless of
             * room type — so the "you are here" color is never confused
             * with the room's own type coloring (especially lair orange). */
            rc = 0.15f; gc = 0.90f; bc = 1.00f;
            tr = 0.02f; tg = 0.10f; tb = 0.35f;
        }

        /* Outer ring: accent color on current; theme dim otherwise */
        float orng_r = acr, orng_g = acg, orng_b = acb;
        if (i != mdw_cur_ri) { orng_r = dmr; orng_g = dmg; orng_b = dmb; }

        mdw_tile_3d(sx, sy, tile, rc, gc, bc,
                    orng_r, orng_g, orng_b,
                    bgdark_r, bgdark_g, bgdark_b, glow,
                    c_x0, c_y0, c_x1, c_y1, vp_w, vp_h);

        /* Special-room glyphs — skip for current-room tile to avoid clashing
         * with the cyan player highlight. */
        if (i != mdw_cur_ri) {
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
                int gx = (int)(sx - cw * 0.5f);
                int gy = (int)(sy - ch * 0.5f);
                ptext(gx - 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx + 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy - 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy + 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy, g, 0.4f, 1.0f, 0.6f, vp_w, vp_h, cw, ch);
                ptext(gx + 1, gy, g, 0.4f, 1.0f, 0.6f, vp_w, vp_h, cw, ch);
            } else if (r->shop && tile >= 5.0f) {
                /* Shop → $ glyph, bold with halo */
                const char *g = "$";
                int gx = (int)(sx - cw * 0.5f);
                int gy = (int)(sy - ch * 0.5f);
                ptext(gx - 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx + 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy - 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy + 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy, g, 1.0f, 1.0f, 0.2f, vp_w, vp_h, cw, ch);
                ptext(gx + 1, gy, g, 1.0f, 1.0f, 0.2f, vp_w, vp_h, cw, ch);
            } else if (r->spell && !r->lair_count && tile >= 5.0f) {
                /* Spell/trainer room → "+" */
                const char *g = "+";
                int gx = (int)(sx - cw * 0.5f);
                int gy = (int)(sy - ch * 0.5f);
                ptext(gx - 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx + 1, gy, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy - 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy + 1, g, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
                ptext(gx, gy, g, 1.0f, 1.0f, 1.0f, vp_w, vp_h, cw, ch);
                ptext(gx + 1, gy, g, 1.0f, 1.0f, 1.0f, vp_w, vp_h, cw, ch);
            }
        }

        /* Lair size (monster cap) — bold, drop-shadowed, centered on tile */
        if (r->lair_count) {
            char nb[8]; wsprintfA(nb, "%u", r->lair_count);
            int nlen = (int)strlen(nb);
            int nw = nlen * cw + 1; /* +1 for fake-bold double print */
            int tx = (int)(sx - nw * 0.5f);
            int ty = (int)(sy - ch * 0.5f);
            /* Drop shadow (2px down/right, black) */
            ptext(tx + 2, ty + 2, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            ptext(tx + 3, ty + 2, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            /* Black halo — 4 directions */
            ptext(tx - 1, ty,     nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            ptext(tx + 1, ty,     nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            ptext(tx,     ty - 1, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            ptext(tx,     ty + 1, nb, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
            /* Main glyph */
            ptext(tx,     ty, nb, tr, tg, tb, vp_w, vp_h, cw, ch);
            ptext(tx + 1, ty, nb, tr, tg, tb, vp_w, vp_h, cw, ch);
        }
    }
    mdw_hover_ri = hover_ri;

    /* Hover just cleared? Flush any queued recenter so the camera catches
     * up with the player's current room. */
    if (mdw_hover_ri == 0xFFFFFFFF && mdw_pending_recenter_ri != 0xFFFFFFFF
        && mdw_pending_recenter_ri < mdw_room_count) {
        MdwRoom *pr = &mdw_rooms[mdw_pending_recenter_ri];
        if (pr->gx != INT16_MIN) {
            mdw_view_x = pr->gx * 20.0f;
            mdw_view_y = pr->gy * 20.0f;
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

    /* ---- Hover tooltip ---- */
    if (mdw_hover_ri != 0xFFFFFFFF && mdw_hover_ri < mdw_room_count) {
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
    /* Content area = start pan (and drop input focus) */
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
    float c_x0 = x0 + 6, c_y0 = tb_y1 + 3;
    float c_x1 = x1 - 6, c_y1 = y1 - 6;
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

static int mdw_wheel(int mx, int my, int delta)
{
    if (!mdw_visible) return 0;
    if (!mdw_hit_window(mx, my)) return 0;
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

    if (!(GetKeyState(VK_NUMLOCK) & 1)) return 0;
    switch (vk) {
    case VK_ADD:      mdw_zoom *= 1.2f; if (mdw_zoom > 10.0f) mdw_zoom = 10.0f; mdw_swallow_char = 1; return 1;
    case VK_SUBTRACT: mdw_zoom /= 1.2f; if (mdw_zoom < 0.25f) mdw_zoom = 0.25f; mdw_swallow_char = 1; return 1;
    default: return 0;
    }
}

/* WM_CHAR router for the map-panel input boxes. Returns 1 if consumed. */
static int mdw_char_input(unsigned int ch)
{
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
        /* Center view on current room if we have one, otherwise fall back
         * to the centroid of the visible map so the user sees rooms
         * immediately instead of a blank canvas requiring pan-to-find. */
        int centered = 0;
        if (mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count &&
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
