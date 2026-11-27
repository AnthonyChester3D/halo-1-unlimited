/*
OXBOX_RAM.C

A 128 MB console's RAM cache (Llama, 2026-11-26: "the people that put in the work to get the extra RAM get really
excited when there are features that help support it"). Its own XBE section, OXRAM, loaded only on a console with
100 MB or more and opence.ini ram_cache not 0 (oxbox_sdk.c oxbox_ram_frame): a 64 MB console carries none of it.

What the extra memory can hold: only what the processor reads. A Cerbios console gives the top 64 MB as scattered
pages, outside the window the graphics chip is pointed through (lab probes, v1.211), so textures, the texture cache,
tag data and the game state stay in the first 64 MB. Held here, first in first out:
- the stock tag pool's frames as a level's tag data is rebuilt (cache_files_windows.c tags_frame): a format 4 level
  read and unpacked 5-15 MB of stocktags.shared at every load (1.0-1.6 s on Llama's console), now once;
- reads of the map's and its shared files' data (textures, sounds, models: cache_file_read), each kept as it came;
  the same read again (the same map or pack, the same offset and size; stock.shared's for every map) copied from
  here. A lab simulation (v1.211, OXBOX_READ_CACHE_SIM) served 54% of a sweep's reads, most of a level's start.

The pool: 64 KB pieces of ordinary memory (NtAllocateVirtualMemory), every page checked above 64 MB (a piece with
one in the first 64 MB, which the graphics chip and sound need, is held until the pool is made, then given back), up
to 48 MB, leaving 12 MB free for the game. A third of it (up to 16 MB) holds the frames, the rest the reads: in one,
a level's textures pushed out the frames the next level drew on (xemu, Boxfight: 51 of 662 found). Reads finish on the cache thread: one lock (the kernel's critical section) for every look and
store.
*/

#include "platform.h"

#include <string.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

typedef unsigned char ram_boolean;

void *oxbox_kernel_export(unsigned long ordinal);
void platform_log(char const *format, ...);

extern ram_boolean (*oxbox_ram_get)(unsigned long kind, unsigned long source, unsigned long offset,
	unsigned long size, void *out);
extern void (*oxbox_ram_put)(unsigned long kind, unsigned long source, unsigned long offset,
	unsigned long size, void const *data);
extern void *(*oxbox_ram_bounce_take)(unsigned long size);
extern void (*oxbox_ram_bounce_give)(void *bounce);
struct cache_ram_request { unsigned long source; void *dest; };   /* (cache_files_windows.c's) */
extern struct cache_ram_request *oxbox_ram_requests;

enum
{
	RAM_PIECE = 0x10000,
	RAM_MOST_PIECES = 768,                 /* 48 MB */
	RAM_KEEP_FREE = 12u << 20,             /* left free for the game (heap, network, the perf log) */
	RAM_ENTRIES = 8192,
	RAM_FRAME_ENTRIES = 1024,              /* of them the stock tag pool's frames' (16 MB of 16 KB frames) */
	RAM_FRAMES_MOST = 16u << 20,           /* the frames' part of the pool: a third of it, up to 16 MB */
	RAM_SLOTS = 16384,                     /* the index: twice the entries, open addressing */
	RAM_LARGEST = 4u << 20,                /* a read bigger than this isn't kept */
	RAM_BOUNCES = 4,                       /* bounce buffers for reads bound for the texture cache (write-combined: */
	RAM_BOUNCE_SIZE = 1u << 20,            /* reading it back to keep it would be slow): 4 at once, up to 1 MB each */
	RAM_EMPTY = -1,
	RAM_GONE = -2
};

struct ram_entry
{
	unsigned long kind, source, offset, size;
	unsigned long at;                      /* where its bytes start in the pool */
	long slot;                             /* its place in the index */
};

static unsigned char *ram_pieces[RAM_MOST_PIECES];
static void *ram_low[512];                 /* (pieces with a page in the first 64 MB, held while the pool is made) */
static long ram_piece_count;
static unsigned long ram_size;             /* the pool's bytes (pieces x 64 KB) */
static struct ram_entry ram_entries[RAM_ENTRIES];
/* the pool in two parts, each first in first out: 0 the stock tag pool's frames (kind 1: every level draws on them,
and a level's textures would push them out), 1 reads (kind 2) */
static struct ram_ring
{
	unsigned long base, size, head;        /* its bytes in the pool, from base; where the next goes (from base) */
	long first, most;                      /* its entries: ram_entries[first, first + most) */
	long oldest, count, next;              /* in the order they came (from first) */
} ram_rings[2];
static long ram_index[RAM_SLOTS];
static long ram_gone;                      /* index slots left as RAM_GONE */
static unsigned char ram_lock[32];         /* (a kernel CRITICAL_SECTION: 28 bytes) */
static void (__stdcall *ram_enter)(void *);
static void (__stdcall *ram_leave)(void *);
static unsigned long ram_held;             /* bytes of live entries */
static unsigned long ram_stats[2][4];      /* [frames, reads][asked, found, found bytes, stored] */
static unsigned long ram_last_report;
static struct cache_ram_request ram_requests[512];   /* (MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS) */
static unsigned char *ram_bounces[RAM_BOUNCES];
static unsigned char ram_bounce_busy[RAM_BOUNCES];

static unsigned long ram_hash(unsigned long kind, unsigned long source, unsigned long offset, unsigned long size)
{
	unsigned long h = kind * 0x9E3779B1u;

	h = (h ^ source) * 0x85EBCA77u;
	h = (h ^ offset) * 0xC2B2AE3Du;
	h = (h ^ size) * 0x27D4EB2Fu;
	return h ^ (h >> 15);
}

/* bytes in or out of the pool at a pool offset: across its pieces */
static void ram_copy(unsigned long at, void *bytes, unsigned long size, int out)
{
	unsigned char *b = (unsigned char *)bytes;

	while (size)
	{
		unsigned long piece = at / RAM_PIECE, within = at % RAM_PIECE, n = RAM_PIECE - within;

		if (n > size)
			n = size;
		if (out)
			memcpy(b, ram_pieces[piece] + within, n);
		else
			memcpy(ram_pieces[piece] + within, b, n);
		b += n;
		at += n;
		size -= n;
	}
}

static long ram_find(unsigned long kind, unsigned long source, unsigned long offset, unsigned long size)
{
	unsigned long slot = ram_hash(kind, source, offset, size) & (RAM_SLOTS - 1);
	long probes;

	for (probes = 0; probes < RAM_SLOTS; probes++, slot = (slot + 1) & (RAM_SLOTS - 1))
	{
		long e = ram_index[slot];

		if (e == RAM_EMPTY)
			return -1;
		if (e >= 0 && ram_entries[e].kind == kind && ram_entries[e].source == source &&
			ram_entries[e].offset == offset && ram_entries[e].size == size)
		{
			return e;
		}
	}
	return -1;
}

/* the index made again from the live entries (too many slots left as RAM_GONE) */
static void ram_reindex(void)
{
	long k, e;

	struct ram_ring *r;

	for (k = 0; k < RAM_SLOTS; k++)
		ram_index[k] = RAM_EMPTY;
	for (r = ram_rings; r < ram_rings + 2; r++)
	{
		for (k = 0, e = r->oldest; k < r->count; k++, e = (e + 1) % r->most)
		{
			struct ram_entry *entry = &ram_entries[r->first + e];
			unsigned long slot = ram_hash(entry->kind, entry->source, entry->offset, entry->size) & (RAM_SLOTS - 1);

			while (ram_index[slot] != RAM_EMPTY)
				slot = (slot + 1) & (RAM_SLOTS - 1);
			ram_index[slot] = r->first + e;
			entry->slot = (long)slot;
		}
	}
	ram_gone = 0;
}

static void ram_drop_oldest(struct ram_ring *r)
{
	struct ram_entry *entry = &ram_entries[r->first + r->oldest];

	ram_index[entry->slot] = RAM_GONE;
	ram_gone++;
	ram_held -= entry->size;
	r->oldest = (r->oldest + 1) % r->most;
	r->count--;
}

static void ram_report(void)
{
	platform_log("ram cache: frames %lu of %lu found (%lu KB), reads %lu of %lu found (%lu KB); holding %lu KB in %ld",
		ram_stats[0][1], ram_stats[0][0], ram_stats[0][2] >> 10, ram_stats[1][1], ram_stats[1][0], ram_stats[1][2] >> 10,
		ram_held >> 10, ram_rings[0].count + ram_rings[1].count);
}

static ram_boolean ram_get(unsigned long kind, unsigned long source, unsigned long offset, unsigned long size,
	void *out)
{
	long e;
	unsigned long now;
	int which = kind == 1 ? 0 : 1;

	if (!size || size > RAM_LARGEST)
		return FALSE;
	ram_enter(ram_lock);
	ram_stats[which][0]++;
	e = ram_find(kind, source, offset, size);
	if (e >= 0)
	{
		ram_copy(ram_entries[e].at, out, size, 1);
		__asm__ __volatile__("sfence" ::: "memory");   /* (the texture cache is write-combined: written out before it's used) */
		ram_stats[which][1]++;
		ram_stats[which][2] += size;
	}
	ram_leave(ram_lock);
	now = GetTickCount();
	if (now - ram_last_report >= 60000 && ram_stats[1][0])
	{
		ram_last_report = now;
		ram_report();
	}
	return e >= 0;
}

static void ram_put(unsigned long kind, unsigned long source, unsigned long offset, unsigned long size,
	void const *data)
{
	struct ram_ring *r = &ram_rings[kind == 1 ? 0 : 1];
	unsigned long at, slot;
	struct ram_entry *entry;
	long e;

	if (!size || size > RAM_LARGEST || size > r->size / 4)
		return;
	ram_enter(ram_lock);
	if (ram_find(kind, source, offset, size) >= 0)
	{
		ram_leave(ram_lock);
		return;
	}
	/* (where it goes in its part: after the last one, or the part's start when it doesn't fit before its end; the
	entries there, its oldest, go: and on going round, those left past the last one, older still) */
	at = r->head + size > r->size ? 0 : r->head;
	while (r->count)
	{
		struct ram_entry *oldest = &ram_entries[r->first + r->oldest];
		unsigned long from = oldest->at - r->base;

		if (r->count < r->most && !(from < at + size && from + oldest->size > at) &&
			!(at == 0 && r->head != 0 && from >= r->head))
		{
			break;
		}
		ram_drop_oldest(r);
	}
	if (ram_gone > RAM_SLOTS / 4)
		ram_reindex();
	ram_copy(r->base + at, (void *)data, size, 0);
	e = r->first + r->next;
	entry = &ram_entries[e];
	entry->kind = kind;
	entry->source = source;
	entry->offset = offset;
	entry->size = size;
	entry->at = r->base + at;
	slot = ram_hash(kind, source, offset, size) & (RAM_SLOTS - 1);
	while (ram_index[slot] >= 0)
		slot = (slot + 1) & (RAM_SLOTS - 1);
	if (ram_index[slot] == RAM_GONE)
		ram_gone--;
	ram_index[slot] = e;
	entry->slot = (long)slot;
	r->next = (r->next + 1) % r->most;
	r->count++;
	ram_held += size;
	r->head = at + size;
	ram_stats[kind == 1 ? 0 : 1][3]++;
	ram_leave(ram_lock);
}

static void *ram_bounce_take(unsigned long size)
{
	void *bounce = NULL;
	int k;

	if (size > RAM_BOUNCE_SIZE)
		return NULL;
	ram_enter(ram_lock);
	for (k = 0; k < RAM_BOUNCES && !bounce; k++)
	{
		if (ram_bounces[k] && !ram_bounce_busy[k])
		{
			ram_bounce_busy[k] = 1;
			bounce = ram_bounces[k];
		}
	}
	ram_leave(ram_lock);
	return bounce;
}

static void ram_bounce_give(void *bounce)
{
	int k;

	ram_enter(ram_lock);
	for (k = 0; k < RAM_BOUNCES; k++)
		if (ram_bounces[k] == bounce)
			ram_bounce_busy[k] = 0;
	ram_leave(ram_lock);
}

/* the pool made and the hooks set (oxbox_sdk.c oxbox_ram_frame, once, the boot's work done): FALSE when there's no
room */
int oxbox_ram_start(void)
{
	long (__stdcall *allocate)(void **, unsigned long, unsigned long *, unsigned long, unsigned long) =
		(long (__stdcall *)(void **, unsigned long, unsigned long *, unsigned long, unsigned long))
		oxbox_kernel_export(184);   /* NtAllocateVirtualMemory */
	long (__stdcall *release)(void **, unsigned long *, unsigned long) =
		(long (__stdcall *)(void **, unsigned long *, unsigned long))oxbox_kernel_export(199);   /* NtFreeVirtualMemory */
	unsigned long (__stdcall *physical)(void *) = (unsigned long (__stdcall *)(void *))oxbox_kernel_export(173);
	void (__stdcall *initialize)(void *) = (void (__stdcall *)(void *))oxbox_kernel_export(291);
	MEMORYSTATUS status;
	unsigned long want, low_count = 0;
	long k;

	ram_enter = (void (__stdcall *)(void *))oxbox_kernel_export(277);
	ram_leave = (void (__stdcall *)(void *))oxbox_kernel_export(294);
	if (!allocate || !release || !physical || !initialize || !ram_enter || !ram_leave)
		return FALSE;
	GlobalMemoryStatus(&status);
	if (status.dwAvailPhys < RAM_KEEP_FREE + RAM_BOUNCES * RAM_BOUNCE_SIZE + (8u << 20))
	{
		platform_log("ram cache: only %lu KB free: none", (unsigned long)(status.dwAvailPhys >> 10));
		return FALSE;
	}
	want = (unsigned long)status.dwAvailPhys - RAM_KEEP_FREE - RAM_BOUNCES * RAM_BOUNCE_SIZE;
	if (want > RAM_MOST_PIECES * (unsigned long)RAM_PIECE)
		want = RAM_MOST_PIECES * (unsigned long)RAM_PIECE;
	/* (a piece with a page in the first 64 MB is held until the pool is made, then given back: given back at once, the
	next one asked for was that same piece again) */
	while (ram_piece_count < RAM_MOST_PIECES && (unsigned long)ram_piece_count * RAM_PIECE < want &&
		low_count < sizeof(ram_low) / sizeof(ram_low[0]))
	{
		void *piece = NULL;
		unsigned long size = RAM_PIECE, page;
		int low = 0;

		if (allocate(&piece, 0, &size, 0x3000 /* commit, reserve */, PAGE_READWRITE) < 0 || !piece)
			break;
		for (page = 0; page < RAM_PIECE && !low; page += 0x1000)
		{
			((volatile unsigned char *)piece)[page] = 0;
			low = physical((unsigned char *)piece + page) < 0x4000000;
		}
		if (low)
			ram_low[low_count++] = piece;
		else
			ram_pieces[ram_piece_count++] = (unsigned char *)piece;
	}
	for (k = 0; k < (long)low_count; k++)
	{
		unsigned long size = 0;

		release(&ram_low[k], &size, 0x8000 /* release */);
	}
	if (ram_piece_count < 64)
	{
		for (k = 0; k < ram_piece_count; k++)
		{
			void *piece = ram_pieces[k];
			unsigned long size = 0;

			release(&piece, &size, 0x8000);
		}
		platform_log("ram cache: only %ld pieces above 64 MB: none", ram_piece_count);
		ram_piece_count = 0;
		return FALSE;
	}
	ram_size = (unsigned long)ram_piece_count * RAM_PIECE;
	ram_rings[0].size = ram_size / 3 / RAM_PIECE * RAM_PIECE;
	if (ram_rings[0].size > RAM_FRAMES_MOST)
		ram_rings[0].size = RAM_FRAMES_MOST;
	ram_rings[0].most = RAM_FRAME_ENTRIES;
	ram_rings[1].base = ram_rings[0].size;
	ram_rings[1].size = ram_size - ram_rings[0].size;
	ram_rings[1].first = RAM_FRAME_ENTRIES;
	ram_rings[1].most = RAM_ENTRIES - RAM_FRAME_ENTRIES;
	for (k = 0; k < RAM_SLOTS; k++)
		ram_index[k] = RAM_EMPTY;
	for (k = 0; k < RAM_BOUNCES; k++)
	{
		void *bounce = NULL;
		unsigned long size = RAM_BOUNCE_SIZE;

		if (allocate(&bounce, 0, &size, 0x3000, PAGE_READWRITE) >= 0)
			ram_bounces[k] = (unsigned char *)bounce;
	}
	initialize(ram_lock);
	GlobalMemoryStatus(&status);
	platform_log("ram cache: %lu KB above 64 MB in %ld pieces (%lu in the first 64 MB given back); %lu KB left free",
		ram_size >> 10, ram_piece_count, low_count, (unsigned long)(status.dwAvailPhys >> 10));
	oxbox_ram_requests = ram_requests;
	oxbox_ram_bounce_take = ram_bounce_take;
	oxbox_ram_bounce_give = ram_bounce_give;
	oxbox_ram_put = ram_put;
	oxbox_ram_get = ram_get;   /* (last: the cache code looks at it to know the cache is on) */
	return TRUE;
}
