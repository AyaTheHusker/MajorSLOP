/*
 * MSIMG32.dll proxy — drops into MegaMUD folder, forwards AlphaBlend to the
 * real system DLL, and runs a TCP control server on DLL_PROCESS_ATTACH.
 *
 * Build (32-bit):
 *   i686-w64-mingw32-gcc -shared -o MSIMG32.dll msimg32_proxy.c msimg32.def -lgdi32 -lws2_32
 *
 * Install:
 *   cp MSIMG32.dll /path/to/MegaMUD/
 *
 * Protocol (line-based, \n terminated):
 *   PING                            → PONG
 *   BASE                            → struct base address as hex
 *   FIND <name>                     → scan heap for game state struct, lock base
 *   READ <hex_offset> <size>        → returns hex bytes (relative to struct base)
 *   WRITE <hex_offset> <hex_bytes>  → writes bytes, returns OK
 *   READ32 <hex_offset>             → returns u32 as decimal
 *   WRITE32 <hex_offset> <decimal>  → writes u32, returns OK
 *   READS <hex_offset> <maxlen>     → reads null-terminated string
 *   WRITES <hex_offset> <string>    → writes string + null terminator
 *   READABS <hex_addr> <size>       → read raw bytes at absolute address
 *   STATUS                          → read all 8 statusbar parts (tab-separated)
 *   DLGTEXT <class> <ctrl_id>       → read text from dialog control
 *   CLICK <class> <ctrl_id>         → send BM_CLICK to dialog button
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

#include <commctrl.h>

/* Status bar messages — define if missing */
#ifndef SB_GETTEXTA
#define SB_GETTEXTA 0x0402
#endif
#ifndef SB_GETPARTS
#define SB_GETPARTS 0x0406
#endif

#define PLUGIN_PORT 9901
#define CMD_BUF 4096
#define MEGAMUD_STATUSBAR_ID 107

/* Known struct offsets for signature matching */
#define OFF_PLAYER_NAME   0x537C
#define OFF_PLAYER_CUR_HP 0x53D4
#define OFF_PLAYER_MAX_HP 0x53DC
#define OFF_PLAYER_LEVEL  0x53D0
#define OFF_CONNECTED     0x563C
#define STRUCT_MIN_SIZE   0x9700

/* ---- Forward AlphaBlend to real msimg32.dll ---- */

typedef BOOL (WINAPI *AlphaBlend_t)(
    HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
    HDC hdcSrc,  int xSrc,  int ySrc,  int wSrc,  int hSrc,
    BLENDFUNCTION blendFunc
);

static AlphaBlend_t real_AlphaBlend = NULL;
static HMODULE real_msimg32 = NULL;

BOOL WINAPI proxy_AlphaBlend(
    HDC hdcDest, int xDest, int yDest, int wDest, int hDest,
    HDC hdcSrc,  int xSrc,  int ySrc,  int wSrc,  int hSrc,
    BLENDFUNCTION blendFunc)
{
    if (real_AlphaBlend)
        return real_AlphaBlend(hdcDest, xDest, yDest, wDest, hDest,
                               hdcSrc, xSrc, ySrc, wSrc, hSrc, blendFunc);
    return FALSE;
}

/* ---- Logging ---- */

static FILE *logfile = NULL;

static void logmsg(const char *fmt, ...)
{
    if (!logfile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logfile, fmt, ap);
    va_end(ap);
    fflush(logfile);
}

/* ---- Hex helpers ---- */

static int hex2byte(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex_bytes(const char *hex, unsigned char *out, int max)
{
    int len = 0;
    while (*hex && *(hex+1) && len < max) {
        int hi = hex2byte(*hex);
        int lo = hex2byte(*(hex+1));
        if (hi < 0 || lo < 0) break;
        out[len++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return len;
}

/* ---- Game state struct finder ---- */

static DWORD struct_base = 0;

/*
 * Scan process memory for the game state struct using VirtualQuery.
 * Signature: player name at +0x5360, reasonable HP/level values.
 */
static DWORD find_struct(const char *player_name)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD addr = 0;
    int namelen = (int)strlen(player_name);

    int regions_scanned = 0;
    int regions_total = 0;
    int names_found = 0;
    while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        regions_total++;
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)))
        {
            DWORD size_check = (DWORD)mbi.RegionSize;
            if (size_check < 0x100 || size_check >= 200000000) {
                addr = (DWORD)mbi.BaseAddress + mbi.RegionSize;
                if (addr < (DWORD)mbi.BaseAddress) break;
                continue;
            }
            regions_scanned++;
            unsigned char *region = (unsigned char *)mbi.BaseAddress;
            DWORD size = (DWORD)mbi.RegionSize;

            /* Scan for player name */
            for (DWORD off = 0; off + namelen < size; off += 4) {
                unsigned char *candidate = region + off + OFF_PLAYER_NAME;

                /* Check name match */
                if (memcmp(candidate, player_name, namelen) != 0)
                    continue;
                if (candidate[namelen] != '\0')
                    continue;

                names_found++;

                /* Log every candidate with surrounding values */
                DWORD cand_addr = (DWORD)(region + off);
                int hp = *(int *)(region + off + OFF_PLAYER_CUR_HP);
                int max_hp = *(int *)(region + off + OFF_PLAYER_MAX_HP);
                int level = *(int *)(region + off + OFF_PLAYER_LEVEL);
                logmsg("[mudplugin] Candidate #%d at 0x%08X: hp=%d max_hp=%d level=%d\n",
                       names_found, cand_addr, hp, max_hp, level);

                /* Also try shifted offsets: maybe stats are at -0x1C from expected */
                int hp2 = *(int *)(region + off + 0x53B8);
                int maxhp2 = *(int *)(region + off + 0x53BC);
                int level2 = *(int *)(region + off + 0x53B4);
                logmsg("[mudplugin]   Alt offsets: hp=%d max_hp=%d level=%d\n",
                       hp2, maxhp2, level2);

                if (hp < 1 || hp > 100000 || max_hp < 1 || max_hp > 100000)
                {
                    /* Try alt offsets */
                    if (hp2 >= 1 && hp2 <= 100000 && maxhp2 >= 1 && maxhp2 <= 100000 &&
                        level2 >= 1 && level2 <= 1000) {
                        logmsg("[mudplugin] Alt offsets matched! Using this candidate.\n");
                        return cand_addr;
                    }
                    continue;
                }
                if (max_hp < hp)
                    continue;

                if (level < 1 || level > 1000)
                    continue;

                /* Found it */
                DWORD base = (DWORD)(region + off);
                logmsg("[mudplugin] Found struct at 0x%08X (name=%s hp=%d/%d lvl=%d)\n",
                       base, player_name, hp, max_hp, level);
                return base;
            }
        }

        /* Move to next region */
        addr = (DWORD)mbi.BaseAddress + mbi.RegionSize;
        if (addr < (DWORD)mbi.BaseAddress)
            break; /* overflow */
    }

    logmsg("[mudplugin] FIND scanned %d/%d regions, found name %d times, no valid struct\n",
           regions_scanned, regions_total, names_found);
    return 0;
}

/* ---- Client handler ---- */

static void handle_client(SOCKET client)
{
    char buf[CMD_BUF];
    int bufpos = 0;

    logmsg("[mudplugin] Client connected, struct_base=0x%08X\n", struct_base);

    /* Send banner with current state */
    char banner[128];
    sprintf(banner, "MUDPLUGIN v2.0 BASE=0x%08X\n", struct_base);
    send(client, banner, (int)strlen(banner), 0);

    while (1) {
        int n = recv(client, buf + bufpos, CMD_BUF - bufpos - 1, 0);
        if (n <= 0) break;
        bufpos += n;
        buf[bufpos] = '\0';

        char *line_start = buf;
        char *newline;
        while ((newline = strchr(line_start, '\n')) != NULL) {
            *newline = '\0';
            if (newline > line_start && *(newline-1) == '\r')
                *(newline-1) = '\0';

            char resp[CMD_BUF];
            resp[0] = '\0';

            if (strncmp(line_start, "PING", 4) == 0) {
                strcpy(resp, "PONG\n");

            } else if (strncmp(line_start, "FIND ", 5) == 0) {
                char *name = line_start + 5;
                while (*name == ' ') name++;
                struct_base = find_struct(name);
                if (struct_base)
                    sprintf(resp, "OK 0x%08X\n", struct_base);
                else
                    strcpy(resp, "ERR struct not found\n");

            } else if (strncmp(line_start, "SETBASE ", 8) == 0) {
                struct_base = (DWORD)strtoul(line_start + 8, NULL, 16);
                logmsg("[mudplugin] Base set to 0x%08X\n", struct_base);
                sprintf(resp, "OK 0x%08X\n", struct_base);

            } else if (strncmp(line_start, "BASE", 4) == 0) {
                sprintf(resp, "0x%08X\n", struct_base);

            } else if (strncmp(line_start, "READABS ", 8) == 0) {
                char *p = line_start + 8;
                DWORD addr = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                int size = atoi(p);
                if (size <= 0) size = 4;
                if (size > (CMD_BUF / 2) - 2) size = (CMD_BUF / 2) - 2;
                unsigned char *src = (unsigned char *)addr;
                char *rp = resp;
                for (int i = 0; i < size; i++) {
                    sprintf(rp, "%02X", src[i]);
                    rp += 2;
                }
                *rp++ = '\n';
                *rp = '\0';

            } else if (struct_base == 0) {
                strcpy(resp, "ERR no base - send FIND <playername> first\n");

            } else if (strncmp(line_start, "READ32 ", 7) == 0) {
                DWORD offset = (DWORD)strtoul(line_start + 7, NULL, 16);
                DWORD *ptr = (DWORD *)(struct_base + offset);
                sprintf(resp, "%u\n", *ptr);

            } else if (strncmp(line_start, "WRITE32 ", 8) == 0) {
                char *p = line_start + 8;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                DWORD val = (DWORD)strtoul(p, NULL, 10);
                DWORD *ptr = (DWORD *)(struct_base + offset);
                *ptr = val;
                strcpy(resp, "OK\n");

            } else if (strncmp(line_start, "READS ", 6) == 0) {
                char *p = line_start + 6;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                int maxlen = atoi(p);
                if (maxlen <= 0) maxlen = 256;
                if (maxlen > CMD_BUF - 2) maxlen = CMD_BUF - 2;
                char *src = (char *)(struct_base + offset);
                int slen = (int)strnlen(src, maxlen);
                memcpy(resp, src, slen);
                resp[slen] = '\n';
                resp[slen+1] = '\0';

            } else if (strncmp(line_start, "WRITES ", 7) == 0) {
                char *p = line_start + 7;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                char *dst = (char *)(struct_base + offset);
                strcpy(dst, p);
                strcpy(resp, "OK\n");

            } else if (strncmp(line_start, "READ ", 5) == 0) {
                char *p = line_start + 5;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                int size = atoi(p);
                if (size <= 0) size = 4;
                if (size > (CMD_BUF / 2) - 2) size = (CMD_BUF / 2) - 2;
                unsigned char *src = (unsigned char *)(struct_base + offset);
                char *rp = resp;
                for (int i = 0; i < size; i++) {
                    sprintf(rp, "%02X", src[i]);
                    rp += 2;
                }
                *rp++ = '\n';
                *rp = '\0';

            } else if (strncmp(line_start, "WRITE ", 6) == 0) {
                char *p = line_start + 6;
                DWORD offset = (DWORD)strtoul(p, &p, 16);
                while (*p == ' ') p++;
                unsigned char bytes[CMD_BUF/2];
                int nbytes = parse_hex_bytes(p, bytes, sizeof(bytes));
                unsigned char *dst = (unsigned char *)(struct_base + offset);
                memcpy(dst, bytes, nbytes);
                sprintf(resp, "OK %d\n", nbytes);

            } else if (strncmp(line_start, "STATUS", 6) == 0) {
                /* Read MegaMUD's status bar (msctls_statusbar32, ctrl_id=107).
                 * Returns all 8 parts tab-separated on one line. */
                HWND main_wnd = FindWindowA("MMMAIN", NULL);
                if (!main_wnd) {
                    strcpy(resp, "ERR no MMMAIN window\n");
                } else {
                    HWND sb = GetDlgItem(main_wnd, MEGAMUD_STATUSBAR_ID);
                    if (!sb) {
                        strcpy(resp, "ERR no statusbar control\n");
                    } else {
                        char *rp = resp;
                        int parts = (int)SendMessageA(sb, SB_GETPARTS, 0, 0);
                        if (parts <= 0) parts = 8;
                        if (parts > 8) parts = 8;
                        for (int i = 0; i < parts; i++) {
                            char part_buf[256] = {0};
                            SendMessageA(sb, SB_GETTEXTA, (WPARAM)i, (LPARAM)part_buf);
                            part_buf[255] = '\0';
                            if (i > 0) *rp++ = '\t';
                            int plen = (int)strlen(part_buf);
                            memcpy(rp, part_buf, plen);
                            rp += plen;
                        }
                        *rp++ = '\n';
                        *rp = '\0';
                    }
                }

            } else if (strncmp(line_start, "DLGTEXT ", 8) == 0) {
                /* Read text from a control in a dialog/window.
                 * DLGTEXT <parent_class_or_title> <ctrl_id>
                 * Finds the parent window, gets child by ctrl_id, reads text.
                 * Used for Player Statistics dialog controls. */
                char *p = line_start + 8;
                /* Parse parent: could be class name like "#32770" or window title */
                char parent_class[128] = {0};
                int pi = 0;
                while (*p && *p != ' ' && pi < 126) parent_class[pi++] = *p++;
                parent_class[pi] = '\0';
                while (*p == ' ') p++;
                int ctrl_id = atoi(p);

                /* Find the parent window */
                HWND parent = NULL;
                if (parent_class[0] == '#') {
                    /* Class name like #32770 — enumerate to find MegaMUD's dialog */
                    HWND candidate = NULL;
                    DWORD our_pid = GetCurrentProcessId();
                    while ((candidate = FindWindowExA(NULL, candidate, parent_class, NULL)) != NULL) {
                        DWORD win_pid = 0;
                        GetWindowThreadProcessId(candidate, &win_pid);
                        if (win_pid == our_pid) { parent = candidate; break; }
                    }
                } else {
                    parent = FindWindowA(parent_class, NULL);
                }

                if (!parent) {
                    strcpy(resp, "ERR parent window not found\n");
                } else {
                    HWND ctrl = GetDlgItem(parent, ctrl_id);
                    if (!ctrl) {
                        sprintf(resp, "ERR control %d not found\n", ctrl_id);
                    } else {
                        char text_buf[512] = {0};
                        GetWindowTextA(ctrl, text_buf, 511);
                        sprintf(resp, "%s\n", text_buf);
                    }
                }

            } else if (strncmp(line_start, "CLICK ", 6) == 0) {
                /* Send BM_CLICK to a button in a dialog.
                 * CLICK <parent_class> <ctrl_id>
                 * Used for resetting exp meter (ctrl 1721). */
                char *p = line_start + 6;
                char parent_class[128] = {0};
                int pi = 0;
                while (*p && *p != ' ' && pi < 126) parent_class[pi++] = *p++;
                parent_class[pi] = '\0';
                while (*p == ' ') p++;
                int ctrl_id = atoi(p);

                HWND parent = NULL;
                if (parent_class[0] == '#') {
                    HWND candidate = NULL;
                    DWORD our_pid = GetCurrentProcessId();
                    while ((candidate = FindWindowExA(NULL, candidate, parent_class, NULL)) != NULL) {
                        DWORD win_pid = 0;
                        GetWindowThreadProcessId(candidate, &win_pid);
                        if (win_pid == our_pid) { parent = candidate; break; }
                    }
                } else {
                    parent = FindWindowA(parent_class, NULL);
                }

                if (!parent) {
                    strcpy(resp, "ERR parent window not found\n");
                } else {
                    HWND ctrl = GetDlgItem(parent, ctrl_id);
                    if (!ctrl) {
                        sprintf(resp, "ERR control %d not found\n", ctrl_id);
                    } else {
                        SendMessageA(ctrl, BM_CLICK, 0, 0);
                        strcpy(resp, "OK\n");
                    }
                }

            } else {
                strcpy(resp, "ERR unknown command\n");
            }

            if (resp[0])
                send(client, resp, (int)strlen(resp), 0);

            line_start = newline + 1;
        }

        if (line_start > buf) {
            int remaining = bufpos - (int)(line_start - buf);
            if (remaining > 0)
                memmove(buf, line_start, remaining);
            bufpos = remaining;
        }
    }

    logmsg("[mudplugin] Client disconnected\n");
    closesocket(client);
}

/* ---- TCP Server ---- */

static DWORD WINAPI payload_main(LPVOID param)
{
    Sleep(2000);

    logfile = fopen("mudplugin.log", "a");
    logmsg("[mudplugin] DLL loaded into PID %lu\n", GetCurrentProcessId());
    logmsg("[mudplugin] Module base: 0x%08X\n", (DWORD)GetModuleHandle(NULL));

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logmsg("[mudplugin] WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCKET) {
        logmsg("[mudplugin] socket() failed: %d\n", WSAGetLastError());
        return 1;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(PLUGIN_PORT);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        logmsg("[mudplugin] bind() failed: %d\n", WSAGetLastError());
        closesocket(srv);
        return 1;
    }

    if (listen(srv, 2) == SOCKET_ERROR) {
        logmsg("[mudplugin] listen() failed: %d\n", WSAGetLastError());
        closesocket(srv);
        return 1;
    }

    logmsg("[mudplugin] TCP server listening on 127.0.0.1:%d\n", PLUGIN_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client = accept(srv, (struct sockaddr*)&client_addr, &client_len);
        if (client == INVALID_SOCKET) {
            logmsg("[mudplugin] accept() failed: %d\n", WSAGetLastError());
            continue;
        }
        handle_client(client);
    }

    closesocket(srv);
    WSACleanup();
    return 0;
}

/* ---- DllMain ---- */

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        {
            char sys_path[MAX_PATH];
            GetSystemDirectory(sys_path, MAX_PATH);
            strcat(sys_path, "\\msimg32.dll");
            real_msimg32 = LoadLibraryA(sys_path);
            if (real_msimg32)
                real_AlphaBlend = (AlphaBlend_t)GetProcAddress(real_msimg32, "AlphaBlend");
        }
        CreateThread(NULL, 0, payload_main, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        if (real_msimg32) FreeLibrary(real_msimg32);
        if (logfile) fclose(logfile);
        break;
    }
    return TRUE;
}
