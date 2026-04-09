/* viz_fx.c — Terminal Visualizations: Pixel Blades, Cannon Balls, Matrix Rain, Vectroids
 * Physics particles interact with terminal text. Beat-reactive via mr_beat_snap.
 * Included into vk_terminal.c after mudradio includes.
 */

/* ---- Configuration ---- */
#define VIZ_MAX_PARTICLES   256
#define VIZ_MAX_FRAGMENTS   1024
#define VIZ_MAX_LASERS      96
#define VIZ_MAX_RAIN        128
#define VIZ_MAX_CBALLS      16    /* max cannon balls on screen */
#define VIZ_GRAVITY         600.0f
#define VIZ_SPRING_K        10.0f
#define VIZ_SPRING_DAMP     0.88f
#define VIZ_SHATTER_VEL     60.0f   /* low threshold so blades shred on contact */
#define VIZ_CELL_RESPAWN    4.0f  /* seconds before shattered cell reappears */

/* ---- Types ---- */
typedef enum {
    VIZ_NONE = 0,
    VIZ_BLADES,       /* Pixel saw blades — spinning jagged discs */
    VIZ_CANNONBALLS,  /* Raymarched 3D metallic spiked orbs */
    VIZ_MATRIX,
    VIZ_VECTROIDS,
    VIZ_MODE_COUNT
} viz_mode_t;

static viz_mode_t viz_mode = VIZ_NONE;
static float viz_time = 0.0f;

/* Global beat state — read once per frame, used by all modes */
static int   viz_last_beat = 0;    /* last beat_count we saw */
static int   viz_beat_hit = 0;     /* 1 on the frame a new beat arrives */
static float viz_beat_flash = 0;   /* decays from 1.0 on beat */
static float viz_kick = 0;         /* current kick/bass energy */
static float viz_mid = 0;
static float viz_treble = 0;
static int   viz_beat_num = 0;     /* total beats since mode started */

static void viz_read_beat(void) {
    EnterCriticalSection(&mr_beat_lock);
    int bc = mr_beat_snap.beat_count;
    viz_kick = mr_beat_snap.bass_energy;
    viz_mid  = mr_beat_snap.mid_energy;
    viz_treble = mr_beat_snap.treble_energy;
    LeaveCriticalSection(&mr_beat_lock);
    if (bc != viz_last_beat) {
        viz_beat_hit = 1;
        viz_beat_flash = 1.0f;
        viz_beat_num++;
        viz_last_beat = bc;
    } else {
        viz_beat_hit = 0;
    }
}

/* Per-cell displacement (shared by all modes) */
typedef struct {
    float dx, dy;       /* pixel displacement */
    float vx, vy;       /* displacement velocity */
    int   shattered;    /* 1 = cell destroyed, render as fragments */
    float shatter_t;    /* time since shatter */
} viz_cell_t;

#define VIZ_ROWS 128
#define VIZ_COLS 256
static viz_cell_t viz_cells[VIZ_ROWS][VIZ_COLS];

/* Particles: boulders, debris, sparks */
typedef struct {
    float x, y, vx, vy;
    float radius, angle, ang_vel;
    float life, max_life;
    float r, g, b, a;
    int   glyph;        /* character to render (0 = solid) */
    int   type;          /* 0=boulder 1=debris 2=spark */
    int   active;
} viz_part_t;

static viz_part_t viz_parts[VIZ_MAX_PARTICLES];

/* Fragments: shattered character pieces */
typedef struct {
    float x, y, vx, vy;
    float u0, v0, u1, v1;
    float r, g, b, a;
    float life, angle, ang_vel;
    float w, h;          /* size in pixels */
    int   active;
} viz_frag_t;

static viz_frag_t viz_frags[VIZ_MAX_FRAGMENTS];

/* Matrix rain columns */
typedef struct {
    float y;             /* head position (pixels) */
    float speed;         /* fall speed */
    int   col;           /* terminal column */
    int   len;           /* trail length in chars */
    int   active;
    float spawn_t;       /* time until next respawn */
    int   chars[80];     /* random characters in trail */
} viz_rain_t;

static viz_rain_t viz_rain[VIZ_MAX_RAIN];

/* Vectroids ship flight styles */
#define VIZ_FLY_HUNTER   0   /* cyan: aggressive bee-line to text, fast turns */
#define VIZ_FLY_ORBIT    1   /* pink: circles around targets, strafing runs */
#define VIZ_FLY_ZIGZAG   2   /* green: fast zigzag sweeps across the screen */
#define VIZ_FLY_DRIFT    3   /* gold: slow floaty drift, sudden dash on beat */
#define VIZ_FLY_PATROL   4   /* purple: smooth figure-8 patrol pattern */

typedef struct {
    float x, y, vx, vy;
    float angle;         /* heading in radians */
    float target_angle;  /* smooth steering target */
    float thrust;
    int   firing;        /* cooldown frames */
    float wander_t;      /* time until next direction change */
    float tx, ty;        /* current target position */
    float cr, cg, cb;    /* ship color */
    int   style;         /* flight pattern style */
    float style_phase;   /* per-ship phase accumulator for patterns */
    float dash_timer;    /* for drift style: time until next dash */
    float orbit_angle;   /* for orbit style: current orbit angle */
    float orbit_radius;  /* for orbit style: orbit distance */
    float zig_dir;       /* for zigzag: current sweep direction */
    int   alive;         /* 1 = alive, 0 = dead/respawning */
    float respawn_timer; /* seconds until respawn (10s) */
    int   hp;            /* hit points (3 hits to die) */
    int   target_type;   /* 0=text, 1=ship, 2=ufo */
    int   target_ship;   /* index of targeted ship (when target_type==1) */
    float fight_timer;   /* seconds remaining in ship-vs-ship/ufo mode */
    int   present;       /* 1 = this ship slot is active on screen (for idle mode) */
    float presence_timer;/* seconds until this ship leaves/arrives (idle mode) */
    int   departing;     /* 1 = flying off screen to leave */
} viz_ship_t;

#define VIZ_MAX_SHIPS 5
static viz_ship_t viz_ships[VIZ_MAX_SHIPS];
static int viz_ship_count = 1;

/* UFO */
typedef struct {
    float x, y;
    float vx;
    float target_vx;       /* velocity we're accelerating toward */
    int   active;
    float stay_timer;      /* how long it stays on screen */
    int   hp;              /* 5 hits to kill */
    int   firing;          /* 1 = laser beam active */
    float laser_charge;    /* 0→1 charge up time */
    float laser_width;     /* current beam width in pixels */
    float laser_alpha;     /* pulsing alpha */
    float laser_time;      /* time since beam started */
    int   direction;       /* 1 = left-to-right, -1 = right-to-left */
    float explode_timer;   /* >0 = exploding animation */
    float dash_timer;      /* time until next dash/reposition */
    int   dashing;         /* 1 = doing a quick lateral dash */
    float dash_dur;        /* how long current dash lasts */
    float text_right_edge; /* rightmost text column in pixels */
} viz_ufo_t;

static viz_ufo_t viz_ufo;
static float viz_ufo_cooldown = 0;    /* seconds until UFO can appear again */
static float viz_scale = 1.0f;        /* resolution scale (vp_h / 1080) */
static float viz_music_energy = 0;    /* smoothed energy for music detection */
#define VIZ_MUSIC_THRESHOLD 0.12f
static float viz_idle_fire_timer = 0; /* occasional shooting without music */

/* Lasers */
typedef struct {
    float x, y, vx, vy;
    float life;
    int   active;
    float lr, lg, lb;    /* laser color from ship */
    int   owner;         /* ship index that fired this, -1 = unknown */
} viz_laser_t;

static viz_laser_t viz_lasers[VIZ_MAX_LASERS];

/* Cannon balls: raymarched 3D metallic spiked orbs */
typedef struct {
    float x, y, vx, vy;
    float radius;
    float angle;           /* rotation for spikes */
    float ang_vel;
    float life;
    int   active;
    int   spike_count;     /* 6-10 spikes */
    float spike_phase;     /* offset for spike pattern variety */
    float metallic_hue;    /* slight color variation per ball */
} viz_cball_t;

static viz_cball_t viz_cballs[VIZ_MAX_CBALLS];

/* ---- Helpers ---- */
static float viz_randf(void) { return (float)(rand() % 10000) / 10000.0f; }
static float viz_randf_range(float lo, float hi) { return lo + viz_randf() * (hi - lo); }

/* ---- Init ---- */
static void viz_init(void) {
    memset(viz_cells, 0, sizeof(viz_cells));
    memset(viz_parts, 0, sizeof(viz_parts));
    memset(viz_frags, 0, sizeof(viz_frags));
    memset(viz_rain, 0, sizeof(viz_rain));
    memset(viz_lasers, 0, sizeof(viz_lasers));
    memset(viz_cballs, 0, sizeof(viz_cballs));
    memset(viz_ships, 0, sizeof(viz_ships));
    for (int i = 0; i < VIZ_MAX_SHIPS; i++) {
        viz_ships[i].alive = 1;
        viz_ships[i].hp = 3;
        viz_ships[i].present = (i == 0) ? 1 : 0; /* only first ship starts present */
        viz_ships[i].presence_timer = viz_randf_range(5.0f, 20.0f);
    }
    viz_ship_count = 1;
    memset(&viz_ufo, 0, sizeof(viz_ufo));
    viz_ufo_cooldown = viz_randf_range(120.0f, 180.0f); /* first UFO after 2-3 min */
    viz_music_energy = 0;
    viz_idle_fire_timer = viz_randf_range(3.0f, 8.0f);
    viz_beat_num = 0;
    viz_last_beat = mr_beat_snap.beat_count;
    viz_beat_flash = 0;
    viz_time = 0.0f;
}

/* ---- Alloc helpers ---- */
static viz_part_t *viz_alloc_part(void) {
    for (int i = 0; i < VIZ_MAX_PARTICLES; i++)
        if (!viz_parts[i].active) return &viz_parts[i];
    return NULL;
}

static viz_frag_t *viz_alloc_frag(void) {
    for (int i = 0; i < VIZ_MAX_FRAGMENTS; i++)
        if (!viz_frags[i].active) return &viz_frags[i];
    return NULL;
}

static viz_laser_t *viz_alloc_laser(void) {
    for (int i = 0; i < VIZ_MAX_LASERS; i++)
        if (!viz_lasers[i].active) return &viz_lasers[i];
    return NULL;
}

/* ---- Shatter a terminal cell into 4 fragments ---- */
static void viz_shatter_cell(int r, int c, float force_vx, float force_vy,
                              float cw, float ch, float x_off, float top_pad_px,
                              float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    if (r < 0 || r >= TERM_ROWS || c < 0 || c >= TERM_COLS) return;
    if (r >= VIZ_ROWS || c >= VIZ_COLS) return;
    unsigned char byte = ansi_term.grid[r][c].ch;
    if (byte == 0 || byte == 32) return;
    if (viz_cells[r][c].shattered) return;

    viz_cells[r][c].shattered = 1;
    viz_cells[r][c].shatter_t = 0.0f;

    /* Get character color */
    ap_attr_t *a = &ansi_term.grid[r][c].attr;
    int fg = (a->fg & 0x07) | (a->bold ? 0x08 : 0);
    float cr = palette[fg].r, cg = palette[fg].g, cb = palette[fg].b;

    /* Character UV */
    float u0 = (byte % 16) * tex_cw + hp_u;
    float v0 = (byte / 16) * tex_ch + hp_v;
    float umid = u0 + (tex_cw - 2*hp_u) * 0.5f;
    float vmid = v0 + (tex_ch - 2*hp_v) * 0.5f;
    float u1 = u0 + tex_cw - 2*hp_u;
    float v1 = v0 + tex_ch - 2*hp_v;

    /* Pixel position */
    float px = x_off + c * cw;
    float py = top_pad_px + r * ch;
    float hw = cw * 0.5f, hh = ch * 0.5f;

    /* Spawn 4 quarter-fragments */
    float frag_uvs[4][4] = {
        { u0, v0, umid, vmid },  /* top-left */
        { umid, v0, u1, vmid },  /* top-right */
        { u0, vmid, umid, v1 },  /* bottom-left */
        { umid, vmid, u1, v1 },  /* bottom-right */
    };
    float frag_offsets[4][2] = {
        { 0, 0 }, { hw, 0 }, { 0, hh }, { hw, hh }
    };

    for (int i = 0; i < 4; i++) {
        viz_frag_t *f = viz_alloc_frag();
        if (!f) break;
        f->active = 1;
        f->x = px + frag_offsets[i][0];
        f->y = py + frag_offsets[i][1];
        /* Explode outward from center + inherit force */
        float ox = (i & 1) ? 1.0f : -1.0f;
        float oy = (i & 2) ? 1.0f : -1.0f;
        f->vx = force_vx * 0.5f + ox * viz_randf_range(80, 200);
        f->vy = force_vy * 0.5f + oy * viz_randf_range(80, 200);
        f->u0 = frag_uvs[i][0]; f->v0 = frag_uvs[i][1];
        f->u1 = frag_uvs[i][2]; f->v1 = frag_uvs[i][3];
        f->r = cr; f->g = cg; f->b = cb; f->a = 1.0f;
        f->life = viz_randf_range(1.0f, 2.5f);
        f->angle = 0; f->ang_vel = viz_randf_range(-8.0f, 8.0f);
        f->w = hw; f->h = hh;
    }
}

/* ---- Apply force to a cell (push it) ---- */
static void viz_push_cell(int r, int c, float fx, float fy) {
    if (r < 0 || r >= VIZ_ROWS || c < 0 || c >= VIZ_COLS) return;
    if (r >= TERM_ROWS || c >= TERM_COLS) return;
    if (viz_cells[r][c].shattered) return;
    viz_cells[r][c].vx += fx;
    viz_cells[r][c].vy += fy;
}

/* ---- Spawn a pixel saw blade ---- */
static void viz_spawn_blade(float cw, float ch, float vp_w, float bass) {
    viz_part_t *p = viz_alloc_part();
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->active = 1;
    p->type = 0;
    p->radius = cw * viz_randf_range(1.5f, 3.5f) * (0.7f + bass * 0.6f);
    p->x = viz_randf_range(p->radius, vp_w - p->radius);
    p->y = -p->radius * 2;
    p->vx = viz_randf_range(-80, 80);
    p->vy = viz_randf_range(100, 250);
    p->angle = 0;
    p->ang_vel = viz_randf_range(-12.0f, 12.0f); /* fast spin for saw blade */
    if (fabsf(p->ang_vel) < 6.0f) p->ang_vel = (p->ang_vel < 0 ? -1 : 1) * 6.0f;
    p->life = 15.0f; p->max_life = 15.0f;
    p->r = 0.6f; p->g = 0.6f; p->b = 0.65f; /* metallic silver-grey */
    p->a = 1.0f;
    p->glyph = 12; /* 12 teeth on the blade */
}

/* ---- Spawn a cannon ball (3D metallic spiked orb) ---- */
static void viz_spawn_cball(float cw, float ch, float vp_w, float bass) {
    viz_cball_t *cb = NULL;
    for (int i = 0; i < VIZ_MAX_CBALLS; i++)
        if (!viz_cballs[i].active) { cb = &viz_cballs[i]; break; }
    if (!cb) return;
    memset(cb, 0, sizeof(*cb));
    cb->active = 1;
    cb->radius = cw * viz_randf_range(0.8f, 1.8f) * (0.7f + bass * 0.4f);
    cb->x = viz_randf_range(cb->radius, vp_w - cb->radius);
    cb->y = -cb->radius * 2;
    cb->vx = viz_randf_range(-60, 60);
    cb->vy = viz_randf_range(80, 200);
    cb->angle = viz_randf() * 6.28f;
    cb->ang_vel = viz_randf_range(-3.0f, 3.0f);
    cb->life = 18.0f;
    cb->spike_count = 6 + (rand() % 5); /* 6-10 spikes */
    cb->spike_phase = viz_randf() * 6.28f;
    cb->metallic_hue = viz_randf_range(-0.05f, 0.05f); /* slight tint variation */
}

/* ---- Spawn debris from impact ---- */
static void viz_spawn_debris(float x, float y, float vx, float vy, int count) {
    for (int i = 0; i < count; i++) {
        viz_part_t *p = viz_alloc_part();
        if (!p) return;
        memset(p, 0, sizeof(*p));
        p->active = 1;
        p->type = 2; /* spark */
        p->radius = viz_randf_range(1, 3);
        p->x = x; p->y = y;
        float a = viz_randf() * 6.28f;
        float spd = viz_randf_range(100, 400);
        p->vx = vx * 0.3f + cosf(a) * spd;
        p->vy = vy * 0.3f + sinf(a) * spd;
        p->life = viz_randf_range(0.3f, 1.0f);
        p->max_life = p->life;
        p->r = 1.0f; p->g = viz_randf_range(0.5f, 1.0f); p->b = viz_randf_range(0.0f, 0.3f);
        p->a = 1.0f;
    }
}

/* ==== PIXEL BLADES UPDATE ==== */
static float viz_blade_timer = 0;

static void viz_update_blades(float dt, float cw, float ch, float x_off,
                                  float top_pad_px, float vp_w, float vp_h,
                                  float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    viz_blade_timer -= dt;
    if (viz_beat_hit && viz_blade_timer <= 0) {
        viz_spawn_blade(cw, ch, vp_w, viz_kick);
        viz_blade_timer = 0.2f;
    }
    /* Passive spawn */
    static float passive_timer = 0;
    passive_timer -= dt;
    if (passive_timer <= 0) {
        viz_spawn_blade(cw, ch, vp_w, 0.3f);
        passive_timer = viz_randf_range(2.0f, 5.0f);
    }

    /* Update boulders */
    for (int i = 0; i < VIZ_MAX_PARTICLES; i++) {
        viz_part_t *p = &viz_parts[i];
        if (!p->active) continue;

        if (p->type == 0) { /* boulder */
            p->vy += VIZ_GRAVITY * dt;
            p->x += p->vx * dt;
            p->y += p->vy * dt;
            p->angle += p->ang_vel * dt;
            p->life -= dt;

            /* Bounce off walls */
            if (p->x < p->radius) { p->x = p->radius; p->vx = fabsf(p->vx) * 0.7f; }
            if (p->x > vp_w - p->radius) { p->x = vp_w - p->radius; p->vx = -fabsf(p->vx) * 0.7f; }

            /* Collision with text grid */
            int cr_min = (int)((p->y - p->radius - top_pad_px) / ch);
            int cr_max = (int)((p->y + p->radius - top_pad_px) / ch);
            int cc_min = (int)((p->x - p->radius - x_off) / cw);
            int cc_max = (int)((p->x + p->radius - x_off) / cw);
            if (cr_min < 0) cr_min = 0;
            if (cc_min < 0) cc_min = 0;
            if (cr_max >= TERM_ROWS) cr_max = TERM_ROWS - 1;
            if (cc_max >= TERM_COLS) cc_max = TERM_COLS - 1;

            for (int r = cr_min; r <= cr_max; r++) {
                for (int c = cc_min; c <= cc_max; c++) {
                    unsigned char byte = ansi_term.grid[r][c].ch;
                    if (byte == 0 || byte == 32) continue;
                    if (viz_cells[r][c].shattered) continue;

                    /* Cell center in pixels */
                    float cx = x_off + c * cw + cw * 0.5f;
                    float cy = top_pad_px + r * ch + ch * 0.5f;
                    float dx = p->x - cx, dy = p->y - cy;
                    float dist = sqrtf(dx*dx + dy*dy);

                    if (dist < p->radius + cw * 0.5f) {
                        float speed = sqrtf(p->vx*p->vx + p->vy*p->vy);
                        if (speed > VIZ_SHATTER_VEL) {
                            viz_shatter_cell(r, c, p->vx, p->vy,
                                            cw, ch, x_off, top_pad_px,
                                            tex_cw, tex_ch, hp_u, hp_v);
                            viz_spawn_debris(cx, cy, p->vx * 0.5f, p->vy * 0.5f, 4);
                        } else {
                            /* Just push it */
                            float push = (speed + 50.0f) * 0.5f;
                            float nx = (dist > 0.1f) ? dx / dist : 0;
                            float ny = (dist > 0.1f) ? dy / dist : 1;
                            viz_push_cell(r, c, -nx * push, -ny * push);
                        }
                        /* Blade barely slows — shreds right through */
                        p->vx *= 0.99f;
                        p->vy *= 0.97f;
                    }
                }
            }

            /* Kill if off screen or expired */
            if (p->y > vp_h + p->radius * 2 || p->life <= 0)
                p->active = 0;

        } else { /* debris/sparks */
            p->vy += VIZ_GRAVITY * 0.5f * dt;
            p->x += p->vx * dt;
            p->y += p->vy * dt;
            p->life -= dt;
            p->a = p->life / p->max_life;
            if (p->life <= 0) p->active = 0;
        }
    }
}

/* ==== CANNON BALLS UPDATE ==== */
static float viz_cball_timer = 0;

static void viz_update_cballs(float dt, float cw, float ch, float x_off,
                               float top_pad_px, float vp_w, float vp_h,
                               float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    /* Spawn 2-5 cannon balls on beat */
    viz_cball_timer -= dt;
    if (viz_beat_hit && viz_cball_timer <= 0) {
        int count = 2 + (rand() % 4);
        for (int n = 0; n < count; n++)
            viz_spawn_cball(cw, ch, vp_w, viz_kick);
        viz_cball_timer = 0.3f;
    }
    /* Passive spawn */
    static float cball_passive = 0;
    cball_passive -= dt;
    if (cball_passive <= 0) {
        int count = 2 + (rand() % 3);
        for (int n = 0; n < count; n++)
            viz_spawn_cball(cw, ch, vp_w, 0.2f);
        cball_passive = viz_randf_range(2.5f, 6.0f);
    }

    /* Update cannon balls */
    for (int i = 0; i < VIZ_MAX_CBALLS; i++) {
        viz_cball_t *cb = &viz_cballs[i];
        if (!cb->active) continue;

        cb->vy += VIZ_GRAVITY * dt;
        cb->x += cb->vx * dt;
        cb->y += cb->vy * dt;
        cb->angle += cb->ang_vel * dt;
        cb->life -= dt;

        /* Bounce off walls */
        if (cb->x < cb->radius) { cb->x = cb->radius; cb->vx = fabsf(cb->vx) * 0.7f; }
        if (cb->x > vp_w - cb->radius) { cb->x = vp_w - cb->radius; cb->vx = -fabsf(cb->vx) * 0.7f; }

        /* Collision with text grid */
        int cr_min = (int)((cb->y - cb->radius - top_pad_px) / ch);
        int cr_max = (int)((cb->y + cb->radius - top_pad_px) / ch);
        int cc_min = (int)((cb->x - cb->radius - x_off) / cw);
        int cc_max = (int)((cb->x + cb->radius - x_off) / cw);
        if (cr_min < 0) cr_min = 0;
        if (cc_min < 0) cc_min = 0;
        if (cr_max >= TERM_ROWS) cr_max = TERM_ROWS - 1;
        if (cc_max >= TERM_COLS) cc_max = TERM_COLS - 1;

        for (int r = cr_min; r <= cr_max; r++) {
            for (int c = cc_min; c <= cc_max; c++) {
                unsigned char byte = ansi_term.grid[r][c].ch;
                if (byte == 0 || byte == 32) continue;
                if (viz_cells[r][c].shattered) continue;
                float cx = x_off + c * cw + cw * 0.5f;
                float cy = top_pad_px + r * ch + ch * 0.5f;
                float dx = cb->x - cx, dy = cb->y - cy;
                float dist = sqrtf(dx*dx + dy*dy);
                if (dist < cb->radius + cw * 0.5f) {
                    float speed = sqrtf(cb->vx*cb->vx + cb->vy*cb->vy);
                    if (speed > VIZ_SHATTER_VEL * 0.8f) {
                        viz_shatter_cell(r, c, cb->vx, cb->vy,
                                        cw, ch, x_off, top_pad_px,
                                        tex_cw, tex_ch, hp_u, hp_v);
                        viz_spawn_debris(cx, cy, cb->vx * 0.4f, cb->vy * 0.4f, 3);
                    } else {
                        float push = (speed + 40.0f) * 0.4f;
                        float nx = (dist > 0.1f) ? dx / dist : 0;
                        float ny = (dist > 0.1f) ? dy / dist : 1;
                        viz_push_cell(r, c, -nx * push, -ny * push);
                    }
                    cb->vx *= 0.93f;
                    cb->vy *= 0.83f;
                }
            }
        }

        if (cb->y > vp_h + cb->radius * 2 || cb->life <= 0)
            cb->active = 0;
    }
}

/* ==== MATRIX RAIN UPDATE ==== */
static void viz_update_matrix(float dt, float cw, float ch, float x_off,
                                float top_pad_px, float vp_w, float vp_h,
                                float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    /* Energy-reactive spawn rate: sparse at low energy, dense when music builds */
    float energy = viz_kick + viz_mid * 0.5f + viz_treble * 0.3f;
    float spawn_rate = viz_beat_hit ? 0.01f : (0.4f - energy * 0.35f);
    if (spawn_rate < 0.03f) spawn_rate = 0.03f;
    if (spawn_rate > 0.5f) spawn_rate = 0.5f;

    /* Spawn new rain columns */
    static float rain_timer = 0;
    rain_timer -= dt;
    /* On beat, spawn a burst of multiple columns */
    int spawn_count = viz_beat_hit ? (2 + (int)(viz_kick * 4)) : 1;
    if (rain_timer <= 0) {
        for (int sc = 0; sc < spawn_count; sc++) {
            for (int i = 0; i < VIZ_MAX_RAIN; i++) {
                if (!viz_rain[i].active) {
                    viz_rain[i].active = 1;
                    viz_rain[i].col = rand() % TERM_COLS;
                    viz_rain[i].y = -ch * 3;
                    viz_rain[i].speed = viz_randf_range(200, 600) * (0.8f + viz_kick * 0.8f);
                    viz_rain[i].len = (int)viz_randf_range(5, 25);
                    for (int j = 0; j < 80; j++)
                        viz_rain[i].chars[j] = 33 + (rand() % 93);
                    break;
                }
            }
        }
        rain_timer = spawn_rate;
    }

    /* Update rain columns */
    for (int i = 0; i < VIZ_MAX_RAIN; i++) {
        viz_rain_t *rn = &viz_rain[i];
        if (!rn->active) continue;

        rn->y += rn->speed * dt;

        /* Randomize characters occasionally */
        if (rand() % 10 == 0) {
            int idx = rand() % rn->len;
            rn->chars[idx] = 33 + (rand() % 93);
        }

        /* Check collision with terminal text at head position */
        int head_row = (int)((rn->y - top_pad_px) / ch);
        int cc = rn->col;
        if (head_row >= 0 && head_row < TERM_ROWS && cc >= 0 && cc < TERM_COLS) {
            unsigned char byte = ansi_term.grid[head_row][cc].ch;
            if (byte != 0 && byte != 32 && !viz_cells[head_row][cc].shattered) {
                /* Splash! Shatter the cell and spawn green sparks */
                viz_shatter_cell(head_row, cc, 0, rn->speed * 0.3f,
                                cw, ch, x_off, top_pad_px,
                                tex_cw, tex_ch, hp_u, hp_v);
                /* Green sparks */
                for (int j = 0; j < 6; j++) {
                    viz_part_t *sp = viz_alloc_part();
                    if (!sp) break;
                    memset(sp, 0, sizeof(*sp));
                    sp->active = 1; sp->type = 2;
                    sp->x = x_off + cc * cw + cw * 0.5f;
                    sp->y = rn->y;
                    float a = viz_randf() * 6.28f;
                    float spd = viz_randf_range(100, 300);
                    sp->vx = cosf(a) * spd;
                    sp->vy = sinf(a) * spd - 100;
                    sp->life = viz_randf_range(0.3f, 0.8f);
                    sp->max_life = sp->life;
                    sp->r = 0.0f; sp->g = 1.0f; sp->b = viz_randf_range(0, 0.3f);
                    sp->a = 1.0f; sp->radius = 2;
                }
            }
        }

        /* Kill if off bottom */
        if (rn->y - rn->len * ch > vp_h + ch)
            rn->active = 0;
    }
}

/* ==== VECTROIDS SHIP UPDATE ==== */
/* Weapon types: 0=normal laser, 1=spread shot (6 orbs), 2=beam (thick line cut),
 * 3=cluster missiles */

/* Pick a random text cell as a target */
static void viz_ship_pick_target(viz_ship_t *s, float cw, float ch, float x_off, float top_pad_px) {
    int attempts = 20;
    while (attempts-- > 0) {
        int tr = rand() % TERM_ROWS;
        int tc = rand() % TERM_COLS;
        unsigned char byte = ansi_term.grid[tr][tc].ch;
        if (byte != 0 && byte != 32 && !viz_cells[tr][tc].shattered) {
            s->tx = x_off + tc * cw + cw * 0.5f;
            s->ty = top_pad_px + tr * ch + ch * 0.5f;
            return;
        }
    }
    /* Fallback: random screen position */
    s->tx = x_off + viz_randf_range(0.1f, 0.9f) * TERM_COLS * cw;
    s->ty = top_pad_px + viz_randf_range(0.1f, 0.9f) * TERM_ROWS * ch;
}

static void viz_update_one_ship(viz_ship_t *s, float dt, float cw, float ch,
                                 float x_off, float top_pad_px, float vp_w, float vp_h)
{
    int si = (int)(s - viz_ships);

    /* Initialize if needed */
    if (s->x == 0 && s->y == 0) {
        /* Spawn at different screen edges so they don't start together */
        switch (si % 4) {
            case 0: s->x = viz_randf_range(20, 80); s->y = viz_randf_range(50, vp_h - 50); break;
            case 1: s->x = vp_w - viz_randf_range(20, 80); s->y = viz_randf_range(50, vp_h - 50); break;
            case 2: s->x = viz_randf_range(50, vp_w - 50); s->y = viz_randf_range(20, 80); break;
            case 3: s->x = viz_randf_range(50, vp_w - 50); s->y = vp_h - viz_randf_range(20, 80); break;
        }
        s->angle = viz_randf() * 6.28f;
        s->wander_t = viz_randf_range(0.5f, 2.0f); /* stagger initial target pick */
        s->style = si % 5;
        s->style_phase = viz_randf() * 6.28f; /* random starting phase */
        s->dash_timer = viz_randf_range(2.0f, 5.0f);
        s->orbit_angle = viz_randf() * 6.28f;
        s->orbit_radius = viz_randf_range(60, 140);
        s->zig_dir = (si % 2) ? 1.0f : -1.0f;

        /* Distinct bright colors per ship */
        static const float ship_colors[][3] = {
            {0.0f, 1.0f, 1.0f},   /* cyan — hunter */
            {1.0f, 0.3f, 0.8f},   /* magenta — orbiter */
            {0.3f, 1.0f, 0.3f},   /* green — zigzag */
            {1.0f, 0.7f, 0.1f},   /* gold — drifter */
            {0.5f, 0.4f, 1.0f},   /* purple — patrol */
        };
        s->cr = ship_colors[s->style][0];
        s->cg = ship_colors[s->style][1];
        s->cb = ship_colors[s->style][2];
    }

    s->style_phase += dt;
    float dx, dy, ta, ad, dist;
    float thrust_amount, steer_rate, drag;

    switch (s->style) {

    case VIZ_FLY_HUNTER:
        /* Aggressive hunter: fast turns, bee-lines to text, quick retarget */
        s->wander_t -= dt;
        if (s->wander_t <= 0) {
            viz_ship_pick_target(s, cw, ch, x_off, top_pad_px);
            s->wander_t = viz_randf_range(0.8f, 2.0f);
        }
        dx = s->tx - s->x; dy = s->ty - s->y;
        ta = atan2f(dy, dx);
        ad = ta - s->angle;
        while (ad > 3.14159f) ad -= 6.28318f;
        while (ad < -3.14159f) ad += 6.28318f;
        s->angle += ad * 5.0f * dt; /* very fast turning */
        thrust_amount = 350.0f + viz_kick * 400.0f;
        s->vx += cosf(s->angle) * thrust_amount * dt;
        s->vy += sinf(s->angle) * thrust_amount * dt;
        s->vx *= (1.0f - 2.0f * dt);
        s->vy *= (1.0f - 2.0f * dt);
        break;

    case VIZ_FLY_ORBIT:
        /* Orbiter: circles around target, spiraling in for strafing runs */
        s->wander_t -= dt;
        if (s->wander_t <= 0) {
            viz_ship_pick_target(s, cw, ch, x_off, top_pad_px);
            s->wander_t = viz_randf_range(3.0f, 6.0f);
            s->orbit_radius = viz_randf_range(60, 160);
        }
        s->orbit_angle += dt * 1.8f; /* orbit speed */
        /* Slowly spiral inward */
        s->orbit_radius *= (1.0f - 0.15f * dt);
        if (s->orbit_radius < 30) s->orbit_radius = 30;
        {
            float ox = s->tx + cosf(s->orbit_angle) * s->orbit_radius;
            float oy = s->ty + sinf(s->orbit_angle) * s->orbit_radius;
            dx = ox - s->x; dy = oy - s->y;
            ta = atan2f(dy, dx);
            ad = ta - s->angle;
            while (ad > 3.14159f) ad -= 6.28318f;
            while (ad < -3.14159f) ad += 6.28318f;
            s->angle += ad * 3.0f * dt;
            /* Always face roughly toward the target center for shooting */
            float face_dx = s->tx - s->x, face_dy = s->ty - s->y;
            float face_a = atan2f(face_dy, face_dx);
            float face_ad = face_a - s->angle;
            while (face_ad > 3.14159f) face_ad -= 6.28318f;
            while (face_ad < -3.14159f) face_ad += 6.28318f;
            s->angle += face_ad * 1.5f * dt;
        }
        thrust_amount = 280.0f + viz_kick * 200.0f;
        s->vx += cosf(s->angle) * thrust_amount * dt;
        s->vy += sinf(s->angle) * thrust_amount * dt;
        s->vx *= (1.0f - 1.8f * dt);
        s->vy *= (1.0f - 1.8f * dt);
        break;

    case VIZ_FLY_ZIGZAG:
        /* Zigzag: fast horizontal/vertical sweeps, sharp direction changes */
        s->wander_t -= dt;
        if (s->wander_t <= 0) {
            viz_ship_pick_target(s, cw, ch, x_off, top_pad_px);
            s->wander_t = viz_randf_range(1.0f, 2.5f);
            s->zig_dir = -s->zig_dir; /* flip zigzag direction */
        }
        dx = s->tx - s->x; dy = s->ty - s->y;
        ta = atan2f(dy, dx);
        /* Add perpendicular zigzag oscillation */
        ta += s->zig_dir * sinf(s->style_phase * 4.0f) * 0.8f;
        ad = ta - s->angle;
        while (ad > 3.14159f) ad -= 6.28318f;
        while (ad < -3.14159f) ad += 6.28318f;
        s->angle += ad * 4.0f * dt;
        thrust_amount = 400.0f + viz_kick * 250.0f; /* fastest ship */
        s->vx += cosf(s->angle) * thrust_amount * dt;
        s->vy += sinf(s->angle) * thrust_amount * dt;
        s->vx *= (1.0f - 2.5f * dt);
        s->vy *= (1.0f - 2.5f * dt);
        break;

    case VIZ_FLY_DRIFT:
        /* Drifter: lazy spiraling float, gentle pulse on beat */
        s->wander_t -= dt;
        if (s->wander_t <= 0) {
            viz_ship_pick_target(s, cw, ch, x_off, top_pad_px);
            s->wander_t = viz_randf_range(3.0f, 7.0f);
        }
        dx = s->tx - s->x; dy = s->ty - s->y;
        ta = atan2f(dy, dx);
        ad = ta - s->angle;
        while (ad > 3.14159f) ad -= 6.28318f;
        while (ad < -3.14159f) ad += 6.28318f;
        s->angle += ad * 1.5f * dt;
        /* Lazy spiral: orbit offset that slowly rotates */
        s->orbit_angle += 0.8f * dt;
        {
            float spiral_r = 60.0f + sinf(s->style_phase * 0.3f) * 30.0f;
            float sx_off = cosf(s->orbit_angle) * spiral_r * dt;
            float sy_off = sinf(s->orbit_angle) * spiral_r * dt;
            s->vx += sx_off;
            s->vy += sy_off;
        }
        /* Gentle forward drift */
        thrust_amount = 100.0f + viz_kick * 60.0f;
        s->vx += cosf(s->angle) * thrust_amount * dt;
        s->vy += sinf(s->angle) * thrust_amount * dt;
        /* Sine wobble perpendicular to heading */
        s->vx += cosf(s->angle + 1.57f) * sinf(s->style_phase * 1.2f) * 50.0f * dt;
        s->vy += sinf(s->angle + 1.57f) * sinf(s->style_phase * 1.2f) * 50.0f * dt;
        /* Beat: gentle pulse outward, not a dash */
        if (viz_beat_hit) {
            float pulse = 80.0f;
            s->vx += cosf(s->angle) * pulse;
            s->vy += sinf(s->angle) * pulse;
            /* Also nudge orbit angle for variety */
            s->orbit_angle += 0.5f;
        }
        s->vx *= (1.0f - 2.0f * dt); /* moderate drag, no more infinite glides */
        s->vy *= (1.0f - 2.0f * dt);
        break;

    case VIZ_FLY_PATROL:
        /* Patrol: smooth figure-8 pattern overlaid on target seeking */
        s->wander_t -= dt;
        if (s->wander_t <= 0) {
            viz_ship_pick_target(s, cw, ch, x_off, top_pad_px);
            s->wander_t = viz_randf_range(4.0f, 8.0f);
        }
        {
            /* Figure-8 lissajous offset */
            float fig_x = sinf(s->style_phase * 0.7f) * 120.0f;
            float fig_y = sinf(s->style_phase * 1.4f) * 80.0f;
            float goal_x = s->tx + fig_x;
            float goal_y = s->ty + fig_y;
            dx = goal_x - s->x; dy = goal_y - s->y;
            ta = atan2f(dy, dx);
            ad = ta - s->angle;
            while (ad > 3.14159f) ad -= 6.28318f;
            while (ad < -3.14159f) ad += 6.28318f;
            s->angle += ad * 2.0f * dt; /* smooth turning */
        }
        thrust_amount = 200.0f + viz_kick * 150.0f;
        s->vx += cosf(s->angle) * thrust_amount * dt;
        s->vy += sinf(s->angle) * thrust_amount * dt;
        s->vx *= (1.0f - 1.3f * dt);
        s->vy *= (1.0f - 1.3f * dt);
        break;
    }

    /* Apply velocity */
    s->x += s->vx * dt;
    s->y += s->vy * dt;

    /* Wrap (but not if departing — let them fly off screen) */
    if (!s->departing) {
        if (s->x < -30) s->x += vp_w + 60;
        if (s->x > vp_w + 30) s->x -= vp_w + 60;
        if (s->y < -30) s->y += vp_h + 60;
        if (s->y > vp_h + 30) s->y -= vp_h + 60;
    }
}

static void viz_fire_laser(viz_ship_t *s, float angle, float speed) {
    viz_laser_t *l = viz_alloc_laser();
    if (!l) return;
    float nose_dist = 28.0f * viz_scale;
    l->active = 1;
    l->x = s->x + cosf(angle) * nose_dist;
    l->y = s->y + sinf(angle) * nose_dist;
    l->vx = cosf(angle) * speed + s->vx * 0.3f;
    l->vy = sinf(angle) * speed + s->vy * 0.3f;
    l->life = 1.8f;
    l->lr = s->cr;
    l->lg = s->cg;
    l->lb = s->cb;
    l->owner = (int)(s - viz_ships);
}

/* Spawn vector explosion at position — line fragments flying outward */
static void viz_spawn_explosion(float x, float y, float cr, float cg, float cb,
                                 int count, float force) {
    for (int i = 0; i < count; i++) {
        viz_part_t *p = viz_alloc_part();
        if (!p) break;
        memset(p, 0, sizeof(*p));
        p->active = 1;
        p->type = 2; /* spark */
        p->radius = viz_randf_range(1.5f, 4.0f) * viz_scale;
        p->x = x + viz_randf_range(-8, 8) * viz_scale;
        p->y = y + viz_randf_range(-8, 8) * viz_scale;
        float a = viz_randf() * 6.28f;
        float spd = viz_randf_range(force * 0.3f, force);
        p->vx = cosf(a) * spd;
        p->vy = sinf(a) * spd;
        p->life = viz_randf_range(0.5f, 2.0f);
        p->max_life = p->life;
        /* Mix in some white flash */
        float flash = viz_randf_range(0.0f, 0.5f);
        p->r = cr * (1.0f - flash) + flash;
        p->g = cg * (1.0f - flash) + flash;
        p->b = cb * (1.0f - flash) + flash;
        p->a = 1.0f;
    }
}

/* Spawn vector shatter: line fragments along triangle edges that spin & fade */
static void viz_spawn_ship_shatter(float x, float y, float angle,
                                    float cr, float cg, float cb) {
    float sc = viz_scale;
    float ca = cosf(angle), sa = sinf(angle);
    float nose_x = x + ca * 28 * sc, nose_y = y + sa * 28 * sc;
    float lw_x = x + cosf(angle + 2.5f) * 20 * sc;
    float lw_y = y + sinf(angle + 2.5f) * 20 * sc;
    float rw_x = x + cosf(angle - 2.5f) * 20 * sc;
    float rw_y = y + sinf(angle - 2.5f) * 20 * sc;
    /* 3 edges, break each into 2-3 segments */
    float edges[][4] = {
        { nose_x, nose_y, lw_x, lw_y },
        { nose_x, nose_y, rw_x, rw_y },
        { lw_x, lw_y, rw_x, rw_y },
    };
    for (int e = 0; e < 3; e++) {
        float ex0 = edges[e][0], ey0 = edges[e][1];
        float ex1 = edges[e][2], ey1 = edges[e][3];
        int segs = 2 + (rand() % 2); /* 2-3 segments per edge */
        for (int s = 0; s < segs; s++) {
            float t0 = (float)s / segs;
            float t1 = (float)(s + 1) / segs;
            float mx = (ex0 + (ex1 - ex0) * (t0 + t1) * 0.5f);
            float my = (ey0 + (ey1 - ey0) * (t0 + t1) * 0.5f);
            /* Fragment flies outward from ship center */
            float dx = mx - x, dy = my - y;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist < 1) dist = 1;
            viz_part_t *p = viz_alloc_part();
            if (!p) continue;
            memset(p, 0, sizeof(*p));
            p->active = 1;
            p->type = 2;
            p->radius = 3.0f * sc; /* visible fragment size */
            p->x = mx;
            p->y = my;
            float spd = viz_randf_range(80, 250);
            p->vx = (dx / dist) * spd + viz_randf_range(-40, 40);
            p->vy = (dy / dist) * spd + viz_randf_range(-40, 40);
            p->life = viz_randf_range(1.5f, 3.0f); /* long fade like classic vector */
            p->max_life = p->life;
            p->r = cr; p->g = cg; p->b = cb;
            p->a = 1.0f;
        }
    }
    /* Bright center flash */
    viz_spawn_explosion(x, y, 1.0f, 1.0f, 1.0f, 8, 200.0f);
}

/* Shatter text cells in a radius around a point */
static void viz_explode_text_radius(float cx, float cy, float radius,
                                     float cw, float ch, float x_off, float top_pad_px,
                                     float tex_cw, float tex_ch, float hp_u, float hp_v) {
    int cr_min = (int)((cy - radius - top_pad_px) / ch);
    int cr_max = (int)((cy + radius - top_pad_px) / ch);
    int cc_min = (int)((cx - radius - x_off) / cw);
    int cc_max = (int)((cx + radius - x_off) / cw);
    if (cr_min < 0) cr_min = 0;
    if (cc_min < 0) cc_min = 0;
    if (cr_max >= TERM_ROWS) cr_max = TERM_ROWS - 1;
    if (cc_max >= TERM_COLS) cc_max = TERM_COLS - 1;
    for (int r = cr_min; r <= cr_max; r++) {
        for (int c = cc_min; c <= cc_max; c++) {
            if (viz_cells[r][c].shattered) continue;
            unsigned char byte = ansi_term.grid[r][c].ch;
            if (byte == 0 || byte == 32) continue;
            float px = x_off + c * cw + cw * 0.5f;
            float py = top_pad_px + r * ch + ch * 0.5f;
            float dx = px - cx, dy = py - cy;
            if (dx*dx + dy*dy < radius * radius) {
                viz_shatter_cell(r, c, dx * 3.0f, dy * 3.0f,
                                cw, ch, x_off, top_pad_px,
                                tex_cw, tex_ch, hp_u, hp_v);
            }
        }
    }
}

/* ==== UFO UPDATE ==== */
static void viz_spawn_ufo(float vp_w, float vp_h, float top_pad_px) {
    memset(&viz_ufo, 0, sizeof(viz_ufo));
    viz_ufo.active = 1;
    viz_ufo.hp = 5;
    float min_y = top_pad_px + 40 * viz_scale;
    float max_y = top_pad_px + vp_h * 0.35f;
    viz_ufo.y = viz_randf_range(min_y, max_y);
    viz_ufo.direction = 1;
    viz_ufo.x = -60 * viz_scale;
    float base_speed = viz_randf_range(40, 80) * viz_scale;
    viz_ufo.vx = base_speed;
    viz_ufo.target_vx = base_speed;
    viz_ufo.stay_timer = viz_randf_range(10.0f, 20.0f);
    viz_ufo.dash_timer = viz_randf_range(3.0f, 6.0f); /* first dash after a few sec */
    /* Find rightmost text column */
    viz_ufo.text_right_edge = vp_w * 0.85f; /* fallback */
}

static void viz_update_ufo(float dt, float cw, float ch, float x_off,
                            float top_pad_px, float vp_w, float vp_h,
                            float tex_cw, float tex_ch, float hp_u, float hp_v,
                            int has_music) {
    /* Cooldown management */
    if (!viz_ufo.active) {
        if (viz_ufo.explode_timer > 0) {
            viz_ufo.explode_timer -= dt;
            return;
        }
        viz_ufo_cooldown -= dt;
        if (has_music) {
            float energy = viz_kick + viz_mid;
            if (viz_ufo_cooldown <= 0 && energy > 0.6f) {
                viz_spawn_ufo(vp_w, vp_h, top_pad_px);
                viz_ufo.stay_timer = 15.0f;
            }
        } else {
            if (viz_ufo_cooldown <= 0) {
                viz_spawn_ufo(vp_w, vp_h, top_pad_px);
            }
        }
        return;
    }

    /* Find the right edge of terminal text (so UFO knows where letters end) */
    {
        float right_edge = x_off;
        for (int c = TERM_COLS - 1; c >= 0; c--) {
            int found = 0;
            for (int r = 0; r < TERM_ROWS && r < 10; r++) {
                if (ansi_term.grid[r][c].ch > 32) { found = 1; break; }
            }
            if (found) { right_edge = x_off + (c + 1) * cw; break; }
        }
        viz_ufo.text_right_edge = right_edge;
    }

    /* ---- Acceleration: smooth toward target_vx ---- */
    float accel = 120.0f * viz_scale; /* px/s^2 */
    if (viz_ufo.vx < viz_ufo.target_vx)
        viz_ufo.vx += accel * dt;
    else if (viz_ufo.vx > viz_ufo.target_vx)
        viz_ufo.vx -= accel * dt;
    /* Clamp to not overshoot */
    if (fabsf(viz_ufo.vx - viz_ufo.target_vx) < accel * dt * 1.5f)
        viz_ufo.vx = viz_ufo.target_vx;

    /* Move UFO */
    viz_ufo.x += viz_ufo.vx * dt;
    viz_ufo.stay_timer -= dt;

    /* ---- Boundary awareness: reverse if past the text right edge ---- */
    float right_margin = viz_ufo.text_right_edge + 80 * viz_scale;
    if (viz_ufo.x > right_margin && viz_ufo.vx > 0 && !viz_ufo.dashing) {
        /* Too far right — reverse direction */
        viz_ufo.direction = -1;
        viz_ufo.target_vx = -fabsf(viz_ufo.target_vx);
    }
    /* Don't go too far left either */
    float left_margin = x_off - 40 * viz_scale;
    if (viz_ufo.x < left_margin && viz_ufo.vx < 0 && !viz_ufo.dashing) {
        viz_ufo.direction = 1;
        viz_ufo.target_vx = fabsf(viz_ufo.target_vx);
    }

    /* ---- Dash / reposition pattern ---- */
    viz_ufo.dash_timer -= dt;
    if (viz_ufo.dashing) {
        viz_ufo.dash_dur -= dt;
        if (viz_ufo.dash_dur <= 0) {
            /* Dash finished — slow down, start charging beam again */
            viz_ufo.dashing = 0;
            float cruise = viz_randf_range(30, 70) * viz_scale;
            viz_ufo.target_vx = viz_ufo.direction * cruise;
            viz_ufo.laser_charge = 0; /* restart charge for beam */
            viz_ufo.dash_timer = viz_randf_range(3.0f, 7.0f);
        }
    } else if (viz_ufo.dash_timer <= 0 && viz_ufo.firing) {
        /* Initiate dash: turn off laser, speed up to reposition */
        viz_ufo.firing = 0;
        viz_ufo.dashing = 1;
        viz_ufo.dash_dur = viz_randf_range(0.8f, 2.0f);
        viz_ufo.laser_width = 0;
        viz_ufo.laser_time = 0;
        /* Dash in current direction, faster */
        float dash_speed = viz_randf_range(200, 400) * viz_scale;
        viz_ufo.target_vx = viz_ufo.direction * dash_speed;
        /* Sometimes change direction during dash */
        if (viz_randf() < 0.3f) {
            viz_ufo.direction = -viz_ufo.direction;
            viz_ufo.target_vx = -viz_ufo.target_vx;
        }
    }

    /* ---- Beam charging & firing ---- */
    if (!viz_ufo.firing && !viz_ufo.dashing) {
        viz_ufo.laser_charge += dt;
        if (viz_ufo.laser_charge > 2.0f) {
            viz_ufo.firing = 1;
            viz_ufo.laser_time = 0;
            /* Slow down while firing */
            float cruise = viz_randf_range(20, 50) * viz_scale;
            viz_ufo.target_vx = viz_ufo.direction * cruise;
        }
    }

    /* Update beam */
    if (viz_ufo.firing) {
        viz_ufo.laser_time += dt;
        float max_width = 40.0f * viz_scale;
        viz_ufo.laser_width = max_width * (1.0f - expf(-viz_ufo.laser_time * 2.0f));
        viz_ufo.laser_alpha = 0.5f + 0.3f * sinf(viz_ufo.laser_time * 8.0f);

        /* Decimate text in vertical column below UFO */
        float beam_x = viz_ufo.x;
        float half_w = viz_ufo.laser_width * 0.5f;
        int cc_min = (int)((beam_x - half_w - x_off) / cw);
        int cc_max = (int)((beam_x + half_w - x_off) / cw);
        if (cc_min < 0) cc_min = 0;
        if (cc_max >= TERM_COLS) cc_max = TERM_COLS - 1;
        for (int r = 0; r < TERM_ROWS; r++) {
            for (int c = cc_min; c <= cc_max; c++) {
                if (viz_cells[r][c].shattered) continue;
                unsigned char byte = ansi_term.grid[r][c].ch;
                if (byte == 0 || byte == 32) continue;
                viz_shatter_cell(r, c, 0, 300.0f,
                                cw, ch, x_off, top_pad_px,
                                tex_cw, tex_ch, hp_u, hp_v);
            }
        }
        /* Beam sparks */
        if (rand() % 3 == 0) {
            float spark_y = viz_randf_range(viz_ufo.y + 20 * viz_scale, vp_h);
            viz_spawn_debris(beam_x + viz_randf_range(-half_w, half_w), spark_y,
                            0, 200, 2);
        }
    }

    /* UFO leaves when timer expires or way off screen */
    if (viz_ufo.stay_timer <= 0 ||
        viz_ufo.x < -200 * viz_scale || viz_ufo.x > vp_w + 200 * viz_scale) {
        viz_ufo.active = 0;
        viz_ufo_cooldown = has_music ? 45.0f : viz_randf_range(120.0f, 180.0f);
    }
}

static void viz_kill_ufo(float cw, float ch, float x_off, float top_pad_px,
                          float tex_cw, float tex_ch, float hp_u, float hp_v) {
    /* Big vector explosion */
    viz_spawn_explosion(viz_ufo.x, viz_ufo.y, 0.8f, 0.2f, 1.0f, 40, 500.0f);
    viz_spawn_explosion(viz_ufo.x, viz_ufo.y, 1.0f, 1.0f, 1.0f, 20, 300.0f);
    /* Shatter text in blast radius */
    viz_explode_text_radius(viz_ufo.x, viz_ufo.y, 120.0f * viz_scale,
                            cw, ch, x_off, top_pad_px, tex_cw, tex_ch, hp_u, hp_v);
    viz_ufo.active = 0;
    viz_ufo.explode_timer = 3.0f; /* explosion animation time */
    viz_ufo_cooldown = 60.0f;
}

static void viz_update_vectroids(float dt, float cw, float ch, float x_off,
                                   float top_pad_px, float vp_w, float vp_h,
                                   float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    /* Detect music vs idle */
    float raw_energy = viz_kick + viz_mid * 0.5f + viz_treble * 0.3f;
    viz_music_energy = viz_music_energy * 0.95f + raw_energy * 0.05f;
    int has_music = (viz_music_energy > VIZ_MUSIC_THRESHOLD);

    /* ---- Ship count management ---- */
    if (has_music) {
        /* Music: 3-5 ships based on energy intensity */
        float energy = viz_kick + viz_mid * 0.5f;
        int target_ships = 3 + (int)(energy * 2.5f);
        if (target_ships > VIZ_MAX_SHIPS) target_ships = VIZ_MAX_SHIPS;
        if (target_ships < 3) target_ships = 3;
        if (target_ships > viz_ship_count) viz_ship_count = target_ships;
        /* Slowly reduce in slow sections */
        static float music_reduce_timer = 0;
        music_reduce_timer -= dt;
        if (music_reduce_timer <= 0 && viz_ship_count > target_ships) {
            viz_ship_count--;
            music_reduce_timer = 3.0f;
        }
        /* All ships present during music */
        for (int i = 0; i < viz_ship_count; i++)
            viz_ships[i].present = 1;
    } else {
        /* No music: 1-3 ships, 2nd occasional, 3rd rare */
        viz_ship_count = 1; /* always at least 1 */
        viz_ships[0].present = 1;
        for (int i = 1; i < 3; i++) {
            viz_ship_t *s = &viz_ships[i];
            /* Handle departing ships — they fly off, then go non-present */
            if (s->departing && s->present) {
                if (s->x < -60 || s->x > vp_w + 60 ||
                    s->y < -60 || s->y > vp_h + 60) {
                    s->present = 0;
                    s->departing = 0;
                    s->presence_timer = viz_randf_range(15.0f, 40.0f);
                }
                /* Keep ship in ship_count while departing so it still updates/draws */
                if (s->present && viz_ship_count < i + 1)
                    viz_ship_count = i + 1;
                continue;
            }
            s->presence_timer -= dt;
            if (s->presence_timer <= 0) {
                if (s->present && s->alive) {
                    /* Ship begins exit — pick a natural exit trajectory */
                    s->departing = 1;
                    s->target_type = 0; /* stop fighting */
                    /* Aim toward nearest screen edge */
                    float edge_l = s->x, edge_r = vp_w - s->x;
                    float edge_t = s->y, edge_b = vp_h - s->y;
                    float min_edge = edge_l;
                    float ex_ang = 3.14159f; /* left */
                    if (edge_r < min_edge) { min_edge = edge_r; ex_ang = 0; }
                    if (edge_t < min_edge) { min_edge = edge_t; ex_ang = -1.5708f; }
                    if (edge_b < min_edge) { ex_ang = 1.5708f; }
                    /* Add some randomness to exit angle */
                    ex_ang += viz_randf_range(-0.4f, 0.4f);
                    s->angle = ex_ang;
                    s->vx = cosf(ex_ang) * 300.0f;
                    s->vy = sinf(ex_ang) * 300.0f;
                    s->wander_t = 99.0f; /* don't retarget */
                } else if (!s->present) {
                    /* Ship arrives — chance depends on ship index */
                    float chance = (i == 1) ? 0.5f : 0.25f;
                    if (viz_randf() < chance) {
                        s->present = 1;
                        s->departing = 0;
                        s->x = 0; s->y = 0;
                        s->alive = 1;
                        s->hp = 3;
                    }
                    s->presence_timer = viz_randf_range(8.0f, 25.0f);
                }
            }
            if (s->present) {
                if (viz_ship_count < i + 1) viz_ship_count = i + 1;
            }
        }
        /* Ships 3-4 only during music */
        for (int i = 3; i < VIZ_MAX_SHIPS; i++) {
            viz_ships[i].present = 0;
        }
    }

    /* ---- Update ships ---- */
    for (int si = 0; si < viz_ship_count; si++) {
        viz_ship_t *s = &viz_ships[si];
        if (!s->present) continue;

        /* Handle respawn */
        if (!s->alive) {
            s->respawn_timer -= dt;
            if (s->respawn_timer <= 0) {
                s->alive = 1;
                s->hp = 3;
                s->x = 0; s->y = 0; /* trigger init in update_one_ship */
                s->target_type = 0;
            }
            continue;
        }

        /* Random chance to start fighting another ship (not while departing) */
        if (s->target_type == 0 && viz_ship_count > 1 && !s->departing) {
            float fight_chance = dt * 0.03f; /* ~3% per second */
            if (viz_randf() < fight_chance) {
                /* Pick a random other alive ship */
                int target = rand() % viz_ship_count;
                if (target != si && viz_ships[target].alive && viz_ships[target].present) {
                    s->target_type = 1;
                    s->target_ship = target;
                    s->fight_timer = viz_randf_range(5.0f, 15.0f);
                }
            }
        }
        /* Random chance to target UFO (not while departing) */
        if (s->target_type == 0 && viz_ufo.active && !s->departing) {
            float ufo_chance = dt * 0.05f; /* ~5% per second */
            if (viz_randf() < ufo_chance) {
                s->target_type = 2;
                s->fight_timer = viz_randf_range(5.0f, 12.0f);
            }
        }

        /* Update fight timer */
        if (s->target_type > 0) {
            s->fight_timer -= dt;
            if (s->fight_timer <= 0) {
                s->target_type = 0; /* back to text */
            }
            /* If targeting a dead/absent ship, stop */
            if (s->target_type == 1) {
                viz_ship_t *ts = &viz_ships[s->target_ship];
                if (!ts->alive || !ts->present)
                    s->target_type = 0;
            }
            if (s->target_type == 2 && !viz_ufo.active)
                s->target_type = 0;
        }

        /* Steer toward target */
        if (s->target_type == 1) {
            /* Fighting another ship: override target position */
            viz_ship_t *ts = &viz_ships[s->target_ship];
            s->tx = ts->x;
            s->ty = ts->y;
            s->wander_t = 0.5f; /* keep retargeting */
        } else if (s->target_type == 2) {
            /* Fighting UFO */
            s->tx = viz_ufo.x;
            s->ty = viz_ufo.y;
            s->wander_t = 0.5f;
        }

        viz_update_one_ship(s, dt, cw, ch, x_off, top_pad_px, vp_w, vp_h);
    }

    /* ---- Fire weapons ---- */
    int should_fire = 0;
    if (has_music) {
        should_fire = viz_beat_hit;
    } else {
        /* Occasional shooting without music */
        viz_idle_fire_timer -= dt;
        if (viz_idle_fire_timer <= 0) {
            should_fire = 1;
            viz_idle_fire_timer = viz_randf_range(3.0f, 8.0f);
        }
    }

    if (should_fire) {
        for (int si = 0; si < viz_ship_count; si++) {
            viz_ship_t *s = &viz_ships[si];
            if (!s->alive || !s->present || s->departing) continue;
            if (s->firing > 0) { s->firing--; continue; }

            float fire_angle = s->angle;
            /* If targeting ship/UFO, aim at target */
            if (s->target_type > 0) {
                float tdx = s->tx - s->x, tdy = s->ty - s->y;
                fire_angle = atan2f(tdy, tdx);
            }

            int weapon = 0;
            if (has_music) {
                if ((viz_beat_num % 4) == 0) weapon = 1;
                if ((viz_beat_num % 8) == 0) weapon = 2;
                if ((viz_beat_num % 12) == 0) weapon = 3;
            }

            if (weapon == 0) {
                viz_fire_laser(s, fire_angle, 900.0f);
                s->firing = 2;
            } else if (weapon == 1) {
                for (int j = -2; j <= 3; j++) {
                    float spread_a = fire_angle + j * 0.18f;
                    viz_fire_laser(s, spread_a, 600.0f);
                }
                s->firing = 3;
            } else if (weapon == 2) {
                float ca = cosf(fire_angle), sa = sinf(fire_angle);
                for (float d = 20; d < 400; d += ch * 0.5f) {
                    float bx = s->x + ca * d;
                    float by = s->y + sa * d;
                    int br = (int)((by - top_pad_px) / ch);
                    int bc = (int)((bx - x_off) / cw);
                    for (int wo = -1; wo <= 1; wo++) {
                        int cr = br + wo;
                        if (cr >= 0 && cr < TERM_ROWS && bc >= 0 && bc < TERM_COLS) {
                            if (!viz_cells[cr][bc].shattered && ansi_term.grid[cr][bc].ch > 32) {
                                viz_shatter_cell(cr, bc, ca * 200, sa * 200,
                                                cw, ch, x_off, top_pad_px,
                                                tex_cw, tex_ch, hp_u, hp_v);
                            }
                        }
                    }
                }
                for (int j = 0; j < 12; j++) {
                    float d = viz_randf_range(20, 350);
                    viz_spawn_debris(s->x + ca * d, s->y + sa * d,
                                    ca * 100, sa * 100, 2);
                }
                viz_fire_laser(s, fire_angle, 1200.0f);
                s->firing = 5;
            } else if (weapon == 3) {
                for (int j = 0; j < 8; j++) {
                    float a = fire_angle + j * (6.28318f / 8) + viz_randf_range(-0.2f, 0.2f);
                    viz_fire_laser(s, a, viz_randf_range(400, 700));
                }
                s->firing = 4;
            }
        }
    }

    /* Decrement firing cooldown */
    for (int si = 0; si < viz_ship_count; si++)
        if (viz_ships[si].firing > 0 && !should_fire) viz_ships[si].firing--;

    /* ---- Update UFO ---- */
    viz_update_ufo(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                   tex_cw, tex_ch, hp_u, hp_v, has_music);

    /* ---- Update lasers ---- */
    for (int i = 0; i < VIZ_MAX_LASERS; i++) {
        viz_laser_t *l = &viz_lasers[i];
        if (!l->active) continue;
        l->x += l->vx * dt;
        l->y += l->vy * dt;
        l->life -= dt;
        if (l->life <= 0 || l->x < -20 || l->x > vp_w + 20 ||
            l->y < -20 || l->y > vp_h + 20) {
            l->active = 0;
            continue;
        }

        /* Laser-ship collision (ship-vs-ship combat) */
        for (int si = 0; si < viz_ship_count; si++) {
            viz_ship_t *s = &viz_ships[si];
            if (!s->alive || !s->present) continue;
            if (l->owner == si) continue; /* don't hit yourself */
            float dx = l->x - s->x, dy = l->y - s->y;
            float hit_r = 18.0f * viz_scale;
            if (dx*dx + dy*dy < hit_r * hit_r) {
                s->hp--;
                viz_spawn_debris(l->x, l->y, l->vx * 0.1f, l->vy * 0.1f, 6);
                l->active = 0;
                if (s->hp <= 0) {
                    /* Ship shatters into vector pieces! */
                    viz_spawn_ship_shatter(s->x, s->y, s->angle, s->cr, s->cg, s->cb);
                    viz_explode_text_radius(s->x, s->y, 60.0f * viz_scale,
                                            cw, ch, x_off, top_pad_px,
                                            tex_cw, tex_ch, hp_u, hp_v);
                    s->alive = 0;
                    s->respawn_timer = 10.0f;
                    /* Anyone targeting this ship should stop */
                    for (int j = 0; j < viz_ship_count; j++) {
                        if (viz_ships[j].target_type == 1 && viz_ships[j].target_ship == si)
                            viz_ships[j].target_type = 0;
                    }
                }
                break; /* laser consumed */
            }
        }
        if (!l->active) continue;

        /* Laser-UFO collision */
        if (viz_ufo.active) {
            float dx = l->x - viz_ufo.x, dy = l->y - viz_ufo.y;
            float hit_r = 30.0f * viz_scale;
            if (dx*dx + dy*dy < hit_r * hit_r) {
                viz_ufo.hp--;
                viz_spawn_debris(l->x, l->y, l->vx * 0.1f, l->vy * 0.1f, 8);
                l->active = 0;
                if (viz_ufo.hp <= 0) {
                    viz_kill_ufo(cw, ch, x_off, top_pad_px,
                                 tex_cw, tex_ch, hp_u, hp_v);
                }
                continue;
            }
        }

        /* Laser-text collision */
        int lr = (int)((l->y - top_pad_px) / ch);
        int lc = (int)((l->x - x_off) / cw);
        if (lr >= 0 && lr < TERM_ROWS && lc >= 0 && lc < TERM_COLS) {
            unsigned char byte = ansi_term.grid[lr][lc].ch;
            if (byte != 0 && byte != 32 && !viz_cells[lr][lc].shattered) {
                viz_shatter_cell(lr, lc, l->vx * 0.2f, l->vy * 0.2f,
                                cw, ch, x_off, top_pad_px,
                                tex_cw, tex_ch, hp_u, hp_v);
                viz_spawn_debris(l->x, l->y, l->vx * 0.1f, l->vy * 0.1f, 8);
                l->active = 0;
            }
        }
    }
}

/* ==== MAIN UPDATE (called once per frame) ==== */
static void viz_update(float dt, float cw, float ch, float x_off,
                        float top_pad_px, float vp_w, float vp_h,
                        float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    if (viz_mode == VIZ_NONE) return;
    viz_time += dt;
    viz_scale = vp_h / 1080.0f;
    if (viz_scale < 0.5f) viz_scale = 0.5f;

    /* Read beat state once per frame */
    viz_read_beat();
    viz_beat_flash *= 0.90f; /* decay flash */

    /* Update cell springs (shared) */
    for (int r = 0; r < TERM_ROWS && r < VIZ_ROWS; r++) {
        for (int c = 0; c < TERM_COLS && c < VIZ_COLS; c++) {
            viz_cell_t *cell = &viz_cells[r][c];
            if (cell->shattered) {
                cell->shatter_t += dt;
                if (cell->shatter_t > VIZ_CELL_RESPAWN)
                    cell->shattered = 0;
                continue;
            }
            /* Spring return to origin */
            cell->vx += -VIZ_SPRING_K * cell->dx * dt * 60.0f;
            cell->vy += -VIZ_SPRING_K * cell->dy * dt * 60.0f;
            cell->vx *= VIZ_SPRING_DAMP;
            cell->vy *= VIZ_SPRING_DAMP;
            cell->dx += cell->vx * dt;
            cell->dy += cell->vy * dt;
            /* Clamp displacement */
            if (cell->dx > cw * 3) cell->dx = cw * 3;
            if (cell->dx < -cw * 3) cell->dx = -cw * 3;
            if (cell->dy > ch * 3) cell->dy = ch * 3;
            if (cell->dy < -ch * 3) cell->dy = -ch * 3;
        }
    }

    /* Update fragments (shared) */
    for (int i = 0; i < VIZ_MAX_FRAGMENTS; i++) {
        viz_frag_t *f = &viz_frags[i];
        if (!f->active) continue;
        f->vy += VIZ_GRAVITY * dt;
        f->x += f->vx * dt;
        f->y += f->vy * dt;
        f->angle += f->ang_vel * dt;
        f->life -= dt;
        f->a = f->life > 0.5f ? 1.0f : f->life * 2.0f;
        if (f->life <= 0 || f->y > vp_h + 50)
            f->active = 0;
    }

    /* Update sparks/debris (shared) */
    for (int i = 0; i < VIZ_MAX_PARTICLES; i++) {
        viz_part_t *p = &viz_parts[i];
        if (!p->active || p->type == 0) continue; /* boulders handled per-mode */
        p->vy += VIZ_GRAVITY * 0.5f * dt;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->life -= dt;
        p->a = p->life / p->max_life;
        if (p->life <= 0) p->active = 0;
    }

    /* Mode-specific update */
    switch (viz_mode) {
        case VIZ_BLADES:
            viz_update_blades(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                                tex_cw, tex_ch, hp_u, hp_v);
            break;
        case VIZ_CANNONBALLS:
            viz_update_cballs(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                              tex_cw, tex_ch, hp_u, hp_v);
            break;
        case VIZ_MATRIX:
            viz_update_matrix(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                              tex_cw, tex_ch, hp_u, hp_v);
            break;
        case VIZ_VECTROIDS:
            viz_update_vectroids(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                                  tex_cw, tex_ch, hp_u, hp_v);
            break;
        default: break;
    }
}

/* ==== RENDERING: push quads for particles, fragments, rain, ship, lasers ==== */
/* Called during vkt_build_vertices, after terminal text, before fx_split_quad.
 * Uses push_quad/push_quad_free which are defined in vk_terminal.c. */
static void viz_push_quads(float cw, float ch, float x_off, float top_pad_px,
                            int vp_w, int vp_h,
                            float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    if (viz_mode == VIZ_NONE) return;

    float beat_glow = viz_beat_flash;

    /* Solid fill UVs (glyph 219) */
    float su0 = (219 % 16) * tex_cw + hp_u;
    float sv0 = (219 / 16) * tex_ch + hp_v;
    float su1 = su0 + tex_cw - 2*hp_u;
    float sv1 = sv0 + tex_ch - 2*hp_v;

    #define VPX(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define VPY(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)

    /* Draw pixel saw blades / sparks */
    for (int i = 0; i < VIZ_MAX_PARTICLES; i++) {
        viz_part_t *p = &viz_parts[i];
        if (!p->active) continue;

        if (p->type == 0) {
            /* Pixel saw blade: jagged rotating disc made of pixel blocks */
            float rad = p->radius;
            int teeth = (p->glyph > 4) ? p->glyph : 10;  /* teeth count stored in glyph */
            float px_sz = rad * 0.25f; /* pixel block size */
            if (px_sz < 2.0f) px_sz = 2.0f;

            /* Draw filled disc as concentric pixel rings */
            for (float ring = 0; ring < rad; ring += px_sz) {
                int steps = (int)(6.28f * ring / px_sz);
                if (steps < 6) steps = 6;
                for (int s = 0; s < steps; s++) {
                    float a = p->angle + s * (6.28318f / steps);
                    /* Tooth pattern: every other tooth extends outward */
                    int tooth_idx = (int)((a - p->angle) / (6.28318f / teeth) + 0.5f) % teeth;
                    float tooth_ext = (tooth_idx % 2 == 0) ? 1.0f : 0.0f;
                    float max_r = rad + tooth_ext * rad * 0.4f;

                    if (ring > max_r) continue;

                    float sx = p->x + cosf(a) * ring;
                    float sy = p->y + sinf(a) * ring;

                    /* Shading: darker toward center, teeth tips bright */
                    float shade = 0.4f + 0.6f * (ring / rad);
                    if (ring > rad * 0.85f && tooth_ext > 0)
                        shade = 1.0f; /* bright tooth tip */
                    float edge_dark = (ring > rad * 0.9f && tooth_ext < 0.5f) ? 0.3f : 1.0f;
                    shade *= edge_dark;

                    /* Beat glow: brighten + shift color on beat */
                    float br = p->r * shade + beat_glow * 0.4f;
                    float bg = p->g * shade + beat_glow * 0.2f;
                    float bb = p->b * shade + beat_glow * 0.5f;
                    if (br > 1.0f) br = 1.0f;
                    if (bg > 1.0f) bg = 1.0f;
                    if (bb > 1.0f) bb = 1.0f;
                    push_quad(VPX(sx - px_sz*0.5f), VPY(sy - px_sz*0.5f),
                              VPX(sx + px_sz*0.5f), VPY(sy + px_sz*0.5f),
                              su0, sv0, su1, sv1,
                              br, bg, bb, p->a);
                }
            }
            /* Center hub — darker */
            float hub = rad * 0.25f;
            push_quad(VPX(p->x - hub), VPY(p->y - hub),
                      VPX(p->x + hub), VPY(p->y + hub),
                      su0, sv0, su1, sv1,
                      p->r * 0.3f, p->g * 0.3f, p->b * 0.35f, p->a);
            /* Center dot — bright spot */
            float dot = rad * 0.08f;
            if (dot < 1.5f) dot = 1.5f;
            push_quad(VPX(p->x - dot), VPY(p->y - dot),
                      VPX(p->x + dot), VPY(p->y + dot),
                      su0, sv0, su1, sv1,
                      0.9f, 0.9f, 1.0f, p->a);
        } else {
            /* Sparks/debris: tiny bright quads */
            float sz = p->radius;
            push_quad(VPX(p->x - sz), VPY(p->y - sz), VPX(p->x + sz), VPY(p->y + sz),
                      su0, sv0, su1, sv1, p->r, p->g, p->b, p->a);
        }
    }

    /* Draw cannon balls (raymarched 3D metallic spiked orbs) */
    for (int i = 0; i < VIZ_MAX_CBALLS; i++) {
        viz_cball_t *cb = &viz_cballs[i];
        if (!cb->active) continue;
        float rad = cb->radius;
        float px_sz = rad * 0.18f; /* scale pixel size with radius to cap quad count */
        if (px_sz < 2.0f) px_sz = 2.0f;

        /* Raymarched sphere: for each pixel in bounding box, compute sphere shading */
        float light_x = -0.5f, light_y = -0.7f, light_z = 0.7f; /* light direction */
        float ll = sqrtf(light_x*light_x + light_y*light_y + light_z*light_z);
        light_x /= ll; light_y /= ll; light_z /= ll;

        for (float py = -rad * 1.3f; py <= rad * 1.3f; py += px_sz) {
            for (float px = -rad * 1.3f; px <= rad * 1.3f; px += px_sz) {
                float dist2d = sqrtf(px*px + py*py);

                /* Check if pixel is on a spike */
                float spike_r = rad;
                float pixel_angle = atan2f(py, px) + cb->angle;
                /* Normalize angle to [0, 2pi) */
                while (pixel_angle < 0) pixel_angle += 6.28318f;
                while (pixel_angle >= 6.28318f) pixel_angle -= 6.28318f;
                float spike_frac = pixel_angle / (6.28318f / cb->spike_count);
                spike_frac = spike_frac - floorf(spike_frac); /* 0-1 within each spike sector */
                float spike_width = 0.15f; /* how wide the spike base is */
                int on_spike = (spike_frac < spike_width || spike_frac > (1.0f - spike_width));
                if (on_spike) {
                    /* Spike extends outward, tapers to point */
                    float taper = 1.0f - fabsf(spike_frac < 0.5f ? spike_frac : spike_frac - 1.0f) / spike_width;
                    spike_r = rad + rad * 0.35f * taper;
                }

                if (dist2d > spike_r) continue;

                /* Sphere normal for 3D shading */
                float nz_sq = 1.0f - (px*px + py*py) / (rad*rad);
                float nz, nx_n, ny_n;
                if (nz_sq > 0 && dist2d <= rad) {
                    nz = sqrtf(nz_sq);
                    nx_n = px / rad;
                    ny_n = py / rad;
                } else {
                    /* On spike (outside sphere radius) — flat normal pointing outward */
                    nz = 0.1f;
                    nx_n = (dist2d > 0.01f) ? px / dist2d : 0;
                    ny_n = (dist2d > 0.01f) ? py / dist2d : 0;
                }

                /* Diffuse lighting */
                float ndotl = nx_n * light_x + ny_n * light_y + nz * light_z;
                if (ndotl < 0) ndotl = 0;

                /* Specular (Blinn-Phong) */
                float hx = light_x, hy = light_y, hz = light_z + 1.0f; /* half-vector (view = 0,0,1) */
                float hl = sqrtf(hx*hx + hy*hy + hz*hz);
                hx /= hl; hy /= hl; hz /= hl;
                float ndoth = nx_n * hx + ny_n * hy + nz * hz;
                if (ndoth < 0) ndoth = 0;
                float spec = ndoth * ndoth;
                spec = spec * spec; /* pow4 */
                spec = spec * spec; /* pow8 */
                spec = spec * spec; /* pow16 — sharp highlight */

                /* Metallic color: dark iron + beat-reactive colorful pulse */
                float bp = beat_glow * 0.6f; /* beat pulse intensity */
                /* Cycle hue on beat for colorful flash */
                float bh = cb->metallic_hue + viz_time * 0.5f;
                float pulse_r = 0.5f + 0.5f * sinf(bh * 6.28f);
                float pulse_g = 0.5f + 0.5f * sinf(bh * 6.28f + 2.09f);
                float pulse_b = 0.5f + 0.5f * sinf(bh * 6.28f + 4.19f);
                float base_r = 0.45f + cb->metallic_hue + bp * pulse_r;
                float base_g = 0.42f + cb->metallic_hue * 0.5f + bp * pulse_g;
                float base_b = 0.48f + cb->metallic_hue + bp * pulse_b;
                float ambient = 0.15f;
                float fr = base_r * (ambient + ndotl * 0.65f) + spec * 0.8f;
                float fg = base_g * (ambient + ndotl * 0.65f) + spec * 0.7f;
                float fb = base_b * (ambient + ndotl * 0.65f) + spec * 0.9f;

                /* Rim lighting for metallic sheen */
                float rim = 1.0f - nz;
                rim = rim * rim * rim;
                fr += rim * 0.2f; fg += rim * 0.18f; fb += rim * 0.25f;

                /* Clamp */
                if (fr > 1.0f) fr = 1.0f;
                if (fg > 1.0f) fg = 1.0f;
                if (fb > 1.0f) fb = 1.0f;

                float sx = cb->x + px, sy = cb->y + py;
                push_quad(VPX(sx - px_sz*0.5f), VPY(sy - px_sz*0.5f),
                          VPX(sx + px_sz*0.5f), VPY(sy + px_sz*0.5f),
                          su0, sv0, su1, sv1,
                          fr, fg, fb, 1.0f);
            }
        }
        /* Soft glow halo on beat — rendered as fading concentric rings */
        if (beat_glow > 0.05f) {
            float bh2 = cb->metallic_hue + viz_time * 0.5f;
            float gR = 0.5f + 0.5f * sinf(bh2 * 6.28f);
            float gG = 0.5f + 0.5f * sinf(bh2 * 6.28f + 2.09f);
            float gB = 0.5f + 0.5f * sinf(bh2 * 6.28f + 4.19f);
            /* 3 concentric glow rings, fading outward */
            for (int ring = 1; ring <= 3; ring++) {
                float gr = rad * (1.0f + ring * 0.25f);
                float ga = beat_glow * (0.15f / ring);
                float bsz = px_sz * 1.5f;
                int steps = (int)(6.28f * gr / bsz);
                if (steps < 12) steps = 12;
                for (int gs = 0; gs < steps; gs++) {
                    float a = gs * (6.28318f / steps);
                    float gx = cb->x + cosf(a) * gr;
                    float gy = cb->y + sinf(a) * gr;
                    push_quad(VPX(gx - bsz*0.5f), VPY(gy - bsz*0.5f),
                              VPX(gx + bsz*0.5f), VPY(gy + bsz*0.5f),
                              su0, sv0, su1, sv1,
                              gR, gG, gB, ga);
                }
            }
        }
    }

    /* Draw fragments (shattered glyph pieces) */
    for (int i = 0; i < VIZ_MAX_FRAGMENTS; i++) {
        viz_frag_t *f = &viz_frags[i];
        if (!f->active) continue;
        push_quad(VPX(f->x), VPY(f->y), VPX(f->x + f->w), VPY(f->y + f->h),
                  f->u0, f->v0, f->u1, f->v1,
                  f->r, f->g, f->b, f->a);
    }

    /* Draw matrix rain characters */
    if (viz_mode == VIZ_MATRIX) {
        for (int i = 0; i < VIZ_MAX_RAIN; i++) {
            viz_rain_t *rn = &viz_rain[i];
            if (!rn->active) continue;
            for (int j = 0; j < rn->len; j++) {
                float cy = rn->y - j * ch;
                if (cy < -ch || cy > vp_h + ch) continue;
                float cx = x_off + rn->col * cw;
                int glyph = rn->chars[j % 80];
                float gu0 = (glyph % 16) * tex_cw + hp_u;
                float gv0 = (glyph / 16) * tex_ch + hp_v;
                float gu1 = gu0 + tex_cw - 2*hp_u;
                float gv1 = gv0 + tex_ch - 2*hp_v;
                /* Head is brightest, trail fades — beat glow pulses everything brighter */
                float bright = (j == 0) ? 1.0f : 0.7f * (1.0f - (float)j / (float)rn->len);
                bright += beat_glow * 0.4f; /* pulse on beat */
                if (bright > 1.0f) bright = 1.0f;
                float gr = beat_glow * 0.15f, gg = bright, gb = bright * 0.2f + beat_glow * 0.3f;
                if (j == 0) { gr = 0.6f + beat_glow * 0.4f; gg = 1.0f; gb = 0.6f + beat_glow * 0.4f; }
                push_quad(VPX(cx), VPY(cy), VPX(cx + cw), VPY(cy + ch),
                          gu0, gv0, gu1, gv1, gr, gg, gb, bright);
            }
        }
    }

    /* Draw Vectroids ships (clean vector triangles, resolution-scaled) */
    if (viz_mode == VIZ_VECTROIDS) {
      float sc = viz_scale;
      for (int si = 0; si < viz_ship_count; si++) {
        viz_ship_t *s = &viz_ships[si];
        if (!s->alive || !s->present) continue;
        if (s->x == 0 && s->y == 0) continue;
        float ca = cosf(s->angle), sa = sinf(s->angle);
        /* Resolution-scaled ship geometry */
        float nose_d = 28.0f * sc, wing_d = 20.0f * sc, rear_d = 12.0f * sc;
        float nose_x = s->x + ca * nose_d, nose_y = s->y + sa * nose_d;
        float lwing_x = s->x + cosf(s->angle + 2.5f) * wing_d;
        float lwing_y = s->y + sinf(s->angle + 2.5f) * wing_d;
        float rwing_x = s->x + cosf(s->angle - 2.5f) * wing_d;
        float rwing_y = s->y + sinf(s->angle - 2.5f) * wing_d;
        float rear_x = s->x - ca * rear_d, rear_y = s->y - sa * rear_d;
        float lw = 1.2f * sc; /* thinner lines for cleaner look */
        /* Per-ship color with beat glow */
        float sr = s->cr + beat_glow * 0.4f;
        float sg = s->cg + beat_glow * 0.3f;
        float sb = s->cb + beat_glow * 0.4f;
        if (sr > 1.0f) sr = 1.0f;
        if (sg > 1.0f) sg = 1.0f;
        if (sb > 1.0f) sb = 1.0f;

        /* ---- Multi-layer blurred drop shadow ---- */
        {
            float shd_off = 3.0f * sc;
            float sn_x = nose_x + shd_off, sn_y = nose_y + shd_off;
            float slw_x = lwing_x + shd_off, slw_y = lwing_y + shd_off;
            float srw_x = rwing_x + shd_off, srw_y = rwing_y + shd_off;
            float snx, sny, slen;
            /* 3 layers: wide+faint, medium, tight — simulates blur */
            float shd_layers[][2] = { {10.0f, 0.08f}, {6.0f, 0.15f}, {3.0f, 0.25f} };
            for (int sl = 0; sl < 3; sl++) {
                float sw = shd_layers[sl][0] * sc;
                float sa2 = shd_layers[sl][1];
                /* Nose→Left wing */
                snx = -(slw_y - sn_y); sny = (slw_x - sn_x);
                slen = sqrtf(snx*snx+sny*sny); if(slen>0){snx/=slen;sny/=slen;}
                push_quad_free(VPX(sn_x+snx*sw), VPY(sn_y+sny*sw),
                               VPX(slw_x+snx*sw), VPY(slw_y+sny*sw),
                               VPX(slw_x-snx*sw), VPY(slw_y-sny*sw),
                               VPX(sn_x-snx*sw), VPY(sn_y-sny*sw),
                               0,0,0, sa2);
                /* Nose→Right wing */
                snx = -(srw_y - sn_y); sny = (srw_x - sn_x);
                slen = sqrtf(snx*snx+sny*sny); if(slen>0){snx/=slen;sny/=slen;}
                push_quad_free(VPX(sn_x+snx*sw), VPY(sn_y+sny*sw),
                               VPX(srw_x+snx*sw), VPY(srw_y+sny*sw),
                               VPX(srw_x-snx*sw), VPY(srw_y-sny*sw),
                               VPX(sn_x-snx*sw), VPY(sn_y-sny*sw),
                               0,0,0, sa2);
                /* Rear */
                snx = -(srw_y - slw_y); sny = (srw_x - slw_x);
                slen = sqrtf(snx*snx+sny*sny); if(slen>0){snx/=slen;sny/=slen;}
                push_quad_free(VPX(slw_x+snx*sw), VPY(slw_y+sny*sw),
                               VPX(srw_x+snx*sw), VPY(srw_y+sny*sw),
                               VPX(srw_x-snx*sw), VPY(srw_y-sny*sw),
                               VPX(slw_x-snx*sw), VPY(slw_y-sny*sw),
                               0,0,0, sa2);
                /* Shadow vertex dots */
                float svr = sw * 0.7f;
                push_quad(VPX(sn_x-svr), VPY(sn_y-svr), VPX(sn_x+svr), VPY(sn_y+svr),
                          su0,sv0,su1,sv1, 0,0,0, sa2);
                push_quad(VPX(slw_x-svr), VPY(slw_y-svr), VPX(slw_x+svr), VPY(slw_y+svr),
                          su0,sv0,su1,sv1, 0,0,0, sa2);
                push_quad(VPX(srw_x-svr), VPY(srw_y-svr), VPX(srw_x+svr), VPY(srw_y+svr),
                          su0,sv0,su1,sv1, 0,0,0, sa2);
            }
        }

        /* ---- Phosphor glow (outer bloom + inner glow) ---- */
        {
            float gnx, gny, glen;
            float gw2 = 10.0f * sc;
            float ga2 = 0.06f + beat_glow * 0.03f;
            float gw1 = 5.0f * sc;
            float ga1 = 0.12f + beat_glow * 0.06f;
            /* Draw both glow layers for each edge */
            float glow_w[] = { gw2, gw1 };
            float glow_a[] = { ga2, ga1 };
            for (int gl = 0; gl < 2; gl++) {
                float gw = glow_w[gl], ga = glow_a[gl];
                gnx = -(lwing_y - nose_y); gny = (lwing_x - nose_x);
                glen = sqrtf(gnx*gnx+gny*gny); if(glen>0){gnx/=glen;gny/=glen;}
                push_quad_free(VPX(nose_x+gnx*gw), VPY(nose_y+gny*gw),
                               VPX(lwing_x+gnx*gw), VPY(lwing_y+gny*gw),
                               VPX(lwing_x-gnx*gw), VPY(lwing_y-gny*gw),
                               VPX(nose_x-gnx*gw), VPY(nose_y-gny*gw),
                               sr, sg, sb, ga);
                gnx = -(rwing_y - nose_y); gny = (rwing_x - nose_x);
                glen = sqrtf(gnx*gnx+gny*gny); if(glen>0){gnx/=glen;gny/=glen;}
                push_quad_free(VPX(nose_x+gnx*gw), VPY(nose_y+gny*gw),
                               VPX(rwing_x+gnx*gw), VPY(rwing_y+gny*gw),
                               VPX(rwing_x-gnx*gw), VPY(rwing_y-gny*gw),
                               VPX(nose_x-gnx*gw), VPY(nose_y-gny*gw),
                               sr, sg, sb, ga);
                gnx = -(rwing_y - lwing_y); gny = (rwing_x - lwing_x);
                glen = sqrtf(gnx*gnx+gny*gny); if(glen>0){gnx/=glen;gny/=glen;}
                push_quad_free(VPX(lwing_x+gnx*gw), VPY(lwing_y+gny*gw),
                               VPX(rwing_x+gnx*gw), VPY(rwing_y+gny*gw),
                               VPX(rwing_x-gnx*gw), VPY(rwing_y-gny*gw),
                               VPX(lwing_x-gnx*gw), VPY(lwing_y-gny*gw),
                               sr, sg, sb, ga);
                /* Glow vertex dots (fill gaps in glow too) */
                float gvr = gw * 0.6f;
                push_quad(VPX(nose_x-gvr), VPY(nose_y-gvr), VPX(nose_x+gvr), VPY(nose_y+gvr),
                          su0,sv0,su1,sv1, sr,sg,sb, ga);
                push_quad(VPX(lwing_x-gvr), VPY(lwing_y-gvr), VPX(lwing_x+gvr), VPY(lwing_y+gvr),
                          su0,sv0,su1,sv1, sr,sg,sb, ga);
                push_quad(VPX(rwing_x-gvr), VPY(rwing_y-gvr), VPX(rwing_x+gvr), VPY(rwing_y+gvr),
                          su0,sv0,su1,sv1, sr,sg,sb, ga);
            }
        }

        /* ---- Core wireframe edges (thin, bright) ---- */
        float nx, ny;
        nx = -(lwing_y - nose_y); ny = (lwing_x - nose_x);
        { float len = sqrtf(nx*nx+ny*ny); if(len>0){nx/=len;ny/=len;} }
        push_quad_free(VPX(nose_x+nx*lw), VPY(nose_y+ny*lw),
                       VPX(lwing_x+nx*lw), VPY(lwing_y+ny*lw),
                       VPX(lwing_x-nx*lw), VPY(lwing_y-ny*lw),
                       VPX(nose_x-nx*lw), VPY(nose_y-ny*lw),
                       sr, sg, sb, 1.0f);
        nx = -(rwing_y - nose_y); ny = (rwing_x - nose_x);
        { float len = sqrtf(nx*nx+ny*ny); if(len>0){nx/=len;ny/=len;} }
        push_quad_free(VPX(nose_x+nx*lw), VPY(nose_y+ny*lw),
                       VPX(rwing_x+nx*lw), VPY(rwing_y+ny*lw),
                       VPX(rwing_x-nx*lw), VPY(rwing_y-ny*lw),
                       VPX(nose_x-nx*lw), VPY(nose_y-ny*lw),
                       sr, sg, sb, 1.0f);
        nx = -(rwing_y - lwing_y); ny = (rwing_x - lwing_x);
        { float len = sqrtf(nx*nx+ny*ny); if(len>0){nx/=len;ny/=len;} }
        push_quad_free(VPX(lwing_x+nx*lw), VPY(lwing_y+ny*lw),
                       VPX(rwing_x+nx*lw), VPY(rwing_y+ny*lw),
                       VPX(rwing_x-nx*lw), VPY(rwing_y-ny*lw),
                       VPX(lwing_x-nx*lw), VPY(lwing_y-ny*lw),
                       sr, sg, sb, 1.0f);

        /* ---- Vertex dots (fill line joints for seamless triangle) ---- */
        {
            float vr = lw * 1.5f; /* dot radius slightly larger than line width */
            push_quad(VPX(nose_x-vr), VPY(nose_y-vr), VPX(nose_x+vr), VPY(nose_y+vr),
                      su0,sv0,su1,sv1, sr,sg,sb, 1.0f);
            push_quad(VPX(lwing_x-vr), VPY(lwing_y-vr), VPX(lwing_x+vr), VPY(lwing_y+vr),
                      su0,sv0,su1,sv1, sr,sg,sb, 1.0f);
            push_quad(VPX(rwing_x-vr), VPY(rwing_y-vr), VPX(rwing_x+vr), VPY(rwing_y+vr),
                      su0,sv0,su1,sv1, sr,sg,sb, 1.0f);
        }

        /* Thrust flame */
        float speed = sqrtf(s->vx*s->vx + s->vy*s->vy);
        if (speed > 30) {
            float flame_len = (8 + speed * 0.05f + viz_randf() * 8) * sc;
            float flame_w = 3.0f * sc;
            float ffx = rear_x - ca * flame_len;
            float ffy = rear_y - sa * flame_len;
            push_quad_free(VPX(rear_x+ny*flame_w), VPY(rear_y-nx*flame_w),
                           VPX(ffx), VPY(ffy),
                           VPX(ffx), VPY(ffy),
                           VPX(rear_x-ny*flame_w), VPY(rear_y+nx*flame_w),
                           1.0f, 0.6f + beat_glow * 0.4f, 0.1f, 0.8f);
        }
      }

        /* Draw UFO */
        if (viz_ufo.active) {
            float ux = viz_ufo.x, uy = viz_ufo.y;
            float ufo_w = 30.0f * sc, ufo_h = 10.0f * sc;
            float dome_h = 8.0f * sc;
            float ulw = 2.0f * sc;

            /* UFO shadow */
            float us_off = 4.0f * sc;
            float us_w = ulw + 5.0f * sc;
            push_quad(VPX(ux - ufo_w + us_off), VPY(uy - ufo_h * 0.3f + us_off),
                      VPX(ux + ufo_w + us_off), VPY(uy + ufo_h * 0.3f + us_off),
                      su0, sv0, su1, sv1, 0, 0, 0, 0.3f);

            /* UFO body — flat ellipse (top + bottom halves) */
            float ufo_r = 0.6f, ufo_g = 0.2f, ufo_b = 1.0f; /* purple */
            ufo_r += beat_glow * 0.3f; ufo_g += beat_glow * 0.2f; ufo_b += beat_glow * 0.2f;
            if (ufo_r > 1.0f) ufo_r = 1.0f; if (ufo_b > 1.0f) ufo_b = 1.0f;

            /* Outer glow */
            push_quad(VPX(ux - ufo_w - 6*sc), VPY(uy - ufo_h - 4*sc),
                      VPX(ux + ufo_w + 6*sc), VPY(uy + ufo_h + 4*sc),
                      su0, sv0, su1, sv1, ufo_r, ufo_g, ufo_b, 0.06f);
            /* Body */
            push_quad(VPX(ux - ufo_w), VPY(uy - ufo_h * 0.3f),
                      VPX(ux + ufo_w), VPY(uy + ufo_h * 0.3f),
                      su0, sv0, su1, sv1, ufo_r * 0.5f, ufo_g * 0.3f, ufo_b * 0.5f, 0.6f);
            /* Top edge */
            push_quad(VPX(ux - ufo_w), VPY(uy - ufo_h * 0.3f - ulw),
                      VPX(ux + ufo_w), VPY(uy - ufo_h * 0.3f + ulw),
                      su0, sv0, su1, sv1, ufo_r, ufo_g, ufo_b, 1.0f);
            /* Bottom edge */
            push_quad(VPX(ux - ufo_w), VPY(uy + ufo_h * 0.3f - ulw),
                      VPX(ux + ufo_w), VPY(uy + ufo_h * 0.3f + ulw),
                      su0, sv0, su1, sv1, ufo_r, ufo_g, ufo_b, 1.0f);
            /* Dome */
            push_quad(VPX(ux - ufo_w * 0.4f), VPY(uy - ufo_h * 0.3f - dome_h),
                      VPX(ux + ufo_w * 0.4f), VPY(uy - ufo_h * 0.3f),
                      su0, sv0, su1, sv1, ufo_r * 0.7f, ufo_g * 0.5f, ufo_b * 0.7f, 0.5f);
            /* Dome edge */
            push_quad(VPX(ux - ufo_w * 0.4f), VPY(uy - ufo_h * 0.3f - dome_h - ulw),
                      VPX(ux + ufo_w * 0.4f), VPY(uy - ufo_h * 0.3f - dome_h + ulw),
                      su0, sv0, su1, sv1, ufo_r, ufo_g, ufo_b, 0.8f);
            /* Left/right edges (vertical lines at saucer edges) */
            push_quad(VPX(ux - ufo_w - ulw), VPY(uy - ufo_h * 0.3f),
                      VPX(ux - ufo_w + ulw), VPY(uy + ufo_h * 0.3f),
                      su0, sv0, su1, sv1, ufo_r, ufo_g, ufo_b, 0.8f);
            push_quad(VPX(ux + ufo_w - ulw), VPY(uy - ufo_h * 0.3f),
                      VPX(ux + ufo_w + ulw), VPY(uy + ufo_h * 0.3f),
                      su0, sv0, su1, sv1, ufo_r, ufo_g, ufo_b, 0.8f);

            /* UFO laser beam */
            if (viz_ufo.firing) {
                float bw = viz_ufo.laser_width;
                float ba = viz_ufo.laser_alpha;
                /* Core beam — bright inner */
                push_quad(VPX(ux - bw * 0.3f), VPY(uy + ufo_h * 0.3f),
                          VPX(ux + bw * 0.3f), VPY((float)vp_h),
                          su0, sv0, su1, sv1, 0.8f, 0.3f, 1.0f, ba * 0.8f);
                /* Outer glow */
                push_quad(VPX(ux - bw * 0.6f), VPY(uy + ufo_h * 0.3f),
                          VPX(ux + bw * 0.6f), VPY((float)vp_h),
                          su0, sv0, su1, sv1, 0.6f, 0.1f, 0.8f, ba * 0.3f);
                /* Wide ambient glow */
                push_quad(VPX(ux - bw), VPY(uy + ufo_h * 0.3f),
                          VPX(ux + bw), VPY((float)vp_h),
                          su0, sv0, su1, sv1, 0.4f, 0.1f, 0.6f, ba * 0.1f);
            }

            /* HP indicator: small dots */
            for (int h = 0; h < viz_ufo.hp; h++) {
                float hx = ux - (viz_ufo.hp - 1) * 4.0f * sc * 0.5f + h * 4.0f * sc;
                float hy = uy + ufo_h * 0.3f + 6.0f * sc;
                float hs = 2.0f * sc;
                push_quad(VPX(hx - hs), VPY(hy - hs), VPX(hx + hs), VPY(hy + hs),
                          su0, sv0, su1, sv1, 1.0f, 0.3f, 0.3f, 0.8f);
            }
        }

        /* Lasers */
        for (int i = 0; i < VIZ_MAX_LASERS; i++) {
            viz_laser_t *l = &viz_lasers[i];
            if (!l->active) continue;
            float llen = 16.0f * sc;
            float lnx = l->vx, lny = l->vy;
            float lspd = sqrtf(lnx*lnx + lny*lny);
            if (lspd > 0) { lnx /= lspd; lny /= lspd; }
            float ttx = l->x - lnx * llen, tty = l->y - lny * llen;
            float pnx = -lny, pny = lnx;
            float lr = l->lr, lg = l->lg, lb = l->lb;
            if (lspd > 1000) { lr = lr*0.7f + 0.3f; lg *= 0.5f; lb = lb*0.5f + 0.5f; }
            else if (lspd < 500) { lr *= 0.6f; lg = lg*0.6f + 0.4f; lb *= 0.6f; }
            lr += beat_glow * 0.3f; lg += beat_glow * 0.2f; lb += beat_glow * 0.3f;
            if (lr > 1.0f) lr = 1.0f; if (lg > 1.0f) lg = 1.0f; if (lb > 1.0f) lb = 1.0f;
            float thick = ((lspd > 1000) ? 2.5f : 1.2f) * sc;
            push_quad_free(VPX(l->x+pnx*thick), VPY(l->y+pny*thick),
                           VPX(ttx+pnx*thick*0.3f), VPY(tty+pny*thick*0.3f),
                           VPX(ttx-pnx*thick*0.3f), VPY(tty-pny*thick*0.3f),
                           VPX(l->x-pnx*thick), VPY(l->y-pny*thick),
                           lr, lg, lb, l->life);
        }
    }

    #undef VPX
    #undef VPY
}
