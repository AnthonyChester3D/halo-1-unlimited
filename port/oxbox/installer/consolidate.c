/* consolidate.c: what moving a pack's maps onto shared files would save, by the rules of docs/shared_maps.md: a texture or
sound stock.shared has is taken from there, one two or more of the pack's maps use goes in the pack's file, and the rest
stays in its map. A match is the same bytes at the same offset modulo 512. It only reads. A map is gone through twice, a
piece at a time (the first for its tag data, usually at its end; the second to hash its blocks): a level can unpack to 280 MB. */
#include "consolidate.h"
#include "mapstream.h"
#include "sha256.h"
#include "text.h"

#include <stdlib.h>
#include <string.h>

#define MOST_TAGS (23u * 1024u * 1024u + SECTOR) /* (the game's tag cache) */
#define MOST_BLOCKS 16384
#define CHUNK (256u * 1024u)
#define TABLE_SIZE (1u << 17)           /* a pack's distinct blocks, and stock.shared's: 8946 of them */
#define REFERENCE_OFFSET 0x700u
#define REFERENCE_SIGNATURE 0x73687264u /* 'shrd' */
/* how a map's other data packs, and a pack's shared file, on the 14 old packs (shared_pass.py at level 6). An estimate */
#define MAP_PACKED_PERCENT 39
#define SHARED_PACKED_PERCENT 47

struct entry
{
	unsigned char digest[12];       /* the start of its SHA-256 */
	uint32_t size;
	uint16_t remainder;             /* its offset modulo 512 */
	uint16_t maps;                  /* the pack's maps using it */
	int16_t last;                   /* the last of them */
	uint8_t used, in_stock;
};

struct measuring
{
	struct entry *table;
	int entries;
	unsigned char *tags, *piece;
	struct block *blocks;
	struct sha256 *hashes;
	int *order;
};

static struct block const *sorting;

static int by_offset(void const *a, void const *b)
{
	uint32_t x = sorting[*(int const *)a].offset, y = sorting[*(int const *)b].offset;
	return x < y ? -1 : x > y;
}

/* its place in the table, added if it's new; NULL when the table is full */
static struct entry *find_entry(struct measuring *m, unsigned char const *digest, uint32_t size, uint32_t remainder)
{
	uint32_t at = u32(digest) ^ (remainder * 2654435761u);
	int tries;
	for (tries = 0; tries < (int)TABLE_SIZE; tries++, at++)
	{
		struct entry *e = &m->table[at % TABLE_SIZE];
		if (!e->used)
		{
			if (m->entries >= (int)(TABLE_SIZE / 4 * 3))
				return NULL;
			m->entries++;
			memset(e, 0, sizeof(*e));
			memcpy(e->digest, digest, sizeof(e->digest));
			e->size = size;
			e->remainder = (uint16_t)remainder;
			e->used = 1;
			e->last = -1;
			return e;
		}
		if (e->remainder == remainder && e->size == size && !memcmp(e->digest, digest, sizeof(e->digest)))
			return e;
	}
	return NULL;
}

/* stock.index's blocks, marked as in stock.shared */
static int load_index(struct measuring *m, char const *path)
{
	HANDLE file = open_read(path);
	unsigned char head[20], entry[40];
	uint32_t k, count;
	int failed = 1;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	if (read_all(file, head, sizeof(head)) || memcmp(head, "OXSI", 4) || u32(head + 4) != 1)
		goto done;
	count = u32(head + 8);
	for (k = 0; k < count; k++)
	{
		struct entry *e;
		if (read_all(file, entry, sizeof(entry)))
			goto done;
		e = find_entry(m, entry, u32(entry + 36), u32(entry + 32) % 512);
		if (!e)
			goto done;
		e->in_stock = 1;
	}
	failed = 0;
done:
	CloseHandle(file);
	return failed;
}

/* one map: its blocks hashed and counted. 0 when measured, 1 when it's to be left as it is (result says why) */
static int measure_map(struct measuring *m, char const *path, int index, struct consolidate_measure *result, uint64_t *length_out)
{
	unsigned char header[HEADER];
	struct reader r;
	struct map_view view;
	struct map_bsp bsps[MOST_BSPS];
	uint32_t bsp_count = 0, at, left, length;
	int count = 0, k, first = 0, failed = 1;

	if (reader_open(&r, path, header))
	{
		result->unreadable++;
		return 1;
	}
	if (u32(header + REFERENCE_OFFSET) == REFERENCE_SIGNATURE)
	{
		result->already++;
		reader_close(&r);
		return 1;
	}
	length = u32(header + 8);
	memset(&view, 0, sizeof(view));
	view.tags = m->tags;
	view.length = length;
	view.tag_offset = u32(header + 0x10);
	view.tag_size = u32(header + 0x14);
	if (length < HEADER || view.tag_offset < HEADER || view.tag_size > MOST_TAGS || view.tag_size > length ||
		view.tag_offset > length - view.tag_size)
		goto unreadable;
	/* first the tag data */
	for (left = view.tag_offset - HEADER; left;)
	{
		uint32_t part = left > CHUNK ? CHUNK : left;
		if (reader_pull(&r, m->piece, part))
			goto unreadable;
		left -= part;
	}
	if (reader_pull(&r, m->tags, view.tag_size) || find_blocks(&view, m->blocks, MOST_BLOCKS, &count, bsps, &bsp_count))
		goto unreadable;
	reader_close(&r);
	/* then the blocks, in the order they're in the file; they can overlap, so each has its own hash */
	for (k = 0; k < count; k++)
	{
		m->order[k] = k;
		sha256_start(&m->hashes[k]);
	}
	sorting = m->blocks;
	qsort(m->order, (size_t)count, sizeof(m->order[0]), by_offset);
	if (reader_open(&r, path, header))
		goto unreadable;
	for (at = HEADER;;)
	{
		uint32_t part;
		if (reader_read(&r, m->piece, CHUNK, &part))
			goto unreadable;
		if (!part)
			break;
		while (first < count && m->blocks[m->order[first]].offset + m->blocks[m->order[first]].size <= at)
			first++;
		for (k = first; k < count && m->blocks[m->order[k]].offset < at + part; k++)
		{
			struct block const *b = &m->blocks[m->order[k]];
			uint32_t start = b->offset > at ? b->offset : at, end = b->offset + b->size;
			if (end > at + part)
				end = at + part;
			if (start < end)
				sha256_add(&m->hashes[m->order[k]], m->piece + (start - at), end - start);
		}
		at += part;
	}
	/* (what it really unpacks to; a block its tags place past that isn't one the game could read) */
	length = at;
	for (k = 0; k < count; k++)
	{
		unsigned char digest[32];
		struct entry *e;
		sha256_end(&m->hashes[k], digest);
		if (m->blocks[k].offset + m->blocks[k].size > length)
			continue;
		e = find_entry(m, digest, m->blocks[k].size, m->blocks[k].offset % 512);
		if (!e)
		{
			failed = 2;
			goto done;
		}
		/* (once in each map, however many tags use it) */
		if (e->last != index)
		{
			e->last = (int16_t)index;
			e->maps++;
			if (e->in_stock)
				result->in_stock += e->size;
		}
	}
	*length_out = length;
	failed = 0;
	goto done;
unreadable:
	result->unreadable++;
done:
	reader_close(&r);
	return failed;
}

int consolidate_measure(char const *stock_index, char const *const *paths, int count, consolidate_say_t say,
	struct consolidate_measure *result, char *error)
{
	struct measuring m;
	unsigned long long lengths = 0, staying;
	uint32_t k;
	int i, status = 1;
	char line[200];

	memset(result, 0, sizeof(*result));
	memset(&m, 0, sizeof(m));
	m.table = calloc(TABLE_SIZE, sizeof(*m.table));
	m.tags = malloc(MOST_TAGS);
	m.piece = malloc(CHUNK);
	m.blocks = malloc(MOST_BLOCKS * sizeof(*m.blocks));
	m.hashes = malloc(MOST_BLOCKS * sizeof(*m.hashes));
	m.order = malloc(MOST_BLOCKS * sizeof(*m.order));
	if (!m.table || !m.tags || !m.piece || !m.blocks || !m.hashes || !m.order)
	{
		text_print(error, 256, "Not enough memory.");
		goto done;
	}
	if (load_index(&m, stock_index))
	{
		text_print(error, 256, "maps\\stock.index can't be read: take the installer's offer to make the stock maps small first.");
		goto done;
	}
	for (i = 0; i < count; i++)
	{
		uint64_t length = 0;
		uint32_t size = 0;
		HANDLE file = open_read(paths[i]);
		int r;
		if (file != INVALID_HANDLE_VALUE)
		{
			size = GetFileSize(file, NULL);
			CloseHandle(file);
		}
		if (say)
		{
			char const *name = strrchr(paths[i], '\\');
			text_print(line, sizeof(line), "%d of %d: %s", i + 1, count, name ? name + 1 : paths[i]);
			say(0, line);
		}
		r = measure_map(&m, paths[i], i, result, &length);
		if (r == 2)
		{
			text_print(error, 256, "Too many different textures and sounds to count in this pack.");
			goto done;
		}
		if (r)
			continue;
		result->maps++;
		result->before += size;
		lengths += length;
	}
	/* what stays in the maps: all they had less what's taken from stock.shared and what goes in the pack's file */
	staying = lengths - (result->in_stock < lengths ? result->in_stock : lengths);
	for (k = 0; k < TABLE_SIZE; k++)
	{
		struct entry const *e = &m.table[k];
		if (!e->used || e->in_stock || !e->maps)
			continue;
		if (e->maps >= 2)
		{
			unsigned long long referenced = (unsigned long long)e->size * e->maps;
			result->in_pack += e->size;
			staying -= referenced < staying ? referenced : staying;
		}
		else
			result->own += e->size;
	}
	result->after = staying * MAP_PACKED_PERCENT / 100 + result->in_pack * SHARED_PACKED_PERCENT / 100;
	status = 0;
done:
	free(m.table);
	free(m.tags);
	free(m.piece);
	free(m.blocks);
	free(m.hashes);
	free(m.order);
	return status;
}
