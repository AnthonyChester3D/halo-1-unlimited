/* packs.c: importing a map pack (another Halo's folder on the hard drive: a modded Halo's maps) into Halo 1
Unlimited (packs.h). Nothing is copied or changed in the pack's folder: the game plays its .map files where they are.
What's written goes in the game's own storage (E:\TDATA\4c4a4855, the game's T:):

	maps\<pack>\                  a pack in the game's map list (port/linux/game/hmp_maps.c)
		imported.txt              "From: <the Halo folder>", what the installer finds the import by
		order.txt                 the pack's maps in its own menu's order
		<map>\map.link            the .map's path ("F:\Games\Halo NHE\maps\atlas.map")
		<map>\info.txt            its name and description, from the pack's ui.map
		<map>\preview.bin         its picture, from the pack's ui.map (256x128 DXT1, as Halo's own)
	campaigns\<pack>\campaign.txt "Name:", "From:", then for each level "Level:" (its file), "Title:", "Text:"
	campaigns\<pack>\<level>.bin  its picture

The names, descriptions and pictures are the pack's ui.map's: the multiplayer maps' (mp_map_list, mp_map_select's
map_data, mp_map_grafix) in the order of the pack's XBE's multiplayer levels (a run of pointers to "levels\..."
names: Halo's 13 in a retail XBE, more in a modded one), the campaign's (map_list, solo_level_select's map_data,
sp_levels) in Halo's level order. A map Halo has already (Halo's 13, or a file the same as the install's) isn't
imported. */
#include "packs.h"
#include "pictures.h"
#include "text.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE "E:\\TDATA\\4c4a4855"
#define MAP_HEADER 2048
#define MAP_BASE 0x803A6000u
#define MAXIMUM_FOLDER_NAME 30 /* (the game's: a pack's name and its map's together, hmp_maps.c) */
#define PICTURE_SIZE 16384
#define CAMPAIGN_LEVELS 10
#define XBE_LEVELS 64
#define TEXT_NAME 128
#define TEXT_DESCRIPTION 600

static char const *const campaign_order[CAMPAIGN_LEVELS] = {"a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40",
	"d20", "d40"};
static char const *const halos_order[13] = {"beavercreek", "sidewinder", "damnation", "ratrace", "prisoner",
	"hangemhigh", "chillout", "carousel", "boardingaction", "bloodgulch", "wizard", "putput", "longest"};
/* Halo's own multiplayer maps (the Xbox's, any region): their headers' length and checksum */
static struct
{
	char const *name;
	unsigned long length, checksum;
} const halos_maps[13] = {
	{"beavercreek", 0x0298c000ul, 0xa831a7bful}, {"bloodgulch", 0x02a46800ul, 0x035e2da3ul},
	{"boardingaction", 0x02b3ee00ul, 0xa0d614f5ul}, {"carousel", 0x02867a00ul, 0xb38b62c7ul},
	{"chillout", 0x02805e00ul, 0x086c74adul}, {"damnation", 0x02dbf200ul, 0x911cd6eeul},
	{"hangemhigh", 0x029ad000ul, 0x8313bd85ul}, {"longest", 0x02830800ul, 0xdf1deaa6ul},
	{"prisoner", 0x029f0000ul, 0xb68da242ul}, {"putput", 0x026c8e00ul, 0xb3f87f50ul},
	{"ratrace", 0x02d35000ul, 0xaf5f3831ul}, {"sidewinder", 0x02dd2800ul, 0x4f8e749dul},
	{"wizard", 0x026cc200ul, 0x2d5a08fcul},
};

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int s16(unsigned char const *p)
{
	return (short)(p[0] | (p[1] << 8));
}

/* a map's header: 0 when it's an Xbox Halo map's */
static int map_header(char const *path, unsigned char *header)
{
	FILE *file = fopen(path, "rb");
	int ok;
	if (!file)
		return 1;
	ok = fread(header, 1, MAP_HEADER, file) == MAP_HEADER;
	fclose(file);
	return !ok || u32(header) != 0x68656164 || u32(header + 4) != 5;
}

/* the folder's own name ("F:\Games\Halo NHE": "Halo NHE") */
static char const *last_part(char const *path)
{
	char const *slash = strrchr(path, '\\');
	return slash ? slash + 1 : path;
}

static int write_file(char const *path, void const *data, unsigned long size)
{
	FILE *file = fopen(path, "wb");
	int ok;
	if (!file)
		return 1;
	ok = fwrite(data, 1, size, file) == size;
	if (fclose(file))
		ok = 0;
	if (!ok)
		DeleteFileA(path);
	return !ok;
}

/* a small text file's "<field>: " line's value (value[size]); 0 when it has one */
static int text_field(char const *path, char const *field, char *value, int size)
{
	char text[1024], *at;
	size_t got, n = strlen(field);
	FILE *file = fopen(path, "rb");
	if (!file)
		return 1;
	got = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[got] = 0;
	for (at = text; at; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL)
	{
		if (!_strnicmp(at, field, n) && at[n] == ':')
		{
			int k = 0;
			for (at += n + 1; *at == ' '; at++)
				;
			while (at[k] && at[k] != '\r' && at[k] != '\n' && k < size - 1)
			{
				value[k] = at[k];
				k++;
			}
			value[k] = 0;
			return 0;
		}
	}
	return 1;
}

static void make_folders(char const *kind, char const *key)
{
	char path[200];
	CreateDirectoryA("E:\\TDATA", NULL);
	CreateDirectoryA(STORAGE, NULL);
	text_print(path, sizeof(path), STORAGE "\\%s", kind);
	CreateDirectoryA(path, NULL);
	text_print(path, sizeof(path), STORAGE "\\%s\\%s", kind, key);
	CreateDirectoryA(path, NULL);
}

/* ---- finding an import, or a name for a new one ---- */

/* the folder in STORAGE\kind whose file's "From:" is path (key[PACK_KEY]); 0 when there's one */
static int find_import(char const *kind, char const *file, char const *path, char *key)
{
	WIN32_FIND_DATAA found;
	char pattern[200], name[200], from[200];
	HANDLE find;
	int result = 1;
	text_print(pattern, sizeof(pattern), STORAGE "\\%s\\*", kind);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return 1;
	do
	{
		if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.' ||
			strlen(found.cFileName) >= PACK_KEY)
			continue;
		text_print(name, sizeof(name), STORAGE "\\%s\\%s\\%s", kind, found.cFileName, file);
		if (!text_field(name, "From", from, sizeof(from)) && !_stricmp(from, path))
		{
			text_print(key, PACK_KEY, "%s", found.cFileName);
			result = 0;
			break;
		}
	} while (FindNextFileA(find, &found));
	FindClose(find);
	return result;
}

static int taken(char const *key)
{
	char path[200];
	text_print(path, sizeof(path), STORAGE "\\maps\\%s", key);
	if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
		return 1;
	text_print(path, sizeof(path), STORAGE "\\campaigns\\%s", key);
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* a new import's name: the folder's ("Halo 1 - NHE": "NHE"), up to 12 letters (the game's folder names are short: a
pack's and its map's together, MAXIMUM_FOLDER_NAME), and not one in use */
static void new_key(char const *path, char *key)
{
	char const *name = last_part(path), *rest = name;
	char base[13];
	int n = 0, k;
	if (!_strnicmp(rest, "halo", 4))
	{
		rest += 4;
		while (*rest == ' ' || *rest == '1' || *rest == '-' || *rest == ':' || *rest == '_')
			rest++;
		if (!*rest)
			rest = name;
	}
	for (; *rest && n < 12; rest++)
	{
		char c = *rest;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' ||
			c == '_')
			base[n++] = c;
	}
	while (n && base[n - 1] == ' ')
		n--;
	base[n] = 0;
	if (!n)
		strcpy(base, "Pack");
	text_print(key, PACK_KEY, "%s", base);
	for (k = 2; k < 100 && taken(key); k++)
		text_print(key, PACK_KEY, "%s %d", base, k);
}

/* ---- the maps in the pack ---- */

static int halos_own(struct pack_map const *map, char const *stock_folder)
{
	unsigned char header[MAP_HEADER];
	char path[200];
	int k;
	if (!map->checksum)
		return 0;
	if (map->type == 1)
	{
		for (k = 0; k < 13; k++)
		{
			if (!_stricmp(map->name, halos_maps[k].name) && map->length == halos_maps[k].length &&
				map->checksum == halos_maps[k].checksum)
				return 1;
		}
	}
	if (!stock_folder || !stock_folder[0])
		return 0;
	text_print(path, sizeof(path), "%s\\%s.map", stock_folder, map->name);
	return !map_header(path, header) && u32(header + 8) == map->length && u32(header + 0x64) == map->checksum;
}

static int count_links(char const *key)
{
	WIN32_FIND_DATAA found;
	char pattern[200], link[240];
	HANDLE find;
	int n = 0;
	text_print(pattern, sizeof(pattern), STORAGE "\\maps\\%s\\*", key);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.')
			continue;
		text_print(link, sizeof(link), STORAGE "\\maps\\%s\\%s\\map.link", key, found.cFileName);
		n += GetFileAttributesA(link) != INVALID_FILE_ATTRIBUTES;
	} while (FindNextFileA(find, &found));
	FindClose(find);
	return n;
}

static int count_levels(char const *key)
{
	static char text[16384];
	char path[200], *at;
	size_t got;
	int n = 0;
	FILE *file;
	text_print(path, sizeof(path), STORAGE "\\campaigns\\%s\\campaign.txt", key);
	if (!(file = fopen(path, "rb")))
		return 0;
	got = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[got] = 0;
	for (at = text; (at = strstr(at, "Level:")) != NULL; at++)
		n++;
	return n;
}

int packs_scan(char const *path, char const *stock_folder, struct pack_scan *scan)
{
	WIN32_FIND_DATAA found;
	char pattern[200], file[240];
	unsigned char header[MAP_HEADER];
	HANDLE find;
	int k, campaign = 0, modded_level = 0;

	memset(scan, 0, sizeof(*scan));
	text_print(scan->path, sizeof(scan->path), "%s", path);
	text_print(pattern, sizeof(pattern), "%s\\maps\\*.map", path);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			struct pack_map *map = &scan->maps[scan->count];
			size_t n = strlen(found.cFileName);
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n < 5 || n - 4 >= sizeof(map->name) ||
				scan->count == PACK_MAPS)
				continue;
			text_print(file, sizeof(file), "%s\\maps\\%s", path, found.cFileName);
			if (map_header(file, header))
				continue;
			memcpy(map->name, found.cFileName, n - 4);
			map->name[n - 4] = 0;
			map->type = s16(header + 0x60);
			map->length = u32(header + 8);
			map->checksum = u32(header + 0x64);
			if (map->type != 0 && map->type != 1)
				continue;
			map->halos = halos_own(map, stock_folder);
			scan->count++;
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	for (k = 0; k < scan->count; k++)
	{
		struct pack_map const *map = &scan->maps[k];
		if (map->type == 1)
			scan->multiplayer += !map->halos;
		else
			campaign++, modded_level |= !map->halos;
	}
	scan->campaign = modded_level ? campaign : 0;
	if (find_import("maps", "imported.txt", path, scan->key) &&
		find_import("campaigns", "campaign.txt", path, scan->key))
		new_key(path, scan->key);
	scan->multiplayer_imported = count_links(scan->key);
	scan->campaign_imported = count_levels(scan->key);
	return 0;
}

/* ---- the pack's XBE: its multiplayer levels' order ---- */

/* the longest run of pointers to "levels\...\<name>" strings in the XBE: their names, in order (names[max][40]) */
static int xbe_levels(char const *path, char (*names)[40], int max)
{
	struct found_string
	{
		unsigned long address, offset;
	} *strings = NULL;
	FILE *file = fopen(path, "rb");
	unsigned char *data = NULL;
	long size;
	unsigned long base, sections, headers, low = ~0ul, high = 0, o, best_at = 0;
	int count = 0, run = 0, best = 0, i, k;

	if (!file)
		return 0;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size < 0x1000 || size > 16 * 1024 * 1024 || !(data = malloc(size)) ||
		fread(data, 1, size, file) != (size_t)size || !(strings = malloc(256 * sizeof(*strings))))
		goto done;
	base = u32(data + 0x104);
	sections = u32(data + 0x11c);
	headers = u32(data + 0x120) - base;
	if (sections > 64 || headers > (unsigned long)size || sections * 56 > (unsigned long)size - headers)
		goto done;
	for (o = 0; o + 8 < (unsigned long)size && count < 256; o++)
	{
		unsigned long e = o + 7, s;
		if (data[o] != 'l' || memcmp(data + o, "levels\\", 7))
			continue;
		while (e < (unsigned long)size && e - o < 96 && ((data[e] >= 'a' && data[e] <= 'z') ||
			(data[e] >= '0' && data[e] <= '9') || data[e] == '_' || data[e] == '\\'))
			e++;
		if (e >= (unsigned long)size || data[e] || e == o + 7)
			continue;
		for (s = 0; s < sections; s++)
		{
			unsigned char const *h = data + headers + s * 56;
			if (o >= u32(h + 12) && o - u32(h + 12) < u32(h + 16))
			{
				strings[count].address = u32(h + 4) + o - u32(h + 12);
				strings[count].offset = o;
				low = strings[count].address < low ? strings[count].address : low;
				high = strings[count].address > high ? strings[count].address : high;
				count++;
				break;
			}
		}
	}
	for (o = 0; o + 4 <= (unsigned long)size; o += 4)
	{
		unsigned long v = u32(data + o);
		int hit = 0;
		if (v >= low && v <= high)
		{
			for (k = 0; k < count && strings[k].address != v; k++)
				;
			hit = k < count;
		}
		if (hit)
			run++;
		else
		{
			if (run > best)
				best = run, best_at = o - run * 4;
			run = 0;
		}
	}
	for (i = 0; i < best && i < max; i++)
	{
		unsigned long v = u32(data + best_at + i * 4);
		for (k = 0; strings[k].address != v; k++)
			;
		text_print(names[i], 40, "%s", last_part((char const *)data + strings[k].offset));
	}
	best = best < max ? best : max;
done:
	free(strings);
	free(data);
	fclose(file);
	return best >= 2 ? best : 0;
}

/* ---- the pack's ui.map ---- */

struct ui_tags
{
	unsigned char *tags;
	unsigned long size;
};

#define UI_AT(pointer) ((pointer) - MAP_BASE)
#define UI_IN(ui, pointer, length) ((pointer) >= MAP_BASE && UI_AT(pointer) <= (ui)->size && (length) <= (ui)->size - UI_AT(pointer))

/* a tag's data (its offset in the tags), by group and name: 0 when there's none */
static unsigned long ui_tag(struct ui_tags const *ui, unsigned long group, char const *name)
{
	unsigned long array, count, i, n = strlen(name);
	if (!ui->tags || ui->size < 16)
		return 0;
	array = u32(ui->tags);
	count = u32(ui->tags + 12);
	if (count > 65536 || !UI_IN(ui, array, count * 32))
		return 0;
	for (i = 0; i < count; i++)
	{
		unsigned char const *entry = ui->tags + UI_AT(array) + i * 32;
		unsigned long name_at = u32(entry + 16), data = u32(entry + 20);
		if (u32(entry) != group || !UI_IN(ui, name_at, n + 1) || memcmp(ui->tags + UI_AT(name_at), name, n + 1))
			continue;
		return UI_IN(ui, data, 0x68) ? UI_AT(data) : 0;
	}
	return 0;
}

/* a string list's string (UTF-16), as UTF-8 on one line. Halo's lines are broken for its own boxes, which the game's
aren't: a line break is a space (a word broken with a hyphen is joined again), and a blank line is a paragraph
("|n|n", in a description; a name is all on one line) */
static void ui_string(struct ui_tags const *ui, unsigned long data, unsigned long index, int name, char *out, int size)
{
	unsigned short text[1024];
	unsigned long count, list, length, at, k;
	int n = 0, t = 0, i;
	out[0] = 0;
	if (!data)
		return;
	count = u32(ui->tags + data);
	list = u32(ui->tags + data + 4);
	if (index >= count || count > 4096 || !UI_IN(ui, list, count * 20))
		return;
	length = u32(ui->tags + UI_AT(list) + index * 20);
	at = u32(ui->tags + UI_AT(list) + index * 20 + 12);
	if (length > 4096 || !UI_IN(ui, at, length))
		return;
	for (k = 0; k + 1 < length && t < 1023; k += 2)
	{
		unsigned c = ui->tags[UI_AT(at) + k] | (ui->tags[UI_AT(at) + k + 1] << 8);
		if (!c)
			break;
		if (c != '\r')
			text[t++] = (unsigned short)(c == '\t' ? ' ' : c == '|' ? '/' : c);
	}
	for (i = 0; i < t && n < size - 5;)
	{
		unsigned c = text[i];
		if (c == ' ' || c == '\n')
		{
			int breaks = 0;
			for (; i < t && (text[i] == ' ' || text[i] == '\n'); i++)
				breaks += text[i] == '\n';
			if (!n || i == t)
				continue;
			if (breaks >= 2 && !name)
				out[n++] = '|', out[n++] = 'n', out[n++] = '|', out[n++] = 'n';
			else if (breaks == 1 && n >= 2 && out[n - 1] == '-' && ((out[n - 2] | 0x20) >= 'a' && (out[n - 2] | 0x20) <= 'z'))
			{
				if (text[i] >= 'a' && text[i] <= 'z')
					n--;                     /* ("wea-|pon": weapon; "Hydro-|Processing" keeps its hyphen) */
			}
			else
				out[n++] = ' ';
			continue;
		}
		if (c < 0x80)
			out[n++] = (char)c;
		else if (c < 0x800)
			out[n++] = (char)(0xc0 | c >> 6), out[n++] = (char)(0x80 | (c & 0x3f));
		else
			out[n++] = (char)(0xe0 | c >> 12), out[n++] = (char)(0x80 | ((c >> 6) & 0x3f)),
				out[n++] = (char)(0x80 | (c & 0x3f));
		i++;
	}
	out[n] = 0;
}

/* a bitmap's picture (256x128 DXT1, as Halo's map pictures): where it is in the map, or 0 */
static int ui_picture(struct ui_tags const *ui, unsigned long data, unsigned long index, struct map_range *range)
{
	unsigned long count, list;
	unsigned char const *bitmap;
	range->size = 0;
	if (!data)
		return 0;
	count = u32(ui->tags + data + 0x60);
	list = u32(ui->tags + data + 0x64);
	if (index >= count || count > 1024 || !UI_IN(ui, list, count * 48))
		return 0;
	bitmap = ui->tags + UI_AT(list) + index * 48;
	if (s16(bitmap + 4) != 256 || s16(bitmap + 6) != 128 || s16(bitmap + 0xc) != 14 ||
		u32(bitmap + 0x1c) != PICTURE_SIZE)
		return 0;
	range->offset = u32(bitmap + 0x18);
	range->size = PICTURE_SIZE;
	return 1;
}

#undef UI_IN
#undef UI_AT

/* ---- importing ---- */

struct entry
{
	int map;                      /* (scan->maps') */
	int order;                    /* its place in the pack's menu (ui.map's lists), or a big number */
	char name[TEXT_NAME];
	char description[TEXT_DESCRIPTION];
	struct map_range picture;
};

static int entry_compare(void const *a, void const *b)
{
	struct entry const *x = a, *y = b;
	return x->order != y->order ? x->order - y->order : x->map - y->map;
}

static void remove_multiplayer(char const *key)
{
	WIN32_FIND_DATAA found;
	char pattern[200], folder[200], file[240];
	static char const *const files[] = {"map.link", "info.txt", "preview.bin"};
	HANDLE find;
	unsigned k;
	text_print(pattern, sizeof(pattern), STORAGE "\\maps\\%s\\*", key);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.')
				continue;
			text_print(folder, sizeof(folder), STORAGE "\\maps\\%s\\%s", key, found.cFileName);
			text_print(file, sizeof(file), "%s\\map.link", folder);
			if (GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES)
				continue; /* (a map that isn't an import's: map sharing's copy, say) */
			for (k = 0; k < sizeof(files) / sizeof(files[0]); k++)
			{
				text_print(file, sizeof(file), "%s\\%s", folder, files[k]);
				DeleteFileA(file);
			}
			RemoveDirectoryA(folder);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	text_print(file, sizeof(file), STORAGE "\\maps\\%s\\imported.txt", key);
	DeleteFileA(file);
	text_print(file, sizeof(file), STORAGE "\\maps\\%s\\order.txt", key);
	DeleteFileA(file);
	text_print(folder, sizeof(folder), STORAGE "\\maps\\%s", key);
	RemoveDirectoryA(folder);
}

static void remove_campaign(char const *key)
{
	WIN32_FIND_DATAA found;
	char pattern[200], file[240];
	HANDLE find;
	text_print(pattern, sizeof(pattern), STORAGE "\\campaigns\\%s\\*", key);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			size_t n = strlen(found.cFileName);
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
				!((n > 4 && !_stricmp(found.cFileName + n - 4, ".bin")) || !_stricmp(found.cFileName, "campaign.txt")))
				continue;
			text_print(file, sizeof(file), STORAGE "\\campaigns\\%s\\%s", key, found.cFileName);
			DeleteFileA(file);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	text_print(file, sizeof(file), STORAGE "\\campaigns\\%s", key);
	RemoveDirectoryA(file);
}

void packs_remove(struct pack_scan *scan, int multiplayer, int campaign, packs_say_t say)
{
	char line[200];
	if (multiplayer && scan->multiplayer_imported)
	{
		remove_multiplayer(scan->key);
		text_print(line, sizeof(line), "Took out %d multiplayer maps (maps\\%s).", scan->multiplayer_imported, scan->key);
		say(0, line);
		scan->multiplayer_imported = 0;
	}
	if (campaign && scan->campaign_imported)
	{
		remove_campaign(scan->key);
		text_print(line, sizeof(line), "Took out the campaign (campaigns\\%s).", scan->key);
		say(0, line);
		scan->campaign_imported = 0;
	}
}

/* the maps of one kind (type) to import, with their names and pictures' places (entries[PACK_MAPS]); how many */
static int gather(struct pack_scan const *scan, int type, struct ui_tags const *ui, char (*xbe)[40], int xbe_count,
	struct entry *entries)
{
	unsigned long names, texts, pictures;
	int n = 0, k, i;
	if (type == 1)
	{
		names = ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\mp_map_list");
		texts = ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\multiplayer_type_select\\mp_map_select\\map_data");
		pictures = ui_tag(ui, 0x6269746d, "ui\\shell\\bitmaps\\mp_map_grafix");
	}
	else
	{
		names = ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\map_list");
		texts = ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\solo_level_select\\map_data");
		pictures = ui_tag(ui, 0x6269746d, "ui\\shell\\bitmaps\\sp_levels");
	}
	for (k = 0; k < scan->count; k++)
	{
		struct pack_map const *map = &scan->maps[k];
		struct entry *e = &entries[n];
		int index = -1;
		if (map->type != type || (type == 1 && map->halos))
			continue;
		if (type == 1)
		{
			for (i = 0; i < xbe_count && index < 0; i++)
				index = _stricmp(xbe[i], map->name) ? -1 : i;
			for (i = 0; !xbe_count && i < 13 && index < 0; i++)
				index = _stricmp(halos_order[i], map->name) ? -1 : i;
		}
		else
		{
			for (i = 0; i < CAMPAIGN_LEVELS && index < 0; i++)
				index = _stricmp(campaign_order[i], map->name) ? -1 : i;
		}
		memset(e, 0, sizeof(*e));
		e->map = k;
		e->order = index >= 0 ? index : 1000 + k;
		if (index >= 0)
		{
			ui_string(ui, names, (unsigned long)index, 1, e->name, sizeof(e->name));
			ui_string(ui, texts, (unsigned long)index, 0, e->description, sizeof(e->description));
			ui_picture(ui, pictures, (unsigned long)index, &e->picture);
		}
		if (!e->name[0])
			text_print(e->name, sizeof(e->name), "%s", map->name);
		n++;
	}
	qsort(entries, n, sizeof(*entries), entry_compare);
	return n;
}

static int write_multiplayer(struct pack_scan *scan, struct entry *entries, int n, packs_say_t say)
{
	char path[240], text[TEXT_NAME + TEXT_DESCRIPTION + 260], order[PACK_MAPS * 42];
	int k, written = 0, at = 0, failed = 0;
	make_folders("maps", scan->key);
	for (k = 0; k < n; k++)
	{
		struct entry const *e = &entries[k];
		char const *name = scan->maps[e->map].name;
		if (strlen(scan->key) + 1 + strlen(name) > MAXIMUM_FOLDER_NAME)
		{
			text_print(text, sizeof(text), "  %s: its name is too long for the game's map list; skipped.", name);
			say(1, text);
			continue;
		}
		text_print(path, sizeof(path), STORAGE "\\maps\\%s\\%s", scan->key, name);
		CreateDirectoryA(path, NULL);
		text_print(path, sizeof(path), STORAGE "\\maps\\%s\\%s\\info.txt", scan->key, name);
		text_print(text, sizeof(text), "%s\r\n%s\r\n", e->name, e->description);
		failed |= write_file(path, text, strlen(text));
		if (e->picture.size)
		{
			text_print(path, sizeof(path), STORAGE "\\maps\\%s\\%s\\preview.bin", scan->key, name);
			failed |= write_file(path, e->picture.data, PICTURE_SIZE);
		}
		/* (the link last: a map is in the list once it's there) */
		text_print(path, sizeof(path), STORAGE "\\maps\\%s\\%s\\map.link", scan->key, name);
		text_print(text, sizeof(text), "%s\\maps\\%s.map\r\n", scan->path, name);
		if (write_file(path, text, strlen(text)))
		{
			failed = 1;
			continue;
		}
		at += text_print(order + at, sizeof(order) - at, "%s\r\n", name);
		written++;
		text_print(text, sizeof(text), "  %s: %s%s", name, e->name, e->picture.size ? "" : " (no picture)");
		say(0, text);
	}
	text_print(path, sizeof(path), STORAGE "\\maps\\%s\\order.txt", scan->key);
	failed |= write_file(path, order, (unsigned long)at);
	text_print(path, sizeof(path), STORAGE "\\maps\\%s\\imported.txt", scan->key);
	text_print(text, sizeof(text), "From: %s\r\nName: %s\r\n", scan->path, last_part(scan->path));
	failed |= write_file(path, text, strlen(text));
	scan->multiplayer_imported = written;
	return failed;
}

static int write_campaign(struct pack_scan *scan, struct entry *entries, int n, packs_say_t say)
{
	char path[240], line[TEXT_NAME + TEXT_DESCRIPTION + 40];
	char *text = malloc(n * sizeof(line) + 512);
	int k, at, failed = 0;
	if (!text)
		return 1;
	make_folders("campaigns", scan->key);
	at = text_print(text, 512, "Name: %s\r\nFrom: %s\r\n", last_part(scan->path), scan->path);
	for (k = 0; k < n; k++)
	{
		struct entry const *e = &entries[k];
		char const *name = scan->maps[e->map].name;
		at += text_print(text + at, sizeof(line), "Level: %s\r\nTitle: %s\r\nText: %s\r\n", name, e->name,
			e->description);
		if (e->picture.size)
		{
			text_print(path, sizeof(path), STORAGE "\\campaigns\\%s\\%s.bin", scan->key, name);
			failed |= write_file(path, e->picture.data, PICTURE_SIZE);
		}
		text_print(line, sizeof(line), "  %s: %s%s", name, e->name, e->picture.size ? "" : " (no picture)");
		say(0, line);
	}
	text_print(path, sizeof(path), STORAGE "\\campaigns\\%s\\campaign.txt", scan->key);
	failed |= write_file(path, text, (unsigned long)at);
	free(text);
	scan->campaign_imported = failed ? 0 : n;
	return failed;
}

int packs_import(struct pack_scan *scan, int multiplayer, int campaign, packs_say_t say, char *error)
{
	struct ui_tags ui = {NULL, 0};
	struct map_range tags_range, *ranges = NULL;
	struct entry *entries = NULL;
	char (*xbe)[40] = NULL;
	char ui_path[200], line[256];
	unsigned char header[MAP_HEADER], *pictures = NULL;
	unsigned long file_size;
	int xbe_count = 0, mp_count = 0, sp_count = 0, k, n, failed = 0;

	error[0] = 0;
	entries = malloc(2 * PACK_MAPS * sizeof(*entries));
	xbe = malloc(XBE_LEVELS * sizeof(*xbe));
	if (!entries || !xbe)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	/* the pack's ui.map's tags (without it: the maps' file names, no pictures) */
	text_print(ui_path, sizeof(ui_path), "%s\\maps\\ui.map", scan->path);
	if (!map_header(ui_path, header) && u32(header + 20) <= 16 * 1024 * 1024 && u32(header + 16) >= MAP_HEADER)
	{
		say(0, "Reading the pack's ui.map (names and pictures)...");
		tags_range.offset = u32(header + 16);
		tags_range.size = u32(header + 20);
		tags_range.data = ui.tags = malloc(tags_range.size);
		ui.size = tags_range.size;
		if (!ui.tags || map_read(ui_path, &tags_range, 1, &file_size, line))
		{
			say(1, ui.tags ? line : "out of memory reading ui.map");
			free(ui.tags);
			ui.tags = NULL;
			ui.size = 0;
		}
	}
	else
		say(1, "The pack has no ui.map that reads: the maps get their file names, and no pictures.");
	if (multiplayer)
	{
		text_print(line, sizeof(line), "%s\\default.xbe", scan->path);
		xbe_count = xbe_levels(line, xbe, XBE_LEVELS);
		mp_count = gather(scan, 1, &ui, xbe, xbe_count, entries);
	}
	if (campaign)
		sp_count = gather(scan, 0, &ui, xbe, 0, entries + mp_count);
	/* the pictures, the second time through ui.map */
	n = mp_count + sp_count;
	ranges = malloc((n + 1) * sizeof(*ranges));
	pictures = malloc((n + 1) * PICTURE_SIZE);
	if (!ranges || !pictures)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	{
		int count = 0;
		for (k = 0; k < n; k++)
		{
			if (!entries[k].picture.size)
				continue;
			entries[k].picture.data = pictures + count * PICTURE_SIZE;
			ranges[count++] = entries[k].picture;
		}
		if (count && map_read(ui_path, ranges, count, &file_size, line))
		{
			say(1, line);
			for (k = 0; k < n; k++)
				entries[k].picture.size = 0;
		}
	}
	free(ui.tags);
	ui.tags = NULL;
	if (multiplayer && mp_count)
	{
		remove_multiplayer(scan->key);
		text_print(line, sizeof(line), "Multiplayer: %d maps, as the pack \"%s\" in the map list:", mp_count, scan->key);
		say(0, line);
		failed |= write_multiplayer(scan, entries, mp_count, say);
	}
	if (campaign && sp_count)
	{
		remove_campaign(scan->key);
		text_print(line, sizeof(line), "Campaign: %d levels, as \"%s\":", sp_count, last_part(scan->path));
		say(0, line);
		failed |= write_campaign(scan, entries + mp_count, sp_count, say);
	}
	if (failed)
		text_print(error, 256, "some files couldn't be written in " STORAGE " (is E: full?)");
done:
	free(ui.tags);
	free(ranges);
	free(pictures);
	free(entries);
	free(xbe);
	return error[0] != 0;
}
