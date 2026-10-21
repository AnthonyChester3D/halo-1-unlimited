/*
HMP_MAPS.C

Halo Universal's map folders on the Xbox (hmp.h; Halo Universal's maplist.c, written against OpenCE's code instead of
the retail XBE's addresses).

	maps\
		bloodgulch.map ...       the stock maps, as before
		order.txt                optional: the menu's order
		Precipice\               a map: a folder with a .map (any name), an info .txt, a preview.bin
		CEX\                     a pack: a folder without a .map; each folder in it with a .map is a map
			series.bin           optional: the pack's logo, stamped on its maps' pictures
			order.txt            optional: the pack's own order
			Abyss\ ...

T:\maps (E:\TDATA\4d530004\maps: Halo's own storage on the hard drive) is read the same way after d:\maps; a map in
both is listed once (the newer .map; map sharing can switch to the copy that matches the host).

info.txt: line 1 the name, the rest the description (UTF-8, UTF-16 with a BOM, or ANSI; "|n" breaks a line).
"Genres:", "Gametypes:" and "Players:" lines are the browser's (hmp_browse.c) and aren't shown. Halo's text boxes
never wrap, so names and descriptions are broken into lines here, with the menu fonts' letter widths.

preview.bin: 256x128 DXT1 (16,384 bytes, no header), the layout of Halo's own map pictures (the top-left 140x114
shows). It's read straight into the texture cache's memory when the picture is first drawn (hmp_texture_read), through
a bitmap set up as Halo's own map pictures are; a few of those bitmaps are kept, for the pictures on screen.

series.bin: "HUSB", width, height, x, y (16 bits each, up to 64x64 inside the 256x128 picture), 4 bytes reserved,
then the pixels (blue, green, red, alpha), top row first. Blended over the picture's DXT1 blocks as it loads.

Memory: the maps found (a small record each, and their names in one block) stay, as a game's map is found by its key;
the list's table and the text caches are the menus' and are freed when a game map loads.
*/

#ifdef HALO_XBOX_TARGET

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "bitmaps/bitmap_group.h"
#include "cache/texture_cache.h"
#include "text/text_group.h"
#include "hmp.h"

#include <string.h>
#include <xtl.h>

/* ---------- constants */

#define MAXIMUM_FOLDER_MAPS 2048
#define MAXIMUM_LIST_POSITIONS 4096          /* (a map can be in several of the browser's groups) */
#define MAXIMUM_FOLDER_NAME 30               /* a folder's name (a pack's and its map's together) */
#define INFO_CACHE_COUNT 8
#define NAME_TEXT_WIDTH 141                  /* large_ui: the boxes are 145 wide, text starts 4 in */
#define DESCRIPTION_TEXT_WIDTH 142           /* small_ui: 3 in */
#define PREVIEW_SIZE 16384
#define SERIES_MAXIMUM_SIZE (16 + 64 * 64 * 4)
#define FAKE_TAG 0x484d0000                  /* a fake bitmap's tag index: 'HM' << 16 | its number */
#define FAKE_ICON_BASE 0xff00                /* (hmp_browse.c's pictures: FAKE_TAG | FAKE_ICON_BASE + n) */
#define PREVIEW_SLOTS 16                     /* preview bitmaps kept (the 3 cards, the lobby, the game browser) */
#define NOT_IN_PACK_ORDER 0x7fff
#define LEVEL_PICTURES_TAG_NAME "ui\\shell\\bitmaps\\mp_map_grafix"
#define INVALID_FILE ((HANDLE)-1)

/* ---------- structures */

/* a folder map: its key ("+folder", "+pack/folder"), .map and .txt file names are offsets into the name block */
struct hmp_map
{
	long key, map_file, info_file;
	unsigned long map_time_low, map_time_high;   /* the .map's last write */
	short order;                                 /* order.txt: 0 not listed, 1 listed, 2 hidden */
	short pack_order;                            /* place in its pack's order.txt (NOT_IN_PACK_ORDER) */
	char root;                                   /* 0 d:\maps, 1 T:\maps */
	char both;                                   /* a copy in the other too */
	char picture;                                /* a preview.bin */
	char series;                                 /* its pack has a series.bin */
	char hidden;                                 /* "-name" in its pack's order.txt */
	char mark;
	char pinned;                                 /* map sharing chose this copy for the session */
	char pad;
};

struct hmp_info
{
	long map_index;
	wchar_t name[64];
	wchar_t wrapped_name[96];
	wchar_t description[384];
};

/* what only the menus need (hmp_menu_memory_release frees it) */
struct hmp_menu_memory
{
	struct hmp_info info[INFO_CACHE_COUNT];
	long info_next;
	wchar_t composed[3][640];
	long composed_next;
	wchar_t raw_description[320];
	unsigned char raw[1024];
};

struct preview_slot
{
	struct bitmap_data bitmap;
	long map_index;
	unsigned long last_used;
};

/* ---------- prototypes */

void *__stdcall LocalAlloc(unsigned int flags, unsigned int bytes);
void *__stdcall LocalFree(void *memory);

/* ---------- globals */

static struct
{
	boolean scanned;
	struct hmp_map *maps;
	long map_count;
	char *names;
	long names_size, names_capacity;
	char const *stock_levels[HMP_STOCK_LEVELS];
	char **table;
	long table_count, table_capacity, listed;
	unsigned char stock_order[HMP_STOCK_LEVELS];
	struct hmp_menu_memory *menu;
	long visible_entry[3], visible_position[3];
	unsigned char *series;                      /* a series.bin being stamped (the menus') */
	struct preview_slot previews[PREVIEW_SLOTS];
	unsigned long preview_clock;
	long scan_t, scan_both;
	char const *pack_key;
	long pack_position;
	struct _WIN32_FIND_DATAA find[3];
	char path[300], picture_path[300], info_path[300];
	wchar_t key_name[32];
} hmp_maps_globals;

static char const *const maps_roots[2] = { "d:\\maps\\", "T:\\maps\\" };

/* the stock levels' names, in the list's stock order (the Xbox's 13; event_handler_functions' multiplayer_levels) */
static char const *const default_stock_levels[HMP_STOCK_LEVELS] =
{
	"levels\\test\\beavercreek\\beavercreek", "levels\\test\\sidewinder\\sidewinder", "levels\\test\\damnation\\damnation",
	"levels\\test\\ratrace\\ratrace", "levels\\test\\prisoner\\prisoner", "levels\\test\\hangemhigh\\hangemhigh",
	"levels\\test\\chillout\\chillout", "levels\\test\\carousel\\carousel", "levels\\test\\boardingaction\\boardingaction",
	"levels\\test\\bloodgulch\\bloodgulch", "levels\\test\\wizard\\wizard", "levels\\test\\putput\\putput",
	"levels\\test\\longest\\longest"
};

/* letter widths of ui\large_ui and ui\small_ui (codes 32-255, then the Windows-1252 extras; -1 = none) */
static signed char const large_widths[224] = {6,5,9,13,11,19,12,4,6,6,9,11,4,7,4,9,12,7,13,12,13,12,12,12,13,12,4,4,8,11,8,11,15,14,12,12,12,11,11,13,13,5,12,13,11,15,13,13,12,13,13,12,12,13,13,16,12,15,12,6,9,7,10,10,8,10,10,10,10,10,8,10,10,6,6,10,5,16,11,10,10,10,7,10,8,10,12,15,11,12,9,8,4,8,10,9,-1,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,9,-1,9,9,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,9,-1,-1,3,5,9,12,15,13,15,12,9,15,8,12,11,15,15,8,8,15,15,15,8,15,13,4,8,15,8,11,15,15,15,11,13,13,13,13,13,13,19,12,11,11,11,11,6,5,7,7,13,13,13,13,13,13,13,15,13,13,13,13,13,14,12,10,10,10,10,10,10,10,15,9,9,9,9,9,6,4,6,6,10,10,10,10,10,10,10,15,10,10,10,10,10,11,10,11};
static signed char const small_widths[224] = {4,3,7,10,8,13,9,6,5,5,7,8,3,5,3,6,9,6,9,9,10,9,9,8,9,9,3,3,6,7,6,8,11,10,10,9,10,8,7,10,10,4,9,9,8,11,10,10,9,10,10,9,9,11,9,12,9,10,9,5,6,4,8,7,5,8,8,7,9,7,5,9,9,4,5,7,4,12,8,8,8,9,6,8,6,9,7,11,7,8,7,6,4,5,8,6,-1,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,6,-1,6,6,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,6,-1,-1,4,3,7,8,6,9,6,9,9,4,6,8,6,5,6,5,5,6,6,6,4,6,9,6,5,6,6,8,6,6,6,8,6,7,7,6,6,6,6,6,6,5,5,6,7,5,7,7,6,5,6,6,6,6,6,6,6,5,5,5,5,6,6,6,6,14,6,6,6,6,6,6,6,10,6,7,7,5,7,7,6,11,6,6,6,6,6,6,6,7,5,7,5,6,6,6};
struct extra_width { unsigned short code; signed char width; };
static struct extra_width const large_extra_widths[] = {{338,20},{339,16},{352,13},{353,10},{376,14},{381,13},{382,11},{402,8},{710,10},{732,10},{8211,9},{8212,12},{8216,5},{8217,5},{8218,5},{8220,10},{8221,10},{8222,10},{8224,11},{8225,11},{8226,9},{8230,16},{8240,27},{8249,7},{8250,7},{8364,10},{8482,16}};
static struct extra_width const small_extra_widths[] = {{338,6},{339,6},{352,6},{353,6},{376,6},{381,9},{382,7},{402,6},{710,6},{732,6},{8211,6},{8212,6},{8216,6},{8217,6},{8218,6},{8220,6},{8221,6},{8222,6},{8224,6},{8225,6},{8226,6},{8230,6},{8240,6},{8249,6},{8250,6},{8364,6},{8482,6}};

/* ---------- private code: strings */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ieq(char const *a, char const *b)
{
	while (*a && lower(*a) == lower(*b)) { a++; b++; }
	return lower(*a) - lower(*b);
}
static char *scpy(char *d, char const *s) { while ((*d = *s) != 0) { d++; s++; } return d; }
static boolean ends_with(char const *s, char const *ext)
{
	long a = (long)strlen(s), b = (long)strlen(ext);
	return a > b && !ieq(s + a - b, ext);
}
static boolean has_text(char const *s, char const *t)
{
	long n = (long)strlen(t), k;
	for (; *s; s++) { for (k = 0; k < n && lower(s[k]) == lower(t[k]); k++) ; if (k == n) return TRUE; }
	return FALSE;
}

static char const *map_string(long offset) { return hmp_maps_globals.names + offset; }
#define MAP_KEY(i) map_string(hmp_maps_globals.maps[i].key)

/* a string into the name block (grown as needed): its offset, NONE when out of memory */
static long names_add(char const *s)
{
	long length = (long)strlen(s) + 1, offset;
	if (hmp_maps_globals.names_size + length > hmp_maps_globals.names_capacity)
	{
		long capacity = hmp_maps_globals.names_capacity ? hmp_maps_globals.names_capacity * 2 : 4096;
		char *names;
		while (capacity < hmp_maps_globals.names_size + length) capacity *= 2;
		names = (char *)LocalAlloc(0, (unsigned int)capacity);
		if (!names) return NONE;
		if (hmp_maps_globals.names) { memcpy(names, hmp_maps_globals.names, hmp_maps_globals.names_size); LocalFree(hmp_maps_globals.names); }
		hmp_maps_globals.names = names;
		hmp_maps_globals.names_capacity = capacity;
	}
	offset = hmp_maps_globals.names_size;
	memcpy(hmp_maps_globals.names + offset, s, length);
	hmp_maps_globals.names_size += length;
	return offset;
}

/* "<root><folder>\<file>" for a key ("+folder"; "+pack/folder" -> "<root>pack\folder\<file>") */
static char *key_path(char *buffer, short root, char const *key, char const *file)
{
	char *p = scpy(buffer, maps_roots[root & 1]);
	char const *k;
	for (k = key + 1; *k; k++) *p++ = *k == '/' ? '\\' : *k;
	*p++ = '\\';
	return scpy(p, file);
}
static char *folder_path(char *buffer, long i, char const *file)
{
	return key_path(buffer, hmp_maps_globals.maps[i].root, MAP_KEY(i), file);
}
/* "<root><pack>\<file>" for a pack's map */
static char *pack_path(char *buffer, long i, char const *file)
{
	char *p = scpy(buffer, maps_roots[hmp_maps_globals.maps[i].root & 1]);
	char const *k;
	for (k = MAP_KEY(i) + 1; *k && *k != '/'; k++) *p++ = *k;
	*p++ = '\\';
	scpy(p, file);
	return buffer;
}
static boolean in_pack(long i) { return strchr(MAP_KEY(i), '/') != NULL; }
/* the top-level folders' names compared (a pack's name, up to its '/') */
static int top_compare(char const *a, char const *b)
{
	for (;; a++, b++)
	{
		int x = *a == '/' ? 0 : lower(*a), y = *b == '/' ? 0 : lower(*b);
		if (x != y || !x) return x - y;
	}
}

static HANDLE open_read(char const *path)
{
	return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
}

/* ---------- private code: order.txt */

char const *hmp_maps_stock_name(long k)
{
	/* (Halo's multiplayer levels, in its order, until the map list has been built: a client that joined from the
	system link browser never builds it, and its postgame asked for these: it crashed, the host's END GAME left
	the clients frozen) */
	char const *s, *b;
	if (k < 0 || k >= HMP_STOCK_LEVELS) return "";
	s = hmp_maps_globals.stock_levels[k] ? hmp_maps_globals.stock_levels[k] : default_stock_levels[k];
	b = s;
	for (; *s; s++) if (*s == '\\') b = s + 1;
	return b;
}

/* a line trimmed; NULL = nothing on it (blank or a note). *hide: it started with '-' */
static char *clean_line(char *s, boolean *hide)
{
	char *t;
	*hide = FALSE;
	while (*s == ' ' || *s == '\t') s++;
	for (t = s + strlen(s); t > s && (t[-1] == ' ' || t[-1] == '\t'); t--) ;
	*t = 0;
	if (!*s || *s == '#' || *s == ';') return NULL;
	if (*s == '-') { *hide = TRUE; s++; while (*s == ' ' || *s == '\t') s++; }
	if (*s == '+') s++;
	for (t = s; *t; t++) if (*t == '\\') *t = '/';
	return *s ? s : NULL;
}

static void order_line(char *s)
{
	boolean hide;
	long i, entry = NONE;
	if (!(s = clean_line(s, &hide))) return;
	for (i = 0; i < hmp_maps_globals.map_count; i++) if (!ieq(MAP_KEY(i) + 1, s)) { entry = HMP_STOCK_LEVELS + i; break; }
	if (entry == NONE) for (i = 0; i < HMP_STOCK_LEVELS; i++) if (!ieq(hmp_maps_stock_name(i), s)) { entry = i; break; }
	if (entry == NONE)
	{
		/* a pack's name: all its maps, in the pack's order (one its pack hides can still be listed by itself) */
		if (!strchr(s, '/'))
		{
			for (i = 0; i < hmp_maps_globals.map_count; i++)
			{
				struct hmp_map *map = &hmp_maps_globals.maps[i];
				if (map->order || !in_pack(i) || top_compare(MAP_KEY(i) + 1, s) || (map->hidden && !hide)) continue;
				map->order = hide ? 2 : 1;
				if (!hide && hmp_maps_globals.listed < hmp_maps_globals.table_capacity)
					hmp_maps_globals.table[hmp_maps_globals.listed++] = (char *)MAP_KEY(i);
			}
		}
		return;
	}
	if (entry < HMP_STOCK_LEVELS)
	{
		if (hmp_maps_globals.stock_order[entry]) return;
		hmp_maps_globals.stock_order[entry] = hide ? 2 : 1;
	}
	else
	{
		if (hmp_maps_globals.maps[entry - HMP_STOCK_LEVELS].order) return;
		hmp_maps_globals.maps[entry - HMP_STOCK_LEVELS].order = hide ? 2 : 1;
	}
	if (!hide && hmp_maps_globals.listed < hmp_maps_globals.table_capacity)
	{
		hmp_maps_globals.table[hmp_maps_globals.listed++] = entry < HMP_STOCK_LEVELS ?
			(char *)hmp_maps_globals.stock_levels[entry] : (char *)MAP_KEY(entry - HMP_STOCK_LEVELS);
	}
}

/* a pack's order.txt: the maps whose key starts with pack_key ("+pack/") */
static void pack_line(char *s)
{
	boolean hide;
	long i;
	if (!(s = clean_line(s, &hide))) return;
	hmp_maps_globals.pack_position++;
	for (i = 0; i < hmp_maps_globals.map_count; i++)
	{
		char const *m = MAP_KEY(i) + 1;
		if (top_compare(m, hmp_maps_globals.pack_key + 1)) continue;
		while (*m && *m != '/') m++;
		if (*m == '/' && !ieq(m + 1, s) && hmp_maps_globals.maps[i].pack_order == NOT_IN_PACK_ORDER)
		{
			hmp_maps_globals.maps[i].pack_order = (short)hmp_maps_globals.pack_position;
			hmp_maps_globals.maps[i].hidden = (char)hide;
		}
	}
}

/* a small text file line by line (UTF-8, UTF-16 with a BOM, ANSI; names are plain ASCII). FALSE = no file */
static boolean read_lines(char const *path, void (*line_proc)(char *))
{
	unsigned char raw[512];
	char line[64];
	long length = 0, k;
	boolean first = TRUE, utf16 = FALSE, half = FALSE;
	HANDLE file = open_read(path);
	if (file == INVALID_FILE) return FALSE;
	for (;;)
	{
		unsigned long got = 0;
		if (!ReadFile(file, raw, sizeof(raw), &got, NULL) || !got) break;
		k = 0;
		if (first)
		{
			first = FALSE;
			if (got >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) k = 3;
			else if (got >= 2 && raw[0] == 0xff && raw[1] == 0xfe) { k = 2; utf16 = TRUE; }
		}
		for (; k < (long)got; k++)
		{
			unsigned char c = raw[k];
			if (utf16 && (half = !half) == FALSE) continue;
			if (c == '\n' || c == '\r') { line[length] = 0; line_proc(line); length = 0; continue; }
			if (length < 63) line[length++] = (char)c;
		}
	}
	line[length] = 0;
	line_proc(line);
	CloseHandle(file);
	return TRUE;
}

/* ---------- private code: the menus' memory */

static struct hmp_menu_memory *menu_memory(void)
{
	if (!hmp_maps_globals.menu)
	{
		long k;
		hmp_maps_globals.menu = (struct hmp_menu_memory *)LocalAlloc(LMEM_ZEROINIT, sizeof(struct hmp_menu_memory));
		if (hmp_maps_globals.menu) for (k = 0; k < INFO_CACHE_COUNT; k++) hmp_maps_globals.menu->info[k].map_index = NONE;
	}
	return hmp_maps_globals.menu;
}

static boolean table_allocate(void)
{
	long capacity = (HMP_STOCK_LEVELS + hmp_maps_globals.map_count) * 3;
	if (capacity > MAXIMUM_LIST_POSITIONS) capacity = MAXIMUM_LIST_POSITIONS;
	if (hmp_maps_globals.table && hmp_maps_globals.table_capacity >= capacity) return TRUE;
	if (hmp_maps_globals.table) LocalFree(hmp_maps_globals.table);
	hmp_maps_globals.table = (char **)LocalAlloc(0, (unsigned int)capacity * sizeof(char *));
	hmp_maps_globals.table_capacity = hmp_maps_globals.table ? capacity : 0;
	return hmp_maps_globals.table != NULL;
}

/* the list in Halo's order (order.txt), then sorted as the browser says */
static void build_list(void)
{
	long i, k, n, s = 0;
	if (!table_allocate()) { hmp_maps_globals.table_count = 0; return; }
	hmp_maps_globals.listed = 0;
	for (k = 0; k < HMP_STOCK_LEVELS; k++) hmp_maps_globals.stock_order[k] = 0;
	for (i = 0; i < hmp_maps_globals.map_count; i++) hmp_maps_globals.maps[i].order = 0;
	if (read_lines("d:\\maps\\order.txt", order_line))
	{
		/* stock maps not listed stay at the top, the listed ones follow in the file's order, folders not listed at the end */
		for (k = 0; k < HMP_STOCK_LEVELS; k++) if (!hmp_maps_globals.stock_order[k]) s++;
		if (hmp_maps_globals.listed + s > hmp_maps_globals.table_capacity) hmp_maps_globals.listed = hmp_maps_globals.table_capacity - s;
		for (i = hmp_maps_globals.listed - 1; i >= 0; i--) hmp_maps_globals.table[i + s] = hmp_maps_globals.table[i];
		n = 0;
		for (k = 0; k < HMP_STOCK_LEVELS; k++) if (!hmp_maps_globals.stock_order[k]) hmp_maps_globals.table[n++] = (char *)hmp_maps_globals.stock_levels[k];
		n += hmp_maps_globals.listed;
		for (i = 0; i < hmp_maps_globals.map_count && n < hmp_maps_globals.table_capacity; i++)
		{
			if (!hmp_maps_globals.maps[i].order && !hmp_maps_globals.maps[i].hidden) hmp_maps_globals.table[n++] = (char *)MAP_KEY(i);
		}
		hmp_maps_globals.table_count = n;
		if (n) { hmp_browse_apply(); return; }
	}
	for (k = 0; k < HMP_STOCK_LEVELS; k++) hmp_maps_globals.table[k] = (char *)hmp_maps_globals.stock_levels[k];
	n = HMP_STOCK_LEVELS;
	for (i = 0; i < hmp_maps_globals.map_count && n < hmp_maps_globals.table_capacity; i++)
	{
		if (!hmp_maps_globals.maps[i].hidden) hmp_maps_globals.table[n++] = (char *)MAP_KEY(i);
	}
	hmp_maps_globals.table_count = n;
	hmp_browse_apply();
}

/* ---------- private code: the scan */

/* what's in a map's folder (in maps folder map->root): its .map, .txt, preview.bin and the .map's time. TRUE = a .map */
static boolean list_folder(struct hmp_map *map)
{
	struct _WIN32_FIND_DATAA *find = &hmp_maps_globals.find[1];
	char map_file[48] = "", info_file[48] = "";
	HANDLE handle;
	map->picture = 0;
	map->map_time_low = map->map_time_high = 0;
	key_path(hmp_maps_globals.path, map->root, map_string(map->key), "*.*");
	handle = FindFirstFileA(hmp_maps_globals.path, find);
	if (handle == INVALID_FILE) return FALSE;
	do
	{
		char const *f = find->cFileName;
		if (find->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		if (strlen(f) > 43) continue;
		if (!map_file[0] && ends_with(f, ".map"))
		{
			scpy(map_file, f);
			map->map_time_low = find->ftLastWriteTime.dwLowDateTime;
			map->map_time_high = find->ftLastWriteTime.dwHighDateTime;
		}
		else if (!info_file[0] && ends_with(f, ".txt")) scpy(info_file, f);
		else if (!ieq(f, "preview.bin") && !find->nFileSizeHigh && find->nFileSizeLow == PREVIEW_SIZE) map->picture = 1;
	}
	while (FindNextFileA(handle, find));
	CloseHandle(handle);
	if (!map_file[0]) return FALSE;
	map->map_file = names_add(map_file);
	map->info_file = info_file[0] ? names_add(info_file) : NONE;
	return map->map_file != NONE;
}

/* adds <root>\<top>\ (<root>\<top>\<sub>\ for a pack's map) if it holds a .map */
static boolean add_folder(short root, char const *top, char const *sub)
{
	struct hmp_map *map = &hmp_maps_globals.maps[hmp_maps_globals.map_count];
	char key[40], *p = scpy(key, "+");
	long names_size = hmp_maps_globals.names_size;
	p = scpy(p, top);
	if (sub) { *p++ = '/'; scpy(p, sub); }
	memset(map, 0, sizeof(*map));
	map->pack_order = sub ? NOT_IN_PACK_ORDER : 0;
	map->root = (char)root;
	map->key = names_add(key);
	if (map->key == NONE || !list_folder(map))
	{
		hmp_maps_globals.names_size = names_size;    /* (nothing kept for a folder that isn't a map) */
		return FALSE;
	}
	hmp_maps_globals.map_count++;
	return TRUE;
}

static boolean is_folder(struct _WIN32_FIND_DATAA const *find)
{
	return (find->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && find->cFileName[0] != '.' && !has_text(find->cFileName, ".hmp_");
}
static char *top_path(char *buffer, short root, char const *top, char const *file)
{
	char *p = scpy(buffer, maps_roots[root]);
	p = scpy(p, top);
	*p++ = '\\';
	scpy(p, file);
	return buffer;
}
static boolean is_series(struct _WIN32_FIND_DATAA const *find)
{
	return !(find->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !ieq(find->cFileName, "series.bin") && !find->nFileSizeHigh &&
		find->nFileSizeLow > 16 && find->nFileSizeLow <= SERIES_MAXIMUM_SIZE;
}
/* the list's order: by top-level folder (a pack's name), then the pack's order, then the name */
static int map_compare(struct hmp_map const *a, struct hmp_map const *b)
{
	int c = top_compare(map_string(a->key) + 1, map_string(b->key) + 1);
	if (c) return c;
	if (a->pack_order != b->pack_order) return a->pack_order - b->pack_order;
	return ieq(map_string(a->key), map_string(b->key));
}

/* every folder, and every folder in one, in a maps folder */
static long count_root(short root)
{
	struct _WIN32_FIND_DATAA *find = &hmp_maps_globals.find[0], *inner = &hmp_maps_globals.find[2];
	HANDLE handle = FindFirstFileA(root ? "T:\\maps\\*.*" : "d:\\maps\\*.*", find), handle2;
	long n = 0;
	if (handle == INVALID_FILE) return 0;
	do
	{
		if (!is_folder(find)) continue;
		n++;
		handle2 = FindFirstFileA(top_path(hmp_maps_globals.path, root, find->cFileName, "*.*"), inner);
		if (handle2 == INVALID_FILE) continue;
		do if (is_folder(inner)) n++; while (FindNextFileA(handle2, inner));
		CloseHandle(handle2);
	}
	while (FindNextFileA(handle, find));
	CloseHandle(handle);
	return n;
}

/* the maps in one maps folder (at most `maximum` in all) */
static void scan_root(short root, long maximum)
{
	struct _WIN32_FIND_DATAA *find = &hmp_maps_globals.find[0], *inner = &hmp_maps_globals.find[2];
	HANDLE handle = FindFirstFileA(root ? "T:\\maps\\*.*" : "d:\\maps\\*.*", find), handle2;
	long i, first;
	boolean series;
	if (handle == INVALID_FILE) return;
	do
	{
		char const *name = find->cFileName;
		if (!is_folder(find) || strlen(name) > MAXIMUM_FOLDER_NAME || hmp_maps_globals.map_count >= maximum) continue;
		if (add_folder(root, name, NULL)) continue;
		first = hmp_maps_globals.map_count;          /* no .map: a pack */
		series = FALSE;
		handle2 = FindFirstFileA(top_path(hmp_maps_globals.picture_path, root, name, "*.*"), inner);
		if (handle2 == INVALID_FILE) continue;
		do
		{
			if (is_series(inner)) series = TRUE;
			else if (is_folder(inner) && strlen(name) + 1 + strlen(inner->cFileName) <= MAXIMUM_FOLDER_NAME &&
				hmp_maps_globals.map_count < maximum)
			{
				add_folder(root, name, inner->cFileName);
			}
		}
		while (FindNextFileA(handle2, inner));
		CloseHandle(handle2);
		for (i = first; i < hmp_maps_globals.map_count; i++) hmp_maps_globals.maps[i].series = (char)series;
	}
	while (FindNextFileA(handle, find));
	CloseHandle(handle);
}

static boolean pack_has_series(short root, char const *key)
{
	struct _WIN32_FIND_DATAA *find = &hmp_maps_globals.find[2];
	char top[32];
	long k;
	boolean series = FALSE;
	HANDLE handle;
	for (key++, k = 0; key[k] && key[k] != '/' && k < 31; k++) top[k] = key[k];
	if (key[k] != '/') return FALSE;
	top[k] = 0;
	handle = FindFirstFileA(top_path(hmp_maps_globals.picture_path, root, top, "*.*"), find);
	if (handle == INVALID_FILE) return FALSE;
	do if (is_series(find)) series = TRUE; while (FindNextFileA(handle, find));
	CloseHandle(handle);
	return series;
}

/* the copy to use when a map is in both maps folders: the newer .map (the same age: the game's folder) */
static boolean newer(struct hmp_map const *a, struct hmp_map const *b)
{
	return a->map_time_high != b->map_time_high ? a->map_time_high > b->map_time_high :
		a->map_time_low != b->map_time_low ? a->map_time_low > b->map_time_low : a->root < b->root;
}

static void info_forget(long map_index)
{
	long k;
	if (!hmp_maps_globals.menu) return;
	for (k = 0; k < INFO_CACHE_COUNT; k++) if (map_index == NONE || hmp_maps_globals.menu->info[k].map_index == map_index)
		hmp_maps_globals.menu->info[k].map_index = NONE;
}

static void previews_forget(long map_index)
{
	long k;
	for (k = 0; k < PREVIEW_SLOTS; k++)
	{
		struct preview_slot *slot = &hmp_maps_globals.previews[k];
		if (slot->map_index == NONE || (map_index != NONE && slot->map_index != map_index)) continue;
		if (slot->bitmap.signature) texture_cache_bitmap_delete(&slot->bitmap);
		memset(slot, 0, sizeof(*slot));
		slot->map_index = NONE;
	}
}

/* map i's copy in maps folder `root` becomes the one used. TRUE = it's there */
static boolean set_root(long i, short root)
{
	struct hmp_map *map = &hmp_maps_globals.maps[i], copy;
	if (map->root == root) return TRUE;
	copy = *map;
	copy.root = (char)root;
	if (!list_folder(&copy)) return FALSE;
	copy.series = (char)(in_pack(i) && pack_has_series(root, map_string(copy.key)));
	copy.both = 1;
	*map = copy;
	info_forget(i);
	previews_forget(i);
	hmp_browse_forget(i);
	return TRUE;
}

/* ---------- private code: info.txt */

static boolean valid_utf8(unsigned char const *b, long n)
{
	long k = 0, m, t;
	while (k < n)
	{
		unsigned char c = b[k];
		m = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
		if (!m) return FALSE;
		if (k + m > n) return k + m - n < 4 && n == 1024;   /* (cut off by the read size) */
		for (t = 1; t < m; t++) if ((b[k + t] >> 6) != 2) return FALSE;
		k += m;
	}
	return TRUE;
}
/* a line that's the browser's ("Genres:", "Gametypes:", "Players:") */
static boolean browser_line(wchar_t const *s)
{
	static char const *const names[3] = { "genres:", "gametypes:", "players:" };
	long j, n;
	while (*s == ' ') s++;
	for (j = 0; j < 3; j++)
	{
		for (n = 0; names[j][n] && s[n] < 128 && lower(s[n]) == names[j][n]; n++) ;
		if (!names[j][n]) return TRUE;
	}
	return FALSE;
}
static long drop_browser_lines(wchar_t *d)
{
	wchar_t *o = d, *l = d;
	while (*l)
	{
		wchar_t *e = l;
		while (*e && *e != '\r') e++;
		if (!browser_line(l)) { while (l < e) *o++ = *l++; if (*e) *o++ = '\r'; }
		l = *e ? e + 1 : e;
	}
	*o = 0;
	return (long)(o - d);
}

/* map i's name into info->name, its description (unwrapped) into the menus' raw_description */
static void info_load(long i, struct hmp_info *info, struct hmp_menu_memory *menu)
{
	unsigned char *raw = menu->raw;
	unsigned long got = 0;
	long k = 0, nn = 0, nd = 0, ls = 0;
	boolean line = FALSE, utf16 = FALSE, ansi = FALSE;
	wchar_t *description = menu->raw_description;
	HANDLE file = INVALID_FILE;
	info->map_index = i;
	info->name[0] = description[0] = 0;
	if (hmp_maps_globals.maps[i].info_file != NONE)
	{
		folder_path(hmp_maps_globals.info_path, i, map_string(hmp_maps_globals.maps[i].info_file));
		file = open_read(hmp_maps_globals.info_path);
	}
	if (file != INVALID_FILE)
	{
		if (!ReadFile(file, raw, sizeof(menu->raw), &got, NULL)) got = 0;
		CloseHandle(file);
	}
	if (got >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) k = 3;
	else if (got >= 2 && raw[0] == 0xff && raw[1] == 0xfe) { k = 2; utf16 = TRUE; }
	else ansi = !valid_utf8(raw, (long)got);         /* Windows-1252 from an old editor: its Latin-1 part as is */
	while (k < (long)got)
	{
		unsigned long ch = raw[k++];
		if (utf16) { if (k >= (long)got) break; ch |= raw[k++] << 8; }
		else if (ansi || ch < 0x80) { }
		else if (ch >= 0xc0 && ch < 0xe0 && k < (long)got) ch = ((ch & 0x1f) << 6) | (raw[k++] & 0x3f);
		else if (ch >= 0xe0 && ch < 0xf0 && k + 1 < (long)got) { ch = ((ch & 0x0f) << 12) | ((raw[k] & 0x3f) << 6) | (raw[k + 1] & 0x3f); k += 2; }
		else { while (k < (long)got && (raw[k] >> 6) == 2) k++; ch = '?'; }   /* beyond Halo's fonts */
		if (ch == '\r') continue;
		if (ch == '|' && line)                       /* Halo's own "|n" line break */
		{
			if (utf16 && k + 1 < (long)got && raw[k] == 'n' && !raw[k + 1]) { k += 2; ch = '\n'; }
			else if (!utf16 && k < (long)got && raw[k] == 'n') { k++; ch = '\n'; }
		}
		if (ch == '\n')
		{
			if (!line) line = TRUE;
			else
			{
				description[nd] = 0;
				if (browser_line(description + ls)) nd = ls;   /* (takes no room from the description) */
				else if (nd && nd < 318) description[nd++] = '\r';
				ls = nd;
			}
			continue;
		}
		if (ch == '\t') ch = ' ';
		if (!line) { if (nn < 63) info->name[nn++] = (wchar_t)ch; }
		else if (nd < 318) description[nd++] = (wchar_t)ch;
	}
	while (nn && info->name[nn - 1] == ' ') nn--;
	description[nd] = 0;
	if (browser_line(description + ls)) description[nd = ls] = 0;
	nd = drop_browser_lines(description);
	while (nd && (description[nd - 1] == ' ' || description[nd - 1] == '\r')) nd--;
	info->name[nn] = 0;
	description[nd] = 0;
	if (!nn)                                         /* no info .txt: the folder's name */
	{
		char const *s = MAP_KEY(i) + 1, *q;
		for (q = s; *q; q++) if (*q == '/') s = q + 1;
		while (*s && nn < 63) info->name[nn++] = (wchar_t)(unsigned char)*s++;
		info->name[nn] = 0;
	}
}

static struct hmp_info *info_get(long i)
{
	struct hmp_menu_memory *menu = menu_memory();
	struct hmp_info *info;
	long k;
	if (!menu || i < 0 || i >= hmp_maps_globals.map_count) return NULL;
	for (k = 0; k < INFO_CACHE_COUNT; k++) if (menu->info[k].map_index == i) return &menu->info[k];
	info = &menu->info[menu->info_next];
	menu->info_next = (menu->info_next + 1) % INFO_CACHE_COUNT;
	info_load(i, info, menu);
	hmp_wrap_text(info->name, info->wrapped_name, NUMBEROF(info->wrapped_name), TRUE, NAME_TEXT_WIDTH);
	hmp_wrap_text(menu->raw_description, info->description, NUMBEROF(info->description), FALSE, DESCRIPTION_TEXT_WIDTH);
	return info;
}

/* ---------- private code: text */

static long glyph_width(boolean large, wchar_t c)
{
	struct extra_width const *extra = large ? large_extra_widths : small_extra_widths;
	long k, w;
	if (c >= 32 && c <= 255) { w = (large ? large_widths : small_widths)[c - 32]; return w > 0 ? w : 0; }
	for (k = 0; k < (long)NUMBEROF(large_extra_widths); k++) if (extra[k].code == c) return extra[k].width;
	return 0;
}

long hmp_text_width(wchar_t const *text, boolean large)
{
	long w = 0;
	while (*text) w += glyph_width(large, *text++);
	return w;
}

/* word wrap, as Halo Map Porter's card preview: breaks at spaces, splits a word wider than a line, keeps the breaks
there ('\r') */
void hmp_wrap_text(wchar_t const *s, wchar_t *out, long cap, boolean large, long max_width)
{
	long i = 0, k, n = 0, line_width, start;
#define PUT(c) do { if (n < cap - 1) out[n++] = (c); } while (0)
	for (;;)
	{
		line_width = 0;
		start = n;
		while (s[i] && s[i] != '\r')
		{
			long j = i, width = 0;
			boolean space = s[i] == ' ';
			while (s[j] && s[j] != '\r' && (s[j] == ' ') == space) width += glyph_width(large, s[j++]);
			if (space)
			{
				if (!s[j] || s[j] == '\r') { i = j; continue; }   /* (trailing spaces never start a line) */
				if (line_width + width > max_width) { PUT('\r'); start = n; line_width = 0; }
				else if (n > start) { for (k = i; k < j; k++) PUT(s[k]); line_width += width; }
			}
			else if (line_width + width <= max_width)
			{
				for (k = i; k < j; k++) PUT(s[k]);
				line_width += width;
			}
			else
			{
				if (n > start)
				{
					while (n > start && out[n - 1] == ' ') n--;
					PUT('\r'); start = n; line_width = 0;
				}
				for (k = i; k < j; k++)
				{
					long cw = glyph_width(large, s[k]);
					if (line_width + cw > max_width && n > start) { PUT('\r'); start = n; line_width = 0; }
					PUT(s[k]);
					line_width += cw;
				}
			}
			i = j;
		}
		while (n > start && out[n - 1] == ' ') n--;
		if (!s[i]) break;
		PUT('\r');
		i++;
	}
#undef PUT
	out[n] = 0;
}

/* a card's description: its group in the current sort, its favorite / hidden mark, a blank line, the description; or,
while Black is held on the map, what holding it does */
static wchar_t *compose(long entry, wchar_t const *description)
{
	struct hmp_menu_memory *menu = menu_memory();
	char const *entry_name = entry < HMP_STOCK_LEVELS ? hmp_maps_globals.stock_levels[entry] : MAP_KEY(entry - HMP_STOCK_LEVELS);
	wchar_t *o, *start, *end, line[48];
	wchar_t const *t;
	long k, position = NONE, n;
	short black;
	if (!menu) return (wchar_t *)description;
	start = o = menu->composed[menu->composed_next = (menu->composed_next + 1) % 3];
	end = o + NUMBEROF(menu->composed[0]) - 1;
	if ((black = hmp_browse_black(entry_name)) != 0)
	{
		wchar_t const *mark = hmp_browse_mark(entry_name);
		boolean hidden = FALSE;
		char const *q;
		for (k = 0; mark[k]; k++) if (mark[k] == 'H') hidden = TRUE;
		if (black == 1) q = hidden ? "Keep holding Black\rto show this map\ragain" : "Keep holding Black\rto hide this map";
		else q = hidden ? "Hold Black to show\rthis map again" : "Hold Black to hide\rthis map";
		while (*q) *o++ = (unsigned char)*q++;
		*o = 0;
		return start;
	}
	for (k = 0; k < 3; k++) if (hmp_maps_globals.visible_entry[k] == entry) { position = hmp_maps_globals.visible_position[k]; break; }
	if (position != NONE && (t = hmp_browse_label(position))[0])
	{
		hmp_wrap_text(t, o, (long)(end - o), FALSE, DESCRIPTION_TEXT_WIDTH);
		while (*o) o++;
		*o++ = '\r';
	}
	t = hmp_browse_mark(entry_name);
	if (t[0])
	{
		for (n = 0; t[n] && n < 47; n++) line[n] = t[n];
		line[n] = 0;
		hmp_wrap_text(line, o, (long)(end - o), FALSE, DESCRIPTION_TEXT_WIDTH);
		while (*o) o++;
		*o++ = '\r';
	}
	if (o > start) *o++ = '\r';
	for (t = description; t && *t && o < end; ) *o++ = *t++;
	*o = 0;
	return start;
}

/* ---------- private code: pictures */

static long picture_frame(long i)
{
	return i >= 0 && i < hmp_maps_globals.map_count && hmp_maps_globals.maps[i].picture ? HMP_NAME_BASE + i : HMP_UNKNOWN_PICTURE;
}

void hmp_bitmap_setup(struct bitmap_data *bitmap, long fake_index, short width, short height, short format,
	unsigned short flags, long size)
{
	memset(bitmap, 0, sizeof(*bitmap));
	bitmap->signature = 'bitm';
	bitmap->width = width;
	bitmap->height = height;
	bitmap->depth = 1;
	bitmap->type = 0;
	bitmap->format = format;
	bitmap->flags = flags;
	bitmap->registration_point.x = width / 2;
	bitmap->registration_point.y = height / 2;
	bitmap->pixels_size = size;
	bitmap->tag_index = FAKE_TAG | fake_index;
	bitmap->cache_block_index = NONE;
}

/* the preview bitmap of folder map i: one of the few kept (the least recently drawn is reused), set up as Halo's own
map pictures are (256x128 DXT1, cached); the texture cache reads it with hmp_texture_read */
static struct bitmap_data *preview_bitmap(long i)
{
	struct preview_slot *slot = NULL, *oldest = NULL;
	long k;
	hmp_maps_globals.preview_clock++;
	for (k = 0; k < PREVIEW_SLOTS; k++)
	{
		struct preview_slot *s = &hmp_maps_globals.previews[k];
		if (s->map_index == i) { slot = s; break; }
		if (!oldest || (s->map_index == NONE && oldest->map_index != NONE) ||
			(s->map_index != NONE && oldest->map_index != NONE && s->last_used < oldest->last_used))
		{
			oldest = s;
		}
	}
	if (!slot)
	{
		slot = oldest;
		if (slot->bitmap.signature) texture_cache_bitmap_delete(&slot->bitmap);
		/* (power-of-two dimensions, compressed, cached) */
		hmp_bitmap_setup(&slot->bitmap, i, 256, 128, 14 /* dxt1 */, 0x83, PREVIEW_SIZE);
		slot->map_index = i;
	}
	slot->last_used = hmp_maps_globals.preview_clock;
	return &slot->bitmap;
}

/* ---------- private code: series.bin */

static void rgb565(unsigned long c, long *o)
{
	o[0] = (c >> 11 & 31) * 255 / 31;
	o[1] = (c >> 5 & 63) * 255 / 63;
	o[2] = (c & 31) * 255 / 31;
}
static unsigned long to565(long const *c)
{
	return (unsigned long)((c[0] * 31 + 127) / 255) << 11 | (unsigned long)((c[1] * 63 + 127) / 255) << 5 |
		(unsigned long)((c[2] * 31 + 127) / 255);
}
static void dxt1_palette(unsigned long c0, unsigned long c1, long palette[4][3])
{
	long c;
	rgb565(c0, palette[0]);
	rgb565(c1, palette[1]);
	for (c = 0; c < 3; c++)
	{
		if (c0 > c1)
		{
			palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
			palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
		}
		else
		{
			palette[2][c] = (palette[0][c] + palette[1][c]) / 2;
			palette[3][c] = 0;
		}
	}
}
static void dxt1_decode(unsigned char const *b, long pixels[16][3])
{
	unsigned long bits = b[4] | b[5] << 8 | b[6] << 16 | (unsigned long)b[7] << 24;
	long palette[4][3], k, c;
	dxt1_palette(b[0] | b[1] << 8, b[2] | b[3] << 8, palette);
	for (k = 0; k < 16; k++) for (c = 0; c < 3; c++) pixels[k][c] = palette[bits >> (2 * k) & 3][c];
}
/* the colour box's diagonal that follows the pixels, pulled in by 1/16 at each end; each pixel the nearest colour */
static void dxt1_encode(long pixels[16][3], unsigned char *b)
{
	long low[3], high[3], mean[3], palette[4][3], k, c, t;
	long covariance[3] = { 0, 0, 0 };
	unsigned long c0, c1, bits = 0;
	for (c = 0; c < 3; c++)
	{
		low[c] = 255; high[c] = 0; mean[c] = 0;
		for (k = 0; k < 16; k++) { t = pixels[k][c]; if (t < low[c]) low[c] = t; if (t > high[c]) high[c] = t; mean[c] += t; }
		mean[c] >>= 4;
	}
	for (k = 0; k < 16; k++) for (c = 1; c < 3; c++) covariance[c] += (pixels[k][0] - mean[0]) * (pixels[k][c] - mean[c]);
	for (c = 1; c < 3; c++) if (covariance[c] < 0) { t = low[c]; low[c] = high[c]; high[c] = t; }
	for (c = 0; c < 3; c++) { t = (high[c] - low[c]) / 16; high[c] -= t; low[c] += t; }
	c0 = to565(high);
	c1 = to565(low);
	if (c0 < c1) { unsigned long x = c0; c0 = c1; c1 = x; }
	if (c0 != c1)
	{
		dxt1_palette(c0, c1, palette);
		for (k = 0; k < 16; k++)
		{
			long best = 0, best_distance = 0x7fffffff, s;
			for (s = 0; s < 4; s++)
			{
				long d = 0;
				for (c = 0; c < 3; c++) { t = pixels[k][c] - palette[s][c]; d += t * t; }
				if (d < best_distance) { best_distance = d; best = s; }
			}
			bits |= (unsigned long)best << (2 * k);
		}
	}
	b[0] = (unsigned char)c0; b[1] = (unsigned char)(c0 >> 8); b[2] = (unsigned char)c1; b[3] = (unsigned char)(c1 >> 8);
	b[4] = (unsigned char)bits; b[5] = (unsigned char)(bits >> 8); b[6] = (unsigned char)(bits >> 16); b[7] = (unsigned char)(bits >> 24);
}
/* stamps the series image (got bytes) on a 256x128 DXT1 texture. TRUE = stamped */
static boolean series_stamp(unsigned char *texture, unsigned char const *series, long got)
{
	long w, h, x0, y0, bx, by, k, c;
	if (got < 16 || series[0] != 'H' || series[1] != 'U' || series[2] != 'S' || series[3] != 'B') return FALSE;
	w = series[4] | series[5] << 8; h = series[6] | series[7] << 8; x0 = series[8] | series[9] << 8; y0 = series[10] | series[11] << 8;
	if (w < 1 || w > 64 || h < 1 || h > 64 || x0 + w > 256 || y0 + h > 128 || got != 16 + w * h * 4) return FALSE;
	for (by = y0 >> 2; by <= (y0 + h - 1) >> 2; by++)
	{
		for (bx = x0 >> 2; bx <= (x0 + w - 1) >> 2; bx++)
		{
			unsigned char *block = texture + (by * 64 + bx) * 8;
			long pixels[16][3];
			dxt1_decode(block, pixels);
			for (k = 0; k < 16; k++)
			{
				long x = bx * 4 + (k & 3) - x0, y = by * 4 + (k >> 2) - y0, a;
				unsigned char const *q;
				if (x < 0 || x >= w || y < 0 || y >= h) continue;
				q = series + 16 + (y * w + x) * 4;   /* blue, green, red, alpha */
				a = q[3];
				for (c = 0; c < 3; c++) pixels[k][c] = (pixels[k][c] * (255 - a) + q[2 - c] * a + 127) / 255;
			}
			dxt1_encode(pixels, block);
		}
	}
	return TRUE;
}
static void stamp_series(long i, unsigned char *texture)
{
	unsigned long got = 0;
	HANDLE file;
	unsigned char *series = (unsigned char *)LocalAlloc(0, SERIES_MAXIMUM_SIZE);
	if (!series) return;
	file = open_read(pack_path(hmp_maps_globals.picture_path, i, "series.bin"));
	if (file != INVALID_FILE)
	{
		if (!ReadFile(file, series, SERIES_MAXIMUM_SIZE, &got, NULL)) got = 0;
		CloseHandle(file);
		series_stamp(texture, series, (long)got);
	}
	LocalFree(series);
}

/* ---------- public code */

char const *cache_files_loaded_map_name(void);
/* the menus are up (the main menu's map, or none loaded yet) */
static boolean hmp_menus_running(void)
{
	char const *map = cache_files_loaded_map_name();
	return !map || !map[0] || !ieq(map, "ui");
}

void hmp_maps_scan(void)
{
	long i, j, n, old_count = hmp_maps_globals.map_count;
	struct hmp_map *old_maps = hmp_maps_globals.maps;
	char *old_names = hmp_maps_globals.names;
	unsigned long start = GetTickCount();

	if (!hmp_maps_globals.scanned)
	{
		for (i = 0; i < HMP_STOCK_LEVELS; i++) if (!hmp_maps_globals.stock_levels[i]) hmp_maps_globals.stock_levels[i] = default_stock_levels[i];
		for (i = 0; i < PREVIEW_SLOTS; i++) hmp_maps_globals.previews[i].map_index = NONE;
	}
	hmp_maps_globals.scanned = TRUE;
	previews_forget(NONE);
	info_forget(NONE);
	hmp_maps_globals.maps = NULL;
	hmp_maps_globals.map_count = 0;
	hmp_maps_globals.names = NULL;
	hmp_maps_globals.names_size = hmp_maps_globals.names_capacity = 0;
	hmp_maps_globals.scan_t = hmp_maps_globals.scan_both = 0;
	n = count_root(0) + count_root(1);
	if (n > MAXIMUM_FOLDER_MAPS) n = MAXIMUM_FOLDER_MAPS;
	if (n && (hmp_maps_globals.maps = (struct hmp_map *)LocalAlloc(LMEM_ZEROINIT, (unsigned int)n * sizeof(struct hmp_map))) != NULL)
	{
		scan_root(0, n);
		scan_root(1, n);
		/* each pack's order.txt (the game's folder's first) */
		for (i = 0; i < hmp_maps_globals.map_count; i++)
		{
			struct hmp_map *map = &hmp_maps_globals.maps[i];
			char top[32];
			if (map->mark || !in_pack(i)) continue;
			for (j = 0; MAP_KEY(i)[j + 1] != '/' && j < 31; j++) top[j] = MAP_KEY(i)[j + 1];
			top[j] = 0;
			hmp_maps_globals.pack_key = MAP_KEY(i);
			hmp_maps_globals.pack_position = 0;
			read_lines(top_path(hmp_maps_globals.picture_path, 0, top, "order.txt"), pack_line);
			read_lines(top_path(hmp_maps_globals.picture_path, 1, top, "order.txt"), pack_line);
			for (j = i; j < hmp_maps_globals.map_count; j++)
				if (!top_compare(MAP_KEY(j) + 1, MAP_KEY(i) + 1)) hmp_maps_globals.maps[j].mark = 1;
		}
		/* in list order (insertion sort) */
		for (i = 1; i < hmp_maps_globals.map_count; i++)
		{
			struct hmp_map t = hmp_maps_globals.maps[i];
			for (j = i - 1; j >= 0 && map_compare(&hmp_maps_globals.maps[j], &t) > 0; j--) hmp_maps_globals.maps[j + 1] = hmp_maps_globals.maps[j];
			hmp_maps_globals.maps[j + 1] = t;
		}
		for (i = 0; i < hmp_maps_globals.map_count; i++) if (hmp_maps_globals.maps[i].root) hmp_maps_globals.scan_t++;
		/* a map in both folders (side by side now): one entry */
		for (i = j = 0; i < hmp_maps_globals.map_count; i++)
		{
			if (j && !ieq(MAP_KEY(j - 1), MAP_KEY(i)))
			{
				struct hmp_map *kept = &hmp_maps_globals.maps[j - 1];
				if (newer(&hmp_maps_globals.maps[i], kept)) *kept = hmp_maps_globals.maps[i];
				kept->both = 1;
				hmp_maps_globals.scan_both++;
				continue;
			}
			if (j != i) hmp_maps_globals.maps[j] = hmp_maps_globals.maps[i];
			j++;
		}
		hmp_maps_globals.map_count = j;
		/* a copy map sharing chose earlier stays chosen (Halo may have it cached) */
		for (i = 0; i < hmp_maps_globals.map_count; i++)
		{
			hmp_maps_globals.maps[i].mark = 0;
			if (!hmp_maps_globals.maps[i].both || !old_maps) continue;
			for (j = 0; j < old_count; j++)
			{
				if (old_maps[j].pinned && !ieq(old_names + old_maps[j].key, MAP_KEY(i)))
				{
					if (set_root(i, old_maps[j].root)) hmp_maps_globals.maps[i].pinned = 1;
					break;
				}
			}
		}
	}
	if (old_maps) LocalFree(old_maps);
	if (old_names) LocalFree(old_names);
	/* (the list is the menus': during a game it's built when the menus come back) */
	if (hmp_menus_running())
	{
		hmp_browse_scan(hmp_maps_globals.map_count);
		build_list();
	}
	else if (hmp_maps_globals.table)
	{
		LocalFree(hmp_maps_globals.table);
		hmp_maps_globals.table = NULL;
		hmp_maps_globals.table_count = hmp_maps_globals.table_capacity = 0;
	}
	for (i = 0; i < HMP_STOCK_LEVELS; i++) hmp_stats_seen(hmp_maps_stock_name(i));
	for (i = 0; i < hmp_maps_globals.map_count; i++) hmp_stats_seen(MAP_KEY(i));
	hmp_log("maps: %d folder maps (%d in T:\\maps, %d in both), %d KB of names, in %d ms", hmp_maps_globals.map_count,
		hmp_maps_globals.scan_t, hmp_maps_globals.scan_both, (hmp_maps_globals.names_capacity + 1023) / 1024,
		GetTickCount() - start);
}

static void scan_if_needed(void)
{
	if (!hmp_maps_globals.scanned) hmp_maps_scan();
	else if (!hmp_maps_globals.table) build_list();   /* (freed while a game ran) */
}

long hmp_maps_count(void) { return hmp_maps_globals.map_count; }
char const *hmp_maps_key(long i) { return i >= 0 && i < hmp_maps_globals.map_count ? MAP_KEY(i) : NULL; }
char const *hmp_maps_stock_level(long k)
{
	return k < 0 || k >= HMP_STOCK_LEVELS ? NULL : hmp_maps_globals.stock_levels[k] ? hmp_maps_globals.stock_levels[k] : default_stock_levels[k];
}
char **hmp_maps_table(void) { return hmp_maps_globals.table; }
long hmp_maps_table_count(void) { return hmp_maps_globals.table_count; }
void hmp_maps_table_set_count(long count) { hmp_maps_globals.table_count = count; }
long hmp_maps_table_capacity(void) { return hmp_maps_globals.table_capacity; }
long hmp_maps_visible_position(short card) { return card >= 0 && card < 3 ? hmp_maps_globals.visible_position[card] : NONE; }

long hmp_maps_find(char const *name)
{
	long i;
	if (!name || name[0] != '+') return NONE;
	if (!hmp_maps_globals.scanned) hmp_maps_scan();
	for (i = 0; i < hmp_maps_globals.map_count; i++) if (!ieq(MAP_KEY(i), name)) return i;
	return NONE;
}

long hmp_maps_entry(char const *s)
{
	long k;
	if (!s) return NONE;
	if (s >= hmp_maps_globals.names && s < hmp_maps_globals.names + hmp_maps_globals.names_size)
	{
		long i, offset = (long)(s - hmp_maps_globals.names);
		for (i = 0; i < hmp_maps_globals.map_count; i++) if (hmp_maps_globals.maps[i].key == offset) return HMP_STOCK_LEVELS + i;
	}
	for (k = 0; k < HMP_STOCK_LEVELS; k++) if (s == hmp_maps_globals.stock_levels[k]) return k;
	return NONE;
}

long hmp_maps_read_info(long i, unsigned char *buffer, long size)
{
	unsigned long got = 0;
	HANDLE file;
	if (i < 0 || i >= hmp_maps_globals.map_count || hmp_maps_globals.maps[i].info_file == NONE) return 0;
	folder_path(hmp_maps_globals.info_path, i, map_string(hmp_maps_globals.maps[i].info_file));
	file = open_read(hmp_maps_globals.info_path);
	if (file == INVALID_FILE) return 0;
	if (!ReadFile(file, buffer, (unsigned long)size, &got, NULL)) got = 0;
	CloseHandle(file);
	return (long)got;
}

boolean hmp_maps_same_pack(char const *a, char const *b)
{
	long x = hmp_maps_entry(a), y = hmp_maps_entry(b);
	if (x == NONE || y == NONE) return FALSE;
	if (x < HMP_STOCK_LEVELS || y < HMP_STOCK_LEVELS) return x < HMP_STOCK_LEVELS && y < HMP_STOCK_LEVELS;
	x -= HMP_STOCK_LEVELS;
	y -= HMP_STOCK_LEVELS;
	if (in_pack(x) != in_pack(y)) return FALSE;
	return !in_pack(x) || !top_compare(MAP_KEY(x) + 1, MAP_KEY(y) + 1);
}

wchar_t const *hmp_maps_title(char const *level_name)
{
	long i = hmp_maps_find(level_name);
	struct hmp_info *info = info_get(i);
	return info ? info->name : NULL;
}

boolean hmp_map_file_path(char const *level_name, char *path, long size)
{
	long i;
	char buffer[300];
	if (!level_name || level_name[0] != '+') return FALSE;
	i = hmp_maps_find(level_name);
	if (i == NONE) buffer[0] = 0;          /* (a folder map this console hasn't: no file) */
	else folder_path(buffer, i, map_string(hmp_maps_globals.maps[i].map_file));
	if ((long)strlen(buffer) >= size) buffer[0] = 0;
	strcpy(path, buffer);
	if (!path[0]) strcpy(path, "d:\\maps\\+missing+.map");   /* (a path no file has) */
	return TRUE;
}

char const *hmp_maps_file(long i, short which)
{
	struct hmp_map *map;
	if (i < 0 || i >= hmp_maps_globals.map_count) return NULL;
	map = &hmp_maps_globals.maps[i];
	switch (which)
	{
	case 0: return map_string(map->map_file);
	case 1: return map->info_file != NONE ? map_string(map->info_file) : NULL;
	case 2: return map->picture ? "preview.bin" : NULL;
	default: return map->series ? "series.bin" : NULL;
	}
}
short hmp_maps_root(long i) { return i >= 0 && i < hmp_maps_globals.map_count ? hmp_maps_globals.maps[i].root : 0; }
boolean hmp_maps_both(long i) { return i >= 0 && i < hmp_maps_globals.map_count && hmp_maps_globals.maps[i].both; }

boolean hmp_maps_map_path(long i, short root, char *path, long size)
{
	struct hmp_map copy;
	char buffer[300];
	long names_size = hmp_maps_globals.names_size;
	if (i < 0 || i >= hmp_maps_globals.map_count) return FALSE;
	if (root == NONE || root == hmp_maps_globals.maps[i].root)
	{
		folder_path(buffer, i, map_string(hmp_maps_globals.maps[i].map_file));
	}
	else
	{
		copy = hmp_maps_globals.maps[i];
		copy.root = (char)root;
		if (!list_folder(&copy)) { hmp_maps_globals.names_size = names_size; return FALSE; }
		key_path(buffer, root, map_string(copy.key), map_string(copy.map_file));
		hmp_maps_globals.names_size = names_size;    /* (the names just looked at aren't kept) */
	}
	if ((long)strlen(buffer) >= size) return FALSE;
	strcpy(path, buffer);
	return TRUE;
}

boolean hmp_maps_use_root(long i, short root)
{
	if (i < 0 || i >= hmp_maps_globals.map_count || !set_root(i, root)) return FALSE;
	hmp_maps_globals.maps[i].pinned = 1;
	return TRUE;
}

void hmp_menu_memory_release(void)
{
	long before = (hmp_maps_globals.menu ? (long)sizeof(struct hmp_menu_memory) : 0) + hmp_maps_globals.table_capacity * 4;
	if (hmp_maps_globals.menu) { LocalFree(hmp_maps_globals.menu); hmp_maps_globals.menu = NULL; }
	if (hmp_maps_globals.table) { LocalFree(hmp_maps_globals.table); hmp_maps_globals.table = NULL; }
	hmp_maps_globals.table_count = hmp_maps_globals.table_capacity = 0;
	hmp_browse_scan(NONE);                      /* (the browser's tables too) */
	previews_forget(NONE);
	if (before) hmp_log("maps: the menus' memory freed for the game (%d KB of map list and text)", (before + 1023) / 1024);
}

/* the main menu's words OpenCE changes (hmp_game.c): GAME DEMOS shown as OPTIONS, SETTINGS as PROFILES (pictures
made from ui.map's letters by port/oxbox/tools/mkword.py, kept in the XBE's file past what the loader loads: xbox/oxbox_entry.c
oxbox_picture_read). TRUE when this bitmap is one of them, read */
static boolean menu_word_read(struct bitmap_data *bitmap, unsigned char *buffer, long size)
{
	extern int oxbox_picture_read(long index, void *buffer, unsigned long size);
	char const *name;
	long word, frame;
	if (bitmap->width != 256 || bitmap->height != 64 || bitmap->format != 15 /* DXT3 */ || size != 16384 ||
		bitmap->tag_index == NONE) return FALSE;
	name = tag_get_name(bitmap->tag_index);
	if (!csstrcasecmp(name, "ui\\shell\\main_menu\\menu_game_demos")) word = 0;
	else if (!csstrcasecmp(name, "ui\\shell\\main_menu\\menu_settings")) word = 1;
	else return FALSE;
	frame = bitmap == bitmap_group_get_bitmap_from_sequence(bitmap->tag_index, 0, 1) ? 1 : 0;
	/* (from the XBE's file, straight into the texture: not kept in memory. Unreadable: Halo's own word) */
	return oxbox_picture_read(word * 2 + frame, buffer, (unsigned long)size) != 0;
}

boolean hmp_texture_read(struct bitmap_data *bitmap, void *buffer, long size)
{
	long i = bitmap->tag_index & 0xffff, k;
	unsigned long got = 0;
	HANDLE file = INVALID_FILE;
	if ((bitmap->tag_index & 0xffff0000) != FAKE_TAG) return menu_word_read(bitmap, (unsigned char *)buffer, size);
	if (i >= FAKE_ICON_BASE)
	{
		if (!hmp_browse_icon_read(i - FAKE_ICON_BASE, buffer, size)) memset(buffer, 0, size);
		return TRUE;
	}
	if (size > PREVIEW_SIZE) size = PREVIEW_SIZE;
	if (i < hmp_maps_globals.map_count)
	{
		folder_path(hmp_maps_globals.picture_path, i, "preview.bin");
		file = open_read(hmp_maps_globals.picture_path);
	}
	if (file != INVALID_FILE)
	{
		if (!ReadFile(file, buffer, (unsigned long)size, &got, NULL)) got = 0;
		CloseHandle(file);
	}
	for (k = (long)got; k < size; k++) ((unsigned char *)buffer)[k] = 0;   /* (unreadable: black) */
	if (got == PREVIEW_SIZE && hmp_maps_globals.maps[i].series) stamp_series(i, (unsigned char *)buffer);
	return TRUE;
}

/* ---------- public code: OpenCE's hooks for the menus' maps (custom_edition_maps.h) */

char **custom_edition_maps_level_list(char **xbox_levels, short xbox_level_count, short *level_count)
{
	long k;
	for (k = 0; k < HMP_STOCK_LEVELS && k < xbox_level_count; k++) hmp_maps_globals.stock_levels[k] = xbox_levels[k];
	scan_if_needed();
	if (!hmp_maps_globals.table)
	{
		*level_count = xbox_level_count;
		return xbox_levels;
	}
	*level_count = (short)hmp_maps_globals.table_count;
	return hmp_maps_globals.table;
}

void custom_edition_maps_look_again(void)
{
	/* (the folders are read once, and again after a download: hmp_share.c) */
	scan_if_needed();
}

short custom_edition_maps_level_display_index(short position)
{
	long entry = position >= 0 && position < hmp_maps_globals.table_count ? hmp_maps_entry(hmp_maps_globals.table[position]) : NONE;
	return entry == NONE ? position : entry < HMP_STOCK_LEVELS ? (short)entry : (short)(HMP_NAME_BASE + entry - HMP_STOCK_LEVELS);
}

/* the map list's three cards: card `card` shows list position `position`: its name, picture and description */
void hmp_maps_list_item(short card, long position, short *name, short *frame, short *description)
{
	long entry = position >= 0 && position < hmp_maps_globals.table_count ? hmp_maps_entry(hmp_maps_globals.table[position]) : NONE;
	if (card == 0)
	{
		long k;
		for (k = 0; k < 3; k++) hmp_maps_globals.visible_entry[k] = hmp_maps_globals.visible_position[k] = NONE;
	}
	if (card < 0 || card >= 3 || entry == NONE) return;
	hmp_maps_globals.visible_entry[card] = entry;
	hmp_maps_globals.visible_position[card] = position;
	if (entry < HMP_STOCK_LEVELS)                /* a stock map, wherever order.txt put it */
	{
		*name = (short)entry;
		*frame = (short)entry;
		*description = (short)(HMP_STOCK_DESCRIPTION_BASE + entry);
	}
	else
	{
		*name = (short)(HMP_WRAPPED_NAME_BASE + entry - HMP_STOCK_LEVELS);
		*frame = (short)picture_frame(entry - HMP_STOCK_LEVELS);
		*description = (short)(HMP_DESCRIPTION_BASE + entry - HMP_STOCK_LEVELS);
	}
}

/* a map this console hasn't got: a game in the system link browser, or the lobby before the map arrives. Its name is
the key's last folder ("+pack/folder" -> "folder"), kept in a small ring so the browser's games each keep theirs */
static struct
{
	char key[48];
	wchar_t name[32];
} missing_names[HMP_MISSING_NAMES];
static long missing_names_next;

static void key_folder_name(char const *key, wchar_t *name, long size)
{
	char const *start = key, *p;
	long k;
	if (*start == '+') start++;
	for (p = start; *p; p++) if (*p == '/' || *p == '\\') start = p + 1;
	for (k = 0; start[k] && k < size - 1; k++) name[k] = (wchar_t)(unsigned char)start[k];
	name[k] = 0;
}

static short missing_name_index(char const *key)
{
	long i;
	for (i = 0; i < HMP_MISSING_NAMES; i++)
		if (missing_names[i].key[0] && !csstrncmp(missing_names[i].key, key, sizeof(missing_names[i].key) - 1))
			return (short)(HMP_MISSING_NAME_BASE + i);
	i = missing_names_next;
	missing_names_next = (missing_names_next + 1) % HMP_MISSING_NAMES;
	csstrncpy(missing_names[i].key, key, sizeof(missing_names[i].key) - 1);
	missing_names[i].key[sizeof(missing_names[i].key) - 1] = 0;
	key_folder_name(key, missing_names[i].name, NUMBEROF(missing_names[i].name));
	return (short)(HMP_MISSING_NAME_BASE + i);
}

short custom_edition_maps_display_index(char const *level_name)
{
	long i;
	if (!level_name || level_name[0] != '+') return NONE;
	if (hmp_share_status_active()) return HMP_LIVE_NAME;
	i = hmp_maps_find(level_name);
	if (i != NONE) return (short)(HMP_NAME_BASE + i);
	return missing_name_index(level_name);
}

boolean custom_edition_maps_campaign(short display_index) { return FALSE; }
short custom_edition_maps_campaign_level(short display_index) { return NONE; }
boolean custom_edition_maps_level_campaign(char const *level_name) { return FALSE; }
short custom_edition_maps_count(boolean campaign) { return 0; }
short custom_edition_maps_display_index_of(boolean campaign, short index) { return NONE; }
char const *custom_edition_maps_level_name(short display_index) { return NULL; }
wchar_t *custom_edition_maps_name(short display_index) { return hmp_string(NONE, display_index); }
wchar_t *custom_edition_maps_description(short display_index) { return hmp_string(NONE, display_index); }

/* the lobby's map name while a folder map is up: map sharing's status, else its name (or its folder's while this
console hasn't the map) */
static wchar_t *live_name(void);

wchar_t *hmp_string(long tag_index, short index)
{
	struct hmp_info *info;
	long i;
	if (index < HMP_NAME_BASE) return NULL;
	if (index == HMP_LIVE_NAME) return live_name();
	if (index >= HMP_STOCK_DESCRIPTION_BASE && index < HMP_STOCK_DESCRIPTION_BASE + HMP_STOCK_LEVELS)
	{
		return compose(index - HMP_STOCK_DESCRIPTION_BASE, tag_index != NONE ?
			unicode_string_list_get_string(tag_index, index - HMP_STOCK_DESCRIPTION_BASE) : L"");
	}
	if (index >= HMP_STRING_BASE && index < HMP_STOCK_DESCRIPTION_BASE) return hmp_game_string(index);
	if (index >= HMP_MISSING_NAME_BASE && index < HMP_MISSING_NAME_BASE + HMP_MISSING_NAMES)
		return missing_names[index - HMP_MISSING_NAME_BASE].name;
	if (index >= 0x7000) return NULL;
	i = index - (index >= HMP_DESCRIPTION_BASE ? HMP_DESCRIPTION_BASE : index >= HMP_WRAPPED_NAME_BASE ? HMP_WRAPPED_NAME_BASE : HMP_NAME_BASE);
	if (!(info = info_get(i))) return NULL;
	if (index >= HMP_DESCRIPTION_BASE) return compose(HMP_STOCK_LEVELS + i, info->description);
	return index >= HMP_WRAPPED_NAME_BASE ? info->wrapped_name : info->name;
}

char const *network_game_current_map_name(void);

static wchar_t *live_name(void)
{
	char const *map = network_game_current_map_name();
	long i, k;
	if (hmp_share_status_active()) return hmp_share_status_text();
	i = map ? hmp_maps_find(map) : NONE;
	if (i != NONE) return hmp_string(NONE, (short)(HMP_NAME_BASE + i));
	(void)k;
	if (map && map[0]) key_folder_name(map, hmp_maps_globals.key_name, NUMBEROF(hmp_maps_globals.key_name));
	else hmp_maps_globals.key_name[0] = 0;
	return hmp_maps_globals.key_name;
}

struct bitmap_data *custom_edition_maps_picture(long bitmap_tag_index, short *frame_index)
{
	long i = *frame_index - HMP_NAME_BASE;
	if (*frame_index < HMP_NAME_BASE || bitmap_tag_index == NONE || csstrcasecmp(tag_get_name(bitmap_tag_index), LEVEL_PICTURES_TAG_NAME))
		return NULL;
	*frame_index = HMP_UNKNOWN_PICTURE;
	if (i < 0 || i >= hmp_maps_globals.map_count || !hmp_maps_globals.maps[i].picture) return NULL;
	return preview_bitmap(i);
}

#endif /* HALO_XBOX_TARGET */
