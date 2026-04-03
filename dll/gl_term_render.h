/*
 * gl_term_render.h — Cross-platform GL terminal renderer
 * =======================================================
 *
 * Shared between Windows DLL plugin and Linux native app.
 * Uses embedded CP437 8x16 bitmap font — no platform font deps.
 */

#ifndef GL_TERM_RENDER_H
#define GL_TERM_RENDER_H

#include <stdint.h>

/* ---- Terminal constants ---- */

#define TERM_ROWS        60
#define TERM_COLS        132
#define TERM_STRIDE      0x84
#define MMANSI_TEXT_OFF  0x62
#define MMANSI_ATTR_OFF  0x1F52

/* Font atlas: 16x16 grid of 8x16 glyphs → 128x256 texture */
#define FONT_GRID        16
#define GLYPH_W          8
#define GLYPH_H          16
#define ATLAS_W          (FONT_GRID * GLYPH_W)   /* 128 */
#define ATLAS_H          (FONT_GRID * GLYPH_H)   /* 256 */

#define INPUT_BAR_H      20   /* pixels for input bar */

/* ---- Shared memory layout ---- */

#define SHM_PATH         "/tmp/mmansi_buf"
#define SHM_CMD_PATH     "/tmp/mmansi_cmd"
#define SHM_MAGIC        0x4D4D414Eu

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t rows;
    uint32_t cols;
    uint8_t  text[TERM_ROWS][TERM_COLS];
    uint8_t  attr[TERM_ROWS][TERM_COLS];
    uint8_t  cursor_row;
    uint8_t  cursor_col;
    uint8_t  cursor_visible;
    uint8_t  _pad;
} mmansi_shm_t;

typedef struct {
    uint32_t         magic;
    volatile uint32_t write_seq;
    volatile uint32_t read_seq;
    uint8_t          mode;          /* 0 = split, 1 = raw */
    uint8_t          raw_key;
    uint8_t          raw_key_pending;
    uint8_t          _pad;
    char             cmd[512];
} mmansi_cmd_t;

/* ---- Color / theme ---- */

typedef struct { float r, g, b; } rgb_t;

#define GLTR_NUM_THEMES  5

/* ---- Render API ---- */

void        gltr_init(void);
void        gltr_shutdown(void);
void        gltr_set_viewport(int w, int h, int input_bar_h);
void        gltr_render(const uint8_t text[TERM_ROWS][TERM_COLS],
                        const uint8_t attr[TERM_ROWS][TERM_COLS]);
void        gltr_render_input_bar(const char *text, int cursor_pos,
                                  int bar_y, int bar_h, int frame);
void        gltr_set_theme(int idx);
int         gltr_get_theme(void);
void        gltr_set_color(int idx, int r, int g, int b);
const char *gltr_theme_name(int idx);
rgb_t      *gltr_get_palette(void);

#endif /* GL_TERM_RENDER_H */
