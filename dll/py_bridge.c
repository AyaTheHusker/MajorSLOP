/*
 * py_bridge.dll — MajorSLOP Plugin: Python Bridge
 * =================================================
 *
 * Opens a TCP server that a native Python process connects to.
 * Forwards terminal lines, round ticks, and struct data to Python.
 * Receives commands from Python (inject text, read memory, etc).
 *
 * Protocol: JSON lines over TCP (\n delimited).
 *
 * Events (DLL -> Python):
 *   {"event":"line","text":"..."}
 *   {"event":"round","num":N}
 *   {"event":"connected"}
 *   {"event":"shutdown"}
 *
 * Requests (Python -> DLL):
 *   {"cmd":"read_i32","offset":N,"id":N}       -> {"id":N,"result":V}
 *   {"cmd":"read_i16","offset":N,"id":N}       -> {"id":N,"result":V}
 *   {"cmd":"read_string","offset":N,"max":N,"id":N} -> {"id":N,"result":"..."}
 *   {"cmd":"inject_command","text":"...","id":N} -> {"id":N,"result":"ok"}
 *   {"cmd":"inject_text","text":"...","id":N}   -> {"id":N,"result":"ok"}
 *   {"cmd":"struct_base","id":N}                -> {"id":N,"result":V}
 *   {"cmd":"ping","id":N}                       -> {"id":N,"result":"pong"}
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o py_bridge.dll py_bridge.c \
 *       -lws2_32 -lgdi32 -luser32 -mwindows
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include "slop_plugin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")

/* ---- Config ---- */
#define PYB_PORT_START  9950
#define PYB_PORT_END    9960
#define PYB_MAX_LINE    4096
#define PYB_RECV_BUF    8192

/* ---- State ---- */
static const slop_api_t *api = NULL;
static SOCKET pyb_server = INVALID_SOCKET;
static SOCKET pyb_client = INVALID_SOCKET;
static int pyb_port = 0;
static volatile int pyb_running = 0;
static CRITICAL_SECTION pyb_send_lock;

/* ---- Helpers ---- */

/* Send a JSON line to the connected Python client (thread-safe) */
static void pyb_send(const char *json_line)
{
    if (pyb_client == INVALID_SOCKET) return;

    EnterCriticalSection(&pyb_send_lock);
    int len = (int)strlen(json_line);
    int sent = send(pyb_client, json_line, len, 0);
    if (sent > 0) {
        send(pyb_client, "\n", 1, 0);
    }
    LeaveCriticalSection(&pyb_send_lock);
}

/* Escape a string for JSON (handles \, ", newlines, tabs) */
static int json_escape(const char *in, char *out, int out_sz)
{
    int pos = 0;
    for (int i = 0; in[i] && pos < out_sz - 2; i++) {
        char c = in[i];
        if (c == '\\' || c == '"') {
            if (pos + 2 >= out_sz) break;
            out[pos++] = '\\';
            out[pos++] = c;
        } else if (c == '\n') {
            if (pos + 2 >= out_sz) break;
            out[pos++] = '\\';
            out[pos++] = 'n';
        } else if (c == '\r') {
            if (pos + 2 >= out_sz) break;
            out[pos++] = '\\';
            out[pos++] = 'r';
        } else if (c == '\t') {
            if (pos + 2 >= out_sz) break;
            out[pos++] = '\\';
            out[pos++] = 't';
        } else if ((unsigned char)c < 0x20) {
            /* skip other control chars */
        } else {
            out[pos++] = c;
        }
    }
    out[pos] = '\0';
    return pos;
}

/* Simple JSON string value extractor — finds "key":"value" or "key":number */
static int json_get_string(const char *json, const char *key, char *out, int out_sz)
{
    char needle[128];
    sprintf(needle, "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    int i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            if (*p == 'n') out[i++] = '\n';
            else if (*p == 't') out[i++] = '\t';
            else if (*p == 'r') out[i++] = '\r';
            else out[i++] = *p;
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

static int json_get_int(const char *json, const char *key, int *out)
{
    char needle[128];
    sprintf(needle, "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    /* skip whitespace */
    while (*p == ' ') p++;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        *out = atoi(p);
        return 1;
    }
    return 0;
}

/* ---- Request handler ---- */

static void pyb_handle_request(const char *line)
{
    char cmd[64] = {0};
    int id = 0;

    json_get_string(line, "cmd", cmd, sizeof(cmd));
    json_get_int(line, "id", &id);

    char response[PYB_MAX_LINE];

    if (strcmp(cmd, "ping") == 0) {
        sprintf(response, "{\"id\":%d,\"result\":\"pong\"}", id);
        pyb_send(response);
    }
    else if (strcmp(cmd, "struct_base") == 0) {
        unsigned int base = api->get_struct_base();
        sprintf(response, "{\"id\":%d,\"result\":%u}", id, base);
        pyb_send(response);
    }
    else if (strcmp(cmd, "read_i32") == 0) {
        int offset = 0;
        json_get_int(line, "offset", &offset);
        int val = api->read_struct_i32((unsigned int)offset);
        sprintf(response, "{\"id\":%d,\"result\":%d}", id, val);
        pyb_send(response);
    }
    else if (strcmp(cmd, "read_i16") == 0) {
        int offset = 0;
        json_get_int(line, "offset", &offset);
        int val = api->read_struct_i16((unsigned int)offset);
        sprintf(response, "{\"id\":%d,\"result\":%d}", id, val);
        pyb_send(response);
    }
    else if (strcmp(cmd, "read_string") == 0) {
        int offset = 0, maxlen = 64;
        json_get_int(line, "offset", &offset);
        json_get_int(line, "max", &maxlen);
        if (maxlen > 512) maxlen = 512;

        unsigned int base = api->get_struct_base();
        char raw[514] = {0};
        if (base && maxlen > 0) {
            memcpy(raw, (const char *)(base + offset), maxlen);
            raw[maxlen] = '\0';
        }
        char escaped[1024];
        json_escape(raw, escaped, sizeof(escaped));
        sprintf(response, "{\"id\":%d,\"result\":\"%s\"}", id, escaped);
        pyb_send(response);
    }
    else if (strcmp(cmd, "read_terminal") == 0) {
        int row = 0;
        json_get_int(line, "row", &row);
        char buf[140] = {0};
        api->read_terminal_row(row, buf, sizeof(buf));
        char escaped[300];
        json_escape(buf, escaped, sizeof(escaped));
        sprintf(response, "{\"id\":%d,\"result\":\"%s\"}", id, escaped);
        pyb_send(response);
    }
    else if (strcmp(cmd, "inject_command") == 0) {
        char text[512] = {0};
        json_get_string(line, "text", text, sizeof(text));
        api->inject_command(text);
        sprintf(response, "{\"id\":%d,\"result\":\"ok\"}", id);
        pyb_send(response);
    }
    else if (strcmp(cmd, "inject_text") == 0) {
        char text[512] = {0};
        json_get_string(line, "text", text, sizeof(text));
        api->inject_text(text);
        sprintf(response, "{\"id\":%d,\"result\":\"ok\"}", id);
        pyb_send(response);
    }
    else if (strcmp(cmd, "write_i32") == 0) {
        int offset = 0, value = 0;
        json_get_int(line, "offset", &offset);
        json_get_int(line, "value", &value);
        unsigned int base = api->get_struct_base();
        if (base) {
            *(int *)(base + offset) = value;
            sprintf(response, "{\"id\":%d,\"result\":\"ok\"}", id);
        } else {
            sprintf(response, "{\"id\":%d,\"error\":\"no struct base\"}", id);
        }
        pyb_send(response);
    }
    else if (strcmp(cmd, "write_i16") == 0) {
        int offset = 0, value = 0;
        json_get_int(line, "offset", &offset);
        json_get_int(line, "value", &value);
        unsigned int base = api->get_struct_base();
        if (base) {
            *(short *)(base + offset) = (short)value;
            sprintf(response, "{\"id\":%d,\"result\":\"ok\"}", id);
        } else {
            sprintf(response, "{\"id\":%d,\"error\":\"no struct base\"}", id);
        }
        pyb_send(response);
    }
    else if (strcmp(cmd, "write_string") == 0) {
        int offset = 0, maxlen = 64;
        char text[514] = {0};
        json_get_int(line, "offset", &offset);
        json_get_int(line, "max", &maxlen);
        json_get_string(line, "text", text, sizeof(text));
        if (maxlen > 512) maxlen = 512;
        unsigned int base = api->get_struct_base();
        if (base) {
            strncpy((char *)(base + offset), text, maxlen);
            ((char *)(base + offset))[maxlen - 1] = '\0';
            sprintf(response, "{\"id\":%d,\"result\":\"ok\"}", id);
        } else {
            sprintf(response, "{\"id\":%d,\"error\":\"no struct base\"}", id);
        }
        pyb_send(response);
    }
    else {
        sprintf(response, "{\"id\":%d,\"error\":\"unknown command: %s\"}", id, cmd);
        pyb_send(response);
    }
}

/* ---- Client handler thread ---- */

static DWORD WINAPI pyb_client_thread(LPVOID param)
{
    (void)param;
    char buf[PYB_RECV_BUF];
    char line_buf[PYB_MAX_LINE];
    int line_pos = 0;

    api->log("[py_bridge] Client connected\n");

    /* Send connected event */
    {
        char msg[256];
        sprintf(msg, "{\"event\":\"connected\",\"api_version\":%d,\"terminal_rows\":%d,\"terminal_cols\":%d}",
                SLOP_API_VERSION, api->terminal_rows, api->terminal_cols);
        pyb_send(msg);
    }

    while (pyb_running && pyb_client != INVALID_SOCKET) {
        int n = recv(pyb_client, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        /* Feed into line buffer */
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    pyb_handle_request(line_buf);
                    line_pos = 0;
                }
            } else if (line_pos < PYB_MAX_LINE - 1) {
                line_buf[line_pos++] = buf[i];
            }
        }
    }

    api->log("[py_bridge] Client disconnected\n");
    closesocket(pyb_client);
    pyb_client = INVALID_SOCKET;
    return 0;
}

/* ---- Server thread ---- */

static DWORD WINAPI pyb_server_thread(LPVOID param)
{
    (void)param;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    pyb_server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pyb_server == INVALID_SOCKET) {
        api->log("[py_bridge] socket() failed\n");
        return 1;
    }

    /* Try ports 9950-9960 */
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int bound = 0;
    for (int port = PYB_PORT_START; port <= PYB_PORT_END; port++) {
        addr.sin_port = htons((unsigned short)port);
        if (bind(pyb_server, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            pyb_port = port;
            bound = 1;
            break;
        }
    }

    if (!bound) {
        api->log("[py_bridge] Could not bind any port %d-%d\n", PYB_PORT_START, PYB_PORT_END);
        closesocket(pyb_server);
        pyb_server = INVALID_SOCKET;
        return 1;
    }

    listen(pyb_server, 2);
    api->log("[py_bridge] Listening on 127.0.0.1:%d\n", pyb_port);

    while (pyb_running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client = accept(pyb_server, (struct sockaddr *)&client_addr, &client_len);
        if (client == INVALID_SOCKET) {
            if (pyb_running) api->log("[py_bridge] accept() failed\n");
            continue;
        }

        /* Close existing client if any */
        if (pyb_client != INVALID_SOCKET) {
            api->log("[py_bridge] Replacing existing client\n");
            closesocket(pyb_client);
        }

        pyb_client = client;
        CreateThread(NULL, 0, pyb_client_thread, NULL, 0, NULL);
    }

    return 0;
}

/* ---- Plugin callbacks ---- */

static void pyb_on_line(const char *line)
{
    if (pyb_client == INVALID_SOCKET) return;

    char escaped[PYB_MAX_LINE];
    json_escape(line, escaped, sizeof(escaped));

    char msg[PYB_MAX_LINE + 64];
    sprintf(msg, "{\"event\":\"line\",\"text\":\"%s\"}", escaped);
    pyb_send(msg);
}

static void pyb_on_round(int round_num)
{
    if (pyb_client == INVALID_SOCKET) return;

    char msg[128];
    sprintf(msg, "{\"event\":\"round\",\"num\":%d}", round_num);
    pyb_send(msg);
}

/* ---- Plugin lifecycle ---- */

static int pyb_init(const slop_api_t *a)
{
    api = a;
    InitializeCriticalSection(&pyb_send_lock);
    pyb_running = 1;

    /* Start server thread */
    CreateThread(NULL, 0, pyb_server_thread, NULL, 0, NULL);

    api->log("[py_bridge] Python bridge plugin initialized\n");
    return 0;
}

static void pyb_shutdown(void)
{
    api->log("[py_bridge] Shutting down...\n");
    pyb_running = 0;

    /* Notify Python */
    if (pyb_client != INVALID_SOCKET) {
        pyb_send("{\"event\":\"shutdown\"}");
        Sleep(50);
        closesocket(pyb_client);
        pyb_client = INVALID_SOCKET;
    }

    if (pyb_server != INVALID_SOCKET) {
        closesocket(pyb_server);
        pyb_server = INVALID_SOCKET;
    }

    DeleteCriticalSection(&pyb_send_lock);
}

/* ---- Menu click handler ---- */

static int pyb_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    if (msg == WM_COMMAND) {
        WORD id = LOWORD(wParam);
        if (id >= 41000 && id < 41100) {
            char info[512];
            sprintf(info,
                "Python Bridge v1.0.0\n\n"
                "TCP Server: 127.0.0.1:%d\n"
                "Status: %s\n\n"
                "Run in a terminal:\n"
                "  python3 plugins/slop_console.py\n\n"
                "Or import slop in any Python script:\n"
                "  import slop\n"
                "  slop.connect()\n"
                "  print(slop.hp())",
                pyb_port,
                (pyb_client != INVALID_SOCKET) ? "Client connected" : "Waiting for connection");
            MessageBoxA(hwnd, info, "Python Bridge", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
    }
    return 0;
}

/* ---- Plugin descriptor ---- */

static slop_plugin_t plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Python Bridge",
    .author      = "MajorSLOP",
    .description = "TCP bridge for 64-bit Python scripts (port 9950+)",
    .version     = "1.0.0",
    .init        = pyb_init,
    .shutdown    = pyb_shutdown,
    .on_line     = pyb_on_line,
    .on_round    = pyb_on_round,
    .on_wndproc  = pyb_on_wndproc,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void)
{
    return &plugin;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p)
{
    (void)h; (void)r; (void)p;
    return TRUE;
}
