/* vft_text.c — Vulkan Floating Text System
 * High-quality TTF-rendered floating text with extensive effects.
 * #included into vk_terminal.c — has access to all rendering globals.
 *
 * MMUDPy API: vft_command("text|x|y|param=val|...")
 * All rendering uses the UI font atlas (TTF, antialiased).
 */

/* ---- Constants & Enums ---- */

#define VFT_MAX_ENTRIES   64
#define VFT_MAX_TEXT      256
#define VFT_MAX_PARTICLES 512
#define VFT_MAX_TRAIL     16

/* Intro/outro effect types */
#define VFT_FX_NONE       0
#define VFT_FX_FADE       1
#define VFT_FX_ZOOM_IN    2   /* scale big → normal (intro) or normal → small (outro) */
#define VFT_FX_ZOOM_OUT   3   /* scale small → normal (intro) or normal → big+fade (outro) */
#define VFT_FX_SLIDE_L    4   /* slide from/to left */
#define VFT_FX_SLIDE_R    5
#define VFT_FX_SLIDE_U    6
#define VFT_FX_SLIDE_D    7
#define VFT_FX_BOUNCE     8   /* drop + bounce */
#define VFT_FX_POP        9   /* quick scale overshoot 0 → 1.3 → 1 */
#define VFT_FX_TYPEWRITER 10  /* chars appear one by one */
#define VFT_FX_SHATTER    11  /* per-char physics explosion */
#define VFT_FX_EXPLODE    12  /* radial burst outward */
#define VFT_FX_MELT       13  /* chars drip down */
#define VFT_FX_DISSOLVE   14  /* random chars vanish */
#define VFT_FX_EVAPORATE  15  /* float up + fade */
#define VFT_FX_WAVE_IN    16  /* wave sweep across chars */
#define VFT_FX_SPIN       17  /* rotate + scale in/out */
#define VFT_FX_COUNT      18

static const char *vft_fx_names[VFT_FX_COUNT] = {
    "none","fade","zoom_in","zoom_out","slide_l","slide_r","slide_u","slide_d",
    "bounce","pop","typewriter","shatter","explode","melt","dissolve",
    "evaporate","wave_in","spin"
};

/* Modifier flags (combinable) */
#define VFT_MOD_GLOW      0x0001
#define VFT_MOD_PULSE     0x0002
#define VFT_MOD_SHAKE     0x0004
#define VFT_MOD_WAVE      0x0008
#define VFT_MOD_RAINBOW   0x0010
#define VFT_MOD_SHADOW    0x0020
#define VFT_MOD_OUTLINE   0x0040
#define VFT_MOD_TRAIL     0x0080
#define VFT_MOD_ORBIT     0x0100
#define VFT_MOD_SPARKS    0x0200
#define VFT_MOD_HELIX     0x0400
#define VFT_MOD_CHROMATIC 0x0800
#define VFT_MOD_FIRE      0x1000
#define VFT_MOD_BREATHE   0x2000

/* ---- Data Structures ---- */

typedef struct {
    float x, y, vx, vy;
    float rot, vrot;
    float alpha, scale;
    float r, g, b;
    float life, max_life;
    int   active;
} vft_part_t;

typedef struct {
    float x, y;         /* offset from entry origin */
    float vx, vy;       /* velocity (shatter/explode) */
    float rot, vrot;    /* rotation */
    float alpha, scale;
    int   ch;           /* character codepoint */
    float base_x;       /* original x offset from center */
    float base_y;       /* original y offset */
    float dissolve_t;   /* random dissolve threshold */
} vft_char_t;

typedef struct {
    char  text[VFT_MAX_TEXT];
    int   len;
    float x, y;           /* 0–1 normalized screen position */
    float size;           /* scale multiplier (1.0 = 14×28 base cell) */
    float r, g, b, a;    /* base RGBA color */
    float elapsed;        /* seconds since spawn */
    float intro_dur;      /* intro phase duration */
    float hold_dur;       /* hold phase duration */
    float outro_dur;      /* outro phase duration */
    int   intro_fx;       /* intro effect ID */
    int   outro_fx;       /* outro effect ID */
    int   mods;           /* VFT_MOD_* flags */
    /* glow */
    float glow_r, glow_g, glow_b, glow_size;
    /* shadow */
    float shadow_ox, shadow_oy, shadow_a;
    /* outline */
    float out_r, out_g, out_b, out_w;
    /* shake */
    float shake_intensity;
    /* wave */
    float wave_amp, wave_freq;
    /* per-char state */
    vft_char_t chars[VFT_MAX_TEXT];
    int   shattered;
    /* trail history */
    float trail_x[VFT_MAX_TRAIL], trail_y[VFT_MAX_TRAIL];
    float trail_a[VFT_MAX_TRAIL], trail_s[VFT_MAX_TRAIL];
    int   trail_head, trail_count;
    /* bookkeeping */
    int   active;
    int   id;
} vft_entry_t;

static vft_entry_t  vft_pool[VFT_MAX_ENTRIES];
static vft_part_t   vft_parts[VFT_MAX_PARTICLES];
/* vft_quad_start, vft_quad_end declared in vk_terminal.c (forward decl) */
static DWORD        vft_last_tick;
static int          vft_next_id = 1;
static float        vft_global_time;

/* ---- Easing Functions ---- */

static float vft_ease_out_cubic(float t) {
    t = 1.0f - t; return 1.0f - t * t * t;
}
static float vft_ease_out_elastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return sinf(-13.0f * 1.5707963f * (t + 1.0f)) * powf(2.0f, -10.0f * t) + 1.0f;
}
static float vft_ease_out_bounce(float t) {
    if (t < 1.0f / 2.75f) return 7.5625f * t * t;
    if (t < 2.0f / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
    if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
    t -= 2.625f / 2.75f; return 7.5625f * t * t + 0.984375f;
}
static float vft_ease_in_out_quad(float t) {
    return t < 0.5f ? 2.0f * t * t : 1.0f - powf(-2.0f * t + 2.0f, 2.0f) * 0.5f;
}
static float vft_ease_overshoot(float t) {
    /* 0→1.2→1 */
    if (t < 0.6f) return (t / 0.6f) * 1.25f;
    return 1.25f - 0.25f * ((t - 0.6f) / 0.4f);
}

/* ---- Simple PRNG for deterministic randomness ---- */
static unsigned int vft_rng_state = 12345;
static float vft_randf(void) {
    vft_rng_state = vft_rng_state * 1103515245u + 12345u;
    return (float)(vft_rng_state >> 16 & 0x7FFF) / 32767.0f;
}
static float vft_randf_range(float lo, float hi) {
    return lo + vft_randf() * (hi - lo);
}

/* ---- Per-Character Rendering ---- */

/* Push a single character quad with arbitrary position, scale, rotation, color, alpha.
 * cx, cy = center in pixels. scale = multiplier on 14×28 base cell.
 * Uses ui_atlas UVs. */
static void vft_push_char(int ch, float cx, float cy, float char_scale,
                           float rot, float r, float g, float b, float a,
                           int vp_w, int vp_h)
{
    if (quad_count >= MAX_QUADS) return;
    if (ch <= 32 || ch > 255) return;
    if (a <= 0.001f) return;

    /* On-screen base size — independent of atlas resolution.
     * VFT display fonts are 1:2 aspect ratio (like the atlas cells). */
    float cw = 14.0f * char_scale * ui_scale;
    float ch_h = 28.0f * char_scale * ui_scale;
    float hw = cw * 0.5f, hh = ch_h * 0.5f;

    /* UV from 16×16 grid (same layout for both atlases) */
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    uint32_t aw = vft_font_ready ? vft_atlas_w : ui_atlas_w;
    uint32_t ah = vft_font_ready ? vft_atlas_h : ui_atlas_h;
    float hp_u = 0.5f / (float)aw, hp_v = 0.5f / (float)ah;
    float u0 = (ch % 16) * tex_cw + hp_u;
    float v0 = (ch / 16) * tex_ch + hp_v;
    float u1 = u0 + tex_cw - 2.0f * hp_u;
    float v1 = v0 + tex_ch - 2.0f * hp_v;

    /* Rotated corners around center */
    float cs = cosf(rot), sn = sinf(rot);

    int vi = quad_count * 4;
    float sx = 2.0f / (float)vp_w, sy = 2.0f / (float)vp_h;

    /* TL, TR, BR, BL (quad winding) */
    float corners[4][2] = {
        { -hw, -hh }, { hw, -hh }, { hw, hh }, { -hw, hh }
    };
    float uvs[4][2] = {
        { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 }
    };
    for (int i = 0; i < 4; i++) {
        float rx = corners[i][0] * cs - corners[i][1] * sn + cx;
        float ry = corners[i][0] * sn + corners[i][1] * cs + cy;
        float nx = rx * sx - 1.0f;
        float ny = ry * sy - 1.0f;
        vk_vdata[vi + i] = (vertex_t){ nx, ny, uvs[i][0], uvs[i][1], r, g, b, a };
    }
    quad_count++;
}

/* Push a solid rectangle (for particles, backgrounds) */
static void vft_push_solid(float cx, float cy, float w, float h,
                            float r, float g, float b, float a,
                            int vp_w, int vp_h)
{
    if (quad_count >= MAX_QUADS || a <= 0.001f) return;
    /* Use glyph 219 (full block) center for solid fill */
    float tex_cw = 1.0f / 16.0f, tex_ch = 1.0f / 16.0f;
    uint32_t aw = vft_font_ready ? vft_atlas_w : ui_atlas_w;
    uint32_t ah = vft_font_ready ? vft_atlas_h : ui_atlas_h;
    float hp_u = 0.5f / (float)aw, hp_v = 0.5f / (float)ah;
    float su = (219 % 16) * tex_cw + tex_cw * 0.5f;
    float sv = (219 / 16) * tex_ch + tex_ch * 0.5f;

    float sx = 2.0f / (float)vp_w, sy = 2.0f / (float)vp_h;
    float x0 = (cx - w * 0.5f) * sx - 1.0f;
    float y0 = (cy - h * 0.5f) * sy - 1.0f;
    float x1 = (cx + w * 0.5f) * sx - 1.0f;
    float y1 = (cy + h * 0.5f) * sy - 1.0f;

    int vi = quad_count * 4;
    vk_vdata[vi+0] = (vertex_t){ x0, y0, su, sv, r, g, b, a };
    vk_vdata[vi+1] = (vertex_t){ x1, y0, su, sv, r, g, b, a };
    vk_vdata[vi+2] = (vertex_t){ x1, y1, su, sv, r, g, b, a };
    vk_vdata[vi+3] = (vertex_t){ x0, y1, su, sv, r, g, b, a };
    quad_count++;
}

/* Push a full string centered at (cx, cy) with scale and base rotation */
static void vft_push_str(const char *str, int len, float cx, float cy,
                          float char_scale, float rot,
                          float r, float g, float b, float a,
                          int vp_w, int vp_h)
{
    float cw = 14.0f * char_scale * ui_scale;
    float total_w = (float)len * cw;
    float start_x = cx - total_w * 0.5f + cw * 0.5f;
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch > 32)
            vft_push_char(ch, start_x + i * cw, cy, char_scale, rot,
                          r, g, b, a, vp_w, vp_h);
    }
}

/* ---- Particle System ---- */

static void vft_spawn_particle(float x, float y, float vx, float vy,
                                float r, float g, float b, float life, float scale)
{
    for (int i = 0; i < VFT_MAX_PARTICLES; i++) {
        if (!vft_parts[i].active) {
            vft_part_t *p = &vft_parts[i];
            p->x = x; p->y = y;
            p->vx = vx; p->vy = vy;
            p->rot = vft_randf() * 6.283f;
            p->vrot = vft_randf_range(-8.0f, 8.0f);
            p->alpha = 1.0f;
            p->scale = scale;
            p->r = r; p->g = g; p->b = b;
            p->life = life; p->max_life = life;
            p->active = 1;
            return;
        }
    }
}

static void vft_spawn_burst(float cx, float cy, int count,
                             float r, float g, float b,
                             float speed, float life, float scale)
{
    for (int i = 0; i < count; i++) {
        float angle = vft_randf() * 6.283f;
        float spd = speed * vft_randf_range(0.3f, 1.0f);
        vft_spawn_particle(cx, cy,
                           cosf(angle) * spd, sinf(angle) * spd,
                           r, g, b, life * vft_randf_range(0.5f, 1.0f), scale);
    }
}

static void vft_update_particles(float dt, int vp_w, int vp_h)
{
    for (int i = 0; i < VFT_MAX_PARTICLES; i++) {
        vft_part_t *p = &vft_parts[i];
        if (!p->active) continue;
        p->life -= dt;
        if (p->life <= 0.0f) { p->active = 0; continue; }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy += 200.0f * ui_scale * dt; /* gravity */
        p->rot += p->vrot * dt;
        p->alpha = p->life / p->max_life;
        /* bounce off screen edges */
        if (p->x < 0)      { p->x = 0;              p->vx *= -0.6f; }
        if (p->x > vp_w)   { p->x = (float)vp_w;    p->vx *= -0.6f; }
        if (p->y > vp_h)   { p->y = (float)vp_h;    p->vy *= -0.5f; }
        if (p->y < 0)      { p->y = 0;               p->vy *= -0.6f; }
    }
}

static void vft_draw_particles(int vp_w, int vp_h)
{
    for (int i = 0; i < VFT_MAX_PARTICLES; i++) {
        vft_part_t *p = &vft_parts[i];
        if (!p->active) continue;
        float sz = 4.0f * ui_scale * p->scale;
        vft_push_solid(p->x, p->y, sz, sz,
                       p->r, p->g, p->b, p->alpha * 0.9f,
                       vp_w, vp_h);
    }
}

/* ---- Shatter Init ---- */

static void vft_init_shatter(vft_entry_t *e, int vp_w, int vp_h)
{
    if (e->shattered) return;
    e->shattered = 1;
    float cw = 14.0f * e->size * ui_scale;
    float ch_h = 28.0f * e->size * ui_scale;
    float px_x = e->x * (float)vp_w;
    float px_y = e->y * (float)vp_h;
    float total_w = (float)e->len * cw;
    float start_x = px_x - total_w * 0.5f + cw * 0.5f;

    for (int i = 0; i < e->len; i++) {
        vft_char_t *c = &e->chars[i];
        c->ch = (unsigned char)e->text[i];
        c->base_x = start_x + i * cw - px_x;
        c->base_y = 0.0f;
        c->x = c->base_x;
        c->y = c->base_y;
        /* random velocity outward from center */
        float dx = c->base_x;
        float angle = atan2f(vft_randf() - 0.5f, dx != 0 ? dx : 0.001f);
        float spd = vft_randf_range(150.0f, 500.0f) * ui_scale;
        c->vx = cosf(angle) * spd + dx * 2.0f;
        c->vy = -vft_randf_range(100.0f, 400.0f) * ui_scale;
        c->rot = 0.0f;
        c->vrot = vft_randf_range(-12.0f, 12.0f);
        c->alpha = 1.0f;
        c->scale = 1.0f;
    }
    /* spawn particle burst at text center */
    vft_spawn_burst(px_x, px_y, 30, e->r, e->g, e->b,
                    400.0f * ui_scale, 1.5f, e->size * 0.5f);
}

static void vft_init_explode(vft_entry_t *e, int vp_w, int vp_h)
{
    if (e->shattered) return;
    e->shattered = 1;
    float cw = 14.0f * e->size * ui_scale;
    float px_x = e->x * (float)vp_w;
    float px_y = e->y * (float)vp_h;
    float total_w = (float)e->len * cw;
    float start_x = px_x - total_w * 0.5f + cw * 0.5f;

    for (int i = 0; i < e->len; i++) {
        vft_char_t *c = &e->chars[i];
        c->ch = (unsigned char)e->text[i];
        c->base_x = start_x + i * cw - px_x;
        c->base_y = 0.0f;
        c->x = c->base_x;
        c->y = c->base_y;
        float angle = ((float)i / (float)e->len) * 6.283f + vft_randf() * 0.5f;
        float spd = vft_randf_range(200.0f, 600.0f) * ui_scale;
        c->vx = cosf(angle) * spd;
        c->vy = sinf(angle) * spd;
        c->rot = 0; c->vrot = vft_randf_range(-15.0f, 15.0f);
        c->alpha = 1.0f; c->scale = 1.0f;
    }
    vft_spawn_burst(px_x, px_y, 40, e->r, e->g, e->b,
                    500.0f * ui_scale, 2.0f, e->size * 0.6f);
}

static void vft_init_melt(vft_entry_t *e, int vp_w, int vp_h)
{
    if (e->shattered) return;
    e->shattered = 1;
    float cw = 14.0f * e->size * ui_scale;
    float px_x = e->x * (float)vp_w;
    float total_w = (float)e->len * cw;
    float start_x = px_x - total_w * 0.5f + cw * 0.5f;

    for (int i = 0; i < e->len; i++) {
        vft_char_t *c = &e->chars[i];
        c->ch = (unsigned char)e->text[i];
        c->base_x = start_x + i * cw - px_x;
        c->base_y = 0.0f;
        c->x = c->base_x; c->y = 0.0f;
        c->vx = vft_randf_range(-20.0f, 20.0f) * ui_scale;
        c->vy = vft_randf_range(80.0f, 300.0f) * ui_scale;
        c->rot = 0; c->vrot = vft_randf_range(-3.0f, 3.0f);
        c->alpha = 1.0f; c->scale = 1.0f;
    }
}

/* ---- Effect Evaluation ---- */

/* Returns effective transform for the whole text during intro phase.
 * out: ox,oy offset (pixels), oscale, oalpha, orot */
static void vft_eval_intro(vft_entry_t *e, float t, int vp_w, int vp_h,
                            float *ox, float *oy, float *oscale, float *oalpha, float *orot)
{
    *ox = 0; *oy = 0; *oscale = 1.0f; *oalpha = 1.0f; *orot = 0;
    float et = (e->intro_dur > 0) ? t / e->intro_dur : 1.0f;
    if (et > 1.0f) et = 1.0f;

    switch (e->intro_fx) {
    case VFT_FX_FADE:
        *oalpha = vft_ease_out_cubic(et);
        break;
    case VFT_FX_ZOOM_IN:
        { float s = 3.0f - 2.0f * vft_ease_out_elastic(et);
          *oscale = s; *oalpha = vft_ease_out_cubic(et); }
        break;
    case VFT_FX_ZOOM_OUT:
        { float s = vft_ease_out_elastic(et);
          *oscale = s; *oalpha = vft_ease_out_cubic(et); }
        break;
    case VFT_FX_SLIDE_L:
        *ox = -(1.0f - vft_ease_out_cubic(et)) * (float)vp_w * 0.6f;
        *oalpha = vft_ease_out_cubic(et);
        break;
    case VFT_FX_SLIDE_R:
        *ox = (1.0f - vft_ease_out_cubic(et)) * (float)vp_w * 0.6f;
        *oalpha = vft_ease_out_cubic(et);
        break;
    case VFT_FX_SLIDE_U:
        *oy = -(1.0f - vft_ease_out_cubic(et)) * (float)vp_h * 0.5f;
        *oalpha = vft_ease_out_cubic(et);
        break;
    case VFT_FX_SLIDE_D:
        *oy = (1.0f - vft_ease_out_cubic(et)) * (float)vp_h * 0.5f;
        *oalpha = vft_ease_out_cubic(et);
        break;
    case VFT_FX_BOUNCE:
        *oy = -(1.0f - vft_ease_out_bounce(et)) * (float)vp_h * 0.4f;
        break;
    case VFT_FX_POP:
        *oscale = vft_ease_overshoot(et);
        *oalpha = et < 0.1f ? et * 10.0f : 1.0f;
        break;
    case VFT_FX_WAVE_IN:
        *oalpha = vft_ease_out_cubic(et);
        *oscale = vft_ease_out_elastic(et);
        break;
    case VFT_FX_SPIN:
        *orot = (1.0f - vft_ease_out_cubic(et)) * 6.283f;
        *oscale = vft_ease_out_elastic(et);
        *oalpha = vft_ease_out_cubic(et);
        break;
    case VFT_FX_TYPEWRITER:
        *oalpha = 1.0f; /* per-char handled in draw */
        break;
    default: break;
    }
}

static void vft_eval_outro(vft_entry_t *e, float t, int vp_w, int vp_h,
                            float *ox, float *oy, float *oscale, float *oalpha, float *orot)
{
    *ox = 0; *oy = 0; *oscale = 1.0f; *oalpha = 1.0f; *orot = 0;
    float outro_start = e->intro_dur + e->hold_dur;
    float ot = (e->outro_dur > 0) ? (t - outro_start) / e->outro_dur : 1.0f;
    if (ot < 0.0f) ot = 0.0f;
    if (ot > 1.0f) ot = 1.0f;

    switch (e->outro_fx) {
    case VFT_FX_FADE:
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_ZOOM_IN:
        *oscale = 1.0f - ot * 0.8f;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_ZOOM_OUT:
        *oscale = 1.0f + ot * 2.0f;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_SLIDE_L:
        *ox = -ot * (float)vp_w * 0.6f;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_SLIDE_R:
        *ox = ot * (float)vp_w * 0.6f;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_SLIDE_U:
        *oy = -ot * (float)vp_h * 0.5f;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_SLIDE_D:
        *oy = ot * (float)vp_h * 0.5f;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_EVAPORATE:
        *oy = -ot * 80.0f * ui_scale;
        *oalpha = 1.0f - ot;
        *oscale = 1.0f + ot * 0.3f;
        break;
    case VFT_FX_SPIN:
        *orot = ot * 6.283f;
        *oscale = 1.0f - ot;
        *oalpha = 1.0f - ot;
        break;
    case VFT_FX_SHATTER:
    case VFT_FX_EXPLODE:
    case VFT_FX_MELT:
    case VFT_FX_DISSOLVE:
        /* handled per-char in draw */
        break;
    default: break;
    }
}

/* ---- HSV helpers for rainbow ---- */

static void vft_hsv2rgb(float h, float s, float v, float *r, float *g, float *b)
{
    int hi = (int)(h / 60.0f) % 6;
    float f = h / 60.0f - (float)hi;
    float p = v * (1.0f - s);
    float q = v * (1.0f - f * s);
    float t_ = v * (1.0f - (1.0f - f) * s);
    switch (hi) {
    case 0: *r = v;  *g = t_; *b = p;  break;
    case 1: *r = q;  *g = v;  *b = p;  break;
    case 2: *r = p;  *g = v;  *b = t_; break;
    case 3: *r = p;  *g = q;  *b = v;  break;
    case 4: *r = t_; *g = p;  *b = v;  break;
    default:*r = v;  *g = p;  *b = q;  break;
    }
}

/* ---- Draw a Single Entry ---- */

static void vft_draw_entry(vft_entry_t *e, float dt, int vp_w, int vp_h)
{
    float t = e->elapsed;
    float total = e->intro_dur + e->hold_dur + e->outro_dur;
    float px_x = e->x * (float)vp_w;
    float px_y = e->y * (float)vp_h;

    /* Determine phase */
    int phase;
    if (t < e->intro_dur) phase = 0; /* intro */
    else if (t < e->intro_dur + e->hold_dur) phase = 1; /* hold */
    else if (t < total) phase = 2; /* outro */
    else { e->active = 0; return; }

    /* Evaluate intro/outro transforms */
    float ox = 0, oy = 0, oscale = 1.0f, oalpha = 1.0f, orot = 0;
    if (phase == 0) {
        vft_eval_intro(e, t, vp_w, vp_h, &ox, &oy, &oscale, &oalpha, &orot);
    } else if (phase == 2) {
        vft_eval_outro(e, t, vp_w, vp_h, &ox, &oy, &oscale, &oalpha, &orot);
        /* Init per-char physics if needed */
        if (e->outro_fx == VFT_FX_SHATTER) vft_init_shatter(e, vp_w, vp_h);
        else if (e->outro_fx == VFT_FX_EXPLODE) vft_init_explode(e, vp_w, vp_h);
        else if (e->outro_fx == VFT_FX_MELT) vft_init_melt(e, vp_w, vp_h);
    }

    /* Apply modifiers */
    float mod_ox = 0, mod_oy = 0;
    if (e->mods & VFT_MOD_SHAKE) {
        float si = e->shake_intensity * ui_scale;
        mod_ox = sinf(t * 47.0f) * si;
        mod_oy = cosf(t * 53.0f) * si;
    }
    if (e->mods & VFT_MOD_BREATHE) {
        oscale *= 1.0f + 0.05f * sinf(t * 2.5f);
    }

    /* Pulse alpha */
    float pulse_a = 1.0f;
    if (e->mods & VFT_MOD_PULSE) {
        pulse_a = 0.6f + 0.4f * (0.5f + 0.5f * sinf(t * 6.0f));
    }

    float final_x = px_x + ox + mod_ox;
    float final_y = px_y + oy + mod_oy;
    float final_scale = e->size * oscale;
    float final_alpha = e->a * oalpha * pulse_a;
    float final_rot = orot;

    /* ---- Per-char physics modes (shatter/explode/melt) ---- */
    if (e->shattered) {
        float outro_start = e->intro_dur + e->hold_dur;
        float ot = t - outro_start;
        for (int i = 0; i < e->len; i++) {
            vft_char_t *c = &e->chars[i];
            if (c->ch <= 32) continue;
            /* update physics */
            c->x += c->vx * dt;
            c->y += c->vy * dt;
            if (e->outro_fx != VFT_FX_MELT)
                c->vy += 400.0f * ui_scale * dt; /* gravity */
            c->rot += c->vrot * dt;
            c->alpha = 1.0f - ot / e->outro_dur;
            if (c->alpha < 0) c->alpha = 0;
            /* bounce off screen edges */
            float char_px = px_x + c->x;
            float char_py = px_y + c->y;
            if (char_px < 0)      { c->x = -px_x;             c->vx *= -0.5f; }
            if (char_px > vp_w)   { c->x = (float)vp_w - px_x; c->vx *= -0.5f; }
            if (char_py > vp_h)   { c->y = (float)vp_h - px_y; c->vy *= -0.4f; }
            if (char_py < 0)      { c->y = -px_y;             c->vy *= -0.5f; }
            /* draw the character */
            if (c->alpha > 0.01f) {
                float cr = e->r, cg = e->g, cb = e->b;
                if (e->mods & VFT_MOD_RAINBOW) {
                    float hue = fmodf((float)i * 30.0f + t * 120.0f, 360.0f);
                    vft_hsv2rgb(hue, 0.9f, 1.0f, &cr, &cg, &cb);
                }
                /* shadow */
                if (e->mods & VFT_MOD_SHADOW) {
                    vft_push_char(c->ch, px_x + c->x + e->shadow_ox * ui_scale,
                                  px_y + c->y + e->shadow_oy * ui_scale,
                                  final_scale, c->rot,
                                  0, 0, 0, c->alpha * e->shadow_a * 0.7f,
                                  vp_w, vp_h);
                }
                vft_push_char(c->ch, px_x + c->x, px_y + c->y,
                              final_scale, c->rot,
                              cr, cg, cb, c->alpha * e->a,
                              vp_w, vp_h);
            }
        }
        return;
    }

    /* ---- Dissolve outro (per-char random fade) ---- */
    if (phase == 2 && e->outro_fx == VFT_FX_DISSOLVE) {
        float outro_start = e->intro_dur + e->hold_dur;
        float ot = (e->outro_dur > 0) ? (t - outro_start) / e->outro_dur : 1.0f;
        float cw = 14.0f * final_scale * ui_scale;
        float total_w = (float)e->len * cw;
        float start_x = final_x - total_w * 0.5f + cw * 0.5f;
        for (int i = 0; i < e->len; i++) {
            float ca = (ot < e->chars[i].dissolve_t) ? 1.0f :
                       1.0f - (ot - e->chars[i].dissolve_t) / (1.0f - e->chars[i].dissolve_t + 0.001f);
            if (ca < 0) ca = 0;
            unsigned char ch = (unsigned char)e->text[i];
            if (ch <= 32 || ca <= 0.01f) continue;
            float cr = e->r, cg = e->g, cb = e->b;
            if (e->mods & VFT_MOD_RAINBOW) {
                float hue = fmodf((float)i * 30.0f + t * 120.0f, 360.0f);
                vft_hsv2rgb(hue, 0.9f, 1.0f, &cr, &cg, &cb);
            }
            vft_push_char(ch, start_x + i * cw, final_y,
                          final_scale, 0, cr, cg, cb, ca * e->a,
                          vp_w, vp_h);
        }
        return;
    }

    /* ---- Normal rendering (whole text or per-char wave/typewriter) ---- */
    float cw = 14.0f * final_scale * ui_scale;
    float total_w = (float)e->len * cw;
    float start_x = final_x - total_w * 0.5f + cw * 0.5f;

    /* Trail: store + render previous positions */
    if (e->mods & VFT_MOD_TRAIL) {
        if (e->trail_count < VFT_MAX_TRAIL) e->trail_count++;
        e->trail_x[e->trail_head] = final_x;
        e->trail_y[e->trail_head] = final_y;
        e->trail_a[e->trail_head] = final_alpha;
        e->trail_s[e->trail_head] = final_scale;
        e->trail_head = (e->trail_head + 1) % VFT_MAX_TRAIL;
        /* render trail (oldest first, faded) */
        for (int j = 0; j < e->trail_count - 1; j++) {
            int idx = (e->trail_head - e->trail_count + j + VFT_MAX_TRAIL) % VFT_MAX_TRAIL;
            float ta = e->trail_a[idx] * (float)j / (float)e->trail_count * 0.4f;
            if (ta > 0.01f) {
                vft_push_str(e->text, e->len, e->trail_x[idx], e->trail_y[idx],
                             e->trail_s[idx], 0, e->r, e->g, e->b, ta,
                             vp_w, vp_h);
            }
        }
    }

    /* Glow: multi-pass at offsets */
    if (e->mods & VFT_MOD_GLOW) {
        float gs = e->glow_size * ui_scale;
        float ga = final_alpha * 0.15f;
        float gr = e->glow_r, gg = e->glow_g, gb = e->glow_b;
        float offsets[] = { -gs, 0, gs, 0, 0, -gs, 0, gs,
                            -gs*0.7f, -gs*0.7f, gs*0.7f, -gs*0.7f,
                            -gs*0.7f, gs*0.7f, gs*0.7f, gs*0.7f };
        for (int j = 0; j < 16; j += 2) {
            vft_push_str(e->text, e->len,
                         final_x + offsets[j], final_y + offsets[j+1],
                         final_scale, final_rot,
                         gr, gg, gb, ga, vp_w, vp_h);
        }
    }

    /* Outline: render at 4 cardinal + 4 diagonal offsets */
    if (e->mods & VFT_MOD_OUTLINE) {
        float ow = e->out_w * ui_scale;
        float oa = final_alpha * 0.9f;
        float dirs[8][2] = {{-1,0},{1,0},{0,-1},{0,1},{-0.7f,-0.7f},{0.7f,-0.7f},{-0.7f,0.7f},{0.7f,0.7f}};
        for (int j = 0; j < 8; j++) {
            vft_push_str(e->text, e->len,
                         final_x + dirs[j][0] * ow, final_y + dirs[j][1] * ow,
                         final_scale, final_rot,
                         e->out_r, e->out_g, e->out_b, oa, vp_w, vp_h);
        }
    }

    /* Shadow */
    if (e->mods & VFT_MOD_SHADOW) {
        vft_push_str(e->text, e->len,
                     final_x + e->shadow_ox * ui_scale,
                     final_y + e->shadow_oy * ui_scale,
                     final_scale, final_rot,
                     0, 0, 0, final_alpha * e->shadow_a, vp_w, vp_h);
    }

    /* Main text — per character for wave/typewriter/rainbow */
    int per_char = (e->mods & (VFT_MOD_WAVE | VFT_MOD_RAINBOW)) ||
                   (phase == 0 && e->intro_fx == VFT_FX_TYPEWRITER) ||
                   (phase == 0 && e->intro_fx == VFT_FX_WAVE_IN);

    if (per_char) {
        for (int i = 0; i < e->len; i++) {
            unsigned char ch = (unsigned char)e->text[i];
            if (ch <= 32) continue;

            float char_x = start_x + i * cw;
            float char_y = final_y;
            float char_a = final_alpha;
            float char_s = final_scale;
            float char_r = final_rot;
            float cr = e->r, cg = e->g, cb = e->b;

            /* wave */
            if (e->mods & VFT_MOD_WAVE) {
                float wf = (e->wave_freq > 0) ? e->wave_freq : 3.0f;
                float wa = (e->wave_amp > 0) ? e->wave_amp : 8.0f;
                char_y += sinf(t * wf * 6.283f + (float)i * 0.5f) * wa * ui_scale;
            }

            /* rainbow */
            if (e->mods & VFT_MOD_RAINBOW) {
                float hue = fmodf((float)i * 30.0f + t * 120.0f, 360.0f);
                vft_hsv2rgb(hue, 0.9f, 1.0f, &cr, &cg, &cb);
            }

            /* typewriter */
            if (phase == 0 && e->intro_fx == VFT_FX_TYPEWRITER) {
                float chars_visible = (t / e->intro_dur) * (float)e->len;
                if ((float)i > chars_visible) continue;
                if ((float)i > chars_visible - 1.0f)
                    char_a *= (chars_visible - (float)i);
            }

            /* wave_in */
            if (phase == 0 && e->intro_fx == VFT_FX_WAVE_IN) {
                float delay = (float)i / (float)e->len * 0.6f;
                float ct = (t / e->intro_dur - delay) / (1.0f - delay + 0.001f);
                if (ct < 0) { continue; }
                if (ct < 1.0f) {
                    char_s *= vft_ease_out_elastic(ct);
                    char_a *= vft_ease_out_cubic(ct);
                }
            }

            vft_push_char(ch, char_x, char_y, char_s, char_r,
                          cr, cg, cb, char_a, vp_w, vp_h);
        }
    } else {
        /* Chromatic aberration */
        if (e->mods & VFT_MOD_CHROMATIC) {
            float ca_off = 2.0f * ui_scale;
            vft_push_str(e->text, e->len, final_x - ca_off, final_y,
                         final_scale, final_rot, 1, 0, 0, final_alpha * 0.5f,
                         vp_w, vp_h);
            vft_push_str(e->text, e->len, final_x + ca_off, final_y,
                         final_scale, final_rot, 0, 0, 1, final_alpha * 0.5f,
                         vp_w, vp_h);
        }
        vft_push_str(e->text, e->len, final_x, final_y,
                     final_scale, final_rot, e->r, e->g, e->b, final_alpha,
                     vp_w, vp_h);
    }

    /* Orbit particles */
    if (e->mods & VFT_MOD_ORBIT) {
        int n_orb = e->len < 12 ? e->len : 12;
        float orbit_r = total_w * 0.55f;
        for (int i = 0; i < n_orb; i++) {
            float angle = t * 2.0f + (float)i * 6.283f / (float)n_orb;
            float ox2 = cosf(angle) * orbit_r;
            float oy2 = sinf(angle) * orbit_r * 0.3f; /* elliptical */
            float pa = final_alpha * 0.7f;
            float sz = 3.0f * ui_scale * e->size;
            vft_push_solid(final_x + ox2, final_y + oy2, sz, sz,
                           e->r, e->g, e->b, pa, vp_w, vp_h);
        }
    }

    /* Helix particles */
    if (e->mods & VFT_MOD_HELIX) {
        int n_helix = 24;
        float helix_r = total_w * 0.5f;
        for (int i = 0; i < n_helix; i++) {
            float frac = (float)i / (float)n_helix;
            float angle = t * 3.0f + frac * 12.566f; /* 2 full rotations */
            float hx = (frac - 0.5f) * total_w;
            float hy1 = sinf(angle) * 15.0f * ui_scale * e->size;
            float hy2 = sinf(angle + 3.14159f) * 15.0f * ui_scale * e->size;
            float pa = final_alpha * 0.5f * (0.5f + 0.5f * cosf(angle));
            float sz = 2.5f * ui_scale * e->size;
            vft_push_solid(final_x + hx, final_y + hy1, sz, sz,
                           e->r * 0.8f, e->g * 0.8f, e->b * 0.8f, pa, vp_w, vp_h);
            vft_push_solid(final_x + hx, final_y + hy2, sz, sz,
                           e->r * 0.6f, e->g * 0.6f, e->b * 0.6f, pa * 0.7f, vp_w, vp_h);
        }
    }

    /* Sparks — continuous particle emission */
    if (e->mods & VFT_MOD_SPARKS) {
        if (vft_randf() < 0.3f) {
            float sx = final_x + vft_randf_range(-total_w * 0.5f, total_w * 0.5f);
            vft_spawn_particle(sx, final_y,
                               vft_randf_range(-40.0f, 40.0f) * ui_scale,
                               vft_randf_range(-80.0f, -30.0f) * ui_scale,
                               e->r, e->g, e->b, 0.6f, e->size * 0.3f);
        }
    }

    /* Fire effect — upward orange/red particles */
    if (e->mods & VFT_MOD_FIRE) {
        for (int i = 0; i < 2; i++) {
            float fx = final_x + vft_randf_range(-total_w * 0.4f, total_w * 0.4f);
            float fy = final_y + vft_randf_range(-5.0f, 5.0f) * ui_scale;
            float fr = vft_randf_range(0.8f, 1.0f);
            float fg = vft_randf_range(0.2f, 0.6f);
            vft_spawn_particle(fx, fy,
                               vft_randf_range(-15.0f, 15.0f) * ui_scale,
                               vft_randf_range(-120.0f, -40.0f) * ui_scale,
                               fr, fg, 0.0f, 0.4f, e->size * 0.25f);
        }
    }
}

/* ---- Main Draw ---- */

static void vft_draw(int vp_w, int vp_h)
{
    /* Compute dt */
    DWORD now = GetTickCount();
    float dt = 0.016f;
    if (vft_last_tick != 0) {
        DWORD delta = now - vft_last_tick;
        if (delta > 0 && delta < 1000) dt = (float)delta * 0.001f;
    }
    vft_last_tick = now;
    vft_global_time += dt;

    /* Update particles */
    vft_update_particles(dt, vp_w, vp_h);

    /* Update & draw entries */
    for (int i = 0; i < VFT_MAX_ENTRIES; i++) {
        vft_entry_t *e = &vft_pool[i];
        if (!e->active) continue;
        e->elapsed += dt;
        float total = e->intro_dur + e->hold_dur + e->outro_dur;
        if (e->elapsed > total + 0.5f) { e->active = 0; continue; }
        vft_draw_entry(e, dt, vp_w, vp_h);
    }

    /* Draw particles on top */
    vft_draw_particles(vp_w, vp_h);
}

/* ---- Spawn Entry ---- */

static int vft_spawn(const char *text, float x, float y, float size,
                      float r, float g, float b, float a,
                      int intro_fx, float intro_dur,
                      float hold_dur,
                      int outro_fx, float outro_dur,
                      int mods)
{
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < VFT_MAX_ENTRIES; i++) {
        if (!vft_pool[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* Evict oldest */
        float oldest_t = 0;
        for (int i = 0; i < VFT_MAX_ENTRIES; i++) {
            if (vft_pool[i].elapsed > oldest_t) {
                oldest_t = vft_pool[i].elapsed;
                slot = i;
            }
        }
    }

    vft_entry_t *e = &vft_pool[slot];
    memset(e, 0, sizeof(*e));
    strncpy(e->text, text, VFT_MAX_TEXT - 1);
    e->text[VFT_MAX_TEXT - 1] = 0;
    e->len = (int)strlen(e->text);
    e->x = x; e->y = y;
    e->size = size;
    e->r = r; e->g = g; e->b = b; e->a = a;
    e->intro_fx = intro_fx; e->intro_dur = intro_dur;
    e->hold_dur = hold_dur;
    e->outro_fx = outro_fx; e->outro_dur = outro_dur;
    e->mods = mods;
    e->active = 1;
    e->id = vft_next_id++;

    /* Defaults for optional params */
    e->glow_r = r; e->glow_g = g; e->glow_b = b; e->glow_size = 6.0f;
    e->shadow_ox = 3.0f; e->shadow_oy = 3.0f; e->shadow_a = 0.5f;
    e->out_r = 0; e->out_g = 0; e->out_b = 0; e->out_w = 2.0f;
    e->shake_intensity = 4.0f;
    e->wave_amp = 8.0f; e->wave_freq = 2.0f;

    /* Init dissolve thresholds */
    for (int i = 0; i < e->len; i++) {
        e->chars[i].dissolve_t = vft_randf_range(0.0f, 0.6f);
        e->chars[i].ch = (unsigned char)e->text[i];
    }

    return e->id;
}

/* ---- Command Parser ---- */

static int vft_parse_fx(const char *name)
{
    for (int i = 0; i < VFT_FX_COUNT; i++)
        if (strcmp(name, vft_fx_names[i]) == 0) return i;
    return VFT_FX_NONE;
}

static int vft_parse_hex(const char *hex, float *r, float *g, float *b)
{
    unsigned int v = 0;
    for (int i = 0; hex[i] && i < 6; i++) {
        v <<= 4;
        char c = hex[i];
        if (c >= '0' && c <= '9') v |= (c - '0');
        else if (c >= 'a' && c <= 'f') v |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (c - 'A' + 10);
    }
    *r = ((v >> 16) & 0xFF) / 255.0f;
    *g = ((v >> 8) & 0xFF) / 255.0f;
    *b = (v & 0xFF) / 255.0f;
    return 1;
}

/* Named color lookup */
/* Case-insensitive compare for color names */
static int vft_streqi(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

#define C(name,R,G,B) {name, R/255.0f, G/255.0f, B/255.0f}
static int vft_named_color(const char *name, float *r, float *g, float *b)
{
    static const struct { const char *n; float r, g, b; } cols[] = {
        /* ---- CSS / Web Standard Colors ---- */
        C("aliceblue",240,248,255), C("antiquewhite",250,235,215),
        C("aqua",0,255,255), C("aquamarine",127,255,212),
        C("azure",240,255,255), C("beige",245,245,220),
        C("bisque",255,228,196), C("black",0,0,0),
        C("blanchedalmond",255,235,205), C("blue",0,0,255),
        C("blueviolet",138,43,226), C("brown",165,42,42),
        C("burlywood",222,184,135), C("cadetblue",95,158,160),
        C("chartreuse",127,255,0), C("chocolate",210,105,30),
        C("coral",255,127,80), C("cornflowerblue",100,149,237),
        C("cornsilk",255,248,220), C("crimson",220,20,60),
        C("cyan",0,255,255), C("darkblue",0,0,139),
        C("darkcyan",0,139,139), C("darkgoldenrod",184,134,11),
        C("darkgray",169,169,169), C("darkgreen",0,100,0),
        C("darkgrey",169,169,169), C("darkkhaki",189,183,107),
        C("darkmagenta",139,0,139), C("darkolivegreen",85,107,47),
        C("darkorange",255,140,0), C("darkorchid",153,50,204),
        C("darkred",139,0,0), C("darksalmon",233,150,122),
        C("darkseagreen",143,188,143), C("darkslateblue",72,61,139),
        C("darkslategray",47,79,79), C("darkslategrey",47,79,79),
        C("darkturquoise",0,206,209), C("darkviolet",148,0,211),
        C("deeppink",255,20,147), C("deepskyblue",0,191,255),
        C("dimgray",105,105,105), C("dimgrey",105,105,105),
        C("dodgerblue",30,144,255), C("firebrick",178,34,34),
        C("floralwhite",255,250,240), C("forestgreen",34,139,34),
        C("fuchsia",255,0,255), C("gainsboro",220,220,220),
        C("ghostwhite",248,248,255), C("gold",255,215,0),
        C("goldenrod",218,165,32), C("gray",128,128,128),
        C("green",0,128,0), C("greenyellow",173,255,47),
        C("grey",128,128,128), C("honeydew",240,255,240),
        C("hotpink",255,105,180), C("indianred",205,92,92),
        C("indigo",75,0,130), C("ivory",255,255,240),
        C("khaki",240,230,140), C("lavender",230,230,250),
        C("lavenderblush",255,240,245), C("lawngreen",124,252,0),
        C("lemonchiffon",255,250,205), C("lightblue",173,216,230),
        C("lightcoral",240,128,128), C("lightcyan",224,255,255),
        C("lightgoldenrodyellow",250,250,210), C("lightgray",211,211,211),
        C("lightgreen",144,238,144), C("lightgrey",211,211,211),
        C("lightpink",255,182,193), C("lightsalmon",255,160,122),
        C("lightseagreen",32,178,170), C("lightskyblue",135,206,250),
        C("lightslategray",119,136,153), C("lightslategrey",119,136,153),
        C("lightsteelblue",176,196,222), C("lightyellow",255,255,224),
        C("lime",0,255,0), C("limegreen",50,205,50),
        C("linen",250,240,230), C("magenta",255,0,255),
        C("maroon",128,0,0), C("mediumaquamarine",102,205,170),
        C("mediumblue",0,0,205), C("mediumorchid",186,85,211),
        C("mediumpurple",147,111,219), C("mediumseagreen",60,179,113),
        C("mediumslateblue",123,104,238), C("mediumspringgreen",0,250,154),
        C("mediumturquoise",72,209,204), C("mediumvioletred",199,21,133),
        C("midnightblue",25,25,112), C("mintcream",245,255,250),
        C("mistyrose",255,228,225), C("moccasin",255,228,181),
        C("navajowhite",255,222,173), C("navy",0,0,128),
        C("oldlace",253,245,230), C("olive",128,128,0),
        C("olivedrab",107,142,35), C("orange",255,165,0),
        C("orangered",255,69,0), C("orchid",218,112,214),
        C("palegoldenrod",238,232,170), C("palegreen",152,251,152),
        C("paleturquoise",175,238,238), C("palevioletred",219,112,147),
        C("papayawhip",255,239,213), C("peachpuff",255,218,185),
        C("peru",205,133,63), C("pink",255,192,203),
        C("plum",221,160,221), C("powderblue",176,224,230),
        C("purple",128,0,128), C("rebeccapurple",102,51,153),
        C("red",255,0,0), C("rosybrown",188,143,143),
        C("royalblue",65,105,225), C("saddlebrown",139,69,19),
        C("salmon",250,128,114), C("sandybrown",244,164,96),
        C("seagreen",46,139,87), C("seashell",255,245,238),
        C("sienna",160,82,45), C("silver",192,192,192),
        C("skyblue",135,206,235), C("slateblue",106,90,205),
        C("slategray",112,128,144), C("slategrey",112,128,144),
        C("snow",255,250,250), C("springgreen",0,255,127),
        C("steelblue",70,130,180), C("tan",210,180,140),
        C("teal",0,128,128), C("thistle",216,191,216),
        C("tomato",255,99,71), C("turquoise",64,224,208),
        C("violet",238,130,238), C("wheat",245,222,179),
        C("white",255,255,255), C("whitesmoke",245,245,245),
        C("yellow",255,255,0), C("yellowgreen",154,205,50),

        /* ---- Game / Fantasy / RPG Colors ---- */
        C("blood",138,7,7), C("darkblood",100,0,0), C("brightblood",180,18,18),
        C("ice",176,224,255), C("darkice",140,190,220), C("brightice",210,240,255),
        C("fire",226,88,34), C("darkfire",180,60,10), C("brightfire",255,120,50),
        C("ember",207,87,60), C("ash",178,178,178),
        C("steel",113,121,126), C("bronze",205,127,50),
        C("copper",184,115,51), C("jade",0,168,107),
        C("ruby",224,17,95), C("sapphire",15,82,186),
        C("emerald",80,200,120), C("amethyst",153,102,204),
        C("obsidian",28,28,32), C("ebony",40,36,33),
        C("rust",183,65,14), C("sand",194,178,128),
        C("sky",135,206,235), C("ocean",0,105,148),
        C("forest",34,100,34), C("midnight",25,25,70),
        C("dawn",255,200,150), C("dusk",180,100,140),
        C("storm",90,100,115), C("shadow",60,55,65),
        C("frost",200,230,240), C("flame",255,100,10),
        C("lava",207,44,0), C("moss",120,154,75),
        C("wine",114,47,55), C("honey",235,177,52),
        C("peach",255,203,164), C("mint",152,255,152),
        C("mauve",224,176,255), C("taupe",72,60,50),
        C("cream",255,253,208), C("burgundy",128,0,32),
        C("charcoal",54,69,79),

        /* ---- Metals & Minerals ---- */
        C("pewter",150,150,150), C("titanium",135,134,129),
        C("platinum",229,228,226), C("iron",82,82,82),
        C("mithril",195,205,215), C("adamantine",90,55,100),
        C("orichalcum",205,150,60), C("electrum",200,190,130),
        C("moonstone",200,200,220), C("sunstone",255,180,50),
        C("bloodstone",100,20,20), C("onyx",15,15,15),
        C("pearl",234,224,200), C("garnet",130,28,40),
        C("topaz",255,200,50), C("citrine",228,208,10),
        C("opal",200,200,220), C("alexandrite",130,90,130),

        /* ---- Light & Atmosphere ---- */
        C("moonlight",210,215,240), C("starlight",240,240,255),
        C("sunlight",255,235,160), C("candlelight",255,200,100),
        C("torchlight",255,165,70), C("firelight",255,140,50),
        C("twilight",140,110,160), C("ethereal",200,220,255),
        C("spectral",180,200,230), C("celestial",200,220,255),
        C("void",10,5,15), C("abyss",5,5,20),
        C("radiant",255,250,220),

        /* ---- Fantasy Creature Colors ---- */
        C("nightshade",80,40,90), C("dragonscale",60,90,60),
        C("dragongold",220,180,50), C("dragonred",180,30,30),
        C("dragonblue",30,60,160), C("phoenix",255,100,0),
        C("necrotic",80,90,40), C("arcane",120,50,200),
        C("divine",255,240,200), C("infernal",160,30,0),
        C("verdant",30,130,50),

        /* ---- Pigments & Art Colors ---- */
        C("cerulean",0,123,167), C("vermilion",227,66,52),
        C("cinnabar",228,77,48), C("ochre",204,153,0),
        C("umber",99,81,71), C("rawsienna",198,137,67),
        C("burntsienna",233,116,81), C("burntumber",138,51,36),
        C("sepia",112,66,20), C("mahogany",192,64,0),
        C("chestnut",149,69,53), C("auburn",165,42,42),
        C("cobalt",0,71,171), C("ultramarine",18,10,143),
        C("prussian",0,49,83), C("periwinkle",204,204,255),

        /* ---- Food & Nature Colors ---- */
        C("scarlet",255,36,0), C("carmine",150,0,24),
        C("claret",127,23,52), C("oxblood",75,0,0),
        C("merlot",115,30,50), C("rosewood",140,60,60),
        C("cherry",140,20,40), C("grape",100,45,110),
        C("mulberry",120,30,70), C("raspberry",135,38,87),
        C("strawberry",210,60,60), C("cranberry",150,20,50),
        C("pomegranate",150,30,30), C("apricot",251,206,177),
        C("tangerine",255,165,0), C("pumpkin",230,120,20),
        C("butterscotch",220,170,60), C("caramel",160,110,40),
        C("cinnamon",170,90,30), C("nutmeg",130,85,50),
        C("toffee",150,100,40), C("mocha",120,80,50),
        C("espresso",60,35,20), C("cocoa",90,55,35),

        /* ---- Earth & Stone Colors ---- */
        C("slate",112,128,144), C("flint",90,95,95),
        C("granite",130,130,130), C("limestone",180,180,160),
        C("sandstone",190,170,130), C("marble",230,225,220),
        C("alabaster",242,240,230), C("chalk",240,235,215),
        C("bone",227,218,201), C("parchment",235,220,185),
        C("terracotta",200,100,60), C("clay",180,120,70),
        C("adobe",190,130,70), C("brick",160,60,40),

        /* ---- Nature & Botanical ---- */
        C("sage",130,150,100), C("ivy",50,90,50),
        C("fern",80,120,60), C("clover",50,100,50),
        C("shamrock",50,170,90), C("seafoam",120,210,180),
        C("lagoon",50,150,160), C("glacier",170,210,230),
        C("arctic",200,230,240), C("tundra",160,170,150),
        C("prairie",180,175,120), C("savanna",190,170,110),
        C("mesa",190,130,80), C("canyon",180,100,60),

        /* ---- Fabric & Material ---- */
        C("silk",210,190,170), C("velvet",100,20,50),
        C("leather",145,100,60), C("suede",150,120,80),
        C("buff",220,200,150), C("ecru",194,178,128),
        C("flax",238,220,130), C("straw",215,200,120),
        C("amber",210,150,0), C("sable",50,40,35),
        C("raven",16,12,20), C("golden",255,205,0),
        C("silvery",195,199,200), C("sanguine",180,30,30),

        /* ---- Crayola & Fun Names ---- */
        C("bubblegum",255,130,170), C("cotton_candy",255,188,217),
        C("jellybean",190,60,60), C("licorice",30,20,15),
        C("watermelon",240,80,90), C("pistachio",147,197,114),
        C("blueberry",80,80,160), C("grapefruit",250,100,80),
        C("papaya",255,200,130), C("mango",255,180,50),
        C("guava",255,140,130), C("kiwi",140,200,70),
        C("avocado",86,130,3), C("olive2",128,128,0),
        C("mustard",205,175,0), C("saffron",244,196,48),
        C("turmeric",210,170,30), C("curry",200,150,40),

        /* ---- Neon / Electric ---- */
        C("neonpink",255,16,240), C("neongreen",57,255,20),
        C("neonblue",77,77,255), C("neonorange",255,95,31),
        C("neonyellow",207,255,4), C("neonpurple",190,60,255),
        C("electric",125,249,255), C("plasma",180,60,255),
        C("laser",255,40,40), C("hologram",140,200,255),

        {NULL,0,0,0}
    };
    for (int i = 0; cols[i].n; i++) {
        if (vft_streqi(name, cols[i].n)) {
            *r = cols[i].r; *g = cols[i].g; *b = cols[i].b;
            return 1;
        }
    }
    return 0;
}
#undef C

static float vft_parse_pos(const char *s, int is_x)
{
    if (strcmp(s, "center") == 0) return 0.5f;
    if (is_x && strcmp(s, "left") == 0) return 0.15f;
    if (is_x && strcmp(s, "right") == 0) return 0.85f;
    if (!is_x && strcmp(s, "top") == 0) return 0.15f;
    if (!is_x && strcmp(s, "bottom") == 0) return 0.85f;
    return (float)atof(s);
}

/* Parse: "text|x|y|param=val|param=val|..."
 * Returns entry ID or -1 on error */
static int vft_parse_cmd(const char *cmd)
{
    if (!cmd || !cmd[0]) return -1;

    char buf[1024];
    strncpy(buf, cmd, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    /* Tokenize by | */
    char *tokens[64];
    int ntok = 0;
    char *p = buf;
    while (*p && ntok < 64) {
        tokens[ntok++] = p;
        char *bar = strchr(p, '|');
        if (bar) { *bar = 0; p = bar + 1; }
        else break;
    }

    if (ntok < 1) return -1;

    /* Positional: text, x, y */
    const char *text = tokens[0];
    float x = 0.5f, y = 0.5f;
    if (ntok >= 2) x = vft_parse_pos(tokens[1], 1);
    if (ntok >= 3) y = vft_parse_pos(tokens[2], 0);

    /* Defaults */
    float size = 2.0f;
    float r = 1, g = 1, b = 1, a = 1;
    int intro_fx = VFT_FX_FADE, outro_fx = VFT_FX_FADE;
    float intro_dur = 0.3f, hold_dur = 2.0f, outro_dur = 0.5f;
    int mods = 0;
    float glow_r = -1, glow_g = 0, glow_b = 0, glow_size = 6.0f;
    float shadow_ox = 3, shadow_oy = 3, shadow_a = 0.5f;
    float out_r = 0, out_g = 0, out_b = 0, out_w = 2.0f;
    float shake = 4.0f, wave_amp = 8.0f, wave_freq = 2.0f;

    /* Parse key=value params */
    for (int i = (ntok >= 3 ? 3 : ntok); i < ntok; i++) {
        char *eq = strchr(tokens[i], '=');
        if (!eq) continue;
        *eq = 0;
        char *key = tokens[i];
        char *val = eq + 1;

        if (strcmp(key, "size") == 0) size = (float)atof(val);
        else if (strcmp(key, "color") == 0 || strcmp(key, "c") == 0) {
            if (!vft_named_color(val, &r, &g, &b))
                vft_parse_hex(val, &r, &g, &b);
        }
        else if (strcmp(key, "alpha") == 0) a = (float)atof(val);
        else if (strcmp(key, "intro") == 0) {
            char *colon = strchr(val, ':');
            if (colon) { *colon = 0; intro_dur = (float)atof(colon + 1); }
            intro_fx = vft_parse_fx(val);
        }
        else if (strcmp(key, "outro") == 0) {
            char *colon = strchr(val, ':');
            if (colon) { *colon = 0; outro_dur = (float)atof(colon + 1); }
            outro_fx = vft_parse_fx(val);
        }
        else if (strcmp(key, "hold") == 0) hold_dur = (float)atof(val);
        else if (strcmp(key, "intro_dur") == 0) intro_dur = (float)atof(val);
        else if (strcmp(key, "outro_dur") == 0) outro_dur = (float)atof(val);
        else if (strcmp(key, "glow") == 0) {
            mods |= VFT_MOD_GLOW;
            char *colon = strchr(val, ':');
            if (colon) { *colon = 0; glow_size = (float)atof(colon + 1); }
            if (val[0] == '1' && val[1] == 0) { glow_r = -1; } /* use text color */
            else if (!vft_named_color(val, &glow_r, &glow_g, &glow_b))
                vft_parse_hex(val, &glow_r, &glow_g, &glow_b);
        }
        else if (strcmp(key, "glow_size") == 0) glow_size = (float)atof(val);
        else if (strcmp(key, "shadow") == 0) {
            mods |= VFT_MOD_SHADOW;
            char *c1 = strchr(val, ':');
            if (c1) {
                *c1 = 0; shadow_ox = (float)atof(val);
                char *c2 = strchr(c1 + 1, ':');
                if (c2) { *c2 = 0; shadow_oy = (float)atof(c1+1); shadow_a = (float)atof(c2+1); }
                else shadow_oy = (float)atof(c1 + 1);
            }
        }
        else if (strcmp(key, "outline") == 0) {
            mods |= VFT_MOD_OUTLINE;
            char *colon = strchr(val, ':');
            if (colon) { *colon = 0; out_w = (float)atof(colon + 1); }
            if (val[0] != '1' || val[1] != 0) {
                if (!vft_named_color(val, &out_r, &out_g, &out_b))
                    vft_parse_hex(val, &out_r, &out_g, &out_b);
            }
        }
        else if (strcmp(key, "shake") == 0) { mods |= VFT_MOD_SHAKE; shake = (float)atof(val); }
        else if (strcmp(key, "wave") == 0) { mods |= VFT_MOD_WAVE; wave_amp = (float)atof(val); }
        else if (strcmp(key, "wave_freq") == 0) { wave_freq = (float)atof(val); }
        else if (strcmp(key, "pulse") == 0) { if (atoi(val)) mods |= VFT_MOD_PULSE; }
        else if (strcmp(key, "rainbow") == 0) { if (atoi(val)) mods |= VFT_MOD_RAINBOW; }
        else if (strcmp(key, "trail") == 0) { if (atoi(val)) mods |= VFT_MOD_TRAIL; }
        else if (strcmp(key, "orbit") == 0) { if (atoi(val)) mods |= VFT_MOD_ORBIT; }
        else if (strcmp(key, "sparks") == 0) { if (atoi(val)) mods |= VFT_MOD_SPARKS; }
        else if (strcmp(key, "helix") == 0) { if (atoi(val)) mods |= VFT_MOD_HELIX; }
        else if (strcmp(key, "chromatic") == 0) { if (atoi(val)) mods |= VFT_MOD_CHROMATIC; }
        else if (strcmp(key, "fire") == 0) { if (atoi(val)) mods |= VFT_MOD_FIRE; }
        else if (strcmp(key, "breathe") == 0) { if (atoi(val)) mods |= VFT_MOD_BREATHE; }
        else if (strcmp(key, "font") == 0) {
            int fi = atoi(val);
            if (fi >= 0 && fi < VFT_NUM_FONTS && fi != vft_current_font) {
                vkt_init_vft_font(fi);
            }
        }
    }

    int id = vft_spawn(text, x, y, size, r, g, b, a,
                       intro_fx, intro_dur, hold_dur, outro_fx, outro_dur, mods);

    /* Apply optional params to spawned entry */
    if (id > 0) {
        for (int i = 0; i < VFT_MAX_ENTRIES; i++) {
            if (vft_pool[i].id == id) {
                vft_entry_t *e = &vft_pool[i];
                if (glow_r < 0) { e->glow_r = r; e->glow_g = g; e->glow_b = b; }
                else { e->glow_r = glow_r; e->glow_g = glow_g; e->glow_b = glow_b; }
                e->glow_size = glow_size;
                e->shadow_ox = shadow_ox; e->shadow_oy = shadow_oy; e->shadow_a = shadow_a;
                e->out_r = out_r; e->out_g = out_g; e->out_b = out_b; e->out_w = out_w;
                e->shake_intensity = shake;
                e->wave_amp = wave_amp; e->wave_freq = wave_freq;
                break;
            }
        }
    }
    return id;
}

/* ---- Clear Commands ---- */

static void vft_clear_all(void)
{
    for (int i = 0; i < VFT_MAX_ENTRIES; i++) vft_pool[i].active = 0;
    for (int i = 0; i < VFT_MAX_PARTICLES; i++) vft_parts[i].active = 0;
}

static void vft_clear_id(int id)
{
    for (int i = 0; i < VFT_MAX_ENTRIES; i++)
        if (vft_pool[i].active && vft_pool[i].id == id)
            vft_pool[i].active = 0;
}

/* ---- Exported API ---- */

__declspec(dllexport) int vft_command(const char *cmd)
{
    if (!cmd) return -1;
    /* Special commands */
    if (strcmp(cmd, "clear") == 0) { vft_clear_all(); return 0; }
    if (strncmp(cmd, "clear:", 6) == 0) { vft_clear_id(atoi(cmd + 6)); return 0; }
    return vft_parse_cmd(cmd);
}

__declspec(dllexport) void vft_help(void)
{
    /* Output to VKW console if open, otherwise fall back to api->log */
    #define VFT_HELP(s) do { \
        if (vkw_console_idx >= 0) vkw_print(vkw_console_idx, s); \
        else if (api) api->log("%s\n", s); \
    } while(0)

    VFT_HELP("=== Vulkan Floating Text (VFT) ===");
    VFT_HELP("");
    VFT_HELP("  SYNTAX: vft(\"text|x|y|param=val|...\")");
    VFT_HELP("");
    VFT_HELP("  POSITIONAL:");
    VFT_HELP("    text    - The text to display (required)");
    VFT_HELP("    x       - Position: 0.0-1.0, center, left, right");
    VFT_HELP("    y       - Position: 0.0-1.0, center, top, bottom");
    VFT_HELP("");
    VFT_HELP("  SIZE & COLOR:");
    VFT_HELP("    size=N        - Scale (default 2.0, try 1-8)");
    VFT_HELP("    color=RRGGBB  - Hex or name: red,green,blue,");
    VFT_HELP("      yellow,cyan,magenta,white,gold,crimson,");
    VFT_HELP("      orange,pink,purple,silver,lime,blood,ice,fire");
    VFT_HELP("    alpha=N       - Opacity 0.0-1.0 (default 1.0)");
    VFT_HELP("");
    VFT_HELP("  INTRO EFFECTS (intro=name or intro=name:dur):");
    VFT_HELP("    fade, zoom_in, zoom_out, slide_l, slide_r,");
    VFT_HELP("    slide_u, slide_d, bounce, pop, typewriter,");
    VFT_HELP("    wave_in, spin");
    VFT_HELP("");
    VFT_HELP("  OUTRO EFFECTS (outro=name or outro=name:dur):");
    VFT_HELP("    fade, zoom_in, zoom_out, slide_l, slide_r,");
    VFT_HELP("    slide_u, slide_d, shatter, explode, melt,");
    VFT_HELP("    dissolve, evaporate, spin");
    VFT_HELP("");
    VFT_HELP("  TIMING:");
    VFT_HELP("    hold=N      - Hold duration in seconds (def 2.0)");
    VFT_HELP("    intro_dur=N - Intro duration (default 0.3)");
    VFT_HELP("    outro_dur=N - Outro duration (default 0.5)");
    VFT_HELP("");
    VFT_HELP("  MODIFIERS (combinable):");
    VFT_HELP("    glow=1|RRGGBB:size  shadow=1|ox:oy:alpha");
    VFT_HELP("    outline=1|RRGGBB:w  shake=N  wave=N");
    VFT_HELP("    pulse=1  rainbow=1  trail=1  orbit=1");
    VFT_HELP("    sparks=1  helix=1  chromatic=1  fire=1");
    VFT_HELP("    breathe=1  wave_freq=N");
    VFT_HELP("");
    VFT_HELP("  FONT (font=N, 0-4):");
    VFT_HELP("    0=Black Ops  1=Teko  2=Russo");
    VFT_HELP("    3=Orbitron   4=Rajdhani");
    VFT_HELP("");
    VFT_HELP("  COMMANDS:");
    VFT_HELP("    vft(\"clear\")    - Clear all floating text");
    VFT_HELP("    vft(\"clear:ID\") - Clear by ID");
    VFT_HELP("    vft_help()      - This help");
    VFT_HELP("");
    VFT_HELP("  EXAMPLES:");
    VFT_HELP("    vft(\"Hello|center|center|size=3|color=gold\"");
    VFT_HELP("        \"|intro=pop|outro=fade\")");
    VFT_HELP("    vft(\"CRITICAL HIT!|0.5|0.3|size=5|color=red\"");
    VFT_HELP("        \"|intro=zoom_in|outro=shatter|glow=1\")");
    VFT_HELP("    vft(\"LEVEL UP!|center|0.3|size=6|color=gold\"");
    VFT_HELP("        \"|intro=bounce|outro=explode|rainbow=1\")");

    #undef VFT_HELP
}

__declspec(dllexport) void vft_fonts_list(void)
{
    if (!api) return;
    api->log("\n=== Available Fonts ===\n");
    for (int i = 0; i < NUM_TTF_FONTS; i++) {
        if (ttf_fonts[i].filename == NULL)
            api->log("\n  -- %s --\n", ttf_fonts[i].name);
        else
            api->log("  [%2d] %s (%s)\n", i, ttf_fonts[i].name, ttf_fonts[i].filename);
    }
    api->log("\n");
}

/* Set the VFT display font (0-4) and list available VFT fonts */
__declspec(dllexport) int vft_set_font(int idx)
{
    if (idx < 0 || idx >= VFT_NUM_FONTS) {
        if (api) api->log("[VFT] Invalid font index %d (0-%d)\n", idx, VFT_NUM_FONTS - 1);
        return -1;
    }
    vkt_init_vft_font(idx);
    if (api) api->log("[VFT] Font set to %d: %s\n", idx, vft_font_names[idx]);
    return idx;
}

__declspec(dllexport) void vft_list_fonts(void)
{
    #define VFT_FL(s) do { \
        if (vkw_console_idx >= 0) vkw_print(vkw_console_idx, s); \
        else if (api) api->log("%s\n", s); \
    } while(0)
    VFT_FL("=== VFT Display Fonts ===");
    for (int i = 0; i < VFT_NUM_FONTS; i++) {
        char buf[128];
        wsprintfA(buf, "  [%d] %s%s", i, vft_font_names[i],
                  (i == vft_current_font) ? " (active)" : "");
        VFT_FL(buf);
    }
    VFT_FL("Use font=N in VFT command or vft_set_font(N)");
    #undef VFT_FL
}
