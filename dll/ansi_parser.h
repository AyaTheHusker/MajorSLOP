/*
 * ansi_parser.h -- Byte-by-byte ANSI/VT100 terminal state machine
 * ================================================================
 *
 * Based on Paul Williams' VT500 state machine (vt100.net/emu/dec_ansi_parser).
 * Reference impl: Joshua Haberman's vtparse (public domain).
 *
 * This is a SINGLE HEADER, self-contained parser + grid buffer.
 * Process arbitrary chunks of bytes -- state persists between calls.
 *
 * Usage:
 *   #define ANSI_PARSER_IMPLEMENTATION   (in exactly ONE .c file)
 *   #include "ansi_parser.h"
 *
 * Public domain / zero-clause BSD -- do whatever you want.
 */

#ifndef ANSI_PARSER_H
#define ANSI_PARSER_H

#include <stdint.h>
#include <string.h>

/* ---- Configuration ---- */

#ifndef AP_ROWS
#define AP_ROWS   60
#endif
#ifndef AP_COLS
#define AP_COLS   132
#endif
#define AP_MAX_PARAMS    16
#define AP_MAX_OSC       256

/* ---- State machine states (Paul Williams diagram) ---- */

typedef enum {
    AP_GROUND = 0,
    AP_ESCAPE,
    AP_ESCAPE_INTERMEDIATE,
    AP_CSI_ENTRY,
    AP_CSI_PARAM,
    AP_CSI_INTERMEDIATE,
    AP_CSI_IGNORE,
    AP_OSC_STRING,
    AP_NUM_STATES
} ap_state_t;

/* ---- SGR attribute packed into one byte ----
 *
 * For the basic 8-color case we pack:
 *   bits 0-2: foreground color (0-7)
 *   bit    3: bold/bright
 *   bits 4-6: background color (0-7)
 *   bit    7: blink (unused, available)
 *
 * For 256-color / truecolor, use the extended attr struct.
 */

typedef struct {
    uint8_t  fg;        /* foreground index: 0-7 normal, +8 = bright */
    uint8_t  bg;        /* background index: 0-7 normal, +8 = bright */
    uint8_t  bold;      /* SGR 1 */
    uint8_t  underline; /* SGR 4 */
    uint8_t  reverse;   /* SGR 7 */
    uint8_t  fg256;     /* if set, fg is 256-color index */
    uint8_t  bg256;     /* if set, bg is 256-color index */
} ap_attr_t;

/* ---- Cell ---- */

typedef struct {
    uint8_t   ch;       /* character (CP437 / ASCII) */
    ap_attr_t attr;     /* color attributes */
} ap_cell_t;

/* ---- Scroll callback (optional) ---- */
/*  Called when the screen scrolls up by one line.
 *  `row` points to the line being scrolled OFF the top.
 *  Useful for scrollback buffer capture. */
typedef void (*ap_scroll_cb_t)(const ap_cell_t *row, int cols, void *user);

/* ---- Terminal state ---- */

typedef struct {
    /* Grid buffer */
    ap_cell_t grid[AP_ROWS][AP_COLS];
    int       rows, cols;

    /* Cursor */
    int       cx, cy;           /* column, row (0-based) */

    /* Current drawing attribute */
    ap_attr_t attr;

    /* Parser state machine */
    ap_state_t state;
    int        params[AP_MAX_PARAMS];
    int        nparams;
    uint8_t    intermediate;    /* collected intermediate char (e.g. '?' '>' ) */

    /* OSC accumulator */
    char       osc_buf[AP_MAX_OSC];
    int        osc_len;

    /* SGR sub-state for 38;5;N and 48;5;N parsing -- handled inline */

    /* Scroll region (for future CSI r support) */
    int        scroll_top, scroll_bot;

    /* Callbacks */
    ap_scroll_cb_t scroll_cb;
    void          *user;
} ap_term_t;

/* ---- API ---- */

void ap_init(ap_term_t *t, int rows, int cols);
void ap_feed(ap_term_t *t, const uint8_t *data, int len);
void ap_feed_byte(ap_term_t *t, uint8_t ch);
void ap_clear(ap_term_t *t);
void ap_set_scroll_cb(ap_term_t *t, ap_scroll_cb_t cb, void *user);

/* Pack attr into a single byte (MMANSI-compatible: low nibble=fg, high nibble=bg) */
static inline uint8_t ap_attr_pack(const ap_attr_t *a) {
    uint8_t fg = (a->fg & 0x07) | (a->bold ? 0x08 : 0);
    uint8_t bg = (a->bg & 0x07);
    return (bg << 4) | fg;
}

#endif /* ANSI_PARSER_H */


/* ==================================================================
 *                        IMPLEMENTATION
 * ================================================================== */

#ifdef ANSI_PARSER_IMPLEMENTATION

/* ---- Helpers ---- */

static const ap_attr_t ap_default_attr = { 7, 0, 0, 0, 0, 0, 0 };

static void ap_scroll_up(ap_term_t *t)
{
    /* Notify callback before losing top line */
    if (t->scroll_cb)
        t->scroll_cb(t->grid[t->scroll_top], t->cols, t->user);

    /* Move lines up within scroll region */
    for (int r = t->scroll_top; r < t->scroll_bot; r++)
        memcpy(t->grid[r], t->grid[r + 1], sizeof(ap_cell_t) * t->cols);

    /* Clear bottom line */
    for (int c = 0; c < t->cols; c++) {
        t->grid[t->scroll_bot][c].ch = ' ';
        t->grid[t->scroll_bot][c].attr = ap_default_attr;
    }
}

static void ap_newline(ap_term_t *t)
{
    t->cy++;
    if (t->cy > t->scroll_bot) {
        t->cy = t->scroll_bot;
        ap_scroll_up(t);
    }
}

static void ap_put_char(ap_term_t *t, uint8_t ch)
{
    if (t->cx >= t->cols) {
        t->cx = 0;
        ap_newline(t);
    }
    if (t->cy >= 0 && t->cy < t->rows && t->cx >= 0 && t->cx < t->cols) {
        t->grid[t->cy][t->cx].ch = ch;
        t->grid[t->cy][t->cx].attr = t->attr;
    }
    t->cx++;
}

static void ap_erase_region(ap_term_t *t, int r0, int c0, int r1, int c1)
{
    for (int r = r0; r <= r1 && r < t->rows; r++) {
        int cs = (r == r0) ? c0 : 0;
        int ce = (r == r1) ? c1 : t->cols - 1;
        for (int c = cs; c <= ce && c < t->cols; c++) {
            t->grid[r][c].ch = ' ';
            t->grid[r][c].attr = ap_default_attr;
        }
    }
}

static int ap_param(ap_term_t *t, int idx, int def)
{
    if (idx < t->nparams && t->params[idx] > 0) return t->params[idx];
    return def;
}

/* ---- SGR (Select Graphic Rendition) ---- */

static void ap_handle_sgr(ap_term_t *t)
{
    if (t->nparams == 0) {
        /* ESC[m == ESC[0m */
        t->attr = ap_default_attr;
        return;
    }

    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];

        if (p == 0) {
            t->attr = ap_default_attr;
        } else if (p == 1) {
            t->attr.bold = 1;
        } else if (p == 4) {
            t->attr.underline = 1;
        } else if (p == 7) {
            t->attr.reverse = 1;
        } else if (p == 22) {
            t->attr.bold = 0;
        } else if (p == 24) {
            t->attr.underline = 0;
        } else if (p == 27) {
            t->attr.reverse = 0;
        } else if (p >= 30 && p <= 37) {
            t->attr.fg = (uint8_t)(p - 30);
            t->attr.fg256 = 0;
        } else if (p == 38) {
            /* Extended foreground: 38;5;N (256-color) or 38;2;R;G;B (truecolor) */
            if (i + 1 < t->nparams && t->params[i + 1] == 5 && i + 2 < t->nparams) {
                t->attr.fg = (uint8_t)t->params[i + 2];
                t->attr.fg256 = 1;
                i += 2;
            } else if (i + 1 < t->nparams && t->params[i + 1] == 2 && i + 4 < t->nparams) {
                /* Truecolor: store as 256-color approximation for now */
                /* TODO: full RGB support if needed */
                int r = t->params[i + 2], g = t->params[i + 3], b = t->params[i + 4];
                /* Approximate to 6x6x6 cube: 16 + 36*r/51 + 6*g/51 + b/51 */
                t->attr.fg = (uint8_t)(16 + 36 * (r / 51) + 6 * (g / 51) + (b / 51));
                t->attr.fg256 = 1;
                i += 4;
            }
        } else if (p == 39) {
            t->attr.fg = 7;  /* default fg */
            t->attr.fg256 = 0;
        } else if (p >= 40 && p <= 47) {
            t->attr.bg = (uint8_t)(p - 40);
            t->attr.bg256 = 0;
        } else if (p == 48) {
            /* Extended background: 48;5;N */
            if (i + 1 < t->nparams && t->params[i + 1] == 5 && i + 2 < t->nparams) {
                t->attr.bg = (uint8_t)t->params[i + 2];
                t->attr.bg256 = 1;
                i += 2;
            } else if (i + 1 < t->nparams && t->params[i + 1] == 2 && i + 4 < t->nparams) {
                int r = t->params[i + 2], g = t->params[i + 3], b = t->params[i + 4];
                t->attr.bg = (uint8_t)(16 + 36 * (r / 51) + 6 * (g / 51) + (b / 51));
                t->attr.bg256 = 1;
                i += 4;
            }
        } else if (p == 49) {
            t->attr.bg = 0;  /* default bg */
            t->attr.bg256 = 0;
        } else if (p >= 90 && p <= 97) {
            /* Bright foreground (aixterm) */
            t->attr.fg = (uint8_t)(p - 90 + 8);
            t->attr.fg256 = 0;
        } else if (p >= 100 && p <= 107) {
            /* Bright background (aixterm) */
            t->attr.bg = (uint8_t)(p - 100 + 8);
            t->attr.bg256 = 0;
        }
    }
}

/* ---- CSI dispatch ---- */

static void ap_csi_dispatch(ap_term_t *t, uint8_t final)
{
    int n, m;

    switch (final) {
    case 'm':  /* SGR */
        ap_handle_sgr(t);
        break;

    case 'H': case 'f':  /* CUP - Cursor Position */
        n = ap_param(t, 0, 1);
        m = ap_param(t, 1, 1);
        t->cy = (n - 1 < t->rows) ? n - 1 : t->rows - 1;
        t->cx = (m - 1 < t->cols) ? m - 1 : t->cols - 1;
        if (t->cy < 0) t->cy = 0;
        if (t->cx < 0) t->cx = 0;
        break;

    case 'A':  /* CUU - Cursor Up */
        n = ap_param(t, 0, 1);
        t->cy -= n;
        if (t->cy < 0) t->cy = 0;
        break;

    case 'B':  /* CUD - Cursor Down */
        n = ap_param(t, 0, 1);
        t->cy += n;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        break;

    case 'C':  /* CUF - Cursor Forward */
        n = ap_param(t, 0, 1);
        t->cx += n;
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        break;

    case 'D':  /* CUB - Cursor Backward */
        n = ap_param(t, 0, 1);
        t->cx -= n;
        if (t->cx < 0) t->cx = 0;
        break;

    case 'E':  /* CNL - Cursor Next Line */
        n = ap_param(t, 0, 1);
        t->cy += n;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        t->cx = 0;
        break;

    case 'F':  /* CPL - Cursor Previous Line */
        n = ap_param(t, 0, 1);
        t->cy -= n;
        if (t->cy < 0) t->cy = 0;
        t->cx = 0;
        break;

    case 'G':  /* CHA - Cursor Horizontal Absolute */
        n = ap_param(t, 0, 1);
        t->cx = n - 1;
        if (t->cx < 0) t->cx = 0;
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        break;

    case 'J':  /* ED - Erase in Display */
        n = ap_param(t, 0, 0);
        if (n == 0) {
            /* cursor to end of screen */
            ap_erase_region(t, t->cy, t->cx, t->rows - 1, t->cols - 1);
        } else if (n == 1) {
            /* start of screen to cursor */
            ap_erase_region(t, 0, 0, t->cy, t->cx);
        } else if (n == 2 || n == 3) {
            /* entire screen */
            ap_erase_region(t, 0, 0, t->rows - 1, t->cols - 1);
        }
        break;

    case 'K':  /* EL - Erase in Line */
        n = ap_param(t, 0, 0);
        if (n == 0) {
            ap_erase_region(t, t->cy, t->cx, t->cy, t->cols - 1);
        } else if (n == 1) {
            ap_erase_region(t, t->cy, 0, t->cy, t->cx);
        } else if (n == 2) {
            ap_erase_region(t, t->cy, 0, t->cy, t->cols - 1);
        }
        break;

    case 'S':  /* SU - Scroll Up */
        n = ap_param(t, 0, 1);
        for (int i = 0; i < n; i++) ap_scroll_up(t);
        break;

    case 'r':  /* DECSTBM - Set Scrolling Region */
        t->scroll_top = ap_param(t, 0, 1) - 1;
        t->scroll_bot = ap_param(t, 1, t->rows) - 1;
        if (t->scroll_top < 0) t->scroll_top = 0;
        if (t->scroll_bot >= t->rows) t->scroll_bot = t->rows - 1;
        if (t->scroll_top > t->scroll_bot) t->scroll_top = 0;
        t->cx = 0;
        t->cy = 0;
        break;

    case 'L':  /* IL - Insert Lines */
        n = ap_param(t, 0, 1);
        for (int i = 0; i < n && t->cy + n < t->rows; i++) {
            for (int r = t->scroll_bot; r > t->cy; r--)
                memcpy(t->grid[r], t->grid[r - 1], sizeof(ap_cell_t) * t->cols);
            for (int c = 0; c < t->cols; c++) {
                t->grid[t->cy][c].ch = ' ';
                t->grid[t->cy][c].attr = ap_default_attr;
            }
        }
        break;

    case 'M':  /* DL - Delete Lines */
        n = ap_param(t, 0, 1);
        for (int i = 0; i < n; i++) {
            for (int r = t->cy; r < t->scroll_bot; r++)
                memcpy(t->grid[r], t->grid[r + 1], sizeof(ap_cell_t) * t->cols);
            for (int c = 0; c < t->cols; c++) {
                t->grid[t->scroll_bot][c].ch = ' ';
                t->grid[t->scroll_bot][c].attr = ap_default_attr;
            }
        }
        break;

    case 'd':  /* VPA - Vertical Line Position Absolute */
        n = ap_param(t, 0, 1);
        t->cy = n - 1;
        if (t->cy < 0) t->cy = 0;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        break;

    /* Private mode sequences (CSI ? N h/l) -- just silently ignore */
    case 'h': case 'l':
        break;

    default:
        /* Unknown CSI -- ignore */
        break;
    }
}

/* ---- ESC dispatch ---- */

static void ap_esc_dispatch(ap_term_t *t, uint8_t ch)
{
    switch (ch) {
    case 'c':  /* RIS - Full Reset */
        ap_clear(t);
        break;
    case 'D':  /* IND - Index (move cursor down, scroll if at bottom) */
        ap_newline(t);
        break;
    case 'E':  /* NEL - Next Line */
        t->cx = 0;
        ap_newline(t);
        break;
    case 'M':  /* RI - Reverse Index (scroll down) */
        t->cy--;
        if (t->cy < t->scroll_top) {
            t->cy = t->scroll_top;
            /* Scroll down: move lines down within scroll region */
            for (int r = t->scroll_bot; r > t->scroll_top; r--)
                memcpy(t->grid[r], t->grid[r - 1], sizeof(ap_cell_t) * t->cols);
            for (int c = 0; c < t->cols; c++) {
                t->grid[t->scroll_top][c].ch = ' ';
                t->grid[t->scroll_top][c].attr = ap_default_attr;
            }
        }
        break;
    case '7':  /* DECSC - Save Cursor (not implemented, stub) */
        break;
    case '8':  /* DECRC - Restore Cursor (not implemented, stub) */
        break;
    default:
        break;
    }
}

/* ---- Control character handling (GROUND execute) ---- */

static void ap_execute(ap_term_t *t, uint8_t ch)
{
    switch (ch) {
    case '\r':  /* CR */
        t->cx = 0;
        break;
    case '\n':  /* LF */
    case '\x0B': /* VT */
    case '\x0C': /* FF */
        ap_newline(t);
        break;
    case '\t':  /* HT - Horizontal Tab */
        t->cx = (t->cx + 8) & ~7;
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        break;
    case '\b':  /* BS - Backspace */
        if (t->cx > 0) t->cx--;
        break;
    case '\a':  /* BEL - Bell (ignore) */
        break;
    case '\x0E': /* SO - Shift Out (ignore) */
    case '\x0F': /* SI - Shift In  (ignore) */
        break;
    default:
        break;
    }
}

/* ---- Main state machine: process one byte ---- */

void ap_feed_byte(ap_term_t *t, uint8_t ch)
{
    /*
     * "Anywhere" transitions -- these override any state.
     * Per Paul Williams: ESC always enters ESCAPE, CAN/SUB go to GROUND.
     */
    if (ch == 0x1B) {
        /* ESC -> enter ESCAPE state */
        t->state = AP_ESCAPE;
        t->nparams = 0;
        t->intermediate = 0;
        return;
    }
    if (ch == 0x18 || ch == 0x1A) {
        /* CAN, SUB -> abort sequence, return to GROUND */
        t->state = AP_GROUND;
        return;
    }

    switch (t->state) {

    case AP_GROUND:
        if (ch < 0x20) {
            /* C0 control */
            ap_execute(t, ch);
        } else if (ch == 0x7F) {
            /* DEL -- ignore */
        } else {
            /* Printable character */
            ap_put_char(t, ch);
        }
        break;

    case AP_ESCAPE:
        if (ch < 0x20) {
            /* C0 control during escape -- execute it */
            ap_execute(t, ch);
        } else if (ch == 0x7F) {
            /* ignore DEL */
        } else if (ch == '[') {
            /* CSI introducer */
            t->state = AP_CSI_ENTRY;
            t->nparams = 0;
            t->intermediate = 0;
        } else if (ch == ']') {
            /* OSC introducer */
            t->state = AP_OSC_STRING;
            t->osc_len = 0;
        } else if (ch >= 0x20 && ch <= 0x2F) {
            /* Intermediate character */
            t->intermediate = ch;
            t->state = AP_ESCAPE_INTERMEDIATE;
        } else if (ch >= 0x30 && ch <= 0x7E) {
            /* Final character -- dispatch ESC sequence */
            ap_esc_dispatch(t, ch);
            t->state = AP_GROUND;
        }
        break;

    case AP_ESCAPE_INTERMEDIATE:
        if (ch < 0x20) {
            ap_execute(t, ch);
        } else if (ch >= 0x20 && ch <= 0x2F) {
            t->intermediate = ch;  /* collect (overwrite for simplicity) */
        } else if (ch >= 0x30 && ch <= 0x7E) {
            /* ESC intermediate final -- dispatch and return to ground */
            ap_esc_dispatch(t, ch);
            t->state = AP_GROUND;
        } else if (ch == 0x7F) {
            /* ignore */
        }
        break;

    case AP_CSI_ENTRY:
        if (ch < 0x20) {
            ap_execute(t, ch);
        } else if (ch == 0x7F) {
            /* ignore */
        } else if (ch >= '0' && ch <= '9') {
            /* First digit -- start collecting params */
            t->nparams = 1;
            t->params[0] = ch - '0';
            t->state = AP_CSI_PARAM;
        } else if (ch == ';') {
            /* Empty first param (defaults to 0) */
            t->nparams = 2;
            t->params[0] = 0;
            t->params[1] = 0;
            t->state = AP_CSI_PARAM;
        } else if (ch >= 0x3C && ch <= 0x3F) {
            /* Private marker: ? > = (e.g., CSI ? 25 h) */
            t->intermediate = ch;
            t->state = AP_CSI_PARAM;
        } else if (ch == ':') {
            t->state = AP_CSI_IGNORE;
        } else if (ch >= 0x20 && ch <= 0x2F) {
            t->intermediate = ch;
            t->state = AP_CSI_INTERMEDIATE;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            /* Immediate final with no params */
            ap_csi_dispatch(t, ch);
            t->state = AP_GROUND;
        }
        break;

    case AP_CSI_PARAM:
        if (ch < 0x20) {
            ap_execute(t, ch);
        } else if (ch >= '0' && ch <= '9') {
            /* Accumulate digit */
            if (t->nparams == 0) {
                t->nparams = 1;
                t->params[0] = 0;
            }
            if (t->nparams <= AP_MAX_PARAMS) {
                t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (ch - '0');
            }
        } else if (ch == ';') {
            /* Next parameter */
            if (t->nparams < AP_MAX_PARAMS) {
                t->params[t->nparams] = 0;
                t->nparams++;
            }
        } else if (ch == ':') {
            /* Sub-parameter separator -- enter ignore */
            t->state = AP_CSI_IGNORE;
        } else if (ch >= 0x3C && ch <= 0x3F) {
            /* Out of place -- ignore rest */
            t->state = AP_CSI_IGNORE;
        } else if (ch >= 0x20 && ch <= 0x2F) {
            t->intermediate = ch;
            t->state = AP_CSI_INTERMEDIATE;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            /* Final character -- dispatch */
            ap_csi_dispatch(t, ch);
            t->state = AP_GROUND;
        } else if (ch == 0x7F) {
            /* ignore */
        }
        break;

    case AP_CSI_INTERMEDIATE:
        if (ch < 0x20) {
            ap_execute(t, ch);
        } else if (ch >= 0x20 && ch <= 0x2F) {
            t->intermediate = ch;
        } else if (ch >= 0x30 && ch <= 0x3F) {
            t->state = AP_CSI_IGNORE;
        } else if (ch >= 0x40 && ch <= 0x7E) {
            ap_csi_dispatch(t, ch);
            t->state = AP_GROUND;
        } else if (ch == 0x7F) {
            /* ignore */
        }
        break;

    case AP_CSI_IGNORE:
        if (ch < 0x20) {
            ap_execute(t, ch);
        } else if (ch >= 0x40 && ch <= 0x7E) {
            /* Final byte ends the ignored sequence */
            t->state = AP_GROUND;
        }
        /* Everything else (0x20-0x3F, 0x7F) is ignored */
        break;

    case AP_OSC_STRING:
        if (ch == 0x07) {
            /* BEL terminates OSC (xterm style) */
            t->state = AP_GROUND;
            /* TODO: handle OSC content if needed */
        } else if (ch == 0x1B) {
            /* ESC -- will be caught at top of next call as anywhere transition.
             * But ST is ESC \ -- handle ESC within OSC specially: */
            /* Actually, the 0x1B check at the top already handles this.
             * The \ in ESCAPE state dispatches back to GROUND. */
            t->state = AP_ESCAPE;  /* let ESC \ terminate */
        } else if (ch >= 0x20 && ch <= 0x7E) {
            if (t->osc_len < AP_MAX_OSC - 1)
                t->osc_buf[t->osc_len++] = (char)ch;
        }
        /* C0 controls in OSC are ignored per spec */
        break;

    default:
        t->state = AP_GROUND;
        break;
    }
}

/* ---- Feed a buffer of bytes ---- */

void ap_feed(ap_term_t *t, const uint8_t *data, int len)
{
    for (int i = 0; i < len; i++)
        ap_feed_byte(t, data[i]);
}

/* ---- Init / clear ---- */

void ap_clear(ap_term_t *t)
{
    t->cx = 0;
    t->cy = 0;
    t->attr = ap_default_attr;
    t->state = AP_GROUND;
    t->nparams = 0;
    t->intermediate = 0;
    t->osc_len = 0;
    t->scroll_top = 0;
    t->scroll_bot = t->rows - 1;

    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            t->grid[r][c].ch = ' ';
            t->grid[r][c].attr = ap_default_attr;
        }
    }
}

void ap_init(ap_term_t *t, int rows, int cols)
{
    memset(t, 0, sizeof(*t));
    t->rows = (rows > AP_ROWS) ? AP_ROWS : rows;
    t->cols = (cols > AP_COLS) ? AP_COLS : cols;
    t->scroll_cb = NULL;
    t->user = NULL;
    ap_clear(t);
}

void ap_set_scroll_cb(ap_term_t *t, ap_scroll_cb_t cb, void *user)
{
    t->scroll_cb = cb;
    t->user = user;
}

#endif /* ANSI_PARSER_IMPLEMENTATION */
