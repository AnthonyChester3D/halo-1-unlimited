/* stockmaps.c: Halo's 23 maps in a Halo folder (13 multiplayer, 10 campaign) swapped for small maps drawing on stock.shared,
and put back (docs/shared_maps.md, tools/shared_pass.py: its `stock` pass makes the same stock.shared and maps). Built for
the Xbox (the installer) and for the PC (stockmaps_test.c, with stockmaps_host.h).

Only the retail maps are swapped (each file's size and CRC: stockmaps_table.h, the same on the three NTSC discs), so every
console's stock.shared is the same file. It's stored unpacked, and the game reads it where it is. A multiplayer map's every
block goes in it; a level's blocks that three levels or more use do (the table's level_ masks), and the rest stay in the
level. Each map's restore table (maps\stock.restore) says where each moved block came from, and the original's size and
CRC: retail maps recompress to their exact bytes (zlib level 6, memLevel 8).

The order, so a power cut can't lose a map (each file written whole and read back before the next step counts on it):
	1. stock.shared.oxtmp and each <map>.map.oxtmp, the small map, built; each retail map recompressed once here, to prove
	   it comes back exact on this console. stock.unpacked is the map being worked on (a level unpacks to 280 MB, and the
	   console has 64: nothing here holds a whole map), left empty after, the mark that a swap is under way
	2. every small map's restore tried, from the files on the disk, against the original's CRC
	3. stock.shared.oxtmp read back; stock.index.oxtmp and stock.restore.oxtmp written and read back
	4. the .oxtmp maps renamed .oxnew, stock.index and stock.shared in place, then stock.restore in place: the point of no
	   return. Before it the retail maps are untouched and a power cut's leftovers are deleted; after it the swap is
	   finished by whoever looks next (stockmaps_examine)
	5. each map swapped: <map>.map to <map>.map.oxold, <map>.map.oxnew to <map>.map, <map>.map.oxold deleted
Putting them back is the same: <map>.map.oxtmp, the retail map, is checked against the size and CRC before it's renamed
.oxnew and swapped in the same way. stock.shared, stock.index and stock.restore go last. */
#include "stockmaps.h"
#include "mapstream.h"
#include "sha256.h"
#include "text.h"

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

/* the retail maps: multiplayer in the order of their file names, then the levels. Each file's size and CRC, and
unpacked, as tools/shared_pass.py measured them (gen_stock_table.py); its blocks; which go in stock.shared (NULL: all) */
struct retail_map
{
	char const *name;
	uint32_t raw_size, raw_crc, length, checksum;
	int blocks;
	unsigned char const *mask;
};
#include "stockmaps_table.h"
#define MOST_BLOCKS (MOST_MAP_BLOCKS + 512) /* (before the ones outside the map's data are left out) */
#define SMALL_MAP_ROOM (6u * MB)     /* (a small multiplayer map packed is under 4 MB) */
#define SMALL_LEVEL_ROOM (100u * MB) /* (a small level, under 95) */
#define RESTORE_VERSION 2            /* (1, v1.95's: 13 maps and stock.shared packed: installer 23 puts those back) */

void (*stockmaps_flush_drive)(char const *path);

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

/* ---- files ---- */

static unsigned char io_in[65536];
static unsigned char const zeros[SECTOR];

static int exists(char const *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static HANDLE create(char const *path)
{
	DeleteFileA(path);
	return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* (written to the disk before it's closed) */
static int finish(HANDLE file)
{
	IO_STATUS_BLOCK status;
	int ok = NT_SUCCESS(NtFlushBuffersFile(file, &status));
	return CloseHandle(file) && ok;
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
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	*size = 0;
	*crc = (uint32_t)crc32(0, NULL, 0);
	while (ReadFile(file, io_in, sizeof(io_in), &got, NULL) && got)
	{
		*crc = (uint32_t)crc32(*crc, io_in, got);
		*size += got;
	}
	CloseHandle(file);
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
		*crc = (uint32_t)crc32(*crc, zeros, part);
		if (to != INVALID_HANDLE_VALUE && write_all(to, zeros, part))
			return 1;
		padding -= part;
	}
	return 0;
}

static unsigned char chunk[CHUNK], chunk_before[CHUNK];

/* a cache file unpacked to a file: its header and the whole's length and CRC */
static int unpack_to(char const *path, HANDLE to, unsigned char *header, uint32_t *length, uint32_t *crc)
{
	struct reader r;
	uint32_t left;
	int failed = 1;
	if (reader_open(&r, path, header))
		return 1;
	*length = u32(header + 8);
	*crc = (uint32_t)crc32(crc32(0, NULL, 0), header, HEADER);
	if (*length < HEADER || write_all(to, header, HEADER))
		goto done;
	for (left = *length - HEADER; left;)
	{
		uint32_t part = left > CHUNK ? CHUNK : left;
		if (reader_pull(&r, chunk, part))
			goto done;
		*crc = (uint32_t)crc32(*crc, chunk, part);
		if (write_all(to, chunk, part))
			goto done;
		left -= part;
	}
	failed = reader_pull(&r, NULL, 0);
done:
	reader_close(&r);
	return failed;
}

/* the retail map's file from its unpacked whole: the CRC of what it would write (the proof it comes back exact) */
static int retail_proof(HANDLE unpacked, uint32_t length, uint32_t raw_size, uint32_t *crc)
{
	struct writer w;
	uint32_t left;
	if (SetFilePointer(unpacked, 0, NULL, FILE_BEGIN) != 0 || read_all(unpacked, chunk, HEADER))
		return 1;
	if (writer_open(&w, 6, INVALID_HANDLE_VALUE, (uint32_t)crc32(crc32(0, NULL, 0), chunk, HEADER)))
		return 1;
	for (left = length - HEADER; left;)
	{
		uint32_t part = left > CHUNK ? CHUNK : left;
		if (read_all(unpacked, chunk, part) || writer_put(&w, chunk, part))
		{
			deflateEnd(&w.z);
			return 1;
		}
		left -= part;
	}
	return writer_finish(&w) || retail_padding(&w, raw_size, INVALID_HANDLE_VALUE, crc);
}

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

/* a map's blocks sorted, the moved ones and those that stay, and covered[] from the moved; the number of sectors covered */
static uint32_t cover(struct restore_block const *blocks, uint32_t count, uint32_t length)
{
	uint32_t k, c, gone = 0, sectors = length / SECTOR + 1, from = 0, to = 0;
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

/* from the map unpacked (a file) and its tag data: the small map's file, its header in small_header. Its tag data's fields
are changed here (it's not needed as it was after) */
static int write_small(HANDLE unpacked, struct map_view *m, unsigned char const *header, struct restore_map const *map,
	struct map_bsp const *bsps, struct restore_block const *blocks, int level, char const *path)
{
	unsigned char small_header[HEADER];
	struct writer w;
	HANDLE file;
	uint32_t k, c, to, gone, out = SECTOR, tail, sectors = map->length / SECTOR + (map->length % SECTOR ? 1 : 0);
	int first_moved = 0, first_stay = 0;

	gone = cover(blocks, map->blocks, map->length);
	for (k = 0; k < map->blocks; k++)
	{
		uint32_t value = STOCK_BIT | blocks[k].stock;
		if (blocks[k].stock == STAYS && moved_down(blocks[k].offset, &value))
			return 1;
		put32(m->tags + (blocks[k].field - m->tag_offset), value);
	}
	for (k = 0; k < map->bsps; k++)
	{
		if (moved_down(bsps[k].offset, &to))
			return 1;
		put32(m->tags + (bsps[k].field - m->tag_offset), to);
	}
	memcpy(small_header, header, HEADER);
	if (moved_down(m->tag_offset, &to))
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

	file = create(path);
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	if (write_all(file, small_header, HEADER) || writer_open(&w, level, file, 0) ||
		SetFilePointer(unpacked, HEADER, NULL, FILE_BEGIN) != HEADER)
	{
		CloseHandle(file);
		return 1;
	}
	for (c = 1; c < sectors; c += CHUNK / SECTOR)
	{
		uint32_t at = c * SECTOR, size = map->length - at > CHUNK ? CHUNK : map->length - at, s, in_piece, in_block, n;
		int i, touched = 0;
		if (read_all(unpacked, chunk, size))
			goto failed;
		/* (what moved goes to zeros, which compress away; a block that stays keeps its bytes, shared or not) */
		while (first_moved < moved_count && moved[first_moved].to <= at)
			first_moved++;
		while (first_stay < stay_count && stay[first_stay].to <= at)
			first_stay++;
		for (i = first_moved; i < moved_count && moved[i].from < at + size; i++)
		{
			if (overlap(moved[i].from, moved[i].to, at, size, &in_piece, &in_block, &n))
			{
				if (!touched)
					memcpy(chunk_before, chunk, size);
				touched = 1;
				memset(chunk + in_piece, 0, n);
			}
		}
		if (touched)
		{
			for (i = first_stay; i < stay_count && stay[i].from < at + size; i++)
			{
				if (overlap(stay[i].from, stay[i].to, at, size, &in_piece, &in_block, &n))
					memcpy(chunk + in_piece, chunk_before + in_piece, n);
			}
		}
		/* (the tag data, with its fields changed) */
		if (overlap(m->tag_offset, m->tag_offset + m->tag_size, at, size, &in_piece, &in_block, &n))
			memcpy(chunk + in_piece, m->tags + in_block, n);
		for (s = 0; s * SECTOR < size; s++)
		{
			uint32_t part = size - s * SECTOR > SECTOR ? SECTOR : size - s * SECTOR;
			if (covered[c + s])
				continue;
			if (writer_put(&w, chunk + s * SECTOR, part))
				goto failed;
			out += part;
		}
	}
	if (out != map->length - gone * SECTOR || writer_finish(&w))
	{
		CloseHandle(file);
		return 1;
	}
	/* (padded to a whole sector: the game copies them unbuffered) */
	tail = (SECTOR - w.written % SECTOR) % SECTOR;
	if (write_all(file, zeros, tail))
	{
		CloseHandle(file);
		return 1;
	}
	return !finish(file);
failed:
	deflateEnd(&w.z);
	CloseHandle(file);
	return 1;
}

/* ---- the original from its small map and stock.shared, a piece at a time ---- */

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
	crc = (uint32_t)crc32(crc32(0, NULL, 0), map->header, HEADER);
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
				if (SetFilePointer(stock, (LONG)place, NULL, FILE_BEGIN) != place || read_all(stock, chunk + in_piece, n))
					goto done;
			}
		}
		while (first_field < field_count && fields[first_field].at + 4 <= at)
			first_field++;
		for (i = first_field; i < field_count && fields[i].at < at + size; i++)
		{
			uint32_t b;
			for (b = 0; b < 4; b++)
			{
				if (fields[i].at + b >= at && fields[i].at + b < at + size)
					chunk[fields[i].at + b - at] = (unsigned char)(fields[i].value >> (b * 8));
			}
		}
		crc = (uint32_t)crc32(crc, chunk, size);
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
	if (exists(path) && !MoveFileA(path, old))
		return 1;
	if (!MoveFileA(fresh, path))
	{
		MoveFileA(old, path);
		return 1;
	}
	DeleteFileA(old);
	return 0;
}

/* what a power cut left: before stock.restore's in place, the retail maps are untouched and what was being built goes;
after it, every .oxnew is whole and checked, so the swap it was part of is finished */
static void recover(char const *halo)
{
	char table[240], unpacked[240], path[240], old[240], fresh[240], temp[240], building[240];
	int k, committed, small = 0;
	maps_file(table, sizeof(table), halo, "stock.restore");
	maps_file(unpacked, sizeof(unpacked), halo, "stock.unpacked");
	maps_file(building, sizeof(building), halo, "stock.shared.oxtmp");
	committed = exists(table);
	if (!committed && !exists(unpacked) && !exists(building))
		return;
	for (k = 0; k < STOCK_MAPS; k++)
	{
		char const *name = retail_maps[k].name;
		int has_map, has_old, has_new;
		map_path(path, sizeof(path), halo, name, "");
		map_path(old, sizeof(old), halo, name, ".oxold");
		map_path(fresh, sizeof(fresh), halo, name, ".oxnew");
		map_path(temp, sizeof(temp), halo, name, ".oxtmp");
		DeleteFileA(temp);
		has_map = exists(path), has_old = exists(old), has_new = exists(fresh);
		if (!committed && has_new)
			DeleteFileA(fresh), has_new = 0;
		if (has_old && !has_map)
		{
			if (!MoveFileA(has_new ? fresh : old, path))
				continue;
			has_map = 1;
			has_new = 0;
			if (!exists(old))
				has_old = 0;
		}
		if (has_old && has_map)
			DeleteFileA(old);
		else if (has_new && has_map)
			swap_in(halo, name);
		else if (has_new)
			MoveFileA(fresh, path);
	}
	DeleteFileA(unpacked);
	DeleteFileA(building);
	maps_file(temp, sizeof(temp), halo, "stock.index.oxtmp");
	DeleteFileA(temp);
	maps_file(temp, sizeof(temp), halo, "stock.restore.oxtmp");
	DeleteFileA(temp);
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
		CloseHandle(file);
	}
	if (!small)
	{
		maps_file(temp, sizeof(temp), halo, "stock.shared");
		DeleteFileA(temp);
		maps_file(temp, sizeof(temp), halo, "stock.index");
		DeleteFileA(temp);
		DeleteFileA(table);
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
		CloseHandle(file);
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

/* a block placed in stock.shared: the same bytes at the same remainder modulo 512 are stored once */
struct placed
{
	unsigned char digest[32];
	uint32_t at, size;
	int next;                       /* the next with the same bucket, -1 */
};
#define BUCKETS 4096

struct stock_build
{
	HANDLE file;
	uint32_t at, crc;
	struct placed *placed;
	int count;
	int buckets[BUCKETS];
};

/* (the block read from the map unpacked, a block at most as MOST_BLOCK_SIZE: 2 MB) */
static int stock_place(struct stock_build *s, unsigned char const *block, uint32_t size, uint32_t remainder, uint32_t *at)
{
	unsigned char digest[32];
	uint32_t padding;
	int i, bucket;
	sha256(block, size, digest);
	bucket = (digest[0] | digest[1] << 8) % BUCKETS;
	for (i = s->buckets[bucket]; i >= 0; i = s->placed[i].next)
	{
		if (s->placed[i].at % 512 == remainder && !memcmp(s->placed[i].digest, digest, 32))
		{
			*at = s->placed[i].at;
			return 0;
		}
	}
	if (s->count == STOCK_BLOCKS)
		return 1;
	padding = (remainder + 512 - s->at % 512) % 512;
	s->crc = (uint32_t)crc32(s->crc, zeros, padding);
	s->crc = (uint32_t)crc32(s->crc, block, size);
	if (write_all(s->file, zeros, padding))
		return 1;
	s->at += padding;
	memcpy(s->placed[s->count].digest, digest, 32);
	s->placed[s->count].at = s->at;
	s->placed[s->count].size = size;
	s->placed[s->count].next = s->buckets[bucket];
	s->buckets[bucket] = s->count;
	s->count++;
	*at = s->at;
	s->at += size;
	return write_all(s->file, block, size);
}

/* stock.shared finished: padded, and its header written */
static int stock_finish(struct stock_build *s)
{
	unsigned char header[HEADER];
	uint32_t padding = (SECTOR - s->at % SECTOR) % SECTOR;
	s->crc = (uint32_t)crc32(s->crc, zeros, padding);
	if (write_all(s->file, zeros, padding))
		return 1;
	s->at += padding;
	memset(header, 0, HEADER);
	memcpy(header, "daeh", 4);
	put32(header + 4, 5);
	put32(header + 8, s->at);
	memcpy(header + 0x20, "stock", 5);
	memcpy(header + 0x40, "shared", 6);
	put32(header + 0x64, s->crc);
	memcpy(header + 0x7FC, "toof", 4);
	return SetFilePointer(s->file, 0, NULL, FILE_BEGIN) != 0 || write_all(s->file, header, HEADER);
}

/* stock.shared, as it is on the disk: 0 when it's the file it should be */
static int stock_check(char const *path)
{
	HANDLE file = open_read(path);
	uint32_t size = 0, crc = (uint32_t)crc32(0, NULL, 0);
	unsigned char header[HEADER];
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	if (read_all(file, header, HEADER))
	{
		CloseHandle(file);
		return 1;
	}
	size = HEADER;
	while (ReadFile(file, io_in, sizeof(io_in), &got, NULL) && got)
	{
		crc = (uint32_t)crc32(crc, io_in, got);
		size += got;
	}
	CloseHandle(file);
	return size != STOCK_LENGTH || crc != STOCK_CHECKSUM || u32(header + 8) != STOCK_LENGTH || u32(header + 0x64) != STOCK_CHECKSUM;
}

/* maps\stock.index's temporary file, read back: 0 when it's right */
static int index_write(char const *path, struct stock_build const *s)
{
	struct index_file head;
	HANDLE file = create(path);
	uint32_t crc = (uint32_t)crc32(0, NULL, 0), size, read_size = 0, read_crc = 0;
	int k;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	memset(&head, 0, sizeof(head));
	memcpy(head.magic, "OXSI", 4);
	head.version = 1;
	head.count = (uint32_t)s->count;
	head.stock_checksum = STOCK_CHECKSUM;
	head.stock_length = STOCK_LENGTH;
	crc = (uint32_t)crc32(crc, (unsigned char const *)&head, sizeof(head));
	if (write_all(file, &head, sizeof(head)))
	{
		CloseHandle(file);
		return 1;
	}
	for (k = 0; k < s->count; k++)
	{
		struct index_block entry;
		memcpy(entry.digest, s->placed[k].digest, 32);
		entry.at = s->placed[k].at;
		entry.size = s->placed[k].size;
		crc = (uint32_t)crc32(crc, (unsigned char const *)&entry, sizeof(entry));
		if (write_all(file, &entry, sizeof(entry)))
		{
			CloseHandle(file);
			return 1;
		}
	}
	if (!finish(file))
		return 1;
	size = (uint32_t)(sizeof(head) + s->count * sizeof(struct index_block));
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
		DeleteFileA(path);
	}
	maps_file(path, sizeof(path), halo, "stock.index.oxtmp");
	DeleteFileA(path);
	maps_file(path, sizeof(path), halo, "stock.shared.oxtmp");
	DeleteFileA(path);
	maps_file(path, sizeof(path), halo, "stock.restore.oxtmp");
	DeleteFileA(path);
	maps_file(path, sizeof(path), halo, "stock.unpacked");
	DeleteFileA(path);
}

/* (for the power cut test: much faster, with no proof and little packing; what it makes still restores exactly) */
#ifdef STOCKMAPS_HOST
#define TEST_FAST (getenv("STOCKMAPS_TEST_FAST") != NULL)
#else
#define TEST_FAST 0
#endif

/* the room while it works: stock.shared, a level unpacked, every small map, the two tables */
#define SWAP_ROOM ((unsigned long long)STOCK_LENGTH + MOST_MAP_LENGTH + (STOCK_MAPS - STOCK_LEVELS) * \
	(unsigned long long)SMALL_MAP_ROOM + STOCK_LEVELS * (unsigned long long)SMALL_LEVEL_ROOM + 8u * MB)

int stockmaps_convert(char const *halo, stockmaps_say_t say, char *error)
{
	char path[240], other[240], unpacked_path[240], building_path[240], shared_path[240], table_path[240], temp_table[240],
		index_path[240], temp_index[240];
	unsigned char *table = NULL, *block_memory = NULL, header[HEADER];
	struct block *found = NULL;
	struct stock_build *stock = NULL;
	struct stockmaps_look look;
	struct restore_file *file_head;
	struct map_view view;
	HANDLE file = INVALID_HANDLE_VALUE;
	uint32_t table_size = sizeof(struct restore_file), table_room, at, size = 0, crc = 0;
	int k, result = 1;

	memset(&view, 0, sizeof(view));
	stockmaps_examine(halo, &look);
	if (look.small)
	{
		text_print(error, 256, "%d of the stock maps already draw on stock.shared.", look.small);
		return 1;
	}
	maps_file(table_path, sizeof(table_path), halo, "stock.restore");
	maps_file(shared_path, sizeof(shared_path), halo, "stock.shared");
	maps_file(index_path, sizeof(index_path), halo, "stock.index");
	maps_file(unpacked_path, sizeof(unpacked_path), halo, "stock.unpacked");
	maps_file(building_path, sizeof(building_path), halo, "stock.shared.oxtmp");
	maps_file(temp_table, sizeof(temp_table), halo, "stock.restore.oxtmp");
	maps_file(temp_index, sizeof(temp_index), halo, "stock.index.oxtmp");
	if (free_bytes(halo) < SWAP_ROOM)
	{
		text_print(error, 256, "Not enough room on the drive: it needs %lu MB free for a while (it ends up using much "
			"less than now).", (unsigned long)(SWAP_ROOM / MB));
		return 1;
	}
	/* every map checked first: a folder with another map finds out at once */
	say_text(say, 0, "Checking your maps...");
	table_room = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		map_path(path, sizeof(path), halo, retail_maps[k].name, "");
		if (file_crc(path, &size, &crc) || size != retail_maps[k].raw_size || crc != retail_maps[k].raw_crc)
		{
			char line[120];
			text_print(line, sizeof(line), "(%s.map: size %lu, CRC %08lx)", retail_maps[k].name, (unsigned long)size,
				(unsigned long)crc);
			say_text(say, 1, "%s", line);
			text_print(error, 256, "%s.map isn't the retail map this knows: nothing changed.", retail_maps[k].name);
			return 1;
		}
		table_room += (uint32_t)(sizeof(struct restore_map) + MOST_BSPS * sizeof(struct map_bsp) +
			retail_maps[k].blocks * sizeof(struct restore_block));
	}
	view.tags = malloc(MOST_TAG_SIZE);
	block_memory = malloc(MOST_BLOCK_SIZE);
	table = malloc(table_room);
	found = malloc(MOST_BLOCKS * sizeof(struct block));
	stock = malloc(sizeof(*stock));
	if (stock)
	{
		stock->file = INVALID_HANDLE_VALUE;
		stock->placed = malloc(STOCK_BLOCKS * sizeof(struct placed));
	}
	if (!view.tags || !block_memory || !table || !found || !stock || !stock->placed)
	{
		text_print(error, 256, "Not enough memory: nothing changed.");
		goto done;
	}
	stock->file = create(building_path);
	if (stock->file == INVALID_HANDLE_VALUE || write_all(stock->file, zeros, HEADER))
	{
		text_print(error, 256, "Couldn't write maps\\stock.shared: nothing changed.");
		goto done;
	}
	stock->at = HEADER;
	stock->crc = (uint32_t)crc32(0, NULL, 0);
	stock->count = 0;
	for (k = 0; k < BUCKETS; k++)
		stock->buckets[k] = -1;
	/* 1. the shared file's data and each small map */
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct retail_map const *retail = &retail_maps[k];
		struct restore_map *map = (struct restore_map *)(table + table_size);
		struct map_bsp *bsps = (struct map_bsp *)(map + 1);
		struct restore_block *blocks;
		uint32_t length, whole_crc, b;
		int count, unpack_failed, finished;

		say_text(say, 0, "%s (%d of %d): building...", retail->name, k + 1, STOCK_MAPS);
		map_path(path, sizeof(path), halo, retail->name, "");
		file = create(unpacked_path);
		if (file == INVALID_HANDLE_VALUE)
		{
			text_print(error, 256, "Couldn't write maps\\stock.unpacked: nothing changed.");
			goto done;
		}
		unpack_failed = unpack_to(path, file, header, &length, &whole_crc);
		finished = finish(file);
		file = INVALID_HANDLE_VALUE;
		if (unpack_failed || !finished || length != retail->length || u32(header + 0x64) != retail->checksum)
		{
			text_print(error, 256, "%s.map doesn't unpack: nothing changed.", retail->name);
			goto done;
		}
		file = open_read(unpacked_path);
		if (file == INVALID_HANDLE_VALUE)
		{
			text_print(error, 256, "Couldn't read maps\\stock.unpacked: nothing changed.");
			goto done;
		}
		/* (the proof that it comes back exact, on this console) */
		if (!TEST_FAST && (retail_proof(file, length, retail->raw_size, &crc) || crc != retail->raw_crc))
		{
			text_print(error, 256, "%s.map doesn't pack back to its own bytes: nothing changed.", retail->name);
			goto done;
		}
		memset(map, 0, sizeof(*map));
		text_print(map->name, sizeof(map->name), "%s", retail->name);
		map->raw_size = retail->raw_size;
		map->raw_crc = retail->raw_crc;
		map->length = length;
		map->crc = whole_crc;
		memcpy(map->header, header, HEADER);
		view.length = length;
		view.tag_offset = u32(header + 0x10);
		view.tag_size = u32(header + 0x14);
		if (view.tag_offset < HEADER || view.tag_size > MOST_TAG_SIZE || view.tag_size > length ||
			view.tag_offset > length - view.tag_size ||
			SetFilePointer(file, (LONG)view.tag_offset, NULL, FILE_BEGIN) != view.tag_offset ||
			read_all(file, view.tags, view.tag_size) || find_blocks(&view, found, MOST_BLOCKS, &count, bsps, &map->bsps) ||
			count != retail->blocks)
		{
			text_print(error, 256, "%s.map's tags don't read: nothing changed.", retail->name);
			goto done;
		}
		map->blocks = (uint32_t)count;
		blocks = (struct restore_block *)(bsps + map->bsps);
		for (b = 0; b < map->blocks; b++)
		{
			blocks[b].field = found[b].field;
			blocks[b].offset = found[b].offset;
			blocks[b].size = found[b].size;
			blocks[b].stock = STAYS;
			if (retail->mask && !(retail->mask[b >> 3] >> (b & 7) & 1))
				continue;
			if (found[b].size > MOST_BLOCK_SIZE ||
				SetFilePointer(file, (LONG)found[b].offset, NULL, FILE_BEGIN) != found[b].offset ||
				read_all(file, block_memory, found[b].size) ||
				stock_place(stock, block_memory, found[b].size, found[b].offset % 512, &blocks[b].stock))
			{
				text_print(error, 256, "Couldn't write maps\\stock.shared: nothing changed.");
				goto done;
			}
		}
		map_path(path, sizeof(path), halo, retail->name, ".oxtmp");
		/* (multiplayer maps as tightly as they'll go, as the tool makes them; a level's bigger, and a console's slow) */
		if (write_small(file, &view, header, map, bsps, blocks, TEST_FAST ? 1 : retail->mask ? 6 : 9, path))
		{
			text_print(error, 256, "Couldn't write the small %s.map: nothing changed.", retail->name);
			goto done;
		}
		CloseHandle(file);
		file = INVALID_HANDLE_VALUE;
		table_size += (uint32_t)(sizeof(*map) + map->bsps * sizeof(*bsps) + map->blocks * sizeof(*blocks));
	}
	/* (no more map to hold: empty, it's the mark that a swap is under way until the end) */
	file = create(unpacked_path);
	if (file == INVALID_HANDLE_VALUE || !finish(file))
	{
		file = INVALID_HANDLE_VALUE;
		text_print(error, 256, "Couldn't write maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	file = INVALID_HANDLE_VALUE;
	if (stock_finish(stock) || !finish(stock->file))
	{
		stock->file = INVALID_HANDLE_VALUE;
		text_print(error, 256, "Couldn't finish maps\\stock.shared: nothing changed.");
		goto done;
	}
	stock->file = INVALID_HANDLE_VALUE;
	if (stock->at != STOCK_LENGTH || stock->crc != STOCK_CHECKSUM)
	{
		char line[120];
		text_print(line, sizeof(line), "(stock.shared: length %lu, checksum %08lx)", (unsigned long)stock->at,
			(unsigned long)stock->crc);
		say_text(say, 1, "%s", line);
		text_print(error, 256, "stock.shared didn't come out as it should: nothing changed.");
		goto done;
	}
	/* 2. every restore tried, from what's on the disk */
	file = open_read(building_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "Couldn't read maps\\stock.shared: nothing changed.");
		goto done;
	}
	at = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct restore_map *map;
		struct map_bsp *bsps;
		struct restore_block *blocks;
		say_text(say, 0, "%s (%d of %d): trying its restore...", retail_maps[k].name, k + 1, STOCK_MAPS);
		map_path(path, sizeof(path), halo, retail_maps[k].name, ".oxtmp");
		if (table_next(table, table_size, &at, &map, &bsps, &blocks) || make_original(path, map, bsps, blocks, file, NULL))
		{
			text_print(error, 256, "%s.map's restore didn't make the original: nothing changed.", retail_maps[k].name);
			goto done;
		}
	}
	CloseHandle(file);
	file = INVALID_HANDLE_VALUE;
	/* 3. stock.shared read back, the index and the restore table */
	say_text(say, 0, "Checking stock.shared...");
	if (stock_check(building_path))
	{
		text_print(error, 256, "stock.shared didn't read back right: nothing changed.");
		goto done;
	}
	if (index_write(temp_index, stock))
	{
		text_print(error, 256, "Couldn't write stock.index: nothing changed.");
		goto done;
	}
	file_head = (struct restore_file *)table;
	memset(file_head, 0, sizeof(*file_head));
	memcpy(file_head->magic, "OXSR", 4);
	file_head->version = RESTORE_VERSION;
	file_head->count = STOCK_MAPS;
	file_head->stock_checksum = STOCK_CHECKSUM;
	file_head->stock_length = STOCK_LENGTH;
	file_head->size = table_size;
	file_head->crc = (uint32_t)crc32(crc32(0, NULL, 0), table + sizeof(*file_head), table_size - (uint32_t)sizeof(*file_head));
	file = create(temp_table);
	if (file == INVALID_HANDLE_VALUE || write_all(file, table, table_size) || !finish(file))
	{
		file = INVALID_HANDLE_VALUE;
		text_print(error, 256, "Couldn't write stock.restore: nothing changed.");
		goto done;
	}
	file = INVALID_HANDLE_VALUE;
	if (file_crc(temp_table, &size, &crc) || size != table_size ||
		crc != (uint32_t)crc32(crc32(0, NULL, 0), table, table_size))
	{
		text_print(error, 256, "stock.restore didn't read back right: nothing changed.");
		goto done;
	}
	/* 4. everything in place but the maps; then stock.restore, the point of no return */
	for (k = 0; k < STOCK_MAPS; k++)
	{
		map_path(path, sizeof(path), halo, retail_maps[k].name, ".oxtmp");
		map_path(other, sizeof(other), halo, retail_maps[k].name, ".oxnew");
		DeleteFileA(other);
		if (!MoveFileA(path, other))
		{
			text_print(error, 256, "Couldn't name the small %s.map: nothing changed.", retail_maps[k].name);
			goto done;
		}
	}
	DeleteFileA(index_path);
	if (!MoveFileA(temp_index, index_path))
	{
		text_print(error, 256, "Couldn't put stock.index in place: nothing changed.");
		goto done;
	}
	DeleteFileA(shared_path);
	if (!MoveFileA(building_path, shared_path))
	{
		DeleteFileA(index_path);
		text_print(error, 256, "Couldn't put stock.shared in place: nothing changed.");
		goto done;
	}
	flush(halo);
	DeleteFileA(table_path);
	if (!MoveFileA(temp_table, table_path))
	{
		DeleteFileA(shared_path);
		DeleteFileA(index_path);
		text_print(error, 256, "Couldn't put stock.restore in place: nothing changed.");
		goto done;
	}
	flush(halo);
	/* 5. the maps */
	say_text(say, 0, "Swapping the maps...");
	for (k = 0; k < STOCK_MAPS; k++)
	{
		if (swap_in(halo, retail_maps[k].name))
		{
			text_print(error, 256, "Couldn't put the small %s.map in place: start the installer again to finish.",
				retail_maps[k].name);
			result = 2;
			goto done;
		}
	}
	DeleteFileA(unpacked_path);
	flush(halo);
	result = 0;
done:
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	if (stock && stock->file != INVALID_HANDLE_VALUE)
		CloseHandle(stock->file);
	free(view.tags);
	free(block_memory);
	free(table);
	free(found);
	if (stock)
		free(stock->placed);
	free(stock);
	if (result == 1)
	{
		/* (nothing changed: the small maps and the rest go, as they would after a power cut) */
		for (k = 0; k < STOCK_MAPS; k++)
		{
			map_path(path, sizeof(path), halo, retail_maps[k].name, ".oxnew");
			DeleteFileA(path);
		}
		clear_temporary(halo);
		flush(halo);
	}
	return result != 0;
}

/* ---- putting them back ---- */

int stockmaps_restore(char const *halo, stockmaps_say_t say, char *error)
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
	file = open_read(table_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "stock.restore can't be read: nothing changed.");
		goto done;
	}
	table_size = GetFileSize(file, NULL);
	table = table_size >= sizeof(struct restore_file) && table_size < 8u * MB ? malloc(table_size) : NULL;
	if (!table || read_all(file, table, table_size))
	{
		text_print(error, 256, "stock.restore can't be read: nothing changed.");
		goto done;
	}
	CloseHandle(file);
	file = INVALID_HANDLE_VALUE;
	file_head = (struct restore_file *)table;
	if (!memcmp(file_head->magic, "OXSR", 4) && file_head->version == 1)
	{
		text_print(error, 256, "These small maps were made by an older test build: X in installer version 23 puts them back. "
			"Nothing changed.");
		goto done;
	}
	if (memcmp(file_head->magic, "OXSR", 4) || file_head->version != RESTORE_VERSION || file_head->count != STOCK_MAPS ||
		file_head->size != table_size ||
		file_head->crc != (uint32_t)crc32(crc32(0, NULL, 0), table + sizeof(*file_head), table_size - (uint32_t)sizeof(*file_head)))
	{
		text_print(error, 256, "stock.restore is damaged: nothing changed.");
		goto done;
	}
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
			CloseHandle(file);
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
				CloseHandle(map_file);
			text_print(error, 256, "%s.map can't be read.", map->name);
			goto done;
		}
		CloseHandle(map_file);
		if (u32(header + REFERENCE_OFFSET) != REFERENCE_SIGNATURE)
			continue;                  /* (already back: a restore a power cut stopped) */
		say_text(say, 0, "%s (%d of %d): putting it back...", map->name, k + 1, STOCK_MAPS);
		map_path(other, sizeof(other), halo, map->name, ".oxtmp");
		out = create(other);
		failed = out == INVALID_HANDLE_VALUE || write_all(out, map->header, HEADER) ||
			writer_open(&w, 6, out, (uint32_t)crc32(crc32(0, NULL, 0), map->header, HEADER));
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
			DeleteFileA(other);
			text_print(error, 256, "%s.map's restore didn't make the original (is the drive full?). The maps not yet back "
				"still play in Halo 1 Unlimited.", map->name);
			goto done;
		}
		/* (as it is on the disk, against the original's size and CRC) */
		if (file_crc(other, &size, &crc) || size != map->raw_size || crc != map->raw_crc)
		{
			DeleteFileA(other);
			text_print(error, 256, "%s.map didn't read back as the original. The maps not yet back still play in "
				"Halo 1 Unlimited.", map->name);
			goto done;
		}
		map_path(path, sizeof(path), halo, map->name, ".oxnew");
		DeleteFileA(path);
		if (!MoveFileA(other, path) || swap_in(halo, map->name))
		{
			text_print(error, 256, "Couldn't put %s.map in place: start the installer again to finish.", map->name);
			goto done;
		}
		flush(halo);
	}
	CloseHandle(file);
	file = INVALID_HANDLE_VALUE;
	/* all back: what the swap added goes */
	DeleteFileA(shared_path);
	DeleteFileA(index_path);
	flush(halo);
	DeleteFileA(table_path);
	flush(halo);
	result = 0;
done:
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	free(table);
	return result;
}
