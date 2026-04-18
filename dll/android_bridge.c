/*
 * android_bridge.c — MajorSLOP Android Remote Bridge Plugin
 *
 * WebSocket server that lets the MajorSLOP Android app remotely
 * view and control MegaMUD. Loads as a SLOP plugin.
 *
 * Features:
 *   - Token-authenticated WebSocket server
 *   - Real-time terminal line forwarding
 *   - Command injection from remote
 *   - @command (fake_remote) support
 *   - MMUDPy command discovery
 *
 * Build:
 *   i686-w64-mingw32-gcc -shared -o android_bridge.dll android_bridge.c \
 *       -lws2_32 -ladvapi32 -lgdi32 -luser32 -O2 -static-libgcc
 *
 * Install:
 *   Copy android_bridge.dll to MegaMUD/plugins/
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <wincrypt.h>
#include <iphlpapi.h>
#include "slop_plugin.h"
#include "qrcodegen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * Constants
 * ================================================================ */

#define AB_VERSION       "0.1.0"
#define AB_DEFAULT_PORT  9420
#define AB_MAX_CLIENTS   4
#define AB_RECV_BUF      8192
#define AB_OUTQ_SIZE     512
#define AB_OUTQ_MSG_MAX  2048
#define AB_TOKEN_LEN     32
#define AB_MAX_PAIRED    16
#define AB_DEVKEY_LEN    32

/* ================================================================
 * Types
 * ================================================================ */

typedef struct {
    SOCKET sock;
    int    authenticated;
    int    ws_ready;
    unsigned char recv_buf[AB_RECV_BUF];
    int    recv_len;
    char   addr[48];
} ab_client_t;

/* ================================================================
 * State
 * ================================================================ */

static const slop_api_t *g_api        = NULL;
static volatile int       g_running   = 0;
static HANDLE             g_thread    = NULL;
static SOCKET             g_listen    = INVALID_SOCKET;
static int                g_port      = AB_DEFAULT_PORT;
static unsigned char      g_token[AB_TOKEN_LEN];
static char               g_token_hex[AB_TOKEN_LEN * 2 + 1];
static DWORD              g_token_created = 0;
#define AB_TOKEN_EXPIRY_MS  120000
static ab_client_t        g_clients[AB_MAX_CLIENTS];
static CRITICAL_SECTION   g_lock;

typedef struct {
    char key_hex[AB_DEVKEY_LEN * 2 + 1];
    char name[64];
} paired_device_t;

static paired_device_t g_paired[AB_MAX_PAIRED];
static int             g_paired_count = 0;
static char            g_paired_file[MAX_PATH];

/* Outbound message queue (on_clean_line → server thread → clients) */
static char      g_outq[AB_OUTQ_SIZE][AB_OUTQ_MSG_MAX];
static volatile int g_outq_head = 0;
static volatile int g_outq_tail = 0;

/* ================================================================
 * SHA-1 (RFC 3174 — needed for WebSocket handshake)
 * ================================================================ */

#define SHA1_ROL(v,n) (((v)<<(n))|((v)>>(32-(n))))

static void sha1_transform(unsigned int s[5], const unsigned char blk[64])
{
    unsigned int w[80], a, b, c, d, e;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (blk[i*4]<<24) | (blk[i*4+1]<<16) | (blk[i*4+2]<<8) | blk[i*4+3];
    for (i = 16; i < 80; i++) {
        unsigned int t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = SHA1_ROL(t, 1);
    }
    a = s[0]; b = s[1]; c = s[2]; d = s[3]; e = s[4];
    for (i = 0; i < 80; i++) {
        unsigned int f, k, tmp;
        if      (i < 20) { f = (b&c)|((~b)&d);     k = 0x5A827999u; }
        else if (i < 40) { f = b^c^d;               k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b&c)|(b&d)|(c&d);   k = 0x8F1BBCDCu; }
        else              { f = b^c^d;               k = 0xCA62C1D6u; }
        tmp = SHA1_ROL(a,5) + f + e + k + w[i];
        e = d; d = c; c = SHA1_ROL(b,30); b = a; a = tmp;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e;
}

static void sha1(const void *data, int len, unsigned char out[20])
{
    unsigned int s[5] = {0x67452301,0xEFCDAB89,0x98BADCFE,0x10325476,0xC3D2E1F0};
    const unsigned char *p = (const unsigned char *)data;
    unsigned char blk[64];
    int rem = len;
    while (rem >= 64) { sha1_transform(s, p); p += 64; rem -= 64; }
    memset(blk, 0, 64);
    memcpy(blk, p, rem);
    blk[rem] = 0x80;
    if (rem >= 56) { sha1_transform(s, blk); memset(blk, 0, 64); }
    { unsigned long long bits = (unsigned long long)len * 8;
      blk[56]=(bits>>56)&0xFF; blk[57]=(bits>>48)&0xFF;
      blk[58]=(bits>>40)&0xFF; blk[59]=(bits>>32)&0xFF;
      blk[60]=(bits>>24)&0xFF; blk[61]=(bits>>16)&0xFF;
      blk[62]=(bits>>8)&0xFF;  blk[63]=bits&0xFF; }
    sha1_transform(s, blk);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (s[i]>>24)&0xFF; out[i*4+1] = (s[i]>>16)&0xFF;
        out[i*4+2] = (s[i]>>8)&0xFF;  out[i*4+3] = s[i]&0xFF;
    }
}

/* ================================================================
 * Base64 encoder
 * ================================================================ */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const unsigned char *in, int in_len, char *out, int out_sz)
{
    int i, j = 0;
    for (i = 0; i < in_len && j < out_sz - 4; i += 3) {
        unsigned int v = in[i] << 16;
        if (i+1 < in_len) v |= in[i+1] << 8;
        if (i+2 < in_len) v |= in[i+2];
        out[j++] = b64[(v>>18)&0x3F];
        out[j++] = b64[(v>>12)&0x3F];
        out[j++] = (i+1 < in_len) ? b64[(v>>6)&0x3F] : '=';
        out[j++] = (i+2 < in_len) ? b64[v&0x3F] : '=';
    }
    out[j] = 0;
    return j;
}

/* ================================================================
 * Helpers
 * ================================================================ */

static void hex_encode(const unsigned char *in, int len, char *out)
{
    for (int i = 0; i < len; i++)
        _snprintf(out + i*2, 3, "%02x", in[i]);
    out[len*2] = 0;
}

static void gen_random(unsigned char *buf, int len)
{
    HCRYPTPROV prov;
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(prov, len, buf);
        CryptReleaseContext(prov, 0);
    } else {
        srand(GetTickCount() ^ (unsigned)buf);
        for (int i = 0; i < len; i++) buf[i] = rand() & 0xFF;
    }
}

static void get_local_ip(char *buf, int buf_sz)
{
    char lan_ip[48] = "127.0.0.1";
    char ts_ip[48] = {0};

    /* Enumerate all adapters, look for Tailscale (100.x.x.x) */
    ULONG sz = 0;
    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &sz);
    if (sz > 0) {
        IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (addrs && GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &sz) == NO_ERROR) {
            for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
                if (a->OperStatus != IfOperStatusUp) continue;
                for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
                    struct sockaddr_in *sa = (struct sockaddr_in *)u->Address.lpSockaddr;
                    if (sa->sin_family != AF_INET) continue;
                    unsigned long ip = ntohl(sa->sin_addr.s_addr);
                    unsigned char b0 = (ip >> 24) & 0xFF;
                    unsigned char b1 = (ip >> 16) & 0xFF;
                    if (b0 == 127) continue;
                    if (b0 == 100 && b1 >= 64 && b1 <= 127) {
                        /* Tailscale CGNAT range 100.64.0.0/10 */
                        strncpy(ts_ip, inet_ntoa(sa->sin_addr), sizeof(ts_ip) - 1);
                    } else if (!lan_ip[0] || strcmp(lan_ip, "127.0.0.1") == 0) {
                        strncpy(lan_ip, inet_ntoa(sa->sin_addr), sizeof(lan_ip) - 1);
                    }
                }
            }
        }
        free(addrs);
    }

    /* Fallback: UDP socket trick for LAN IP if enumeration missed it */
    if (strcmp(lan_ip, "127.0.0.1") == 0) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s != INVALID_SOCKET) {
            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_port = htons(53);
            dest.sin_addr.s_addr = inet_addr("8.8.8.8");
            connect(s, (struct sockaddr *)&dest, sizeof(dest));
            struct sockaddr_in name;
            int namelen = sizeof(name);
            getsockname(s, (struct sockaddr *)&name, &namelen);
            strncpy(lan_ip, inet_ntoa(name.sin_addr), sizeof(lan_ip) - 1);
            closesocket(s);
        }
    }

    /* Prefer Tailscale IP if available */
    strncpy(buf, ts_ip[0] ? ts_ip : lan_ip, buf_sz - 1);
    buf[buf_sz - 1] = 0;
}

/* ================================================================
 * JSON helpers
 * ================================================================ */

static int json_escape(const char *in, char *out, int out_sz)
{
    int j = 0;
    for (int i = 0; in[i] && j < out_sz - 6; i++) {
        unsigned char c = (unsigned char)in[i];
        if      (c == '"')  { out[j++]='\\'; out[j++]='"'; }
        else if (c == '\\') { out[j++]='\\'; out[j++]='\\'; }
        else if (c == '\n') { out[j++]='\\'; out[j++]='n'; }
        else if (c == '\r') { out[j++]='\\'; out[j++]='r'; }
        else if (c == '\t') { out[j++]='\\'; out[j++]='t'; }
        else if (c < 0x20)  { /* skip control chars */ }
        else                { out[j++] = c; }
    }
    out[j] = 0;
    return j;
}

static const char *json_str(const char *json, const char *key, char *out, int out_sz)
{
    char needle[64];
    _snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(json, needle);
    if (!p) { out[0] = 0; return NULL; }
    p += strlen(needle);
    int j = 0;
    for (; p[0] && p[0] != '"' && j < out_sz - 1; p++) {
        if (p[0] == '\\' && p[1]) {
            p++;
            if      (p[0] == 'n') out[j++] = '\n';
            else if (p[0] == 'r') out[j++] = '\r';
            else if (p[0] == 't') out[j++] = '\t';
            else out[j++] = p[0];
        } else {
            out[j++] = p[0];
        }
    }
    out[j] = 0;
    return out;
}

/* ================================================================
 * Outbound queue (thread-safe ring buffer)
 * ================================================================ */

static void outq_push(const char *msg)
{
    EnterCriticalSection(&g_lock);
    int next = (g_outq_head + 1) % AB_OUTQ_SIZE;
    if (next != g_outq_tail) {
        strncpy(g_outq[g_outq_head], msg, AB_OUTQ_MSG_MAX - 1);
        g_outq[g_outq_head][AB_OUTQ_MSG_MAX - 1] = 0;
        g_outq_head = next;
    }
    LeaveCriticalSection(&g_lock);
}

static int outq_pop(char *out, int out_sz)
{
    EnterCriticalSection(&g_lock);
    if (g_outq_tail == g_outq_head) { LeaveCriticalSection(&g_lock); return 0; }
    strncpy(out, g_outq[g_outq_tail], out_sz - 1);
    out[out_sz - 1] = 0;
    g_outq_tail = (g_outq_tail + 1) % AB_OUTQ_SIZE;
    LeaveCriticalSection(&g_lock);
    return 1;
}

/* ================================================================
 * WebSocket handshake (RFC 6455)
 * ================================================================ */

static const char *ws_magic = "258EAFA5-E914-47DA-95CA-5AB53606BE37";

static int ws_handshake(SOCKET sock, char *buf, int buf_len)
{
    char *key = strstr(buf, "Sec-WebSocket-Key: ");
    if (!key) return -1;
    key += 19;
    char *key_end = strstr(key, "\r\n");
    if (!key_end) return -1;

    int klen = (int)(key_end - key);
    char combined[256];
    if (klen + 36 >= (int)sizeof(combined)) return -1;
    memcpy(combined, key, klen);
    memcpy(combined + klen, ws_magic, 36);

    unsigned char hash[20];
    sha1(combined, klen + 36, hash);

    char accept[32];
    base64_encode(hash, 20, accept, sizeof(accept));

    char resp[512];
    int rlen = _snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);

    send(sock, resp, rlen, 0);
    return 0;
}

/* ================================================================
 * WebSocket frame read/write
 * ================================================================ */

typedef struct {
    int opcode;
    unsigned char *payload;
    int payload_len;
} ws_frame_t;

static int ws_read_frame(ab_client_t *c, ws_frame_t *f)
{
    if (c->recv_len < 2) return 0;
    unsigned char *p = c->recv_buf;
    f->opcode = p[0] & 0x0F;
    int masked = (p[1] >> 7) & 1;
    int plen = p[1] & 0x7F;
    int hlen = 2;

    if (plen == 126) {
        if (c->recv_len < 4) return 0;
        plen = (p[2] << 8) | p[3];
        hlen = 4;
    } else if (plen == 127) {
        if (c->recv_len < 10) return 0;
        plen = (p[6]<<24) | (p[7]<<16) | (p[8]<<8) | p[9];
        hlen = 10;
    }

    if (masked) hlen += 4;
    int total = hlen + plen;
    if (c->recv_len < total) return 0;
    if (plen >= AB_RECV_BUF - 1) { c->recv_len = 0; return 0; }

    unsigned char *mask = masked ? (p + hlen - 4) : NULL;
    f->payload = p + hlen;
    f->payload_len = plen;

    if (mask) {
        for (int i = 0; i < plen; i++)
            f->payload[i] ^= mask[i & 3];
    }

    int remaining = c->recv_len - total;
    if (remaining > 0) memmove(c->recv_buf, c->recv_buf + total, remaining);
    c->recv_len = remaining;
    return 1;
}

static int ws_send(SOCKET sock, int opcode, const char *data, int len)
{
    unsigned char hdr[10];
    int hlen;
    hdr[0] = 0x80 | (opcode & 0x0F);
    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hlen = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (len >> 8) & 0xFF;
        hdr[3] = len & 0xFF;
        hlen = 4;
    } else {
        hdr[1] = 127;
        memset(hdr+2, 0, 4);
        hdr[6] = (len>>24)&0xFF; hdr[7] = (len>>16)&0xFF;
        hdr[8] = (len>>8)&0xFF;  hdr[9] = len&0xFF;
        hlen = 10;
    }
    if (send(sock, (const char *)hdr, hlen, 0) <= 0) return -1;
    if (len > 0 && send(sock, data, len, 0) <= 0) return -1;
    return 0;
}

static int ws_send_text(SOCKET sock, const char *text)
{
    return ws_send(sock, 0x1, text, (int)strlen(text));
}

/* ================================================================
 * Broadcast to all authenticated clients
 * ================================================================ */

static void broadcast(const char *msg)
{
    for (int i = 0; i < AB_MAX_CLIENTS; i++) {
        if (g_clients[i].sock != INVALID_SOCKET &&
            g_clients[i].ws_ready && g_clients[i].authenticated) {
            ws_send_text(g_clients[i].sock, msg);
        }
    }
}

/* Forward declarations for paired device functions (defined after token mgmt) */
static int paired_check(const char *key_hex);
static const char *paired_add(const char *addr);

/* Forward declarations for cross-DLL bridge */
static void send_map_data(SOCKET sock);
static void poll_map_changes(void);
static void send_icon_states(SOCKET sock);
static void poll_icon_changes(void);
static void load_vkt_api(void);
static void (*fn_icon_toggle)(int) = NULL;
static char  icon_last_states[128] = "";
static HMODULE h_msimg = NULL;
static void (WINAPI *fn_rm_queue_increment)(void) = NULL;
static int  (WINAPI *fn_rm_every_step_get)(void) = NULL;

static void load_msimg_api(void)
{
    if (h_msimg) return;
    h_msimg = GetModuleHandleA("msimg32.dll");
    if (!h_msimg) h_msimg = GetModuleHandleA("msimg32");
    if (!h_msimg) return;
    fn_rm_queue_increment = (void *)GetProcAddress(h_msimg, "auto_rm_queue_increment");
    fn_rm_every_step_get  = (void *)GetProcAddress(h_msimg, "rm_every_step_get");
    if (g_api) g_api->log("[android_bridge] msimg32 API: rm_inc=%s rm_get=%s\n",
                          fn_rm_queue_increment ? "OK" : "no",
                          fn_rm_every_step_get ? "OK" : "no");
}

static void get_mud_name(char *out, int out_sz)
{
    out[0] = 0;
    if (!g_api || !g_api->get_mmmain_hwnd) return;
    HWND hw = g_api->get_mmmain_hwnd();
    if (!hw) return;
    char title[256];
    if (GetWindowTextA(hw, title, sizeof(title)) <= 0) return;
    int k = 0;
    for (int i = 0; title[i] && title[i] != ' ' && title[i] != '[' && k < out_sz - 1; i++)
        out[k++] = title[i];
    out[k] = 0;
}

/* ================================================================
 * Protocol: handle incoming client message
 * ================================================================ */

static void handle_message(ab_client_t *client, const char *msg, int len)
{
    char type[32], data[1024];
    json_str(msg, "t", type, sizeof(type));
    json_str(msg, "d", data, sizeof(data));

    if (strcmp(type, "auth") == 0) {
        char mud_name[64];
        get_mud_name(mud_name, sizeof(mud_name));
        DWORD age = GetTickCount() - g_token_created;
        if (strcmp(data, g_token_hex) == 0 && age < AB_TOKEN_EXPIRY_MS) {
            client->authenticated = 1;
            const char *dev_key = paired_add(client->addr);
            char resp[512];
            if (dev_key) {
                _snprintf(resp, sizeof(resp),
                    "{\"t\":\"auth_ok\",\"d\":\"" AB_VERSION "\",\"device_key\":\"%s\",\"name\":\"%s\"}",
                    dev_key, mud_name);
            } else {
                _snprintf(resp, sizeof(resp),
                    "{\"t\":\"auth_ok\",\"d\":\"" AB_VERSION "\",\"name\":\"%s\"}", mud_name);
            }
            ws_send_text(client->sock, resp);
            if (g_api) g_api->log("[android_bridge] Client %s paired\n", client->addr);
        } else if (paired_check(data)) {
            client->authenticated = 1;
            char resp[512];
            _snprintf(resp, sizeof(resp),
                "{\"t\":\"auth_ok\",\"d\":\"" AB_VERSION "\",\"name\":\"%s\"}", mud_name);
            ws_send_text(client->sock, resp);
            if (g_api) g_api->log("[android_bridge] Client %s reconnected (paired key)\n", client->addr);
        } else {
            ws_send_text(client->sock, "{\"t\":\"auth_fail\"}");
            if (g_api) g_api->log("[android_bridge] Auth failed from %s (%s)\n",
                client->addr, age >= AB_TOKEN_EXPIRY_MS ? "token expired" : "bad token");
        }
        return;
    }

    if (!client->authenticated) {
        ws_send_text(client->sock, "{\"t\":\"auth_required\"}");
        return;
    }

    if (strcmp(type, "cmd") == 0) {
        if (g_api && data[0]) g_api->inject_command(data);
    }
    else if (strcmp(type, "text") == 0) {
        if (g_api && data[0]) g_api->inject_text(data);
    }
    else if (strcmp(type, "remote") == 0) {
        if (g_api && data[0]) g_api->fake_remote(data);
    }
    else if (strcmp(type, "walk") == 0) {
        if (g_api && data[0]) {
            g_api->inject_command(data);
            load_msimg_api();
            if (fn_rm_every_step_get && fn_rm_every_step_get()) {
                if (fn_rm_queue_increment) fn_rm_queue_increment();
                g_api->inject_command("rm");
            }
        }
    }
    else if (strcmp(type, "ping") == 0) {
        ws_send_text(client->sock, "{\"t\":\"pong\"}");
    }
    else if (strcmp(type, "map_sync") == 0) {
        send_map_data(client->sock);
        load_msimg_api();
        if (g_api && g_api->inject_command) {
            if (fn_rm_queue_increment) fn_rm_queue_increment();
            g_api->inject_command("rm");
        }
    }
    else if (strcmp(type, "icon_sync") == 0) {
        send_icon_states(client->sock);
    }
    else if (strcmp(type, "icon_toggle") == 0) {
        load_vkt_api();
        if (fn_icon_toggle) {
            int idx = atoi(data);
            fn_icon_toggle(idx);
            icon_last_states[0] = 0;
        }
    }
    else if (strcmp(type, "terminal_sync") == 0) {
        if (!g_api) return;
        char row_buf[256], escaped[512], line_msg[600];
        for (int r = 0; r < g_api->terminal_rows; r++) {
            int n = g_api->read_terminal_row(r, row_buf, sizeof(row_buf));
            if (n <= 0) continue;
            json_escape(row_buf, escaped, sizeof(escaped));
            _snprintf(line_msg, sizeof(line_msg),
                "{\"t\":\"term_row\",\"row\":%d,\"d\":\"%s\"}", r, escaped);
            ws_send_text(client->sock, line_msg);
        }
    }
}

/* ================================================================
 * Client management
 * ================================================================ */

static void client_disconnect(int idx)
{
    if (g_clients[idx].sock != INVALID_SOCKET) {
        closesocket(g_clients[idx].sock);
        if (g_api) g_api->log("[android_bridge] Client %s disconnected\n", g_clients[idx].addr);
        g_clients[idx].sock = INVALID_SOCKET;
        g_clients[idx].authenticated = 0;
        g_clients[idx].ws_ready = 0;
        g_clients[idx].recv_len = 0;
    }
}

static int client_count(void)
{
    int n = 0;
    for (int i = 0; i < AB_MAX_CLIENTS; i++)
        if (g_clients[i].sock != INVALID_SOCKET && g_clients[i].authenticated) n++;
    return n;
}

/* ================================================================
 * Server thread
 * ================================================================ */

static DWORD WINAPI server_thread(LPVOID param)
{
    (void)param;
    if (g_api) g_api->log("[android_bridge] Server thread started on port %d\n", g_port);

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_listen, &rfds);

        for (int i = 0; i < AB_MAX_CLIENTS; i++)
            if (g_clients[i].sock != INVALID_SOCKET)
                FD_SET(g_clients[i].sock, &rfds);

        struct timeval tv = {0, 50000};
        int ready = select(0, &rfds, NULL, NULL, &tv);

        if (ready > 0) {
            /* Accept new connections */
            if (FD_ISSET(g_listen, &rfds)) {
                struct sockaddr_in addr;
                int alen = sizeof(addr);
                SOCKET ns = accept(g_listen, (struct sockaddr *)&addr, &alen);
                if (ns != INVALID_SOCKET) {
                    int slot = -1;
                    for (int i = 0; i < AB_MAX_CLIENTS; i++)
                        if (g_clients[i].sock == INVALID_SOCKET) { slot = i; break; }
                    if (slot >= 0) {
                        memset(&g_clients[slot], 0, sizeof(ab_client_t));
                        g_clients[slot].sock = ns;
                        strncpy(g_clients[slot].addr, inet_ntoa(addr.sin_addr),
                                sizeof(g_clients[slot].addr) - 1);
                        if (g_api) g_api->log("[android_bridge] Connection from %s\n",
                                              g_clients[slot].addr);
                    } else {
                        closesocket(ns);
                        if (g_api) g_api->log("[android_bridge] Max clients reached, rejected\n");
                    }
                }
            }

            /* Read from clients */
            for (int i = 0; i < AB_MAX_CLIENTS; i++) {
                if (g_clients[i].sock == INVALID_SOCKET) continue;
                if (!FD_ISSET(g_clients[i].sock, &rfds)) continue;

                int space = AB_RECV_BUF - g_clients[i].recv_len - 1;
                if (space <= 0) { client_disconnect(i); continue; }

                int n = recv(g_clients[i].sock,
                             (char *)g_clients[i].recv_buf + g_clients[i].recv_len,
                             space, 0);
                if (n <= 0) { client_disconnect(i); continue; }
                g_clients[i].recv_len += n;

                if (!g_clients[i].ws_ready) {
                    g_clients[i].recv_buf[g_clients[i].recv_len] = 0;
                    if (strstr((char *)g_clients[i].recv_buf, "\r\n\r\n")) {
                        if (ws_handshake(g_clients[i].sock,
                                        (char *)g_clients[i].recv_buf,
                                        g_clients[i].recv_len) == 0) {
                            g_clients[i].ws_ready = 1;
                            g_clients[i].recv_len = 0;
                            if (g_api) g_api->log("[android_bridge] WebSocket ready for %s\n",
                                                  g_clients[i].addr);
                        } else {
                            client_disconnect(i);
                        }
                    }
                } else {
                    ws_frame_t frame;
                    while (ws_read_frame(&g_clients[i], &frame)) {
                        if (frame.opcode == 0x8) {
                            client_disconnect(i);
                            break;
                        } else if (frame.opcode == 0x9) {
                            ws_send(g_clients[i].sock, 0xA,
                                    (char *)frame.payload, frame.payload_len);
                        } else if (frame.opcode == 0x1) {
                            frame.payload[frame.payload_len] = 0;
                            handle_message(&g_clients[i],
                                          (char *)frame.payload, frame.payload_len);
                        }
                    }
                }
            }
        }

        /* Poll map walker for room changes */
        poll_map_changes();
        poll_icon_changes();

        /* Drain outbound queue → broadcast */
        char msg[AB_OUTQ_MSG_MAX];
        while (outq_pop(msg, sizeof(msg)))
            broadcast(msg);
    }

    /* Cleanup */
    for (int i = 0; i < AB_MAX_CLIENTS; i++)
        client_disconnect(i);

    if (g_api) g_api->log("[android_bridge] Server thread stopped\n");
    return 0;
}

/* ================================================================
 * Connection info display
 * ================================================================ */

static void show_connection_info(void)
{
    if (!g_api) return;
    char ip[48];
    get_local_ip(ip, sizeof(ip));

    char buf[512];
    _snprintf(buf, sizeof(buf),
        "\r\n"
        "\x1b[1;36m========= Android Bridge =========\x1b[0m\r\n"
        "\x1b[1;37m Address: \x1b[1;33m%s:%d\x1b[0m\r\n"
        "\x1b[1;37m Token:   \x1b[1;32m%.16s\x1b[0m\r\n"
        "\x1b[1;37m          \x1b[1;32m%.16s\x1b[0m\r\n"
        "\x1b[1;37m Clients: \x1b[1;33m%d/%d\x1b[0m\r\n"
        "\x1b[1;36m==================================\x1b[0m\r\n",
        ip, g_port,
        g_token_hex, g_token_hex + 16,
        client_count(), AB_MAX_CLIENTS);

    g_api->inject_server_data(buf, (int)strlen(buf));
    g_api->log("[android_bridge] Connection info: %s:%d token=%s\n",
               ip, g_port, g_token_hex);
}

/* ================================================================
 * Token management
 * ================================================================ */

static void generate_token(void)
{
    gen_random(g_token, AB_TOKEN_LEN);
    hex_encode(g_token, AB_TOKEN_LEN, g_token_hex);
    g_token_created = GetTickCount();
}

/* ================================================================
 * Paired device persistence
 * ================================================================ */

static void paired_save(void)
{
    if (!g_paired_file[0]) return;
    FILE *f = fopen(g_paired_file, "w");
    if (!f) return;
    for (int i = 0; i < g_paired_count; i++)
        fprintf(f, "%s %s\n", g_paired[i].key_hex, g_paired[i].name);
    fclose(f);
}

static void paired_load(void)
{
    if (!g_paired_file[0]) return;
    g_paired_count = 0;
    FILE *f = fopen(g_paired_file, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_paired_count < AB_MAX_PAIRED) {
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        char *name = sp + 1;
        char *nl = strchr(name, '\n');
        if (nl) *nl = 0;
        strncpy(g_paired[g_paired_count].key_hex, line,
                sizeof(g_paired[0].key_hex) - 1);
        strncpy(g_paired[g_paired_count].name, name,
                sizeof(g_paired[0].name) - 1);
        g_paired_count++;
    }
    fclose(f);
}

static int paired_check(const char *key_hex)
{
    for (int i = 0; i < g_paired_count; i++)
        if (strcmp(g_paired[i].key_hex, key_hex) == 0) return 1;
    return 0;
}

static const char *paired_add(const char *addr)
{
    if (g_paired_count >= AB_MAX_PAIRED) return NULL;
    unsigned char key[AB_DEVKEY_LEN];
    gen_random(key, AB_DEVKEY_LEN);
    hex_encode(key, AB_DEVKEY_LEN, g_paired[g_paired_count].key_hex);
    strncpy(g_paired[g_paired_count].name, addr,
            sizeof(g_paired[0].name) - 1);
    const char *hex = g_paired[g_paired_count].key_hex;
    g_paired_count++;
    paired_save();
    return hex;
}

/* ================================================================
 * Map Walker cross-DLL bridge (vk_terminal.dll exports)
 * ================================================================ */

static HMODULE   h_vkt = NULL;
static int      (*fn_mdw_loaded)(void) = NULL;
static int      (*fn_mdw_cur_map)(void) = NULL;
static int      (*fn_mdw_cur_room_map)(void) = NULL;
static int      (*fn_mdw_cur_room_num)(void) = NULL;
static const char *(*fn_mdw_serialize)(void) = NULL;
static const char *(*fn_icon_states)(void) = NULL;

static int  mdw_last_map  = -1;
static int  mdw_last_rmap = -1;
static int  mdw_last_rnum = -1;

static void load_vkt_api(void)
{
    if (h_vkt) return;
    h_vkt = GetModuleHandleA("vk_terminal.dll");
    if (!h_vkt) return;
    fn_mdw_loaded       = (void *)GetProcAddress(h_vkt, "vkt_mdw_loaded");
    fn_mdw_cur_map      = (void *)GetProcAddress(h_vkt, "vkt_mdw_cur_map");
    fn_mdw_cur_room_map = (void *)GetProcAddress(h_vkt, "vkt_mdw_cur_room_map");
    fn_mdw_cur_room_num = (void *)GetProcAddress(h_vkt, "vkt_mdw_cur_room_num");
    fn_mdw_serialize    = (void *)GetProcAddress(h_vkt, "vkt_mdw_serialize_map");
    fn_icon_states      = (void *)GetProcAddress(h_vkt, "vkt_icon_states");
    fn_icon_toggle      = (void *)GetProcAddress(h_vkt, "vkt_icon_toggle");
    if (g_api) g_api->log("[android_bridge] vk_terminal API: map=%s icons=%s\n",
                          fn_mdw_loaded ? "OK" : "no",
                          fn_icon_states ? "OK" : "no");
}

static void send_map_data(SOCKET sock)
{
    load_vkt_api();
    if (!fn_mdw_loaded || !fn_mdw_loaded() || !fn_mdw_serialize) {
        ws_send_text(sock, "{\"t\":\"map_data\",\"d\":null}");
        return;
    }
    const char *json = fn_mdw_serialize();
    if (!json) {
        ws_send_text(sock, "{\"t\":\"map_data\",\"d\":null}");
        return;
    }
    int jlen = (int)strlen(json);
    int msglen = jlen + 32;
    char *msg = (char *)malloc(msglen);
    if (!msg) return;
    _snprintf(msg, msglen, "{\"t\":\"map_data\",\"d\":%s}", json);
    ws_send_text(sock, msg);
    free(msg);
    if (g_api) g_api->log("[android_bridge] Sent map_data (%d bytes)\n", jlen);
}

static void poll_map_changes(void)
{
    if (!fn_mdw_loaded || !fn_mdw_loaded()) return;
    if (!fn_mdw_cur_map || !fn_mdw_cur_room_map || !fn_mdw_cur_room_num) return;
    if (client_count() == 0) return;

    int cur_map  = fn_mdw_cur_map();
    int cur_rmap = fn_mdw_cur_room_map();
    int cur_rnum = fn_mdw_cur_room_num();

    if (cur_rmap == mdw_last_rmap && cur_rnum == mdw_last_rnum) return;

    int map_changed = (cur_map != mdw_last_map);
    mdw_last_map  = cur_map;
    mdw_last_rmap = cur_rmap;
    mdw_last_rnum = cur_rnum;

    if (map_changed && fn_mdw_serialize) {
        const char *json = fn_mdw_serialize();
        if (json) {
            int jlen = (int)strlen(json);
            int msglen = jlen + 32;
            char *msg = (char *)malloc(msglen);
            if (msg) {
                _snprintf(msg, msglen, "{\"t\":\"map_data\",\"d\":%s}", json);
                broadcast(msg);
                free(msg);
            }
        }
    }

    char pos_msg[128];
    _snprintf(pos_msg, sizeof(pos_msg),
              "{\"t\":\"map_pos\",\"m\":%d,\"r\":%d}", cur_rmap, cur_rnum);
    outq_push(pos_msg);
}

static void send_icon_states(SOCKET sock)
{
    load_vkt_api();
    if (!fn_icon_states) {
        ws_send_text(sock, "{\"t\":\"icon_states\",\"d\":null}");
        return;
    }
    const char *json = fn_icon_states();
    if (!json) return;
    char msg[512];
    _snprintf(msg, sizeof(msg), "{\"t\":\"icon_states\",\"d\":%s}", json);
    ws_send_text(sock, msg);
}

static void poll_icon_changes(void)
{
    if (!fn_icon_states) return;
    if (client_count() == 0) return;
    const char *cur = fn_icon_states();
    if (!cur) return;
    if (strcmp(cur, icon_last_states) == 0) return;
    strncpy(icon_last_states, cur, sizeof(icon_last_states) - 1);
    char msg[512];
    _snprintf(msg, sizeof(msg), "{\"t\":\"icon_states\",\"d\":%s}", cur);
    outq_push(msg);
}

/* ================================================================
 * SLOP Plugin callbacks
 * ================================================================ */

static int ab_init(const slop_api_t *api)
{
    g_api = api;
    InitializeCriticalSection(&g_lock);

    for (int i = 0; i < AB_MAX_CLIENTS; i++)
        g_clients[i].sock = INVALID_SOCKET;

    _snprintf(g_paired_file, sizeof(g_paired_file),
              "plugins\\android_paired.dat");
    paired_load();
    api->log("[android_bridge] Loaded %d paired device(s)\n", g_paired_count);

    generate_token();

    /* Init Winsock */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        api->log("[android_bridge] WSAStartup failed\n");
        return -1;
    }

    /* Create listen socket */
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) {
        api->log("[android_bridge] socket() failed\n");
        WSACleanup();
        return -1;
    }

    int reuse = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(g_port);

    if (bind(g_listen, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
        api->log("[android_bridge] bind() failed on port %d\n", g_port);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }

    if (listen(g_listen, 4) == SOCKET_ERROR) {
        api->log("[android_bridge] listen() failed\n");
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }

    /* Start server thread */
    g_running = 1;
    g_thread = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (!g_thread) {
        api->log("[android_bridge] CreateThread failed\n");
        g_running = 0;
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        WSACleanup();
        return -1;
    }

    /* Load map walker API from vk_terminal.dll */
    load_vkt_api();

    /* Menu items */
    api->add_menu_item("Show Connection Info", api->menu_base_id + 0);
    api->add_menu_separator();
    api->add_menu_item("Disconnect All Clients", api->menu_base_id + 1);
    api->add_menu_item("Regenerate Token", api->menu_base_id + 2);

    char ip[48];
    get_local_ip(ip, sizeof(ip));
    api->log("[android_bridge] v%s started — %s:%d\n", AB_VERSION, ip, g_port);
    api->log("[android_bridge] Token: %s\n", g_token_hex);

    return 0;
}

static void ab_shutdown(void)
{
    g_running = 0;

    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }

    if (g_thread) {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    WSACleanup();
    DeleteCriticalSection(&g_lock);

    if (g_api) g_api->log("[android_bridge] Shutdown complete\n");
}

static int ab_on_data_count = 0;

static void ab_on_data(const char *data, int len)
{
    if (!data || len <= 0) return;
    if (client_count() == 0) return;

    if (++ab_on_data_count <= 3 && g_api)
        g_api->log("[android_bridge] on_data[%d]: len=%d\n", ab_on_data_count, len);

    char b64[4096], msg[AB_OUTQ_MSG_MAX];
    int b64_len = base64_encode((const unsigned char *)data, len, b64, sizeof(b64));
    if (b64_len <= 0) return;
    _snprintf(msg, sizeof(msg), "{\"t\":\"ansi\",\"d\":\"%s\"}", b64);
    outq_push(msg);
}

static void ab_on_clean_line(const char *line)
{
    if (!line || !line[0]) return;
    if (client_count() == 0) return;

    char escaped[1024], msg[AB_OUTQ_MSG_MAX];
    json_escape(line, escaped, sizeof(escaped));
    _snprintf(msg, sizeof(msg), "{\"t\":\"line\",\"d\":\"%s\"}", escaped);
    outq_push(msg);
}

static void ab_on_round(int round_num)
{
    if (client_count() == 0) return;
    char msg[128];
    _snprintf(msg, sizeof(msg), "{\"t\":\"round\",\"d\":%d}", round_num);
    outq_push(msg);
}

static int ab_on_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COMMAND && g_api) {
        int id = LOWORD(wParam);
        if (id == g_api->menu_base_id + 0) {
            show_connection_info();
            return 1;
        }
        if (id == g_api->menu_base_id + 1) {
            for (int i = 0; i < AB_MAX_CLIENTS; i++) client_disconnect(i);
            g_api->log("[android_bridge] All clients disconnected\n");
            return 1;
        }
        if (id == g_api->menu_base_id + 2) {
            for (int i = 0; i < AB_MAX_CLIENTS; i++) client_disconnect(i);
            generate_token();
            g_api->log("[android_bridge] New token: %s\n", g_token_hex);
            show_connection_info();
            return 1;
        }
    }
    return 0;
}

/* ================================================================
 * Plugin descriptor
 * ================================================================ */

static slop_plugin_t g_plugin = {
    .magic       = SLOP_PLUGIN_MAGIC,
    .api_version = SLOP_API_VERSION,
    .name        = "Android Bridge",
    .author      = "MajorSLOP",
    .description = "Remote control via Android app over WebSocket",
    .version     = AB_VERSION,
    .init        = ab_init,
    .shutdown    = ab_shutdown,
    .on_line     = NULL,
    .on_round    = ab_on_round,
    .on_wndproc  = ab_on_wndproc,
    .on_data     = ab_on_data,
    .on_clean_line = ab_on_clean_line,
};

SLOP_EXPORT slop_plugin_t *slop_get_plugin(void) { return &g_plugin; }

/* ================================================================
 * MMUDPy command exports
 * ================================================================ */

__declspec(dllexport) const char *ab_get_status(void)
{
    static char buf[256];
    char ip[48];
    get_local_ip(ip, sizeof(ip));
    _snprintf(buf, sizeof(buf), "%s:%d | %d client(s) connected",
              ip, g_port, client_count());
    return buf;
}

__declspec(dllexport) void ab_show_info(void)     { show_connection_info(); }
__declspec(dllexport) int  ab_client_count(void)  { return client_count(); }
__declspec(dllexport) void ab_disconnect_all(void) {
    for (int i = 0; i < AB_MAX_CLIENTS; i++) client_disconnect(i);
}
__declspec(dllexport) void ab_regen_token(void)    {
    for (int i = 0; i < AB_MAX_CLIENTS; i++) client_disconnect(i);
    generate_token();
    show_connection_info();
}
__declspec(dllexport) void ab_set_port(int p)      {
    if (p > 0 && p < 65536) {
        g_port = p;
        if (g_api) g_api->log("[android_bridge] Port set to %d (restart plugin to apply)\n", p);
    }
}
__declspec(dllexport) void ab_send_to_clients(const char *msg) {
    if (msg && msg[0]) outq_push(msg);
}

static slop_command_t g_commands[] = {
    {"ab.status",     "ab_get_status",      "",  "s", "Get bridge status (address, clients)"},
    {"ab.show_info",  "ab_show_info",       "",  "v", "Show connection info in terminal"},
    {"ab.disconnect", "ab_disconnect_all",  "",  "v", "Disconnect all clients"},
    {"ab.new_token",  "ab_regen_token",     "",  "v", "Generate new pairing token"},
    {"ab.port",       "ab_set_port",        "i", "v", "Set WebSocket port (restart to apply)"},
    {"ab.send",       "ab_send_to_clients", "s", "v", "Send raw JSON message to all clients"},
    {"ab.conn_str",   "ab_get_connection_string", "", "s", "Get connection string (slop://...)"},
    {"ab.qr_gen",     "ab_generate_qr",    "",  "i", "Generate QR code, returns side length"},
    {"ab.qr_size",    "ab_qr_get_size",    "",  "i", "Get current QR code side length"},
};

SLOP_EXPORT slop_command_t *slop_get_commands(int *count)
{
    *count = sizeof(g_commands) / sizeof(g_commands[0]);
    return g_commands;
}

/* ================================================================
 * QR Code generation
 * ================================================================ */

static uint8_t g_qr_code[qrcodegen_BUFFER_LEN_MAX];
static uint8_t g_qr_temp[qrcodegen_BUFFER_LEN_MAX];
static char    g_qr_conn_str[256];
static int     g_qr_size = 0;  /* side length of current QR code, 0 = none */

__declspec(dllexport) const char *ab_get_connection_string(void)
{
    char ip[48];
    get_local_ip(ip, sizeof(ip));
    _snprintf(g_qr_conn_str, sizeof(g_qr_conn_str),
              "slop://%s:%d/%s", ip, g_port, g_token_hex);
    return g_qr_conn_str;
}

__declspec(dllexport) int ab_generate_qr(void)
{
    const char *str = ab_get_connection_string();
    bool ok = qrcodegen_encodeText(str, g_qr_temp, g_qr_code,
                                   qrcodegen_Ecc_MEDIUM, 1, 10,
                                   qrcodegen_Mask_AUTO, true);
    if (ok) {
        g_qr_size = qrcodegen_getSize(g_qr_code);
        if (g_api) g_api->log("[android_bridge] QR generated (%dx%d) for %s\n",
                              g_qr_size, g_qr_size, str);
    } else {
        g_qr_size = 0;
        if (g_api) g_api->log("[android_bridge] QR generation failed for %s\n", str);
    }
    return g_qr_size;
}

__declspec(dllexport) int ab_qr_get_size(void)
{
    return g_qr_size;
}

__declspec(dllexport) int ab_qr_get_module(int x, int y)
{
    if (g_qr_size <= 0) return 0;
    return qrcodegen_getModule(g_qr_code, x, y) ? 1 : 0;
}

/* ================================================================ */

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)h; (void)reason; (void)reserved;
    return TRUE;
}
