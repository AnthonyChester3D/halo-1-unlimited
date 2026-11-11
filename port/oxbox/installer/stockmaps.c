/* stockmaps.c: Halo's 23 maps in a Halo folder (13 multiplayer, 10 campaign) swapped for small maps drawing on stock.shared,
and put back (docs/shared_maps.md, tools/shared_pass.py: its `stock` pass makes the same stock.shared and maps). Built for
the Xbox (the installer) and for the PC (stockmaps_test.c, with stockmaps_host.h).

Only the retail maps are swapped (each file's size and CRC: stockmaps_table.h, the same on the three NTSC discs), so every
console's stock.shared is the same file. It's stored unpacked, and the game reads it where it is. A multiplayer map's every
block goes in it; a level's blocks that three levels or more use do, and the rest stay in the level. The table says where
each goes, so nothing is hashed or counted. Each map's restore table (maps\stock.restore) says where each moved block came
from, and the original's size and CRC: retail maps recompress to their exact bytes (zlib level 6, memLevel 8).

The order, so a power cut can't lose a map, and as little room as can be is needed (a level unpacks to 280 MB and the console
has 64 MB: nothing holds a whole map or writes one out unpacked):
	1. stock.shared.oxtmp built, in one pass through the retail maps, each checked against its size and CRC, and the
	   restore table from their tags. If that fails, the leftovers go and nothing has changed. In the same pass each small
	   map is made in the maps folder (<map>.map.oxsmall) if its drive has room, else on a cache drive (X:, Y:, Z::
	   stockmaps_cache) with room for it, from the table's layout of the map's
	   moved data (its tags are at its end; the small map's part from their first sector on is made once they're read),
	   reusing the retail map's packed data where the table's runs say (struct reuse_run), and checked against the
	   table's length and CRC, unpacked and as a file; one that isn't is made in step 3 as before
	2. stock.shared.oxtmp read back (its CRC); stock.index.oxtmp (from the table: every console's is the same) and
	   stock.restore.oxtmp written and read back; stock.index and stock.shared put in place, then stock.restore
	3. then each map on its own: <map>.map.oxtmp, the small map, renamed from the maps folder or copied from the cache
	   drive (checked as it's read against the file written there), or else built and checked against the small map it must be (the table's length and CRC
	   unpacked: tools/shared_pass.py's, whose restore the lab proves exact), then read back from the disk against the
	   bytes written; renamed .oxnew, and swapped in (<map>.map to .oxold, .oxnew to <map>.map, .oxold
	   deleted). Each map's room comes back as it's done. If it stops here, for any reason, what's done stays small and the
	   rest stay retail: both play, X puts the small ones back, and the next swap finishes the job
Putting them back is the same: <map>.map.oxtmp, the retail map, is checked against the size and CRC before it's renamed
.oxnew and swapped in the same way. stock.shared, stock.index and stock.restore go last. */
#include "stockmaps.h"
#include "mapstream.h"
#include "sha256.h"
#include "text.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define STOCK_BIT 0xC0000000u
#define REFERENCE_OFFSET 0x700u
#define REFERENCE_SIGNATURE 0x73687264u /* 'shrd' */
#define MB (1024u * 1024u)
#define CHUNK (256u * 1024u)        /* a whole number of sectors: what's worked on at a time */
#define STAYS 0xFFFFFFFFu           /* a restore_block's stock: the block stays in the map */

struct first_copy
{
	uint32_t offset, size, at;
};

/* a run of the retail file's deflate blocks that the small map has unchanged (and the 32 KB before them, all a block can refer
back to): where they are unpacked, and in the file's bits. The small map takes them bit for bit, an empty stored block after
each (so what follows starts on a byte), and packs only the rest, from a fresh start after each run (gen_stock_table.py) */
struct reuse_run
{
	uint32_t from, to, bit_from, bit_to;
	uint32_t adler;                 /* the Adler-32 of its bytes unpacked (the small map's stream ends with all of its) */
};

/* the retail maps: multiplayer in the order of their file names, then the levels. Each file's size and CRC, and
unpacked, as tools/shared_pass.py measured them (gen_stock_table.py); its blocks; where they go */
struct retail_map
{
	char const *name;
	uint32_t raw_size, raw_crc, length, checksum;
	uint32_t unpacked_crc;          /* its CRC unpacked, header and all (the restore table's: the file's CRC and zlib's check say so) */
	int blocks;
	uint32_t const *places;         /* each block's place in stock.shared, in the map's tags' order (STAYS: it stays) */
	struct first_copy const *firsts;/* the blocks this map is the first to give stock.shared, by offset */
	int first_count;
	uint32_t small_length, small_crc; /* its small map unpacked, as tools/shared_pass.py makes it: what this one must be */
	uint32_t small_disk, small_file_crc; /* its small map's file as the one pass makes it (reusing runs): its size and CRC */
	struct reuse_run const *runs;   /* the runs of the retail file's deflate blocks the small map has as they are */
	int run_count;
};

/* maps\stock.index: this, then each block of stock.shared's in its order (the table has them: the same on every console) */
struct index_file
{
	char magic[4];                  /* "OXSI" */
	uint32_t version, count, stock_checksum, stock_length;
};
struct index_block
{
	unsigned char digest[32];       /* its bytes' SHA-256 */
	uint32_t at, size;              /* where it is in stock.shared (at % 512 is the remainder it matches), how big */
};
#include "stockmaps_table.h"
#define MOST_BLOCKS (MOST_MAP_BLOCKS + 512) /* (before the ones outside the map's data are left out) */
#define SMALL_LEVEL_ROOM (100u * MB) /* (a small level, under 95) */
#define RESTORE_VERSION 2            /* (1, v1.95's: 13 maps and stock.shared packed: installer 23 puts those back) */

void (*stockmaps_flush_drive)(char const *path);
void (*stockmaps_log)(char const *text);
char const *stockmaps_cache[STOCKMAPS_CACHES];

/* ---- the "time:" lines: each step, from when it began ---- */

static uint64_t step_began, whole_began;
static struct stream_times step_then, whole_then;

static void step_begin(void)
{
	step_began = stream_now();
	stream_times_get(&step_then);
}

static void step_end(char const *format, ...)
{
	char what[64], how[200], line[280];
	va_list list;
	if (!stockmaps_log)
		return;
	va_start(list, format);
	text_vprint(what, sizeof(what), format, list);
	va_end(list);
	stream_times_text(how, sizeof(how), step_began, &step_then);
	text_print(line, sizeof(line), "time: %s: %s", what, how);
	stockmaps_log(line);
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
	char how[200], line[280];
	if (!stockmaps_log)
		return;
	stream_times_text(how, sizeof(how), whole_began, &whole_then);
	text_print(line, sizeof(line), "time: %s, all of it: %s", what, how);
	stockmaps_log(line);
}

/* the restore table, one file: this, then each map's restore_map, its BSP fields, its blocks */
struct restore_file
{
	char magic[4];                  /* "OXSR" */
	uint32_t version, count, stock_checksum, stock_length;
	uint32_t size;                  /* the whole file's */
	uint32_t crc;                   /* of what follows this */
};
struct restore_map
{
	char name[32];
	uint32_t raw_size, raw_crc;     /* the original file's */
	uint32_t length, crc;           /* and unpacked, the whole of it */
	uint32_t bsps, blocks;
	unsigned char header[HEADER];   /* the original's */
};
struct restore_block
{
	uint32_t field, offset, size, stock; /* the original's field and block, and where the block is in stock.shared (STAYS) */
};

/* maps\stock.index: every block in stock.shared, for matching a pack's against it without reading the file (docs): this,
then each index_block */
/* ---- files ---- */

static unsigned char io_in[1024 * 1024], io_in2[1024 * 1024]; /* (1 MB reads, in turns: mapstream.c's io_in says why) */
static unsigned char const zeros[SECTOR];

static int exists(char const *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* (after what the disk thread has queued: a rename or a delete comes after every write made before it, as without it) */
static int delete_file(char const *path)
{
	disk_sync();
	return DeleteFileA(path);
}

static int move_file(char const *from, char const *to)
{
	disk_sync();
	return MoveFileA(from, to);
}

static HANDLE create(char const *path)
{
	delete_file(path);
	return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* (written to the disk before it's closed) */
static int finish(HANDLE file)
{
	int ok = !timed_flush(file);
	return file_close(file) && ok;
}

static void map_path(char *path, int size, char const *halo, char const *name, char const *extension)
{
	text_print(path, size, "%s\\maps\\%s.map%s", halo, name, extension);
}

static void maps_file(char *path, int size, char const *halo, char const *name)
{
	text_print(path, size, "%s\\maps\\%s", halo, name);
}

static void flush(char const *halo)
{
	if (stockmaps_flush_drive)
		stockmaps_flush_drive(halo);
}

/* a file's size and CRC, as it is on the disk; 0 when read */
static int file_crc(char const *path, uint32_t *size, uint32_t *crc)
{
	HANDLE file = open_read(path);
	struct ahead ahead;
	unsigned char *data;
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	*size = 0;
	*crc = crc_add(0, NULL, 0);
	ahead_open(&ahead, file, io_in, io_in2);
	while (!ahead_next(&ahead, &data, &got) && got)
	{
		*crc = crc_add(*crc, data, got);
		*size += got;
	}
	file_close(file);
	return 0;
}

/* ---- cache files: a header, the rest one zlib stream, read and written a piece at a time ---- */

/* zeros after a retail map's stream, up to its file's size */
static int retail_padding(struct writer const *w, uint32_t raw_size, HANDLE to, uint32_t *crc)
{
	uint32_t padding;
	if (w->written > raw_size - HEADER)
		return 1;
	*crc = w->crc;
	for (padding = raw_size - HEADER - w->written; padding;)
	{
		uint32_t part = padding > SECTOR ? SECTOR : padding;
		*crc = crc_add(*crc, zeros, part);
		if (to != INVALID_HANDLE_VALUE && write_all(to, zeros, part))
			return 1;
		padding -= part;
	}
	return 0;
}

static unsigned char chunk[CHUNK], chunk_before[CHUNK];

/* ---- the sectors wholly of moved data ---- */

static unsigned char covered[MOST_MAP_LENGTH / SECTOR + 2];
static uint32_t sectors_before[MOST_MAP_LENGTH / SECTOR + 2]; /* the covered sectors before each */
struct range
{
	uint32_t from, to, stock;
};
static struct range moved[MOST_BLOCKS], stay[MOST_BLOCKS];
static int moved_count, stay_count;

static int range_order(void const *a, void const *b)
{
	struct range const *x = a, *y = b;
	return x->from < y->from ? -1 : x->from > y->from;
}

/* covered[] from moved[] (in the order they start), and sectors_before[]; the number of sectors covered */
static uint32_t cover_moved(uint32_t length)
{
	uint32_t k, c, gone = 0, sectors = length / SECTOR + 1, from = 0, to = 0;
	memset(covered, 0, sectors + 1);
	for (k = 0; k <= (uint32_t)moved_count; k++)
	{
		if (k < (uint32_t)moved_count && k && moved[k].from <= to)
		{
			if (moved[k].to > to)
				to = moved[k].to;
			continue;
		}
		if (k)
		{
			for (c = (from + SECTOR - 1) / SECTOR; c < to / SECTOR; c++)
				covered[c] = 1;
		}
		if (k < (uint32_t)moved_count)
		{
			from = moved[k].from;
			to = moved[k].to;
		}
	}
	covered[0] = 0;
	for (c = 0; c <= sectors; c++)
	{
		sectors_before[c] = gone;
		if (c < sectors)
			gone += covered[c];
	}
	return gone;
}

/* a map's blocks sorted, the moved ones and those that stay, and covered[] from the moved; the number of sectors covered */
static uint32_t cover(struct restore_block const *blocks, uint32_t count, uint32_t length)
{
	uint32_t k;
	moved_count = stay_count = 0;
	for (k = 0; k < count; k++)
	{
		struct range *r = blocks[k].stock == STAYS ? &stay[stay_count++] : &moved[moved_count++];
		r->from = blocks[k].offset;
		r->to = blocks[k].offset + blocks[k].size;
		r->stock = blocks[k].stock;
	}
	qsort(moved, (size_t)moved_count, sizeof(moved[0]), range_order);
	qsort(stay, (size_t)stay_count, sizeof(stay[0]), range_order);
	return cover_moved(length);
}

/* the same from the table's layout (unpacked: stock_layout) for retail map k, before its tags are read: its moved ranges as
cover() runs them together, and those that stay where they overlap them. 0 when they're there, the sectors covered in *gone */
static int cover_baked(unsigned char const *layout, uint32_t layout_size, int k, uint32_t length, uint32_t *gone)
{
	uint32_t at = 0, moved_n = 0, stay_n = 0, i, end;
	int m;
	for (m = 0; m <= k; m++)
	{
		if (m)
			at += 8 + 8 * (moved_n + stay_n);
		if (at > layout_size || layout_size - at < 8)
			return 1;
		moved_n = u32(layout + at);
		stay_n = u32(layout + at + 4);
		if (moved_n > MOST_BLOCKS || stay_n > MOST_BLOCKS || (layout_size - at - 8) / 8 < moved_n + stay_n)
			return 1;
	}
	at += 8;
	moved_count = (int)moved_n;
	stay_count = (int)stay_n;
	for (i = 0, end = 0; i < moved_n + stay_n; i++, at += 8)
	{
		struct range *r = i < moved_n ? &moved[i] : &stay[i - moved_n];
		if (i == moved_n)
			end = 0;
		r->from = end + u32(layout + at);
		r->to = r->from + u32(layout + at + 4);
		r->stock = i < moved_n ? 0 : STAYS;
		if (r->from < end || r->to < r->from || r->to > length)
			return 1;
		end = r->to;
	}
	*gone = cover_moved(length);
	return 0;
}

/* an offset in the map, in the small map laid out without the covered sectors; 1 if it's in one */
static int moved_down(uint32_t offset, uint32_t *to)
{
	uint32_t sector = offset / SECTOR;
	if (covered[sector])
		return 1;
	*to = offset - sectors_before[sector] * SECTOR;
	return 0;
}

/* ---- the small map: laid out again without the moved data (as tools/shared_pass.py's rebuild) ---- */

struct field
{
	uint32_t at, value;
};
static struct field fields[MOST_BLOCKS + MOST_BSPS];

static int field_order(void const *a, void const *b)
{
	struct field const *x = a, *y = b;
	return x->at < y->at ? -1 : x->at > y->at;
}

/* a piece's fields set */
static void set_fields(int field_count, int *first_field, uint32_t at, uint32_t size)
{
	int i;
	while (*first_field < field_count && fields[*first_field].at + 4 <= at)
		(*first_field)++;
	for (i = *first_field; i < field_count && fields[i].at < at + size; i++)
	{
		uint32_t b;
		for (b = 0; b < 4; b++)
		{
			if (fields[i].at + b >= at && fields[i].at + b < at + size)
				chunk[fields[i].at + b - at] = (unsigned char)(fields[i].value >> (b * 8));
		}
	}
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

/* one small map being made, a piece of the original at a time (covered[] set first: cover() or cover_baked()) */
struct small
{
	struct writer w;
	HANDLE file;
	uint32_t length, gone;          /* the original's length unpacked, and the sectors left out of it */
	uint32_t out, crc;              /* what's gone into it so far, unpacked (header and all), and its CRC32 */
	int first_moved, first_stay, first_field, field_count;
	/* reusing the retail map's packed data (runs NULL: all of it packed, as before). The tap (small_tap) takes each run's
	bits from what the retail map's reader reads, a little ahead of the small map's bytes (one CHUNK), and they wait in
	pending[] until the small map reaches them; a run's end must have come before the bytes after it are packed */
	struct reuse_run const *runs;
	int run_count, run, tap_run;    /* the run the small map is in or comes to next; the one the tap is in or comes to */
	uint32_t at, adler;             /* where in the original the small map has come to (from small_piece); its Adler-32 */
	uint32_t pend_from, pend_to, acc;
	int acc_bits, broken;
	uint32_t run_ends[MOST_RUNS];   /* (where each run the tap has finished ends in pending[]) */
};

/* the runs' bits, waiting (malloc'd by convert(): MOST_PENDING, and what the tap can be ahead) */
#define PENDING_ROOM (MOST_PENDING + 2u * MB)
static unsigned char *pending;

/* pending[] moved down to its start, for room */
static void pend_shift(struct small *s)
{
	int i;
	if (!s->pend_from)
		return;
	memmove(pending, pending + s->pend_from, s->pend_to - s->pend_from);
	for (i = 0; i < s->tap_run; i++)
		s->run_ends[i] = s->run_ends[i] > s->pend_from ? s->run_ends[i] - s->pend_from : 0;
	s->pend_to -= s->pend_from;
	s->pend_from = 0;
}

/* room in pending[] for n more bytes; 0 when there is */
static int pend_room(struct small *s, uint32_t n)
{
	if (PENDING_ROOM - s->pend_to >= n)
		return 0;
	pend_shift(s);
	if (PENDING_ROOM - s->pend_to >= n)
		return 0;
	s->broken = 1;
	return 1;
}

/* bits bits from p, from its bit first on, into pending[] after what's there */
static void tap_bits(struct small *s, unsigned char const *p, uint32_t first, uint32_t bits)
{
	unsigned char *out;
	uint32_t acc = s->acc;
	int n = s->acc_bits;
	if (pend_room(s, bits / 8 + 2))
		return;
	out = pending + s->pend_to;
	if (first)
	{
		uint32_t take = 8 - first < bits ? 8 - first : bits;
		acc |= ((uint32_t)(*p++ >> first) & ((1u << take) - 1)) << n;
		n += (int)take;
		bits -= take;
		if (n >= 8)
		{
			*out++ = (unsigned char)acc;
			acc >>= 8;
			n -= 8;
		}
	}
	if (!n)
	{
		memcpy(out, p, bits / 8);
		out += bits / 8;
		p += bits / 8;
		bits %= 8;
	}
	for (; bits >= 8; bits -= 8)
	{
		acc |= (uint32_t)*p++ << n;
		*out++ = (unsigned char)acc;
		acc >>= 8;
	}
	if (bits)
	{
		acc |= ((uint32_t)*p & ((1u << bits) - 1)) << n;
		n += (int)bits;
		if (n >= 8)
		{
			*out++ = (unsigned char)acc;
			acc >>= 8;
			n -= 8;
		}
	}
	s->pend_to = (uint32_t)(out - pending);
	s->acc = acc;
	s->acc_bits = n;
}

/* the reader's tap: each run's bits as inflate takes them, and once a run's done, an empty stored block after it */
static void small_tap(void *context, uint32_t offset, unsigned char const *data, uint32_t size)
{
	struct small *s = context;
	uint32_t first = offset * 8u, end = (offset + size) * 8u;
	while (!s->broken && s->tap_run < s->run_count)
	{
		struct reuse_run const *r = &s->runs[s->tap_run];
		uint32_t from = r->bit_from > first ? r->bit_from : first, to = r->bit_to < end ? r->bit_to : end;
		if (r->bit_from >= end)
			return;
		if (from < to)
			tap_bits(s, data + (from / 8 - offset), from % 8, to - from);
		if (r->bit_to > end || s->broken || pend_room(s, 6))
			return;
		/* (the stored block: 3 bits, 0 (not the last) and 00 (stored), to the byte's end, then its length 0 and that's ~) */
		s->acc_bits += 3;
		while (s->acc_bits > 0)
		{
			pending[s->pend_to++] = (unsigned char)s->acc;
			s->acc >>= 8;
			s->acc_bits -= 8;
		}
		s->acc = 0;
		s->acc_bits = 0;
		pending[s->pend_to++] = 0;
		pending[s->pend_to++] = 0;
		pending[s->pend_to++] = 0xFF;
		pending[s->pend_to++] = 0xFF;
		s->run_ends[s->tap_run++] = s->pend_to;
	}
}

/* the runs' bits the small map has come to, out of pending[] into the file; 0 when they've gone */
static int small_drain(struct small *s)
{
	int in_run = s->run < s->run_count && s->at >= s->runs[s->run].from;
	uint32_t limit;
	if (s->tap_run < s->run)
		limit = s->pend_to;                                     /* (the tap's still in a run the small map is past) */
	else if (s->tap_run == s->run)
		limit = in_run ? s->pend_to : s->run ? s->run_ends[s->run - 1] : s->pend_from;
	else
		limit = in_run ? s->run_ends[s->run] : s->run ? s->run_ends[s->run - 1] : s->pend_from;
	if (limit > s->pend_from)
	{
		if (writer_emit(&s->w, pending + s->pend_from, limit - s->pend_from))
			return 1;
		s->pend_from = limit;
	}
	if (s->pend_from == s->pend_to)
		pend_shift(s);
	return 0;
}

/* the small map's bytes from the original's at `from` on: packed, or where a run is, its bits instead; 0 when they're in */
static int small_out(struct small *s, uint32_t from, unsigned char const *p, uint32_t n)
{
	if (!s->runs)
		return writer_put(&s->w, p, n);
	while (n)
	{
		struct reuse_run const *r = s->run < s->run_count ? &s->runs[s->run] : NULL;
		uint32_t m;
		if (s->broken)
			return 1;
		if (r && from >= r->from)
		{
			/* (in a run: the piece before it ends on a byte, and the run's bits go out as the tap has them) */
			if (from >= r->to || writer_cut(&s->w))
				return 1;
			m = r->to - from < n ? r->to - from : n;
			from += m;
			p += m;
			n -= m;
			s->at = from;
			if (from == r->to)
			{
				s->adler = adler32_combine(s->adler, r->adler, (z_off_t)(r->to - r->from));
				s->run++;
			}
			if (small_drain(s))
				return 1;
			continue;
		}
		/* (packed: every run before must be out whole first) */
		m = r && r->from - from < n ? r->from - from : n;
		if (s->tap_run < s->run || small_drain(s) || s->pend_from != (s->run ? s->run_ends[s->run - 1] : s->pend_from) ||
			writer_put(&s->w, p, m))
			return 1;
		s->adler = adler32(s->adler, p, m);
		from += m;
		p += m;
		n -= m;
		s->at = from;
	}
	return 0;
}

/* the fields that change (each block's: where it is in stock.shared, or where it's moved down to if it stays; each BSP's),
in the order they're in the map; 0 when they're set */
static int small_fields(struct small *s, struct restore_map const *map, struct map_bsp const *bsps,
	struct restore_block const *blocks)
{
	uint32_t k, to;
	s->field_count = 0;
	s->first_field = 0;
	for (k = 0; k < map->blocks; k++)
	{
		uint32_t value = STOCK_BIT | blocks[k].stock;
		if (blocks[k].stock == STAYS && moved_down(blocks[k].offset, &value))
			return 1;
		fields[s->field_count].at = blocks[k].field;
		fields[s->field_count++].value = value;
	}
	for (k = 0; k < map->bsps; k++)
	{
		if (moved_down(bsps[k].offset, &to))
			return 1;
		fields[s->field_count].at = bsps[k].field;
		fields[s->field_count++].value = to;
	}
	qsort(fields, (size_t)s->field_count, sizeof(fields[0]), field_order);
	return 0;
}

/* the small map's file begun at path, its header written: 0 when it's open (small_piece, then small_close or
small_abandon). map: the original's header and length; gone: the sectors covered */
static int small_open(struct small *s, struct restore_map const *map, uint32_t gone, int level, struct retail_map const *reuse,
	char const *path)
{
	unsigned char small_header[HEADER];
	uint32_t to;
	s->length = map->length;
	s->gone = gone;
	s->out = SECTOR;
	s->first_moved = s->first_stay = 0;
	s->runs = reuse && pending && reuse->run_count <= MOST_RUNS ? reuse->runs : NULL;
	s->run_count = s->runs ? reuse->run_count : 0;
	s->run = s->tap_run = s->acc_bits = s->broken = 0;
	s->at = HEADER;
	s->adler = adler32(0, NULL, 0);
	s->pend_from = s->pend_to = s->acc = 0;
	memcpy(small_header, map->header, HEADER);
	if (moved_down(u32(map->header + 0x10), &to))
		return 1;
	put32(small_header + 0x10, to);
	/* the reference (docs/shared_maps.md): no pack's file, the stock one's, and the map's identity kept */
	memset(small_header + REFERENCE_OFFSET, 0, 92);
	put32(small_header + REFERENCE_OFFSET, REFERENCE_SIGNATURE);
	put32(small_header + REFERENCE_OFFSET + 4, 2);
	put32(small_header + REFERENCE_OFFSET + 0x30, map->raw_size);
	put32(small_header + REFERENCE_OFFSET + 0x34, u32(map->header + 0x64));
	memcpy(small_header + REFERENCE_OFFSET + 0x38, "stock", 5);
	put32(small_header + REFERENCE_OFFSET + 0x58, STOCK_CHECKSUM);
	put32(small_header + REFERENCE_OFFSET + 0x5C, STOCK_LENGTH);
	put32(small_header + 8, map->length - gone * SECTOR);
	s->file = create(path);
	if (s->file == INVALID_HANDLE_VALUE)
		return 1;
	s->crc = crc_add(crc_add(0, NULL, 0), small_header, HEADER);
	if (write_all(s->file, small_header, HEADER) ||
		(s->runs ? writer_open_pieces(&s->w, level, s->file, s->crc) : writer_open(&s->w, level, s->file, s->crc)))
	{
		file_close(s->file);
		return 1;
	}
	return 0;
}

/* the original's piece at `at` (a whole sector in; size bytes, in chunk[]) gone into the small map: what moved is zeros (a
block that stays keeps its bytes, shared or not), the fields set, the covered sectors left out. 0 when it's in */
static int small_piece(struct small *s, uint32_t at, uint32_t size)
{
	static unsigned char saved[CHUNK / SECTOR]; /* (the piece's sectors kept in chunk_before before they were zeroed) */
	uint32_t c = at / SECTOR, sector, in_piece, in_block, n;
	int i, touched = 0;
	while (s->first_moved < moved_count && moved[s->first_moved].to <= at)
		s->first_moved++;
	while (s->first_stay < stay_count && stay[s->first_stay].to <= at)
		s->first_stay++;
	/* (only in the sectors the small map has: a covered one's left out, and most moved data is in those) */
	for (i = s->first_moved; i < moved_count && moved[i].from < at + size; i++)
	{
		uint32_t from, end;
		if (!overlap(moved[i].from, moved[i].to, at, size, &in_piece, &in_block, &n))
			continue;
		for (from = in_piece, end = in_piece + n; from < end;)
		{
			uint32_t k = from / SECTOR, stop = (k + 1) * SECTOR < end ? (k + 1) * SECTOR : end;
			if (!covered[c + k])
			{
				if (!touched)
					memset(saved, 0, sizeof(saved));
				touched = 1;
				if (!saved[k])
				{
					memcpy(chunk_before + k * SECTOR, chunk + k * SECTOR, size - k * SECTOR > SECTOR ? SECTOR : size - k * SECTOR);
					saved[k] = 1;
				}
				memset(chunk + from, 0, stop - from);
			}
			from = stop;
		}
	}
	if (touched)
	{
		for (i = s->first_stay; i < stay_count && stay[i].from < at + size; i++)
		{
			uint32_t from, end;
			if (!overlap(stay[i].from, stay[i].to, at, size, &in_piece, &in_block, &n))
				continue;
			for (from = in_piece, end = in_piece + n; from < end;)
			{
				uint32_t k = from / SECTOR, stop = (k + 1) * SECTOR < end ? (k + 1) * SECTOR : end;
				if (saved[k])
					memcpy(chunk + from, chunk_before + from, stop - from);
				from = stop;
			}
		}
	}
	set_fields(s->field_count, &s->first_field, at, size);
	for (sector = 0; sector * SECTOR < size; sector++)
	{
		uint32_t part = size - sector * SECTOR > SECTOR ? SECTOR : size - sector * SECTOR;
		if (covered[c + sector])
			continue;
		if (small_out(s, at + sector * SECTOR, chunk + sector * SECTOR, part))
			return 1;
		if (!s->runs) /* (reusing runs, the file's checked against the table's size and CRC instead) */
			s->crc = crc_add(s->crc, chunk + sector * SECTOR, part);
		s->out += part;
	}
	return 0;
}

/* a small map that won't be finished: its file closed (the caller deletes it) */
static void small_abandon(struct small *s)
{
	deflateEnd(&s->w.z);
	file_close(s->file);
}

/* the small map finished, padded to a whole sector (the game copies them unbuffered) and written to the disk: 0 when it's
whole, with what it unpacks to (its length and CRC32, header and all) and the file as it should be on the disk (its size and
CRC32) */
static int small_close(struct small *s, uint32_t *small_length, uint32_t *small_crc, uint32_t *file_size, uint32_t *file_crc)
{
	uint32_t tail;
	int failed;
	if (s->out != s->length - s->gone * SECTOR)
	{
		small_abandon(s);
		return 1;
	}
	if (s->runs && (s->broken || s->run != s->run_count || s->tap_run != s->run_count || small_drain(s) || s->pend_from != s->pend_to))
	{
		deflateEnd(&s->w.z);
		failed = 1;
	}
	else if (s->runs)
		failed = writer_end(&s->w, s->adler);
	else
		failed = writer_finish(&s->w);
	tail = (SECTOR - s->w.written % SECTOR) % SECTOR;
	if (!failed)
		failed = write_all(s->file, zeros, tail);
	*small_length = s->out;
	*small_crc = s->crc;
	*file_size = HEADER + s->w.written + tail;
	*file_crc = crc_add(s->w.crc, zeros, tail);
	if (!finish(s->file))
		failed = 1;
	return failed;
}

/* the small map written to path, from the retail map as it unpacks (checked against its size and CRC): 0 when written, with
what it unpacks to (its length and CRC32, header and all) and the file as it should be on the disk (its size and CRC32) */
static int write_small(char const *retail_path, struct retail_map const *retail, struct restore_map const *map,
	struct map_bsp const *bsps, struct restore_block const *blocks, int level, char const *path, uint32_t *small_length,
	uint32_t *small_crc, uint32_t *file_size, uint32_t *file_crc)
{
	unsigned char header[HEADER];
	struct reader r;
	struct small s;
	uint32_t c, raw_size, raw_crc, gone, sectors = map->length / SECTOR + (map->length % SECTOR ? 1 : 0);

	gone = cover(blocks, map->blocks, map->length);
	if (small_fields(&s, map, bsps, blocks) || reader_open(&r, retail_path, header))
		return 1;
	if (small_open(&s, map, gone, level, NULL, path))
	{
		reader_close(&r);
		return 1;
	}
	for (c = 1; c < sectors; c += CHUNK / SECTOR)
	{
		uint32_t at = c * SECTOR, size = map->length - at > CHUNK ? CHUNK : map->length - at;
		if (reader_pull(&r, chunk, size) || small_piece(&s, at, size))
			goto stop;
	}
	if (reader_pull(&r, NULL, 0) || reader_raw(&r, &raw_size, &raw_crc) || raw_size != retail->raw_size || raw_crc != retail->raw_crc)
		goto stop;
	reader_close(&r);
	return small_close(&s, small_length, small_crc, file_size, file_crc);
stop:
	small_abandon(&s);
	reader_close(&r);
	return 1;
}

/* ---- the original from its small map and stock.shared, a piece at a time ---- */


/* the original's unpacked bytes, every step undoing one of write_small's: checked against its CRC, and given to w if it's
there (the retail file's stream). small_path: the small map; stock: stock.shared */
static int make_original(char const *small_path, struct restore_map const *map, struct map_bsp const *bsps,
	struct restore_block const *blocks, HANDLE stock, struct writer *w)
{
	unsigned char header[HEADER];
	struct reader r;
	uint32_t k, c, gone, sectors, crc;
	int first_moved = 0, first_field = 0, field_count = 0, failed = 1;

	if (map->length > MOST_MAP_LENGTH || map->length < HEADER || map->bsps > MOST_BSPS)
		return 1;
	for (k = 0; k < map->blocks; k++)
	{
		if (blocks[k].offset < HEADER || blocks[k].size > map->length || blocks[k].offset > map->length - blocks[k].size ||
			blocks[k].field < HEADER || blocks[k].field > map->length - 4)
			return 1;
		fields[field_count].at = blocks[k].field;
		fields[field_count++].value = blocks[k].offset;
	}
	for (k = 0; k < map->bsps; k++)
	{
		if (bsps[k].field < HEADER || bsps[k].field > map->length - 4)
			return 1;
		fields[field_count].at = bsps[k].field;
		fields[field_count++].value = bsps[k].offset;
	}
	qsort(fields, (size_t)field_count, sizeof(fields[0]), field_order);
	gone = cover(blocks, map->blocks, map->length);
	if (reader_open(&r, small_path, header))
		return 1;
	if (u32(header + 8) != map->length - gone * SECTOR)
		goto done;
	sectors = map->length / SECTOR + (map->length % SECTOR ? 1 : 0);
	crc = crc_add(crc_add(0, NULL, 0), map->header, HEADER);
	for (c = 1; c < sectors; c += CHUNK / SECTOR)
	{
		uint32_t at = c * SECTOR, size = map->length - at > CHUNK ? CHUNK : map->length - at, s, in_piece, in_block, n;
		int i;
		for (s = 0; s * SECTOR < size; s++)
		{
			uint32_t part = size - s * SECTOR > SECTOR ? SECTOR : size - s * SECTOR;
			if (covered[c + s])
				memset(chunk + s * SECTOR, 0, part);
			else if (reader_pull(&r, chunk + s * SECTOR, part))
				goto done;
		}
		while (first_moved < moved_count && moved[first_moved].to <= at)
			first_moved++;
		for (i = first_moved; i < moved_count && moved[i].from < at + size; i++)
		{
			if (overlap(moved[i].from, moved[i].to, at, size, &in_piece, &in_block, &n))
			{
				uint32_t place = moved[i].stock + in_block;
				if (disk_seek(stock, place) || read_all(stock, chunk + in_piece, n))
					goto done;
			}
		}
		set_fields(field_count, &first_field, at, size);
		crc = crc_add(crc, chunk, size);
		if (w && writer_put(w, chunk, size))
			goto done;
	}
	failed = reader_pull(&r, NULL, 0) || crc != map->crc;
done:
	reader_close(&r);
	return failed;
}

/* ---- a swap's own steps ---- */

/* the map replaced by its .oxnew */
static int swap_in(char const *halo, char const *name)
{
	char path[240], old[240], fresh[240];
	map_path(path, sizeof(path), halo, name, "");
	map_path(old, sizeof(old), halo, name, ".oxold");
	map_path(fresh, sizeof(fresh), halo, name, ".oxnew");
	if (exists(path) && !move_file(path, old))
		return 1;
	if (!move_file(fresh, path))
	{
		move_file(old, path);
		return 1;
	}
	delete_file(old);
	return 0;
}

/* what a power cut left: before stock.restore's in place, the retail maps are untouched and what was being built goes;
after it, every .oxnew is whole and checked, so the swap it was part of is finished */
/* (below) */
static void here_clear(char const *halo);

static void recover(char const *halo)
{
	char table[240], path[240], old[240], fresh[240], temp[240], building[240];
	int k, committed, small = 0;
	stockmaps_cache_clear();
	here_clear(halo);
	maps_file(table, sizeof(table), halo, "stock.restore");
	maps_file(building, sizeof(building), halo, "stock.shared.oxtmp");
	committed = exists(table);
	if (!committed && !exists(building))
		return;
	for (k = 0; k < STOCK_MAPS; k++)
	{
		char const *name = retail_maps[k].name;
		int has_map, has_old, has_new;
		map_path(path, sizeof(path), halo, name, "");
		map_path(old, sizeof(old), halo, name, ".oxold");
		map_path(fresh, sizeof(fresh), halo, name, ".oxnew");
		map_path(temp, sizeof(temp), halo, name, ".oxtmp");
		delete_file(temp);
		has_map = exists(path), has_old = exists(old), has_new = exists(fresh);
		if (!committed && has_new)
			delete_file(fresh), has_new = 0;
		if (has_old && !has_map)
		{
			if (!move_file(has_new ? fresh : old, path))
				continue;
			has_map = 1;
			has_new = 0;
			if (!exists(old))
				has_old = 0;
		}
		if (has_old && has_map)
			delete_file(old);
		else if (has_new && has_map)
			swap_in(halo, name);
		else if (has_new)
			move_file(fresh, path);
	}
	delete_file(building);
	maps_file(temp, sizeof(temp), halo, "stock.index.oxtmp");
	delete_file(temp);
	maps_file(temp, sizeof(temp), halo, "stock.restore.oxtmp");
	delete_file(temp);
	/* (nothing draws on stock.shared: the end of a restore, or a swap stopped before stock.restore went in place) */
	for (k = 0; k < STOCK_MAPS; k++)
	{
		unsigned char header[HEADER];
		HANDLE file;
		map_path(path, sizeof(path), halo, retail_maps[k].name, "");
		file = open_read(path);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		if (!read_all(file, header, HEADER) && u32(header + REFERENCE_OFFSET) == REFERENCE_SIGNATURE)
			small++;
		file_close(file);
	}
	if (!small)
	{
		maps_file(temp, sizeof(temp), halo, "stock.shared");
		delete_file(temp);
		maps_file(temp, sizeof(temp), halo, "stock.index");
		delete_file(temp);
		delete_file(table);
	}
	flush(halo);
}

void stockmaps_examine(char const *halo, struct stockmaps_look *look)
{
	char path[240];
	int k;
	recover(halo);
	memset(look, 0, sizeof(*look));
	for (k = 0; k < STOCK_MAPS; k++)
	{
		unsigned char header[HEADER];
		HANDLE file;
		map_path(path, sizeof(path), halo, retail_maps[k].name, "");
		file = open_read(path);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		if (GetFileSize(file, NULL) == retail_maps[k].raw_size)
			look->retail_sized++;
		if (!read_all(file, header, HEADER) && u32(header + REFERENCE_OFFSET) == REFERENCE_SIGNATURE)
			look->small++;
		file_close(file);
	}
	maps_file(path, sizeof(path), halo, "stock.restore");
	look->restorable = exists(path);
	maps_file(path, sizeof(path), halo, "stock.shared");
	look->restorable = look->restorable && exists(path);
}

static unsigned long long free_bytes(char const *path)
{
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {path[0], ':', '\\', 0};
	disk_sync(); /* (what's still to be written takes room too) */
	if (!GetDiskFreeSpaceExA(root, &available, &total, &free_total))
		return ~0ull;
	return available.QuadPart;
}

static void say_text(stockmaps_say_t say, int warning, char const *format, ...)
{
	char text[200];
	va_list list;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	if (say)
		say(warning, text);
}

/* ---- the swap ---- */

/* stock.shared, as it is on the disk: 0 when it's the file it should be */
static int stock_check(char const *path)
{
	HANDLE file = open_read(path);
	uint32_t size = 0, crc = crc_add(0, NULL, 0);
	unsigned char header[HEADER], *data;
	struct ahead ahead;
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	if (read_all(file, header, HEADER))
	{
		file_close(file);
		return 1;
	}
	size = HEADER;
	ahead_open(&ahead, file, io_in, io_in2);
	while (!ahead_next(&ahead, &data, &got) && got)
	{
		crc = crc_add(crc, data, got);
		size += got;
	}
	file_close(file);
	return size != STOCK_LENGTH || crc != STOCK_CHECKSUM || u32(header + 8) != STOCK_LENGTH || u32(header + 0x64) != STOCK_CHECKSUM;
}

/* maps\stock.index's temporary file, from the table (stock.shared is checked against its CRC first, so its blocks are these);
read back: 0 when it's right */
static int index_write(char const *path)
{
	struct index_file head;
	HANDLE file = create(path);
	uint32_t crc, size = (uint32_t)(sizeof(head) + sizeof(stock_index)), read_size = 0, read_crc = 0;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	memset(&head, 0, sizeof(head));
	memcpy(head.magic, "OXSI", 4);
	head.version = 1;
	head.count = STOCK_BLOCKS;
	head.stock_checksum = STOCK_CHECKSUM;
	head.stock_length = STOCK_LENGTH;
	crc = crc_add(crc_add(crc_add(0, NULL, 0), (unsigned char const *)&head, sizeof(head)),
		(unsigned char const *)stock_index, sizeof(stock_index));
	if (write_all(file, &head, sizeof(head)) || write_all(file, stock_index, sizeof(stock_index)))
	{
		file_close(file);
		return 1;
	}
	if (!finish(file))
		return 1;
	return file_crc(path, &read_size, &read_crc) || read_size != size || read_crc != crc;
}

/* the table walked: the next map's parts, from the map at *at; 1 past what's there */
static int table_next(unsigned char *table, uint32_t size, uint32_t *at, struct restore_map **map,
	struct map_bsp **bsps, struct restore_block **blocks)
{
	uint32_t need;
	if (*at > size || size - *at < sizeof(struct restore_map))
		return 1;
	*map = (struct restore_map *)(table + *at);
	if ((*map)->bsps > MOST_BSPS || (*map)->blocks > MOST_BLOCKS)
		return 1;
	need = (uint32_t)sizeof(struct restore_map) + (*map)->bsps * (uint32_t)sizeof(struct map_bsp) +
		(*map)->blocks * (uint32_t)sizeof(struct restore_block);
	if (size - *at < need)
		return 1;
	*bsps = (struct map_bsp *)(table + *at + sizeof(struct restore_map));
	*blocks = (struct restore_block *)((unsigned char *)*bsps + (*map)->bsps * sizeof(struct map_bsp));
	*at += need;
	return 0;
}

/* the temporary files of a swap or a restore that won't go on */
static void clear_temporary(char const *halo)
{
	char path[240];
	int k;
	for (k = 0; k < STOCK_MAPS; k++)
	{
		map_path(path, sizeof(path), halo, retail_maps[k].name, ".oxtmp");
		delete_file(path);
	}
	maps_file(path, sizeof(path), halo, "stock.index.oxtmp");
	delete_file(path);
	maps_file(path, sizeof(path), halo, "stock.shared.oxtmp");
	delete_file(path);
	maps_file(path, sizeof(path), halo, "stock.restore.oxtmp");
	delete_file(path);
	maps_file(path, sizeof(path), halo, "stock.unpacked");
	delete_file(path);
	here_clear(halo);
}

/* the room while it works: stock.shared, the two tables, and the biggest small map beside its retail one */
#define SWAP_ROOM ((unsigned long long)STOCK_LENGTH + SMALL_LEVEL_ROOM + 8u * MB)

/* ---- the cache drives: each small map made there in the one pass, then copied into the maps folder ---- */

static struct cached
{
	int ok;                         /* made there, and the small map it must be */
	int here;                       /* in the maps folder (<map>.map.oxsmall), not on a cache drive: step 3 renames it */
	int checked;                    /* (here) read back in the one pass, as it was written (back_step): step 3 needn't */
	uint32_t size, crc;             /* its file, as it should be on the disk */
	char path[240];
} cached[STOCK_MAPS];

/* where map k's small map goes in the one pass when the Halo folder's drive has room for it, and for a level made from its
retail map in step 3 besides: the maps folder, beside its retail map, as <map>.map.oxsmall, so step 3 needn't copy it (it's
renamed); 0 when it has the room */
#define HERE_ROOM (SMALL_LEVEL_ROOM + 8u * MB)
static int here_pick(char const *halo, int k, char *path, int size)
{
#ifdef STOCKMAPS_HOST
	if (getenv("STOCKMAPS_TEST_NOT_HERE")) /* (testing the cache drives' way) */
		return 1;
#endif
	if (free_bytes(halo) < (unsigned long long)retail_maps[k].small_disk + HERE_ROOM)
		return 1;
	map_path(path, size, halo, retail_maps[k].name, ".oxsmall");
	return 0;
}

/* a small map the one pass made in the maps folder, read back while the pass goes on to the next maps (when the disk has
the next 1 MB ready: its spare time, while zlib works), and checked against the file written (cached[k].checked) */
static struct
{
	int k, open;
	HANDLE file;
	struct ahead ahead;
	uint32_t size, crc;
} back;

static void back_end(int ok)
{
	file_close(back.file);
	back.open = 0;
	cached[back.k].checked = ok;
	if (!ok)
	{
		/* (it didn't read back as written: step 3 makes it from its retail map) */
		delete_file(cached[back.k].path);
		cached[back.k].ok = 0;
	}
}

/* the read-back taken on: one part if the disk has it ready, or (all) to its end */
static void back_step(int all)
{
	while (back.open && (all || ahead_ready(&back.ahead)))
	{
		unsigned char *data;
		DWORD got;
		if (ahead_next(&back.ahead, &data, &got))
		{
			back_end(0);
			return;
		}
		if (!got)
		{
#ifdef STOCKMAPS_HOST
			if (getenv("STOCKMAPS_TEST_BAD_BACK") && atoi(getenv("STOCKMAPS_TEST_BAD_BACK")) == back.k) /* (testing) */
				back.crc ^= 1;
#endif
			back_end(back.size == cached[back.k].size && back.crc == cached[back.k].crc);
			return;
		}
		back.crc = crc_add(back.crc, data, got);
		back.size += got;
		if (!all)
			return;
	}
}

/* map k's read-back begun (the one before it finished first) */
static void back_start(int k)
{
	back_step(1);
	back.file = open_read(cached[k].path);
	if (back.file == INVALID_HANDLE_VALUE)
		return;
	ahead_open(&back.ahead, back.file, io_in, io_in2);
	back.k = k;
	back.open = 1;
	back.size = 0;
	back.crc = crc_add(0, NULL, 0);
}

/* the small maps the one pass left in the maps folder deleted */
static void here_clear(char const *halo)
{
	char path[240];
	int k;
	for (k = 0; k < STOCK_MAPS; k++)
	{
		map_path(path, sizeof(path), halo, retail_maps[k].name, ".oxsmall");
		if (exists(path))
			delete_file(path);
	}
}

static void cache_file(char *path, int size, char const *cache, char const *name)
{
	text_print(path, size, "%s\\h1u_%s.oxsmall", cache, name);
}

static unsigned long long cache_room(char const *cache)
{
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {cache[0], ':', '\\', 0};
	disk_sync();
	return GetDiskFreeSpaceExA(root, &available, &total, &free_total) ? available.QuadPart : 0;
}

unsigned long long stockmaps_cache_need(char const *halo)
{
	unsigned long long need = 0, small = 0;
	int k;
	for (k = 0; k < STOCK_MAPS; k++)
	{
		need += retail_maps[k].small_disk + 8u * MB;
		small += retail_maps[k].small_disk;
	}
	/* (stock.shared and its two tables, the small maps beside the retail ones, and what here_pick keeps free besides) */
#ifdef STOCKMAPS_HOST
	if (getenv("STOCKMAPS_TEST_NOT_HERE"))
		return need;
#endif
	if (free_bytes(halo) >= (unsigned long long)STOCK_LENGTH + 8u * MB + small + HERE_ROOM)
		return 0;
	return need;
}

void stockmaps_cache_clear(void)
{
	char path[240];
	int i, k;
	for (i = 0; i < STOCKMAPS_CACHES; i++)
	{
		if (!stockmaps_cache[i])
			continue;
		for (k = 0; k < STOCK_MAPS; k++)
		{
			cache_file(path, sizeof(path), stockmaps_cache[i], retail_maps[k].name);
			if (exists(path))
				delete_file(path);
		}
	}
}

/* where map k's small map goes: the cache drive with the most room, if it has room for it; 0 when there's none */
static int cache_pick(int k, char *path, int size)
{
	unsigned long long most = 0;
	int i, best = -1;
	for (i = 0; i < STOCKMAPS_CACHES; i++)
	{
		unsigned long long room;
		if (!stockmaps_cache[i])
			continue;
		room = cache_room(stockmaps_cache[i]);
		if (room > most)
			most = room, best = i;
	}
	if (best < 0 || most < retail_maps[k].small_disk + 8u * MB)
		return 1;
	cache_file(path, size, stockmaps_cache[best], retail_maps[k].name);
	return 0;
}

/* a small map copied from the cache drive into the maps folder, checked as it's read against the file written there: 0 when
it's copied (the caller reads the copy back) */
static int copy_small(char const *from, char const *to, uint32_t size, uint32_t crc)
{
	HANDLE in = open_read(from), out;
	uint32_t got_size = 0, got_crc = crc_add(0, NULL, 0);
	struct ahead ahead;
	unsigned char *data;
	DWORD got;
	if (in == INVALID_HANDLE_VALUE)
		return 1;
	out = create(to);
	if (out == INVALID_HANDLE_VALUE)
	{
		file_close(in);
		return 1;
	}
	ahead_open(&ahead, in, io_in, io_in2);
	while (!ahead_next(&ahead, &data, &got) && got)
	{
		got_crc = crc_add(got_crc, data, got);
		got_size += got;
		if (write_all(out, data, got))
		{
			file_close(in);
			file_close(out);
			return 1;
		}
	}
	file_close(in);
	if (!finish(out))
		return 1;
	return got_size != size || got_crc != crc;
}

/* step 1: stock.shared's file and the restore table, in one pass through the retail maps; with layout (the table's, unpacked)
and a cache drive with room, each map's small map too (cached[]). 0 when done, else error[256] */
static int build_stock(char const *halo, char const *building_path, unsigned char *table, uint32_t *table_size, struct block *found,
	unsigned char *tail, unsigned char const *layout, uint32_t layout_size, stockmaps_say_t say, char *error)
{
	char path[240], cache_path[240];
	unsigned char header[HEADER], stock_header[HEADER];
	HANDLE stock;
	uint32_t left;
	int k;

	stock = create(building_path);
	if (stock == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "Couldn't write maps\\stock.shared: nothing changed.");
		return 1;
	}
	memset(chunk_before, 0, CHUNK);
	for (left = STOCK_LENGTH; left;)
	{
		uint32_t part = left > CHUNK ? CHUNK : left;
		if (write_all(stock, chunk_before, part))
		{
			file_close(stock);
			text_print(error, 256, "Couldn't write maps\\stock.shared (is the drive full?): nothing changed.");
			return 1;
		}
		left -= part;
	}
	step_end("stock.shared filled with zeros");
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct retail_map const *retail = &retail_maps[k];
		struct restore_map *map = (struct restore_map *)(table + *table_size);
		struct map_bsp *bsps = (struct map_bsp *)(map + 1);
		struct restore_block *blocks;
		struct map_view view;
		struct reader r;
		struct small s;
		uint32_t at, raw_size, raw_crc, b, defer, gone, small_length, small_crc;
		int first = 0, count, making = 0, here;

		say_text(say, 0, "%s (%d of %d): reading...", retail->name, k + 1, STOCK_MAPS);
		map_path(path, sizeof(path), halo, retail->name, "");
		if (reader_open(&r, path, header) || u32(header + 8) != retail->length || u32(header + 0x64) != retail->checksum)
		{
			reader_close(&r);
			say_text(say, 1, "(%s.map: its header isn't the retail map's)", retail->name);
			text_print(error, 256, "%s.map isn't the retail map this knows: nothing changed.", retail->name);
			file_close(stock);
			return 1;
		}
		memset(map, 0, sizeof(*map));
		text_print(map->name, sizeof(map->name), "%s", retail->name);
		map->raw_size = retail->raw_size;
		map->raw_crc = retail->raw_crc;
		map->length = retail->length;
		memcpy(map->header, header, HEADER);
		memset(&view, 0, sizeof(view));
		view.length = retail->length;
		view.tag_offset = u32(header + 0x10);
		view.tag_size = u32(header + 0x14);
		if (view.tag_offset < HEADER || view.tag_size > MOST_TAG_SIZE || view.tag_size > retail->length ||
			view.tag_offset > retail->length - view.tag_size)
			goto bad_map;
		/* (from the tags' first sector on, the map's kept: its tags are read from it, and the small map's last part is made
		from it once they are, since its fields are in them) */
		defer = view.tag_offset / SECTOR * SECTOR;
		if (retail->length - defer > MOST_TAIL)
			goto bad_map;
		view.tags = tail + (view.tag_offset - defer);
		/* (the small map on a cache drive, made as it's read) */
		here = layout && !here_pick(halo, k, cache_path, sizeof(cache_path));
		if (layout && (here || !cache_pick(k, cache_path, sizeof(cache_path))) &&
			!cover_baked(layout, layout_size, k, retail->length, &gone))
		{
			s.field_count = s.first_field = 0;
			if (!small_open(&s, map, gone, 1, retail, cache_path))
			{
				making = 1;
				if (s.runs)
				{
					r.tap = small_tap;
					r.tap_context = &s;
				}
			}
			else
				delete_file(cache_path);
		}
		for (at = HEADER; at < retail->length;)
		{
			uint32_t size = retail->length - at > CHUNK ? CHUNK : retail->length - at, in_piece, in_block, n;
			int i;
			if (reader_pull(&r, chunk, size))
				goto bad_map;
			/* (what stock.shared gets from this map) */
			while (first < retail->first_count && retail->firsts[first].offset + retail->firsts[first].size <= at)
				first++;
			for (i = first; i < retail->first_count && retail->firsts[i].offset < at + size; i++)
			{
				struct first_copy const *f = &retail->firsts[i];
				if (overlap(f->offset, f->offset + f->size, at, size, &in_piece, &in_block, &n))
				{
					uint32_t place = f->at + in_block;
					if (disk_seek(stock, place) || write_all(stock, chunk + in_piece, n))
					{
						reader_close(&r);
						file_close(stock);
						if (making)
						{
							small_abandon(&s);
							delete_file(cache_path);
						}
						text_print(error, 256, "Couldn't write maps\\stock.shared: nothing changed.");
						return 1;
					}
				}
			}
			if (at + size > defer)
			{
				uint32_t from = at > defer ? at : defer;
				memcpy(tail + (from - defer), chunk + (from - at), at + size - from);
			}
			if (making && at < defer && small_piece(&s, at, at + size <= defer ? size : defer - at))
			{
				r.tap = NULL;
				small_abandon(&s);
				delete_file(cache_path);
				making = 0;
			}
			back_step(0);
			at += size;
		}
		if (reader_pull(&r, NULL, 0) || reader_raw(&r, &raw_size, &raw_crc) || raw_size != retail->raw_size || raw_crc != retail->raw_crc)
		{
			say_text(say, 1, "(%s.map: size %lu, CRC %08lx)", retail->name, (unsigned long)r.raw_size, (unsigned long)r.raw_crc);
			reader_close(&r);
			if (making)
			{
				small_abandon(&s);
				delete_file(cache_path);
			}
			text_print(error, 256, "%s.map isn't the retail map this knows: nothing changed.", retail->name);
			file_close(stock);
			return 1;
		}
		reader_close(&r);
		map->crc = retail->unpacked_crc;
		if (find_blocks(&view, found, MOST_BLOCKS, &count, bsps, &map->bsps) || count != retail->blocks)
			goto bad_tags;
		map->blocks = (uint32_t)count;
		blocks = (struct restore_block *)(bsps + map->bsps);
		for (b = 0; b < map->blocks; b++)
		{
			blocks[b].field = found[b].field;
			blocks[b].offset = found[b].offset;
			blocks[b].size = found[b].size;
			blocks[b].stock = retail->places[b];
		}
		*table_size += (uint32_t)(sizeof(*map) + map->bsps * sizeof(*bsps) + map->blocks * sizeof(*blocks));
		/* (the small map's last part, its fields set now; it must be the small map the table says, else it's made as before) */
		if (making)
		{
			int failed = small_fields(&s, map, bsps, blocks) || (s.field_count && fields[0].at < defer);
			for (at = defer; !failed && at < retail->length; at += CHUNK)
			{
				uint32_t size = retail->length - at > CHUNK ? CHUNK : retail->length - at;
				memcpy(chunk, tail + (at - defer), size);
				failed = small_piece(&s, at, size);
			}
			if (failed)
				small_abandon(&s);
			else
				failed = small_close(&s, &small_length, &small_crc, &cached[k].size, &cached[k].crc) ||
					small_length != retail->small_length || (s.runs ?
					cached[k].size != retail->small_disk || cached[k].crc != retail->small_file_crc : small_crc != retail->small_crc);
			if (failed)
			{
				say_text(say, 1, "(%s: its small map on the cache drive didn't come out right: it's made later)", retail->name);
				delete_file(cache_path);
			}
			else
			{
				cached[k].ok = 1;
				cached[k].here = here;
				text_print(cached[k].path, sizeof(cached[k].path), "%s", cache_path);
				if (here)
					back_start(k);
			}
		}
		step_end(cached[k].ok ? "%s read into stock.shared and made small" : "%s read into stock.shared", retail->name);
		continue;
bad_map:
		reader_close(&r);
		file_close(stock);
		if (making)
		{
			small_abandon(&s);
			delete_file(cache_path);
		}
		text_print(error, 256, "%s.map doesn't unpack: nothing changed.", retail->name);
		return 1;
bad_tags:
		file_close(stock);
		if (making)
		{
			small_abandon(&s);
			delete_file(cache_path);
		}
		text_print(error, 256, "%s.map's tags don't read: nothing changed.", retail->name);
		return 1;
	}
	memset(stock_header, 0, HEADER);
	memcpy(stock_header, "daeh", 4);
	put32(stock_header + 4, 5);
	put32(stock_header + 8, STOCK_LENGTH);
	memcpy(stock_header + 0x20, "stock", 5);
	memcpy(stock_header + 0x40, "shared", 6);
	put32(stock_header + 0x64, STOCK_CHECKSUM);
	memcpy(stock_header + 0x7FC, "toof", 4);
	if (disk_seek(stock, 0) || write_all(stock, stock_header, HEADER) || !finish(stock))
	{
		text_print(error, 256, "Couldn't finish maps\\stock.shared: nothing changed.");
		return 1;
	}
	return 0;
}

/* the restore table read and checked; a malloc'd copy, 0 when it's good, else error[256] */
static int table_read(char const *table_path, unsigned char **table_out, uint32_t *size_out, char *error)
{
	struct restore_file *head;
	unsigned char *table;
	uint32_t size;
	HANDLE file = open_read(table_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "stock.restore can't be read: nothing changed.");
		return 1;
	}
	size = GetFileSize(file, NULL);
	table = size >= sizeof(struct restore_file) && size < 8u * MB ? malloc(size) : NULL;
	if (!table || read_all(file, table, size))
	{
		file_close(file);
		free(table);
		text_print(error, 256, "stock.restore can't be read: nothing changed.");
		return 1;
	}
	file_close(file);
	head = (struct restore_file *)table;
	if (!memcmp(head->magic, "OXSR", 4) && head->version == 1)
	{
		free(table);
		text_print(error, 256, "These small maps were made by an older test build: X in installer version 23 puts them back. "
			"Nothing changed.");
		return 1;
	}
	if (memcmp(head->magic, "OXSR", 4) || head->version != RESTORE_VERSION || head->count != STOCK_MAPS || head->size != size ||
		head->crc != crc_add(crc_add(0, NULL, 0), table + sizeof(*head), size - (uint32_t)sizeof(*head)))
	{
		free(table);
		text_print(error, 256, "stock.restore is damaged: nothing changed.");
		return 1;
	}
	*table_out = table;
	*size_out = size;
	return 0;
}

/* whether a map in the folder is small now */
static int map_is_small(char const *path)
{
	unsigned char header[HEADER];
	HANDLE file = open_read(path);
	int small = 0;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	small = !read_all(file, header, HEADER) && u32(header + REFERENCE_OFFSET) == REFERENCE_SIGNATURE;
	file_close(file);
	return small;
}

static voidpf layout_alloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void layout_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

/* the table's layout unpacked and checked (malloc'd), or NULL: then each small map's made from its retail map, as before */
static unsigned char *layout_unpack(void)
{
	z_stream z;
	unsigned char *out = malloc(STOCK_LAYOUT_LENGTH);
	int ok;
	if (!out)
		return NULL;
	memset(&z, 0, sizeof(z));
	z.zalloc = layout_alloc;
	z.zfree = layout_free;
	if (inflateInit(&z) != Z_OK)
	{
		free(out);
		return NULL;
	}
	z.next_in = (Bytef *)stock_layout;
	z.avail_in = sizeof(stock_layout);
	z.next_out = out;
	z.avail_out = STOCK_LAYOUT_LENGTH;
	ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == STOCK_LAYOUT_LENGTH &&
		crc_add(crc_add(0, NULL, 0), out, STOCK_LAYOUT_LENGTH) == STOCK_LAYOUT_CRC;
	inflateEnd(&z);
	if (!ok)
	{
		free(out);
		return NULL;
	}
	return out;
}

static int convert(char const *halo, stockmaps_say_t say, char *error)
{
	char path[240], other[240], building_path[240], shared_path[240], table_path[240], temp_table[240], index_path[240], temp_index[240];
	unsigned char *table = NULL, *tail = NULL, *layout = NULL;
	struct block *found = NULL;
	struct stockmaps_look look;
	uint32_t table_size = sizeof(struct restore_file), table_room, at, size = 0, crc = 0;
	uint32_t small_length, small_crc, disk_size, disk_crc;
	int k, result = 1, committed = 0, done_maps, failed, checked;

	memset(cached, 0, sizeof(cached));
	stockmaps_examine(halo, &look);
	maps_file(table_path, sizeof(table_path), halo, "stock.restore");
	maps_file(shared_path, sizeof(shared_path), halo, "stock.shared");
	maps_file(index_path, sizeof(index_path), halo, "stock.index");
	maps_file(building_path, sizeof(building_path), halo, "stock.shared.oxtmp");
	maps_file(temp_table, sizeof(temp_table), halo, "stock.restore.oxtmp");
	maps_file(temp_index, sizeof(temp_index), halo, "stock.index.oxtmp");
	whole_begin();
	if (look.small == STOCK_MAPS)
	{
		text_print(error, 256, "%d of the stock maps already draw on stock.shared.", look.small);
		return 1;
	}
	committed = look.small && look.restorable;
	if (look.small && !committed)
	{
		text_print(error, 256, "%d of the stock maps draw on stock.shared, but stock.shared or stock.restore isn't there.", look.small);
		return 1;
	}
	if (free_bytes(halo) < (committed ? (unsigned long long)SMALL_LEVEL_ROOM : SWAP_ROOM))
	{
		text_print(error, 256, "Not enough room on the drive: it needs %lu MB free for a while (it ends up using much "
			"less than now).", (unsigned long)((committed ? (unsigned long long)SMALL_LEVEL_ROOM : SWAP_ROOM) / MB));
		return 1;
	}
	/* every map there, the right size: a folder with another map finds out at once (its CRC's checked as it's read) */
	table_room = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		map_path(path, sizeof(path), halo, retail_maps[k].name, "");
		if (!map_is_small(path))
		{
			HANDLE file = open_read(path);
			size = 0;
			if (file != INVALID_HANDLE_VALUE)
			{
				size = GetFileSize(file, NULL);
				file_close(file);
			}
			if (size != retail_maps[k].raw_size)
			{
				say_text(say, 1, "(%s.map: size %lu)", retail_maps[k].name, (unsigned long)size);
				text_print(error, 256, "%s.map isn't the retail map this knows: nothing changed.", retail_maps[k].name);
				return 1;
			}
		}
		table_room += (uint32_t)(sizeof(struct restore_map) + MOST_BSPS * sizeof(struct map_bsp) +
			retail_maps[k].blocks * sizeof(struct restore_block));
	}
	if (committed)
	{
		/* (a swap that stopped: on with it) */
		if (table_read(table_path, &table, &table_size, error))
			goto done;
		say_text(say, 0, "Going on: %d of %d are done.", look.small, STOCK_MAPS);
	}
	else
	{
		struct restore_file *file_head;
		HANDLE file;
		tail = malloc(MOST_TAIL);
		table = malloc(table_room);
		found = malloc(MOST_BLOCKS * sizeof(struct block));
		layout = layout_unpack();
		pending = layout ? malloc(PENDING_ROOM) : NULL; /* (none: the small maps are packed all through, as before) */
		if (!tail || !table || !found)
		{
			text_print(error, 256, "Not enough memory: nothing changed.");
			goto done;
		}
		/* 1. the shared file and the restore table */
		step_begin();
		failed = build_stock(halo, building_path, table, &table_size, found, tail, layout, STOCK_LAYOUT_LENGTH, say, error);
		back_step(1); /* (the last small map's read-back finished) */
		if (failed)
			goto done;
		free(tail);
		tail = NULL;
		free(found);
		found = NULL;
		free(layout);
		layout = NULL;
		free(pending);
		pending = NULL;
		/* 2. read back, and in place */
		say_text(say, 0, "Checking stock.shared...");
		if (stock_check(building_path))
		{
			text_print(error, 256, "stock.shared didn't read back right: nothing changed.");
			goto done;
		}
		step_end("stock.shared read back");
		if (index_write(temp_index))
		{
			text_print(error, 256, "Couldn't write stock.index: nothing changed.");
			goto done;
		}
		step_end("stock.index written");
		file_head = (struct restore_file *)table;
		memset(file_head, 0, sizeof(*file_head));
		memcpy(file_head->magic, "OXSR", 4);
		file_head->version = RESTORE_VERSION;
		file_head->count = STOCK_MAPS;
		file_head->stock_checksum = STOCK_CHECKSUM;
		file_head->stock_length = STOCK_LENGTH;
		file_head->size = table_size;
		file_head->crc = crc_add(crc_add(0, NULL, 0), table + sizeof(*file_head), table_size - (uint32_t)sizeof(*file_head));
		file = create(temp_table);
		if (file == INVALID_HANDLE_VALUE || write_all(file, table, table_size) || !finish(file))
		{
			text_print(error, 256, "Couldn't write stock.restore: nothing changed.");
			goto done;
		}
		if (file_crc(temp_table, &size, &crc) || size != table_size || crc != crc_add(crc_add(0, NULL, 0), table, table_size))
		{
			text_print(error, 256, "stock.restore didn't read back right: nothing changed.");
			goto done;
		}
		delete_file(index_path);
		if (!move_file(temp_index, index_path))
		{
			text_print(error, 256, "Couldn't put stock.index in place: nothing changed.");
			goto done;
		}
		delete_file(shared_path);
		if (!move_file(building_path, shared_path))
		{
			delete_file(index_path);
			text_print(error, 256, "Couldn't put stock.shared in place: nothing changed.");
			goto done;
		}
		flush(halo);
		delete_file(table_path);
		if (!move_file(temp_table, table_path))
		{
			delete_file(shared_path);
			delete_file(index_path);
			text_print(error, 256, "Couldn't put stock.restore in place: nothing changed.");
			goto done;
		}
		flush(halo);
		step_end("stock.restore written, files put in place");
		committed = 1;
	}
	/* 3. each map on its own: written, checked against the small map it must be (its length and CRC unpacked), and read back
	from the disk as it was written (its size and CRC) */
	at = sizeof(struct restore_file);
	done_maps = 0;
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct retail_map const *retail = &retail_maps[k];
		struct restore_map *map;
		struct map_bsp *bsps;
		struct restore_block *blocks;
		if (table_next(table, table_size, &at, &map, &bsps, &blocks))
		{
			text_print(error, 256, "stock.restore is damaged.");
			result = 2;
			goto done;
		}
		map_path(path, sizeof(path), halo, retail->name, "");
		if (map_is_small(path))
		{
			done_maps++;
			continue;
		}
		map_path(other, sizeof(other), halo, retail->name, ".oxtmp");
		step_begin();
		failed = 1;
		checked = 0;
		if (cached[k].ok)
		{
			/* (made in the one pass: copied from the cache drive, or renamed if it's in the maps folder, then read back like one
			made here) */
			say_text(say, 0, "%s (%d of %d): putting the small map in place...", retail->name, k + 1, STOCK_MAPS);
			if (cached[k].here)
			{
				delete_file(other);
				failed = !move_file(cached[k].path, other);
				checked = cached[k].checked;
			}
			else
				failed = copy_small(cached[k].path, other, cached[k].size, cached[k].crc);
			delete_file(cached[k].path);
			cached[k].ok = 0;
			small_length = retail->small_length;
			small_crc = retail->small_crc;
			disk_size = cached[k].size;
			disk_crc = cached[k].crc;
			if (failed)
				delete_file(other);
			else
				step_end(cached[k].here ? "%s renamed into place" : "%s copied from the cache drive", retail->name);
		}
		if (failed)
		{
			say_text(say, 0, "%s (%d of %d): making it small...", retail->name, k + 1, STOCK_MAPS);
			failed = write_small(path, retail, map, bsps, blocks, 1, other, &small_length, &small_crc, &disk_size, &disk_crc);
			if (!failed)
				step_end("%s made small", retail->name);
		}
		if (!failed)
		{
			/* (one the one pass read back needn't be again: renamed, it's the same file) */
			size = disk_size;
			crc = disk_crc;
			failed = small_length != retail->small_length || small_crc != retail->small_crc ||
				(!checked && (file_crc(other, &size, &crc) || size != disk_size || crc != disk_crc));
			if (failed)
				say_text(say, 1, "(%s: small map %lu bytes, CRC %08lx; file %lu of %lu bytes, CRC %08lx of %08lx)", retail->name,
					(unsigned long)small_length, (unsigned long)small_crc, (unsigned long)size, (unsigned long)disk_size,
					(unsigned long)crc, (unsigned long)disk_crc);
		}
		if (failed)
		{
			delete_file(other);
			text_print(error, 256, "%s.map couldn't be made small. %d of %d are done: the rest stay as they are, and all of them "
				"play. Install again to go on, or X to put them back.", retail->name, done_maps, STOCK_MAPS);
			result = 2;
			goto done;
		}
		{
			char fresh[240];
			map_path(fresh, sizeof(fresh), halo, retail->name, ".oxnew");
			delete_file(fresh);
			if (!move_file(other, fresh) || swap_in(halo, retail->name))
			{
				text_print(error, 256, "Couldn't put the small %s.map in place: start the installer again to finish.", retail->name);
				result = 2;
				goto done;
			}
		}
		flush(halo);
		step_end("%s checked and swapped in", retail->name);
		done_maps++;
	}
	result = 0;
	whole_end("the swap");
done:
	free(tail);
	free(table);
	free(found);
	free(layout);
	free(pending);
	pending = NULL;
	stockmaps_cache_clear();
	here_clear(halo);
	if (result == 1 && !committed)
	{
		/* (nothing changed: what was being built goes) */
		clear_temporary(halo);
		flush(halo);
	}
	return result != 0;
}

/* ---- putting them back ---- */

static int restore(char const *halo, stockmaps_say_t say, char *error)
{
	char path[240], other[240], shared_path[240], table_path[240], index_path[240];
	unsigned char *table = NULL;
	struct stockmaps_look look;
	struct restore_file *file_head;
	HANDLE file = INVALID_HANDLE_VALUE;
	uint32_t table_size = 0, at, size, crc;
	unsigned long long need;
	int k, result = 1;

	stockmaps_examine(halo, &look);
	if (!look.small)
		return 0;
	whole_begin();
	if (!look.restorable)
	{
		text_print(error, 256, "The stock maps draw on stock.shared, but stock.shared or stock.restore isn't there: "
			"they can't be put back from here.");
		return 1;
	}
	maps_file(table_path, sizeof(table_path), halo, "stock.restore");
	maps_file(shared_path, sizeof(shared_path), halo, "stock.shared");
	maps_file(index_path, sizeof(index_path), halo, "stock.index");
	/* the table, checked */
	if (table_read(table_path, &table, &table_size, error))
		goto done;
	file_head = (struct restore_file *)table;
	/* the room: each retail map less the small one it replaces, and one map's overlap */
	need = 16u * MB;
	{
		uint32_t biggest = 0;
		at = sizeof(struct restore_file);
		for (k = 0; k < STOCK_MAPS; k++)
		{
			struct restore_map *map;
			struct map_bsp *bsps;
			struct restore_block *blocks;
			unsigned char header[HEADER];
			if (table_next(table, table_size, &at, &map, &bsps, &blocks))
			{
				text_print(error, 256, "stock.restore is damaged: nothing changed.");
				goto done;
			}
			map_path(path, sizeof(path), halo, map->name, "");
			file = open_read(path);
			if (file == INVALID_HANDLE_VALUE)
			{
				text_print(error, 256, "%s.map isn't there: nothing changed.", map->name);
				goto done;
			}
			size = GetFileSize(file, NULL);
			if (read_all(file, header, HEADER))
			{
				text_print(error, 256, "%s.map can't be read: nothing changed.", map->name);
				goto done;
			}
			file_close(file);
			file = INVALID_HANDLE_VALUE;
			if (u32(header + REFERENCE_OFFSET) == REFERENCE_SIGNATURE && map->raw_size > size)
			{
				need += map->raw_size - size;
				if (size > biggest)
					biggest = size;
			}
		}
		need += biggest;
	}
	if (free_bytes(halo) < need)
	{
		text_print(error, 256, "Not enough room on the drive: putting the maps back needs %lu MB free. Nothing changed.",
			(unsigned long)(need / MB));
		goto done;
	}
	say_text(say, 0, "Checking stock.shared...");
	if (stock_check(shared_path) || file_head->stock_checksum != STOCK_CHECKSUM || file_head->stock_length != STOCK_LENGTH)
	{
		text_print(error, 256, "stock.shared isn't the one these maps were made with: nothing changed.");
		goto done;
	}
	file = open_read(shared_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "Couldn't read stock.shared: nothing changed.");
		goto done;
	}
	/* each map */
	at = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct restore_map *map;
		struct map_bsp *bsps;
		struct restore_block *blocks;
		struct writer w;
		unsigned char header[HEADER];
		HANDLE map_file, out;
		int failed;

		table_next(table, table_size, &at, &map, &bsps, &blocks);
		map_path(path, sizeof(path), halo, map->name, "");
		map_file = open_read(path);
		if (map_file == INVALID_HANDLE_VALUE || read_all(map_file, header, HEADER))
		{
			if (map_file != INVALID_HANDLE_VALUE)
				file_close(map_file);
			text_print(error, 256, "%s.map can't be read.", map->name);
			goto done;
		}
		file_close(map_file);
		if (u32(header + REFERENCE_OFFSET) != REFERENCE_SIGNATURE)
			continue;                  /* (already back: a restore a power cut stopped) */
		say_text(say, 0, "%s (%d of %d): putting it back...", map->name, k + 1, STOCK_MAPS);
		step_begin();
		map_path(other, sizeof(other), halo, map->name, ".oxtmp");
		out = create(other);
		failed = out == INVALID_HANDLE_VALUE || write_all(out, map->header, HEADER) ||
			writer_open(&w, 6, out, crc_add(crc_add(0, NULL, 0), map->header, HEADER));
		if (!failed)
		{
			failed = make_original(path, map, bsps, blocks, file, &w);
			if (failed)
				deflateEnd(&w.z);
			else
				failed = writer_finish(&w) || retail_padding(&w, map->raw_size, out, &crc);
		}
		if (out != INVALID_HANDLE_VALUE && !finish(out))
			failed = 1;
		if (failed || crc != map->raw_crc)
		{
			delete_file(other);
			text_print(error, 256, "%s.map's restore didn't make the original (is the drive full?). The maps not yet back "
				"still play in Halo 1 Unlimited.", map->name);
			goto done;
		}
		/* (as it is on the disk, against the original's size and CRC) */
		if (file_crc(other, &size, &crc) || size != map->raw_size || crc != map->raw_crc)
		{
			delete_file(other);
			text_print(error, 256, "%s.map didn't read back as the original. The maps not yet back still play in "
				"Halo 1 Unlimited.", map->name);
			goto done;
		}
		map_path(path, sizeof(path), halo, map->name, ".oxnew");
		delete_file(path);
		if (!move_file(other, path) || swap_in(halo, map->name))
		{
			text_print(error, 256, "Couldn't put %s.map in place: start the installer again to finish.", map->name);
			goto done;
		}
		flush(halo);
		step_end("%s put back", map->name);
	}
	file_close(file);
	file = INVALID_HANDLE_VALUE;
	/* all back: what the swap added goes */
	delete_file(shared_path);
	delete_file(index_path);
	flush(halo);
	delete_file(table_path);
	flush(halo);
	result = 0;
	whole_end("putting them back");
done:
	if (file != INVALID_HANDLE_VALUE)
		file_close(file);
	free(table);
	return result;
}

/* (with the disk thread on: it reads ahead and writes behind while zlib works) */
int stockmaps_convert(char const *halo, stockmaps_say_t say, char *error)
{
	int failed;
	disk_on();
	failed = convert(halo, say, error);
	disk_off();
	return failed;
}

int stockmaps_restore(char const *halo, stockmaps_say_t say, char *error)
{
	int failed;
	disk_on();
	failed = restore(halo, say, error);
	disk_off();
	return failed;
}
