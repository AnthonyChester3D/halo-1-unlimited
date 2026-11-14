/* consolidate.c: a pack's maps moved onto shared files, by the rules of docs/shared_maps.md: a texture or sound stock.shared
has is taken from there, one two or more of the pack's maps use goes in the pack's file, and the rest stays in its map. A
match is the same bytes at the same offset modulo 512. The layout is tools/shared_pass.py's (the same maps in the same order
give the same files), and a converted map is checked as tools/verify_shared.py checks one before it's used.

Measuring only reads. A map is gone through twice, a piece at a time (the first for its tag data, usually at its end; the
second to hash its blocks): a level can unpack to 280 MB. Converting adds a pass for each map (its small map written, its
part of the pack's file with it), one to pack the pack's file, and one to read each small map back. The disk thread
(mapstream.c) is on throughout, as in the stock maps' swap: a file that's written is moved with disk_seek and closed with
file_close, and anything else done to it comes after disk_sync. Each step's time goes in the log (consolidate_log). */
#include "consolidate.h"
#include "mapstream.h"
#include "sha256.h"
#include "text.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define MOST_TAGS (23u * 1024u * 1024u + SECTOR) /* (the game's tag cache) */
#define MOST_BLOCKS 16384
#define CHUNK (256u * 1024u)
#define TABLE_SIZE (1u << 17)           /* a pack's distinct blocks, and stock.shared's: 8946 of them */
#define REFERENCE_OFFSET 0x700u
#define REFERENCE_SIZE 0x68u           /* (a pack's map's: the original's identity's CRCs at 0x60) */
#define REFERENCE_SIGNATURE 0x73687264u /* 'shrd' */
#define PACK_BIT 0x80000000u
#define STOCK_BIT 0xC0000000u
#define OFFSET_BITS 0x3FFFFFFFu
#define MP_SLOT (47u * 1024u * 1024u)   /* what a multiplayer map may unpack to (the game's cache file for one) */
#define CAMPAIGN_SLOT (278u * 1024u * 1024u) /* a campaign level's (header 0x60: scenario type 0) */
#ifndef UNPACKED_OVER
/* a pack's file bigger than this stays unpacked (shared_unpacked; tests: smaller): the game's seventh cache file, on a
second cache drive (cache_files_windows.c SHARED_CACHE_FILE_MAXIMUM_SIZE), holds one up to this unpacked */
#define UNPACKED_OVER 0x2B000000u
#endif
#define MAP_LEVEL 1                     /* (as the stock maps': far quicker on a console, and only 3% bigger than 6) */
#define SHARED_LEVEL 6
#define EDGE 65536u                     /* a map's identity: its size and the CRC32s of this much at its start and its end */
#define LIST_SIGNATURE "OXPL"           /* a pack's file's list, after its header */
#define LIST_HEAD 16u
#define LIST_MAP 20u
#define LIST_BLOCK 40u

void (*consolidate_log)(char const *text);

static char const *last_part(char const *path);

/* ---- the "time:" lines (as stockmaps.c's): each step, from when it began ---- */

static uint64_t step_began, whole_began;
static struct stream_times step_then, whole_then;

static void step_begin(void)
{
	step_began = stream_now();
	stream_times_get(&step_then);
}

static void step_end(char const *format, ...)
{
	char what[80], how[200], line[300];
	va_list list;
	if (!consolidate_log)
		return;
	va_start(list, format);
	text_vprint(what, sizeof(what), format, list);
	va_end(list);
	stream_times_text(how, sizeof(how), step_began, &step_then);
	text_print(line, sizeof(line), "time: %s: %s", what, how);
	consolidate_log(line);
	step_begin();
}

static void whole_begin(void)
{
	whole_began = stream_now();
	stream_times_get(&whole_then);
	step_begin();
}

static void whole_end(char const *what)
{
	char how[200], line[300];
	if (!consolidate_log)
		return;
	stream_times_text(how, sizeof(how), whole_began, &whole_then);
	text_print(line, sizeof(line), "time: %s, all of it: %s", what, how);
	consolidate_log(line);
}
/* how a map's other data packs, and a pack's shared file, on the 14 old packs (shared_pass.py at level 6). An estimate */
#define MAP_PACKED_PERCENT 39
#define SHARED_PACKED_PERCENT 47

struct entry
{
	unsigned char digest[12];       /* the start of its SHA-256 */
	uint32_t size;
	uint32_t at;                    /* where it is: in stock.shared, or (in_pack) in the pack's file */
	uint16_t remainder;             /* its offset modulo 512 */
	uint16_t maps;                  /* the pack's maps using it */
	int16_t last;                   /* the last of them */
	int16_t writer;                 /* the map whose pass copies it into the pack's file */
	uint8_t used, in_stock, in_pack, wanted; /* (wanted: a map being converted takes it from a shared file) */
};

/* one block of a map being converted: where its field is, where it was, and what the field becomes */
struct cblock
{
	uint32_t field, offset, size, want;
	struct entry *e;                /* NULL: past the map's end, nothing the game could read: left as it is */
};

struct cbsp
{
	uint32_t field, offset, size, want;
	unsigned char digest[32];
};

/* a map being converted */
struct cmap
{
	int state;                      /* CONVERTING, or why it's left */
	uint32_t length;                /* what it really unpacks to */
	uint32_t raw_size;              /* its file */
	uint32_t tag_offset, tag_size, out_length;
	uint32_t head, tail;            /* its identity's CRCs (with raw_size) */
	uint32_t body_crc, small_crc;   /* the small map's unpacked: after its header, and all of it */
	int count;
	struct cblock *blocks;
	uint32_t bsp_count;
	struct cbsp bsps[MOST_BSPS];
	unsigned char tag_digest[32];   /* its tag data with the fields this changes as zeros */
	unsigned char header[HEADER];   /* the small map's */
};
enum { LEFT, CONVERTING, CONVERTED };

struct measuring
{
	struct entry *table;
	int entries;
	unsigned char *tags, *piece;
	struct block *blocks;
	struct sha256 *hashes;
	int *order;
	uint32_t stock_checksum, stock_length;
};

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
			e->writer = -1;
			return e;
		}
		if (e->remainder == remainder && e->size == size && !memcmp(e->digest, digest, sizeof(e->digest)))
			return e;
	}
	return NULL;
}

/* stock.index's blocks, marked as in stock.shared, with where they are in it */
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
	m->stock_checksum = u32(head + 12);
	m->stock_length = u32(head + 16);
	for (k = 0; k < count; k++)
	{
		struct entry *e;
		if (read_all(file, entry, sizeof(entry)))
			goto done;
		e = find_entry(m, entry, u32(entry + 36), u32(entry + 32) % 512);
		if (!e)
			goto done;
		e->in_stock = 1;
		e->at = u32(entry + 32);
	}
	failed = 0;
done:
	CloseHandle(file);
	return failed;
}

/* the rest of a cache file's stream read from where it is (at), each span hashed (sorted by offset; they can overlap), and
the bytes of [capture_at, capture_at + capture_size) copied to capture; what it unpacks to in *length. 0 when read */
struct span
{
	uint32_t offset, size;
	struct sha256 *hash;
};

static int hash_spans(struct reader *r, unsigned char *piece, uint32_t at, struct span const *spans, int count,
	uint32_t capture_at, uint32_t capture_size, unsigned char *capture, uint32_t *length)
{
	int first = 0, k;
	for (;;)
	{
		uint32_t part;
		if (reader_read(r, piece, CHUNK, &part))
			return 1;
		if (!part)
			break;
		while (first < count && spans[first].offset + spans[first].size <= at)
			first++;
		for (k = first; k < count && spans[k].offset < at + part; k++)
		{
			uint32_t start = spans[k].offset > at ? spans[k].offset : at, end = spans[k].offset + spans[k].size;
			if (end > at + part)
				end = at + part;
			if (start < end)
				sha256_add(spans[k].hash, piece + (start - at), end - start);
		}
		if (capture && capture_at < at + part && at < capture_at + capture_size)
		{
			uint32_t start = capture_at > at ? capture_at : at, end = capture_at + capture_size;
			if (end > at + part)
				end = at + part;
			memcpy(capture + (start - capture_at), piece + (start - at), end - start);
		}
		at += part;
	}
	*length = at;
	return 0;
}

static int span_order(void const *a, void const *b)
{
	struct span const *x = a, *y = b;
	return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* one map: its blocks hashed and counted (and, with keep, what converting it needs). 0 when measured, 1 when it's to be left
as it is (result says why), 2 when the table is full */
static int measure_map(struct measuring *m, char const *path, int index, struct consolidate_measure *result,
	uint64_t *length_out, struct cmap *keep)
{
	unsigned char header[HEADER];
	struct reader r;
	struct map_view view;
	struct map_bsp bsps[MOST_BSPS];
	struct span *spans = (struct span *)m->order; /* (room for MOST_BLOCKS + MOST_BSPS of them: see consolidate_*) */
	uint32_t bsp_count = 0, left, length;
	int count = 0, k, failed = 1;

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
	/* (the BSPs are hashed with the blocks, after them) */
	for (k = 0; k < (int)bsp_count; k++)
	{
		struct block *b = &m->blocks[count + k];
		/* (its offset and size lie in the tag data, or the map isn't one the game could load) */
		if (bsps[k].field < view.tag_offset || bsps[k].field - view.tag_offset > view.tag_size - 8)
			goto unreadable;
		b->field = bsps[k].field;
		b->offset = bsps[k].offset;
		b->size = u32(m->tags + bsps[k].field + 4 - view.tag_offset);
	}
	if (keep)
	{
		/* the tag data as checking the small map will see it: the fields this changes as zeros */
		for (k = 0; k < count + (int)bsp_count; k++)
			memset(m->tags + m->blocks[k].field - view.tag_offset, 0, 4);
		sha256(m->tags, view.tag_size, keep->tag_digest);
	}
	/* then the blocks, in the order they're in the file; they can overlap, so each has its own hash */
	for (k = 0; k < count + (int)bsp_count; k++)
	{
		spans[k].offset = m->blocks[k].offset;
		spans[k].size = m->blocks[k].size;
		spans[k].hash = &m->hashes[k];
		sha256_start(&m->hashes[k]);
	}
	qsort(spans, (size_t)(count + (int)bsp_count), sizeof(spans[0]), span_order);
	if (reader_open(&r, path, header) || hash_spans(&r, m->piece, HEADER, spans, count + (int)bsp_count, 0, 0, NULL, &length))
		goto unreadable;
	/* (length: what it really unpacks to; a block its tags place past that isn't one the game could read) */
	for (k = 0; k < (int)bsp_count; k++)
	{
		struct block const *b = &m->blocks[count + k];
		if (b->offset + b->size > length || b->offset + b->size < b->offset)
			goto unreadable;
		if (keep)
		{
			keep->bsps[k].field = b->field;
			keep->bsps[k].offset = b->offset;
			keep->bsps[k].size = b->size;
			sha256_end(&m->hashes[count + k], keep->bsps[k].digest);
		}
	}
	if (keep)
	{
		keep->blocks = malloc((size_t)(count ? count : 1) * sizeof(*keep->blocks));
		if (!keep->blocks)
			goto unreadable;
		keep->count = count;
		keep->bsp_count = bsp_count;
		keep->length = length;
		keep->tag_offset = view.tag_offset;
		keep->tag_size = view.tag_size;
		memcpy(keep->header, header, HEADER);
	}
	for (k = 0; k < count; k++)
	{
		unsigned char digest[32];
		struct entry *e;
		sha256_end(&m->hashes[k], digest);
		if (keep)
		{
			keep->blocks[k].field = m->blocks[k].field;
			keep->blocks[k].offset = m->blocks[k].offset;
			keep->blocks[k].size = m->blocks[k].size;
			keep->blocks[k].e = NULL;
		}
		if (m->blocks[k].offset + m->blocks[k].size > length)
			continue;
		e = find_entry(m, digest, m->blocks[k].size, m->blocks[k].offset % 512);
		if (!e)
		{
			failed = 2;
			goto done;
		}
		if (keep)
			keep->blocks[k].e = e;
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
	if (failed && keep)
	{
		free(keep->blocks);
		keep->blocks = NULL;
	}
	return failed;
}

static int measuring_start(struct measuring *m, char const *stock_index, char *error)
{
	memset(m, 0, sizeof(*m));
	m->table = calloc(TABLE_SIZE, sizeof(*m->table));
	m->tags = malloc(MOST_TAGS);
	m->piece = malloc(CHUNK);
	m->blocks = malloc((MOST_BLOCKS + MOST_BSPS) * sizeof(*m->blocks));
	m->hashes = malloc((MOST_BLOCKS + MOST_BSPS) * sizeof(*m->hashes));
	m->order = malloc((MOST_BLOCKS + MOST_BSPS) * sizeof(struct span));
	if (!m->table || !m->tags || !m->piece || !m->blocks || !m->hashes || !m->order)
	{
		text_print(error, 256, "Not enough memory.");
		return 1;
	}
	if (load_index(m, stock_index))
	{
		text_print(error, 256, "maps\\stock.index can't be read: take the installer's offer to make the stock maps small first.");
		return 1;
	}
	return 0;
}

static void measuring_end(struct measuring *m)
{
	free(m->table);
	free(m->tags);
	free(m->piece);
	free(m->blocks);
	free(m->hashes);
	free(m->order);
}

/* a map's identity: its file's size and the CRC32s of its first and last 64 KiB (all of it when it's smaller), read with
buffer (EDGE bytes). 0 when read */
static int identity(char const *path, unsigned char *buffer, uint32_t *size, uint32_t *head, uint32_t *tail)
{
	HANDLE file = open_read(path);
	uint32_t n, at;
	int failed = 1;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	*size = GetFileSize(file, NULL);
	n = *size < EDGE ? *size : EDGE;
	at = *size - n;
	if (*size != 0xFFFFFFFFu && !read_all(file, buffer, n))
	{
		*head = crc_add(crc_add(0, NULL, 0), buffer, n);
		if (SetFilePointer(file, (LONG)at, NULL, FILE_BEGIN) == at && !read_all(file, buffer, n))
		{
			*tail = crc_add(crc_add(0, NULL, 0), buffer, n);
			failed = 0;
		}
	}
	CloseHandle(file);
	return failed;
}

int consolidate_identity(char const *path, uint32_t *size, uint32_t *head, uint32_t *tail)
{
	unsigned char *buffer = malloc(EDGE);
	int failed = !buffer || identity(path, buffer, size, head, tail);
	free(buffer);
	return failed;
}

/* every map measured (keeps: NULL, or each one's to convert); 0 when done, else error[256] */
static int measure_all(struct measuring *m, char const *const *paths, int count, consolidate_say_t say,
	struct consolidate_measure *result, struct cmap *keeps, char *error)
{
	unsigned long long lengths = 0, staying;
	uint32_t k;
	int i;
	char line[200];

	for (i = 0; i < count; i++)
	{
		uint64_t length = 0;
		uint32_t size = 0, head = 0, tail = 0;
		HANDLE file = open_read(paths[i]);
		int r;
		if (file != INVALID_HANDLE_VALUE)
		{
			size = GetFileSize(file, NULL);
			CloseHandle(file);
		}
		/* (a map to convert: its identity first, which orders the pack's file) */
		if (keeps && identity(paths[i], m->piece, &size, &head, &tail))
		{
			result->unreadable++;
			continue;
		}
		if (say)
		{
			char const *name = strrchr(paths[i], '\\');
			text_print(line, sizeof(line), "%s %d of %d: %s", keeps ? "Reading" : "Measuring", i + 1, count,
				name ? name + 1 : paths[i]);
			say(0, line);
		}
		r = measure_map(m, paths[i], i, result, &length, keeps ? &keeps[i] : NULL);
		step_end("%s measured", last_part(paths[i]));
		if (r == 2)
		{
			text_print(error, 256, "Too many different textures and sounds to count in this pack.");
			return 1;
		}
		if (r)
			continue;
		if (keeps)
		{
			keeps[i].state = CONVERTING;
			keeps[i].raw_size = size;
			keeps[i].head = head;
			keeps[i].tail = tail;
		}
		result->maps++;
		result->before += size;
		lengths += length;
	}
	/* what stays in the maps: all they had less what's taken from stock.shared and what goes in the pack's file */
	staying = lengths - (result->in_stock < lengths ? result->in_stock : lengths);
	for (k = 0; k < TABLE_SIZE; k++)
	{
		struct entry const *e = &m->table[k];
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
	return 0;
}

int consolidate_measure(char const *stock_index, char const *const *paths, int count, consolidate_say_t say,
	struct consolidate_measure *result, char *error)
{
	struct measuring m;
	int status = 1;
	memset(result, 0, sizeof(*result));
	disk_on();
	whole_begin();
	if (!measuring_start(&m, stock_index, error) && !measure_all(&m, paths, count, say, result, NULL, error))
		status = 0;
	measuring_end(&m);
	whole_end("measuring");
	disk_off();
	return status;
}

/* ---- converting ---- */

static unsigned char const zeros[SECTOR];

static HANDLE create(char const *path, int and_read)
{
	DeleteFileA(path);
	return CreateFileA(path, and_read ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
}

/* (written to the disk before it's closed) */
static int finish(HANDLE file)
{
	int ok = !timed_flush(file);
	return file_close(file) && ok;
}

static int write_at(HANDLE file, uint32_t at, void const *from, uint32_t size)
{
	return disk_seek(file, at) || write_all(file, from, size);
}

struct range
{
	uint32_t from, to;
};

static int range_order(void const *a, void const *b)
{
	struct range const *x = a, *y = b;
	return x->from < y->from ? -1 : x->from > y->from;
}

struct field
{
	uint32_t at, value;
};

static int field_order(void const *a, void const *b)
{
	struct field const *x = a, *y = b;
	return x->at < y->at ? -1 : x->at > y->at;
}

/* what laying out one map needs, as shared_pass.py's rebuild: the moved blocks and those that stay, sorted; the sectors wholly
of moved data (covered), which go, unless a block that stays or a BSP needs one; and how many go before each */
struct layout
{
	struct range *moved, *stay;
	int moved_count, stay_count;
	unsigned char *covered;
	uint32_t *before;
	uint32_t sectors;
	struct field *fields;
	int field_count;
};

static void layout_free(struct layout *l)
{
	free(l->moved);
	free(l->stay);
	free(l->covered);
	free(l->before);
	free(l->fields);
	memset(l, 0, sizeof(*l));
}

static int moved_down(struct layout const *l, uint32_t offset, uint32_t *to)
{
	uint32_t sector = offset / SECTOR;
	if (sector > l->sectors || l->covered[sector])
		return 1;
	*to = offset - l->before[sector] * SECTOR;
	return 0;
}

static int is_moved(struct entry const *e)
{
	return e && (e->in_stock || e->maps >= 2);
}

/* a map's layout, its small map's length and its fields' new values (each block's want). 0 when laid out */
static int lay_out(struct cmap *c, struct layout *l)
{
	uint32_t k, s, from = 0, to = 0, gone = 0;
	int i;
	memset(l, 0, sizeof(*l));
	l->sectors = c->length / SECTOR + 1;
	l->moved = malloc((size_t)(c->count + 1) * sizeof(*l->moved));
	l->stay = malloc((size_t)(c->count + 1) * sizeof(*l->stay));
	l->covered = calloc(l->sectors + 1, 1);
	l->before = malloc((l->sectors + 1) * sizeof(*l->before));
	l->fields = malloc((size_t)(c->count + MOST_BSPS) * sizeof(*l->fields));
	if (!l->moved || !l->stay || !l->covered || !l->before || !l->fields)
		return 1;
	for (i = 0; i < c->count; i++)
	{
		struct cblock const *b = &c->blocks[i];
		struct range *r;
		if (!b->e)
			continue;
		r = is_moved(b->e) ? &l->moved[l->moved_count++] : &l->stay[l->stay_count++];
		r->from = b->offset;
		r->to = b->offset + b->size;
	}
	qsort(l->moved, (size_t)l->moved_count, sizeof(l->moved[0]), range_order);
	qsort(l->stay, (size_t)l->stay_count, sizeof(l->stay[0]), range_order);
	/* (moved data that touches or overlaps is one run) */
	for (i = 0; i <= l->moved_count; i++)
	{
		if (i < l->moved_count && i && l->moved[i].from <= to)
		{
			if (l->moved[i].to > to)
				to = l->moved[i].to;
			continue;
		}
		if (i)
		{
			for (s = (from + SECTOR - 1) / SECTOR; s < to / SECTOR; s++)
				l->covered[s] = 1;
		}
		if (i < l->moved_count)
		{
			from = l->moved[i].from;
			to = l->moved[i].to;
		}
	}
	l->covered[0] = 0;
	/* (a sector something else needs stays: a block that stays or a BSP can share one with moved data, or lie in it) */
	for (i = 0; i < l->stay_count + (int)c->bsp_count; i++)
	{
		uint32_t o = i < l->stay_count ? l->stay[i].from : c->bsps[i - l->stay_count].offset;
		uint32_t n = i < l->stay_count ? l->stay[i].to - l->stay[i].from : c->bsps[i - l->stay_count].size;
		for (s = o / SECTOR; s <= (o + (n ? n : 1) - 1) / SECTOR && s < l->sectors; s++)
			l->covered[s] = 0;
	}
	for (k = 0; k <= l->sectors; k++)
	{
		l->before[k] = gone;
		if (k < l->sectors)
			gone += l->covered[k];
	}
	/* the small map: its header's sector, then every sector not covered (the last one as long as it is) */
	c->out_length = HEADER;
	for (k = 1; k * SECTOR < c->length; k++)
	{
		if (!l->covered[k])
			c->out_length += c->length - k * SECTOR < SECTOR ? c->length - k * SECTOR : SECTOR;
	}
	/* (to a whole sector, zeros after: a map's stream can end short of one, and the game copies a map to its cache in whole
	sectors) */
	c->out_length += (SECTOR - c->out_length % SECTOR) % SECTOR;
	/* the fields */
	for (i = 0; i < c->count; i++)
	{
		struct cblock *b = &c->blocks[i];
		if (!b->e)
			continue;
		if (b->e->in_stock)
			b->want = STOCK_BIT | b->e->at;
		else if (b->e->maps >= 2)
			b->want = PACK_BIT | b->e->at;
		else if (moved_down(l, b->offset, &b->want))
			return 1;
		l->fields[l->field_count].at = b->field;
		l->fields[l->field_count++].value = b->want;
	}
	for (k = 0; k < c->bsp_count; k++)
	{
		if (moved_down(l, c->bsps[k].offset, &c->bsps[k].want))
			return 1;
		l->fields[l->field_count].at = c->bsps[k].field;
		l->fields[l->field_count++].value = c->bsps[k].want;
	}
	qsort(l->fields, (size_t)l->field_count, sizeof(l->fields[0]), field_order);
	/* the header: the length and tag data's place, and the reference (the pack's file is named once it's made) */
	put32(c->header + 8, c->out_length);
	if (moved_down(l, c->tag_offset, &to))
		return 1;
	put32(c->header + 0x10, to);
	return 0;
}

static void set_reference(struct cmap *c, struct measuring const *m, char const *pack_name, uint32_t pack_checksum,
	uint32_t pack_length)
{
	unsigned char *r = c->header + REFERENCE_OFFSET;
	uint32_t raw_checksum = u32(c->header + 0x64);
	memset(r, 0, REFERENCE_SIZE);
	put32(r, REFERENCE_SIGNATURE);
	put32(r + 4, 2);
	if (pack_name)
	{
		memcpy(r + 8, pack_name, strlen(pack_name));
		put32(r + 0x28, pack_checksum);
		put32(r + 0x2C, pack_length);
	}
	put32(r + 0x30, c->raw_size);
	put32(r + 0x34, raw_checksum);
	memcpy(r + 0x38, "stock", 5);
	put32(r + 0x58, m->stock_checksum);
	put32(r + 0x5C, m->stock_length);
	put32(r + 0x60, c->head);
	put32(r + 0x64, c->tail);
}

/* the small map's CRC32 unpacked, now its header is made */
static void small_crc(struct cmap *c)
{
	c->small_crc = (uint32_t)crc32_combine(crc_add(crc_add(0, NULL, 0), c->header, HEADER), c->body_crc,
		(z_off_t)(c->out_length - HEADER));
}

/* the overlap of [from, to) with the piece at [at, at + size): 1 and its place in the piece and how much */
static int overlap(uint32_t from, uint32_t to, uint32_t at, uint32_t size, uint32_t *in_piece, uint32_t *in_block, uint32_t *n)
{
	uint32_t start = from > at ? from : at, end = to < at + size ? to : at + size;
	if (start >= end)
		return 0;
	*in_piece = start - at;
	*in_block = start - from;
	*n = end - start;
	return 1;
}

struct converting
{
	struct measuring m;
	struct cmap *maps;
	int count;
	unsigned char *before;          /* a piece as it was read (CHUNK) */
	struct cblock **writes;         /* the blocks a map's pass copies into the pack's file, sorted by offset */
};

static int write_order(void const *a, void const *b)
{
	uint32_t x = (*(struct cblock *const *)a)->offset, y = (*(struct cblock *const *)b)->offset;
	return x < y ? -1 : x > y;
}

/* map index's small map at out_path, from the map at path as it unpacks; its part of the pack's file into pack (unpacked,
where the layout puts it). 0 when written */
static int convert_map(struct converting *v, int index, char const *path, char const *out_path, HANDLE pack)
{
	struct cmap *c = &v->maps[index];
	struct layout l;
	unsigned char header[HEADER];
	unsigned char *chunk = v->m.piece;
	struct reader r;
	struct writer w;
	HANDLE file = INVALID_HANDLE_VALUE;
	uint32_t at, out = HEADER, tail, crc = crc_add(0, NULL, 0);
	int i, writes = 0, first_moved = 0, first_stay = 0, first_field = 0, first_write = 0, failed = 1, writing = 0;

	if (lay_out(c, &l))
		goto stop;
	for (i = 0; i < c->count; i++)
	{
		if (c->blocks[i].e && c->blocks[i].e->in_pack && c->blocks[i].e->writer == index)
			v->writes[writes++] = &c->blocks[i];
	}
	qsort(v->writes, (size_t)writes, sizeof(v->writes[0]), write_order);
	if (reader_open(&r, path, header))
		goto stop;
	file = create(out_path, 0);
	if (file == INVALID_HANDLE_VALUE || write_all(file, c->header, HEADER) || writer_open(&w, MAP_LEVEL, file, 0))
	{
		reader_close(&r);
		goto stop;
	}
	writing = 1;
	for (at = HEADER; at < c->length; at += CHUNK)
	{
		uint32_t size = c->length - at > CHUNK ? CHUNK : c->length - at, s, in_piece, in_block, n;
		int touched = 0;
		if (reader_pull(&r, chunk, size))
			goto stop_reading;
		/* its part of the pack's file, as it was */
		while (first_write < writes && v->writes[first_write]->offset + v->writes[first_write]->size <= at)
			first_write++;
		for (i = first_write; i < writes && v->writes[i]->offset < at + size; i++)
		{
			struct cblock const *b = v->writes[i];
			if (overlap(b->offset, b->offset + b->size, at, size, &in_piece, &in_block, &n) &&
				write_at(pack, b->e->at + in_block, chunk + in_piece, n))
				goto stop_reading;
		}
		/* (what moved goes to zeros, which compress away; a block that stays keeps its bytes, shared or not) */
		while (first_moved < l.moved_count && l.moved[first_moved].to <= at)
			first_moved++;
		while (first_stay < l.stay_count && l.stay[first_stay].to <= at)
			first_stay++;
		for (i = first_moved; i < l.moved_count && l.moved[i].from < at + size; i++)
		{
			if (overlap(l.moved[i].from, l.moved[i].to, at, size, &in_piece, &in_block, &n))
			{
				if (!touched)
					memcpy(v->before, chunk, size);
				touched = 1;
				memset(chunk + in_piece, 0, n);
			}
		}
		if (touched)
		{
			for (i = first_stay; i < l.stay_count && l.stay[i].from < at + size; i++)
			{
				if (overlap(l.stay[i].from, l.stay[i].to, at, size, &in_piece, &in_block, &n))
					memcpy(chunk + in_piece, v->before + in_piece, n);
			}
		}
		/* the fields */
		while (first_field < l.field_count && l.fields[first_field].at + 4 <= at)
			first_field++;
		for (i = first_field; i < l.field_count && l.fields[i].at < at + size; i++)
		{
			uint32_t b;
			for (b = 0; b < 4; b++)
			{
				if (l.fields[i].at + b >= at && l.fields[i].at + b < at + size)
					chunk[l.fields[i].at + b - at] = (unsigned char)(l.fields[i].value >> (b * 8));
			}
		}
		for (s = 0; s * SECTOR < size; s++)
		{
			uint32_t part = size - s * SECTOR > SECTOR ? SECTOR : size - s * SECTOR;
			if (l.covered[(at + s * SECTOR) / SECTOR])
				continue;
			if (writer_put(&w, chunk + s * SECTOR, part))
				goto stop_reading;
			crc = crc_add(crc, chunk + s * SECTOR, part);
			out += part;
		}
	}
	reader_close(&r);
	if (out < c->out_length && out + SECTOR > c->out_length)
	{
		if (writer_put(&w, zeros, c->out_length - out))
			goto stop;
		crc = crc_add(crc, zeros, c->out_length - out);
		out = c->out_length;
	}
	writing = 0;
	if (out != c->out_length || writer_finish(&w))
		goto stop;
	c->body_crc = crc;
	/* (padded to a whole sector: the game copies maps unbuffered) */
	tail = (SECTOR - (HEADER + w.written) % SECTOR) % SECTOR;
	if (write_all(file, zeros, tail))
		goto stop;
	failed = !finish(file);
	file = INVALID_HANDLE_VALUE;
	goto stop;
stop_reading:
	reader_close(&r);
stop:
	if (writing)
		deflateEnd(&w.z);
	if (file != INVALID_HANDLE_VALUE)
		file_close(file);
	layout_free(&l);
	return failed;
}

/* a block of the pack's file, in the order they're in it, with its whole SHA-256 for the file's list */
struct pblock
{
	struct entry *e;
	unsigned char digest[32];
};

/* the pack's file's data read back from its unpacked layout (temporary, from its list's end to length): its checksum (a
CRC32 of all of it) and each block's SHA-256, checked against the hash it was laid out by. 0 when read */
static int pack_data(struct converting *v, HANDLE temporary, uint32_t from, uint32_t length, struct pblock *pblocks,
	int count, uint32_t *checksum)
{
	struct sha256 h;
	uint32_t at, crc = crc_add(0, NULL, 0);
	int k = 0;
	if (disk_seek(temporary, from))
		return 1;
	for (at = from; at < length; at += CHUNK)
	{
		uint32_t size = length - at > CHUNK ? CHUNK : length - at;
		if (read_all(temporary, v->m.piece, size))
			return 1;
		crc = crc_add(crc, v->m.piece, size);
		/* (its blocks follow each other, none overlapping) */
		while (k < count && pblocks[k].e->at < at + size)
		{
			struct entry const *e = pblocks[k].e;
			uint32_t start = e->at > at ? e->at : at, end = e->at + e->size < at + size ? e->at + e->size : at + size;
			if (e->at >= at)
				sha256_start(&h);
			sha256_add(&h, v->m.piece + (start - at), end - start);
			if (e->at + e->size > at + size)
				break;
			sha256_end(&h, pblocks[k].digest);
			if (memcmp(pblocks[k].digest, e->digest, sizeof(e->digest)))
				return 1;
			k++;
		}
	}
	*checksum = crc;
	return k != count;
}

/* the pack's file's list (docs/shared_maps.md), list_room bytes, written into its unpacked layout after its header: the maps
it was made from (order, members of them) with their small maps, then its blocks. 0 when written */
static int write_list(struct converting *v, HANDLE temporary, uint32_t list_room, int const *order, int members,
	struct pblock const *pblocks, int count)
{
	unsigned char *list = calloc(list_room, 1), *p;
	int k, failed;
	if (!list)
		return 1;
	memcpy(list, LIST_SIGNATURE, 4);
	put32(list + 4, 1);
	put32(list + 8, (uint32_t)members);
	put32(list + 12, (uint32_t)count);
	p = list + LIST_HEAD;
	for (k = 0; k < members; k++, p += LIST_MAP)
	{
		struct cmap const *c = &v->maps[order[k]];
		put32(p, c->raw_size);
		put32(p + 4, c->head);
		put32(p + 8, c->tail);
		put32(p + 12, c->out_length);
		put32(p + 16, c->small_crc);
	}
	for (k = 0; k < count; k++, p += LIST_BLOCK)
	{
		memcpy(p, pblocks[k].digest, 32);
		put32(p + 32, pblocks[k].e->at);
		put32(p + 36, pblocks[k].e->size);
	}
	failed = write_at(temporary, HEADER, list, list_room);
	free(list);
	return failed;
}

/* the pack's file from its unpacked layout (temporary, length bytes, its list in the list_room after its header): packed to
path with its header, as shared_pass.py's: its name, "shared" as its build, its checksum (a CRC32 of all after the list,
checked again as it's packed) and its list's length at 0x18. 0 when written */
/* the pack's file's header (the same packed or not) */
static void shared_header(unsigned char *header, uint32_t length, uint32_t list_room, char const *name, uint32_t checksum)
{
	memset(header, 0, HEADER);
	memcpy(header, "daeh", 4);
	put32(header + 4, 5);
	put32(header + 8, length);
	put32(header + 0x18, list_room);
	memcpy(header + 0x20, name, strlen(name));
	memcpy(header + 0x40, "shared", 6);
	put32(header + 0x64, checksum);
	memcpy(header + 0x7FC, "toof", 4);
}

/* a pack's file bigger than the game's seventh cache file (UNPACKED_OVER) stays unpacked, as stock.shared is: the game
reads it where it is, so it takes no cache file (a packed one that big would take a campaign one, and a level change
would find no room for the next level: docs/shared_maps.md). In practice none is: packs' are 1-150 MB. The temporary file is it: its blocks checked by the CRC, its header
written, and it's closed (*temporary). 0 when done */
static int shared_unpacked(struct converting *v, HANDLE *temporary, uint32_t length, uint32_t list_room,
	char const *name, uint32_t checksum)
{
	unsigned char header[HEADER];
	uint32_t at, crc = crc_add(0, NULL, 0), data = HEADER + list_room;
	if (disk_seek(*temporary, data))
		return 1;
	for (at = data; at < length; at += CHUNK)
	{
		uint32_t size = length - at > CHUNK ? CHUNK : length - at;
		if (read_all(*temporary, v->m.piece, size))
			return 1;
		crc = crc_add(crc, v->m.piece, size);
	}
	if (crc != checksum)
		return 1;
	shared_header(header, length, list_room, name, checksum);
	if (write_at(*temporary, 0, header, HEADER))
		return 1;
	at = !finish(*temporary);
	*temporary = INVALID_HANDLE_VALUE;
	return (int)at;
}

static int pack_shared(struct converting *v, HANDLE temporary, uint32_t length, uint32_t list_room, char const *path,
	char const *name, uint32_t checksum)
{
	unsigned char header[HEADER];
	struct writer w;
	HANDLE file = create(path, 0);
	uint32_t at, crc = crc_add(0, NULL, 0), tail, data = HEADER + list_room;
	int failed = 1, writing = 0;
	if (file == INVALID_HANDLE_VALUE || write_all(file, zeros, HEADER) || writer_open(&w, SHARED_LEVEL, file, 0))
		goto stop;
	writing = 1;
	if (disk_seek(temporary, HEADER))
		goto stop;
	for (at = HEADER; at < length; at += CHUNK)
	{
		uint32_t size = length - at > CHUNK ? CHUNK : length - at;
		if (read_all(temporary, v->m.piece, size))
			goto stop;
		if (at + size > data)
		{
			uint32_t skip = data > at ? data - at : 0;
			crc = crc_add(crc, v->m.piece + skip, size - skip);
		}
		if (writer_put(&w, v->m.piece, size))
			goto stop;
	}
	writing = 0;
	if (writer_finish(&w) || crc != checksum)
		goto stop;
	tail = (SECTOR - (HEADER + w.written) % SECTOR) % SECTOR;
	if (write_all(file, zeros, tail))
		goto stop;
	shared_header(header, length, list_room, name, checksum);
	if (write_at(file, 0, header, HEADER))
		goto stop;
	failed = !finish(file);
	file = INVALID_HANDLE_VALUE;
stop:
	if (writing)
		deflateEnd(&w.z);
	if (file != INVALID_HANDLE_VALUE)
		file_close(file);
	return failed;
}

static int entry_by_at(void const *a, void const *b)
{
	uint32_t x = (*(struct entry *const *)a)->at, y = (*(struct entry *const *)b)->at;
	return x < y ? -1 : x > y;
}

/* is this shared file stored unpacked (its file's size is its file_length, as the game tells)? */
static int stored_unpacked(char const *path)
{
	unsigned char header[HEADER];
	HANDLE file = open_read(path);
	int unpacked = 0;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	if (!read_all(file, header, HEADER))
		unpacked = GetFileSize(file, NULL) == u32(header + 8);
	CloseHandle(file);
	return unpacked;
}

/* the blocks in list (n of them, in the order they're in the file) read where they are in an unpacked shared file and
checked against their hashes. 0 when all are */
static int check_where_they_are(struct converting *v, char const *path, struct entry *const *list, int n)
{
	HANDLE file = open_read(path);
	int k;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	for (k = 0; k < n; k++)
	{
		struct sha256 h;
		unsigned char digest[32];
		uint32_t left = list[k]->size, at = list[k]->at;
		sha256_start(&h);
		if (SetFilePointer(file, (LONG)at, NULL, FILE_BEGIN) != at)
			break;
		while (left)
		{
			uint32_t part = left > CHUNK ? CHUNK : left;
			if (read_all(file, v->m.piece, part))
				break;
			sha256_add(&h, v->m.piece, part);
			left -= part;
		}
		sha256_end(&h, digest);
		if (left || memcmp(digest, list[k]->digest, sizeof(list[k]->digest)))
			break;
	}
	CloseHandle(file);
	return k < n;
}

/* every block a converted map takes from stock.shared and the pack's file read back from there and checked against the
blocks it came from (their hashes). 0 when all are as they were */
static int check_shared(struct converting *v, char const *stock_path, char const *pack_path)
{
	struct entry **list = malloc((size_t)v->m.entries * sizeof(*list));
	struct span *spans = (struct span *)v->m.order;
	struct sha256 *hashes = v->m.hashes;
	int n, k, i, failed = 1, from_pack;
	uint32_t length;
	if (!list)
		return 1;
	for (k = 0; k < v->count; k++)
	{
		if (v->maps[k].state != CONVERTING)
			continue;
		for (i = 0; i < v->maps[k].count; i++)
		{
			if (is_moved(v->maps[k].blocks[i].e))
				v->maps[k].blocks[i].e->wanted = 1;
		}
	}
	for (from_pack = 0; from_pack < 2; from_pack++)
	{
		n = 0;
		for (k = 0; k < (int)TABLE_SIZE; k++)
		{
			struct entry *e = &v->m.table[k];
			if (e->used && e->wanted && (from_pack ? e->in_pack && !e->in_stock : e->in_stock))
				list[n++] = e;
		}
		qsort(list, (size_t)n, sizeof(list[0]), entry_by_at);
		/* stock.shared is unpacked, and a pack's file may be: each block read where it is */
		if (!from_pack || (n && stored_unpacked(pack_path)))
		{
			if (check_where_they_are(v, from_pack ? pack_path : stock_path, list, n))
				goto done;
		}
		else if (n)
		{
			/* the pack's file is packed: read through once, its blocks in the order they're in it (a few at a time) */
			for (k = 0; k < n; k += MOST_BLOCKS)
			{
				struct reader r;
				unsigned char header[HEADER];
				int part = n - k > MOST_BLOCKS ? MOST_BLOCKS : n - k, ok = 1;
				for (i = 0; i < part; i++)
				{
					spans[i].offset = list[k + i]->at;
					spans[i].size = list[k + i]->size;
					spans[i].hash = &hashes[i];
					sha256_start(&hashes[i]);
				}
				if (reader_open(&r, pack_path, header))
					goto done;
				ok = !hash_spans(&r, v->m.piece, HEADER, spans, part, 0, 0, NULL, &length) && length == u32(header + 8);
				reader_close(&r);
				for (i = 0; ok && i < part; i++)
				{
					unsigned char digest[32];
					sha256_end(&hashes[i], digest);
					ok = !memcmp(digest, list[k + i]->digest, sizeof(list[k + i]->digest));
				}
				if (!ok)
					goto done;
			}
		}
	}
	failed = 0;
done:
	free(list);
	return failed;
}

/* map index's small map read back: its header as made, every field as laid out, its tag data otherwise as the original's, and
every block that stays and every BSP as they were (their hashes). 0 when it is */
static int check_map(struct converting *v, int index, char const *path)
{
	struct cmap const *c = &v->maps[index];
	struct span *spans = (struct span *)v->m.order;
	struct sha256 *hashes = v->m.hashes;
	unsigned char header[HEADER], digest[32];
	struct reader r;
	uint32_t length, k, tag_offset = u32(c->header + 0x10);
	int n = 0, i, ok;
	for (i = 0; i < c->count; i++)
	{
		struct cblock const *b = &c->blocks[i];
		if (!b->e || is_moved(b->e))
			continue;
		spans[n].offset = b->want;
		spans[n].size = b->size;
		spans[n].hash = &hashes[i];
		sha256_start(&hashes[i]);
		n++;
	}
	for (k = 0; k < c->bsp_count; k++)
	{
		spans[n].offset = c->bsps[k].want;
		spans[n].size = c->bsps[k].size;
		spans[n].hash = &hashes[c->count + k];
		sha256_start(&hashes[c->count + k]);
		n++;
	}
	qsort(spans, (size_t)n, sizeof(spans[0]), span_order);
	if (reader_open(&r, path, header))
		return 1;
	ok = !memcmp(header, c->header, HEADER) &&
		!hash_spans(&r, v->m.piece, HEADER, spans, n, tag_offset, c->tag_size, v->m.tags, &length) &&
		length == c->out_length;
	reader_close(&r);
	if (!ok)
		return 1;
	/* (every field first, then they're zeroed: two tags can share one list of bitmaps or sounds, so a field can be there
	twice) */
	for (i = 0; i < c->count; i++)
	{
		struct cblock const *b = &c->blocks[i];
		if (!b->e)
			continue;
		if (u32(v->m.tags + b->field - c->tag_offset) != b->want)
			return 1;
		if (!is_moved(b->e))
		{
			sha256_end(&hashes[i], digest);
			if (memcmp(digest, b->e->digest, sizeof(b->e->digest)))
				return 1;
		}
	}
	for (k = 0; k < c->bsp_count; k++)
	{
		if (u32(v->m.tags + c->bsps[k].field - c->tag_offset) != c->bsps[k].want)
			return 1;
		sha256_end(&hashes[c->count + k], digest);
		if (memcmp(digest, c->bsps[k].digest, 32))
			return 1;
	}
	/* (the measuring zeroed them all, those past the map's end too, which were left as they were) */
	for (i = 0; i < c->count; i++)
		memset(v->m.tags + c->blocks[i].field - c->tag_offset, 0, 4);
	for (k = 0; k < c->bsp_count; k++)
		memset(v->m.tags + c->bsps[k].field - c->tag_offset, 0, 4);
	sha256(v->m.tags, c->tag_size, digest);
	return memcmp(digest, c->tag_digest, 32) != 0;
}

static uint32_t file_size(char const *path)
{
	HANDLE file = open_read(path);
	uint32_t size = 0;
	if (file != INVALID_HANDLE_VALUE)
	{
		size = GetFileSize(file, NULL);
		CloseHandle(file);
	}
	return size;
}

static char const *last_part(char const *path)
{
	char const *slash = strrchr(path, '\\');
	return slash ? slash + 1 : path;
}

/* (the maps' order in a pack's file: by their identities, whatever their names) */
static struct cmap const *ordering;

static int identity_order(void const *a, void const *b)
{
	struct cmap const *x = &ordering[*(int const *)a], *y = &ordering[*(int const *)b];
	if (x->raw_size != y->raw_size)
		return x->raw_size < y->raw_size ? -1 : 1;
	if (x->head != y->head)
		return x->head < y->head ? -1 : 1;
	if (x->tail != y->tail)
		return x->tail < y->tail ? -1 : 1;
	return *(int const *)a - *(int const *)b;
}

static uint32_t to_sector(uint32_t n)
{
	return n + (SECTOR - n % SECTOR) % SECTOR;
}

static int convert_pack(char const *stock_folder, char const *pack_folder, char const *pack_prefix, char const *const *paths,
	char const *const *folders, int count, consolidate_say_t say, struct consolidate_result *result, char *error)
{
	struct converting v;
	struct consolidate_measure measured;
	char path[260], out_path[260], temporary_path[260], shared_path[260], name[32], line[200];
	HANDLE temporary = INVALID_HANDLE_VALUE;
	uint32_t pack_length = HEADER, checksum = 0, list_room = 0;
	int i, k, status = 1, has_pack, changed, members = 0, blocks = 0, placed = 0;
	int *order = NULL;
	struct pblock *pblocks = NULL;
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {'E', ':', '\\', 0};

	memset(result, 0, sizeof(*result));
	memset(&v, 0, sizeof(v));
	name[0] = 0;
	text_print(path, sizeof(path), "%s\\stock.index", stock_folder);
	if (measuring_start(&v.m, path, error))
		goto done;
	v.count = count;
	v.maps = calloc((size_t)(count ? count : 1), sizeof(*v.maps));
	v.before = malloc(CHUNK);
	v.writes = malloc((MOST_BLOCKS + 1) * sizeof(*v.writes));
	if (!v.maps || !v.before || !v.writes)
	{
		text_print(error, 256, "Not enough memory.");
		goto done;
	}
	memset(&measured, 0, sizeof(measured));
	if (measure_all(&v.m, paths, count, say, &measured, v.maps, error))
		goto done;
	result->already = measured.already;
	result->unreadable = measured.unreadable;
	step_begin();
	/* each map's small map laid out: one that would be too big to play is left (a block's place doesn't change that), and
	what it uses no longer counts, so another may keep a block it shared and grow: again until none is left */
	for (changed = 1; changed;)
	{
		changed = 0;
		for (i = 0; i < count; i++)
		{
			struct layout l;
			struct cmap *c = &v.maps[i];
			int too_big;
			if (c->state != CONVERTING)
				continue;
			too_big = lay_out(c, &l) || c->out_length > (c->header[0x60] == 0 && c->header[0x61] == 0 ? CAMPAIGN_SLOT : MP_SLOT);
			layout_free(&l);
			if (!too_big)
				continue;
			c->state = LEFT;
			changed = 1;
			result->too_big++;
			for (k = 0; k < c->count; k++)
			{
				struct entry *e = c->blocks[k].e;
				if (e && e->last != -2 - i)
				{
					e->last = (int16_t)(-2 - i);
					e->maps--;
				}
			}
			text_print(line, sizeof(line), "%s: it would be too big to play, left as it is", last_part(paths[i]));
			if (say)
				say(1, line);
		}
	}
	/* the pack's file laid out as shared_pass.py lays it out: its list (the maps in the order of their identities, then its
	blocks), then each map's blocks in that order, in its tags' order, each at its remainder modulo 512 */
	order = malloc((size_t)(count > 0 ? count : 1) * sizeof(*order));
	if (!order)
	{
		text_print(error, 256, "Not enough memory.");
		goto done;
	}
	for (i = 0; i < count; i++)
	{
		if (v.maps[i].state == CONVERTING)
			order[members++] = i;
	}
	ordering = v.maps;
	qsort(order, (size_t)members, sizeof(order[0]), identity_order);
	for (k = 0; k < (int)TABLE_SIZE; k++)
	{
		struct entry const *e = &v.m.table[k];
		if (e->used && !e->in_stock && e->maps >= 2)
			blocks++;
	}
	pblocks = malloc((size_t)(blocks ? blocks : 1) * sizeof(*pblocks));
	if (!pblocks)
	{
		text_print(error, 256, "Not enough memory.");
		goto done;
	}
	if (blocks)
		list_room = to_sector(LIST_HEAD + LIST_MAP * (uint32_t)members + LIST_BLOCK * (uint32_t)blocks);
	pack_length = HEADER + list_room;
	for (i = 0; i < members; i++)
	{
		struct cmap *c = &v.maps[order[i]];
		for (k = 0; k < c->count; k++)
		{
			struct entry *e = c->blocks[k].e;
			if (!e || e->in_stock || e->maps < 2 || e->in_pack)
				continue;
			pack_length += (e->remainder + 512 - pack_length % 512) % 512;
			e->at = pack_length;
			e->in_pack = 1;
			e->writer = (int16_t)order[i];
			if (placed < blocks)
				pblocks[placed].e = e;
			placed++;
			pack_length += e->size;
		}
	}
	pack_length = to_sector(pack_length);
	if (placed != blocks)
	{
		text_print(error, 256, "The pack's shared file couldn't be laid out.");
		goto done;
	}
	has_pack = blocks > 0;
	/* room: the pack's file twice (as it's made, and packed) and the maps, by the measure's estimate, and some to spare */
	step_end("laid out");
	root[0] = pack_folder[0];
	disk_sync(); /* (what's still to be written takes room too) */
	if (GetDiskFreeSpaceExA(root, &available, &total, &free_total) &&
		available.QuadPart < (unsigned long long)pack_length * 2 + measured.after + 16 * 1024 * 1024)
	{
		text_print(error, 256, "Not enough room: it needs about %lu MB free on the drive.",
			(unsigned long)(((unsigned long long)pack_length * 2 + measured.after) >> 20) + 16);
		goto done;
	}
	/* the pack's file unpacked, zeros between its blocks */
	text_print(temporary_path, sizeof(temporary_path), "%s\\consolidating.tmp", pack_folder);
	if (has_pack)
	{
		uint32_t at;
		temporary = create(temporary_path, 1);
		if (temporary == INVALID_HANDLE_VALUE)
		{
			text_print(error, 256, "Can't write in %s.", pack_folder);
			goto done;
		}
		/* (1 MB a write, as stock.shared's: a sector a write took 157 s for Covenant Edition's 143 MB on a console;
		without the memory, a sector a write) */
		{
			/* (one stored unpacked is exactly its length: the game reads it where it is when its size is that) */
			uint32_t piece = 1024u * 1024u, whole = pack_length > UNPACKED_OVER ? pack_length :
				(pack_length + SECTOR - 1) / SECTOR * SECTOR;
			unsigned char *zeroed = calloc(1, piece);
			if (!zeroed)
				piece = SECTOR;
			for (at = 0; at < whole; at += piece)
			{
				uint32_t part = whole - at < piece ? whole - at : piece;
				if (write_all(temporary, zeroed ? zeroed : zeros, part))
				{
					free(zeroed);
					text_print(error, 256, "Can't write in %s.", pack_folder);
					goto done;
				}
			}
			free(zeroed);
		}
		step_end("the pack's file filled with zeros");
	}
	/* each map's small map, beside its map.link as <name>.map.new until it's checked */
	for (i = 0; i < count; i++)
	{
		if (v.maps[i].state != CONVERTING)
			continue;
		text_print(line, sizeof(line), "Converting %d of %d: %s", i + 1, count, last_part(paths[i]));
		if (say)
			say(0, line);
		text_print(out_path, sizeof(out_path), "%s\\%s.new", folders[i], last_part(paths[i]));
		if (convert_map(&v, i, paths[i], out_path, temporary))
		{
			text_print(error, 256, "%s couldn't be converted (written to %s).", last_part(paths[i]), folders[i]);
			goto done;
		}
		step_end("%s made small", last_part(paths[i]));
	}
	/* the pack's file read back from its layout: its checksum, which names it, and its blocks' hashes, for its list */
	if (has_pack)
	{
		if (say)
			say(0, "Reading the pack's shared file back...");
		if (pack_data(&v, temporary, HEADER + list_room, pack_length, pblocks, blocks, &checksum))
		{
			text_print(error, 256, "The pack's shared file didn't read back right: nothing was changed.");
			goto done;
		}
		text_print(name, 32, "%s-%08lx", pack_prefix, (unsigned long)checksum);
		step_end("the pack's file read back");
	}
	/* the small maps' headers name it */
	for (i = 0; i < count; i++)
	{
		HANDLE file;
		struct cmap *c = &v.maps[i];
		int ok;
		if (c->state != CONVERTING)
			continue;
		set_reference(c, &v.m, has_pack ? name : NULL, checksum, pack_length);
		small_crc(c);
		text_print(out_path, sizeof(out_path), "%s\\%s.new", folders[i], last_part(paths[i]));
		file = CreateFileA(out_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		ok = file != INVALID_HANDLE_VALUE && !write_at(file, 0, c->header, HEADER);
		if (file != INVALID_HANDLE_VALUE && !finish(file))
			ok = 0;
		if (!ok)
		{
			text_print(error, 256, "%s couldn't be written.", out_path);
			goto done;
		}
	}
	step_end("the small maps' headers written");
	/* then its list, and it's packed and named */
	if (has_pack && pack_length > UNPACKED_OVER)
	{
		if (say)
			say(0, "Finishing the pack's shared file (kept unpacked: it's bigger than the game's biggest cache file)...");
		if (write_list(&v, temporary, list_room, order, members, pblocks, blocks) ||
			shared_unpacked(&v, &temporary, pack_length, list_room, name, checksum))
		{
			text_print(error, 256, "The pack's shared file couldn't be written.");
			goto done;
		}
		text_print(path, sizeof(path), "%s\\%s.shared", pack_folder, name);
		DeleteFileA(path);
		if (!MoveFileA(temporary_path, path))
		{
			text_print(error, 256, "The pack's shared file couldn't be named.");
			goto done;
		}
		text_print(shared_path, sizeof(shared_path), "%s", path);
		step_end("the pack's file finished, unpacked");
	}
	else if (has_pack)
	{
		if (say)
			say(0, "Packing the pack's shared file...");
		text_print(shared_path, sizeof(shared_path), "%s\\consolidating.shared", pack_folder);
		if (write_list(&v, temporary, list_room, order, members, pblocks, blocks) ||
			pack_shared(&v, temporary, pack_length, list_room, shared_path, name, checksum))
		{
			text_print(error, 256, "The pack's shared file couldn't be written.");
			goto done;
		}
		file_close(temporary);
		temporary = INVALID_HANDLE_VALUE;
		DeleteFileA(temporary_path);
		text_print(path, sizeof(path), "%s\\%s.shared", pack_folder, name);
		DeleteFileA(path);
		if (!MoveFileA(shared_path, path))
		{
			text_print(error, 256, "The pack's shared file couldn't be named.");
			goto done;
		}
		text_print(shared_path, sizeof(shared_path), "%s", path);
		step_end("the pack's file packed");
	}
	/* then read back: what's in the shared files, and each map; a map that reads back right takes its link's place */
	if (say)
		say(0, "Checking what's in the shared files...");
	text_print(path, sizeof(path), "%s\\stock.shared", stock_folder);
	if (check_shared(&v, path, has_pack ? shared_path : NULL))
	{
		text_print(error, 256, "The shared files didn't read back right: nothing was changed.");
		goto done;
	}
	step_end("the shared files' blocks checked");
	for (i = 0; i < count; i++)
	{
		struct cmap *c = &v.maps[i];
		char final_path[260];
		if (c->state != CONVERTING)
			continue;
		text_print(line, sizeof(line), "Checking %d of %d: %s", i + 1, count, last_part(paths[i]));
		if (say)
			say(0, line);
		text_print(out_path, sizeof(out_path), "%s\\%s.new", folders[i], last_part(paths[i]));
		text_print(final_path, sizeof(final_path), "%s\\%s", folders[i], last_part(paths[i]));
		if (check_map(&v, i, out_path))
		{
			c->state = LEFT;
			result->failed++;
			DeleteFileA(out_path);
			text_print(line, sizeof(line), "%s didn't read back right: left as it is", last_part(paths[i]));
			if (say)
				say(1, line);
			continue;
		}
		/* (a .map in the folder is played before its map.link, which stays: it marks the folder as the import's) */
		DeleteFileA(final_path);
		if (!MoveFileA(out_path, final_path))
		{
			c->state = LEFT;
			result->failed++;
			DeleteFileA(out_path);
			continue;
		}
		c->state = CONVERTED;
		result->converted++;
		result->before += c->raw_size;
		result->after += file_size(final_path);
		step_end("%s read back and put in place", last_part(paths[i]));
	}
	if (has_pack)
	{
		if (result->converted)
			result->after += file_size(shared_path);
		else
			DeleteFileA(shared_path);
	}
	if (result->converted && has_pack)
		text_print(result->shared, sizeof(result->shared), "%s", name);
	status = 0;
done:
	if (temporary != INVALID_HANDLE_VALUE)
		file_close(temporary);
	disk_sync();
	if (status)
	{
		/* (nothing changes when it stops: what it made goes) */
		DeleteFileA(temporary_path);
		if (name[0])
		{
			text_print(path, sizeof(path), "%s\\%s.shared", pack_folder, name);
			DeleteFileA(path);
		}
		text_print(path, sizeof(path), "%s\\consolidating.shared", pack_folder);
		DeleteFileA(path);
		for (i = 0; i < count; i++)
		{
			text_print(out_path, sizeof(out_path), "%s\\%s.new", folders[i], last_part(paths[i]));
			DeleteFileA(out_path);
		}
	}
	if (v.maps)
	{
		for (i = 0; i < count; i++)
			free(v.maps[i].blocks);
	}
	free(v.maps);
	free(v.before);
	free(v.writes);
	free(order);
	free(pblocks);
	measuring_end(&v.m);
	return status;
}

int consolidate_convert(char const *stock_folder, char const *pack_folder, char const *pack_prefix, char const *const *paths,
	char const *const *folders, int count, consolidate_say_t say, struct consolidate_result *result, char *error)
{
	int status;
	disk_on();
	whole_begin();
	status = convert_pack(stock_folder, pack_folder, pack_prefix, paths, folders, count, say, result, error);
	whole_end("consolidating");
	disk_off();
	return status;
}

/* ---- shrinking: full-size maps a pack's file was made from, made small against it ---- */

/* a pack's file's header and its list (malloc'd, *room bytes, checked as one), packed or stored unpacked. 0 when read */
static int list_read(char const *path, unsigned char *header, unsigned char **list, uint32_t *room)
{
	HANDLE file = open_read(path);
	uint32_t size, maps, blocks;
	int failed = 1;
	*list = NULL;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	size = GetFileSize(file, NULL);
	if (read_all(file, header, HEADER) || memcmp(header, "daeh", 4) || u32(header + 4) != 5 ||
		!memchr(header + 0x20, 0, 32) || !header[0x20])
	{
		CloseHandle(file);
		return 1;
	}
	*room = u32(header + 0x18);
	if (*room < LIST_HEAD || *room % SECTOR || *room > (16u << 20) || u32(header + 8) < HEADER ||
		*room > u32(header + 8) - HEADER || !(*list = malloc(*room)))
	{
		CloseHandle(file);
		return 1;
	}
	if (size == u32(header + 8))
	{
		/* (stored unpacked: read where it is) */
		failed = read_all(file, *list, *room);
		CloseHandle(file);
	}
	else
	{
		struct reader r;
		CloseHandle(file);
		failed = reader_open(&r, path, header) || reader_pull(&r, *list, *room);
		reader_close(&r);
	}
	if (!failed)
	{
		maps = u32(*list + 8);
		blocks = u32(*list + 12);
		failed = memcmp(*list, LIST_SIGNATURE, 4) || u32(*list + 4) != 1 || maps > *room / LIST_MAP ||
			blocks > *room / LIST_BLOCK || LIST_HEAD + maps * LIST_MAP + blocks * LIST_BLOCK > *room;
	}
	if (failed)
	{
		free(*list);
		*list = NULL;
	}
	return failed;
}

int consolidate_members(char const *pack_path, struct consolidate_member *members, int most, int *count, char *name)
{
	unsigned char header[HEADER], *list, *p;
	uint32_t room;
	int k;
	*count = 0;
	if (list_read(pack_path, header, &list, &room))
		return 1;
	memcpy(name, header + 0x20, 32);
	for (k = 0, p = list + LIST_HEAD; k < (int)u32(list + 8) && k < most; k++, p += LIST_MAP)
	{
		members[k].size = u32(p);
		members[k].head = u32(p + 4);
		members[k].tail = u32(p + 8);
		members[k].small_length = u32(p + 12);
		members[k].small_crc = u32(p + 16);
	}
	*count = k;
	free(list);
	return 0;
}

static int shrink_maps(char const *stock_folder, char const *pack_path, char const *const *paths, int count,
	consolidate_say_t say, struct consolidate_result *result, char *error)
{
	struct converting v;
	struct consolidate_measure measured;
	unsigned char header[HEADER], *list = NULL, *p;
	char path[260], out_path[260], old_path[260], name[32], line[200];
	uint32_t room = 0, maps = 0, blocks, checksum, length, k;
	unsigned long long need = 16ull << 20;
	int i, status = 1, *member = NULL;
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {'E', ':', '\\', 0};

	memset(result, 0, sizeof(*result));
	memset(&v, 0, sizeof(v));
	text_print(path, sizeof(path), "%s\\stock.index", stock_folder);
	if (measuring_start(&v.m, path, error))
		goto done;
	if (list_read(pack_path, header, &list, &room))
	{
		text_print(error, 256, "%s has no list of its maps.", last_part(pack_path));
		goto done;
	}
	memcpy(name, header + 0x20, 32);
	checksum = u32(header + 0x64);
	length = u32(header + 8);
	maps = u32(list + 8);
	blocks = u32(list + 12);
	/* its blocks, as two of its maps' blocks in it (what stock.shared has is taken from there, as when it was made) */
	for (k = 0, p = list + LIST_HEAD + maps * LIST_MAP; k < blocks; k++, p += LIST_BLOCK)
	{
		struct entry *e = find_entry(&v.m, p, u32(p + 36), u32(p + 32) % 512);
		if (!e)
		{
			text_print(error, 256, "Too many different textures and sounds to count.");
			goto done;
		}
		if (!e->in_stock)
		{
			e->in_pack = 1;
			e->at = u32(p + 32);
		}
	}
	v.count = count;
	v.maps = calloc((size_t)(count > 0 ? count : 1), sizeof(*v.maps));
	member = calloc((size_t)(count > 0 ? count : 1), sizeof(*member));
	v.before = malloc(CHUNK);
	v.writes = malloc((MOST_BLOCKS + 1) * sizeof(*v.writes));
	if (!v.maps || !member || !v.before || !v.writes)
	{
		text_print(error, 256, "Not enough memory.");
		goto done;
	}
	memset(&measured, 0, sizeof(measured));
	if (measure_all(&v.m, paths, count, say, &measured, v.maps, error))
		goto done;
	result->already = measured.already;
	result->unreadable = measured.unreadable;
	/* (what's in the pack's file moves there, the rest stays: nothing is added to it) */
	for (k = 0; k < TABLE_SIZE; k++)
	{
		struct entry *e = &v.m.table[k];
		if (!e->used)
			continue;
		e->writer = -1;
		if (!e->in_stock)
			e->maps = e->in_pack ? 2 : e->maps ? 1 : 0;
	}
	/* each map: one the list names, and whose small map comes out the length it says */
	for (i = 0; i < count; i++)
	{
		struct cmap *c = &v.maps[i];
		struct layout l;
		int laid;
		if (c->state != CONVERTING)
			continue;
		member[i] = -1;
		memset(&l, 0, sizeof(l));
		for (k = 0, p = list + LIST_HEAD; k < maps; k++, p += LIST_MAP)
		{
			if (u32(p) == c->raw_size && u32(p + 4) == c->head && u32(p + 8) == c->tail)
				member[i] = (int)k;
		}
		laid = member[i] >= 0 && !lay_out(c, &l) &&
			c->out_length == u32(list + LIST_HEAD + (uint32_t)member[i] * LIST_MAP + 12);
		layout_free(&l);
		if (!laid)
		{
			c->state = LEFT;
			result->failed++;
			text_print(line, sizeof(line), member[i] < 0 ? "%s isn't one of %s's maps: left as it is" :
				"%s comes out another length than %s says: left as it is", last_part(paths[i]), name);
			if (say)
				say(1, line);
			continue;
		}
		/* (its small map packed about as well as it does) */
		need += c->length ? (unsigned long long)c->out_length * c->raw_size / c->length : c->out_length;
	}
	root[0] = count ? paths[0][0] : 'E';
	disk_sync();
	if (count && GetDiskFreeSpaceExA(root, &available, &total, &free_total) && available.QuadPart < need)
	{
		text_print(error, 256, "Not enough room: it needs about %lu MB free on the drive.", (unsigned long)(need >> 20));
		goto done;
	}
	/* each small map beside its map as <name>.map.new, its header naming the pack's file, its CRC the list's */
	for (i = 0; i < count; i++)
	{
		struct cmap *c = &v.maps[i];
		HANDLE file;
		int ok;
		if (c->state != CONVERTING)
			continue;
		text_print(line, sizeof(line), "Shrinking %d of %d: %s", i + 1, count, last_part(paths[i]));
		if (say)
			say(0, line);
		text_print(out_path, sizeof(out_path), "%s.new", paths[i]);
		if (convert_map(&v, i, paths[i], out_path, INVALID_HANDLE_VALUE))
		{
			text_print(error, 256, "%s couldn't be shrunk (written beside it).", last_part(paths[i]));
			goto done;
		}
		set_reference(c, &v.m, name, checksum, length);
		small_crc(c);
		if (c->small_crc != u32(list + LIST_HEAD + (uint32_t)member[i] * LIST_MAP + 16))
		{
			c->state = LEFT;
			result->failed++;
			DeleteFileA(out_path);
			text_print(line, sizeof(line), "%s doesn't come out as %s says: left as it is", last_part(paths[i]), name);
			if (say)
				say(1, line);
			continue;
		}
		file = CreateFileA(out_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		ok = file != INVALID_HANDLE_VALUE && !write_at(file, 0, c->header, HEADER);
		if (file != INVALID_HANDLE_VALUE && !finish(file))
			ok = 0;
		if (!ok)
		{
			text_print(error, 256, "%s couldn't be written.", out_path);
			goto done;
		}
		step_end("%s made small", last_part(paths[i]));
	}
	/* read back: what's in the shared files, then each map, which then takes its full-size one's place */
	if (say)
		say(0, "Checking what's in the shared files...");
	text_print(path, sizeof(path), "%s\\stock.shared", stock_folder);
	if (check_shared(&v, path, pack_path))
	{
		text_print(error, 256, "The shared files didn't read back right: nothing was changed.");
		goto done;
	}
	step_end("the shared files' blocks checked");
	for (i = 0; i < count; i++)
	{
		struct cmap *c = &v.maps[i];
		if (c->state != CONVERTING)
			continue;
		text_print(line, sizeof(line), "Checking %d of %d: %s", i + 1, count, last_part(paths[i]));
		if (say)
			say(0, line);
		text_print(out_path, sizeof(out_path), "%s.new", paths[i]);
		text_print(old_path, sizeof(old_path), "%s.big", paths[i]);
		if (check_map(&v, i, out_path))
		{
			c->state = LEFT;
			result->failed++;
			DeleteFileA(out_path);
			text_print(line, sizeof(line), "%s didn't read back right: left as it is", last_part(paths[i]));
			if (say)
				say(1, line);
			continue;
		}
		/* (through <name>.map.big: a power cut leaves one of the two, which the next look puts right) */
		DeleteFileA(old_path);
		if (!MoveFileA(paths[i], old_path) || !MoveFileA(out_path, paths[i]))
		{
			if (GetFileAttributesA(paths[i]) == INVALID_FILE_ATTRIBUTES)
				MoveFileA(old_path, paths[i]);
			c->state = LEFT;
			result->failed++;
			DeleteFileA(out_path);
			text_print(line, sizeof(line), "%s couldn't be swapped for its small map: left as it is", last_part(paths[i]));
			if (say)
				say(1, line);
			continue;
		}
		DeleteFileA(old_path);
		c->state = CONVERTED;
		result->converted++;
		result->before += c->raw_size;
		result->after += file_size(paths[i]);
		step_end("%s read back and put in place", last_part(paths[i]));
	}
	text_print(result->shared, sizeof(result->shared), "%s", name);
	status = 0;
done:
	disk_sync();
	if (status && v.maps)
	{
		for (i = 0; i < count; i++)
		{
			text_print(out_path, sizeof(out_path), "%s.new", paths[i]);
			DeleteFileA(out_path);
		}
	}
	if (v.maps)
	{
		for (i = 0; i < count; i++)
			free(v.maps[i].blocks);
	}
	free(v.maps);
	free(v.before);
	free(v.writes);
	free(member);
	free(list);
	measuring_end(&v.m);
	return status;
}

int consolidate_shrink(char const *stock_folder, char const *pack_path, char const *const *paths, int count,
	consolidate_say_t say, struct consolidate_result *result, char *error)
{
	int status;
	disk_on();
	whole_begin();
	status = shrink_maps(stock_folder, pack_path, paths, count, say, result, error);
	whole_end("shrinking");
	disk_off();
	return status;
}
