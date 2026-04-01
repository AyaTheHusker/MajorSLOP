/*
 * tts_sam.c — SAM (Software Automatic Mouth) TTS Plugin
 *
 * Classic 1982 C64-style text-to-speech. Exports tts_speak() for
 * the main DLL bridge, and provides menu controls.
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

/* SAM source */
int debug = 0;  /* SAM debug global */
#include "sam/sam.h"
#include "sam/reciter.h"

static const slop_api_t *api = NULL;

/* Menu IDs */
#define IDM_SAM_TEST    50300

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
 * tts_speak() — The exported TTS function.
 * Main DLL discovers this via GetProcAddress("tts_speak").
 *
 * text:  English text to speak
 * pitch: 0 = default (64), or 1-255
 * speed: 0 = default (72), or 1-255
 */
__declspec(dllexport) void tts_speak(const char *text, int pitch, int speed)
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
    SetPitch(pitch > 0 ? (unsigned char)pitch : 64);
    SetSpeed(speed > 0 ? (unsigned char)speed : 72);
    SetMouth(128);
    SetThroat(128);

    /* Synthesize */
    if (!SAMMain()) {
        if (api) api->log("[TTS_SAM] Synthesis failed\n");
        return;
    }

    char *buf = GetBuffer();
    int len = GetBufferLength();
    if (!buf || len <= 0) return;

    if (api) api->log("[TTS_SAM] Speaking: \"%s\" (%d bytes, pitch=%d speed=%d)\n",
                       text, len, pitch > 0 ? pitch : 64, speed > 0 ? speed : 72);

    /* Play via waveOut — SAM: 8-bit unsigned PCM, 22050Hz, mono */
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = 22050;
    wfx.wBitsPerSample = 8;
    wfx.nBlockAlign = 1;
    wfx.nAvgBytesPerSec = 22050;

    HWAVEOUT hwo = NULL;
    MMRESULT rv = waveOutOpen(&hwo, WAVE_MAPPER, &wfx,
                               (DWORD_PTR)wave_out_callback, 0, CALLBACK_FUNCTION);
    if (rv != MMSYSERR_NOERROR) {
        if (api) api->log("[TTS_SAM] waveOutOpen failed (code %d)\n", rv);
        return;
    }

    /* Copy buffer — SAM's internal buffer gets reused on next call */
    WAVEHDR *hdr = (WAVEHDR *)calloc(1, sizeof(WAVEHDR));
    hdr->lpData = (char *)malloc(len);
    memcpy(hdr->lpData, buf, len);
    hdr->dwBufferLength = len;

    waveOutPrepareHeader(hwo, hdr, sizeof(WAVEHDR));
    tts_playing = 1;
    waveOutWrite(hwo, hdr, sizeof(WAVEHDR));
}

/* ---- Plugin interface ---- */

static int sam_init(const slop_api_t *a)
{
    api = a;
    api->add_menu_item("Test SAM Voice", IDM_SAM_TEST);
    api->log("[TTS_SAM] SAM text-to-speech loaded\n");
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
        tts_speak("Hello, I am Sam. I am a voice synthesizer from nineteen eighty two.", 0, 0);
        return 1;
    }
    return 0;
}

static slop_plugin_t sam_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "SAM",
    .author      = "MajorSLOP",
    .description = "SAM (Software Automatic Mouth) text-to-speech",
    .version     = "1.0.0",
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
