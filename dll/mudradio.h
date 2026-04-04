/* mudradio.h — MUDRadio types, global state, forward declarations.
 * Included early in vk_terminal.c (before usage in menu/draw/mouse).
 */

#ifndef MUDRADIO_H
#define MUDRADIO_H

/* ---- Beat / Analysis Data ---- */

typedef struct {
    float onset_strength;
    float bpm;
    float bass_energy, mid_energy, treble_energy;
    float spectrum[512];
    int   onset_detected;
    int   beat_count;       /* monotonic counter, increments on each onset */
    DWORD tick;
} mr_beat_t;

/* ---- Metadata ---- */

typedef struct {
    char station_name[128];
    char stream_title[256];
    char artist[128];
    char track[128];
    char genre[64];
    int  bitrate;
} mr_meta_t;

/* ---- Enums ---- */

typedef enum { MR_STATE_STOPPED, MR_STATE_PLAYING, MR_STATE_PAUSED, MR_STATE_CONNECTING, MR_STATE_ERROR } mr_state_t;
typedef enum { MR_SRC_NONE, MR_SRC_FILE, MR_SRC_STREAM } mr_src_t;
typedef enum { MR_TAB_RADIO, MR_TAB_PLAYER } mr_tab_t;
typedef enum { MR_LIST_FAVORITES, MR_LIST_SEARCH, MR_LIST_GENRES } mr_list_mode_t;

/* ---- Data types ---- */

#define MR_MAX_PLAYLIST  256
#define MR_MAX_PATH_LEN  512
#define MR_MAX_SEARCH    100
#define MR_MAX_FAVORITES  64

typedef struct { char path[MR_MAX_PATH_LEN]; char title[128]; int is_stream; } mr_entry_t;
typedef struct { char name[128]; char url[512]; char genre[64]; char country[64]; int bitrate; int is_favorite; } mr_station_t;

/* ---- Global State ---- */

static volatile mr_state_t mr_transport = MR_STATE_STOPPED;
static volatile float mr_volume = 0.8f;
static volatile int mr_shuffle = 0;
static volatile int mr_repeat = 0;

static mr_beat_t  mr_beat;
static mr_beat_t  mr_beat_snap;
static CRITICAL_SECTION mr_beat_lock;
static mr_meta_t  mr_meta;
static CRITICAL_SECTION mr_meta_lock;

static mr_entry_t   mr_playlist[MR_MAX_PLAYLIST];
static int mr_pl_count = 0;
static int mr_pl_current = -1;

static mr_station_t mr_search_results[MR_MAX_SEARCH];
static int mr_search_count = 0;
static volatile int mr_searching = 0;
static mr_station_t mr_favorites[MR_MAX_FAVORITES];
static int mr_fav_count = 0;

/* Panel state */
static int   mr_visible = 0;
static float mr_x = 100.0f, mr_y = 60.0f;
static float mr_w = 380.0f, mr_h = 500.0f;
static int   mr_dragging = 0;
static float mr_drag_ox, mr_drag_oy;
static mr_tab_t mr_tab = MR_TAB_RADIO;
static mr_list_mode_t mr_list_mode = MR_LIST_FAVORITES;
static int   mr_scroll = 0;
static int   mr_hover_item = -1;
static int   mr_search_focus = 0;
static char  mr_search_buf[128] = {0};
static int   mr_search_len = 0;
static int   mr_vol_dragging = 0;
static int   mr_resizing = 0;

/* ---- Forward Declarations ---- */

static void mr_init(void);
static void mr_shutdown(void);
static void mr_draw(int vp_w, int vp_h);
static void mr_toggle(void);
static int  mr_mouse_down(int mx, int my);
static void mr_mouse_move(int mx, int my);
static void mr_mouse_up(void);
static int  mr_key_char(int ch_code);
static void mr_play_file(const char *path);
static void mr_play_stream(const char *url);
static void mr_stop(void);
static void mr_pause(void);
static void mr_resume(void);
static void mr_next(void);
static void mr_prev(void);

#endif /* MUDRADIO_H */
