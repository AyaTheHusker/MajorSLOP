/* script_manager_ui.c — Vulkan-rendered Script Manager + Code Editor
 * Included into vk_terminal.c (like mudradio_ui.c)
 *
 * Features:
 *   Tab 0: Script list (load/unload/edit/delete)
 *   Tab 1: Code editor with Python syntax highlighting,
 *           auto-indent, find/replace, line numbers
 *   Fullscreen/windowed toggle (F11)
 */

/* ---- Text Buffer ---- */
#define SCM_MAX_LINES   16384
#define SCM_LINE_ALLOC  256   /* initial alloc per line */

static char  **scm_lines = NULL;  /* array of malloc'd strings */
static int    *scm_line_cap = NULL; /* capacity of each line */
static int     scm_line_count = 0;

/* ---- Panel State ---- */
static int   scm_visible = 0;
static float scm_x = 100.0f, scm_y = 50.0f;
static float scm_w = 800.0f, scm_h = 600.0f;
static int   scm_dragging = 0;
static float scm_drag_ox, scm_drag_oy;
static int   scm_resizing = 0;
static int   scm_scaled = 0;    /* initial scale done? */
#define SCM_MIN_W 400.0f
#define SCM_MIN_H 300.0f
#define SCM_RESIZE_GRIP 14
static int   scm_btn_hover = -1; /* hovered button index in script list */
static int   scm_tab = 0;       /* 0=Scripts, 1=Editor */
static int   scm_focused = 0;   /* editor has keyboard focus */
static int   scm_fullscreen = 0;
static float scm_restore_x, scm_restore_y, scm_restore_w, scm_restore_h;

/* Editor state */
static int   scm_cur_row = 0, scm_cur_col = 0;  /* cursor position */
static int   scm_scroll_y = 0;   /* first visible line */
static int   scm_scroll_x = 0;   /* horizontal scroll (columns) */
static int   scm_sel_active = 0; /* selection active */
static int   scm_sel_row = 0, scm_sel_col = 0;  /* selection anchor */
static int   scm_modified = 0;
static char  scm_filepath[MAX_PATH] = {0};
static char  scm_filename[64] = "untitled.py";

/* Find/Replace */
static int   scm_find_open = 0;
static char  scm_find_text[128] = {0};
static char  scm_repl_text[128] = {0};
static int   scm_find_cursor = 0;  /* cursor in find input */
static int   scm_repl_cursor = 0;
static int   scm_find_focused = 0; /* 0=find, 1=replace */

/* Script list */
#define SCM_MAX_SCRIPTS 64
static char  scm_script_names[SCM_MAX_SCRIPTS][64];
static int   scm_script_loaded[SCM_MAX_SCRIPTS]; /* 1=loaded */
static int   scm_script_count = 0;
static int   scm_list_sel = -1;
static int   scm_list_scroll = 0;
static int   scm_list_hover = -1;

/* ---- Syntax Colors (normalized 0-1 floats) ---- */
#define SCM_COL_TEXT_R     0.83f
#define SCM_COL_TEXT_G     0.83f
#define SCM_COL_TEXT_B     0.86f
#define SCM_COL_KW_R       0.78f  /* purple — keywords */
#define SCM_COL_KW_G       0.47f
#define SCM_COL_KW_B       0.87f
#define SCM_COL_BI_R       0.38f  /* blue — builtins */
#define SCM_COL_BI_G       0.69f
#define SCM_COL_BI_B       0.94f
#define SCM_COL_STR_R      0.60f  /* green — strings */
#define SCM_COL_STR_G      0.76f
#define SCM_COL_STR_B      0.47f
#define SCM_COL_CMT_R      0.36f  /* gray — comments */
#define SCM_COL_CMT_G      0.39f
#define SCM_COL_CMT_B      0.44f
#define SCM_COL_NUM_R      0.82f  /* orange — numbers */
#define SCM_COL_NUM_G      0.60f
#define SCM_COL_NUM_B      0.40f
#define SCM_COL_DEC_R      0.90f  /* yellow — decorators */
#define SCM_COL_DEC_G      0.75f
#define SCM_COL_DEC_B      0.48f
#define SCM_COL_MUD_R      0.34f  /* cyan — mud.* */
#define SCM_COL_MUD_G      0.71f
#define SCM_COL_MUD_B      0.76f
#define SCM_COL_OFF_R      0.88f  /* red — OFF.*, self */
#define SCM_COL_OFF_G      0.42f
#define SCM_COL_OFF_B      0.46f
#define SCM_COL_GUTTER_R   0.40f
#define SCM_COL_GUTTER_G   0.40f
#define SCM_COL_GUTTER_B   0.50f
#define SCM_COL_CURSOR_R   1.0f
#define SCM_COL_CURSOR_G   1.0f
#define SCM_COL_CURSOR_B   0.8f
#define SCM_COL_SEL_R      0.22f
#define SCM_COL_SEL_G      0.35f
#define SCM_COL_SEL_B      0.55f

/* Python keywords */
static const char *scm_keywords[] = {
    "False","None","True","and","as","assert","async","await",
    "break","class","continue","def","del","elif","else","except",
    "finally","for","from","global","if","import","in","is",
    "lambda","nonlocal","not","or","pass","raise","return","try",
    "while","with","yield", NULL
};
static const char *scm_builtins[] = {
    "abs","all","any","bin","bool","bytes","callable","chr",
    "dict","dir","enumerate","eval","exec","filter","float","format",
    "getattr","globals","hasattr","hex","id","input","int",
    "isinstance","iter","len","list","locals","map",
    "max","min","next","object","open","ord","pow","print",
    "property","range","repr","reversed","round","set",
    "slice","sorted","str","sum","super","tuple","type","vars","zip",
    /* Common stdlib modules (highlighted when used as identifiers) */
    "threading","Thread","re","ctypes","time","os","sys","struct",
    "json","math","random","socket","signal","logging","io",
    "collections","functools","itertools","pathlib",
    "create_string_buffer","c_int","c_float","c_char_p","POINTER",
    "compile","match","search","sub","findall","sleep",
    NULL
};

/* ---- Buffer Management ---- */

static void scm_buf_init(void)
{
    if (scm_lines) return;
    scm_lines = (char **)calloc(SCM_MAX_LINES, sizeof(char *));
    scm_line_cap = (int *)calloc(SCM_MAX_LINES, sizeof(int));
    scm_lines[0] = (char *)calloc(SCM_LINE_ALLOC, 1);
    scm_line_cap[0] = SCM_LINE_ALLOC;
    scm_lines[0][0] = '\0';
    scm_line_count = 1;
}

static void scm_buf_clear(void)
{
    if (!scm_lines) { scm_buf_init(); return; }
    for (int i = 0; i < scm_line_count; i++) {
        if (scm_lines[i]) { free(scm_lines[i]); scm_lines[i] = NULL; }
        scm_line_cap[i] = 0;
    }
    scm_lines[0] = (char *)calloc(SCM_LINE_ALLOC, 1);
    scm_line_cap[0] = SCM_LINE_ALLOC;
    scm_line_count = 1;
    scm_cur_row = scm_cur_col = 0;
    scm_scroll_y = scm_scroll_x = 0;
    scm_sel_active = 0;
}

static void scm_ensure_cap(int line, int need)
{
    if (need < scm_line_cap[line]) return;
    int newcap = scm_line_cap[line];
    while (newcap <= need) newcap *= 2;
    scm_lines[line] = (char *)realloc(scm_lines[line], newcap);
    scm_line_cap[line] = newcap;
}

static int scm_line_len(int line)
{
    if (line < 0 || line >= scm_line_count || !scm_lines[line]) return 0;
    return (int)strlen(scm_lines[line]);
}

static void scm_insert_line(int at)
{
    if (scm_line_count >= SCM_MAX_LINES - 1) return;
    for (int i = scm_line_count; i > at; i--) {
        scm_lines[i] = scm_lines[i-1];
        scm_line_cap[i] = scm_line_cap[i-1];
    }
    scm_lines[at] = (char *)calloc(SCM_LINE_ALLOC, 1);
    scm_line_cap[at] = SCM_LINE_ALLOC;
    scm_line_count++;
}

static void scm_delete_line(int at)
{
    if (scm_line_count <= 1) return;
    free(scm_lines[at]);
    for (int i = at; i < scm_line_count - 1; i++) {
        scm_lines[i] = scm_lines[i+1];
        scm_line_cap[i] = scm_line_cap[i+1];
    }
    scm_lines[scm_line_count - 1] = NULL;
    scm_line_cap[scm_line_count - 1] = 0;
    scm_line_count--;
}

/* Insert character at cursor */
static void scm_insert_char(char ch)
{
    scm_buf_init();
    int len = scm_line_len(scm_cur_row);
    if (scm_cur_col > len) scm_cur_col = len;
    scm_ensure_cap(scm_cur_row, len + 2);
    memmove(&scm_lines[scm_cur_row][scm_cur_col + 1],
            &scm_lines[scm_cur_row][scm_cur_col], len - scm_cur_col + 1);
    scm_lines[scm_cur_row][scm_cur_col] = ch;
    scm_cur_col++;
    scm_modified = 1;
}

/* Insert newline with auto-indent */
static void scm_insert_newline(void)
{
    scm_buf_init();
    int len = scm_line_len(scm_cur_row);
    if (scm_cur_col > len) scm_cur_col = len;

    /* Get current line's indentation */
    int indent = 0;
    while (indent < len && (scm_lines[scm_cur_row][indent] == ' ' || scm_lines[scm_cur_row][indent] == '\t'))
        indent++;

    /* Check if line ends with colon → extra indent */
    int extra = 0;
    {
        int end = len - 1;
        while (end >= 0 && (scm_lines[scm_cur_row][end] == ' ' || scm_lines[scm_cur_row][end] == '\t'))
            end--;
        if (end >= 0 && scm_lines[scm_cur_row][end] == ':')
            extra = 4;
    }

    /* Split line at cursor */
    char *tail = scm_lines[scm_cur_row] + scm_cur_col;
    int tail_len = len - scm_cur_col;

    scm_insert_line(scm_cur_row + 1);

    /* New line = indent + extra + tail */
    int new_indent = indent + extra;
    if (new_indent > scm_cur_col) new_indent = indent; /* indent can't exceed cursor pos context */
    scm_ensure_cap(scm_cur_row + 1, new_indent + tail_len + 1);

    /* Build new line with indentation */
    for (int i = 0; i < indent && i < scm_cur_col; i++)
        scm_lines[scm_cur_row + 1][i] = scm_lines[scm_cur_row][i]; /* preserve tab/space style */
    for (int i = indent; i < new_indent; i++)
        scm_lines[scm_cur_row + 1][i] = ' ';
    memcpy(&scm_lines[scm_cur_row + 1][new_indent], tail, tail_len + 1);

    /* Truncate current line at cursor */
    scm_lines[scm_cur_row][scm_cur_col] = '\0';

    scm_cur_row++;
    scm_cur_col = new_indent;
    scm_modified = 1;
}

/* Backspace */
static void scm_backspace(void)
{
    if (scm_cur_col > 0) {
        int len = scm_line_len(scm_cur_row);
        memmove(&scm_lines[scm_cur_row][scm_cur_col - 1],
                &scm_lines[scm_cur_row][scm_cur_col], len - scm_cur_col + 1);
        scm_cur_col--;
        scm_modified = 1;
    } else if (scm_cur_row > 0) {
        /* Join with previous line */
        int prev_len = scm_line_len(scm_cur_row - 1);
        int cur_len = scm_line_len(scm_cur_row);
        scm_ensure_cap(scm_cur_row - 1, prev_len + cur_len + 1);
        memcpy(&scm_lines[scm_cur_row - 1][prev_len], scm_lines[scm_cur_row], cur_len + 1);
        scm_delete_line(scm_cur_row);
        scm_cur_row--;
        scm_cur_col = prev_len;
        scm_modified = 1;
    }
}

/* Delete key */
static void scm_delete_char(void)
{
    int len = scm_line_len(scm_cur_row);
    if (scm_cur_col < len) {
        memmove(&scm_lines[scm_cur_row][scm_cur_col],
                &scm_lines[scm_cur_row][scm_cur_col + 1], len - scm_cur_col);
        scm_modified = 1;
    } else if (scm_cur_row < scm_line_count - 1) {
        /* Join with next line */
        int next_len = scm_line_len(scm_cur_row + 1);
        scm_ensure_cap(scm_cur_row, len + next_len + 1);
        memcpy(&scm_lines[scm_cur_row][len], scm_lines[scm_cur_row + 1], next_len + 1);
        scm_delete_line(scm_cur_row + 1);
        scm_modified = 1;
    }
}

/* Tab → insert 4 spaces */
static void scm_insert_tab(void)
{
    for (int i = 0; i < 4; i++) scm_insert_char(' ');
}

/* ---- File I/O ---- */

static void scm_load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    scm_buf_clear();

    char line[4096];
    int row = 0;
    while (fgets(line, sizeof(line), f) && row < SCM_MAX_LINES - 1) {
        /* Strip trailing newline */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        if (row > 0) scm_insert_line(row);
        scm_ensure_cap(row, len + 1);
        memcpy(scm_lines[row], line, len + 1);
        row++;
    }
    if (row == 0) row = 1; /* keep at least one line */
    scm_line_count = row;
    fclose(f);

    strncpy(scm_filepath, path, MAX_PATH - 1);
    const char *fn = strrchr(path, '\\');
    if (!fn) fn = strrchr(path, '/');
    if (fn) fn++; else fn = path;
    strncpy(scm_filename, fn, 63);

    scm_cur_row = scm_cur_col = 0;
    scm_scroll_y = scm_scroll_x = 0;
    scm_modified = 0;
}

static void scm_save_file(void)
{
    if (scm_filepath[0] == '\0') return;
    FILE *f = fopen(scm_filepath, "w");
    if (!f) return;
    for (int i = 0; i < scm_line_count; i++) {
        fputs(scm_lines[i] ? scm_lines[i] : "", f);
        fputc('\n', f);
    }
    fclose(f);
    scm_modified = 0;
}

/* ---- Script List ---- */

static void scm_scan_scripts(void)
{
    /* Save which scripts were loaded before rescan */
    char prev_loaded[SCM_MAX_SCRIPTS][64];
    int  prev_count = 0;
    for (int i = 0; i < scm_script_count; i++) {
        if (scm_script_loaded[i]) {
            strncpy(prev_loaded[prev_count], scm_script_names[i], 63);
            prev_loaded[prev_count][63] = '\0';
            prev_count++;
        }
    }

    char search[MAX_PATH];
    char base[MAX_PATH];
    GetModuleFileNameA(NULL, base, MAX_PATH);
    char *sl = strrchr(base, '\\');
    if (sl) *sl = '\0';
    _snprintf(search, MAX_PATH, "%s\\plugins\\MMUDPy\\scripts\\*.py", base);

    scm_script_count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (scm_script_count >= SCM_MAX_SCRIPTS) break;
        strncpy(scm_script_names[scm_script_count], fd.cFileName, 63);
        char *dot = strstr(scm_script_names[scm_script_count], ".py");
        if (dot) *dot = '\0';

        /* Restore loaded state if this script was loaded before */
        scm_script_loaded[scm_script_count] = 0;
        for (int j = 0; j < prev_count; j++) {
            if (strcmp(scm_script_names[scm_script_count], prev_loaded[j]) == 0) {
                scm_script_loaded[scm_script_count] = 1;
                break;
            }
        }
        scm_script_count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---- Syntax Highlighting Helpers ---- */

static int scm_is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

static int scm_is_keyword(const char *w, int wlen)
{
    for (int i = 0; scm_keywords[i]; i++)
        if ((int)strlen(scm_keywords[i]) == wlen && memcmp(scm_keywords[i], w, wlen) == 0) return 1;
    return 0;
}

static int scm_is_builtin(const char *w, int wlen)
{
    for (int i = 0; scm_builtins[i]; i++)
        if ((int)strlen(scm_builtins[i]) == wlen && memcmp(scm_builtins[i], w, wlen) == 0) return 1;
    return 0;
}

/* Get syntax color for character at position in line.
 * Returns color via r,g,b pointers. Simple left-to-right scanner per line. */
static void scm_get_char_color(const char *line, int col,
                                float *r, float *g, float *b)
{
    *r = SCM_COL_TEXT_R; *g = SCM_COL_TEXT_G; *b = SCM_COL_TEXT_B;

    int len = (int)strlen(line);
    int i = 0;

    /* Scan from start to find what token 'col' falls in */
    while (i <= col && i < len) {
        /* Comment */
        if (line[i] == '#') {
            if (col >= i) { *r = SCM_COL_CMT_R; *g = SCM_COL_CMT_G; *b = SCM_COL_CMT_B; }
            return;
        }

        /* Strings */
        if (line[i] == '\'' || line[i] == '"') {
            char q = line[i];
            int start = i;
            /* Check triple quote */
            if (i + 2 < len && line[i+1] == q && line[i+2] == q) {
                i += 3;
                while (i < len && !(line[i] == q && i + 2 < len && line[i+1] == q && line[i+2] == q)) {
                    if (line[i] == '\\') i++;
                    i++;
                }
                if (i < len) i += 3; /* skip closing triple */
            } else {
                i++;
                while (i < len && line[i] != q && line[i] != '\n') {
                    if (line[i] == '\\') i++;
                    i++;
                }
                if (i < len) i++; /* skip closing quote */
            }
            if (col >= start && col < i) {
                *r = SCM_COL_STR_R; *g = SCM_COL_STR_G; *b = SCM_COL_STR_B;
                return;
            }
            continue;
        }

        /* Decorator */
        if (line[i] == '@' && (i == 0 || !scm_is_ident(line[i-1]))) {
            int start = i; i++;
            while (i < len && (scm_is_ident(line[i]) || line[i] == '.')) i++;
            if (col >= start && col < i) {
                *r = SCM_COL_DEC_R; *g = SCM_COL_DEC_G; *b = SCM_COL_DEC_B;
                return;
            }
            continue;
        }

        /* Number */
        if (isdigit((unsigned char)line[i]) && (i == 0 || !scm_is_ident(line[i-1]))) {
            int start = i;
            if (line[i] == '0' && i + 1 < len && (line[i+1] == 'x' || line[i+1] == 'X')) {
                i += 2;
                while (i < len && isxdigit((unsigned char)line[i])) i++;
            } else {
                while (i < len && (isdigit((unsigned char)line[i]) || line[i] == '.')) i++;
            }
            if (col >= start && col < i) {
                *r = SCM_COL_NUM_R; *g = SCM_COL_NUM_G; *b = SCM_COL_NUM_B;
                return;
            }
            continue;
        }

        /* Identifier / keyword / builtin */
        if (scm_is_ident(line[i])) {
            int start = i;
            while (i < len && scm_is_ident(line[i])) i++;
            int wlen = i - start;

            /* Check for mud.xxx.yyy.zzz() or OFF.xxx chain */
            int end_of_token = i;
            if (i < len && line[i] == '.') {
                /* Follow entire dotted chain: mud.bg.plasma.speed */
                int di = i;
                while (di < len && (scm_is_ident(line[di]) || line[di] == '.')) di++;
                /* Don't include trailing dot */
                if (di > 0 && line[di-1] == '.') di--;
                if (wlen == 3 && memcmp(line + start, "mud", 3) == 0) {
                    end_of_token = di;
                    if (col >= start && col < end_of_token) {
                        *r = SCM_COL_MUD_R; *g = SCM_COL_MUD_G; *b = SCM_COL_MUD_B;
                        return;
                    }
                    i = di;
                    continue;
                }
                if (wlen == 3 && memcmp(line + start, "OFF", 3) == 0) {
                    end_of_token = di;
                    if (col >= start && col < end_of_token) {
                        *r = SCM_COL_OFF_R; *g = SCM_COL_OFF_G; *b = SCM_COL_OFF_B;
                        return;
                    }
                    i = di;
                    continue;
                }
            }

            if (col >= start && col < i) {
                if (wlen == 4 && memcmp(line + start, "self", 4) == 0) {
                    *r = SCM_COL_OFF_R; *g = SCM_COL_OFF_G; *b = SCM_COL_OFF_B;
                } else if (wlen == 4 && memcmp(line + start, "_dll", 4) == 0) {
                    *r = SCM_COL_MUD_R; *g = SCM_COL_MUD_G; *b = SCM_COL_MUD_B;
                } else if (scm_is_keyword(line + start, wlen)) {
                    *r = SCM_COL_KW_R; *g = SCM_COL_KW_G; *b = SCM_COL_KW_B;
                } else if (scm_is_builtin(line + start, wlen)) {
                    *r = SCM_COL_BI_R; *g = SCM_COL_BI_G; *b = SCM_COL_BI_B;
                }
                return;
            }
            continue;
        }

        i++;
    }
}

/* ---- Toggle / Open ---- */

static void scm_toggle(void)
{
    scm_visible = !scm_visible;
    if (scm_visible) {
        scm_buf_init();
        scm_scan_scripts();
        if (scm_w < 100) {
            scm_w = 800 * ui_scale;
            scm_h = 600 * ui_scale;
        }
    }
}

static void scm_open_script_in_editor(const char *name)
{
    char base[MAX_PATH];
    GetModuleFileNameA(NULL, base, MAX_PATH);
    char *sl = strrchr(base, '\\');
    if (sl) *sl = '\0';

    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%s\\plugins\\MMUDPy\\scripts\\%s.py", base, name);
    scm_load_file(path);
    scm_tab = 1;
    scm_focused = 1;
}

/* ---- Ensure cursor visible ---- */

static void scm_ensure_cursor_visible(int vis_lines, int vis_cols)
{
    if (scm_cur_row < scm_scroll_y) scm_scroll_y = scm_cur_row;
    if (scm_cur_row >= scm_scroll_y + vis_lines) scm_scroll_y = scm_cur_row - vis_lines + 1;
    if (scm_cur_col < scm_scroll_x) scm_scroll_x = scm_cur_col;
    if (scm_cur_col >= scm_scroll_x + vis_cols - 6) scm_scroll_x = scm_cur_col - vis_cols + 7;
    if (scm_scroll_x < 0) scm_scroll_x = 0;
    if (scm_scroll_y < 0) scm_scroll_y = 0;
}

/* ---- Shared layout helper ---- */

/* Button layout used by both draw and mouse */
#define SCM_BTN_COUNT 5
static const char *scm_btn_labels[SCM_BTN_COUNT] = { "Load", "Unload", "Edit", "Delete", "Refresh" };

/* Returns the button rect for button index bi given layout params.
 * All buttons are same width, horizontally spaced. */
static void scm_btn_rect(int bi, float x0, float btn_y, int btn_w, int btn_h, int pad,
                          float *bx0, float *by0, float *bx1, float *by1)
{
    *bx0 = x0 + (float)(pad + bi * (btn_w + pad / 2));
    *by0 = btn_y;
    *bx1 = *bx0 + (float)btn_w;
    *by1 = *by0 + (float)btn_h;
}

/* Draw a 3D raised button (same style as MudRadio transport) */
static void scm_draw_button(float bx0, float by0, float bx1, float by1,
                             const char *label, int hovered, int disabled,
                             float bgr, float bgg, float bgb,
                             float acr, float acg, float acb,
                             float txr, float txg, float txb,
                             float dmr, float dmg, float dmb,
                             int cw, int ch, int pad, int vp_w, int vp_h,
                             void (*psolid)(float,float,float,float,float,float,float,float,int,int),
                             void (*ptext)(int,int,const char*,float,float,float,int,int,int,int))
{
    float lift = hovered ? 0.06f : 0.0f;
    float br = bgr + 0.12f + lift, bg_ = bgg + 0.12f + lift, bb = bgb + 0.12f + lift;

    /* Button background */
    psolid(bx0, by0, bx1, by1, br, bg_, bb, 0.95f, vp_w, vp_h);
    /* Top highlight (bright) */
    psolid(bx0, by0, bx1, by0 + 2.0f, 1.0f, 1.0f, 1.0f, 0.12f, vp_w, vp_h);
    /* Left highlight */
    psolid(bx0, by0, bx0 + 1.0f, by1, 1.0f, 1.0f, 1.0f, 0.08f, vp_w, vp_h);
    /* Bottom shadow */
    psolid(bx0, by1 - 2.0f, bx1, by1, 0.0f, 0.0f, 0.0f, 0.20f, vp_w, vp_h);
    /* Right shadow */
    psolid(bx1 - 1.0f, by0, bx1, by1, 0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);

    /* Hover inner glow */
    if (hovered && !disabled) {
        psolid(bx0 + 2.0f, by0 + 2.0f, bx1 - 2.0f, by1 - 2.0f,
               acr, acg, acb, 0.10f, vp_w, vp_h);
    }

    /* Button text — centered */
    float lr = disabled ? dmr : txr, lg = disabled ? dmg : txg, lb = disabled ? dmb : txb;
    int lw = (int)strlen(label) * cw;
    int tx = (int)(bx0 + (bx1 - bx0 - lw) / 2.0f);
    int ty = (int)(by0 + (by1 - by0 - ch) / 2.0f);
    ptext(tx + 1, ty + 1, label, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch); /* shadow */
    ptext(tx, ty, label, lr, lg, lb, vp_w, vp_h, cw, ch);
}

/* ---- Draw ---- */

static void scm_draw(int vp_w, int vp_h)
{
    if (!scm_visible) return;
    if (!scm_scaled) {
        scm_w = 800 * ui_scale; scm_h = 600 * ui_scale;
        scm_x = 80 * ui_scale; scm_y = 40 * ui_scale;
        scm_scaled = 1;
    }

    const ui_theme_t *t = &ui_themes[current_theme];
    void (*psolid)(float, float, float, float, float, float, float, float, int, int) =
        ui_font_ready ? push_solid_ui : push_solid;
    void (*ptext)(int, int, const char *, float, float, float, int, int, int, int) =
        ui_font_ready ? push_text_ui : push_text;

    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);
    float x0 = scm_x, y0 = scm_y;
    float pw = scm_w, ph = scm_h;

    if (scm_fullscreen) {
        x0 = 0; y0 = 0; pw = (float)vp_w; ph = (float)vp_h;
    }

    float x1 = x0 + pw, y1 = y0 + ph;
    float bgr = t->bg[0], bgg = t->bg[1], bgb = t->bg[2];
    float txr = t->text[0], txg = t->text[1], txb = t->text[2];
    float dmr = t->dim[0], dmg = t->dim[1], dmb = t->dim[2];
    float acr = t->accent[0], acg = t->accent[1], acb = t->accent[2];

    int titlebar_h = ch + (int)(10 * ui_scale);
    int tab_h = ch + (int)(8 * ui_scale);
    int row_h = ch + (int)(4 * ui_scale);
    int pad = (int)(8 * ui_scale);
    int gutter_w = cw * 5; /* 5-char gutter for line numbers */
    int btn_h = ch + (int)(10 * ui_scale); /* taller buttons */
    int btn_w = cw * 8 + pad;

    /* ---- Panel background ---- */
    psolid(x0, y0, x1, y1, bgr + 0.04f, bgg + 0.04f, bgb + 0.04f, 0.97f, vp_w, vp_h);

    /* ---- Outer bevel: light top/left, dark bottom/right ---- */
    psolid(x0, y0, x1, y0 + 1.0f,
           bgr + 0.18f, bgg + 0.18f, bgb + 0.18f, 0.8f, vp_w, vp_h);
    psolid(x0, y0, x0 + 1.0f, y1,
           bgr + 0.14f, bgg + 0.14f, bgb + 0.14f, 0.7f, vp_w, vp_h);
    psolid(x0, y1 - 1.0f, x1, y1,
           bgr * 0.4f, bgg * 0.4f, bgb * 0.4f, 0.9f, vp_w, vp_h);
    psolid(x1 - 1.0f, y0, x1, y1,
           bgr * 0.5f, bgg * 0.5f, bgb * 0.5f, 0.85f, vp_w, vp_h);

    /* Inner bevel (second line for depth) */
    psolid(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f,
           bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.5f, vp_w, vp_h);
    psolid(x0 + 1.0f, y0 + 1.0f, x0 + 2.0f, y1 - 1.0f,
           bgr + 0.08f, bgg + 0.08f, bgb + 0.08f, 0.4f, vp_w, vp_h);

    /* ---- Title bar ---- */
    float tb_y0 = y0 + 2.0f, tb_y1 = y0 + (float)titlebar_h;
    /* Title bar background — accent tinted */
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y1,
           acr * 0.25f + bgr * 0.5f, acg * 0.25f + bgg * 0.5f, acb * 0.25f + bgb * 0.5f, 0.95f,
           vp_w, vp_h);
    /* Gloss highlight (top half lighter) */
    psolid(x0 + 2.0f, tb_y0, x1 - 2.0f, tb_y0 + (float)(titlebar_h / 2),
           1.0f, 1.0f, 1.0f, 0.06f, vp_w, vp_h);
    /* Title bar bottom edge */
    psolid(x0 + 2.0f, tb_y1 - 1.0f, x1 - 2.0f, tb_y1,
           acr, acg, acb, 0.5f, vp_w, vp_h);

    char title[128];
    _snprintf(title, sizeof(title), "Script Manager - %s%s",
              scm_filename, scm_modified ? " *" : "");
    int title_ty = (int)tb_y0 + (titlebar_h - ch) / 2;
    ptext((int)x0 + pad + 1, title_ty + 1, title, 0.0f, 0.0f, 0.0f, vp_w, vp_h, cw, ch);
    ptext((int)x0 + pad, title_ty, title, txr, txg, txb, vp_w, vp_h, cw, ch);

    /* Close [X] button — draw as a small 3D button */
    {
        float cbx1 = x1 - 4.0f, cbx0 = cbx1 - (float)(cw * 2 + 6);
        float cby0 = tb_y0 + 2.0f, cby1 = tb_y1 - 3.0f;
        psolid(cbx0, cby0, cbx1, cby1, bgr + 0.10f, bgg + 0.08f, bgb + 0.08f, 0.95f, vp_w, vp_h);
        psolid(cbx0, cby0, cbx1, cby0 + 1.0f, 1.0f, 1.0f, 1.0f, 0.10f, vp_w, vp_h);
        psolid(cbx0, cby1 - 1.0f, cbx1, cby1, 0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        int cx = (int)(cbx0 + (cbx1 - cbx0 - cw) / 2);
        int cy = (int)(cby0 + (cby1 - cby0 - ch) / 2);
        ptext(cx, cy, "X", 1.0f, 0.35f, 0.35f, vp_w, vp_h, cw, ch);
    }

    /* Fullscreen/Windowed button */
    {
        float fbx1 = x1 - 4.0f - (float)(cw * 2 + 6) - 4.0f;
        float fbx0 = fbx1 - (float)(cw * 2 + 6);
        float fby0 = tb_y0 + 2.0f, fby1 = tb_y1 - 3.0f;
        psolid(fbx0, fby0, fbx1, fby1, bgr + 0.10f, bgg + 0.10f, bgb + 0.10f, 0.95f, vp_w, vp_h);
        psolid(fbx0, fby0, fbx1, fby0 + 1.0f, 1.0f, 1.0f, 1.0f, 0.10f, vp_w, vp_h);
        psolid(fbx0, fby1 - 1.0f, fbx1, fby1, 0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        int fx = (int)(fbx0 + (fbx1 - fbx0 - cw) / 2);
        int fy = (int)(fby0 + (fby1 - fby0 - ch) / 2);
        ptext(fx, fy, scm_fullscreen ? "W" : "F", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
    }

    /* ---- Tabs ---- */
    float tab_y0_f = tb_y1;
    float tab_y1_f = tab_y0_f + (float)tab_h;

    psolid(x0, tab_y0_f, x1, tab_y1_f, bgr + 0.02f, bgg + 0.02f, bgb + 0.02f, 0.95f, vp_w, vp_h);

    const char *tab_labels[] = { "Scripts", "Editor" };
    for (int ti = 0; ti < 2; ti++) {
        float tx0 = x0 + (float)(ti * cw * 9 + pad);
        float tx1 = tx0 + (float)(cw * 8);
        float tr, tg, tb;
        if (ti == scm_tab) {
            tr = acr; tg = acg; tb = acb;
            /* Active tab underline */
            psolid(tx0, tab_y1_f - 3, tx1, tab_y1_f, acr, acg, acb, 1.0f, vp_w, vp_h);
            /* Active tab background highlight */
            psolid(tx0, tab_y0_f + 1, tx1, tab_y1_f - 3,
                   acr * 0.1f + bgr * 0.8f, acg * 0.1f + bgg * 0.8f, acb * 0.1f + bgb * 0.8f,
                   0.5f, vp_w, vp_h);
        } else {
            tr = dmr; tg = dmg; tb = dmb;
        }
        int tw = (int)strlen(tab_labels[ti]) * cw;
        int ttx = (int)(tx0 + (tx1 - tx0 - tw) / 2);
        ptext(ttx, (int)tab_y0_f + (tab_h - ch) / 2, tab_labels[ti],
              tr, tg, tb, vp_w, vp_h, cw, ch);
    }

    float content_y = tab_y1_f;
    float content_h = y1 - content_y - (float)row_h; /* status bar at bottom */

    /* ---- Status bar ---- */
    float sb_y = y1 - (float)row_h;
    psolid(x0, sb_y, x1, y1, bgr * 0.7f, bgg * 0.7f, bgb * 0.7f, 0.95f, vp_w, vp_h);
    /* Status bar top edge */
    psolid(x0, sb_y, x1, sb_y + 1.0f, 0.0f, 0.0f, 0.0f, 0.12f, vp_w, vp_h);

    char status[128];
    if (scm_tab == 1) {
        _snprintf(status, sizeof(status), "Ln %d Col %d  %s  %d lines",
                  scm_cur_row + 1, scm_cur_col + 1,
                  scm_modified ? "[modified]" : "", scm_line_count);
    } else {
        _snprintf(status, sizeof(status), "%d scripts", scm_script_count);
    }
    ptext((int)x0 + pad, (int)sb_y + (row_h - ch) / 2, status, dmr, dmg, dmb, vp_w, vp_h, cw, ch);

    /* ---- Resize grip (bottom-right corner) ---- */
    if (!scm_fullscreen) {
        for (int i = 0; i < 3; i++) {
            float off = (float)(i * 4 + 2);
            psolid(x1 - off - 1.0f, y1 - 2.0f, x1 - off, y1 - off - 1.0f,
                   dmr, dmg, dmb, 0.4f, vp_w, vp_h);
        }
    }

    /* ==== TAB 0: Script List ==== */
    if (scm_tab == 0) {
        int btn_area_h = btn_h + pad * 2;
        int list_h = (int)content_h - btn_area_h;
        int max_visible = list_h / row_h;

        /* Alternate row backgrounds for readability */
        for (int i = 0; i < max_visible && (i + scm_list_scroll) < scm_script_count; i++) {
            int si = i + scm_list_scroll;
            float ry = content_y + (float)(i * row_h);

            /* Alternating stripe */
            if (i % 2 == 1) {
                psolid(x0 + 2, ry, x1 - 2, ry + (float)row_h,
                       bgr + 0.02f, bgg + 0.02f, bgb + 0.02f, 0.3f, vp_w, vp_h);
            }
            /* Highlight selected */
            if (si == scm_list_sel) {
                psolid(x0 + 2, ry, x1 - 2, ry + (float)row_h,
                       acr * 0.2f, acg * 0.2f, acb * 0.2f, 0.8f, vp_w, vp_h);
            } else if (si == scm_list_hover) {
                psolid(x0 + 2, ry, x1 - 2, ry + (float)row_h,
                       acr * 0.1f, acg * 0.1f, acb * 0.1f, 0.5f, vp_w, vp_h);
            }

            /* Status indicator dot */
            float dot_sz = 10.0f;
            float dot_x = x0 + (float)pad;
            float dot_y = ry + (float)(row_h - (int)dot_sz) / 2;
            if (scm_script_loaded[si]) {
                psolid(dot_x, dot_y, dot_x + dot_sz, dot_y + dot_sz,
                       0.3f, 1.0f, 0.3f, 0.9f, vp_w, vp_h); /* green = loaded */
            } else {
                psolid(dot_x, dot_y, dot_x + dot_sz, dot_y + dot_sz,
                       dmr, dmg, dmb, 0.5f, vp_w, vp_h); /* dim = unloaded */
            }

            if (scm_script_loaded[si]) {
                ptext((int)x0 + pad + (int)dot_sz + 6, (int)ry + (row_h - ch) / 2,
                      scm_script_names[si], 0.3f, 1.0f, 0.3f, vp_w, vp_h, cw, ch);
            } else {
                ptext((int)x0 + pad + (int)dot_sz + 6, (int)ry + (row_h - ch) / 2,
                      scm_script_names[si], dmr, dmg, dmb, vp_w, vp_h, cw, ch);
            }
        }

        /* Buttons at bottom — proper 3D raised */
        float btn_y = content_y + content_h - (float)btn_area_h + (float)pad;
        int disabled = (scm_list_sel < 0 || scm_list_sel >= scm_script_count);
        for (int bi = 0; bi < SCM_BTN_COUNT; bi++) {
            float bx0f, by0f, bx1f, by1f;
            scm_btn_rect(bi, x0, btn_y, btn_w, btn_h, pad, &bx0f, &by0f, &bx1f, &by1f);
            int is_disabled = disabled && bi < 4; /* Refresh always enabled */
            scm_draw_button(bx0f, by0f, bx1f, by1f,
                           scm_btn_labels[bi], scm_btn_hover == bi, is_disabled,
                           bgr, bgg, bgb, acr, acg, acb, txr, txg, txb, dmr, dmg, dmb,
                           cw, ch, pad, vp_w, vp_h, psolid, ptext);
        }
        return;
    }

    /* ==== TAB 1: Code Editor ==== */
    float editor_y = content_y;
    float editor_h = content_h;

    /* Find bar at top if open */
    if (scm_find_open) {
        float fb_y = editor_y;
        float fb_h = (float)row_h + (float)pad;
        psolid(x0, fb_y, x1, fb_y + fb_h, bgr + 0.06f, bgg + 0.06f, bgb + 0.06f, 0.95f, vp_w, vp_h);

        /* Find input */
        ptext((int)x0 + pad, (int)fb_y + (int)(fb_h - ch) / 2, "Find:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        float fi_x = x0 + (float)(pad + cw * 6);
        float fi_w = (float)(cw * 16);
        psolid(fi_x, fb_y + 2, fi_x + fi_w, fb_y + fb_h - 2,
               bgr, bgg, bgb, 0.95f, vp_w, vp_h);
        /* Input border */
        psolid(fi_x, fb_y + 2, fi_x + fi_w, fb_y + 3, 0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        psolid(fi_x, fb_y + 2, fi_x + 1, fb_y + fb_h - 2, 0.0f, 0.0f, 0.0f, 0.10f, vp_w, vp_h);
        ptext((int)fi_x + 2, (int)fb_y + (int)(fb_h - ch) / 2,
              scm_find_text, txr, txg, txb, vp_w, vp_h, cw, ch);

        /* Replace input */
        float ri_x = fi_x + fi_w + (float)pad;
        ptext((int)ri_x, (int)fb_y + (int)(fb_h - ch) / 2, "Repl:", dmr, dmg, dmb, vp_w, vp_h, cw, ch);
        float ri_x2 = ri_x + (float)(cw * 6);
        psolid(ri_x2, fb_y + 2, ri_x2 + fi_w, fb_y + fb_h - 2,
               bgr, bgg, bgb, 0.95f, vp_w, vp_h);
        psolid(ri_x2, fb_y + 2, ri_x2 + fi_w, fb_y + 3, 0.0f, 0.0f, 0.0f, 0.15f, vp_w, vp_h);
        psolid(ri_x2, fb_y + 2, ri_x2 + 1, fb_y + fb_h - 2, 0.0f, 0.0f, 0.0f, 0.10f, vp_w, vp_h);
        ptext((int)ri_x2 + 2, (int)fb_y + (int)(fb_h - ch) / 2,
              scm_repl_text, txr, txg, txb, vp_w, vp_h, cw, ch);

        /* Close [X] */
        ptext((int)x1 - pad - cw, (int)fb_y + (int)(fb_h - ch) / 2,
              "X", 0.9f, 0.4f, 0.4f, vp_w, vp_h, cw, ch);

        editor_y += fb_h;
        editor_h -= fb_h;
    }

    /* Gutter background */
    psolid(x0, editor_y, x0 + (float)gutter_w, editor_y + editor_h,
           bgr * 0.8f, bgg * 0.8f, bgb * 0.8f, 0.95f, vp_w, vp_h);
    /* Gutter right edge */
    psolid(x0 + (float)gutter_w - 1, editor_y, x0 + (float)gutter_w, editor_y + editor_h,
           acr, acg, acb, 0.15f, vp_w, vp_h);

    /* Code area */
    int vis_lines = (int)(editor_h / (float)row_h);
    int vis_cols = ((int)pw - gutter_w - pad) / cw;

    scm_ensure_cursor_visible(vis_lines, vis_cols);

    for (int i = 0; i < vis_lines && (i + scm_scroll_y) < scm_line_count; i++) {
        int li = i + scm_scroll_y;
        float ry = editor_y + (float)(i * row_h);
        int ty = (int)ry + (row_h - ch) / 2;

        /* Line number */
        char lnbuf[8];
        _snprintf(lnbuf, sizeof(lnbuf), "%4d", li + 1);
        ptext((int)x0 + 2, ty, lnbuf,
              SCM_COL_GUTTER_R, SCM_COL_GUTTER_G, SCM_COL_GUTTER_B,
              vp_w, vp_h, cw, ch);

        /* Cursor line highlight */
        if (li == scm_cur_row && scm_focused && !scm_sel_active) {
            psolid(x0 + (float)gutter_w, ry, x1 - 1, ry + (float)row_h,
                   bgr + 0.06f, bgg + 0.06f, bgb + 0.06f, 0.5f, vp_w, vp_h);
        }

        /* Selection highlight */
        if (scm_sel_active) {
            int sr = scm_sel_row, sc = scm_sel_col;
            int er = scm_cur_row, ec = scm_cur_col;
            if (sr > er || (sr == er && sc > ec)) {
                int t_; t_ = sr; sr = er; er = t_; t_ = sc; sc = ec; ec = t_;
            }
            if (li >= sr && li <= er) {
                int ll = scm_line_len(li);
                int c0 = (li == sr) ? sc : 0;
                int c1 = (li == er) ? ec : ll;
                if (c0 < scm_scroll_x) c0 = scm_scroll_x;
                float sx0 = x0 + (float)gutter_w + (float)(c0 - scm_scroll_x) * cw;
                float sx1 = x0 + (float)gutter_w + (float)(c1 - scm_scroll_x) * cw;
                if (sx1 > x1) sx1 = x1;
                if (sx0 < sx1) {
                    psolid(sx0, ry, sx1, ry + (float)row_h,
                           SCM_COL_SEL_R, SCM_COL_SEL_G, SCM_COL_SEL_B, 0.7f, vp_w, vp_h);
                }
            }
        }

        /* Draw text character by character with syntax colors */
        const char *line = scm_lines[li] ? scm_lines[li] : "";
        int line_len = (int)strlen(line);

        for (int c = scm_scroll_x; c < line_len && (c - scm_scroll_x) < vis_cols; c++) {
            int px_x = (int)x0 + gutter_w + (c - scm_scroll_x) * cw;
            if (px_x + cw > (int)x1) break;

            float cr, cg, cb;
            scm_get_char_color(line, c, &cr, &cg, &cb);

            char ch_str[2] = { line[c], '\0' };
            if ((unsigned char)line[c] > 32)
                ptext(px_x, ty, ch_str, cr, cg, cb, vp_w, vp_h, cw, ch);
        }

        /* Cursor */
        if (li == scm_cur_row && scm_focused) {
            int cur_px = (int)x0 + gutter_w + (scm_cur_col - scm_scroll_x) * cw;
            if (cur_px >= (int)x0 + gutter_w && cur_px < (int)x1) {
                psolid((float)cur_px, ry, (float)(cur_px + 2), ry + (float)row_h,
                       SCM_COL_CURSOR_R, SCM_COL_CURSOR_G, SCM_COL_CURSOR_B, 0.9f,
                       vp_w, vp_h);
            }
        }
    }
}

/* ---- Find ---- */

static void scm_find_next_match(void)
{
    if (scm_find_text[0] == '\0') return;
    int flen = (int)strlen(scm_find_text);
    /* Search from current pos forward */
    for (int r = scm_cur_row; r < scm_line_count; r++) {
        int start_col = (r == scm_cur_row) ? scm_cur_col + 1 : 0;
        const char *line = scm_lines[r] ? scm_lines[r] : "";
        const char *found = strstr(line + start_col, scm_find_text);
        if (found) {
            scm_cur_row = r;
            scm_cur_col = (int)(found - line);
            return;
        }
    }
    /* Wrap from top */
    for (int r = 0; r <= scm_cur_row; r++) {
        const char *line = scm_lines[r] ? scm_lines[r] : "";
        const char *found = strstr(line, scm_find_text);
        if (found) {
            scm_cur_row = r;
            scm_cur_col = (int)(found - line);
            return;
        }
    }
}

/* ---- Selection Helpers ---- */

static void scm_sel_delete(void)
{
    if (!scm_sel_active) return;
    /* Normalize: s <= e */
    int sr = scm_sel_row, sc = scm_sel_col;
    int er = scm_cur_row, ec = scm_cur_col;
    if (sr > er || (sr == er && sc > ec)) {
        int t; t = sr; sr = er; er = t; t = sc; sc = ec; ec = t;
    }
    if (sr == er) {
        /* Single line deletion */
        int ll = scm_line_len(sr);
        if (ec > ll) ec = ll;
        memmove(scm_lines[sr] + sc, scm_lines[sr] + ec, ll - ec + 1);
    } else {
        /* Multi-line deletion: keep start of first line + end of last line */
        int ll_end = scm_line_len(er);
        if (ec > ll_end) ec = ll_end;
        int tail_len = ll_end - ec;
        scm_ensure_cap(sr, sc + tail_len + 1);
        memcpy(scm_lines[sr] + sc, scm_lines[er] + ec, tail_len + 1);
        /* Delete lines sr+1 .. er */
        for (int r = sr + 1; r <= er; r++) {
            if (scm_lines[r]) free(scm_lines[r]);
        }
        int del_count = er - sr;
        for (int r = sr + 1; r < scm_line_count - del_count; r++) {
            scm_lines[r] = scm_lines[r + del_count];
            scm_line_cap[r] = scm_line_cap[r + del_count];
        }
        scm_line_count -= del_count;
    }
    scm_cur_row = sr; scm_cur_col = sc;
    scm_sel_active = 0;
    scm_modified = 1;
}

static char *scm_sel_get_text(int *out_len)
{
    if (!scm_sel_active) { *out_len = 0; return NULL; }
    int sr = scm_sel_row, sc = scm_sel_col;
    int er = scm_cur_row, ec = scm_cur_col;
    if (sr > er || (sr == er && sc > ec)) {
        int t; t = sr; sr = er; er = t; t = sc; sc = ec; ec = t;
    }
    /* Calculate size */
    int total = 0;
    for (int r = sr; r <= er; r++) {
        int ll = scm_line_len(r);
        int c0 = (r == sr) ? sc : 0;
        int c1 = (r == er) ? ec : ll;
        if (c0 > ll) c0 = ll;
        if (c1 > ll) c1 = ll;
        total += (c1 - c0);
        if (r < er) total += 1; /* newline */
    }
    char *buf = (char *)malloc(total + 1);
    if (!buf) { *out_len = 0; return NULL; }
    int pos = 0;
    for (int r = sr; r <= er; r++) {
        int ll = scm_line_len(r);
        int c0 = (r == sr) ? sc : 0;
        int c1 = (r == er) ? ec : ll;
        if (c0 > ll) c0 = ll;
        if (c1 > ll) c1 = ll;
        int n = c1 - c0;
        if (n > 0) { memcpy(buf + pos, scm_lines[r] + c0, n); pos += n; }
        if (r < er) buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    *out_len = pos;
    return buf;
}

static void scm_copy_selection(void)
{
    int len = 0;
    char *text = scm_sel_get_text(&len);
    if (text && len > 0) clipboard_copy(text, len);
    if (text) free(text);
}

static void scm_cut_selection(void)
{
    scm_copy_selection();
    scm_sel_delete();
}

static void scm_select_all(void)
{
    scm_sel_active = 1;
    scm_sel_row = 0;
    scm_sel_col = 0;
    scm_cur_row = scm_line_count - 1;
    scm_cur_col = scm_line_len(scm_cur_row);
}

/* ---- Mouse Handling ---- */

static int scm_mouse_down(int mx, int my)
{
    if (!scm_visible) return 0;
    float x0 = scm_fullscreen ? 0 : scm_x;
    float y0 = scm_fullscreen ? 0 : scm_y;
    float pw = scm_fullscreen ? (float)vk_sc_extent.width : scm_w;
    float ph = scm_fullscreen ? (float)vk_sc_extent.height : scm_h;

    if (mx < (int)x0 || mx >= (int)(x0 + pw) || my < (int)y0 || my >= (int)(y0 + ph))
        return 0;

    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);
    int titlebar_h = ch + (int)(10 * ui_scale);
    int tab_h = ch + (int)(8 * ui_scale);
    int row_h = ch + (int)(4 * ui_scale);
    int pad = (int)(8 * ui_scale);
    int gutter_w = cw * 5;
    int btn_h_loc = ch + (int)(10 * ui_scale);
    int btn_w_loc = cw * 8 + pad;

    float x1 = x0 + pw;
    float tb_y1 = y0 + (float)titlebar_h;
    float tab_y1_f = tb_y1 + (float)tab_h;

    /* Resize grip — check first (bottom-right corner) */
    if (!scm_fullscreen) {
        int lx = mx - (int)x0, ly = my - (int)y0;
        if (lx >= (int)pw - SCM_RESIZE_GRIP && ly >= (int)ph - SCM_RESIZE_GRIP) {
            scm_resizing = 1;
            scm_drag_ox = (float)mx;
            scm_drag_oy = (float)my;
            return 1;
        }
    }

    /* Close button (top-right, small 3D button) */
    {
        float cbx1 = x1 - 4.0f, cbx0 = cbx1 - (float)(cw * 2 + 6);
        if (my >= (int)y0 + 2 && my < (int)tb_y1 - 3 && mx >= (int)cbx0 && mx < (int)cbx1) {
            scm_visible = 0;
            scm_focused = 0;
            return 1;
        }
    }

    /* Fullscreen toggle button */
    {
        float fbx1 = x1 - 4.0f - (float)(cw * 2 + 6) - 4.0f;
        float fbx0 = fbx1 - (float)(cw * 2 + 6);
        if (my >= (int)y0 + 2 && my < (int)tb_y1 - 3 && mx >= (int)fbx0 && mx < (int)fbx1) {
            scm_fullscreen = !scm_fullscreen;
            if (scm_fullscreen) {
                scm_restore_x = scm_x; scm_restore_y = scm_y;
                scm_restore_w = scm_w; scm_restore_h = scm_h;
            } else {
                scm_x = scm_restore_x; scm_y = scm_restore_y;
                scm_w = scm_restore_w; scm_h = scm_restore_h;
            }
            return 1;
        }
    }

    /* Title bar drag */
    if (my < (int)tb_y1 && !scm_fullscreen) {
        scm_dragging = 1;
        scm_drag_ox = (float)mx - scm_x;
        scm_drag_oy = (float)my - scm_y;
        return 1;
    }

    /* Tab clicks */
    if (my >= (int)tb_y1 && my < (int)tab_y1_f) {
        int ti = (mx - (int)x0 - pad) / (cw * 9);
        if (ti >= 0 && ti < 2) {
            scm_tab = ti;
            scm_focused = (ti == 1);
            if (ti == 0) scm_scan_scripts();
        }
        return 1;
    }

    float content_y = tab_y1_f;
    float content_h = (y0 + ph) - content_y - (float)row_h;

    /* Script list tab */
    if (scm_tab == 0) {
        int btn_area_h = btn_h_loc + pad * 2;
        int list_h = (int)content_h - btn_area_h;
        float btn_y = content_y + content_h - (float)btn_area_h + (float)pad;

        /* List click */
        if (my >= (int)content_y && my < (int)content_y + list_h) {
            int idx = (my - (int)content_y) / row_h + scm_list_scroll;
            if (idx >= 0 && idx < scm_script_count) {
                scm_list_sel = idx;
            }
            return 1;
        }

        /* Button clicks — use same rect calculation as draw */
        if (my >= (int)btn_y && my < (int)btn_y + btn_h_loc) {
            for (int bi = 0; bi < SCM_BTN_COUNT; bi++) {
                float bx0f, by0f, bx1f, by1f;
                scm_btn_rect(bi, x0, btn_y, btn_w_loc, btn_h_loc, pad, &bx0f, &by0f, &bx1f, &by1f);
                if (mx >= (int)bx0f && mx < (int)bx1f) {
                    /* Refresh always works; others need selection */
                    if (bi == 4) { scm_scan_scripts(); return 1; }
                    if (scm_list_sel < 0 || scm_list_sel >= scm_script_count) return 1;
                    char cmd[256];
                    const char *name = scm_script_names[scm_list_sel];
                    if (bi == 0) { /* Load — skip if already loaded */
                        if (!scm_script_loaded[scm_list_sel]) {
                            _snprintf(cmd, sizeof(cmd), "mud.load('%s')", name);
                            vkt_eval_python(cmd, 0);
                            scm_script_loaded[scm_list_sel] = 1;
                        }
                    } else if (bi == 1) { /* Unload — skip if not loaded */
                        if (scm_script_loaded[scm_list_sel]) {
                            _snprintf(cmd, sizeof(cmd), "mud.stop('%s')", name);
                            vkt_eval_python(cmd, 0);
                            scm_script_loaded[scm_list_sel] = 0;
                        }
                    } else if (bi == 2) { /* Edit */
                        scm_open_script_in_editor(name);
                    } else if (bi == 3) { /* Delete */
                        _snprintf(cmd, sizeof(cmd), "mud.delete('%s')", name);
                        vkt_eval_python(cmd, 0);
                        scm_scan_scripts();
                        scm_list_sel = -1;
                    }
                    return 1;
                }
            }
            return 1;
        }
        return 1;
    }

    /* Editor tab — click in code area positions cursor */
    if (scm_tab == 1) {
        scm_focused = 1;

        float editor_y = content_y;
        if (scm_find_open) editor_y += (float)(row_h + pad);

        /* Find bar close button */
        if (scm_find_open && my >= (int)content_y && my < (int)editor_y) {
            if (mx > (int)(x0 + pw) - pad - cw * 2) {
                scm_find_open = 0;
                return 1;
            }
            float fi_x = x0 + (float)(pad + cw * 6);
            float fi_w = (float)(cw * 16);
            float ri_x = fi_x + fi_w + (float)pad + (float)(cw * 6);
            if (mx >= (int)fi_x && mx < (int)(fi_x + fi_w)) scm_find_focused = 0;
            else if (mx >= (int)ri_x && mx < (int)(ri_x + fi_w)) scm_find_focused = 1;
            return 1;
        }

        if (my >= (int)editor_y && mx >= (int)x0 + gutter_w) {
            int clicked_row = (my - (int)editor_y) / row_h + scm_scroll_y;
            int clicked_col = (mx - (int)x0 - gutter_w) / cw + scm_scroll_x;
            if (clicked_row >= scm_line_count) clicked_row = scm_line_count - 1;
            if (clicked_row < 0) clicked_row = 0;
            int ll = scm_line_len(clicked_row);
            if (clicked_col > ll) clicked_col = ll;
            if (clicked_col < 0) clicked_col = 0;

            /* Start selection on click (drag will extend it) */
            scm_sel_active = 1;
            scm_sel_row = clicked_row;
            scm_sel_col = clicked_col;
            scm_cur_row = clicked_row;
            scm_cur_col = clicked_col;
        }
        return 1;
    }

    return 1;
}

static void scm_mouse_move(int mx, int my)
{
    if (scm_dragging) {
        scm_x = (float)mx - scm_drag_ox;
        scm_y = (float)my - scm_drag_oy;
        return;
    }
    if (scm_resizing) {
        float dx = (float)mx - scm_drag_ox;
        float dy = (float)my - scm_drag_oy;
        scm_w += dx;
        scm_h += dy;
        if (scm_w < SCM_MIN_W) scm_w = SCM_MIN_W;
        if (scm_h < SCM_MIN_H) scm_h = SCM_MIN_H;
        scm_drag_ox = (float)mx;
        scm_drag_oy = (float)my;
        return;
    }

    if (!scm_visible) return;

    float x0 = scm_fullscreen ? 0 : scm_x;
    float y0 = scm_fullscreen ? 0 : scm_y;
    float pw = scm_fullscreen ? (float)vk_sc_extent.width : scm_w;
    int cw = (int)(VSB_CHAR_W * ui_scale), ch = (int)(VSB_CHAR_H * ui_scale);
    int titlebar_h = ch + (int)(10 * ui_scale);
    int tab_h = ch + (int)(8 * ui_scale);
    int row_h = ch + (int)(4 * ui_scale);
    int pad = (int)(8 * ui_scale);
    int gutter_w = cw * 5;
    int btn_h_loc = ch + (int)(10 * ui_scale);
    int btn_w_loc = cw * 8 + pad;

    if (scm_tab == 0) {
        /* Button hover tracking */
        float content_y = y0 + (float)titlebar_h + (float)tab_h;
        float content_h = (y0 + (scm_fullscreen ? (float)vk_sc_extent.height : scm_h)) - content_y - (float)row_h;
        int btn_area_h = btn_h_loc + pad * 2;
        float btn_y = content_y + content_h - (float)btn_area_h + (float)pad;

        scm_btn_hover = -1;
        if (my >= (int)btn_y && my < (int)btn_y + btn_h_loc) {
            for (int bi = 0; bi < SCM_BTN_COUNT; bi++) {
                float bx0f, by0f, bx1f, by1f;
                scm_btn_rect(bi, x0, btn_y, btn_w_loc, btn_h_loc, pad, &bx0f, &by0f, &bx1f, &by1f);
                if (mx >= (int)bx0f && mx < (int)bx1f) { scm_btn_hover = bi; break; }
            }
        }

        /* List hover */
        int idx = (my - (int)content_y) / row_h + scm_list_scroll;
        scm_list_hover = (idx >= 0 && idx < scm_script_count) ? idx : -1;
    }

    /* Editor: drag-select */
    if (scm_tab == 1 && scm_sel_active && (GetKeyState(VK_LBUTTON) & 0x8000)) {
        float editor_y = y0 + (float)titlebar_h + (float)tab_h;
        if (scm_find_open) editor_y += (float)(row_h + pad);

        if (my >= (int)editor_y && mx >= (int)x0 + gutter_w) {
            int drag_row = (my - (int)editor_y) / row_h + scm_scroll_y;
            int drag_col = (mx - (int)x0 - gutter_w) / cw + scm_scroll_x;
            if (drag_row >= scm_line_count) drag_row = scm_line_count - 1;
            if (drag_row < 0) drag_row = 0;
            int ll = scm_line_len(drag_row);
            if (drag_col > ll) drag_col = ll;
            if (drag_col < 0) drag_col = 0;
            scm_cur_row = drag_row;
            scm_cur_col = drag_col;
        }
    }
}

static void scm_mouse_up(void)
{
    scm_dragging = 0;
    scm_resizing = 0;
    /* If selection start == cursor, cancel selection */
    if (scm_sel_active && scm_sel_row == scm_cur_row && scm_sel_col == scm_cur_col)
        scm_sel_active = 0;
}

static int scm_scroll_wheel(int mx, int my, int delta)
{
    if (!scm_visible) return 0;
    float x0 = scm_fullscreen ? 0 : scm_x;
    float y0 = scm_fullscreen ? 0 : scm_y;
    float pw = scm_fullscreen ? (float)vk_sc_extent.width : scm_w;
    float ph = scm_fullscreen ? (float)vk_sc_extent.height : scm_h;

    if (mx < (int)x0 || mx >= (int)(x0 + pw) || my < (int)y0 || my >= (int)(y0 + ph))
        return 0;

    if (scm_tab == 0) {
        scm_list_scroll += (delta > 0) ? -2 : 2;
        if (scm_list_scroll < 0) scm_list_scroll = 0;
        if (scm_list_scroll > scm_script_count - 5) scm_list_scroll = scm_script_count - 5;
        if (scm_list_scroll < 0) scm_list_scroll = 0;
    } else {
        scm_scroll_y += (delta > 0) ? -3 : 3;
        if (scm_scroll_y < 0) scm_scroll_y = 0;
        if (scm_scroll_y > scm_line_count - 5) scm_scroll_y = scm_line_count - 5;
        if (scm_scroll_y < 0) scm_scroll_y = 0;
    }
    return 1;
}

/* ---- Clipboard Paste ---- */

static void scm_paste(void)
{
    if (!OpenClipboard(NULL)) return;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        const char *text = (const char *)GlobalLock(h);
        if (text) {
            for (int i = 0; text[i]; i++) {
                if (text[i] == '\r') continue; /* skip CR */
                if (text[i] == '\n') {
                    scm_insert_newline();
                } else if (text[i] == '\t') {
                    scm_insert_tab();
                } else if ((unsigned char)text[i] >= 32) {
                    scm_insert_char(text[i]);
                }
            }
            scm_modified = 1;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

/* ---- Key Handling ---- */

static int scm_key_down(unsigned int vk)
{
    if (!scm_visible || !scm_focused) return 0;

    int ctrl = GetKeyState(VK_CONTROL) & 0x8000;

    /* Find bar shortcuts */
    if (ctrl && vk == 'F') { scm_find_open = !scm_find_open; return 1; }
    if (ctrl && vk == 'H') { scm_find_open = 1; scm_find_focused = 1; return 1; }
    if (ctrl && vk == 'S') { scm_save_file(); return 1; }
    if (ctrl && vk == 'V') {
        if (scm_sel_active) scm_sel_delete();
        scm_paste(); return 1;
    }
    if (ctrl && vk == 'C') { scm_copy_selection(); return 1; }
    if (ctrl && vk == 'X') { scm_cut_selection(); return 1; }
    if (ctrl && vk == 'A') { scm_select_all(); return 1; }
    if (vk == VK_F11) { scm_fullscreen = !scm_fullscreen; return 1; }
    if (vk == VK_ESCAPE) {
        if (scm_find_open) { scm_find_open = 0; return 1; }
        scm_focused = 0;
        return 1;
    }

    /* Find bar input */
    if (scm_find_open) {
        if (vk == VK_RETURN) { scm_find_next_match(); return 1; }
        if (vk == VK_TAB) { scm_find_focused = !scm_find_focused; return 1; }
    }

    /* Navigation keys — Shift extends selection */
    int shift = GetKeyState(VK_SHIFT) & 0x8000;

    #define SCM_SEL_BEGIN() do { if (shift && !scm_sel_active) { \
        scm_sel_active = 1; scm_sel_row = scm_cur_row; scm_sel_col = scm_cur_col; } \
        if (!shift) scm_sel_active = 0; } while(0)

    if (vk == VK_UP) {
        SCM_SEL_BEGIN();
        if (scm_cur_row > 0) {
            scm_cur_row--;
            int ll = scm_line_len(scm_cur_row);
            if (scm_cur_col > ll) scm_cur_col = ll;
        }
        return 1;
    }
    if (vk == VK_DOWN) {
        SCM_SEL_BEGIN();
        if (scm_cur_row < scm_line_count - 1) {
            scm_cur_row++;
            int ll = scm_line_len(scm_cur_row);
            if (scm_cur_col > ll) scm_cur_col = ll;
        }
        return 1;
    }
    if (vk == VK_LEFT) {
        SCM_SEL_BEGIN();
        if (scm_cur_col > 0) scm_cur_col--;
        else if (scm_cur_row > 0) { scm_cur_row--; scm_cur_col = scm_line_len(scm_cur_row); }
        return 1;
    }
    if (vk == VK_RIGHT) {
        SCM_SEL_BEGIN();
        int ll = scm_line_len(scm_cur_row);
        if (scm_cur_col < ll) scm_cur_col++;
        else if (scm_cur_row < scm_line_count - 1) { scm_cur_row++; scm_cur_col = 0; }
        return 1;
    }
    if (vk == VK_HOME) {
        SCM_SEL_BEGIN();
        int indent = 0;
        const char *line = scm_lines[scm_cur_row];
        while (line && (line[indent] == ' ' || line[indent] == '\t')) indent++;
        scm_cur_col = (scm_cur_col == indent) ? 0 : indent;
        return 1;
    }
    if (vk == VK_END) {
        SCM_SEL_BEGIN();
        scm_cur_col = scm_line_len(scm_cur_row);
        return 1;
    }
    if (vk == VK_PRIOR) { /* Page Up */
        SCM_SEL_BEGIN();
        scm_cur_row -= 20;
        if (scm_cur_row < 0) scm_cur_row = 0;
        int ll = scm_line_len(scm_cur_row);
        if (scm_cur_col > ll) scm_cur_col = ll;
        return 1;
    }
    if (vk == VK_NEXT) { /* Page Down */
        SCM_SEL_BEGIN();
        scm_cur_row += 20;
        if (scm_cur_row >= scm_line_count) scm_cur_row = scm_line_count - 1;
        int ll = scm_line_len(scm_cur_row);
        if (scm_cur_col > ll) scm_cur_col = ll;
        return 1;
    }
    if (vk == VK_DELETE) {
        if (scm_sel_active) { scm_sel_delete(); return 1; }
        scm_delete_char();
        return 1;
    }

    #undef SCM_SEL_BEGIN

    return 0;
}

static int scm_key_char(unsigned int ch)
{
    if (!scm_visible || !scm_focused) return 0;

    /* Find bar input */
    if (scm_find_open && (scm_find_focused == 0 || scm_find_focused == 1)) {
        char *target = scm_find_focused ? scm_repl_text : scm_find_text;
        int *cursor = scm_find_focused ? &scm_repl_cursor : &scm_find_cursor;
        int maxlen = 126;

        if (ch == '\b') {
            int len = (int)strlen(target);
            if (len > 0) target[len - 1] = '\0';
            return 1;
        }
        if (ch == '\r') { scm_find_next_match(); return 1; }
        if (ch == 27) { scm_find_open = 0; return 1; }
        if (ch >= 32 && ch < 127) {
            int len = (int)strlen(target);
            if (len < maxlen) {
                target[len] = (char)ch;
                target[len + 1] = '\0';
            }
            return 1;
        }
        return 1;
    }

    /* Editor input */
    if (scm_tab != 1) return 0;

    if (ch == '\r') {
        if (scm_sel_active) scm_sel_delete();
        scm_insert_newline(); return 1;
    }
    if (ch == '\b') {
        if (scm_sel_active) { scm_sel_delete(); return 1; }
        scm_backspace(); return 1;
    }
    if (ch == '\t') { scm_insert_tab(); return 1; }
    if (ch == 27) { scm_focused = 0; return 1; }
    if (ch >= 32 && ch < 127) {
        if (scm_sel_active) scm_sel_delete();
        scm_insert_char((char)ch);
        return 1;
    }
    return 0;
}

/* Update the status bar scripts string from loaded state */
static void scm_update_vsb_scripts(void)
{
    int slen = 0, any = 0;
    for (int si = 0; si < scm_script_count && slen < 240; si++) {
        if (scm_script_loaded[si]) {
            if (any) { vsb_scripts_str[slen++] = ','; vsb_scripts_str[slen++] = ' '; }
            int nlen = (int)strlen(scm_script_names[si]);
            if (slen + nlen < 240) {
                memcpy(vsb_scripts_str + slen, scm_script_names[si], nlen);
                slen += nlen;
            }
            any = 1;
        }
    }
    if (any) vsb_scripts_str[slen] = '\0';
    else     strcpy(vsb_scripts_str, "None");
}
