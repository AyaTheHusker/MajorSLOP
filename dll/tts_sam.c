/*
 * tts_sam.c -SAM (Software Automatic Mouth) TTS Plugin with DSP
 *
 * Classic 1982 C64-style text-to-speech with:
 *   - Lowpass filter (single-pole IIR)
 *   - Schroeder reverb (4 comb + 2 allpass filters)
 *   - Stereo ping-pong delay
 *
 * All DSP parameters are exposed as exported functions for MMUDPy control.
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o TTS_SAM.dll tts_sam.c \
 *       sam/sam.c sam/reciter.c sam/render.c sam/debug.c \
 *       -lwinmm -lgdi32 -luser32
 *
 * Install:
 *   Copy TTS_SAM.dll to MegaMUD/plugins/
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* SAM source */
int debug = 0;  /* SAM debug global */
#include "sam/sam.h"
#include "sam/reciter.h"

static const slop_api_t *api = NULL;

/* Menu IDs */
#define IDM_SAM_TEST    50300

#define SAMPLE_RATE 22050
#define OUTPUT_RATE 44100

/* ---- DSP Parameters (exported for MMUDPy ctypes) ---- */

/* Lowpass: cutoff 0.0 = max filtering, 1.0 = bypass */
static float lp_cutoff = 0.35f;
static int   lp_enabled = 1;

/* Reverb: Schroeder-style */
static float rv_length   = 0.6f;   /* room size 0.0-1.0 (scales comb delays) */
static float rv_predelay = 20.0f;  /* ms before reverb starts */
static float rv_wet      = 0.25f;  /* wet/dry mix 0.0-1.0 */
static float rv_damping  = 0.4f;   /* high-freq damping in reverb 0.0-1.0 */
static int   rv_enabled  = 0;      /* off by default */

/* Stereo delay */
static float dl_left_ms  = 250.0f; /* left channel delay time in ms */
static float dl_right_ms = 375.0f; /* right channel delay time in ms */
static float dl_feedback = 0.35f;  /* feedback amount 0.0-0.95 */
static float dl_wet      = 0.3f;   /* wet/dry mix 0.0-1.0 */
static int   dl_enabled  = 0;      /* off by default */

/* Stereo pan: -1.0 = full left, 0.0 = center, 1.0 = full right */
static float pan_value = 0.0f;

/* Volume: 0.0 = silent, 1.0 = full (default) */
static float volume = 1.0f;

/* SAM voice shape (defaults from original SAM) */
static int sam_mouth_val  = 128;  /* 0-255 */
static int sam_throat_val = 128;  /* 0-255 */
static int sam_pitch_def  = 64;   /* default pitch if say() gets 0 */
static int sam_speed_def  = 72;   /* default speed if say() gets 0 */

/* ---- Exported settings functions for MMUDPy ---- */

#define EXPORT __declspec(dllexport)

EXPORT void sam_set_lowpass(float cutoff, int enabled) {
    if (cutoff < 0.0f) cutoff = 0.0f;
    if (cutoff > 1.0f) cutoff = 1.0f;
    lp_cutoff = cutoff;
    lp_enabled = enabled;
    if (api) api->log("[TTS_SAM] Lowpass: cutoff=%.2f enabled=%d\n", cutoff, enabled);
}

EXPORT void sam_set_reverb(float length, float predelay, float wet, float damping, int enabled) {
    if (length < 0.0f) length = 0.0f;
    if (length > 1.0f) length = 1.0f;
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 1.0f) wet = 1.0f;
    if (damping < 0.0f) damping = 0.0f;
    if (damping > 1.0f) damping = 1.0f;
    rv_length = length;
    rv_predelay = predelay;
    rv_wet = wet;
    rv_damping = damping;
    rv_enabled = enabled;
    if (api) api->log("[TTS_SAM] Reverb: length=%.2f predelay=%.0fms wet=%.2f damp=%.2f enabled=%d\n",
                       length, predelay, wet, damping, enabled);
}

EXPORT void sam_set_delay(float left_ms, float right_ms, float feedback, float wet, int enabled) {
    if (feedback < 0.0f) feedback = 0.0f;
    if (feedback > 0.95f) feedback = 0.95f;
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 1.0f) wet = 1.0f;
    dl_left_ms = left_ms;
    dl_right_ms = right_ms;
    dl_feedback = feedback;
    dl_wet = wet;
    dl_enabled = enabled;
    if (api) api->log("[TTS_SAM] Delay: L=%.0fms R=%.0fms fb=%.2f wet=%.2f enabled=%d\n",
                       left_ms, right_ms, feedback, wet, enabled);
}

EXPORT void sam_set_pan(float pan) {
    if (pan < -1.0f) pan = -1.0f;
    if (pan > 1.0f) pan = 1.0f;
    pan_value = pan;
    if (api) api->log("[TTS_SAM] Pan: %.2f\n", pan);
}

EXPORT void sam_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 2.0f) vol = 2.0f;  /* allow slight boost */
    volume = vol;
    if (api) api->log("[TTS_SAM] Volume: %.2f\n", vol);
}

EXPORT void sam_set_voice(int mouth, int throat, int pitch, int speed) {
    if (mouth >= 0 && mouth <= 255) sam_mouth_val = mouth;
    if (throat >= 0 && throat <= 255) sam_throat_val = throat;
    if (pitch >= 0 && pitch <= 255) sam_pitch_def = pitch;
    if (speed >= 0 && speed <= 255) sam_speed_def = speed;
    if (api) api->log("[TTS_SAM] Voice: mouth=%d throat=%d pitch=%d speed=%d\n",
                       sam_mouth_val, sam_throat_val, sam_pitch_def, sam_speed_def);
}

/* Returns settings as a formatted string (for MMUDPy display) */
EXPORT int sam_get_settings(char *out, int out_sz) {
    return snprintf(out, out_sz,
        "SAM DSP Settings:\n"
        "  Voice:    mouth=%d throat=%d pitch=%d speed=%d\n"
        "  Volume:   %.2f\n"
        "  Pan:      %.2f (%s)\n"
        "  Lowpass:  cutoff=%.2f enabled=%d\n"
        "  Reverb:   length=%.2f predelay=%.0fms wet=%.2f damping=%.2f enabled=%d\n"
        "  Delay:    L=%.0fms R=%.0fms feedback=%.2f wet=%.2f enabled=%d\n",
        sam_mouth_val, sam_throat_val, sam_pitch_def, sam_speed_def,
        volume,
        pan_value, pan_value < -0.01f ? "left" : pan_value > 0.01f ? "right" : "center",
        lp_cutoff, lp_enabled,
        rv_length, rv_predelay, rv_wet, rv_damping, rv_enabled,
        dl_left_ms, dl_right_ms, dl_feedback, dl_wet, dl_enabled);
}

/* ---- 2x Linear interpolation upsampler ---- */
static float *upsample_2x(float *in, int in_len, int *out_len)
{
    *out_len = in_len * 2;
    float *out = (float *)malloc(*out_len * sizeof(float));
    for (int i = 0; i < in_len; i++) {
        float s = in[i];
        float next = (i + 1 < in_len) ? in[i + 1] : s;
        out[i * 2]     = s;
        out[i * 2 + 1] = (s + next) * 0.5f;
    }
    return out;
}

/* ---- DSP Processing (all at OUTPUT_RATE) ---- */

/* Single-pole lowpass filter */
static void dsp_lowpass(float *buf, int len, float cutoff)
{
    /* alpha maps cutoff (0-1) to filter coefficient.
       Lower alpha = more filtering. cutoff=1 bypasses. */
    float alpha = cutoff * cutoff;  /* quadratic curve feels more musical */
    if (alpha > 0.99f) return;      /* bypass */
    if (alpha < 0.001f) alpha = 0.001f;

    float prev = buf[0];
    for (int i = 1; i < len; i++) {
        buf[i] = prev + alpha * (buf[i] - prev);
        prev = buf[i];
    }
}

/* Comb filter for reverb */
typedef struct {
    float *buf;
    int    len;
    int    pos;
    float  fb;
    float  damp;
    float  damp_prev;
} comb_t;

static void comb_init(comb_t *c, int delay_samples, float feedback, float damping)
{
    c->len = delay_samples;
    c->buf = (float *)calloc(delay_samples, sizeof(float));
    c->pos = 0;
    c->fb = feedback;
    c->damp = damping;
    c->damp_prev = 0.0f;
}

static float comb_process(comb_t *c, float input)
{
    float output = c->buf[c->pos];
    /* Damped feedback -lowpass in the feedback loop */
    c->damp_prev = output * (1.0f - c->damp) + c->damp_prev * c->damp;
    c->buf[c->pos] = input + c->damp_prev * c->fb;
    c->pos = (c->pos + 1) % c->len;
    return output;
}

static void comb_free(comb_t *c) { free(c->buf); c->buf = NULL; }

/* Allpass filter for reverb */
typedef struct {
    float *buf;
    int    len;
    int    pos;
    float  fb;
} allpass_t;

static void allpass_init(allpass_t *a, int delay_samples, float feedback)
{
    a->len = delay_samples;
    a->buf = (float *)calloc(delay_samples, sizeof(float));
    a->pos = 0;
    a->fb = feedback;
}

static float allpass_process(allpass_t *a, float input)
{
    float delayed = a->buf[a->pos];
    float output = delayed - input;
    a->buf[a->pos] = input + delayed * a->fb;
    a->pos = (a->pos + 1) % a->len;
    return output;
}

static void allpass_free(allpass_t *a) { free(a->buf); a->buf = NULL; }

/* Schroeder reverb: 4 parallel comb filters → 2 series allpass filters
 * in_len = input sample count, out_len = output sample count (>= in_len for tail) */
static void dsp_reverb(float *in, float *out, int in_len, int out_len,
                        float room, float predelay_ms, float wet, float damping)
{
    /* Comb filter base delays (in samples at 22050) scaled by room size and output rate */
    int base_delays[4] = { 1116, 1188, 1277, 1356 };
    int ap_delays[2]   = { 225, 556 };
    float rate_scale = (float)OUTPUT_RATE / 22050.0f;
    float comb_fb = 0.7f + room * 0.28f;  /* feedback: 0.7 - 0.98 */

    comb_t combs[4];
    allpass_t aps[2];
    for (int i = 0; i < 4; i++) {
        int d = (int)(base_delays[i] * (0.5f + room * 0.8f) * rate_scale);
        if (d < 10) d = 10;
        comb_init(&combs[i], d, comb_fb, damping);
    }
    for (int i = 0; i < 2; i++) {
        int d = (int)(ap_delays[i] * (0.5f + room * 0.5f) * rate_scale);
        if (d < 10) d = 10;
        allpass_init(&aps[i], d, 0.5f);
    }

    int predelay_samples = (int)(predelay_ms * OUTPUT_RATE / 1000.0f);
    float dry = 1.0f - wet * 0.5f;  /* keep dry loud */

    for (int i = 0; i < out_len; i++) {
        /* Feed input (with predelay) only during input range, silence for tail */
        int src = i - predelay_samples;
        float x = (src >= 0 && src < in_len) ? in[src] : 0.0f;
        float orig = (i < in_len) ? in[i] : 0.0f;

        /* Sum of parallel comb filters */
        float rev = 0.0f;
        for (int c = 0; c < 4; c++)
            rev += comb_process(&combs[c], x);
        rev *= 0.25f;

        /* Series allpass filters */
        for (int a = 0; a < 2; a++)
            rev = allpass_process(&aps[a], rev);

        out[i] = orig * dry + rev * wet;
    }

    for (int i = 0; i < 4; i++) comb_free(&combs[i]);
    for (int i = 0; i < 2; i++) allpass_free(&aps[i]);
}

/* Stereo delay -takes mono input, writes interleaved stereo output.
 * out must be 2*len floats (L R L R ...).
 * Adds reverb tail for the delay feedback. */
static void dsp_stereo_delay(float *in, float *out_lr, int len,
                              float left_ms, float right_ms,
                              float feedback, float wet)
{
    int dl_L = (int)(left_ms * OUTPUT_RATE / 1000.0f);
    int dl_R = (int)(right_ms * OUTPUT_RATE / 1000.0f);
    int max_delay = dl_L > dl_R ? dl_L : dl_R;

    /* Extra samples for delay tail to ring out */
    int tail = (int)(max_delay * feedback / (1.0f - feedback + 0.01f));
    if (tail > OUTPUT_RATE * 3) tail = OUTPUT_RATE * 3;  /* cap at 3s tail */
    int total = len + tail;

    float *buf_L = (float *)calloc(total, sizeof(float));
    float *buf_R = (float *)calloc(total, sizeof(float));

    float dry = 1.0f - wet * 0.3f;

    for (int i = 0; i < total; i++) {
        float x = (i < len) ? in[i] : 0.0f;

        /* Read from delay lines */
        float tap_L = (i >= dl_L) ? buf_L[i - dl_L] : 0.0f;
        float tap_R = (i >= dl_R) ? buf_R[i - dl_R] : 0.0f;

        /* Ping-pong: left feeds right, right feeds left */
        buf_L[i] = x + tap_R * feedback;
        buf_R[i] = x + tap_L * feedback;

        /* Mix to output */
        out_lr[i * 2]     = x * dry + buf_L[i] * wet;  /* Left */
        out_lr[i * 2 + 1] = x * dry + buf_R[i] * wet;  /* Right */
    }

    free(buf_L);
    free(buf_R);
}

/* ---- waveOut playback ---- */

static HWAVEOUT hWaveOut = NULL;
static volatile int tts_playing = 0;

static void CALLBACK wave_out_callback(HWAVEOUT hwo, UINT msg, DWORD_PTR inst,
                                        DWORD_PTR param1, DWORD_PTR param2)
{
    (void)hwo; (void)inst; (void)param2;
    if (msg == WOM_DONE) {
        WAVEHDR *hdr = (WAVEHDR *)param1;
        waveOutUnprepareHeader(hwo, hdr, sizeof(WAVEHDR));
        free(hdr->lpData);
        free(hdr);
        waveOutClose(hwo);
        tts_playing = 0;
    }
}

/*
 * tts_speak() -The exported TTS function.
 * Main DLL discovers this via GetProcAddress("tts_speak").
 */
EXPORT void tts_speak(const char *text, int pitch, int speed)
{
    if (!text || !text[0]) return;
    if (tts_playing) return;  /* don't overlap */

    /* Prepare input for SAM reciter: uppercase + 0x9b terminator */
    unsigned char input[256];
    memset(input, 0, sizeof(input));
    int i;
    for (i = 0; i < 254 && text[i]; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        input[i] = (unsigned char)c;
    }
    input[i] = 0x9b;
    input[i+1] = '\0';

    /* English text → phonemes */
    if (!TextToPhonemes(input)) {
        if (api) api->log("[TTS_SAM] Reciter failed for: %s\n", text);
        return;
    }

    /* Configure SAM */
    SetInput((char *)input);
    SetPitch(pitch > 0 ? (unsigned char)pitch : (unsigned char)sam_pitch_def);
    SetSpeed(speed > 0 ? (unsigned char)speed : (unsigned char)sam_speed_def);
    SetMouth((unsigned char)sam_mouth_val);
    SetThroat((unsigned char)sam_throat_val);

    /* Synthesize */
    if (!SAMMain()) {
        if (api) api->log("[TTS_SAM] Synthesis failed\n");
        return;
    }

    char *buf = GetBuffer();
    int raw_len = GetBufferLength();
    if (!buf || raw_len <= 0) return;

    /* SAM's bufferpos is in time units, not bytes. Actual byte count = bufferpos / 50 */
    int mono_len = raw_len / 50;
    if (mono_len <= 0) return;

    if (api) api->log("[TTS_SAM] Speaking: \"%s\" (%d samples, pitch=%d speed=%d)\n",
                       text, mono_len, pitch > 0 ? pitch : 64, speed > 0 ? speed : 72);

    /* Convert 8-bit unsigned to float (-1.0 to 1.0) */
    float *raw_float = (float *)malloc(mono_len * sizeof(float));
    for (int s = 0; s < mono_len; s++)
        raw_float[s] = ((unsigned char)buf[s] - 128) / 128.0f;

    /* Upsample 22050 -> 44100 for better DSP quality */
    int up_len;
    float *fbuf = upsample_2x(raw_float, mono_len, &up_len);
    free(raw_float);
    mono_len = up_len;

    /* DSP chain at 44100 Hz: lowpass -> reverb -> stereo delay */

    /* 1. Lowpass filter */
    if (lp_enabled)
        dsp_lowpass(fbuf, mono_len, lp_cutoff);

    /* 2. Reverb (extends buffer with decay tail) */
    if (rv_enabled && rv_wet > 0.001f) {
        /* Tail length based on room size — longer room = longer decay */
        int rv_tail = (int)(OUTPUT_RATE * (0.5f + rv_length * 2.0f));
        if (rv_tail > OUTPUT_RATE * 4) rv_tail = OUTPUT_RATE * 4;
        int rv_out_len = mono_len + rv_tail;
        float *rvbuf = (float *)calloc(rv_out_len, sizeof(float));
        dsp_reverb(fbuf, rvbuf, mono_len, rv_out_len, rv_length, rv_predelay, rv_wet, rv_damping);
        free(fbuf);
        fbuf = rvbuf;
        mono_len = rv_out_len;  /* extended length carries through to delay/output */
    }

    /* 3. Stereo delay */
    int out_channels, out_len;
    float *out_float;

    if (dl_enabled && dl_wet > 0.001f) {
        /* Delay adds tail samples */
        int max_dl = (int)(((dl_left_ms > dl_right_ms ? dl_left_ms : dl_right_ms)
                           * OUTPUT_RATE / 1000.0f));
        int tail = (int)(max_dl * dl_feedback / (1.0f - dl_feedback + 0.01f));
        if (tail > OUTPUT_RATE * 3) tail = OUTPUT_RATE * 3;
        out_len = mono_len + tail;
        out_float = (float *)calloc(out_len * 2, sizeof(float));
        dsp_stereo_delay(fbuf, out_float, mono_len,
                         dl_left_ms, dl_right_ms, dl_feedback, dl_wet);
        out_channels = 2;
    } else {
        /* No delay -output mono as stereo (dual mono) for consistent format */
        out_len = mono_len;
        out_float = (float *)malloc(out_len * 2 * sizeof(float));
        for (int s = 0; s < out_len; s++) {
            out_float[s * 2]     = fbuf[s];
            out_float[s * 2 + 1] = fbuf[s];
        }
        out_channels = 2;
    }

    free(fbuf);

    /* Apply volume */
    if (volume < 0.99f || volume > 1.01f) {
        for (int s = 0; s < out_len * 2; s++)
            out_float[s] *= volume;
    }

    /* Apply stereo pan: constant-power panning (equal-power law)
     * pan -1.0=left, 0.0=center, 1.0=right */
    if (pan_value < -0.01f || pan_value > 0.01f) {
        float angle = (pan_value + 1.0f) * 0.25f * 3.14159265f;  /* 0..pi/2 */
        float gain_L = cosf(angle);
        float gain_R = sinf(angle);
        for (int s = 0; s < out_len; s++) {
            out_float[s * 2]     *= gain_L;
            out_float[s * 2 + 1] *= gain_R;
        }
    }

    /* Convert float stereo to 16-bit signed interleaved PCM */
    int total_samples = out_len * out_channels;
    short *pcm = (short *)malloc(total_samples * sizeof(short));
    for (int s = 0; s < total_samples; s++) {
        float v = out_float[s];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[s] = (short)(v * 32767.0f);
    }
    free(out_float);

    /* Play via waveOut -16-bit signed, stereo, 44100Hz */
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = OUTPUT_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;  /* 2 channels * 2 bytes */
    wfx.nAvgBytesPerSec = OUTPUT_RATE * 4;

    HWAVEOUT hwo = NULL;
    MMRESULT rv_result = waveOutOpen(&hwo, WAVE_MAPPER, &wfx,
                               (DWORD_PTR)wave_out_callback, 0, CALLBACK_FUNCTION);
    if (rv_result != MMSYSERR_NOERROR) {
        if (api) api->log("[TTS_SAM] waveOutOpen failed (code %d)\n", rv_result);
        free(pcm);
        return;
    }

    int byte_len = total_samples * sizeof(short);
    WAVEHDR *hdr = (WAVEHDR *)calloc(1, sizeof(WAVEHDR));
    hdr->lpData = (char *)pcm;
    hdr->dwBufferLength = byte_len;

    waveOutPrepareHeader(hwo, hdr, sizeof(WAVEHDR));
    tts_playing = 1;
    waveOutWrite(hwo, hdr, sizeof(WAVEHDR));

    if (api) api->log("[TTS_SAM] Playing %d bytes stereo 16-bit (lp=%d rv=%d dl=%d)\n",
                       byte_len, lp_enabled, rv_enabled, dl_enabled);
}

/* Wait for current speech to finish */
static void tts_wait(void)
{
    while (tts_playing) Sleep(50);
}

/* Known struct offsets for demo readout */
#define OFF_PLAYER_NAME     0x537C
#define OFF_PLAYER_CUR_HP   0x53D4
#define OFF_PLAYER_MAX_HP   0x53DC
#define OFF_PLAYER_CUR_MANA 0x53E0
#define OFF_PLAYER_MAX_MANA 0x53E8
#define OFF_CONNECTED       0x563C

/* Read a string from struct memory */
static void read_struct_str(unsigned int base, unsigned int offset, char *out, int out_sz)
{
    if (!base) { out[0] = '\0'; return; }
    const char *src = (const char *)(base + offset);
    int i;
    for (i = 0; i < out_sz - 1 && src[i] && src[i] != '\r' && src[i] != '\n'; i++)
        out[i] = src[i];
    out[i] = '\0';
}

/* Demo: reads game state and speaks it with varying DSP effects */
static DWORD WINAPI sam_msgbox_thread(LPVOID param)
{
    HWND parent = api ? api->get_mmmain_hwnd() : NULL;
    MessageBoxA(parent, (const char *)param, "SAM TTS Demo", MB_OK | MB_ICONINFORMATION);
    free(param);
    return 0;
}

static DWORD WINAPI sam_demo_thread(LPVOID param)
{
    (void)param;

    /* Check if connected */
    unsigned int base = api ? api->get_struct_base() : 0;
    if (!base) {
        tts_speak("Connect to the B B S first.", 0, 0);
        return 0;
    }
    int connected = api->read_struct_i32(OFF_CONNECTED);
    if (!connected) {
        tts_speak("Connect to the B B S first.", 0, 0);
        return 0;
    }

    /* Read player data */
    char name[64];
    read_struct_str(base, OFF_PLAYER_NAME, name, sizeof(name));
    int cur_hp  = api->read_struct_i32(OFF_PLAYER_CUR_HP);
    int max_hp  = api->read_struct_i32(OFF_PLAYER_MAX_HP);
    int cur_mana = api->read_struct_i32(OFF_PLAYER_CUR_MANA);
    int max_mana = api->read_struct_i32(OFF_PLAYER_MAX_MANA);
    int mana_pct = max_mana > 0 ? (cur_mana * 100 / max_mana) : 0;

    char line[256];

    /* Save original settings */
    float orig_lp = lp_cutoff; int orig_lp_en = lp_enabled;
    float orig_rv_len = rv_length, orig_rv_pre = rv_predelay;
    float orig_rv_wet = rv_wet, orig_rv_damp = rv_damping; int orig_rv_en = rv_enabled;
    float orig_dl_l = dl_left_ms, orig_dl_r = dl_right_ms;
    float orig_dl_fb = dl_feedback, orig_dl_w = dl_wet; int orig_dl_en = dl_enabled;
    float orig_pan = pan_value, orig_vol = volume;
    int orig_mouth = sam_mouth_val, orig_throat = sam_throat_val;
    int orig_pitch = sam_pitch_def, orig_speed = sam_speed_def;

    /* Show transcript popup on background thread so speech plays immediately */
    char *transcript = (char *)malloc(1024);
    sprintf(transcript,
        "Hello %s.\n"
        "This is Sam, text to speech engine.\n"
        "You have %d out of %d hit points.\n"
        "And you are at %d percent mana.\n"
        "Enjoy your script.",
        name, cur_hp, max_hp, mana_pct);
    CreateThread(NULL, 0, sam_msgbox_thread, transcript, 0, NULL);

    /* Line 1: Greeting - normal voice, panning left */
    sam_set_lowpass(0.35f, 1);
    sam_set_reverb(0.4f, 15.0f, 0.1f, 0.4f, 0);
    sam_set_delay(120.0f, 180.0f, 0.1f, 0.08f, 0);
    sam_set_volume(1.0f);
    sam_set_voice(128, 128, 64, 72);
    sam_set_pan(-0.5f);
    sprintf(line, "Hello %s.", name);
    if (api) api->log("[SAM Demo] %s\n", line);
    tts_speak(line, 0, 0);
    tts_wait(); Sleep(200);

    /* Line 2: Engine intro - deep voice, light reverb, center */
    sam_set_voice(180, 160, 50, 78);
    sam_set_reverb(0.3f, 10.0f, 0.08f, 0.4f, 1);
    sam_set_pan(0.0f);
    if (api) api->log("[SAM Demo] This is Sam, text to speech engine.\n");
    tts_speak("This is Sam, text to speech engine.", 0, 0);
    tts_wait(); Sleep(300);

    /* Line 3: Health - high voice, subtle delay, panning right */
    sam_set_voice(110, 100, 80, 65);
    sam_set_reverb(0.3f, 10.0f, 0.08f, 0.4f, 0);
    sam_set_delay(100.0f, 150.0f, 0.08f, 0.06f, 1);
    sam_set_pan(0.5f);
    sprintf(line, "You have %d out of %d hit points.", cur_hp, max_hp);
    if (api) api->log("[SAM Demo] %s\n", line);
    tts_speak(line, 0, 0);
    tts_wait(); Sleep(300);

    /* Line 4: Mana - robot voice, mild reverb+delay */
    sam_set_voice(160, 60, 45, 85);
    sam_set_reverb(0.3f, 8.0f, 0.08f, 0.3f, 1);
    sam_set_delay(80.0f, 120.0f, 0.08f, 0.06f, 1);
    sam_set_lowpass(0.3f, 1);
    sam_set_pan(-0.3f);
    sam_set_volume(0.9f);
    sprintf(line, "And you are at %d percent mana.", mana_pct);
    if (api) api->log("[SAM Demo] %s\n", line);
    tts_speak(line, 0, 0);
    tts_wait(); Sleep(400);

    /* Line 5: Sign off - soft voice, gentle reverb, NO delay */
    sam_set_voice(120, 190, 75, 55);
    sam_set_reverb(0.3f, 10.0f, 0.08f, 0.5f, 1);
    sam_set_delay(0.0f, 0.0f, 0.0f, 0.0f, 0);
    sam_set_volume(0.75f);
    sam_set_lowpass(0.45f, 1);
    sam_set_pan(0.2f);
    if (api) api->log("[SAM Demo] Enjoy your script.\n");
    tts_speak("Enjoy your script.", 0, 0);
    tts_wait();

    /* Restore */
    sam_set_lowpass(orig_lp, orig_lp_en);
    sam_set_reverb(orig_rv_len, orig_rv_pre, orig_rv_wet, orig_rv_damp, orig_rv_en);
    sam_set_delay(orig_dl_l, orig_dl_r, orig_dl_fb, orig_dl_w, orig_dl_en);
    sam_set_pan(orig_pan);
    sam_set_volume(orig_vol);
    sam_set_voice(orig_mouth, orig_throat, orig_pitch, orig_speed);

    return 0;
}

/* ---- Plugin interface ---- */

/* Python code injected into MMUDPy via mmudpy_exec */
static const char *sam_py_inject =
    "class _SAM:\n"
    "    def __init__(self):\n"
    "        import ctypes, ctypes.util\n"
    "        # DLL is already loaded in process - get handle\n"
    "        h = ctypes.windll.kernel32.GetModuleHandleA(b'TTS_SAM.dll')\n"
    "        if not h: h = ctypes.windll.kernel32.GetModuleHandleA(b'TTS_SAM')\n"
    "        self._dll = ctypes.CDLL('TTS_SAM', handle=h) if h else ctypes.CDLL('TTS_SAM.dll')\n"
    "        d = self._dll\n"
    "        d.tts_speak.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]\n"
    "        d.tts_speak.restype = None\n"
    "        d.sam_set_lowpass.argtypes = [ctypes.c_float, ctypes.c_int]\n"
    "        d.sam_set_lowpass.restype = None\n"
    "        d.sam_set_reverb.argtypes = [ctypes.c_float]*4 + [ctypes.c_int]\n"
    "        d.sam_set_reverb.restype = None\n"
    "        d.sam_set_delay.argtypes = [ctypes.c_float]*4 + [ctypes.c_int]\n"
    "        d.sam_set_delay.restype = None\n"
    "        d.sam_get_settings.argtypes = [ctypes.c_char_p, ctypes.c_int]\n"
    "        d.sam_get_settings.restype = ctypes.c_int\n"
    "        d.sam_set_pan.argtypes = [ctypes.c_float]\n"
    "        d.sam_set_pan.restype = None\n"
    "        d.sam_set_volume.argtypes = [ctypes.c_float]\n"
    "        d.sam_set_volume.restype = None\n"
    "        d.sam_set_voice.argtypes = [ctypes.c_int]*4\n"
    "        d.sam_set_voice.restype = None\n"
    "    def say(self, text, pitch=0, speed=0, reverb=None, delay=None, pan=None):\n"
    "        '''Speak text. reverb/delay: True/tuple/False. pan: -1.0(L) to 1.0(R).'''\n"
    "        if reverb is None or reverb is False:\n"
    "            self._dll.sam_set_reverb(0.6, 20.0, 0.25, 0.4, 0)\n"
    "        elif reverb is True:\n"
    "            self._dll.sam_set_reverb(0.6, 20.0, 0.25, 0.4, 1)\n"
    "        elif isinstance(reverb, (tuple, list)):\n"
    "            r = list(reverb) + [0.6, 20.0, 0.25, 0.4][len(reverb):]\n"
    "            self._dll.sam_set_reverb(r[0], r[1], r[2], r[3] if len(r)>3 else 0.4, 1)\n"
    "        if delay is None or delay is False:\n"
    "            self._dll.sam_set_delay(250.0, 375.0, 0.35, 0.3, 0)\n"
    "        elif delay is True:\n"
    "            self._dll.sam_set_delay(250.0, 375.0, 0.35, 0.3, 1)\n"
    "        elif isinstance(delay, (tuple, list)):\n"
    "            d = list(delay) + [250.0, 375.0, 0.35, 0.3][len(delay):]\n"
    "            self._dll.sam_set_delay(d[0], d[1], d[2], d[3] if len(d)>3 else 0.3, 1)\n"
    "        if pan is not None:\n"
    "            self._dll.sam_set_pan(pan)\n"
    "        self._dll.tts_speak(text.encode() if isinstance(text, str) else text, pitch, speed)\n"
    "        if pan is not None:\n"
    "            self._dll.sam_set_pan(0.0)\n"
    "    def lowpass(self, cutoff=0.35, enabled=True):\n"
    "        '''Lowpass filter. cutoff: 0=heavy, 1=bypass. Default 0.35.'''\n"
    "        self._dll.sam_set_lowpass(cutoff, int(enabled))\n"
    "    def reverb(self, length=0.6, predelay=20.0, wet=0.25, damping=0.4):\n"
    "        '''Set and enable reverb.'''\n"
    "        self._dll.sam_set_reverb(length, predelay, wet, damping, 1)\n"
    "    def reverb_on(self): self.reverb()\n"
    "    def reverb_off(self): self._dll.sam_set_reverb(0.6, 20.0, 0.25, 0.4, 0)\n"
    "    def delay(self, left_ms=250.0, right_ms=375.0, feedback=0.35, wet=0.3):\n"
    "        '''Set and enable stereo delay.'''\n"
    "        self._dll.sam_set_delay(left_ms, right_ms, feedback, wet, 1)\n"
    "    def delay_on(self): self.delay()\n"
    "    def delay_off(self): self._dll.sam_set_delay(250.0, 375.0, 0.35, 0.3, 0)\n"
    "    def pan(self, value=0.0):\n"
    "        '''Stereo pan. -1.0=full left, 0.0=center, 1.0=full right.'''\n"
    "        self._dll.sam_set_pan(value)\n"
    "    def vol(self, value=1.0):\n"
    "        '''Volume. 0.0=silent, 1.0=normal, up to 2.0=boost.'''\n"
    "        self._dll.sam_set_volume(value)\n"
    "    def voice(self, mouth=128, throat=128, pitch=64, speed=72):\n"
    "        '''Set SAM voice shape. mouth/throat: 0-255, pitch/speed: 0-255.'''\n"
    "        self._dll.sam_set_voice(mouth, throat, pitch, speed)\n"
    "    def mouth(self, val):\n"
    "        '''Set mouth formant 0-255 (default 128).'''\n"
    "        self._dll.sam_set_voice(val, -1, -1, -1)\n"
    "    def throat(self, val):\n"
    "        '''Set throat formant 0-255 (default 128).'''\n"
    "        self._dll.sam_set_voice(-1, val, -1, -1)\n"
    "    def settings(self):\n"
    "        '''Show all DSP and voice settings.'''\n"
    "        import ctypes\n"
    "        buf = ctypes.create_string_buffer(512)\n"
    "        self._dll.sam_get_settings(buf, 512)\n"
    "        print(buf.value.decode())\n"
    "    def reset(self):\n"
    "        '''Reset everything to defaults.'''\n"
    "        self.lowpass(0.35, True)\n"
    "        self.reverb_off()\n"
    "        self.delay_off()\n"
    "        self.pan(0.0)\n"
    "        self.vol(1.0)\n"
    "        self.voice(128, 128, 64, 72)\n"
    "        print('SAM reset to defaults.')\n"
    "    def __repr__(self):\n"
    "        import ctypes\n"
    "        buf = ctypes.create_string_buffer(512)\n"
    "        self._dll.sam_get_settings(buf, 512)\n"
    "        return buf.value.decode()\n"
    "\n"
    "sam = _SAM()\n"
    "\n"
    "# Register SAM help topic\n"
    "if 'help' in dir() and hasattr(help, '__wrapped_topics__'):\n"
    "    help.__wrapped_topics__['sam'] = (\n"
    "        'SAM TTS with DSP. Control via `sam` object:\\n'\n"
    "        '\\n'\n"
    "        '  Speech:\\n'\n"
    "        '  sam.say(\"text\")                         - speak with lowpass\\n'\n"
    "        '  sam.say(\"text\", reverb=True)             - + reverb\\n'\n"
    "        '  sam.say(\"text\", delay=True)              - + stereo delay\\n'\n"
    "        '  sam.say(\"text\", reverb=True, delay=True) - both\\n'\n"
    "        '  sam.say(\"text\", pan=-1.0)                - panned left\\n'\n"
    "        '  sam.say(\"text\", reverb=(0.8,30,0.4))     - custom reverb(len,pre,wet)\\n'\n"
    "        '  sam.say(\"text\", delay=(250,375,.35,.3))   - custom delay(L,R,fb,wet)\\n'\n"
    "        '\\n'\n"
    "        '  DSP Controls:\\n'\n"
    "        '  sam.lowpass(cutoff)     - 0=heavy, 1=bypass (default 0.35)\\n'\n"
    "        '  sam.reverb(len,pre,wet,damp) / reverb_on() / reverb_off()\\n'\n"
    "        '  sam.delay(L,R,fb,wet)   / delay_on()  / delay_off()\\n'\n"
    "        '  sam.pan(val)            - -1.0=left, 0=center, 1.0=right\\n'\n"
    "        '  sam.vol(val)            - 0=silent, 1=normal, 2=boost\\n'\n"
    "        '\\n'\n"
    "        '  Voice Shape:\\n'\n"
    "        '  sam.voice(mouth,throat,pitch,speed) - full voice control\\n'\n"
    "        '  sam.mouth(val)          - formant 0-255 (default 128)\\n'\n"
    "        '  sam.throat(val)         - formant 0-255 (default 128)\\n'\n"
    "        '\\n'\n"
    "        '  sam.settings()  sam.reset()'\n"
    "    )\n"
    "print('[SAM] TTS loaded - type `sam` or help(\"sam\")')\n"
    "print()\n"
;

typedef int (*mmudpy_exec_fn)(const char *);

static void sam_inject_python(void)
{
    /* Wait briefly for MMUDPy to initialize Python */
    for (int i = 0; i < 20; i++) {
        HMODULE m = GetModuleHandleA("mmudpy.dll");
        if (m) {
            mmudpy_exec_fn py_exec = (mmudpy_exec_fn)GetProcAddress(m, "mmudpy_exec");
            if (py_exec) {
                /* Give Python bootstrap a moment to finish */
                Sleep(500);
                int rc = py_exec(sam_py_inject);
                if (api) api->log("[TTS_SAM] Python injection %s (rc=%d)\n",
                                   rc == 0 ? "OK" : "FAILED", rc);
                return;
            }
        }
        Sleep(500);
    }
    if (api) api->log("[TTS_SAM] MMUDPy not found -Python wrapper not injected\n");
}

static DWORD WINAPI sam_inject_thread(LPVOID param)
{
    (void)param;
    sam_inject_python();
    return 0;
}

static int sam_init(const slop_api_t *a)
{
    api = a;
    api->add_menu_item("Demo (SAM TTS)", IDM_SAM_TEST);
    api->log("[TTS_SAM] SAM TTS + DSP loaded (lowpass=%d reverb=%d delay=%d)\n",
             lp_enabled, rv_enabled, dl_enabled);

    /* Inject Python wrapper on a background thread (MMUDPy may not be ready yet) */
    CreateThread(NULL, 0, sam_inject_thread, NULL, 0, NULL);

    return 0;
}

static void sam_shutdown(void)
{
    if (tts_playing && hWaveOut) {
        waveOutReset(hWaveOut);
    }
    if (api) api->log("[TTS_SAM] Unloaded\n");
}

static int sam_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND && LOWORD(wParam) == IDM_SAM_TEST) {
        if (!tts_playing) {
            CreateThread(NULL, 0, sam_demo_thread, NULL, 0, NULL);
        }
        return 1;
    }
    return 0;
}

static slop_plugin_t sam_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "SAM",
    .author      = "Tripmunk",
    .description = "SAM TTS with reverb, lowpass, and stereo delay DSP",
    .version     = "2.0.0",
    .init        = sam_init,
    .shutdown    = sam_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = sam_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &sam_plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
