/*
OXBOX_PERF.C

The developers' performance and memory log (test build 31), for Halo Map Porter: how much a converted map really
costs on a real Xbox, and what limits its frame rate (the CPU's game ticks or drawing, or the graphics chip).

Off unless opence.ini says perf_log = 1 (or perf_sweep = 1: the perf sweep, v1.131, its section below). Everything here (code, data, the ring buffer) is in the XBE section
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
#include "interface/event_manager.h"
#include "interface/ui_widget.h"
#include "units/units.h"
#include "cutscene/cinematics.h"
#include "main/main.h"
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
	byte *start2;
	long size2;
	long gap_block_index2;
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
	boolean quiet;                   /* (the perf sweep without perf_sweep_detail: no per-second files) */
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

	if (perf.quiet)
		return;
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

static void sweep_watch(void);

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
		sweep_watch();
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
	long except_block_index,
	long except_block_index2)
{
	struct data_iterator iterator;
	struct lruv_cache_block *block;
	long pages = 0;

	if (!cache || !cache->blocks)
		return 0;
	data_iterator_new(&iterator, cache->blocks);
	while ((block = data_iterator_next(&iterator)) != NULL)
	{
		if (iterator.datum_index != except_block_index && iterator.datum_index != except_block_index2)
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

	static int mapped = -1;

	if (total_pages == 16384)
		pfn = (unsigned long const *)0x83FF0000;
	else if (total_pages == 32768)
		pfn = (unsigned long const *)0x87FE0000;
	else
		return 0;
	/* (every page of it mapped, asked once (MmIsAddressValid, kernel 174): a 128 MB console whose kernel doesn't map its
	top 64 MB in the contiguous window has none there (Cerbios: the perf log read 0x87FE0000 and crashed, xemu with
	Llama's BIOS)) */
	if (mapped < 0)
	{
		void *oxbox_kernel_export(unsigned long ordinal);
		unsigned char (__stdcall *valid)(void const *) = (unsigned char (__stdcall *)(void const *))oxbox_kernel_export(174);
		unsigned long page;

		mapped = valid != NULL;
		for (page = 0; mapped && page < total_pages * 4; page += 0x1000)
			mapped = valid((char const *)pfn + page) != 0;
	}
	if (!mapped)
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
	gap_pages = texture_cache_lent.page_count ? block_pages(textures, texture_cache_lent.gap_block_index) +
		block_pages(textures, texture_cache_lent.gap_block_index2) : 0;

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
	put_signed(&line, cache_used_pages(textures, texture_cache_lent.page_count ? texture_cache_lent.gap_block_index : NONE,
		texture_cache_lent.page_count ? texture_cache_lent.gap_block_index2 : NONE) *
		texture_page_kb);
	put_comma(&line);
	put_signed(&line, (texture_cache_lent.size + texture_cache_lent.size2) / 1024); put_comma(&line);
	put_signed(&line, sounds ? sounds->page_count * sound_page_kb : 0); put_comma(&line);
	put_signed(&line, cache_used_pages(sounds, NONE, NONE) * sound_page_kb); put_comma(&line);
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

	/* the map's name, for a file name: letters, digits, - and _; at most 22 of them (v1.187), as a FATX name holds 42
	and "_YYYYMMDD-HHMMSS.csv" takes 20 (+Boxfight/T4d Flood Room's 24 made a 44-character name that couldn't be
	made: no file for it, only "perf: 1 writes to the log failed") */
	for (index = 0; index < 22 && cache_file_globals.header.name[index]; index++)
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

/* ---------- the perf sweep (v1.131)

opence.ini perf_sweep = 1 (with or without perf_log): soon after the main menu is up, the showcase (showcase.c) plays
every map this log hasn't measured yet at its current version, one run after another, and this log measures each run
and writes a row for it to perf\sweep.csv beside the XBE (T:\perf when the XBE's folder can't be written). A press
stops it until the Xbox restarts; the next boot carries on where it stopped.

The runs: a multiplayer map plays three, with no bots, with 4 and with its suggested players' count (info.txt's
"Players:", else 8; at most 15; this run is left out when that's 4 or fewer), in a system link game the showcase hosts
(its camera shots, or a turn at each of up to 8 spawn points for a map without a showcase.txt). A campaign level plays
one, from its start at Normal, the player deathless and still, its own view. Each run's first SWEEP_WARMUP_SECONDS
aren't measured (the textures streaming in); then perf_sweep_seconds (60) are. A run that has stayed under 10 frames
a second for 5 seconds in a row fails at once, and the map's later runs are skipped (more bots are only slower).

A map is done, for this sweep, when perf\sweep.csv has a row for each of its runs at its current version (map
sharing's: the .map's size and its header's checksum; a stock map's never changes). A crash: before each run its key
goes in perf\sweep_now.txt, deleted after its row is written; one still there at the next boot is a run that never
ended (the Xbox crashed or hung there), written as "crashed", and the map's later runs skipped.

v1.199, the master table: without a pass (perf_sweep_pass, res_scale) perf\sweep.csv is the one table, every row's
last column its build's sweep version (SWEEP_VERSION, raised by a build that changes how fast maps run). A map's latest
measurement is its rows at the highest sweep version (version 0, the tables before: the newest build's). The runs go:
perf\sweep_priority.txt's maps (a key a line) not measured at this version; maps with a run never measured (new maps,
new map versions); maps measured at an older version that weren't perfect (the slowest run under SWEEP_SCALE_BELOW_FPS),
the slowest first, those that crashed, hung or didn't load last; with perf_sweep_redo = 1 every other map measured at an
older version. The first boot brings the pass tables (sweep_p<1-99>.csv) in once (sweep_import). A run cut short
without a crash's or the watchdog's mark (the console switched off) isn't counted but measured again; cut short twice,
it's written crashed.

v1.132: a folder map the map browser says has a problem (hmp_maps_problem: its header's quick check, or it failed to
load on this Xbox before) isn't loaded: its row is "bad_map", the reason in its limits column. And a watchdog: the
perf log's writer thread (sweep_watch) restarts H1U (a quick reboot into this XBE, as the updater is started) when a
run hasn't ended SWEEP_WATCH_SECONDS after perf_sweep_seconds from its start, or the main menu isn't back
SWEEP_RETURN_SECONDS after it ended. Before that, the last 16 KB of oxbox_log.txt (its ring; the next boot's log
replaces the file) go on the end of perf\sweep_hangs.txt, and sweep_now.txt is marked, so the run is written "hung"
at the next boot. A hang the writer thread can't run through (the processor stopped) still needs a reboot by hand.

v1.133, the resolution scaling test: with opence.ini res_scale (1-99) the sweep is a pass of its own: its rows go in
perf\sweep_r<scale>.csv (its first line says the scale) and its marker in sweep_now_r<scale>.txt, so the normal
pass's sweep.csv is untouched and every map is measured again at that scale; and when perf\sweep_list.txt is there it
plays only the maps it names (a key a line, as sweep.csv's key column; perf_sweep_report.py --write-list makes it).
perf_sweep_report.py --compare sets the passes side by side. v1.137: perf_sweep_pass (1-99) is a pass of its own the
same way (sweep_p<pass>.csv, sweep_now_p<pass>.txt, the list), at full size and without scaling runs: the same maps
measured again on another build, for --compare.

v1.168, spots: in a pass of its own (perf_sweep_pass), a list line may put a campaign level's player somewhere,
	<key> bsp <index> at <x> <y> <z> yaw <radians>
(the scenario's BSP index; the facing in radians, as Halo's own). Each such line is a run of its own, its rows' key
"<key> spot <n>" (n counting that level's spot lines from 1) and its name saying where; a level with spot lines plays
only those. The level's cutscenes are skipped (as a press would; one before the run measures puts the player back
after it), then, 3 seconds in at the soonest, its BSP is switched to and the player put on the spot facing that way, and kept there (the camera tour's pin: put back when it's moved; the BSP switched back when a
script changes it, 3 times at most); the run's warmup counts from then. Each pass may have its own list,
perf\sweep_list_p<pass>.txt, read instead of sweep_list.txt when it's there.

v1.138: a crash (oxbox_entry.c's handler) or a halt (main.c) stops the console where the watchdog's thread can't run:
both call sweep_fatal after their report, which keeps the log (sweep_hangs.txt, ", crashed") and restarts H1U; the
run's marker stays, so the next boot writes it "crashed" and the sweep goes on.

v1.139: the bots count the perf log's own memory as free (bots.c oxbox_perf_footprint), as a player's game would have
it, and a row whose bots still came up short says so in its limits ("bots without waypoints", "bots missing
waypoints"): their numbers flatter the map. An earlier build's measured bot runs (1, 2, 4, 5) aren't counted when
the rows are read: most of them had bots without waypoints, and they're measured again; no-bot and campaign runs, and
the skips after a failed run, stand.

v1.141: a press during a run interrupts it (not counted: showcase.c sends _oxbox_sweep_ended_interrupted) and the sweep
goes on once the main menu has had no press for 2 minutes (showcase_idle_milliseconds): a cat on the controller costs
a run, not the night. Back held 3 seconds on any controller (sweep_menu_frame) stops it until the next boot (at the
main menu: a hold begun in a run is broken by the menu's loading, which a moment up of a second doesn't count).

v1.134, the scaling runs (perf_sweep_scaling, on by default; never in a res_scale pass): once a map's normal runs all
have rows, one whose slowest run averaged under SWEEP_SCALE_BELOW_FPS with the graphics chip busy SWEEP_SCALE_GPU% of
it or more (not one the CPU holds back) plays that run again with the 3D view drawn at 85% (run 4), and when that's
still under the line, at 70% (run 5). res_scale is read each frame, so it's set for the run (sweep_set_scale) and put
back when the run ends, however it ends (a watchdog restart boots with opence.ini's anyway). Which maps want them is
worked out from the rows (sweep_note: each map's slowest normal run, a broken one, run 4 still slow), so it carries on
across boots, and maps measured before v1.134 get theirs when the sweep comes back to them.

v1.136: a map that can't load in the showcase's system link game stalls the game while Halo retries it, its own
client hears nothing from its own host for 8 seconds, and the game ends with "the game host went down": a "network
connection" error held for the main menu, whose dialog waited for a press (an hour on the console). While the sweep
goes on, sweep_menu_frame takes any error held for the main menu each frame (ui_widget_oxbox_take_main_menu_error,
logged), and the watchdog stays on at the main menu between runs (SWEEP_MENU_SECONDS) until the next run starts; a
press there stands it down (someone's at the menus). Campaign levels aren't put through the multiplayer list's check
(v1.132-v1.135 skipped every one as a bad map, "it isn't a multiplayer map"; those rows aren't counted, so they're
measured now).

perf\sweep.csv: "#" lines, a "status,..." line naming the columns, then a row a run (fields with commas quoted):
	status       ok, fail_fps (under 10 fps for 5 s), crashed, hung (the watchdog restarted H1U), bad_map (not loaded:
	             the map browser's problem), no_start (the game didn't start: the map didn't load), left (the game
	             went away while it played), skipped (an earlier run failed)
	key          the map as the game knows it ("+pack/folder", a stock level's name)
	identity     its version (hex), run (0 no bots, 1 four, 2 the suggested count, 3 a campaign level's, 4 and 5 the
	             scaling runs: the slowest of those again at 85% and 70%), bots
	build, utc   this XBE's build and the time (the Xbox's clock)
	name, path   its menu name, its .map's path on the Xbox; origin: ce / mcc / ce+mcc from its .h1ulink, xbox for a
	             stock map, unknown without a link file; file_kb (the .map's size), map_kb (its header's: unpacked);
	             type (mp / sp), bsps, encounters (the scenario's AI encounters)
	seconds, frames, avg_fps, low1_fps (the 99th percentile frame's rate), worst_ms; under30_pct...under15_pct (the
	             share of the time in frames longer than 34, 41, 51 and 68 ms); under10_s (seconds in seconds under
	             10 fps), longest_under10_s
	tick_ms, draw_ms, present_ms, vsync_ms (a frame's parts, the P rows' columns), gpu_busy_pct
	top1..top3   the three steps of a game tick or of drawing that took the most time (the P rows' column names), ms a frame
	objects_peak, actors_peak, particles_peak, effects_peak; level_triangles (a frame); texture_read_kb, texture_waits
	             (reads the game waited for), free_kb_min; limits (oxbox_diagnostics.c's: "particle x12;...")
The per-second CSVs (perf\<map>_<time>.csv) aren't written during a sweep unless perf_sweep_detail = 1: some hundreds
of maps would make thousands of them. port/oxbox/tools/perf_sweep_report.py reads perf\sweep.csv on a PC. */

/* the limits hit in this map so far (oxbox_diagnostics.c's): "particle x12;texture cache x3" */
extern struct oxbox_limit { char name[32]; long limit, times; } oxbox_limits[];
extern int oxbox_limit_count;

static void oxbox_limits_text(
	char *out,
	long size)
{
	long k, n = 0;

	out[0] = 0;
	for (k = 0; k < oxbox_limit_count; k++)
	{
		long length = (long)strlen(oxbox_limits[k].name);

		if (n + length + 16 >= size)
			break;
		n += _snprintf(out + n, size - n, "%s%s x%ld", k ? ";" : "", oxbox_limits[k].name, oxbox_limits[k].times);
	}
	out[size - 1] = 0;
}
long hmp_maps_count(void);
char const *hmp_maps_problem(long map_index);
void oxbox_log_flush(void);
void hmp_maps_scan(void);
char const *hmp_maps_key(long map_index);
long hmp_maps_find(char const *level_name);
char const *hmp_maps_stock_level(long stock_index);
wchar_t const *hmp_maps_title(char const *level_name);
boolean hmp_maps_map_path(long map_index, short root, char *path, long size);
char const *hmp_browse_stock_title(char const *level_name);
short hmp_browse_suggested_players(char const *level_name);
boolean custom_edition_maps_level_campaign(char const *level_name);
unsigned long hmp_share_map_version(char const *level_name);

enum
{
	SWEEP_STOCK_MULTIPLAYER = 13,               /* (hmp.h's HMP_STOCK_LEVELS) */
	SWEEP_STOCK_SOLO = 10,
	SWEEP_MAXIMUM_MAPS = 2048 + SWEEP_STOCK_MULTIPLAYER + SWEEP_STOCK_SOLO,
	SWEEP_KEY_LENGTH = 48,                      /* (showcase.c's SHOWCASE_KEY_LENGTH) */
	SWEEP_RUNS = 6,                             /* (0-3 as ever; 4 and 5 the scaling runs: 85% and 70%) */
	SWEEP_HISTOGRAM = 256,                      /* frame times, 1 ms each, the last everything longer */
	SWEEP_COLUMN_NAME = 40
};
#define SWEEP_WARMUP_SECONDS 4.5f               /* (the showcase's 1.5 s settle, then 3 s of textures streaming) */
#define SWEEP_SPOT_SETTLE_SECONDS 3.0f          /* (a spot run: the level's startup scripts this long before the spot, v1.168) */
#if !defined(SWEEP_FAIL_FPS)
#define SWEEP_FAIL_FPS 10.0   /* (a lab build may set it lower: xemu draws a campaign level slower) */
#endif
#define SWEEP_FAIL_SECONDS 5.0
#define SWEEP_WATCH_SECONDS 360                 /* (a run: its load, up to the showcase's 180 s, warm-up, leaving) */
#define SWEEP_RETURN_SECONDS 180                /* (back to the main menu after a run) */
#define SWEEP_MENU_SECONDS 300                  /* (at the main menu: the next run started; a press stands it down) */
#define SWEEP_SCALE_BELOW_FPS 29                /* a map whose slowest run is slower than this gets the scaling runs */
#define SWEEP_SCALE_GPU 50                      /* ...if the graphics chip was busy this share (percent) of it, or more */
#define SWEEP_SCALE_85 85                       /* run 4's res_scale; run 5's (when run 4 is still slow) */
#define SWEEP_SCALE_70 70
#define SWEEP_RUNS_ROUGH 0x40                   /* (sweep_done.runs: run 4 was still slower than SWEEP_SCALE_BELOW_FPS) */
#define SWEEP_RUNS_BROKEN 0x80                  /* (a normal run crashed, hung or didn't load: no scaling runs) */
#define SWEEP_RUNS_MASK 0x3f

static char const *const sweep_solo_levels[SWEEP_STOCK_SOLO] =
{
	"levels\\a10\\a10", "levels\\a30\\a30", "levels\\a50\\a50", "levels\\b30\\b30", "levels\\b40\\b40",
	"levels\\c10\\c10", "levels\\c20\\c20", "levels\\c40\\c40", "levels\\d20\\d20", "levels\\d40\\d40"
};

static char const sweep_fields[] =
	"status,key,identity,run,bots,build,utc,name,path,origin,file_kb,map_kb,type,bsps,encounters,seconds,frames,"
	"avg_fps,low1_fps,worst_ms,under30_pct,under25_pct,under20_pct,under15_pct,under10_s,longest_under10_s,tick_ms,"
	"draw_ms,present_ms,vsync_ms,gpu_busy_pct,top1,top1_ms,top2,top2_ms,top3,top3_ms,objects_peak,actors_peak,"
	"particles_peak,effects_peak,level_triangles,texture_read_kb,texture_waits,free_kb_min,limits,sweep_version";

/* v1.199: the sweep version, every row's last column (none in the tables before: 0). Raised by a build that changes how
fast maps run (v1.199: 1, after v1.195-198's multiplayer savings; v1.200: 2, resting vehicles, equipment, scenery and
off lights; v1.203: 3, resting doors, lifts and switches, teleporters): the maps measured at an older one that weren't
perfect are measured again, the slowest first (sweep_next) */
#define SWEEP_VERSION 3
#define SWEEP_PRIORITY "sweep_priority.txt"     /* (perf\: maps measured first, a key a line) */

/* v1.201: perf_sweep_lod = 1 with perf_sweep_pass N: the pass's maps (perf\sweep_list_p<N>.txt) measured with widescreen
models' level of detail size (opence.ini lod_widescreen, render_objects.c) at each of these, one after the other, round r
in perf\sweep_p<N+r>.csv (its first line says the view), all in one boot; perf_sweep_report.py --compare sets two side by
side */
static long const sweep_lod_values[] = { 50, 75, 100 };

struct sweep_done
{
	unsigned long key_hash, identity;
	unsigned char runs;                         /* (a bit a run with a row, at any sweep version) */
	unsigned char slow_run;                     /* its slowest normal run (0-3), 0xff none yet */
	unsigned char slow_fps4, slow_gpu;          /* (that run's frame rate, in quarters of a frame a second; its busy %) */
	/* v1.199: its latest measurement: the highest sweep version (version 0, the tables before: the newest build's
	rows), its runs (a bit each; SWEEP_RUNS_ROUGH, SWEEP_RUNS_BROKEN), and the slowest run above is that one's */
	unsigned char version, build, latest;
};

static struct
{
	boolean on, detail, initialized;
	long seconds;                               /* measured a run (perf_sweep_seconds) */
	char drive;                                 /* where perf\ is: 'D' or 'T' */
	long done_count;
	struct sweep_done done[SWEEP_MAXIMUM_MAPS];
	char column_names[NUMBEROF(window_time_columns)][SWEEP_COLUMN_NAME];
	/* the map at the cursor */
	long cursor, map_count;
	boolean map_ready;
	char key[SWEEP_KEY_LENGTH];                 /* (the rows': the map's, or "<map key> spot <n>", v1.168) */
	char map_key[SWEEP_KEY_LENGTH];             /* (the map's own, which loads it) */
	boolean solo;
	long spot, spot_count;                      /* (a campaign level's spot run, from 1, and how many it has; 0 none) */
	short spot_bsp, spot_switches;
	real_point3d spot_position;
	real spot_yaw;
	float spot_ready, spot_skip;                /* (the run's seconds when the player was put there, 0 not yet; a skip asked) */
	char list[24];                              /* (perf\sweep_list_p<pass>.txt when there's one, else sweep_list.txt) */
	unsigned long identity;
	unsigned char needed, have;                 /* (its runs: wanted, and with a row) */
	short full_bots;
	/* the run under way */
	boolean active, measuring, over;
	long run;
	short bots;
	char name[96], path[200], origin[12];
	unsigned long file_kb, map_kb;
	long bsps, encounters;
	/* its measures */
	__int64 time, frame_worst, ticks, drawing, present, vsync;
	__int64 under[4];                           /* (frames longer than 34, 41, 51, 68 ms) */
	unsigned long frames, histogram[SWEEP_HISTOGRAM];
	unsigned long gpu_busy, gpu_samples, texture_bytes, texture_waits;
	unsigned long objects_peak, actors_peak, particles_peak, effects_peak, free_kb_min;
	unsigned __int64 columns[NUMBEROF(window_time_columns)];
	unsigned long level_triangles, window_frames;   /* (the P rows' seconds the run measured, and their frames) */
	__int64 second_start;
	unsigned long second_frames;
	double low_seconds, under10_seconds, longest_low;
	char status[12];
	char const *problem;                        /* (a bad_map row's reason: the map browser's) */
	boolean scaling;                            /* (perf_sweep_scaling: runs 4 and 5 for the slow maps) */
	unsigned char needed_base, slow_run;        /* (the map's normal runs; the run the scaling runs repeat) */
	long base_scale;                            /* (opence.ini's res_scale, put back after a scaling run) */
	boolean halted;                             /* (stopped by a press, or every map done: errors shown again) */
	boolean at_menu;                            /* (back at the main menu, the next run not started) */
	unsigned long menu_input;                   /* (the input clock then: a press since stands the watchdog down) */
	__int64 back_since, back_seen;              /* (Back held since then, last seen down: 3 seconds stop it till the next boot) */
	long scale;                                 /* (opence.ini res_scale, 1-99: a scaled pass; 0 the normal one) */
	long pass;                                  /* (opence.ini perf_sweep_pass, 1-99: a pass of its own; 0 the normal one) */
	long lod_rounds;                            /* (v1.201 perf_sweep_lod: the pass played again at each SWEEP_LOD_VALUES) */
	long base_pass;                             /* (... the first round's pass; each round the next number) */
	long base_lod;                              /* (... opence.ini's lod_widescreen, put back after) */
	char results[24], marker[24];               /* (its files in perf\: sweep.csv, sweep_now.txt; sweep_r70.csv...) */
	long runs_this_boot;                        /* (rows written since this boot: the sweep complete box's text) */
	/* v1.199, the master table (perf\sweep.csv, no pass): which maps come next (sweep_next) */
	long phase;                                 /* (SWEEP_PHASE_...) */
	long step;                                  /* (the phase's: its priority line, map index or place in the order) */
	long order_count;                           /* (the stale slow maps, slowest first: sweep_order) */
	boolean redo_all;                           /* (opence.ini perf_sweep_redo = 1: the perfect maps measured again too) */
	boolean imported;                           /* (sweep.csv says the pass tables were brought in) */
	unsigned long again_hash;                   /* (a run cut short last boot, the console switched off: its key's hash, */
	long again_run;                             /* and run, -1 none: if it's cut short again, it crashed) */
} sweep;

enum
{
	SWEEP_PHASE_PRIORITY,                       /* perf\sweep_priority.txt's maps, not measured at this sweep version */
	SWEEP_PHASE_NEW,                            /* maps never measured (or a run missing), new versions too */
	SWEEP_PHASE_SLOW,                           /* measured at an older sweep version, not perfect: slowest first */
	SWEEP_PHASE_REST,                           /* measured at an older sweep version (perf_sweep_redo = 1 only) */
	SWEEP_PHASE_DONE
};

/* (the stale slow maps' indices, slowest first, and their frame rates in quarters while sorting) */
static short sweep_order[SWEEP_MAXIMUM_MAPS];
static unsigned char sweep_order_fps4[SWEEP_MAXIMUM_MAPS];

/* the watchdog (sweep_watch, on the writer thread): on while a run is under way or coming back from one */
static volatile long sweep_watching;
static volatile __int64 sweep_watch_deadline;

static void sweep_spot_end(void);

static void sweep_watch_on(
	long seconds)
{
	LARGE_INTEGER now;

	sweep_watching = 0;
	QueryPerformanceCounter(&now);
	sweep_watch_deadline = now.QuadPart + perf.frequency * seconds;
	PERF_BARRIER();
	sweep_watching = seconds;
}

static void sweep_watch_off(
	void)
{
	sweep_watching = 0;
}

static unsigned long sweep_hash(
	char const *text)
{
	unsigned long hash = 2166136261ul;

	for (; *text; text++)
		hash = (hash ^ (unsigned char)(*text >= 'A' && *text <= 'Z' ? *text + 32 : *text)) * 16777619ul;
	return hash;
}

static void sweep_path(
	char *path,
	long size,
	char const *file)
{
	_snprintf(path, size, "%c:\\perf\\%s", sweep.drive ? sweep.drive : 'D', file);
	path[size - 1] = 0;
}

static struct sweep_done *sweep_find(
	unsigned long key_hash,
	unsigned long identity,
	boolean add)
{
	long index;

	for (index = 0; index < sweep.done_count; index++)
		if (sweep.done[index].key_hash == key_hash && sweep.done[index].identity == identity)
			return &sweep.done[index];
	if (!add || sweep.done_count >= SWEEP_MAXIMUM_MAPS)
		return NULL;
	sweep.done[sweep.done_count].key_hash = key_hash;
	sweep.done[sweep.done_count].identity = identity;
	sweep.done[sweep.done_count].runs = 0;
	sweep.done[sweep.done_count].slow_run = 0xff;
	sweep.done[sweep.done_count].slow_fps4 = sweep.done[sweep.done_count].slow_gpu = 0;
	sweep.done[sweep.done_count].version = sweep.done[sweep.done_count].build = sweep.done[sweep.done_count].latest = 0;
	return &sweep.done[sweep.done_count++];
}

/* a CSV field: its text from line (at), up to its comma; at moves past it */
static long sweep_field(
	char const *line,
	long *at,
	char *out,
	long size)
{
	long n = 0, k = *at;
	boolean quoted = line[k] == '"';

	if (quoted)
		k++;
	for (; line[k]; k++)
	{
		if (quoted && line[k] == '"')
		{
			if (line[k + 1] == '"')
			{
				k++;
			}
			else
			{
				quoted = FALSE;
				continue;
			}
		}
		else if (!quoted && line[k] == ',')
		{
			break;
		}
		if (n < size - 1)
			out[n++] = line[k];
	}
	out[n] = 0;
	*at = line[k] == ',' ? k + 1 : k;
	return n;
}

/* a row's results, for the map's scaling runs: its slowest normal run (frame rate, graphics chip), whether one
broke, and whether run 4 (85%) was still slow */
static void sweep_note(
	struct sweep_done *done,
	long run,
	char const *status,
	double fps,
	long gpu)
{
	boolean measured = !strcmp(status, "ok") || !strcmp(status, "fail_fps");
	long fps4 = fps <= 0.0 ? 0 : fps >= 63.0 ? 252 : (long)(fps * 4.0 + 0.5);

	if (!done)
		return;
	if (run <= 3)
	{
		if (!measured && strcmp(status, "skipped"))
			done->latest |= SWEEP_RUNS_BROKEN;
		else if (measured && fps > 0.0 && (done->slow_run == 0xff || fps4 < done->slow_fps4))
		{
			done->slow_run = (unsigned char)run;
			done->slow_fps4 = (unsigned char)fps4;
			done->slow_gpu = (unsigned char)(gpu < 0 ? 0 : gpu > 100 ? 100 : gpu);
		}
	}
	else if (run == 4 && !strcmp(status, "ok") && fps4 < SWEEP_SCALE_BELOW_FPS * 4)
	{
		done->latest |= SWEEP_RUNS_ROUGH;
	}
}

/* v1.199: a row for the map: its run measured, and its results the latest when it's of the map's newest measurement
(the highest sweep version; within version 0, the tables before it, the newest build's: v1.<100 + build>) */
static void sweep_take(
	struct sweep_done *done,
	long run,
	char const *status,
	double fps,
	long gpu,
	unsigned char version,
	unsigned char build)
{
	if (!done || run < 0 || run >= SWEEP_RUNS)
		return;
	done->runs |= (unsigned char)(1 << run);
	if (!done->latest || version > done->version || (version == done->version && !version && build > done->build))
	{
		done->version = version;
		done->build = build;
		done->latest = 0;
		done->slow_run = 0xff;
		done->slow_fps4 = done->slow_gpu = 0;
	}
	else if (version < done->version || (!version && build < done->build))
	{
		return;
	}
	done->latest |= (unsigned char)(1 << run);
	sweep_note(done, run, status, fps, gpu);
}

/* the scaling runs the map wants now (runs 4 and 5's bits), once its normal runs all have rows */
static unsigned char sweep_extra(
	struct sweep_done const *done)
{
	unsigned char extra;

	if (!done || !sweep.scaling || sweep.scale || sweep.pass || done->version != SWEEP_VERSION ||
		(done->latest & sweep.needed_base) != sweep.needed_base ||
		(done->latest & SWEEP_RUNS_BROKEN) || done->slow_run == 0xff ||
		done->slow_fps4 >= SWEEP_SCALE_BELOW_FPS * 4 || done->slow_gpu < SWEEP_SCALE_GPU)
		return 0;
	extra = 1 << 4;
	if ((done->latest & (1 << 4)) && (done->latest & SWEEP_RUNS_ROUGH))
		extra |= 1 << 5;
	return extra;
}

static void sweep_read_line(
	char const *line)
{
	char status[16], key[SWEEP_KEY_LENGTH + 8], identity[16], run[8], field[64];
	long at = 0, k, gpu = 0;
	double fps = 0.0;
	struct sweep_done *done;

	if (!strncmp(line, "# imported", 10))
		sweep.imported = TRUE;
	if (line[0] == '#' || !strncmp(line, "status,", 7))
		return;
	sweep_field(line, &at, status, sizeof(status));
	sweep_field(line, &at, key, sizeof(key));
	sweep_field(line, &at, identity, sizeof(identity));
	sweep_field(line, &at, run, sizeof(run));
	if (!status[0] || !key[0] || run[0] < '0' || run[0] >= '0' + SWEEP_RUNS)
		return;
	/* (v1.132-v1.135 skipped campaign levels as bad maps, the multiplayer list's reason: not counted) */
	if (!strcmp(status, "bad_map") && strstr(line, "isn't a multiplayer map"))
		return;
	/* (before v1.139 the sweep's bots mostly went without waypoints, the sweep's own memory under their reserve: an
	earlier build's measured bot run isn't counted, and is measured again) */
	if ((run[0] == '1' || run[0] == '2' || run[0] == '4' || run[0] == '5') &&
		(!strcmp(status, "ok") || !strcmp(status, "fail_fps")))
	{
		char const *build = line;
		long field;

		for (field = 0; field < 5 && build; field++)
		{
			build = strchr(build, ',');
			if (build)
				build++;
		}
		if (build && !strncmp(build, "v1.", 3) && strtol(build + 3, NULL, 10) < 139)
			return;
	}
	done = sweep_find(sweep_hash(key), strtoul(identity, NULL, 16), TRUE);
	if (!done)
		return;
	/* (its build, the 6th column, avg_fps the 18th, gpu_busy_pct the 31st, and v1.199's sweep_version the last: none
	in the tables before) */
	{
		long version_column = 0, build = 0, version = 0;
		char const *f;

		for (f = sweep_fields; *f; f++)
			version_column += *f == ',';
		for (k = 4; k <= version_column && line[at]; k++)
		{
			sweep_field(line, &at, field, sizeof(field));
			if (k == 5 && !strncmp(field, "v1.", 3))
				build = strtol(field + 3, NULL, 10) - 100;
			else if (k == 17)
				fps = atof(field);
			else if (k == 30)
				gpu = strtol(field, NULL, 10);
			else if (k == version_column)
				version = strtol(field, NULL, 10);
		}
		sweep_take(done, run[0] - '0', status, fps, gpu, (unsigned char)(version < 0 ? 0 : version > 255 ? 255 : version),
			(unsigned char)(build < 0 ? 0 : build > 255 ? 255 : build));
	}
}

/* perf\sweep.csv's rows: the runs done */
static void sweep_read_results(
	void)
{
	char path[64], buffer[4096], line[512];
	long line_length = 0;
	unsigned long got;
	HANDLE file;

	sweep_path(path, sizeof(path), sweep.results);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	while (ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got)
	{
		unsigned long k;

		for (k = 0; k < got; k++)
		{
			if (buffer[k] == '\n' || buffer[k] == '\r')
			{
				line[line_length] = 0;
				if (line_length)
					sweep_read_line(line);
				line_length = 0;
			}
			else if (line_length < (long)sizeof(line) - 1)
			{
				line[line_length++] = buffer[k];
			}
		}
	}
	line[line_length] = 0;
	if (line_length)
		sweep_read_line(line);
	CloseHandle(file);
}

/* the CSV's text field: quoted when it has a comma or a quote */
static void sweep_put_text(
	struct perf_line *line,
	char const *text)
{
	char const *p;

	if (!strchr(text, ',') && !strchr(text, '"'))
	{
		put_text(line, text);
		return;
	}
	put_char(line, '"');
	for (p = text; *p; p++)
	{
		if (*p == '"')
			put_char(line, '"');
		put_char(line, *p);
	}
	put_char(line, '"');
}

static void sweep_put_hex(
	struct perf_line *line,
	unsigned long value)
{
	long shift;

	for (shift = 28; shift >= 0; shift -= 4)
		put_char(line, "0123456789abcdef"[(value >> shift) & 15]);
}

static void sweep_append(
	char const *text,
	long length)
{
	char path[64];
	unsigned long put;
	HANDLE file;

	sweep_path(path, sizeof(path), sweep.results);
	file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
	{
		platform_log("perf sweep: %s couldn't be written (error %lu)", path, (unsigned long)GetLastError());
		return;
	}
	if (!GetFileSize(file, NULL))
	{
		char head[160];
		extern int oxbox_widescreen(void);
		extern int oxbox_setting_lod_widescreen;

		/* (v1.201: and the view the table's rows were measured in) */
		_snprintf(head, sizeof(head), "# Halo 1 Unlimited perf sweep v1 (port/oxbox/oxbox_perf.c), res_scale %ld, "
			"widescreen %d, lod_widescreen %d\r\n", sweep.scale ? sweep.scale : 100L, oxbox_widescreen() ? 1 : 0,
			oxbox_setting_lod_widescreen);
		head[sizeof(head) - 1] = 0;
		WriteFile(file, head, (unsigned long)strlen(head), &put, NULL);
		WriteFile(file, sweep_fields, sizeof(sweep_fields) - 1, &put, NULL);
		WriteFile(file, "\r\n", 2, &put, NULL);
	}
	SetFilePointer(file, 0, NULL, FILE_END);
	WriteFile(file, text, (unsigned long)length, &put, NULL);
	CloseHandle(file);
}

static void sweep_marker(
	long set)                                   /* (0 deleted, 1 written, 2 written as hung: the watchdog's, 3 as crashed) */
{
	char path[64];

	sweep_path(path, sizeof(path), sweep.marker);
	if (set)
	{
		char text[96];
		unsigned long put;
		HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if (file == INVALID_HANDLE_VALUE)
			return;
		/* (v1.199: "again", the run the last boot's was cut short in, tried again) */
		_snprintf(text, sizeof(text), "%s\t%ld\t%08lx%s\r\n", sweep.key, sweep.run, sweep.identity,
			set == 2 ? "\thung" : set == 3 ? "\tcrashed" :
			sweep_hash(sweep.key) == sweep.again_hash && sweep.run == sweep.again_run ? "\tagain" : "");
		text[sizeof(text) - 1] = 0;
		WriteFile(file, text, (unsigned long)strlen(text), &put, NULL);
		CloseHandle(file);
	}
	else
	{
		DeleteFileA(path);
	}
}

/* the .h1ulink files beside the map: which games' maps it was made from */
static void sweep_origin(
	char const *folder)
{
	WIN32_FIND_DATAA found;
	char pattern[220];
	boolean ce = FALSE, mcc = FALSE;
	HANDLE find;

	strcpy(sweep.origin, "unknown");
	_snprintf(pattern, sizeof(pattern), "%s\\*.h1ulink", folder);
	pattern[sizeof(pattern) - 1] = 0;
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		char path[260], text[2049];
		unsigned long got = 0;
		HANDLE file;
		long k;

		_snprintf(path, sizeof(path), "%s\\%s", folder, found.cFileName);
		path[sizeof(path) - 1] = 0;
		file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
			got = 0;
		CloseHandle(file);
		text[got] = 0;
		for (k = 0; k < (long)got; k++)
		{
			if (k && text[k - 1] != '\n')
				continue;
			if (!strncmp(text + k, "ce ", 3) || !strncmp(text + k, "ce\t", 3))
				ce = TRUE;
			else if (!strncmp(text + k, "mcc ", 4) || !strncmp(text + k, "mcc\t", 4))
				mcc = TRUE;
		}
	}
	while (FindNextFileA(find, &found));
	CloseHandle(find);
	strcpy(sweep.origin, ce && mcc ? "ce+mcc" : ce ? "ce" : mcc ? "mcc" : "unknown");
}

/* the map at index in the sweep's order: the folder maps, then Halo's multiplayer maps, then its campaign's levels
(the ones this Xbox has) */
/* v1.199: a folder map left out of the sweep: a file named "nosweep" (or nosweep.txt) in its folder or a folder above it,
up to the maps folder (a pack's: maps\\<pack>\\nosweep). Said once a folder */
static boolean sweep_map_excluded(
	long index)
{
	static char said[200];
	char path[200], file[216], *slash;

	if (!hmp_maps_map_path(index, NONE, path, sizeof(path)))
		return FALSE;
	while ((slash = strrchr(path, '\\')) != NULL)
	{
		char const *name;

		*slash = 0;
		name = strrchr(path, '\\');
		name = name ? name + 1 : path;
		if (!csstrcasecmp(name, "maps") || strchr(name, ':'))
			break;
		_snprintf(file, sizeof(file), "%s\\nosweep", path);
		file[sizeof(file) - 1] = 0;
		if (GetFileAttributesA(file) == (DWORD)-1)
		{
			_snprintf(file, sizeof(file), "%s\\nosweep.txt", path);
			file[sizeof(file) - 1] = 0;
			if (GetFileAttributesA(file) == (DWORD)-1)
				continue;
		}
		if (strcmp(said, path))
		{
			strcpy(said, path);
			platform_log("perf sweep: the maps in %s are left out (its nosweep file)", path);
		}
		return TRUE;
	}
	return FALSE;
}

static boolean sweep_map_key(
	long index,
	char *key,
	boolean *solo)
{
	long folder_maps = hmp_maps_count();
	char const *name = NULL;

	*solo = FALSE;
	if (index < folder_maps)
	{
		if (sweep_map_excluded(index))
			return FALSE;
		name = hmp_maps_key(index);
		*solo = name && custom_edition_maps_level_campaign(name);
	}
	else if (index < folder_maps + SWEEP_STOCK_MULTIPLAYER)
	{
		name = hmp_maps_stock_level(index - folder_maps);
	}
	else if (index < folder_maps + SWEEP_STOCK_MULTIPLAYER + SWEEP_STOCK_SOLO)
	{
		name = sweep_solo_levels[index - folder_maps - SWEEP_STOCK_MULTIPLAYER];
		*solo = TRUE;
	}
	if (!name || !name[0])
		return FALSE;
	/* (Halo's own maps this Xbox has: maps\<name>.map) */
	if (index >= folder_maps)
	{
		char path[48];
		char const *leaf = strrchr(name, '\\');

		_snprintf(path, sizeof(path), "d:\\maps\\%s.map", leaf ? leaf + 1 : name);
		path[sizeof(path) - 1] = 0;
		if (GetFileAttributesA(path) == (DWORD)-1)
			return FALSE;
	}
	if ((long)strlen(name) >= SWEEP_KEY_LENGTH)
	{
		platform_log("perf sweep: %s left out: its key is longer than %d characters", name, SWEEP_KEY_LENGTH - 1);
		return FALSE;
	}
	strcpy(key, name);
	return TRUE;
}

/* the run's map: its name, path, origin, sizes and type */
static void sweep_map_info(
	void)
{
	char const *leaf = strrchr(sweep.map_key, '\\');
	unsigned char header[0x800];
	unsigned long got = 0;
	HANDLE file;

	leaf = leaf ? leaf + 1 : sweep.map_key;
	sweep.name[0] = sweep.path[0] = 0;
	sweep.file_kb = sweep.map_kb = 0;
	if (sweep.map_key[0] == '+')
	{
		wchar_t const *title = hmp_maps_title(sweep.map_key);
		long index = hmp_maps_find(sweep.map_key), k;

		for (k = 0; title && title[k] && k < (long)sizeof(sweep.name) - 1; k++)
			sweep.name[k] = (char)(title[k] >= 32 && title[k] < 127 ? title[k] : '?');
		sweep.name[k] = 0;
		if (index == NONE || !hmp_maps_map_path(index, NONE, sweep.path, sizeof(sweep.path)))
			sweep.path[0] = 0;
	}
	else
	{
		char const *title = hmp_browse_stock_title(sweep.map_key);

		csstrcpy(sweep.name, title ? title : leaf);
		_snprintf(sweep.path, sizeof(sweep.path), "d:\\maps\\%s.map", leaf);
		sweep.path[sizeof(sweep.path) - 1] = 0;
	}
	if (!sweep.name[0])
		csstrcpy(sweep.name, leaf[0] == '+' ? leaf + 1 : leaf);
	/* (a spot: where, v1.168) */
	if (sweep.spot && sweep.spot_count)
	{
		long length = (long)strlen(sweep.name);

		_snprintf(sweep.name + length, sizeof(sweep.name) - length, " at BSP %d (%.1f %.1f %.1f yaw %.2f)",
			(int)sweep.spot_bsp, sweep.spot_position.x, sweep.spot_position.y, sweep.spot_position.z, sweep.spot_yaw);
		sweep.name[sizeof(sweep.name) - 1] = 0;
	}
	strcpy(sweep.origin, sweep.map_key[0] == '+' ? "unknown" : "xbox");
	if (!sweep.path[0])
		return;
	file = CreateFileA(sweep.path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		sweep.file_kb = (GetFileSize(file, NULL) + 1023) / 1024;
		if (ReadFile(file, header, sizeof(header), &got, NULL) && got == sizeof(header))
			sweep.map_kb = (*(unsigned long const *)(header + 8) + 1023) / 1024;
		CloseHandle(file);
	}
	if (sweep.map_key[0] == '+')
	{
		char folder[200];
		char *end;

		csstrcpy(folder, sweep.path);
		end = strrchr(folder, '\\');
		if (end)
		{
			*end = 0;
			sweep_origin(folder);
		}
	}
}

/* the measures cleared, for a run starting (or written without one) */
static void sweep_clear_measures(
	void)
{
	sweep.time = sweep.frame_worst = sweep.ticks = sweep.drawing = sweep.present = sweep.vsync = 0;
	sweep.problem = NULL;
	memset(sweep.under, 0, sizeof(sweep.under));
	sweep.frames = 0;
	memset(sweep.histogram, 0, sizeof(sweep.histogram));
	sweep.gpu_busy = sweep.gpu_samples = sweep.texture_bytes = sweep.texture_waits = 0;
	sweep.objects_peak = sweep.actors_peak = sweep.particles_peak = sweep.effects_peak = 0;
	sweep.free_kb_min = 0;
	memset(sweep.columns, 0, sizeof(sweep.columns));
	sweep.level_triangles = sweep.window_frames = 0;
	sweep.second_frames = 0;
	sweep.second_start = 0;
	sweep.low_seconds = sweep.under10_seconds = sweep.longest_low = 0.0;
	sweep.bsps = sweep.encounters = -1;
}

/* the run's row in perf\sweep.csv */
static void sweep_row(
	char const *status)
{
	struct perf_line line;
	double seconds = ticks_to_ms(sweep.time) / 1000.0;
	double frames = sweep.frames ? (double)sweep.frames : 1.0;
	double window_frames = sweep.window_frames ? (double)sweep.window_frames : 1.0;
	SYSTEMTIME time;
	char text[96];
	long column, top[3] = { NONE, NONE, NONE }, rank;

	line.length = 0;
	put_text(&line, status); put_comma(&line);
	sweep_put_text(&line, sweep.key); put_comma(&line);
	sweep_put_hex(&line, sweep.identity); put_comma(&line);
	put_signed(&line, sweep.run); put_comma(&line);
	put_signed(&line, sweep.bots); put_comma(&line);
	put_text(&line, "v1."); put_unsigned(&line, OXBOX_BUILD_NUMBER); put_comma(&line);
	GetSystemTime(&time);
	_snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d", time.wYear, time.wMonth, time.wDay, time.wHour,
		time.wMinute, time.wSecond);
	text[sizeof(text) - 1] = 0;
	put_text(&line, text); put_comma(&line);
	sweep_put_text(&line, sweep.name); put_comma(&line);
	sweep_put_text(&line, sweep.path); put_comma(&line);
	put_text(&line, sweep.origin); put_comma(&line);
	put_unsigned(&line, sweep.file_kb); put_comma(&line);
	put_unsigned(&line, sweep.map_kb); put_comma(&line);
	put_text(&line, sweep.solo ? "sp" : "mp"); put_comma(&line);
	if (sweep.bsps >= 0)
		put_signed(&line, sweep.bsps);
	put_comma(&line);
	if (sweep.encounters >= 0)
		put_signed(&line, sweep.encounters);
	put_comma(&line);
	if (sweep.frames)
	{
		unsigned long count = 0, low_ms = SWEEP_HISTOGRAM - 1;
		long k;

		/* (the 99th percentile frame: the frame time 1% of the frames are longer than) */
		for (k = SWEEP_HISTOGRAM - 1; k >= 0; k--)
		{
			count += sweep.histogram[k];
			if (count * 100 >= sweep.frames)
			{
				low_ms = (unsigned long)k;
				break;
			}
		}
		put_fixed(&line, seconds, 1); put_comma(&line);
		put_unsigned(&line, sweep.frames); put_comma(&line);
		put_fixed(&line, seconds > 0.0 ? sweep.frames / seconds : 0.0, 1); put_comma(&line);
		put_fixed(&line, 1000.0 / (low_ms + 1), 1); put_comma(&line);
		put_fixed(&line, ticks_to_ms(sweep.frame_worst), 1); put_comma(&line);
		for (k = 0; k < 4; k++)
		{
			put_fixed(&line, sweep.time ? 100.0 * (double)sweep.under[k] / (double)sweep.time : 0.0, 1);
			put_comma(&line);
		}
		put_fixed(&line, sweep.under10_seconds, 1); put_comma(&line);
		put_fixed(&line, sweep.longest_low, 1); put_comma(&line);
		put_fixed(&line, ticks_to_ms(sweep.ticks) / frames, 2); put_comma(&line);
		put_fixed(&line, ticks_to_ms(sweep.drawing) / frames, 2); put_comma(&line);
		put_fixed(&line, ticks_to_ms(sweep.present) / frames, 2); put_comma(&line);
		put_fixed(&line, ticks_to_ms(sweep.vsync) / frames, 2); put_comma(&line);
		if (sweep.gpu_samples)
			put_unsigned(&line, (sweep.gpu_busy * 100 + sweep.gpu_samples / 2) / sweep.gpu_samples);
		put_comma(&line);
		/* (the three biggest steps: not the profile sections, which are parts of the steps) */
		for (rank = 0; rank < 3; rank++)
		{
			for (column = 0; column < (long)NUMBEROF(window_time_columns); column++)
			{
				if (window_time_columns[column] >= 0 && window_time_columns[column] < NUMBER_OF_SECTIONS)
					continue;
				if ((rank > 0 && column == top[0]) || (rank > 1 && column == top[1]))
					continue;
				if (top[rank] == NONE || sweep.columns[column] > sweep.columns[top[rank]])
					top[rank] = column;
			}
			if (top[rank] != NONE)
				put_text(&line, sweep.column_names[top[rank]]);
			put_comma(&line);
			if (top[rank] != NONE)
				put_fixed(&line, cycles_to_ms((double)(__int64)sweep.columns[top[rank]]) / window_frames, 2);
			put_comma(&line);
		}
		put_unsigned(&line, sweep.objects_peak); put_comma(&line);
		put_unsigned(&line, sweep.actors_peak); put_comma(&line);
		put_unsigned(&line, sweep.particles_peak); put_comma(&line);
		put_unsigned(&line, sweep.effects_peak); put_comma(&line);
		if (sweep.window_frames)
			put_unsigned(&line, sweep.level_triangles / sweep.window_frames);
		put_comma(&line);
		put_unsigned(&line, sweep.texture_bytes / 1024); put_comma(&line);
		put_unsigned(&line, sweep.texture_waits); put_comma(&line);
		put_unsigned(&line, sweep.free_kb_min); put_comma(&line);
	}
	else
	{
		/* (the 30 measured columns, seconds to free_kb_min, empty: before v1.199 one comma too many, a skipped row a
		column longer than its table) */
		put_text(&line, ",,,,,,,,,,,,,,,,,,,,,,,,,,,,,,");
	}
	text[0] = 0;
	if (sweep.problem)
		_snprintf(text, sizeof(text), "problem: %s", sweep.problem), text[sizeof(text) - 1] = 0;
	else if (strcmp(status, "skipped") && strcmp(status, "crashed") && strcmp(status, "hung"))
	{
		extern long bots_oxbox_short;

		oxbox_limits_text(text, sizeof(text));
		/* (the bots short of memory: their numbers flatter the map) */
		if (bots_oxbox_short && strlen(text) < sizeof(text) - 40)
			strcat(text, bots_oxbox_short == 2 ? (text[0] ? ";bots without waypoints" : "bots without waypoints") :
				(text[0] ? ";bots missing waypoints" : "bots missing waypoints"));
	}
	sweep_put_text(&line, text); put_comma(&line);
	put_unsigned(&line, SWEEP_VERSION);
	line.text[line.length++] = '\r';
	line.text[line.length++] = '\n';
	sweep_append(line.text, line.length);
	sweep.have |= (unsigned char)(1 << sweep.run);
	sweep_take(sweep_find(sweep_hash(sweep.key), sweep.identity, TRUE), sweep.run, status,
		sweep.frames && seconds > 0.0 ? sweep.frames / seconds : 0.0,
		sweep.gpu_samples ? (long)((sweep.gpu_busy * 100 + sweep.gpu_samples / 2) / sweep.gpu_samples) : 0,
		SWEEP_VERSION, (unsigned char)(OXBOX_BUILD_NUMBER - 100));
}

/* the run's bots (run 0, 1 or 2) */
static short sweep_run_bots(
	long run)
{
	if (run >= 4)
		run = sweep.slow_run;
	return run == 0 || run == 3 ? 0 : run == 1 ? 4 : sweep.full_bots;
}

/* the scale this run draws at (a scaling run's; else opence.ini's) */
static void sweep_set_scale(
	long run)
{
	extern int oxbox_setting_res_scale;

	oxbox_setting_res_scale = run == 4 ? SWEEP_SCALE_85 : run == 5 ? SWEEP_SCALE_70 : (int)sweep.base_scale;
}

/* back at the main menu between runs: the watchdog on till the next run starts (SWEEP_MENU_SECONDS), unless a press
says someone's at the menus */
static void sweep_at_menu(
	void)
{
	if (sweep.halted)
		return;
	sweep.at_menu = TRUE;
	sweep.menu_input = event_manager_time_of_last_event();
	sweep_watch_on(SWEEP_MENU_SECONDS);
}

/* each frame while the sweep goes on (perf_frame): an error Halo has waiting for the main menu dropped (logged), so no
dialog waits for a press; and a press at the main menu between runs stands the watchdog down */
static void sweep_menu_frame(
	void)
{
	short ui_widget_oxbox_take_main_menu_error(void);
	short error_code;

	if (sweep.halted)
		return;
	/* (Back held 3 seconds on any controller: the sweep stops until the next boot) */
	{
		boolean back = FALSE;
		short pad;
		LARGE_INTEGER now;

		for (pad = 0; pad < 4; pad++)
		{
			struct gamepad_state const *state = input_has_gamepad(pad) ? input_get_gamepad_state(pad) : NULL;

			if (state && state->buttons[_gamepad_binary_button_back])
				back = TRUE;
		}
		QueryPerformanceCounter(&now);
		/* (a moment up doesn't count: the press that ends a run reloads the main menu, and the button reads up through it) */
		if (!back)
		{
			if (sweep.back_since && now.QuadPart - sweep.back_seen > perf.frequency)
				sweep.back_since = 0;
		}
		else if (!sweep.back_since)
			sweep.back_since = sweep.back_seen = now.QuadPart | 1;
		else if ((sweep.back_seen = now.QuadPart) - sweep.back_since >= perf.frequency * 3)
		{
			sweep.halted = TRUE;
			sweep.at_menu = FALSE;
			sweep_watch_off();
			sweep_set_scale(0);
			platform_log("perf sweep: Back held 3 seconds: the sweep stops until the next boot");
		}
	}
	if (!sweep.initialized || sweep.halted)
		return;
	error_code = ui_widget_oxbox_take_main_menu_error();
	if (error_code != NONE)
		platform_log("perf sweep: Halo's error %d for the main menu dropped, not shown (%s run %ld): an unattended sweep "
			"doesn't wait at its dialog", (int)error_code, sweep.key, sweep.run);
	if (sweep.at_menu && event_manager_time_of_last_event() != sweep.menu_input)
	{
		sweep.at_menu = FALSE;
		sweep_watch_off();
		platform_log("perf sweep: a press at the main menu: the watchdog stands down till the next run");
	}

}

/* the run ended (its row written with status); a failed one skips the map's later runs */
static void sweep_finish(
	char const *status)
{
	boolean passed = !strcmp(status, "ok");
	long run;

	sweep_row(status);
	sweep_spot_end();
	if (sweep.frames)
		platform_log("perf sweep: %s run %ld (%d bots): %s, %.1f fps over %.1f s", sweep.key, sweep.run, (int)sweep.bots,
			status, ticks_to_ms(sweep.time) > 0.0 ? sweep.frames * 1000.0 / ticks_to_ms(sweep.time) : 0.0,
			ticks_to_ms(sweep.time) / 1000.0);
	else
		platform_log("perf sweep: %s run %ld (%d bots): %s", sweep.key, sweep.run, (int)sweep.bots, status);
	if (!passed)
	{
		long failed_run = sweep.run;

		for (run = failed_run + 1; run < SWEEP_RUNS; run++)
		{
			if (!(sweep.needed & (1 << run)) || (sweep.have & (1 << run)))
				continue;
			sweep.run = run;
			sweep.bots = sweep_run_bots(run);
			sweep_clear_measures();
			sweep_row("skipped");
		}
		sweep.run = failed_run;
	}
	sweep_marker(0);
	sweep.runs_this_boot++;
	sweep.active = sweep.measuring = FALSE;
	sweep.over = TRUE;
	sweep_set_scale(0);
	sweep_watch_on(SWEEP_RETURN_SECONDS);
}

/* a folder map's version: map sharing's (its size and checksum, the standalone map's for a converted one), else (a
map that can't say, such as one whose shared files aren't here) its file's size and its header's checksum */
static unsigned long sweep_identity(
	char const *key)
{
	unsigned long identity = hmp_share_map_version(key), got = 0;
	unsigned char header[0x800];
	char path[200];
	long index = hmp_maps_find(key);
	HANDLE file;

	if (identity || index == NONE || !hmp_maps_map_path(index, NONE, path, sizeof(path)))
		return identity;
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	if (ReadFile(file, header, sizeof(header), &got, NULL) && got == sizeof(header))
		identity = (GetFileSize(file, NULL) * 2654435761ul ^ *(unsigned long const *)(header + 0x64)) | 1;
	CloseHandle(file);
	return identity;
}

/* the map at the cursor: its runs, and which have rows */
/* v1.168: a list line's spot, "<key> bsp <index> at <x> <y> <z> yaw <radians>": the line cut where it starts (the
key left); FALSE when it isn't one */
static boolean sweep_spot_parse(
	char *line,
	short *bsp,
	real_point3d *position,
	real *yaw)
{
	char *at = line, *found = NULL, *p, *end;
	double values[5];
	long k;

	/* (the last " bsp ": a folder's name may have one) */
	while ((at = strstr(at, " bsp ")) != NULL)
		found = at++;
	if (!found)
		return FALSE;
	p = found + 5;
	for (k = 0; k < 5; k++)
	{
		values[k] = strtod(p, &end);
		if (end == p)
			return FALSE;
		p = end;
		while (*p == ' ')
			p++;
		if (k == 0 && strncmp(p, "at ", 3) == 0)
			p += 3;
		else if (k == 3 && strncmp(p, "yaw ", 4) == 0)
			p += 4;
		else if (k == 0 || k == 3)
			return FALSE;
	}
	while (found > line && found[-1] == ' ')
		found--;
	*found = 0;
	if (bsp)
	{
		*bsp = (short)values[0];
		position->x = (real)values[1];
		position->y = (real)values[2];
		position->z = (real)values[3];
		*yaw = (real)values[4];
	}
	return TRUE;
}

/* a pass's maps: its list's keys, a line each (perf_sweep_report.py --write-list makes it), or a key and a spot (v1.168);
every map without a list. -1: the key isn't listed; else how many spot lines it has (0 none), and the want'th one's
spot (from 1) in bsp, position and yaw. Read for each map (no memory kept: the sweep's console was short of it) */
static long sweep_list_find(
	char const *key,
	long want,
	short *bsp,
	real_point3d *position,
	real *yaw)
{
	char path[64], buffer[1024], line[SWEEP_KEY_LENGTH + 96];
	long length = 0, spots = 0;
	unsigned long got, k;
	boolean listed = FALSE, more = TRUE;
	HANDLE file;

	sweep_path(path, sizeof(path), sweep.list);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	while (more)
	{
		more = ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got;
		if (!more)
		{
			/* (the last line, when the file doesn't end with one) */
			buffer[0] = '\n';
			got = 1;
		}
		for (k = 0; k < got; k++)
		{
			if (buffer[k] == '\n' || buffer[k] == '\r')
			{
				short line_bsp = 0;
				real_point3d line_position = { 0.0f, 0.0f, 0.0f };
				real line_yaw = 0.0f;
				boolean spot;

				while (length && line[length - 1] == ' ')
					length--;
				line[length] = 0;
				spot = length && sweep_spot_parse(line, &line_bsp, &line_position, &line_yaw);
				if (length && !csstrcasecmp(line, key))
				{
					listed = TRUE;
					if (spot && ++spots == want && bsp)
					{
						*bsp = line_bsp;
						*position = line_position;
						*yaw = line_yaw;
					}
				}
				length = 0;
			}
			else if (length < (long)sizeof(line) - 1 && (length || buffer[k] != ' '))
			{
				line[length++] = buffer[k];
			}
		}
	}
	CloseHandle(file);
	return listed ? spots : -1;
}

/* the rows' key: the map's, or "<map key> spot <n>" with that spot's place from the list */
static void sweep_spot_key(
	void)
{
	strcpy(sweep.key, sweep.map_key);
	if (!sweep.spot)
		return;
	if ((long)strlen(sweep.map_key) + 12 >= SWEEP_KEY_LENGTH ||
		sweep_list_find(sweep.map_key, sweep.spot, &sweep.spot_bsp, &sweep.spot_position, &sweep.spot_yaw) < sweep.spot)
	{
		platform_log("perf sweep: %s's spots left out (its key is too long, or the list changed): its normal run instead",
			sweep.map_key);
		sweep.spot = sweep.spot_count = 0;
		return;
	}
	_snprintf(sweep.key, SWEEP_KEY_LENGTH, "%s spot %ld", sweep.map_key, sweep.spot);
	sweep.key[SWEEP_KEY_LENGTH - 1] = 0;
}

/* v1.199: a map's runs that count as done now: in a pass of its own any row; in the master table its rows at this sweep
version, or (while looking for the maps never measured) at any when it has none at this one */
static unsigned char sweep_have(
	struct sweep_done const *done)
{
	if (!done)
		return 0;
	if (sweep.scale || sweep.pass)
		return done->runs & SWEEP_RUNS_MASK;
	/* (one being measured again at this version finishes it: its old rows don't count then) */
	if (done->version == SWEEP_VERSION)
		return done->latest & SWEEP_RUNS_MASK;
	return sweep.phase == SWEEP_PHASE_NEW ? done->runs & SWEEP_RUNS_MASK : 0;
}

/* v1.199: a key's map index (sweep_map_key's): a folder map's (+Pack/Map, the map browser's; +Library/X is +X), a
stock level's; NONE when it isn't here */
static long sweep_map_index(
	char const *key)
{
	long folder_maps = hmp_maps_count(), index;

	if (key[0] == '+')
		return hmp_maps_find(key);
	for (index = 0; index < SWEEP_STOCK_MULTIPLAYER; index++)
	{
		char const *name = hmp_maps_stock_level(index);

		if (name && !csstrcasecmp(name, key))
			return folder_maps + index;
	}
	for (index = 0; index < SWEEP_STOCK_SOLO; index++)
		if (!csstrcasecmp(sweep_solo_levels[index], key))
			return folder_maps + SWEEP_STOCK_MULTIPLAYER + index;
	return NONE;
}

/* v1.199: perf\sweep_priority.txt's want'th map (from 0: a key a line, as sweep.csv's key column; blank lines and
"#" ones skipped): FALSE past its last, or without the file. Read for each (no memory kept) */
static boolean sweep_priority_line(
	long want,
	char *key,
	long size)
{
	char path[64], buffer[1024];
	long length = 0, seen = 0;
	unsigned long got, k;
	boolean more = TRUE, found = FALSE;
	HANDLE file;

	sweep_path(path, sizeof(path), SWEEP_PRIORITY);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	while (more && !found)
	{
		more = ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got;
		if (!more)
		{
			buffer[0] = '\n';
			got = 1;
		}
		for (k = 0; k < got && !found; k++)
		{
			if (buffer[k] == '\n' || buffer[k] == '\r')
			{
				while (length && key[length - 1] == ' ')
					length--;
				key[length] = 0;
				if (length && key[0] != '#' && seen++ == want)
					found = TRUE;
				length = 0;
			}
			else if (length < size - 1 && (length || buffer[k] != ' '))
			{
				key[length++] = buffer[k];
			}
		}
	}
	CloseHandle(file);
	return found;
}

/* v1.199: the maps measured at an older sweep version and not perfect (their slowest normal run under
SWEEP_SCALE_BELOW_FPS), slowest first; those whose last measurement crashed, hung or didn't load at the end */
static void sweep_order_build(
	void)
{
	long index;

	sweep.order_count = 0;
	for (index = 0; index < sweep.map_count; index++)
	{
		char key[SWEEP_KEY_LENGTH];
		boolean solo;
		struct sweep_done const *done;
		unsigned char fps4;
		long at;

		if (!sweep_map_key(index, key, &solo))
			continue;
		done = sweep_find(sweep_hash(key), key[0] == '+' ? sweep_identity(key) : 1, FALSE);
		if (!done || !done->latest || done->version >= SWEEP_VERSION)
			continue;
		if (done->latest & SWEEP_RUNS_BROKEN)
			fps4 = 0xff;
		else if (done->slow_run != 0xff && done->slow_fps4 < SWEEP_SCALE_BELOW_FPS * 4)
			fps4 = done->slow_fps4;
		else
			continue;
		for (at = sweep.order_count; at > 0 && sweep_order_fps4[at - 1] > fps4; at--)
		{
			sweep_order[at] = sweep_order[at - 1];
			sweep_order_fps4[at] = sweep_order_fps4[at - 1];
		}
		sweep_order[at] = (short)index;
		sweep_order_fps4[at] = fps4;
		sweep.order_count++;
	}
	platform_log("perf sweep: %ld maps measured at an older sweep version weren't perfect: measured again, the slowest "
		"first", sweep.order_count);
}

/* v1.199: the phase's map at sweep.step: its index; NONE to pass over (a priority line's map that isn't here); -2 at
the phase's end */
static long sweep_step_map(
	void)
{
	switch (sweep.phase)
	{
	case SWEEP_PHASE_PRIORITY:
	{
		char key[SWEEP_KEY_LENGTH + 8];
		long index;

		if (!sweep_priority_line(sweep.step, key, sizeof(key)))
			return -2;
		index = sweep_map_index(key);
		if (index == NONE)
			platform_log("perf sweep: %s (perf\\%s) isn't on this Xbox", key, SWEEP_PRIORITY);
		return index;
	}
	case SWEEP_PHASE_NEW:
	case SWEEP_PHASE_REST:
		return sweep.step < sweep.map_count ? sweep.step : -2;
	case SWEEP_PHASE_SLOW:
		return sweep.step < sweep.order_count ? sweep_order[sweep.step] : -2;
	}
	return -2;
}

static void sweep_phase_next(
	void)
{
	sweep.step = 0;
	sweep.map_ready = FALSE;
	if (sweep.scale || sweep.pass)
	{
		sweep.phase = SWEEP_PHASE_DONE;
		return;
	}
	sweep.phase++;
	if (sweep.phase == SWEEP_PHASE_SLOW)
		sweep_order_build();
	if (sweep.phase == SWEEP_PHASE_REST && !sweep.redo_all)
		sweep.phase = SWEEP_PHASE_DONE;
}

static boolean sweep_map_ready(
	void)
{
	struct sweep_done *done;
	short players;

	if (!sweep_map_key(sweep.cursor, sweep.map_key, &sweep.solo))
		return FALSE;
	sweep.spot = sweep.spot_count = 0;
	if (sweep.scale || sweep.pass)
	{
		long spots = sweep_list_find(sweep.map_key, 0, NULL, NULL, NULL);

		if (spots < 0)
			return FALSE;
		/* (spots are a campaign level's: a multiplayer map's views are the showcase's) */
		if (sweep.solo && spots > 0)
		{
			sweep.spot_count = spots;
			sweep.spot = 1;
		}
	}
	sweep_spot_key();
	sweep.identity = sweep.map_key[0] == '+' ? sweep_identity(sweep.map_key) : 1;
	if (sweep.solo)
	{
		sweep.needed = sweep.needed_base = 1 << 3;
		sweep.full_bots = 0;
	}
	else
	{
		players = hmp_browse_suggested_players(sweep.map_key);
		if (players <= 0)
			players = 8;
		sweep.full_bots = players > 15 ? 15 : players;
		sweep.needed = sweep.needed_base = (unsigned char)(sweep.full_bots > 4 ? 7 : 3);
	}
	done = sweep_find(sweep_hash(sweep.key), sweep.identity, FALSE);
	sweep.have = sweep_have(done);
	return TRUE;
}

/* v1.199: once, the pass tables' rows (perf\\sweep_p<1-99>.csv: before v1.199 most sweeps went in a pass of their own)
brought into the master table, perf\\sweep.csv, so the maps measured there aren't measured again (at an older sweep
version: 0); a spot's rows and the res_scale passes stay out (other questions). A table whose columns aren't these is
left out. Its last line, "# imported ...", says it's done */
static void sweep_import(
	void)
{
	char out[4096], path[64], name[24], buffer[2048], line[1024], note[200], field[SWEEP_KEY_LENGTH + 16];
	long legacy = (long)strlen(sweep_fields) - (long)strlen(",sweep_version"), pass, total = 0;
	long note_length;
	HANDLE file;

	strcpy(note, "# imported (v1.199) from");
	note_length = (long)strlen(note);
	for (pass = 1; pass <= 99; pass++)
	{
		long length = 0, out_length = 0, rows = 0, at;
		unsigned long got, k;
		boolean header = FALSE, more = TRUE, too_long = FALSE;

		_snprintf(name, sizeof(name), "sweep_p%ld.csv", pass);
		name[sizeof(name) - 1] = 0;
		sweep_path(path, sizeof(path), name);
		file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (file == INVALID_HANDLE_VALUE)
			continue;
		while (more)
		{
			more = ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got;
			if (!more)
			{
				buffer[0] = '\n';
				got = 1;
			}
			for (k = 0; k < got; k++)
			{
				if (buffer[k] != '\n' && buffer[k] != '\r')
				{
					if (length < (long)sizeof(line) - 1)
						line[length++] = buffer[k];
					else
						too_long = TRUE;
					continue;
				}
				line[length] = 0;
				if (!length || too_long)
				{
					length = 0;
					too_long = FALSE;
					continue;
				}
				if (!strncmp(line, "status,", 7))
				{
					header = !strncmp(line, sweep_fields, legacy) &&
						(!line[legacy] || !strcmp(line + legacy, ",sweep_version"));
				}
				else if (line[0] != '#' && header)
				{
					/* (its key, the second column: a spot's is "<map> spot <n>") */
					at = 0;
					sweep_field(line, &at, field, sizeof(field));
					sweep_field(line, &at, field, sizeof(field));
					if (!strstr(field, " spot "))
					{
						if (out_length + length + 2 > (long)sizeof(out))
						{
							sweep_append(out, out_length);
							out_length = 0;
						}
						memcpy(out + out_length, line, length);
						out_length += length;
						out[out_length++] = '\r';
						out[out_length++] = '\n';
						rows++;
					}
				}
				length = 0;
			}
		}
		CloseHandle(file);
		if (out_length)
			sweep_append(out, out_length);
		total += rows;
		platform_log("perf sweep: %s: %ld rows brought into sweep.csv%s", name, rows,
			header ? "" : " (its columns aren't this build's: left out)");
		if (note_length + 32 < (long)sizeof(note))
		{
			_snprintf(note + note_length, sizeof(note) - note_length, " %s (%ld)", name, rows);
			note[sizeof(note) - 1] = 0;
			note_length = (long)strlen(note);
		}
	}
	if (!total)
		strcat(note, " none");
	strcat(note, "\r\n");
	sweep_append(note, (long)strlen(note));
	sweep.imported = TRUE;
}

/* once, at the first run: the rows so far, and a run the last boot never ended */
static void sweep_initialize(
	void)
{
	char path[64], text[128];
	unsigned long got = 0;
	HANDLE file;

	if (sweep.initialized)
		return;
	sweep.initialized = TRUE;
	if (!hmp_maps_count())
		hmp_maps_scan();
	/* (perf\ on D:, else T:) */
	sweep.drive = 'D';
	if (!CreateDirectoryA("D:\\perf", NULL) && GetFileAttributesA("D:\\perf") == (DWORD)-1)
	{
		sweep.drive = 'T';
		CreateDirectoryA("T:\\perf", NULL);
	}
	/* (a scaled pass, opence.ini res_scale: its own rows, and only perf\sweep_list.txt's maps when there's one) */
	{
		extern int oxbox_setting_res_scale;

		sweep.scale = oxbox_setting_res_scale >= 1 && oxbox_setting_res_scale < 100 ? oxbox_setting_res_scale : 0;
		sweep.base_scale = oxbox_setting_res_scale;
		if (sweep.scale)
		{
			_snprintf(sweep.results, sizeof(sweep.results), "sweep_r%ld.csv", sweep.scale);
			_snprintf(sweep.marker, sizeof(sweep.marker), "sweep_now_r%ld.txt", sweep.scale);
		}
		else if (sweep.pass)
		{
			_snprintf(sweep.results, sizeof(sweep.results), "sweep_p%ld.csv", sweep.pass);
			_snprintf(sweep.marker, sizeof(sweep.marker), "sweep_now_p%ld.txt", sweep.pass);
			/* (v1.201: a level-of-detail round, its value set for the whole round) */
			if (sweep.lod_rounds)
			{
				extern int oxbox_setting_lod_widescreen;

				if (!sweep.base_pass)
				{
					sweep.base_pass = sweep.pass;
					sweep.base_lod = oxbox_setting_lod_widescreen;
				}
				oxbox_setting_lod_widescreen = (int)sweep_lod_values[sweep.pass - sweep.base_pass];
			}
		}
		else
		{
			strcpy(sweep.results, "sweep.csv");
			strcpy(sweep.marker, "sweep_now.txt");
		}
	}
	/* (v1.168: a pass's own list, perf\sweep_list_p<pass>.txt, when there's one) */
	strcpy(sweep.list, "sweep_list.txt");
	if (sweep.pass)
	{
		char name[24];

		_snprintf(name, sizeof(name), "sweep_list_p%ld.txt", sweep.base_pass ? sweep.base_pass : sweep.pass);
		name[sizeof(name) - 1] = 0;
		sweep_path(path, sizeof(path), name);
		if (GetFileAttributesA(path) != (DWORD)-1)
			strcpy(sweep.list, name);
	}
	/* (the column names: the P rows' own) */
	{
		char const *names = strstr(frame_fields, "tick_units_ms");
		long column, n;

		for (column = 0; names && column < (long)NUMBEROF(window_time_columns); column++)
		{
			for (n = 0; names[n] && names[n] != ',' && n < SWEEP_COLUMN_NAME - 1; n++)
				sweep.column_names[column][n] = names[n];
			sweep.column_names[column][n] = 0;
			names = strchr(names, ',');
			if (names)
				names++;
		}
	}
	sweep_read_results();
	/* (v1.199: the master table brings in the pass tables once) */
	if (!sweep.scale && !sweep.pass && !sweep.imported)
	{
		sweep_import();
		sweep.done_count = 0;
		sweep_read_results();
	}
	sweep.phase = sweep.scale || sweep.pass ? SWEEP_PHASE_NEW : SWEEP_PHASE_PRIORITY;
	sweep.step = 0;
	sweep.again_hash = 0;
	sweep.again_run = NONE;
	sweep.map_count = hmp_maps_count() + SWEEP_STOCK_MULTIPLAYER + SWEEP_STOCK_SOLO;
	platform_log("perf sweep: %ld maps' versions have rows in %c:\\perf\\%s; %ld maps here; sweep version %d%s%s%s",
		sweep.done_count, sweep.drive, sweep.results, hmp_maps_count() + SWEEP_STOCK_MULTIPLAYER, SWEEP_VERSION,
		sweep.scale || sweep.pass ? " (a pass of its own: only perf\\" : "", sweep.scale || sweep.pass ? sweep.list : "",
		sweep.scale || sweep.pass ? "'s maps, when it's there)" : "");
	{
		extern int oxbox_widescreen(void);
		extern int oxbox_setting_lod_widescreen;

		platform_log("perf sweep: the view: %s; widescreen models' detail chosen at %d%% of their size%s",
			oxbox_widescreen() ? "widescreen" : "4:3", oxbox_setting_lod_widescreen,
			sweep.base_pass ? " (perf_sweep_lod: this round's)" : "");
	}
	/* (a run the last boot started and never wrote: the Xbox crashed or hung in it) */
	sweep_path(path, sizeof(path), sweep.marker);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		char *tab, *tab2;

		if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
			got = 0;
		CloseHandle(file);
		text[got] = 0;
		tab = strchr(text, '\t');
		tab2 = tab ? strchr(tab + 1, '\t') : NULL;
		if (tab && tab2 && tab - text < SWEEP_KEY_LENGTH)
		{
			char *end = NULL;
			boolean hung;

			*tab = 0;
			strcpy(sweep.key, text);
			strcpy(sweep.map_key, text);
			sweep.spot = sweep.spot_count = 0;
			/* (a spot's run: "<map key> spot <n>") */
			{
				char *spot = NULL, *at = sweep.map_key;

				while ((at = strstr(at, " spot ")) != NULL)
					spot = at++;
				if (spot && spot[6] >= '1' && spot[6] <= '9' && strspn(spot + 6, "0123456789") == strlen(spot + 6))
					*spot = 0;
			}
			sweep.run = strtol(tab + 1, NULL, 10);
			sweep.identity = strtoul(tab2 + 1, &end, 16);
			hung = end && !strncmp(end, "\thung", 5);
			/* (v1.199: a crash's restart marks it "crashed" (sweep_fatal), the watchdog's "hung"; a marker with neither
			is a run the console was switched off or reset in: not counted, and measured again; cut short a second
			time, "again", it's written "crashed" (a freeze nothing caught)) */
			if (sweep.run >= 0 && sweep.run < SWEEP_RUNS && end && !hung && strncmp(end, "\tcrashed", 8) &&
				strncmp(end, "\tagain", 6))
			{
				sweep.again_hash = sweep_hash(sweep.key);
				sweep.again_run = sweep.run;
				platform_log("perf sweep: %s run %ld was cut short (the console switched off or reset): measured again",
					sweep.key, sweep.run);
			}
			else if (sweep.run >= 0 && sweep.run < SWEEP_RUNS)
			{
				short players = sweep.map_key[0] == '+' || hmp_browse_stock_title(sweep.map_key) ? hmp_browse_suggested_players(sweep.map_key) : 0;

				struct sweep_done const *done = sweep_find(sweep_hash(sweep.key), sweep.identity, FALSE);

				sweep.slow_run = done && done->slow_run != 0xff ? done->slow_run : 0;
				sweep.solo = sweep.run == 3 || (sweep.run >= 4 && sweep.slow_run == 3);
				sweep.full_bots = players <= 0 ? 8 : players > 15 ? 15 : players;
				sweep.needed = sweep.needed_base = (unsigned char)(sweep.solo ? 1 << 3 : sweep.full_bots > 4 ? 7 : 3);
				sweep.have = 0;
				sweep.bots = sweep_run_bots(sweep.run);
				sweep_map_info();
				sweep_clear_measures();
				platform_log(hung ? "perf sweep: %s run %ld hung: the watchdog restarted H1U (perf\\sweep_hangs.txt has its log)" :
					"perf sweep: %s run %ld never ended (the Xbox crashed or hung there)", sweep.key, sweep.run);
				sweep_finish(hung ? "hung" : "crashed");
				sweep_watch_off();
			}
		}
		sweep_marker(0);
	}
	sweep.over = FALSE;
}

/* the watchdog's restart: this XBE again, from its own folder, as a dashboard starts a title (oxbox_sdk.c's
oxbox_launch_updater: the launch data page, then a quick reboot). Returns only when it couldn't */
static void sweep_restart(
	void)
{
	typedef struct { unsigned short Length, MaximumLength; char *Buffer; } ansi_string;
	ansi_string *image = (ansi_string *)oxbox_kernel_export(326);                        /* XeImageFileName */
	unsigned char **launch_page = (unsigned char **)oxbox_kernel_export(164);            /* LaunchDataPage */
	void *(WINAPI *allocate)(unsigned long) = (void *(WINAPI *)(unsigned long))oxbox_kernel_export(165);
	void (WINAPI *persist)(void *, unsigned long, int) = (void (WINAPI *)(void *, unsigned long, int))oxbox_kernel_export(178);
	void (WINAPI *reboot)(int) = (void (WINAPI *)(int))oxbox_kernel_export(49);         /* HalReturnToFirmware */
	unsigned char const *certificate = *(unsigned char const *const *)((unsigned char const *)0x10000 + 0x118);
	char folder[300], *slash, *semicolon;
	unsigned char *page;
	int length;

	if (!image || !image->Buffer || !launch_page || !allocate || !persist || !reboot)
		return;
	length = image->Length < sizeof(folder) - 1 ? image->Length : sizeof(folder) - 1;
	memcpy(folder, image->Buffer, length);
	folder[length] = 0;
	if ((semicolon = strchr(folder, ';')) != NULL)
		*semicolon = '\\';
	if (strncmp(folder, "\\Device\\", 8) || (slash = strrchr(folder, '\\')) == NULL)
		return;
	*slash = 0;
	if (!*launch_page && !(*launch_page = (unsigned char *)allocate(0x1000)))
		return;
	page = *launch_page;
	persist(page, 0x1000, 1);
	memset(page, 0, 0x1000);
	*(unsigned long *)(page + 4) = *(unsigned long const *)(certificate + 8);
	if (_snprintf((char *)page + 8, 520, "%s;%s", folder, slash + 1) < 0)
		return;
	platform_log("perf sweep: restarting %s", (char *)page + 8);
	oxbox_log_flush();
	{
		void oxbox_quiesce_devices(void);   /* (oxbox_entry.c: the network card and USB stopped writing, v1.171) */

		oxbox_quiesce_devices();
	}
	reboot(2);
}

/* the log's last 16 KB (its ring: the next boot's log replaces oxbox_log.txt) on the end of perf\sweep_hangs.txt, headed
with the run and what (" (after it ended)", ", crashed"...) */
static void sweep_save_log(
	char const *what)
{
	extern char oxbox_log_ring[0x4000];
	extern unsigned long oxbox_log_ring_position;

	char path[64], head[160];
	unsigned long put, end = oxbox_log_ring_position, size = end < sizeof(oxbox_log_ring) ? end : sizeof(oxbox_log_ring);
	unsigned long start = (end - size) & (sizeof(oxbox_log_ring) - 1);
	SYSTEMTIME time;
	HANDLE file;

	sweep_path(path, sizeof(path), "sweep_hangs.txt");
	file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		GetSystemTime(&time);
		_snprintf(head, sizeof(head), "\r\n==== %04d-%02d-%02d %02d:%02d:%02d v1.%d: %s run %ld%s; the log's last %lu bytes:\r\n",
			time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, OXBOX_BUILD_NUMBER, sweep.key,
			sweep.run, what, size);
		head[sizeof(head) - 1] = 0;
		SetFilePointer(file, 0, NULL, FILE_END);
		WriteFile(file, head, (unsigned long)strlen(head), &put, NULL);
		if (start + size > sizeof(oxbox_log_ring))
		{
			WriteFile(file, oxbox_log_ring + start, sizeof(oxbox_log_ring) - start, &put, NULL);
			WriteFile(file, oxbox_log_ring, start + size - sizeof(oxbox_log_ring), &put, NULL);
		}
		else
			WriteFile(file, oxbox_log_ring + start, size, &put, NULL);
		CloseHandle(file);
	}
}

/* the writer thread's look, every pass: a run that hasn't ended in time (the game hung, or sits on a black screen)
restarts H1U, its log's last 16 KB kept and the run marked hung */
static void sweep_watch(
	void)
{
	LARGE_INTEGER now;
	long seconds = sweep_watching;

	if (!seconds)
		return;
	QueryPerformanceCounter(&now);
	if (now.QuadPart < sweep_watch_deadline)
		return;
	sweep_watching = 0;
	if (sweep.active)
		platform_log("perf sweep: %s run %ld hasn't ended in %ld s: the watchdog restarts H1U", sweep.key, sweep.run, seconds);
	else
		platform_log("perf sweep: the main menu isn't back %ld s after %s run %ld: the watchdog restarts H1U", seconds,
			sweep.key, sweep.run);
	if (sweep.active)
		sweep_marker(2);
	oxbox_log_flush();
	sweep_save_log(sweep.active ? "" : " (after it ended)");
	sweep_restart();
	platform_log("perf sweep: the watchdog couldn't restart H1U");
}

/* the game crashed or halted (oxbox_entry.c's crash handler, main.c's halt, their report written): a sweep under way
restarts H1U, its log kept, rather than leave the console stopped (a crash stops it: the watchdog's thread doesn't
run). The run's marker stays: the next boot writes it "crashed" */
static void sweep_fatal(
	char const *why)
{
	if (!sweep.on || !sweep.initialized || sweep.halted)
		return;
	sweep_watching = 0;
	/* (v1.199: so the next boot writes it "crashed": a run cut short without this was the console switched off) */
	if (sweep.active)
		sweep_marker(3);
	platform_log("perf sweep: the game stopped (%s) in %s run %ld: restarting H1U", why, sweep.key, sweep.run);
	oxbox_log_flush();
	sweep_save_log(!strcmp(why, "crash") ? ", crashed" : ", halted");
	sweep_restart();
	platform_log("perf sweep: couldn't restart H1U after the %s", why);
	oxbox_log_flush();
}

static long sweep_next(
	char *key,
	long size,
	short *bots)
{
	sweep_initialize();
	sweep.active = sweep.measuring = sweep.over = FALSE;
	if (sweep.halted)
		return _oxbox_sweep_none;
	/* (v1.199: the master table's phases; a pass of its own has only SWEEP_PHASE_NEW's walk over every map) */
	for (; sweep.phase < SWEEP_PHASE_DONE; sweep.step++, sweep.map_ready = FALSE)
	{
		long run;

		if (!sweep.map_ready)
		{
			long index = sweep_step_map();

			if (index == -2)
			{
				sweep_phase_next();
				sweep.step--;   /* (the loop's step++ brings it to 0) */
				continue;
			}
			if (index == NONE)
				continue;
			sweep.cursor = index;
			if (!sweep_map_ready())
				continue;
			sweep.map_ready = TRUE;
		}
		for (;;)
		{
			struct sweep_done const *done = sweep_find(sweep_hash(sweep.key), sweep.identity, FALSE);

			sweep.needed = (unsigned char)(sweep.needed_base | sweep_extra(done));
			sweep.slow_run = done && done->slow_run != 0xff ? done->slow_run : 0;
			for (run = 0; run < SWEEP_RUNS && (!(sweep.needed & (1 << run)) || (sweep.have & (1 << run))); run++)
				;
			if (run < SWEEP_RUNS || !sweep.spot || sweep.spot >= sweep.spot_count)
				break;
			/* (that spot's done: the level's next, v1.168) */
			sweep.spot++;
			sweep_spot_key();
			done = sweep_find(sweep_hash(sweep.key), sweep.identity, FALSE);
			sweep.have = sweep_have(done);
		}
		if (run == SWEEP_RUNS)
			continue;
		if ((long)strlen(sweep.map_key) >= size)
			continue;
		sweep.run = run;
		sweep.bots = sweep_run_bots(run);
		sweep_map_info();
		sweep_clear_measures();
		/* (a folder map the map browser won't choose isn't loaded: its rows say why) */
		if (sweep.map_key[0] == '+' && !sweep.solo && sweep.cursor < hmp_maps_count() &&
			(sweep.problem = hmp_maps_problem(sweep.cursor)) != NULL)
		{
			platform_log("perf sweep: %s isn't loaded: %s", sweep.key, sweep.problem);
			sweep_finish("bad_map");
			sweep_watch_off();
			continue;
		}
		sweep_marker(1);
		sweep.at_menu = FALSE;
		{
			extern long bots_oxbox_short;

			bots_oxbox_short = 0;
		}
		sweep_set_scale(run);
		sweep_watch_on(sweep.seconds + SWEEP_WATCH_SECONDS);
		sweep.active = TRUE;
		sweep.spot_ready = sweep.spot_skip = 0.0f;
		sweep.spot_switches = 0;
		strcpy(key, sweep.map_key);
		*bots = sweep.bots;
		platform_log("perf sweep: map %ld of %ld, %s (%s, %s), run %ld: %d bots%s", sweep.cursor + 1, sweep.map_count,
			sweep.key, sweep.name, sweep.solo ? "campaign" : "multiplayer", run, (int)sweep.bots,
			run == 4 ? ", drawn at 85% (its slowest run was under 29 fps)" : run == 5 ? ", drawn at 70% (still under 29 at 85%)" : "");
		return sweep.solo ? _oxbox_sweep_solo : _oxbox_sweep_multiplayer;
	}
	/* (v1.201: the next level-of-detail round, its own table: sweep_initialize again on the next pass number) */
	if (sweep.base_pass && sweep.pass - sweep.base_pass + 1 < (long)NUMBEROF(sweep_lod_values))
	{
		platform_log("perf sweep: level-of-detail round %ld (%ld%%) is done (%c:\\perf\\%s); the next: %ld%%",
			sweep.pass - sweep.base_pass + 1, sweep_lod_values[sweep.pass - sweep.base_pass], sweep.drive, sweep.results,
			sweep_lod_values[sweep.pass - sweep.base_pass + 1]);
		sweep.pass++;
		sweep.initialized = FALSE;
		sweep.done_count = 0;
		return sweep_next(key, size, bots);
	}
	if (sweep.base_pass)
	{
		extern int oxbox_setting_lod_widescreen;

		oxbox_setting_lod_widescreen = (int)sweep.base_lod;
	}
	sweep_watch_off();
	sweep.halted = TRUE;
	platform_log("perf sweep: every map is done (port/oxbox/tools/perf_sweep_report.py reads %c:\\perf\\%s; sweep version %d)",
		sweep.drive, sweep.results, SWEEP_VERSION);
	/* (v1.174: an error box at the main menu says so, till A: whoever left the sweep running sees it's done) */
	display_error_text_deferred(sweep.runs_this_boot ?
		L"Sweeps complete.\r\nEvery map has been\r\nmeasured.\r\nSend logs." :
		L"Sweeps complete.\r\nEvery map was\r\nmeasured before:\r\nnothing left to do.", NONE);
	return _oxbox_sweep_none;
}

/* a map without a showcase.txt (showcase.c): shots at up to maximum of its spawn points (spread over its list), each
two keys (showcase.c's: seconds, x, y, z, yaw, pitch, field of view) a turn round at eye height in 8 seconds; how many */
static long sweep_spawn_shots(
	float (*keys)[7],
	long maximum)
{
	struct scenario *scenario = global_scenario_try_and_get();
	long count = scenario ? scenario->players.count : 0, index;
	long shots = count < maximum ? count : maximum;

	for (index = 0; index < shots; index++)
	{
		struct player_starting_location const *spawn = TAG_BLOCK_GET_ELEMENT(&scenario->players, index * count / shots,
			struct player_starting_location);
		long key;

		for (key = 0; key < 2; key++)
		{
			float *v = keys[index * 2 + key];

			v[0] = key ? 8.0f : 0.0f;
			v[1] = spawn->position.x;
			v[2] = spawn->position.y;
			v[3] = spawn->position.z + 0.62f;
			v[4] = (float)(spawn->facing * (180.0 / 3.14159265358979)) + (key ? 360.0f : 0.0f);
			v[5] = 0.0f;
			v[6] = 70.0f;
		}
	}
	platform_log("perf sweep: %s has no showcase.txt: shots at %ld of its %ld spawn points", sweep.key, shots, count);
	return shots;
}

/* v1.168: a spot run's frame: the opening cutscene skipped (as a press would), then the BSP switched to and the player
put on the spot and kept there (the camera tour's pin). TRUE once it's there: the warmup counts from then */
static boolean sweep_spot_frame(
	float seconds)
{
	struct scenario *scenario = global_scenario_try_and_get();
	LARGE_INTEGER now;
	long player_index;

	if (!scenario || !game_in_progress())
		return FALSE;
	QueryPerformanceCounter(&now);
	/* (a cutscene, whenever one plays: skipped as a press would, asked again every 5 seconds. One that starts before
	the run measures puts the player back on the spot after it, the warmup again) */
	if (cinematic_in_progress())
	{
		if (cinematic_can_be_skipped() && (!sweep.spot_skip || seconds - sweep.spot_skip >= 5.0f))
		{
			platform_log("perf sweep: %s: a cutscene skipped, %.1f s in", sweep.key, seconds);
			sweep.spot_skip = seconds > 0.01f ? seconds : 0.01f;
			main_skip_cinematic();
		}
		if (sweep.spot_ready && !sweep.measuring)
		{
			tour_pin_release();
			sweep.spot_ready = 0.0f;
		}
	}
	if (!sweep.spot_ready)
	{
		/* (the level's startup scripts first, which start its opening cutscene; one that can't be skipped is waited
		for, 2 minutes at most) */
		if (seconds < SWEEP_SPOT_SETTLE_SECONDS || (cinematic_in_progress() && seconds < 120.0f))
			return FALSE;
		tour.local_player_index = 0;
		if (tour_player_unit(&player_index) == NONE)
			return FALSE;
		if (sweep.spot_bsp < 0 || sweep.spot_bsp >= scenario->structure_bsp_references.count)
		{
			platform_log("perf sweep: %s: it has no BSP %d (%ld): the player put there in BSP %d", sweep.key,
				(int)sweep.spot_bsp, scenario->structure_bsp_references.count, (int)global_structure_bsp_index_get());
			sweep.spot_bsp = global_structure_bsp_index_get();
		}
		if (global_structure_bsp_index_get() != sweep.spot_bsp)
			scenario_switch_structure_bsp(sweep.spot_bsp);
		tour.pin = TRUE;
		tour.pin_unit = NONE;
		tour.pin_settled = FALSE;
		tour.pin_set_invulnerable = FALSE;
		tour.pins = 0;
		tour.pin_position = sweep.spot_position;
		tour.pin_yaw = sweep.spot_yaw;
		sweep.spot_ready = seconds > 0.01f ? seconds : 0.01f;
		platform_log("perf sweep: %s: BSP %d, the player at (%.2f %.2f %.2f) facing %.2f, %.1f s in", sweep.key,
			(int)global_structure_bsp_index_get(), sweep.spot_position.x, sweep.spot_position.y, sweep.spot_position.z,
			sweep.spot_yaw, seconds);
	}
	else if (global_structure_bsp_index_get() != sweep.spot_bsp && sweep.spot_switches < 3)
	{
		sweep.spot_switches++;
		platform_log("perf sweep: %s: the level switched to BSP %d: back to %d", sweep.key,
			(int)global_structure_bsp_index_get(), (int)sweep.spot_bsp);
		scenario_switch_structure_bsp(sweep.spot_bsp);
		tour.pin_unit = NONE;
		tour.pin_settled = FALSE;
	}
	tour_pin_update(now.QuadPart);
	return TRUE;
}

/* the spot run's end: the player's pin let go */
static void sweep_spot_end(
	void)
{
	if (sweep.spot_ready)
		tour_pin_release();
	sweep.spot_ready = 0.0f;
}

static boolean sweep_frame(
	float seconds)
{
	if (!sweep.active || sweep.over)
		return TRUE;
	if (sweep.spot && !sweep_spot_frame(seconds))
		return FALSE;
	if (!sweep.measuring && seconds >= sweep.spot_ready + SWEEP_WARMUP_SECONDS)
	{
		struct scenario *scenario = global_scenario_try_and_get();

		sweep_clear_measures();
		if (scenario)
		{
			sweep.bsps = scenario->structure_bsp_references.count;
			sweep.encounters = scenario->ai_encounters.count;
		}
		sweep.free_kb_min = free_kb(NULL);
		sweep.measuring = TRUE;
	}
	return FALSE;
}

static void sweep_ended(
	long how)
{
	if (how == _oxbox_sweep_ended_stopped)
	{
		sweep_watch_off();
		sweep.halted = TRUE;
	}
	if (how == _oxbox_sweep_ended_menu)
		sweep_at_menu();
	if (!sweep.active)
		return;
	if (how == _oxbox_sweep_ended_interrupted)
	{
		platform_log("perf sweep: a press interrupted %s run %ld (not counted): the sweep goes on after 2 minutes without a "
			"press (hold Back 3 seconds at the main menu to stop it until the next boot)", sweep.key, sweep.run);
		/* (the run's watchdog off: the player plays on, maybe for long; the next run sets it again) */
		sweep_watch_off();
		sweep_marker(0);
		sweep_spot_end();
		sweep.active = sweep.measuring = FALSE;
		sweep_set_scale(0);
		return;
	}
	if (how == _oxbox_sweep_ended_stopped)
	{
		platform_log("perf sweep: stopped by a press (%s run %ld isn't counted); it carries on at the next boot", sweep.key,
			sweep.run);
		sweep_marker(0);
		sweep_spot_end();
		sweep.active = sweep.measuring = FALSE;
		sweep_set_scale(0);
		return;
	}
	sweep_finish(how == _oxbox_sweep_ended_no_start ? "no_start" : "left");
	if (how == _oxbox_sweep_ended_menu)
		sweep_at_menu();
}

/* a frame of the run (perf_frame, while it measures) */
static void sweep_sample(
	struct perf_sample const *sample,
	__int64 now)
{
	double ms = ticks_to_ms(sample->frame);
	unsigned long bucket = ms < 0.0 ? 0 : ms >= SWEEP_HISTOGRAM - 1 ? SWEEP_HISTOGRAM - 1 : (unsigned long)ms;

	sweep.frames++;
	sweep.time += sample->frame;
	if (sample->frame > sweep.frame_worst)
		sweep.frame_worst = sample->frame;
	sweep.histogram[bucket]++;
	if (ms > 34.0)
		sweep.under[0] += sample->frame;
	if (ms > 41.0)
		sweep.under[1] += sample->frame;
	if (ms > 51.0)
		sweep.under[2] += sample->frame;
	if (ms > 68.0)
		sweep.under[3] += sample->frame;
	sweep.ticks += sample->ticks;
	sweep.drawing += sample->drawing;
	sweep.present += sample->present;
	sweep.vsync += sample->vsync;
	sweep.gpu_busy += sample->gpu_busy;
	sweep.gpu_samples += sample->gpu_samples;
	sweep.texture_bytes += sample->texture_bytes;
	sweep.texture_waits += sample->texture_waits;
	if (object_header_data && (unsigned long)object_header_data->actual_count > sweep.objects_peak)
		sweep.objects_peak = object_header_data->actual_count;
	if (actor_data && (unsigned long)actor_data->actual_count > sweep.actors_peak)
		sweep.actors_peak = actor_data->actual_count;
	if (particle_data && (unsigned long)particle_data->actual_count > sweep.particles_peak)
		sweep.particles_peak = particle_data->actual_count;
	if (effect_data && (unsigned long)effect_data->actual_count > sweep.effects_peak)
		sweep.effects_peak = effect_data->actual_count;

	/* the frame rate a second: under 10 for 5 seconds in a row fails the run */
	if (!sweep.second_start)
		sweep.second_start = now - sample->frame;
	sweep.second_frames++;
	if (now - sweep.second_start >= perf.frequency)
	{
		double seconds = ticks_to_ms(now - sweep.second_start) / 1000.0;
		double fps = sweep.second_frames / seconds;
		unsigned long free = free_kb(NULL);

		if (free < sweep.free_kb_min)
			sweep.free_kb_min = free;
		if (fps < SWEEP_FAIL_FPS)
		{
			sweep.low_seconds += seconds;
			sweep.under10_seconds += seconds;
			if (sweep.low_seconds > sweep.longest_low)
				sweep.longest_low = sweep.low_seconds;
		}
		else
		{
			sweep.low_seconds = 0.0;
		}
		sweep.second_start = now;
		sweep.second_frames = 0;
		if (sweep.low_seconds >= SWEEP_FAIL_SECONDS)
		{
			sweep_finish("fail_fps");
			return;
		}
	}
	if (ticks_to_ms(sweep.time) >= sweep.seconds * 1000.0)
		sweep_finish("ok");
}

/* a second's steps (window_row, while a run measures) */
static void sweep_window(
	void)
{
	long column;

	for (column = 0; column < (long)NUMBEROF(window_time_columns); column++)
	{
		long source = window_time_columns[column];
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
		sweep.columns[column] += cycles;
	}
	sweep.level_triangles += window.level_triangles;
	sweep.window_frames += (unsigned long)perf.second.frames;
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

#if defined(OXBOX_DENSE_PERF)
/* (lab builds only: each second, every thread's processor time in the game's process, from the kernel's own count
(KTHREAD.KernelTime: a clock tick, 1 ms, for the thread running at each tick; on the Xbox all threads run in kernel
mode). Threads are named by their base priority (not the current one, which a wait's end boosts): the game's (the one
calling this), lwIP's (highest: 10), internet play's (above normal: 9), the boot watch's (time critical); each also
with its start address and base priority. Offsets from nxdk's xboxkrnl.h: KernelTime 0x18, BasePriority 0x70,
ApcState.Process 0x44, ThreadListEntry 0x104; KPROCESS.ThreadListHead 0x8; ETHREAD.StartAddress 0x130) */
static unsigned char *volatile sampler_game;                 /* (the profile's, below) */
static unsigned long __stdcall sampler_thread(void *parameter);

static void threads_line(
	double t)
{
	struct list { struct list *next, *previous; };
	struct seen { unsigned char *thread; unsigned long ticks; };
	static struct seen last[48];
	static long last_count;
	static unsigned char *game;
	struct seen now[48];
	unsigned char *(__stdcall *current)(void) = (unsigned char *(__stdcall *)(void))oxbox_kernel_export(104);
	unsigned char (__stdcall *raise)(void) = (unsigned char (__stdcall *)(void))oxbox_kernel_export(129);
	void (__attribute__((fastcall)) *lower)(unsigned char) =
		(void (__attribute__((fastcall)) *)(unsigned char))oxbox_kernel_export(161);
	char line[LINE_SIZE];
	unsigned long total = 0, game_ticks = 0;
	long count = 0, index, k;
	int used = 0;
	struct list *head, *entry;
	unsigned char old;

	if (!current || !raise || !lower)
		return;
	if (!game)
	{
		HANDLE sampler;

		game = current();
		sampler_game = game;
		sampler = CreateThread(NULL, 0x4000, sampler_thread, NULL, 0, NULL);
		if (sampler)
			SetThreadPriority(sampler, 15);                  /* (time critical) */
	}
	head = (struct list *)(*(unsigned char **)(game + 0x44) + 0x8);
	old = raise();
	for (entry = head->next; entry != head && count < 48; entry = entry->next)
	{
		now[count].thread = (unsigned char *)entry - 0x104;
		now[count].ticks = *(unsigned long *)(now[count].thread + 0x18);
		count++;
	}
	lower(old);
	used = _snprintf(line, sizeof(line), "threads %.0f s:", t);
	for (index = 0; index < count; index++)
	{
		unsigned char *thread = now[index].thread;
		unsigned long delta = now[index].ticks;
		signed char priority = *(signed char *)(thread + 0x70);
		char const *name;

		for (k = 0; k < last_count; k++)
		{
			if (last[k].thread == thread)
			{
				delta = now[index].ticks - last[k].ticks;
				break;
			}
		}
		if (k == last_count)
			continue;                                         /* (new this second: next second's line has it) */
		total += delta;
		name = thread == game ? "game" : priority == 10 ? "lwip" : priority == 9 ? "p2p" : priority >= 15 ? "watch" : NULL;
		if (thread == game)
			game_ticks = delta;
		if (!delta && thread != game)
			continue;
		if (used < (int)sizeof(line) - 64)
		{
			used += _snprintf(line + used, sizeof(line) - used, " %s%s%p/p%d %lu", name ? name : "", name ? ":" : "",
				*(void **)(thread + 0x130), (int)priority, delta);
		}
	}
	if (used < (int)sizeof(line) - 48)
		_snprintf(line + used, sizeof(line) - used, "; all threads %lu ms, the game %lu%% of them", total,
			total ? game_ticks * 100 / total : 0);
	platform_log("%s", line);
	memcpy(last, now, sizeof(now[0]) * count);
	last_count = count;
}
#endif

#if defined(OXBOX_DENSE_PERF)
/* (lab builds only: a sampling profile of the game's thread. A thread at time-critical priority wakes every
millisecond; the game's thread is then waiting (KTHREAD.State 5: for the graphics chip, a frame's end) or was
preempted by the clock (1, ready): its kernel stack (KTHREAD.KernelStack 0x24, up to StackBase 0x1c) then holds the
clock interrupt's trap frame, whose first code address is the instruction it was at. Those addresses are counted;
every SAMPLER_WINDOW samples the most taken go to the log ("profile N s: ..."), symbolized on the PC with oxbox.elf) */
enum { SAMPLER_SLOTS = 16384, SAMPLER_WINDOW = 2500, SAMPLER_TOP = 48, SAMPLER_BUCKET = 16 };
/* (v2: 16-byte buckets of code in a 16384-slot table (v1's 4096 exact addresses overflowed in a10's busy hangar shot);
v1.186: windows of 2,500 samples (~5 s on the console: a10's hangar shot is ~15 s, its worst stretch 5),
and every window's whole table also goes to perf\profile.bin: its header (SAMPLER_FIELDS words: 'OXPF', version 1,
window, session ms at its end, samples, waiting, ready, other, no frame found, table full, buckets taken once) then
each bucket taken twice or more as 6 bytes: its address (4) and count (2); tools/perf_profile.py reads it) */
enum { SAMPLER_FIELDS = 12 };
static struct { unsigned long address, count; } sampler_table[SAMPLER_SLOTS];
static unsigned long sampler_samples, sampler_waiting, sampler_ready, sampler_other, sampler_no_frame, sampler_full,
	sampler_windows;
static HANDLE sampler_file = INVALID_HANDLE_VALUE;
static boolean sampler_file_tried;
static unsigned char sampler_chunk[1536];

static void sampler_note(unsigned long address)
{
	unsigned long slot, k;

	address &= ~(unsigned long)(SAMPLER_BUCKET - 1);
	slot = (address * 2654435761u) >> 18;
	for (k = 0; k < SAMPLER_SLOTS; k++, slot++)
	{
		slot &= SAMPLER_SLOTS - 1;
		if (sampler_table[slot].address == address)
		{
			sampler_table[slot].count++;
			return;
		}
		if (!sampler_table[slot].address)
		{
			sampler_table[slot].address = address;
			sampler_table[slot].count = 1;
			return;
		}
	}
	sampler_full++;
}

static void sampler_write(
	void const *data,
	unsigned long size)
{
	unsigned long written;

	if (sampler_file != INVALID_HANDLE_VALUE && (!WriteFile(sampler_file, data, size, &written, NULL) || written != size))
	{
		CloseHandle(sampler_file);
		sampler_file = INVALID_HANDLE_VALUE;
	}
}

/* (the whole table to perf\profile.bin, from the sampler's own thread: no samples while it writes) */
static void sampler_save(
	unsigned long session_ms)
{
	unsigned long header[SAMPLER_FIELDS], singles = 0, entries = 0, used = 0;
	int k;

	if (!sampler_file_tried)
	{
		static char const *const paths[] = { "D:\\perf\\profile.bin", "T:\\perf\\profile.bin" };

		sampler_file_tried = TRUE;
		for (k = 0; k < 2 && sampler_file == INVALID_HANDLE_VALUE; k++)
		{
			char folder[8];

			memcpy(folder, paths[k], 7);
			folder[7] = 0;
			CreateDirectoryA(folder, NULL);
			sampler_file = CreateFileA(paths[k], GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
		}
	}
	if (sampler_file == INVALID_HANDLE_VALUE)
		return;
	for (k = 0; k < SAMPLER_SLOTS; k++)
	{
		if (sampler_table[k].count == 1)
			singles++;
		else if (sampler_table[k].count > 1)
			entries++;
	}
	header[0] = 0x4650584f;
	header[1] = 1;
	header[2] = sampler_windows;
	header[3] = session_ms;
	header[4] = sampler_samples;
	header[5] = sampler_waiting;
	header[6] = sampler_ready;
	header[7] = sampler_other;
	header[8] = sampler_no_frame;
	header[9] = sampler_full;
	header[10] = singles;
	header[11] = entries;
	sampler_write(header, sizeof(header));
	for (k = 0; k < SAMPLER_SLOTS; k++)
	{
		unsigned long count = sampler_table[k].count;

		if (count < 2)
			continue;
		if (count > 0xffff)
			count = 0xffff;
		memcpy(sampler_chunk + used, &sampler_table[k].address, 4);
		sampler_chunk[used + 4] = (unsigned char)count;
		sampler_chunk[used + 5] = (unsigned char)(count >> 8);
		used += 6;
		if (used == sizeof(sampler_chunk))
		{
			sampler_write(sampler_chunk, used);
			used = 0;
		}
	}
	if (used)
		sampler_write(sampler_chunk, used);
}

static void sampler_dump(void)
{
	char line[LINE_SIZE];
	int used, k, n;
	LARGE_INTEGER now;
	unsigned long session_ms;

	QueryPerformanceCounter(&now);
	session_ms = perf.frequency > 0 ? (unsigned long)((now.QuadPart - perf.session_start) * 1000 / perf.frequency) : 0;
	sampler_windows++;
	sampler_save(session_ms);
	used = _snprintf(line, sizeof(line), "profile %lu s: %lu samples, waiting %lu, ready %lu (%lu not in code), other %lu; top:",
		session_ms / 1000, sampler_samples, sampler_waiting, sampler_ready, sampler_no_frame + sampler_full,
		sampler_other);
	for (n = 0; n < SAMPLER_TOP; n++)
	{
		int best = -1;

		for (k = 0; k < SAMPLER_SLOTS; k++)
		{
			if (sampler_table[k].count && (best < 0 || sampler_table[k].count > sampler_table[best].count))
				best = k;
		}
		if (best < 0)
			break;
		if (used < (int)sizeof(line) - 24)
			used += _snprintf(line + used, sizeof(line) - used, " %lx:%lu", sampler_table[best].address,
				sampler_table[best].count);
		sampler_table[best].count = 0;
	}
	platform_log("%s", line);
	memset(sampler_table, 0, sizeof(sampler_table));
	sampler_samples = sampler_waiting = sampler_ready = sampler_other = sampler_no_frame = sampler_full = 0;
}

static unsigned long __stdcall sampler_thread(void *parameter)
{
	(void)parameter;
	for (;;)
	{
		unsigned char *game;
		unsigned long esp, base, k;

		Sleep(1);
		game = sampler_game;
		if (!game)
			continue;
		sampler_samples++;
		if (game[0x2c] == 5)
			sampler_waiting++;
		else if (game[0x2c] != 1)
			sampler_other++;
		else
		{
			sampler_ready++;
			esp = *(unsigned long *)(game + 0x24);
			base = *(unsigned long *)(game + 0x1c);
			/* (the clock interrupt's frame: on the Xbox every thread runs in ring 0, so the interrupt pushed the
			interrupted EIP, CS (0x0008) and EFLAGS (bit 1 always set; interrupts were on: 0x200) on the thread's own
			stack: found by that pattern, not by the first code-like value (saved registers and the interrupted code's
			return addresses are there too)) */
			for (k = 0; k + 2 < 512 && esp + (k + 2) * 4 < base; k++)
			{
				unsigned long const *v = (unsigned long const *)esp + k;

				if (v[1] == 0x0008 && (v[2] & 0x202) == 0x202 && v[2] < 0x400000 && v[0] >= 0x11000 && v[0] < 0x4e0000)
				{
					sampler_note(v[0]);
					break;
				}
			}
			if (k + 2 >= 512 || esp + (k + 2) * 4 >= base)
				sampler_no_frame++;
		}
		if (sampler_samples >= SAMPLER_WINDOW)
			sampler_dump();
	}
	return 0;
}
#endif

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
	if (sweep.measuring && kind == 'P')
		sweep_window();
	frame_row(kind, ticks_to_ms(now - perf.session_start) / 1000.0, &perf.second);
#if defined(OXBOX_DENSE_PERF)
	if (kind == 'P')
		threads_line(ticks_to_ms(now - perf.session_start) / 1000.0);
#endif
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

/* ---------- a map load's steps (v1.178): the "load:" line, how long each step of a map's load took, once its first
frame is drawn (the marks: game.c game_precache_new_map, game_load and game_initialize_for_new_map, cache_files.c
scenario_tags_load, scenario.c scenario_load) */

static struct
{
	long state;                              /* 0: no load under way; 1: loading; 2: set up, the first frame to come */
	__int64 start, at[NUMBER_OF_OXBOX_PERF_LOAD_STEPS];
	__int64 copy_from, copied_at, copying, finishing;
	long copies;
	char map[64];
} load;

static __int64 load_now(
	void)
{
	LARGE_INTEGER counter;

	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}

static void perf_load(
	long step,
	char const *name)
{
	__int64 now = load_now();

	if (step < 0 || step >= NUMBER_OF_OXBOX_PERF_LOAD_STEPS)
		return;
	/* (a load begins at its first copy, or at the tags when nothing's copied) */
	if (load.state != 1 && (step == _oxbox_perf_load_copy_begin || step == _oxbox_perf_load_begin))
	{
		memset(&load, 0, sizeof(load));
		load.state = 1;
		load.start = now;
	}
	if (load.state != 1)
		return;
	if (step == _oxbox_perf_load_copy_begin)
	{
		load.copy_from = now;
		load.copies++;
	}
	else if (step == _oxbox_perf_load_copy_copied && load.copy_from)
	{
		load.copying += now - load.copy_from;
		load.copied_at = now;
		load.copy_from = 0;
	}
	else if (step == _oxbox_perf_load_copy_end && load.copied_at)
	{
		load.finishing += now - load.copied_at;
		load.copied_at = 0;
	}
	else if (step == _oxbox_perf_load_begin && name)
	{
		strncpy(load.map, name, sizeof(load.map) - 1);
	}
	load.at[step] = now;
	if (step == _oxbox_perf_load_setup)
		load.state = 2;
}

/* milliseconds from one step's end to another's (-1: one didn't come) */
static long load_ms(
	__int64 from,
	__int64 to)
{
	return from && to && to >= from ? (long)((to - from) * 1000 / perf.frequency) : -1;
}

/* (v1.180: one map to the next through the main menu, as the showcase goes: the "switch:" line) */
static struct
{
	char from[64];                           /* the last map that wasn't the main menu ("": none yet) */
	__int64 menu_start, menu_frame;          /* the main menu's load after it: its start, its first frame */
} between;

/* the first frame after a load: its line (probe_marks: that frame's steps, main.c, when there are NUMBER_OF_MARKS) */
static void load_finish(
	void const *probe_marks,
	long mark_count)
{
	__int64 now = load_now(), *at = load.at;
	__int64 tags_end = at[_oxbox_perf_load_tags_rebuilt] ? at[_oxbox_perf_load_tags_rebuilt] : at[_oxbox_perf_load_tags_read];
	long before = load_ms(load.start, at[_oxbox_perf_load_begin]);
	long copying = (long)(load.copying * 1000 / perf.frequency), finishing = (long)(load.finishing * 1000 / perf.frequency);
	boolean menu = !strcmp(load.map, "levels\\ui\\ui");

	load.state = 0;
	/* from the main menu's load (the last map left) to this map's first frame: the menu's load, the time at the menu
	and in the lobby (the showcase's waits: SHOWCASE_MENU_SETTLE_SECONDS, the lobby's), this map's load (its copy
	into the cache with it, when the lobby's precache copies) */
	if (menu)
	{
		between.menu_start = between.from[0] ? load.start : 0;
		between.menu_frame = between.from[0] ? now : 0;
	}
	else if (load.map[0])
	{
		if (between.menu_frame)
		{
			platform_log("switch: %s to %s: %ld ms from the main menu's load to its first frame: the main menu %ld ms, "
				"at the menu and in the lobby %ld ms, its load %ld ms", between.from, load.map,
				load_ms(between.menu_start, now), load_ms(between.menu_start, between.menu_frame),
				load_ms(between.menu_frame, load.start), load_ms(load.start, now));
		}
		strncpy(between.from, load.map, sizeof(between.from) - 1);
		between.menu_start = between.menu_frame = 0;
	}
	/* (v1.180: "to the first frame" in its parts: from the set up to the frame's start, then the frame's own steps) */
	if (probe_marks && mark_count == NUMBER_OF_MARKS)
	{
		LARGE_INTEGER const *m = probe_marks;
		__int64 step[NUMBER_OF_MARKS];
		long index;

		for (index = 0; index < NUMBER_OF_MARKS; index++)
		{
			step[index] = m[index].QuadPart;
			if (index && step[index] < step[index - 1])
				step[index] = step[index - 1];
		}
		platform_log("load: %s's first frame: %ld ms from its set up to the frame's start; the frame: input %ld, network %ld, "
			"menus %ld, network end %ld, ticks %ld, camera %ld, drawing %ld, gpu wait %ld, present %ld, after %ld ms",
			load.map[0] ? load.map : "?", load_ms(at[_oxbox_perf_load_setup], step[_mark_start]),
			load_ms(step[_mark_start], step[_mark_input]), load_ms(step[_mark_input], step[_mark_network]),
			load_ms(step[_mark_network], step[_mark_menus]), load_ms(step[_mark_menus], step[_mark_network_end]),
			load_ms(step[_mark_network_end], step[_mark_ticks]), load_ms(step[_mark_ticks], step[_mark_camera]),
			load_ms(step[_mark_camera], step[_mark_drawing]), load_ms(step[_mark_drawing], step[_mark_gpu_wait]),
			load_ms(step[_mark_gpu_wait], step[_mark_present]), load_ms(step[_mark_present], now));
	}
	platform_log("load: %s: %ld ms: copied into the cache %ld ms (%ld file%s), the loading animation's end %ld ms, "
		"the rest before the tags %ld ms; tags read %ld ms, rebuilt %ld ms, checked %ld ms; first BSP %ld ms; "
		"set up %ld ms; objects and AI placed %ld ms; to the first frame %ld ms",
		load.map[0] ? load.map : "?", load_ms(load.start, now), copying, load.copies, load.copies == 1 ? "" : "s",
		finishing, before >= 0 ? before - copying - finishing : -1,
		load_ms(at[_oxbox_perf_load_begin], at[_oxbox_perf_load_tags_read]),
		at[_oxbox_perf_load_tags_rebuilt] ? load_ms(at[_oxbox_perf_load_tags_read], at[_oxbox_perf_load_tags_rebuilt]) : 0,
		load_ms(tags_end, at[_oxbox_perf_load_tags_checked]),
		load_ms(at[_oxbox_perf_load_tags_checked], at[_oxbox_perf_load_scenario]),
		load_ms(at[_oxbox_perf_load_scenario], at[_oxbox_perf_load_initialized]),
		load_ms(at[_oxbox_perf_load_initialized], at[_oxbox_perf_load_setup]),
		load_ms(at[_oxbox_perf_load_setup], now));
}

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
	if (load.state == 2)
		load_finish(probe_marks, mark_count);
	if (sweep.on)
		sweep_menu_frame();

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
	if (sweep.measuring)
		sweep_sample(&sample, now);

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
		else if (!strcmp(name, "perf_sweep"))
			sweep.on = value != 0;
		else if (!strcmp(name, "perf_sweep_detail"))
			sweep.detail = value != 0;
		else if (!strcmp(name, "perf_sweep_scaling"))
			sweep.scaling = value != 0;
		else if (!strcmp(name, "perf_sweep_pass"))
			sweep.pass = value >= 1 && value <= 99 ? value : 0;
		else if (!strcmp(name, "perf_sweep_lod"))
			sweep.lod_rounds = value != 0;
		else if (!strcmp(name, "perf_sweep_redo"))
			sweep.redo_all = value != 0;
		else if (!strcmp(name, "perf_sweep_seconds") && value >= 10)
			sweep.seconds = value > 600 ? 600 : value;
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
	perf_hooks.load = perf_load;
	perf.slow_frame_ms = 34;
	perf.gpu_sample_ms = 1;
	perf.cycle_frequency = 733333333.0;
	memset(&sweep, 0, sizeof(sweep));
	sweep.seconds = 60;
	sweep.scaling = TRUE;
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
	/* the perf sweep (opence.ini perf_sweep = 1): its hooks for the showcase */
	{
		extern unsigned long oxbox_perf_footprint;

		oxbox_perf_footprint = (unsigned long)(((__oxperf_end - __oxperf_start) + 0xFFF) & ~0xFFF) + WRITER_STACK_SIZE;
	}
	if (sweep.on)
	{
		perf_hooks.sweep_next = sweep_next;
		perf_hooks.sweep_frame = sweep_frame;
		perf_hooks.sweep_ended = sweep_ended;
		perf_hooks.sweep_spawn_shots = sweep_spawn_shots;
		perf_hooks.sweep_fatal = sweep_fatal;
		perf.quiet = !sweep.detail;
		platform_log("perf sweep: on: %ld s a run%s", sweep.seconds, perf.quiet ? ", no per-second files" : "");
	}
	platform_log("perf: on (OXPERF loaded at %p, %lu KB, and a %lu KB writer stack); slow frames over %ld ms; "
		"a GPU sample every %ld ms; an object mark costs %lu cycles, a hook call %lu", __oxperf_start,
		(unsigned long)(((__oxperf_end - __oxperf_start) + 0xFFF) & ~0xFFF) / 1024,
		(unsigned long)WRITER_STACK_SIZE / 1024, perf.slow_frame_ms, perf.gpu_sample_ms, inline_mark_cycles,
		hook_call_cycles);
	return &perf_hooks;
}
