/*
 * voice.c — MajorSLOP Voice Control Plugin
 *
 * Offline speech recognition (PocketSphinx) + text-to-speech (SAM + eSpeak).
 * Push-to-talk via configurable hotkey, waveIn audio capture.
 * TTS accessible via TCP bridge commands: SAMSAY, ESPEAKSAY
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o voice.dll voice.c \
 *       sam/sam.c sam/reciter.c sam/render.c sam/debug.c \
 *       -I/home/bucka/AI/pocketsphinx-win32/include \
 *       -L/home/bucka/AI/pocketsphinx-win32/lib \
 *       -lpocketsphinx -lwinmm -lgdi32 -luser32 -lm
 *
 * Install:
 *   Copy voice.dll to MegaMUD/plugins/
 *   Copy voice-model/ directory to MegaMUD/plugins/voice-model/
 */

#include "slop_plugin.h"
#include <pocketsphinx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SAM TTS is in the main DLL (msimg32_proxy.c), not here.
 * Voice.dll handles speech RECOGNITION only.
 * TTS is accessed via TCP bridge: SAMSAY, ESPEAKSAY */

/* Disable pocketsphinx DLL import decorations (we link statically) */
#ifdef POCKETSPHINX_EXPORT
#undef POCKETSPHINX_EXPORT
#define POCKETSPHINX_EXPORT
#endif

/* ---- Configuration ---- */
#define VOICE_PTT_KEY       VK_F9       /* default push-to-talk key */
#define VOICE_SAMPLE_RATE   16000       /* PocketSphinx expects 16kHz */
#define VOICE_CHANNELS      1           /* mono */
#define VOICE_BITS          16          /* 16-bit PCM */
#define VOICE_BUF_SAMPLES   1024        /* samples per buffer */
#define VOICE_NUM_BUFS      4           /* double-buffering for audio */
#define VOICE_MAX_RESULT    256         /* max recognized text length */

/* ---- Menu IDs ---- */
#define IDM_VOICE_TOGGLE    50200
#define IDM_VOICE_PTT_CFG   50201
#define IDM_VOICE_DEVICE    50202
#define IDM_VOICE_REBUILD   50203

/* ---- State ---- */
static const slop_api_t *api = NULL;
static ps_decoder_t *decoder = NULL;
static ps_config_t *ps_cfg = NULL;

static HWAVEIN hWaveIn = NULL;
static WAVEHDR waveHdrs[VOICE_NUM_BUFS];
static char waveBufs[VOICE_NUM_BUFS][VOICE_BUF_SAMPLES * VOICE_BITS / 8];

static volatile int voice_enabled = 0;     /* plugin enabled */
static volatile int ptt_held = 0;          /* push-to-talk key held */
static volatile int recording = 0;         /* actively recording */
static int ptt_key = VOICE_PTT_KEY;        /* configurable PTT key */
static int audio_device_id = 0;            /* waveIn device index */
static char audio_device_name[64] = "Default";

static char last_result[VOICE_MAX_RESULT] = {0};
static char model_dir[MAX_PATH] = {0};     /* path to voice-model/ */

/* ---- Forward declarations ---- */
static void voice_start_recording(void);
static void voice_stop_recording(void);
static int voice_init_decoder(void);
static void voice_build_grammar(void);
static void CALLBACK wave_in_callback(HWAVEIN hwi, UINT msg, DWORD_PTR inst,
                                       DWORD_PTR param1, DWORD_PTR param2);

/* ---- Grammar ---- */
#define MAX_GRAMMAR_SIZE  (256 * 1024)  /* 256KB max grammar string */
static char *grammar_str = NULL;

/* Build path to model directory (next to the DLL) */
static void find_model_dir(void)
{
    /* MegaMUD directory */
    GetModuleFileNameA(NULL, model_dir, MAX_PATH);
    char *slash = strrchr(model_dir, '\\');
    if (slash) *(slash + 1) = '\0';
    strncat(model_dir, "plugins\\voice-model", MAX_PATH - strlen(model_dir) - 1);
}

/* ---- Audio device enumeration ---- */
static int enumerate_devices(void)
{
    int count = waveInGetNumDevs();
    api->log("[voice] Found %d audio input device(s):\n", count);
    for (int i = 0; i < count; i++) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            api->log("[voice]   %d: %s\n", i, caps.szPname);
        }
    }
    if (count > 0) {
        WAVEINCAPSA caps;
        if (waveInGetDevCapsA(audio_device_id, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            strncpy(audio_device_name, caps.szPname, sizeof(audio_device_name) - 1);
        }
    }
    return count;
}

/* ---- PocketSphinx decoder init ---- */
static int voice_init_decoder(void)
{
    find_model_dir();

    /* Check model directory exists */
    DWORD attr = GetFileAttributesA(model_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        api->log("[voice] ERROR: Model directory not found: %s\n", model_dir);
        api->log("[voice] Copy the en-us model to plugins/voice-model/\n");
        return -1;
    }

    /* Build HMM path */
    char hmm_path[MAX_PATH];
    sprintf(hmm_path, "%s\\en-us", model_dir);

    char dict_path[MAX_PATH];
    sprintf(dict_path, "%s\\cmudict-en-us.dict", model_dir);

    ps_cfg = ps_config_init(NULL);
    if (!ps_cfg) {
        api->log("[voice] ERROR: ps_config_init failed\n");
        return -1;
    }

    ps_config_set_str(ps_cfg, "hmm", hmm_path);
    ps_config_set_str(ps_cfg, "dict", dict_path);
    ps_config_set_str(ps_cfg, "logfn", "NUL");  /* suppress PS logs */
    ps_config_set_int(ps_cfg, "samprate", VOICE_SAMPLE_RATE);

    decoder = ps_init(ps_cfg);
    if (!decoder) {
        api->log("[voice] ERROR: ps_init failed — check model path: %s\n", hmm_path);
        return -1;
    }

    api->log("[voice] PocketSphinx decoder initialized (model: %s)\n", hmm_path);
    return 0;
}

/* ---- Grammar builder ---- */

/* Scan .mp files and extract loop names for grammar */
static int scan_loop_names(char *buf, int maxlen)
{
    char mp_dir[MAX_PATH];
    GetModuleFileNameA(NULL, mp_dir, MAX_PATH);
    char *slash = strrchr(mp_dir, '\\');
    if (slash) *(slash + 1) = '\0';

    int pos = 0;
    int count = 0;
    const char *subdirs[] = { "Default", "Chars\\All" };

    for (int d = 0; d < 2; d++) {
        char search_path[MAX_PATH];
        sprintf(search_path, "%s%s\\*.mp", mp_dir, subdirs[d]);

        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search_path, &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;

        do {
            /* Read first line to get loop name: [LoopName][Creator] */
            char full_path[MAX_PATH];
            sprintf(full_path, "%s%s\\%s", mp_dir, subdirs[d], fd.cFileName);

            FILE *f = fopen(full_path, "r");
            if (!f) continue;

            char line[256];
            if (fgets(line, sizeof(line), f)) {
                /* Extract [LoopName] */
                if (line[0] == '[') {
                    char *end = strchr(line + 1, ']');
                    if (end && (end - line - 1) > 0) {
                        *end = '\0';
                        const char *name = line + 1;

                        /* Convert to lowercase for grammar */
                        char lower[128];
                        int len = 0;
                        for (const char *p = name; *p && len < 126; p++) {
                            char c = *p;
                            if (c >= 'A' && c <= 'Z') c += 32;
                            /* Only keep letters, digits, spaces */
                            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
                                lower[len++] = c;
                            } else if (c == '-' || c == '_' || c == '\'') {
                                lower[len++] = ' '; /* normalize separators to spaces */
                            }
                        }
                        lower[len] = '\0';

                        /* Trim trailing spaces */
                        while (len > 0 && lower[len-1] == ' ') lower[--len] = '\0';

                        if (len > 1) {
                            int wrote;
                            if (count == 0) {
                                wrote = snprintf(buf + pos, maxlen - pos, "%s", lower);
                            } else {
                                wrote = snprintf(buf + pos, maxlen - pos, " | %s", lower);
                            }
                            if (wrote > 0 && pos + wrote < maxlen) {
                                pos += wrote;
                                count++;
                            }
                        }
                    }
                }
            }
            fclose(f);
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }

    api->log("[voice] Scanned %d loop/path names for grammar\n", count);
    return count;
}

static void voice_build_grammar(void)
{
    if (grammar_str) { free(grammar_str); grammar_str = NULL; }
    grammar_str = (char *)malloc(MAX_GRAMMAR_SIZE);
    if (!grammar_str) return;

    /* Build loop names list */
    char *loop_names = (char *)malloc(128 * 1024);
    if (!loop_names) { free(grammar_str); grammar_str = NULL; return; }
    loop_names[0] = '\0';
    int nloops = scan_loop_names(loop_names, 128 * 1024);

    /* Build the JSGF grammar */
    int pos = 0;
    pos += sprintf(grammar_str + pos,
        "#JSGF V1.0;\n"
        "grammar mudcommands;\n\n");

    /* Directions */
    pos += sprintf(grammar_str + pos,
        "<direction> = north | south | east | west | up | down "
        "| northeast | northwest | southeast | southwest "
        "| n | s | e | w | ne | nw | se | sw;\n\n");

    /* Combat */
    pos += sprintf(grammar_str + pos,
        "<combat> = attack | flee | backstab | kill;\n\n");

    /* Utility */
    pos += sprintf(grammar_str + pos,
        "<utility> = rest | meditate | look | search | sneak | hide "
        "| pick lock | get all | drop all | inventory;\n\n");

    /* Loop names */
    if (nloops > 0) {
        pos += sprintf(grammar_str + pos,
            "<loop_name> = %s;\n\n", loop_names);
    } else {
        pos += sprintf(grammar_str + pos,
            "<loop_name> = placeholder;\n\n");
    }

    /* Loop/path commands */
    pos += sprintf(grammar_str + pos,
        "<loopcommand> = loop <loop_name> | go to <loop_name> "
        "| walk me to <loop_name> | run me to <loop_name> "
        "| stop loop | stop;\n\n");

    /* Spell casting (basic for now — will be expanded with MDB2 data) */
    pos += sprintf(grammar_str + pos,
        "<castverb> = cast | sing | invoke;\n\n");

    /* Info queries (for MMUDPy voice hooks) */
    pos += sprintf(grammar_str + pos,
        "<query> = check exp | check rate | check stats | check health "
        "| how much exp | what is my rate | exp check | status;\n\n");

    /* Top-level rule */
    pos += sprintf(grammar_str + pos,
        "public <command> = <direction> | <combat> | <utility> "
        "| <loopcommand> | <castverb> | <query>;\n");

    free(loop_names);

    /* Load grammar into decoder */
    if (decoder) {
        int rv = ps_add_jsgf_string(decoder, "mudcmds", grammar_str);
        if (rv < 0) {
            api->log("[voice] ERROR: Failed to load JSGF grammar\n");
        } else {
            ps_activate_search(decoder, "mudcmds");
            api->log("[voice] Grammar loaded (%d bytes, %d loops)\n", pos, nloops);
        }
    }
}

/* ---- Audio capture ---- */

static void CALLBACK wave_in_callback(HWAVEIN hwi, UINT msg, DWORD_PTR inst,
                                       DWORD_PTR param1, DWORD_PTR param2)
{
    (void)inst; (void)param2;

    if (msg == WIM_DATA && recording && decoder) {
        WAVEHDR *hdr = (WAVEHDR *)param1;

        /* Feed audio to PocketSphinx */
        ps_process_raw(decoder,
                       (const int16 *)hdr->lpData,
                       hdr->dwBytesRecorded / 2,  /* samples = bytes / 2 for 16-bit */
                       FALSE, FALSE);

        /* Re-queue buffer if still recording */
        if (recording) {
            hdr->dwFlags &= ~WHDR_DONE;
            waveInAddBuffer(hwi, hdr, sizeof(WAVEHDR));
        }
    }
}

static void voice_start_recording(void)
{
    if (recording || !decoder) return;

    /* Start utterance */
    if (ps_start_utt(decoder) < 0) {
        api->log("[voice] ERROR: ps_start_utt failed\n");
        return;
    }

    /* Open audio device */
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = VOICE_CHANNELS;
    wfx.nSamplesPerSec = VOICE_SAMPLE_RATE;
    wfx.wBitsPerSample = VOICE_BITS;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT rv = waveInOpen(&hWaveIn, audio_device_id, &wfx,
                              (DWORD_PTR)wave_in_callback, 0, CALLBACK_FUNCTION);
    if (rv != MMSYSERR_NOERROR) {
        api->log("[voice] ERROR: waveInOpen failed (code %d)\n", rv);
        ps_end_utt(decoder);
        return;
    }

    /* Prepare and queue buffers */
    int buf_bytes = VOICE_BUF_SAMPLES * (VOICE_BITS / 8);
    for (int i = 0; i < VOICE_NUM_BUFS; i++) {
        memset(&waveHdrs[i], 0, sizeof(WAVEHDR));
        waveHdrs[i].lpData = waveBufs[i];
        waveHdrs[i].dwBufferLength = buf_bytes;
        waveInPrepareHeader(hWaveIn, &waveHdrs[i], sizeof(WAVEHDR));
        waveInAddBuffer(hWaveIn, &waveHdrs[i], sizeof(WAVEHDR));
    }

    recording = 1;
    waveInStart(hWaveIn);
    api->log("[voice] Recording started (PTT held)\n");
}

static void voice_stop_recording(void)
{
    if (!recording) return;
    recording = 0;

    /* Stop capture */
    waveInStop(hWaveIn);
    waveInReset(hWaveIn);

    /* Unprepare buffers */
    for (int i = 0; i < VOICE_NUM_BUFS; i++) {
        waveInUnprepareHeader(hWaveIn, &waveHdrs[i], sizeof(WAVEHDR));
    }
    waveInClose(hWaveIn);
    hWaveIn = NULL;

    /* Finalize recognition */
    ps_end_utt(decoder);

    /* Get hypothesis */
    const char *hyp = ps_get_hyp(decoder, NULL);
    if (hyp && hyp[0]) {
        strncpy(last_result, hyp, VOICE_MAX_RESULT - 1);
        last_result[VOICE_MAX_RESULT - 1] = '\0';
        api->log("[voice] Recognized: \"%s\"\n", last_result);

        /* TODO: Map recognized text to MUD command and inject.
         * For now, just log it. Command mapping will be Phase 6. */
        api->inject_command(last_result);
    } else {
        api->log("[voice] No speech recognized\n");
        last_result[0] = '\0';
    }
}

/* ---- Plugin callbacks ---- */

static int voice_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)hwnd; (void)lParam;

    /* Menu commands */
    if (msg == WM_COMMAND) {
        int id = LOWORD(wParam);
        if (id == IDM_VOICE_TOGGLE) {
            voice_enabled = !voice_enabled;
            api->log("[voice] Voice control %s\n", voice_enabled ? "enabled" : "disabled");
            return 1;
        }
        if (id == IDM_VOICE_REBUILD) {
            voice_build_grammar();
            return 1;
        }
    }

    /* Push-to-talk key handling */
    if (voice_enabled && decoder) {
        if (msg == WM_KEYDOWN && (int)wParam == ptt_key) {
            if (!ptt_held) {
                ptt_held = 1;
                voice_start_recording();
            }
            return 1;  /* consume the key */
        }
        if (msg == WM_KEYUP && (int)wParam == ptt_key) {
            if (ptt_held) {
                ptt_held = 0;
                voice_stop_recording();
            }
            return 1;
        }
    }

    return 0;
}

static int voice_init(const slop_api_t *a)
{
    api = a;

    api->log("[voice] Initializing voice control plugin...\n");

    /* Enumerate audio devices */
    int ndevs = enumerate_devices();
    if (ndevs == 0) {
        api->log("[voice] WARNING: No audio input devices found\n");
    }

    /* Initialize PocketSphinx */
    if (voice_init_decoder() < 0) {
        api->log("[voice] Decoder init failed — voice commands unavailable\n");
        /* Don't abort plugin — user might add model later */
    } else {
        /* Build initial grammar */
        voice_build_grammar();
    }

    /* Register menu items */
    api->add_menu_item("Enable/Disable Voice", IDM_VOICE_TOGGLE);
    api->add_menu_item("Rebuild Grammar", IDM_VOICE_REBUILD);

    api->log("[voice] Plugin loaded (PTT key: F%d, device: %s)\n",
             ptt_key - VK_F1 + 1, audio_device_name);
    return 0;
}

static void voice_shutdown(void)
{
    if (recording) voice_stop_recording();

    if (decoder) {
        ps_free(decoder);
        decoder = NULL;
    }
    if (ps_cfg) {
        ps_config_free(ps_cfg);
        ps_cfg = NULL;
    }
    if (grammar_str) {
        free(grammar_str);
        grammar_str = NULL;
    }

    if (api) api->log("[voice] Plugin unloaded\n");
}

/* ---- Plugin descriptor ---- */

static slop_plugin_t voice_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Voice Control",
    .author      = "MajorSLOP",
    .description = "Push-to-talk voice commands via PocketSphinx",
    .version     = "0.1.0",
    .init        = voice_init,
    .shutdown    = voice_shutdown,
    .on_line     = NULL,
    .on_round    = NULL,
    .on_wndproc  = voice_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &voice_plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
