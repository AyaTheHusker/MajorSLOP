/* mudradio.c — MUDRadio Audio Engine
 * Stream, decode, and play audio via miniaudio + HTTP streaming.
 * Included into vk_terminal.c
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

/* WinINet for HTTP (dynamically loaded) */
typedef void *HINTERNET;
#define INTERNET_OPEN_TYPE_PRECONFIG 0
#define INTERNET_FLAG_NO_CACHE_WRITE 0x04000000
#define INTERNET_FLAG_RELOAD         0x80000000
#define INTERNET_FLAG_NO_UI          0x00000200
#define HTTP_QUERY_STATUS_CODE       19
#define HTTP_QUERY_FLAG_NUMBER       0x20000000
#define HTTP_QUERY_RAW_HEADERS_CRLF  22
#define HTTP_QUERY_CUSTOM            65535

typedef HINTERNET (WINAPI *InternetOpenA_fn)(const char*, DWORD, const char*, const char*, DWORD);
typedef HINTERNET (WINAPI *InternetOpenUrlA_fn)(HINTERNET, const char*, const char*, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *InternetReadFile_fn)(HINTERNET, void*, DWORD, DWORD*);
typedef BOOL (WINAPI *InternetCloseHandle_fn)(HINTERNET);
typedef BOOL (WINAPI *HttpQueryInfoA_fn)(HINTERNET, DWORD, void*, DWORD*, DWORD*);

static HMODULE mr_wininet = NULL;
static InternetOpenA_fn      pInternetOpenA = NULL;
static InternetOpenUrlA_fn   pInternetOpenUrlA = NULL;
static InternetReadFile_fn   pInternetReadFile = NULL;
static InternetCloseHandle_fn pInternetCloseHandle = NULL;
static HttpQueryInfoA_fn     pHttpQueryInfoA = NULL;

static int mr_load_wininet(void) {
    if (pInternetOpenA) return 1;
    mr_wininet = LoadLibraryA("wininet.dll");
    if (!mr_wininet) return 0;
    pInternetOpenA = (InternetOpenA_fn)GetProcAddress(mr_wininet, "InternetOpenA");
    pInternetOpenUrlA = (InternetOpenUrlA_fn)GetProcAddress(mr_wininet, "InternetOpenUrlA");
    pInternetReadFile = (InternetReadFile_fn)GetProcAddress(mr_wininet, "InternetReadFile");
    pInternetCloseHandle = (InternetCloseHandle_fn)GetProcAddress(mr_wininet, "InternetCloseHandle");
    pHttpQueryInfoA = (HttpQueryInfoA_fn)GetProcAddress(mr_wininet, "HttpQueryInfoA");
    return (pInternetOpenA && pInternetOpenUrlA && pInternetReadFile && pInternetCloseHandle) ? 1 : 0;
}

/* ---- Simple JSON parser for Radio Browser API ---- */

/* Extract quoted string value for a key from JSON. Returns pointer into buf, or NULL. */
static const char *mr_json_str(const char *json, const char *key, char *out, int out_sz) {
    char search[64];
    _snprintf(search, sizeof(search), "\"%s\"", key);
    search[sizeof(search) - 1] = 0;
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p+1)) { p++; } /* skip escape */
        out[i++] = *p++;
    }
    out[i] = 0;
    return out;
}

static int mr_json_int(const char *json, const char *key) {
    char search[64];
    _snprintf(search, sizeof(search), "\"%s\"", key);
    search[sizeof(search) - 1] = 0;
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    return atoi(p);
}

/* Parse Radio Browser JSON array into mr_search_results */
static void mr_parse_stations(const char *json) {
    mr_search_count = 0;
    const char *p = json;
    while (*p && mr_search_count < MR_MAX_SEARCH) {
        /* Find next object start */
        p = strchr(p, '{');
        if (!p) break;
        /* Find object end */
        const char *end = strchr(p, '}');
        if (!end) break;
        /* Extract into a temp buffer */
        int len = (int)(end - p + 1);
        if (len > 2048) len = 2048;
        char obj[2048];
        memcpy(obj, p, len);
        obj[len] = 0;

        mr_station_t *s = &mr_search_results[mr_search_count];
        memset(s, 0, sizeof(*s));
        mr_json_str(obj, "name", s->name, sizeof(s->name));
        mr_json_str(obj, "url_resolved", s->url, sizeof(s->url));
        if (!s->url[0]) mr_json_str(obj, "url", s->url, sizeof(s->url));
        mr_json_str(obj, "tags", s->genre, sizeof(s->genre));
        mr_json_str(obj, "country", s->country, sizeof(s->country));
        s->bitrate = mr_json_int(obj, "bitrate");

        /* Check if favorite */
        for (int i = 0; i < mr_fav_count; i++) {
            if (strcmp(mr_favorites[i].url, s->url) == 0) { s->is_favorite = 1; break; }
        }

        if (s->name[0] && s->url[0]) mr_search_count++;
        p = end + 1;
    }
}

/* ---- Search Thread ---- */

static HANDLE mr_search_thread = NULL;
static char mr_search_query[256] = {0};

static DWORD WINAPI mr_search_thread_fn(LPVOID arg) {
    (void)arg;
    if (!mr_load_wininet()) { mr_searching = 0; return 1; }

    HINTERNET hinet = pInternetOpenA("MUDRadio/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hinet) { mr_searching = 0; return 1; }

    /* Build URL — search by tag or name */
    char url[512];
    /* URL-encode spaces as + */
    char encoded[256];
    int ei = 0;
    for (int i = 0; mr_search_query[i] && ei < 250; i++) {
        if (mr_search_query[i] == ' ') encoded[ei++] = '+';
        else encoded[ei++] = mr_search_query[i];
    }
    encoded[ei] = 0;

    _snprintf(url, sizeof(url),
        "https://de1.api.radio-browser.info/json/stations/search?name=%s&limit=50&order=votes&reverse=true",
        encoded);
    url[sizeof(url) - 1] = 0;

    HINTERNET hurl = pInternetOpenUrlA(hinet, url, "Accept: application/json\r\n", (DWORD)-1,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0);
    if (!hurl) {
        /* Try tag search as fallback */
        _snprintf(url, sizeof(url),
            "https://de1.api.radio-browser.info/json/stations/bytag/%s?limit=50&order=votes&reverse=true",
            encoded);
        url[sizeof(url) - 1] = 0;
        hurl = pInternetOpenUrlA(hinet, url, "Accept: application/json\r\n", (DWORD)-1,
            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0);
    }

    if (hurl) {
        /* Read response into buffer */
        char *buf = (char *)malloc(256 * 1024);
        if (buf) {
            int total = 0;
            DWORD got;
            while (pInternetReadFile(hurl, buf + total, 256 * 1024 - total - 1, &got) && got > 0) {
                total += got;
                if (total >= 256 * 1024 - 1) break;
            }
            buf[total] = 0;
            mr_parse_stations(buf);
            free(buf);
        }
        pInternetCloseHandle(hurl);
    }
    pInternetCloseHandle(hinet);
    mr_searching = 0;
    return 0;
}

static void mr_do_search(const char *query) {
    if (mr_searching) return;
    strncpy(mr_search_query, query, sizeof(mr_search_query));
    mr_search_query[sizeof(mr_search_query) - 1] = 0;
    mr_searching = 1;
    mr_search_count = 0;
    mr_list_mode = MR_LIST_SEARCH;
    mr_scroll = 0;

    HANDLE h = CreateThread(NULL, 0, mr_search_thread_fn, NULL, 0, NULL);
    if (h) CloseHandle(h); /* fire and forget */
    else mr_searching = 0;
}

/* ---- miniaudio-based audio output + MP3 decoding ---- */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define MR_SAMPLE_RATE   48000

static volatile int mr_audio_running;

/* ---- Net Ring Buffer (raw MP3 bytes, network→decode decoupling) ---- */
#define MR_NET_RING_SZ (256 * 1024) /* 256KB raw MP3 — ~16 sec at 128kbps */
static unsigned char mr_net_ring[MR_NET_RING_SZ];
static volatile int mr_net_w = 0;
static volatile int mr_net_r = 0;
static volatile int mr_net_running = 0;
static volatile HINTERNET mr_net_hurl = NULL;
static volatile HINTERNET mr_net_hinet = NULL;
static HANDLE mr_net_thread = NULL;

static int mr_net_avail(void) {
    int w = mr_net_w, r = mr_net_r;
    return (w >= r) ? (w - r) : (MR_NET_RING_SZ - r + w);
}
static int mr_net_free(void) {
    return MR_NET_RING_SZ - 1 - mr_net_avail();
}
static void mr_net_write(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) {
        mr_net_ring[mr_net_w] = data[i];
        mr_net_w = (mr_net_w + 1) % MR_NET_RING_SZ;
    }
}

/* ---- Debug Log (forward decl — defined later) ---- */
static FILE *mr_dbg;
static void mr_dbg_log(const char *evt, const char *detail);
static void mr_dbg_open(void);
static void mr_dbg_close(void);

/* ---- miniaudio decoder + device (handles EVERYTHING: decode, resample, output) ---- */

static ma_device  mr_ma_dev;
static ma_decoder mr_ma_dec;
static volatile int mr_ma_dev_started = 0;
static volatile int mr_ma_dec_ready = 0;

/* ma_decoder read callback — pulls raw MP3 bytes from net ring.
 * Called from the device's audio thread via ma_decoder_read_pcm_frames.
 * If not enough data available, we spin-wait briefly. */
static ma_result mr_ma_read_cb(ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead, size_t *pBytesRead)
{
    (void)pDecoder;
    unsigned char *out = (unsigned char *)pBufferOut;
    size_t total = 0;
    int spins = 0;

    while (total < bytesToRead) {
        /* inline net ring read — avoid forward decl issues */
        int w = mr_net_w, r = mr_net_r;
        int avail = (w >= r) ? (w - r) : (MR_NET_RING_SZ - r + w);
        if (avail > 0) {
            int want = (int)(bytesToRead - total);
            if (want > avail) want = avail;
            for (int i = 0; i < want; i++) {
                out[total + i] = mr_net_ring[r];
                r = (r + 1) % MR_NET_RING_SZ;
            }
            mr_net_r = r;
            total += want;
            spins = 0;
        } else {
            if (!mr_audio_running || !mr_net_running) break;
            if (++spins > 200) break; /* 1 sec max wait */
            Sleep(5);
        }
    }

    *pBytesRead = total;
    return (total > 0) ? MA_SUCCESS : MA_AT_END;
}

static ma_result mr_ma_seek_cb(ma_decoder *pDecoder, ma_int64 byteOffset, ma_seek_origin origin)
{
    (void)pDecoder; (void)byteOffset; (void)origin;
    /* Return success but do nothing — streams can't truly seek, but returning
     * an error causes ma_decoder_init to fail. Miniaudio only seeks during init
     * to check for ID3 tags / Xing headers, which is harmless to skip. */
    return MA_SUCCESS;
}

/* Device data callback — reads decoded PCM from ma_decoder */
static void mr_ma_data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount)
{
    (void)pDevice; (void)pInput;
    float *out = (float *)pOutput;

    if (!mr_ma_dec_ready) {
        memset(out, 0, frameCount * 2 * sizeof(float));
        return;
    }

    ma_uint64 frames_read = 0;
    ma_decoder_read_pcm_frames(&mr_ma_dec, out, frameCount, &frames_read);

    /* Apply volume */
    float vol = mr_volume;
    for (ma_uint64 i = 0; i < frames_read * 2; i++)
        out[i] *= vol;

    /* Silence-fill remainder */
    if (frames_read < frameCount)
        memset(out + frames_read * 2, 0, (frameCount - frames_read) * 2 * sizeof(float));
}

static int mr_open_audio(void) {
    if (mr_ma_dev_started) return 1;

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = MR_SAMPLE_RATE;
    cfg.dataCallback      = mr_ma_data_callback;
    cfg.pUserData         = NULL;
    cfg.periodSizeInFrames = 2048;
    cfg.performanceProfile = ma_performance_profile_conservative;

    if (ma_device_init(NULL, &cfg, &mr_ma_dev) != MA_SUCCESS) return 0;
    if (ma_device_start(&mr_ma_dev) != MA_SUCCESS) {
        ma_device_uninit(&mr_ma_dev);
        return 0;
    }
    mr_ma_dev_started = 1;
    return 1;
}

static int mr_open_decoder(void) {
    ma_decoder_config dec_cfg = ma_decoder_config_init(ma_format_f32, 2, MR_SAMPLE_RATE);
    dec_cfg.encodingFormat = ma_encoding_format_mp3;
    ma_result rc = ma_decoder_init(mr_ma_read_cb, mr_ma_seek_cb, NULL, &dec_cfg, &mr_ma_dec);
    if (rc != MA_SUCCESS) {
        FILE *f = fopen("C:\\MegaMUD\\radio_error.txt", "a");
        if (f) { fprintf(f, "ma_decoder_init failed: %d, net_avail=%d\n", rc, mr_net_avail()); fclose(f); }
        return 0;
    }
    mr_ma_dec_ready = 1;
    FILE *f = fopen("C:\\MegaMUD\\radio_error.txt", "a");
    if (f) { fprintf(f, "decoder OK, net_avail=%d\n", mr_net_avail()); fclose(f); }
    return 1;
}

static void mr_close_audio(void) {
    /* Order matters: stop callback from using decoder, then stop device, then uninit decoder */
    mr_ma_dec_ready = 0;
    Sleep(50); /* let any in-flight callback finish */
    if (mr_ma_dev_started) {
        ma_device_stop(&mr_ma_dev);
        ma_device_uninit(&mr_ma_dev);
        mr_ma_dev_started = 0;
    }
    ma_decoder_uninit(&mr_ma_dec);
}

/* ---- Audio Thread ---- */

static HANDLE mr_audio_thread = NULL;
static volatile mr_src_t mr_src_type = MR_SRC_NONE;
static char mr_src_path[MR_MAX_PATH_LEN] = {0};

/* ---- Network Thread: HTTP read + ICY strip → net ring ---- */
static volatile int mr_net_connected = 0;
static volatile int mr_net_error = 0;

/* ---- Debug Log ---- */
static FILE *mr_dbg = NULL;
static DWORD mr_dbg_start = 0;
static void mr_dbg_open(void) {
    if (mr_dbg) return;
    mr_dbg = fopen("C:\\MegaMUD\\radio_debug.txt", "w");
    mr_dbg_start = GetTickCount();
    if (mr_dbg) fprintf(mr_dbg, "T(ms) | event | ring_avail | net_avail | detail\n");
}
static void mr_dbg_log(const char *evt, const char *detail) {
    if (!mr_dbg) return;
    DWORD t = GetTickCount() - mr_dbg_start;
    fprintf(mr_dbg, "%6lu | %-12s | %6d | %s\n",
            (unsigned long)t, evt, mr_net_avail(),
            detail ? detail : "");
    fflush(mr_dbg);
}
static void mr_dbg_close(void) {
    if (mr_dbg) { fclose(mr_dbg); mr_dbg = NULL; }
}

static DWORD WINAPI mr_net_thread_fn(LPVOID arg) {
    (void)arg;
    if (!mr_load_wininet()) { mr_net_error = 1; mr_net_running = 0; return 1; }

    HINTERNET hinet = pInternetOpenA("MUDRadio/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hinet) { mr_net_error = 1; mr_net_running = 0; return 1; }
    mr_net_hinet = hinet;

    HINTERNET hurl = pInternetOpenUrlA(hinet, mr_src_path,
        "Icy-MetaData: 1\r\n", (DWORD)-1,
        INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0);
    if (!hurl) {
        mr_net_hinet = NULL;
        pInternetCloseHandle(hinet);
        mr_net_error = 1;
        mr_net_running = 0;
        return 1;
    }
    mr_net_hurl = hurl;

    /* Parse icy-metaint from headers — multiple strategies for Wine compat */
    int icy_metaint = 0;
    if (pHttpQueryInfoA) {
        /* Strategy 1: HTTP_QUERY_CUSTOM — ask WinINet for the header by name */
        char custom_val[64] = "icy-metaint";
        DWORD custom_sz = sizeof(custom_val);
        DWORD custom_idx = 0;
        if (pHttpQueryInfoA(hurl, HTTP_QUERY_CUSTOM, custom_val, &custom_sz, &custom_idx)) {
            icy_metaint = atoi(custom_val);
        }

        /* Strategy 2: case-insensitive search through raw headers */
        if (icy_metaint <= 0) {
            char hdrs[8192] = {0};
            DWORD hdr_sz = sizeof(hdrs) - 1;
            DWORD idx = 0;
            if (pHttpQueryInfoA(hurl, HTTP_QUERY_RAW_HEADERS_CRLF, hdrs, &hdr_sz, &idx)) {
                /* Log ALL headers for debugging */
                if (mr_dbg) { fprintf(mr_dbg, "RAW HEADERS (%lu bytes):\n%s\n---END HEADERS---\n", (unsigned long)hdr_sz, hdrs); fflush(mr_dbg); }
                /* Convert to lowercase for case-insensitive search */
                char hdrs_lower[8192];
                for (DWORD i = 0; i <= hdr_sz && i < sizeof(hdrs_lower) - 1; i++)
                    hdrs_lower[i] = (hdrs[i] >= 'A' && hdrs[i] <= 'Z') ? hdrs[i] + 32 : hdrs[i];
                hdrs_lower[hdr_sz < sizeof(hdrs_lower) - 1 ? hdr_sz : sizeof(hdrs_lower) - 1] = 0;
                const char *mi = strstr(hdrs_lower, "icy-metaint:");
                if (mi) {
                    mi += 12;
                    while (*mi == ' ') mi++;
                    icy_metaint = atoi(mi);
                }
            }
        }
    }
    if (mr_dbg) { fprintf(mr_dbg, "icy_metaint = %d\n", icy_metaint); fflush(mr_dbg); }

    /* Strategy 3: if we requested ICY but got no metaint, reconnect WITHOUT
       ICY metadata to guarantee clean audio (lose song titles, gain stability) */
    if (icy_metaint <= 0) {
        if (mr_dbg) { fprintf(mr_dbg, "WARNING: no icy-metaint found, reconnecting without ICY\n"); fflush(mr_dbg); }
        pInternetCloseHandle(hurl);
        mr_net_hurl = NULL;
        hurl = pInternetOpenUrlA(hinet, mr_src_path,
            NULL, 0,
            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI, 0);
        if (!hurl) {
            mr_net_hinet = NULL;
            pInternetCloseHandle(hinet);
            mr_net_error = 1;
            mr_net_running = 0;
            return 1;
        }
        mr_net_hurl = hurl;
    }

    #ifndef MR_STREAM_BUF_SZ
    #define MR_STREAM_BUF_SZ (64 * 1024)
    #endif
    unsigned char *sbuf = (unsigned char *)malloc(MR_STREAM_BUF_SZ);
    int sbuf_fill = 0;
    int audio_bytes_since_meta = 0;

    /* Signal decode thread that connection is good */
    mr_net_connected = 1;

    while (mr_net_running) {
        /* Wait if net ring is mostly full */
        if (mr_net_free() < 16384) {
            Sleep(10);
            continue;
        }

        DWORD got = 0;
        int to_read = MR_STREAM_BUF_SZ - sbuf_fill;
        if (to_read > 16384) to_read = 16384;

        if (to_read > 0 && pInternetReadFile(hurl, sbuf + sbuf_fill, to_read, &got) && got > 0) {
            sbuf_fill += got;
        } else {
            break; /* Connection closed or aborted by mr_stop */
        }

        /* Strip ICY metadata, write clean audio to net ring */
        if (icy_metaint > 0) {
            int pos = 0;
            while (pos < sbuf_fill) {
                int audio_remaining = icy_metaint - audio_bytes_since_meta;
                if (audio_remaining > 0) {
                    int chunk = sbuf_fill - pos;
                    if (chunk > audio_remaining) chunk = audio_remaining;
                    /* Wait for net ring space */
                    while (mr_net_free() < chunk && mr_net_running) Sleep(5);
                    if (!mr_net_running) break;
                    mr_net_write(sbuf + pos, chunk);
                    pos += chunk;
                    audio_bytes_since_meta += chunk;
                }
                if (audio_bytes_since_meta >= icy_metaint && pos < sbuf_fill) {
                    int meta_len = sbuf[pos] * 16;
                    pos++;
                    if (pos + meta_len <= sbuf_fill) {
                        if (meta_len > 0) {
                            char meta_str[4081];
                            int mlen = meta_len < 4080 ? meta_len : 4080;
                            memcpy(meta_str, sbuf + pos, mlen);
                            meta_str[mlen] = 0;
                            char *st = strstr(meta_str, "StreamTitle='");
                            if (st) {
                                st += 13;
                                char *end = strchr(st, '\'');
                                if (end) *end = 0;
                                EnterCriticalSection(&mr_meta_lock);
                                strncpy(mr_meta.stream_title, st, sizeof(mr_meta.stream_title));
                                mr_meta.stream_title[sizeof(mr_meta.stream_title) - 1] = 0;
                                char *dash = strstr(st, " - ");
                                if (dash) {
                                    *dash = 0;
                                    strncpy(mr_meta.artist, st, sizeof(mr_meta.artist));
                                    mr_meta.artist[sizeof(mr_meta.artist) - 1] = 0;
                                    strncpy(mr_meta.track, dash + 3, sizeof(mr_meta.track));
                                    mr_meta.track[sizeof(mr_meta.track) - 1] = 0;
                                } else {
                                    strncpy(mr_meta.track, st, sizeof(mr_meta.track));
                                    mr_meta.track[sizeof(mr_meta.track) - 1] = 0;
                                    mr_meta.artist[0] = 0;
                                }
                                LeaveCriticalSection(&mr_meta_lock);
                            }
                        }
                        pos += meta_len;
                    } else {
                        pos--;
                        break;
                    }
                    audio_bytes_since_meta = 0;
                }
            }
            if (pos < sbuf_fill) {
                memmove(sbuf, sbuf + pos, sbuf_fill - pos);
                sbuf_fill -= pos;
            } else {
                sbuf_fill = 0;
            }
        } else {
            /* No ICY — raw audio bytes go straight to net ring */
            while (mr_net_free() < sbuf_fill && mr_net_running) Sleep(5);
            if (mr_net_running) mr_net_write(sbuf, sbuf_fill);
            sbuf_fill = 0;
        }
    }

    free(sbuf);
    /* Close handles only if mr_stop() didn't already close them */
    if (mr_net_hurl) {
        mr_net_hurl = NULL;
        pInternetCloseHandle(hurl);
    }
    if (mr_net_hinet) {
        mr_net_hinet = NULL;
        pInternetCloseHandle(hinet);
    }
    mr_net_running = 0;
    return 0;
}

/* ---- Stream: net thread fills net_ring, ma_decoder reads it, ma_device plays it ---- */
static void mr_stream_radio(void) {
    /* Start network thread */
    mr_net_w = 0; mr_net_r = 0;
    mr_net_connected = 0;
    mr_net_error = 0;
    mr_net_running = 1;
    mr_transport = MR_STATE_CONNECTING;
    mr_net_thread = CreateThread(NULL, 0, mr_net_thread_fn, NULL, 0, NULL);
    if (!mr_net_thread) {
        mr_net_running = 0;
        mr_transport = MR_STATE_ERROR;
        return;
    }

    /* Wait for net thread to connect */
    while (!mr_net_connected && !mr_net_error && mr_audio_running)
        Sleep(20);
    if (mr_net_error || !mr_audio_running) {
        mr_transport = MR_STATE_ERROR;
        goto cleanup;
    }

    /* Wait for enough MP3 data in net ring before starting decoder (~64KB) */
    while (mr_net_avail() < 65536 && mr_net_running && mr_audio_running)
        Sleep(20);

    mr_transport = MR_STATE_PLAYING;

    /* Open miniaudio decoder — reads from net ring via callback */
    if (!mr_open_decoder()) {
        mr_transport = MR_STATE_ERROR;
        goto cleanup;
    }

    /* Open audio device — its callback reads from the decoder */
    if (!mr_open_audio()) {
        mr_transport = MR_STATE_ERROR;
        goto cleanup;
    }

    /* Just keep the thread alive while playing — all work is in callbacks */
    while (mr_audio_running && mr_transport != MR_STATE_STOPPED &&
           mr_transport != MR_STATE_ERROR && mr_net_running) {
        Sleep(100);
        while (mr_transport == MR_STATE_PAUSED && mr_audio_running)
            Sleep(50);
    }

cleanup:
    mr_close_audio();
    mr_net_running = 0;
    {
        HINTERNET h = (HINTERNET)mr_net_hurl;
        if (h) { mr_net_hurl = NULL; pInternetCloseHandle(h); }
        h = (HINTERNET)mr_net_hinet;
        if (h) { mr_net_hinet = NULL; pInternetCloseHandle(h); }
    }
    if (mr_net_thread) {
        WaitForSingleObject(mr_net_thread, 2000);
        CloseHandle(mr_net_thread);
        mr_net_thread = NULL;
    }
}

static DWORD WINAPI mr_audio_thread_fn(LPVOID arg)
{
    (void)arg;

    while (mr_audio_running) {
        if (mr_transport == MR_STATE_STOPPED || mr_src_type == MR_SRC_NONE) {
            Sleep(50);
            continue;
        }
        if (mr_transport == MR_STATE_PAUSED) {
            Sleep(50);
            continue;
        }

        /* ---- Stream source ---- */
        if (mr_src_type == MR_SRC_STREAM) {
            mr_stream_radio();
            if (mr_audio_running && mr_transport == MR_STATE_PLAYING) {
                mr_transport = MR_STATE_STOPPED;
                mr_close_audio();
            }
            continue;
        }

        /* ---- File source — use ma_decoder for files too ---- */
        if (mr_src_type == MR_SRC_FILE) {
            EnterCriticalSection(&mr_meta_lock);
            {
                const char *name = mr_src_path;
                const char *p = mr_src_path;
                while (*p) { if (*p == '\\' || *p == '/') name = p + 1; p++; }
                _snprintf(mr_meta.track, sizeof(mr_meta.track), "%s", name);
                mr_meta.track[sizeof(mr_meta.track) - 1] = 0;
                mr_meta.artist[0] = 0;
                mr_meta.station_name[0] = 0;
                mr_meta.stream_title[0] = 0;
            }
            LeaveCriticalSection(&mr_meta_lock);

            /* Use miniaudio file decoder */
            ma_decoder_config fcfg = ma_decoder_config_init(ma_format_f32, 2, MR_SAMPLE_RATE);
            if (ma_decoder_init_file(mr_src_path, &fcfg, &mr_ma_dec) != MA_SUCCESS) {
                mr_transport = MR_STATE_ERROR;
                continue;
            }
            mr_ma_dec_ready = 1;
            mr_transport = MR_STATE_PLAYING;

            if (!mr_open_audio()) {
                mr_ma_dec_ready = 0;
                ma_decoder_uninit(&mr_ma_dec);
                mr_transport = MR_STATE_ERROR;
                continue;
            }

            /* Wait for playback to finish or stop */
            while (mr_audio_running && mr_transport == MR_STATE_PLAYING)
                Sleep(100);

            if (mr_audio_running && mr_transport == MR_STATE_PLAYING) {
                mr_transport = MR_STATE_STOPPED;
                mr_close_audio();
            }
        }
        Sleep(50);
    }
    mr_close_audio();
    return 0;
}

/* ---- Public Control Functions ---- */

static void mr_play_file(const char *path) {
    mr_stop();
    strncpy(mr_src_path, path, MR_MAX_PATH_LEN);
    mr_src_path[MR_MAX_PATH_LEN - 1] = 0;
    mr_src_type = MR_SRC_FILE;
    mr_transport = MR_STATE_PLAYING;
}

static void mr_play_stream(const char *url) {
    mr_stop();
    strncpy(mr_src_path, url, MR_MAX_PATH_LEN);
    mr_src_path[MR_MAX_PATH_LEN - 1] = 0;
    mr_src_type = MR_SRC_STREAM;

    EnterCriticalSection(&mr_meta_lock);
    mr_meta.stream_title[0] = 0;
    mr_meta.artist[0] = 0;
    mr_meta.track[0] = 0;
    LeaveCriticalSection(&mr_meta_lock);

    mr_transport = MR_STATE_PLAYING;
}

static void mr_stop(void) {
    if (mr_transport == MR_STATE_STOPPED && mr_src_type == MR_SRC_NONE) return;
    mr_transport = MR_STATE_STOPPED;
    mr_src_type = MR_SRC_NONE;
    /* Signal decoder callback to output silence immediately */
    mr_ma_dec_ready = 0;
    /* Kill network thread — closing handles aborts blocking reads */
    mr_net_running = 0;
    {
        HINTERNET h = (HINTERNET)mr_net_hurl;
        if (h) { mr_net_hurl = NULL; pInternetCloseHandle(h); }
        h = (HINTERNET)mr_net_hinet;
        if (h) { mr_net_hinet = NULL; pInternetCloseHandle(h); }
    }
    /* Wait for audio thread to finish stream_radio/file and call mr_close_audio itself */
    Sleep(300);
    /* Safety: close audio if stream_radio didn't get to it */
    mr_close_audio();
    mr_net_w = 0; mr_net_r = 0;
}

static void mr_pause(void) {
    if (mr_transport == MR_STATE_PLAYING)
        mr_transport = MR_STATE_PAUSED;
}

static void mr_resume(void) {
    if (mr_transport == MR_STATE_PAUSED)
        mr_transport = MR_STATE_PLAYING;
}

static void mr_next(void) {
    if (mr_pl_count == 0) return;
    if (mr_shuffle)
        mr_pl_current = (int)(GetTickCount() % (unsigned)mr_pl_count);
    else {
        mr_pl_current++;
        if (mr_pl_current >= mr_pl_count) mr_pl_current = 0;
    }
    mr_play_file(mr_playlist[mr_pl_current].path);
}

static void mr_prev(void) {
    if (mr_pl_count == 0) return;
    mr_pl_current--;
    if (mr_pl_current < 0) mr_pl_current = mr_pl_count - 1;
    mr_play_file(mr_playlist[mr_pl_current].path);
}

/* Play a station from search results by index */
static void mr_play_station(int idx) {
    mr_station_t *s = NULL;
    if (mr_list_mode == MR_LIST_FAVORITES && idx >= 0 && idx < mr_fav_count)
        s = &mr_favorites[idx];
    else if (mr_list_mode == MR_LIST_SEARCH && idx >= 0 && idx < mr_search_count)
        s = &mr_search_results[idx];
    if (!s || !s->url[0]) return;

    EnterCriticalSection(&mr_meta_lock);
    strncpy(mr_meta.station_name, s->name, sizeof(mr_meta.station_name));
    mr_meta.station_name[sizeof(mr_meta.station_name) - 1] = 0;
    mr_meta.bitrate = s->bitrate;
    strncpy(mr_meta.genre, s->genre, sizeof(mr_meta.genre));
    mr_meta.genre[sizeof(mr_meta.genre) - 1] = 0;
    LeaveCriticalSection(&mr_meta_lock);

    mr_play_stream(s->url);
}

/* Toggle favorite for a station */
static void mr_toggle_favorite(int idx) {
    mr_station_t *s = NULL;
    if (mr_list_mode == MR_LIST_SEARCH && idx >= 0 && idx < mr_search_count)
        s = &mr_search_results[idx];
    else if (mr_list_mode == MR_LIST_FAVORITES && idx >= 0 && idx < mr_fav_count)
        s = &mr_favorites[idx];
    if (!s) return;

    if (s->is_favorite) {
        /* Remove from favorites */
        for (int i = 0; i < mr_fav_count; i++) {
            if (strcmp(mr_favorites[i].url, s->url) == 0) {
                memmove(&mr_favorites[i], &mr_favorites[i+1],
                        (mr_fav_count - i - 1) * sizeof(mr_station_t));
                mr_fav_count--;
                break;
            }
        }
        s->is_favorite = 0;
    } else {
        /* Add to favorites */
        if (mr_fav_count < MR_MAX_FAVORITES) {
            mr_favorites[mr_fav_count] = *s;
            mr_favorites[mr_fav_count].is_favorite = 1;
            mr_fav_count++;
            s->is_favorite = 1;
        }
    }
}

/* ---- Path Normalization (Linux <-> Windows) ---- */

/* Convert path to Windows format for file APIs.
 * Handles: ~/path -> Z:\home\user\path (Wine), /path -> Z:\path, forward slashes -> backslash
 * Input can be Linux or Windows format. */
static void mr_normalize_path(const char *input, char *out, int out_sz) {
    char tmp[MR_MAX_PATH_LEN];
    strncpy(tmp, input, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = 0;

    /* Expand ~ to HOME if it starts with ~/ or ~ alone */
    if (tmp[0] == '~' && (tmp[1] == '/' || tmp[1] == '\\' || tmp[1] == 0)) {
        const char *home = getenv("HOME");
        if (!home) home = getenv("USERPROFILE");
        if (home) {
            char expanded[MR_MAX_PATH_LEN];
            _snprintf(expanded, sizeof(expanded), "%s%s", home, tmp + 1);
            expanded[sizeof(expanded) - 1] = 0;
            strncpy(tmp, expanded, sizeof(tmp));
            tmp[sizeof(tmp) - 1] = 0;
        }
    }

    /* If it looks like a Linux absolute path (/home/..., /mnt/...) convert to Wine Z: drive */
    if (tmp[0] == '/' && (tmp[1] != '/' && tmp[1] != 0)) {
        char wine_path[MR_MAX_PATH_LEN];
        _snprintf(wine_path, sizeof(wine_path), "Z:%s", tmp);
        wine_path[sizeof(wine_path) - 1] = 0;
        strncpy(tmp, wine_path, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = 0;
    }

    /* Convert forward slashes to backslashes */
    for (int i = 0; tmp[i]; i++) {
        if (tmp[i] == '/') tmp[i] = '\\';
    }

    strncpy(out, tmp, out_sz);
    out[out_sz - 1] = 0;
}

/* Load all audio files from a directory into the playlist.
 * Supports: *.mp3, *.wav, *.ogg, *.flac */
static void mr_load_dir(const char *dir_path) {
    char norm[MR_MAX_PATH_LEN];
    mr_normalize_path(dir_path, norm, sizeof(norm));

    /* Ensure trailing backslash */
    int len = (int)strlen(norm);
    if (len > 0 && norm[len - 1] != '\\') {
        if (len < MR_MAX_PATH_LEN - 2) { norm[len] = '\\'; norm[len + 1] = 0; len++; }
    }

    /* Clear current playlist */
    mr_pl_count = 0;
    mr_pl_current = -1;

    /* Search for audio files */
    const char *exts[] = { "*.mp3", "*.wav", "*.ogg", "*.flac", NULL };
    for (int e = 0; exts[e] && mr_pl_count < MR_MAX_PLAYLIST; e++) {
        char pattern[MR_MAX_PATH_LEN];
        _snprintf(pattern, sizeof(pattern), "%s%s", norm, exts[e]);
        pattern[sizeof(pattern) - 1] = 0;

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(pattern, &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            mr_entry_t *ent = &mr_playlist[mr_pl_count];
            _snprintf(ent->path, sizeof(ent->path), "%s%s", norm, fd.cFileName);
            ent->path[sizeof(ent->path) - 1] = 0;
            /* Title = filename without extension */
            strncpy(ent->title, fd.cFileName, sizeof(ent->title));
            ent->title[sizeof(ent->title) - 1] = 0;
            char *dot = strrchr(ent->title, '.');
            if (dot) *dot = 0;
            ent->is_stream = 0;
            mr_pl_count++;
        } while (FindNextFileA(hFind, &fd) && mr_pl_count < MR_MAX_PLAYLIST);
        FindClose(hFind);
    }
}

/* ---- MMUDPy Exported Functions ---- */
/* These are __declspec(dllexport) so mmudpy can call them via ctypes */

__declspec(dllexport) void mr_cmd_show(void)   { mr_visible = 1; }
__declspec(dllexport) void mr_cmd_hide(void)   { mr_visible = 0; }
__declspec(dllexport) void mr_cmd_toggle(void) { mr_toggle(); }
__declspec(dllexport) void mr_cmd_play(void) {
    if (mr_transport == MR_STATE_PAUSED) mr_resume();
    else if (mr_pl_count > 0 && mr_transport == MR_STATE_STOPPED) {
        if (mr_pl_current < 0) mr_pl_current = 0;
        mr_play_file(mr_playlist[mr_pl_current].path);
    }
}
__declspec(dllexport) void mr_cmd_pause(void)  { mr_pause(); }
__declspec(dllexport) void mr_cmd_stop(void)   { mr_stop(); }
__declspec(dllexport) void mr_cmd_next(void)   { mr_next(); }
__declspec(dllexport) void mr_cmd_prev(void)   { mr_prev(); }

__declspec(dllexport) void mr_cmd_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    mr_volume = vol;
}
__declspec(dllexport) float mr_cmd_get_volume(void) { return mr_volume; }

__declspec(dllexport) void mr_cmd_shuffle(int on) { mr_shuffle = on ? 1 : 0; }
__declspec(dllexport) void mr_cmd_repeat(int mode) { mr_repeat = mode; } /* 0=off, 1=one, 2=all */

__declspec(dllexport) void mr_cmd_play_file(const char *path) {
    char norm[MR_MAX_PATH_LEN];
    mr_normalize_path(path, norm, sizeof(norm));
    mr_play_file(norm);
}

__declspec(dllexport) void mr_cmd_play_stream(const char *url) {
    mr_play_stream(url);
}

__declspec(dllexport) void mr_cmd_search(const char *query) {
    mr_do_search(query);
}

__declspec(dllexport) void mr_cmd_play_station(int idx) {
    mr_play_station(idx);
}

__declspec(dllexport) void mr_cmd_fav_toggle(int idx) {
    mr_toggle_favorite(idx);
}

__declspec(dllexport) void mr_cmd_load_dir(const char *dir) {
    mr_load_dir(dir);
}

__declspec(dllexport) int mr_cmd_playlist_count(void) { return mr_pl_count; }

__declspec(dllexport) const char *mr_cmd_now_playing(void) {
    static char np_buf[256];
    EnterCriticalSection(&mr_meta_lock);
    if (mr_meta.station_name[0])
        _snprintf(np_buf, sizeof(np_buf), "%s - %s", mr_meta.station_name, mr_meta.stream_title);
    else if (mr_meta.track[0])
        _snprintf(np_buf, sizeof(np_buf), "%s", mr_meta.track);
    else
        strcpy(np_buf, "");
    np_buf[sizeof(np_buf) - 1] = 0;
    LeaveCriticalSection(&mr_meta_lock);
    return np_buf;
}

__declspec(dllexport) const char *mr_cmd_status(void) {
    switch (mr_transport) {
    case MR_STATE_PLAYING:    return "playing";
    case MR_STATE_PAUSED:     return "paused";
    case MR_STATE_CONNECTING: return "connecting";
    case MR_STATE_ERROR:      return "error";
    default:                  return "stopped";
    }
}

/* ---- Init / Shutdown ---- */

static void mr_init(void) {
    InitializeCriticalSection(&mr_beat_lock);
    InitializeCriticalSection(&mr_meta_lock);
    memset(&mr_beat, 0, sizeof(mr_beat));
    memset(&mr_beat_snap, 0, sizeof(mr_beat_snap));
    memset(&mr_meta, 0, sizeof(mr_meta));

    mr_audio_running = 1;
    mr_audio_thread = CreateThread(NULL, 0, mr_audio_thread_fn, NULL, 0, NULL);
}

static void mr_shutdown(void) {
    mr_audio_running = 0;
    mr_transport = MR_STATE_STOPPED;
    if (mr_audio_thread) {
        WaitForSingleObject(mr_audio_thread, 3000);
        CloseHandle(mr_audio_thread);
        mr_audio_thread = NULL;
    }
    mr_close_audio();
    DeleteCriticalSection(&mr_beat_lock);
    DeleteCriticalSection(&mr_meta_lock);
    if (mr_wininet) { FreeLibrary(mr_wininet); mr_wininet = NULL; }
}
