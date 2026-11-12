/* packs.c: importing a map pack (another Halo's folder on the hard drive: a modded Halo's maps) into Halo 1
Unlimited (packs.h). Nothing is copied or changed in the pack's folder: the game plays its .map files where they are.
What's written goes in the game's own folder, packs_storage (the install the installer's list stars: "F:\Games\H1U"; the
game reads its d:\maps as it reads T:\maps), never E:'s storage, as installers before 41 did (packs_remove_old):

	maps\<pack>\                  a pack in the game's map list (port/linux/game/hmp_maps.c)
		imported.txt              "From: <the Halo folder>", what the installer finds the import by
		order.txt                 the pack's maps in its own menu's order
		<map>\map.link            the .map's path ("F:\Games\Halo NHE\maps\atlas.map")
		<map>\info.txt            its name and description, from the pack's ui.map
		<map>\preview.bin         its picture, from the pack's ui.map (256x128 DXT1, as Halo's own; a 128x128 one goes top left)
	campaigns\<pack>\campaign.txt "Name:", "From:", then for each level "Level:" (its file), "Title:", "Text:"
	campaigns\<pack>\<level>.bin  its picture

The names, descriptions and pictures are the pack's ui.map's: the multiplayer maps' (mp_map_list, mp_map_select's
map_data, mp_map_grafix) in the order of the pack's XBE's multiplayer levels (a run of pointers to "levels\..."
names: Halo's 13 in a retail XBE, more in a modded one), the campaign's (map_list, solo_level_select's map_data,
sp_levels) in Halo's level order. A map Halo has already (Halo's 13, or a file the same as the install's) isn't
imported. */
#include "packs.h"
#include "packart.h"
#include "pictures.h"
#include "text.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OLD_STORAGE "E:\\TDATA\\4c4a4855" /* (where installers before 41 put imports: E: is the smallest drive) */
char packs_storage[160];
static char const *storage = packs_storage;
#define STORAGE storage
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
	text_print(path, sizeof(path), "%s\\%s", STORAGE, kind);
	CreateDirectoryA(path, NULL);
	text_print(path, sizeof(path), "%s\\%s\\%s", STORAGE, kind, key);
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
	text_print(pattern, sizeof(pattern), "%s\\%s\\*", STORAGE, kind);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return 1;
	do
	{
		if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.' ||
			strlen(found.cFileName) >= PACK_KEY)
			continue;
		text_print(name, sizeof(name), "%s\\%s\\%s\\%s", STORAGE, kind, found.cFileName, file);
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
	text_print(path, sizeof(path), "%s\\maps\\%s", STORAGE, key);
	if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
		return 1;
	text_print(path, sizeof(path), "%s\\campaigns\\%s", STORAGE, key);
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

/* (a map drawing on shared files keeps its header's checksum, its length being its small file's: docs/shared_maps.md) */
#define SHARED_REFERENCE_OFFSET 0x700
#define SHARED_REFERENCE_SIGNATURE 0x73687264ul

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
			if (!_stricmp(map->name, halos_maps[k].name) && (map->small || map->length == halos_maps[k].length) &&
				map->checksum == halos_maps[k].checksum)
				return 1;
		}
	}
	if (!stock_folder || !stock_folder[0])
		return 0;
	text_print(path, sizeof(path), "%s\\%s.map", stock_folder, map->name);
	if (map_header(path, header) || u32(header + 0x64) != map->checksum)
		return 0;
	return map->small || u32(header + SHARED_REFERENCE_OFFSET) == SHARED_REFERENCE_SIGNATURE || u32(header + 8) == map->length;
}

static int count_links(char const *key)
{
	WIN32_FIND_DATAA found;
	char pattern[200], link[240];
	HANDLE find;
	int n = 0;
	text_print(pattern, sizeof(pattern), "%s\\maps\\%s\\*", STORAGE, key);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.')
			continue;
		text_print(link, sizeof(link), "%s\\maps\\%s\\%s\\map.link", STORAGE, key, found.cFileName);
		if (GetFileAttributesA(link) == INVALID_FILE_ATTRIBUTES)
			continue;
		/* (a campaign level's folder: level.txt beside its link) */
		text_print(link, sizeof(link), "%s\\maps\\%s\\%s\\level.txt", STORAGE, key, found.cFileName);
		n += GetFileAttributesA(link) == INVALID_FILE_ATTRIBUTES;
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
	text_print(path, sizeof(path), "%s\\campaigns\\%s\\campaign.txt", STORAGE, key);
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
			map->size = found.nFileSizeLow;
			map->length = u32(header + 8);
			map->checksum = u32(header + 0x64);
			map->small = u32(header + SHARED_REFERENCE_OFFSET) == SHARED_REFERENCE_SIGNATURE;
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
	/* (its fingerprint: a CRC-32 of its maps' "name size checksum" lines (lower case, size and header checksum 0x64 in
	hex: "beavercreek 22802432 1a2b3c4d"), in name order; and their size. tools/pack_fingerprint.py makes the same) */
	{
		int order[PACK_MAPS], i, j;
		unsigned long crc = 0xFFFFFFFFul;
		char line[96];
		for (i = 0, j = 0; i < scan->count; i++)
			order[j++] = i;
		for (i = 1; i < j; i++)
		{
			int t = order[i], m = i;
			for (; m > 0 && _stricmp(scan->maps[order[m - 1]].name, scan->maps[t].name) > 0; m--)
				order[m] = order[m - 1];
			order[m] = t;
		}
		scan->size = 0;
		for (i = 0; i < j; i++)
		{
			struct pack_map const *m = &scan->maps[order[i]];
			int n = text_print(line, sizeof(line), "%s %lu %08lx\n", m->name, m->size, m->checksum), c;
			for (c = 0; c < n; c++)
			{
				int bit;
				unsigned char ch = (unsigned char)(line[c] >= 'A' && line[c] <= 'Z' ? line[c] + 32 : line[c]);
				crc ^= ch;
				for (bit = 0; bit < 8; bit++)
					crc = crc >> 1 ^ (0xEDB88320ul & (0 - (crc & 1)));
			}
			scan->size += m->size;
		}
		scan->fingerprint = ~crc & 0xFFFFFFFFul;
	}
	if (find_import("maps", "imported.txt", path, scan->key) &&
		find_import("campaigns", "campaign.txt", path, scan->key))
		new_key(path, scan->key);
	scan->multiplayer_imported = count_links(scan->key);
	scan->campaign_imported = count_levels(scan->key);
	/* (what an installer before 41 imported into E:'s storage counts too: the game still plays it, and taking it out or
	importing it again removes it) */
	if (_stricmp(packs_storage, OLD_STORAGE))
	{
		char old[PACK_KEY];
		storage = OLD_STORAGE;
		if (!find_import("maps", "imported.txt", path, old))
			scan->multiplayer_imported += count_links(old);
		if (!find_import("campaigns", "campaign.txt", path, old))
			scan->campaign_imported += count_levels(old);
		storage = packs_storage;
	}
	return 0;
}

static void remove_multiplayer(char const *key);
static void remove_campaign(char const *key);

/* an import of path in OLD_STORAGE taken out (its multiplayer maps and/or campaign); how many maps and levels it had */
int packs_remove_old(char const *path, int multiplayer, int campaign)
{
	char key[PACK_KEY];
	int n = 0;
	if (!_stricmp(packs_storage, OLD_STORAGE))
		return 0;
	storage = OLD_STORAGE;
	if (multiplayer && !find_import("maps", "imported.txt", path, key))
	{
		n += count_links(key);
		remove_multiplayer(key);
	}
	if (campaign && !find_import("campaigns", "campaign.txt", path, key))
	{
		n += count_levels(key);
		remove_campaign(key);
	}
	storage = packs_storage;
	return n;
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

/* a tag's index entry by its ID (NULL when there's none): its group at 0, name at 16, data at 20 */
static unsigned char const *ui_entry(struct ui_tags const *ui, unsigned long id)
{
	unsigned long array, count, i;
	if (!ui->tags || ui->size < 16 || !id || id == 0xFFFFFFFFul)
		return NULL;
	array = u32(ui->tags);
	count = u32(ui->tags + 12);
	if (count > 65536 || !UI_IN(ui, array, count * 32))
		return NULL;
	/* (the ID's low half is its place in the table) */
	i = id & 0xFFFF;
	if (i < count && u32(ui->tags + UI_AT(array) + i * 32 + 12) == id)
		return ui->tags + UI_AT(array) + i * 32;
	for (i = 0; i < count; i++)
		if (u32(ui->tags + UI_AT(array) + i * 32 + 12) == id)
			return ui->tags + UI_AT(array) + i * 32;
	return NULL;
}

/* a tag's data by its ID and group (0: none), and its name ("" when it hasn't one that reads) */
static unsigned long ui_data(struct ui_tags const *ui, unsigned long id, unsigned long group, unsigned long length)
{
	unsigned char const *entry = ui_entry(ui, id);
	if (!entry || u32(entry) != group || !UI_IN(ui, u32(entry + 20), length))
		return 0;
	return UI_AT(u32(entry + 20));
}

static void ui_name(struct ui_tags const *ui, unsigned long id, char *out, int size)
{
	unsigned char const *entry = ui_entry(ui, id);
	int k;
	out[0] = 0;
	if (!entry || !UI_IN(ui, u32(entry + 16), 1))
		return;
	for (k = 0; k < size - 1 && UI_AT(u32(entry + 16)) + k < ui->size && ui->tags[UI_AT(u32(entry + 16)) + k]; k++)
	{
		char c = (char)ui->tags[UI_AT(u32(entry + 16)) + k];
		out[k] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
	}
	out[k] = 0;
}

#define GROUP_WIDGET 0x44654c61ul     /* 'DeLa' */
#define GROUP_STRINGS 0x75737472ul    /* 'ustr' */
#define GROUP_BITMAP 0x6269746dul     /* 'bitm' */
#define WIDGET_SIZE 0x3EC
/* (a widget: its type at 0, bounds at 0x24 (top, left, bottom, right), background bitmap's reference at 0x38 (its ID at
+12), game data inputs at 0x48 (count, pointer; 36 bytes each, the function first), text's string list at 0xEC,
children at 0x3E0 (count, pointer; 0x50 bytes each, the widget's reference first)) */
static unsigned long widget_child(struct ui_tags const *ui, unsigned long widget, unsigned long index)
{
	unsigned long count = u32(ui->tags + widget + 0x3E0), list = u32(ui->tags + widget + 0x3E4);
	if (index >= count || count > 64 || !UI_IN(ui, list, count * 0x50))
		return 0;
	return u32(ui->tags + UI_AT(list) + index * 0x50 + 12);
}

/* the pack's menu that lists its maps (the multiplayer map select, function 22, or the campaign's level select, 6), as
the game finds what it shows: its list's first item's first three children are the name, the picture and the text
(ui_widget_game_data_input_functions.c). Their string lists and bitmap: 0 when there's no such menu */
static int menu_tags(struct ui_tags const *ui, int function, unsigned long *names, unsigned long *texts,
	unsigned long *pictures)
{
	unsigned long array, count, i, k;
	if (!ui->tags || ui->size < 16)
		return 0;
	array = u32(ui->tags);
	count = u32(ui->tags + 12);
	if (count > 65536 || !UI_IN(ui, array, count * 32))
		return 0;
	for (i = 0; i < count; i++)
	{
		unsigned char const *entry = ui->tags + UI_AT(array) + i * 32;
		unsigned long list, inputs, input_list, item, child[3];
		if (u32(entry) != GROUP_WIDGET || !(list = ui_data(ui, u32(entry + 12), GROUP_WIDGET, WIDGET_SIZE)))
			continue;
		inputs = u32(ui->tags + list + 0x48);
		input_list = u32(ui->tags + list + 0x4C);
		if (inputs > 16 || !UI_IN(ui, input_list, inputs * 36))
			continue;
		for (k = 0; k < inputs && s16(ui->tags + UI_AT(input_list) + k * 36) != function; k++)
			;
		if (k == inputs || !(item = ui_data(ui, widget_child(ui, list, 0), GROUP_WIDGET, WIDGET_SIZE)))
			continue;
		for (k = 0; k < 3; k++)
			child[k] = ui_data(ui, widget_child(ui, item, k), GROUP_WIDGET, WIDGET_SIZE);
		if (!child[0] || !child[1] || !child[2])
			continue;
		*names = ui_data(ui, u32(ui->tags + child[0] + 0xEC + 12), GROUP_STRINGS, 12);
		*pictures = ui_data(ui, u32(ui->tags + child[1] + 0x38 + 12), GROUP_BITMAP, 0x68);
		*texts = ui_data(ui, u32(ui->tags + child[2] + 0xEC + 12), GROUP_STRINGS, 12);
		if (*names || *pictures || *texts)
			return 1;
	}
	return 0;
}

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

/* a bitmap's picture `index` (as the game picks it: its sequence 0's index-th bitmap, else the index-th): where its
pixels are in the map (range), and what they are (frame). 0 when it has none */
#define MOST_PICTURE_BYTES (4ul << 20)
static int ui_picture(struct ui_tags const *ui, unsigned long data, unsigned long index, struct map_range *range,
	struct art_frame *frame)
{
	unsigned long count, list, sequences, sequence_list;
	unsigned char const *bitmap;
	range->size = 0;
	if (!data)
		return 0;
	count = u32(ui->tags + data + 0x60);
	list = u32(ui->tags + data + 0x64);
	sequences = u32(ui->tags + data + 0x54);
	sequence_list = u32(ui->tags + data + 0x58);
	if (sequences && sequences <= 1024 && UI_IN(ui, sequence_list, 64))
	{
		int first = s16(ui->tags + UI_AT(sequence_list) + 0x20), frames = s16(ui->tags + UI_AT(sequence_list) + 0x22);
		if (frames > 0 && first >= 0)
		{
			if (index >= (unsigned long)frames)
				return 0;
			index += (unsigned long)first;
		}
	}
	if (index >= count || count > 4096 || !UI_IN(ui, list, count * 48))
		return 0;
	bitmap = ui->tags + UI_AT(list) + index * 48;
	frame->width = s16(bitmap + 4);
	frame->height = s16(bitmap + 6);
	frame->format = s16(bitmap + 0xc);
	frame->flags = (int)(bitmap[0xe] | bitmap[0xf] << 8);
	frame->offset = u32(bitmap + 0x18);
	frame->size = u32(bitmap + 0x1c);
	if (frame->width <= 0 || frame->height <= 0 || !frame->size || frame->size > MOST_PICTURE_BYTES)
		return 0;
	range->offset = frame->offset;
	range->size = frame->size;
	return 1;
}

/* a 128x128 DXT1 picture (its first PICTURE_SIZE / 2 bytes) made 256x128 in place: at the top left, where the map list
shows (its top-left 140x114, as Halo's own pictures and as the pack's own menu showed it), black to the right (an all-zero
DXT1 block is black). A row of 32 blocks moves to the start of a row of 64, the last row first */
static void picture_widen(unsigned char *p)
{
	int row;
	for (row = 31; row >= 0; row--)
	{
		memmove(p + row * 64 * 8, p + row * 32 * 8, 32 * 8);
		memset(p + (row * 64 + 32) * 8, 0, 32 * 8);
	}
}

/* ---- the pack's logo: on its main menu (packs usually swap the HALO logo for their own), as Halo Map Porter finds it
(its mappack.cpp find_menu_logos): the pictures of ui\shell\main_menu\main_menu's widgets (not its lists' items: its
buttons), then any bitmap named a logo; each one's first picture, read with the maps' (logo_wants), then cut and
scored (packart.c art_logo_cut). Halo's own HALO logo is skipped ---- */

#define MOST_LOGOS 16
#define MOST_SEEN 256
struct logo_want
{
	struct map_range range;
	struct art_frame frame;
	char names[200];                  /* (its bitmap's name and its widget's, lower case) */
	int screen, named_only;
};
struct logo_search
{
	struct logo_want *wants;
	int count;
	unsigned long seen[MOST_SEEN];
	int seen_count;
};

static int has_any(char const *s, char const *const *words)
{
	for (; *words; words++)
		if (strstr(s, *words))
			return 1;
	return 0;
}
static char const *const logo_words[] = {"logo", "title", "banner", "emblem", NULL};
static char const *const not_logo_words[] = {"button", "cursor", "arrow", "highlight", "background", "select", "overscan",
	"vignette", NULL};

static int seen_before(struct logo_search *search, unsigned long id)
{
	int k;
	for (k = 0; k < search->seen_count; k++)
		if (search->seen[k] == id)
			return 1;
	if (search->seen_count < MOST_SEEN)
		search->seen[search->seen_count++] = id;
	return 0;
}

static void logo_look(struct ui_tags const *ui, unsigned long widget_id, unsigned long bitmap_id, int named_only,
	struct logo_search *search)
{
	unsigned long bitmap = ui_data(ui, bitmap_id, GROUP_BITMAP, 0x68), widget;
	struct logo_want *want;
	char widget_name[96];
	int logo;
	if (!bitmap || search->count == MOST_LOGOS || seen_before(search, bitmap_id))
		return;
	want = &search->wants[search->count];
	if (!ui_picture(ui, bitmap, 0, &want->range, &want->frame))
		return;
	ui_name(ui, bitmap_id, want->names, 96);
	ui_name(ui, widget_id, widget_name, sizeof(widget_name));
	strcat(want->names, " ");
	strcat(want->names, widget_name);
	logo = has_any(want->names, logo_words);
	if ((named_only && !logo) || (!logo && has_any(want->names, not_logo_words)))
		return;
	want->screen = 0;
	if ((widget = ui_data(ui, widget_id, GROUP_WIDGET, WIDGET_SIZE)) != 0)
		want->screen = s16(ui->tags + widget + 0x2A) - s16(ui->tags + widget + 0x26) >= 600 &&
			s16(ui->tags + widget + 0x28) - s16(ui->tags + widget + 0x24) >= 440;
	want->named_only = named_only;
	search->count++;
}

static void logo_walk(struct ui_tags const *ui, unsigned long widget_id, int depth, struct logo_search *search)
{
	unsigned long widget = ui_data(ui, widget_id, GROUP_WIDGET, WIDGET_SIZE), k;
	int type;
	if (!widget || depth > 4 || seen_before(search, widget_id))
		return;
	logo_look(ui, widget_id, u32(ui->tags + widget + 0x38 + 12), 0, search);
	type = s16(ui->tags + widget);
	if (type == 2 || type == 3)
		return;                       /* (a list: its items are the menu's buttons) */
	for (k = 0; k < 64 && widget_child(ui, widget, k); k++)
		logo_walk(ui, widget_child(ui, widget, k), depth + 1, search);
}

/* the logo's possible pictures (search->wants[MOST_LOGOS]), best guesses first */
static void logo_wants(struct ui_tags const *ui, struct logo_search *search)
{
	unsigned long array, count, i, root = 0;
	char name[160];
	search->count = search->seen_count = 0;
	if (!ui->tags || ui->size < 16)
		return;
	array = u32(ui->tags);
	count = u32(ui->tags + 12);
	if (count > 65536 || !UI_IN(ui, array, count * 32))
		return;
	for (i = 0; i < count; i++)
	{
		unsigned char const *entry = ui->tags + UI_AT(array) + i * 32;
		size_t n;
		char const *last;
		if (u32(entry) != GROUP_WIDGET)
			continue;
		ui_name(ui, u32(entry + 12), name, sizeof(name));
		n = strlen(name);
		last = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
		if (n >= 19 && !strcmp(name + n - 19, "main_menu\\main_menu"))
		{
			root = u32(entry + 12);
			break;
		}
		if (!root && !strcmp(last, "main_menu"))
			root = u32(entry + 12);
	}
	if (root)
		logo_walk(ui, root, 0, search);
	for (i = 0; i < count; i++)
	{
		unsigned char const *entry = ui->tags + UI_AT(array) + i * 32;
		if (u32(entry) != GROUP_BITMAP)
			continue;
		ui_name(ui, u32(entry + 12), name, sizeof(name));
		if (strstr(name, "logo"))
			logo_look(ui, 0, u32(entry + 12), 1, search);
	}
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
	struct map_range picture;     /* (its pixels as ui.map has them, then the map picture made of them) */
	struct art_frame frame;
};

static int entry_compare(void const *a, void const *b)
{
	struct entry const *x = a, *y = b;
	return x->order != y->order ? x->order - y->order : x->map - y->map;
}

/* the files in folder matching pattern ("*.map") deleted */
static void delete_matching(char const *folder, char const *pattern)
{
	WIN32_FIND_DATAA found;
	char path[260];
	HANDLE find;
	text_print(path, sizeof(path), "%s\\%s", folder, pattern);
	find = FindFirstFileA(path, &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
		DeleteFileA(path);
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

/* the import's map folders in maps\<key>: its multiplayer maps' (levels 0) or its campaign levels' (levels 1, with
level.txt); how many of the other kind are left */
static int remove_folders(char const *key, int levels)
{
	WIN32_FIND_DATAA found;
	char pattern[200], folder[200], file[240];
	static char const *const files[] = {"map.link", "info.txt", "preview.bin", "level.txt", "bots.txt",
		"bots_learned.txt", "showcase.txt"};
	HANDLE find;
	unsigned k;
	int others = 0;
	text_print(pattern, sizeof(pattern), "%s\\maps\\%s\\*", STORAGE, key);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.')
				continue;
			text_print(folder, sizeof(folder), "%s\\maps\\%s\\%s", STORAGE, key, found.cFileName);
			text_print(file, sizeof(file), "%s\\map.link", folder);
			if (GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES)
				continue; /* (a map that isn't an import's: map sharing's copy, say) */
			text_print(file, sizeof(file), "%s\\level.txt", folder);
			if ((GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES) != levels)
			{
				others++;
				continue;
			}
			for (k = 0; k < sizeof(files) / sizeof(files[0]); k++)
			{
				text_print(file, sizeof(file), "%s\\%s", folder, files[k]);
				DeleteFileA(file);
			}
			/* (a consolidated map's small map, beside its link) */
			delete_matching(folder, "*.map");
			RemoveDirectoryA(folder);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	return others;
}

static void remove_multiplayer(char const *key)
{
	char folder[200], file[240];
	/* (the pack's shared file, a consolidated pack's, unless its campaign's levels are still there) */
	text_print(folder, sizeof(folder), "%s\\maps\\%s", STORAGE, key);
	if (!remove_folders(key, 0))
		delete_matching(folder, "*.shared");
	text_print(file, sizeof(file), "%s\\maps\\%s\\imported.txt", STORAGE, key);
	DeleteFileA(file);
	text_print(file, sizeof(file), "%s\\maps\\%s\\order.txt", STORAGE, key);
	DeleteFileA(file);
	text_print(file, sizeof(file), "%s\\maps\\%s\\series.bin", STORAGE, key);
	DeleteFileA(file);
	text_print(folder, sizeof(folder), "%s\\maps\\%s", STORAGE, key);
	RemoveDirectoryA(folder);
}

static void remove_campaign(char const *key)
{
	WIN32_FIND_DATAA found;
	char pattern[200], file[240];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\campaigns\\%s\\*", STORAGE, key);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			size_t n = strlen(found.cFileName);
			if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
				!((n > 4 && !_stricmp(found.cFileName + n - 4, ".bin")) || !_stricmp(found.cFileName, "campaign.txt")))
				continue;
			text_print(file, sizeof(file), "%s\\campaigns\\%s\\%s", STORAGE, key, found.cFileName);
			DeleteFileA(file);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	text_print(file, sizeof(file), "%s\\campaigns\\%s", STORAGE, key);
	RemoveDirectoryA(file);
	/* (its levels' folders in maps\<key>; the folder too when nothing else is in it) */
	text_print(file, sizeof(file), "%s\\maps\\%s", STORAGE, key);
	if (!remove_folders(key, 1) && GetFileAttributesA(file) != INVALID_FILE_ATTRIBUTES)
	{
		char check[240];
		text_print(check, sizeof(check), "%s\\imported.txt", file);
		if (GetFileAttributesA(check) == INVALID_FILE_ATTRIBUTES)
		{
			delete_matching(file, "*.shared");
			RemoveDirectoryA(file);
		}
	}
}

/* (what an installer before 41 imported from it, in E:'s storage, goes: E:'s room back) */
static void old_say(struct pack_scan const *scan, int multiplayer, int campaign, packs_say_t say)
{
	char line[200];
	int n = packs_remove_old(scan->path, multiplayer, campaign);
	if (!n)
		return;
	text_print(line, sizeof(line), "Took out the copy an older installer put in E:'s storage (%d map%s).", n, n == 1 ? "" : "s");
	say(0, line);
}

void packs_remove(struct pack_scan *scan, int multiplayer, int campaign, packs_say_t say)
{
	char line[200];
	int n;
	if (multiplayer && (n = count_links(scan->key)) != 0)
	{
		remove_multiplayer(scan->key);
		text_print(line, sizeof(line), "Took out %d multiplayer maps (maps\\%s).", n, scan->key);
		say(0, line);
	}
	if (campaign && count_levels(scan->key))
	{
		remove_campaign(scan->key);
		text_print(line, sizeof(line), "Took out the campaign (campaigns\\%s).", scan->key);
		say(0, line);
	}
	old_say(scan, multiplayer, campaign, say);
	if (multiplayer)
		scan->multiplayer_imported = 0;
	if (campaign)
		scan->campaign_imported = 0;
}

/* the maps of one kind (type) to import, with their names and pictures' places (entries[PACK_MAPS]); how many */
/* a menu's lists (type 1: the multiplayer map select's, 0: the campaign's level select): the menu's own, wherever the
pack put them; else Halo's */
static void list_tags(struct ui_tags const *ui, int type, unsigned long *names_out, unsigned long *texts_out,
	unsigned long *pictures_out)
{
	unsigned long names = 0, texts = 0, pictures = 0;
	if (!menu_tags(ui, type == 1 ? 22 : 6, &names, &texts, &pictures) || !names || !texts || !pictures)
	{
		if (type == 1)
		{
			names = names ? names : ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\mp_map_list");
			texts = texts ? texts : ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\multiplayer_type_select\\mp_map_select\\map_data");
			pictures = pictures ? pictures : ui_tag(ui, 0x6269746d, "ui\\shell\\bitmaps\\mp_map_grafix");
		}
		else
		{
			names = names ? names : ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\map_list");
			texts = texts ? texts : ui_tag(ui, 0x75737472, "ui\\shell\\main_menu\\solo_level_select\\map_data");
			pictures = pictures ? pictures : ui_tag(ui, 0x6269746d, "ui\\shell\\bitmaps\\sp_levels");
		}
	}
	*names_out = names, *texts_out = texts, *pictures_out = pictures;
}

/* the multiplayer menu's place for a map: in the pack's XBE's list, else Halo's 13; -1 when it isn't there */
static int multiplayer_index(char const *name, char (*xbe)[40], int xbe_count)
{
	int i;
	for (i = 0; i < xbe_count; i++)
		if (!_stricmp(xbe[i], name))
			return i;
	for (i = 0; !xbe_count && i < 13; i++)
		if (!_stricmp(halos_order[i], name))
			return i;
	return -1;
}

static int gather(struct pack_scan const *scan, int type, struct ui_tags const *ui, char (*xbe)[40], int xbe_count,
	struct entry *entries)
{
	unsigned long names, texts, pictures, mp_names = 0, mp_texts = 0, mp_pictures = 0;
	int n = 0, k, i;
	list_tags(ui, type, &names, &texts, &pictures);
	if (type == 0)
		list_tags(ui, 1, &mp_names, &mp_texts, &mp_pictures);
	for (k = 0; k < scan->count; k++)
	{
		struct pack_map const *map = &scan->maps[k];
		struct entry *e = &entries[n];
		int index = -1;
		if (map->type != type || (type == 1 && map->halos))
			continue;
		if (type == 1)
			index = multiplayer_index(map->name, xbe, xbe_count);
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
			ui_picture(ui, pictures, (unsigned long)index, &e->picture, &e->frame);
		}
		else if (type == 0 && (i = multiplayer_index(map->name, xbe, xbe_count)) >= 0)
		{
			/* (a level under a multiplayer map's name, as co-op packs' are: what the multiplayer menu says of it) */
			e->order = 500 + i;
			ui_string(ui, mp_names, (unsigned long)i, 1, e->name, sizeof(e->name));
			ui_string(ui, mp_texts, (unsigned long)i, 0, e->description, sizeof(e->description));
			ui_picture(ui, mp_pictures, (unsigned long)i, &e->picture, &e->frame);
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
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s", STORAGE, scan->key, name);
		CreateDirectoryA(path, NULL);
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\info.txt", STORAGE, scan->key, name);
		text_print(text, sizeof(text), "%s\r\n%s\r\n", e->name, e->description);
		failed |= write_file(path, text, strlen(text));
		if (e->picture.size)
		{
			text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\preview.bin", STORAGE, scan->key, name);
			failed |= write_file(path, e->picture.data, PICTURE_SIZE);
		}
		/* (the link last: a map is in the list once it's there) */
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\map.link", STORAGE, scan->key, name);
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
	text_print(path, sizeof(path), "%s\\maps\\%s\\order.txt", STORAGE, scan->key);
	failed |= write_file(path, order, (unsigned long)at);
	text_print(path, sizeof(path), "%s\\maps\\%s\\imported.txt", STORAGE, scan->key);
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
	make_folders("maps", scan->key);
	at = text_print(text, 512, "Name: %s\r\nFrom: %s\r\n", last_part(scan->path), scan->path);
	for (k = 0; k < n; k++)
	{
		struct entry const *e = &entries[k];
		char const *name = scan->maps[e->map].name;
		at += text_print(text + at, sizeof(line), "Level: %s\r\nTitle: %s\r\nText: %s\r\n", name, e->name,
			e->description);
		if (e->picture.size)
		{
			text_print(path, sizeof(path), "%s\\campaigns\\%s\\%s.bin", STORAGE, scan->key, name);
			failed |= write_file(path, e->picture.data, PICTURE_SIZE);
		}
		/* (the level as a folder map, played as "+<key>/<level>": maps\<key>\<level>, as a multiplayer map's, with
		level.txt; the game leaves it out of the multiplayer list: its header says campaign) */
		if (strlen(scan->key) + 1 + strlen(name) > MAXIMUM_FOLDER_NAME)
		{
			text_print(line, sizeof(line), "  %s: its name is too long for the game's map list; skipped.", name);
			say(1, line);
			continue;
		}
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s", STORAGE, scan->key, name);
		CreateDirectoryA(path, NULL);
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\level.txt", STORAGE, scan->key, name);
		text_print(line, sizeof(line), "Campaign: %s\r\n", scan->key);
		failed |= write_file(path, line, strlen(line));
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\info.txt", STORAGE, scan->key, name);
		text_print(line, sizeof(line), "%s\r\n%s\r\n", e->name, e->description);
		failed |= write_file(path, line, strlen(line));
		if (e->picture.size)
		{
			text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\preview.bin", STORAGE, scan->key, name);
			failed |= write_file(path, e->picture.data, PICTURE_SIZE);
		}
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\map.link", STORAGE, scan->key, name);
		text_print(line, sizeof(line), "%s\\maps\\%s.map\r\n", scan->path, name);
		failed |= write_file(path, line, strlen(line));
		text_print(line, sizeof(line), "  %s: %s%s", name, e->name, e->picture.size ? "" : " (no picture)");
		say(0, line);
	}
	text_print(path, sizeof(path), "%s\\campaigns\\%s\\campaign.txt", STORAGE, scan->key);
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
	unsigned char header[MAP_HEADER], *pictures = NULL, *series = NULL;
	unsigned long series_size = 0;
	struct logo_search logos = {NULL, 0};
	unsigned long file_size;
	int xbe_count = 0, mp_count = 0, sp_count = 0, k, n, failed = 0, read_ok = 1;

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
	text_print(line, sizeof(line), "%s\\default.xbe", scan->path);
	xbe_count = xbe_levels(line, xbe, XBE_LEVELS);
	if (multiplayer)
		mp_count = gather(scan, 1, &ui, xbe, xbe_count, entries);
	if (campaign)
		sp_count = gather(scan, 0, &ui, xbe, xbe_count, entries + mp_count);
	/* the pictures and the logo's, the second time through ui.map: each map's picture made Halo's kind (packart.c) */
	n = mp_count + sp_count;
	ranges = malloc((n + MOST_LOGOS + 1) * sizeof(*ranges));
	pictures = malloc((n + 1) * PICTURE_SIZE);
	logos.wants = calloc(MOST_LOGOS, sizeof(*logos.wants));
	if (!ranges || !pictures || !logos.wants)
	{
		text_print(error, 256, "out of memory");
		goto done;
	}
	logos.count = 0;
	if (multiplayer && mp_count)
		logo_wants(&ui, &logos);
	{
		int count = 0, ok = 1;
		for (k = 0; k < n && ok; k++)
		{
			if (!entries[k].picture.size)
				continue;
			ok = (entries[k].picture.data = malloc(entries[k].picture.size)) != NULL;
			ranges[count++] = entries[k].picture;
		}
		for (k = 0; k < logos.count && ok; k++)
		{
			ok = (logos.wants[k].range.data = malloc(logos.wants[k].range.size)) != NULL;
			ranges[count++] = logos.wants[k].range;
		}
		if (!ok || (count && map_read(ui_path, ranges, count, &file_size, line)))
		{
			say(1, ok ? line : "out of memory for the pictures");
			read_ok = 0;
		}
		for (k = 0; k < n; k++)
		{
			unsigned char *raw = entries[k].picture.data;
			struct art_frame const *f = &entries[k].frame;
			if (!raw)
				continue;
			if (!read_ok)
			{
				entries[k].picture.size = 0;
				free(raw);
				continue;
			}
			entries[k].picture.data = pictures + k * PICTURE_SIZE;
			if (f->width == 128 && f->height == 128 && f->format == 14 && f->size >= PICTURE_SIZE / 2)
			{
				/* (a 128x128 one, as some of HMP's packs': top left, as the pack's own menu showed it) */
				memcpy(entries[k].picture.data, raw, PICTURE_SIZE / 2);
				picture_widen(entries[k].picture.data);
			}
			else if (art_picture(f, raw, entries[k].picture.data))
				entries[k].picture.size = 0;
			free(raw);
		}
		for (k = 0; k < n; k++)
			if (!entries[k].picture.size)
				entries[k].picture.data = NULL;
	}
	/* (the logo: the best of what's on the main menu, as Halo Map Porter picks it) */
	{
		struct art_logo best = {0, 0, 0, NULL};
		int stock = 0;
		for (k = 0; k < logos.count; k++)
		{
			struct logo_want *want = &logos.wants[k];
			struct art_logo logo;
			unsigned char *rgba;
			if (!want->range.data || !read_ok)
				;
			else if (art_hash(want->range.data, want->range.size) == ART_STOCK_LOGO)
				stock = 1;
			else if ((rgba = art_decode(&want->frame, want->range.data)) != NULL &&
				art_logo_cut(rgba, want->frame.width, want->frame.height, has_any(want->names, logo_words), want->screen,
					want->named_only, &logo))
			{
				if (logo.score > best.score || !best.rgba)
				{
					free(best.rgba);
					best = logo;
				}
				else
					free(logo.rgba);
			}
			free(want->range.data);
			want->range.data = NULL;
		}
		if (best.rgba && (series = malloc(ART_SERIES_MOST)) != NULL)
			series_size = art_series(&best, 32, series);
		free(best.rgba);
		if (multiplayer && mp_count)
			say(0, series_size ? "Its logo, from its main menu, stamps its maps' pictures." : stock ?
				"Its main menu has Halo's own logo: no stamp." : "No logo found on its main menu: no stamp.");
	}
	free(ui.tags);
	ui.tags = NULL;
	if (multiplayer && mp_count)
	{
		remove_multiplayer(scan->key);
		text_print(line, sizeof(line), "Multiplayer: %d maps, as the pack \"%s\" in the map list:", mp_count, scan->key);
		say(0, line);
		failed |= write_multiplayer(scan, entries, mp_count, say);
		/* (its logo, stamped on its maps' pictures by the game: hmp_maps.c) */
		text_print(line, sizeof(line), "%s\\maps\\%s\\series.bin", STORAGE, scan->key);
		if (series_size)
			failed |= write_file(line, series, series_size);
	}
	if (campaign && sp_count)
	{
		remove_campaign(scan->key);
		text_print(line, sizeof(line), "Campaign: %d levels, as \"%s\":", sp_count, last_part(scan->path));
		say(0, line);
		failed |= write_campaign(scan, entries + mp_count, sp_count, say);
	}
	if (failed)
		text_print(error, 256, "some files couldn't be written in %s (is its drive full?)", STORAGE);
	else
		old_say(scan, multiplayer && mp_count, campaign && sp_count, say);
done:
	if (logos.wants)
		for (k = 0; k < logos.count; k++)
			free(logos.wants[k].range.data);
	free(logos.wants);
	free(series);
	free(ui.tags);
	free(ranges);
	free(pictures);
	free(entries);
	free(xbe);
	return error[0] != 0;
}

/* ---- the packs known: what consolidating them saves and takes (packs_known.h, made by tools/gen_packs_known.py from
packs_known.txt: Llama's runs on a console) ---- */

#include "packs_known.h"

struct pack_known const *packs_known(unsigned long fingerprint)
{
	unsigned k;
	for (k = 0; k < sizeof(known_packs) / sizeof(known_packs[0]); k++)
		if (known_packs[k].fingerprint == fingerprint)
			return &known_packs[k];
	return NULL;
}
