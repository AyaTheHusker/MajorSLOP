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
 *   mdw_pulse_on_room(cksum)      — external trigger for glow pulse
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
static float    mdw_zoom = 2.5f;          /* 1.0 = default CELL px */

/* Current room + glow pulse */
static uint32_t mdw_cur_ri = 0xFFFFFFFF;
static DWORD    mdw_pulse_tick = 0;       /* GetTickCount() of most recent change */

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
    for (uint32_t i = 0; i < mdw_map_count; i++) free(mdw_maps[i].indices);
    free(mdw_maps); mdw_maps = NULL; mdw_map_count = 0;
    free(mdw_exits); mdw_exits = NULL; mdw_exit_count = 0;
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
        (version != 1 && version != 2 && version != 3))
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

static void mdw_pulse_on_room(uint32_t cksum)
{
    if (!mdw_loaded) return;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        if (mdw_rooms[i].checksum == cksum) {
            if (mdw_cur_ri != i) {
                mdw_cur_ri = i;
                mdw_pulse_tick = GetTickCount();
                if (mdw_rooms[i].map != mdw_cur_map_view)
                    mdw_relayout(mdw_rooms[i].map);
            }
            return;
        }
    }
}

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
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;

    /* Trivial clip */
    float x0 = cx - hs, y0 = cy - hs, x1 = cx + hs, y1 = cy + hs;
    if (x1 < cx0 || y1 < cy0 || x0 > cx1 || y0 > cy1) return;

    /* Glow halo — 3 expanding rings using glow_alpha */
    if (glow_alpha > 0.01f) {
        for (int i = 0; i < 3; i++) {
            float ring = hs + 3.0f + i * 4.0f;
            float a = glow_alpha * (0.35f - i * 0.10f);
            if (a <= 0.0f) continue;
            float gx0 = cx - ring, gy0 = cy - ring;
            float gx1 = cx + ring, gy1 = cy + ring;
            if (gx0 < cx0) gx0 = cx0; if (gy0 < cy0) gy0 = cy0;
            if (gx1 > cx1) gx1 = cx1; if (gy1 > cy1) gy1 = cy1;
            /* draw as a filled square; cheap but reads as a glow bloom */
            psolid(gx0, gy0, gx1, gy1,
                   r * 1.5f > 1.0f ? 1.0f : r * 1.5f,
                   g * 1.5f > 1.0f ? 1.0f : g * 1.5f,
                   b * 1.5f > 1.0f ? 1.0f : b * 1.5f,
                   a, vp_w, vp_h);
        }
    }

    /* Clip box itself */
    if (x0 < cx0) x0 = cx0; if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1; if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return;

    /* Dark outer shadow (1 px offset down-right) — pure depth cue */
    psolid(cx - hs + 1, cy - hs + 1, cx + hs + 2, cy + hs + 2,
           0.0f, 0.0f, 0.0f, 0.40f, vp_w, vp_h);

    /* Base fill */
    psolid(x0, y0, x1, y1, r, g, b, 0.96f, vp_w, vp_h);

    /* Top-left highlight bevel (2 px) */
    psolid(x0, y0, x1, y0 + 1, r * 1.4f > 1.0f ? 1.0f : r * 1.4f,
                              g * 1.4f > 1.0f ? 1.0f : g * 1.4f,
                              b * 1.4f > 1.0f ? 1.0f : b * 1.4f, 0.95f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1, y1, r * 1.3f > 1.0f ? 1.0f : r * 1.3f,
                              g * 1.3f > 1.0f ? 1.0f : g * 1.3f,
                              b * 1.3f > 1.0f ? 1.0f : b * 1.3f, 0.90f, vp_w, vp_h);
    psolid(x0 + 1, y0 + 1, x1 - 1, y0 + 2, 1.0f, 1.0f, 1.0f, 0.10f, vp_w, vp_h);

    /* Top gloss highlight (upper half, fading) */
    float gloss_h = (y1 - y0) * 0.45f;
    psolid(x0 + 1, y0 + 1, x1 - 1, y0 + 1 + gloss_h,
           1.0f, 1.0f, 1.0f, 0.14f, vp_w, vp_h);

    /* Bottom-right shadow bevel */
    psolid(x0, y1 - 1, x1, y1, r * 0.35f, g * 0.35f, b * 0.35f, 0.95f, vp_w, vp_h);
    psolid(x1 - 1, y0, x1, y1, r * 0.45f, g * 0.45f, b * 0.45f, 0.90f, vp_w, vp_h);
    psolid(x0 + 1, y1 - 2, x1 - 1, y1 - 1, 0.0f, 0.0f, 0.0f, 0.20f, vp_w, vp_h);

    /* Rounded corners: paint 1-px bg overdraw at each corner.
     * (Simulates a rounded look by removing the 4 corner pixels.) */
    psolid(x0, y0, x0 + 1, y0 + 1, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
    psolid(x1 - 1, y0, x1, y0 + 1, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
    psolid(x0, y1 - 1, x0 + 1, y1, bgr, bgg, bgb, 1.0f, vp_w, vp_h);
    psolid(x1 - 1, y1 - 1, x1, y1, bgr, bgg, bgb, 1.0f, vp_w, vp_h);

    /* Outer ring (1 px) — sits after corner cut so it wraps the tile cleanly */
    psolid(x0 + 1, y0, x1 - 1, y0 + 1, or_, og, ob, 1.0f, vp_w, vp_h);
    psolid(x0 + 1, y1 - 1, x1 - 1, y1, or_, og, ob, 1.0f, vp_w, vp_h);
    psolid(x0, y0 + 1, x0 + 1, y1 - 1, or_, og, ob, 1.0f, vp_w, vp_h);
    psolid(x1 - 1, y0 + 1, x1, y1 - 1, or_, og, ob, 1.0f, vp_w, vp_h);
}

/* Simple axis-aligned line via a 1-px quad, with clipping. */
static void mdw_line_h(float x0, float y0, float x1, float y1, float thick,
                       float r, float g, float b, float a,
                       float cx0, float cy0, float cx1, float cy1, int vp_w, int vp_h)
{
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    /* Draw as 2..4 short quads along the line (stepped, simple look) */
    int steps = (int)(len / 2.0f);
    if (steps < 2) steps = 2;
    if (steps > 64) steps = 64;
    for (int s = 0; s < steps; s++) {
        float t0 = (float)s / steps, t1 = (float)(s + 1) / steps;
        float px0 = x0 + dx * t0, py0 = y0 + dy * t0;
        float px1 = x0 + dx * t1, py1 = y0 + dy * t1;
        float qx0 = px0 < px1 ? px0 : px1;
        float qy0 = py0 < py1 ? py0 : py1;
        float qx1 = px0 > px1 ? px0 : px1;
        float qy1 = py0 > py1 ? py0 : py1;
        if (qx1 - qx0 < thick) qx1 = qx0 + thick;
        if (qy1 - qy0 < thick) qy1 = qy0 + thick;
        if (qx0 < cx0) qx0 = cx0; if (qy0 < cy0) qy0 = cy0;
        if (qx1 > cx1) qx1 = cx1; if (qy1 > cy1) qy1 = cy1;
        if (qx1 <= qx0 || qy1 <= qy0) continue;
        psolid(qx0, qy0, qx1, qy1, r, g, b, a, vp_w, vp_h);
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

    /* ---- Panel background (translucent — let starfield/plasma BG show) ---- */
    psolid(x0, y0, x1, y1,
           bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.55f, vp_w, vp_h);

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

    /* Title text */
    int title_tx = (int)x0 + pad;
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    char tbuf[96];
    if (mdw_loaded)
        _snprintf(tbuf, sizeof(tbuf), "Map Walker  \xB3  Map %u  \xB3  %u rooms",
                  (unsigned)mdw_cur_map_view, (unsigned)mdw_room_count);
    else
        _snprintf(tbuf, sizeof(tbuf), "Map Walker  \xB3  (no map loaded)");
    tbuf[sizeof(tbuf) - 1] = 0;
    ptext(title_tx + 1, title_ty + 1, tbuf, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext(title_tx, title_ty, tbuf, txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Close [X] */
    int close_tx = (int)x1 - pad - cw;
    ptext(close_tx, title_ty, "X", 1.0f, 0.3f, 0.3f, vp_w, vp_h, cw, ch);

    /* ---- Content area ---- */
    float c_x0 = x0 + 6, c_y0 = (float)tb_y1 + 3;
    float c_x1 = x1 - 6, c_y1 = y1 - 6;
    /* Content bg — translucent so Vulkan starfield/plasma BG shows through */
    psolid(c_x0, c_y0, c_x1, c_y1,
           bgr * 0.6f, bgg * 0.6f, bgb * 0.6f, 0.35f, vp_w, vp_h);
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

    /* Exits first */
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->gx == INT16_MIN) continue;
        if (r->map != mdw_cur_map_view) continue;
        float sx0 = cview_x + (r->gx * 20.0f - mdw_view_x) * zfac;
        float sy0 = cview_y + (r->gy * 20.0f - mdw_view_y) * zfac;
        for (uint32_t e = 0; e < r->exit_count; e++) {
            MdwExit *ex = &mdw_exits[r->exit_ofs + e];
            if (ex->dir > 7) continue;
            if (ex->tmap != mdw_cur_map_view) continue;
            uint32_t ni = mdw_room_lookup(ex->tmap, ex->troom);
            if (ni == 0xFFFFFFFF) continue;
            if (mdw_rooms[ni].gx == INT16_MIN) continue;
            float sx1 = cview_x + (mdw_rooms[ni].gx * 20.0f - mdw_view_x) * zfac;
            float sy1 = cview_y + (mdw_rooms[ni].gy * 20.0f - mdw_view_y) * zfac;
            float lr = 0.45f, lg = 0.45f, lb = 0.50f;
            if (ex->flags & 0x0001) { lr = 0.85f; lg = 0.55f; lb = 0.20f; }
            if (ex->flags & 0x0002) { lr = 0.35f; lg = 0.35f; lb = 0.35f; }
            if (ex->flags & 0x0004) { lr = 0.90f; lg = 0.15f; lb = 0.15f; }
            mdw_line_h(sx0, sy0, sx1, sy1, 1.5f, lr, lg, lb, 0.80f,
                       c_x0, c_y0, c_x1, c_y1, vp_w, vp_h);
        }
    }

    /* Rooms */
    float bgdark_r = bgr * 0.6f, bgdark_g = bgg * 0.6f, bgdark_b = bgb * 0.6f;
    for (uint32_t i = 0; i < mdw_room_count; i++) {
        MdwRoom *r = &mdw_rooms[i];
        if (r->gx == INT16_MIN) continue;
        if (r->map != mdw_cur_map_view) continue;
        float sx = cview_x + (r->gx * 20.0f - mdw_view_x) * zfac;
        float sy = cview_y + (r->gy * 20.0f - mdw_view_y) * zfac;
        if (sx + tile < c_x0 || sx - tile > c_x1) continue;
        if (sy + tile < c_y0 || sy - tile > c_y1) continue;

        float rc = 0.30f, gc = 0.55f, bc = 0.85f;
        if (r->shop)           { rc = 0.90f; gc = 0.80f; bc = 0.15f; }
        else if (r->npc)       { rc = 0.85f; gc = 0.20f; bc = 0.20f; }
        else if (r->spell)     { rc = 0.70f; gc = 0.30f; bc = 0.90f; }
        else if (r->lair_count){ rc = 0.90f; gc = 0.40f; bc = 0.10f; }

        float glow = 0.0f;
        if (i == mdw_cur_ri) glow = pulse;

        /* Outer ring: accent color on current; theme dim otherwise */
        float orng_r = acr, orng_g = acg, orng_b = acb;
        if (i != mdw_cur_ri) { orng_r = dmr; orng_g = dmg; orng_b = dmb; }

        mdw_tile_3d(sx, sy, tile, rc, gc, bc,
                    orng_r, orng_g, orng_b,
                    bgdark_r, bgdark_g, bgdark_b, glow,
                    c_x0, c_y0, c_x1, c_y1, vp_w, vp_h);

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
            /* Main glyph — bright yellow, printed twice (fake-bold) */
            ptext(tx,     ty, nb, 1.0f, 0.95f, 0.40f, vp_w, vp_h, cw, ch);
            ptext(tx + 1, ty, nb, 1.0f, 0.95f, 0.40f, vp_w, vp_h, cw, ch);
        }
    }

    /* Legend in bottom-left of content area */
    if (mdw_loaded && (c_y1 - c_y0) > 40) {
        int lx = (int)c_x0 + 6;
        int ly = (int)c_y1 - ch - 4;
        char lb[96];
        _snprintf(lb, sizeof(lb), "[%s FOCUS] +/- zoom  \xAFmpan\xAE  numpad=walk",
                  mdw_focused ? "WALKER" : "CLICK TO");
        lb[sizeof(lb) - 1] = 0;
        mdw_text_outlined(lx, ly, lb, dmr, dmg, dmb, 0.0f, 0.0f, 0.0f,
                          vp_w, vp_h, cw, ch);
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
    /* Content area = start pan */
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

static int mdw_wheel(int mx, int my, int delta)
{
    if (!mdw_visible) return 0;
    if (!mdw_hit_window(mx, my)) return 0;
    float old = mdw_zoom;
    if (delta > 0) mdw_zoom *= 1.2f;
    else mdw_zoom /= 1.2f;
    if (mdw_zoom < 0.25f) mdw_zoom = 0.25f;
    if (mdw_zoom > 4.0f) mdw_zoom = 4.0f;
    /* Zoom about cursor: adjust view so the room under the cursor stays put. */
    float cview_x = mdw_x + mdw_w * 0.5f;
    float cview_y = mdw_y + mdw_h * 0.5f;
    float wx = mdw_view_x + ((float)mx - cview_x) / old;
    float wy = mdw_view_y + ((float)my - cview_y) / old;
    mdw_view_x = wx - ((float)mx - cview_x) / mdw_zoom;
    mdw_view_y = wy - ((float)my - cview_y) / mdw_zoom;
    return 1;
}

/* Numpad walking. Called by the main WM_KEYDOWN handler BEFORE it dispatches
 * to the MUD. Returns 1 if consumed (so MegaMUD doesn't also see it).
 *
 * NumLock + numpad:
 *   8/2/6/4/9/7/3/1 = 8-way view pan (one tile each press)
 *   5              = center view on current room
 *   +/-            = zoom in/out */
static int mdw_key_down(unsigned int vk)
{
    if (!mdw_has_focus()) return 0;
    if (!(GetKeyState(VK_NUMLOCK) & 1)) return 0;
    int dx = 0, dy = 0;
    switch (vk) {
    case VK_NUMPAD8: dy = -1; break;
    case VK_NUMPAD2: dy =  1; break;
    case VK_NUMPAD6: dx =  1; break;
    case VK_NUMPAD4: dx = -1; break;
    case VK_NUMPAD9: dx =  1; dy = -1; break;
    case VK_NUMPAD7: dx = -1; dy = -1; break;
    case VK_NUMPAD3: dx =  1; dy =  1; break;
    case VK_NUMPAD1: dx = -1; dy =  1; break;
    case VK_NUMPAD5: case VK_CLEAR:
        if (mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count &&
            mdw_rooms[mdw_cur_ri].gx != INT16_MIN) {
            mdw_view_x = mdw_rooms[mdw_cur_ri].gx * 20.0f;
            mdw_view_y = mdw_rooms[mdw_cur_ri].gy * 20.0f;
        }
        mdw_swallow_char = 1;
        return 1;
    case VK_ADD:      mdw_zoom *= 1.2f; if (mdw_zoom > 4.0f) mdw_zoom = 4.0f;  mdw_swallow_char = 1; return 1;
    case VK_SUBTRACT: mdw_zoom /= 1.2f; if (mdw_zoom < 0.25f) mdw_zoom = 0.25f; mdw_swallow_char = 1; return 1;
    default: return 0;
    }
    /* Pan one tile in the chosen direction */
    mdw_view_x += dx * 20.0f;
    mdw_view_y += dy * 20.0f;
    mdw_swallow_char = 1;
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
        /* Center view on current room so user sees tiles immediately
         * instead of a blank canvas requiring pan-to-find. */
        if (mdw_cur_ri != 0xFFFFFFFF && mdw_cur_ri < mdw_room_count &&
            mdw_rooms[mdw_cur_ri].gx != INT16_MIN) {
            mdw_view_x = mdw_rooms[mdw_cur_ri].gx * 20.0f;
            mdw_view_y = mdw_rooms[mdw_cur_ri].gy * 20.0f;
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
