/*
LINKED_MAPS.C

A dedicated server's linked maps: a Custom Edition map and its Xbox port played together, the server and its PCs on
the Custom Edition map, its Xboxes on the port (the native builds' host side; an Xbox needs nothing of its own: it plays
a folder map, as the host tells it). docs/linked_maps.md and HaloMapPorter's Perfect Conversion brief have the why.

The link. A link file (.h1ulink, Halo Map Porter's, or tools/h1ulink.py's for tests) sits in the Xbox port's folder
(maps\<folder>\, beside its .map). It names the Custom Edition map (custom_maps\<name>, its size and its hash, map
sharing's: map_share_files.c), the Xbox map (its path under the maps folder, size, hash), and every tag of both with
its tag ID in each. When the host's game is on a Custom Edition map, the folder maps' links are looked through for one
naming that map at the game's version (the first 32 bits of its hash, which the game's settings carry); its Xbox map
must be the one in that folder, of the hash the link names (worked out once a map, on the game's thread). That link is
then the game's (linked_map_key, linked_map_version), until the game's map changes.

What it does with it:
- An Xbox's copy of the game's settings names the Xbox map ("+folder", its version as an Xbox works it out:
  hmp_share_map_version) instead of the Custom Edition map (network_server_message_handler.c): the Xbox loads its own
  copy, or downloads it from the host (map_share_xbox.c serves the linked folder map).
- The tag IDs in the host's messages to an Xbox are the Xbox map's, and those in an Xbox's to the host the Custom
  Edition map's (network_distributed.c: linked_map_tag_to_xbox, linked_map_tag_to_ce). A tag not in the other map is
  NONE there.
- The scripts run on the host alone (network_coop.c's network_scripts_host_only: the two copies may carry different
  scripts).
- The game is listed for Xboxes (network_server_manager.c's oxbox_server_xbox_joinable).

An Xbox is a machine whose hardware id carries the Xbox's mark (p2p.h's P2P_XBOX_HARDWARE_ID_MARK).
*/

#if !defined(HALO_XBOX_TARGET)

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_server_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* ---------- constants */

#define LINK_PIECE_BYTES 0x40000
#define LINK_HASH_LABEL "OpenCE map 1"
#define LINK_MAXIMUM_TAGS 65536
#define LINK_LINE_SIZE 600
#define XBOX_HARDWARE_ID_MARK "0b0c0b0c"

/* ---------- structures */

struct linked_tag
{
	unsigned long from, to;
};

/* ---------- globals */

static struct
{
	/* the game's map as last looked at: its level name and version */
	char looked_level[128];
	unsigned long looked_version;
	/* the link found (none: active FALSE) */
	boolean active;
	char key[40];
	char link_path[300];
	unsigned long xbox_version;
	struct linked_tag *to_xbox;              /* by the Custom Edition map's tag ID */
	struct linked_tag *to_ce;                /* by the Xbox map's */
	long tag_count;
	/* the Xbox copy of the game's settings */
	struct network_game xbox_game;
} linked_maps;

/* ---------- prototypes */

void platform_log(char const *format, ...);
void p2p_blake2b_256(const void *data, unsigned long size, unsigned char *digest);
boolean custom_edition_level_name(char const *level_name);
long hmp_maps_find(char const *level_name);
boolean hmp_map_file_path(char const *level_name, char *path, long size);
unsigned long hmp_share_map_version(char const *level_name);
char const *folder_map_key(long map_index);
int folder_maps_each(void (*each)(void *context, char const *key), void *context);
char const *network_game_server_machine_hardware_id(long machine_index);
void network_game_server_port_set_host_scripts(boolean host_scripts);
struct network_game *network_game_server_get_game(struct network_game_server *server);
int config_boolean(char const *name);

/* ---------- private code */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ieq(char const *a, char const *b)
{
	while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
	return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static int linked_tag_compare(void const *a, void const *b)
{
	unsigned long x = ((struct linked_tag const *)a)->from, y = ((struct linked_tag const *)b)->from;

	return x < y ? -1 : x > y;
}

/* map sharing's hash of a file (an Xbox path), as 64 hex digits; FALSE if it can't be read */
static boolean file_hash_text(char const *path, unsigned long *size, char *text)
{
	FILE *file = fopen(path, "rb");
	byte *piece;
	byte *hashes = NULL;
	long count = 0;
	long capacity = 0;
	unsigned char digest[32];
	long index;

	if (!file)
		return FALSE;
	piece = malloc(LINK_PIECE_BYTES);
	if (!piece)
	{
		fclose(file);
		return FALSE;
	}
	*size = 0;
	for (;;)
	{
		size_t got = fread(piece, 1, LINK_PIECE_BYTES, file);

		if (!got)
			break;
		if (count == capacity)
		{
			byte *grown;

			capacity = capacity ? capacity * 2 : 256;
			grown = realloc(hashes, 12 + 4 + (size_t)capacity * 32);
			if (!grown)
			{
				free(hashes);
				free(piece);
				fclose(file);
				return FALSE;
			}
			hashes = grown;
		}
		p2p_blake2b_256(piece, (unsigned long)got, hashes + 16 + count * 32);
		count++;
		*size += (unsigned long)got;
		if (got < LINK_PIECE_BYTES)
			break;
	}
	fclose(file);
	free(piece);
	if (!hashes)
	{
		hashes = malloc(16);
		if (!hashes)
			return FALSE;
	}
	memcpy(hashes, LINK_HASH_LABEL, 12);
	for (index = 0; index < 4; index++)
		hashes[12 + index] = (byte)(*size >> (index * 8));
	p2p_blake2b_256(hashes, (unsigned long)(16 + count * 32), digest);
	free(hashes);
	for (index = 0; index < 32; index++)
		sprintf(text + index * 2, "%02x", digest[index]);
	text[64] = 0;
	return TRUE;
}

/* a hash's first 32 bits, as the game's version (map_share_hash_version: 0 is 1) */
static unsigned long hash_text_version(char const *text)
{
	unsigned long version = 0;
	int index;

	for (index = 0; index < 8 && text[index]; index++)
	{
		int c = lower((unsigned char)text[index]);

		version = version << 4 | (unsigned long)(c >= 'a' ? c - 'a' + 10 : c - '0');
	}
	return version ? version : 1;
}

/* the next word of a line (spaces between), into word; the rest after it */
static char const *next_word(char const *line, char *word, int size)
{
	int length = 0;

	while (*line == ' ' || *line == '\t')
		line++;
	while (*line && *line != ' ' && *line != '\t' && *line != '\r' && *line != '\n')
	{
		if (length < size - 1)
			word[length++] = *line;
		line++;
	}
	word[length] = 0;
	return line;
}

struct link_search
{
	char const *level;                       /* custom_maps\<name> */
	unsigned long version;
	boolean found;
};

/* a link file's first lines: whether it names the Custom Edition map at that version (and its Xbox map's hash) */
static boolean link_header_matches(char const *path, char const *level, unsigned long version, char *xbox_hash)
{
	FILE *file = fopen(path, "r");
	char line[LINK_LINE_SIZE];
	boolean ce_matches = FALSE, xbox_named = FALSE, first = TRUE;

	if (!file)
		return FALSE;
	while (fgets(line, sizeof(line), file))
	{
		char word[160], name[160], size_text[32], hash[80];
		char const *rest;

		if (first)
		{
			first = FALSE;
			if (strncmp(line, "h1u-link 1", 10))
				break;
			continue;
		}
		rest = next_word(line, word, sizeof(word));
		if (!strcmp(word, "ce"))
		{
			rest = next_word(rest, name, sizeof(name));
			rest = next_word(rest, size_text, sizeof(size_text));
			next_word(rest, hash, sizeof(hash));
			ce_matches = !ieq(name, level) && strlen(hash) == 64 && hash_text_version(hash) == version;
		}
		else if (!strcmp(word, "xbox"))
		{
			rest = next_word(rest, name, sizeof(name));
			rest = next_word(rest, size_text, sizeof(size_text));
			next_word(rest, hash, sizeof(hash));
			if (strlen(hash) == 64)
			{
				strcpy(xbox_hash, hash);
				xbox_named = TRUE;
			}
		}
		else if (!strcmp(word, "tag"))
			break;
	}
	fclose(file);
	return ce_matches && xbox_named;
}

/* parses a link's tag lines into the tables */
static boolean link_read_tags(char const *path)
{
	FILE *file = fopen(path, "r");
	char line[LINK_LINE_SIZE];
	long capacity = 0;
	long count = 0;
	struct linked_tag *tags = NULL;

	if (!file)
		return FALSE;
	while (fgets(line, sizeof(line), file))
	{
		char word[16], group[16], ce_text[16], xbox_text[16];
		char const *rest = next_word(line, word, sizeof(word));

		if (strcmp(word, "tag"))
			continue;
		rest = next_word(rest, group, sizeof(group));
		rest = next_word(rest, ce_text, sizeof(ce_text));
		next_word(rest, xbox_text, sizeof(xbox_text));
		if (!strcmp(ce_text, "-") || !strcmp(xbox_text, "-"))
			continue;
		if (count == capacity)
		{
			struct linked_tag *grown;

			if (capacity >= LINK_MAXIMUM_TAGS)
				break;
			capacity = capacity ? capacity * 2 : 1024;
			grown = realloc(tags, (size_t)capacity * sizeof(*tags));
			if (!grown)
			{
				free(tags);
				fclose(file);
				return FALSE;
			}
			tags = grown;
		}
		tags[count].from = strtoul(ce_text, NULL, 16);
		tags[count].to = strtoul(xbox_text, NULL, 16);
		count++;
	}
	fclose(file);
	free(linked_maps.to_xbox);
	free(linked_maps.to_ce);
	linked_maps.to_xbox = tags;
	linked_maps.to_ce = count ? malloc((size_t)count * sizeof(*tags)) : NULL;
	linked_maps.tag_count = count;
	if (count && !linked_maps.to_ce)
	{
		linked_maps.tag_count = 0;
		return FALSE;
	}
	if (count)
	{
		long index;

		for (index = 0; index < count; index++)
		{
			linked_maps.to_ce[index].from = tags[index].to;
			linked_maps.to_ce[index].to = tags[index].from;
		}
		qsort(linked_maps.to_xbox, (size_t)count, sizeof(*tags), linked_tag_compare);
		qsort(linked_maps.to_ce, (size_t)count, sizeof(*tags), linked_tag_compare);
	}
	return TRUE;
}

/* a folder map: whether one of its link files links the game's Custom Edition map, which becomes the game's link */
static void link_search_folder(void *context, char const *key)
{
	struct link_search *search = context;
	char map_path[300], pattern[300], folder[300], link_path[300];
	char xbox_hash[80], hash[80];
	WIN32_FIND_DATAA find;
	HANDLE handle;
	char *slash;

	if (search->found || !hmp_map_file_path(key, map_path, sizeof(map_path)))
		return;
	snprintf(folder, sizeof(folder), "%s", map_path);
	slash = strrchr(folder, '\\');
	if (!slash)
		return;
	*slash = 0;
	snprintf(pattern, sizeof(pattern), "%s\\*.h1ulink", folder);
	handle = FindFirstFileA(pattern, &find);
	if (handle == INVALID_HANDLE_VALUE)
		return;
	do
	{
		unsigned long size = 0;

		if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		snprintf(link_path, sizeof(link_path), "%s\\%s", folder, find.cFileName);
		if (!link_header_matches(link_path, search->level, search->version, xbox_hash))
			continue;
		/* (the folder's .map must be the Xbox map the link names) */
		if (!file_hash_text(map_path, &size, hash) || ieq(hash, xbox_hash))
		{
			platform_log("linked maps: %s names %s, but %s isn't the Xbox map it names (another build?)", link_path,
				search->level, map_path);
			continue;
		}
		if (!link_read_tags(link_path))
			continue;
		snprintf(linked_maps.key, sizeof(linked_maps.key), "%s", key);
		snprintf(linked_maps.link_path, sizeof(linked_maps.link_path), "%s", link_path);
		linked_maps.xbox_version = hmp_share_map_version(key);
		linked_maps.active = TRUE;
		search->found = TRUE;
		platform_log("linked maps: %s (version %08lx) is linked to the Xbox map %s (version %08lx, %ld tags in both): %s",
			search->level, search->version, key, linked_maps.xbox_version, linked_maps.tag_count, link_path);
		break;
	}
	while (FindNextFileA(handle, &find));
	CloseHandle(handle);
}

static long linked_tag_find(struct linked_tag const *table, unsigned long tag)
{
	long low = 0, high = linked_maps.tag_count - 1;

	while (low <= high)
	{
		long middle = (low + high) / 2;

		if (table[middle].from == tag)
			return middle;
		if (table[middle].from < tag)
			low = middle + 1;
		else
			high = middle - 1;
	}
	return NONE;
}

static long linked_tag_translate(struct linked_tag const *table, long tag)
{
	long index;

	if (!linked_maps.active || tag == NONE || !table)
		return tag;
	index = linked_tag_find(table, (unsigned long)tag);
	if (index == NONE)
	{
		/* (the same tag at another salt: by its index alone) */
		long scan;

		for (scan = 0; scan < linked_maps.tag_count; scan++)
		{
			if ((table[scan].from & 0xFFFF) == ((unsigned long)tag & 0xFFFF))
				return (long)table[scan].to;
		}
		return NONE;
	}
	return (long)table[index].to;
}

/* ---------- public code */

/* every frame (map_share_xbox_update): the host's game's link, looked for again when its map changes */
void linked_maps_update(void)
{
	struct network_game_server *server = global_network_game_server_get();
	struct network_game *game = server ? network_game_server_get_game(server) : NULL;
	struct link_search search;

	if (!server || !game || !game->map.name[0])
	{
		linked_maps.looked_level[0] = 0;
		linked_maps.active = FALSE;
		return;
	}
	if (!strcmp(linked_maps.looked_level, game->map.name) && linked_maps.looked_version == (unsigned long)game->map.version)
		return;
	snprintf(linked_maps.looked_level, sizeof(linked_maps.looked_level), "%s", game->map.name);
	linked_maps.looked_version = (unsigned long)game->map.version;
	linked_maps.active = FALSE;
	if (!custom_edition_level_name(game->map.name) || !game->map.version)
	{
		network_game_server_port_set_host_scripts(config_boolean("server.host_scripts") != 0);
		return;
	}
	search.level = game->map.name;
	search.version = (unsigned long)game->map.version;
	search.found = FALSE;
	folder_maps_each(link_search_folder, &search);
	if (!search.found)
		platform_log("linked maps: no link for %s (version %08lx): Xboxes can't join its games", game->map.name,
			(unsigned long)game->map.version);
	/* (a linked game's scripts run on the host alone, the two maps' copies of them perhaps not the same:
	network_coop.c's network_scripts_host_only; else as server.host_scripts says) */
	network_game_server_port_set_host_scripts(search.found || config_boolean("server.host_scripts"));
}

/* whether the host's game is on a linked map: its Xbox map's key and version */
boolean linked_map_active(void)
{
	return linked_maps.active;
}

char const *linked_map_key(void)
{
	return linked_maps.active ? linked_maps.key : NULL;
}

unsigned long linked_map_version(void)
{
	return linked_maps.active ? linked_maps.xbox_version : 0;
}

/* whether a machine of the host's game is an Xbox (its hardware id's mark), which plays a linked game's Xbox map */
boolean linked_map_machine_is_xbox(long machine_index)
{
	char const *id;

	if (!linked_maps.active || machine_index < 0)
		return FALSE;
	id = network_game_server_machine_hardware_id(machine_index);
	return id && !strncmp(id, XBOX_HARDWARE_ID_MARK, sizeof(XBOX_HARDWARE_ID_MARK) - 1);
}

/* a tag ID of the Custom Edition map as the Xbox map's, and back (NONE: none there) */
long linked_map_tag_to_xbox(long tag)
{
	return linked_tag_translate(linked_maps.to_xbox, tag);
}

long linked_map_tag_to_ce(long tag)
{
	return linked_tag_translate(linked_maps.to_ce, tag);
}

/* the game's settings as an Xbox in a linked game has them: the Xbox map's level name and version; the game's own for
any other machine or game */
void const *linked_map_game_for_machine(void const *game, long game_size, long machine_index)
{
	if (!linked_map_machine_is_xbox(machine_index) || game_size != (long)sizeof(struct network_game))
		return game;
	csmemcpy(&linked_maps.xbox_game, game, sizeof(linked_maps.xbox_game));
	csmemset(linked_maps.xbox_game.map.name, 0, sizeof(linked_maps.xbox_game.map.name));
	csstrncpy(linked_maps.xbox_game.map.name, linked_maps.key, sizeof(linked_maps.xbox_game.map.name) - 1);
	linked_maps.xbox_game.map.version = (long)linked_maps.xbox_version;
	return &linked_maps.xbox_game;
}

#endif
