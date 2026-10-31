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
frame), T (a camera tour's view, test build 34). P, S and T have the same columns; an empty field isn't available.

Timing comes from the game loop's probe marks (main.c frame_probe_marks): the game's ticks (game_time_update: the
simulation), drawing (main_game_render: the CPU building the frame for the graphics chip), present (the CPU blocked
in IDirect3DDevice8_Present while the chip is behind), vsync (the frame-rate throttle spinning to the vertical
blank: idle) and the rest (input, network, menus, camera). The graphics chip itself has no timestamps: a 1 ms
kernel timer asks Direct3D whether it's busy (D3DDevice_IsBusy: the chip hasn't caught up with the commands sent
to it), which gives the share of the time it was working, gpu_busy_pct.

The game thread never touches the hard drive: it puts rows in a 32 KB ring, and a writer thread of its own writes
them out every 2 seconds (at once at round end and on pause), so the log can't make the stutters it measures.

Test build 32 (phase 2): the P rows also have the time of each step of a game tick and of a player's render
window, in ms a frame (oxbox_perf.h: the marks in game.c game_tick and render.c render_window, and the game's own
profile sections for the visibility traversals and the level's lights and shadows), and counts a frame (objects
drawn and updated, actors, effects, particles, dynamic lights, the level's triangles and clusters drawn, the
dynamic triangles used of 32768). Sums over the row's second: S rows leave them empty.

Test build 33 (phase 3), more columns at the rows' end:
- other_ms split by the frame's own marks (both rows); in S rows the writer thread's state and timing.
- objects_update's steps (P rows): the activation pass, each object's update steps (summed over all objects,
  children too), its top-level updates by the object's type (each with its children), the children, the created
  objects, object_translate, the garbage collection.
- S rows with a tick over 25 ms fill in the tick steps for that frame.
- The weather's counts and the HUD's parts (P rows).

Test build 34: the camera tour (T rows). A text file a map in perf\tours (D:, else T:), named as the map is found
under maps\: a stock map's name (maps\sidewinder.map: perf\tours\sidewinder.txt), a folder map's folder
(maps\Precipice\: perf\tours\Precipice.txt; a pack's, maps\unsorted\Precipice\: perf\tours\unsorted\Precipice.txt),
lists stops: the view flies to each, waits for it to settle, then measures it for a few seconds and writes a T
row (the P row's columns, summed over the stop's frames, and the stop's number, view and label at the end). A tour
starts by itself a few seconds after its map loads, and Back + the right stick's click stops it. The same combo when
no tour runs adds the current view to the map's tour file. The writer thread reads and appends the files: the game
thread still never touches the disk. The format (tour_parse):
	# a comment (anything after # on a line)
	start 5            seconds after the map loads before the tour starts (once, anywhere in the file)
	settle 1.5         seconds at each view before measuring (for the stops after it)
	hold 3             seconds measured at each view (for the stops after it)
	player x y z yaw   where the player stands during the tour (default: the map's first spawn point; yaw in
	                   degrees, optional); "player none" leaves the player where it is
	x y z yaw pitch label...   a stop: world units, then degrees (yaw 0 = +x, counterclockwise; pitch up positive),
	                   as the P rows' columns; a yaw of * looks around (4 views: yaw 0, 90, 180 and 270); pitch and
	                   the label can be left out. Spaces, tabs, commas or semicolons between the numbers.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "camera/observer.h"
#include "memory/data.h"
#include "memory/lruv_cache.h"
#include "cseries/profile.h"
#include "rasterizer/rasterizer.h"
#include "ai/actors.h"
#include "effects/effects.h"
#include "effects/particles.h"
#include "objects/objects.h"
#include "render/render.h"
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "shell/shell.h"
#include "input/input.h"
#include "interface/hud_messaging.h"
#include "units/units.h"
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

/* phase 2's counts: render_objects.c, rasterizer_xbox_draw_primitives.c (their own files define them), effects.c,
object_lights.c */
extern struct
{
	short rendered_object_count;
} render_object_globals;
extern struct
{
	struct
	{
		long triangle_start_index;
		long triangle_count;
		short *triangles;
	} buffers[RASTERIZER_MAXIMUM_DYNAMIC_TRIANGLE_BUFFERS];
	long buffer_count;
	long triangle_count;
} dynamic_triangles;
extern struct data_array *effect_data;
extern short debug_rasterizer_light_count;

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
	LINE_SIZE = 1536,
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
#define WRITER_TOUR_LOAD '\003' /* then the map's name: read its tour file (test build 34) */
#define WRITER_TOUR_ADD '\004'  /* then the map's name, a tab and a line to add to its tour file */
#define WRITER_COMMAND(c) ((c) >= WRITER_OPEN && (c) <= WRITER_TOUR_ADD)

/* ---------- structures */

/* one frame, or a second's frames summed */
struct perf_sample
{
	long frames;
	__int64 frame;               /* (counter ticks) */
	__int64 worst;
	__int64 ticks, drawing, present, vsync, other;
	/* (other's parts, test build 33: the frame's marks) */
	__int64 other_input, other_network, other_menus, other_network_end, other_camera, other_after_present, other_between;
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

/* phase 2: the game's profile sections the log times (the others are left alone), the steps the marks end, and the
counts; summed over a P row's second (window) */
enum
{
	_section_game_update,     /* (a tick's start: its steps are timed from it) */
	_section_visibility_portal,
	_section_visibility_subcluster,
	_section_visibility_surface,
	_section_structure_diffuse_lights,
	_section_structure_specular_lights,
	_section_structure_shadows,
	_section_objects_update,  /* (objects_update's steps are timed from its start) */
	NUMBER_OF_SECTIONS,

	/* (a profile section's field_C: this tag and its slot, 0xFFFF for one the log leaves alone) */
	SECTION_SLOT_TAG = 0x7E3A0000
};
static char const *const section_names[NUMBER_OF_SECTIONS] =
{
	"game_update",
	"render_structure_visibility_portal_traversal",
	"render_structure_visibility_subcluster_traversal",
	"render_structure_visibility_surface_traversal",
	"render_structure_diffuse_lights",
	"render_structure_specular_lights",
	"render_structure_shadows",
	"objects_update",
};
static struct
{
	unsigned __int64 steps[NUMBER_OF_OXBOX_PERF_STEPS];
	unsigned __int64 sections[NUMBER_OF_SECTIONS];
	unsigned long mark_last;
	unsigned long objects_drawn, objects_updated, actors, effects, particles, dynamic_lights, level_triangles,
		clusters, dynamic_triangles, dynamic_triangles_peak, windows;
	/* (test build 33) */
	unsigned __int64 object_steps[NUMBER_OF_OXBOX_PERF_OBJECT_STEPS];
	unsigned long object_updates, activation_runs;
	unsigned __int64 type_cycles[NUMBER_OF_OBJECT_TYPES];
	unsigned long type_counts[NUMBER_OF_OBJECT_TYPES];
	unsigned __int64 loop_cycles, children_cycles, translate_cycles;
	unsigned long children_begun, created_count, translate_count, translate_cluster_queries, translate_clusters;
	unsigned long weather_particles, weather_sprites, weather_boxes, weather_polyhedra;
	unsigned long hook_calls, object_marks;
} window;

/* objects_update's top-level update under way (the object hook) and the children's updates' starts */
static struct
{
	long root_type;
	long root_definition_index;
	unsigned long root_start;
	long children_depth;
	unsigned long children_start;
} objects = { NONE, NONE, 0, 0, 0 };

/* the top-level updates' time by object tag over the window (open addressing on the tag's absolute index) */
enum { TAG_SLOTS = 256 };
static struct
{
	long definition_index;   /* (NONE: empty) */
	unsigned long count;
	unsigned __int64 cycles;
} tag_slots[TAG_SLOTS];

static void tag_slots_clear(
	void)
{
	long index;

	for (index = 0; index < TAG_SLOTS; index++)
	{
		tag_slots[index].definition_index = NONE;
		tag_slots[index].count = 0;
		tag_slots[index].cycles = 0;
	}
}

static void tag_slot_add(
	long definition_index,
	unsigned long cycles)
{
	long index = definition_index & (TAG_SLOTS - 1), probes;

	for (probes = 0; probes < TAG_SLOTS; probes++, index = (index + 1) & (TAG_SLOTS - 1))
	{
		if (tag_slots[index].definition_index == definition_index || tag_slots[index].definition_index == NONE)
		{
			tag_slots[index].definition_index = definition_index;
			tag_slots[index].count++;
			tag_slots[index].cycles += cycles;
			return;
		}
	}
}

/* the tick steps of the frame under way (S rows of a long tick) */
static unsigned __int64 frame_tick_steps[_oxbox_perf_tick_other + 1];

/* what a hook costs, measured at start-up: an inline object mark and a hook call, in cycles */
static unsigned long inline_mark_cycles, hook_call_cycles;

/* the writer's last pass (QueryPerformanceCounter: S rows) */
static volatile __int64 writer_pass_start, writer_pass_end;

static struct oxbox_perf_hooks perf_hooks;

static void window_reset(void);
static unsigned long session_hook_calls, session_frames, hook_calls_at_window_start;
static void frame_counts(long game_ticks);

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
/* (for a debugger and S rows: what the writer is doing, 1 sleeping, 2 opening, 3 writing, 4 flushing, 5 closing, 6 reading
a tour file, 7 adding to one; and its passes) */
volatile unsigned long oxbox_perf_writer_state, oxbox_perf_writer_passes, oxbox_perf_frames;
static volatile char writer_drive;        /* where the files went: 'D', 'T', or 0 */

/* ---------- the camera tour's state (test build 34) */

enum
{
	TOUR_MAXIMUM_STOPS = 64,
	TOUR_LABEL_SIZE = 32,
	TOUR_FILE_SIZE = 6144,
	TOUR_LOOK_AROUND_VIEWS = 4,
	/* the game thread's tour */
	_tour_none = 0,           /* no tour this session (or its file not read yet) */
	_tour_waiting,            /* the file's read: until the start time */
	_tour_settling,           /* at a view, before measuring */
	_tour_measuring,          /* at a view: the T row's frames */
	_tour_done
};

struct tour_stop
{
	real_point3d position;
	real yaw, pitch;          /* (degrees) */
	real settle, hold;        /* (seconds) */
	boolean look_around;
	char label[TOUR_LABEL_SIZE];
};

/* the writer fills these from the file, then sets loaded_request to the request it answered */
static struct
{
	struct tour_stop stops[TOUR_MAXIMUM_STOPS];
	long stop_count;
	long lines_skipped, first_skipped_line, stops_dropped;
	real start_delay;
	boolean found, truncated;
	boolean player_set, player_none;  /* (the player line: a spot, or none) */
	real_point3d player_position;
	real player_yaw;                  /* (degrees) */
	char path[64];            /* (the file read, or where a stop added this session goes) */
	volatile long loaded_request;
} tour_file;

static char tour_text[TOUR_FILE_SIZE];   /* (the writer's) */

/* the game thread's */
static struct
{
	long request;             /* (the last load asked of the writer) */
	long state;
	char map[0x20];
	long stop_index, view_index;
	__int64 phase_start;
	short local_player_index;
	boolean camera_valid;
	struct observer_result camera;
	long stops_added;         /* (this session, by the combo) */
	/* the player's spot during the tour (pin: moving it at all) */
	boolean pin, pin_settled, pin_set_invulnerable;
	real_point3d pin_position, pin_actual;
	real pin_yaw;             /* (radians) */
	long pin_unit;
	__int64 pin_time;
	long pins;                /* (the times it was put back) */
	__int64 started;          /* (QueryPerformanceCounter: GetTickCount ran slow in xemu) */
	long rows;
} tour;

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

/* ---------- the camera tour's file (the writer's side, test build 34) */

static boolean tour_separator(
	char c)
{
	return c == ' ' || c == '\t' || c == ',' || c == ';';
}

/* a number at *text ("-12.5", ".5", "3"): its value, and *text past it; FALSE (and *text as it was) if none */
static boolean tour_number(
	char const **text,
	real *value)
{
	char const *p = *text;
	boolean negative = FALSE, digits = FALSE;
	double result = 0.0, scale = 1.0;

	if (*p == '-' || *p == '+')
		negative = *p++ == '-';
	for (; *p >= '0' && *p <= '9'; p++)
	{
		result = result * 10.0 + (*p - '0');
		digits = TRUE;
	}
	if (*p == '.')
	{
		for (p++; *p >= '0' && *p <= '9'; p++)
		{
			scale *= 0.1;
			result += (*p - '0') * scale;
			digits = TRUE;
		}
	}
	if (!digits || (*p && !tour_separator(*p)))
		return FALSE;
	*value = (real)(negative ? -result : result);
	*text = p;
	return TRUE;
}

static void tour_skip(
	char const **text)
{
	while (**text && tour_separator(**text))
		(*text)++;
}

/* the file's text (the format: this file's header comment) into tour_file's stops */
static void tour_parse(
	char *text)
{
	real settle = 1.5f, hold = 3.0f;
	long line_number = 0;
	char *line = text;

	while (*line)
	{
		char *end, *cut;
		char const *p;
		real value;

		for (end = line; *end && *end != '\n'; end++) ;
		if (*end)
			*end++ = 0;
		line_number++;
		if ((cut = strchr(line, '#')) != NULL)
			*cut = 0;
		for (cut = line + strlen(line); cut > line && (cut[-1] == '\r' || tour_separator(cut[-1])); )
			*--cut = 0;
		p = line;
		tour_skip(&p);
		if (!*p)
		{
			line = end;
			continue;
		}
		if ((*p | 32) >= 'a' && (*p | 32) <= 'z')
		{
			/* an option: start, settle or hold, then seconds */
			char word[8];
			long length = 0;

			while ((*p | 32) >= 'a' && (*p | 32) <= 'z' && length < (long)sizeof(word) - 1)
				word[length++] = (char)(*p++ | 32);
			word[length] = 0;
			tour_skip(&p);
			if (!strcmp(word, "player"))
			{
				/* the player's spot during the tour: x y z [yaw], or none */
				real_point3d position;
				real yaw = 0.0f;

				if ((p[0] | 32) == 'n' && (p[1] | 32) == 'o' && (p[2] | 32) == 'n' && (p[3] | 32) == 'e' &&
					(!p[4] || tour_separator(p[4])))
				{
					tour_file.player_none = TRUE;
					tour_file.player_set = FALSE;
					line = end;
					continue;
				}
				if (tour_number(&p, &position.x) && (tour_skip(&p), tour_number(&p, &position.y)) &&
					(tour_skip(&p), tour_number(&p, &position.z)))
				{
					tour_skip(&p);
					if (!*p || (tour_number(&p, &yaw) && (tour_skip(&p), !*p)))
					{
						tour_file.player_position = position;
						tour_file.player_yaw = yaw;
						tour_file.player_set = TRUE;
						tour_file.player_none = FALSE;
						line = end;
						continue;
					}
				}
			}
			else if (tour_number(&p, &value) && value >= 0.0f && value <= 600.0f)
			{
				tour_skip(&p);
				if (!*p && !strcmp(word, "start"))
				{
					tour_file.start_delay = value;
					line = end;
					continue;
				}
				if (!*p && !strcmp(word, "settle"))
				{
					settle = value > 60.0f ? 60.0f : value;
					line = end;
					continue;
				}
				if (!*p && !strcmp(word, "hold"))
				{
					hold = value < 0.5f ? 0.5f : value > 60.0f ? 60.0f : value;
					line = end;
					continue;
				}
			}
		}
		else
		{
			/* a stop: x y z, then yaw (or *), pitch, the label */
			struct tour_stop stop;
			boolean ok;

			memset(&stop, 0, sizeof(stop));
			ok = tour_number(&p, &stop.position.x);
			tour_skip(&p);
			ok = ok && tour_number(&p, &stop.position.y);
			tour_skip(&p);
			ok = ok && tour_number(&p, &stop.position.z);
			tour_skip(&p);
			if (ok)
			{
				if (*p == '*' && (!p[1] || tour_separator(p[1])))
				{
					stop.look_around = TRUE;
					p++;
				}
				else if (!tour_number(&p, &stop.yaw))
				{
					ok = FALSE;
				}
			}
			tour_skip(&p);
			if (ok && tour_number(&p, &value))
			{
				stop.pitch = value < -89.0f ? -89.0f : value > 89.0f ? 89.0f : value;
				tour_skip(&p);
			}
			if (ok)
			{
				long length = 0;

				/* the label: the rest of the line (a CSV field: no commas) */
				for (; *p && length < TOUR_LABEL_SIZE - 1; p++)
					stop.label[length++] = *p == ',' ? ';' : *p == '\t' ? ' ' : *p;
				stop.label[length] = 0;
				stop.settle = settle;
				stop.hold = hold;
				if (tour_file.stop_count < TOUR_MAXIMUM_STOPS)
					tour_file.stops[tour_file.stop_count++] = stop;
				else
					tour_file.stops_dropped++;
				line = end;
				continue;
			}
		}
		if (!tour_file.lines_skipped++)
			tour_file.first_skipped_line = line_number;
		line = end;
	}
}

/* WRITER_TOUR_LOAD's argument, "<request> <name>": the map's tour file, <name>.txt (a folder map's: a path) in
D:\perf\tours, else T:\perf\tours */
static void tour_load(
	char const *argument)
{
	char const *map = argument;
	long request = 0, index;

	while (*map >= '0' && *map <= '9')
		request = request * 10 + (*map++ - '0');
	if (*map == ' ')
		map++;
	tour_file.stop_count = tour_file.lines_skipped = tour_file.first_skipped_line = tour_file.stops_dropped = 0;
	tour_file.start_delay = 5.0f;
	tour_file.found = tour_file.truncated = FALSE;
	tour_file.player_set = tour_file.player_none = FALSE;
	for (index = 0; index < 2 && !tour_file.found; index++)
	{
		HANDLE file;
		unsigned long got = 0;

		_snprintf(tour_file.path, sizeof(tour_file.path), "%c:\\perf\\tours\\%s.txt", "DT"[index], map);
		tour_file.path[sizeof(tour_file.path) - 1] = 0;
		file = CreateFileA(tour_file.path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		if (!ReadFile(file, tour_text, sizeof(tour_text) - 1, &got, NULL))
			got = 0;
		tour_file.truncated = got == sizeof(tour_text) - 1 && GetFileSize(file, NULL) > got;
		CloseHandle(file);
		tour_text[got] = 0;
		tour_file.found = TRUE;
		tour_parse(tour_text);
	}
	if (!tour_file.found)
	{
		/* (where the combo's stops go: beside the CSV files) */
		_snprintf(tour_file.path, sizeof(tour_file.path), "%c:\\perf\\tours\\%s.txt", writer_drive ? writer_drive : 'D', map);
		tour_file.path[sizeof(tour_file.path) - 1] = 0;
	}
	PERF_BARRIER();
	tour_file.loaded_request = request;
}

/* WRITER_TOUR_ADD's argument: a line for the end of the session's tour file (a new file gets a header first) */
static void tour_add(
	char const *line)
{
	static char const header[] =
		"# Halo 1 Unlimited camera tour v1 (README_OpenCE_test_34.txt)\r\n"
		"# x y z yaw pitch label: a stop (a yaw of * looks around: 4 views); start, settle, hold <seconds>: options\r\n";
	char folder[sizeof(tour_file.path)];
	unsigned long written = 0, size, length = strlen(line);
	long index;
	HANDLE file;

	/* (the folders on the way: perf, perf\tours, a pack's) */
	for (index = 3; tour_file.path[index]; index++)
	{
		if (tour_file.path[index] == '\\')
		{
			memcpy(folder, tour_file.path, index);
			folder[index] = 0;
			CreateDirectoryA(folder, NULL);
		}
	}
	file = CreateFileA(tour_file.path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		writer_last_error = GetLastError();
		writer_failures++;
		return;
	}
	size = GetFileSize(file, NULL);
	if (size == 0 || size == 0xFFFFFFFF)
	{
		WriteFile(file, header, sizeof(header) - 1, &written, NULL);
	}
	else
	{
		/* (a file whose last line has no line end gets one first) */
		char last = '\n';

		SetFilePointer(file, -1, NULL, FILE_END);
		if (ReadFile(file, &last, 1, &written, NULL) && written == 1 && last != '\n')
		{
			SetFilePointer(file, 0, NULL, FILE_END);
			WriteFile(file, "\r\n", 2, &written, NULL);
		}
	}
	SetFilePointer(file, 0, NULL, FILE_END);
	if (!WriteFile(file, line, length, &written, NULL) || written != length ||
		!WriteFile(file, "\r\n", 2, &written, NULL))
	{
		writer_last_error = GetLastError();
		writer_failures++;
	}
	CloseHandle(file);
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
		{
			LARGE_INTEGER counter;

			QueryPerformanceCounter(&counter);
			writer_pass_start = counter.QuadPart;
		}
		while (tail != head)
		{
			char c = ring[tail & (RING_SIZE - 1)];

			if (WRITER_COMMAND(c))
			{
				char name[192];
				long length = 0;

				for (tail++; tail != head && ring[tail & (RING_SIZE - 1)] != '\n'; tail++)
				{
					if (length < (long)sizeof(name) - 1)
						name[length++] = ring[tail & (RING_SIZE - 1)];
				}
				name[length] = 0;
				if (tail != head)
					tail++;
				if (c == WRITER_TOUR_LOAD)
				{
					oxbox_perf_writer_state = 6;
					tour_load(name);
					continue;
				}
				if (c == WRITER_TOUR_ADD)
				{
					oxbox_perf_writer_state = 7;
					tour_add(name);
					continue;
				}
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
				while (run != limit && !WRITER_COMMAND(ring[run & (RING_SIZE - 1)]))
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
		{
			LARGE_INTEGER counter;

			QueryPerformanceCounter(&counter);
			writer_pass_end = counter.QuadPart;
		}
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

/* value with 2 decimals at most, its trailing zeros dropped ("1.5", "0"): phase 2's many columns, kept short */
static void put_trimmed(
	struct perf_line *line,
	double value)
{
	long start = line->length;

	put_fixed(line, value, 2);
	if (line->length > start + 3 && line->text[line->length - 3] == '.')
	{
		while (line->text[line->length - 1] == '0')
			line->length--;
		if (line->text[line->length - 1] == '.')
			line->length--;
	}
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
	"cluster,local_players,log_ms,"
	/* (phase 2, P rows only: ms a frame) */
	"tick_units_ms,tick_ai_ms,tick_players_before_ms,tick_effects_ms,tick_antennas_ms,tick_first_person_ms,"
	"tick_game_engine_ms,tick_hs_ms,tick_recorded_animations_ms,tick_objects_ms,tick_players_after_ms,tick_hud_ms,"
	"tick_player_effect_ms,tick_other_ms,"
	"render_visibility_ms,visibility_portal_ms,visibility_subcluster_ms,visibility_surface_ms,render_sky_ms,"
	"render_first_person_weapon_ms,render_lights_ms,render_objects_ms,render_structure_lightmaps_ms,"
	"render_lens_flares_ms,render_object_shadows_ms,render_structure_shadows_ms,render_diffuse_lights_ms,"
	"render_structure_diffuse_lights_ms,render_decals_ms,render_structure_diffuse_textures_ms,"
	"render_specular_lights_ms,render_structure_specular_lights_ms,render_structure_reflections_ms,"
	"render_structure_transparent_geometry_ms,render_structure_fog_ms,render_weather_ms,render_particles_ms,"
	"render_contrails_ms,render_transparent_ms,render_detail_objects_ms,render_hud_ms,render_other_ms,"
	/* (phase 2, P rows only: counts a frame) */
	"objects_drawn,objects_updated,actors_active,effects_alive,particles_alive,dynamic_lights,level_triangles,"
	"clusters_rendered,dynamic_triangles,dynamic_triangles_peak,"
	/* (phase 3: other_ms's parts, both rows) */
	"other_input_ms,other_network_ms,other_menus_ms,other_network_end_ms,other_camera_ms,other_after_present_ms,"
	"other_between_frames_ms,"
	/* (phase 3, S rows: the writer thread) */
	"writer_state,writer_since_ms,writer_write_ms,writer_overlap_ms,"
	/* (phase 3, P rows: objects_update, ms and counts a frame) */
	"obj_activation_ms,obj_activation_runs,obj_loop_ms,obj_type_update_ms,obj_damage_ms,obj_export_functions_ms,"
	"obj_node_matrices_ms,obj_functions_colors_ms,obj_connect_lights_ms,obj_postprocess_ms,obj_create_delete_ms,"
	"obj_garbage_ms,obj_updates,obj_children_ms,obj_children_count,obj_created_count,obj_translate_ms,"
	"obj_translate_count,"
	"obj_biped_ms,obj_biped_count,obj_vehicle_ms,obj_vehicle_count,obj_weapon_ms,obj_weapon_count,"
	"obj_equipment_ms,obj_equipment_count,obj_garbage_type_ms,obj_garbage_type_count,obj_projectile_ms,"
	"obj_projectile_count,obj_scenery_ms,obj_scenery_count,obj_machine_ms,obj_machine_count,obj_control_ms,"
	"obj_control_count,obj_light_fixture_ms,obj_light_fixture_count,obj_placeholder_ms,obj_placeholder_count,"
	"obj_sound_scenery_ms,obj_sound_scenery_count,"
	/* (phase 3, P rows: the weather, counts a frame; the HUD's parts, ms a frame; the hooks' own time) */
	"weather_particles,weather_sprites,weather_boxes,weather_polyhedra,"
	"render_interface_ms,hud_screen_effect_ms,hud_players_ms,hud_motion_sensor_ms,hud_crosshair_ms,hud_weapon_ms,"
	"hud_unit_ms,hud_nav_damage_ms,hud_messages_ms,hud_other_ms,hud_game_engine_ms,render_screen_flash_ms,"
	"render_ui_widgets_ms,hooks_ms,"
	/* (phase 3, P rows: the clusters an object_translate call touches, and its cluster queries, a call) */
	"obj_translate_clusters,obj_translate_cluster_queries,"
	/* (phase 3, P rows: the three object tags whose top-level updates took the most time, ms a frame each) */
	"top_object_1,top_object_1_ms,top_object_2,top_object_2_ms,top_object_3,top_object_3_ms,"
	/* (test build 34, T rows: the camera tour's stop, view and label) */
	"tour_stop,tour_view,tour_label,tour_player_x,tour_player_y,tour_player_z";

/* phase 2's columns (P rows) and the steps or profile sections each one sums (a step is NUMBER_OF_SECTIONS + it) */
#define STEP_COLUMN(step) (NUMBER_OF_SECTIONS + (step))
#define HUD_COLUMN (-1)   /* (render_hud_ms: the HUD's parts, test build 33, added up) */
static short const window_time_columns[] =
{
	STEP_COLUMN(_oxbox_perf_tick_units), STEP_COLUMN(_oxbox_perf_tick_ai), STEP_COLUMN(_oxbox_perf_tick_players_before),
	STEP_COLUMN(_oxbox_perf_tick_effects), STEP_COLUMN(_oxbox_perf_tick_antennas),
	STEP_COLUMN(_oxbox_perf_tick_first_person), STEP_COLUMN(_oxbox_perf_tick_game_engine),
	STEP_COLUMN(_oxbox_perf_tick_hs), STEP_COLUMN(_oxbox_perf_tick_recorded_animations),
	STEP_COLUMN(_oxbox_perf_tick_objects), STEP_COLUMN(_oxbox_perf_tick_players_after),
	STEP_COLUMN(_oxbox_perf_tick_hud), STEP_COLUMN(_oxbox_perf_tick_player_effect), STEP_COLUMN(_oxbox_perf_tick_other),
	STEP_COLUMN(_oxbox_perf_draw_visibility), _section_visibility_portal, _section_visibility_subcluster,
	_section_visibility_surface, STEP_COLUMN(_oxbox_perf_draw_sky), STEP_COLUMN(_oxbox_perf_draw_first_person_weapon),
	STEP_COLUMN(_oxbox_perf_draw_lights), STEP_COLUMN(_oxbox_perf_draw_objects), STEP_COLUMN(_oxbox_perf_draw_lightmaps),
	STEP_COLUMN(_oxbox_perf_draw_lens_flares), STEP_COLUMN(_oxbox_perf_draw_object_shadows), _section_structure_shadows,
	STEP_COLUMN(_oxbox_perf_draw_diffuse_lights), _section_structure_diffuse_lights,
	STEP_COLUMN(_oxbox_perf_draw_decals), STEP_COLUMN(_oxbox_perf_draw_diffuse_textures),
	STEP_COLUMN(_oxbox_perf_draw_specular_lights), _section_structure_specular_lights,
	STEP_COLUMN(_oxbox_perf_draw_reflections), STEP_COLUMN(_oxbox_perf_draw_structure_transparent),
	STEP_COLUMN(_oxbox_perf_draw_fog), STEP_COLUMN(_oxbox_perf_draw_weather), STEP_COLUMN(_oxbox_perf_draw_particles),
	STEP_COLUMN(_oxbox_perf_draw_contrails), STEP_COLUMN(_oxbox_perf_draw_transparent),
	STEP_COLUMN(_oxbox_perf_draw_detail_objects), HUD_COLUMN,
	STEP_COLUMN(_oxbox_perf_draw_other),   /* (and _oxbox_perf_draw_end's) */
};
enum { NUMBER_OF_WINDOW_COUNT_COLUMNS = 10 };

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

static struct observer_result const *row_camera(void);

/* a P (a second), S (a slow frame) or T (a camera tour's view, test build 34) row: T rows have P rows' columns */
static void frame_row(
	char kind,
	double t,
	struct perf_sample const *sample)
{
	struct perf_line line;
	double frames = sample->frames > 0 ? (double)sample->frames : 1.0;
	boolean summed = kind == 'P' || kind == 'T';

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
		struct observer_result const *camera = row_camera();

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
	/* phase 2: P rows, the second's sums a frame; S rows, nothing (but a long tick's steps, test build 33) */
	{
		long column;
		boolean long_tick = kind == 'S' && ticks_to_ms(sample->ticks) > 25.0;

		for (column = 0; column < (long)NUMBEROF(window_time_columns); column++)
		{
			long source = window_time_columns[column];

			put_comma(&line);
			if (summed)
			{
				unsigned __int64 cycles = 0;

				if (source == HUD_COLUMN)
				{
					long step;

					for (step = _oxbox_perf_hud_screen_effect; step <= _oxbox_perf_draw_ui_widgets; step++)
						cycles += window.steps[step];
				}
				else
				{
					cycles = source < NUMBER_OF_SECTIONS ? window.sections[source] : window.steps[source - NUMBER_OF_SECTIONS];
					if (source == STEP_COLUMN(_oxbox_perf_draw_other))
						cycles += window.steps[_oxbox_perf_draw_end];
				}
				put_trimmed(&line, cycles_to_ms((double)(__int64)cycles) / frames);
			}
			else if (long_tick && source >= STEP_COLUMN(0) && source <= STEP_COLUMN(_oxbox_perf_tick_other))
			{
				put_trimmed(&line, cycles_to_ms((double)(__int64)frame_tick_steps[source - NUMBER_OF_SECTIONS]));
			}
		}
		if (summed)
		{
			unsigned long const counts[NUMBER_OF_WINDOW_COUNT_COLUMNS - 1] =
			{
				window.objects_drawn, window.objects_updated, window.actors, window.effects, window.particles,
				window.dynamic_lights, window.level_triangles, window.clusters, window.dynamic_triangles
			};

			for (column = 0; column < NUMBER_OF_WINDOW_COUNT_COLUMNS - 1; column++)
			{
				put_comma(&line);
				put_trimmed(&line, (double)counts[column] / frames);
			}
			put_comma(&line);
			put_unsigned(&line, window.dynamic_triangles_peak);
		}
		else
		{
			for (column = 0; column < NUMBER_OF_WINDOW_COUNT_COLUMNS; column++)
				put_comma(&line);
		}
	}
	/* phase 3: other's parts (both rows) */
	{
		__int64 const parts[7] =
		{
			sample->other_input, sample->other_network, sample->other_menus, sample->other_network_end,
			sample->other_camera, sample->other_after_present, sample->other_between
		};
		long part;

		for (part = 0; part < 7; part++)
		{
			put_comma(&line);
			put_trimmed(&line, ticks_to_ms(parts[part]) / frames);
		}
	}
	/* the writer thread (S rows): its state now, the ms since its last pass ended, that pass's length, and how much of
	this frame it was writing */
	if (kind == 'S')
	{
		LARGE_INTEGER counter;
		__int64 now, start = writer_pass_start, end = writer_pass_end, frame_start, overlap = 0;
		unsigned long state = oxbox_perf_writer_state;

		QueryPerformanceCounter(&counter);
		now = counter.QuadPart;
		frame_start = now - sample->frame;
		if (state == 3 || end < start)
			end = now;   /* (a pass under way) */
		if (start && end > frame_start)
			overlap = end - (start > frame_start ? start : frame_start);
		put_comma(&line);
		put_unsigned(&line, state);
		put_comma(&line);
		if (writer_pass_end)
			put_trimmed(&line, ticks_to_ms(now - writer_pass_end));
		put_comma(&line);
		if (start && end >= start)
			put_trimmed(&line, ticks_to_ms(end - start));
		put_comma(&line);
		put_trimmed(&line, ticks_to_ms(overlap > 0 ? overlap : 0));
	}
	else
	{
		put_text(&line, ",,,,");
	}
	/* phase 3: objects_update, the weather, the HUD and the hooks (P and T rows) */
	if (summed)
	{
		static short const object_step_order[] =
		{
			_oxbox_perf_object_type_update, _oxbox_perf_object_damage, _oxbox_perf_object_export_functions,
			_oxbox_perf_object_node_matrices, _oxbox_perf_object_functions_colors, _oxbox_perf_object_connect_lights,
			_oxbox_perf_object_postprocess, _oxbox_perf_object_create_delete, _oxbox_perf_object_garbage
		};
		long index, type, roots = 0;
		double hooks_cycles;

		put_comma(&line);
		put_trimmed(&line, cycles_to_ms((double)(__int64)(window.object_steps[_oxbox_perf_object_activation] +
			window.object_steps[_oxbox_perf_object_activation_changed])) / frames);
		put_comma(&line);
		put_unsigned(&line, window.activation_runs);
		put_comma(&line);
		put_trimmed(&line, cycles_to_ms((double)(__int64)window.loop_cycles) / frames);
		for (index = 0; index < (long)NUMBEROF(object_step_order); index++)
		{
			put_comma(&line);
			put_trimmed(&line, cycles_to_ms((double)(__int64)window.object_steps[object_step_order[index]]) / frames);
		}
		for (type = 0; type < NUMBER_OF_OBJECT_TYPES; type++)
			roots += window.type_counts[type];
		put_comma(&line);
		put_trimmed(&line, (double)window.object_updates / frames);
		put_comma(&line);
		put_trimmed(&line, cycles_to_ms((double)(__int64)window.children_cycles) / frames);
		put_comma(&line);
		put_trimmed(&line, (double)(window.object_updates > (unsigned long)roots + window.created_count ?
			window.object_updates - roots - window.created_count : 0) / frames);
		put_comma(&line);
		put_trimmed(&line, (double)window.created_count / frames);
		put_comma(&line);
		put_trimmed(&line, cycles_to_ms((double)(__int64)window.translate_cycles) / frames);
		put_comma(&line);
		put_trimmed(&line, (double)window.translate_count / frames);
		for (type = 0; type < NUMBER_OF_OBJECT_TYPES; type++)
		{
			put_comma(&line);
			put_trimmed(&line, cycles_to_ms((double)(__int64)window.type_cycles[type]) / frames);
			put_comma(&line);
			put_trimmed(&line, (double)window.type_counts[type] / frames);
		}
		put_comma(&line);
		put_trimmed(&line, (double)window.weather_particles / frames);
		put_comma(&line);
		put_trimmed(&line, (double)window.weather_sprites / frames);
		put_comma(&line);
		put_trimmed(&line, (double)window.weather_boxes / frames);
		put_comma(&line);
		put_trimmed(&line, (double)window.weather_polyhedra / frames);
		{
			unsigned __int64 interface_cycles = 0;
			long step;

			for (step = _oxbox_perf_hud_screen_effect; step <= _oxbox_perf_hud_game_engine; step++)
				interface_cycles += window.steps[step];
			put_comma(&line);
			put_trimmed(&line, cycles_to_ms((double)(__int64)interface_cycles) / frames);
			for (step = _oxbox_perf_hud_screen_effect; step <= _oxbox_perf_draw_ui_widgets; step++)
			{
				put_comma(&line);
				put_trimmed(&line, cycles_to_ms((double)(__int64)window.steps[step]) / frames);
			}
		}
		/* (the hooks' own time, from their counts and what each costs, measured at start-up) */
		hooks_cycles = (double)window.hook_calls * hook_call_cycles + (double)window.object_marks * inline_mark_cycles;
		put_comma(&line);
		put_trimmed(&line, cycles_to_ms(hooks_cycles) / frames);
		put_comma(&line);
		if (window.translate_count)
			put_trimmed(&line, (double)window.translate_clusters / window.translate_count);
		put_comma(&line);
		if (window.translate_count)
			put_trimmed(&line, (double)window.translate_cluster_queries / window.translate_count);
		/* the three tags whose top-level updates took the most (each with its children's) */
		{
			long rank, best[3] = { NONE, NONE, NONE };

			for (rank = 0; rank < 3; rank++)
			{
				long slot;

				for (slot = 0; slot < TAG_SLOTS; slot++)
				{
					if (tag_slots[slot].definition_index != NONE && slot != best[0] && slot != best[1] &&
						(best[rank] == NONE || tag_slots[slot].cycles > tag_slots[best[rank]].cycles))
					{
						best[rank] = slot;
					}
				}
			}
			for (rank = 0; rank < 3; rank++)
			{
				put_comma(&line);
				if (best[rank] != NONE)
				{
					char const *name = tag_get_name(tag_slots[best[rank]].definition_index);
					long start = line.length, index;

					put_text(&line, name ? name : "?");
					/* (a CSV field: no commas) */
					for (index = start; index < line.length; index++)
					{
						if (line.text[index] == ',')
							line.text[index] = ';';
					}
				}
				put_comma(&line);
				if (best[rank] != NONE)
					put_trimmed(&line, cycles_to_ms((double)(__int64)tag_slots[best[rank]].cycles) / frames);
			}
		}
	}
	else
	{
		long column;

		/* (18 + 24 objects, 4 weather, 13 HUD, 1 hooks, 2 object_translate's clusters, 6 top objects) */
		for (column = 0; column < 18 + 2 * NUMBER_OF_OBJECT_TYPES + 4 + 13 + 1 + 2 + 6; column++)
			put_comma(&line);
	}
	/* the camera tour (T rows): the stop (1 up, in the file's order), the view (1 up: 1 to 4 looking around), the label,
	and where the player was (empty if it had no body) */
	put_comma(&line);
	if (kind == 'T')
	{
		long player_index = local_player_get_player_index(tour.local_player_index);
		long unit_index = player_index != NONE ? player_get(player_index)->unit_index : NONE;

		put_signed(&line, tour.stop_index + 1);
		put_comma(&line);
		put_signed(&line, tour.view_index + 1);
		put_comma(&line);
		put_text(&line, tour_file.stops[tour.stop_index].label);
		put_comma(&line);
		if (unit_index != NONE)
		{
			real_point3d origin;

			object_get_origin(unit_index, &origin);
			put_fixed(&line, origin.x, 2); put_comma(&line);
			put_fixed(&line, origin.y, 2); put_comma(&line);
			put_fixed(&line, origin.z, 2);
		}
		else
		{
			put_text(&line, ",,");
		}
	}
	else
	{
		put_text(&line, ",,,,,");
	}
	put_line(&line);
}

/* ---------- the camera tour (the game thread's side, test build 34) */

/* a line in the HUD's messages (at most 62 characters) */
static void tour_message(
	char const *text)
{
	short player = local_player_get_next(NONE);
	wchar_t wide[63];
	long index;

	if (player == NONE || !game_in_progress())
		return;
	for (index = 0; text[index] && index < (long)NUMBEROF(wide) - 1; index++)
		wide[index] = (unsigned char)text[index];
	wide[index] = 0;
	hud_print_message(player, wide);
}

/* the session's start: the writer reads the map's tour file */
static void tour_session_start(
	char const *map)
{
	static long requests;
	struct perf_line line;

	tour.state = _tour_none;
	tour.camera_valid = FALSE;
	tour.stop_index = tour.view_index = 0;
	tour.stops_added = 0;
	tour.rows = 0;
	tour.started = 0;
	/* the tour file's name under perf\tours: a stock map's name, or a folder map's key (hmp_maps.c: '+', then its
	folder under maps\, a pack's as "pack/map") as that folder's path */
	{
		long from = map[0] == '+', to = 0;

		for (; map[from] && to < (long)sizeof(tour.map) - 1; from++)
		{
			char c = map[from];

			tour.map[to++] = c == '/' ? '\\' : (c < ' ' || strchr(":*?\"<>|", c)) ? '_' : c;
		}
		tour.map[to] = 0;
		if (!to)
			csstrcpy(tour.map, "map");
	}
	tour.request = ++requests;
	line.length = 0;
	put_char(&line, WRITER_TOUR_LOAD);
	put_unsigned(&line, (unsigned long)tour.request);
	put_char(&line, ' ');
	put_text(&line, tour.map);
	line.text[line.length++] = '\n';
	ring_put(line.text, line.length);
}

static long tour_views(
	struct tour_stop const *stop)
{
	return stop->look_around ? TOUR_LOOK_AROUND_VIEWS : 1;
}

/* the view at the tour's stop and view: the camera there, and its settling starts */
static void tour_view_begin(
	__int64 now)
{
	struct tour_stop const *stop = &tour_file.stops[tour.stop_index];
	real yaw = DEGREES_TO_RADIANS(stop->look_around ? 90.0f * tour.view_index : stop->yaw);
	real pitch = DEGREES_TO_RADIANS(stop->pitch);

	memset(&tour.camera, 0, sizeof(tour.camera));
	tour.camera.position = stop->position;
	tour.camera.forward.i = (real)(cos(pitch) * cos(yaw));
	tour.camera.forward.j = (real)(cos(pitch) * sin(yaw));
	tour.camera.forward.k = (real)sin(pitch);
	tour.camera.up.i = (real)(-sin(pitch) * cos(yaw));
	tour.camera.up.j = (real)(-sin(pitch) * sin(yaw));
	tour.camera.up.k = (real)cos(pitch);
	/* (Halo's own field of view, not the player's: a zoomed weapon, or the combo's click zooming it, doesn't change a
	tour's views) */
	tour.camera.field_of_view = DEGREES_TO_RADIANS(70.0f);
	scenario_location_from_point(&tour.camera.location, &tour.camera.position);
	tour.camera_valid = TRUE;
	tour.state = _tour_settling;
	tour.phase_start = now;
}

/* the player's spot for the tour: the file's player line, else the map's first spawn point; the same every run, so the
game's cost around the player (its update, the objects it keeps active, the bots near it) is too */
static void tour_pin_choose(
	void)
{
	struct scenario *scenario = global_scenario_try_and_get();

	tour.pin = FALSE;
	tour.pin_unit = NONE;
	tour.pin_set_invulnerable = FALSE;
	tour.pin_settled = FALSE;
	tour.pins = 0;
	if (tour_file.player_none)
		return;
	if (game_connection() == _game_connection_network_client)
	{
		platform_log("perf: camera tour: the player stays where it is (this console joined a game: only the host moves players)");
		return;
	}
	if (tour_file.player_set)
	{
		tour.pin_position = tour_file.player_position;
		tour.pin_yaw = DEGREES_TO_RADIANS(tour_file.player_yaw);
	}
	else if (scenario && scenario->players.count > 0)
	{
		struct player_starting_location const *spawn =
			TAG_BLOCK_GET_ELEMENT(&scenario->players, 0, struct player_starting_location);

		tour.pin_position = spawn->position;
		tour.pin_yaw = spawn->facing;
	}
	else
	{
		return;
	}
	tour.pin = TRUE;
}

/* the first local player's body (NONE: none, dead) and its player */
static long tour_player_unit(
	long *player_index)
{
	*player_index = local_player_get_player_index(tour.local_player_index);
	return *player_index != NONE ? player_get(*player_index)->unit_index : NONE;
}

/* the player to its spot (as the scripts' object_teleport does), facing the spot's way, and unhurt by anything */
static void tour_pin_place(
	__int64 now,
	long player_index,
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	real_vector3d forward;

	if (!unit)
		return;
	if (unit_index == tour.pin_unit)
	{
		tour.pins++;
	}
	else
	{
		tour.pin_set_invulnerable = !TEST_FLAG(unit->object.damage_flags, _object_cannot_take_damage_bit);
		SET_FLAG(unit->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
	}
	forward.i = (real)cos(tour.pin_yaw);
	forward.j = (real)sin(tour.pin_yaw);
	forward.k = 0.0f;
	object_reset(unit_index);
	unit->unit.desired_facing_vector = forward;
	unit->unit.desired_aiming_vector = forward;
	unit->unit.desired_looking_vector = forward;
	/* (once it has landed on its spot, back to where it landed: no fall each time) */
	player_teleport(player_index, NONE, tour.pin_settled ? &tour.pin_actual : &tour.pin_position);
	player_control_set_facing(tour.local_player_index, &forward);
	object_set_position(unit_index, NULL, NULL, NULL);
	tour.pin_unit = unit_index;
	tour.pin_time = now;
}

/* once a frame while the tour runs: the player stays on its spot (a second after it's put there, where it landed is
the spot; moved 0.2 units from it, by the controller, a bot or an explosion, it goes back; a new body goes there too) */
static void tour_pin_update(
	__int64 now)
{
	long player_index, unit_index;
	real_point3d origin;

	if (!tour.pin || (unit_index = tour_player_unit(&player_index)) == NONE)
		return;
	if (unit_index != tour.pin_unit)
	{
		tour_pin_place(now, player_index, unit_index);
		return;
	}
	object_get_origin(unit_index, &origin);
	if (!tour.pin_settled)
	{
		if (ticks_to_ms(now - tour.pin_time) >= 1000.0)
		{
			tour.pin_settled = TRUE;
			tour.pin_actual = origin;
		}
	}
	else if (distance_squared3d(&origin, &tour.pin_actual) > 0.2f * 0.2f)
	{
		tour_pin_place(now, player_index, unit_index);
	}
}

/* the tour's end: the player can be hurt again (it stays on the spot) */
static void tour_pin_release(
	void)
{
	if (tour.pin && tour.pin_unit != NONE && tour.pin_set_invulnerable && game_in_progress())
	{
		struct object_datum *object = object_try_and_get(tour.pin_unit);

		if (object)
			SET_FLAG(object->object.damage_flags, _object_cannot_take_damage_bit, FALSE);
	}
	if (tour.pin && tour.pins)
		platform_log("perf: camera tour: the player was put back on its spot %ld time%s", tour.pins, tour.pins == 1 ? "" : "s");
	tour.pin = FALSE;
	tour.pin_unit = NONE;
}

static void tour_finish(
	char const *how)
{
	char text[64];
	LARGE_INTEGER now;
	long seconds;

	QueryPerformanceCounter(&now);
	seconds = tour.started ? (long)(ticks_to_ms(now.QuadPart - tour.started) / 1000.0 + 0.5) : 0;

	tour.camera_valid = FALSE;
	tour.state = _tour_done;
	tour_pin_release();
	platform_log("perf: camera tour %s after %ld s: %ld T rows", how, seconds, tour.rows);
	_snprintf(text, sizeof(text), "Camera tour %s: %ld T rows", how, tour.rows);
	text[sizeof(text) - 1] = 0;
	tour_message(text);
}

/* hold Back, click the right stick (any controller): its frame */
static boolean tour_combo(
	void)
{
	short index;

	for (index = 0; index < MAXIMUM_GAMEPADS; index++)
	{
		struct gamepad_state const *state = input_get_gamepad_state(index);

		if (state && state->buttons[_gamepad_binary_button_back] && state->buttons[_gamepad_binary_button_right_thumb] == 1)
			return TRUE;
	}
	return FALSE;
}

/* the combo when no tour runs: the first local player's view goes at the end of the map's tour file */
static void tour_add_stop(
	void)
{
	short player = local_player_get_next(NONE);
	struct observer_result const *camera = player != NONE ? observer_get_camera(player) : NULL;
	struct perf_line line;
	double yaw, k;
	char text[64];

	if (!camera)
		return;
	yaw = atan2(camera->forward.j, camera->forward.i) * (180.0 / 3.14159265358979);
	if (yaw < 0.0)
		yaw += 360.0;
	k = camera->forward.k > 1.0f ? 1.0 : camera->forward.k < -1.0f ? -1.0 : camera->forward.k;
	tour.stops_added++;
	line.length = 0;
	put_char(&line, WRITER_TOUR_ADD);
	put_fixed(&line, camera->position.x, 2); put_char(&line, ' ');
	put_fixed(&line, camera->position.y, 2); put_char(&line, ' ');
	put_fixed(&line, camera->position.z, 2); put_char(&line, ' ');
	put_fixed(&line, yaw, 1); put_char(&line, ' ');
	put_fixed(&line, asin(k) * (180.0 / 3.14159265358979), 1);
	put_text(&line, " added ");
	put_unsigned(&line, (unsigned long)tour.stops_added);
	put_text(&line, " (cluster ");
	put_signed(&line, camera->location.cluster_index);
	put_char(&line, ')');
	line.text[line.length] = 0;
	platform_log("perf: tour stop added to %s: %s", tour_file.path, line.text + 1);
	line.text[line.length++] = '\n';
	ring_put(line.text, line.length);
	writer_flush_requests++;
	_snprintf(text, sizeof(text), "Tour stop saved (%ld in the file)", tour_file.stop_count + tour.stops_added);
	text[sizeof(text) - 1] = 0;
	tour_message(text);
}

/* the tour file, once the writer has read it */
static void tour_loaded(
	void)
{
	long index, views = 0;
	double seconds = tour_file.start_delay;
	char text[64];

	tour.request = 0;
	if (!tour_file.found)
	{
		platform_log("perf: no camera tour for this map (Back + the right stick's click adds the view to %s)", tour_file.path);
		tour.state = _tour_done;
		return;
	}
	for (index = 0; index < tour_file.stop_count; index++)
	{
		views += tour_views(&tour_file.stops[index]);
		seconds += (tour_file.stops[index].settle + tour_file.stops[index].hold) * tour_views(&tour_file.stops[index]);
	}
	if (tour_file.lines_skipped || tour_file.stops_dropped || tour_file.truncated)
	{
		platform_log("perf: tour file %s: %ld lines not understood (the first: line %ld), %ld stops over %d left out%s",
			tour_file.path, tour_file.lines_skipped, tour_file.first_skipped_line, tour_file.stops_dropped,
			TOUR_MAXIMUM_STOPS, tour_file.truncated ? ", the file's end (over 6 KB) left out" : "");
	}
	if (!tour_file.stop_count)
	{
		platform_log("perf: tour file %s has no stops", tour_file.path);
		tour.state = _tour_done;
		return;
	}
	platform_log("perf: camera tour %s: %ld stops, %ld views, about %ld s, starting %ld s after the map loaded",
		tour_file.path, tour_file.stop_count, views, (long)(seconds + 0.5), (long)(tour_file.start_delay + 0.5));
	_snprintf(text, sizeof(text), "Camera tour: %ld views, about %ld s", views, (long)(seconds + 0.5));
	text[sizeof(text) - 1] = 0;
	/* (the HUD shows the newest message on top) */
	tour_message("Hold Back and click the right stick to stop it");
	tour_message(text);
	tour.state = _tour_waiting;
}

static void window_row(char kind, __int64 now, struct perf_counters const *counters);

/* once a frame in a session, after the frame went in the second's sums */
static void tour_update(
	__int64 now,
	struct perf_counters const *counters)
{
	boolean blocked = shell_application_is_paused() || game_time_get_paused() || game_engine_showing_postgame();
	boolean combo = !blocked && tour_combo();
	struct tour_stop const *stop;

	if (tour.state == _tour_none)
	{
		if (tour.request && tour_file.loaded_request == tour.request)
			tour_loaded();
		return;
	}
	if (combo)
	{
		if (tour.state == _tour_done)
		{
			tour_add_stop();
		}
		else
		{
			/* (a view's frames measured so far: a P row, with the view's camera) */
			if (tour.state == _tour_measuring && perf.second.frames)
				window_row('P', now, counters);
			tour_finish("stopped");
		}
		return;
	}
	switch (tour.state)
	{
	case _tour_waiting:
		if (!blocked && local_player_get_next(NONE) != NONE &&
			ticks_to_ms(now - perf.session_start) >= tour_file.start_delay * 1000.0)
		{
			long player_index, unit_index;

			tour.local_player_index = local_player_get_next(NONE);
			tour_pin_choose();
			unit_index = tour_player_unit(&player_index);
			if (tour.pin && unit_index == NONE)
				break;   /* (the player's dead: the tour starts when it's back) */
			tour.stop_index = tour.view_index = 0;
			tour.started = now;
			tour.rows = 0;
			tour_view_begin(now);
			if (tour.pin)
			{
				char text[64];
				struct perf_line spot;

				tour_pin_place(now, player_index, unit_index);
				/* (a second more for the first view: the player lands on its spot) */
				tour.phase_start += perf.frequency;
				spot.length = 0;
				put_fixed(&spot, tour.pin_position.x, 2); put_char(&spot, ' ');
				put_fixed(&spot, tour.pin_position.y, 2); put_char(&spot, ' ');
				put_fixed(&spot, tour.pin_position.z, 2);
				spot.text[spot.length] = 0;
				platform_log("perf: camera tour: the player stands at %s during the tour (%s)", spot.text,
					tour_file.player_set ? "the tour file's player line" : "the map's first spawn point");
				_snprintf(text, sizeof(text), "Camera tour: the player waits at %s",
					tour_file.player_set ? "the tour's spot" : "spawn point 1");
				text[sizeof(text) - 1] = 0;
				tour_message(text);
			}
		}
		break;

	case _tour_settling:
		stop = &tour_file.stops[tour.stop_index];
		tour_pin_update(now);
		if (blocked)
		{
			tour.phase_start = now;
		}
		else if (ticks_to_ms(now - tour.phase_start) >= stop->settle * 1000.0)
		{
			/* (the frames so far go in a P row of their own: the T row's start here) */
			if (perf.second.frames)
				window_row('P', now, counters);
			tour.state = _tour_measuring;
			tour.phase_start = now;
		}
		break;

	case _tour_measuring:
		stop = &tour_file.stops[tour.stop_index];
		tour_pin_update(now);
		if (blocked)
		{
			/* (paused or the game's over: these frames go in the next P row, and the view settles again) */
			tour.state = _tour_settling;
			tour.phase_start = now;
		}
		else if (ticks_to_ms(now - tour.phase_start) >= stop->hold * 1000.0)
		{
			window_row('T', now, counters);
			tour.rows++;
			if (++tour.view_index >= tour_views(stop))
			{
				tour.view_index = 0;
				tour.stop_index++;
			}
			if (tour.stop_index >= tour_file.stop_count)
				tour_finish("done");
			else
				tour_view_begin(now);
		}
		break;
	}
}

/* the camera hook: a local player's view about to be drawn */
static struct observer_result const *perf_camera(
	short local_player_index,
	struct observer_result const *observer)
{
	session_hook_calls++;
	if (!tour.camera_valid || !perf.active || local_player_index != tour.local_player_index)
		return observer;
	return &tour.camera;
}

/* the camera the rows show: the tour's while it flies the view */
static struct observer_result const *row_camera(
	void)
{
	return tour.camera_valid ? &tour.camera : observer_get_camera(0);
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
#if defined(OXBOX_BUILD_O2)
	put_text(&line, " O2");
#endif
	put_text(&line, " (OpenCE on the Xbox, test build ");
	put_unsigned(&line, OXBOX_BUILD_NUMBER);
#if defined(OXBOX_BUILD_O2)
	/* (test build 33's experiment: gen_xbox.py OXBOX_HOT_EXTRA) */
	put_text(&line, "; the -O2 experiment: units devices items ai interface compiled -O2");
#endif
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
	/* (straight into the ring: longer than a row's buffer) */
	ring_put("# fields ", 9);
	ring_put(frame_fields, sizeof(frame_fields) - 1);
	ring_put("\r\n", 2);

	perf.active = TRUE;
	perf.map_checksum = cache_file_globals.header.checksum;
	perf.session_start = now;
	perf.session_cycles = cycles64();
	perf.session_log_cycles = 0;
	session_hook_calls = session_frames = hook_calls_at_window_start = 0;
	/* (the object marks' sums from before the session, and the objects' state) */
	memset(perf_hooks.object_cycles, 0, sizeof(perf_hooks.object_cycles));
	memset(perf_hooks.object_counts, 0, sizeof(perf_hooks.object_counts));
	perf_hooks.translate_cycles = perf_hooks.translate_count = 0;
	perf_hooks.translate_cluster_queries = perf_hooks.translate_clusters = 0;
	objects.root_type = NONE;
	objects.children_depth = 0;
	perf.seconds_reported = 0;
	perf.session_gpu_samples = gpu_samples;
	perf.second_start = now;
	counters_now(&perf.second_counters);
	memset(&perf.second, 0, sizeof(perf.second));
	window_reset();
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
	/* the camera tour: the writer reads the map's tour file (test build 34; named by the map's key, not the CSV's name) */
	tour_session_start(cache_file_globals.header.name);
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
	platform_log("perf: the log has taken %ld ms of the session's %ld s (%ld.%02ld%%), %lu GPU samples, %lu step and "
		"section hooks a frame; QueryPerformanceFrequency %ld, the cycle counter %ld Hz", (long)log_ms, (long)seconds,
		hundredths / 100, hundredths % 100, gpu_samples - (unsigned long)perf.session_gpu_samples,
		session_frames ? session_hook_calls / session_frames : 0, (long)perf.frequency, (long)perf.cycle_frequency);
}

static void session_end(
	void)
{
	static char const close_line[] = { WRITER_CLOSE, '\n' };

	if (perf.cancel_timer)
		perf.cancel_timer(gpu_timer);
	cost_report();
	if (tour.state == _tour_waiting || tour.state == _tour_settling || tour.state == _tour_measuring)
		platform_log("perf: camera tour stopped at the map's end: %ld T rows", tour.rows);
	tour_pin_release();
	tour.camera_valid = FALSE;
	tour.state = _tour_none;
	tour.request = 0;
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

/* ---------- phase 2's hooks: the profile sections and the steps' marks */

/* the log's slot for a profile section (NONE: one it leaves alone), found by its name the first time */
static long section_slot(
	struct profile_section *section)
{
	long tag = section->field_C, slot;

	if ((tag & 0xFFFF0000) == SECTION_SLOT_TAG)
		return (tag & 0xFFFF) == 0xFFFF ? NONE : (tag & 0xFFFF);
	slot = NONE;
	if (section->name)
	{
		long index;

		for (index = 0; index < NUMBER_OF_SECTIONS; index++)
		{
			if (!strcmp(section->name, section_names[index]))
			{
				slot = index;
				break;
			}
		}
	}
	section->field_C = SECTION_SLOT_TAG | (slot & 0xFFFF);
	return slot;
}

static void perf_enter(
	struct profile_section *section)
{
	long slot = section_slot(section);

	session_hook_calls++;
	if (slot == NONE)
		return;
	section->entry_timebase = cycles64();
	if (slot == _section_game_update)
		window.mark_last = (unsigned long)section->entry_timebase;
	else if (slot == _section_objects_update)
		perf_hooks.object_last = (unsigned long)section->entry_timebase;
}

static void perf_exit(
	struct profile_section *section)
{
	long slot = section_slot(section);

	session_hook_calls++;
	if (slot == NONE || slot == _section_game_update || slot == _section_objects_update || !section->entry_timebase)
		return;
	window.sections[slot] += cycles64() - section->entry_timebase;
	section->entry_timebase = 0;
}

static void perf_mark(
	long step)
{
	unsigned long now = cycles_now();
	unsigned long elapsed = now - window.mark_last;

	window.mark_last = now;
	session_hook_calls++;
	if (step == _oxbox_perf_draw_start || step < 0 || step >= NUMBER_OF_OXBOX_PERF_STEPS)
		return;
	window.steps[step] += elapsed;
	if (step <= _oxbox_perf_tick_other)
		frame_tick_steps[step] += elapsed;
	if (step == _oxbox_perf_draw_end)
	{
		/* (the window's counts, as it left them) */
		window.objects_drawn += render_object_globals.rendered_object_count;
		window.clusters += render.rendered_cluster_count;
		window.level_triangles += render.environment_surface_count;
		window.dynamic_lights += debug_rasterizer_light_count > 0 ? debug_rasterizer_light_count : 0;
		window.dynamic_triangles += dynamic_triangles.triangle_count;
		if ((unsigned long)dynamic_triangles.triangle_count > window.dynamic_triangles_peak)
			window.dynamic_triangles_peak = dynamic_triangles.triangle_count;
		window.windows++;
	}
}

/* a frame's counts of what's alive (the frame hook), the objects updated a tick times its ticks */
static void frame_counts(
	long game_ticks)
{
	unsigned long active = 0;

	if (object_header_data && object_header_data->data)
	{
		struct object_header_datum const *header = (struct object_header_datum const *)object_header_data->data;
		long index;

		for (index = 0; index < object_header_data->count; index++, header++)
		{
			if (header->identifier && TEST_FLAG(header->flags, _object_header_active_bit) &&
				!TEST_FLAG(header->flags, _object_header_being_created_bit))
			{
				active++;
			}
		}
	}
	window.objects_updated += active * (game_ticks > 0 ? game_ticks : 0);
	window.actors += actor_data ? actor_data->actual_count : 0;
	window.effects += effect_data ? effect_data->actual_count : 0;
	window.particles += particle_data ? particle_data->actual_count : 0;
}

/* objects_update's loop (test build 33): a top-level object's update about to start (its type) ends the one before
it, which ended at its last object mark (the time between them is the loop's); the children's updates' start and
end; a created object's update */
static void perf_object(
	long event)
{
	unsigned long now = cycles_now();

	session_hook_calls++;
	if (event >= 0 || event == _oxbox_perf_object_event_loop_end)
	{
		if (objects.root_type != NONE)
		{
			unsigned long cycles = perf_hooks.object_last - objects.root_start;

			window.type_cycles[objects.root_type] += cycles;
			if (objects.root_definition_index != NONE)
				tag_slot_add(objects.root_definition_index, cycles);
		}
		window.loop_cycles += now - perf_hooks.object_last;
		perf_hooks.object_last = now;
		objects.root_type = NONE;
		if (event >= 0 && object_header_data && event < object_header_data->count)
		{
			struct object_header_datum const *header = (struct object_header_datum const *)object_header_data->data + event;

			if (header->type < NUMBER_OF_OBJECT_TYPES)
			{
				objects.root_type = header->type;
				objects.root_definition_index = header->datum ? header->datum->definition_index : NONE;
				objects.root_start = now;
				window.type_counts[header->type]++;
			}
		}
	}
	else if (event == _oxbox_perf_object_event_children_begin)
	{
		if (!objects.children_depth++)
			objects.children_start = now;
		window.children_begun++;
	}
	else if (event == _oxbox_perf_object_event_children_end)
	{
		if (objects.children_depth > 0 && !--objects.children_depth)
			window.children_cycles += now - objects.children_start;
	}
	else if (event == _oxbox_perf_object_event_created)
	{
		perf_hooks.object_cycles[_oxbox_perf_object_create_delete] += now - perf_hooks.object_last;
		perf_hooks.object_last = now;
		window.created_count++;
	}
}

/* the weather, a particle type drawn (test build 33) */
static void perf_weather(
	long particles,
	long sprites,
	long boxes,
	long polyhedra)
{
	session_hook_calls++;
	window.weather_particles += particles > 0 ? particles : 0;
	window.weather_sprites += sprites > 0 ? sprites : 0;
	window.weather_boxes += boxes > 0 ? boxes : 0;
	window.weather_polyhedra += polyhedra > 0 ? polyhedra : 0;
}

/* the inline object marks' and object_translate's sums, into the window (the frame hook: they're 32 bits) */
static void objects_fold(
	void)
{
	long step;

	for (step = 0; step < NUMBER_OF_OXBOX_PERF_OBJECT_STEPS; step++)
	{
		window.object_steps[step] += perf_hooks.object_cycles[step];
		perf_hooks.object_cycles[step] = 0;
	}
	window.object_updates += perf_hooks.object_counts[_oxbox_perf_object_type_update];
	window.activation_runs += perf_hooks.object_counts[_oxbox_perf_object_activation_changed];
	/* (the marks a frame: 8 an object update, about 3 more a tick) */
	window.object_marks += perf_hooks.object_counts[_oxbox_perf_object_type_update] * 8;
	memset(perf_hooks.object_counts, 0, sizeof(perf_hooks.object_counts));
	window.translate_cycles += perf_hooks.translate_cycles;
	window.translate_count += perf_hooks.translate_count;
	window.object_marks += perf_hooks.translate_count * 2 + perf_hooks.translate_cluster_queries;
	window.translate_cluster_queries += perf_hooks.translate_cluster_queries;
	window.translate_clusters += perf_hooks.translate_clusters;
	perf_hooks.translate_cycles = 0;
	perf_hooks.translate_count = 0;
	perf_hooks.translate_cluster_queries = 0;
	perf_hooks.translate_clusters = 0;
}

/* the second's sums (or a tour view's) in a row, and the next ones' start */
static void window_row(
	char kind,
	__int64 now,
	struct perf_counters const *counters)
{
	/* (the cycle counter's rate, from the session so far) */
	if (now - perf.session_start >= perf.frequency)
	{
		perf.cycle_frequency = (double)(__int64)(cycles64() - perf.session_cycles) * (double)perf.frequency /
			(double)(now - perf.session_start);
	}
	counters_since(&perf.second, &perf.second_counters, counters);
	perf.session_log_cycles += perf.second.log_cycles;
	if (kind == 'P' && ++perf.seconds_reported % 60 == 0)
		cost_report();
	window.hook_calls = session_hook_calls - hook_calls_at_window_start;
	hook_calls_at_window_start = session_hook_calls;
	frame_row(kind, ticks_to_ms(now - perf.session_start) / 1000.0, &perf.second);
	window_reset();
	perf.second_start = now;
	perf.second_counters = *counters;
	memset(&perf.second, 0, sizeof(perf.second));
	perf.slow_lines_this_second = 0;
}

static void window_reset(
	void)
{
	unsigned long mark_last = window.mark_last;

	tag_slots_clear();

	memset(&window, 0, sizeof(window));
	window.mark_last = mark_last;
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
	__int64 previous_frame_end;

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
	previous_frame_end = perf.last_frame_end;
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
		memset(frame_tick_steps, 0, sizeof(frame_tick_steps));
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
	/* (other's parts: the frame's own marks, and the time from the last frame's end to this one's start) */
	sample.other_input = marks[_mark_input] - marks[_mark_start];
	sample.other_network = marks[_mark_network] - marks[_mark_input];
	sample.other_menus = marks[_mark_menus] - marks[_mark_network];
	sample.other_network_end = marks[_mark_network_end] - marks[_mark_menus];
	sample.other_camera = marks[_mark_camera] - marks[_mark_ticks];
	sample.other_after_present = now > marks[_mark_present] ? now - marks[_mark_present] : 0;
	sample.other_between = marks[_mark_start] > previous_frame_end ? marks[_mark_start] - previous_frame_end : 0;
	counters_since(&sample, &perf.frame_start, &counters);
	perf.frame_start = counters;
	frame_counts(sample.game_ticks);
	objects_fold();
	session_frames++;

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
	perf.second.other_input += sample.other_input;
	perf.second.other_network += sample.other_network;
	perf.second.other_menus += sample.other_menus;
	perf.second.other_network_end += sample.other_network_end;
	perf.second.other_camera += sample.other_camera;
	perf.second.other_after_present += sample.other_after_present;
	perf.second.other_between += sample.other_between;
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
	memset(frame_tick_steps, 0, sizeof(frame_tick_steps));

	/* the camera tour (it may end the second's sums early: a T row's start and end) */
	tour_update(now, &counters);

	/* a second's summary (not while a tour measures a view: its frames go in its T row) */
	if (now - perf.second_start >= perf.frequency && tour.state != _tour_measuring)
		window_row('P', now, &counters);

	/* to the disk at once at round end and on pause */
	paused = shell_application_is_paused() || game_time_get_paused();
	postgame = game_engine_showing_postgame();
	if ((paused && !perf.paused) || (postgame && !perf.postgame))
		writer_flush_requests++;
	perf.paused = paused;
	perf.postgame = postgame;
	perf.last_hook_cycles = cycles_now() - hook_start;
}

static struct oxbox_perf_hooks perf_hooks = { perf_frame, perf_enter, perf_exit, perf_mark, perf_object, perf_weather, perf_camera };

/* ---------- start-up */

/* what a hook costs, in cycles: an inline object mark, and a hook call (perf_mark's quickest path) */
static void calibrate_hooks(
	void)
{
	struct oxbox_perf_hooks *saved = oxbox_perf;
	unsigned long start, index;

	oxbox_perf = &perf_hooks;
	start = cycles_now();
	for (index = 0; index < 1000; index++)
		OXBOX_PERF_OBJECT_MARK(_oxbox_perf_object_garbage)
	inline_mark_cycles = (cycles_now() - start) / 1000;
	start = cycles_now();
	for (index = 0; index < 1000; index++)
		OXBOX_PERF_MARK(_oxbox_perf_draw_start)
	hook_call_cycles = (cycles_now() - start) / 1000;
	oxbox_perf = saved;
	perf_hooks.object_cycles[_oxbox_perf_object_garbage] = 0;
	session_hook_calls = 0;
}

/* opence.ini's perf_ settings (oxbox_entry.c reads perf_log, and the others are only read here) */
static void read_settings(
	char const *path)
{
	char text[8192], *p, *e;   /* (as oxbox_entry.c: the whole reference opence.ini; 1 KB missed its perf_ lines) */
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
	calibrate_hooks();
	platform_log("perf: on (OXPERF loaded at %p, %lu KB, and a %lu KB writer stack); slow frames over %ld ms; "
		"a GPU sample every %ld ms; an object mark costs %lu cycles, a hook call %lu", __oxperf_start,
		(unsigned long)(((__oxperf_end - __oxperf_start) + 0xFFF) & ~0xFFF) / 1024,
		(unsigned long)WRITER_STACK_SIZE / 1024, perf.slow_frame_ms, perf.gpu_sample_ms, inline_mark_cycles,
		hook_call_cycles);
	return &perf_hooks;
}
