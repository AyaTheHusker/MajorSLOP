/* viz_fx.c — Terminal Visualizations: Rolling Boulders, Matrix Rain, Asteroids
 * Physics particles interact with terminal text. Beat-reactive via mr_beat_snap.
 * Included into vk_terminal.c after mudradio includes.
 */

/* ---- Configuration ---- */
#define VIZ_MAX_PARTICLES   256
#define VIZ_MAX_FRAGMENTS   1024
#define VIZ_MAX_LASERS      32
#define VIZ_MAX_RAIN        128
#define VIZ_GRAVITY         600.0f
#define VIZ_SPRING_K        10.0f
#define VIZ_SPRING_DAMP     0.88f
#define VIZ_SHATTER_VEL     150.0f
#define VIZ_CELL_RESPAWN    4.0f  /* seconds before shattered cell reappears */

/* ---- Types ---- */
typedef enum {
    VIZ_NONE = 0,
    VIZ_BOULDERS,
    VIZ_MATRIX,
    VIZ_ASTEROIDS,
    VIZ_MODE_COUNT
} viz_mode_t;

static viz_mode_t viz_mode = VIZ_NONE;
static float viz_time = 0.0f;

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

/* Asteroids ship */
typedef struct {
    float x, y, vx, vy;
    float angle;         /* heading in radians */
    float target_angle;  /* smooth steering target */
    float thrust;
    int   firing;        /* cooldown frames */
    float wander_t;      /* time until next direction change */
    float tx, ty;        /* current target position */
} viz_ship_t;

static viz_ship_t viz_ship;

/* Lasers */
typedef struct {
    float x, y, vx, vy;
    float life;
    int   active;
} viz_laser_t;

static viz_laser_t viz_lasers[VIZ_MAX_LASERS];

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
    memset(&viz_ship, 0, sizeof(viz_ship));
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

/* ---- Spawn a boulder ---- */
static void viz_spawn_boulder(float cw, float ch, float vp_w, float bass) {
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
    p->ang_vel = viz_randf_range(-4.0f, 4.0f);
    p->life = 15.0f; p->max_life = 15.0f;
    p->r = viz_randf_range(0.35f, 0.55f);
    p->g = viz_randf_range(0.25f, 0.40f);
    p->b = viz_randf_range(0.10f, 0.20f);
    p->a = 1.0f;
    p->glyph = 0;
}

/* ---- Spawn debris from boulder impact ---- */
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

/* ==== ROLLING BOULDERS UPDATE ==== */
static float viz_boulder_timer = 0;

static void viz_update_boulders(float dt, float cw, float ch, float x_off,
                                  float top_pad_px, float vp_w, float vp_h,
                                  float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    /* Beat-reactive spawning */
    EnterCriticalSection(&mr_beat_lock);
    float bass = mr_beat_snap.bass_energy;
    int onset = mr_beat_snap.onset_detected;
    LeaveCriticalSection(&mr_beat_lock);

    viz_boulder_timer -= dt;
    if (onset && viz_boulder_timer <= 0) {
        viz_spawn_boulder(cw, ch, vp_w, bass);
        viz_boulder_timer = 0.3f;
    }
    /* Also spawn periodically even without beat */
    static float passive_timer = 0;
    passive_timer -= dt;
    if (passive_timer <= 0) {
        viz_spawn_boulder(cw, ch, vp_w, 0.3f);
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
                        /* Boulder loses some speed on impact */
                        p->vx *= 0.95f;
                        p->vy *= 0.85f;
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

/* ==== MATRIX RAIN UPDATE ==== */
static void viz_update_matrix(float dt, float cw, float ch, float x_off,
                                float top_pad_px, float vp_w, float vp_h,
                                float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    EnterCriticalSection(&mr_beat_lock);
    float bass = mr_beat_snap.bass_energy;
    int onset = mr_beat_snap.onset_detected;
    LeaveCriticalSection(&mr_beat_lock);

    /* Spawn new rain columns */
    static float rain_timer = 0;
    rain_timer -= dt;
    float spawn_rate = onset ? 0.02f : 0.15f;
    if (rain_timer <= 0) {
        for (int i = 0; i < VIZ_MAX_RAIN; i++) {
            if (!viz_rain[i].active) {
                viz_rain[i].active = 1;
                viz_rain[i].col = rand() % TERM_COLS;
                viz_rain[i].y = -ch * 3;
                viz_rain[i].speed = viz_randf_range(200, 600) * (0.8f + bass * 0.5f);
                viz_rain[i].len = (int)viz_randf_range(5, 25);
                for (int j = 0; j < 80; j++)
                    viz_rain[i].chars[j] = 33 + (rand() % 93); /* printable ASCII */
                break;
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

/* ==== ASTEROIDS SHIP UPDATE ==== */
static void viz_update_asteroids(float dt, float cw, float ch, float x_off,
                                   float top_pad_px, float vp_w, float vp_h,
                                   float tex_cw, float tex_ch, float hp_u, float hp_v)
{
    EnterCriticalSection(&mr_beat_lock);
    float bass = mr_beat_snap.bass_energy;
    int onset = mr_beat_snap.onset_detected;
    float treble = mr_beat_snap.treble_energy;
    LeaveCriticalSection(&mr_beat_lock);

    viz_ship_t *s = &viz_ship;

    /* Initialize ship if needed */
    if (s->x == 0 && s->y == 0) {
        s->x = vp_w * 0.5f;
        s->y = vp_h * 0.5f;
        s->angle = -1.57f; /* pointing up */
        s->wander_t = 0;
    }

    /* Smooth wandering AI — pick targets, steer toward them */
    s->wander_t -= dt;
    if (s->wander_t <= 0) {
        /* Pick a new target: find a non-empty cell to fly toward */
        int attempts = 20;
        while (attempts-- > 0) {
            int tr = rand() % TERM_ROWS;
            int tc = rand() % TERM_COLS;
            unsigned char byte = ansi_term.grid[tr][tc].ch;
            if (byte != 0 && byte != 32 && !viz_cells[tr][tc].shattered) {
                s->tx = x_off + tc * cw + cw * 0.5f;
                s->ty = top_pad_px + tr * ch + ch * 0.5f;
                break;
            }
        }
        s->wander_t = viz_randf_range(1.5f, 4.0f);
    }

    /* Steer toward target */
    float dx = s->tx - s->x, dy = s->ty - s->y;
    float target_angle = atan2f(dy, dx);
    /* Smooth angle interpolation */
    float angle_diff = target_angle - s->angle;
    while (angle_diff > 3.14159f) angle_diff -= 6.28318f;
    while (angle_diff < -3.14159f) angle_diff += 6.28318f;
    s->angle += angle_diff * 2.5f * dt;

    /* Thrust */
    float thrust_amount = 250.0f + bass * 200.0f;
    s->vx += cosf(s->angle) * thrust_amount * dt;
    s->vy += sinf(s->angle) * thrust_amount * dt;

    /* Damping */
    s->vx *= (1.0f - 1.5f * dt);
    s->vy *= (1.0f - 1.5f * dt);

    /* Move */
    s->x += s->vx * dt;
    s->y += s->vy * dt;

    /* Wrap around screen */
    if (s->x < 0) s->x += vp_w;
    if (s->x > vp_w) s->x -= vp_w;
    if (s->y < 0) s->y += vp_h;
    if (s->y > vp_h) s->y -= vp_h;

    /* Fire on beat (or high treble) */
    if (s->firing > 0) s->firing--;
    if ((onset || treble > 0.3f) && s->firing <= 0) {
        viz_laser_t *l = viz_alloc_laser();
        if (l) {
            l->active = 1;
            l->x = s->x + cosf(s->angle) * 12.0f;
            l->y = s->y + sinf(s->angle) * 12.0f;
            l->vx = cosf(s->angle) * 800.0f + s->vx * 0.5f;
            l->vy = sinf(s->angle) * 800.0f + s->vy * 0.5f;
            l->life = 1.5f;
        }
        s->firing = 4; /* cooldown */
    }

    /* Update lasers */
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

        /* Laser-text collision */
        int lr = (int)((l->y - top_pad_px) / ch);
        int lc = (int)((l->x - x_off) / cw);
        if (lr >= 0 && lr < TERM_ROWS && lc >= 0 && lc < TERM_COLS) {
            unsigned char byte = ansi_term.grid[lr][lc].ch;
            if (byte != 0 && byte != 32 && !viz_cells[lr][lc].shattered) {
                viz_shatter_cell(lr, lc, l->vx * 0.2f, l->vy * 0.2f,
                                cw, ch, x_off, top_pad_px,
                                tex_cw, tex_ch, hp_u, hp_v);
                /* Red/orange explosion sparks */
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
        case VIZ_BOULDERS:
            viz_update_boulders(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                                tex_cw, tex_ch, hp_u, hp_v);
            break;
        case VIZ_MATRIX:
            viz_update_matrix(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
                              tex_cw, tex_ch, hp_u, hp_v);
            break;
        case VIZ_ASTEROIDS:
            viz_update_asteroids(dt, cw, ch, x_off, top_pad_px, vp_w, vp_h,
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

    /* Solid fill UVs (glyph 219) */
    float su0 = (219 % 16) * tex_cw + hp_u;
    float sv0 = (219 / 16) * tex_ch + hp_v;
    float su1 = su0 + tex_cw - 2*hp_u;
    float sv1 = sv0 + tex_ch - 2*hp_v;

    #define VPX(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define VPY(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)

    /* Draw boulders */
    for (int i = 0; i < VIZ_MAX_PARTICLES; i++) {
        viz_part_t *p = &viz_parts[i];
        if (!p->active) continue;

        if (p->type == 0) {
            /* Boulder: cluster of solid quads in rough circle with spikes */
            float rad = p->radius;
            int segs = 8;
            for (int s = 0; s < segs; s++) {
                float a = p->angle + s * (6.28318f / segs);
                /* Irregular radius for spiky look */
                float spike = (s % 2 == 0) ? 1.0f : 1.4f;
                float sr = rad * spike;
                float sx = p->x + cosf(a) * sr * 0.6f;
                float sy = p->y + sinf(a) * sr * 0.6f;
                float qr = rad * 0.4f;
                push_quad(VPX(sx - qr), VPY(sy - qr), VPX(sx + qr), VPY(sy + qr),
                          su0, sv0, su1, sv1,
                          p->r * (0.8f + 0.2f * spike), p->g, p->b, p->a);
            }
            /* Core */
            push_quad(VPX(p->x - rad*0.5f), VPY(p->y - rad*0.5f),
                      VPX(p->x + rad*0.5f), VPY(p->y + rad*0.5f),
                      su0, sv0, su1, sv1,
                      p->r * 0.6f, p->g * 0.6f, p->b * 0.6f, p->a);
        } else {
            /* Sparks/debris: tiny bright quads */
            float sz = p->radius;
            push_quad(VPX(p->x - sz), VPY(p->y - sz), VPX(p->x + sz), VPY(p->y + sz),
                      su0, sv0, su1, sv1, p->r, p->g, p->b, p->a);
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
                /* Head is brightest, trail fades */
                float bright = (j == 0) ? 1.0f : 0.7f * (1.0f - (float)j / (float)rn->len);
                float gr = 0.0f, gg = bright, gb = bright * 0.2f;
                if (j == 0) { gr = 0.6f; gg = 1.0f; gb = 0.6f; } /* white head */
                push_quad(VPX(cx), VPY(cy), VPX(cx + cw), VPY(cy + ch),
                          gu0, gv0, gu1, gv1, gr, gg, gb, bright);
            }
        }
    }

    /* Draw asteroids ship (vector style — lines as thin quads) */
    if (viz_mode == VIZ_ASTEROIDS) {
        viz_ship_t *s = &viz_ship;
        float ca = cosf(s->angle), sa = sinf(s->angle);
        /* Ship points: nose, left wing, right wing, rear */
        float nose_x = s->x + ca * 14, nose_y = s->y + sa * 14;
        float lwing_x = s->x + cosf(s->angle + 2.5f) * 10;
        float lwing_y = s->y + sinf(s->angle + 2.5f) * 10;
        float rwing_x = s->x + cosf(s->angle - 2.5f) * 10;
        float rwing_y = s->y + sinf(s->angle - 2.5f) * 10;
        float rear_x = s->x - ca * 6, rear_y = s->y - sa * 6;
        float lw = 1.5f; /* line width */

        /* Ship body — 3 line segments as thin quads */
        /* Nose → Left wing */
        float nx, ny;
        nx = -(lwing_y - nose_y); ny = (lwing_x - nose_x);
        { float len = sqrtf(nx*nx+ny*ny); if(len>0){nx/=len;ny/=len;} }
        push_quad_free(VPX(nose_x+nx*lw), VPY(nose_y+ny*lw),
                       VPX(lwing_x+nx*lw), VPY(lwing_y+ny*lw),
                       VPX(lwing_x-nx*lw), VPY(lwing_y-ny*lw),
                       VPX(nose_x-nx*lw), VPY(nose_y-ny*lw),
                       0.0f, 1.0f, 1.0f, 1.0f);
        /* Nose → Right wing */
        nx = -(rwing_y - nose_y); ny = (rwing_x - nose_x);
        { float len = sqrtf(nx*nx+ny*ny); if(len>0){nx/=len;ny/=len;} }
        push_quad_free(VPX(nose_x+nx*lw), VPY(nose_y+ny*lw),
                       VPX(rwing_x+nx*lw), VPY(rwing_y+ny*lw),
                       VPX(rwing_x-nx*lw), VPY(rwing_y-ny*lw),
                       VPX(nose_x-nx*lw), VPY(nose_y-ny*lw),
                       0.0f, 1.0f, 1.0f, 1.0f);
        /* Left wing → Right wing (rear) */
        nx = -(rwing_y - lwing_y); ny = (rwing_x - lwing_x);
        { float len = sqrtf(nx*nx+ny*ny); if(len>0){nx/=len;ny/=len;} }
        push_quad_free(VPX(lwing_x+nx*lw), VPY(lwing_y+ny*lw),
                       VPX(rwing_x+nx*lw), VPY(rwing_y+ny*lw),
                       VPX(rwing_x-nx*lw), VPY(rwing_y-ny*lw),
                       VPX(lwing_x-nx*lw), VPY(lwing_y-ny*lw),
                       0.0f, 1.0f, 1.0f, 1.0f);

        /* Thrust flame when moving fast */
        float speed = sqrtf(s->vx*s->vx + s->vy*s->vy);
        if (speed > 30) {
            float flame_len = 5 + speed * 0.03f + viz_randf() * 5;
            float fx = rear_x - ca * flame_len;
            float fy = rear_y - sa * flame_len;
            push_quad_free(VPX(rear_x+ny*2), VPY(rear_y-nx*2),
                           VPX(fx), VPY(fy),
                           VPX(fx), VPY(fy),
                           VPX(rear_x-ny*2), VPY(rear_y+nx*2),
                           1.0f, 0.6f, 0.1f, 0.8f);
        }

        /* Lasers */
        for (int i = 0; i < VIZ_MAX_LASERS; i++) {
            viz_laser_t *l = &viz_lasers[i];
            if (!l->active) continue;
            float len = 12.0f;
            float lnx = l->vx, lny = l->vy;
            float lspd = sqrtf(lnx*lnx + lny*lny);
            if (lspd > 0) { lnx /= lspd; lny /= lspd; }
            float tx = l->x - lnx * len, ty = l->y - lny * len;
            float pnx = -lny * 1.0f, pny = lnx * 1.0f;
            push_quad_free(VPX(l->x+pnx), VPY(l->y+pny),
                           VPX(tx+pnx*0.3f), VPY(ty+pny*0.3f),
                           VPX(tx-pnx*0.3f), VPY(ty-pny*0.3f),
                           VPX(l->x-pnx), VPY(l->y-pny),
                           1.0f, 0.3f, 0.3f, l->life);
        }
    }

    #undef VPX
    #undef VPY
}
