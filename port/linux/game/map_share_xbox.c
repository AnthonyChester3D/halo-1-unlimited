/*
MAP_SHARE_XBOX.C

A dedicated server's folder maps and their sharing with Halo 1 Unlimited's Xboxes (the native builds; the Xbox's own
are hmp_maps.c and hmp_share.c, which this follows).

Folder maps. The maps folder's folders, as an Xbox has them:

	maps\
		bloodgulch.map ...       the stock maps
		Precipice\               a map: a folder with a .map (any name), an info .txt, a preview.bin
		CEX\                     a pack: a folder without a .map; each folder in it with a .map is a map
			series.bin           optional: the pack's logo
			Abyss\ ...

A folder map's level name is "+" and its folder ("+Precipice"), or "+pack/folder" for a pack's map, found by that key
on every machine (cache_files.c: hmp_map_file_path, hmp_maps_find). Its version, which the game's settings carry, is
its .map's size and the checksum Halo Map Porter writes in its header (hmp_share_map_version), as the Xbox works it
out: an Xbox plays its own copy of that build, or downloads the host's. The server's rotation names one by its key
(server_rotation.c), and its web page lists them (server_maps_each), titled by their info .txt's first line.

Sharing. An Xbox joining a game on a folder map it hasn't (or has another build of) asks the host for it over TCP port
5152 (internet play's tunnel carries it as it carries the game's connections). This is the host's side of Halo
Universal's protocol (hmp_share.c, version 6): the request names the key, the build the Xbox has and any partial
download; the reply says the same build, or sends the folder's files (the .map, its info .txt, preview.bin, a pack's
series.bin, showcase.txt), small ones first, then the map from where a partial download left off, in 16 KB blocks
(where it goes, the data, its CRC32) over up to 3 connections. While an Xbox downloads, the host doesn't wait for it
(map_share.c's map_share_machine_needs_map and map_share_machine_downloading ask map_share_xbox_machine_*): it joins
the game in progress once it has the map. The data moves on the game's thread, every frame (map_share_update).

Lines go to the log ("hmp: ..."). Custom Edition maps go to PCs by OpenCE's own map sharing (map_share.c, port 5153).
*/

#if !defined(HALO_XBOX_TARGET)

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "bungie_net/network/transport.h"
#include "bungie_net/network/transport_endpoint_winsock.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"

#include "../src/server_log.h" /* port: port/linux/src/server_log.c */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* ---------- constants */

#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

#define MAXIMUM_FOLDER_MAPS 2048
#define MAXIMUM_FOLDER_NAME 30               /* a folder's name (a pack's and its map's together) */
#define FOLDER_MAPS_RESCAN_MS 5000           /* a key not found looks again, this long after the last look */
#define PREVIEW_SIZE 16384
#define SERIES_MAXIMUM_SIZE (16 + 64 * 64 * 4)

/* the protocol (hmp_share.c's) */
#define SHARE_PORT 5152
#define REQ_MAGIC 0x71504d48                 /* "HMPq" */
#define REP_MAGIC 0x72504d48                 /* "HMPr" */
#define JOIN_MAGIC 0x6a504d48                /* "HMPj": another connection for a download under way */
#define SHARE_VERSION 6
#define REP_SAME 0
#define REP_SENDING 1
#define REP_UNAVAILABLE 2
#define WHY_NOT_HOSTED 1                     /* REP_UNAVAILABLE's reason (in .streams): not the map hosted now */
#define NAME_LEN 44
#define MAX_FILES 5                          /* the .map, info .txt, preview.bin, series.bin, showcase.txt */
#define CHUNK 16384                          /* a block; each is followed by its CRC32 */
#define PIECE 4096                           /* a block goes out in pieces this size */
#define MAXS 3                               /* connections one download may use */
#define SLOTS 8                              /* downloads at once */
#define PEND 12                              /* connections that haven't said what they want yet */
#define SEND_BUFFER_KB 64                    /* each connection's send buffer */
#define SENDS_PER_FRAME 64                   /* sends per connection per frame (64 x 4 KB: ~7 MB/s at 30 frames) */
#define STALL_MS 10000                       /* no progress for this long: the transfer is dropped */
#define BEAT_MS 5000                         /* a progress line in the log while a transfer runs */
#define DOWNLOAD_GRACE_MS 15000              /* a machine just done: still taken for downloading (it loads the map) */

/* ---------- structures */

struct folder_map
{
	char key[40];                            /* "+folder", "+pack/folder" */
	char map_file[48];
	char info_file[48];                      /* "" for none */
	char picture;                            /* a preview.bin */
	char series;                             /* its pack has a series.bin */
};

/* the request (or JOIN_MAGIC: another connection, quoting the token) and the reply, as the Xbox sends and reads them
(little-endian, 32 bits each) */
struct share_request
{
	unsigned int magic, version;
	char key[32];
	unsigned int have_size, have_chk, resume_size, resume_chk, resume_offset, flags, rtt, token, stream, machine;
};
struct share_file
{
	char name[NAME_LEN];
	unsigned int size;
};
struct share_reply
{
	unsigned int magic, version, status, nfiles, map_chk, start, streams, token, in_game, map_version;
	struct share_file files[MAX_FILES];
};

typedef char share_request_size_assert[sizeof(struct share_request) == 80 ? 1 : -1];
typedef char share_reply_size_assert[sizeof(struct share_reply) == 280 ? 1 : -1];

/* one connection of a transfer */
struct share_stream
{
	boolean on, closed;                      /* closed: the Xbox hung this one up (everything sent) */
	SOCKET socket;
	FILE *file;
	int open_file;                           /* the file open (its index + 1, 0: none) */
	int block_file, phase;                   /* the block going out: its file; 0 none, 1 header and data, 2 its CRC */
	unsigned long offset, length, done;
	unsigned int crc;
	byte buffer[PIECE + 16];
	int position, size;                      /* what of buffer is still to go */
};

struct share_slot
{
	int state;                               /* 0 free, 2 sending, 3 everything sent: waiting for the hang-ups */
	unsigned long started, heard, sent, total, start, token, address, beat, beat_sent;
	int want;                                /* connections the Xbox was asked to open */
	int machine;                             /* its machine (NONE: not known yet) */
	int nfiles, cursor_file;
	unsigned long cursor_offset;
	char names[MAX_FILES][NAME_LEN];
	unsigned long sizes[MAX_FILES];
	char key[40];
	struct share_stream streams[MAXS];
};

struct share_pending
{
	boolean used;
	SOCKET socket;
	unsigned long time, address;
	int length;
	struct share_request request;
};

/* ---------- globals */

static struct
{
	struct folder_map *maps;
	long count;
	boolean scanned;
	unsigned long scanned_at;
} folder_maps;

static struct
{
	SOCKET listener;
	unsigned long listen_tried_at;
	struct share_slot slots[SLOTS];
	struct share_pending pending[PEND];
	unsigned int crc_table[256];
	unsigned long machine_seen[16];
	byte machine_active[16];
} share = { INVALID_SOCKET };

/* ---------- helpers */

void platform_log(char const *format, ...);
int halo_server_mode(void);
unsigned long system_milliseconds(void);
char const *network_game_current_map_name(void);
/* linked_maps.c */
void linked_maps_update(void);
char const *linked_map_key(void);

static void share_log(
	char const *format,
	...)
{
	char line[300];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	/* (a dedicated server's: in its log, with the web console's lines, not on the standard error alone) */
	if (halo_server_mode())
		server_log(_server_log_info, "maps", "%s", line);
	else
		platform_log("hmp: %s", line);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* compares case-insensitively: 0 when the same */
static int ieq(char const *a, char const *b)
{
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
	return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static boolean ends_with(char const *s, char const *ending)
{
	size_t a = strlen(s), b = strlen(ending);

	return a > b && !ieq(s + a - b, ending);
}

static unsigned long now_ms(void)
{
	return system_milliseconds();
}

/* ---------- folder maps */

static boolean is_folder(WIN32_FIND_DATAA const *find)
{
	return (find->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && find->cFileName[0] != '.' &&
		!strstr(find->cFileName, ".hmp_");
}

static boolean is_series(WIN32_FIND_DATAA const *find)
{
	return !(find->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !ieq(find->cFileName, "series.bin") &&
		!find->nFileSizeHigh && find->nFileSizeLow > 16 && find->nFileSizeLow <= SERIES_MAXIMUM_SIZE;
}

/* "d:\maps\<folder>\<file>" for a key ("+folder"; "+pack/folder": "d:\maps\pack\folder\<file>") */
static void key_path(char *path, int size, char const *key, char const *file)
{
	char folder[64];
	int index;

	for (index = 0; key[index + 1] && index < (int)sizeof(folder) - 1; index++)
		folder[index] = key[index + 1] == '/' ? '\\' : key[index + 1];
	folder[index] = 0;
	snprintf(path, (size_t)size, "d:\\maps\\%s\\%s", folder, file);
}

/* a folder with a .map: kept, its files named (TRUE) */
static boolean folder_map_add(char const *top, char const *sub)
{
	struct folder_map *map;
	WIN32_FIND_DATAA find;
	char path[300];
	HANDLE handle;
	int info_rank = 0;

	if (folder_maps.count >= MAXIMUM_FOLDER_MAPS)
		return FALSE;
	map = &folder_maps.maps[folder_maps.count];
	memset(map, 0, sizeof(*map));
	snprintf(map->key, sizeof(map->key), sub ? "+%s/%s" : "+%s", top, sub);
	key_path(path, sizeof(path), map->key, "*.*");
	handle = FindFirstFileA(path, &find);
	if (handle == INVALID_HANDLE_VALUE)
		return FALSE;
	do
	{
		char const *name = find.cFileName;

		if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || strlen(name) > 43)
			continue;
		if (!map->map_file[0] && ends_with(name, ".map"))
			snprintf(map->map_file, sizeof(map->map_file), "%s", name);
		else if (ends_with(name, ".txt"))
		{
			/* (the info .txt: info.txt first, then any other, but never the showcase's, the bots' file or a pack's
			order.txt) */
			int rank = !ieq(name, "showcase.txt") || !ieq(name, "bots.txt") || !ieq(name, "order.txt") ? 0 :
				!ieq(name, "info.txt") ? 2 : 1;

			if (rank > info_rank)
			{
				snprintf(map->info_file, sizeof(map->info_file), "%s", name);
				info_rank = rank;
			}
		}
		else if (!ieq(name, "preview.bin") && !find.nFileSizeHigh && find.nFileSizeLow == PREVIEW_SIZE)
			map->picture = 1;
	}
	while (FindNextFileA(handle, &find));
	CloseHandle(handle);
	if (!map->map_file[0])
		return FALSE;
	folder_maps.count++;
	return TRUE;
}

static int folder_map_compare(void const *a, void const *b)
{
	return ieq(((struct folder_map const *)a)->key, ((struct folder_map const *)b)->key);
}

/* the maps folder's folder maps, looked at again */
void hmp_maps_scan(void)
{
	WIN32_FIND_DATAA find, inner;
	HANDLE handle;

	if (!folder_maps.maps)
	{
		folder_maps.maps = calloc(MAXIMUM_FOLDER_MAPS, sizeof(*folder_maps.maps));
		if (!folder_maps.maps)
			return;
	}
	folder_maps.count = 0;
	folder_maps.scanned = TRUE;
	folder_maps.scanned_at = now_ms();
	handle = FindFirstFileA("d:\\maps\\*.*", &find);
	if (handle == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char path[300];
		HANDLE inner_handle;
		long first;
		boolean series = FALSE;
		long index;

		if (!is_folder(&find) || strlen(find.cFileName) > MAXIMUM_FOLDER_NAME)
			continue;
		if (folder_map_add(find.cFileName, NULL))
			continue;
		/* (no .map: a pack) */
		first = folder_maps.count;
		snprintf(path, sizeof(path), "d:\\maps\\%s\\*.*", find.cFileName);
		inner_handle = FindFirstFileA(path, &inner);
		if (inner_handle == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			if (is_series(&inner))
				series = TRUE;
			else if (is_folder(&inner) && strlen(find.cFileName) + 1 + strlen(inner.cFileName) <= MAXIMUM_FOLDER_NAME)
				folder_map_add(find.cFileName, inner.cFileName);
		}
		while (FindNextFileA(inner_handle, &inner));
		CloseHandle(inner_handle);
		for (index = first; index < folder_maps.count; index++)
			folder_maps.maps[index].series = (char)series;
	}
	while (FindNextFileA(handle, &find));
	CloseHandle(handle);
	qsort(folder_maps.maps, (size_t)folder_maps.count, sizeof(*folder_maps.maps), folder_map_compare);
}

/* a level name's folder map (a "+key"), or NONE (looked for again a while after the last look: a map just copied in) */
long hmp_maps_find(char const *level_name)
{
	long index;
	int pass;

	if (!level_name || level_name[0] != '+')
		return NONE;
	for (pass = 0; pass < 2; pass++)
	{
		if (!folder_maps.scanned)
			hmp_maps_scan();
		for (index = 0; index < folder_maps.count; index++)
		{
			if (!ieq(folder_maps.maps[index].key, level_name))
				return index;
		}
		if (pass || now_ms() - folder_maps.scanned_at < FOLDER_MAPS_RESCAN_MS)
			break;
		hmp_maps_scan();
	}
	return NONE;
}

/* the .map of a folder map (a "+key"): its path, TRUE; FALSE for any other level name (a folder map not here: a path no
file has) */
boolean hmp_map_file_path(char const *level_name, char *path, long size)
{
	long index;
	char buffer[300];

	if (!level_name || level_name[0] != '+')
		return FALSE;
	index = hmp_maps_find(level_name);
	if (index == NONE)
		buffer[0] = 0;
	else
		key_path(buffer, sizeof(buffer), folder_maps.maps[index].key, folder_maps.maps[index].map_file);
	if (!buffer[0] || (long)strlen(buffer) >= size)
		snprintf(buffer, sizeof(buffer), "d:\\maps\\+missing+.map");
	snprintf(path, (size_t)size, "%s", buffer);
	return TRUE;
}

/* a folder map's identity: its .map's size and the checksum in its header (Halo Map Porter's CRC); FALSE for none */
static boolean map_identity(char const *path, unsigned long *size, unsigned long *checksum)
{
	byte header[0x68];
	unsigned long got = 0;
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	*size = GetFileSize(file, NULL);
	if (!ReadFile(file, header, sizeof(header), &got, NULL))
		got = 0;
	CloseHandle(file);
	if (got != sizeof(header))
		return FALSE;
	*checksum = (unsigned long)header[0x64] | (unsigned long)header[0x65] << 8 | (unsigned long)header[0x66] << 16 |
		(unsigned long)header[0x67] << 24;
	return TRUE;
}

/* the version of a folder map, as the Xbox works it out (hmp_share.c): 0 for none */
unsigned long hmp_share_map_version(char const *level_name)
{
	char path[300];
	unsigned long size = 0, checksum = 0;

	if (!hmp_map_file_path(level_name, path, sizeof(path)) || !map_identity(path, &size, &checksum))
		return 0;
	return (((size * 2654435761UL) ^ checksum) & 0xFFFFFFFFUL) | 1;
}

/* a folder map's title: its info .txt's first line (UTF-8, UTF-16 with a BOM, or ANSI), else its folder */
boolean folder_map_title(char const *key, char *title, int size)
{
	long index = hmp_maps_find(key);
	char path[300];
	byte text[600];
	unsigned long got = 0;
	int length = 0;
	int position = 0;
	HANDLE file;

	if (index == NONE || size < 2)
		return FALSE;
	snprintf(title, (size_t)size, "%s", strrchr(key, '/') ? strrchr(key, '/') + 1 : key + 1);
	if (!folder_maps.maps[index].info_file[0])
		return TRUE;
	key_path(path, sizeof(path), key, folder_maps.maps[index].info_file);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return TRUE;
	if (!ReadFile(file, text, sizeof(text), &got, NULL))
		got = 0;
	CloseHandle(file);
	if (got >= 2 && text[0] == 0xFF && text[1] == 0xFE)
	{
		/* (UTF-16: the characters ASCII has, '?' for the rest) */
		for (position = 2; position + 1 < (int)got && length < size - 1; position += 2)
		{
			unsigned int c = text[position] | text[position + 1] << 8;

			if (c == '\r' || c == '\n')
				break;
			title[length++] = (char)(c >= 32 && c < 127 ? c : '?');
		}
	}
	else
	{
		if (got >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF)
			position = 3;
		for (; position < (int)got && length < size - 1; position++)
		{
			byte c = text[position];

			if (c == '\r' || c == '\n')
				break;
			if (c >= 0x80)
			{
				/* (a UTF-8 sequence, or an ANSI letter: one '?') */
				while (position + 1 < (int)got && (text[position + 1] & 0xC0) == 0x80)
					position++;
				c = '?';
			}
			else if (c < 32)
				continue;
			title[length++] = (char)c;
		}
	}
	/* ("|n" breaks a line in Halo's text: a space here) */
	title[length] = 0;
	{
		char *mark;

		while ((mark = strstr(title, "|n")) != NULL)
		{
			mark[0] = ' ';
			memmove(mark + 1, mark + 2, strlen(mark + 2) + 1);
		}
	}
	if (!length)
		snprintf(title, (size_t)size, "%s", strrchr(key, '/') ? strrchr(key, '/') + 1 : key + 1);
	return TRUE;
}

/* folder map i's key, NULL past them */
char const *folder_map_key(long map_index)
{
	return map_index >= 0 && map_index < folder_maps.count ? folder_maps.maps[map_index].key : NULL;
}

/* "4-16 players" / "Players: 4-16" / "8 players" (hmp_browse.c's): its most, 0 for none */
static short players_in(char const *s)
{
	long a = 0, b = 0, k;

	while (*s == ' ')
		s++;
	for (k = 0; s[k] >= '0' && s[k] <= '9' && k < 3; k++)
		a = a * 10 + s[k] - '0';
	if (!k)
		return 0;
	s += k;
	while (*s == ' ')
		s++;
	if (*s == '-' || *s == 'x' || (s[0] == 't' && s[1] == 'o'))
	{
		s += *s == 't' ? 2 : 1;
		while (*s == ' ')
			s++;
		for (k = 0; s[k] >= '0' && s[k] <= '9' && k < 3; k++)
			b = b * 10 + s[k] - '0';
		if (!k)
			return 0;
	}
	else
		b = a;
	return a >= 1 && b >= a && b <= 16 ? (short)b : 0;
}

/* the most players a map suggests, as the Xbox's map browser has it (hmp_browse.c's hmp_browse_suggested_players):
a stock level's own, a folder map's info .txt "Players:" line or "4-16 players" in its description; 0 for none (the
bots' "suggested" count: bots.c) */
short folder_map_suggested_players(char const *level_name)
{
	static struct
	{
		char const *name;
		short players;
	} const stock[] =
	{
		{ "beavercreek", 8 }, { "sidewinder", 16 }, { "damnation", 8 }, { "ratrace", 6 }, { "prisoner", 8 },
		{ "hangemhigh", 16 }, { "chillout", 8 }, { "carousel", 8 }, { "boardingaction", 16 }, { "bloodgulch", 16 },
		{ "wizard", 8 }, { "putput", 6 }, { "longest", 8 },
	};
	char path[300];
	char text[4096];
	unsigned long got = 0;
	char const *leaf;
	long index;
	long map_index;
	short from_text = 0;
	HANDLE file;
	char *line;

	if (!level_name || !level_name[0])
		return 0;
	if (level_name[0] != '+')
	{
		leaf = strrchr(level_name, '\\') ? strrchr(level_name, '\\') + 1 : level_name;
		for (index = 0; index < (long)NUMBEROF(stock); index++)
		{
			if (!ieq(leaf, stock[index].name) && strncmp(level_name, "custom_maps", 11))
				return stock[index].players;
		}
		return 0;
	}
	map_index = hmp_maps_find(level_name);
	if (map_index == NONE || !folder_maps.maps[map_index].info_file[0])
		return 0;
	key_path(path, sizeof(path), folder_maps.maps[map_index].key, folder_maps.maps[map_index].info_file);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
		got = 0;
	CloseHandle(file);
	/* (UTF-16: its ASCII, the rest '?') */
	if (got >= 2 && (byte)text[0] == 0xFF && (byte)text[1] == 0xFE)
	{
		unsigned long from, to = 0;

		for (from = 2; from + 1 < got; from += 2)
			text[to++] = text[from + 1] ? '?' : text[from];
		got = to;
	}
	text[got] = 0;
	for (line = text; line && *line; )
	{
		char *end = line + strcspn(line, "\r\n");
		char saved = *end;
		char *colon;

		*end = 0;
		colon = strchr(line, ':');
		if (colon)
		{
			char *name = line;

			while (*name == ' ')
				name++;
			if (!strncmp(name, "Players", 7) || !strncmp(name, "players", 7))
			{
				short players = players_in(colon + 1);

				if (players)
					return players;
			}
		}
		if (!from_text)
		{
			char *word;

			for (word = line; *word; word++)
			{
				if (word > line && (!strncmp(word, "player", 6) || !strncmp(word, "Player", 6)))
				{
					char *number = word;

					while (number > line && number[-1] == ' ')
						number--;
					while (number > line && ((number[-1] >= '0' && number[-1] <= '9') || number[-1] == '-' ||
						number[-1] == ' '))
					{
						number--;
					}
					if ((from_text = players_in(number)) != 0)
						break;
				}
			}
		}
		*end = saved;
		line = saved ? end + 1 : NULL;
	}
	return from_text;
}

/* every folder map's key, each told (looked at afresh); how many */
int folder_maps_each(void (*each)(void *context, char const *key), void *context)
{
	long index;

	hmp_maps_scan();
	for (index = 0; index < folder_maps.count; index++)
		each(context, folder_maps.maps[index].key);
	return (int)folder_maps.count;
}

/* ---------- sharing: the host */

static void crc_initialize(void)
{
	unsigned int index, bit, c;

	if (share.crc_table[1])
		return;
	for (index = 0; index < 256; index++)
	{
		c = index;
		for (bit = 0; bit < 8; bit++)
			c = c & 1 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
		share.crc_table[index] = c;
	}
}

static unsigned int crc_update(unsigned int crc, byte const *data, unsigned long size)
{
	while (size--)
		crc = share.crc_table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
	return crc;
}

static void socket_setup(SOCKET socket_handle)
{
	unsigned long nonblocking = 1;
	int buffer = SEND_BUFFER_KB << 10;
	int one = 1;

	ioctlsocket(socket_handle, FIONBIO, &nonblocking);
	setsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF, (char const *)&buffer, sizeof(buffer));
	setsockopt(socket_handle, IPPROTO_TCP, TCP_NODELAY, (char const *)&one, sizeof(one));
}

/* a file of a transfer: the folder's (a pack's series.bin: the pack's) */
static void slot_file_path(struct share_slot *slot, char const *file, char *path, int size)
{
	if (!ieq(file, "series.bin") && strchr(slot->key, '/'))
	{
		char pack[40];
		int index;

		for (index = 0; slot->key[index + 1] && slot->key[index + 1] != '/' && index < (int)sizeof(pack) - 1; index++)
			pack[index] = slot->key[index + 1];
		pack[index] = 0;
		snprintf(path, (size_t)size, "d:\\maps\\%s\\%s", pack, file);
		return;
	}
	key_path(path, size, slot->key, file);
}

static void stream_close(struct share_stream *stream)
{
	if (stream->file)
		fclose(stream->file);
	if (stream->on)
		closesocket(stream->socket);
	memset(stream, 0, sizeof(*stream));
}

static void slot_close(struct share_slot *slot, char const *why)
{
	int index;

	if (why)
	{
		share_log("host: player %d: %s (%lu bytes sent)", (int)(slot - share.slots), why, slot->sent);
	}
	for (index = 0; index < MAXS; index++)
		stream_close(&slot->streams[index]);
	memset(slot, 0, sizeof(*slot));
}

/* the next block to send: the small files first, then the map (file 0) from where a partial download left off */
static boolean next_block(struct share_slot *slot, int *file, unsigned long *offset)
{
	while (slot->cursor_file < slot->nfiles)
	{
		int which = (slot->cursor_file + 1) % slot->nfiles;

		if (slot->cursor_offset < slot->sizes[which])
		{
			*file = which;
			*offset = slot->cursor_offset;
			slot->cursor_offset += CHUNK;
			return TRUE;
		}
		slot->cursor_file++;
		slot->cursor_offset = (slot->cursor_file + 1) % slot->nfiles ? 0 : slot->start;
	}
	return FALSE;
}

/* what goes out next on a connection: 1 in its buffer, 0 nothing left, -1 a file couldn't be read */
static int stream_fill(struct share_slot *slot, struct share_stream *stream)
{
	unsigned long count;

	if (stream->phase == 1 && stream->done == stream->length)
	{
		/* (the block's data is out: its CRC) */
		unsigned int crc = ~stream->crc;

		memcpy(stream->buffer, &crc, 4);
		stream->phase = 2;
		stream->position = 0;
		stream->size = 4;
		return 1;
	}
	if (stream->phase == 2)
		stream->phase = 0;
	if (stream->phase == 0)
	{
		/* (a new block: where it goes) */
		unsigned int where;

		if (!next_block(slot, &stream->block_file, &stream->offset))
			return 0;
		stream->length = slot->sizes[stream->block_file] - stream->offset;
		if (stream->length > CHUNK)
			stream->length = CHUNK;
		where = (unsigned int)stream->block_file << 28 | (unsigned int)stream->offset;
		memcpy(stream->buffer, &where, 4);
		stream->crc = crc_update(0xFFFFFFFFU, stream->buffer, 4);
		stream->done = 0;
		stream->phase = 1;
		stream->position = 0;
		stream->size = 4;
		return 1;
	}
	if (stream->open_file != stream->block_file + 1)
	{
		char path[300];

		if (stream->file)
			fclose(stream->file);
		stream->file = NULL;
		stream->open_file = 0;
		slot_file_path(slot, slot->names[stream->block_file], path, sizeof(path));
		/* (the game's fopen finds a "d:\" path where the platform keeps it) */
		stream->file = fopen(path, "rb");
		if (!stream->file)
			return -1;
		stream->open_file = stream->block_file + 1;
	}
	count = stream->length - stream->done;
	if (count > PIECE)
		count = PIECE;
	if (fseek(stream->file, (long)(stream->offset + stream->done), SEEK_SET) != 0 ||
		fread(stream->buffer, 1, count, stream->file) != count)
	{
		return -1;
	}
	stream->crc = crc_update(stream->crc, stream->buffer, count);
	stream->done += count;
	slot->sent += count;
	stream->position = 0;
	stream->size = (int)count;
	return 1;
}

/* moves a transfer's data on its connections; FALSE once it's over (closed, the reason logged) */
static boolean slot_pump(struct share_slot *slot, unsigned long now)
{
	int open = 0, busy = 0, index;

	for (index = 0; index < MAXS; index++)
	{
		struct share_stream *stream = &slot->streams[index];
		int sends;

		if (!stream->on || stream->closed)
			continue;
		open++;
		/* (the Xbox's one-byte acknowledgements: read and dropped) */
		for (;;)
		{
			char junk[256];
			int got = recv(stream->socket, junk, sizeof(junk), 0);

			if (got > 0)
			{
				slot->heard = now;
				continue;
			}
			if (got < 0 && WSAGetLastError() == WSAEWOULDBLOCK)
				break;
			if (slot->state == 3)
			{
				/* (everything sent: the Xbox is done with this one) */
				stream->closed = TRUE;
				break;
			}
			slot_close(slot, got == 0 ? "hung up mid-transfer" : "receive failed");
			return FALSE;
		}
		if (stream->closed)
		{
			open--;
			continue;
		}
		for (sends = 0; sends < SENDS_PER_FRAME; sends++)
		{
			int sent;

			if (stream->position >= stream->size)
			{
				int filled = stream_fill(slot, stream);

				if (filled < 0)
				{
					slot_close(slot, "couldn't read the map's files");
					return FALSE;
				}
				if (!filled)
					break;
			}
			sent = send(stream->socket, (char const *)stream->buffer + stream->position,
				stream->size - stream->position, 0);
			if (sent < 0)
			{
				if (WSAGetLastError() != WSAEWOULDBLOCK)
				{
					slot_close(slot, "send failed");
					return FALSE;
				}
				break;
			}
			stream->position += sent;
			slot->heard = now;
			if (stream->position < stream->size)
				break;
		}
		if (stream->phase || stream->position < stream->size)
			busy = 1;
	}
	if (slot->state == 2 && !busy && slot->cursor_file >= slot->nfiles)
	{
		share_log("host: player %d: sent %lu bytes in %lu ms", (int)(slot - share.slots), slot->sent,
			now - slot->started);
		slot->state = 3;
		slot->heard = now;
	}
	/* (everything sent: it waits for the Xbox to hang up each connection; closing first could cut off data in flight) */
	if (slot->state == 3 && !open)
	{
		slot_close(slot, NULL);
		return FALSE;
	}
	if (now - slot->heard > STALL_MS)
	{
		slot_close(slot, "no progress for 10 s");
		return FALSE;
	}
	if (slot->state == 2 && now - slot->beat >= BEAT_MS)
	{
		share_log("host: player %d: %lu of %lu bytes sent, %lu KB/s", (int)(slot - share.slots), slot->sent,
			slot->total, (slot->sent - slot->beat_sent) / (now - slot->beat + 1));
		slot->beat = now;
		slot->beat_sent = slot->sent;
	}
	return TRUE;
}

static boolean host_in_game(void)
{
	struct network_game_server *server = global_network_game_server_get();

	return server && network_game_server_playing(server);
}

/* a request from a pending connection: a slot taken, and the reply on its way */
static void slot_request(struct share_slot *slot, struct share_pending *pending, unsigned long now)
{
	struct share_request *request = &pending->request;
	struct share_stream *stream = &slot->streams[0];
	struct share_reply reply;
	char const *host_map = network_game_current_map_name();
	char const *why = "not the map being hosted";
	char path[300];
	unsigned long size = 0, checksum = 0, total = 0, start = 0;
	long map_index;
	int index = (int)(slot - share.slots);
	int count = 0;
	int which;

	memset(slot, 0, sizeof(*slot));
	slot->machine = request->machine < 16 ? (int)request->machine : NONE;
	slot->address = pending->address;
	slot->started = slot->heard = slot->beat = now;
	stream->socket = pending->socket;
	stream->on = TRUE;
	pending->used = FALSE;
	memset(&reply, 0, sizeof(reply));
	reply.magic = REP_MAGIC;
	reply.version = SHARE_VERSION;
	reply.status = REP_UNAVAILABLE;
	reply.in_game = (unsigned int)host_in_game();
	request->key[sizeof(request->key) - 1] = 0;
	map_index = hmp_maps_find(request->key);
	if (request->magic != REQ_MAGIC || request->version != SHARE_VERSION)
		why = "different map sharing version";
	else if (request->key[0] != '+' || !host_map ||
		(ieq(request->key, host_map) && !(linked_map_key() && !ieq(request->key, linked_map_key()))))
	{
		/* (the map hosted, or a linked game's Xbox map: linked_maps.c) */
		reply.streams = WHY_NOT_HOSTED;
	}
	else if (map_index == NONE || !hmp_map_file_path(request->key, path, sizeof(path)) ||
		!map_identity(path, &size, &checksum))
	{
		why = "can't read its own copy";
	}
	else
	{
		struct folder_map const *map = &folder_maps.maps[map_index];

		reply.map_chk = (unsigned int)checksum;
		reply.map_version = 0;
		if (map->info_file[0])
		{
			/* (its info .txt's "Version:", which says which of two copies is newer) */
			char info_path[300];
			char text[2048];
			unsigned long got = 0;
			HANDLE file;

			key_path(info_path, sizeof(info_path), map->key, map->info_file);
			file = CreateFileA(info_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
			if (file != INVALID_HANDLE_VALUE)
			{
				char *line;

				if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
					got = 0;
				CloseHandle(file);
				text[got] = 0;
				for (line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
				{
					while (*line == ' ' || *line == '\t')
						line++;
					if (!strncmp(line, "Version:", 8) || !strncmp(line, "version:", 8))
					{
						reply.map_version = (unsigned int)strtoul(line + 8, NULL, 10);
						break;
					}
				}
			}
		}
		if (request->have_size == size && request->have_chk == checksum)
		{
			reply.status = REP_SAME;
		}
		else
		{
			char const *names[MAX_FILES];

			snprintf(slot->key, sizeof(slot->key), "%s", map->key);
			names[0] = map->map_file;
			names[1] = map->info_file[0] ? map->info_file : NULL;
			names[2] = map->picture ? "preview.bin" : NULL;
			names[3] = map->series ? "series.bin" : NULL;
			names[4] = "showcase.txt";
			for (which = 0; which < MAX_FILES; which++)
			{
				char file_path[300];
				HANDLE file;
				unsigned long file_size;

				if (!names[which] || strlen(names[which]) >= NAME_LEN)
					continue;
				slot_file_path(slot, names[which], file_path, sizeof(file_path));
				file = CreateFileA(file_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
				if (file == INVALID_HANDLE_VALUE)
					continue;
				file_size = GetFileSize(file, NULL);
				CloseHandle(file);
				snprintf(slot->names[count], NAME_LEN, "%s", names[which]);
				slot->sizes[count] = file_size;
				snprintf(reply.files[count].name, NAME_LEN, "%s", names[which]);
				reply.files[count].size = (unsigned int)file_size;
				total += file_size;
				count++;
			}
			if (count && ends_with(slot->names[0], ".map"))
			{
				reply.status = REP_SENDING;
				reply.nfiles = (unsigned int)count;
				slot->nfiles = count;
				if (request->resume_size == size && request->resume_chk == checksum && request->resume_offset &&
					request->resume_offset < size && request->resume_offset % CHUNK == 0)
				{
					start = request->resume_offset;
				}
				slot->start = start;
				reply.start = (unsigned int)start;
				slot->total = total - start;
				slot->cursor_offset = count == 1 ? start : 0;
				slot->want = request->flags >= MAXS ? MAXS : request->flags ? (int)request->flags : 1;
				slot->token = (now ^ (slot->address * 2654435761UL) ^ ((unsigned long)index << 24) ^ 0x5eed) & 0xFFFFFFFFUL;
				reply.streams = (unsigned int)slot->want;
				reply.token = (unsigned int)slot->token;
			}
		}
	}
	if (reply.status == REP_SAME)
		share_log("host: player %d asked for %s: has the same build", index, request->key);
	else if (reply.status == REP_SENDING)
	{
		share_log("host: player %d asked for %s: sending %d files, %lu bytes%s, over %d connection%s", index,
			request->key, count, total, start ? " (resuming)" : "", slot->want, slot->want == 1 ? "" : "s");
	}
	else if (reply.streams == WHY_NOT_HOSTED)
		share_log("host: player %d asked for %s: not the map being hosted now", index, request->key);
	else
		share_log("host: player %d asked for %s: refused (%s)", index, request->key, why);
	memcpy(stream->buffer, &reply, sizeof(reply));
	stream->position = 0;
	stream->size = sizeof(reply);
	slot->state = reply.status == REP_SENDING ? 2 : 3;
}

/* another connection for a transfer under way */
static void slot_join(struct share_pending *pending)
{
	int which = (int)pending->request.stream;
	int index;

	for (index = 0; index < SLOTS; index++)
	{
		struct share_slot *slot = &share.slots[index];
		struct share_stream *stream;

		if (slot->state != 2 || slot->token != pending->request.token || slot->address != pending->address)
			continue;
		if (which < 1 || which >= slot->want || slot->streams[which].on)
			break;
		stream = &slot->streams[which];
		memset(stream, 0, sizeof(*stream));
		stream->socket = pending->socket;
		stream->on = TRUE;
		pending->used = FALSE;
		share_log("host: player %d: connection %d of %d joined", index, which + 1, slot->want);
		return;
	}
	share_log("host: turned away a connection for a transfer that isn't running");
	closesocket(pending->socket);
	pending->used = FALSE;
}

static void host_stop(void)
{
	int index;

	for (index = 0; index < SLOTS; index++)
	{
		if (share.slots[index].state)
			slot_close(&share.slots[index], "the game moved off the map");
	}
	for (index = 0; index < PEND; index++)
	{
		if (share.pending[index].used)
		{
			closesocket(share.pending[index].socket);
			share.pending[index].used = FALSE;
		}
	}
	if (share.listener != INVALID_SOCKET)
	{
		closesocket(share.listener);
		share.listener = INVALID_SOCKET;
		share_log("host: stopped sharing");
	}
}

static void host_update(unsigned long now)
{
	int index;

	if (share.listener == INVALID_SOCKET)
	{
		struct sockaddr_in address;
		char const *step = "socket";

		if (share.listen_tried_at && now - share.listen_tried_at < 2000)
			return;
		share.listen_tried_at = now | 1;
		share.listener = socket(AF_INET, SOCK_STREAM, 0);
		if (share.listener != INVALID_SOCKET)
		{
			socket_setup(share.listener);
			memset(&address, 0, sizeof(address));
			address.sin_family = AF_INET;
			address.sin_port = htons(SHARE_PORT);
			step = "bind";
			if (!bind(share.listener, (struct sockaddr *)&address, sizeof(address)))
			{
				step = "listen";
				if (!listen(share.listener, SLOTS * MAXS))
				{
					share_log("host: sharing folder maps with Xboxes on port %d", SHARE_PORT);
					goto ready;
				}
			}
			closesocket(share.listener);
			share.listener = INVALID_SOCKET;
		}
		share_log("host: couldn't start sharing (%s failed, error %d); retrying", step, WSAGetLastError());
		return;
	}
ready:
	/* (connections) */
	for (;;)
	{
		struct sockaddr_in address;
		int length = sizeof(address);
		SOCKET accepted;

		for (index = 0; index < PEND && share.pending[index].used; index++)
			;
		if (index == PEND)
			break;
		memset(&address, 0, sizeof(address));
		accepted = accept(share.listener, (struct sockaddr *)&address, &length);
		if (accepted == INVALID_SOCKET)
			break;
		socket_setup(accepted);
		memset(&share.pending[index], 0, sizeof(share.pending[index]));
		share.pending[index].used = TRUE;
		share.pending[index].socket = accepted;
		share.pending[index].time = now;
		share.pending[index].address = address.sin_addr.s_addr;
	}
	/* (what each new connection wants) */
	for (index = 0; index < PEND; index++)
	{
		struct share_pending *pending = &share.pending[index];
		int got;
		int free_slot;

		if (!pending->used)
			continue;
		if (now - pending->time > STALL_MS)
		{
			share_log("host: a connection never asked for anything; closed");
			closesocket(pending->socket);
			pending->used = FALSE;
			continue;
		}
		got = recv(pending->socket, (char *)&pending->request + pending->length,
			(int)sizeof(pending->request) - pending->length, 0);
		if (got == 0 || (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK))
		{
			closesocket(pending->socket);
			pending->used = FALSE;
			continue;
		}
		if (got > 0)
			pending->length += got;
		if (pending->length < (int)sizeof(pending->request))
			continue;
		if (pending->request.magic == JOIN_MAGIC)
		{
			slot_join(pending);
			continue;
		}
		for (free_slot = 0; free_slot < SLOTS && share.slots[free_slot].state; free_slot++)
			;
		if (free_slot == SLOTS)
		{
			share_log("host: turned away a player (all %d transfer slots busy)", SLOTS);
			closesocket(pending->socket);
			pending->used = FALSE;
			continue;
		}
		share_log("host: player %d connected", free_slot);
		slot_request(&share.slots[free_slot], pending, now);
	}
	/* (the transfers) */
	for (index = 0; index < SLOTS; index++)
	{
		if (share.slots[index].state)
			slot_pump(&share.slots[index], now);
	}
}

/* the machine that joined from this address (either byte order), NONE for none */
static int machine_by_address(unsigned long address)
{
	int machine;

	for (machine = 0; address && machine < 16; machine++)
	{
		unsigned long known = network_game_server_machine_address(machine);
		unsigned long swapped = (known >> 24) | ((known >> 8) & 0xFF00) | ((known & 0xFF00) << 8) | (known << 24);

		if (known && (known == address || swapped == address))
			return machine;
	}
	return NONE;
}

static void machines_update(unsigned long now)
{
	int index, machine;

	memset(share.machine_active, 0, sizeof(share.machine_active));
	for (index = 0; index < PEND; index++)
	{
		if (share.pending[index].used && (machine = machine_by_address(share.pending[index].address)) != NONE)
			share.machine_seen[machine] = now | 1;
	}
	for (index = 0; index < SLOTS; index++)
	{
		struct share_slot *slot = &share.slots[index];

		if (slot->state != 2)
			continue;
		/* (one that asked before it had joined: its machine by the address it joined from, once it has) */
		if (slot->machine == NONE && (machine = machine_by_address(slot->address)) != NONE)
		{
			slot->machine = machine;
			share_log("host: the Xbox downloading the map is machine #%d (it asked before it had joined)", machine);
		}
		if (slot->machine >= 0 && slot->machine < 16)
		{
			share.machine_active[slot->machine] = 1;
			share.machine_seen[slot->machine] = now | 1;
		}
	}
}

/* ---------- map_share.c's */

/* whether a machine is downloading the host's folder map (its players are held out of the game meanwhile) */
boolean map_share_xbox_machine_needs_map(long machine_index)
{
	return machine_index >= 0 && machine_index < 16 && share.machine_active[machine_index];
}

/* ... or did a moment ago, or asked (the host doesn't time it out for adding no players meanwhile) */
boolean map_share_xbox_machine_downloading(long machine_index)
{
	return machine_index >= 0 && machine_index < 16 && (share.machine_active[machine_index] ||
		(share.machine_seen[machine_index] && now_ms() - share.machine_seen[machine_index] < DOWNLOAD_GRACE_MS));
}

/* every frame (map_share_update): the host shares the folder map it's hosting */
void map_share_xbox_update(void)
{
	struct network_game_server *server = global_network_game_server_get();
	char const *map = network_game_current_map_name();
	unsigned long now = now_ms();

	crc_initialize();
	/* (a Custom Edition map's link to an Xbox map: linked_maps.c) */
	linked_maps_update();
	if (server && map && (map[0] == '+' || linked_map_key()))
		host_update(now);
	else if (share.listener != INVALID_SOCKET)
		host_stop();
	machines_update(server ? now : 0);
}

#endif
