/* mapstream.h: what the installer's map work shares (stockmaps.c, consolidate.c): a cache file (a header, the rest one zlib
stream) read and written a piece at a time, and a map's blocks (a bitmap's pixels, a sound permutation's samples) */
#ifndef OXBOX_MAPSTREAM_H
#define OXBOX_MAPSTREAM_H

#include <stdint.h>
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
#define MOST_BSPS 16

uint32_t u32(unsigned char const *p);
void put32(unsigned char *p, uint32_t value);
HANDLE open_read(char const *path);
int write_all(HANDLE file, void const *from, uint32_t size);
int read_all(HANDLE file, void *to, uint32_t size);

/* where the time goes, for the log (stockmaps.c's "time:" lines): the disk's reads and writes (a flush is a write), and
zlib's unpacking and packing; the rest is the CPU's own work (CRCs, hashes, copying) */
struct stream_times
{
	uint64_t read, write, unzip, zip;          /* microseconds */
	uint64_t read_bytes, write_bytes, unzipped, zipped; /* (unzipped: what came out; zipped: what went in) */
};
extern struct stream_times stream_times;
uint64_t stream_now(void);                      /* microseconds */
int timed_read(HANDLE file, void *to, DWORD size, DWORD *got);
int timed_write(HANDLE file, void const *from, DWORD size, DWORD *put);
int timed_flush(HANDLE file);                   /* 0 when flushed */
/* the time since then (stream_now) and how it went, from the times then: "12.3 s: read 4.1 s (61 MB), ..." */
void stream_times_text(char *text, int size, uint64_t since, struct stream_times const *then);

struct reader
{
	HANDLE file;
	z_stream z;
	int started, ended;
	uint32_t raw_crc, raw_size;     /* of the file as it is on the disk, so far */
};
/* a cache file opened, its header read */
int reader_open(struct reader *r, char const *path, unsigned char *header);
/* exactly size bytes of what's unpacked; with size 0, 0 only if the stream ends here */
int reader_pull(struct reader *r, unsigned char *to, uint32_t size);
/* up to size bytes: what there is before the stream ends (a pack's map can end short of its header's length, which is
rounded up to a sector). 0 when read, the count in *got */
int reader_read(struct reader *r, unsigned char *to, uint32_t size, uint32_t *got);
/* the rest of the file read (what's after the stream: its padding): its whole size and CRC, 0 when read */
int reader_raw(struct reader *r, uint32_t *size, uint32_t *crc);
void reader_close(struct reader *r);

struct writer
{
	z_stream z;
	HANDLE to;
	uint32_t written, crc;          /* what's gone out so far, and the running CRC of it */
};
/* one zlib stream (memLevel 8) to a file, or only measured; the CRC goes on from crc */
int writer_open(struct writer *w, int level, HANDLE to, uint32_t crc);
int writer_put(struct writer *w, unsigned char const *from, uint32_t size);
int writer_finish(struct writer *w);

/* a map's tag data in memory: everything read of its tags is in it */
struct map_view
{
	unsigned char *tags;
	uint32_t length, tag_offset, tag_size;
};
struct block
{
	uint32_t field, offset, size;
};
struct map_bsp
{
	uint32_t field, offset;
};
/* the blocks in the map's own data (at most `most` before those outside it are left out), and its BSPs' fields; 0 when read */
int find_blocks(struct map_view const *m, struct block *blocks, int most, int *count, struct map_bsp *bsps, uint32_t *bsp_count);

#endif
