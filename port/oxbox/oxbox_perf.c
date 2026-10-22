/*
OXBOX_PERF.C

The developers' performance and memory log (test build 31), for Halo Map Porter: how much a converted map really
costs on a real Xbox, and what limits its frame rate (the CPU's game ticks or drawing, or the graphics chip).

Off unless opence.ini says perf_log = 1. Everything here (code, data, the ring buffer) is in the XBE section
OXPERF (link.ld, mkxbe.py), which isn't loaded at boot: with the log off it takes no memory, and the game's one hook
(main_loop: oxbox_perf.h) finds oxbox_perf NULL. With it on, start-up loads the section (oxbox_sdk.c
oxbox_perf_load) and calls oxbox_perf_start once.

opence.ini (read again here, D:\ then T:\, the later winning):
	perf_log = 1             on
	perf_slow_frame_ms = 34  a frame longer than this gets a line of its own (S)
	perf_gpu_sample_ms = 1   how often the graphics chip is asked whether it's busy (gpu_busy_pct); 0: never

The log: a CSV file a map session, perf\<map>_<yyyymmdd-hhmmss>.csv beside the XBE (T:\perf when the XBE's folder
can't be written; the time is the console's clock, UTC). Header lines start with #:
	# Halo 1 Unlimited perf log v1
	# build <version>
	# map <name> <map file size> <map checksum>
	# started <date> UTC; slow frame <ms> ms
	# memory_fields <the M row's fields>
	# fields <the P, S and T rows' fields>
Then a row a line, its first column the kind: M (memory, once at map load), P (a second's summary), S (a slow
frame), T (a tour stop: later). P, S and T have the same columns; an empty field isn't available.

Timing comes from the game loop's probe marks (main.c frame_probe_marks): the game's ticks (game_time_update: the
simulation), drawing (main_game_render: the CPU building the frame for the graphics chip), present (the CPU blocked
in IDirect3DDevice8_Present while the chip is behind), vsync (the frame-rate throttle spinning to the vertical
blank: idle) and the rest (input, network, menus, camera). The graphics chip itself has no timestamps: a 1 ms
kernel timer asks Direct3D whether it's busy (D3DDevice_IsBusy: the chip hasn't caught up with the commands sent
to it), which gives the share of the time it was working, gpu_busy_pct.

The game thread never touches the hard drive: it puts rows in a 32 KB ring, and a writer thread of its own writes
them out every 2 seconds (at once at round end and on pause), so the log can't make the stutters it measures.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "camera/observer.h"
#include "memory/data.h"
#include "memory/lruv_cache.h"
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "shell/shell.h"
#include "oxbox_perf.h"

#include <math.h>

void platform_log(char const *format, ...);
void *oxbox_kernel_export(unsigned long ordinal);

#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001
#endif
#ifndef FILE_FLAG_WRITE_THROUGH
#define FILE_FLAG_WRITE_THROUGH 0x80000000
#endif

/* ---------- what the log reads elsewhere (made visible to it: their own files keep the definitions) */

/* cache_files.c */
struct perf_cache_file_header
{
	unsigned long header_signature;
	long version;
	long file_length;
	byte reservedC[4];
	long tag_data_offset;
	long tag_data_size;
	byte reserved18[8];
	char name[0x20];
	char build[0x20];
	byte reserved60[4];
	unsigned long checksum;
	byte reserved68[0x794];
	unsigned long footer_signature;
};
extern struct
{
	boolean tags_loaded;
	byte pad1[3];
	struct perf_cache_file_header header;
	void *tag_header;
	void *structure_bsp_header;
} cache_file_globals;
typedef char perf_cache_file_header_size_assert[sizeof(struct perf_cache_file_header) == 0x800 ? 1 : -1];

/* xbox_texture_cache.c */
extern struct
{
	struct data_array *textures;
	byte *base_address;
	struct lruv_cache *cache;
	boolean stolen_memory;
} xbox_texture_cache_globals;
extern struct
{
	byte *start;
	long size;
	long gap_block_index;
	long page_count;
} texture_cache_lent;

/* xbox_sound_cache.c */
extern struct
{
	char debug_block_name[0x100];
	struct data_array *cache_sounds;
	byte *base_address;
	struct lruv_cache *cache;
	unsigned long last_allocation_failure_time;
} xbox_sound_cache_globals;

/* the counters the loop's long-frame lines use (main.c), the heaps' (cseries_windows.c, oxbox_libc.c) */
extern unsigned long oxbox_texture_reads, oxbox_texture_read_bytes, oxbox_texture_blocking_reads;
extern unsigned long oxbox_sound_reads, oxbox_sound_read_bytes;
extern unsigned long oxbox_net_bytes_sent, oxbox_net_bytes_received;
extern unsigned long oxbox_system_live, oxbox_system_peak, oxbox_heap_live, oxbox_heap_peak;
extern char __oxperf_start[], __oxperf_end[];

/* ---------- the kernel */

/* MmQueryStatistics' record (export 181) */
struct perf_mm_statistics
{
	unsigned long length, total_pages, available_pages, virtual_bytes_committed, virtual_bytes_reserved,
		cache_pages, pool_pages, stack_pages, image_pages;
};
typedef LONG (__stdcall *perf_statistics_proc)(struct perf_mm_statistics *statistics);
typedef void (__stdcall *perf_deferred_routine)(void *dpc, void *context, void *argument1, void *argument2);
typedef void (__stdcall *perf_initialize_dpc_proc)(void *dpc, perf_deferred_routine routine, void *context);
typedef void (__stdcall *perf_initialize_timer_proc)(void *timer, long type);
typedef unsigned char (__stdcall *perf_set_timer_proc)(void *timer, LARGE_INTEGER due_time, LONG period, void *dpc);
typedef unsigned char (__stdcall *perf_cancel_timer_proc)(void *timer);

enum
{
	_kernel_cancel_timer = 97,
	_kernel_initialize_dpc = 107,
	_kernel_initialize_timer_ex = 113,
	_kernel_set_timer_ex = 150,
	_kernel_mm_query_statistics = 181,
};

/* ---------- constants */

enum
{
#if defined(OXBOX_TEST_PERF_RING)
	RING_SIZE = OXBOX_TEST_PERF_RING,
#else
	RING_SIZE = 0x8000,          /* (a power of two) */
#endif
	LINE_SIZE = 512,
	WRITER_STACK_SIZE = 0x4000,
	WRITER_PERIOD_MS = 2000,
	MAXIMUM_SLOW_LINES_A_SECOND = 30,
	/* the probe marks (main.c) */
	_mark_start = 0,
	_mark_input,
	_mark_network,
	_mark_menus,
	_mark_network_end,
	_mark_ticks,
	_mark_camera,
	_mark_drawing,
	_mark_gpu_wait,
	_mark_present,
	NUMBER_OF_MARKS
};

/* the in-band commands to the writer: a line starting with one of these bytes */
#define WRITER_OPEN '\001'      /* then the file's name in perf\ */
#define WRITER_CLOSE '\002'

/* ---------- structures */

/* one frame, or a second's frames summed */
struct perf_sample
{
	long frames;
	__int64 frame;               /* (counter ticks) */
	__int64 worst;
	__int64 ticks, drawing, present, vsync, other;
	unsigned long gpu_busy, gpu_samples;
	long game_ticks;
	unsigned long texture_bytes, texture_reads, texture_waits, sound_bytes, net_sent, net_received;
	unsigned long log_cycles;    /* the log's own time: its hook and its timer's samples */
};

/* the counters at a moment (a frame's or a second's start) */
struct perf_counters
{
	unsigned long gpu_busy, gpu_samples, gpu_sample_cycles;
	long game_time;
	unsigned long texture_bytes, texture_reads, texture_waits, sound_bytes, net_sent, net_received;
};

/* ---------- globals (all in OXPERF) */

static struct
{
	long slow_frame_ms;
	long gpu_sample_ms;              /* (0: no samples, no gpu_busy_pct) */
	__int64 frequency;
	__int64 last_frame_end;
	struct perf_counters frame_start;
	boolean started;

	/* the map session */
	boolean active;
	unsigned long map_checksum;
	__int64 session_start;
	__int64 second_start;
	struct perf_counters second_counters;
	struct perf_sample second;
	long slow_lines_this_second;
	unsigned long slow_lines_skipped;
	boolean paused, postgame;
	unsigned long last_hook_cycles;

	perf_statistics_proc statistics;
	perf_set_timer_proc set_timer;
	perf_cancel_timer_proc cancel_timer;
	double cycle_frequency;          /* the cycle counter's rate (counts a second) */
	unsigned __int64 session_cycles; /* at the session's start */
	unsigned __int64 session_log_cycles, session_gpu_samples;
	long seconds_reported;
} perf;

/* the graphics chip's busy samples (the timer's DPC; only ever counted up) */
static volatile unsigned long gpu_busy_samples, gpu_samples, gpu_sample_cycles;
static unsigned char gpu_dpc[64] __attribute__((aligned(16))), gpu_timer[64] __attribute__((aligned(16)));

/* the ring: the game thread puts (ring_head), the writer takes (ring_tail) */
static char ring[RING_SIZE];
static volatile unsigned long ring_head, ring_tail;
static volatile unsigned long ring_dropped_bytes;
static volatile long writer_flush_requests;
static volatile long writer_failures;     /* (the game thread logs them) */
static volatile unsigned long writer_last_error, writer_last_length, writer_last_written, writer_last_tail;
/* (for a debugger: what the writer is doing, 1 sleeping, 2 opening, 3 writing, 4 flushing, 5 closing; and its passes) */
volatile unsigned long oxbox_perf_writer_state, oxbox_perf_writer_passes, oxbox_perf_frames;
static volatile char writer_drive;        /* where the files went: 'D', 'T', or 0 */

/* ---------- the ring and the writer */

#define PERF_BARRIER() __asm__ volatile ("" : : : "memory")

/* the CPU's cycle counter (733 MHz on an Xbox; its rate is measured against QueryPerformanceCounter's) */
static unsigned __int64 cycles64(
	void)
{
	unsigned long low, high;

	__asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
	return ((unsigned __int64)high << 32) | low;
}

static unsigned long cycles_now(
	void)
{
	return (unsigned long)cycles64();
}

static void ring_put(
	char const *text,
	unsigned long length)
{
	unsigned long head = ring_head, index;

	if (RING_SIZE - (head - ring_tail) < length)
	{
		ring_dropped_bytes += length;
		return;
	}
	for (index = 0; index < length; index++)
		ring[(head + index) & (RING_SIZE - 1)] = text[index];
	PERF_BARRIER();
	ring_head = head + length;
}

static HANDLE writer_open(
	char const *name)
{
	static char const *const drives[] = { "D:\\perf", "T:\\perf" };
	char path[128];
	long index;

	for (index = 0; index < 2; index++)
	{
		HANDLE file;

		CreateDirectoryA(drives[index], NULL);
		_snprintf(path, sizeof(path), "%s\\%s", drives[index], name);
		path[sizeof(path) - 1] = 0;
		/* (written through to the disk, a batch at a time: no FlushFileBuffers from this thread, which took the
		kernel down in xemu, at a NULL pointer, while the game thread flushed oxbox_log.txt) */
		file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			writer_drive = drives[index][0];
			return file;
		}
	}
	writer_failures++;
	return INVALID_HANDLE_VALUE;
}

static unsigned long __stdcall writer_thread(
	void *unused)
{
	HANDLE file = INVALID_HANDLE_VALUE;
	unsigned long last_write = GetTickCount();
	long flushes_done = 0;

	for (;;)
	{
		unsigned long head, tail, now;
		long flushes;

		oxbox_perf_writer_state = 1;
		Sleep(250);
		oxbox_perf_writer_passes++;
		head = ring_head;
		PERF_BARRIER();
		tail = ring_tail;
		flushes = writer_flush_requests;
		now = GetTickCount();
		if (head == tail ||
			(head - tail < RING_SIZE / 4 && flushes == flushes_done && now - last_write < WRITER_PERIOD_MS))
		{
			continue;
		}
		while (tail != head)
		{
			char c = ring[tail & (RING_SIZE - 1)];

			if (c == WRITER_OPEN || c == WRITER_CLOSE)
			{
				char name[96];
				long length = 0;

				for (tail++; tail != head && ring[tail & (RING_SIZE - 1)] != '\n'; tail++)
				{
					if (length < (long)sizeof(name) - 1)
						name[length++] = ring[tail & (RING_SIZE - 1)];
				}
				name[length] = 0;
				if (tail != head)
					tail++;
				if (file != INVALID_HANDLE_VALUE)
				{
					oxbox_perf_writer_state = 5;
					CloseHandle(file);
					file = INVALID_HANDLE_VALUE;
				}
				oxbox_perf_writer_state = 2;
				if (c == WRITER_OPEN)
					file = writer_open(name);
			}
			else
			{
				/* the text up to the next command, or the ring's end */
				unsigned long run = tail, limit = head, written;
				unsigned long ring_end = (tail | (RING_SIZE - 1)) + 1;

				if (head - tail > ring_end - tail)
					limit = ring_end;
				while (run != limit && ring[run & (RING_SIZE - 1)] != WRITER_OPEN && ring[run & (RING_SIZE - 1)] != WRITER_CLOSE)
					run++;
				written = 0;
				oxbox_perf_writer_state = 3;
				if (file != INVALID_HANDLE_VALUE &&
					(!WriteFile(file, &ring[tail & (RING_SIZE - 1)], run - tail, &written, NULL) || written != run - tail))
				{
					writer_last_error = GetLastError();
					writer_last_length = run - tail;
					writer_last_written = written;
					writer_last_tail = tail;
					writer_failures++;
				}
				tail = run;
			}
		}
		PERF_BARRIER();
		ring_tail = tail;
		/* (the file is written through: a crash or a power-off loses at most the last 2 seconds) */
		flushes_done = flushes;
		last_write = now;
	}
	return 0;
}

/* ---------- the graphics chip's busy share */

static void __stdcall gpu_sample(
	void *dpc,
	void *context,
	void *argument1,
	void *argument2)
{
	/* (a register read in the retail Direct3D, no side effects: D3DDevice_IsBusy compares the chip's read
	pointer with the last kicked-off write pointer, then reads PGRAPH's status) */
	if (global_d3d_device)
	{
		unsigned long start = cycles_now();

		gpu_samples++;
		if (D3DDevice_IsBusy())
			gpu_busy_samples++;
		gpu_sample_cycles += cycles_now() - start;
	}
}

/* ---------- text */

struct perf_line
{
	char text[LINE_SIZE];
	long length;
};

static void put_text(
	struct perf_line *line,
	char const *text)
{
	while (*text && line->length < LINE_SIZE - 3)
		line->text[line->length++] = *text++;
}

static void put_char(
	struct perf_line *line,
	char c)
{
	if (line->length < LINE_SIZE - 3)
		line->text[line->length++] = c;
}

static void put_unsigned(
	struct perf_line *line,
	unsigned long value)
{
	char digits[12];
	long count = 0;

	do
	{
		digits[count++] = (char)('0' + value % 10);
		value /= 10;
	}
	while (value);
	while (count)
		put_char(line, digits[--count]);
}

static void put_signed(
	struct perf_line *line,
	long value)
{
	if (value < 0)
	{
		put_char(line, '-');
		put_unsigned(line, (unsigned long)-value);
	}
	else
	{
		put_unsigned(line, (unsigned long)value);
	}
}

/* value with decimals (1 to 3) after the point */
static void put_fixed(
	struct perf_line *line,
	double value,
	long decimals)
{
	static long const scales[] = { 1, 10, 100, 1000 };
	long scale = scales[decimals];
	double scaled;
	unsigned long whole, fraction;
	long digit;

	if (value != value || value > 2.0e9 || value < -2.0e9)
	{
		return;
	}
	if (value < 0.0)
	{
		put_char(line, '-');
		value = -value;
	}
	scaled = value * scale + 0.5;
	whole = (unsigned long)(scaled / scale);
	fraction = (unsigned long)(scaled - (double)whole * scale);
	if (fraction >= (unsigned long)scale)
	{
		whole++;
		fraction -= scale;
	}
	put_unsigned(line, whole);
	put_char(line, '.');
	for (digit = decimals - 1; digit >= 0; digit--)
		put_char(line, (char)('0' + fraction / scales[digit] % 10));
}

static void put_line(
	struct perf_line *line)
{
	line->text[line->length++] = '\r';
	line->text[line->length++] = '\n';
	ring_put(line->text, line->length);
	line->length = 0;
}

/* ---------- what the rows hold */

static double ticks_to_ms(
	__int64 ticks)
{
	return perf.frequency > 0 ? (double)ticks * 1000.0 / (double)perf.frequency : 0.0;
}

static double cycles_to_ms(
	double cycles)
{
	return perf.cycle_frequency > 0.0 ? cycles * 1000.0 / perf.cycle_frequency : 0.0;
}

static unsigned long free_kb(
	struct perf_mm_statistics *statistics)
{
	struct perf_mm_statistics local;

	if (!statistics)
		statistics = &local;
	memset(statistics, 0, sizeof(*statistics));
	statistics->length = sizeof(*statistics);
	if (perf.statistics)
		perf.statistics(statistics);
	return statistics->available_pages * 4;
}

static void counters_now(
	struct perf_counters *counters)
{
	counters->gpu_busy = gpu_busy_samples;
	counters->gpu_samples = gpu_samples;
	counters->gpu_sample_cycles = gpu_sample_cycles;
	counters->game_time = game_time_get();
	counters->texture_bytes = oxbox_texture_read_bytes;
	counters->texture_reads = oxbox_texture_reads;
	counters->texture_waits = oxbox_texture_blocking_reads;
	counters->sound_bytes = oxbox_sound_read_bytes;
	counters->net_sent = oxbox_net_bytes_sent;
	counters->net_received = oxbox_net_bytes_received;
}

static void counters_since(
	struct perf_sample *sample,
	struct perf_counters const *before,
	struct perf_counters const *now)
{
	sample->gpu_busy += now->gpu_busy - before->gpu_busy;
	sample->gpu_samples += now->gpu_samples - before->gpu_samples;
	sample->log_cycles += now->gpu_sample_cycles - before->gpu_sample_cycles;
	sample->game_ticks += now->game_time - before->game_time;
	sample->texture_bytes += now->texture_bytes - before->texture_bytes;
	sample->texture_reads += now->texture_reads - before->texture_reads;
	sample->texture_waits += now->texture_waits - before->texture_waits;
	sample->sound_bytes += now->sound_bytes - before->sound_bytes;
	sample->net_sent += now->net_sent - before->net_sent;
	sample->net_received += now->net_received - before->net_received;
}

/* a lruv cache's pages in use, less one block's (NONE: none) */
static long cache_used_pages(
	struct lruv_cache *cache,
	long except_block_index)
{
	struct data_iterator iterator;
	struct lruv_cache_block *block;
	long pages = 0;

	if (!cache || !cache->blocks)
		return 0;
	data_iterator_new(&iterator, cache->blocks);
	while ((block = data_iterator_next(&iterator)) != NULL)
	{
		if (iterator.datum_index != except_block_index)
			pages += block->page_count;
	}
	return pages;
}

static long block_pages(
	struct lruv_cache *cache,
	long block_index)
{
	struct data_iterator iterator;
	struct lruv_cache_block *block;

	if (!cache || !cache->blocks || block_index == NONE)
		return 0;
	data_iterator_new(&iterator, cache->blocks);
	while ((block = data_iterator_next(&iterator)) != NULL)
	{
		if (iterator.datum_index == block_index)
			return block->page_count;
	}
	return 0;
}

static char const memory_fields[] =
	"kind,t,free_kb,largest_free_kb,total_kb,image_kb,stacks_kb,pool_kb,file_cache_kb,virtual_kb,"
	"tag_data_kb,bsp_kb,bsp,texture_cache_kb,texture_cache_used_kb,texture_cache_lent_kb,sound_cache_kb,"
	"sound_cache_used_kb,game_heap_kb,game_heap_peak_kb,c_heap_kb,c_heap_peak_kb,perf_log_kb";

static char const frame_fields[] =
	"kind,t,frames,avg_ms,worst_ms,tick_ms,draw_ms,present_ms,vsync_ms,other_ms,gpu_busy_pct,game_ticks,free_kb,"
	"texture_read_kb,texture_reads,texture_waits,sound_read_kb,net_sent_b,net_received_b,x,y,z,yaw,pitch,bsp,"
	"cluster,local_players,log_ms";

static void put_comma(
	struct perf_line *line)
{
	put_char(line, ',');
}

/* the longest run of free physical pages, in pages, from the kernel's page frame database (the last pages of
memory: 0x83FF0000 on a 64 MB console, 0x87FE0000 on 128 MB; a 4-byte entry a page). A free page's entry has
neither bit 0 (an entry in page-table form: contiguous memory) nor bit 16 (busy) set. Checked against the kernel's
own count of free pages: 0 (not known) when they don't agree, give or take a few pages that came or went while
it was read. */
static long largest_free_pages(
	unsigned long total_pages,
	unsigned long available_pages)
{
	unsigned long const *pfn;
	unsigned long index, run = 0, longest = 0, free = 0;

	if (total_pages == 16384)
		pfn = (unsigned long const *)0x83FF0000;
	else if (total_pages == 32768)
		pfn = (unsigned long const *)0x87FE0000;
	else
		return 0;
	for (index = 0; index < total_pages; index++)
	{
		if (pfn[index] & 0x10001)
		{
			run = 0;
		}
		else
		{
			free++;
			if (++run > longest)
				longest = run;
		}
	}
	if (free + 8 < available_pages || free > available_pages + 8)
		return 0;
	return (long)longest;
}

static void memory_row(
	double t)
{
	struct perf_line line;
	struct perf_mm_statistics statistics;
	struct scenario *scenario = global_scenario_try_and_get();
	struct lruv_cache *textures = xbox_texture_cache_globals.cache;
	struct lruv_cache *sounds = xbox_sound_cache_globals.cache;
	long bsp_size = 0, gap_pages, texture_page_kb = 16, sound_page_kb = 4;
	unsigned long free = free_kb(&statistics);

	if (scenario && global_structure_bsp_index >= 0 && global_structure_bsp_index < scenario->structure_bsp_references.count)
	{
		struct scenario_structure_bsp_reference *reference = TAG_BLOCK_GET_ELEMENT(
			&scenario->structure_bsp_references,
			global_structure_bsp_index,
			struct scenario_structure_bsp_reference);

		bsp_size = reference->file_size;
	}
	if (textures)
		texture_page_kb = (1 << textures->page_size_bits) / 1024;
	if (sounds)
		sound_page_kb = (1 << sounds->page_size_bits) / 1024;
	gap_pages = texture_cache_lent.start ? block_pages(textures, texture_cache_lent.gap_block_index) : 0;

	line.length = 0;
	put_text(&line, "M,");
	put_fixed(&line, t, 3); put_comma(&line);
	put_unsigned(&line, free); put_comma(&line);
	{
		long largest = largest_free_pages(statistics.total_pages, statistics.available_pages);

		if (largest)
			put_signed(&line, largest * 4);
		put_comma(&line);
	}
	put_unsigned(&line, statistics.total_pages * 4); put_comma(&line);
	put_unsigned(&line, statistics.image_pages * 4); put_comma(&line);
	put_unsigned(&line, statistics.stack_pages * 4); put_comma(&line);
	put_unsigned(&line, statistics.pool_pages * 4); put_comma(&line);
	put_unsigned(&line, statistics.cache_pages * 4); put_comma(&line);
	put_unsigned(&line, statistics.virtual_bytes_committed / 1024); put_comma(&line);
	put_signed(&line, cache_file_globals.header.tag_data_size / 1024); put_comma(&line);
	put_signed(&line, bsp_size / 1024); put_comma(&line);
	put_signed(&line, global_structure_bsp_index); put_comma(&line);
	put_signed(&line, textures ? (textures->page_count - gap_pages) * texture_page_kb : 0); put_comma(&line);
	put_signed(&line, cache_used_pages(textures, texture_cache_lent.start ? texture_cache_lent.gap_block_index : NONE) *
		texture_page_kb);
	put_comma(&line);
	put_signed(&line, texture_cache_lent.start ? texture_cache_lent.size / 1024 : 0); put_comma(&line);
	put_signed(&line, sounds ? sounds->page_count * sound_page_kb : 0); put_comma(&line);
	put_signed(&line, cache_used_pages(sounds, NONE) * sound_page_kb); put_comma(&line);
	put_unsigned(&line, oxbox_system_live / 1024); put_comma(&line);
	put_unsigned(&line, oxbox_system_peak / 1024); put_comma(&line);
	put_unsigned(&line, oxbox_heap_live / 1024); put_comma(&line);
	put_unsigned(&line, oxbox_heap_peak / 1024); put_comma(&line);
	/* (the log's own: its section's pages and the writer's stack) */
	put_unsigned(&line, (unsigned long)((((__oxperf_end - __oxperf_start) + 0xFFF) & ~0xFFF) + WRITER_STACK_SIZE) / 1024);
	put_line(&line);
}

static void frame_row(
	char kind,
	double t,
	struct perf_sample const *sample)
{
	struct perf_line line;
	double frames = sample->frames > 0 ? (double)sample->frames : 1.0;

	line.length = 0;
	put_char(&line, kind); put_comma(&line);
	put_fixed(&line, t, 3); put_comma(&line);
	put_signed(&line, sample->frames); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->frame) / frames, 2); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->worst), 2); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->ticks) / frames, 2); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->drawing) / frames, 2); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->present) / frames, 2); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->vsync) / frames, 2); put_comma(&line);
	put_fixed(&line, ticks_to_ms(sample->other) / frames, 2); put_comma(&line);
	if (sample->gpu_samples)
		put_signed(&line, (long)((sample->gpu_busy * 100 + sample->gpu_samples / 2) / sample->gpu_samples));
	put_comma(&line);
	put_signed(&line, sample->game_ticks); put_comma(&line);
	put_unsigned(&line, free_kb(NULL)); put_comma(&line);
	put_unsigned(&line, sample->texture_bytes / 1024); put_comma(&line);
	put_unsigned(&line, sample->texture_reads); put_comma(&line);
	put_unsigned(&line, sample->texture_waits); put_comma(&line);
	put_unsigned(&line, sample->sound_bytes / 1024); put_comma(&line);
	put_unsigned(&line, sample->net_sent); put_comma(&line);
	put_unsigned(&line, sample->net_received); put_comma(&line);
	if (game_in_progress() && global_structure_bsp_index != NONE)
	{
		struct observer_result const *camera = observer_get_camera(0);

		if (camera)
		{
			double yaw = atan2(camera->forward.j, camera->forward.i) * (180.0 / 3.14159265358979);
			double k = camera->forward.k;

			if (yaw < 0.0)
				yaw += 360.0;
			k = k > 1.0 ? 1.0 : k < -1.0 ? -1.0 : k;
			put_fixed(&line, camera->position.x, 2); put_comma(&line);
			put_fixed(&line, camera->position.y, 2); put_comma(&line);
			put_fixed(&line, camera->position.z, 2); put_comma(&line);
			put_fixed(&line, yaw, 1); put_comma(&line);
			put_fixed(&line, asin(k) * (180.0 / 3.14159265358979), 1); put_comma(&line);
			put_signed(&line, global_structure_bsp_index); put_comma(&line);
			put_signed(&line, camera->location.cluster_index); put_comma(&line);
		}
		else
		{
			put_text(&line, ",,,,,,,");
		}
	}
	else
	{
		put_text(&line, ",,,,,,,");
	}
	put_signed(&line, local_player_count()); put_comma(&line);
	put_fixed(&line, cycles_to_ms((double)sample->log_cycles) / frames, 3);
	put_line(&line);
}

/* ---------- the map session */

static boolean session_wanted(
	void)
{
	struct scenario *scenario;

	if (!game_in_progress())
		return FALSE;
	scenario = global_scenario_try_and_get();
	return scenario && scenario->type != _scenario_type_main_menu && cache_file_globals.tags_loaded;
}

static void session_start(
	__int64 now)
{
	struct perf_line line;
	SYSTEMTIME time;
	char name[96], map[0x20];
	long index;

	/* the map's name, for a file name: letters, digits, - and _ */
	for (index = 0; index < (long)sizeof(map) - 1 && cache_file_globals.header.name[index]; index++)
	{
		char c = cache_file_globals.header.name[index];

		map[index] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ? c : '_';
	}
	map[index] = 0;
	if (!index)
		csstrcpy(map, "map");
	GetSystemTime(&time);
	_snprintf(name, sizeof(name), "%s_%04d%02d%02d-%02d%02d%02d.csv", map, time.wYear, time.wMonth, time.wDay,
		time.wHour, time.wMinute, time.wSecond);
	name[sizeof(name) - 1] = 0;

	line.length = 0;
	put_char(&line, WRITER_OPEN);
	put_text(&line, name);
	line.text[line.length++] = '\n';
	ring_put(line.text, line.length);
	line.length = 0;

	put_text(&line, "# Halo 1 Unlimited perf log v1");
	put_line(&line);
	put_text(&line, "# build v1.");
	put_unsigned(&line, OXBOX_BUILD_NUMBER);
	put_text(&line, " (OpenCE on the Xbox, test build ");
	put_unsigned(&line, OXBOX_BUILD_NUMBER);
	put_char(&line, ')');
	put_line(&line);
	put_text(&line, "# map ");
	put_text(&line, cache_file_globals.header.name[0] ? map : "?");
	put_char(&line, ' ');
	put_signed(&line, cache_file_globals.header.file_length);
	put_text(&line, " 0x");
	for (index = 28; index >= 0; index -= 4)
		put_char(&line, "0123456789abcdef"[(cache_file_globals.header.checksum >> index) & 15]);
	put_line(&line);
	{
		char started[96];

		_snprintf(started, sizeof(started), "# started %04d-%02d-%02d %02d:%02d:%02d UTC; slow frame %ld ms; gpu sample %ld ms",
			time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, perf.slow_frame_ms, perf.gpu_sample_ms);
		started[sizeof(started) - 1] = 0;
		put_text(&line, started);
		put_line(&line);
	}
	put_text(&line, "# memory_fields ");
	put_text(&line, memory_fields);
	put_line(&line);
	put_text(&line, "# fields ");
	put_text(&line, frame_fields);
	put_line(&line);

	perf.active = TRUE;
	perf.map_checksum = cache_file_globals.header.checksum;
	perf.session_start = now;
	perf.session_cycles = cycles64();
	perf.session_log_cycles = 0;
	perf.seconds_reported = 0;
	perf.session_gpu_samples = gpu_samples;
	perf.second_start = now;
	counters_now(&perf.second_counters);
	memset(&perf.second, 0, sizeof(perf.second));
	perf.slow_lines_this_second = 0;
	perf.paused = perf.postgame = FALSE;
	memory_row(0.0);
	/* the graphics chip's busy share: a sample every perf_gpu_sample_ms while a map plays (not at start-up, while Direct3D is
	still setting the chip up: xemu stops on a read of its channel then) */
	if (perf.set_timer && perf.gpu_sample_ms > 0)
	{
		LARGE_INTEGER due;

		due.QuadPart = -10000 * (__int64)perf.gpu_sample_ms;   /* (in 100 ns, from now) */
		perf.set_timer(gpu_timer, due, perf.gpu_sample_ms, gpu_dpc);
	}
	writer_flush_requests++;
	platform_log("perf: logging %s to perf\\%s", map, name);
}

/* what the log cost so far this session: its hook and its samples, measured on the console (oxbox_log.txt) */
static void cost_report(
	void)
{
	LARGE_INTEGER now;
	double seconds, log_ms;
	long hundredths;

	QueryPerformanceCounter(&now);
	seconds = ticks_to_ms(now.QuadPart - perf.session_start) / 1000.0;
	log_ms = cycles_to_ms((double)(__int64)perf.session_log_cycles);
	hundredths = seconds > 0.0 ? (long)(log_ms / (seconds * 1000.0) * 10000.0 + 0.5) : 0;
	platform_log("perf: the log has taken %ld ms of the session's %ld s (%ld.%02ld%%), %lu GPU samples; "
		"QueryPerformanceFrequency %ld, the cycle counter %ld Hz", (long)log_ms, (long)seconds, hundredths / 100,
		hundredths % 100, gpu_samples - (unsigned long)perf.session_gpu_samples, (long)perf.frequency,
		(long)perf.cycle_frequency);
}

static void session_end(
	void)
{
	static char const close_line[] = { WRITER_CLOSE, '\n' };

	if (perf.cancel_timer)
		perf.cancel_timer(gpu_timer);
	cost_report();
	ring_put(close_line, sizeof(close_line));
	writer_flush_requests++;
	perf.active = FALSE;
	if (perf.slow_lines_skipped || ring_dropped_bytes)
	{
		platform_log("perf: %lu slow-frame lines skipped (more than %d a second), %lu bytes dropped (the ring was full)",
			perf.slow_lines_skipped, MAXIMUM_SLOW_LINES_A_SECOND, ring_dropped_bytes);
	}
	perf.slow_lines_skipped = 0;
}

/* ---------- the hook */

static void perf_frame(
	void const *probe_marks,
	long mark_count)
{
	LARGE_INTEGER now_counter;
	__int64 marks[NUMBER_OF_MARKS], now, frame;
	struct perf_counters counters;
	struct perf_sample sample;
	long index;
	boolean wanted, paused, postgame;

	unsigned long hook_start = cycles_now();

	oxbox_perf_frames++;

	QueryPerformanceCounter(&now_counter);
	now = now_counter.QuadPart;
	counters_now(&counters);
	if (!perf.started || mark_count != NUMBER_OF_MARKS)
	{
		perf.started = TRUE;
		perf.last_frame_end = now;
		perf.frame_start = counters;
		return;
	}
	frame = now - perf.last_frame_end;
	perf.last_frame_end = now;

	if (writer_failures)
	{
		platform_log("perf: %ld writes to the log failed (D:\\perf and T:\\perf; the last: error %lu, %lu of %lu bytes at %lu)",
			writer_failures, writer_last_error, writer_last_written, writer_last_length, writer_last_tail);
		writer_failures = 0;
	}

	wanted = session_wanted();
	if (perf.active && (!wanted || cache_file_globals.header.checksum != perf.map_checksum))
		session_end();
	if (!perf.active && wanted)
		session_start(now);
	if (!perf.active)
	{
		perf.frame_start = counters;
		return;
	}

	/* the frame's steps (a step it skipped has the mark before it) */
	for (index = 0; index < NUMBER_OF_MARKS; index++)
	{
		marks[index] = ((LARGE_INTEGER const *)probe_marks)[index].QuadPart;
		if (index && marks[index] < marks[index - 1])
			marks[index] = marks[index - 1];
	}
	memset(&sample, 0, sizeof(sample));
	sample.frames = 1;
	sample.log_cycles = perf.last_hook_cycles;
	sample.frame = frame;
	sample.worst = frame;
	sample.ticks = marks[_mark_ticks] - marks[_mark_network_end];
	sample.drawing = marks[_mark_drawing] - marks[_mark_camera];
	sample.vsync = marks[_mark_gpu_wait] - marks[_mark_drawing];
	sample.present = marks[_mark_present] - marks[_mark_gpu_wait];
	sample.other = frame - sample.ticks - sample.drawing - sample.vsync - sample.present;
	if (sample.other < 0)
		sample.other = 0;
	counters_since(&sample, &perf.frame_start, &counters);
	perf.frame_start = counters;

	/* this second */
	perf.second.frames++;
	perf.second.frame += sample.frame;
	if (sample.frame > perf.second.worst)
		perf.second.worst = sample.frame;
	perf.second.ticks += sample.ticks;
	perf.second.drawing += sample.drawing;
	perf.second.present += sample.present;
	perf.second.vsync += sample.vsync;
	perf.second.other += sample.other;
	perf.second.log_cycles += perf.last_hook_cycles;

	/* a slow frame: a line of its own */
	if (ticks_to_ms(frame) > (double)perf.slow_frame_ms)
	{
		if (perf.slow_lines_this_second < MAXIMUM_SLOW_LINES_A_SECOND)
		{
			perf.slow_lines_this_second++;
			frame_row('S', ticks_to_ms(now - perf.session_start) / 1000.0, &sample);
		}
		else
		{
			perf.slow_lines_skipped++;
		}
	}

	/* a second's summary */
	if (now - perf.second_start >= perf.frequency)
	{
		/* (the cycle counter's rate, from the session so far) */
		if (now - perf.session_start >= perf.frequency)
		{
			perf.cycle_frequency = (double)(__int64)(cycles64() - perf.session_cycles) * (double)perf.frequency /
				(double)(now - perf.session_start);
		}
		counters_since(&perf.second, &perf.second_counters, &counters);
		perf.session_log_cycles += perf.second.log_cycles;
		if (++perf.seconds_reported % 60 == 0)
			cost_report();
		frame_row('P', ticks_to_ms(now - perf.session_start) / 1000.0, &perf.second);
		perf.second_start = now;
		perf.second_counters = counters;
		memset(&perf.second, 0, sizeof(perf.second));
		perf.slow_lines_this_second = 0;
	}

	/* to the disk at once at round end and on pause */
	paused = shell_application_is_paused() || game_time_get_paused();
	postgame = game_engine_showing_postgame();
	if ((paused && !perf.paused) || (postgame && !perf.postgame))
		writer_flush_requests++;
	perf.paused = paused;
	perf.postgame = postgame;
	perf.last_hook_cycles = cycles_now() - hook_start;
}

static struct oxbox_perf_hooks perf_hooks = { perf_frame };

/* ---------- start-up */

/* opence.ini's perf_ settings (oxbox_entry.c reads perf_log, and the others are only read here) */
static void read_settings(
	char const *path)
{
	char text[1024], *p, *e;
	unsigned long got = 0;
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file == INVALID_HANDLE_VALUE)
		return;
	if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
		got = 0;
	CloseHandle(file);
	text[got] = 0;
	for (p = text; *p; p = e)
	{
		char name[32];
		long n = 0, value = 0, digits = 0;

		for (e = p; *e && *e != '\n'; e++) ;
		if (*e)
			*e++ = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		while (*p && *p != '=' && *p != ' ' && *p != '\t' && n < (long)sizeof(name) - 1)
			name[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p), p++;
		name[n] = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p++ != '=')
			continue;
		while (*p == ' ' || *p == '\t')
			p++;
		for (; *p >= '0' && *p <= '9' && digits < 6; p++, digits++)
			value = value * 10 + (*p - '0');
		if (!digits)
			continue;
		if (!strcmp(name, "perf_slow_frame_ms") && value > 0)
			perf.slow_frame_ms = value;
		else if (!strcmp(name, "perf_gpu_sample_ms"))
			perf.gpu_sample_ms = value > 100 ? 100 : value;
	}
}

struct oxbox_perf_hooks *oxbox_perf_start(
	void)
{
	perf_initialize_dpc_proc initialize_dpc = (perf_initialize_dpc_proc)oxbox_kernel_export(_kernel_initialize_dpc);
	perf_initialize_timer_proc initialize_timer = (perf_initialize_timer_proc)oxbox_kernel_export(_kernel_initialize_timer_ex);
	perf_set_timer_proc set_timer = (perf_set_timer_proc)oxbox_kernel_export(_kernel_set_timer_ex);
	perf_cancel_timer_proc cancel_timer = (perf_cancel_timer_proc)oxbox_kernel_export(_kernel_cancel_timer);
	LARGE_INTEGER frequency;
	HANDLE writer;

	/* (the section's zero-filled part: the kernel gives fresh pages, but to be sure) */
	memset(&perf, 0, sizeof(perf));
	perf.slow_frame_ms = 34;
	perf.gpu_sample_ms = 1;
	perf.cycle_frequency = 733333333.0;
	read_settings("D:\\opence.ini");
	read_settings("T:\\opence.ini");
	QueryPerformanceFrequency(&frequency);
	perf.frequency = frequency.QuadPart;
	perf.statistics = (perf_statistics_proc)oxbox_kernel_export(_kernel_mm_query_statistics);

	writer = CreateThread(NULL, WRITER_STACK_SIZE, writer_thread, NULL, 0, NULL);
	if (!writer)
	{
		platform_log("perf: no writer thread: the log is off");
		return NULL;
	}
	CloseHandle(writer);

	/* the graphics chip's busy share: its timer, set while a map plays (session_start) */
	if (initialize_dpc && initialize_timer && set_timer && cancel_timer)
	{
		initialize_dpc(gpu_dpc, gpu_sample, NULL);
		initialize_timer(gpu_timer, 0);
		perf.set_timer = set_timer;
		perf.cancel_timer = cancel_timer;
	}
	else
	{
		platform_log("perf: the kernel's timer functions weren't found: no gpu_busy_pct");
	}
	platform_log("perf: on (OXPERF loaded at %p, %lu KB, and a %lu KB writer stack); slow frames over %ld ms; "
		"a GPU sample every %ld ms", __oxperf_start, (unsigned long)(((__oxperf_end - __oxperf_start) + 0xFFF) & ~0xFFF) / 1024,
		(unsigned long)WRITER_STACK_SIZE / 1024, perf.slow_frame_ms, perf.gpu_sample_ms);
	return &perf_hooks;
}
