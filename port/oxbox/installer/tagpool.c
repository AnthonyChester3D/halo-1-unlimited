/* tagpool.c: the stock tag pool (docs/shared_maps.md, "Shared tag data (format 4)"), on a PC.

	cc -O2 -o tagpool tagpool.c -lz
	tagpool RETAIL_MAPS_DIR OUT_DIR [--maps]      (about 4 GB of memory with --maps: the maps unpacked)

From the 23 retail maps (Xbox cache files, as on the disc) it makes, in OUT_DIR:
- stocktags.shared: the pool (every word of the 23 maps' tag data once), in 16 KB frames, each zlib or raw;
- stock_recipes.bin: each map's recipe and its rebuilt tag data's size and CRC32 (what the installer will carry: its
  runs that copy from the pool's end, as it then is, are how the pool is made, so the installer needs nothing else);
- with --maps, maps/<name>.map: each retail map with a recipe (its tag data's words zeros, the recipe after its data,
  its reference version 3), packed as the disc's are: for testing the game's rebuild.
Then it checks every map: its tag data rebuilt from the written stocktags.shared and its recipe, word for word against
the retail map's, and prints the sizes. Nothing it writes may be distributed: the pool is Microsoft's tag data. */

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
};

static struct map maps[MAP_COUNT];
static uint32_t *pool;
static size_t pool_count, pool_room;
static uint32_t *table;           /* each distinct 8-word window of the pool: its earliest position + 1 (0: none) */

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
static void map_read(char const *dir, int k)
{
	char path[1024];
	size_t size;
	uint8_t *raw;
	struct map *m = &maps[k];

	snprintf(path, sizeof(path), "%s/%s.map", dir, map_names[k]);
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

/* ---------- the matcher (the pool's rule) */

static uint32_t window_slot(uint32_t const *w)
{
	uint64_t h = 0x9E3779B97F4A7C15ull;
	int k;

	for (k = 0; k < WINDOW; k++)
		h = (h ^ w[k]) * 0x100000001B3ull, h ^= h >> 29;
	return (uint32_t)(h >> (64 - TABLE_BITS));
}

static void table_add(size_t position)
{
	uint32_t slot = window_slot(pool + position);

	for (;; slot = (slot + 1) & (TABLE_SIZE - 1))
	{
		uint32_t at = table[slot];

		if (!at)
		{
			table[slot] = (uint32_t)position + 1;
			return;
		}
		if (!memcmp(pool + at - 1, pool + position, WINDOW * 4))
			return;                 /* (the window's earliest place stays) */
	}
}

static long table_find(uint32_t const *w)
{
	uint32_t slot = window_slot(w);

	for (;; slot = (slot + 1) & (TABLE_SIZE - 1))
	{
		uint32_t at = table[slot];

		if (!at)
			return -1;
		if (!memcmp(pool + at - 1, w, WINDOW * 4))
			return (long)at - 1;
	}
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
		table_add(pool_count - WINDOW);
}

/* a range of the map's words from the pool, split at frames; one continuing the last run (in both) joins it */
static void add_run(struct map *m, size_t dest, size_t source, size_t length)
{
	while (length)
	{
		size_t room = FRAME_WORDS - source % FRAME_WORDS, piece = length < room ? length : room;
		struct run *last = m->run_count ? &m->runs[m->run_count - 1] : NULL;

		if (last && last->dest + last->length == dest && last->source + last->length == source && source % FRAME_WORDS)
			last->length += (uint32_t)piece;
		else
		{
			m->runs[m->run_count].dest = (uint32_t)dest;
			m->runs[m->run_count].source = (uint32_t)source;
			m->runs[m->run_count].length = (uint32_t)piece;
			m->run_count++;
		}
		dest += piece, source += piece, length -= piece;
	}
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

static void map_recipe(struct map *m)
{
	struct run *r = malloc(m->run_count * sizeof(struct run));
	size_t k = 0, groups = 0, g;
	uint32_t last_frame = 0;

	memcpy(r, m->runs, m->run_count * sizeof(struct run));
	qsort(r, m->run_count, sizeof(struct run), run_order);
	for (g = 0; g < m->run_count; g++)
		groups += !g || r[g].source / FRAME_WORDS != r[g - 1].source / FRAME_WORDS;
	out_count = 0;
	out_leb((uint32_t)groups);
	for (g = 0; g < groups; g++)
	{
		uint32_t frame = r[k].source / FRAME_WORDS, end = 0;
		size_t first = k, count;

		while (k < m->run_count && r[k].source / FRAME_WORDS == frame)
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
	out_leb(0);                     /* (no pack groups: a stock map) */
	m->recipe = malloc(out_count);
	memcpy(m->recipe, out_bytes, out_count);
	m->recipe_size = out_count;
	free(r);
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

static void map_rebuild(struct map const *m, uint32_t *words)
{
	uint8_t const *p = m->recipe, *end = m->recipe + m->recipe_size;
	uint32_t groups = leb(&p, end), g, frame = 0, frames = u32(stocktags + 0x18), pool_bytes = u32(stocktags + 0x0C);
	uint8_t unpacked[FRAME];

	for (g = 0; g < groups; g++)
	{
		uint32_t count, k, at, stored, size, dest = 0;

		frame = g ? frame + leb(&p, end) : leb(&p, end);
		if (frame >= frames)
			fail("a recipe names a frame the pool hasn't", NULL);
		at = u32(stocktags + HEADER + frame * 8);
		stored = u32(stocktags + HEADER + frame * 8 + 4);
		size = pool_bytes - frame * FRAME < FRAME ? pool_bytes - frame * FRAME : FRAME;
		if (stored == size)
			memcpy(unpacked, stocktags + at, size);
		else
		{
			uLongf out = size;

			if (uncompress(unpacked, &out, stocktags + at, stored) != Z_OK || out != size)
				fail("a damaged frame", NULL);
		}
		count = leb(&p, end);
		for (k = 0; k < count; k++)
		{
			uint32_t offset, length;

			dest += leb(&p, end);
			offset = leb(&p, end);
			length = leb(&p, end);
			if (!length || (offset + length) * 4 > size || (dest + (uint64_t)length) * 4 > m->tag_size)
				fail("a run outside its frame or the tag data", NULL);
			memcpy(words + dest, unpacked + offset * 4, length * 4);
			dest += length;
		}
	}
	if (leb(&p, end) != 0 || p != end)
		fail("a stock map's recipe with pack groups, or bytes after its end", NULL);
}

/* ---------- the test maps: each retail map with its recipe, packed as the disc's */

static void map_write_test(struct map const *m, int k, char const *dir)
{
	char path[1024];
	uint32_t recipe_offset = (m->file_length + 2047) & ~2047u, length = (recipe_offset + (uint32_t)m->recipe_size + 2047) & ~2047u;
	uint8_t *image = calloc(1, length), *r;
	uLongf packed_size = compressBound(length - HEADER);
	uint8_t *packed = malloc(HEADER + packed_size);

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
	write_file(path, packed, HEADER + packed_size);
	free(image), free(packed);
}

int main(int argc, char **argv)
{
	char path[1024];
	int k, test_maps = argc > 3 && !strcmp(argv[3], "--maps");
	size_t total_tags = 0, total_runs = 0, total_recipes = 0, bundle_size, at;
	uint8_t *bundle;

	if (argc < 3)
		fail("use: tagpool RETAIL_MAPS_DIR OUT_DIR [--maps]", NULL);
	table = calloc(TABLE_SIZE, 4);
	for (k = 0; k < MAP_COUNT; k++)
	{
		map_read(argv[1], k);
		map_match(&maps[k]);
		map_recipe(&maps[k]);
		total_tags += maps[k].tag_size, total_runs += maps[k].run_count, total_recipes += maps[k].recipe_size;
	}
	free(table);
	stocktags_make();
	snprintf(path, sizeof(path), "%s/stocktags.shared", argv[2]);
	write_file(path, stocktags, stocktags_size);

	/* the check, and each map's rebuilt CRC */
	for (k = 0; k < MAP_COUNT; k++)
	{
		struct map *m = &maps[k];
		uint32_t *words = calloc(1, m->tag_size);

		map_rebuild(m, words);
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
