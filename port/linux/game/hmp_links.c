/*
HMP_LINKS.C

An Xbox in someone else's PC game on a Custom Edition or MCC map (a dedicated server or a PC host of the PC builds,
crossplay), played here on that map's Xbox port: a folder map whose folder has a link file (.h1ulink, Halo Map
Porter's) naming the PC map. It's the Xbox side of what a Halo 1 Unlimited server does for its Xboxes
(port/linux/game/linked_maps.c on the Server branch, docs/linked_maps.md): a server that isn't ours sends the PC map's
tag IDs and its name, and this Xbox works them out itself.

The link. Its first lines name the PC map and the Xbox map:
  ce   custom_maps\<name>   <size>   <hash>                        (a Custom Edition map: map sharing's hash)
  mcc  mcc_maps\<package>\<level>   <content sha256>   <metadata sha256 or ->   <checksum>   (an MCC map, OpenCE's
       identity of it: the game's settings carry it, version 42 on)
  xbox <folder>\<file>.map   <size>   <hash>
  tag <group> <PC map's tag ID> <Xbox map's tag ID> <path>         (hex, - where the map hasn't it)
A Custom Edition game matches by its name and version (the hash's first 32 bits); an MCC game by its level's file name
(not its package's folder name, which OpenCE's players may differ in), checksum (its version) and content SHA-256 (the map
and the bitmaps/sounds files it uses); the metadata SHA-256 is read but not matched. The folder's .map is checked against the
link's tag lines as it loads (each Xbox tag ID the map's tag of that path), not by the xbox line's size and hash.

What it does:
- The games list shows a PC game on a Custom Edition or MCC map this Xbox has a link for (by its name: the version is
  checked on joining), under the Xbox map's name (hmp_links_listed, hmp_links_shown_name).
- Joining: the game's settings name the Xbox map ("+folder", its version as the Xbox works it out) in place of the PC
  map (hmp_links_join, network_client_message_handler.c's crossplay_settings_to_xbox). No link at that version: the
  Xbox doesn't join, and says why.
- The tag IDs in the host's messages are translated to the Xbox map's, and those in this Xbox's to the host's
  (network_distributed.c, linked_map_tag_to_xbox/_ce, as on the server): a table of each, read from the link as the
  map loads, in pages of the AI's pools' room the map leaves (hmp_links_memory, cache_files.c), not kept otherwise.
- The scripts: each machine runs its own copy, as the host has it (a server's server.host_scripts; HMP keeps the
  port's scripts the map's).
*/

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_CROSSPLAY)

/* ---------- headers */

#include "cseries.h"
#include "game/game.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "interface/ui_widget.h"
#include "hmp.h"

#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* ---------- constants */

#define LINK_LINE_SIZE 400
#define LINK_MAXIMUM 512
/* the message a translated copy of the host's is made in: a whole one (message_header.h's MAXIMUM_MESSAGE_SIZE) */
#define LINK_SCRATCH_SIZE 0x1010

enum
{
	_link_kind_ce = 1,
	_link_kind_mcc,
};

/* ---------- structures */

struct link_entry
{
	unsigned long name_hash;                 /* the PC map's name (link_name_hash) */
	unsigned long version;                   /* a Custom Edition map's hash's first 32 bits; an MCC map's checksum */
	unsigned long content;                   /* an MCC map's content SHA-256's first 4 bytes (0: none) */
	unsigned long metadata;                  /* ... its metadata SHA-256's (0: the link doesn't say) */
	unsigned long file_hash;                 /* the link file's name (link_name_hash) */
	short map_index;                         /* the folder map (hmp_maps.c) */
	short kind;
};

/* ---------- globals */

static struct
{
	/* the links of the folder maps (built when first needed after a map scan) */
	struct link_entry *entries;
	long count;
	long generation;                         /* hmp_maps_generation's when built (0: not built) */
	/* the game joined: its Xbox map's key, the link (none: active FALSE) */
	boolean active;
	char key[64];
	char pc_name[64];
	unsigned long file_hash;
	/* the tables, in the map's memory (NULL: not read): the Xbox map's tag ID by the PC map's tag index, and back */
	long *to_xbox;
	long to_xbox_count;
	long *to_ce;
	long to_ce_count;
	byte *scratch;
	unsigned long refused_time;              /* GetTickCount's when it last left a game, saying why (0: never) */
} links;

/* ---------- prototypes */

/* (the retail allocator's, as hmp_maps.c has them: xtl.h's declarations don't say stdcall, and a call through them
leaves the stack 8 bytes off) */
void *__stdcall LocalAlloc(unsigned int flags, unsigned int bytes);
void *__stdcall LocalFree(void *memory);
long hmp_maps_generation(void);
unsigned long hmp_share_map_version(char const *level_name);
boolean hmp_map_file_path(char const *level_name, char *path, long size);

/* ---------- private code */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* a name's hash: lower case, '/' as '\\' (FNV-1a) */
static unsigned long link_name_hash(char const *name)
{
	unsigned long hash = 2166136261UL;

	for (; *name; name++)
	{
		int c = lower((unsigned char)*name);

		if (c == '/')
			c = '\\';
		hash = (hash ^ (unsigned long)c) * 16777619UL;
	}
	return hash;
}

/* the name a PC map is matched by: an MCC map's level file name alone (OpenCE's players may keep a package
under another folder name: the level and its contents decide), a Custom Edition map's whole name */
static unsigned long link_match_hash(char const *name, short kind)
{
	char const *leaf = kind == _link_kind_mcc ? strrchr(name, '\\') : NULL;
	char const *slash = kind == _link_kind_mcc ? strrchr(name, '/') : NULL;

	if (slash && (!leaf || slash > leaf))
		leaf = slash;
	return link_name_hash(leaf ? leaf + 1 : name);
}

/* the PC map's kind by its name (0: neither: a stock or folder map) */
static short link_kind_of(char const *name)
{
	static char const ce[] = "custom_maps", mcc[] = "mcc_maps";
	int i;

	if (!name)
		return 0;
	for (i = 0; ce[i] && lower((unsigned char)name[i]) == ce[i]; i++);
	if (!ce[i] && (name[i] == '\\' || name[i] == '/'))
		return _link_kind_ce;
	for (i = 0; mcc[i] && lower((unsigned char)name[i]) == mcc[i]; i++);
	if (!mcc[i] && (name[i] == '\\' || name[i] == '/'))
		return _link_kind_mcc;
	return 0;
}

static unsigned long hex_value(char const *text, int digits)
{
	unsigned long value = 0;
	int index;

	for (index = 0; index < digits && text[index]; index++)
	{
		int c = lower((unsigned char)text[index]);

		value = value << 4 | (unsigned long)(c >= 'a' ? c - 'a' + 10 : c - '0');
	}
	return value;
}

/* the next word of a line, into word; the rest after it */
static char const *next_word(char const *line, char *word, int size)
{
	int length = 0;

	while (*line == ' ' || *line == '\t')
		line++;
	while (*line && *line != ' ' && *line != '\t')
	{
		if (length < size - 1)
			word[length++] = *line;
		line++;
	}
	word[length] = 0;
	return line;
}

/* a text file's lines to each (lines past LINK_LINE_SIZE cut short); each returns FALSE to stop */
static boolean link_lines(char const *path, boolean (*each)(void *context, char const *line), void *context)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	char raw[1024], line[LINK_LINE_SIZE];
	long length = 0;
	boolean going = TRUE;

	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	while (going)
	{
		unsigned long got = 0, k;

		if (!ReadFile(file, raw, sizeof(raw), &got, NULL) || !got)
			break;
		for (k = 0; k < got && going; k++)
		{
			char c = raw[k];

			if (c == '\n' || c == '\r')
			{
				line[length] = 0;
				if (length)
					going = each(context, line);
				length = 0;
			}
			else if (length < LINK_LINE_SIZE - 1)
				line[length++] = c;
		}
	}
	if (going && length)
	{
		line[length] = 0;
		each(context, line);
	}
	CloseHandle(file);
	return TRUE;
}

struct link_header
{
	boolean first;
	boolean valid;
	struct link_entry entry;
	boolean pc_named, xbox_named;
};

/* a link's first lines: what it names */
static boolean link_header_line(void *context, char const *line)
{
	struct link_header *header = (struct link_header *)context;
	char word[16], name[160], a[80], b[80], c[80];
	char const *rest;

	if (header->first)
	{
		header->first = FALSE;
		header->valid = !strncmp(line, "h1u-link 1", 10);
		return header->valid;
	}
	rest = next_word(line, word, sizeof(word));
	if (!strcmp(word, "tag"))
		return FALSE;
	if (!strcmp(word, "ce"))
	{
		rest = next_word(rest, name, sizeof(name));
		rest = next_word(rest, a, sizeof(a));
		next_word(rest, b, sizeof(b));
		if (link_kind_of(name) != _link_kind_ce || strlen(b) != 64)
			return TRUE;
		header->entry.kind = _link_kind_ce;
		header->entry.name_hash = link_match_hash(name, _link_kind_ce);
		header->entry.version = hex_value(b, 8);
		if (!header->entry.version)
			header->entry.version = 1;      /* (map_share_hash_version: 0 is 1) */
		header->pc_named = TRUE;
	}
	else if (!strcmp(word, "mcc"))
	{
		rest = next_word(rest, name, sizeof(name));
		rest = next_word(rest, a, sizeof(a));
		rest = next_word(rest, b, sizeof(b));
		next_word(rest, c, sizeof(c));
		if (link_kind_of(name) != _link_kind_mcc || strlen(a) != 64 || !c[0])
			return TRUE;
		header->entry.kind = _link_kind_mcc;
		header->entry.name_hash = link_match_hash(name, _link_kind_mcc);
		header->entry.content = hex_value(a, 8);
		header->entry.metadata = strlen(b) == 64 ? hex_value(b, 8) : 0;
		header->entry.version = strtoul(c, NULL, 16);
		header->pc_named = TRUE;
	}
	else if (!strcmp(word, "xbox"))
	{
		rest = next_word(rest, name, sizeof(name));
		header->xbox_named = TRUE;
	}
	return TRUE;
}

/* the folder maps' links, listed again after a map scan */
static void links_build(void)
{
	long generation = hmp_maps_generation();
	long map_count = hmp_maps_count();
	long map_index;
	unsigned long start = GetTickCount();

	if (links.generation == generation && generation)
		return;
	if (links.entries)
		LocalFree(links.entries);
	links.entries = NULL;
	links.count = 0;
	links.generation = generation;
	for (map_index = 0; map_index < map_count; map_index++)
	{
		char pattern[300], folder[300], path[300];
		char const *key = hmp_maps_key(map_index);
		WIN32_FIND_DATAA find;
		HANDLE handle;
		char *slash;

		if (!key || !hmp_map_folder_file(key, "*.h1ulink", pattern, sizeof(pattern)))
			continue;
		handle = FindFirstFileA(pattern, &find);
		if (handle == INVALID_HANDLE_VALUE)
			continue;
		strcpy(folder, pattern);
		slash = strrchr(folder, '\\');
		if (slash)
			*slash = 0;
		do
		{
			struct link_header header;

			if ((find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
				strlen(folder) + strlen(find.cFileName) + 2 > sizeof(path))
			{
				continue;
			}
			sprintf(path, "%s\\%s", folder, find.cFileName);
			memset(&header, 0, sizeof(header));
			header.first = TRUE;
			if (!link_lines(path, link_header_line, &header) || !header.valid || !header.pc_named ||
				!header.xbox_named)
			{
				continue;
			}
			if (!links.entries)
			{
				links.entries = (struct link_entry *)LocalAlloc(LMEM_ZEROINIT, LINK_MAXIMUM * sizeof(struct link_entry));
				if (!links.entries)
				{
					CloseHandle(handle);
					return;
				}
			}
			if (links.count >= LINK_MAXIMUM)
				break;
			header.entry.map_index = (short)map_index;
			header.entry.file_hash = link_name_hash(find.cFileName);
			links.entries[links.count++] = header.entry;
		}
		while (FindNextFileA(handle, &find));
		CloseHandle(handle);
	}
	hmp_log("links: %ld links to PC maps among %ld folder maps, in %lu ms", links.count, map_count,
		GetTickCount() - start);
}

/* the link for a PC map (version 0: any version; mcc: the record's 64 bytes of MCC identity, or NULL); NULL for none */
static struct link_entry const *links_find(char const *name, unsigned long version, byte const *mcc)
{
	short kind = link_kind_of(name);
	unsigned long name_hash;
	long index;

	if (!kind)
		return NULL;
	links_build();
	name_hash = link_match_hash(name, kind);
	for (index = 0; index < links.count; index++)
	{
		struct link_entry const *entry = &links.entries[index];

		if (entry->kind != kind || entry->name_hash != name_hash)
			continue;
		if (!version)
			return entry;
		if (entry->version != version)
			continue;
		if (kind == _link_kind_mcc && mcc)
		{
			/* (the map's and its resources' contents: a package's metadata changes no gameplay, so it isn't
			matched, nor is the package's folder name) */
			unsigned long content = (unsigned long)mcc[0] << 24 | (unsigned long)mcc[1] << 16 |
				(unsigned long)mcc[2] << 8 | mcc[3];

			if (entry->content != content)
				continue;
		}
		return entry;
	}
	return NULL;
}

/* the size of a file (0: none) */
static unsigned long file_size_of(char const *path)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned long size;

	if (file == INVALID_HANDLE_VALUE)
		return 0;
	size = GetFileSize(file, NULL);
	CloseHandle(file);
	return size == 0xFFFFFFFFUL ? 0 : size;
}

/* the joined game's link file's path (FALSE: gone) */
static boolean link_path(char *path, long size)
{
	char pattern[300], folder[300];
	WIN32_FIND_DATAA find;
	HANDLE handle;
	boolean found = FALSE;
	char *slash;

	if (!hmp_map_folder_file(links.key, "*.h1ulink", pattern, sizeof(pattern)))
		return FALSE;
	handle = FindFirstFileA(pattern, &find);
	if (handle == INVALID_HANDLE_VALUE)
		return FALSE;
	strcpy(folder, pattern);
	slash = strrchr(folder, '\\');
	if (slash)
		*slash = 0;
	do
	{
		if (!(find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && link_name_hash(find.cFileName) == links.file_hash &&
			(long)(strlen(folder) + strlen(find.cFileName) + 2) <= size)
		{
			sprintf(path, "%s\\%s", folder, find.cFileName);
			found = TRUE;
		}
	}
	while (!found && FindNextFileA(handle, &find));
	CloseHandle(handle);
	return found;
}

struct link_tags
{
	long pc_highest, xbox_highest;
	boolean fill;
	long checked, wrong;                     /* (fill) the Xbox tags whose path the map has at that ID, and not */
};

char const *cache_file_tag_name_if_any(long tag_index);

/* whether two tag paths are the same (case, and '/' for '\\', aside) */
static boolean same_path(char const *a, char const *b)
{
	for (; *a && *b; a++, b++)
	{
		int x = lower((unsigned char)*a), y = lower((unsigned char)*b);

		if (x == '/')
			x = '\\';
		if (y == '/')
			y = '\\';
		if (x != y)
			return FALSE;
	}
	return !*a && !*b;
}

/* a tag line: the highest tag index of each map (counting), or the tables' entries (fill) */
static boolean link_tag_line(void *context, char const *line)
{
	struct link_tags *tags = (struct link_tags *)context;
	char word[16], group[16], pc_text[16], xbox_text[16];
	char const *rest = next_word(line, word, sizeof(word));
	unsigned long pc_id, xbox_id;

	if (strcmp(word, "tag"))
		return TRUE;
	rest = next_word(rest, group, sizeof(group));
	rest = next_word(rest, pc_text, sizeof(pc_text));
	rest = next_word(rest, xbox_text, sizeof(xbox_text));
	if (!strcmp(pc_text, "-") || !strcmp(xbox_text, "-") || !pc_text[0] || !xbox_text[0])
		return TRUE;
	pc_id = strtoul(pc_text, NULL, 16);
	xbox_id = strtoul(xbox_text, NULL, 16);
	if (!tags->fill)
	{
		tags->pc_highest = MAX(tags->pc_highest, (long)(pc_id & 0xFFFF));
		tags->xbox_highest = MAX(tags->xbox_highest, (long)(xbox_id & 0xFFFF));
		return TRUE;
	}
	/* (the Xbox map's tag must be the map's own at that ID: the link is checked against the loaded map, not by its
	xbox line's size and hash, which Halo Map Porter writes before its last steps) */
	{
		char const *name = cache_file_tag_name_if_any((long)xbox_id);

		while (*rest == ' ' || *rest == '\t')
			rest++;
		if (name && same_path(name, rest))
			tags->checked++;
		else
		{
			if (tags->wrong < 3)
				hmp_log("links: the link's tag %08lx %s isn't the map's (%s)", xbox_id, rest,
					name ? name : "no such tag");
			tags->wrong++;
			return TRUE;
		}
	}
	if ((long)(pc_id & 0xFFFF) < links.to_xbox_count)
		links.to_xbox[pc_id & 0xFFFF] = (long)xbox_id;
	if ((long)(xbox_id & 0xFFFF) < links.to_ce_count)
		links.to_ce[xbox_id & 0xFFFF] = (long)pc_id;
	return TRUE;
}

/* leaves the game, saying why */
static void links_refuse(wchar_t const *why)
{
	/* (once: the settings that made it come again before it has gone) */
	if (links.refused_time && GetTickCount() - links.refused_time < 5000)
		return;
	links.refused_time = GetTickCount();
	display_error_text_when_main_menu_loaded(why);
	network_game_client_all_local_players_have_quit();
}

static long links_translate(long const *table, long count, long tag)
{
	long index;

	if (tag == NONE || !table)
		return NONE;
	index = tag & 0xFFFF;
	return index < count ? table[index] : NONE;
}

/* ---------- public code */

/* whether the games list shows a PC game on this map: a Custom Edition or MCC map this Xbox has a link for */
boolean hmp_links_listed(char const *map)
{
	long map_index, index;

	if (!map || map[0] != '+')
		return links_find(map, 0, NULL) != NULL;
	/* (one already named by its port: an advertisement's, network_client_message_handler.c) */
	map_index = hmp_maps_find(map);
	if (map_index == NONE)
		return FALSE;
	links_build();
	for (index = 0; index < links.count; index++)
		if (links.entries[index].map_index == map_index)
			return TRUE;
	return FALSE;
}

/* the name the games list shows for a PC game's map: its Xbox port's key for a linked one (FALSE: as it is) */
boolean hmp_links_shown_name(char const *map, char *name, long size)
{
	struct link_entry const *entry = links_find(map, 0, NULL);
	char const *key = entry ? hmp_maps_key(entry->map_index) : NULL;

	if (!key || (long)strlen(key) >= size)
		return FALSE;
	strcpy(name, key);
	return TRUE;
}

/* joining or in a PC game (its settings, as the Xbox's, with the record's 64 bytes of MCC identity): a Custom Edition
or MCC map's game named by its Xbox port, the link the game's; FALSE when this Xbox has no link for the map at that
version (it leaves, saying so). Any other map's: TRUE, no link */
boolean hmp_links_join(struct network_game *game, byte const *mcc)
{
	struct link_entry const *entry;
	char path[300];
	char const *key;

	if (!link_kind_of(game->map.name))
	{
		links.active = FALSE;
		return TRUE;
	}
	entry = links_find(game->map.name, (unsigned long)game->map.version, mcc);
	key = entry ? hmp_maps_key(entry->map_index) : NULL;
	if (!key || strlen(key) >= sizeof(links.key))
	{
		hmp_log("links: no link here for %s (version %08lx): not joined", game->map.name,
			(unsigned long)game->map.version);
		links.active = FALSE;
		links_refuse(links_find(game->map.name, 0, NULL) ?
			L"The host's copy of this map is another version than the one your Xbox port is linked to." :
			L"Your Xbox has no port of this game's map.");
		return FALSE;
	}
	/* (its file there; that it's the map the link is for is checked as it loads, tag by tag: hmp_links_memory) */
	if (!hmp_map_file_path(key, path, sizeof(path)) || !file_size_of(path))
	{
		hmp_log("links: %s's port %s has no map file: not joined", game->map.name, key);
		links.active = FALSE;
		links_refuse(L"Your Xbox has no port of this game's map.");
		return FALSE;
	}
	if (!links.active || strcmp(links.key, key) || links.file_hash != entry->file_hash)
	{
		hmp_log("links: %s (version %08lx) played on %s", game->map.name, (unsigned long)game->map.version, key);
		links.to_xbox = links.to_ce = NULL;
		links.scratch = NULL;
	}
	strcpy(links.key, key);
	csstrncpy(links.pc_name, game->map.name, sizeof(links.pc_name) - 1);
	links.file_hash = entry->file_hash;
	links.active = TRUE;
	csmemset(game->map.name, 0, sizeof(game->map.name));
	csstrncpy(game->map.name, key, sizeof(game->map.name) - 1);
	game->map.version = (long)hmp_share_map_version(key);
	return TRUE;
}

/* the PC map's name for this Xbox's port of it in the linked game joined (the host's name for the map: the precached
report, network_client_manager.c); FALSE: not the linked game's map */
boolean hmp_links_pc_name(char const *map, char *name, long size)
{
	if (!links.active || !map || csstrcasecmp(map, links.key) || (long)strlen(links.pc_name) >= size)
		return FALSE;
	strcpy(name, links.pc_name);
	return TRUE;
}

/* the client left (or joined another game): no link */
void hmp_links_clear(void)
{
	links.active = FALSE;
	links.to_xbox = links.to_ce = NULL;
	links.scratch = NULL;
}

/* as a map loads (cache_files.c): the joined linked game's tables read into the start of the AI's pools' room the
map leaves (*idle, whole 16 KB pages: before the rest goes to the texture cache), none otherwise */
void hmp_links_memory(void **idle, long *idle_size)
{
	struct network_game *game;
	struct link_tags tags;
	char path[300];
	long bytes, index;

	links.to_xbox = links.to_ce = NULL;
	links.scratch = NULL;
	if (!idle || !links.active || game_connection() != _game_connection_network_client)
		return;
	game = network_game_get_game();
	if (!game || csstrcasecmp(game->map.name, links.key))
		return;
	memset(&tags, 0, sizeof(tags));
	tags.pc_highest = tags.xbox_highest = -1;
	if (!link_path(path, sizeof(path)) || !link_lines(path, link_tag_line, &tags) || tags.pc_highest < 0)
	{
		hmp_log("links: %s's link can't be read", links.key);
		links_refuse(L"Your Xbox port's link to this map can't be read.");
		return;
	}
	bytes = (tags.pc_highest + 1 + tags.xbox_highest + 1) * (long)sizeof(long) + LINK_SCRATCH_SIZE;
	bytes = (bytes + 0x3FFF) & ~0x3FFFL;
	if (!*idle || *idle_size < bytes)
	{
		hmp_log("links: %s's tables need %ld KB, %ld KB free here", links.key, bytes / 1024, *idle_size / 1024);
		links_refuse(L"This map's link is too big for your Xbox's memory.");
		return;
	}
	links.to_xbox = (long *)*idle;
	links.to_xbox_count = tags.pc_highest + 1;
	links.to_ce = links.to_xbox + links.to_xbox_count;
	links.to_ce_count = tags.xbox_highest + 1;
	links.scratch = (byte *)(links.to_ce + links.to_ce_count);
	for (index = 0; index < links.to_xbox_count; index++)
		links.to_xbox[index] = NONE;
	for (index = 0; index < links.to_ce_count; index++)
		links.to_ce[index] = NONE;
	tags.fill = TRUE;
	link_lines(path, link_tag_line, &tags);
	if (tags.wrong || !tags.checked)
	{
		hmp_log("links: %s's link doesn't fit the map: %ld of its tags aren't the map's", links.key, tags.wrong);
		links.to_xbox = links.to_ce = NULL;
		links.scratch = NULL;
		links_refuse(L"Your Xbox port of this map isn't the one its link is for.");
		return;
	}
	*idle = (byte *)*idle + bytes;
	*idle_size -= bytes;
	hmp_log("links: %s's tables read (%ld tags, all the map's; %ld KB)", links.key, tags.checked, bytes / 1024);
}

/* whether this Xbox plays a linked game, its tables read */
boolean linked_map_active(void)
{
	return links.active && links.to_xbox && game_connection() == _game_connection_network_client;
}

/* a whole message's room for a translated copy of the host's (linked_map_active) */
void *linked_map_scratch(word size)
{
	return links.scratch && size <= LINK_SCRATCH_SIZE ? links.scratch : NULL;
}

/* a tag ID of the PC map as the Xbox map's, and back (NONE: none there) */
long linked_map_tag_to_xbox(long tag)
{
	return links_translate(links.to_xbox, links.to_xbox_count, tag);
}

long linked_map_tag_to_ce(long tag)
{
	return links_translate(links.to_ce, links.to_ce_count, tag);
}

#endif
