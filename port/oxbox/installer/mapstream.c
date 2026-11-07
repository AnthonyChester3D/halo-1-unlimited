/* mapstream.c: a cache file read and written a piece at a time, and a map's blocks found (mapstream.h) */
#include "mapstream.h"

#include <stdlib.h>
#include <string.h>

#define MB (1024u * 1024u)

static unsigned char const zeros[1];
static unsigned char io_in[65536], io_out[65536];

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

uint32_t u32(unsigned char const *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void put32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)value;
	p[1] = (unsigned char)(value >> 8);
	p[2] = (unsigned char)(value >> 16);
	p[3] = (unsigned char)(value >> 24);
}

HANDLE open_read(char const *path)
{
	return CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
}

int write_all(HANDLE file, void const *from, uint32_t size)
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

int read_all(HANDLE file, void *to, uint32_t size)
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


void reader_close(struct reader *r)
{
	if (r->started)
		inflateEnd(&r->z);
	if (r->file != INVALID_HANDLE_VALUE)
		CloseHandle(r->file);
	r->started = 0;
	r->file = INVALID_HANDLE_VALUE;
}

/* a cache file opened, its header read */
int reader_open(struct reader *r, char const *path, unsigned char *header)
{
	memset(r, 0, sizeof(*r));
	r->file = open_read(path);
	if (r->file == INVALID_HANDLE_VALUE)
		return 1;
	if (read_all(r->file, header, HEADER) || memcmp(header, "daeh", 4) || u32(header + 4) != 5)
	{
		reader_close(r);
		return 1;
	}
	r->raw_crc = (uint32_t)crc32(crc32(0, NULL, 0), header, HEADER);
	r->raw_size = HEADER;
	r->z.zalloc = z_alloc;
	r->z.zfree = z_free;
	if (inflateInit(&r->z) != Z_OK)
	{
		reader_close(r);
		return 1;
	}
	r->started = 1;
	return 0;
}

/* exactly size bytes of what's unpacked; with size 0, 0 only if the stream ends here */
int reader_pull(struct reader *r, unsigned char *to, uint32_t size)
{
	unsigned char spare;
	int checking = !size;

	r->z.next_out = checking ? &spare : to;
	r->z.avail_out = checking ? 1 : size;
	while (r->z.avail_out)
	{
		int status;
		if (r->ended)
			return !checking;
		if (!r->z.avail_in)
		{
			DWORD got = 0;
			if (!ReadFile(r->file, io_in, sizeof(io_in), &got, NULL) || !got)
				return 1;
			r->raw_crc = (uint32_t)crc32(r->raw_crc, io_in, got);
			r->raw_size += got;
			r->z.next_in = io_in;
			r->z.avail_in = got;
		}
		status = inflate(&r->z, Z_NO_FLUSH);
		if (status == Z_STREAM_END)
			r->ended = 1;
		else if (status != Z_OK && !(status == Z_BUF_ERROR && !r->z.avail_in))
			return 1;
	}
	return checking; /* (a byte more came: it doesn't end here) */
}


int reader_read(struct reader *r, unsigned char *to, uint32_t size, uint32_t *got)
{
	r->z.next_out = to;
	r->z.avail_out = size;
	while (r->z.avail_out && !r->ended)
	{
		int status;
		if (!r->z.avail_in)
		{
			DWORD part = 0;
			if (!ReadFile(r->file, io_in, sizeof(io_in), &part, NULL) || !part)
				return 1;
			r->raw_crc = (uint32_t)crc32(r->raw_crc, io_in, part);
			r->raw_size += part;
			r->z.next_in = io_in;
			r->z.avail_in = part;
		}
		status = inflate(&r->z, Z_NO_FLUSH);
		if (status == Z_STREAM_END)
			r->ended = 1;
		else if (status != Z_OK && !(status == Z_BUF_ERROR && !r->z.avail_in))
			return 1;
	}
	*got = size - r->z.avail_out;
	return 0;
}

int reader_raw(struct reader *r, uint32_t *size, uint32_t *crc)
{
	DWORD got;
	while (ReadFile(r->file, io_in, sizeof(io_in), &got, NULL) && got)
	{
		r->raw_crc = (uint32_t)crc32(r->raw_crc, io_in, got);
		r->raw_size += got;
	}
	*size = r->raw_size;
	*crc = r->raw_crc;
	return 0;
}

/* one zlib stream (memLevel 8) to a file, or only measured; the CRC goes on from crc */
int writer_open(struct writer *w, int level, HANDLE to, uint32_t crc)
{
	memset(w, 0, sizeof(*w));
	w->z.zalloc = z_alloc;
	w->z.zfree = z_free;
	w->to = to;
	w->crc = crc;
	return deflateInit2(&w->z, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK;
}

static int writer_run(struct writer *w, int flush)
{
	int status;
	do
	{
		uint32_t n;
		w->z.next_out = io_out;
		w->z.avail_out = sizeof(io_out);
		status = deflate(&w->z, flush);
		if (status != Z_OK && status != Z_BUF_ERROR && status != Z_STREAM_END)
			return 1;
		n = (uint32_t)sizeof(io_out) - w->z.avail_out;
		w->crc = (uint32_t)crc32(w->crc, io_out, n);
		w->written += n;
		if (w->to != INVALID_HANDLE_VALUE && write_all(w->to, io_out, n))
			return 1;
	} while (flush == Z_FINISH ? status != Z_STREAM_END : !w->z.avail_out);
	return 0;
}

int writer_put(struct writer *w, unsigned char const *from, uint32_t size)
{
	w->z.next_in = (Bytef *)from;
	w->z.avail_in = size;
	return writer_run(w, Z_NO_FLUSH);
}

int writer_finish(struct writer *w)
{
	int failed;
	w->z.next_in = (Bytef *)zeros;
	w->z.avail_in = 0;
	failed = writer_run(w, Z_FINISH);
	deflateEnd(&w->z);
	return failed;
}

/* ---- a map's blocks: a bitmap's pixels, a sound permutation's samples (as tools/shared_pass.py's Map) ---- */

/* a place in the map (in the tag data) */
static uint32_t tag32(struct map_view const *m, uint32_t offset)
{
	if (offset < m->tag_offset || offset - m->tag_offset > m->tag_size - 4)
		return 0;
	return u32(m->tags + (offset - m->tag_offset));
}

/* an address in the tag data, as an offset in the map, or 0 */
static uint32_t tag_at(struct map_view const *m, uint32_t address)
{
	uint32_t o = address - TAG_BASE;
	if (address < TAG_BASE || o >= m->tag_size)
		return 0;
	return m->tag_offset + o;
}

int find_blocks(struct map_view const *m, struct block *blocks, int most, int *count, struct map_bsp *bsps, uint32_t *bsp_count)
{
	uint32_t instances, scenario, tags, k, table, s;
	int n = 0;

	instances = tag32(m, m->tag_offset);
	scenario = tag32(m, m->tag_offset + 4);
	tags = tag32(m, m->tag_offset + 12);
	table = tag_at(m, instances);
	if (!table)
		return 1;
	for (k = 0; k < tags; k++)
	{
		uint32_t instance = table + k * 0x20, group = tag32(m, instance), base = tag32(m, instance + 0x14), b, i;
		if (!base || (group != BITMAP && group != SOUND))
			continue;
		if (!(b = tag_at(m, base)))
			return 1;
		if (group == BITMAP)
		{
			/* (each bitmap's own pixels: its pixels_offset is the file's) */
			uint32_t bitmaps = tag_at(m, tag32(m, b + 0x64)), bitmap_count = tag32(m, b + 0x60);
			for (i = 0; i < bitmap_count; i++)
			{
				uint32_t e = bitmaps + i * 0x30;
				if (!bitmaps || n == most)
					return 1;
				blocks[n].field = e + 0x18;
				blocks[n].offset = tag32(m, e + 0x18);
				blocks[n].size = tag32(m, e + 0x1C);
				n++;
			}
		}
		else
		{
			uint32_t ranges = tag_at(m, tag32(m, b + 0x9C)), range_count = tag32(m, b + 0x98);
			for (i = 0; i < range_count; i++)
			{
				uint32_t range = ranges + i * 0x48, q, permutation_count, permutations;
				if (!ranges)
					return 1;
				permutation_count = tag32(m, range + 0x3C);
				permutations = tag_at(m, tag32(m, range + 0x40));
				for (q = 0; q < permutation_count; q++)
				{
					uint32_t p = permutations + q * 0x7C;
					if (!permutations || n == most)
						return 1;
					blocks[n].field = p + 0x48;
					blocks[n].offset = tag32(m, p + 0x48);
					blocks[n].size = tag32(m, p + 0x40);
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
	if (!(s = tag_at(m, tag32(m, table + (scenario & 0xFFFF) * 0x20 + 0x14))))
		return 1;
	*bsp_count = tag32(m, s + SCENARIO_BSPS);
	if (*bsp_count > MOST_BSPS)
		return 1;
	if (*bsp_count)
	{
		uint32_t references = tag_at(m, tag32(m, s + SCENARIO_BSPS + 4));
		if (!references)
			return 1;
		for (k = 0; k < *bsp_count; k++)
		{
			bsps[k].field = references + k * 0x20;
			bsps[k].offset = tag32(m, bsps[k].field);
		}
	}
	return 0;
}

