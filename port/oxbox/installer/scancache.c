/* scancache.c: the installer's facts about folders and files, kept between runs (scancache.h).

The file (scan_cache.bin beside the installer): "OXSC", a version, a count, then each entry: its path's length (2
bytes) and path (lower case), its key (4), its facts' length (1) and facts. A file that doesn't read right is
dropped whole: everything is looked at again, as before there was a cache. Kept in memory as an array with an
open-addressed table of the paths' hashes. */
#include "scancache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_VERSION 1
#define MOST_ENTRIES 8192
#define TABLE_SIZE 16384            /* (a power of two, twice the entries) */
#define MOST_PATH 240

struct entry
{
	char *path;
	unsigned long key;
	unsigned char size;
	unsigned char facts[SCAN_CACHE_MOST_FACTS];
};

static struct entry *entries;
static int entry_count;
static short *table;                /* (an entry's index + 1; 0: empty) */
static int dirty;
static unsigned long hits, misses;

unsigned long scan_cache_hash(unsigned long key, void const *data, unsigned long size)
{
	unsigned char const *p = (unsigned char const *)data;
	unsigned long h = key ? key : 2166136261ul;
	while (size--)
		h = (h ^ *p++) * 16777619ul;
	return h;
}

static void lower_path(char const *path, char *out)
{
	int n;
	for (n = 0; path[n] && n < MOST_PATH - 1; n++)
		out[n] = (char)(path[n] >= 'A' && path[n] <= 'Z' ? path[n] + 32 : path[n]);
	out[n] = 0;
}

static int ready(void)
{
	if (entries)
		return 1;
	entries = (struct entry *)calloc(MOST_ENTRIES, sizeof(*entries));
	table = (short *)calloc(TABLE_SIZE, sizeof(*table));
	if (!entries || !table)
	{
		free(entries);
		free(table);
		entries = NULL;
		table = NULL;
		return 0;
	}
	return 1;
}

/* the slot of path (lower case) in the table: its entry's, or the empty one it'd go in */
static int slot_of(char const *path)
{
	unsigned long i = scan_cache_hash(0, path, (unsigned long)strlen(path)) & (TABLE_SIZE - 1);
	while (table[i] && strcmp(entries[table[i] - 1].path, path))
		i = (i + 1) & (TABLE_SIZE - 1);
	return (int)i;
}

int scan_cache_get(char const *path, unsigned long key, void *facts, int size)
{
	char low[MOST_PATH];
	int slot;
	struct entry *e;
	if (!ready())
		return 0;
	lower_path(path, low);
	slot = slot_of(low);
	e = table[slot] ? &entries[table[slot] - 1] : NULL;
	if (!e || e->key != key || e->size != size)
	{
		misses++;
		return 0;
	}
	memcpy(facts, e->facts, size);
	hits++;
	return 1;
}

void scan_cache_put(char const *path, unsigned long key, void const *facts, int size)
{
	char low[MOST_PATH];
	int slot;
	struct entry *e;
	if (size > SCAN_CACHE_MOST_FACTS || !ready())
		return;
	lower_path(path, low);
	slot = slot_of(low);
	if (table[slot])
		e = &entries[table[slot] - 1];
	else
	{
		if (entry_count == MOST_ENTRIES || !(entries[entry_count].path = strdup(low)))
			return;
		e = &entries[entry_count++];
		table[slot] = (short)entry_count;
	}
	if (e->key == key && e->size == size && !memcmp(e->facts, facts, size))
		return;
	e->key = key;
	e->size = (unsigned char)size;
	memcpy(e->facts, facts, size);
	dirty = 1;
}

void scan_cache_counts(unsigned long *hit_count, unsigned long *miss_count)
{
	*hit_count = hits;
	*miss_count = misses;
}

static int read_bytes(FILE *file, void *out, size_t size)
{
	return fread(out, 1, size, file) == size;
}

void scan_cache_load(char const *path)
{
	FILE *file = fopen(path, "rb");
	unsigned char head[12];
	unsigned long count, k;
	if (!file)
		return;
	if (!ready() || !read_bytes(file, head, sizeof(head)) || memcmp(head, "OXSC", 4) ||
		(head[4] | head[5] << 8 | head[6] << 16 | (unsigned long)head[7] << 24) != CACHE_VERSION)
	{
		fclose(file);
		return;
	}
	count = head[8] | head[9] << 8 | head[10] << 16 | (unsigned long)head[11] << 24;
	for (k = 0; k < count && k < MOST_ENTRIES; k++)
	{
		unsigned char length[2], key[4], size;
		unsigned char facts[SCAN_CACHE_MOST_FACTS];
		char text[MOST_PATH];
		unsigned n;
		if (!read_bytes(file, length, 2) || (n = length[0] | length[1] << 8) >= MOST_PATH ||
			!read_bytes(file, text, n) || !read_bytes(file, key, 4) || !read_bytes(file, &size, 1) ||
			size > SCAN_CACHE_MOST_FACTS || !read_bytes(file, facts, size))
			break;
		text[n] = 0;
		scan_cache_put(text, key[0] | key[1] << 8 | key[2] << 16 | (unsigned long)key[3] << 24, facts, size);
	}
	fclose(file);
	dirty = 0;
}

void scan_cache_save(char const *path)
{
	FILE *file;
	unsigned char head[12] = {'O', 'X', 'S', 'C', CACHE_VERSION, 0, 0, 0};
	int k, ok = 1;
	if (!dirty || !entries)
		return;
	file = fopen(path, "wb");
	if (!file)
		return;
	head[8] = (unsigned char)entry_count, head[9] = (unsigned char)(entry_count >> 8);
	ok = fwrite(head, 1, sizeof(head), file) == sizeof(head);
	for (k = 0; k < entry_count && ok; k++)
	{
		struct entry const *e = &entries[k];
		size_t n = strlen(e->path);
		unsigned char length[2] = {(unsigned char)n, (unsigned char)(n >> 8)};
		unsigned char key[4] = {(unsigned char)e->key, (unsigned char)(e->key >> 8), (unsigned char)(e->key >> 16),
			(unsigned char)(e->key >> 24)};
		ok = fwrite(length, 1, 2, file) == 2 && fwrite(e->path, 1, n, file) == n && fwrite(key, 1, 4, file) == 4 &&
			fwrite(&e->size, 1, 1, file) == 1 && fwrite(e->facts, 1, e->size, file) == e->size;
	}
	if (fclose(file) || !ok)
		remove(path);
	else
		dirty = 0;
}
