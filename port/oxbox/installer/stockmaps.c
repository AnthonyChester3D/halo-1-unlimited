/* stockmaps.c: Halo's 13 multiplayer maps in a Halo folder swapped for small maps drawing on stock.shared, and put back
(docs/shared_maps.md, tools/shared_pass.py: its `stock` pass makes the same stock.shared and maps). Built for the Xbox
(the installer) and for the PC (stockmaps_test.c, with stockmaps_host.h).

Only the retail maps are swapped (each file's size and CRC below), so every console's stock.shared is the same file.
Each map's restore table (maps\stock.restore) says where each moved block came from, and the original's size and CRC:
retail maps recompress to their exact bytes (zlib level 6, memLevel 8).

The order, so a power cut can't lose a map (each file written whole and read back before the next step counts on it):
	1. stock.unpacked (stock.shared unpacked: a temporary file, 78 MB) and each <map>.map.oxtmp, the small map, built;
	   each retail map recompressed once here, to prove it comes back exact on this console
	2. every small map's restore tried, from the files on the disk, against the original's CRC
	3. stock.shared.oxtmp, stock.restore.oxtmp written and read back
	4. the .oxtmp maps renamed .oxnew, stock.shared in place, then stock.restore in place: the point of no return.
	   Before it the retail maps are untouched and a power cut's leftovers are deleted; after it the swap is finished
	   by whoever looks next (stockmaps_examine)
	5. each map swapped: <map>.map to <map>.map.oxold, <map>.map.oxnew to <map>.map, <map>.map.oxold deleted
Putting them back is the same: <map>.map.oxtmp, the retail map, is checked against the size and CRC before it's renamed
.oxnew and swapped in the same way. stock.shared and stock.restore go last.

A whole map is in memory (46 MiB at most, 64 MiB in the console), so the package is let go first (main.c). */
#include "stockmaps.h"
#include "sha256.h"
#include "text.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#ifdef STOCKMAPS_HOST
#include "stockmaps_host.h"
#else
#undef FAR /* (zlib's, and windows.h's own) */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#endif

#define HEADER 2048u
#define SECTOR 2048u
#define TAG_BASE 0x803A6000u
#define BITMAP 0x6269746Du          /* 'bitm' */
#define SOUND 0x736E6421u           /* 'snd!' */
#define SCENARIO_BSPS 0x5A4u        /* a scenario's structure_bsp_references (0x20 each: file offset, size, ...) */
#define STOCK_BIT 0xC0000000u
#define REFERENCE_OFFSET 0x700u
#define REFERENCE_SIGNATURE 0x73687264u /* 'shrd' */
#define MAP_MEMORY 48050176u        /* the biggest retail map's whole size unpacked, and a sector */
#define MOST_BLOCKS 4096            /* (a map's; the retail maps have up to 1483, 2048 in stock.shared) */
#define MOST_BSPS 16
#define MB (1024u * 1024u)

/* the retail maps, in the order of their file names: each file's size and CRC, and unpacked, as tools/shared_pass.py
measured them; and the stock.shared they make */
static struct retail_map
{
	char const *name;
	uint32_t raw_size, raw_crc, length, checksum;
} const retail_maps[STOCK_MAPS] = {
	{"beavercreek", 22122496u, 0xccddcd8bu, 43565056u, 0xa831a7bfu},
	{"bloodgulch", 22571008u, 0x920a91b5u, 44328960u, 0x035e2da3u},
	{"boardingaction", 22075392u, 0x93d513c0u, 45346304u, 0xa0d614f5u},
	{"carousel", 20215808u, 0x4e8d431au, 42367488u, 0xb38b62c7u},
	{"chillout", 20336640u, 0x30f9085fu, 41967104u, 0x086c74adu},
	{"damnation", 23756800u, 0x72b26164u, 47968768u, 0x911cd6eeu},
	{"hangemhigh", 21338112u, 0x9bca5277u, 43700224u, 0x8313bd85u},
	{"longest", 20512768u, 0x646e3b39u, 42141696u, 0xdf1deaa6u},
	{"prisoner", 21323776u, 0xd497a860u, 43974656u, 0xb68da242u},
	{"putput", 19777536u, 0x2cee2513u, 40668672u, 0xb3f87f50u},
	{"ratrace", 23840768u, 0x27f712b7u, 47403008u, 0xaf5f3831u},
	{"sidewinder", 24823808u, 0x43164e3du, 48048128u, 0x4f8e749du},
	{"wizard", 19687424u, 0x00768890u, 40681984u, 0x2d5a08fcu},
};
#define STOCK_LENGTH 78260224u
#define STOCK_CHECKSUM 0xc028f8bfu
#define STOCK_PACKED_ROOM (48u * MB) /* (stock.shared packed is about 41 MB) */
#define SMALL_MAP_ROOM (6u * MB)     /* (a small map packed is under 4 MB) */

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
struct restore_bsp
{
	uint32_t field, offset;
};
struct restore_block
{
	uint32_t field, offset, size, stock; /* the original's field and block, and where the block is in stock.shared */
};
#define TABLE_MOST (sizeof(struct restore_file) + STOCK_MAPS * (sizeof(struct restore_map) + \
	MOST_BSPS * sizeof(struct restore_bsp) + MOST_BLOCKS * sizeof(struct restore_block)))

static uint32_t u32(unsigned char const *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

/* (zlib built without its own allocator on the Xbox: nxdk's Z_SOLO) */
static voidpf z_alloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void z_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

/* ---- files ---- */

static unsigned char io_in[65536], io_out[65536];
static unsigned char const zeros[SECTOR];

static int exists(char const *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static HANDLE open_read(char const *path)
{
	return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
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

static int write_all(HANDLE file, void const *from, uint32_t size)
{
	unsigned char const *at = from;
	while (size)
	{
		DWORD put = 0, part = size > MB ? MB : size;
		if (!WriteFile(file, at, part, &put, NULL) || put != part)
			return 1;
		at += part;
		size -= part;
	}
	return 0;
}

static int read_all(HANDLE file, void *to, uint32_t size)
{
	unsigned char *at = to;
	while (size)
	{
		DWORD got = 0, part = size > MB ? MB : size;
		if (!ReadFile(file, at, part, &got, NULL) || got != part)
			return 1;
		at += part;
		size -= part;
	}
	return 0;
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

/* ---- cache files: a header, the rest one zlib stream ---- */

struct unpacked
{
	uint32_t length;                /* the whole of it, unpacked */
	uint32_t body_crc;              /* of what's after the header, unpacked */
};

/* a cache file unpacked: whole into memory (room bytes), or into a file, or only read (body_crc) */
static int unpack(char const *path, unsigned char *memory, uint32_t room, HANDLE to, struct unpacked *u)
{
	HANDLE file = open_read(path);
	unsigned char header[HEADER];
	z_stream z;
	int status = Z_OK, result = 1, started = 0;
	DWORD got;

	if (file == INVALID_HANDLE_VALUE)
		return 1;
	if (read_all(file, header, HEADER) || memcmp(header, "daeh", 4) || u32(header + 4) != 5)
		goto done;
	u->length = u32(header + 8);
	u->body_crc = (uint32_t)crc32(0, NULL, 0);
	if (u->length < HEADER || (memory && u->length > room))
		goto done;
	memset(&z, 0, sizeof(z));
	z.zalloc = z_alloc;
	z.zfree = z_free;
	if (inflateInit(&z) != Z_OK)
		goto done;
	started = 1;
	if (memory)
	{
		memcpy(memory, header, HEADER);
		z.next_out = memory + HEADER;
		z.avail_out = room - HEADER;
	}
	else if (to != INVALID_HANDLE_VALUE && write_all(to, header, HEADER))
		goto done;
	while (status != Z_STREAM_END)
	{
		if (!ReadFile(file, io_in, sizeof(io_in), &got, NULL) || !got)
			goto done;
		z.next_in = io_in;
		z.avail_in = got;
		/* (until the input's used and the output didn't fill: it may hold more to give) */
		do
		{
			if (!memory)
			{
				z.next_out = io_out;
				z.avail_out = sizeof(io_out);
			}
			else if (!z.avail_out)
				goto done;
			status = inflate(&z, Z_NO_FLUSH);
			if (status == Z_BUF_ERROR && !z.avail_in)
				status = Z_OK;
			if (status != Z_OK && status != Z_STREAM_END)
				goto done;
			if (!memory)
			{
				uint32_t n = (uint32_t)sizeof(io_out) - z.avail_out;
				u->body_crc = (uint32_t)crc32(u->body_crc, io_out, n);
				if (to != INVALID_HANDLE_VALUE && write_all(to, io_out, n))
					goto done;
			}
		} while (status != Z_STREAM_END && (z.avail_in || (!memory && !z.avail_out)));
	}
	if (z.total_out != u->length - HEADER)
		goto done;
	if (memory)
		u->body_crc = (uint32_t)crc32(u->body_crc, memory + HEADER, u->length - HEADER);
	result = 0;
done:
	if (started)
		inflateEnd(&z);
	CloseHandle(file);
	return result;
}

/* in packed as one zlib stream (memLevel 8) to a file, or only measured; its size and the running CRC of it */
static int pack(unsigned char const *in, uint32_t size, int level, HANDLE to, uint32_t *written, uint32_t *crc)
{
	z_stream z;
	int status;
	memset(&z, 0, sizeof(z));
	z.zalloc = z_alloc;
	z.zfree = z_free;
	if (deflateInit2(&z, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		return 1;
	z.next_in = (Bytef *)in;
	z.avail_in = size;
	*written = 0;
	do
	{
		uint32_t n;
		z.next_out = io_out;
		z.avail_out = sizeof(io_out);
		status = deflate(&z, Z_FINISH);
		n = (uint32_t)sizeof(io_out) - z.avail_out;
		*crc = (uint32_t)crc32(*crc, io_out, n);
		*written += n;
		if (to != INVALID_HANDLE_VALUE && write_all(to, io_out, n))
			status = Z_ERRNO;
	} while (status == Z_OK);
	deflateEnd(&z);
	return status != Z_STREAM_END;
}

/* the retail map's bytes from its unpacked whole: the header, the stream, zeros to its size; or only their CRC */
static int pack_retail(unsigned char const *data, uint32_t length, uint32_t raw_size, HANDLE to, uint32_t *crc)
{
	uint32_t written, padding;
	*crc = (uint32_t)crc32(crc32(0, NULL, 0), data, HEADER);
	if (to != INVALID_HANDLE_VALUE && write_all(to, data, HEADER))
		return 1;
	if (pack(data + HEADER, length - HEADER, 6, to, &written, crc) || written > raw_size - HEADER)
		return 1;
	for (padding = raw_size - HEADER - written; padding;)
	{
		uint32_t part = padding > SECTOR ? SECTOR : padding;
		*crc = (uint32_t)crc32(*crc, zeros, part);
		if (to != INVALID_HANDLE_VALUE && write_all(to, zeros, part))
			return 1;
		padding -= part;
	}
	return 0;
}

/* ---- a map's blocks: a bitmap's pixels, a sound permutation's samples (as tools/shared_pass.py's Map) ---- */

struct map_view
{
	unsigned char const *data;
	uint32_t length, tag_offset, tag_size;
};

/* an address in the tag data, as an offset in the map, or 0 */
static uint32_t tag_at(struct map_view const *m, uint32_t address)
{
	uint32_t o = address - TAG_BASE;
	if (address < TAG_BASE || o >= m->tag_size)
		return 0;
	return m->tag_offset + o;
}

struct block
{
	uint32_t field, offset, size;
};

static int find_blocks(struct map_view *m, struct block *blocks, int *count, struct restore_bsp *bsps, uint32_t *bsp_count)
{
	unsigned char const *data = m->data;
	uint32_t instances, scenario, tags, k, table, s;
	int n = 0;

	m->tag_offset = u32(data + 0x10);
	m->tag_size = u32(data + 0x14);
	if (m->tag_offset < HEADER || m->tag_size > m->length || m->tag_offset > m->length - m->tag_size)
		return 1;
	instances = u32(data + m->tag_offset);
	scenario = u32(data + m->tag_offset + 4);
	tags = u32(data + m->tag_offset + 12);
	table = tag_at(m, instances);
	if (!table)
		return 1;
	for (k = 0; k < tags; k++)
	{
		uint32_t instance = table + k * 0x20, group = u32(data + instance), base = u32(data + instance + 0x14), b, i;
		if (!base || (group != BITMAP && group != SOUND))
			continue;
		if (!(b = tag_at(m, base)))
			return 1;
		if (group == BITMAP)
		{
			/* (each bitmap's own pixels: its pixels_offset is the file's) */
			uint32_t bitmaps = tag_at(m, u32(data + b + 0x64)), bitmap_count = u32(data + b + 0x60);
			for (i = 0; i < bitmap_count; i++)
			{
				uint32_t e = bitmaps + i * 0x30;
				if (!bitmaps || n == MOST_BLOCKS)
					return 1;
				blocks[n].field = e + 0x18;
				blocks[n].offset = u32(data + e + 0x18);
				blocks[n].size = u32(data + e + 0x1C);
				n++;
			}
		}
		else
		{
			uint32_t ranges = tag_at(m, u32(data + b + 0x9C)), range_count = u32(data + b + 0x98);
			for (i = 0; i < range_count; i++)
			{
				uint32_t range = ranges + i * 0x48, q, permutation_count, permutations;
				if (!ranges)
					return 1;
				permutation_count = u32(data + range + 0x3C);
				permutations = tag_at(m, u32(data + range + 0x40));
				for (q = 0; q < permutation_count; q++)
				{
					uint32_t p = permutations + q * 0x7C;
					if (!permutations || n == MOST_BLOCKS)
						return 1;
					blocks[n].field = p + 0x48;
					blocks[n].offset = u32(data + p + 0x48);
					blocks[n].size = u32(data + p + 0x40);
					n++;
				}
			}
		}
	}
	/* the ones in the map's own data, outside the tag data */
	for (k = 0, *count = 0; k < (uint32_t)n; k++)
	{
		struct block const *e = &blocks[k];
		if (e->size && e->offset >= HEADER && e->size <= m->length && e->offset <= m->length - e->size &&
			!(e->offset & 0x80000000u) &&
			!(m->tag_offset < e->offset + e->size && e->offset < m->tag_offset + m->tag_size))
			blocks[(*count)++] = *e;
	}
	/* the BSPs' offset fields */
	if (!(table + (scenario & 0xFFFF) * 0x20 + 0x18 <= m->length))
		return 1;
	if (!(s = tag_at(m, u32(data + table + (scenario & 0xFFFF) * 0x20 + 0x14))))
		return 1;
	*bsp_count = u32(data + s + SCENARIO_BSPS);
	if (*bsp_count > MOST_BSPS)
		return 1;
	if (*bsp_count)
	{
		uint32_t references = tag_at(m, u32(data + s + SCENARIO_BSPS + 4));
		if (!references)
			return 1;
		for (k = 0; k < *bsp_count; k++)
		{
			bsps[k].field = references + k * 0x20;
			bsps[k].offset = u32(data + bsps[k].field);
		}
	}
	return 0;
}

/* ---- the sectors wholly of moved data ---- */

static unsigned char covered[MAP_MEMORY / SECTOR + 2];
struct range
{
	uint32_t from, to;
};
static struct range ranges[MOST_BLOCKS];

static int range_order(void const *a, void const *b)
{
	struct range const *x = a, *y = b;
	return x->from < y->from ? -1 : x->from > y->from;
}

/* covered[] from the moved blocks; the number of sectors covered */
static uint32_t cover(struct restore_block const *blocks, uint32_t count, uint32_t length)
{
	uint32_t k, c, gone = 0, sectors = length / SECTOR + 1, from = 0, to = 0;
	memset(covered, 0, sectors + 1);
	for (k = 0; k < count; k++)
	{
		ranges[k].from = blocks[k].offset;
		ranges[k].to = blocks[k].offset + blocks[k].size;
	}
	qsort(ranges, count, sizeof(ranges[0]), range_order);
	for (k = 0; k <= count; k++)
	{
		if (k < count && k && ranges[k].from <= to)
		{
			if (ranges[k].to > to)
				to = ranges[k].to;
			continue;
		}
		if (k)
		{
			for (c = (from + SECTOR - 1) / SECTOR; c < to / SECTOR; c++)
				covered[c] = 1;
		}
		if (k < count)
		{
			from = ranges[k].from;
			to = ranges[k].to;
		}
	}
	covered[0] = 0;
	for (c = 0; c < sectors; c++)
		gone += covered[c];
	return gone;
}

/* an offset in the map, in the small map laid out without the covered sectors; 1 if it's in one */
static int moved_down(uint32_t offset, uint32_t *to)
{
	uint32_t c, sector = offset / SECTOR, gone = 0;
	if (covered[sector])
		return 1;
	for (c = 0; c < sector; c++)
		gone += covered[c];
	*to = offset - gone * SECTOR;
	return 0;
}

/* ---- the small map: laid out again without the moved data, in place (as tools/shared_pass.py's rebuild) ---- */

/* data: the map, unpacked. Every block moves to stock.shared. Its new length */
static int make_small(unsigned char *data, struct restore_map const *map, struct restore_bsp const *bsps,
	struct restore_block const *blocks, uint32_t *small_length)
{
	uint32_t k, c, out, gone, to, sectors = map->length / SECTOR + (map->length % SECTOR ? 1 : 0);

	for (k = 0; k < map->blocks; k++)
		put32(data + blocks[k].field, STOCK_BIT | blocks[k].stock);
	for (k = 0; k < map->blocks; k++)
		memset(data + blocks[k].offset, 0, blocks[k].size); /* (what's left of it in a sector that stays: zeros) */
	gone = cover(blocks, map->blocks, map->length);
	for (k = 0; k < map->bsps; k++)
	{
		if (moved_down(bsps[k].offset, &to))
			return 1;
		put32(data + bsps[k].field, to);
	}
	if (moved_down(u32(data + 0x10), &to))
		return 1;
	put32(data + 0x10, to);
	for (c = 1, out = SECTOR; c < sectors; c++)
	{
		uint32_t part = (c + 1) * SECTOR <= map->length ? SECTOR : map->length - c * SECTOR;
		if (covered[c])
			continue;
		if (out != c * SECTOR)
			memmove(data + out, data + c * SECTOR, part);
		out += part;
	}
	if (out != map->length - gone * SECTOR)
		return 1;
	put32(data + 8, out);
	/* the reference (docs/shared_maps.md): no pack's file, the stock one's, and the map's identity kept */
	memset(data + REFERENCE_OFFSET, 0, 92);
	put32(data + REFERENCE_OFFSET, REFERENCE_SIGNATURE);
	put32(data + REFERENCE_OFFSET + 4, 2);
	put32(data + REFERENCE_OFFSET + 0x30, map->raw_size);
	put32(data + REFERENCE_OFFSET + 0x34, u32(map->header + 0x64));
	memcpy(data + REFERENCE_OFFSET + 0x38, "stock", 5);
	put32(data + REFERENCE_OFFSET + 0x58, STOCK_CHECKSUM);
	put32(data + REFERENCE_OFFSET + 0x5C, STOCK_LENGTH);
	*small_length = out;
	return 0;
}

/* writes a small map: the header, the rest packed, padded to a whole sector (the game copies them unbuffered) */
static int write_cache_file(char const *path, unsigned char const *data, uint32_t length)
{
	HANDLE file = create(path);
	uint32_t written, crc = 0;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	if (write_all(file, data, HEADER) || pack(data + HEADER, length - HEADER, 9, file, &written, &crc) ||
		write_all(file, zeros, (SECTOR - written % SECTOR) % SECTOR))
	{
		CloseHandle(file);
		return 1;
	}
	return !finish(file);
}

/* the original's whole unpacked bytes from its small map in data and stock (stock.shared unpacked), checked. Every
step undoes one of make_small's */
static int make_original(unsigned char *data, uint32_t small_length, struct restore_map const *map,
	struct restore_bsp const *bsps, struct restore_block const *blocks, HANDLE stock)
{
	uint32_t k, c, gone, sectors;
	if (map->length > MAP_MEMORY || map->length < HEADER)
		return 1;
	for (k = 0; k < map->blocks; k++)
	{
		if (blocks[k].offset < HEADER || blocks[k].size > map->length || blocks[k].offset > map->length - blocks[k].size ||
			blocks[k].field < HEADER || blocks[k].field > map->length - 4)
			return 1;
	}
	gone = cover(blocks, map->blocks, map->length);
	if (small_length != map->length - gone * SECTOR)
		return 1;
	sectors = map->length / SECTOR + (map->length % SECTOR ? 1 : 0);
	/* (the sectors back in their places, from the last, so nothing is written over before it's moved) */
	for (c = sectors - 1; c >= 1; c--)
	{
		uint32_t part = (c + 1) * SECTOR <= map->length ? SECTOR : map->length - c * SECTOR;
		if (covered[c])
		{
			memset(data + c * SECTOR, 0, part);
			gone--;
		}
		else if (gone)
			memmove(data + c * SECTOR, data + (c - gone) * SECTOR, part);
	}
	memcpy(data, map->header, HEADER);
	for (k = 0; k < map->bsps; k++)
	{
		if (bsps[k].field < HEADER || bsps[k].field > map->length - 4)
			return 1;
		put32(data + bsps[k].field, bsps[k].offset);
	}
	for (k = 0; k < map->blocks; k++)
	{
		put32(data + blocks[k].field, blocks[k].offset);
		if (SetFilePointer(stock, (LONG)blocks[k].stock, NULL, FILE_BEGIN) != blocks[k].stock ||
			read_all(stock, data + blocks[k].offset, blocks[k].size))
			return 1;
	}
	return (uint32_t)crc32(crc32(0, NULL, 0), data, map->length) != map->crc;
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
	char table[240], unpacked[240], path[240], old[240], fresh[240], temp[240];
	int k, committed, small = 0;
	maps_file(table, sizeof(table), halo, "stock.restore");
	maps_file(unpacked, sizeof(unpacked), halo, "stock.unpacked");
	committed = exists(table);
	if (!committed && !exists(unpacked))
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
	maps_file(temp, sizeof(temp), halo, "stock.shared.oxtmp");
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
	uint32_t remainder, at;
};

struct stock_build
{
	HANDLE file;
	uint32_t at, crc;
	struct placed *placed;
	int count;
};

static int stock_place(struct stock_build *s, unsigned char const *block, uint32_t size, uint32_t remainder, uint32_t *at)
{
	unsigned char digest[32];
	uint32_t padding;
	int i;
	sha256(block, size, digest);
	for (i = 0; i < s->count; i++)
	{
		if (s->placed[i].remainder == remainder && !memcmp(s->placed[i].digest, digest, 32))
		{
			*at = s->placed[i].at;
			return 0;
		}
	}
	if (s->count == MOST_BLOCKS)
		return 1;
	padding = (remainder + 512 - s->at % 512) % 512;
	s->crc = (uint32_t)crc32(s->crc, zeros, padding);
	s->crc = (uint32_t)crc32(s->crc, block, size);
	if (write_all(s->file, zeros, padding))
		return 1;
	s->at += padding;
	memcpy(s->placed[s->count].digest, digest, 32);
	s->placed[s->count].remainder = remainder;
	s->placed[s->count].at = s->at;
	s->count++;
	*at = s->at;
	s->at += size;
	return write_all(s->file, block, size);
}

/* stock.shared unpacked finished: padded, and its header written */
static int stock_finish(struct stock_build *s, unsigned char *header)
{
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
	if (SetFilePointer(s->file, 0, NULL, FILE_BEGIN) != 0 || write_all(s->file, header, HEADER))
		return 1;
	return 0;
}

/* stock.shared from stock.unpacked: the header, the rest packed, padded to a whole sector */
static int stock_pack(char const *from, char const *to_path, unsigned char const *header)
{
	static unsigned char in[256 * 1024];
	HANDLE source = open_read(from), to;
	z_stream z;
	uint32_t written = 0;
	int status = Z_OK, result = 1;
	DWORD got;
	if (source == INVALID_HANDLE_VALUE)
		return 1;
	to = create(to_path);
	if (to == INVALID_HANDLE_VALUE)
	{
		CloseHandle(source);
		return 1;
	}
	memset(&z, 0, sizeof(z));
	z.zalloc = z_alloc;
	z.zfree = z_free;
	if (SetFilePointer(source, HEADER, NULL, FILE_BEGIN) != HEADER || write_all(to, header, HEADER) ||
		deflateInit2(&z, 9, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		goto done;
	while (status != Z_STREAM_END)
	{
		if (!ReadFile(source, in, sizeof(in), &got, NULL))
			break;
		z.next_in = in;
		z.avail_in = got;
		do
		{
			uint32_t n;
			z.next_out = io_out;
			z.avail_out = sizeof(io_out);
			status = deflate(&z, got ? Z_NO_FLUSH : Z_FINISH);
			if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END)
				break;
			n = (uint32_t)sizeof(io_out) - z.avail_out;
			written += n;
			if (write_all(to, io_out, n))
			{
				status = Z_ERRNO;
				break;
			}
		} while (status != Z_STREAM_END && (z.avail_in || !z.avail_out || !got));
		if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END)
			break;
	}
	deflateEnd(&z);
	result = status != Z_STREAM_END || write_all(to, zeros, (SECTOR - written % SECTOR) % SECTOR);
done:
	CloseHandle(source);
	if (!finish(to))
		result = 1;
	return result;
}

/* the table walked: the next map's parts, from the map at *at; 1 past what's there */
static int table_next(unsigned char *table, uint32_t size, uint32_t *at, struct restore_map **map,
	struct restore_bsp **bsps, struct restore_block **blocks)
{
	uint32_t need;
	if (*at > size || size - *at < sizeof(struct restore_map))
		return 1;
	*map = (struct restore_map *)(table + *at);
	if ((*map)->bsps > MOST_BSPS || (*map)->blocks > MOST_BLOCKS)
		return 1;
	need = (uint32_t)sizeof(struct restore_map) + (*map)->bsps * (uint32_t)sizeof(struct restore_bsp) +
		(*map)->blocks * (uint32_t)sizeof(struct restore_block);
	if (size - *at < need)
		return 1;
	*bsps = (struct restore_bsp *)(table + *at + sizeof(struct restore_map));
	*blocks = (struct restore_block *)((unsigned char *)*bsps + (*map)->bsps * sizeof(struct restore_bsp));
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
	maps_file(path, sizeof(path), halo, "stock.unpacked");
	DeleteFileA(path);
	maps_file(path, sizeof(path), halo, "stock.shared.oxtmp");
	DeleteFileA(path);
	maps_file(path, sizeof(path), halo, "stock.restore.oxtmp");
	DeleteFileA(path);
}

int stockmaps_convert(char const *halo, stockmaps_say_t say, char *error)
{
	char path[240], other[240], stock_path[240], shared_path[240], table_path[240], temp_table[240];
	unsigned char *data = NULL, *table = NULL, stock_header[HEADER];
	struct block *found = NULL;
	struct stock_build stock;
	struct stockmaps_look look;
	struct restore_file *file_head;
	HANDLE file = INVALID_HANDLE_VALUE;
	uint32_t table_size = sizeof(struct restore_file), at, size = 0, crc = 0;
	unsigned long long need = (unsigned long long)STOCK_LENGTH + STOCK_PACKED_ROOM +
		STOCK_MAPS * (unsigned long long)SMALL_MAP_ROOM + MB;
	int k, result = 1;

	memset(&stock, 0, sizeof(stock));
	stock.file = INVALID_HANDLE_VALUE;
	stockmaps_examine(halo, &look);
	if (look.small)
	{
		text_print(error, 256, "%d of the stock maps already draw on stock.shared.", look.small);
		return 1;
	}
	maps_file(table_path, sizeof(table_path), halo, "stock.restore");
	maps_file(shared_path, sizeof(shared_path), halo, "stock.shared");
	maps_file(stock_path, sizeof(stock_path), halo, "stock.unpacked");
	maps_file(temp_table, sizeof(temp_table), halo, "stock.restore.oxtmp");
	if (free_bytes(halo) < need)
	{
		text_print(error, 256, "Not enough room on the drive: it needs %lu MB free for a while (it ends up using less "
			"than now).", (unsigned long)(need / MB));
		return 1;
	}
	/* every map checked first: a folder with another map finds out at once */
	say_text(say, 0, "Checking your stock maps...");
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
	}
	data = malloc(MAP_MEMORY);
	table = malloc(TABLE_MOST);
	found = malloc(MOST_BLOCKS * sizeof(struct block));
	stock.placed = malloc(MOST_BLOCKS * sizeof(struct placed));
	if (!data || !table || !found || !stock.placed)
	{
		text_print(error, 256, "Not enough memory: nothing changed.");
		goto done;
	}
	stock.file = create(stock_path);
	if (stock.file == INVALID_HANDLE_VALUE || write_all(stock.file, zeros, HEADER))
	{
		text_print(error, 256, "Couldn't write maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	stock.at = HEADER;
	stock.crc = (uint32_t)crc32(0, NULL, 0);
	/* 1. the shared file's data and each small map */
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct retail_map const *retail = &retail_maps[k];
		struct restore_map *map = (struct restore_map *)(table + table_size);
		struct restore_bsp *bsps = (struct restore_bsp *)(map + 1);
		struct restore_block *blocks;
		struct map_view view;
		struct unpacked u;
		uint32_t small_length, b;
		int count;

		say_text(say, 0, "%s (%d of %d): building...", retail->name, k + 1, STOCK_MAPS);
		map_path(path, sizeof(path), halo, retail->name, "");
		if (unpack(path, data, MAP_MEMORY, INVALID_HANDLE_VALUE, &u) || u.length != retail->length ||
			u32(data + 0x64) != retail->checksum)
		{
			text_print(error, 256, "%s.map doesn't unpack: nothing changed.", retail->name);
			goto done;
		}
		/* (the proof that it comes back exact, on this console) */
		if (pack_retail(data, u.length, retail->raw_size, INVALID_HANDLE_VALUE, &crc) || crc != retail->raw_crc)
		{
			text_print(error, 256, "%s.map doesn't pack back to its own bytes: nothing changed.", retail->name);
			goto done;
		}
		memset(map, 0, sizeof(*map));
		text_print(map->name, sizeof(map->name), "%s", retail->name);
		map->raw_size = retail->raw_size;
		map->raw_crc = retail->raw_crc;
		map->length = u.length;
		map->crc = (uint32_t)crc32(crc32(0, NULL, 0), data, u.length);
		memcpy(map->header, data, HEADER);
		view.data = data;
		view.length = u.length;
		if (find_blocks(&view, found, &count, bsps, &map->bsps))
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
			if (stock_place(&stock, data + found[b].offset, found[b].size, found[b].offset % 512, &blocks[b].stock))
			{
				text_print(error, 256, "Couldn't write maps\\stock.unpacked: nothing changed.");
				goto done;
			}
		}
		if (make_small(data, map, bsps, blocks, &small_length))
		{
			text_print(error, 256, "%s.map couldn't be laid out again: nothing changed.", retail->name);
			goto done;
		}
		map_path(path, sizeof(path), halo, retail->name, ".oxtmp");
		if (write_cache_file(path, data, small_length))
		{
			text_print(error, 256, "Couldn't write the small %s.map: nothing changed.", retail->name);
			goto done;
		}
		table_size += (uint32_t)(sizeof(*map) + map->bsps * sizeof(*bsps) + map->blocks * sizeof(*blocks));
	}
	if (stock_finish(&stock, stock_header))
	{
		text_print(error, 256, "Couldn't finish maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	if (!finish(stock.file))
	{
		stock.file = INVALID_HANDLE_VALUE;
		text_print(error, 256, "Couldn't finish maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	stock.file = INVALID_HANDLE_VALUE;
	if (stock.at != STOCK_LENGTH || stock.crc != STOCK_CHECKSUM)
	{
		char line[120];
		text_print(line, sizeof(line), "(stock.shared: length %lu, checksum %08lx)", (unsigned long)stock.at,
			(unsigned long)stock.crc);
		say_text(say, 1, "%s", line);
		text_print(error, 256, "stock.shared didn't come out as it should: nothing changed.");
		goto done;
	}
	/* 2. every restore tried, from what's on the disk */
	file = open_read(stock_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "Couldn't read maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	at = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct restore_map *map;
		struct restore_bsp *bsps;
		struct restore_block *blocks;
		struct unpacked u;
		say_text(say, 0, "%s (%d of %d): trying its restore...", retail_maps[k].name, k + 1, STOCK_MAPS);
		map_path(path, sizeof(path), halo, retail_maps[k].name, ".oxtmp");
		if (table_next(table, table_size, &at, &map, &bsps, &blocks) ||
			unpack(path, data, MAP_MEMORY, INVALID_HANDLE_VALUE, &u) ||
			make_original(data, u.length, map, bsps, blocks, file))
		{
			text_print(error, 256, "%s.map's restore didn't make the original: nothing changed.", retail_maps[k].name);
			goto done;
		}
	}
	CloseHandle(file);
	file = INVALID_HANDLE_VALUE;
	/* 3. stock.shared and the restore table, read back */
	say_text(say, 0, "Packing stock.shared...");
	free(data);                        /* (a map's room isn't needed for this: only deflate's) */
	data = NULL;
	maps_file(path, sizeof(path), halo, "stock.shared.oxtmp");
	if (stock_pack(stock_path, path, stock_header))
	{
		text_print(error, 256, "Couldn't write stock.shared: nothing changed.");
		goto done;
	}
	{
		struct unpacked u;
		if (unpack(path, NULL, 0, INVALID_HANDLE_VALUE, &u) || u.length != STOCK_LENGTH || u.body_crc != STOCK_CHECKSUM)
		{
			text_print(error, 256, "stock.shared didn't read back right: nothing changed.");
			goto done;
		}
	}
	file_head = (struct restore_file *)table;
	memset(file_head, 0, sizeof(*file_head));
	memcpy(file_head->magic, "OXSR", 4);
	file_head->version = 1;
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
	maps_file(path, sizeof(path), halo, "stock.shared.oxtmp");
	DeleteFileA(shared_path);
	if (!MoveFileA(path, shared_path))
	{
		text_print(error, 256, "Couldn't put stock.shared in place: nothing changed.");
		goto done;
	}
	flush(halo);
	DeleteFileA(table_path);
	if (!MoveFileA(temp_table, table_path))
	{
		DeleteFileA(shared_path);
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
	DeleteFileA(stock_path);
	flush(halo);
	result = 0;
done:
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	if (stock.file != INVALID_HANDLE_VALUE)
		CloseHandle(stock.file);
	free(data);
	free(table);
	free(found);
	free(stock.placed);
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
	char path[240], other[240], stock_path[240], shared_path[240], table_path[240];
	unsigned char *data = NULL, *table = NULL;
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
	maps_file(stock_path, sizeof(stock_path), halo, "stock.unpacked");
	data = malloc(MAP_MEMORY);
	table = malloc(TABLE_MOST);
	if (!data || !table)
	{
		text_print(error, 256, "Not enough memory: nothing changed.");
		goto done;
	}
	/* the table, checked */
	file = open_read(table_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "stock.restore can't be read: nothing changed.");
		goto done;
	}
	table_size = GetFileSize(file, NULL);
	if (table_size < sizeof(struct restore_file) || table_size > TABLE_MOST || read_all(file, table, table_size))
	{
		text_print(error, 256, "stock.restore can't be read: nothing changed.");
		goto done;
	}
	CloseHandle(file);
	file = INVALID_HANDLE_VALUE;
	file_head = (struct restore_file *)table;
	if (memcmp(file_head->magic, "OXSR", 4) || file_head->version != 1 || file_head->count != STOCK_MAPS ||
		file_head->size != table_size ||
		file_head->crc != (uint32_t)crc32(crc32(0, NULL, 0), table + sizeof(*file_head), table_size - (uint32_t)sizeof(*file_head)))
	{
		text_print(error, 256, "stock.restore is damaged: nothing changed.");
		goto done;
	}
	/* the room: each retail map less the small one it replaces, the unpacked stock.shared, one map's overlap */
	need = (unsigned long long)file_head->stock_length + 16u * MB;
	at = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct restore_map *map;
		struct restore_bsp *bsps;
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
			need += map->raw_size - size;
	}
	if (free_bytes(halo) < need)
	{
		text_print(error, 256, "Not enough room on the drive: putting the maps back needs %lu MB free. Nothing changed.",
			(unsigned long)(need / MB));
		goto done;
	}
	/* stock.shared unpacked, checked */
	say_text(say, 0, "Unpacking stock.shared...");
	file = create(stock_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "Couldn't write maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	{
		struct unpacked u;
		int unpack_failed = unpack(shared_path, NULL, 0, file, &u), finished = finish(file);
		file = INVALID_HANDLE_VALUE;
		if (unpack_failed || !finished)
		{
			text_print(error, 256, "stock.shared couldn't be unpacked: nothing changed.");
			goto done;
		}
		if (u.length != file_head->stock_length || u.body_crc != file_head->stock_checksum)
		{
			text_print(error, 256, "stock.shared isn't the one these maps were made with: nothing changed.");
			goto done;
		}
	}
	file = open_read(stock_path);
	if (file == INVALID_HANDLE_VALUE)
	{
		text_print(error, 256, "Couldn't read maps\\stock.unpacked: nothing changed.");
		goto done;
	}
	/* each map */
	at = sizeof(struct restore_file);
	for (k = 0; k < STOCK_MAPS; k++)
	{
		struct restore_map *map;
		struct restore_bsp *bsps;
		struct restore_block *blocks;
		struct unpacked u;
		unsigned char header[HEADER];
		HANDLE map_file, out;
		int small;

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
		small = u32(header + REFERENCE_OFFSET) == REFERENCE_SIGNATURE;
		if (!small)
			continue;                  /* (already back: a restore a power cut stopped) */
		say_text(say, 0, "%s (%d of %d): putting it back...", map->name, k + 1, STOCK_MAPS);
		if (unpack(path, data, MAP_MEMORY, INVALID_HANDLE_VALUE, &u) ||
			make_original(data, u.length, map, bsps, blocks, file))
		{
			text_print(error, 256, "%s.map's restore didn't make the original. The maps not yet back still play in "
				"Halo 1 Unlimited.", map->name);
			goto done;
		}
		map_path(other, sizeof(other), halo, map->name, ".oxtmp");
		out = create(other);
		if (out == INVALID_HANDLE_VALUE || pack_retail(data, map->length, map->raw_size, out, &crc) || !finish(out) ||
			crc != map->raw_crc)
		{
			DeleteFileA(other);
			text_print(error, 256, "Couldn't write %s.map (is the drive full?). The maps not yet back still play in "
				"Halo 1 Unlimited.", map->name);
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
	DeleteFileA(stock_path);
	DeleteFileA(shared_path);
	flush(halo);
	DeleteFileA(table_path);
	flush(halo);
	result = 0;
done:
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
	free(data);
	free(table);
	if (result)
	{
		maps_file(path, sizeof(path), halo, "stock.unpacked");
		DeleteFileA(path);
		flush(halo);
	}
	return result;
}
