/*
HMP_SHARE.C

Halo Universal's map sharing on OpenCE (hmp.h; Halo Universal's net.c, its transfer engine kept as it was tuned on
LAN and XLink Kai, its lobby side written against OpenCE's netcode).

A client joining a game on a folder map ("+key") that it hasn't, or whose build differs from the host's (the game's
map version: cache_files_map_version, the .map's size and the checksum Halo Map Porter writes in its header), asks the
host over TCP port 5152 (beside the game's 5150/5151, on the same secure system link association). The host sends the
folder (the .map, its info .txt, preview.bin, and a pack's series.bin) in 16 KB blocks, each with its CRC32, over up to
3 connections (by ping: 1 on a LAN, 2 up to 100 ms, 3 above), with a window of about 1 KB per ms of round trip from a
128 KB budget for every download together. The client writes each block where it goes as it arrives, into
maps\<folder>.hmp_dl (the map as .part until it's whole, resume.hmp naming the build), then swaps the folder in. A
download that drops resumes from the last good block. Downloads go to T:\maps when d:\maps can't be written (a disc).

OpenCE's netcode does the rest (map_share.h's hooks, which the PC version's map sharing answers there): while a client
downloads, the consoles that have the map don't wait for it (cache_files.c gives the map no time to precache, the
host's server manager starts without it); it joins the game in progress once it has the map. If it can't get the map
(the host unreachable for ~40 s, 10 failed downloads, no room) it leaves the game with a message saying why.

maps\mapshare.txt (then T:\mapshare.txt): upload_limit_kb, window_kb, streams, total_window_kb, maps_readonly,
test_free_mb, log=0 (Halo Universal's PATCHES.txt item 10). Lines go to oxbox_log.txt ("hmp: ...").

The data moves on a background thread (16 KB stack), which also writes the play stats (hmp_stats.c).
*/

#ifdef HALO_XBOX_TARGET

/* ---------- headers */

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "bitmaps/bitmap_group.h"
#include "text/draw_string.h"
#include "interface/ui_widget.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "bungie_net/network/transport.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "hmp.h"

#include <stdarg.h>
#include <string.h>
#include <xtl.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define BARRIER() __asm__ __volatile__("" ::: "memory")
#define INVALID_HANDLE ((HANDLE)-1)

/* ---------- XNet's winsock (the retail XBE's, linked in: xbox/retail.ld) */

int __stdcall retail_ws_socket(int af, int type, int protocol);
int __stdcall retail_ws_bind(int s, void const *name, int length);
int __stdcall retail_ws_connect(int s, void const *name, int length);
int __stdcall retail_ws_send(int s, char const *buffer, int length, int flags);
int __stdcall retail_ws_recv(int s, char *buffer, int length, int flags);
int __stdcall retail_ws_closesocket(int s);
int __stdcall WSAGetLastError(void);
#define w_socket  retail_ws_socket
#define w_bind    retail_ws_bind
#define w_listen(s, b) halo_ws_listen((SOCKET)(s), (b))
#define w_accept(s, a, l) (int)halo_ws_accept((SOCKET)(s), (struct sockaddr *)(void *)(a), (l))
#define w_connect retail_ws_connect
#define w_send    retail_ws_send
#define w_recv    retail_ws_recv
#define w_close   retail_ws_closesocket
#define w_ioctl(s, c, a) halo_ws_ioctlsocket((SOCKET)(s), (long)(c), (u_long *)(a))
#define w_select(n, r, w, e, t) halo_ws_select((n), (fd_set *)(void *)(r), (fd_set *)(void *)(w), (fd_set *)(void *)(e), \
	(const struct timeval *)(void const *)(t))
#define w_setopt(s, l, n, v, len) halo_ws_setsockopt((SOCKET)(s), (l), (n), (const char *)(void const *)(v), (len))
#define w_error   WSAGetLastError
/* XNet's SOCKET is the address of its socket record (kernel pool, 0x8xxxxxxx), so a good socket is negative as an int:
only INVALID_SOCKET (-1) means "no socket". Never test sockets with < 0. */
#define NO_SOCK (-1)
#define WOULDBLOCK 10035
#define INPROGRESS 10036
#define FIONBIO 0x8004667e
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002

/* ---------- XAPI */

void *__stdcall LocalAlloc(unsigned int flags, unsigned int bytes);
void *__stdcall LocalFree(void *memory);
int __stdcall FlushFileBuffers(HANDLE file);
#define FindFirstFileA(p, b) FindFirstFileA((p), (struct _WIN32_FIND_DATAA *)(void *)(b))
#define FindNextFileA(h, b) FindNextFileA((h), (struct _WIN32_FIND_DATAA *)(void *)(b))

/* ---------- the game */

void platform_log(char const *format, ...);
char const *network_game_current_map_name(void);
char const *cache_files_loaded_map_name(void);
boolean cache_files_forget_cached_map(char const *map_name);
void display_error_text_when_main_menu_loaded(wchar_t const *text);

/* ---------- the maps (hmp_maps.c), as net.c called them */

static int hmp_find_key(const char *name) { return (int)hmp_maps_find(name); }
static const char *hmp_maps_file_c(int i, int which) { return hmp_maps_file(i, (short)which); }
static int hmp_root_c(int i) { return hmp_maps_root(i); }
static int hmp_both_c(int i) { return hmp_maps_both(i); }
static int hmp_map_path_c(int i, int r, char *buf) { return hmp_maps_map_path(i, (short)r, buf, 128); }
static int hmp_use_root_c(int i, int r) { return hmp_maps_use_root(i, (short)r); }

static void leave_game(char const *why);
static int my_machine(void);
static int host_in_game(void);
boolean network_game_server_machine_joining(void);   /* network_server_manager.c (test build 24) */
static u32 host_rtt(void) { return 0; }   /* (the client's own measure: its connection's round trip) */

/* ---- protocol ---- */
#define PORT_BE 0x2014                      /* htons(5152) */
#define REQ_MAGIC 0x71504d48                /* "HMPq" */
#define REP_MAGIC 0x72504d48                /* "HMPr" */
#define RES_MAGIC 0x73504d48                /* "HMPs": a partial download's resume.hmp */
#define JOIN_MAGIC 0x6a504d48               /* "HMPj": another connection for a download already under way */
#define VERSION 6                           /* 3: pack maps ("+pack/folder") and their series.bin; 4: several connections;
                                               5 (OpenCE): the request carries the machine, the reply whether the game is under way;
                                               6: a fifth file, the folder's showcase.txt (the showcase screensaver's shots),
                                               and the reply carries the host's copy's "Version:" (its info .txt) */
#define REP_SAME 0
#define REP_SENDING 1
#define REP_UNAVAILABLE 2
#define WHY_NOT_HOSTED 1                    /* REP_UNAVAILABLE's reason (in .streams, build 20): not the map the host has up now */
#define WHY_CHECKING 2                      /* ...the host's own copy hasn't loaded on it yet (hmp_maps.c's map checks): it
                                               sends only a map it knows loads, so a broken one reaches nobody */
#define NAME_LEN 44
#define MAX_FILES 5                         /* the .map, info .txt, preview.bin, series.bin, showcase.txt */
#define MAX_MAP_SIZE (96u << 20)
#define MAX_TXT_SIZE (64u << 10)
#define PREVIEW_SIZE 16384
#define SERIES_MAX (16 + 64 * 64 * 4)       /* a pack's series.bin (see maplist.c) */
#define CHUNK 16384                         /* block size; each block is followed by its CRC32 */
#define WINDOW_KB 8                         /* most one transfer may have in flight: the client's receive window, KB.
                                               More than ~8 packets overflows Halo's own 8-packet queue (build 9) */
#define WINDOW_BIG_KB 16                    /* ...with the 32-packet queue from build 11 (2.9 MB/s in xemu); the
                                               automatic window's floor (a LAN) */
#define WINDOW_MAX_KB 63                    /* XNet's limit for a socket buffer (it quietly cuts bigger ones to this) */
#define PING_FRAME_MS 33                    /* a heartbeat ping includes up to a frame of waiting at each end: 33 on a LAN */
#define STALL_SLOW_MS 700                   /* a send that waited this long for the client: probably a lost packet */
#define REGROW_MS 15000                     /* after backing off, 8 KB more window every 15 s without another */
#define TOTAL_KB 24                         /* the host's in-flight budget, shared by every transfer it runs, KB */
#define TOTAL_BIG_KB 48                     /* ...with build 11's 192 KB network memory (the same quarter of it) */
#define TOTAL_HUGE_KB 128                   /* ...with build 13's 256 KB (half of it: up to 3 connections x 42 KB) */
#define MAXS 3                              /* connections one download may use */
#define WINDOW_LEAST_KB 24                  /* automatic (test build 24): the least a download has in flight, all its
                                               connections together (3 x 8 KB); more by the ping as before (about 1 KB per ms
                                               of round trip, up to 3 x 63 KB, the host's budget shared). One connection of
                                               16 KB (a ping under 50 ms) ran at ~215 KB/s over a path with 80 ms and 1% loss,
                                               each lost packet stopping it for ~1 s; 3 x 8 KB at 334 KB/s, the others going
                                               on while one waits. (Bursts of 3 x 16 KB and more at a low ping overflowed a
                                               slow xemu's 32 receive slots, which then took no packets at all and left the
                                               game; real Xboxes took 63 KB windows over XLink Kai at 170-200 ms in Halo
                                               Universal's tests) */
#define WINDOW_GAME_MOST_KB 64              /* the most while the host plays a match (a console joining it): the rest of the
                                               host's upload left to the game */
#define TOTAL_JOINING_KB 64                 /* the budget while a console is joining the host's game: room in the network
                                               memory for its join */
#define PIECE 4096                          /* the host reads and sends a block in pieces this size (memory per connection) */
#define PEND 6                              /* connections the host is waiting on to say what they want */
#define HOST_BUDGET 48                      /* sends per worker pass per connection (a block is a header, 4 pieces, a CRC) */
#define FLUSH_BLOCKS 256                    /* flush the partial map to disk every 4 MB */
#define STALL_MS 10000                      /* no progress for this long: drop the transfer (Halo gives up on a silent peer at 15 s) */
#define BEAT_MS 5000                        /* progress line in the log while a transfer runs */
#define SLOTS 4
#define BUDGET 16                           /* blocks per frame per transfer */

/* a request, or (magic JOIN_MAGIC) another connection for the download with this token. flags: the most
   connections the client will open; rtt: its ping to the host (ms), in case the host has none yet */
struct req { u32 magic, version; char key[32]; u32 have_size, have_chk, resume_size, resume_chk, resume_offset, flags, rtt, token, stream, machine; };
struct rfile { char name[NAME_LEN]; u32 size; };
struct rep { u32 magic, version, status, nfiles, map_chk, start, streams, token, in_game, map_version; struct rfile f[MAX_FILES]; };
struct resume { u32 magic, size, chk; char name[NAME_LEN]; u32 good; };   /* good: bytes of the .part flushed to disk */
struct sin { short family; u16 port; u32 addr; char zero[8]; };
struct fds { u32 count; int fd[1]; };
struct tv { int sec, usec; };

static u32 g_host_addr;                     /* the host's (secure) address, from the client's connection */

/* ---- small helpers (freestanding: gcc may still call memcpy/memset) ---- */
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int ieq(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) - lower(*b);
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static char *scpy(char *d, const char *s) { while ((*d = *s)) { d++; s++; } return d; }
static int ends_with(const char *s, const char *ext) {
    int a = slen(s), b = slen(ext);
    return a > b && ieq(s + a - b, ext) == 0;
}

static u32 g_crctab[256];
static void crc_init(void) {
    u32 i, k, c;
    if (g_crctab[1]) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crctab[i] = c;
    }
}
static u32 crc_upd(u32 c, const u8 *p, u32 n) {   /* CRC32 a piece at a time: start at ~0, finish with ~ */
    while (n--) c = g_crctab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c;
}

static char g_fd[320];                      /* WIN32_FIND_DATAA */
static char g_p1[128], g_p2[128], g_p3[128];

/* The maps folders: the game's (d:\maps) and T:\maps (E:\TDATA\4d530004\maps: Halo's own storage on the hard drive),
   where downloads go when the game's can't be written (a disc). Test build 19. */
static const char *const k_root[2] = { "d:\\maps\\", "T:\\maps\\" };
static const char *const k_root_name[2] = { "maps", "T:\\maps" };   /* (for the log) */
static int g_dl_root;                       /* where downloads go (decided at the first frame) */
/* "<root><name>[suffix][\file]" (a pack's map: name = "pack/folder" -> "<root>pack\folder...") */
static char *mpath(char *buf, int r, const char *name, const char *suffix, const char *file) {
    char *p = scpy(buf, k_root[r & 1]);
    for (; *name; name++) *p++ = *name == '/' ? '\\' : *name;
    *p = 0;
    if (suffix) p = scpy(p, suffix);
    if (file) { *p++ = '\\'; p = scpy(p, file); }
    return buf;
}

/* ---- settings (maps\mapshare.txt) and the log (maps\hmp_log.txt) ---- */
static int g_cfg_log = 1;
static u32 g_ticks;                         /* menu frames seen (progress lines show the frame rate) */
static u32 g_cfg_limit;                     /* host upload limit, KB/s (0 = none) */
static u32 g_cfg_window = WINDOW_KB;        /* the most one transfer has in flight, KB (automatic: where it starts) */
static int g_cfg_auto;                      /* window_kb unset and the bigger queue taken: windows sized from ping */
static u32 g_cfg_total = TOTAL_KB;          /* the host's in-flight budget for all transfers, KB */
static u8 g_xn_pool = 64, g_xn_rxq = 32;    /* what XNetStartup has (transport_endpoint_set_winsock.c: 256 KB, 32 packets) */
static u32 g_cfg_streams;                    /* connections per download (0 = by ping); a client opens at most this many */
static int g_cfg_readonly;                   /* maps_readonly=1: act as if the maps folder can't be written (a disc) */
static u32 g_cfg_free_mb;                    /* test_free_mb=<n>: act as if only n MB were free (1-4095; 0 = the real figure) */

/* does this "name = value" line set `name`? (case-insensitive, spaces before the '=' ignored) */
static int key_is(const char *p, const char *name) {
    while (*p == ' ' || *p == '\t') p++;
    while (*name && lower(*p) == *name) { p++; name++; }
    if (*name) return 0;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '=';
}
static u32 clamp(u32 v, u32 lo, u32 hi) { return v < lo ? lo : v > hi ? hi : v; }

/* maps\mapshare.txt, then T:\mapshare.txt (test build 19: a console playing from a disc can't change the disc's
   copy; what the one in T: sets wins) */
static void config_file(const char *path);
static int g_cfg_read;
static void read_config(void) {
    g_cfg_read = 1;
    g_cfg_log = 1; g_cfg_limit = 0; g_cfg_window = 0; g_cfg_total = 0;
    g_cfg_streams = 0;
    g_cfg_readonly = 0; g_cfg_free_mb = 0;
    config_file("d:\\maps\\mapshare.txt");
    config_file("T:\\mapshare.txt");
    /* not set: once the network library took the bigger queue, the window follows each console's ping
       (from 16 KB); with Halo's own 8 packets it stays at 8 KB. The budget grows with the memory it took. */
    g_cfg_auto = !g_cfg_window && g_xn_rxq >= 16;
    if (!g_cfg_window) g_cfg_window = g_xn_rxq >= 16 ? WINDOW_BIG_KB : WINDOW_KB;
    if (!g_cfg_total) g_cfg_total = g_xn_pool >= 64 ? TOTAL_HUGE_KB : g_xn_pool >= 48 ? TOTAL_BIG_KB : TOTAL_KB;
}
static void config_file(const char *path) {
    char b[512], *p, *e;
    u32 got = 0;
    HANDLE h = CreateFileA(path, GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE) return;
    if (!ReadFile(h, b, sizeof b - 1, &got, 0)) got = 0;
    CloseHandle(h);
    b[got] = 0;
    for (p = b; *p; p = e) {
        u32 v = 0; int k, set = 0;
        for (e = p; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        for (k = 0; p[k] && p[k] != '='; k++) ;
        if (p[k] != '=') continue;
        for (k++; p[k] == ' '; k++) ;
        for (; p[k] >= '0' && p[k] <= '9'; k++) { v = v * 10 + (p[k] - '0'); set = 1; if (v > 100000) v = 100000; }
        if (!set) continue;
        if (key_is(p, "upload_limit_kb")) g_cfg_limit = v;
        else if (key_is(p, "log")) g_cfg_log = v != 0;
        else if (key_is(p, "window_kb")) g_cfg_window = clamp(v, 2, WINDOW_MAX_KB);
        else if (key_is(p, "total_window_kb")) g_cfg_total = clamp(v, 8, 128);
        else if (key_is(p, "streams")) g_cfg_streams = clamp(v, 1, MAXS);
        else if (key_is(p, "maps_readonly")) g_cfg_readonly = v != 0;
        else if (key_is(p, "test_free_mb")) g_cfg_free_mb = clamp(v, 0, 4095);   /* (0: the real figure) */
    }
}

static char *put_u(char *o, u32 v) {
    char d[11]; int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) *o++ = d[--k];
    return o;
}
/* Can the game's maps folder be written? Tried once (a file made and removed), on Halo's thread before the first log
   line. No (a disc, or maps_readonly=1): downloads go to T:\maps and the log to T:\hmp_log.txt (test build 19). */
static int g_writable = -1;
static int maps_writable(void) {
    HANDLE h;
    if (g_writable >= 0) return g_writable;
    if (!g_cfg_read) read_config();
    g_writable = 0;
    if (!g_cfg_readonly) {
        h = CreateFileA("d:\\maps\\hmp_write.tmp", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0x80, 0);
        if (h != INVALID_HANDLE) { CloseHandle(h); DeleteFileA("d:\\maps\\hmp_write.tmp"); g_writable = 1; }
    }
    g_dl_root = g_writable ? 0 : 1;
    return g_writable;
}
/* a line in the log (oxbox_log.txt), Halo's thread only: %s %d %u %x, %b (a byte in hex), %a (an IPv4 address in
network byte order), %w (a wide string) */
static char g_line[240];
static void lg(const char *fmt, ...) {
    va_list ap;
    char *o = g_line, *end = g_line + sizeof g_line - 4;
    if (!g_cfg_log) return;
    va_start(ap, fmt);
    for (; *fmt && o < end - 12; fmt++) {
        if (*fmt != '%') { *o++ = *fmt; continue; }
        switch (*++fmt) {
        case 's': { const char *s = va_arg(ap, const char *); while (s && *s && o < end) *o++ = *s++; break; }
        case 'd': { int v = va_arg(ap, int); if (v < 0) { *o++ = '-'; v = -v; } o = put_u(o, (u32)v); break; }
        case 'u': o = put_u(o, va_arg(ap, u32)); break;
        case 'w': { const u16 *w = va_arg(ap, const u16 *); if (!w) *o++ = '?'; while (w && *w && o < end) { *o++ = *w < 127 ? (char)*w : '?'; w++; } break; }
        case 'x': { u32 v = va_arg(ap, u32); int k; for (k = 28; k >= 0; k -= 4) *o++ = "0123456789abcdef"[(v >> k) & 15]; break; }
        case 'b': { u32 v = va_arg(ap, u32); *o++ = "0123456789abcdef"[(v >> 4) & 15]; *o++ = "0123456789abcdef"[v & 15]; break; }
        case 'a': { u32 v = va_arg(ap, u32); int k; for (k = 0; k < 4; k++) { o = put_u(o, (v >> (8 * k)) & 255); if (k < 3) *o++ = '.'; } break; }
        default: *o++ = *fmt;
        }
    }
    va_end(ap);
    *o = 0;
    platform_log("hmp: %s", g_line);
}

/* size of a file (0xffffffff if it isn't there) */
static u32 file_size(const char *path) {
    u32 n;
    HANDLE h = CreateFileA(path, GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE) return 0xffffffff;
    n = SetFilePointer(h, 0, 0, 2);
    CloseHandle(h);
    return n;
}

/* a folder map's identity: file size + the checksum in its header (Halo Map Porter stamps a CRC there).
   path = the .map; 0 = none */
static int map_identity(const char *path, u32 *size, u32 *chk) {
    u8 hdr[0x68];
    u32 got = 0;
    HANDLE h;
    if (!path) return 0;
    h = CreateFileA(path, GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE) return 0;
    *size = SetFilePointer(h, 0, 0, 2);
    SetFilePointer(h, 0, 0, 0);
    if (!ReadFile(h, hdr, sizeof hdr, &got, 0)) got = 0;
    CloseHandle(h);
    if (got != sizeof hdr) return 0;
    *chk = *(u32 *)(hdr + 0x64);
    return 1;
}

/* delete a folder and the files in it (no subfolders) */
static void rm_folder(const char *path) {
    char pat[128], f[128];
    HANDLE h;
    scpy(scpy(pat, path), "\\*.*");
    h = FindFirstFileA(pat, g_fd);
    if (h != INVALID_HANDLE) {
        do {
            char *nm = g_fd + 0x2c;
            if (*(u32 *)g_fd & 0x10) continue;
            scpy(scpy(scpy(f, path), "\\"), nm);
            DeleteFileA(f);
        } while (FindNextFileA(h, g_fd));
        CloseHandle(h);
    }
    RemoveDirectoryA(path);
}

/* "<root><pack>[\file]" for a pack's map ("pack/folder"); 0 = not in a pack */
static char *pack_dir(char *buf, int r, const char *key1, const char *file) {
    char pk[32];
    int k = 0;
    for (; key1[k] && key1[k] != '/' && k < 31; k++) pk[k] = key1[k];
    if (key1[k] != '/') return 0;
    pk[k] = 0;
    return mpath(buf, r, pk, 0, file);
}
/* where the host reads a map's file from: the folder (in maps folder r), except a pack's series.bin (in the pack's folder) */
static char *src_path(char *buf, int r, const char *folder, const char *file) {
    if (ieq(file, "series.bin") == 0 && pack_dir(buf, r, folder, file)) return buf;
    return mpath(buf, r, folder, 0, file);
}
/* free space where a download goes, bytes (capped at 4 GB; test_free_mb in mapshare.txt pretends) */
static u32 free_bytes(int r) {
    ULARGE_INTEGER avail, total, tfree;
    if (g_cfg_free_mb) return g_cfg_free_mb << 20;
    if (!GetDiskFreeSpaceExA(r ? "T:\\" : "d:\\maps\\", &avail, &total, &tfree)) return 0xffffffff;   /* (can't tell: try) */
    return avail.HighPart ? 0xffffffff : avail.LowPart;
}
/* a name we're willing to create: plain characters, no path tricks */
static int safe_name(const char *s, int max) {
    int n = 0;
    if (!s[0] || s[0] == '.' || s[0] == ' ') return 0;
    for (; s[n]; n++) {
        u8 c = (u8)s[n];
        if (n >= max || c < 32 || c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') return 0;
        if (c == '.' && s[n + 1] == '.') return 0;
    }
    return 1;
}

/* a map's folder: "folder" or "pack/folder", 30 characters at most */
static int safe_key(const char *s) {
    char part[32];
    int k = 0;
    if (slen(s) > 30) return 0;
    for (; s[k] && s[k] != '/'; k++) part[k] = s[k];
    part[k] = 0;
    return safe_name(part, 30) && (!s[k] || safe_name(s + k + 1, 30));
}

static void nonblock(int s) { u32 one = 1; w_ioctl(s, FIONBIO, &one); }
/* XNet refuses to shrink a connected socket's receive buffer (it has already offered that window),
   so the host sets its accepted sockets' send buffer only. The send buffer is what caps the host's
   unacknowledged data (build 3's 64 KB filled XNet's pool). */
static void sockbuf(int s, u32 snd_kb, u32 rcv_kb) {
    u32 v = snd_kb << 10, w = rcv_kb << 10, one = 1;
    if (w_setopt(s, 0xffff, SO_SNDBUF, &v, 4) || (rcv_kb && w_setopt(s, 0xffff, SO_RCVBUF, &w, 4)))
        lg("socket buffer %u/%u not accepted (error %d)", v, w, w_error());
    w_setopt(s, 6, 1, &one, 4);                   /* TCP_NODELAY */
}
/* the window a client offers (and reads per pass): with automatic windows the most XNet allows, since the
   host's send buffer is what decides how much is in flight */
static u32 rcv_window(void) { return g_cfg_auto ? WINDOW_MAX_KB : g_cfg_window; }

/* ---- on-screen status: the lobby's map name line, and the countdown line ----
   Build 17: the countdown line is one text across the lobby's message bar (maplist.c widens Halo's label
   box while it shows), so it can say who: "Sending map to King: 53%, 1:20 left". */
static u16 g_status[64];
static u16 g_label[64];                     /* the line under the lobby (hmp_share_render) */
static u16 *g_lo;                           /* where the countdown line is being written */
static int g_status_on;
#define LINE_END (g_label + 60)
static void L_a(const char *s) { while (s && *s && g_lo < LINE_END) *g_lo++ = (u8)*s++; *g_lo = 0; }
static void L_w(const u16 *s) { int n = 0; while (s && *s && n++ < 24 && g_lo < LINE_END) *g_lo++ = *s++; *g_lo = 0; }
static void L_u(u32 v) { char d[12]; *put_u(d, v) = 0; L_a(d); }
static void L_start(const char *s) { g_lo = g_label; *g_lo = 0; L_a(s); }
static void status(const char *a, int pct, const char *b) {
    int n = 0;
    while (*a && n < 60) g_status[n++] = (u8)*a++;
    if (pct >= 0) {
        char d[4]; int k = 0;
        if (pct > 100) pct = 100;
        do { d[k++] = (char)('0' + pct % 10); pct /= 10; } while (pct && k < 3);
        while (k && n < 60) g_status[n++] = (u8)d[--k];
        if (n < 60) g_status[n++] = '%';
    }
    while (b && *b && n < 62) g_status[n++] = (u8)*b++;
    g_status[n] = 0;
    g_status_on = 1;
}
/* "53%, 1:20 left" (eta in seconds; 0 = not known yet, left out) */
static char *pct_eta(char *buf, int pct, u32 eta) {
    char *o = buf;
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    o = put_u(o, (u32)pct); *o++ = '%';
    if (eta) {
        if (eta > 5999) eta = 5999;
        *o++ = ','; *o++ = ' '; o = put_u(o, eta / 60); *o++ = ':';
        *o++ = (char)('0' + eta % 60 / 10); *o++ = (char)('0' + eta % 10);
        o = scpy(o, " left");
    }
    *o = 0;
    return buf;
}
static void L_pct(int pct, u32 eta) { char v[24]; L_a(pct_eta(v, pct, eta)); }
/* recent speed in bytes/s from a byte count that only grows, updated about once a second (0 = not known yet) */
static void rate_upd(u32 *rate, u32 *t, u32 *last, u32 count, u32 now) {
    u32 dt = now - *t, d = count - *last, inst;
    if (!*t) { *t = now; *last = count; return; }
    if (dt < 1000) return;
    inst = d / dt * 1000 + d % dt * 1000 / dt;
    *rate = *rate ? (*rate * 3 + inst) / 4 : inst;
    *t = now; *last = count;
}
boolean hmp_share_status_active(void) { return g_status_on != 0; }
wchar_t *hmp_share_status_text(void) { return (wchar_t *)g_status; }

/* ================================================================ worker ================ */

/* A transfer is either the main thread's (JOB_IDLE / JOB_DONE) or the worker's (JOB_RUN): while the
   worker has it, the main thread only reads its progress counters and may set `stop`. */
enum { JOB_IDLE, JOB_RUN, JOB_DONE };
#define JOB_END(j, r) do { (j).result = (r); BARRIER(); (j).run = JOB_DONE; return 1; } while (0)
enum { R_NONE, R_FILE_DONE, R_FINISHED, R_HUNGUP, R_RECV, R_SEND, R_CRC, R_WRITE, R_READ, R_STALL, R_STOPPED };
static HANDLE g_worker;
static int g_worker_failed, g_worker_priority;
static u32 g_flush_t;
static unsigned long __stdcall worker_main(void *unused);
#define WORKER_STACK 0x4000     /* 16 KB, like Halo's own file-copying threads (0 = the XBE's 512 KB: too much at boot) */
#define WORKER_PRIORITY 2         /* THREAD_PRIORITY_HIGHEST in the menus, so it runs the moment data can move (it sleeps
                                     1 ms after every pass, so it can't hog); normal during a game (hmp_share_tick) */
static void start_worker(void) {
    if (g_worker) return;
    g_worker = CreateThread(0, WORKER_STACK, worker_main, 0, 0, 0);
    if (g_worker) { SetThreadPriority(g_worker, WORKER_PRIORITY); g_worker_priority = WORKER_PRIORITY; lg("thread: transfer thread started"); }
    else if (!g_worker_failed) { g_worker_failed = 1; lg("thread: couldn't start the transfer thread; the menu frame moves the data"); }
}
/* what the thread did since the last progress line (read and cleared by the main thread) */
static struct { volatile u32 passes, busy, woke, gap, last; } g_ws;

/* ================================================================= host ================ */

/* One connection of a transfer. The main thread fills it in, then sets `on`; after that only the worker
   touches it until the transfer is handed back. */
struct hst {
    volatile int on;
    int s, closed;                /* closed: the client hung up this connection (worker) */
    HANDLE h; int hf; u32 hpos;   /* the worker's handle on a file (hf = file + 1, 0 = none) and where its pointer is */
    int file, phase;              /* the block going out: its file; 0 none, 1 header and data, 2 its CRC */
    u32 off, len, done, crc;      /* its offset and length, data read so far, CRC so far */
    u8 *buf; int bpos, blen;      /* what's going out now: the reply, a header, a piece of data or a CRC */
    u32 wait, win_cur;            /* when send() first found the window full; the send buffer it has now (KB) */
};
struct slot {
    volatile int run, stop, result, err;   /* worker hand-over (see JOB_*) */
    int orphan;                   /* the slot was closed while the worker had it: free it when it lets go */
    volatile int state;           /* 0 free, 2 sending, 3 everything sent: waiting for the hang-ups */
    volatile u32 t, sent;         /* last progress (worker), file bytes sent (worker) */
    u32 t0, addr, done_ms, token; /* start, client address, time to send everything, what its extra connections quote */
    int logged_done, want;        /* want: connections the client was asked to open */
    volatile int nst;             /* connections attached */
    struct hst st[MAXS];
    volatile u32 wait_max;        /* the longest wait for room on any of its connections (ms) */
    volatile u32 win_want;        /* send buffer per connection (KB) the main thread wants; the worker applies it */
    volatile int win_err;         /* the worker couldn't change it: XNet's error, for the main thread to log */
    u32 win_set;                  /* the transfer's share the main thread last settled on (KB, all connections) */
    volatile u32 stalls, stall_ms;  /* worker: sends that waited STALL_SLOW_MS or more for the client; the last such wait */
    u32 stalls_seen, win_cap, cap_t;  /* main thread: stalls handled; the share's cap after backing off, and since when */
    u32 win_logged, ping;         /* main thread: the share last logged; the console's ping (ms, 0 = unknown) */
    u32 beat, beat_sent, beat_ticks;   /* last progress line in the log */
    u32 total, start;             /* bytes this transfer will send; where the map starts (resume) */
    u32 rate, rate_t, rate_sent;  /* main thread: recent speed (bytes/s, 0 = not known yet), for the time left */
    int nfiles, ci; u32 coff;     /* the cursor: the next block to hand out (ci-th file in sending order, offset) */
    char names[MAX_FILES][NAME_LEN];
    u32 sizes[MAX_FILES];
    char folder[32];
    int root;                     /* the maps folder the hosted copy is in */
    int machine;                  /* its Halo machine index (-1: not known) */
};
static struct slot g_sl[SLOTS];
static struct pend { int used, s, qlen; u32 t, addr; struct req q; } g_pd[PEND];   /* accepted, not yet asked */
static char g_wpath[128];                   /* the worker's path buffer */
static int g_lsock = NO_SOCK;
static u32 g_lretry, g_tok_t;               /* g_tok_t / g_tokens: the upload limit, kept by the worker */
static volatile int g_tokens;
static struct rep g_hrep;

static void slot_close(struct slot *c, const char *why, int err) {
    int k;
    if (why) lg("host: player %d: %s (error %d, %u bytes sent)", (int)(c - g_sl), why, err, c->sent);
    if (c->run == JOB_RUN) { c->stop = 1; c->orphan = 1; return; }   /* freed once the worker lets go */
    for (k = 0; k < MAXS; k++) {
        struct hst *t = &c->st[k];
        if (t->hf) CloseHandle(t->h);
        if (t->on) w_close(t->s);
        if (t->buf) LocalFree(t->buf);
    }
    memset(c, 0, sizeof *c);
}
static void pend_close(struct pend *p) { w_close(p->s); p->used = 0; }

/* the cursor: the next block to send. Files go small ones first, then the map (file 0) from the resume point. */
static int next_block(struct slot *c, int *file, u32 *off) {
    while (c->ci < c->nfiles) {
        int f = (c->ci + 1) % c->nfiles;
        if (c->coff < c->sizes[f]) { *file = f; *off = c->coff; c->coff += CHUNK; return 1; }
        c->ci++;
        c->coff = (c->ci + 1) % c->nfiles ? 0 : c->start;
    }
    return 0;
}
static int cursor_done(struct slot *c) { return c->ci >= c->nfiles; }

/* worker: what goes out next on this connection: 1 = in t->buf, 0 = nothing left, -1 = read error */
static int st_fill(struct slot *c, struct hst *t) {
    u32 n, got = 0, pos;
    if (t->phase == 1 && t->done == t->len) {            /* the block's data is out: its CRC */
        u32 crc = ~t->crc;
        memcpy(t->buf, &crc, 4);
        t->phase = 2; t->bpos = 0; t->blen = 4;
        return 1;
    }
    if (t->phase == 2) t->phase = 0;
    if (t->phase == 0) {                                  /* a new block: its header */
        u32 where;
        if (!next_block(c, &t->file, &t->off)) return 0;
        t->len = c->sizes[t->file] - t->off;
        if (t->len > CHUNK) t->len = CHUNK;
        where = (u32)t->file << 28 | t->off;
        memcpy(t->buf, &where, 4);
        t->crc = crc_upd(~0u, t->buf, 4); t->done = 0; t->phase = 1;
        t->bpos = 0; t->blen = 4;
        return 1;
    }
    if (t->hf != t->file + 1) {                          /* the next piece of its data */
        if (t->hf) CloseHandle(t->h);
        t->hf = 0;
        t->h = CreateFileA(src_path(g_wpath, c->root, c->folder, c->names[t->file]), GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
        if (t->h == INVALID_HANDLE) return -1;
        t->hf = t->file + 1; t->hpos = 0;
    }
    pos = t->off + t->done;
    if (t->hpos != pos) { SetFilePointer(t->h, (int)pos, 0, 0); t->hpos = pos; }
    n = t->len - t->done;
    if (n > PIECE) n = PIECE;
    if (!ReadFile(t->h, t->buf, n, &got, 0) || got != n) return -1;
    t->crc = crc_upd(t->crc, t->buf, n);
    t->done += n; t->hpos += n; c->sent += n;
    t->bpos = 0; t->blen = (int)n;
    return 1;
}

/* worker: send on one connection. 1 = something moved, 0 = nothing could, -1 = send failed, -2 = read failed */
static int st_send(struct slot *c, struct hst *t, u32 now) {
    int k, n, r, moved = 0;
    for (k = 0; k < HOST_BUDGET; k++) {
        int want;
        if (t->bpos >= t->blen) {
            r = st_fill(c, t);
            if (r < 0) return -2;
            if (!r) break;
        }
        want = t->blen - t->bpos;
        if (g_cfg_limit) { if (g_tokens <= 0) break; if (want > g_tokens) want = g_tokens; }
        n = w_send(t->s, t->buf + t->bpos, want, 0);
        if (n < 0) {
            if ((r = w_error()) != WOULDBLOCK) { c->err = r; return -1; }
            if (!t->wait) t->wait = now;
            break;
        }
        if (t->wait) {
            u32 w = now - t->wait;
            if (w > c->wait_max) c->wait_max = w;
            if (w >= STALL_SLOW_MS) { c->stall_ms = w; BARRIER(); c->stalls++; }   /* (the main thread backs off) */
            t->wait = 0;
        }
        t->bpos += n; c->t = now; moved = 1;
        if (g_cfg_limit) g_tokens -= n;
        if (t->bpos < t->blen) break;
    }
    return moved;
}

/* worker: move one transfer's data on all its connections. 1 = something moved. */
static int slot_pump(struct slot *c, u32 now) {
    int k, n, r = 0, moved = 0, open = 0, busy = 0;
    u32 wkb = c->win_want;
    if (c->stop) { JOB_END(*c, R_STOPPED); }
    for (k = 0; k < MAXS; k++) {
        struct hst *t = &c->st[k];
        if (!t->on || t->closed) continue;
        open++;
        if (wkb && wkb != t->win_cur) {                   /* a new share (another player, another connection, the ping) */
            u32 v = wkb << 10;
            if (w_setopt(t->s, 0xffff, SO_SNDBUF, &v, 4)) { r = w_error(); c->win_err = r ? r : -1; t->win_cur = wkb; }
            else t->win_cur = wkb;                        /* (on failure: no retry until the share changes again) */
        }
        for (;;) {                                        /* the client's one-byte acks: read and drop */
            char junk[64];
            n = w_recv(t->s, junk, sizeof junk, 0);
            if (n > 0) { moved = 1; continue; }
            if (n < 0 && (r = w_error()) == WOULDBLOCK) break;
            if (c->state == 3) { t->closed = 1; break; }  /* everything sent: the client is done with this one */
            if (n == 0) { JOB_END(*c, R_HUNGUP); }
            c->err = r; JOB_END(*c, R_RECV);
        }
        if (t->closed) { open--; continue; }
        r = st_send(c, t, now);
        if (r == -1) { JOB_END(*c, R_SEND); }
        if (r == -2) { JOB_END(*c, R_READ); }
        moved |= r;
        if (t->phase || t->bpos < t->blen) busy = 1;
    }
    if (c->state == 2 && !busy && cursor_done(c)) { c->done_ms = now - c->t0 + 1; c->state = 3; c->t = now; }
    /* everything sent: wait for the client to hang up every connection; closing first could cut off data in flight */
    if (c->state == 3 && !open) { JOB_END(*c, R_FINISHED); }
    if (!moved && now - c->t > STALL_MS) { JOB_END(*c, R_STALL); }
    return moved;
}

/* how many connections a transfer gets: by ping (the host's measure, else the client's), at most what the
   client will open; with a fixed window or Halo's own queue, one */
static int conn_count(struct slot *c, const struct req *q) {
    u32 ping = q->rtt, n;
    (void)ping;
    /* (test build 24: all of them, whatever the ping. A lost packet stops a connection until XNet sends it again,
       ~1 s later; over the internet the others keep the download going meanwhile) */
    n = g_cfg_streams ? g_cfg_streams : !g_cfg_auto ? 1 : MAXS;
    if (n > q->flags) n = q->flags;
    return n ? (int)n : 1;
}

/* a request from pending connection p: take a slot and answer */
static void slot_request(struct slot *c, struct pend *p, u32 now) {
    struct req *q = &p->q;
    struct hst *t = &c->st[0];
    const char *host_map = network_game_current_map_name();
    const char *why = "not the map being hosted";
    int i, k, n = 0, kk = (int)(c - g_sl);
    u32 size = 0, chk = 0, start = 0, total = 0;
    memset(c, 0, sizeof *c);
    c->machine = q->machine < 16 ? (int)q->machine : -1; c->ping = q->rtt;
    c->addr = p->addr; c->t = c->t0 = c->beat = now;
    t->s = p->s; p->used = 0;
    memset(&g_hrep, 0, sizeof g_hrep);
    g_hrep.magic = REP_MAGIC; g_hrep.version = VERSION; g_hrep.status = REP_UNAVAILABLE; g_hrep.in_game = host_in_game();
    q->key[31] = 0;
    i = hmp_find_key(q->key);
    if (q->magic != REQ_MAGIC || q->version != VERSION) why = "different map sharing version";
    else if (q->key[0] != '+' || !host_map || ieq(q->key, host_map) != 0) g_hrep.streams = WHY_NOT_HOSTED;   /* (changing maps: ask again) */
    else if (i < 0 || !hmp_map_path_c(i, -1, g_p1) || !map_identity(g_p1, &size, &chk)) why = "can't read its own copy";
    else if (!hmp_maps_is_good(q->key)) g_hrep.streams = WHY_CHECKING;   /* (not yet loaded here: ask again) */
    else {
        g_hrep.map_chk = chk;
        g_hrep.map_version = hmp_maps_version(i);
        if (q->have_size == size && q->have_chk == chk) {
            g_hrep.status = REP_SAME;
        } else {
            scpy(c->folder, q->key + 1);
            c->root = hmp_root_c(i);
            for (k = 0; k < MAX_FILES; k++) {
                const char *f = hmp_maps_file_c(i, k);
                u32 sz;
                if (!f || slen(f) >= NAME_LEN) continue;
                sz = file_size(src_path(g_p2, c->root, c->folder, f));
                if (sz == 0xffffffff) continue;
                scpy(c->names[n], f); c->sizes[n] = sz;
                scpy(g_hrep.f[n].name, f); g_hrep.f[n].size = sz;
                total += sz; n++;
            }
            if (n && ends_with(c->names[0], ".map")) {
                g_hrep.status = REP_SENDING; g_hrep.nfiles = n; c->nfiles = n;
                if (q->resume_size == size && q->resume_chk == chk && q->resume_offset && q->resume_offset < size &&
                    q->resume_offset % CHUNK == 0)
                    start = q->resume_offset;
                g_hrep.start = c->start = start;
                c->total = total - start;
                c->coff = n == 1 ? start : 0;
                c->want = conn_count(c, q);
                c->token = now ^ (c->addr * 2654435761u) ^ ((u32)kk << 24) ^ 0x5eed;
                g_hrep.streams = (u32)c->want; g_hrep.token = c->token;
            }
        }
    }
    if (g_hrep.status == REP_SAME) lg("host: player %d asked for %s: has the same build", kk, q->key);
    else if (g_hrep.status == REP_SENDING) {
        const char *over = c->want == 3 ? " over 3 connections" : c->want == 2 ? " over 2 connections" : "";
        if (start) lg("host: player %d asked for %s: sending %d files, %u bytes, resuming at %u%s", kk, q->key, n, total, start, over);
        else lg("host: player %d asked for %s: sending %d files, %u bytes%s", kk, q->key, n, total, over);
    } else lg("host: player %d asked for %s: refused (%s)", kk, q->key, why);
    t->buf = (u8 *)LocalAlloc(0, PIECE + 16);
    if (!t->buf) { t->on = 1; slot_close(c, "out of memory", 0); return; }
    memcpy(t->buf, &g_hrep, sizeof g_hrep);
    t->bpos = 0; t->blen = sizeof g_hrep; t->win_cur = g_cfg_window;
    c->win_set = c->win_logged = g_cfg_window; c->win_cap = WINDOW_MAX_KB * MAXS; c->cap_t = now;
    c->nst = 1; t->on = 1;
    c->beat_sent = 0; c->beat_ticks = g_ticks;
    c->state = g_hrep.status == REP_SENDING ? 2 : 3;
    c->result = R_NONE;
    start_worker();
    BARRIER();
    c->run = JOB_RUN;
}

/* another connection for a transfer under way (pending connection p, which sent a JOIN) */
static void slot_join(struct pend *p, u32 now) {
    int i, k = (int)p->q.stream;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        struct hst *t;
        if (c->state != 2 || c->orphan || c->token != p->q.token || c->addr != p->addr) continue;
        if (k < 1 || k >= c->want || c->st[k].on) break;
        t = &c->st[k];
        memset(t, 0, sizeof *t);
        t->buf = (u8 *)LocalAlloc(0, PIECE + 16);
        if (!t->buf) break;
        t->s = p->s; t->win_cur = g_cfg_window; p->used = 0;
        BARRIER();
        t->on = 1;
        c->nst++;
        lg("host: player %d: connection %d of %d joined", i, k + 1, c->want);
        (void)now;
        return;
    }
    lg("host: turned away a connection for a transfer that isn't running");
    pend_close(p);
}

static void slot_tick(struct slot *c, u32 now) {
    int k = (int)(c - g_sl);
    if (c->run == JOB_DONE) {                             /* the worker let go: say why and free the slot */
        static const char *const why[] = { 0, 0, 0, "hung up mid-transfer", "receive failed", "send failed", 0, 0,
                                           "couldn't read the map file", "no progress for 10 s", 0 };
        int res = c->result;
        BARRIER();
        c->run = JOB_IDLE;
        if (c->orphan) { slot_close(c, 0, 0); return; }
        if (res == R_FINISHED && c->done_ms && !c->logged_done)
            lg("host: player %d: sent %u bytes in %u ms (%u KB/s)", k, c->sent, c->done_ms, c->sent / c->done_ms);
        slot_close(c, res < (int)(sizeof why / sizeof why[0]) ? why[res] : "stopped", c->err);
        return;
    }
    if (c->run != JOB_RUN) return;
    if (c->state == 2) rate_upd(&c->rate, &c->rate_t, &c->rate_sent, c->sent, now);
    if (c->win_err) { lg("host: player %d: window change not accepted (error %d)", k, c->win_err); c->win_err = 0; }
    if (c->state == 3 && c->done_ms && !c->logged_done) {
        c->logged_done = 1;
        lg("host: player %d: sent %u bytes in %u ms (%u KB/s)", k, c->sent, c->done_ms, c->sent / c->done_ms);
    } else if (c->state == 2 && now - c->beat >= BEAT_MS) {
        u32 sent = c->sent, per = c->st[0].win_cur;
        char w[16], *o = w;
        if (c->nst > 1) { o = put_u(o, (u32)c->nst); *o++ = 'x'; }
        o = put_u(o, per); *o = 0;
        lg("host: player %d: %u of %u bytes sent, %u KB/s, %u frames, window %s KB, ping %u ms; thread %u passes (%u busy, "
           "%u woken, longest gap %u ms), longest wait for the client %u ms", k, sent, c->total, (sent - c->beat_sent) / (now - c->beat),
           g_ticks - c->beat_ticks, w, c->ping, g_ws.passes, g_ws.busy, g_ws.woke, g_ws.gap, c->wait_max);
        g_ws.passes = g_ws.busy = g_ws.woke = g_ws.gap = 0; c->wait_max = 0;
        c->beat = now; c->beat_sent = sent; c->beat_ticks = g_ticks;
    }
}

/* the connections that haven't said what they want yet: read their request or JOIN */
static void pend_tick(u32 now) {
    int i, j, n, r;
    for (i = 0; i < PEND; i++) {
        struct pend *p = &g_pd[i];
        if (!p->used) continue;
        if (now - p->t > STALL_MS) { lg("host: a connection never asked for anything; closed"); pend_close(p); continue; }
        n = w_recv(p->s, (char *)&p->q + p->qlen, sizeof p->q - p->qlen, 0);
        if (n == 0 || (n < 0 && (r = w_error()) != WOULDBLOCK)) { pend_close(p); continue; }
        if (n > 0) p->qlen += n;
        if (p->qlen < (int)sizeof p->q) continue;
        if (p->q.magic == JOIN_MAGIC) { slot_join(p, now); continue; }
        for (j = 0; j < SLOTS && (g_sl[j].state || g_sl[j].run != JOB_IDLE); j++) ;
        if (j == SLOTS) { lg("host: turned away a player (all %d transfer slots busy)", SLOTS); pend_close(p); continue; }
        lg("host: player %d connected from %a", j, p->addr);
        slot_request(&g_sl[j], p, now);
    }
}

static int host_sending(void) {
    int i, n = 0;
    for (i = 0; i < SLOTS; i++) if (g_sl[i].state == 2) n++;
    return n;
}

/* how much one transfer should have in flight, all its connections together. Fixed: window_kb a connection.
   Automatic: about 1 KB per ms of round trip (~1 MB/s), from the console's ping (its connect time), from 24 KB over
   3 connections (test build 24; was 16 KB on one) to 63 KB a connection, at most 64 KB while the host plays a match,
   and less for a while after a send had to wait ~1 s for the client (a lost packet: XNet resends after ~1 s). */
static u32 slot_target(struct slot *c, u32 now) {
    u32 kb, ping, most = WINDOW_MAX_KB * (u32)c->want, least;
    ping = c->ping;
    if (!g_cfg_auto) return g_cfg_window * (u32)c->want;
    /* (test build 24: at least 8 KB a connection; while the host plays, at most WINDOW_GAME_MOST_KB) */
    least = WINDOW_LEAST_KB * (u32)c->want / MAXS;
    if (least < 8) least = 8;
    if (host_in_game() && most > WINDOW_GAME_MOST_KB) most = WINDOW_GAME_MOST_KB;
    if (least > most) least = most;
    if (c->stalls != c->stalls_seen) {               /* (all its connections stalling at once is one hiccup: once per 2 s) */
        c->stalls_seen = c->stalls;
        if (c->win_set > least && now - c->cap_t >= 2000) {
            kb = c->win_set * 3 / 4;
            c->win_cap = kb < least ? least : kb;
            lg("host: player %d: waited %u ms for the client (a lost packet?); window down to %u KB", (int)(c - g_sl), c->stall_ms, c->win_cap);
        }
        c->cap_t = now;
    } else if (c->win_cap < most && now - c->cap_t >= REGROW_MS) {
        c->win_cap = c->win_cap + 8 < most ? c->win_cap + 8 : most;
        c->cap_t = now;
    }
    kb = ping > PING_FRAME_MS ? ping - PING_FRAME_MS : 0;
    kb = kb >= most ? most : kb & ~3u;                    /* (steps of 4 KB: a ping that wobbles doesn't) */
    if (kb < least) kb = least;
    if (kb > c->win_cap) kb = c->win_cap;
    return kb < least ? least : kb;
}

/* split the in-flight budget between the players downloading right now: each wants its target; whoever wants
   less than an even share gets it, and the rest split what's left evenly (at least 4 KB each); each transfer's
   share is split evenly between its connections (4-63 KB each) */
static int g_win_n;
static void share_windows(u32 now) {
    int i, n = 0, m, any, same = 1;
    u32 want[SLOTS], got[SLOTS], left = g_cfg_total, fair;
    u8 on[SLOTS], set[SLOTS];
    static int joining_logged;
    /* (a console joining the host's game: the transfers leave room in the network memory for its join) */
    if (g_cfg_auto && network_game_server_machine_joining()) {
        if (left > TOTAL_JOINING_KB) left = TOTAL_JOINING_KB;
        if (!joining_logged) { joining_logged = 1; lg("host: a console is joining: downloads' windows down to %u KB in all for now", left); }
    } else joining_logged = 0;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        on[i] = c->state == 2 && !c->orphan; set[i] = 0;
        if (on[i]) { want[i] = slot_target(c, now); n++; }
    }
    if (!n) { g_win_n = 0; return; }
    for (m = n; m > 0; ) {
        fair = left / (u32)m; any = 0;
        for (i = 0; i < SLOTS; i++)
            if (on[i] && !set[i] && want[i] <= fair) { got[i] = want[i]; left -= want[i]; set[i] = 1; m--; any = 1; }
        if (!any) {
            for (i = 0; i < SLOTS; i++) if (on[i] && !set[i]) { got[i] = fair < 4 ? 4 : fair; set[i] = 1; }
            break;
        }
    }
    if (n != g_win_n && (n > 1 || g_win_n > 1)) {
        char b[40], *o = b;
        u32 first = 0;
        for (i = 0; i < SLOTS; i++) if (on[i]) {
            if (o != b) *o++ = '/';
            o = put_u(o, got[i]);
            if (!first) first = got[i]; else if (got[i] != first) same = 0;
        }
        *o = 0;
        if (same) lg("host: %d player%s downloading: window %u KB each", n, n > 1 ? "s" : "", first);
        else lg("host: %d players downloading: windows %s KB", n, b);
    }
    g_win_n = n;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        u32 kb = got[i], per;
        if (!on[i]) continue;
        per = kb / (u32)(c->nst ? c->nst : 1);
        per = per < 4 ? 4 : per > WINDOW_MAX_KB ? WINDOW_MAX_KB : per;
        if (g_cfg_auto && (kb >= c->win_logged + 8 || kb + 8 <= c->win_logged)) {
            if (c->want > 1) lg("host: player %d: ping %u ms: window %u KB (%d connections)", i, c->ping, kb, c->want);
            else lg("host: player %d: ping %u ms: window %u KB", i, c->ping, kb);
            c->win_logged = kb;
        }
        c->win_set = kb;
        if (c->win_want != per) c->win_want = per;        /* the worker has the sockets: it applies it */
    }
}

static int dl_summary(int *pct, u32 *eta);
static void host_line(int ns, int pct, u32 eta);
static void host_stop(void) {
    int i;
    for (i = 0; i < SLOTS; i++) if (g_sl[i].state && !g_sl[i].orphan) slot_close(&g_sl[i], "game closed", 0);
    for (i = 0; i < PEND; i++) if (g_pd[i].used) pend_close(&g_pd[i]);
    if (g_lsock != NO_SOCK) { w_close(g_lsock); lg("host: stopped sharing"); }
    g_lsock = NO_SOCK;
}

static void host_tick(u32 now) {
    int i, ns, e;
    if (g_lsock == NO_SOCK) {
        struct sin a;
        const char *step = "socket";
        if (now - g_lretry < 2000) return;
        g_lretry = now;
        read_config();
        for (i = 0; i < SLOTS; i++)
            if (g_sl[i].run == JOB_IDLE && !g_sl[i].state) memset(&g_sl[i], 0, sizeof g_sl[i]);
        g_lsock = w_socket(2, 1, 0);
        if (g_lsock != NO_SOCK) {
            nonblock(g_lsock);
            sockbuf(g_lsock, g_cfg_window, g_cfg_window);
            memset(&a, 0, sizeof a); a.family = 2; a.port = PORT_BE;
            step = "bind";
            if (!w_bind(g_lsock, &a, sizeof a)) {
                step = "listen";
                if (!w_listen(g_lsock, SLOTS * MAXS)) {
                    if (g_cfg_limit) lg("host: sharing maps on port 5152, upload limit %u KB/s", g_cfg_limit);
                    else lg("host: sharing maps on port 5152");
                    g_tok_t = now; g_tokens = 0;
                    goto ready;
                }
            }
        }
        e = w_error();
        lg("host: couldn't start sharing (%s failed, error %d); retrying", step, e);
        if (g_lsock != NO_SOCK) w_close(g_lsock);
        g_lsock = NO_SOCK;
        return;
    }
ready:
    for (;;) {
        struct sin a; int alen = sizeof a;
        for (i = 0; i < PEND && g_pd[i].used; i++) ;
        if (i == PEND) break;                             /* (the rest wait in XNet's backlog) */
        memset(&a, 0, sizeof a);
        ns = w_accept(g_lsock, &a, &alen);
        if (ns == NO_SOCK) break;
        nonblock(ns);
        sockbuf(ns, g_cfg_window, 0);
        memset(&g_pd[i], 0, sizeof g_pd[i]);
        g_pd[i].used = 1; g_pd[i].s = ns; g_pd[i].t = now; g_pd[i].addr = a.addr;
    }
    pend_tick(now);
    share_windows(now);
    for (i = 0; i < SLOTS; i++) if (g_sl[i].state || g_sl[i].run != JOB_IDLE) slot_tick(&g_sl[i], now);
    if ((ns = host_sending())) {
        int pct;
        u32 eta;
        dl_summary(&pct, &eta);
        status(ns == 1 ? "Sending map " : "Sending map to players ", pct, 0);
        host_line(ns, pct, eta);
    }
}

/* ================================================================ client ================ */

enum { CL_IDLE, CL_START, CL_CONNECT, CL_REPLY, CL_RECV, CL_OK, CL_WAIT, CL_LEFT, CL_ASK, CL_NOROOM };
static struct {
    int state, s;
    char key[32];
    u32 t, t0, retry_at, total, got_now, beat, beat_got, beat_ticks, rtt;
    struct rep r; int rlen;
    struct req q; int qsent;      /* the request, and how much of it has gone (XNet may take it in pieces) */
    int xs[MAXS], xsent[MAXS];    /* extra connections still opening (NO_SOCK: none); how much of their JOIN has gone */
    u32 xt;                       /* when they started */
    int tries, misses, quarter;   /* failed downloads; tries in a row that couldn't reach the host */
    int loading;                  /* logged that Halo went ahead with the map */
    int mismatch;                 /* the host shares maps differently (another test build) */
    int notyet;                   /* times the host said it isn't hosting this map (yet): it was changing maps */
    int visiting;                 /* the host's copy is an older version than this console's: it goes in the other maps
                                     folder, for games with this host, and this console's own stays as it is */
    u32 mine;                     /* this console's copy's version ("Version:"; 0 none) */
    u32 rate, rate_t, rate_done;  /* recent download speed (bytes/s), for the time left */
    u32 need_mb, free_mb;         /* CL_NOROOM: what the download needs and what's free (MB) */
} g_cl = { .s = NO_SOCK, .xs = { NO_SOCK, NO_SOCK, NO_SOCK } };
/* not enough room for the host's map: say so on the lobby's map and countdown lines (and in the heartbeats, so the
   host and the others can show who). It stays like this until the map changes; nothing is downloaded. */
static void cl_noroom_text(void) {
    status("No room for this map", -1, 0);
    L_start("No room for this map: needs "); L_u(g_cl.need_mb); L_a(" MB, "); L_u(g_cl.free_mb); L_a(" MB free");
}

/* the download's data phase, on the worker: every file at once, from every connection */
struct cst {
    volatile int on;              /* set by the main thread once s and buf are ready */
    int s, dead, got;             /* dead: an extra connection the host closed before sending anything */
    u8 *buf; int blen; u32 need;  /* the block coming in: header, data, CRC (need = its whole size, 0 = header first) */
    int ack; u32 t_ack;           /* bytes read since the last one-byte ack, and when that was */
};
static struct {
    volatile int run, stop, result, err;
    int orphan;                   /* the download was abandoned while the worker had it */
    char key[32];
    struct cst st[MAXS];
    HANDLE h[MAX_FILES]; u32 hpos[MAX_FILES];   /* every file of the download, open for writing (0 = none) */
    u32 size[MAX_FILES]; int nfiles;
    volatile u32 done;            /* bytes on disk, counting the part a resume started from */
    volatile u32 left;            /* blocks still to come */
    u32 first, prefix, nblk;      /* map blocks: the first this download fetches, all here below `prefix`, how many */
    u8 small[MAX_FILES];          /* blocks of the small files that are here (bits) */
    u32 t_last, unflushed;
    u32 pass_max;                 /* most bytes read from one connection in one pass (the window) */
    volatile u32 gap_max, acks;   /* longest wait between data arrivals (ms); one-byte acks sent */
    volatile u32 wr_max, fl_max;  /* longest WriteFile of a block; longest flush + resume.hmp update (ms) */
    char resume_path[128];
    struct resume rs;
} g_job;
static u8 g_have[MAX_MAP_SIZE / CHUNK / 8]; /* map blocks that are here (bits) */
#define HAVE(b) (g_have[(b) >> 3] & (1 << ((b) & 7)))

/* the partial download written since the console was switched on: its file is consistent (Halo's
   file cache never lost anything), so it can be continued from where its blocks stop being complete.
   One left over from before a restart is only trusted up to the last flush. */
static char g_sess_key[32];
static u32 g_sess_part;
static void cl_close(void) {
    int k;
    for (k = 0; k < MAXS; k++) if (g_cl.xs[k] != NO_SOCK) { w_close(g_cl.xs[k]); g_cl.xs[k] = NO_SOCK; }
    if (g_cl.s != NO_SOCK) w_close(g_cl.s);
    g_cl.s = NO_SOCK;
}
static void put_resume(const char *path, const struct resume *rs);
/* close everything the download had (the worker has let go): its connections, buffers and files */
static void job_release(void) {
    int k;
    if (g_job.h[0] && g_job.left) {                       /* a partial map: remember how far it's complete */
        u32 b = g_job.prefix * CHUNK;
        scpy(g_sess_key, g_job.key); g_sess_part = b < g_job.size[0] ? b : g_job.size[0];
        if (g_job.rs.magic == RES_MAGIC && g_sess_part > g_job.rs.good) {   /* (build 20) on disk now, so it still resumes */
            FlushFileBuffers(g_job.h[0]);                 /* from here after the host has had other maps up, or a restart */
            g_job.rs.good = g_sess_part;
            put_resume(g_job.resume_path, &g_job.rs);
        }
    }
    for (k = 0; k < MAXS; k++) {
        struct cst *t = &g_job.st[k];
        if (t->on) w_close(t->s);
        if (t->buf) LocalFree(t->buf);
        memset(t, 0, sizeof *t);
    }
    for (k = 0; k < MAX_FILES; k++) { if (g_job.h[k]) CloseHandle(g_job.h[k]); g_job.h[k] = 0; }
    g_job.run = JOB_IDLE;
}

/* the temporary folder a download goes into: "<folder>.hmp_dl" (the map saved as .part until it's done) */
static char *tmp_dir(char *buf) { return mpath(buf, g_dl_root, g_cl.key + 1, ".hmp_dl", 0); }
static char *tmp_file(char *buf, const char *name, int part) {
    mpath(buf, g_dl_root, g_cl.key + 1, ".hmp_dl", name);
    if (part) scpy(buf + slen(buf), ".part");
    return buf;
}

/* couldn't check or fetch the map: use the local copy if there is one, otherwise keep trying.
   A failed download keeps its verified blocks for the next try. With no copy, stop after a while
   (about 40 s without reaching the host, or 10 failed downloads) and leave the game the way Halo
   does when it loses the host (back to the menu with its network error), instead of every player
   sitting on "loading level" or this console hitting the disc error. */
#define GIVE_UP_MISSES 8
#define GIVE_UP_TRIES 10
static void cl_give_up(u32 now, int failed, const char *why, int err) {
    cl_close();
    if (why) lg("client: %s: %s (error %d)", failed ? "download failed" : "couldn't check the map with the host", why, err);
    if (failed) { g_cl.tries++; g_cl.misses = 0; } else g_cl.misses++;
    if ((!failed || g_cl.tries >= 5) && hmp_find_key(g_cl.key) >= 0) {
        lg("client: using the copy already in %s\\%s", k_root_name[hmp_root_c(hmp_find_key(g_cl.key)) & 1], g_cl.key + 1);
        g_cl.state = CL_OK; g_status_on = 0; return;
    }
    if (g_cl.misses >= GIVE_UP_MISSES || g_cl.tries >= GIVE_UP_TRIES) {
        lg("client: giving up on %s; leaving the game", g_cl.key);
        leave_game(g_cl.mismatch ? "The host shares maps differently (another build of OpenCE). Update one of the Xboxes." :
            "Couldn't get the host's map.");
        g_cl.state = CL_LEFT; g_status_on = 0; return;
    }
    g_cl.state = CL_WAIT;
    g_cl.retry_at = now + (failed ? 3000 : 5000);
    if (failed) { status("Map download failed - retrying", -1, 0); L_start("Map download failed - retrying"); }
    else if (g_cl.mismatch) { status("Map missing: other host build", -1, 0); L_start("Map missing: the host is on another build"); }
    else { status("Map missing: ", -1, g_cl.key + 1); L_start("Map missing: "); L_a(g_cl.key + 1); L_a(" - trying again"); }
}

/* the partial download can't be continued (a write failed): throw it away so the next try starts clean */
static void cl_discard(u32 now, const char *why) {
    cl_close();
    g_sess_key[0] = 0;
    lg("client: throwing away the partial download of %s", g_cl.key);
    rm_folder(tmp_dir(g_p1));
    cl_give_up(now, 1, why, 0);
}

static void put_resume(const char *path, const struct resume *rs) {
    u32 put = 0;
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0x80, 0);
    if (h != INVALID_HANDLE) { WriteFile(h, rs, sizeof *rs, &put, 0); FlushFileBuffers(h); CloseHandle(h); }
}

/* a folder's info .txt: its first .txt file (name into `path`, which holds the folder). 1 = found */
static int txt_in(char *path) {
    char *e = path + slen(path);
    int found = 0;
    HANDLE h;
    scpy(e, "\\*.*");
    h = FindFirstFileA(path, g_fd);
    *e = 0;
    if (h == INVALID_HANDLE) return 0;
    do {
        char *nm = g_fd + 0x2c;
        if (!(*(u32 *)g_fd & 0x10) && ends_with(nm, ".txt") && slen(nm) < 40) { *e = '\\'; scpy(e + 1, nm); found = 1; }
    } while (!found && FindNextFileA(h, g_fd));
    CloseHandle(h);
    return found;
}
/* a "Genres:" line in b (n bytes): where it starts and how long it is (with its line end). 1 = there is one */
static int genres_line(const u8 *b, u32 n, u32 *at, u32 *len) {
    u32 k = 0, e;
    if (n >= 3 && b[0] == 0xef && b[1] == 0xbb && b[2] == 0xbf) k = 3;
    while (k < n) {
        u32 s = k, j = k;
        for (e = k; e < n && b[e] != '\n'; e++) ;
        while (j < e && (b[j] == ' ' || b[j] == '\t')) j++;
        if (j + 7 <= e && lower(b[j]) == 'g' && lower(b[j + 1]) == 'e' && lower(b[j + 2]) == 'n' && lower(b[j + 3]) == 'r' &&
            lower(b[j + 4]) == 'e' && lower(b[j + 5]) == 's' && b[j + 6] == ':') { *at = s; *len = (e < n ? e + 1 : e) - s; return 1; }
        k = e + 1;
    }
    return 0;
}
/* build 20: the genres are this console's own choice, so a download that replaces a map keeps the old info .txt's
   "Genres:" line (the host's Gametypes / Players lines, name and description come with the map) */
static void keep_genres(const char *old_dir, const char *new_dir) {
    char op[160], np[160];
    u8 *a, *b;
    u32 na = 0, nb = 0, at, len, at2, len2, put = 0;
    HANDLE h;
    scpy(op, old_dir); scpy(np, new_dir);
    if (!txt_in(op) || !txt_in(np) || !(a = (u8 *)LocalAlloc(0, 8192))) return;
    b = a + 4096;
    if ((h = CreateFileA(op, GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0)) != INVALID_HANDLE) { if (!ReadFile(h, a, 4096, &na, 0)) na = 0; CloseHandle(h); }
    if ((h = CreateFileA(np, GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0)) != INVALID_HANDLE) { if (!ReadFile(h, b, 4000, &nb, 0)) nb = 0; CloseHandle(h); }
    if (na < 4096 && nb && nb < 4000 && !(na >= 2 && a[0] == 0xff) && !(b[0] == 0xff) && genres_line(a, na, &at, &len)) {
        if (genres_line(b, nb, &at2, &len2)) {             /* the host's own Genres line steps aside */
            u32 k;
            for (k = at2; k + len2 < nb; k++) b[k] = b[k + len2];
            nb -= len2;
        }
        if (nb && b[nb - 1] != '\n') { b[nb++] = '\r'; b[nb++] = '\n'; }
        if (nb + len + 2 <= 4096) {
            u32 k;
            for (k = 0; k < len; k++) b[nb++] = a[at + k];
            if (b[nb - 1] != '\n') { b[nb++] = '\r'; b[nb++] = '\n'; }
            if ((h = CreateFileA(np, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0x80, 0)) != INVALID_HANDLE) {
                WriteFile(h, b, nb, &put, 0); FlushFileBuffers(h); CloseHandle(h);
                if (put == nb) lg("client: kept this Xbox's own Genres line in the new info .txt");
            }
        }
    }
    LocalFree(a);
}
static void cl_finish(u32 now) {
    char *fin = mpath(g_p1, g_dl_root, g_cl.key + 1, 0, 0), *tmp = tmp_dir(g_p2);
    char *old = mpath(g_p3, g_dl_root, g_cl.key + 1, ".hmp_old", 0);
    char part[128], whole[128];
    int i, had;
    u32 ms = now - g_cl.t0;
    cl_close();
    lg("client: received %u bytes in %u ms (%u KB/s)", g_cl.got_now, ms, ms ? g_cl.got_now / ms : 0);
    DeleteFileA(tmp_file(part, "resume.hmp", 0));
    if (!MoveFileA(tmp_file(part, g_cl.r.f[0].name, 1), tmp_file(whole, g_cl.r.f[0].name, 0))) {
        cl_give_up(now, 1, "couldn't rename the finished map", 0); return;
    }
    if (pack_dir(whole, g_dl_root, g_cl.key + 1, "series.bin")) {   /* a pack's logo goes in the pack's folder, unless it has one */
        if (MoveFileA(tmp_file(part, "series.bin", 0), whole)) lg("client: added the pack's series.bin");
        else DeleteFileA(part);
    }
    rm_folder(old);
    had = MoveFileA(fin, old);                   /* an older copy (or a folder without a map) steps aside */
    if (!MoveFileA(tmp, fin)) { if (had) MoveFileA(old, fin); cl_give_up(now, 1, "couldn't move the map into place", 0); return; }
    if (had) {
        keep_genres(old, fin);
        if (!g_cl.visiting && g_cl.r.map_version <= g_cl.mine) {
            /* the versions don't say the host's is newer (none, or the same): the replaced copy is kept as
               <folder>.hmp_prev (one per map; the map list skips it), in case it was the newer one */
            char *prev = mpath(g_p2, g_dl_root, g_cl.key + 1, ".hmp_prev", 0);
            rm_folder(prev);
            if (MoveFileA(old, prev)) lg("client: kept the replaced copy as %s.hmp_prev", g_cl.key + 1);
            else rm_folder(old);
        } else rm_folder(old);
    }
    if (cache_files_forget_cached_map(g_cl.key)) lg("client: dropped the old build from Halo's map cache");
    hmp_maps_scan();
    if (!hmp_use_root_c(hmp_find_key(g_cl.key), g_dl_root)) lg("client: the new copy isn't in the map list!");   /* (this one, whatever the clock says) */
    g_sess_key[0] = 0;
    lg("client: installed %s\\%s%s", k_root_name[g_dl_root], g_cl.key + 1, had ? " (replacing the older copy)" : "");
    g_cl.state = CL_OK;
    g_status_on = 0;
}

static int cl_check_reply(void) {
    struct rep *r = &g_cl.r;
    u32 k, total = 0;
    if (r->magic != REP_MAGIC || r->version != VERSION || r->nfiles < 1 || r->nfiles > MAX_FILES) return 0;
    for (k = 0; k < r->nfiles; k++) {
        char *n = r->f[k].name;
        n[NAME_LEN - 1] = 0;
        if (!safe_name(n, NAME_LEN - 6) || ieq(n, "resume.hmp") == 0) return 0;
        if (k == 0 ? !ends_with(n, ".map") || r->f[k].size > MAX_MAP_SIZE || r->f[k].size < 0x800
                   : ends_with(n, ".txt") ? r->f[k].size > MAX_TXT_SIZE
                   : ieq(n, "series.bin") == 0 ? r->f[k].size <= 16 || r->f[k].size > SERIES_MAX
                   : ieq(n, "preview.bin") != 0 || r->f[k].size != PREVIEW_SIZE) return 0;
        total += r->f[k].size;
    }
    if (r->start >= r->f[0].size || r->start % CHUNK) return 0;
    g_cl.total = total;
    return 1;
}

/* leftovers from other downloads: an interrupted swap (".hmp_old") goes; other maps' partial downloads are kept, the
   newest KEEP_PARTIALS of them in each folder (build 20: the host may go back to a map it switched away from; up to
   build 19 only this download's was kept). `sub` = a pack's folder to look in (0 = maps itself); `ours` = this
   download's folder name there ("" = none); keep = how many other partials to keep (0: none, to make room) */
#define KEEP_PARTIALS 2
static int g_tidy_keep = KEEP_PARTIALS;
static void tidy_dir(const char *sub, const char *ours) {
    char names[8][40], rel[72];
    u32 tl[8], th[8];
    int n = 0, k, j, our = slen(ours), kept;
    HANDLE h;
    if (sub) mpath(g_p3, g_dl_root, sub, 0, "*.*"); else scpy(scpy(g_p3, k_root[g_dl_root]), "*.*");
    h = FindFirstFileA(g_p3, g_fd);
    if (h == INVALID_HANDLE) return;
    do {
        char *nm = g_fd + 0x2c;
        int len = slen(nm);
        if (!(*(u32 *)g_fd & 0x10) || len >= 40 || n == 8) continue;
        if (!ends_with(nm, ".hmp_dl") && !ends_with(nm, ".hmp_old")) continue;
        if (our && ends_with(nm, ".hmp_dl") && len == our + 7) {
            char c = nm[our]; nm[our] = 0; k = ieq(nm, ours); nm[our] = c;
            if (!k) continue;                    /* ours: keep it for resuming */
        }
        tl[n] = *(u32 *)(g_fd + 0x14); th[n] = *(u32 *)(g_fd + 0x18);   /* (last written) */
        scpy(names[n++], nm);
    } while (FindNextFileA(h, g_fd));
    CloseHandle(h);
    for (k = 1; k < n; k++)                       /* newest first */
        for (j = k; j > 0 && (th[j] > th[j - 1] || (th[j] == th[j - 1] && tl[j] > tl[j - 1])); j--) {
            char t[40]; u32 a = tl[j], b = th[j];
            scpy(t, names[j]); scpy(names[j], names[j - 1]); scpy(names[j - 1], t);
            tl[j] = tl[j - 1]; th[j] = th[j - 1]; tl[j - 1] = a; th[j - 1] = b;
        }
    for (k = kept = 0; k < n; k++)
        if (ends_with(names[k], ".hmp_dl") && kept < g_tidy_keep) { kept++; names[k][0] = 0; }
    for (k = 0; k < n; k++) {
        char *p = rel;
        if (!names[k][0]) continue;
        if (sub) { p = scpy(p, sub); *p++ = '/'; }
        scpy(p, names[k]);
        lg("client: removing leftover %s\\%s", k_root_name[g_dl_root], rel);
        rm_folder(mpath(g_p3, g_dl_root, rel, 0, 0));
    }
}
static void cl_tidy(void) {
    char pk[32];
    const char *k = g_cl.key + 1;
    int i = 0;
    for (; k[i] && k[i] != '/' && i < 31; i++) pk[i] = k[i];
    pk[i] = 0;
    if (k[i] == '/') { tidy_dir(0, ""); tidy_dir(pk, k + i + 1); }
    else tidy_dir(0, k);
}

/* the most connections this console will open for one download */
static u32 my_streams(void) { return g_cfg_streams ? g_cfg_streams : g_cfg_auto ? MAXS : 1; }

/* a new connection to the host's map sharing port (connecting in the background) */
static int cl_socket(int *e) {
    struct sin a;
    int s = w_socket(2, 1, 0), r;
    *e = 0;
    if (s == NO_SOCK) { *e = w_error(); return NO_SOCK; }
    nonblock(s);
    sockbuf(s, g_cfg_window, rcv_window());
    memset(&a, 0, sizeof a); a.family = 2; a.port = PORT_BE; a.addr = g_host_addr;
    r = w_connect(s, &a, sizeof a);
    if (r && (*e = w_error()) != WOULDBLOCK && *e != INPROGRESS) { w_close(s); return NO_SOCK; }
    *e = 0;
    return s;
}
/* has a connecting socket connected? 1 yes, 0 not yet, -1 refused or failed */
static int connected(int s) {
    struct fds w, e; struct tv z = {0, 0};
    w.count = e.count = 1; w.fd[0] = e.fd[0] = s;
    if (w_select(0, 0, &w, &e, &z) < 0 || e.count) return -1;
    return w.count != 0;
}

static void cl_start(u32 now) {
    int e;
    if (now < g_cl.retry_at || g_job.run != JOB_IDLE) return;
    read_config();
    cl_tidy();
    g_cl.mismatch = 0;
    if (!safe_key(g_cl.key + 1)) { cl_give_up(now, 0, "unusable folder name", 0); return; }
    if (!g_host_addr) { cl_give_up(now, 0, "no host address", 0); return; }
    lg("client: checking %s with the host at %a (window up to %u KB)", g_cl.key, g_host_addr, rcv_window());
    g_cl.s = cl_socket(&e);
    if (g_cl.s == NO_SOCK) { cl_give_up(now, 0, e == 10055 || !e ? "socket failed" : "connect failed", e); return; }
    g_cl.state = CL_CONNECT; g_cl.t = g_cl.t0 = now;
}

static void cl_ask(u32 now);
static void cl_connect(u32 now) {
    struct req q;
    struct resume rs;
    HANDLE h;
    u32 got = 0;
    int i, c = connected(g_cl.s);
    if (c < 0) { cl_give_up(now, 0, "connection refused (is the host on this XBE?)", 0); return; }
    if (!c) { if (now - g_cl.t > 3000) cl_give_up(now, 0, "no answer within 3 s (is the host on this XBE?)", 0); return; }
    g_cl.rtt = now - g_cl.t0;
    lg("client: connected in %u ms", g_cl.rtt);
    memset(&q, 0, sizeof q);
    q.magic = REQ_MAGIC; q.version = VERSION;
    scpy(q.key, g_cl.key);
    q.flags = my_streams();
    q.machine = (u32)my_machine();
    q.rtt = host_rtt();
    if (!q.rtt) q.rtt = g_cl.rtt;
    i = hmp_find_key(g_cl.key);
    if (i < 0 || !hmp_map_path_c(i, -1, g_p1) || !map_identity(g_p1, &q.have_size, &q.have_chk)) q.have_size = q.have_chk = 0;
    h = CreateFileA(tmp_file(g_p2, "resume.hmp", 0), GENERIC_READ, 1, 0, OPEN_EXISTING, 0, 0);
    if (h != INVALID_HANDLE) {                    /* a partial download of this map: offer to continue it */
        if (!ReadFile(h, &rs, sizeof rs, &got, 0)) got = 0;
        CloseHandle(h);
        rs.name[NAME_LEN - 1] = 0;
        if (got == sizeof rs && rs.magic == RES_MAGIC && safe_name(rs.name, NAME_LEN - 6)) {
            u32 have = file_size(tmp_file(g_p2, rs.name, 1)), use;
            int fresh = have != 0xffffffff && ieq(g_sess_key, g_cl.key) == 0 && have >= g_sess_part;
            use = fresh ? g_sess_part : have < rs.good ? have : rs.good;   /* from before a restart: only what was flushed */
            if (have != 0xffffffff && use >= CHUNK) {
                q.resume_size = rs.size; q.resume_chk = rs.chk; q.resume_offset = use - use % CHUNK;
                if (fresh) lg("client: %u bytes of an earlier download are here; asking to resume", q.resume_offset);
                else lg("client: %u bytes of an earlier download are here (%u of them flushed to disk); asking to resume",
                        have, q.resume_offset);
            }
        }
    }
    g_cl.q = q; g_cl.qsent = 0; g_cl.state = CL_ASK; g_cl.t = now;
    cl_ask(now);
}

/* send the request; a send that would block or only takes part of it carries on next frame */
static void cl_ask(u32 now) {
    while (g_cl.qsent < (int)sizeof g_cl.q) {
        int n = w_send(g_cl.s, (char *)&g_cl.q + g_cl.qsent, (int)sizeof g_cl.q - g_cl.qsent, 0), e;
        if (n < 0) {
            if ((e = w_error()) != WOULDBLOCK) { cl_give_up(now, 0, "couldn't send the request", e); return; }
            if (now - g_cl.t > 3000) cl_give_up(now, 0, "couldn't send the request within 3 s", 0);
            return;
        }
        g_cl.qsent += n;
    }
    g_cl.state = CL_REPLY; g_cl.rlen = 0; g_cl.t = now;
}

/* open every file of the download for the worker (the map continues a partial download when the host agreed
   to resume); 0 = couldn't */
static int cl_open_files(void) {
    u32 k, b;
    for (k = 0; k < g_cl.r.nfiles; k++) {
        struct rfile *f = &g_cl.r.f[k];
        int resume = k == 0 && g_cl.r.start;
        HANDLE h = CreateFileA(tmp_file(g_p2, f->name, k == 0), GENERIC_WRITE, 0, 0, resume ? OPEN_EXISTING : CREATE_ALWAYS, 0x80, 0);
        if (h == INVALID_HANDLE) return 0;
        g_job.h[k] = h; g_job.hpos[k] = 0; g_job.size[k] = f->size;
        b = (f->size + CHUNK - 1) / CHUNK;
        g_job.left += k ? b : b - g_cl.r.start / CHUNK;
    }
    g_job.nfiles = (int)g_cl.r.nfiles;
    g_job.first = g_job.prefix = g_cl.r.start / CHUNK;
    g_job.nblk = (g_cl.r.f[0].size + CHUNK - 1) / CHUNK;
    memset(&g_job.rs, 0, sizeof g_job.rs);
    g_job.rs.magic = RES_MAGIC; g_job.rs.size = g_cl.r.f[0].size; g_job.rs.chk = g_cl.r.map_chk; scpy(g_job.rs.name, g_cl.r.f[0].name);
    tmp_file(g_job.resume_path, "resume.hmp", 0);
    if (!g_cl.r.start) put_resume(g_job.resume_path, &g_job.rs);   /* which build the partial map belongs to */
    return 1;
}
/* give connection k to the worker */
static int cl_stream(int k, int s) {
    struct cst *t = &g_job.st[k];
    memset(t, 0, sizeof *t);
    t->buf = (u8 *)LocalAlloc(0, CHUNK + 8);
    if (!t->buf) return 0;
    t->s = s; t->t_ack = GetTickCount() - 1;
    BARRIER();
    t->on = 1;
    return 1;
}

static void cl_reply(u32 now) {
    int n = w_recv(g_cl.s, (char *)&g_cl.r + g_cl.rlen, sizeof g_cl.r - g_cl.rlen, 0), e, k;
    u32 streams;
    if (n == 0) { cl_give_up(now, 0, "host hung up", 0); return; }
    if (n < 0) {
        if ((e = w_error()) != WOULDBLOCK) { cl_give_up(now, 0, "receive failed", e); return; }
        if (now - g_cl.t > 5000) cl_give_up(now, 0, "no reply within 5 s", 0);
        return;
    }
    g_cl.rlen += n; g_cl.t = now;
    if (g_cl.rlen < (int)sizeof g_cl.r) return;
    if (g_cl.r.magic == REP_MAGIC && g_cl.r.status == REP_SAME && hmp_find_key(g_cl.key) >= 0) {
        lg("client: same build as the host");
        cl_close(); rm_folder(tmp_dir(g_p1)); g_cl.state = CL_OK; g_status_on = 0; return;
    }
    if (g_cl.r.magic == REP_MAGIC && g_cl.r.version != VERSION) {
        g_cl.mismatch = 1;
        cl_give_up(now, 0, "the host is on a test build that shares maps differently", (int)g_cl.r.version);
        return;
    }
    if (g_cl.r.magic == REP_MAGIC && g_cl.r.status == REP_UNAVAILABLE && g_cl.r.streams == WHY_CHECKING && g_cl.notyet < 60) {
        g_cl.notyet++;                                /* the host's copy is still loading on the host (up to 5 minutes) */
        if (g_cl.notyet == 1) lg("client: the host is checking %s (loading its own copy) before it sends it; asking every 5 s", g_cl.key);
        status("Map: the host is checking it", -1, 0);
        cl_close();
        g_cl.state = CL_WAIT; g_cl.retry_at = now + 5000;
        return;
    }
    if (g_cl.r.magic == REP_MAGIC && g_cl.r.status == REP_UNAVAILABLE && g_cl.r.streams == WHY_NOT_HOSTED && g_cl.notyet < 12) {
        g_cl.notyet++;                                /* the host is between maps (or this console hasn't seen its newest pick yet) */
        lg("client: the host isn't hosting %s right now (changing maps?); asking again in 5 s", g_cl.key);
        cl_close();
        g_cl.state = CL_WAIT; g_cl.retry_at = now + 5000;
        return;
    }
    if (g_cl.r.magic == REP_MAGIC && g_cl.r.status == REP_UNAVAILABLE) { cl_give_up(now, 0, "the host won't send this map", 0); return; }
    if (g_cl.r.status != REP_SENDING || !cl_check_reply()) { cl_give_up(now, 0, "bad reply from the host", 0); return; }
    k = hmp_find_key(g_cl.key);
    if (k >= 0 && hmp_both_c(k)) {                      /* in both maps folders: maybe the other copy is the host's build */
        u32 sz, ck;
        int r2 = !hmp_root_c(k);
        if (hmp_map_path_c(k, r2, g_p1) && map_identity(g_p1, &sz, &ck) && sz == g_cl.r.f[0].size && ck == g_cl.r.map_chk &&
            hmp_use_root_c(k, r2)) {
            lg("client: the copy in %s is the host's build; using that one", k_root_name[r2]);
            cl_close(); g_cl.state = CL_OK; g_status_on = 0; return;
        }
    }
    /* the host's copy is an older version than this console's: never in place of it. It goes in the other maps folder
       (where an older one for another host may be), chosen for this game only; the newer copy is used again once
       the game is left (map_share_update). Asked again from there, so a partial download there resumes */
    if (k >= 0 && !g_cl.visiting) {
        int other = !hmp_root_c(k);
        g_cl.mine = hmp_maps_version(k);
        if (g_cl.r.map_version < g_cl.mine) {
            if (other == 1 || g_writable) {
                g_cl.visiting = 1;
                lg("client: the host's %s is version %u, older than this console's %u: getting it for this game only, in %s",
                   g_cl.key, g_cl.r.map_version, g_cl.mine, k_root_name[other]);
                if (g_dl_root != other) {
                    g_dl_root = other;
                    cl_close(); g_cl.state = CL_START; return;
                }
            } else
                lg("client: the host's %s is version %u, older than this console's %u, and there's no other maps folder to "
                   "keep it in: it replaces this console's", g_cl.key, g_cl.r.map_version, g_cl.mine);
        }
    }
    {                                                 /* room for it? Nothing old is deleted to make room. */
        u32 need = g_cl.total - g_cl.r.start + g_cl.r.nfiles * CHUNK + (256u << 10), have = free_bytes(g_dl_root);
        if (have < need) {                            /* other maps' partial downloads go first (build 20) */
            g_tidy_keep = 0; cl_tidy(); g_tidy_keep = KEEP_PARTIALS;
            have = free_bytes(g_dl_root);
        }
        if (have < need) {
            g_cl.need_mb = (need + (1u << 20) - 1) >> 20; g_cl.free_mb = have >> 20;
            lg("client: no room for %s in %s: it needs %u MB, %u MB free; not downloading", g_cl.key, k_root_name[g_dl_root],
               g_cl.need_mb, g_cl.free_mb);
            cl_close();
            g_cl.state = CL_NOROOM;
            cl_noroom_text();
            return;
        }
    }
    streams = g_cl.r.streams < 1 ? 1 : g_cl.r.streams > my_streams() ? my_streams() : g_cl.r.streams;
    {
        const char *over = streams == 3 ? " over 3 connections" : streams == 2 ? " over 2 connections" : "";
        if (g_cl.r.start) lg("client: downloading %u files, %u bytes, resuming at %u%s", g_cl.r.nfiles, g_cl.total, g_cl.r.start, over);
        else lg("client: downloading %u files, %u bytes%s", g_cl.r.nfiles, g_cl.total, over);
    }
    if (!g_cl.r.start) rm_folder(tmp_dir(g_p1));  /* not resuming: start clean */
    if (g_dl_root) CreateDirectoryA("T:\\maps", 0);
    if (pack_dir(g_p1, g_dl_root, g_cl.key + 1, 0)) CreateDirectoryA(g_p1, 0);   /* a pack's map: the pack's folder first */
    CreateDirectoryA(tmp_dir(g_p1), 0);
    memset(&g_job, 0, sizeof g_job);
    memset(g_have, 0, sizeof g_have);
    scpy(g_job.key, g_cl.key);
    g_job.done = g_cl.r.start;
    g_cl.got_now = 0; g_cl.t0 = g_cl.beat = now; g_cl.beat_got = 0; g_cl.beat_ticks = g_ticks; g_cl.rate = g_cl.rate_t = 0;
    g_cl.quarter = g_cl.total >= 4 ? (int)(g_job.done / (g_cl.total / 4)) : 0;   /* resuming: skip the quarters already done */
    if (g_cl.quarter > 3) g_cl.quarter = 3;
    if (!cl_open_files()) { job_release(); cl_discard(now, "couldn't create the file"); return; }
    if (!cl_stream(0, g_cl.s)) { job_release(); cl_give_up(now, 1, "out of memory", 0); return; }
    g_cl.s = NO_SOCK;
    for (k = 1; k < (int)streams; k++) {              /* the extra connections open while the first one downloads */
        g_cl.xsent[k] = 0;
        g_cl.xs[k] = cl_socket(&e);
        if (g_cl.xs[k] == NO_SOCK) lg("client: couldn't open connection %d (error %d); carrying on with fewer", k + 1, e);
    }
    g_cl.xt = now;
    g_job.t_last = now; g_job.pass_max = rcv_window() << 10;
    g_cl.state = CL_RECV;
    status("Downloading map ", g_cl.total >= 100 ? (int)(g_job.done / (g_cl.total / 100)) : 0, 0);
    L_start("Downloading map: "); L_pct(g_cl.total >= 100 ? (int)(g_job.done / (g_cl.total / 100)) : 0, 0);
    start_worker();
    BARRIER();
    g_job.run = JOB_RUN;
}

/* main thread: the extra connections that are still opening: once connected, say which download they're
   for (JOIN) and hand them to the worker */
static void cl_extra(u32 now) {
    int k, c, n;
    for (k = 1; k < MAXS; k++) {
        int s = g_cl.xs[k];
        struct req j;
        if (s == NO_SOCK) continue;
        c = g_cl.xsent[k] ? 1 : connected(s);
        if (c > 0) {                                      /* connected: the JOIN (XNet may take it in pieces) */
            memset(&j, 0, sizeof j);
            j.magic = JOIN_MAGIC; j.version = VERSION; scpy(j.key, g_cl.key); j.token = g_cl.r.token; j.stream = (u32)k;
            n = w_send(s, (char *)&j + g_cl.xsent[k], (int)sizeof j - g_cl.xsent[k], 0);
            if (n > 0) g_cl.xsent[k] += n;
            else if (n < 0 && w_error() != WOULDBLOCK) c = -1;
            if (g_cl.xsent[k] == (int)sizeof j) {
                g_cl.xs[k] = NO_SOCK;
                if (cl_stream(k, s)) { lg("client: connection %d open", k + 1); continue; }
                c = -1;
            }
        }
        if (c >= 0 && now - g_cl.xt <= 3000 && g_cl.xs[k] != NO_SOCK) continue;
        g_cl.xs[k] = NO_SOCK;
        lg("client: connection %d %s; carrying on with fewer", k + 1, c < 0 ? "failed" : "didn't open within 3 s");
        w_close(s);
    }
}

/* take the download back (the worker has let go) */
static void cl_take(void) {
    int k;
    for (k = 1; k < MAXS; k++) if (g_cl.xs[k] != NO_SOCK) { w_close(g_cl.xs[k]); g_cl.xs[k] = NO_SOCK; }
    job_release();
}

/* worker: a whole block arrived on connection t: check it, write it where it goes. 0 = fine, else R_* */
static int cl_block(struct cst *t) {
    u32 where = *(u32 *)t->buf, f = where >> 28, off = where & 0x0fffffff, b = off / CHUNK, m = t->need - 8, put = 0, t1, t2;
    u8 *data = t->buf + 4;
    if (~crc_upd(~0u, t->buf, m + 4) != *(u32 *)(data + m)) return R_CRC;
    if (f ? g_job.small[f] & (1 << b) : HAVE(b)) return 0;   /* (already here) */
    t1 = GetTickCount();
    if (g_job.hpos[f] != off) SetFilePointer(g_job.h[f], (int)off, 0, 0);
    if (!WriteFile(g_job.h[f], data, m, &put, 0) || put != m) return R_WRITE;
    g_job.hpos[f] = off + m;
    t2 = GetTickCount();
    if (t2 - t1 > g_job.wr_max) g_job.wr_max = t2 - t1;
    if (f) g_job.small[f] |= (u8)(1 << b);
    else {
        g_have[b >> 3] |= (u8)(1 << (b & 7));
        while (g_job.prefix < g_job.nblk && HAVE(g_job.prefix)) g_job.prefix++;
        if (++g_job.unflushed >= FLUSH_BLOCKS) {         /* every 4 MB: flush, and note how much of it is complete */
            u32 good = g_job.prefix * CHUNK;
            FlushFileBuffers(g_job.h[0]); g_job.unflushed = 0;
            g_job.rs.good = good < g_job.size[0] ? good : g_job.size[0];
            put_resume(g_job.resume_path, &g_job.rs);
            t1 = GetTickCount();
            if (t1 - t2 > g_job.fl_max) g_job.fl_max = t1 - t2;
        }
    }
    g_job.done += m; g_job.left--;
    return 0;
}

/* worker: receive, check and write blocks from every connection. 1 = something moved. */
static int cl_pump(u32 now) {
    int k, n, e = 0, r, got = 0;
    if (g_job.stop) { JOB_END(g_job, R_STOPPED); }
    for (k = 0; k < MAXS; k++) {
        struct cst *t = &g_job.st[k];
        u32 rd = 0;
        if (!t->on || t->dead) continue;
        while (rd < g_job.pass_max) {                     /* at most one window a pass */
            u32 room = (t->blen < 4 ? 4 : t->need) - (u32)t->blen;
            if (room > g_job.pass_max - rd) room = g_job.pass_max - rd;
            n = w_recv(t->s, t->buf + t->blen, (int)room, 0);
            if (n < 0 && (e = w_error()) == WOULDBLOCK) break;
            if (n <= 0) {
                if (k && !t->got) { t->dead = 1; break; }  /* the host turned an extra connection away: fine */
                if (n == 0) { JOB_END(g_job, R_HUNGUP); }
                g_job.err = e; JOB_END(g_job, R_RECV);
            }
            if (now - g_job.t_last > g_job.gap_max) g_job.gap_max = now - g_job.t_last;
            g_job.t_last = now; got = 1; t->got = 1;
            t->blen += n; t->ack += n; rd += (u32)n;
            if (t->blen == 4 && !t->need) {               /* a block's header: which file, where */
                u32 where = *(u32 *)t->buf, f = where >> 28, off = where & 0x0fffffff, len;
                if ((int)f >= g_job.nfiles || off % CHUNK || off >= g_job.size[f] || (!f && off < g_job.first * CHUNK)) {
                    JOB_END(g_job, R_CRC);
                }
                len = g_job.size[f] - off;
                t->need = 8 + (len < CHUNK ? len : CHUNK);
            } else if (t->need && t->blen == (int)t->need) {
                if ((r = cl_block(t))) { JOB_END(g_job, r); }
                t->blen = 0; t->need = 0;
                if (!g_job.left) {                        /* everything's here: carry the last acks, then finish */
                    int j;
                    for (j = 0; j < MAXS; j++) if (g_job.st[j].on && !g_job.st[j].dead) { char a = 'a'; w_send(g_job.st[j].s, &a, 1, 0); }
                    JOB_END(g_job, R_FINISHED);
                }
            }
        }
        /* a one-byte ack carries TCP's ack and the reopened window now, instead of when XNet's delayed-ack timer
           fires: after every 4 KB read, or when 2 ms have passed since the last one. At most one a millisecond
           a connection (Kai tunnels every packet, so no more than that). */
        if (t->ack && now != t->t_ack && (t->ack >= 4096 || now - t->t_ack >= 2)) {
            char a = 'a';
            if (w_send(t->s, &a, 1, 0) == 1) { t->ack = 0; t->t_ack = now; g_job.acks++; }
        }
    }
    if (!got && now - g_job.t_last > STALL_MS) { JOB_END(g_job, R_STALL); }
    return got;
}

/* main thread, every frame of the data phase: progress, extra connections, and whatever the worker handed back */
static void cl_recv(u32 now) {
    int res;
    u32 done = g_job.done;
    g_cl.got_now = done - g_cl.r.start;
    if (g_job.run == JOB_RUN) {
        cl_extra(now);
        if (g_cl.total >= 4 && done >= (g_cl.total / 4) * (g_cl.quarter + 1) && g_cl.quarter < 3) {
            u32 ms = now - g_cl.t0;
            g_cl.quarter++;
            lg("client: %u%% (%u bytes) at %u KB/s", g_cl.quarter * 25, done, ms ? g_cl.got_now / ms : 0);
        }
        if (now - g_cl.beat >= BEAT_MS) {
            lg("client: %u of %u bytes, %u KB/s, %u frames; thread %u passes (%u busy, %u woken, longest gap %u ms), "
               "longest wait for data %u ms, %u acks; disk %u ms, flush %u ms", done, g_cl.total,
               (g_cl.got_now - g_cl.beat_got) / (now - g_cl.beat), g_ticks - g_cl.beat_ticks, g_ws.passes, g_ws.busy,
               g_ws.woke, g_ws.gap, g_job.gap_max, g_job.acks, g_job.wr_max, g_job.fl_max);
            g_ws.passes = g_ws.busy = g_ws.woke = g_ws.gap = 0; g_job.gap_max = g_job.acks = g_job.wr_max = g_job.fl_max = 0;
            g_cl.beat = now; g_cl.beat_got = g_cl.got_now; g_cl.beat_ticks = g_ticks;
        }
        {
            int pct = g_cl.total >= 100 ? (int)(done / (g_cl.total / 100)) : 0;
            rate_upd(&g_cl.rate, &g_cl.rate_t, &g_cl.rate_done, done, now);
            status("Downloading map ", pct, 0);
            L_start("Downloading map: "); L_pct(pct, g_cl.rate ? (g_cl.total - done) / g_cl.rate + 1 : 0);
        }
        return;
    }
    if (g_job.run != JOB_DONE) return;
    BARRIER();
    res = g_job.result;
    if (res == R_CRC) lg("client: a block of %s arrived damaged (the first one missing is at %u)", g_cl.r.f[0].name, g_job.prefix * CHUNK);
    cl_take();
    switch (res) {
    case R_FINISHED: cl_finish(now); return;
    case R_CRC: cl_give_up(now, 1, "checksum mismatch", 0); return;
    case R_WRITE: cl_discard(now, "couldn't write (disk full?)"); return;
    case R_HUNGUP: cl_give_up(now, 1, "host hung up", 0); return;
    case R_RECV: cl_give_up(now, 1, "receive failed", g_job.err); return;
    case R_STALL: cl_give_up(now, 1, "no data for 10 s", 0); return;
    default: cl_give_up(now, 1, "stopped", 0); return;
    }
}

static void client_tick(u32 now) {
    switch (g_cl.state) {
    case CL_START: cl_start(now); break;
    case CL_CONNECT: cl_connect(now); break;
    case CL_ASK: cl_ask(now); break;
    case CL_REPLY: cl_reply(now); break;
    case CL_RECV: cl_recv(now); break;
    case CL_WAIT: if (now >= g_cl.retry_at) g_cl.state = CL_START; break;
    }
}

static void client_reset(void) {
    int k;
    if (g_cl.state == CL_RECV) lg("client: download of %s stopped (map changed or left the lobby); kept for resuming", g_cl.key);
    if (g_job.run == JOB_RUN) { g_job.stop = 1; g_job.orphan = 1; }   /* the worker lets go within a few ms */
    else if (g_job.run == JOB_DONE) { BARRIER(); cl_take(); }
    cl_close();
    memset(&g_cl, 0, sizeof g_cl);
    g_cl.s = NO_SOCK;
    for (k = 0; k < MAXS; k++) g_cl.xs[k] = NO_SOCK;
    g_status_on = 0;
}

/* ================================================================= hooks ================ */

/* transfers abandoned while the worker had them: free them once it lets go */
static void reap(void) {
    int i;
    if (g_job.orphan && g_job.run == JOB_DONE) { BARRIER(); g_job.orphan = 0; job_release(); }
    for (i = 0; i < SLOTS; i++)
        if (g_sl[i].orphan && g_sl[i].run == JOB_DONE) { BARRIER(); g_sl[i].run = JOB_IDLE; g_sl[i].orphan = 0; slot_close(&g_sl[i], 0, 0); }
}

/* one pass of the worker over every transfer it has: 2 = data moved, 1 = waiting on the network, 0 = none.
   (The test calls this directly.) */
static int hmp_worker_step(void) {
    u32 now = GetTickCount();
    int i, did = 0, any = 0;
    if (now - g_flush_t >= 250) { g_flush_t = now; hmp_stats_flush(); }   /* a few writes a second at most */
    if (g_cfg_limit) {                                    /* upload limit: 1 KB/s ~ 1 byte per ms, at most 1 s of credit */
        u32 dt = now - g_tok_t;
        if (dt > 1000) dt = 1000;
        if (dt) {
            int t = g_tokens + (int)(dt * g_cfg_limit);
            g_tokens = t > (int)(g_cfg_limit * 1000) ? (int)(g_cfg_limit * 1000) : t;
            g_tok_t = now;
        }
    }
    if (g_job.run == JOB_RUN) { any = 1; did |= cl_pump(now); }
    for (i = 0; i < SLOTS; i++) if (g_sl[i].run == JOB_RUN) { any = 1; did |= slot_pump(&g_sl[i], now); }
    if (any) {
        if (g_ws.last && now - g_ws.last > g_ws.gap) g_ws.gap = now - g_ws.last;
        g_ws.passes++; g_ws.busy += did != 0;
        g_ws.last = now;
    } else g_ws.last = 0;
    return did ? 2 : any;
}
/* the worker's sockets for select(): Winsock fd_sets (a count, then the sockets) */
static struct { u32 count; int fd[SLOTS * MAXS + MAXS]; } g_rfd, g_wfd;
#define WAIT_US 2000
/* worker: wait until one of its sockets has data, an ack or a hang-up, or a full window has room
   again; at most 2 ms. 1 = something is ready, 0 = timed out (or nothing to wait on), -1 = select failed.
   (The test calls this directly.) */
static int hmp_worker_wait(void) {
    struct tv t = {0, WAIT_US};
    int i, k, r;
    g_rfd.count = g_wfd.count = 0;
    if (g_job.run == JOB_RUN)
        for (k = 0; k < MAXS; k++) if (g_job.st[k].on && !g_job.st[k].dead) g_rfd.fd[g_rfd.count++] = g_job.st[k].s;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        if (c->run != JOB_RUN) continue;
        for (k = 0; k < MAXS; k++) {
            struct hst *t = &c->st[k];
            if (!t->on || t->closed) continue;
            g_rfd.fd[g_rfd.count++] = t->s;           /* the client's acks, or its hang-up */
            if (t->wait && !g_cfg_limit) g_wfd.fd[g_wfd.count++] = t->s;   /* send() found the window full */
        }                                             /* (with an upload limit, room isn't what it waits for) */
    }
    if (!g_rfd.count) return 0;
    r = w_select(0, &g_rfd, g_wfd.count ? &g_wfd : 0, 0, &t);
    if (r > 0) g_ws.woke++;
    return r < 0 ? -1 : r > 0;
}
static unsigned long __stdcall worker_main(void *unused) {
    int r, ready = 0, idle = 0;
    (void)unused;
    for (;;) {
        r = hmp_worker_step();
        if (!r) { Sleep(10); ready = idle = 0; continue; }    /* no transfer: look again in 10 ms */
        if (r == 2 || !ready) idle = 0;
        else if (++idle >= 2) { Sleep(1); idle = 0; }        /* select said ready twice and nothing moved: don't spin */
        ready = hmp_worker_wait();
        if (ready < 0) { Sleep(1); ready = 0; }              /* select failed: build 8's 1 ms sleep */
    }
    return 0;
}


/* ================================================================ OpenCE's side ================ */

/* the host's status line: how many are downloading, the lowest percentage, the longest time left */
static int dl_summary(int *pct, u32 *eta) {
    int i, n = 0;
    *pct = 100; *eta = 0;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        int p;
        u32 e;
        if (c->state != 2 || c->orphan) continue;
        n++;
        /* (of the whole map, as the downloading console counts it: a resumed download's bytes already there count) */
        p = c->start + c->total >= 100 ? (int)((c->start + c->sent) / ((c->start + c->total) / 100)) : 0;
        if (p < *pct) *pct = p;
        e = c->rate ? (c->total - c->sent) / c->rate + 1 : 0;
        if (e > *eta) *eta = e;
    }
    if (!n) *pct = 0;
    return n;
}
static void host_line(int ns, int pct, u32 eta) {
    if (ns == 1) L_start("Sending map: ");
    else { L_start("Sending map to "); L_u((u32)ns); L_a(": "); }
    L_pct(pct, eta);
}

static void leave_game(char const *why) {
    wchar_t text[160];
    int k;
    for (k = 0; why[k] && k < 159; k++) text[k] = (unsigned char)why[k];
    text[k] = 0;
    display_error_text_when_main_menu_loaded(text);
    network_game_client_all_local_players_have_quit();
}
static int my_machine(void) {
    struct network_game_client *client = global_network_game_client_get();
    return client ? network_game_client_get_machine_index(client) : -1;
}
static int host_in_game(void) {
    struct network_game_server *server = global_network_game_server_get();
    return server && network_game_server_playing(server);
}

/* the host: when each machine last had a transfer running (a client installs and precaches the map just after) */
#define DOWNLOAD_GRACE_MS 15000
static u32 g_machine_seen[16];
static u8 g_machine_active[16];

unsigned long network_game_server_machine_address(long machine_index);
/* the machine that joined from this address (either byte order), -1 for none */
static int machine_by_address(u32 addr) {
    int m;
    for (m = 0; addr && m < 16; m++) {
        u32 a = (u32)network_game_server_machine_address(m);
        u32 swapped = (a >> 24) | ((a >> 8) & 0xff00) | ((a & 0xff00) << 8) | (a << 24);
        if (a && (a == addr || swapped == addr)) return m;
    }
    return -1;
}
static void machines_tick(u32 now) {
    int i, m;
    for (m = 0; m < 16; m++) g_machine_active[m] = 0;
    /* (test build 24) a console that has connected to ask for the map, not asked yet: as one downloading, for a while
       (its players held meanwhile, it isn't turned away for having none) */
    for (i = 0; now && i < PEND; i++)
        if (g_pd[i].used && (m = machine_by_address(g_pd[i].addr)) >= 0) g_machine_seen[m] = now;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        /* a console that asked before it had joined (its machine not known yet, as a console joining a lobby whose
           map it hasn't checks the map first): its machine by the address it joined from, once it has. Without
           it the host took it for one with the map: the game started with it, and its map not all there, its load
           failed (the damaged disc error) */
        if (c->state == 2 && !c->orphan && c->machine < 0 && c->addr && (m = machine_by_address(c->addr)) >= 0) {
            c->machine = m;
            lg("host: the console downloading the map is machine #%d (it asked before it had joined)", m);
        }
        if (c->state == 2 && !c->orphan && c->machine >= 0 && c->machine < 16) {
            g_machine_active[c->machine] = 1;
            g_machine_seen[c->machine] = now ? now : 1;
        }
    }
}

/* ---------- map_share.h */

/* the lobby's machine names (ui_widget_game_data_input_functions.c): the host, how far along each machine it is sending
   the map to is (of the whole map, as dl_summary counts it); -1: not downloading */
int map_share_machine_percent(long machine_index) {
    int i, p;
    if (machine_index < 0 || machine_index >= 16) return -1;
    for (i = 0; i < SLOTS; i++) {
        struct slot *c = &g_sl[i];
        if (c->state != 2 || c->orphan || c->machine != machine_index) continue;
        p = c->start + c->total >= 100 ? (int)((c->start + c->sent) / ((c->start + c->total) / 100)) : 0;
        return p < 0 ? 0 : p > 100 ? 100 : p;
    }
    return -1;
}
/* a client: how far along its own download is (0 while it asks the host); -1: none */
int map_share_download_percent(void) {
    int p;
    if (g_cl.state == CL_RECV) {
        p = g_cl.total >= 100 ? (int)(g_job.done / (g_cl.total / 100)) : 0;
        return p < 0 ? 0 : p > 100 ? 100 : p;
    }
    if (g_cl.state == CL_START || g_cl.state == CL_CONNECT || g_cl.state == CL_REPLY || g_cl.state == CL_ASK) return 0;
    return -1;
}

boolean map_share_machine_needs_map(long machine_index) {
    return machine_index >= 0 && machine_index < 16 && g_machine_active[machine_index];
}
boolean map_share_machine_downloading(long machine_index) {
    return machine_index >= 0 && machine_index < 16 &&
        (g_machine_active[machine_index] || (g_machine_seen[machine_index] && GetTickCount() - g_machine_seen[machine_index] < DOWNLOAD_GRACE_MS));
}

/* a client: the host's game is on a folder map this console hasn't (or of another build): get it from the host */
boolean map_share_download_begin(char const *level_name, unsigned long version, char const *message) {
    struct network_game_client *client = global_network_game_client_get();
    struct transport_address address;
    (void)message;
    if (!level_name || level_name[0] != '+' || slen(level_name) > 31 || global_network_game_server_get() || !client) return FALSE;
    if (g_cl.state && !ieq(level_name, g_cl.key) && g_cl.state != CL_LEFT) return TRUE;   /* (already on it) */
    if (g_cl.state) client_reset();
    memset(&address, 0, sizeof address);
    network_game_client_get_remote_server_address(client, &address);
    g_host_addr = ((address.address.long_words[0] & 0xff) << 24) | ((address.address.long_words[0] & 0xff00) << 8) |
        ((address.address.long_words[0] >> 8) & 0xff00) | (address.address.long_words[0] >> 24);   /* (network order) */
    scpy(g_cl.key, level_name);
    g_cl.state = CL_START;
    maps_writable();
    g_dl_root = g_writable ? 0 : 1;               /* (an older version's download went to the other folder) */
    lg("client: the host's map %s (version %x) isn't here; asking the host for it", level_name, (u32)version);
    status("Map: asking the host", -1, 0);
    L_start("Asking the host for its map");
    return TRUE;
}

static int client_busy(void) { return g_cl.state != CL_IDLE && g_cl.state != CL_OK; }

boolean map_share_downloading(char const *level_name) {
    return client_busy() && level_name && !ieq(level_name, g_cl.key);
}

boolean map_share_holds_players(void) {
    struct network_game_client *client = global_network_game_client_get();
    /* a client whose host hasn't named the game's map yet: whether this console has it isn't known, and a game in
       progress would start it at once. Asking, failed or out of room: the players wait (or leave). Downloading: only
       for a game already under way (a game in the lobby starts without this console, which joins it once it has
       the map) */
    if (client && !global_network_game_server_get() && !network_game_client_get_game(client)->map.name[0]) return TRUE;
    switch (g_cl.state) {
    case CL_START: case CL_CONNECT: case CL_ASK: case CL_REPLY: case CL_WAIT: case CL_LEFT: case CL_NOROOM: return TRUE;
    case CL_RECV: return g_cl.r.in_game != 0;
    default: return FALSE;
    }
}

boolean map_share_handles_message(unsigned short const *message, unsigned short size) { (void)message; (void)size; return FALSE; }
void map_share_handle_message(long machine_index, unsigned short const *message, unsigned short size, boolean from_stream) { }
void map_share_level_name_choose(char *level_name, long size) { (void)level_name; (void)size; }

/* the version of a folder map: its .map's size and the checksum in its header (Halo Map Porter's CRC); 0 = none */
unsigned long hmp_share_map_version(char const *level_name) {
    char path[300];
    long i = hmp_maps_find(level_name);
    u32 size = 0, chk = 0;
    if (i == NONE || !hmp_maps_map_path(i, NONE, path, sizeof path) || !map_identity(path, &size, &chk)) return 0;
    return (size * 2654435761u ^ chk) | 1;
}

/* a game left: the copies map sharing chose for it (an older version from a host, say) give way to the newer ones
   again, out of Halo's map cache too, so the menus and this console's own games use the newer */
static int g_in_session;
static void unpinned(char const *key) { cache_files_forget_cached_map(key); }
static void session_tick(int on) {
    long n;
    if (on) { g_in_session = 1; return; }
    if (!g_in_session) return;
    g_in_session = 0;
    if ((n = hmp_maps_unpin_all(unpinned)) != 0) {
        hmp_maps_scan();
        lg("game left: %d map%s back to this console's newest copy", (int)n, n == 1 ? "" : "s");
    }
}

/* every frame, menus and matches (main.c) */
static char g_loaded_map[32];
void map_share_update(void) {
    u32 now = GetTickCount();
    struct network_game_server *server = global_network_game_server_get();
    struct network_game_client *client = global_network_game_client_get();
    char const *map = network_game_current_map_name(), *loaded = cache_files_loaded_map_name();
    int priority;
    g_ticks++;
    crc_init();
    if (!g_worker && !g_worker_failed) { maps_writable(); start_worker(); }
    priority = game_in_progress() ? 0 : WORKER_PRIORITY;
    if (g_worker && priority != g_worker_priority) { SetThreadPriority(g_worker, priority); g_worker_priority = priority; }
    reap();
    /* a game map loaded: the menus' memory goes back */
    if (loaded && strncmp(g_loaded_map, loaded, sizeof g_loaded_map - 1)) {
        strncpy(g_loaded_map, loaded, sizeof g_loaded_map - 1);
        if (ieq(loaded, "ui")) hmp_menu_memory_release();
    }
    /* the host shares the folder map it's hosting */
    if (server && map && map[0] == '+') host_tick(now);
    else if (g_lsock != NO_SOCK || host_sending()) host_stop();
    machines_tick(server ? now : 0);
    /* a client: a download for a map the host has moved away from (or a game left) stops */
    if (g_cl.state) {
        if (!client || server || !map || ieq(map, g_cl.key)) client_reset();
        else client_tick(now);
    }
    session_tick(server || client);
    hmp_maps_check_tick();
    {
        void oxbox_ftp_frame(int menus);   /* (oxbox_sdk.c: the developers' FTP server, in the menus only) */
        oxbox_ftp_frame(loaded && ieq(loaded, "ui") == 0);
    }
    if (!server && !g_cl.state) g_status_on = 0;
    if (server && !host_sending()) g_status_on = 0;
    hmp_stats_tick();
    hmp_browse_tick();
    hmp_game_tick();
}

/* the status line in the lobby (the downloading console's and the host's) */
void map_share_render(void) {
    long font;
    real_argb_color color = { 1.0f, 0.156863f, 0.588235f, 1.0f };
    rectangle2d bounds;
    /* (in the lobby's countdown bar, right-aligned after "Game Start in: 0:17"; drawn as plain text: the icon drawing
       takes '%' for the start of a button's name and drew each piece aligned on its own) */
    /* (the lobby's alone: not the gametype and map choices opened from it) */
    if (!g_status_on || !g_label[0] || game_engine || !ui_widget_lobby_showing() ||
        (font = tag_loaded('font', "ui\\large_ui")) == NONE) return;
    draw_string_set_draw_mode(font, NONE, 1, 0, &color);
    draw_string_set_tab_stops(NULL, 0);
    bounds.y0 = 82; bounds.x0 = 250; bounds.y1 = 106; bounds.x1 = 565;
    rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, (wchar_t const *)g_label);
}

boolean hmp_worker_running(void) { return g_worker != 0; }

/* oxbox: whether the lobby's countdown bar shows a download's status (bots.c draws its label there otherwise) */
boolean map_share_status_showing(void) { return g_status_on && g_label[0]; }

void hmp_log(char const *format, ...) {
    char line[300];
    va_list arguments;
    int halo_linux_vsnprintf(char *buffer, size_t count, char const *format, va_list arguments);
    va_start(arguments, format);
    halo_linux_vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    platform_log("hmp: %s", line);
}

#endif /* HALO_XBOX_TARGET */
