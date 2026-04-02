/*
 * gl_term_render.h — Cross-platform GL terminal renderer
 * =======================================================
 *
 * Shared header for Windows DLL and Linux native terminal.
 * Defines shared memory layout, render API, and constants.
 */

#ifndef GL_TERM_RENDER_H
#define GL_TERM_RENDER_H

#include <stdint.h>

/* ---- Terminal constants ---- */

#define TERM_ROWS        60
#define TERM_COLS        132
#define TERM_STRIDE      0x84   /* MMANSI row stride in bytes */
#define MMANSI_TEXT_OFF  0x62
#define MMANSI_ATTR_OFF  0x1F52

/* ---- Shared memory layout ---- */

#define SHM_NAME         "/mmansi_buf"
#define SHM_CMD_NAME     "/mmansi_cmd"
#define SHM_MAGIC        0x4D4D414Eu  /* "MMAN" */

typedef struct {
    uint32_t magic;                    /* SHM_MAGIC */
    uint32_t seq;                      /* incremented each write */
    uint32_t rows;                     /* 60 */
    uint32_t cols;                     /* 132 */
    uint8_t  text[TERM_ROWS][TERM_COLS];
    uint8_t  attr[TERM_ROWS][TERM_COLS];
    uint8_t  cursor_row;
    uint8_t  cursor_col;
    uint8_t  cursor_visible;
    uint8_t  _pad;
} mmansi_shm_t;

/* Command channel: Linux → DLL */
typedef struct {
    uint32_t         magic;
    volatile uint32_t write_seq;       /* writer increments */
    volatile uint32_t read_seq;        /* reader sets = write_seq when consumed */
    uint8_t          mode;             /* 0 = split, 1 = raw */
    uint8_t          raw_key;          /* single keystroke in raw mode */
    uint8_t          raw_key_pending;
    uint8_t          _pad;
    char             cmd[512];         /* null-terminated command */
} mmansi_cmd_t;

/* ---- Color / theme types ---- */

typedef struct { float r, g, b; } rgb_t;

#define GLTR_NUM_THEMES  5

/* ---- Render API (platform-agnostic, needs current GL context) ---- */

void        gltr_init(void);
void        gltr_shutdown(void);
void        gltr_set_viewport(int w, int h);
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
