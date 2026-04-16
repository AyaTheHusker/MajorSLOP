/*
 * tts_espeak.c - eSpeak-ng TTS Plugin with DSP
 *
 * Modern text-to-speech with full DSP chain:
 *   - 2x upsampling (22050 -> 44100 Hz) for better quality
 *   - Lowpass filter (single-pole IIR)
 *   - Schroeder reverb (4 comb + 2 allpass filters)
 *   - Stereo ping-pong delay
 *   - Constant-power stereo panning
 *   - Volume control
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o TTS_eSpeak.dll tts_espeak.c \
 *       -DLIBESPEAK_NG_EXPORT \
 *       -I$HOME/AI/espeak-ng-src/src/include \
 *       -L$HOME/AI/espeak-ng-src/build-mingw32/src/libespeak-ng \
 *       -L$HOME/AI/espeak-ng-src/build-mingw32/src/ucd-tools \
 *       -lespeak-ng -lucd -lwinmm -lgdi32 -luser32 -static-libgcc
 *
 * Install:
 *   Copy TTS_eSpeak.dll to MegaMUD/plugins/
 *   Copy espeak-ng-data/ to MegaMUD/plugins/espeak-ng-data/
 */

#include "slop_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LIBESPEAK_NG_EXPORT
#include <espeak-ng/speak_lib.h>

static const slop_api_t *api = NULL;

#define IDM_ESPEAK_TEST  50500

/* Native eSpeak sample rate (set by espeak_Initialize) */
static int espeak_sr = 22050;

/* Output sample rate after upsampling */
#define OUTPUT_RATE 44100

/* ---- DSP Parameters ---- */

static float lp_cutoff = 0.45f;
static int   lp_enabled = 1;

static float rv_length   = 0.6f;
static float rv_predelay = 20.0f;
static float rv_wet      = 0.25f;
static float rv_damping  = 0.4f;
static int   rv_enabled  = 0;

static float dl_left_ms  = 250.0f;
static float dl_right_ms = 375.0f;
static float dl_feedback = 0.35f;
static float dl_wet      = 0.3f;
static int   dl_enabled  = 0;

static float pan_value = 0.0f;
static float volume    = 1.0f;

/* eSpeak voice params */
static int es_rate   = 175;  /* words per minute, 80-450 */
static int es_pitch  = 50;   /* 0-100, 50=normal */
static int es_range  = 50;   /* 0-100, 50=normal */
static int es_volume = 100;  /* 0-200 */
static char es_voice[64] = "en";  /* voice name */

/* ---- Exported settings functions ---- */

#define EXPORT __declspec(dllexport)

EXPORT void espeak_set_lowpass(float cutoff, int enabled) {
    if (cutoff < 0.0f) cutoff = 0.0f;
    if (cutoff > 1.0f) cutoff = 1.0f;
    lp_cutoff = cutoff;
    lp_enabled = enabled;
}

EXPORT void espeak_set_reverb(float length, float predelay, float wet, float damping, int enabled) {
    if (length < 0.0f) length = 0.0f; if (length > 1.0f) length = 1.0f;
    if (wet < 0.0f) wet = 0.0f; if (wet > 1.0f) wet = 1.0f;
    if (damping < 0.0f) damping = 0.0f; if (damping > 1.0f) damping = 1.0f;
    rv_length = length; rv_predelay = predelay; rv_wet = wet;
    rv_damping = damping; rv_enabled = enabled;
}

EXPORT void espeak_set_delay(float left_ms, float right_ms, float feedback, float wet, int enabled) {
    if (feedback < 0.0f) feedback = 0.0f; if (feedback > 0.95f) feedback = 0.95f;
    if (wet < 0.0f) wet = 0.0f; if (wet > 1.0f) wet = 1.0f;
    dl_left_ms = left_ms; dl_right_ms = right_ms;
    dl_feedback = feedback; dl_wet = wet; dl_enabled = enabled;
}

EXPORT void espeak_set_pan(float pan) {
    if (pan < -1.0f) pan = -1.0f; if (pan > 1.0f) pan = 1.0f;
    pan_value = pan;
}

EXPORT void espeak_set_volume(float vol) {
    if (vol < 0.0f) vol = 0.0f; if (vol > 2.0f) vol = 2.0f;
    volume = vol;
}

EXPORT void espeak_set_voice_params(int rate, int pitch, int range, int vol) {
    if (rate >= 0) es_rate = rate;
    if (pitch >= 0) es_pitch = pitch;
    if (range >= 0) es_range = range;
    if (vol >= 0) es_volume = vol;
}

EXPORT void espeak_set_voice_name(const char *name) {
    if (name && name[0]) {
        strncpy(es_voice, name, 63);
        es_voice[63] = '\0';
    }
}

EXPORT int espeak_get_settings(char *out, int out_sz) {
    return snprintf(out, out_sz,
        "eSpeak DSP Settings:\n"
        "  Voice:    \"%s\" rate=%d pitch=%d range=%d vol=%d\n"
        "  Volume:   %.2f\n"
        "  Pan:      %.2f (%s)\n"
        "  Lowpass:  cutoff=%.2f enabled=%d\n"
        "  Reverb:   length=%.2f predelay=%.0fms wet=%.2f damping=%.2f enabled=%d\n"
        "  Delay:    L=%.0fms R=%.0fms feedback=%.2f wet=%.2f enabled=%d\n"
        "  Output:   %d Hz (upsampled from %d Hz)\n",
        es_voice, es_rate, es_pitch, es_range, es_volume,
        volume,
        pan_value, pan_value < -0.01f ? "left" : pan_value > 0.01f ? "right" : "center",
        lp_cutoff, lp_enabled,
        rv_length, rv_predelay, rv_wet, rv_damping, rv_enabled,
        dl_left_ms, dl_right_ms, dl_feedback, dl_wet, dl_enabled,
        OUTPUT_RATE, espeak_sr);
}

/* ---- DSP Processing (same as SAM plugin, at OUTPUT_RATE) ---- */

static void dsp_lowpass(float *buf, int len, float cutoff)
{
    float alpha = cutoff * cutoff;
    if (alpha > 0.99f) return;
    if (alpha < 0.001f) alpha = 0.001f;
    float prev = buf[0];
    for (int i = 1; i < len; i++) {
        buf[i] = prev + alpha * (buf[i] - prev);
        prev = buf[i];
    }
}

typedef struct { float *buf; int len, pos; float fb, damp, damp_prev; } comb_t;

static void comb_init(comb_t *c, int delay_samples, float feedback, float damping) {
    c->len = delay_samples; c->buf = (float *)calloc(delay_samples, sizeof(float));
    c->pos = 0; c->fb = feedback; c->damp = damping; c->damp_prev = 0.0f;
}
static float comb_process(comb_t *c, float input) {
    float output = c->buf[c->pos];
    c->damp_prev = output * (1.0f - c->damp) + c->damp_prev * c->damp;
    c->buf[c->pos] = input + c->damp_prev * c->fb;
    c->pos = (c->pos + 1) % c->len;
    return output;
}
static void comb_free(comb_t *c) { free(c->buf); c->buf = NULL; }

typedef struct { float *buf; int len, pos; float fb; } allpass_t;

static void allpass_init(allpass_t *a, int delay_samples, float feedback) {
    a->len = delay_samples; a->buf = (float *)calloc(delay_samples, sizeof(float));
    a->pos = 0; a->fb = feedback;
}
static float allpass_process(allpass_t *a, float input) {
    float delayed = a->buf[a->pos];
    float output = delayed - input;
    a->buf[a->pos] = input + delayed * a->fb;
    a->pos = (a->pos + 1) % a->len;
    return output;
}
static void allpass_free(allpass_t *a) { free(a->buf); a->buf = NULL; }

static void dsp_reverb(float *in, float *out, int in_len, int out_len,
                        float room, float predelay_ms, float wet, float damping)
{
    int base_delays[4] = { 1116, 1188, 1277, 1356 };
    int ap_delays[2]   = { 225, 556 };
    /* Scale delays for OUTPUT_RATE (base delays are for 22050) */
    float rate_scale = (float)OUTPUT_RATE / 22050.0f;
    float comb_fb = 0.7f + room * 0.28f;

    comb_t combs[4]; allpass_t aps[2];
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
    float dry = 1.0f - wet * 0.5f;

    for (int i = 0; i < out_len; i++) {
        int src = i - predelay_samples;
        float x = (src >= 0 && src < in_len) ? in[src] : 0.0f;
        float orig = (i < in_len) ? in[i] : 0.0f;
        float rev = 0.0f;
        for (int c = 0; c < 4; c++) rev += comb_process(&combs[c], x);
        rev *= 0.25f;
        for (int a = 0; a < 2; a++) rev = allpass_process(&aps[a], rev);
        out[i] = orig * dry + rev * wet;
    }
    for (int i = 0; i < 4; i++) comb_free(&combs[i]);
    for (int i = 0; i < 2; i++) allpass_free(&aps[i]);
}

static void dsp_stereo_delay(float *in, float *out_lr, int len,
                              float left_ms, float right_ms,
                              float feedback, float wet)
{
    int dl_L = (int)(left_ms * OUTPUT_RATE / 1000.0f);
    int dl_R = (int)(right_ms * OUTPUT_RATE / 1000.0f);
    int max_delay = dl_L > dl_R ? dl_L : dl_R;
    int tail = (int)(max_delay * feedback / (1.0f - feedback + 0.01f));
    if (tail > OUTPUT_RATE * 3) tail = OUTPUT_RATE * 3;
    int total = len + tail;

    float *buf_L = (float *)calloc(total, sizeof(float));
    float *buf_R = (float *)calloc(total, sizeof(float));
    float dry = 1.0f - wet * 0.3f;

    for (int i = 0; i < total; i++) {
        float x = (i < len) ? in[i] : 0.0f;
        float tap_L = (i >= dl_L) ? buf_L[i - dl_L] : 0.0f;
        float tap_R = (i >= dl_R) ? buf_R[i - dl_R] : 0.0f;
        buf_L[i] = x + tap_R * feedback;
        buf_R[i] = x + tap_L * feedback;
        out_lr[i * 2]     = x * dry + buf_L[i] * wet;
        out_lr[i * 2 + 1] = x * dry + buf_R[i] * wet;
    }
    free(buf_L); free(buf_R);
}

/* ---- 2x Linear interpolation upsampler ---- */
static float *upsample_2x(short *in, int in_len, int *out_len)
{
    *out_len = in_len * 2;
    float *out = (float *)malloc(*out_len * sizeof(float));
    for (int i = 0; i < in_len; i++) {
        float s = in[i] / 32768.0f;
        float next = (i + 1 < in_len) ? in[i + 1] / 32768.0f : s;
        out[i * 2]     = s;
        out[i * 2 + 1] = (s + next) * 0.5f;  /* linear interp */
    }
    return out;
}

/* ---- waveOut playback ---- */

static HWAVEOUT hWaveOut = NULL;
static volatile int tts_playing = 0;
static HANDLE tts_done_event = NULL;
static WAVEHDR *tts_pending_hdr = NULL;

static DWORD WINAPI tts_cleanup_thread(LPVOID param)
{
    (void)param;
    WaitForSingleObject(tts_done_event, INFINITE);
    if (hWaveOut && tts_pending_hdr) {
        waveOutUnprepareHeader(hWaveOut, tts_pending_hdr, sizeof(WAVEHDR));
        free(tts_pending_hdr->lpData);
        free(tts_pending_hdr);
        tts_pending_hdr = NULL;
    }
    tts_playing = 0;
    return 0;
}

/* ---- eSpeak synthesis callback: accumulates PCM ---- */

static short *synth_buf = NULL;
static int synth_len = 0;
static int synth_cap = 0;

static int espeak_synth_cb(short *wav, int numsamples, espeak_EVENT *events)
{
    (void)events;
    if (!wav || numsamples == 0) return 0;
    if (synth_len + numsamples > synth_cap) {
        synth_cap = (synth_len + numsamples) * 2;
        synth_buf = realloc(synth_buf, synth_cap * sizeof(short));
    }
    memcpy(synth_buf + synth_len, wav, numsamples * sizeof(short));
    synth_len += numsamples;
    return 0;
}

/* ---- Main TTS function ---- */

EXPORT void tts_speak(const char *text, int pitch, int speed)
{
    if (!text || !text[0]) return;
    if (tts_playing) return;

    /* Apply voice params */
    espeak_SetParameter(espeakRATE, speed > 0 ? speed : es_rate, 0);
    espeak_SetParameter(espeakPITCH, pitch > 0 ? pitch : es_pitch, 0);
    espeak_SetParameter(espeakRANGE, es_range, 0);
    espeak_SetParameter(espeakVOLUME, es_volume, 0);

    /* Synthesize to buffer */
    synth_cap = espeak_sr * 10;
    synth_buf = (short *)malloc(synth_cap * sizeof(short));
    synth_len = 0;

    espeak_Synth(text, strlen(text) + 1, 0, POS_CHARACTER, 0,
                 espeakCHARS_AUTO, NULL, NULL);
    espeak_Synchronize();

    if (synth_len <= 0) {
        free(synth_buf); synth_buf = NULL;
        if (api) api->log("[TTS_eSpeak] Synthesis produced no audio\n");
        return;
    }

    if (api) api->log("[TTS_eSpeak] Speaking: \"%s\" (%d samples @ %dHz)\n",
                       text, synth_len, espeak_sr);

    /* Upsample 22050 -> 44100 */
    int mono_len;
    float *fbuf = upsample_2x(synth_buf, synth_len, &mono_len);
    free(synth_buf); synth_buf = NULL;

    /* DSP chain at 44100 Hz */

    if (lp_enabled)
        dsp_lowpass(fbuf, mono_len, lp_cutoff);

    if (rv_enabled && rv_wet > 0.001f) {
        int rv_tail = (int)(OUTPUT_RATE * (0.5f + rv_length * 2.0f));
        if (rv_tail > OUTPUT_RATE * 4) rv_tail = OUTPUT_RATE * 4;
        int rv_out_len = mono_len + rv_tail;
        float *rvbuf = (float *)calloc(rv_out_len, sizeof(float));
        dsp_reverb(fbuf, rvbuf, mono_len, rv_out_len, rv_length, rv_predelay, rv_wet, rv_damping);
        free(fbuf);
        fbuf = rvbuf;
        mono_len = rv_out_len;
    }

    int out_len;
    float *out_float;

    if (dl_enabled && dl_wet > 0.001f) {
        int max_dl = (int)(((dl_left_ms > dl_right_ms ? dl_left_ms : dl_right_ms)
                           * OUTPUT_RATE / 1000.0f));
        int tail = (int)(max_dl * dl_feedback / (1.0f - dl_feedback + 0.01f));
        if (tail > OUTPUT_RATE * 3) tail = OUTPUT_RATE * 3;
        out_len = mono_len + tail;
        out_float = (float *)calloc(out_len * 2, sizeof(float));
        dsp_stereo_delay(fbuf, out_float, mono_len,
                         dl_left_ms, dl_right_ms, dl_feedback, dl_wet);
    } else {
        out_len = mono_len;
        out_float = (float *)malloc(out_len * 2 * sizeof(float));
        for (int s = 0; s < out_len; s++) {
            out_float[s * 2]     = fbuf[s];
            out_float[s * 2 + 1] = fbuf[s];
        }
    }
    free(fbuf);

    /* Volume */
    if (volume < 0.99f || volume > 1.01f) {
        for (int s = 0; s < out_len * 2; s++)
            out_float[s] *= volume;
    }

    /* Pan */
    if (pan_value < -0.01f || pan_value > 0.01f) {
        float angle = (pan_value + 1.0f) * 0.25f * 3.14159265f;
        float gain_L = cosf(angle);
        float gain_R = sinf(angle);
        for (int s = 0; s < out_len; s++) {
            out_float[s * 2]     *= gain_L;
            out_float[s * 2 + 1] *= gain_R;
        }
    }

    /* Float to 16-bit PCM */
    int total_samples = out_len * 2;
    short *pcm = (short *)malloc(total_samples * sizeof(short));
    for (int s = 0; s < total_samples; s++) {
        float v = out_float[s];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        pcm[s] = (short)(v * 32767.0f);
    }
    free(out_float);

    /* Play via waveOut at 44100 Hz stereo */
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = OUTPUT_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nAvgBytesPerSec = OUTPUT_RATE * 4;

    if (!tts_done_event)
        tts_done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    else
        ResetEvent(tts_done_event);

    if (!hWaveOut) {
        MMRESULT res = waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx,
                                   (DWORD_PTR)tts_done_event, 0, CALLBACK_EVENT);
        if (res != MMSYSERR_NOERROR) {
            if (api) api->log("[TTS_eSpeak] waveOutOpen failed (code %d)\n", res);
            hWaveOut = NULL;
            free(pcm);
            return;
        }
    }

    int byte_len = total_samples * sizeof(short);
    WAVEHDR *hdr = (WAVEHDR *)calloc(1, sizeof(WAVEHDR));
    hdr->lpData = (char *)pcm;
    hdr->dwBufferLength = byte_len;
    tts_pending_hdr = hdr;

    waveOutPrepareHeader(hWaveOut, hdr, sizeof(WAVEHDR));
    tts_playing = 1;
    waveOutWrite(hWaveOut, hdr, sizeof(WAVEHDR));
    CreateThread(NULL, 0, tts_cleanup_thread, NULL, 0, NULL);

    if (api) api->log("[TTS_eSpeak] Playing %d bytes stereo 16-bit @ %dHz (lp=%d rv=%d dl=%d)\n",
                       byte_len, OUTPUT_RATE, lp_enabled, rv_enabled, dl_enabled);
}

/* Wait for playback to finish */
static void tts_wait(void) { while (tts_playing) Sleep(50); }

/* ---- Python injection into MMUDPy ---- */

static const char *espeak_py_inject =
    "class _eSpeak:\n"
    "    def __init__(self):\n"
    "        import ctypes\n"
    "        h = ctypes.windll.kernel32.GetModuleHandleA(b'TTS_eSpeak.dll')\n"
    "        if not h: h = ctypes.windll.kernel32.GetModuleHandleA(b'TTS_eSpeak')\n"
    "        self._dll = ctypes.CDLL('TTS_eSpeak', handle=h) if h else ctypes.CDLL('TTS_eSpeak.dll')\n"
    "        d = self._dll\n"
    "        d.tts_speak.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int]\n"
    "        d.tts_speak.restype = None\n"
    "        d.espeak_set_lowpass.argtypes = [ctypes.c_float, ctypes.c_int]\n"
    "        d.espeak_set_lowpass.restype = None\n"
    "        d.espeak_set_reverb.argtypes = [ctypes.c_float]*4 + [ctypes.c_int]\n"
    "        d.espeak_set_reverb.restype = None\n"
    "        d.espeak_set_delay.argtypes = [ctypes.c_float]*4 + [ctypes.c_int]\n"
    "        d.espeak_set_delay.restype = None\n"
    "        d.espeak_get_settings.argtypes = [ctypes.c_char_p, ctypes.c_int]\n"
    "        d.espeak_get_settings.restype = ctypes.c_int\n"
    "        d.espeak_set_pan.argtypes = [ctypes.c_float]\n"
    "        d.espeak_set_pan.restype = None\n"
    "        d.espeak_set_volume.argtypes = [ctypes.c_float]\n"
    "        d.espeak_set_volume.restype = None\n"
    "        d.espeak_set_voice_params.argtypes = [ctypes.c_int]*4\n"
    "        d.espeak_set_voice_params.restype = None\n"
    "        d.espeak_set_voice_name.argtypes = [ctypes.c_char_p]\n"
    "        d.espeak_set_voice_name.restype = None\n"
    "    def say(self, text, pitch=0, speed=0, reverb=None, delay=None, pan=None):\n"
    "        '''Speak text. reverb/delay: True/tuple/False. pan: -1.0(L) to 1.0(R).'''\n"
    "        if reverb is None or reverb is False:\n"
    "            self._dll.espeak_set_reverb(0.6, 20.0, 0.25, 0.4, 0)\n"
    "        elif reverb is True:\n"
    "            self._dll.espeak_set_reverb(0.6, 20.0, 0.25, 0.4, 1)\n"
    "        elif isinstance(reverb, (tuple, list)):\n"
    "            r = list(reverb) + [0.6, 20.0, 0.25, 0.4][len(reverb):]\n"
    "            self._dll.espeak_set_reverb(r[0], r[1], r[2], r[3] if len(r)>3 else 0.4, 1)\n"
    "        if delay is None or delay is False:\n"
    "            self._dll.espeak_set_delay(250.0, 375.0, 0.35, 0.3, 0)\n"
    "        elif delay is True:\n"
    "            self._dll.espeak_set_delay(250.0, 375.0, 0.35, 0.3, 1)\n"
    "        elif isinstance(delay, (tuple, list)):\n"
    "            d = list(delay) + [250.0, 375.0, 0.35, 0.3][len(delay):]\n"
    "            self._dll.espeak_set_delay(d[0], d[1], d[2], d[3] if len(d)>3 else 0.3, 1)\n"
    "        if pan is not None:\n"
    "            self._dll.espeak_set_pan(pan)\n"
    "        self._dll.tts_speak(text.encode() if isinstance(text, str) else text, pitch, speed)\n"
    "        if pan is not None:\n"
    "            self._dll.espeak_set_pan(0.0)\n"
    "    def lowpass(self, cutoff=0.45, enabled=True):\n"
    "        '''Lowpass filter. cutoff: 0=heavy, 1=bypass. Default 0.45.'''\n"
    "        self._dll.espeak_set_lowpass(cutoff, int(enabled))\n"
    "    def reverb(self, length=0.6, predelay=20.0, wet=0.25, damping=0.4):\n"
    "        '''Set and enable reverb.'''\n"
    "        self._dll.espeak_set_reverb(length, predelay, wet, damping, 1)\n"
    "    def reverb_on(self): self.reverb()\n"
    "    def reverb_off(self): self._dll.espeak_set_reverb(0.6, 20.0, 0.25, 0.4, 0)\n"
    "    def delay(self, left_ms=250.0, right_ms=375.0, feedback=0.35, wet=0.3):\n"
    "        '''Set and enable stereo delay.'''\n"
    "        self._dll.espeak_set_delay(left_ms, right_ms, feedback, wet, 1)\n"
    "    def delay_on(self): self.delay()\n"
    "    def delay_off(self): self._dll.espeak_set_delay(250.0, 375.0, 0.35, 0.3, 0)\n"
    "    def pan(self, value=0.0):\n"
    "        '''Stereo pan. -1.0=full left, 0.0=center, 1.0=full right.'''\n"
    "        self._dll.espeak_set_pan(value)\n"
    "    def vol(self, value=1.0):\n"
    "        '''Volume. 0.0=silent, 1.0=normal, up to 2.0=boost.'''\n"
    "        self._dll.espeak_set_volume(value)\n"
    "    def voice(self, name=None, rate=-1, pitch=-1, range_=-1, vol=-1):\n"
    "        '''Set eSpeak voice. name: voice name (e.g. \"en\", \"en+f3\"). rate: 80-450 wpm.'''\n"
    "        if name is not None:\n"
    "            self._dll.espeak_set_voice_name(name.encode() if isinstance(name, str) else name)\n"
    "        self._dll.espeak_set_voice_params(rate, pitch, range_, vol)\n"
    "    def rate(self, val):\n"
    "        '''Speech rate in words per minute (80-450, default 175).'''\n"
    "        self._dll.espeak_set_voice_params(val, -1, -1, -1)\n"
    "    def settings(self):\n"
    "        '''Show all DSP and voice settings.'''\n"
    "        import ctypes\n"
    "        buf = ctypes.create_string_buffer(512)\n"
    "        self._dll.espeak_get_settings(buf, 512)\n"
    "        print(buf.value.decode())\n"
    "    def reset(self):\n"
    "        '''Reset everything to defaults.'''\n"
    "        self.lowpass(0.45, True)\n"
    "        self.reverb_off()\n"
    "        self.delay_off()\n"
    "        self.pan(0.0)\n"
    "        self.vol(1.0)\n"
    "        self.voice('en', 175, 50, 50, 100)\n"
    "        print('eSpeak reset to defaults.')\n"
    "    def __repr__(self):\n"
    "        import ctypes\n"
    "        buf = ctypes.create_string_buffer(512)\n"
    "        self._dll.espeak_get_settings(buf, 512)\n"
    "        return buf.value.decode()\n"
    "\n"
    "espeak = _eSpeak()\n"
    "\n"
    "# Register eSpeak help topic\n"
    "if 'help' in dir() and hasattr(help, '__wrapped_topics__'):\n"
    "    help.__wrapped_topics__['espeak'] = (\n"
    "        'eSpeak-ng TTS with DSP (44.1kHz upsampled). Control via `espeak` object:\\n'\n"
    "        '\\n'\n"
    "        '  Speech:\\n'\n"
    "        '  espeak.say(\"text\")                         - speak with lowpass\\n'\n"
    "        '  espeak.say(\"text\", reverb=True)             - + reverb\\n'\n"
    "        '  espeak.say(\"text\", delay=True)              - + stereo delay\\n'\n"
    "        '  espeak.say(\"text\", reverb=True, delay=True) - both\\n'\n"
    "        '  espeak.say(\"text\", pan=-1.0)                - panned left\\n'\n"
    "        '\\n'\n"
    "        '  DSP Controls:\\n'\n"
    "        '  espeak.lowpass(cutoff)     - 0=heavy, 1=bypass (default 0.45)\\n'\n"
    "        '  espeak.reverb(len,pre,wet,damp) / reverb_on() / reverb_off()\\n'\n"
    "        '  espeak.delay(L,R,fb,wet)   / delay_on()  / delay_off()\\n'\n"
    "        '  espeak.pan(val)            - -1.0=left, 0=center, 1.0=right\\n'\n"
    "        '  espeak.vol(val)            - 0=silent, 1=normal, 2=boost\\n'\n"
    "        '\\n'\n"
    "        '  Voice:\\n'\n"
    "        '  espeak.voice(name,rate,pitch,range_,vol) - full voice control\\n'\n"
    "        '  espeak.voice(\"en+f3\")     - female voice variant\\n'\n"
    "        '  espeak.rate(val)           - words per minute 80-450 (default 175)\\n'\n"
    "        '\\n'\n"
    "        '  espeak.settings()  espeak.reset()'\n"
    "    )\n"
    "print('[eSpeak] TTS loaded - type `espeak` or help(\"espeak\")')\n"
    "print()\n"
;

typedef int (*mmudpy_exec_fn)(const char *);

static void espeak_inject_python(void)
{
    for (int i = 0; i < 20; i++) {
        HMODULE m = GetModuleHandleA("mmudpy.dll");
        if (m) {
            mmudpy_exec_fn py_exec = (mmudpy_exec_fn)GetProcAddress(m, "mmudpy_exec");
            if (py_exec) {
                Sleep(500);
                int rc = py_exec(espeak_py_inject);
                if (api) api->log("[TTS_eSpeak] Python injection %s (rc=%d)\n",
                                   rc == 0 ? "OK" : "FAILED", rc);
                return;
            }
        }
        Sleep(500);
    }
    if (api) api->log("[TTS_eSpeak] MMUDPy not found, Python injection skipped\n");
}

static DWORD WINAPI espeak_inject_thread(LPVOID param)
{
    (void)param;
    espeak_inject_python();
    return 0;
}

/* ---- Plugin interface ---- */

static int espeak_plugin_init(const slop_api_t *a)
{
    api = a;

    /* Find espeak-ng-data relative to this DLL */
    char datapath[MAX_PATH];
    HMODULE self = GetModuleHandleA("TTS_eSpeak.dll");
    if (!self) self = GetModuleHandleA("TTS_eSpeak");
    GetModuleFileNameA(self, datapath, MAX_PATH);
    char *slash = strrchr(datapath, '\\');
    if (slash) *slash = '\0';
    strcat(datapath, "\\espeak-ng-data");

    espeak_sr = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0, datapath, 0);
    if (espeak_sr <= 0) {
        api->log("[TTS_eSpeak] espeak_Initialize FAILED (path=%s)\n", datapath);
        return -1;  /* abort plugin load */
    }

    espeak_SetSynthCallback(espeak_synth_cb);
    espeak_SetParameter(espeakRATE, es_rate, 0);
    espeak_SetParameter(espeakPITCH, es_pitch, 0);
    espeak_SetParameter(espeakVOLUME, es_volume, 0);

    api->add_menu_item("Demo (eSpeak TTS)", IDM_ESPEAK_TEST);
    api->log("[TTS_eSpeak] eSpeak-ng loaded (sr=%d, output=%d, lp=%d rv=%d dl=%d)\n",
             espeak_sr, OUTPUT_RATE, lp_enabled, rv_enabled, dl_enabled);

    CreateThread(NULL, 0, espeak_inject_thread, NULL, 0, NULL);
    return 0;
}

static void espeak_plugin_shutdown(void)
{
    if (hWaveOut) {
        waveOutReset(hWaveOut);
        waveOutClose(hWaveOut);
        hWaveOut = NULL;
    }
    tts_playing = 0;
    if (tts_done_event) {
        CloseHandle(tts_done_event);
        tts_done_event = NULL;
    }
    espeak_Terminate();
    if (api) api->log("[TTS_eSpeak] Unloaded\n");
}

/* Demo thread */
/* Known struct offsets for demo readout */
#define OFF_PLAYER_NAME     0x537C
#define OFF_PLAYER_CUR_HP   0x53D4
#define OFF_PLAYER_MAX_HP   0x53DC
#define OFF_PLAYER_CUR_MANA 0x53E0
#define OFF_PLAYER_MAX_MANA 0x53E8
#define OFF_CONNECTED       0x563C

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
static DWORD WINAPI espeak_msgbox_thread(LPVOID param)
{
    HWND parent = api ? api->get_mmmain_hwnd() : NULL;
    MessageBoxA(parent, (const char *)param, "eSpeak TTS Demo", MB_OK | MB_ICONINFORMATION);
    free(param);
    return 0;
}

static DWORD WINAPI espeak_demo_thread(LPVOID param)
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

    /* Save settings */
    float orig_lp = lp_cutoff; int orig_lp_en = lp_enabled;
    float orig_rv_len = rv_length, orig_rv_pre = rv_predelay;
    float orig_rv_wet = rv_wet, orig_rv_damp = rv_damping; int orig_rv_en = rv_enabled;
    float orig_dl_l = dl_left_ms, orig_dl_r = dl_right_ms;
    float orig_dl_fb = dl_feedback, orig_dl_w = dl_wet; int orig_dl_en = dl_enabled;
    float orig_pan = pan_value, orig_vol = volume;
    int orig_rate = es_rate, orig_pitch = es_pitch;
    int orig_range = es_range, orig_volume_v = es_volume;

    /* Show transcript popup on background thread so speech plays immediately */
    char *transcript = (char *)malloc(1024);
    sprintf(transcript,
        "Hello %s.\n"
        "This is eSpeak, text to speech engine.\n"
        "You have %d out of %d hit points.\n"
        "And you are at %d percent mana.\n"
        "Enjoy your script.",
        name, cur_hp, max_hp, mana_pct);
    CreateThread(NULL, 0, espeak_msgbox_thread, transcript, 0, NULL);

    /* Line 1: Greeting - normal voice, panning left */
    espeak_set_lowpass(0.45f, 1);
    espeak_set_reverb(0.4f, 15.0f, 0.1f, 0.4f, 0);
    espeak_set_delay(120.0f, 180.0f, 0.1f, 0.08f, 0);
    espeak_set_volume(1.0f);
    espeak_set_voice_params(175, 50, 50, 100);
    espeak_set_pan(-0.5f);
    sprintf(line, "Hello %s.", name);
    if (api) api->log("[eSpeak Demo] %s\n", line);
    tts_speak(line, 0, 0);
    tts_wait(); Sleep(200);

    /* Line 2: Engine intro - deep voice, light reverb, center */
    espeak_set_voice_params(140, 30, 40, 100);
    espeak_set_reverb(0.3f, 10.0f, 0.08f, 0.3f, 1);
    espeak_set_pan(0.0f);
    if (api) api->log("[eSpeak Demo] This is eSpeak, text to speech engine.\n");
    tts_speak("This is eSpeak, text to speech engine.", 0, 0);
    tts_wait(); Sleep(300);

    /* Line 3: Health - fast voice, subtle delay, panning right */
    espeak_set_voice_params(200, 60, 60, 90);
    espeak_set_reverb(0.3f, 10.0f, 0.08f, 0.4f, 0);
    espeak_set_delay(100.0f, 150.0f, 0.08f, 0.06f, 1);
    espeak_set_pan(0.5f);
    sprintf(line, "You have %d out of %d hit points.", cur_hp, max_hp);
    if (api) api->log("[eSpeak Demo] %s\n", line);
    tts_speak(line, 0, 0);
    tts_wait(); Sleep(300);

    /* Line 4: Mana - robotic, mild reverb+delay */
    espeak_set_voice_params(130, 10, 5, 100);
    espeak_set_reverb(0.3f, 8.0f, 0.08f, 0.2f, 1);
    espeak_set_delay(80.0f, 120.0f, 0.08f, 0.06f, 1);
    espeak_set_lowpass(0.25f, 1);
    espeak_set_pan(-0.3f);
    espeak_set_volume(0.9f);
    sprintf(line, "And you are at %d percent mana.", mana_pct);
    if (api) api->log("[eSpeak Demo] %s\n", line);
    tts_speak(line, 0, 0);
    tts_wait(); Sleep(400);

    /* Line 5: Sign off - quiet, gentle reverb, NO delay */
    espeak_set_voice_params(110, 65, 70, 70);
    espeak_set_reverb(0.3f, 10.0f, 0.08f, 0.5f, 1);
    espeak_set_delay(0.0f, 0.0f, 0.0f, 0.0f, 0);
    espeak_set_volume(0.65f);
    espeak_set_lowpass(0.5f, 1);
    espeak_set_pan(0.2f);
    if (api) api->log("[eSpeak Demo] Enjoy your script.\n");
    tts_speak("Enjoy your script.", 0, 0);
    tts_wait();

    /* Restore */
    espeak_set_lowpass(orig_lp, orig_lp_en);
    espeak_set_reverb(orig_rv_len, orig_rv_pre, orig_rv_wet, orig_rv_damp, orig_rv_en);
    espeak_set_delay(orig_dl_l, orig_dl_r, orig_dl_fb, orig_dl_w, orig_dl_en);
    espeak_set_pan(orig_pan);
    espeak_set_volume(orig_vol);
    espeak_set_voice_params(orig_rate, orig_pitch, orig_range, orig_volume_v);

    return 0;
}

static int espeak_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;
    if (msg == WM_COMMAND) {
        WORD id = LOWORD(wParam);
        if (id == IDM_ESPEAK_TEST) {
            if (api) api->log("[TTS_eSpeak] Demo clicked (tts_playing=%d)\n", tts_playing);
            if (!tts_playing)
                CreateThread(NULL, 0, espeak_demo_thread, NULL, 0, NULL);
            return 1;
        }
    }
    return 0;
}

static slop_plugin_t espeak_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "eSpeak",
    .author      = "Tripmunk",
    .description = "eSpeak-ng TTS with DSP (44.1kHz upsampled)",
    .version     = "1.0.0",
    .init        = espeak_plugin_init,
    .shutdown    = espeak_plugin_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = espeak_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &espeak_plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
