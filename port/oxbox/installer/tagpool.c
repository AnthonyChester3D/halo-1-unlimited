/* tagpool.c: the stock tag pool (docs/shared_maps.md, "Shared tag data (format 4)"), on a PC.

	cc -O2 -o tagpool tagpool.c -lz
	tagpool RETAIL_MAPS_DIR OUT_DIR [--maps]      (about 4 GB of memory with --maps: the maps unpacked)
	tagpool --pack STOCKTAGS PACK_SHARED NAME OUT_DIR MAP...

From the 23 retail maps (Xbox cache files, as on the disc) it makes, in OUT_DIR:
- stocktags.shared: the pool (every word of the 23 maps' tag data once), in 16 KB frames, each zlib or raw;
- stock_recipes.bin: each map's recipe and its rebuilt tag data's size and CRC32 (what the installer will carry: its
  runs that copy from the pool's end, as it then is, are how the pool is made, so the installer needs nothing else);
- with --maps, maps/<name>.map: each retail map with a recipe (its tag data's words zeros, the recipe after its data,
  its reference version 3), packed as the disc's are: for testing the game's rebuild.
Then it checks every map: its tag data rebuilt from the written stocktags.shared and its recipe, word for word against
the retail map's, and prints the sizes. Nothing it writes may be distributed: the pool is Microsoft's tag data.

With --pack, a pack's pass (format 4's "pack pass"): a pack's maps (each drawing on PACK_SHARED, made by shared_pass.py or
Halo Map Porter) matched against the pool in STOCKTAGS (a stocktags.shared) and each other, in the order of their
identities; it writes OUT_DIR/NAME-<checksum>.shared (PACK_SHARED with its tag part after its last block) and each map
with its recipe (OUT_DIR/<the map's folder>/<file>), checking each rebuilds the same. The maps it writes carry no word of
the pool, but a pack's maps still hold whatever the pack's author may share (a Custom Edition pack's EULA applies). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <zlib.h>

#define HEADER 0x800
#define WINDOW 8                   /* words a window */
#define MINIMUM_RUN 16             /* words: 64 bytes */
#define FRAME 16384                /* bytes */
#define FRAME_WORDS (FRAME / 4)
#define TABLE_BITS 25
#define TABLE_SIZE (1u << TABLE_BITS)
#define MAP_COUNT 23

static char const *const map_names[MAP_COUNT] =
{
	"beavercreek", "bloodgulch", "boardingaction", "carousel", "chillout", "damnation", "hangemhigh", "longest",
	"prisoner", "putput", "ratrace", "sidewinder", "wizard",
	"a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40",
};

struct run { uint32_t dest, source, length; };   /* words */

struct map
{
	uint8_t *image;                 /* the map unpacked, header included */
	uint32_t file_length, tag_offset, tag_size;
	struct run *runs;
	size_t run_count;
	uint8_t *recipe;
	size_t recipe_size;
	uint32_t tag_crc;
	/* (a pack's map) */
	char const *path;
	uint32_t identity[3];
	struct run *pack_runs;          /* (from the pack's tag part) */
	size_t pack_run_count;
	struct run *candidate_runs;     /* (from the candidates, before the tag part is made) */
	size_t candidate_run_count;
	uint32_t *own;                  /* its own words' places (dest) */
	size_t own_count, own_first;    /* (own_first: the first's place in the candidates) */
};

static struct map maps[MAP_COUNT];
static uint32_t *pool;
static size_t pool_count, pool_room;

static void fail(char const *what, char const *detail)
{
	fprintf(stderr, "tagpool: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	exit(1);
}

static uint32_t u32(uint8_t const *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static uint8_t *read_file(char const *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *data;
	long length;

	if (!f)
		fail("can't read", path);
	fseek(f, 0, SEEK_END), length = ftell(f), fseek(f, 0, SEEK_SET);
	data = malloc((size_t)length + 1);
	if (fread(data, 1, (size_t)length, f) != (size_t)length)
		fail("can't read", path);
	fclose(f);
	*size = (size_t)length;
	return data;
}

static void write_file(char const *path, void const *data, size_t size)
{
	FILE *f = fopen(path, "wb");

	if (!f || fwrite(data, 1, size, f) != size || fclose(f))
		fail("can't write", path);
}

/* a map as the game sees it: its header, then the rest unpacked (one zlib stream on the disc) */
static void map_read_path(struct map *m, char const *path)
{
	size_t size;
	uint8_t *raw;

	raw = read_file(path, &size);
	if (size < HEADER || u32(raw) != 0x68656164u || u32(raw + 4) != 5)
		fail("not an Xbox map", path);
	m->file_length = u32(raw + 8);
	m->image = calloc(1, m->file_length);
	memcpy(m->image, raw, HEADER);
	if (raw[HEADER] == 0x78)
	{
		uLongf out = m->file_length - HEADER;

		if (uncompress(m->image + HEADER, &out, raw + HEADER, size - HEADER) != Z_OK || out != m->file_length - HEADER)
			fail("can't unpack", path);
	}
	else
		memcpy(m->image + HEADER, raw + HEADER, (size < m->file_length ? size : m->file_length) - HEADER);
	free(raw);
	m->tag_offset = u32(m->image + 0x10);
	m->tag_size = u32(m->image + 0x14);
	if ((m->tag_size & 3) || m->tag_offset + (uint64_t)m->tag_size > m->file_length)
		fail("its tag data isn't whole words inside it", path);
}

static void map_read(char const *dir, int k)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s.map", dir, map_names[k]);
	map_read_path(&maps[k], path);
}

/* ---------- the matcher (the pool's rule) */

/* each distinct 8-word window of some words: its earliest position + 1 (0: none), open addressing */
struct windows { uint32_t *slots; unsigned bits; };

static struct windows pool_windows = { NULL, TABLE_BITS };

static uint32_t window_slot(uint32_t const *w, unsigned bits)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;
	int k;

	for (k = 0; k < WINDOW; k++)
		h = (h ^ w[k]) * 0x100000001B3ull, h ^= h >> 29;
	return (uint32_t)(h >> (64 - bits));
}

static void windows_add(struct windows *t, uint32_t const *base, size_t position)
{
	uint32_t mask = (1u << t->bits) - 1, slot = window_slot(base + position, t->bits);

	for (;; slot = (slot + 1) & mask)
	{
		uint32_t at = t->slots[slot];

		if (!at)
		{
			t->slots[slot] = (uint32_t)position + 1;
			return;
		}
		if (!memcmp(base + at - 1, base + position, WINDOW * 4))
			return;                 /* (the window's earliest place stays) */
	}
}

static long windows_find(struct windows const *t, uint32_t const *base, uint32_t const *w)
{
	uint32_t mask = (1u << t->bits) - 1, slot = window_slot(w, t->bits);

	for (;; slot = (slot + 1) & mask)
	{
		uint32_t at = t->slots[slot];

		if (!at)
			return -1;
		if (!memcmp(base + at - 1, w, WINDOW * 4))
			return (long)at - 1;
	}
}

static long table_find(uint32_t const *w)
{
	return windows_find(&pool_windows, pool, w);
}

static void pool_append(uint32_t word)
{
	if (pool_count == pool_room)
	{
		pool_room = pool_room ? pool_room * 2 : 1 << 22;
		pool = realloc(pool, pool_room * 4);
	}
	pool[pool_count++] = word;
	if (pool_count >= WINDOW)
		windows_add(&pool_windows, pool, pool_count - WINDOW);
}

/* a range of the map's words from a source (the pool, or the pack's tag part), split at its frames; one continuing the
last run (in both) joins it. Fed in the order of dest, so runs are maximal */
static void add_run_to(struct run *runs, size_t *count, size_t dest, size_t source, size_t length)
{
	while (length)
	{
		size_t room = FRAME_WORDS - source % FRAME_WORDS, piece = length < room ? length : room;
		struct run *last = *count ? &runs[*count - 1] : NULL;

		if (last && last->dest + last->length == dest && last->source + last->length == source && source % FRAME_WORDS)
			last->length += (uint32_t)piece;
		else
		{
			runs[*count].dest = (uint32_t)dest;
			runs[*count].source = (uint32_t)source;
			runs[*count].length = (uint32_t)piece;
			(*count)++;
		}
		dest += piece, source += piece, length -= piece;
	}
}

static void add_run(struct map *m, size_t dest, size_t source, size_t length)
{
	add_run_to(m->runs, &m->run_count, dest, source, length);
}

static void map_match(struct map *m)
{
	uint32_t const *w = (uint32_t const *)(m->image + m->tag_offset);
	size_t n = m->tag_size / 4, i = 0;

	m->runs = malloc((n + 1) * sizeof(struct run));
	m->run_count = 0;
	while (i < n)
	{
		long source = i + WINDOW <= n ? table_find(w + i) : -1;
		size_t length = 0;

		if (source >= 0)
			while (i + length < n && (size_t)source + length < pool_count && pool[source + length] == w[i + length])
				length++;
		if (source < 0 || length < MINIMUM_RUN)
		{
			/* (new: appended one by one until a word starts a window the pool has) */
			source = (long)pool_count;
			length = 0;
			do
			{
				pool_append(w[i + length]);
				length++;
			} while (i + length < n && !(i + length + WINDOW <= n && table_find(w + i + length) >= 0));
		}
		add_run(m, i, (size_t)source, length);
		i += length;
	}
}

/* ---------- the recipe (LEB128, grouped by frame) */

static uint8_t *out_bytes;
static size_t out_count, out_room;

static void out_byte(uint8_t b)
{
	if (out_count == out_room)
		out_room = out_room ? out_room * 2 : 1 << 16, out_bytes = realloc(out_bytes, out_room);
	out_bytes[out_count++] = b;
}

static void out_leb(uint32_t v)
{
	do
	{
		uint8_t b = v & 0x7F;

		v >>= 7;
		out_byte(v ? b | 0x80 : b);
	} while (v);
}

static int run_order(void const *a, void const *b)
{
	struct run const *x = a, *y = b;
	uint32_t fx = x->source / FRAME_WORDS, fy = y->source / FRAME_WORDS;

	return fx != fy ? (fx < fy ? -1 : 1) : x->dest < y->dest ? -1 : x->dest > y->dest;
}

/* a source's groups: its runs by frame, then dest */
static void groups_out(struct run const *runs, size_t run_count)
{
	struct run *r = malloc((run_count + 1) * sizeof(struct run));
	size_t k = 0, groups = 0, g;
	uint32_t last_frame = 0;

	memcpy(r, runs, run_count * sizeof(struct run));
	qsort(r, run_count, sizeof(struct run), run_order);
	for (g = 0; g < run_count; g++)
		groups += !g || r[g].source / FRAME_WORDS != r[g - 1].source / FRAME_WORDS;
	out_leb((uint32_t)groups);
	for (g = 0; g < groups; g++)
	{
		uint32_t frame = r[k].source / FRAME_WORDS, end = 0;
		size_t first = k, count;

		while (k < run_count && r[k].source / FRAME_WORDS == frame)
			k++;
		count = k - first;
		out_leb(g ? frame - last_frame : frame);
		out_leb((uint32_t)count);
		for (; first < k; first++)
		{
			out_leb(r[first].dest - end);
			out_leb(r[first].source % FRAME_WORDS);
			out_leb(r[first].length);
			end = r[first].dest + r[first].length;
		}
		last_frame = frame;
	}
	free(r);
}

static void map_recipe(struct map *m)
{
	out_count = 0;
	groups_out(m->runs, m->run_count);
	groups_out(m->pack_runs, m->pack_run_count);     /* (none for a stock map) */
	m->recipe = malloc(out_count);
	memcpy(m->recipe, out_bytes, out_count);
	m->recipe_size = out_count;
}

/* ---------- stocktags.shared */

static uint8_t *stocktags;
static size_t stocktags_size;

static void stocktags_make(void)
{
	size_t pool_bytes = pool_count * 4, frames = (pool_bytes + FRAME - 1) / FRAME, f, at;
	uint8_t *packed = malloc(compressBound(FRAME));

	stocktags = calloc(1, HEADER + frames * 8 + pool_bytes + frames * 64);
	memcpy(stocktags, "OXST", 4);
	put32(stocktags + 0x04, 1);
	put32(stocktags + 0x0C, (uint32_t)pool_bytes);
	put32(stocktags + 0x10, (uint32_t)crc32(0, (uint8_t const *)pool, (uInt)pool_bytes));
	put32(stocktags + 0x14, FRAME);
	put32(stocktags + 0x18, (uint32_t)frames);
	put32(stocktags + 0x1C, HEADER);
	at = HEADER + frames * 8;
	for (f = 0; f < frames; f++)
	{
		uint8_t const *frame = (uint8_t const *)pool + f * FRAME;
		size_t size = pool_bytes - f * FRAME < FRAME ? pool_bytes - f * FRAME : FRAME;
		uLongf packed_size = compressBound(FRAME);

		if (compress2(packed, &packed_size, frame, (uLong)size, 9) != Z_OK)
			fail("zlib", NULL);
		/* (raw when zlib's isn't smaller: a stored size equal to the frame's means raw) */
		if (packed_size >= size)
			memcpy(stocktags + at, frame, size), packed_size = size;
		else
			memcpy(stocktags + at, packed, packed_size);
		put32(stocktags + HEADER + f * 8, (uint32_t)at);
		put32(stocktags + HEADER + f * 8 + 4, (uint32_t)packed_size);
		at += packed_size;
	}
	put32(stocktags + 0x08, (uint32_t)at);
	stocktags_size = at;
	free(packed);
}

/* ---------- the check: as the game will rebuild a map, from the file */

static uint32_t leb(uint8_t const **p, uint8_t const *end)
{
	uint32_t v = 0;
	int shift = 0;

	for (;;)
	{
		uint8_t b;

		if (*p >= end || shift > 28)
			fail("a recipe runs past its end", NULL);
		b = *(*p)++;
		v |= (uint32_t)(b & 0x7F) << shift;
		if (!(b & 0x80))
			return v;
		shift += 7;
	}
}

/* a recipe's groups from one source: the pool's frames (from stocktags.shared), or 16 KB slices of a tag part */
static void groups_rebuild(struct map const *m, uint8_t const **p, uint8_t const *end, uint32_t *words, uint8_t const *part,
	size_t part_bytes)
{
	uint32_t groups = leb(p, end), g, frame = 0, pool_bytes = part ? (uint32_t)part_bytes : u32(stocktags + 0x0C);
	uint32_t frames = (pool_bytes + FRAME - 1) / FRAME;
	uint8_t unpacked[FRAME];

	for (g = 0; g < groups; g++)
	{
		uint32_t count, k, size, dest = 0;

		frame = g ? frame + leb(p, end) : leb(p, end);
		if (frame >= frames)
			fail("a recipe names a frame its source hasn't", NULL);
		size = pool_bytes - frame * FRAME < FRAME ? pool_bytes - frame * FRAME : FRAME;
		if (part)
			memcpy(unpacked, part + (size_t)frame * FRAME, size);
		else
		{
			uint32_t at = u32(stocktags + HEADER + frame * 8), stored = u32(stocktags + HEADER + frame * 8 + 4);

			if (stored == size)
				memcpy(unpacked, stocktags + at, size);
			else
			{
				uLongf out = size;

				if (uncompress(unpacked, &out, stocktags + at, stored) != Z_OK || out != size)
					fail("a damaged frame", NULL);
			}
		}
		count = leb(p, end);
		for (k = 0; k < count; k++)
		{
			uint32_t offset, length;

			dest += leb(p, end);
			offset = leb(p, end);
			length = leb(p, end);
			if (!length || (offset + length) * 4 > size || (dest + (uint64_t)length) * 4 > m->tag_size)
				fail("a run outside its frame or the tag data", NULL);
			memcpy(words + dest, unpacked + offset * 4, length * 4);
			dest += length;
		}
	}
}

static void map_rebuild(struct map const *m, uint32_t *words, uint8_t const *part, size_t part_bytes)
{
	uint8_t const *p = m->recipe, *end = m->recipe + m->recipe_size;

	groups_rebuild(m, &p, end, words, NULL, 0);
	if (!part)
	{
		if (leb(&p, end) != 0 || p != end)
			fail("a stock map's recipe with pack groups, or bytes after its end", NULL);
		return;
	}
	groups_rebuild(m, &p, end, words, part, part_bytes);
	if (p != end)
		fail("a recipe with bytes after its end", NULL);
}

/* ---------- the test maps: each retail map with its recipe, packed as the disc's */

static void map_write_test(struct map const *m, int k, char const *dir)
{
	char path[1024];
	uint32_t recipe_offset = (m->file_length + 2047) & ~2047u, length = (recipe_offset + (uint32_t)m->recipe_size + 2047) & ~2047u;
	uint8_t *image = calloc(1, length), *r;
	uLongf packed_size = compressBound(length - HEADER);
	uint8_t *packed = calloc(1, HEADER + packed_size + 2048);

	memcpy(image, m->image, m->file_length);
	memset(image + m->tag_offset, 0, m->tag_size);          /* (every word of a stock map's is the pool's) */
	memcpy(image + recipe_offset, m->recipe, m->recipe_size);
	put32(image + 0x08, length);
	r = image + 0x700;
	memset(r, 0, 0x7C);
	put32(r + 0x00, 0x73687264u);                           /* 'shrd' */
	put32(r + 0x04, 3);
	put32(r + 0x30, m->file_length);                        /* (its identity: the map as it was) */
	put32(r + 0x34, u32(m->image + 0x64));
	put32(r + 0x68, recipe_offset);
	put32(r + 0x6C, (uint32_t)m->recipe_size);
	put32(r + 0x70, u32(stocktags + 0x10));
	put32(r + 0x74, u32(stocktags + 0x0C));
	put32(r + 0x78, m->tag_crc);
	memcpy(packed, image, HEADER);
	if (compress2(packed + HEADER, &packed_size, image + HEADER, length - HEADER, 1) != Z_OK)
		fail("zlib", NULL);
	snprintf(path, sizeof(path), "%s/maps/%s.map", dir, map_names[k]);
	/* (the file a whole number of 2048-byte sectors, as the disc's: the game copies maps a sector at a time) */
	write_file(path, packed, (HEADER + packed_size + 2047) & ~(size_t)2047);
	free(image), free(packed);
}

/* ---------- --pack: a pack's pass against the pool and its own maps */

/* the pool from a stocktags.shared (its frames unpacked), and its windows */
static void pool_load(char const *path)
{
	size_t size, f, frames, pool_bytes;

	stocktags = read_file(path, &size);
	stocktags_size = size;
	if (size < HEADER || memcmp(stocktags, "OXST", 4) || u32(stocktags + 4) != 1 || u32(stocktags + 0x14) != FRAME ||
		u32(stocktags + 0x1C) != HEADER)
		fail("not a stocktags.shared", path);
	pool_bytes = u32(stocktags + 0x0C);
	frames = u32(stocktags + 0x18);
	if ((pool_bytes & 3) || frames != (pool_bytes + FRAME - 1) / FRAME || HEADER + frames * 8 > size)
		fail("a damaged stocktags.shared", path);
	pool = malloc(pool_bytes + 4);
	pool_count = pool_room = pool_bytes / 4;
	for (f = 0; f < frames; f++)
	{
		uint32_t at = u32(stocktags + HEADER + f * 8), stored = u32(stocktags + HEADER + f * 8 + 4);
		size_t frame_size = pool_bytes - f * FRAME < FRAME ? pool_bytes - f * FRAME : FRAME;
		uLongf out = frame_size;

		if ((size_t)at + stored > size || stored > frame_size || !stored)
			fail("a damaged stocktags.shared (its frame table)", path);
		if (stored == frame_size)
			memcpy((uint8_t *)pool + f * FRAME, stocktags + at, frame_size);
		else if (uncompress((uint8_t *)pool + f * FRAME, &out, stocktags + at, stored) != Z_OK || out != frame_size)
			fail("a damaged stocktags.shared (a frame)", path);
	}
	if (crc32(0, (uint8_t const *)pool, (uInt)pool_bytes) != u32(stocktags + 0x10))
		fail("stocktags.shared's pool isn't its CRC32", path);
	pool_windows.slots = calloc(TABLE_SIZE, 4);
	for (f = 0; f + WINDOW <= pool_count; f++)
		windows_add(&pool_windows, pool, f);
}

/* a shared or map file as the game sees it: header, then the rest unpacked */
static uint8_t *cache_file_read_path(char const *path, uint32_t *file_length)
{
	size_t size;
	uint8_t *raw = read_file(path, &size), *image;

	if (size < HEADER || u32(raw) != 0x68656164u || u32(raw + 4) != 5)
		fail("not an Xbox cache file", path);
	*file_length = u32(raw + 8);
	image = calloc(1, *file_length);
	memcpy(image, raw, HEADER);
	if (size > HEADER && raw[HEADER] == 0x78)
	{
		uLongf out = *file_length - HEADER;

		if (uncompress(image + HEADER, &out, raw + HEADER, size - HEADER) != Z_OK || out != *file_length - HEADER)
			fail("can't unpack", path);
	}
	else
		memcpy(image + HEADER, raw + HEADER, (size < *file_length ? size : *file_length) - HEADER);
	free(raw);
	return image;
}

static void write_cache_file(char const *path, uint8_t const *image, uint32_t length, int level)
{
	uLongf packed_size = compressBound(length - HEADER);
	uint8_t *packed = calloc(1, HEADER + packed_size + 2048);

	memcpy(packed, image, HEADER);
	if (compress2(packed + HEADER, &packed_size, image + HEADER, length - HEADER, level) != Z_OK)
		fail("zlib", NULL);
	write_file(path, packed, (HEADER + packed_size + 2047) & ~(size_t)2047);
	free(packed);
}

static uint32_t *candidates;
static size_t candidate_count, candidate_room;
static struct windows candidate_windows = { NULL, TABLE_BITS };

static void candidate_append(uint32_t word)
{
	if (candidate_count == candidate_room)
	{
		candidate_room = candidate_room ? candidate_room * 2 : 1 << 20;
		candidates = realloc(candidates, candidate_room * 4);
	}
	candidates[candidate_count++] = word;
	if (candidate_count >= WINDOW)
		windows_add(&candidate_windows, candidates, candidate_count - WINDOW);
}

/* how far w matches base from at (at most left words, and to base's end) */
static size_t match_length(uint32_t const *base, size_t base_count, long at, uint32_t const *w, size_t left)
{
	size_t n = 0;

	if (at >= 0)
		while (n < left && (size_t)at + n < base_count && base[at + n] == w[n])
			n++;
	return n;
}

/* the pack pass's step 2 for one map: each word, the longer of the pool's and the candidates' earliest matches (a tie
the pool's); 16 words or more a run, else the word is the map's own. Its own words then become candidates */
static void pack_match(struct map *m)
{
	uint32_t const *w = (uint32_t const *)(m->image + m->tag_offset);
	size_t n = m->tag_size / 4, i = 0, k;

	m->runs = malloc((n + 1) * sizeof(struct run));
	m->candidate_runs = malloc((n + 1) * sizeof(struct run));
	m->own = malloc((n + 1) * 4);
	m->run_count = m->candidate_run_count = m->own_count = 0;
	while (i < n)
	{
		long from_pool = -1, from_candidates = -1;
		size_t in_pool = 0, in_candidates = 0;

		if (i + WINDOW <= n)
		{
			from_pool = windows_find(&pool_windows, pool, w + i);
			in_pool = match_length(pool, pool_count, from_pool, w + i, n - i);
			from_candidates = windows_find(&candidate_windows, candidates, w + i);
			in_candidates = match_length(candidates, candidate_count, from_candidates, w + i, n - i);
		}
		if (in_pool >= MINIMUM_RUN && in_pool >= in_candidates)
		{
			add_run(m, i, (size_t)from_pool, in_pool);
			i += in_pool;
		}
		else if (in_candidates >= MINIMUM_RUN)
		{
			struct run *r = &m->candidate_runs[m->candidate_run_count++];

			r->dest = (uint32_t)i, r->source = (uint32_t)from_candidates, r->length = (uint32_t)in_candidates;
			i += in_candidates;
		}
		else
			m->own[m->own_count++] = (uint32_t)i++;
	}
	m->own_first = candidate_count;
	for (k = 0; k < m->own_count; k++)
		candidate_append(w[m->own[k]]);
}

static int identity_order(void const *a, void const *b)
{
	struct map const *x = a, *y = b;
	int k;

	for (k = 0; k < 3; k++)
		if (x->identity[k] != y->identity[k])
			return x->identity[k] < y->identity[k] ? -1 : 1;
	return 0;
}

static int pack_main(int argc, char **argv)
{
	char const *stocktags_path = argv[2], *pack_path = argv[3], *name = argv[4], *out_dir = argv[5];
	int count = argc - 6, k;
	struct map *pm;
	uint8_t *pack, *used, *part_bytes_image;
	uint32_t pack_length, list_length, part_at, new_length, checksum, *part_index, *part;
	size_t part_count = 0, c, before = 0, after = 0, own_kept = 0, tags_all = 0;
	char pack_name[64], path[1024];

	if (argc < 7 || strlen(name) > 21)
		fail("use: tagpool --pack STOCKTAGS PACK_SHARED NAME OUT_DIR MAP... (NAME at most 21 characters)", NULL);
	pool_load(stocktags_path);
	pack = cache_file_read_path(pack_path, &pack_length);
	list_length = u32(pack + 0x18);
	if (u32(pack + 0x10) || u32(pack + 0x14) || u32(pack + 0x68) || u32(pack + 0x6C) || (pack_length & 2047) ||
		HEADER + list_length > pack_length)
		fail("not a pack's shared file without a tag part", pack_path);
	{
		size_t size;
		uint8_t *raw = read_file(pack_path, &size);

		before += size;
		free(raw);
	}
	pm = calloc((size_t)count, sizeof(struct map));
	for (k = 0; k < count; k++)
	{
		uint8_t const *r;
		size_t size;
		uint8_t *raw = read_file(argv[6 + k], &size);

		before += size;
		free(raw);
		pm[k].path = argv[6 + k];
		map_read_path(&pm[k], argv[6 + k]);
		r = pm[k].image + 0x700;
		if (u32(r) != 0x73687264u || u32(r + 4) != 2 || u32(r + 0x28) != u32(pack + 0x64))
			fail("not a map drawing on that pack's shared file (reference version 2, its checksum)", argv[6 + k]);
		pm[k].identity[0] = u32(r + 0x30), pm[k].identity[1] = u32(r + 0x60), pm[k].identity[2] = u32(r + 0x64);
	}
	/* 1. in the order of their identities */
	qsort(pm, (size_t)count, sizeof(struct map), identity_order);
	candidate_windows.slots = calloc(TABLE_SIZE, 4);
	for (k = 0; k < count; k++)
		pack_match(&pm[k]);
	/* 3. the tag part: every candidate some run uses, in candidate order */
	used = calloc(candidate_count + 1, 1);
	for (k = 0; k < count; k++)
		for (c = 0; c < pm[k].candidate_run_count; c++)
			memset(used + pm[k].candidate_runs[c].source, 1, pm[k].candidate_runs[c].length);
	part_index = malloc((candidate_count + 1) * 4);
	part = malloc((candidate_count + 1) * 4);
	for (c = 0; c < candidate_count; c++)
		if (used[c])
			part_index[c] = (uint32_t)part_count, part[part_count++] = candidates[c];
	/* each map's runs from the tag part, in the order of dest: its runs into the candidates, and its own words that went
	into the tag part */
	for (k = 0; k < count; k++)
	{
		struct map *m = &pm[k];
		size_t a = 0, b = 0;

		m->pack_runs = malloc((m->tag_size / 4 + 1) * sizeof(struct run));
		m->pack_run_count = 0;
		while (a < m->candidate_run_count || b < m->own_count)
		{
			if (b == m->own_count || (a < m->candidate_run_count && m->candidate_runs[a].dest < m->own[b]))
			{
				struct run const *r = &m->candidate_runs[a++];

				add_run_to(m->pack_runs, &m->pack_run_count, r->dest, part_index[r->source], r->length);
			}
			else
			{
				size_t at = m->own_first + b;

				if (used[at])
					add_run_to(m->pack_runs, &m->pack_run_count, m->own[b], part_index[at], 1);
				else
					own_kept++;
				b++;
			}
		}
		map_recipe(m);
		m->tag_crc = (uint32_t)crc32(0, m->image + m->tag_offset, m->tag_size);
		tags_all += m->tag_size;
	}
	/* the pack's file: its tag part after its last block, at a 2048-byte boundary */
	part_at = (pack_length + 2047) & ~2047u;
	new_length = (uint32_t)((part_at + part_count * 4 + 2047) & ~(size_t)2047);
	part_bytes_image = calloc(1, new_length);
	memcpy(part_bytes_image, pack, pack_length);
	memcpy(part_bytes_image + part_at, part, part_count * 4);
	put32(part_bytes_image + 0x08, new_length);
	put32(part_bytes_image + 0x68, part_at);
	put32(part_bytes_image + 0x6C, (uint32_t)(part_count * 4));
	checksum = (uint32_t)crc32(0, part_bytes_image + HEADER + list_length, new_length - HEADER - list_length);
	put32(part_bytes_image + 0x64, checksum);
	snprintf(pack_name, sizeof(pack_name), "%s-%08x", name, checksum);
	memset(part_bytes_image + 0x20, 0, 32);
	memcpy(part_bytes_image + 0x20, pack_name, strlen(pack_name));
	/* each map: its covered words zeros, its recipe after its data, its reference version 3 */
	for (k = 0; k < count; k++)
	{
		struct map *m = &pm[k];
		uint32_t recipe_offset = (m->file_length + 2047) & ~2047u;
		uint32_t length = (recipe_offset + (uint32_t)m->recipe_size + 2047) & ~2047u;
		uint8_t *image = calloc(1, length), *r, *list;
		uint32_t *words = calloc(1, m->tag_size + 4);
		char const *slash = strrchr(m->path, '/'), *folder_end = slash, *folder = slash;
		size_t j;

		memcpy(image, m->image, m->file_length);
		for (c = 0; c < m->run_count; c++)
			memset(image + m->tag_offset + m->runs[c].dest * 4, 0, m->runs[c].length * 4);
		for (c = 0; c < m->pack_run_count; c++)
			memset(image + m->tag_offset + m->pack_runs[c].dest * 4, 0, m->pack_runs[c].length * 4);
		memcpy(image + recipe_offset, m->recipe, m->recipe_size);
		put32(image + 0x08, length);
		r = image + 0x700;
		put32(r + 0x04, 3);
		memset(r + 0x08, 0, 32);
		memcpy(r + 0x08, pack_name, strlen(pack_name));
		put32(r + 0x28, checksum);
		put32(r + 0x2C, new_length);
		put32(r + 0x68, recipe_offset);
		put32(r + 0x6C, (uint32_t)m->recipe_size);
		put32(r + 0x70, u32(stocktags + 0x10));
		put32(r + 0x74, u32(stocktags + 0x0C));
		put32(r + 0x78, m->tag_crc);
		/* (the check: rebuilt from what's written, over the map's own words) */
		memcpy(words, image + m->tag_offset, m->tag_size);
		map_rebuild(m, words, part_bytes_image + part_at, part_count * 4);
		if (memcmp(words, m->image + m->tag_offset, m->tag_size))
			fail("a map's tag data didn't rebuild the same", m->path);
		/* (its list entry: its small map's length and CRC32) */
		list = part_bytes_image + HEADER;
		if (list_length && !memcmp(list, "OXPL", 4))
			for (j = 0; j < u32(list + 8); j++)
				if (u32(list + 16 + j * 20) == m->identity[0] && u32(list + 20 + j * 20) == m->identity[1] &&
					u32(list + 24 + j * 20) == m->identity[2])
				{
					put32(list + 28 + j * 20, length);
					put32(list + 32 + j * 20, (uint32_t)crc32(0, image, length));
				}
		if (!slash)
			fail("a map's path needs its folder", m->path);
		for (folder = slash; folder > m->path && folder[-1] != '/'; folder--)
			;
		snprintf(path, sizeof(path), "%s/%.*s", out_dir, (int)(folder_end - folder), folder);
		{
			char command[1200];

			snprintf(command, sizeof(command), "mkdir -p '%s'", path);
			if (system(command))
				fail("can't make", path);
		}
		snprintf(path, sizeof(path), "%s/%.*s%s", out_dir, (int)(folder_end - folder), folder, slash);
		write_cache_file(path, image, length, 9);
		{
			size_t size;
			uint8_t *raw = read_file(path, &size);

			after += size;
			free(raw);
		}
		for (j = 0, c = 0; j < m->own_count; j++)
			c += !used[m->own_first + j];
		printf("%-40s tag data %8u: %7zu stock runs, %6zu pack runs, %6zu own words kept, recipe %7zu, CRC32 %08x\n",
			m->path, m->tag_size, m->run_count, m->pack_run_count, c, m->recipe_size, m->tag_crc);
		free(image), free(words);
	}
	snprintf(path, sizeof(path), "%s/%s.shared", out_dir, pack_name);
	write_cache_file(path, part_bytes_image, new_length, 9);
	{
		size_t size;
		uint8_t *raw = read_file(path, &size);

		after += size;
		free(raw);
	}
	printf("\n%s.shared: tag part %zu bytes at %08x (%zu candidate words, %zu used); its own words kept in the maps: %zu "
		"bytes of %zu\n%d maps and the pack's file: %zu -> %zu bytes, all rebuilt the same\n", pack_name, part_count * 4,
		part_at, candidate_count, part_count, own_kept * 4, tags_all, count, before, after);
	return 0;
}

int main(int argc, char **argv)
{
	char path[1024];
	int k, test_maps = argc > 3 && !strcmp(argv[3], "--maps");
	size_t total_tags = 0, total_runs = 0, total_recipes = 0, bundle_size, at;
	uint8_t *bundle;

	if (argc < 3)
		fail("use: tagpool RETAIL_MAPS_DIR OUT_DIR [--maps], or tagpool --pack STOCKTAGS PACK_SHARED NAME OUT_DIR MAP...",
			NULL);
	if (argc > 1 && !strcmp(argv[1], "--pack"))
		return pack_main(argc, argv);
	pool_windows.slots = calloc(TABLE_SIZE, 4);
	for (k = 0; k < MAP_COUNT; k++)
	{
		map_read(argv[1], k);
		map_match(&maps[k]);
		map_recipe(&maps[k]);
		total_tags += maps[k].tag_size, total_runs += maps[k].run_count, total_recipes += maps[k].recipe_size;
	}
	free(pool_windows.slots);
	stocktags_make();
	snprintf(path, sizeof(path), "%s/stocktags.shared", argv[2]);
	write_file(path, stocktags, stocktags_size);

	/* the check, and each map's rebuilt CRC */
	for (k = 0; k < MAP_COUNT; k++)
	{
		struct map *m = &maps[k];
		uint32_t *words = calloc(1, m->tag_size);

		map_rebuild(m, words, NULL, 0);
		if (memcmp(words, m->image + m->tag_offset, m->tag_size))
			fail("a map's tag data didn't rebuild the same", map_names[k]);
		m->tag_crc = (uint32_t)crc32(0, (uint8_t const *)words, m->tag_size);
		free(words);
		printf("%-15s tag data %8u bytes: %7zu runs, recipe %7zu bytes, rebuilt CRC32 %08x: same as the disc's\n",
			map_names[k], m->tag_size, m->run_count, m->recipe_size, m->tag_crc);
	}

	/* stock_recipes.bin: OXSR, version 1, map count, the pool's size and CRC32, then each map's name (16 bytes), tag
	data size, rebuilt CRC32, recipe offset and size in this file; then the recipes */
	bundle_size = 0x14 + MAP_COUNT * 32 + total_recipes;
	bundle = calloc(1, bundle_size);
	memcpy(bundle, "OXSR", 4);
	put32(bundle + 0x04, 1);
	put32(bundle + 0x08, MAP_COUNT);
	put32(bundle + 0x0C, u32(stocktags + 0x0C));
	put32(bundle + 0x10, u32(stocktags + 0x10));
	at = 0x14 + MAP_COUNT * 32;
	for (k = 0; k < MAP_COUNT; k++)
	{
		uint8_t *e = bundle + 0x14 + k * 32;

		strncpy((char *)e, map_names[k], 15);
		put32(e + 16, maps[k].tag_size);
		put32(e + 20, maps[k].tag_crc);
		put32(e + 24, (uint32_t)at);
		put32(e + 28, (uint32_t)maps[k].recipe_size);
		memcpy(bundle + at, maps[k].recipe, maps[k].recipe_size);
		at += maps[k].recipe_size;
	}
	snprintf(path, sizeof(path), "%s/stock_recipes.bin", argv[2]);
	write_file(path, bundle, bundle_size);
	if (test_maps)
		for (k = 0; k < MAP_COUNT; k++)
			map_write_test(&maps[k], k, argv[2]);
	{
		uLongf z = compressBound(bundle_size);
		uint8_t *zb = malloc(z);

		compress2(zb, &z, bundle, bundle_size, 9);
		printf("\npool: %zu bytes, CRC32 %08x; stocktags.shared %zu bytes (%u frames)\n", pool_count * 4,
			u32(stocktags + 0x10), stocktags_size, u32(stocktags + 0x18));
		printf("23 maps: tag data %zu bytes, %zu runs, recipes %zu bytes (stock_recipes.bin %zu, %lu packed)\n",
			total_tags, total_runs, total_recipes, bundle_size, (unsigned long)z);
		free(zb);
	}
	return 0;
}
