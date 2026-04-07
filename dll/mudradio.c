/* mudradio.c — MUDRadio Audio Engine
 * Stream, decode, and play audio via miniaudio + HTTP streaming.
 * Included into vk_terminal.c
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include "kiss_fftr.h"
#include <math.h>

/* ---- Beat detection state ---- */
#define MR_FFT_SIZE   1024
static kiss_fftr_cfg mr_fft_cfg = NULL;
static float         mr_fft_buf[MR_FFT_SIZE];  /* mono accumulator */
static int           mr_fft_pos = 0;
static float         mr_prev_kick = 0;     /* previous kick energy for onset */
static float         mr_kick_avg  = 0;     /* running average of kick energy */
static DWORD         mr_last_onset_tick = 0; /* cooldown between onsets */
#define MR_ONSET_COOLDOWN_MS  120          /* min ms between beat triggers */

static void mr_analyze_audio(const float *pcm_stereo, int frames)
{
    if (!mr_fft_cfg) {
        mr_fft_cfg = kiss_fftr_alloc(MR_FFT_SIZE, 0, NULL, NULL);
        if (!mr_fft_cfg) return;
    }

    /* Downmix stereo to mono into accumulator */
    for (int i = 0; i < frames; i++) {
        mr_fft_buf[mr_fft_pos] = (pcm_stereo[i*2] + pcm_stereo[i*2+1]) * 0.5f;
        mr_fft_pos++;
        if (mr_fft_pos >= MR_FFT_SIZE) {
            /* Run FFT */
            kiss_fft_cpx freq[MR_FFT_SIZE / 2 + 1];
            kiss_fftr(mr_fft_cfg, mr_fft_buf, freq);

            int n_bins = MR_FFT_SIZE / 2 + 1;

            /* Kick drum band: 40-215 Hz (bins 1-5 at 44100/1024 ≈ 43Hz/bin) */
            float kick = 0;
            for (int b = 1; b <= 5 && b < n_bins; b++) {
                float mag = sqrtf(freq[b].r * freq[b].r + freq[b].i * freq[b].i);
                kick += mag;
            }
            kick /= 5.0f;

            /* Sub-bass: 0-40 Hz (bin 0) — rumble, not beats */
            float sub = sqrtf(freq[0].r * freq[0].r + freq[0].i * freq[0].i);

            /* Full bass: 0-300 Hz (bins 0-7) */
            float bass = 0;
            for (int b = 0; b < 7 && b < n_bins; b++) {
                float mag = sqrtf(freq[b].r * freq[b].r + freq[b].i * freq[b].i);
                bass += mag;
            }
            bass /= 7.0f;

            /* Mid: 300-4000 Hz (bins 7-93) */
            float mid = 0;
            for (int b = 7; b < 93 && b < n_bins; b++) {
                float mag = sqrtf(freq[b].r * freq[b].r + freq[b].i * freq[b].i);
                mid += mag;
            }
            mid /= 86.0f;

            /* Treble: 4000-16000 Hz (bins 93-372) */
            float treble = 0;
            for (int b = 93; b < 372 && b < n_bins; b++) {
                float mag = sqrtf(freq[b].r * freq[b].r + freq[b].i * freq[b].i);
                treble += mag;
            }
            treble /= 279.0f;

            /* Normalize — auto-scale: track peak and normalize against it */
            static float kick_peak = 1.0f;
            if (kick > kick_peak) kick_peak = kick;
            else kick_peak *= 0.9999f; /* slow decay so it adapts */
            if (kick_peak < 0.001f) kick_peak = 0.001f;
            float kick_n = kick / kick_peak; /* 0-1 relative to recent peak */

            static float bass_peak = 1.0f;
            if (bass > bass_peak) bass_peak = bass;
            else bass_peak *= 0.9999f;
            if (bass_peak < 0.001f) bass_peak = 0.001f;
            float bass_n = bass / bass_peak;
            float mid_n    = mid    * 0.04f;
            float treble_n = treble * 0.08f;
            if (kick_n > 1.0f) kick_n = 1.0f;
            if (bass_n > 1.0f) bass_n = 1.0f;
            if (mid_n > 1.0f) mid_n = 1.0f;
            if (treble_n > 1.0f) treble_n = 1.0f;

            /* Running average for adaptive threshold */
            mr_kick_avg = mr_kick_avg * 0.92f + kick_n * 0.08f;

            /* Onset detection: kick-only, must exceed running average by 40%+ */
            DWORD now = GetTickCount();
            int onset = 0;
            float thresh = mr_kick_avg * 1.4f;
            if (thresh < 0.12f) thresh = 0.12f; /* minimum absolute threshold */

            if (kick_n > thresh && kick_n > mr_prev_kick * 1.3f &&
                (now - mr_last_onset_tick) > MR_ONSET_COOLDOWN_MS) {
                onset = 1;
                mr_last_onset_tick = now;
            }

            /* Update beat snapshot */
            EnterCriticalSection(&mr_beat_lock);
            mr_beat_snap.bass_energy   = bass_n;
            mr_beat_snap.mid_energy    = mid_n;
            mr_beat_snap.treble_energy = treble_n;
            mr_beat_snap.onset_detected = onset;
            if (onset) mr_beat_snap.beat_count++;
            mr_beat_snap.onset_strength = kick_n;
            mr_beat_snap.tick = now;
            LeaveCriticalSection(&mr_beat_lock);

            mr_prev_kick = kick_n;

            mr_fft_pos = 0;
        }
    }
}

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

/* ---- FAAD2 AAC decoder ---- */
#define NEAACDECAPI   /* strip dllexport — static link, don't export AAC symbols */
#include "neaacdec.h"
#undef NEAACDECAPI

/* ---- Opus decoder (via opusfile) ---- */
#include "opus_inc/opusfile.h"

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
 * During init (device not started): blocks briefly waiting for data.
 * During playback (device started): returns immediately with whatever is available
 * so the audio thread never stalls. */
static ma_result mr_ma_read_cb(ma_decoder *pDecoder, void *pBufferOut, size_t bytesToRead, size_t *pBytesRead)
{
    (void)pDecoder;
    unsigned char *out = (unsigned char *)pBufferOut;
    size_t total = 0;
    int max_spins = mr_ma_dev_started ? 0 : 200; /* non-blocking during playback */
    int spins = 0;

    while (total < bytesToRead) {
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
            if (++spins > max_spins) break;
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

    /* Beat analysis on raw decoded audio (before volume) */
    if (frames_read > 0)
        mr_analyze_audio(out, (int)frames_read);

    /* Apply volume */
    float vol = mr_volume;
    for (ma_uint64 i = 0; i < frames_read * 2; i++)
        out[i] *= vol;

    /* Silence-fill remainder */
    if (frames_read < frameCount)
        memset(out + frames_read * 2, 0, (frameCount - frames_read) * 2 * sizeof(float));
}

static volatile int mr_dec_inited = 0;

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

/* ==== AAC (ADTS) custom decoding backend for miniaudio via FAAD2 ==== */

#define AAC_INBUF_SZ  (8192 * 4)   /* read-ahead buffer for ADTS frames */
#define AAC_PCMBUF_SZ (4096 * 2)   /* decoded PCM float buffer (stereo frames) */

typedef struct {
    ma_data_source_base ds_base;   /* MUST be first — miniaudio casts to this */
    /* Stream I/O callbacks (from miniaudio) */
    ma_read_proc  onRead;
    ma_seek_proc  onSeek;
    ma_tell_proc  onTell;
    void         *pReadSeekTellUserData;
    /* FAAD2 state */
    NeAACDecHandle hDec;
    unsigned long  aac_sr;
    unsigned char  aac_ch;
    int            inited;
    /* ADTS raw byte buffer */
    unsigned char  inbuf[AAC_INBUF_SZ];
    int            inbuf_fill;       /* bytes available */
    /* Decoded PCM output ring */
    float          pcmbuf[AAC_PCMBUF_SZ];
    int            pcm_frames;       /* total frames in pcmbuf */
    int            pcm_pos;          /* read position in frames */
} mr_aac_dec_t;

/* Find ADTS sync word (0xFFF) in buffer, return offset or -1 */
static int mr_aac_find_sync(const unsigned char *buf, int len) {
    for (int i = 0; i < len - 1; i++) {
        if (buf[i] == 0xFF && (buf[i+1] & 0xF0) == 0xF0) return i;
    }
    return -1;
}

/* Get ADTS frame length from header (13 bits across bytes 3-5) */
static int mr_aac_frame_len(const unsigned char *hdr) {
    return ((int)(hdr[3] & 0x03) << 11) | ((int)hdr[4] << 3) | ((int)(hdr[5] >> 5) & 0x07);
}

/* Pull more raw bytes from the stream into inbuf */
static int mr_aac_fill_inbuf(mr_aac_dec_t *d) {
    if (d->inbuf_fill >= AAC_INBUF_SZ) return d->inbuf_fill;
    int want = AAC_INBUF_SZ - d->inbuf_fill;
    size_t got = 0;
    ma_result rc = d->onRead(d->pReadSeekTellUserData, d->inbuf + d->inbuf_fill, want, &got);
    if (got > 0) d->inbuf_fill += (int)got;
    return d->inbuf_fill;
}

/* Shift consumed bytes out of inbuf */
static void mr_aac_consume(mr_aac_dec_t *d, int bytes) {
    if (bytes <= 0) return;
    if (bytes >= d->inbuf_fill) { d->inbuf_fill = 0; return; }
    memmove(d->inbuf, d->inbuf + bytes, d->inbuf_fill - bytes);
    d->inbuf_fill -= bytes;
}

/* Decode one ADTS frame, put float32 stereo PCM into pcmbuf. Returns frames decoded. */
static int mr_aac_decode_frame(mr_aac_dec_t *d) {
    /* Ensure we have enough data */
    mr_aac_fill_inbuf(d);
    if (d->inbuf_fill < 7) return 0; /* need at least ADTS header */

    /* Find sync */
    int sync = mr_aac_find_sync(d->inbuf, d->inbuf_fill);
    if (sync < 0) { d->inbuf_fill = 0; return 0; }
    if (sync > 0) mr_aac_consume(d, sync);
    if (d->inbuf_fill < 7) return 0;

    int flen = mr_aac_frame_len(d->inbuf);
    if (flen < 7 || flen > AAC_INBUF_SZ) { mr_aac_consume(d, 1); return 0; }
    if (d->inbuf_fill < flen) {
        mr_aac_fill_inbuf(d);
        if (d->inbuf_fill < flen) return 0; /* not enough data yet */
    }

    /* Init FAAD2 on first valid frame */
    if (!d->inited) {
        long rc = NeAACDecInit(d->hDec, d->inbuf, d->inbuf_fill, &d->aac_sr, &d->aac_ch);
        if (rc < 0) { mr_aac_consume(d, 1); return 0; }
        /* NeAACDecInit returns bytes consumed for init */
        d->inited = 1;
        /* Don't consume here — the frame is still valid, decode it */
    }

    NeAACDecFrameInfo info;
    void *samples = NeAACDecDecode(d->hDec, &info, d->inbuf, flen);
    mr_aac_consume(d, (info.bytesconsumed > 0) ? (int)info.bytesconsumed : flen);

    if (info.error || !samples || info.samples == 0) return 0;

    /* Convert to float32 stereo into pcmbuf */
    int out_frames = info.samples / info.channels;
    if (out_frames > AAC_PCMBUF_SZ / 2) out_frames = AAC_PCMBUF_SZ / 2;
    float *src = (float *)samples;

    if (info.channels == 1) {
        /* Mono → stereo */
        for (int i = 0; i < out_frames; i++) {
            d->pcmbuf[i*2]   = src[i];
            d->pcmbuf[i*2+1] = src[i];
        }
    } else if (info.channels == 2) {
        memcpy(d->pcmbuf, src, out_frames * 2 * sizeof(float));
    } else {
        /* Downmix to stereo: just take first two channels */
        for (int i = 0; i < out_frames; i++) {
            d->pcmbuf[i*2]   = src[i * info.channels];
            d->pcmbuf[i*2+1] = src[i * info.channels + 1];
        }
    }
    d->pcm_frames = out_frames;
    d->pcm_pos = 0;
    return out_frames;
}

/* ---- ma_data_source vtable for AAC ---- */

static ma_result mr_aac_ds_read(ma_data_source *pDS, void *pFramesOut, ma_uint64 frameCount, ma_uint64 *pFramesRead) {
    mr_aac_dec_t *d = (mr_aac_dec_t *)pDS;
    float *out = (float *)pFramesOut;
    ma_uint64 total = 0;

    while (total < frameCount) {
        /* Drain pcmbuf first */
        int avail = d->pcm_frames - d->pcm_pos;
        if (avail > 0) {
            int want = (int)(frameCount - total);
            if (want > avail) want = avail;
            memcpy(out + total * 2, d->pcmbuf + d->pcm_pos * 2, want * 2 * sizeof(float));
            d->pcm_pos += want;
            total += want;
        } else {
            /* Decode next frame */
            if (mr_aac_decode_frame(d) <= 0) break;
        }
    }
    *pFramesRead = total;
    return (total > 0) ? MA_SUCCESS : MA_AT_END;
}

static ma_result mr_aac_ds_seek(ma_data_source *pDS, ma_uint64 frameIndex) {
    (void)pDS; (void)frameIndex;
    return MA_SUCCESS; /* streams can't seek */
}

static ma_result mr_aac_ds_get_format(ma_data_source *pDS, ma_format *pFormat, ma_uint32 *pChannels, ma_uint32 *pSampleRate, ma_channel *pChannelMap, size_t channelMapCap) {
    mr_aac_dec_t *d = (mr_aac_dec_t *)pDS;
    if (pFormat) *pFormat = ma_format_f32;
    if (pChannels) *pChannels = 2; /* always stereo output */
    if (pSampleRate) *pSampleRate = d->aac_sr ? d->aac_sr : 44100;
    if (pChannelMap) {
        if (channelMapCap >= 2) {
            pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
            pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
        }
    }
    return MA_SUCCESS;
}

static ma_result mr_aac_ds_get_cursor(ma_data_source *pDS, ma_uint64 *pCursor) {
    (void)pDS; *pCursor = 0; return MA_SUCCESS;
}

static ma_result mr_aac_ds_get_length(ma_data_source *pDS, ma_uint64 *pLength) {
    (void)pDS; *pLength = 0; return MA_SUCCESS; /* unknown length for streams */
}

static ma_data_source_vtable mr_aac_ds_vtable = {
    mr_aac_ds_read,
    mr_aac_ds_seek,
    mr_aac_ds_get_format,
    mr_aac_ds_get_cursor,
    mr_aac_ds_get_length,
    NULL, /* onSetLooping */
    0     /* flags */
};

/* ---- miniaudio decoding backend vtable ---- */

static ma_result mr_aac_backend_init(
    void *pUserData,
    ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
    void *pReadSeekTellUserData,
    const ma_decoding_backend_config *pConfig,
    const ma_allocation_callbacks *pAllocationCallbacks,
    ma_data_source **ppBackend)
{
    (void)pUserData; (void)pConfig;
    mr_aac_dec_t *d = (mr_aac_dec_t *)ma_malloc(sizeof(mr_aac_dec_t), pAllocationCallbacks);
    if (!d) return MA_OUT_OF_MEMORY;
    memset(d, 0, sizeof(*d));

    /* Init data source base */
    ma_data_source_config ds_cfg = ma_data_source_config_init();
    ds_cfg.vtable = &mr_aac_ds_vtable;
    ma_result rc = ma_data_source_init(&ds_cfg, &d->ds_base);
    if (rc != MA_SUCCESS) { ma_free(d, pAllocationCallbacks); return rc; }

    d->onRead = onRead;
    d->onSeek = onSeek;
    d->onTell = onTell;
    d->pReadSeekTellUserData = pReadSeekTellUserData;

    /* Open FAAD2 decoder */
    d->hDec = NeAACDecOpen();
    if (!d->hDec) { ma_free(d, pAllocationCallbacks); return MA_ERROR; }

    /* Configure for float output */
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(d->hDec);
    cfg->outputFormat = FAAD_FMT_FLOAT;
    cfg->defSampleRate = 44100;
    cfg->defObjectType = LC;
    cfg->downMatrix = 0;
    NeAACDecSetConfiguration(d->hDec, cfg);

    /* Read initial data and verify it's ADTS */
    mr_aac_fill_inbuf(d);
    int sync = mr_aac_find_sync(d->inbuf, d->inbuf_fill);
    if (sync < 0) {
        /* Not AAC — let miniaudio try other decoders */
        NeAACDecClose(d->hDec);
        ma_data_source_uninit(&d->ds_base);
        ma_free(d, pAllocationCallbacks);
        return MA_INVALID_FILE;
    }
    if (sync > 0) mr_aac_consume(d, sync);

    /* Init FAAD2 with the ADTS header */
    long init_rc = NeAACDecInit(d->hDec, d->inbuf, d->inbuf_fill, &d->aac_sr, &d->aac_ch);
    if (init_rc < 0) {
        FILE *ef = fopen("C:\\MegaMUD\\radio_error.txt", "a");
        if (ef) { fprintf(ef, "AAC: NeAACDecInit failed rc=%ld\n", init_rc); fclose(ef); }
        NeAACDecClose(d->hDec);
        ma_data_source_uninit(&d->ds_base);
        ma_free(d, pAllocationCallbacks);
        return MA_INVALID_FILE;
    }
    d->inited = 1;

    {
        FILE *ef = fopen("C:\\MegaMUD\\radio_error.txt", "a");
        if (ef) { fprintf(ef, "AAC: init OK sr=%lu ch=%u\n", d->aac_sr, d->aac_ch); fclose(ef); }
    }

    *ppBackend = (ma_data_source *)d;
    return MA_SUCCESS;
}

static void mr_aac_backend_uninit(void *pUserData, ma_data_source *pBackend, const ma_allocation_callbacks *pAllocationCallbacks) {
    (void)pUserData;
    mr_aac_dec_t *d = (mr_aac_dec_t *)pBackend;
    if (d->hDec) NeAACDecClose(d->hDec);
    ma_data_source_uninit(&d->ds_base);
    ma_free(d, pAllocationCallbacks);
}

static ma_decoding_backend_vtable mr_aac_backend_vtable = {
    mr_aac_backend_init,
    NULL, /* onInitFile */
    NULL, /* onInitFileW */
    NULL, /* onInitMemory */
    mr_aac_backend_uninit
};

/* ==== End AAC backend ==== */

/* ==== Opus (Ogg Opus) custom decoding backend via opusfile ==== */

typedef struct {
    ma_data_source_base ds_base;   /* MUST be first */
    ma_read_proc  onRead;
    ma_seek_proc  onSeek;
    ma_tell_proc  onTell;
    void         *pReadSeekTellUserData;
    OggOpusFile  *of;              /* opusfile handle */
    int           channels;
    opus_int32    sample_rate;     /* always 48000 for Opus */
    /* Pre-read buffer for format detection */
    unsigned char prebuf[8192];
    int           prebuf_fill;
    int           prebuf_pos;      /* how much of prebuf consumed by opusfile */
} mr_opus_dec_t;

/* ---- opusfile I/O callbacks ---- */

static int mr_opus_read_cb(void *_stream, unsigned char *_ptr, int _nbytes) {
    mr_opus_dec_t *d = (mr_opus_dec_t *)_stream;
    int total = 0;
    /* Drain prebuf first (used during op_open_callbacks) */
    if (d->prebuf_pos < d->prebuf_fill) {
        int avail = d->prebuf_fill - d->prebuf_pos;
        int take = (_nbytes < avail) ? _nbytes : avail;
        memcpy(_ptr, d->prebuf + d->prebuf_pos, take);
        d->prebuf_pos += take;
        _ptr += take;
        _nbytes -= take;
        total += take;
    }
    /* Read rest from stream */
    if (_nbytes > 0) {
        size_t got = 0;
        d->onRead(d->pReadSeekTellUserData, _ptr, _nbytes, &got);
        total += (int)got;
    }
    return total;
}

static int mr_opus_seek_cb(void *_stream, opus_int64 _offset, int _whence) {
    (void)_stream; (void)_offset; (void)_whence;
    return -1; /* unseekable stream */
}

static int mr_opus_close_cb(void *_stream) {
    (void)_stream;
    return 0; /* we handle cleanup ourselves */
}

static OpusFileCallbacks mr_opus_file_cbs = {
    mr_opus_read_cb,
    mr_opus_seek_cb,
    NULL, /* tell — NULL for unseekable */
    mr_opus_close_cb
};

/* ---- ma_data_source vtable for Opus ---- */

static ma_result mr_opus_ds_read(ma_data_source *pDS, void *pFramesOut, ma_uint64 frameCount, ma_uint64 *pFramesRead) {
    mr_opus_dec_t *d = (mr_opus_dec_t *)pDS;
    float *out = (float *)pFramesOut;
    ma_uint64 total = 0;

    while (total < frameCount) {
        int want = (int)(frameCount - total);
        if (want > 960) want = 960; /* Opus max frame size */
        /* op_read_float_stereo always outputs stereo interleaved float */
        int got = op_read_float_stereo(d->of, out + total * 2, want * 2);
        if (got <= 0) break; /* error or EOF */
        total += got;
    }
    *pFramesRead = total;
    return (total > 0) ? MA_SUCCESS : MA_AT_END;
}

static ma_result mr_opus_ds_seek(ma_data_source *pDS, ma_uint64 frameIndex) {
    (void)pDS; (void)frameIndex;
    return MA_SUCCESS;
}

static ma_result mr_opus_ds_get_format(ma_data_source *pDS, ma_format *pFormat, ma_uint32 *pChannels, ma_uint32 *pSampleRate, ma_channel *pChannelMap, size_t channelMapCap) {
    (void)pDS;
    if (pFormat)     *pFormat = ma_format_f32;
    if (pChannels)   *pChannels = 2;
    if (pSampleRate) *pSampleRate = 48000; /* Opus always decodes at 48kHz */
    if (pChannelMap && channelMapCap >= 2) {
        pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
        pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
    }
    return MA_SUCCESS;
}

static ma_result mr_opus_ds_get_cursor(ma_data_source *pDS, ma_uint64 *pCursor) {
    (void)pDS; *pCursor = 0; return MA_SUCCESS;
}

static ma_result mr_opus_ds_get_length(ma_data_source *pDS, ma_uint64 *pLength) {
    (void)pDS; *pLength = 0; return MA_SUCCESS;
}

static ma_data_source_vtable mr_opus_ds_vtable = {
    mr_opus_ds_read,
    mr_opus_ds_seek,
    mr_opus_ds_get_format,
    mr_opus_ds_get_cursor,
    mr_opus_ds_get_length,
    NULL, 0
};

/* ---- Opus miniaudio decoding backend ---- */

static ma_result mr_opus_backend_init(
    void *pUserData,
    ma_read_proc onRead, ma_seek_proc onSeek, ma_tell_proc onTell,
    void *pReadSeekTellUserData,
    const ma_decoding_backend_config *pConfig,
    const ma_allocation_callbacks *pAllocationCallbacks,
    ma_data_source **ppBackend)
{
    (void)pUserData; (void)pConfig;
    mr_opus_dec_t *d = (mr_opus_dec_t *)ma_malloc(sizeof(mr_opus_dec_t), pAllocationCallbacks);
    if (!d) return MA_OUT_OF_MEMORY;
    memset(d, 0, sizeof(*d));

    ma_data_source_config ds_cfg = ma_data_source_config_init();
    ds_cfg.vtable = &mr_opus_ds_vtable;
    ma_result rc = ma_data_source_init(&ds_cfg, &d->ds_base);
    if (rc != MA_SUCCESS) { ma_free(d, pAllocationCallbacks); return rc; }

    d->onRead = onRead;
    d->onSeek = onSeek;
    d->onTell = onTell;
    d->pReadSeekTellUserData = pReadSeekTellUserData;

    /* Pre-read initial data for Ogg page detection.
     * opusfile's op_open_callbacks takes initial_data + initial_bytes which
     * it processes FIRST before calling our read callback, so we must NOT
     * also serve this data from the read callback. Set prebuf_pos = prebuf_fill
     * before calling op_open_callbacks so the read callback skips past prebuf. */
    size_t got = 0;
    onRead(pReadSeekTellUserData, d->prebuf, sizeof(d->prebuf), &got);
    d->prebuf_fill = (int)got;
    d->prebuf_pos = d->prebuf_fill; /* already consumed by initial_data param */

    /* Check for OggS magic */
    if (d->prebuf_fill < 4 ||
        d->prebuf[0] != 'O' || d->prebuf[1] != 'g' ||
        d->prebuf[2] != 'g' || d->prebuf[3] != 'S') {
        ma_data_source_uninit(&d->ds_base);
        ma_free(d, pAllocationCallbacks);
        return MA_INVALID_FILE;
    }

    /* Open with opusfile — initial_data avoids seeking back to start */
    int err = 0;
    d->of = op_open_callbacks(d, &mr_opus_file_cbs,
                              d->prebuf, d->prebuf_fill, &err);
    if (!d->of) {
        FILE *ef = fopen("C:\\MegaMUD\\radio_error.txt", "a");
        if (ef) { fprintf(ef, "Opus: op_open_callbacks failed err=%d prebuf=%d\n", err, d->prebuf_fill); fclose(ef); }
        ma_data_source_uninit(&d->ds_base);
        ma_free(d, pAllocationCallbacks);
        return MA_INVALID_FILE;
    }

    const OpusHead *head = op_head(d->of, -1);
    d->channels = head ? head->channel_count : 2;
    d->sample_rate = 48000; /* Opus always decodes at 48kHz */

    {
        FILE *ef = fopen("C:\\MegaMUD\\radio_error.txt", "a");
        if (ef) { fprintf(ef, "Opus: init OK ch=%d sr=%d\n", d->channels, d->sample_rate); fclose(ef); }
    }

    *ppBackend = (ma_data_source *)d;
    return MA_SUCCESS;
}

static void mr_opus_backend_uninit(void *pUserData, ma_data_source *pBackend, const ma_allocation_callbacks *pAllocationCallbacks) {
    (void)pUserData;
    mr_opus_dec_t *d = (mr_opus_dec_t *)pBackend;
    if (d->of) op_free(d->of);
    ma_data_source_uninit(&d->ds_base);
    ma_free(d, pAllocationCallbacks);
}

static ma_decoding_backend_vtable mr_opus_backend_vtable = {
    mr_opus_backend_init,
    NULL, NULL, NULL,
    mr_opus_backend_uninit
};

/* ==== End Opus backend ==== */

static int mr_open_decoder(void) {
    ma_decoding_backend_vtable *custom_backends[] = { &mr_opus_backend_vtable, &mr_aac_backend_vtable };
    ma_decoder_config dec_cfg = ma_decoder_config_init(ma_format_f32, 2, MR_SAMPLE_RATE);
    dec_cfg.ppCustomBackendVTables = custom_backends;
    dec_cfg.customBackendCount = 2;
    /* Try built-in decoders first (mp3/flac/wav/vorbis), then Opus, then AAC */
    ma_result rc = ma_decoder_init(mr_ma_read_cb, mr_ma_seek_cb, NULL, &dec_cfg, &mr_ma_dec);
    if (rc != MA_SUCCESS) {
        FILE *f = fopen("C:\\MegaMUD\\radio_error.txt", "a");
        if (f) {
            /* Log the first bytes to help identify unknown formats */
            int avail = mr_net_avail();
            fprintf(f, "ma_decoder_init FAILED: rc=%d, net_avail=%d, first bytes:", rc, avail);
            int peek = (avail > 32) ? 32 : avail;
            for (int i = 0; i < peek; i++)
                fprintf(f, " %02X", mr_net_ring[(mr_net_r + i) % MR_NET_RING_SZ]);
            /* Also print as ASCII for content-type hints */
            fprintf(f, " \"");
            for (int i = 0; i < peek; i++) {
                unsigned char b = mr_net_ring[(mr_net_r + i) % MR_NET_RING_SZ];
                fprintf(f, "%c", (b >= 32 && b < 127) ? b : '.');
            }
            fprintf(f, "\"\n");
            fclose(f);
        }
        return 0;
    }
    mr_dec_inited = 1;
    mr_ma_dec_ready = 1;
    FILE *f = fopen("C:\\MegaMUD\\radio_error.txt", "a");
    if (f) { fprintf(f, "decoder OK, net_avail=%d\n", mr_net_avail()); fclose(f); }
    return 1;
}

static void mr_close_audio(void) {
    EnterCriticalSection(&mr_close_lock);
    FILE *f = fopen("C:\\MegaMUD\\radio_error.txt", "a");
    if (f) fprintf(f, "close_audio: dec_ready=%d dev_started=%d dec_inited=%d\n",
                   mr_ma_dec_ready, mr_ma_dev_started, mr_dec_inited);

    /* 1. Tell callback to output silence */
    mr_ma_dec_ready = 0;

    /* 2. Stop device FIRST — this guarantees callback is no longer running */
    if (mr_ma_dev_started) {
        if (f) fprintf(f, "  stopping device...\n");
        ma_device_stop(&mr_ma_dev);
        if (f) fprintf(f, "  uninit device...\n");
        ma_device_uninit(&mr_ma_dev);
        mr_ma_dev_started = 0;
        if (f) fprintf(f, "  device done\n");
    }

    /* 3. NOW safe to uninit decoder — no callback can touch it */
    if (mr_dec_inited) {
        if (f) fprintf(f, "  uninit decoder...\n");
        ma_decoder_uninit(&mr_ma_dec);
        mr_dec_inited = 0;
        if (f) fprintf(f, "  decoder done\n");
    }

    if (f) { fprintf(f, "close_audio: OK\n"); fclose(f); }
    LeaveCriticalSection(&mr_close_lock);
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
                /* Log content-type for format debugging */
                const char *ct = strstr(hdrs_lower, "content-type:");
                if (ct && mr_dbg) {
                    ct += 13;
                    while (*ct == ' ') ct++;
                    char ct_val[128]; int j = 0;
                    while (ct[j] && ct[j] != '\r' && ct[j] != '\n' && j < 127) { ct_val[j] = ct[j]; j++; }
                    ct_val[j] = 0;
                    fprintf(mr_dbg, "content-type: %s\n", ct_val); fflush(mr_dbg);
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

    /* Wait for enough MP3 data in net ring before starting decoder (~128KB) */
    while (mr_net_avail() < 131072 && mr_net_running && mr_audio_running)
        Sleep(20);

    mr_transport = MR_STATE_PLAYING;

    /* Open miniaudio decoder — reads from net ring via callback */
    if (!mr_net_running || !mr_audio_running) goto cleanup;
    if (!mr_open_decoder()) {
        mr_transport = MR_STATE_ERROR;
        goto cleanup;
    }

    /* Wait for net ring to refill after decoder init consumed data for format detection */
    while (mr_net_avail() < 65536 && mr_net_running && mr_audio_running)
        Sleep(20);

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
            char prev_url[MR_MAX_PATH_LEN];
            strncpy(prev_url, mr_src_path, MR_MAX_PATH_LEN);
            prev_url[MR_MAX_PATH_LEN - 1] = 0;
            mr_stream_radio();
            /* Only mark stopped if stream ended naturally (not switched to new station) */
            if (mr_audio_running && mr_transport != MR_STATE_STOPPED &&
                strcmp(mr_src_path, prev_url) == 0) {
                mr_transport = MR_STATE_STOPPED;
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
            mr_dec_inited = 1;
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
    /* Wait for audio thread to see stop signal and exit stream_radio */
    Sleep(200);
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
    InitializeCriticalSection(&mr_close_lock);
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
    if (mr_fft_cfg) { kiss_fftr_free(mr_fft_cfg); mr_fft_cfg = NULL; }
    DeleteCriticalSection(&mr_beat_lock);
    DeleteCriticalSection(&mr_meta_lock);
    DeleteCriticalSection(&mr_close_lock);
    if (mr_wininet) { FreeLibrary(mr_wininet); mr_wininet = NULL; }
}
