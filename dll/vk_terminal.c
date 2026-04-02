/*
 * vk_terminal.c — Vulkan Fullscreen Terminal Plugin for MajorSLOP
 * ================================================================
 *
 * Renders MMANSI terminal buffer using Vulkan. F11 toggles fullscreen.
 * Pixel-perfect CP437 bitmap font, DOS color palette, input bar with
 * command history. Writes buffer to /tmp/mmansi_buf for external tools.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o vk_terminal.dll vk_terminal.c \
 *       -I. -L/usr/lib/wine/i386-windows -lvulkan-1 \
 *       -lgdi32 -luser32 -mwindows -O2
 */

#define VK_USE_PLATFORM_WIN32_KHR
#include "vulkan_headers/vulkan.h"
#include "slop_plugin.h"
#include "cp437_hd.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Proper byte-by-byte ANSI/VT100 state machine parser */
#define AP_ROWS 25
#define AP_COLS 80
#define ANSI_PARSER_IMPLEMENTATION
#include "ansi_parser.h"

/* ---- Terminal constants ---- */

#define TERM_ROWS       25
#define TERM_COLS       80

#define INPUT_BAR_H     40      /* pixels for input bar */
#define MAX_HISTORY     64
#define INPUT_BUF_SZ    256

#define IDM_VKT_TOGGLE  51000
#define IDM_VKT_RES_BASE 51010  /* 51010..51019 for resolutions */
#define IDM_VKT_THEME_BASE 51020
#define IDM_VKT_SETTINGS 51030

/* Max quads: 60*132 bg + 60*132 text + input bar ≈ 16000 */
#define MAX_QUADS       17000
#define VERTS_PER_QUAD  4
#define IDXS_PER_QUAD   6

/* ---- Vertex format ---- */

typedef struct {
    float x, y;     /* position in NDC */
    float u, v;     /* texture UV */
    float r, g, b;  /* color */
    float a;        /* alpha (1.0 for bg, texture alpha for text) */
} vertex_t;

/* ---- Color palettes ---- */

typedef struct { float r, g, b; } rgb_t;

static rgb_t pal_classic[16] = {
    /* ANSI order: SGR 30-37 maps directly to indices 0-7 */
    {0.00f,0.00f,0.00f},  /*  0 black   */
    {0.67f,0.00f,0.00f},  /*  1 red     */
    {0.00f,0.67f,0.00f},  /*  2 green   */
    {0.67f,0.33f,0.00f},  /*  3 brown   */
    {0.00f,0.00f,0.67f},  /*  4 blue    */
    {0.67f,0.00f,0.67f},  /*  5 magenta */
    {0.00f,0.67f,0.67f},  /*  6 cyan    */
    {0.67f,0.67f,0.67f},  /*  7 light gray */
    {0.33f,0.33f,0.33f},  /*  8 dark gray  */
    {1.00f,0.33f,0.33f},  /*  9 light red  */
    {0.33f,1.00f,0.33f},  /* 10 light green */
    {1.00f,1.00f,0.33f},  /* 11 yellow     */
    {0.33f,0.33f,1.00f},  /* 12 light blue */
    {1.00f,0.33f,1.00f},  /* 13 light magenta */
    {0.33f,1.00f,1.00f},  /* 14 light cyan */
    {1.00f,1.00f,1.00f},  /* 15 bright white */
};
static rgb_t pal_dracula[16] = {
    {0.16f,0.16f,0.21f}, {0.38f,0.45f,0.94f}, {0.31f,0.98f,0.48f}, {0.55f,0.91f,0.99f},
    {1.00f,0.33f,0.40f}, {0.74f,0.47f,0.95f}, {0.95f,0.71f,0.24f}, {0.73f,0.75f,0.82f},
    {0.38f,0.40f,0.50f}, {0.51f,0.59f,0.97f}, {0.44f,0.99f,0.58f}, {0.65f,0.94f,0.99f},
    {1.00f,0.47f,0.53f}, {0.84f,0.60f,0.98f}, {0.98f,0.80f,0.40f}, {0.97f,0.97f,0.95f},
};
static rgb_t pal_solarized[16] = {
    {0.00f,0.17f,0.21f}, {0.15f,0.55f,0.82f}, {0.52f,0.60f,0.00f}, {0.16f,0.63f,0.60f},
    {0.86f,0.20f,0.18f}, {0.83f,0.21f,0.51f}, {0.71f,0.54f,0.00f}, {0.58f,0.63f,0.63f},
    {0.40f,0.48f,0.51f}, {0.26f,0.65f,0.90f}, {0.63f,0.72f,0.14f}, {0.29f,0.74f,0.73f},
    {0.93f,0.33f,0.31f}, {0.91f,0.34f,0.60f}, {0.80f,0.67f,0.14f}, {0.93f,0.91f,0.84f},
};
static rgb_t pal_amber[16] = {
    {0.05f,0.03f,0.00f}, {0.40f,0.25f,0.00f}, {0.60f,0.40f,0.00f}, {0.50f,0.35f,0.00f},
    {0.70f,0.30f,0.00f}, {0.55f,0.25f,0.00f}, {0.80f,0.55f,0.00f}, {0.85f,0.60f,0.00f},
    {0.45f,0.30f,0.00f}, {0.50f,0.35f,0.00f}, {0.75f,0.55f,0.00f}, {0.65f,0.48f,0.00f},
    {0.90f,0.45f,0.00f}, {0.70f,0.40f,0.00f}, {1.00f,0.75f,0.00f}, {1.00f,0.80f,0.10f},
};
static rgb_t pal_green[16] = {
    {0.00f,0.04f,0.00f}, {0.00f,0.25f,0.00f}, {0.00f,0.50f,0.00f}, {0.00f,0.40f,0.10f},
    {0.10f,0.30f,0.00f}, {0.05f,0.25f,0.05f}, {0.00f,0.55f,0.00f}, {0.00f,0.65f,0.00f},
    {0.00f,0.30f,0.00f}, {0.00f,0.35f,0.05f}, {0.00f,0.75f,0.00f}, {0.00f,0.60f,0.10f},
    {0.10f,0.45f,0.00f}, {0.05f,0.40f,0.05f}, {0.10f,0.85f,0.00f}, {0.15f,1.00f,0.10f},
};

#define NUM_THEMES 5
static const char *theme_names[NUM_THEMES] = {
    "Classic VGA", "Dracula", "Solarized Dark", "Amber CRT", "Green Phosphor"
};
static rgb_t *theme_palettes[NUM_THEMES] = {
    pal_classic, pal_dracula, pal_solarized, pal_amber, pal_green
};

/* ---- Resolution presets ---- */

typedef struct { int w, h; const char *name; } res_t;
static res_t resolutions[] = {
    { 1920, 1080, "1920x1080 (Full HD)" },
    { 1600,  900, "1600x900" },
    { 1366,  768, "1366x768" },
    { 1280,  720, "1280x720 (HD)" },
    { 1024,  768, "1024x768" },
};
#define NUM_RES (sizeof(resolutions)/sizeof(resolutions[0]))

/* ---- Embedded SPIR-V shaders ---- */

#include "shaders/term_vert.h"
#include "shaders/term_frag.h"

/* ---- Plugin state ---- */

static const slop_api_t *api = NULL;
static HWND mmansi_hwnd = NULL;
static HWND mmmain_hwnd = NULL;
static HWND vkt_hwnd = NULL;
static HANDLE vkt_thread_handle = NULL;
static volatile int vkt_running = 0;
static volatile int vkt_visible = 0;
static volatile int vkt_screenshot_pending = 0;
static uint32_t vkt_screenshot_img_idx = 0;

/* Settings */
static int current_theme = 0;
static rgb_t *palette = NULL;
static int fs_res_idx = 0;     /* fullscreen resolution index */
static int fs_width = 1920;
static int fs_height = 1080;

/* Terminal buffer */
/* ANSI terminal state machine — replaces old term_text/term_attr arrays */
static ap_term_t ansi_term;

/* Input state */
static char input_buf[INPUT_BUF_SZ];
static int input_len = 0;
static int input_cursor = 0;
static char *cmd_history[MAX_HISTORY];
static int history_count = 0;
static int history_idx = 0;
static int input_mode = 0;  /* 0 = split, 1 = raw */

/* Frame counter for cursor blink */
static int frame_count = 0;

/* ---- Vulkan state ---- */

static VkInstance vk_inst = VK_NULL_HANDLE;
static VkPhysicalDevice vk_pdev = VK_NULL_HANDLE;
static VkDevice vk_dev = VK_NULL_HANDLE;
static VkQueue vk_queue = VK_NULL_HANDLE;
static uint32_t vk_qfam = 0;

static VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
static VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;
static VkFormat vk_sc_fmt;
static VkExtent2D vk_sc_extent;
static uint32_t vk_sc_count = 0;
static VkImage *vk_sc_images = NULL;
static VkImageView *vk_sc_views = NULL;
static VkFramebuffer *vk_sc_fbs = NULL;

static VkRenderPass vk_renderpass = VK_NULL_HANDLE;
static VkPipelineLayout vk_pipe_layout = VK_NULL_HANDLE;
static VkPipeline vk_pipeline = VK_NULL_HANDLE;
static VkDescriptorSetLayout vk_desc_layout = VK_NULL_HANDLE;
static VkDescriptorPool vk_desc_pool = VK_NULL_HANDLE;
static VkDescriptorSet vk_desc_set = VK_NULL_HANDLE;

static VkCommandPool vk_cmd_pool = VK_NULL_HANDLE;
static VkCommandBuffer vk_cmd_buf = VK_NULL_HANDLE;
static VkSemaphore vk_sem_avail = VK_NULL_HANDLE;
static VkSemaphore vk_sem_done = VK_NULL_HANDLE;
static VkFence vk_fence = VK_NULL_HANDLE;

/* Font texture */
static VkImage vk_font_img = VK_NULL_HANDLE;
static VkDeviceMemory vk_font_mem = VK_NULL_HANDLE;
static VkImageView vk_font_view = VK_NULL_HANDLE;
static VkSampler vk_font_sampler = VK_NULL_HANDLE;

/* Vertex + index buffers */
static VkBuffer vk_vbuf = VK_NULL_HANDLE;
static VkDeviceMemory vk_vmem = VK_NULL_HANDLE;
static VkBuffer vk_ibuf = VK_NULL_HANDLE;
static VkDeviceMemory vk_imem = VK_NULL_HANDLE;
static vertex_t *vk_vdata = NULL;  /* persistently mapped */
static int quad_count = 0;

/* ---- Forward declarations ---- */

static void vkt_destroy_swapchain(void);
static int  vkt_create_swapchain(void);
static void vkt_cleanup_vulkan(void);
void vkt_show(void);
void vkt_hide(void);
void vkt_toggle(void);
static void vkt_screenshot(uint32_t img_idx);

/* ---- Helpers ---- */

static uint32_t vk_find_memory(VkPhysicalDevice pd, uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

/* ---- ANSI Terminal — uses ansi_parser.h state machine ---- */

static CRITICAL_SECTION ansi_lock;

/* on_data callback — raw server bytes fed to state machine */
static void vkt_on_data(const char *data, int len)
{
    EnterCriticalSection(&ansi_lock);
    ap_feed(&ansi_term, (const uint8_t *)data, len);
    LeaveCriticalSection(&ansi_lock);
}

/* Read buffer for renderer — snapshot under lock */
static void vkt_read_buffer(void)
{
    EnterCriticalSection(&ansi_lock);
    LeaveCriticalSection(&ansi_lock);
}

/* ---- Vertex building ---- */

static void push_quad(float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1,
                      float r, float g, float b, float a)
{
    if (quad_count >= MAX_QUADS) return;
    int vi = quad_count * 4;
    /* NDC: x in [-1,1], y in [-1,1] */
    vk_vdata[vi+0] = (vertex_t){ x0, y0, u0, v0, r, g, b, a };
    vk_vdata[vi+1] = (vertex_t){ x1, y0, u1, v0, r, g, b, a };
    vk_vdata[vi+2] = (vertex_t){ x1, y1, u1, v1, r, g, b, a };
    vk_vdata[vi+3] = (vertex_t){ x0, y1, u0, v1, r, g, b, a };
    quad_count++;
}

static void vkt_build_vertices(void)
{
    quad_count = 0;
    if (!palette || !vk_vdata) return;

    int vp_w = (int)vk_sc_extent.width;
    int vp_h = (int)vk_sc_extent.height;
    int top_pad = 4;
    int bot_pad = 4;
    int term_h = vp_h - INPUT_BAR_H - top_pad - bot_pad;

    /* Maintain 1:2 (w:h) CP437 aspect ratio per character.
     * Scale uniformly to fit, center horizontally with black bars. */
    float cw_fill = (float)vp_w / (float)TERM_COLS;
    float ch_fill = (float)term_h / (float)TERM_ROWS;
    float cw, ch;
    /* Pick scale that maintains 1:2 aspect and fits */
    if (cw_fill * 2.0f <= ch_fill) {
        /* Width-limited: chars fit by width, won't overflow height */
        cw = cw_fill;
        ch = cw * 2.0f;
    } else {
        /* Height-limited: chars fit by height */
        ch = ch_fill;
        cw = ch / 2.0f;
    }
    float grid_w = cw * TERM_COLS;
    float x_offset = ((float)vp_w - grid_w) / 2.0f; /* center horizontally */

    /* UV constants for font atlas (512x1024, 32x64 glyphs in 16x16 grid) */
    float tex_cw = 1.0f / 16.0f;
    float tex_ch = 1.0f / 16.0f;

    /* Helper: pixel to NDC */
    #define PX2NDC_X(px) (((float)(px) / (float)vp_w) * 2.0f - 1.0f)
    #define PX2NDC_Y(py) (((float)(py) / (float)vp_h) * 2.0f - 1.0f)

    /* Pass 1: background colors */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            int bg = ansi_term.grid[r][c].attr.bg & 0x07;
            if (bg == 0) continue;
            float px0 = c * cw, py0 = r * ch;
            float px1 = px0 + cw, py1 = py0 + ch;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      0, 0, 0, 0, /* no texture for bg */
                      palette[bg].r, palette[bg].g, palette[bg].b, 0.0f);
            /* alpha=0 so fragment shader: texAlpha=texture(0,0).a, output alpha=0*texAlpha=0
               Hmm, that won't work. Need a different approach for bg vs text. */
        }
    }

    /* Actually, for bg quads we need alpha=1 and a white pixel in the atlas.
       The fragment shader does: outColor = vec4(color.rgb, color.a * texAlpha).
       For bg quads, if we use UV (0,0) which is glyph 0 (null) — its alpha is 0.

       Better approach: use a special region of the atlas that's solid white.
       Or: modify the shader to handle this differently.

       Simplest: glyph 219 (█ full block) has all bits set → all alpha=1.
       Use its UVs for background quads. */

    /* Re-do: reset and use glyph 219 UVs for bg */
    quad_count = 0;
    float bg_u0 = (219 % 16) * tex_cw;
    float bg_v0 = (219 / 16) * tex_ch;
    float bg_u1 = bg_u0 + tex_cw;
    float bg_v1 = bg_v0 + tex_ch;

    /* Pass 1: backgrounds using solid glyph */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            int bg = ansi_term.grid[r][c].attr.bg & 0x07;
            if (bg == 0) continue;
            float px0 = x_offset + c * cw, py0 = top_pad + r * ch;
            float px1 = px0 + cw, py1 = py0 + ch;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      palette[bg].r, palette[bg].g, palette[bg].b, 1.0f);
        }
    }

    /* Pass 2: text characters */
    for (int r = 0; r < TERM_ROWS; r++) {
        for (int c = 0; c < TERM_COLS; c++) {
            unsigned char byte = ansi_term.grid[r][c].ch;
            if (byte == 0 || byte == 32) continue;
            ap_attr_t *a = &ansi_term.grid[r][c].attr;
            int fg = (a->fg & 0x07) | (a->bold ? 0x08 : 0);
            float u0 = (byte % 16) * tex_cw;
            float v0 = (byte / 16) * tex_ch;
            float u1 = u0 + tex_cw;
            float v1 = v0 + tex_ch;
            float px0 = x_offset + c * cw, py0 = top_pad + r * ch;
            float px1 = px0 + cw, py1 = py0 + ch;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      u0, v0, u1, v1,
                      palette[fg].r, palette[fg].g, palette[fg].b, 1.0f);
        }
    }

    /* Pass 3: input bar background */
    {
        float bar_y0 = (float)(top_pad + term_h + bot_pad);
        push_quad(PX2NDC_X(0), PX2NDC_Y(bar_y0), PX2NDC_X(vp_w), PX2NDC_Y(vp_h),
                  bg_u0, bg_v0, bg_u1, bg_v1,
                  0.12f, 0.12f, 0.14f, 1.0f);
    }

    /* Pass 4: input bar text */
    {
        float ibar_cw = cw;  /* same char width as terminal */
        float ibar_ch = (float)INPUT_BAR_H;
        float bar_y0 = (float)(top_pad + term_h + bot_pad);
        for (int i = 0; i < input_len && i < TERM_COLS; i++) {
            unsigned char ch = (unsigned char)input_buf[i];
            if (ch <= 32) continue;
            float u0 = (ch % 16) * tex_cw;
            float v0 = (ch / 16) * tex_ch;
            float u1 = u0 + tex_cw;
            float v1 = v0 + tex_ch;
            float px0 = x_offset + i * ibar_cw, py0 = bar_y0 + 2;
            float px1 = px0 + ibar_cw, py1 = bar_y0 + ibar_ch - 2;
            push_quad(PX2NDC_X(px0), PX2NDC_Y(py0), PX2NDC_X(px1), PX2NDC_Y(py1),
                      u0, v0, u1, v1,
                      1.0f, 1.0f, 1.0f, 1.0f);
        }

        /* Blinking cursor */
        if ((frame_count / 30) % 2 == 0) {
            float cx0 = x_offset + input_cursor * ibar_cw;
            float cy0 = bar_y0 + ibar_ch - 4;
            float cx1 = cx0 + ibar_cw;
            float cy1 = bar_y0 + ibar_ch - 1;
            push_quad(PX2NDC_X(cx0), PX2NDC_Y(cy0), PX2NDC_X(cx1), PX2NDC_Y(cy1),
                      bg_u0, bg_v0, bg_u1, bg_v1,
                      0.7f, 0.7f, 0.7f, 1.0f);
        }
    }

    #undef PX2NDC_X
    #undef PX2NDC_Y
}

/* ---- Input handling ---- */

static void input_send(void)
{
    if (!input_buf[0] || !mmansi_hwnd) return;

    /* Add to history */
    if (history_count < MAX_HISTORY) {
        cmd_history[history_count++] = _strdup(input_buf);
    } else {
        free(cmd_history[0]);
        memmove(cmd_history, cmd_history + 1, (MAX_HISTORY - 1) * sizeof(char *));
        cmd_history[MAX_HISTORY - 1] = _strdup(input_buf);
    }
    history_idx = history_count;

    /* Send to MMANSI */
    for (int i = 0; input_buf[i]; i++)
        PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)(unsigned char)input_buf[i], 0);
    PostMessageA(mmansi_hwnd, WM_CHAR, (WPARAM)'\r', 0);

    input_buf[0] = '\0';
    input_len = 0;
    input_cursor = 0;
}

static void input_handle_key(WPARAM vk, int is_char)
{
    if (is_char) {
        if (vk >= 32 && vk < 127 && input_len < INPUT_BUF_SZ - 1) {
            /* Insert at cursor */
            memmove(input_buf + input_cursor + 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_buf[input_cursor] = (char)vk;
            input_len++;
            input_cursor++;
        }
        return;
    }

    /* Key down */
    switch (vk) {
    case VK_RETURN:
        input_send();
        break;
    case VK_BACK:
        if (input_cursor > 0) {
            memmove(input_buf + input_cursor - 1, input_buf + input_cursor,
                    input_len - input_cursor + 1);
            input_cursor--;
            input_len--;
        }
        break;
    case VK_DELETE:
        if (input_cursor < input_len) {
            memmove(input_buf + input_cursor, input_buf + input_cursor + 1,
                    input_len - input_cursor);
            input_len--;
        }
        break;
    case VK_LEFT:
        if (input_cursor > 0) input_cursor--;
        break;
    case VK_RIGHT:
        if (input_cursor < input_len) input_cursor++;
        break;
    case VK_HOME:
        input_cursor = 0;
        break;
    case VK_END:
        input_cursor = input_len;
        break;
    case VK_UP:
        if (history_idx > 0) {
            history_idx--;
            strncpy(input_buf, cmd_history[history_idx], INPUT_BUF_SZ - 1);
            input_len = (int)strlen(input_buf);
            input_cursor = input_len;
        }
        break;
    case VK_DOWN:
        if (history_idx < history_count - 1) {
            history_idx++;
            strncpy(input_buf, cmd_history[history_idx], INPUT_BUF_SZ - 1);
            input_len = (int)strlen(input_buf);
            input_cursor = input_len;
        } else {
            history_idx = history_count;
            input_buf[0] = '\0';
            input_len = 0;
            input_cursor = 0;
        }
        break;
    case VK_ESCAPE:
        input_buf[0] = '\0';
        input_len = 0;
        input_cursor = 0;
        history_idx = history_count;
        break;
    case VK_F11:
        vkt_hide();
        break;
    case VK_F12:
        vkt_screenshot_pending = 1;
        break;
    }
}

/* ---- Screenshot (F12) ---- */

static uint32_t vkt_find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(vk_pdev, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static void vkt_screenshot(uint32_t img_idx)
{
    if (!vk_dev || !vk_swapchain || !vk_sc_images) return;

    uint32_t w = vk_sc_extent.width;
    uint32_t h = vk_sc_extent.height;

    /* Wait for any pending rendering */
    vkDeviceWaitIdle(vk_dev);

    /* Create host-visible buffer to copy swapchain image into */
    VkDeviceSize buf_size = w * h * 4;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = buf_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(vk_dev, &bci, NULL, &staging_buf) != VK_SUCCESS) return;

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(vk_dev, staging_buf, &mem_req);

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex = vkt_find_memory_type(mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(vk_dev, &mai, NULL, &staging_mem) != VK_SUCCESS) {
        vkDestroyBuffer(vk_dev, staging_buf, NULL);
        return;
    }
    vkBindBufferMemory(vk_dev, staging_buf, staging_mem, 0);

    /* Record copy command: swapchain image[0] → staging buffer */
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = vk_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk_dev, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    /* Transition image to TRANSFER_SRC */
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk_sc_images[img_idx];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = w;
    region.imageExtent.height = h;
    region.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, vk_sc_images[img_idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_buf, 1, &region);

    /* Transition back to PRESENT_SRC */
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(vk_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(vk_queue);
    vkFreeCommandBuffers(vk_dev, vk_cmd_pool, 1, &cmd);

    /* Map and write BMP */
    void *mapped = NULL;
    vkMapMemory(vk_dev, staging_mem, 0, buf_size, 0, &mapped);
    if (mapped) {
        unsigned char *src = (unsigned char *)mapped;
        int row_bytes = ((w * 3 + 3) & ~3);
        int img_size = row_bytes * h;
        unsigned char *bmp_data = (unsigned char *)malloc(img_size);
        if (bmp_data) {
            /* Convert BGRA → BGR, flip vertically for BMP bottom-up */
            for (uint32_t y = 0; y < h; y++) {
                unsigned char *dst_row = bmp_data + (h - 1 - y) * row_bytes;
                unsigned char *src_row = src + y * w * 4;
                for (uint32_t x = 0; x < w; x++) {
                    dst_row[x * 3 + 0] = src_row[x * 4 + 0]; /* B */
                    dst_row[x * 3 + 1] = src_row[x * 4 + 1]; /* G */
                    dst_row[x * 3 + 2] = src_row[x * 4 + 2]; /* R */
                }
            }

            char fname[128];
            SYSTEMTIME st;
            GetLocalTime(&st);
            sprintf(fname, "/tmp/vk_screenshot_%04d%02d%02d_%02d%02d%02d_%03d.bmp",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            FILE *fp = fopen(fname, "wb");
            if (fp) {
                /* BITMAPFILEHEADER */
                uint16_t bfType = 0x4D42;
                uint32_t bfSize = 14 + 40 + img_size;
                uint16_t bfReserved = 0;
                uint32_t bfOffBits = 14 + 40;
                fwrite(&bfType, 2, 1, fp);
                fwrite(&bfSize, 4, 1, fp);
                fwrite(&bfReserved, 2, 1, fp);
                fwrite(&bfReserved, 2, 1, fp);
                fwrite(&bfOffBits, 4, 1, fp);
                /* BITMAPINFOHEADER */
                uint32_t biSize = 40;
                int32_t biWidth = (int32_t)w;
                int32_t biHeight = (int32_t)h; /* bottom-up */
                uint16_t biPlanes = 1;
                uint16_t biBitCount = 24;
                uint32_t biCompression = 0;
                uint32_t biSizeImage = img_size;
                int32_t biXPels = 0, biYPels = 0;
                uint32_t biClrUsed = 0, biClrImportant = 0;
                fwrite(&biSize, 4, 1, fp);
                fwrite(&biWidth, 4, 1, fp);
                fwrite(&biHeight, 4, 1, fp);
                fwrite(&biPlanes, 2, 1, fp);
                fwrite(&biBitCount, 2, 1, fp);
                fwrite(&biCompression, 4, 1, fp);
                fwrite(&biSizeImage, 4, 1, fp);
                fwrite(&biXPels, 4, 1, fp);
                fwrite(&biYPels, 4, 1, fp);
                fwrite(&biClrUsed, 4, 1, fp);
                fwrite(&biClrImportant, 4, 1, fp);
                fwrite(bmp_data, img_size, 1, fp);
                fclose(fp);
                if (api) api->log("[vk_terminal] Screenshot saved: %s (%dx%d)\n", fname, w, h);
            }
            free(bmp_data);
        }
        vkUnmapMemory(vk_dev, staging_mem);
    }

    vkDestroyBuffer(vk_dev, staging_buf, NULL);
    vkFreeMemory(vk_dev, staging_mem, NULL);
}

/* ---- Vulkan initialization ---- */

static int vkt_init_vulkan(void)
{
    VkResult res;

    /* Instance */
    const char *inst_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "VkTerminal";
    app.apiVersion = VK_API_VERSION_1_0;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;
    res = vkCreateInstance(&ici, NULL, &vk_inst);
    if (res != VK_SUCCESS) {
        api->log("[vk_terminal] vkCreateInstance failed: %d\n", res);
        return -1;
    }

    /* Physical device */
    uint32_t pdc = 0;
    vkEnumeratePhysicalDevices(vk_inst, &pdc, NULL);
    if (pdc == 0) { api->log("[vk_terminal] No Vulkan devices\n"); return -1; }
    VkPhysicalDevice *pdevs = (VkPhysicalDevice *)malloc(pdc * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(vk_inst, &pdc, pdevs);
    vk_pdev = pdevs[0];
    free(pdevs);

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(vk_pdev, &pdp);
    api->log("[vk_terminal] GPU: %s\n", pdp.deviceName);

    /* Find graphics queue family */
    uint32_t qfc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_pdev, &qfc, NULL);
    VkQueueFamilyProperties *qfp = (VkQueueFamilyProperties *)malloc(qfc * sizeof(VkQueueFamilyProperties));
    vkGetPhysicalDeviceQueueFamilyProperties(vk_pdev, &qfc, qfp);
    vk_qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qfc; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk_qfam = i; break; }
    }
    free(qfp);
    if (vk_qfam == UINT32_MAX) { api->log("[vk_terminal] No graphics queue\n"); return -1; }

    /* Logical device */
    float qpri = 1.0f;
    VkDeviceQueueCreateInfo dqci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    dqci.queueFamilyIndex = vk_qfam;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &qpri;
    const char *dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;
    res = vkCreateDevice(vk_pdev, &dci, NULL, &vk_dev);
    if (res != VK_SUCCESS) { api->log("[vk_terminal] vkCreateDevice failed: %d\n", res); return -1; }
    vkGetDeviceQueue(vk_dev, vk_qfam, 0, &vk_queue);

    /* Command pool */
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpci.queueFamilyIndex = vk_qfam;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(vk_dev, &cpci, NULL, &vk_cmd_pool);

    /* Command buffer */
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbai.commandPool = vk_cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vk_dev, &cbai, &vk_cmd_buf);

    /* Sync objects */
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(vk_dev, &sci, NULL, &vk_sem_avail);
    vkCreateSemaphore(vk_dev, &sci, NULL, &vk_sem_done);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vk_dev, &fci, NULL, &vk_fence);

    api->log("[vk_terminal] Vulkan init OK\n");
    return 0;
}

/* ---- Font texture ---- */

static int vkt_create_font_texture(void)
{
    /* Build RGBA pixel data from HD CP437 atlas (Px437 IBM VGA 32x64) */
    uint32_t atlas_w = CP437_HD_ATLAS_W, atlas_h = CP437_HD_ATLAS_H;
    uint32_t tex_sz = atlas_w * atlas_h * 4;
    uint8_t *pixels = (uint8_t *)calloc(tex_sz, 1);

    for (uint32_t y = 0; y < atlas_h; y++) {
        for (uint32_t x = 0; x < atlas_w; x++) {
            uint8_t alpha = cp437_hd_atlas[y * atlas_w + x];
            int idx = (y * atlas_w + x) * 4;
            pixels[idx+0] = 255;
            pixels[idx+1] = 255;
            pixels[idx+2] = 255;
            pixels[idx+3] = alpha;
        }
    }

    /* Create VkImage */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = (VkExtent3D){ atlas_w, atlas_h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_LINEAR;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    vkCreateImage(vk_dev, &ici, NULL, &vk_font_img);

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk_dev, vk_font_img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = vk_find_memory(vk_pdev, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &mai, NULL, &vk_font_mem);
    vkBindImageMemory(vk_dev, vk_font_img, vk_font_mem, 0);

    /* Copy pixel data */
    void *mapped;
    vkMapMemory(vk_dev, vk_font_mem, 0, mr.size, 0, &mapped);
    /* Need to account for row pitch — get subresource layout */
    VkImageSubresource sub = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(vk_dev, vk_font_img, &sub, &layout);
    for (uint32_t row = 0; row < atlas_h; row++) {
        memcpy((uint8_t *)mapped + layout.offset + row * layout.rowPitch,
               pixels + row * atlas_w * 4, atlas_w * 4);
    }
    vkUnmapMemory(vk_dev, vk_font_mem);
    free(pixels);

    /* Transition to SHADER_READ_ONLY_OPTIMAL */
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(vk_cmd_buf, 0);
    vkBeginCommandBuffer(vk_cmd_buf, &cbbi);
    VkImageMemoryBarrier imb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    imb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    imb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imb.image = vk_font_img;
    imb.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(vk_cmd_buf, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &imb);
    vkEndCommandBuffer(vk_cmd_buf);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk_cmd_buf;
    vkResetFences(vk_dev, 1, &vk_fence);
    vkQueueSubmit(vk_queue, 1, &si, vk_fence);
    vkWaitForFences(vk_dev, 1, &vk_fence, VK_TRUE, UINT64_MAX);

    /* Image view */
    VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ivci.image = vk_font_img;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(vk_dev, &ivci, NULL, &vk_font_view);

    /* Sampler — GL_NEAREST for crisp pixel font */
    VkSamplerCreateInfo saci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    saci.magFilter = VK_FILTER_NEAREST;
    saci.minFilter = VK_FILTER_NEAREST;
    saci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    saci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vk_dev, &saci, NULL, &vk_font_sampler);

    api->log("[vk_terminal] Font texture OK (%dx%d Px437 IBM VGA HD)\n", atlas_w, atlas_h);
    return 0;
}

/* ---- Vertex/Index buffers ---- */

static int vkt_create_buffers(void)
{
    /* Vertex buffer — host visible, updated every frame */
    VkBufferCreateInfo vbci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    vbci.size = MAX_QUADS * 4 * sizeof(vertex_t);
    vbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vkCreateBuffer(vk_dev, &vbci, NULL, &vk_vbuf);

    VkMemoryRequirements vmr;
    vkGetBufferMemoryRequirements(vk_dev, vk_vbuf, &vmr);
    VkMemoryAllocateInfo vmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    vmai.allocationSize = vmr.size;
    vmai.memoryTypeIndex = vk_find_memory(vk_pdev, vmr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &vmai, NULL, &vk_vmem);
    vkBindBufferMemory(vk_dev, vk_vbuf, vk_vmem, 0);
    vkMapMemory(vk_dev, vk_vmem, 0, vmr.size, 0, (void **)&vk_vdata);

    /* Index buffer — static, generated once */
    uint32_t idx_count = MAX_QUADS * 6;
    VkBufferCreateInfo ibci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ibci.size = idx_count * sizeof(uint16_t);
    ibci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vkCreateBuffer(vk_dev, &ibci, NULL, &vk_ibuf);

    VkMemoryRequirements imr;
    vkGetBufferMemoryRequirements(vk_dev, vk_ibuf, &imr);
    VkMemoryAllocateInfo imai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex = vk_find_memory(vk_pdev, imr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(vk_dev, &imai, NULL, &vk_imem);
    vkBindBufferMemory(vk_dev, vk_ibuf, vk_imem, 0);

    /* Fill index buffer: 0,1,2, 2,3,0 for each quad */
    uint16_t *idata;
    vkMapMemory(vk_dev, vk_imem, 0, imr.size, 0, (void **)&idata);
    for (int q = 0; q < MAX_QUADS; q++) {
        uint16_t base = (uint16_t)(q * 4);
        idata[q * 6 + 0] = base + 0;
        idata[q * 6 + 1] = base + 1;
        idata[q * 6 + 2] = base + 2;
        idata[q * 6 + 3] = base + 2;
        idata[q * 6 + 4] = base + 3;
        idata[q * 6 + 5] = base + 0;
    }
    vkUnmapMemory(vk_dev, vk_imem);

    return 0;
}

/* ---- Descriptor set ---- */

static int vkt_create_descriptors(void)
{
    /* Layout */
    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    vkCreateDescriptorSetLayout(vk_dev, &dslci, NULL, &vk_desc_layout);

    /* Pool */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    vkCreateDescriptorPool(vk_dev, &dpci, NULL, &vk_desc_pool);

    /* Allocate */
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = vk_desc_pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &vk_desc_layout;
    vkAllocateDescriptorSets(vk_dev, &dsai, &vk_desc_set);

    /* Update with font texture */
    VkDescriptorImageInfo dii = {0};
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dii.imageView = vk_font_view;
    dii.sampler = vk_font_sampler;
    VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wds.dstSet = vk_desc_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(vk_dev, 1, &wds, 0, NULL);

    return 0;
}

/* ---- Swapchain + pipeline ---- */

static int vkt_create_swapchain(void)
{
    VkResult res;

    /* Surface capabilities */
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_pdev, vk_surface, &caps);

    /* Format */
    uint32_t fmtc;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_pdev, vk_surface, &fmtc, NULL);
    VkSurfaceFormatKHR *fmts = (VkSurfaceFormatKHR *)malloc(fmtc * sizeof(VkSurfaceFormatKHR));
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_pdev, vk_surface, &fmtc, fmts);
    vk_sc_fmt = fmts[0].format;
    VkColorSpaceKHR cs = fmts[0].colorSpace;
    for (uint32_t i = 0; i < fmtc; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            vk_sc_fmt = fmts[i].format;
            cs = fmts[i].colorSpace;
            break;
        }
    }
    free(fmts);

    vk_sc_extent = caps.currentExtent;
    if (vk_sc_extent.width == UINT32_MAX) {
        vk_sc_extent.width = (uint32_t)fs_width;
        vk_sc_extent.height = (uint32_t)fs_height;
    }
    api->log("[vk_terminal] Surface extent: %dx%d (window: %dx%d)\n",
             vk_sc_extent.width, vk_sc_extent.height, fs_width, fs_height);

    uint32_t img_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && img_count > caps.maxImageCount)
        img_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = vk_surface;
    sci.minImageCount = img_count;
    sci.imageFormat = vk_sc_fmt;
    sci.imageColorSpace = cs;
    sci.imageExtent = vk_sc_extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    res = vkCreateSwapchainKHR(vk_dev, &sci, NULL, &vk_swapchain);
    if (res != VK_SUCCESS) { api->log("[vk_terminal] Swapchain failed: %d\n", res); return -1; }

    vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &vk_sc_count, NULL);
    vk_sc_images = (VkImage *)malloc(vk_sc_count * sizeof(VkImage));
    vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &vk_sc_count, vk_sc_images);

    /* Image views */
    vk_sc_views = (VkImageView *)malloc(vk_sc_count * sizeof(VkImageView));
    for (uint32_t i = 0; i < vk_sc_count; i++) {
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ivci.image = vk_sc_images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = vk_sc_fmt;
        ivci.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCreateImageView(vk_dev, &ivci, NULL, &vk_sc_views[i]);
    }

    /* Render pass */
    VkAttachmentDescription att = {0};
    att.format = vk_sc_fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference aref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &aref;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    vkCreateRenderPass(vk_dev, &rpci, NULL, &vk_renderpass);

    /* Framebuffers */
    vk_sc_fbs = (VkFramebuffer *)malloc(vk_sc_count * sizeof(VkFramebuffer));
    for (uint32_t i = 0; i < vk_sc_count; i++) {
        VkFramebufferCreateInfo fbci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbci.renderPass = vk_renderpass;
        fbci.attachmentCount = 1;
        fbci.pAttachments = &vk_sc_views[i];
        fbci.width = vk_sc_extent.width;
        fbci.height = vk_sc_extent.height;
        fbci.layers = 1;
        vkCreateFramebuffer(vk_dev, &fbci, NULL, &vk_sc_fbs[i]);
    }

    /* Pipeline layout */
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &vk_desc_layout;
    vkCreatePipelineLayout(vk_dev, &plci, NULL, &vk_pipe_layout);

    /* Shader modules */
    VkShaderModuleCreateInfo vsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vsci.codeSize = term_vert_spv_size;
    vsci.pCode = term_vert_spv;
    VkShaderModule vs;
    vkCreateShaderModule(vk_dev, &vsci, NULL, &vs);

    VkShaderModuleCreateInfo fsci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fsci.codeSize = term_frag_spv_size;
    fsci.pCode = term_frag_spv;
    VkShaderModule fs;
    vkCreateShaderModule(vk_dev, &fsci, NULL, &fs);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
          VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };

    /* Vertex input */
    VkVertexInputBindingDescription vibd = { 0, sizeof(vertex_t), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription viad[] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(vertex_t, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(vertex_t, u) },
        { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vertex_t, r) },
        { 3, 0, VK_FORMAT_R32_SFLOAT,       offsetof(vertex_t, a) },
    };
    VkPipelineVertexInputStateCreateInfo visci = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    visci.vertexBindingDescriptionCount = 1;
    visci.pVertexBindingDescriptions = &vibd;
    visci.vertexAttributeDescriptionCount = 4;
    visci.pVertexAttributeDescriptions = viad;

    VkPipelineInputAssemblyStateCreateInfo iasci = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport = { 0, 0, (float)vk_sc_extent.width, (float)vk_sc_extent.height, 0, 1 };
    VkRect2D scissor = { {0,0}, vk_sc_extent };
    VkPipelineViewportStateCreateInfo vpsci = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpsci.viewportCount = 1;
    vpsci.pViewports = &viewport;
    vpsci.scissorCount = 1;
    vpsci.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rsci = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.lineWidth = 1.0f;
    rsci.cullMode = VK_CULL_MODE_NONE;
    rsci.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo msci = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Alpha blending for text overlay */
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbsci = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbsci.attachmentCount = 1;
    cbsci.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState = &vpsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState = &msci;
    gpci.pColorBlendState = &cbsci;
    gpci.layout = vk_pipe_layout;
    gpci.renderPass = vk_renderpass;
    gpci.subpass = 0;
    res = vkCreateGraphicsPipelines(vk_dev, VK_NULL_HANDLE, 1, &gpci, NULL, &vk_pipeline);

    vkDestroyShaderModule(vk_dev, vs, NULL);
    vkDestroyShaderModule(vk_dev, fs, NULL);

    if (res != VK_SUCCESS) { api->log("[vk_terminal] Pipeline failed: %d\n", res); return -1; }
    api->log("[vk_terminal] Swapchain %dx%d, %d images\n",
             vk_sc_extent.width, vk_sc_extent.height, vk_sc_count);
    return 0;
}

static void vkt_destroy_swapchain(void)
{
    vkDeviceWaitIdle(vk_dev);
    if (vk_pipeline) { vkDestroyPipeline(vk_dev, vk_pipeline, NULL); vk_pipeline = VK_NULL_HANDLE; }
    if (vk_pipe_layout) { vkDestroyPipelineLayout(vk_dev, vk_pipe_layout, NULL); vk_pipe_layout = VK_NULL_HANDLE; }
    if (vk_renderpass) { vkDestroyRenderPass(vk_dev, vk_renderpass, NULL); vk_renderpass = VK_NULL_HANDLE; }
    if (vk_sc_fbs) {
        for (uint32_t i = 0; i < vk_sc_count; i++) vkDestroyFramebuffer(vk_dev, vk_sc_fbs[i], NULL);
        free(vk_sc_fbs); vk_sc_fbs = NULL;
    }
    if (vk_sc_views) {
        for (uint32_t i = 0; i < vk_sc_count; i++) vkDestroyImageView(vk_dev, vk_sc_views[i], NULL);
        free(vk_sc_views); vk_sc_views = NULL;
    }
    if (vk_sc_images) { free(vk_sc_images); vk_sc_images = NULL; }
    if (vk_swapchain) { vkDestroySwapchainKHR(vk_dev, vk_swapchain, NULL); vk_swapchain = VK_NULL_HANDLE; }
}

/* ---- Render frame ---- */

static void vkt_render_frame(void)
{
    vkWaitForFences(vk_dev, 1, &vk_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(vk_dev, 1, &vk_fence);

    uint32_t img_idx;
    VkResult res = vkAcquireNextImageKHR(vk_dev, vk_swapchain, UINT64_MAX, vk_sem_avail, VK_NULL_HANDLE, &img_idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        vkt_destroy_swapchain();
        vkt_create_swapchain();
        return;
    }

    /* Build vertex data */
    vkt_read_buffer();
    vkt_build_vertices();

    /* Record command buffer */
    vkResetCommandBuffer(vk_cmd_buf, 0);
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(vk_cmd_buf, &cbbi);

    VkClearValue clear = {{{ palette[0].r, palette[0].g, palette[0].b, 1.0f }}};
    VkRenderPassBeginInfo rpbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rpbi.renderPass = vk_renderpass;
    rpbi.framebuffer = vk_sc_fbs[img_idx];
    rpbi.renderArea.extent = vk_sc_extent;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear;
    vkCmdBeginRenderPass(vk_cmd_buf, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline);
    vkCmdBindDescriptorSets(vk_cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_pipe_layout, 0, 1, &vk_desc_set, 0, NULL);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(vk_cmd_buf, 0, 1, &vk_vbuf, &offset);
    vkCmdBindIndexBuffer(vk_cmd_buf, vk_ibuf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(vk_cmd_buf, quad_count * 6, 1, 0, 0, 0);

    vkCmdEndRenderPass(vk_cmd_buf);
    vkEndCommandBuffer(vk_cmd_buf);

    /* Submit */
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &vk_sem_avail;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &vk_cmd_buf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &vk_sem_done;
    vkQueueSubmit(vk_queue, 1, &si, vk_fence);

    /* Present */
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &vk_sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &vk_swapchain;
    pi.pImageIndices = &img_idx;
    vkQueuePresentKHR(vk_queue, &pi);

    /* Screenshot after render if requested */
    if (vkt_screenshot_pending) {
        vkt_screenshot_pending = 0;
        vkt_screenshot(img_idx);
    }

    frame_count++;
}

/* ---- Window procedure ---- */

static LRESULT CALLBACK vkt_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_KEYDOWN:
        input_handle_key(wParam, 0);
        return 0;
    case WM_CHAR:
        input_handle_key(wParam, 1);
        return 0;
    case WM_RBUTTONUP: {
        POINT pt = { (short)LOWORD(lParam), (short)HIWORD(lParam) };
        ClientToScreen(hwnd, &pt);
        HMENU menu = CreatePopupMenu();
        HMENU theme_sub = CreatePopupMenu();
        for (int i = 0; i < NUM_THEMES; i++)
            AppendMenuA(theme_sub, MF_STRING | (i == current_theme ? MF_CHECKED : 0),
                        IDM_VKT_THEME_BASE + i, theme_names[i]);
        AppendMenuA(menu, MF_POPUP, (UINT_PTR)theme_sub, "Theme");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, IDM_VKT_TOGGLE, "Close (F11)");
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id >= IDM_VKT_THEME_BASE && id < IDM_VKT_THEME_BASE + NUM_THEMES) {
            current_theme = id - IDM_VKT_THEME_BASE;
            palette = theme_palettes[current_theme];
            return 0;
        }
        if (id == IDM_VKT_TOGGLE) { vkt_hide(); return 0; }
        return 0;
    }
    case WM_CLOSE:
        vkt_hide();
        return 0;
    case WM_DESTROY:
        vkt_visible = 0;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ---- Thread ---- */

static DWORD WINAPI vkt_thread(LPVOID param)
{
    HINSTANCE hInst = (HINSTANCE)param;

    /* Wait for MMANSI */
    for (int i = 0; i < 100 && !mmansi_hwnd; i++) {
        HWND mw = FindWindowA("MMMAIN", NULL);
        if (mw) {
            mmmain_hwnd = mw;
            mmansi_hwnd = FindWindowExA(mw, NULL, "MMANSI", NULL);
        }
        if (!mmansi_hwnd) Sleep(100);
    }
    if (!mmansi_hwnd) { api->log("[vk_terminal] MMANSI not found\n"); return 1; }

    /* Init Vulkan (instance, device, etc.) */
    if (vkt_init_vulkan() != 0) return 1;
    if (vkt_create_font_texture() != 0) return 1;
    if (vkt_create_buffers() != 0) return 1;
    if (vkt_create_descriptors() != 0) return 1;

    /* Register window class */
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = vkt_wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "SlopVkTerminal";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    palette = theme_palettes[current_theme];
    vkt_running = 1;
    api->log("[vk_terminal] Ready (F11 to toggle fullscreen)\n");

    /* Main loop — wait for F11 toggle, then render */
    while (vkt_running) {
        if (!vkt_visible) {
            /* Tear down presentation if window exists */
            if (vkt_hwnd) {
                if (vk_dev) vkDeviceWaitIdle(vk_dev);
                vkt_destroy_swapchain();
                if (vk_surface) { vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE; }
                DestroyWindow(vkt_hwnd);
                vkt_hwnd = NULL;
                /* Return focus to MegaMUD */
                if (mmmain_hwnd) {
                    SetForegroundWindow(mmmain_hwnd);
                    if (mmansi_hwnd) SetFocus(mmansi_hwnd);
                }
                api->log("[vk_terminal] Closed fullscreen\n");
            }
            Sleep(50);
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { vkt_running = 0; break; }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            continue;
        }

        /* Create fullscreen window if needed */
        if (!vkt_hwnd) {
            /* Use GetSystemMetrics for logical desktop size.
             * Under XWayland with KDE 200% scaling, Wine sees 1920x1080 logical
             * which is the correct coordinate space for window creation. */
            {
                int sm_w = GetSystemMetrics(SM_CXSCREEN);
                int sm_h = GetSystemMetrics(SM_CYSCREEN);
                if (sm_w > 0 && sm_h > 0) {
                    fs_width = sm_w;
                    fs_height = sm_h;
                }
                api->log("[vk_terminal] GetSystemMetrics: %dx%d\n", sm_w, sm_h);
            }
            api->log("[vk_terminal] Fullscreen target: %dx%d\n", fs_width, fs_height);

            vkt_hwnd = CreateWindowExA(
                WS_EX_TOPMOST,
                "SlopVkTerminal", "MajorMUD Terminal",
                WS_POPUP | WS_VISIBLE,
                0, 0, fs_width, fs_height,
                NULL, NULL, hInst, NULL);
            if (!vkt_hwnd) {
                api->log("[vk_terminal] CreateWindow failed\n");
                vkt_visible = 0;
                continue;
            }

            /* Create Vulkan surface */
            VkWin32SurfaceCreateInfoKHR wsci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
            wsci.hinstance = hInst;
            wsci.hwnd = vkt_hwnd;
            VkResult res = vkCreateWin32SurfaceKHR(vk_inst, &wsci, NULL, &vk_surface);
            if (res != VK_SUCCESS) {
                api->log("[vk_terminal] Surface failed: %d\n", res);
                DestroyWindow(vkt_hwnd); vkt_hwnd = NULL;
                vkt_visible = 0;
                continue;
            }

            /* Verify surface support */
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(vk_pdev, vk_qfam, vk_surface, &supported);
            if (!supported) {
                api->log("[vk_terminal] Surface not supported by queue family\n");
                vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE;
                DestroyWindow(vkt_hwnd); vkt_hwnd = NULL;
                vkt_visible = 0;
                continue;
            }

            if (vkt_create_swapchain() != 0) {
                vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE;
                DestroyWindow(vkt_hwnd); vkt_hwnd = NULL;
                vkt_visible = 0;
                continue;
            }

            ShowWindow(vkt_hwnd, SW_SHOW);
            SetForegroundWindow(vkt_hwnd);
            SetFocus(vkt_hwnd);
            api->log("[vk_terminal] Fullscreen %dx%d\n", fs_width, fs_height);
        }

        /* Process messages */
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { vkt_running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!vkt_running) break;

        /* Render if visible */
        if (vkt_visible && vkt_hwnd && vk_swapchain) {
            vkt_render_frame();
        }

        Sleep(1); /* ~vsync via FIFO present mode */
    }

    /* Cleanup */
    vkt_cleanup_vulkan();
    if (vkt_hwnd) { DestroyWindow(vkt_hwnd); vkt_hwnd = NULL; }
    return 0;
}

/* ---- Vulkan cleanup ---- */

static void vkt_cleanup_vulkan(void)
{
    if (vk_dev) vkDeviceWaitIdle(vk_dev);

    vkt_destroy_swapchain();

    if (vk_vdata) { vkUnmapMemory(vk_dev, vk_vmem); vk_vdata = NULL; }
    if (vk_vbuf) { vkDestroyBuffer(vk_dev, vk_vbuf, NULL); vk_vbuf = VK_NULL_HANDLE; }
    if (vk_vmem) { vkFreeMemory(vk_dev, vk_vmem, NULL); vk_vmem = VK_NULL_HANDLE; }
    if (vk_ibuf) { vkDestroyBuffer(vk_dev, vk_ibuf, NULL); vk_ibuf = VK_NULL_HANDLE; }
    if (vk_imem) { vkFreeMemory(vk_dev, vk_imem, NULL); vk_imem = VK_NULL_HANDLE; }

    if (vk_font_sampler) { vkDestroySampler(vk_dev, vk_font_sampler, NULL); vk_font_sampler = VK_NULL_HANDLE; }
    if (vk_font_view) { vkDestroyImageView(vk_dev, vk_font_view, NULL); vk_font_view = VK_NULL_HANDLE; }
    if (vk_font_img) { vkDestroyImage(vk_dev, vk_font_img, NULL); vk_font_img = VK_NULL_HANDLE; }
    if (vk_font_mem) { vkFreeMemory(vk_dev, vk_font_mem, NULL); vk_font_mem = VK_NULL_HANDLE; }

    if (vk_desc_pool) { vkDestroyDescriptorPool(vk_dev, vk_desc_pool, NULL); vk_desc_pool = VK_NULL_HANDLE; }
    if (vk_desc_layout) { vkDestroyDescriptorSetLayout(vk_dev, vk_desc_layout, NULL); vk_desc_layout = VK_NULL_HANDLE; }

    if (vk_sem_avail) { vkDestroySemaphore(vk_dev, vk_sem_avail, NULL); vk_sem_avail = VK_NULL_HANDLE; }
    if (vk_sem_done) { vkDestroySemaphore(vk_dev, vk_sem_done, NULL); vk_sem_done = VK_NULL_HANDLE; }
    if (vk_fence) { vkDestroyFence(vk_dev, vk_fence, NULL); vk_fence = VK_NULL_HANDLE; }
    if (vk_cmd_pool) { vkDestroyCommandPool(vk_dev, vk_cmd_pool, NULL); vk_cmd_pool = VK_NULL_HANDLE; }

    if (vk_surface) { vkDestroySurfaceKHR(vk_inst, vk_surface, NULL); vk_surface = VK_NULL_HANDLE; }
    if (vk_dev) { vkDestroyDevice(vk_dev, NULL); vk_dev = VK_NULL_HANDLE; }
    if (vk_inst) { vkDestroyInstance(vk_inst, NULL); vk_inst = VK_NULL_HANDLE; }
}

/* ---- Exported commands ---- */

__declspec(dllexport) void vkt_show(void)
{
    vkt_visible = 1;
}

__declspec(dllexport) void vkt_hide(void)
{
    vkt_visible = 0;
    /* Don't destroy Vulkan objects here — the render thread handles it.
     * Just set the flag and the thread loop will tear down the window
     * on the next iteration. */
}

__declspec(dllexport) void vkt_toggle(void)
{
    if (vkt_visible) vkt_hide(); else vkt_show();
}

__declspec(dllexport) void vkt_set_resolution(int idx)
{
    if (idx >= 0 && idx < (int)NUM_RES) {
        fs_res_idx = idx;
        fs_width = resolutions[idx].w;
        fs_height = resolutions[idx].h;
    }
}

static slop_command_t vkt_commands[] = {
    { "vkt_show",     "vkt_show",     "",  "v", "Show Vulkan fullscreen terminal" },
    { "vkt_hide",     "vkt_hide",     "",  "v", "Hide Vulkan terminal" },
    { "vkt_toggle",   "vkt_toggle",   "",  "v", "Toggle Vulkan terminal (F11)" },
    { "vkt_set_res",  "vkt_set_resolution", "i", "v", "Set resolution (0=1080p, 1=900, 2=768, 3=720, 4=1024x768)" },
};

__declspec(dllexport) slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(vkt_commands) / sizeof(vkt_commands[0]);
    return vkt_commands;
}

/* ---- Plugin interface ---- */

static int vkt_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_VKT_TOGGLE) {
        vkt_toggle();
        return 1;
    }
    return 0;
}

static int vkt_init(const slop_api_t *a)
{
    api = a;
    api->log("[vk_terminal] Initializing Vulkan Terminal...\n");
    api->add_menu_item("Vulkan Terminal (F11)", IDM_VKT_TOGGLE);

    /* Initialize ANSI terminal emulator */
    InitializeCriticalSection(&ansi_lock);
    ap_init(&ansi_term, TERM_ROWS, TERM_COLS);

    palette = theme_palettes[current_theme];
    input_buf[0] = '\0';

    HINSTANCE hInst = GetModuleHandleA(NULL);
    vkt_thread_handle = CreateThread(NULL, 0, vkt_thread, (LPVOID)hInst, 0, NULL);
    return 0;
}

static void vkt_shutdown(void)
{
    vkt_running = 0;
    if (vkt_hwnd) PostMessageA(vkt_hwnd, WM_QUIT, 0, 0);
    if (vkt_thread_handle) {
        WaitForSingleObject(vkt_thread_handle, 2000);
        CloseHandle(vkt_thread_handle);
        vkt_thread_handle = NULL;
    }
    for (int i = 0; i < history_count; i++) free(cmd_history[i]);
    history_count = 0;
    DeleteCriticalSection(&ansi_lock);
    if (api) api->log("[vk_terminal] Shutdown\n");
}

static slop_plugin_t vkt_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Vulkan Terminal",
    .author      = "Tripmunk",
    .description = "Fullscreen Vulkan terminal — F11 toggle, CP437 font, DOS colors",
    .version     = "0.1.0",
    .init        = vkt_init,
    .shutdown    = vkt_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = vkt_on_wndproc,
    .on_data     = vkt_on_data,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &vkt_plugin; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
