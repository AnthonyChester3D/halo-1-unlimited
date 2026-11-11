/*
CACHE_FILES_WINDOWS.C

symbols in this file:
001ABD40 0050:
	_cache_files_dispose (0000)
001ABD90 01a0:
	_cached_map_issue_async_request (0000)
001ABF30 0030:
	_cached_map_block_on_async_request (0000)
001ABF60 0040:
	_cache_request_get (0000)
001ABFA0 0060:
	_cache_request_next_free_index (0000)
001AC000 0080:
	_cache_requests_flush (0000)
001AC080 0010:
	_cache_files_precache_set_priority (0000)
001AC090 0010:
	_cache_files_precache_in_progress (0000)
001AC0A0 0030:
	_cache_files_precache_is_copying_map (0000)
001AC0D0 0030:
	_cache_files_precache_map_queue_end (0000)
001AC100 0040:
	_cached_map_file_get (0000)
001AC140 0040:
	_cached_map_file_invalidate (0000)
001AC180 0040:
	_cached_map_file_get_handle (0000)
001AC1C0 0050:
	_cached_map_file_get_size (0000)
001AC210 0020:
	_cached_map_file_get_path (0000)
001AC230 0010:
	_cache_file_windows_thread_wake (0000)
001AC240 0090:
	_cache_file_read_io_completion_routine@12 (0000)
001AC2D0 0070:
	_cache_file_blocking_io_completion_routine@12 (0000)
001AC340 0020:
	_cache_file_get_map_path (0000)
001AC360 0010:
	_IDirect3DVertexBuffer8_IsBusy@4 (0000)
001AC370 0010:
	_IDirect3DVertexBuffer8_BlockUntilNotBusy@4 (0000)
001AC380 0010:
	_IDirect3DVertexBuffer8_Register@8 (0000)
001AC390 0010:
	_IDirect3DIndexBuffer8_IsBusy@4 (0000)
001AC3A0 0010:
	_IDirect3DIndexBuffer8_BlockUntilNotBusy@4 (0000)
001AC3B0 0020:
	_cache_file_close (0000)
001AC3D0 0150:
	_cache_file_read (0000)
001AC520 0050:
	_cache_file_promote_read (0000)
001AC570 0090:
	_cache_file_read_header_from_dvd (0000)
001AC600 0020:
	_cached_map_issue_async_read (0000)
001AC620 0020:
	_cached_map_issue_async_write (0000)
001AC640 0060:
	_cache_file_block_until_not_busy (0000)
001AC6A0 0060:
	_tags_header_register_vertex_and_index_buffers (0000)
001AC700 00b0:
	_tags_header_deregister_vertex_and_index_buffers (0000)
001AC7B0 0070:
	_structure_bsp_header_register_vertex_buffers (0000)
001AC820 0070:
	_structure_bsp_header_deregister_vertex_buffers (0000)
001AC890 0060:
	_cached_map_files_delete (0000)
001AC8F0 00f0:
	_cache_files_verify_language (0000)
001AC9E0 00b0:
	_cache_files_precache_map_status (0000)
001ACA90 0070:
	_cached_map_file_set_modification_date (0000)
001ACB00 0190:
	_cached_map_file_read_header (0000)
001ACC90 0060:
	_cached_map_files_find_map (0000)
001ACCF0 0190:
	_cached_map_files_find_free_map (0000)
001ACE80 0130:
	_cache_file_windows_thread_proc (0000)
001ACFB0 0120:
	_cache_file_open (0000)
001AD0D0 02f0:
	_cache_files_open_cache_files (0000)
001AD3C0 0030:
	_cache_files_precache_map_loaded (0000)
001AD3F0 0130:
	_cache_files_precache_map_begin (0000)
001AD520 0060:
	_cache_files_precache_map_end (0000)
001AD580 0080:
	_cache_file_windows_thread_create (0000)
001AD600 0060:
	_cache_files_initialize (0000)
002A71F8 002d:
	??_C@_0CN@LGGFIMDL@cache_file_globals?4open_map_file@ (0000)
002A7228 002b:
	??_C@_0CL@CIGLGOJJ@c?3?2halo?2SOURCE?2cache?2cache_files@ (0000)
002A7254 0020:
	??_C@_0CA@OGKEMPOH@Read?1WriteFileEx?$CI?$CJ?5returned?5?$CD?$CFd?$AA@ (0000)
002A7274 0013:
	??_C@_0BD@PLJFCFLH@completion_routine?$AA@ (0000)
002A7288 0010:
	??_C@_0BA@LMKACIBF@completion_flag?$AA@ (0000)
002A7298 000b:
	??_C@_0L@GFMJGGB@overlapped?$AA@ (0000)
002A72A4 001b:
	??_C@_0BL@LMFIEMFC@file?$CB?$DNINVALID_HANDLE_VALUE?$AA@ (0000)
002A72C0 0017:
	??_C@_0BH@MFHBKAIA@async_request_function?$AA@ (0000)
002A72D8 0046:
	??_C@_0EG@KCENDOIE@request_index?$DO?$DN0?5?$CG?$CG?5request_inde@ (0000)
002A7320 002d:
	??_C@_0CN@BGBLHKIB@cache_file_globals?4open_map_file@ (0000)
002A7350 0024:
	??_C@_0CE@EPABAAJM@cache_file_globals?4copy_in_progr@ (0000)
002A7374 003f:
	??_C@_0DP@INCJNNMP@map_file_index?$DO?$DN0?5?$CG?$CG?5map_file_in@ (0000)
002A73B4 0011:
	??_C@_0BB@JJFBACKB@z?3?2cache?$CF03d?4map?$AA@ (0000)
002A73C8 0024:
	??_C@_0CE@EPGEPEPL@finished_request?9?$DOoverlapped?4hEv@ (0000)
002A73EC 002a:
	??_C@_0CK@GKGPAPGO@bytes_transferred?$DN?$DNfinished_requ@ (0000)
002A7418 001a:
	??_C@_0BK@GBNCJMGM@error_code?$DN?$DNERROR_SUCCESS?$AA@ (0000)
002A7434 0013:
	??_C@_0BD@BDBACLGP@overlapped?9?$DOhEvent?$AA@ (0000)
002A7448 0009:
	??_C@_08DCOLLFMA@?$CFs?$CFs?4map?$AA@ (0000)
002A7454 000a:
	??_C@_09GPIOEEJE@offset?$DO?$DN0?$AA@ (0000)
002A7460 001a:
	??_C@_0BK@OLOIOEMB@completion_flag_reference?$AA@ (0000)
002A747C 002c:
	??_C@_0CM@KBEGNHBI@?$CBIDirect3DIndexBuffer8_IsBusy?$CIin@ (0000)
002A74A8 002e:
	??_C@_0CO@CGLHMLAM@?$CBIDirect3DVertexBuffer8_IsBusy?$CIv@ (0000)
002A74D8 0015:
	??_C@_0BF@COEFPJKL@z?3?2last_language?4dat?$AA@ (0000)
002A74F0 002b:
	??_C@_0CL@NILOOFC@couldn?8t?5read?5header?5from?5cache?5@ (0000)
002A751C 003c:
	??_C@_0DM@LINBCMFH@cache_file_globals?4open_map_file@ (0000)
002A7558 0017:
	??_C@_0BH@BPIEKPLM@?$CBbest_request?9?$DOrunning?$AA@ (0000)
002A7570 0015:
	??_C@_0BF@DANPCANK@map_file_index?$CB?$DNNONE?$AA@ (0000)
002A7588 000e:
	??_C@_0O@OLMPCHGI@scenario_name?$AA@ (0000)
002A7598 002d:
	??_C@_0CN@LAKIIMED@couldn?8t?5open?5or?5create?5new?5cach@ (0000)
002A75C8 0026:
	??_C@_0CG@BLEGAMFF@setup?5for?5new?5cache?5file?5failed?5@ (0000)
002A75F0 0014:
	??_C@_0BE@KLDNFNMA@full?5path?5name?5?8?$CFs?8?$AA@ (0000)
002A7604 0022:
	??_C@_0CC@PCGPBKMC@couldn?8t?5find?5map?5?8?$CFs?8?5on?5the?5DV@ (0000)
002A7628 0020:
	??_C@_0CA@GMOAPCML@starting?5precaching?5of?5map?5?8?$CFs?8?$AA@ (0000)
002A7648 001a:
	??_C@_0BK@MIGLNFOL@cache_file_globals?4thread?$AA@ (0000)
002A7664 001f:
	??_C@_0BP@DJCANAAM@cache_file_globals?4sleep_event?$AA@ (0000)
002A7684 001c:
	??_C@_0BM@POLGJDBF@cache_file_globals?4requests?$AA@ (0000)
004CDFF8 307c:
	_cache_file_globals (0000)
*/

/* ---------- headers */

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "cache/cache_files.h"
#include "cache/cache_files_decompress_windows.h"
#include "cache/texture_cache.h"
#include "interface/ui_widget.h"
#include "tag_files/files.h"
#include "tag_files/tag_files.h"
#include "scenario/scenario_definitions.h"
#include "rasterizer/rasterizer.h"
#include "custom_edition_cache.h" /* port: port/linux/game/custom_edition_cache.c */

#include <xtl.h>

/* ---------- constants */

enum
{
	MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS = 512,
	NUMBER_OF_CACHED_MAP_FILES = 6,
	CACHE_FILE_SECTOR_SIZE = 512,
	MAXIMUM_CACHE_FILES = 20,
	CACHE_FILE_THREAD_STACK_SIZE = 0x4000,

	SOLO_CACHE_FILE_MAXIMUM_SIZE = 0x11600000,
	MAIN_MENU_CACHE_FILE_MAXIMUM_SIZE = 0x02300000,
	MULTIPLAYER_CACHE_FILE_MAXIMUM_SIZE = 0x02F00000,
};

/* ---------- macros */

#define CACHE_FILE_BUILD_STRING "01.01.14.2342"

/* ---------- structures */

struct cache_file_tag_instance;

struct cache_file_tag_header
{
	struct cache_file_tag_instance *tag_instances;
	long scenario_tag_index;
	unsigned long checksum;
	long tag_count;
	long vertex_buffer_count;
	D3DVertexBuffer *vertex_buffers;
	long index_buffer_count;
	D3DIndexBuffer *index_buffers;
	unsigned long signature;
};

struct cache_file_structure_bsp_header
{
	void *base_address;
	long vertex_buffer_count;
	D3DVertexBuffer *vertex_buffers;
	long lightmap_vertex_buffer_count;
	D3DVertexBuffer *lightmap_vertex_buffers;
	unsigned long signature;
};

struct cache_file_header
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
	short scenario_type;
	short pad62;
	unsigned long checksum;
	byte reserved68[0x794];
	unsigned long footer_signature;
};

struct cached_map_file
{
	HANDLE file;
	FILETIME last_modification_date;
	struct cache_file_header header;
};

struct cache_file_request
{
	OVERLAPPED overlapped;
	long size;
	void *buffer;
	boolean blocking;
	boolean pending;
	boolean running;
	/* oxbox: read from a shared file the open map draws on, not the map: 1 its pack's, 2 the stock one (cache_file_read) */
	boolean shared;
};

typedef BOOL (WINAPI *cached_map_async_request_proc)(
	HANDLE file,
	void *buffer,
	unsigned long size,
	OVERLAPPED *overlapped,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine);

struct cache_file_runtime_globals
{
	struct cached_map_file cached_map_files[NUMBER_OF_CACHED_MAP_FILES];
	boolean copy_in_progress;
	byte reserved3049;
	short copying_to_map_file_index;
	char copying_to_map_file_name[32];
	short open_map_file_index;
	short blocking_request_index;
	HANDLE sleep_event;
	HANDLE thread;
	struct cache_file_request *requests;
};

typedef char verify_cache_file_header_size[
	sizeof(struct cache_file_header) == 0x800 ? 1 : -1];
typedef char verify_cached_map_file_size[
	sizeof(struct cached_map_file) == 0x80C ? 1 : -1];
typedef char verify_cached_map_file_name_offset[
	offsetof(
		struct cached_map_file,
		header.name) == 0x2C ? 1 : -1];
typedef char verify_cache_file_request_size[
	sizeof(struct cache_file_request) == 0x20 ? 1 : -1];
typedef char verify_cache_file_request_blocking_offset[
	offsetof(
		struct cache_file_request,
		blocking) == 0x1C ? 1 : -1];
typedef char verify_cache_file_request_pending_offset[
	offsetof(
		struct cache_file_request,
		pending) == 0x1D ? 1 : -1];
typedef char verify_cache_file_copy_in_progress_offset[
	offsetof(
		struct cache_file_runtime_globals,
		copy_in_progress) == 0x3048 ? 1 : -1];
typedef char verify_cache_file_copying_map_index_offset[
	offsetof(
		struct cache_file_runtime_globals,
		copying_to_map_file_index) == 0x304A ? 1 : -1];
typedef char verify_cache_file_copying_map_name_offset[
	offsetof(
		struct cache_file_runtime_globals,
		copying_to_map_file_name) == 0x304C ? 1 : -1];
typedef char verify_cache_file_open_map_index_offset[
	offsetof(
		struct cache_file_runtime_globals,
		open_map_file_index) == 0x306C ? 1 : -1];
typedef char verify_cache_file_requests_offset[
	offsetof(
		struct cache_file_runtime_globals,
		requests) == 0x3078 ? 1 : -1];

/* ---------- prototypes */

static void cache_file_get_map_path(
	const char *map_name,
	char *path);
static boolean cache_file_read_header_from_dvd(
	const char *map_name,
	struct cache_file_header *header);
static short cached_map_files_find_free_map(
	long file_length,
	short scenario_type);
static long cached_map_file_get_size(
	short map_file_index);
static HANDLE cached_map_file_get_handle(
	short map_file_index);

static void CALLBACK cache_file_blocking_io_completion_routine(
	unsigned long error_code,
	unsigned long bytes_transferred,
	OVERLAPPED *overlapped);
static void cached_map_issue_async_request(
	cached_map_async_request_proc async_request_function,
	HANDLE file,
	OVERLAPPED *overlapped,
	void *buffer,
	long size,
	long offset,
	volatile boolean *completion_flag,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine);
static void cached_map_issue_async_read(
	HANDLE file,
	OVERLAPPED *overlapped,
	void *buffer,
	long size,
	long offset,
	volatile boolean *completion_flag,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine);
static void cached_map_issue_async_write(
	HANDLE file,
	OVERLAPPED *overlapped,
	void *buffer,
	long size,
	long offset,
	volatile boolean *completion_flag,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine);
static boolean cached_map_block_on_async_request(
	volatile boolean const *completion_flag);
static void cached_map_file_get_path(
	short map_file_index,
	char *path);
static void cached_map_file_set_modification_date(
	short map_file_index);
static void cached_map_file_read_header(
	short map_file_index);
static void cached_map_files_delete(
	short map_file_index);
static void cache_files_verify_language(
	void);
static void cache_files_open_cache_files(
	void);
static void CALLBACK cache_file_read_io_completion_routine(
	unsigned long error_code,
	unsigned long bytes_transferred,
	OVERLAPPED *overlapped);
static void cache_file_windows_thread_proc(
	void);
static void cache_file_windows_thread_create(
	void);
static struct cache_file_request *cache_request_get(
	short request_index);
static short cache_request_next_free_index(
	void);
static void cache_file_windows_thread_wake(
	void);
static void cache_requests_flush(
	void);
static struct cached_map_file *cached_map_file_get(
	short map_file_index);
static void cached_map_file_invalidate(
	short map_file_index);
static short cached_map_files_find_map(
	const char *map_name);
#if defined(HALO_XBOX_TARGET)
static boolean shared_file_path(const char *key, char *path);
boolean cache_files_shared_wanted(const char *map_name, char *key);
static boolean cache_files_shared_open(const char *map_name);
static boolean shared_slot_kept(short map_file_index);
static void shared_lookup_forget(void);
/* the shared files the open map draws on: its pack's, the stock one's (docs/shared_maps.md). A packed one is in a cache
file; one stored unpacked is read where it is, on its own handle, and has no cache file (NONE) */
static short shared_open_map_file_index[2] = {NONE, NONE};
static HANDLE shared_open_file[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
static long shared_open_length[2];
static void shared_open_close(void);
#endif

/* ---------- globals */

static struct cache_file_runtime_globals cache_file_globals;

/* ---------- public code */

void tags_header_register_vertex_and_index_buffers(
	struct cache_file_tag_header *header)
{
	/* port: a long, as the buffers' counts are (a short wrapped on a count
	past 0x7FFF, and the loop never ended) */
	long index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	for (index = 0; index < header->index_buffer_count; index++)
	{
		D3DIndexBuffer *index_buffer = &header->index_buffers[index];

		index_buffer->Common = D3DCOMMON_TYPE_INDEXBUFFER | 1;
	}

	return;
}

void tags_header_deregister_vertex_and_index_buffers(
	struct cache_file_tag_header *header)
{
	/* port: a long, as the buffers' counts are (a short wrapped on a count
	past 0x7FFF, and the loop never ended) */
	long index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		IDirect3DVertexBuffer8_BlockUntilNotBusy(vertex_buffer);
		match_assert(
			"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
			523,
			!IDirect3DVertexBuffer8_IsBusy(vertex_buffer));
	}

	for (index = 0; index < header->index_buffer_count; index++)
	{
		D3DIndexBuffer *index_buffer = &header->index_buffers[index];

		IDirect3DIndexBuffer8_BlockUntilNotBusy(index_buffer);
		match_assert(
			"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
			536,
			!IDirect3DIndexBuffer8_IsBusy(index_buffer));
	}

	return;
}

void structure_bsp_header_register_vertex_buffers(
	struct cache_file_structure_bsp_header *header)
{
	/* port: a long, as the buffers' counts are (a short wrapped on a count
	past 0x7FFF, and the loop never ended) */
	long index;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	for (index = 0; index < header->lightmap_vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->lightmap_vertex_buffers[index];

		vertex_buffer->Common = D3DCOMMON_TYPE_VERTEXBUFFER | 1;
		IDirect3DVertexBuffer8_Register(vertex_buffer, NULL);
	}

	return;
}

void structure_bsp_header_deregister_vertex_buffers(
	struct cache_file_structure_bsp_header *header)
{
	/* port: a long, as the buffers' counts are (a short wrapped on a count
	past 0x7FFF, and the loop never ended) */
	long index;

	rasterizer_globals.current_lock_operation = _rasterizer_lock_bsp_switch;

	for (index = 0; index < header->vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->vertex_buffers[index];

		IDirect3DVertexBuffer8_BlockUntilNotBusy(vertex_buffer);
	}

	for (index = 0; index < header->lightmap_vertex_buffer_count; index++)
	{
		D3DVertexBuffer *vertex_buffer = &header->lightmap_vertex_buffers[index];

		IDirect3DVertexBuffer8_BlockUntilNotBusy(vertex_buffer);
	}

	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;

	return;
}

void cache_files_dispose(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		207,
		cache_file_globals.open_map_file_index==NONE);
	match_free(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		209,
		cache_file_globals.requests);

	return;
}

void cache_files_precache_set_priority(
	boolean blocking)
{
	cache_copy_set_priority(blocking);

	return;
}

#if defined(HALO_XBOX_TARGET)
/* oxbox: a map's cached copy is forgotten (map sharing just replaced its folder with the host's build); TRUE if one was */
boolean cache_files_forget_cached_map(
	char const *map_name)
{
	short map_file_index = cached_map_files_find_map(map_name);

	/* (what was last read of its shared files: a copy just installed draws on what the one before didn't) */
	shared_lookup_forget();
	if (map_file_index == NONE || map_file_index == cache_file_globals.open_map_file_index ||
		map_file_index == cache_file_globals.copying_to_map_file_index || shared_slot_kept(map_file_index))
	{
		return FALSE;
	}
	cached_map_file_get(map_file_index)->header.name[0] = 0;
	return TRUE;
}
#endif

boolean cache_files_precache_in_progress(
	void)
{
	return cache_file_globals.copy_in_progress;
}

boolean cache_files_precache_is_copying_map(
	const char *map_name)
{
	if (cache_file_globals.copying_to_map_file_index != NONE &&
		strcmp(
			cache_file_globals.copying_to_map_file_name,
			tag_name_strip_path(map_name)) == 0)
	{
		return TRUE;
	}

	return FALSE;
}

boolean cache_files_precache_map_loaded(
	const char *map_name)
{
	/* port: a Halo Custom Edition map (custom_maps\<name>) is read in place
	and never copied to the cache partition; it is never the game's own map
	of that file name (port/linux/game/custom_edition_cache.c) */
	if (custom_edition_level_name(map_name))
		return custom_edition_cache_playable(map_name);
#if defined(HALO_XBOX_TARGET)
	/* oxbox: nor is a map whose shared file isn't (cache_files_shared_wanted) */
	if (cache_files_shared_wanted(map_name, NULL))
		return FALSE;
#endif
	return cached_map_files_find_map(tag_name_strip_path(map_name)) != NONE;
}

/* (the port's, port/linux/src/sdl_platform.c) */
void platform_log(char const *format, ...);

boolean cache_files_precache_map_begin(
	const char *map_name,
	boolean copy_map)
{
	const char *cache_map_name = tag_name_strip_path(map_name);

	/* port: a Halo Custom Edition map (custom_maps\<name>) this machine has
	not is missing, as a map not on the DVD is: never the game's own map of
	its file name (port/linux/game/custom_edition_cache.c) */
	if (custom_edition_level_name(map_name) && !custom_edition_cache_playable(map_name))
	{
		error(_error_silent, "couldn't find the Custom Edition map '%s' in custom_maps", map_name);
		if (copy_map)
		{
			display_error_damaged_media();
		}

		return FALSE;
	}
	if (!cache_files_precache_map_loaded(map_name))
	{
		struct cache_file_header header;
		char path[256];

		if (cache_file_read_header_from_dvd(cache_map_name, &header))
		{
			long buffer_size = cache_copy_buffer_size(copy_map);
			/* oxbox: a pack's shared file (its key "*<name>") goes in a multiplayer cache file when it fits (they're
			47 MiB; a pack's is 1-27 MiB), so both of the campaign's stay for the level being left and the next
			(docs/shared_maps.md); a bigger one goes in a campaign one (cached_map_files_find_free_map) */
			short map_file_index = cached_map_files_find_free_map(
				header.file_length,
#if defined(HALO_XBOX_TARGET)
				cache_map_name[0] == '*' ? (short)_scenario_type_multiplayer :
#endif
				header.scenario_type);
			void *buffer;
			struct cached_map_file *map_file;

			/* port: the cache file slots are found by the name in their
			header (cached_map_files_find_map): a map file whose header names
			another map would be copied again each time it was asked for, for
			ever */
			/* oxbox: a folder map's file names itself whatever its maker called it; the cache slot gets its key
			(cache_files_precache_map_end) */
			if (_stricmp(header.name, cache_map_name) != 0
#if defined(HALO_XBOX_TARGET)
				&& cache_map_name[0] != '+' && cache_map_name[0] != '*'
#endif
				)
			{
				error(_error_silent, "map '%s' names itself '%s' in its header: refused", cache_map_name, header.name);
				platform_log("map %s.map names itself '%s' in its header; a map's name must be its file's",
					cache_map_name, header.name);
				if (copy_map)
				{
					display_error_damaged_media();
				}

				return FALSE;
			}

			/* port: a map no cache file holds (of no type the cache files are
			for, or too big for its type's) is not precached; the texture
			cache's memory is taken only once one does */
			if (map_file_index == NONE)
			{
				error(_error_silent, "no cache file can hold map '%s' (%08x bytes, type %d)",
					cache_map_name, header.file_length, header.scenario_type);
				if (copy_map)
				{
					display_error_damaged_media();
				}

				return FALSE;
			}
			buffer = texture_cache_steal_memory(buffer_size);
			map_file = cached_map_file_get(map_file_index);

			memset(
				&map_file->header,
				0,
				sizeof(struct cache_file_header));
			cache_file_globals.copy_in_progress = TRUE;
			cache_file_globals.copying_to_map_file_index = map_file_index;
			strncpy(
				cache_file_globals.copying_to_map_file_name,
				cache_map_name,
				sizeof(cache_file_globals.copying_to_map_file_name) - 1);
			cache_file_globals.copying_to_map_file_name[
				sizeof(cache_file_globals.copying_to_map_file_name) - 1] = 0;
			cache_file_get_map_path(cache_map_name, path);
			error(_error_silent, "starting precaching of map '%s'", cache_map_name);
			cache_copy_begin(
				buffer,
				buffer_size,
				cached_map_file_get_handle(map_file_index),
				cached_map_file_get_size(map_file_index),
				path);
		}
		else
		{
			error(_error_silent, "couldn't find map '%s' on the DVD", cache_map_name);
			error(_error_silent, "full path name '%s'", map_name);
			if (copy_map)
			{
				display_error_damaged_media();
			}

			return FALSE;
		}
	}

	return TRUE;
}

void cache_files_initialize(
	void)
{
	cache_file_globals.open_map_file_index = NONE;
	cache_file_globals.requests = match_malloc(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		187,
		MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS * sizeof(struct cache_file_request));
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		188,
		cache_file_globals.requests);
	/* port: cache_file_open clears the requests before a map is read; a Halo
	Custom Edition map is read without it, so they start out free
	(port/linux/game/custom_edition_cache.c) */
	memset(cache_file_globals.requests, 0, MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS * sizeof(struct cache_file_request));
	cache_file_windows_thread_create();
	cache_files_verify_language();
	cache_files_open_cache_files();
	cache_copy_initialize();

	return;
}

void cache_files_precache_map_end(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1031,
		cache_file_globals.copy_in_progress);
	cache_copy_end();
	texture_cache_return_memory();
	cached_map_file_set_modification_date(cache_file_globals.copying_to_map_file_index);
	cached_map_file_read_header(cache_file_globals.copying_to_map_file_index);
#if defined(HALO_XBOX_TARGET)
	/* oxbox: a folder map's slot is found by its key ("+Precipice"), whatever name the map has inside (Halo
	Universal's cache identity) */
	if (cache_file_globals.copying_to_map_file_name[0] == '+' || cache_file_globals.copying_to_map_file_name[0] == '*')
	{
		struct cached_map_file *map_file = cached_map_file_get(cache_file_globals.copying_to_map_file_index);

		if (map_file->header.name[0])
		{
			csstrncpy(map_file->header.name, cache_file_globals.copying_to_map_file_name, sizeof(map_file->header.name) - 1);
			map_file->header.name[sizeof(map_file->header.name) - 1] = 0;
		}
	}
#endif
	cache_file_globals.copy_in_progress = FALSE;
	cache_file_globals.copying_to_map_file_index = NONE;

	return;
}

void cache_files_precache_map_queue_end(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1022,
		cache_file_globals.copy_in_progress);
	cache_copy_queue_end();

	return;
}

void cache_file_promote_read(
	short request_index)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		614,
		request_index>=0 && request_index<MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS);
	cache_file_globals.requests[request_index].blocking = TRUE;

	return;
}

void cache_file_block_until_not_busy(
	void)
{
	boolean busy;
	short request_index;

	do
	{
		SleepEx(0, TRUE);
		busy = FALSE;
		for (request_index = 0;
			request_index < MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS;
			request_index++)
		{
			match_assert(
				"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
				614,
				request_index>=0 && request_index<MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS);
			if (cache_file_globals.requests[request_index].pending)
			{
				busy = TRUE;
			}
		}
	}
	while (busy);

	return;
}

void cache_file_close(
	void)
{
	if (cache_file_globals.open_map_file_index != NONE)
	{
		cache_requests_flush();
		cache_file_globals.open_map_file_index = NONE;
	}
#if defined(HALO_XBOX_TARGET)
	shared_open_close();
#endif

	return;
}

boolean cache_file_open(
	const char *scenario_name,
	struct cache_file_header *header)
{
	short map_file_index = cached_map_files_find_map(scenario_name);

	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		220,
		scenario_name);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		221,
		header);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		223,
		cache_file_globals.open_map_file_index==NONE);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		224,
		map_file_index!=NONE);
	/* port: a map in no cache file is not opened (its index would be
	NONE's, before the cache files) */
	if (map_file_index == NONE)
	{
		error(_error_silent, "the map '%s' is in no cache file", scenario_name);

		return FALSE;
	}
#if defined(HALO_XBOX_TARGET)
	/* oxbox: a map that draws on a shared file opens only with that file cached, the right version (docs/shared_maps.md) */
	if (!cache_files_shared_open(scenario_name))
		return FALSE;
#endif
	memset(
		cache_file_globals.requests,
		0,
		MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS * sizeof(struct cache_file_request));
	cache_file_globals.open_map_file_index = map_file_index;
	memcpy(
		header,
		&cached_map_file_get(map_file_index)->header,
		sizeof(struct cache_file_header));

	return TRUE;
}

short cache_file_read(
	long tag_index,
	long offset,
	long size,
	void *buffer,
	boolean *completion_flag_reference,
	boolean blocking)
{
	short request_index = cache_request_next_free_index();
	struct cache_file_request *request = cache_request_get(request_index);

	/* port: the reads of a Halo Custom Edition map are served in place, at
	once; the request stays free (port/linux/game/custom_edition_cache.c) */
	if (custom_edition_cache_tags_loaded())
	{
		custom_edition_cache_read(tag_index, offset, size, buffer);
		*completion_flag_reference = TRUE;

		return request_index;
	}
#if defined(HALO_XBOX_TARGET)
	/* oxbox: an offset with its top bit set is in a shared file the open map draws on: the next bit says which, its
	pack's (0) or the stock one (1) (docs/shared_maps.md) */
	boolean shared = FALSE;

	if (offset < 0)
	{
		short which = (offset & 0x40000000) ? 1 : 0;
		long length = shared_open_file[which] != INVALID_HANDLE_VALUE ? shared_open_length[which] : 0;

		offset &= 0x3FFFFFFF;
		if (!length || size < 0 || offset > length || size > length - offset)
		{
			error(_error_silent, "shared file read of %08x bytes at %08x refused (%s)", size, offset,
				!length ? "the map has no such shared file" : "past its end");
			*completion_flag_reference = _cache_file_read_failed;

			return NONE;
		}
		shared = (boolean)(1 + which);
	}
#endif
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		269,
		cache_file_globals.open_map_file_index!=NONE);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		272,
		buffer);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		273,
		completion_flag_reference);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		276,
		offset>=0);
	/* port: the offset and size are the map's (a texture's or sound's,
	which nothing checks): a negative one is no read, failed at once (a
	negative size read the file to its end over what follows the buffer) */
	if (offset < 0 || size < 0)
	{
		error(_error_silent, "cache file read of %08x bytes at %08x refused", size, offset);
		*completion_flag_reference = _cache_file_read_failed;

		return NONE;
	}
	if (size & (CACHE_FILE_SECTOR_SIZE - 1))
	{
		size = (size | (CACHE_FILE_SECTOR_SIZE - 1)) + 1;
	}
	*completion_flag_reference = FALSE;
	memset(
		&request->overlapped,
		0,
		sizeof(OVERLAPPED));
	request->overlapped.hEvent = (HANDLE)completion_flag_reference;
	request->size = size;
	request->overlapped.OffsetHigh = 0;
	request->overlapped.Offset = offset;
	request->buffer = buffer;
	request->pending = TRUE;
	request->blocking = blocking;
	request->running = FALSE;
#if defined(HALO_XBOX_TARGET)
	request->shared = shared;
#else
	request->shared = FALSE;
#endif
	cache_file_windows_thread_wake();

	return request_index;
}

short cache_files_precache_map_status(
	real *progress)
{
	short status = _cached_map_file_failed;

	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		984,
		cache_file_globals.copy_in_progress);
	switch (cache_copy_get_status(progress))
	{
		case _cache_copy_bad_file_failure:
		case _cache_copy_read_failure:
			status = _cached_map_file_failed;
			break;

		case _cache_copy_write_failure:
			cached_map_file_invalidate(cache_file_globals.copying_to_map_file_index);
			status = _cached_map_file_failed;
			break;

		case _cache_copy_in_progress:
			status = _cached_map_file_in_progress;
			break;

		case _cache_copy_finished:
			status = _cached_map_file_success;
			break;

		default:
			match_vassert("c:\\halo\\SOURCE\\cache\\cache_files_windows.c", 1013, FALSE, NULL);
			break;
	}

	return status;
}

/* ---------- private code */

static void cached_map_files_delete(
	short map_file_index)
{
	char path[256];

	while (++map_file_index < MAXIMUM_CACHE_FILES)
	{
		cached_map_file_get_path(map_file_index, path);
		DeleteFileA(path);
	}

	SetLastError(ERROR_SUCCESS);

	return;
}

static void cache_files_verify_language(
	void)
{
	struct file_reference reference;
	long file_language;
	long language = XGetLanguage();
	long last_language = NONE;

	if (file_reference_create_from_path(&reference, "z:\\last_language.dat", FALSE))
	{
		if (file_open(&reference, FLAG(_permission_read_bit)))
		{
			if (file_read(&reference, sizeof(long), &file_language))
			{
				last_language = file_language;
			}
			file_close(&reference);
		}
	}

	if (last_language != language)
	{
		short map_file_index;

		for (map_file_index = NONE;
			map_file_index < NUMBER_OF_CACHED_MAP_FILES;
			map_file_index++)
		{
			cached_map_files_delete(map_file_index);
		}
	}

	if (file_reference_create_from_path(&reference, "z:\\last_language.dat", FALSE))
	{
		if (file_create(&reference))
		{
			if (file_open(&reference, FLAG(_permission_write_bit)))
			{
				file_write(&reference, sizeof(long), &language);
				file_close(&reference);
			}
		}
	}

	return;
}

static void cache_files_open_cache_files(
	void)
{
	struct cache_file_header blank_header;
	struct cache_file_header dvd_header;
	char path[256];
	OVERLAPPED overlapped;
	boolean deleted_stale_cache_files = FALSE;
	volatile boolean completion_flag;
	boolean valid;
	short map_file_index;

	cached_map_files_delete(NUMBER_OF_CACHED_MAP_FILES);
	for (map_file_index = 0;
		map_file_index < NUMBER_OF_CACHED_MAP_FILES;
		map_file_index++)
	{
		struct cached_map_file *map_file = cached_map_file_get(map_file_index);
		long size;
		HANDLE file;

		valid = FALSE;
		cached_map_file_get_path(map_file_index, path);
		size = cached_map_file_get_size(map_file_index);
		file = CreateFileA(
			path,
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_ALWAYS,
			FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
			NULL);
		if (file != INVALID_HANDLE_VALUE)
		{
			if (GetLastError() == ERROR_ALREADY_EXISTS &&
				GetFileSize(file, NULL) == (unsigned long)size)
			{
				valid = TRUE;
			}
			else
			{
				if (!deleted_stale_cache_files)
				{
					cached_map_files_delete(map_file_index);
					deleted_stale_cache_files = TRUE;
				}

				completion_flag = FALSE;
				cached_map_issue_async_write(
					file,
					&overlapped,
					&blank_header,
					sizeof(struct cache_file_header),
					0,
					&completion_flag,
					cache_file_blocking_io_completion_routine);
				cached_map_block_on_async_request(&completion_flag);
				if (completion_flag &&
					SetFilePointer(file, size, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
					SetEndOfFile(file))
				{
					valid = TRUE;
				}
				else
				{
					valid = FALSE;
					match_vassert(
						"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
						811,
						FALSE,
						csprintf(temporary, "setup for new cache file failed (#%d)", GetLastError()));
				}
			}
		}
		else
		{
			match_vassert(
				"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
				823,
				FALSE,
				csprintf(temporary, "couldn't open or create new cache file (#%d)", GetLastError()));
		}

		if (!valid && file != INVALID_HANDLE_VALUE)
		{
			CloseHandle(file);
			file = INVALID_HANDLE_VALUE;
		}

		map_file->file = file;
		if (valid)
		{
			char *cache_map_name = cached_map_file_get(map_file_index)->header.name;

			cached_map_file_read_header(map_file_index);
#if defined(HALO_XBOX_TARGET)
			/* oxbox: never keep a map cached by an earlier launch, as Halo Universal does (PATCHES.txt 1):
			a copy cut short (xemu closed, a console switched off while it copied) keeps a whole header
			with the map's checksum, and its missing data drew as garbage in the menus. Each map is copied
			fresh the first time a launch loads it. */
			(void)cache_map_name;
			(void)dvd_header;
#else
			if (cache_file_read_header_from_dvd(cache_map_name, &dvd_header) &&
				map_file->header.checksum == dvd_header.checksum &&
				valid)
			{
				continue;
			}
#endif
		}

		memset(
			&map_file->header,
			0,
			sizeof(struct cache_file_header));
	}

	return;
}

static void CALLBACK cache_file_read_io_completion_routine(
	unsigned long error_code,
	unsigned long bytes_transferred,
	OVERLAPPED *overlapped)
{
	struct cache_file_request *finished_request = (struct cache_file_request *)overlapped;

	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1387,
		error_code==ERROR_SUCCESS);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1388,
		bytes_transferred==finished_request->size);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1389,
		finished_request->overlapped.hEvent);
	/* port: a read that failed or came up short completes as failed: what
	waits on it stops waiting, and a caller that checks (the map's tags and
	bsps) sees it, instead of taking what is in its buffer for the map */
	*(volatile boolean *)finished_request->overlapped.hEvent =
		error_code == ERROR_SUCCESS && bytes_transferred == (unsigned long)finished_request->size ?
			TRUE :
			_cache_file_read_failed;
	finished_request->pending = FALSE;
	finished_request->running = FALSE;

	return;
}

static void cache_file_windows_thread_proc(
	void)
{
	while (TRUE)
	{
		while (WaitForSingleObjectEx(
			cache_file_globals.sleep_event,
			INFINITE,
			TRUE) == WAIT_IO_COMPLETION)
			;

		while (TRUE)
		{
			struct cache_file_request *best_request = NULL;
			short request_index;
			HANDLE file;

			for (request_index = 0;
				request_index < MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS;
				request_index++)
			{
				struct cache_file_request *request = cache_request_get(request_index);

				if (request->pending &&
					!request->running &&
					(!best_request ||
						(best_request->blocking > request->blocking &&
						best_request->overlapped.Offset > request->overlapped.Offset)))
				{
					best_request = request;
				}
			}

			if (!best_request)
			{
				break;
			}

			file = cached_map_file_get_handle(cache_file_globals.open_map_file_index);
#if defined(HALO_XBOX_TARGET)
			/* oxbox: a shared file's read (cache_file_read) */
			if (best_request->shared && shared_open_file[best_request->shared - 1] != INVALID_HANDLE_VALUE)
				file = shared_open_file[best_request->shared - 1];
#endif
			match_assert(
				"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
				1327,
				!best_request->running);
			best_request->running = TRUE;
			cached_map_issue_async_read(
				file,
				&best_request->overlapped,
				best_request->buffer,
				best_request->size,
				best_request->overlapped.Offset,
				(volatile boolean *)best_request->overlapped.hEvent,
				cache_file_read_io_completion_routine);
		}
	}

	return;
}

#ifdef HALO_WEB
/* port: WebAssembly calls a function only through a pointer of its own type,
so the thread starts here, with the type CreateThread calls */
static DWORD WINAPI cache_file_windows_thread_start(
	void *parameter)
{
	cache_file_windows_thread_proc();
	return 0;
}
#endif

static void cache_file_windows_thread_create(
	void)
{
	cache_file_globals.sleep_event = CreateEventA(NULL, FALSE, FALSE, NULL);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1253,
		cache_file_globals.sleep_event);
	cache_file_globals.thread = CreateThread(
		NULL,
		CACHE_FILE_THREAD_STACK_SIZE,
#ifdef HALO_WEB
		cache_file_windows_thread_start,
#else
		(LPTHREAD_START_ROUTINE)cache_file_windows_thread_proc,
#endif
		NULL,
		0,
		NULL);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1257,
		cache_file_globals.thread);

	return;
}

static void cache_file_get_map_path(
	const char *map_name,
	char *path)
{
	/* port: no more than the callers' paths hold (256; the name can be a
	host's, over the network). One that doesn't fit is no path (no file is
	found), not a cut one (another file could be). */
	enum
	{
		MAXIMUM_MAP_PATH_LENGTH = 256,
	};
	int length;

#if defined(HALO_XBOX_TARGET)
	/* oxbox: a folder map ("+key") is read from its folder (port/linux/game/hmp_maps.c) */
	{
		extern boolean hmp_map_file_path(char const *level_name, char *path, long size);

		if (hmp_map_file_path(map_name, path, MAXIMUM_MAP_PATH_LENGTH))
			return;
		/* oxbox: a shared file ("*name": cache_files_shared_wanted found where it is) */
		if (map_name[0] == '*')
		{
			if (!shared_file_path(map_name, path))
				path[0] = 0;
			return;
		}
	}
#endif
	length = snprintf(path, MAXIMUM_MAP_PATH_LENGTH, "%s%s.map", cache_files_map_directory(), map_name);

	if (length < 0 || length >= MAXIMUM_MAP_PATH_LENGTH)
	{
		error(_error_silent, "map path for '%.64s' is too long", map_name);
		path[0] = 0;
	}

	return;
}

static boolean cache_file_read_header_from_dvd(
	const char *map_name,
	struct cache_file_header *header)
{
	boolean result = FALSE;
	char path[256];
	HANDLE file;

	cache_file_get_map_path(map_name, path);
	file = CreateFileA(
		path,
		GENERIC_READ,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		unsigned long bytes_read;

		if (ReadFile(
			file,
			header,
			sizeof(struct cache_file_header),
			&bytes_read,
			NULL) &&
			bytes_read == sizeof(struct cache_file_header) &&
			cache_file_header_verify(header, path, TRUE))
		{
			result = TRUE;
		}
		CloseHandle(file);
	}

	return result;
}

static short cached_map_files_find_free_map(
	long file_length,
	short scenario_type)
{
	short best_map_file_index = NONE;
	struct cached_map_file *best_map_file;
	short first_map_file_index;
	short last_map_file_index;
	short map_file_index;

	switch (scenario_type)
	{
		case _scenario_type_solo:
			first_map_file_index = 0;
			last_map_file_index = 1;
			break;

		case _scenario_type_multiplayer:
			first_map_file_index = 3;
			last_map_file_index = 5;
			break;

		case _scenario_type_main_menu:
			first_map_file_index = 2;
			last_map_file_index = 2;
			break;

		/* port: the type is the map's header's, and a release build's assertion
		goes on: a type no cache file is for finds none */
		default:
			match_vassert("c:\\halo\\SOURCE\\cache\\cache_files_windows.c", 1172, FALSE, NULL);
			return NONE;
	}

	for (map_file_index = first_map_file_index;
		map_file_index <= last_map_file_index;
		map_file_index++)
	{
		if (cache_file_globals.open_map_file_index != map_file_index
#if defined(HALO_XBOX_TARGET)
			&& !shared_slot_kept(map_file_index)
#endif
			)
		{
			struct cached_map_file *map_file = cached_map_file_get(map_file_index);

			if (cached_map_file_get_size(map_file_index) > file_length)
			{
				if (best_map_file_index == NONE ||
					cached_map_file_get_size(map_file_index) < cached_map_file_get_size(best_map_file_index) ||
					CompareFileTime(
						&best_map_file->last_modification_date,
						&map_file->last_modification_date) > 0)
				{
					best_map_file_index = map_file_index;
					best_map_file = map_file;
				}
			}
		}
	}

	/* oxbox (Halo Universal 2): a multiplayer or menu map too big for its own cache files goes into one of the
	campaign's (278 MiB), which keep their stock size. The dedicated server too (its multiplayer cache files are the
	Xbox's size: Halo 1 Maps Collection's 57-66 MB maps) */
	if (best_map_file_index == NONE && first_map_file_index != 0)
	{
		for (map_file_index = 0; map_file_index <= 1; map_file_index++)
		{
			struct cached_map_file *map_file = cached_map_file_get(map_file_index);

			if (cache_file_globals.open_map_file_index != map_file_index &&
#if defined(HALO_XBOX_TARGET)
				!shared_slot_kept(map_file_index) &&
#endif
				cached_map_file_get_size(map_file_index) > file_length &&
				(best_map_file_index == NONE ||
					CompareFileTime(&best_map_file->last_modification_date, &map_file->last_modification_date) > 0))
			{
				best_map_file_index = map_file_index;
				best_map_file = map_file;
			}
		}
	}
#if defined(OXBOX_STRIP)
	/* oxbox: a campaign map too busy for the campaign's two (one holding the shared file it draws on, the other
	the map being played) goes into a multiplayer one if it fits: a map drawing on a shared file is small */
	if (best_map_file_index == NONE && first_map_file_index == 0)
	{
		for (map_file_index = 3; map_file_index <= 5; map_file_index++)
		{
			struct cached_map_file *map_file = cached_map_file_get(map_file_index);

			if (cache_file_globals.open_map_file_index != map_file_index && !shared_slot_kept(map_file_index) &&
				cached_map_file_get_size(map_file_index) > file_length &&
				(best_map_file_index == NONE ||
					CompareFileTime(&best_map_file->last_modification_date, &map_file->last_modification_date) > 0))
			{
				best_map_file_index = map_file_index;
				best_map_file = map_file;
			}
		}
	}
#endif
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1200,
		cache_file_globals.open_map_file_index!=best_map_file_index);

	return best_map_file_index;
}

static long cached_map_file_get_size(
	short map_file_index)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1232,
		map_file_index>=0 && map_file_index<NUMBER_OF_CACHED_MAP_FILES);
	if (map_file_index <= 1)
	{
		return SOLO_CACHE_FILE_MAXIMUM_SIZE;
	}

	return (map_file_index <= 2)
		? MAIN_MENU_CACHE_FILE_MAXIMUM_SIZE
		: MULTIPLAYER_CACHE_FILE_MAXIMUM_SIZE;
}

static HANDLE cached_map_file_get_handle(
	short map_file_index)
{
	return cached_map_file_get(map_file_index)->file;
}

static void CALLBACK cache_file_blocking_io_completion_routine(
	unsigned long error_code,
	unsigned long bytes_transferred,
	OVERLAPPED *overlapped)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1405,
		error_code==ERROR_SUCCESS);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1406,
		overlapped->hEvent);
	*(volatile boolean *)overlapped->hEvent = TRUE;

	return;
}

static void cached_map_issue_async_request(
	cached_map_async_request_proc async_request_function,
	HANDLE file,
	OVERLAPPED *overlapped,
	void *buffer,
	long size,
	long offset,
	volatile boolean *completion_flag,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		390,
		async_request_function);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		391,
		file!=INVALID_HANDLE_VALUE);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		392,
		overlapped);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		393,
		buffer);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		394,
		completion_flag);
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		395,
		completion_routine);
	memset(
		overlapped,
		0,
		sizeof(OVERLAPPED));
	overlapped->Offset = offset;
	overlapped->OffsetHigh = 0;
	overlapped->hEvent = (HANDLE)completion_flag;
	while (TRUE)
	{
		unsigned long error_code;

		SleepEx(0, TRUE);
		SetLastError(ERROR_SUCCESS);
		if (async_request_function(
			file,
			buffer,
			size,
			overlapped,
			completion_routine))
		{
			break;
		}

		error_code = GetLastError();
		match_vassert(
			"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
			422,
			error_code==ERROR_NOT_ENOUGH_MEMORY || error_code==ERROR_NO_SYSTEM_RESOURCES || error_code==ERROR_INVALID_USER_BUFFER,
			csprintf(temporary, "Read/WriteFileEx() returned #%d", GetLastError()));
	}

	return;
}

static void cached_map_issue_async_read(
	HANDLE file,
	OVERLAPPED *overlapped,
	void *buffer,
	long size,
	long offset,
	volatile boolean *completion_flag,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	cached_map_issue_async_request(
		ReadFileEx,
		file,
		overlapped,
		buffer,
		size,
		offset,
		completion_flag,
		completion_routine);

	return;
}

static void cached_map_issue_async_write(
	HANDLE file,
	OVERLAPPED *overlapped,
	void *buffer,
	long size,
	long offset,
	volatile boolean *completion_flag,
	LPOVERLAPPED_COMPLETION_ROUTINE completion_routine)
{
	cached_map_issue_async_request(
		(cached_map_async_request_proc)WriteFileEx,
		file,
		overlapped,
		buffer,
		size,
		offset,
		completion_flag,
		completion_routine);

	return;
}

static boolean cached_map_block_on_async_request(
	volatile boolean const *completion_flag)
{
	boolean completed;

	while (!*completion_flag)
	{
		if (SleepEx(5000, TRUE) != WAIT_IO_COMPLETION)
		{
			break;
		}
	}
	completed = *completion_flag;

	return completed;
}

static void cached_map_file_get_path(
	short map_file_index,
	char *path)
{
	sprintf(path, "z:\\cache%03d.map", map_file_index);

	return;
}

static void cached_map_file_set_modification_date(
	short map_file_index)
{
	struct cached_map_file *map_file = cached_map_file_get(map_file_index);
	SYSTEMTIME system_time;

	GetSystemTime(&system_time);
	SystemTimeToFileTime(&system_time, &map_file->last_modification_date);
	SetFileTime(map_file->file, &map_file->last_modification_date, NULL, NULL);

	return;
}

static void cached_map_file_read_header(
	short map_file_index)
{
	struct cached_map_file *map_file = cached_map_file_get(map_file_index);
	char path[256];
	OVERLAPPED overlapped;
	volatile boolean completion_flag;

	cached_map_file_get_path(map_file_index, path);
	GetFileTime(map_file->file, &map_file->last_modification_date, NULL, NULL);
	completion_flag = FALSE;
	cached_map_issue_async_read(
		map_file->file,
		&overlapped,
		&map_file->header,
		sizeof(struct cache_file_header),
		0,
		&completion_flag,
		cache_file_blocking_io_completion_routine);
	cached_map_block_on_async_request(&completion_flag);
	if (completion_flag)
	{
		if (!cache_file_header_verify(&map_file->header, path, FALSE))
		{
			memset(&map_file->header, 0, sizeof(struct cache_file_header));
			memset(&map_file->last_modification_date, 0, sizeof(FILETIME));
		}
	}
	else
	{
		match_vassert(
			"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
			1121,
			FALSE,
			csprintf(temporary, "couldn't read header from cache file (#%d)", GetLastError()));
		cached_map_file_invalidate(map_file_index);
	}

	return;
}

static struct cache_file_request *cache_request_get(
	short request_index)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		614,
		request_index>=0 && request_index<MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS);

	return cache_file_globals.requests + request_index;
}

static short cache_request_next_free_index(
	void)
{
	boolean out_of_requests = FALSE;

	while (TRUE)
	{
		short request_index;

		for (request_index = 0;
			request_index < MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS;
			request_index++)
		{
			if (!cache_request_get(request_index)->pending)
			{
				return request_index;
			}
		}

		/* every request slot is busy: remember that we blocked and scan again
		until the elevator thread retires one of them */
		if (!out_of_requests)
		{
			out_of_requests = TRUE;
		}
	}

	return NONE;
}

static void cache_file_windows_thread_wake(
	void)
{
	SetEvent(cache_file_globals.sleep_event);

	return;
}

static void cache_requests_flush(
	void)
{
	short request_index;

	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		651,
		cache_file_globals.open_map_file_index!=NONE);
	for (request_index = 0;
		request_index < MAXIMUM_SIMULTANEOUS_CACHE_REQUESTS;
		request_index++)
	{
		volatile struct cache_file_request *request = cache_request_get(request_index);

		while (request->pending)
			;
	}

	return;
}

static struct cached_map_file *cached_map_file_get(
	short map_file_index)
{
	match_assert(
		"c:\\halo\\SOURCE\\cache\\cache_files_windows.c",
		1208,
		map_file_index>=0 && map_file_index<NUMBER_OF_CACHED_MAP_FILES);

	return cache_file_globals.cached_map_files + map_file_index;
}

static void cached_map_file_invalidate(
	short map_file_index)
{
	struct cached_map_file *map_file = cached_map_file_get(map_file_index);

	if (map_file->file != INVALID_HANDLE_VALUE)
	{
		CloseHandle(map_file->file);
	}
	map_file->file = INVALID_HANDLE_VALUE;

	return;
}

static short cached_map_files_find_map(
	const char *map_name)
{
	short map_file_index;

	for (map_file_index = 0;
		map_file_index < NUMBER_OF_CACHED_MAP_FILES;
		map_file_index++)
	{
		if (_stricmp(
			map_name,
			cached_map_file_get(map_file_index)->header.name) == 0)
		{
			return map_file_index;
		}
	}

	return NONE;
}

#if defined(HALO_XBOX_TARGET)
/* ---------- oxbox: shared files (docs/shared_maps.md)

A folder map can keep its textures' and sounds' data in shared files, named in its header, so maps that use the same
data store it once: its pack's (beside it, or in the folder above it: a pack's or a campaign's) and the stock one
(stock.shared: the stock maps' data, which the installer builds from the player's own disc; beside it, above it, or in
the game's maps folder). A shared file is a cache file like a map (a header, its data after it) with no tags. Stored
unpacked (stock.shared is, by the installer) it's read where it is, with no copy and no cache file: the campaign needs its
two big cache files for the level being left and the next. Packed, it's copied into a campaign cache file (278 MiB) as a
map is, and kept there while a map that draws on it is loaded or played. A texture's or sound's offset with its top bit
set is in a shared file, the next bit saying which (cache_file_read). */

#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001
#endif

enum
{
	SHARED_REFERENCE_OFFSET = 0x700,    /* in the map's header */
	SHARED_REFERENCE_SIGNATURE = 'shrd',
	SHARED_FILES = 2,                   /* the pack's, the stock one */
};

struct cache_file_shared_reference
{
	unsigned long signature;
	long version;                       /* 1: the pack's alone; 2: the stock one too */
	char name[32];                      /* the pack's shared file's, without .shared ("" for none, version 2) */
	unsigned long checksum;             /* its header's checksum */
	long length;                        /* its header's file_length */
	long standalone_length;             /* the map built without shared files: its file's size and its header's */
	unsigned long standalone_checksum;  /* checksum, the map's identity in map sharing (port/linux/game/hmp_share.c) */
	char stock_name[32];                /* version 2: the stock shared file's ("" for none) */
	unsigned long stock_checksum;
	long stock_length;
};

/* the last map looked at: what shared files it draws on and where they are */
static struct
{
	char map_name[64];
	unsigned long time;
	boolean has;
	struct
	{
		char key[32];                   /* "*name", its cache file's name; "" for none */
		char path[256];                 /* "" when it's nowhere */
		unsigned long checksum;
		long length;
		boolean unpacked;               /* its file is the right version, stored unpacked: read where it is */
	} files[SHARED_FILES];
} shared_lookup;

static void shared_lookup_forget(void)
{
	shared_lookup.map_name[0] = 0;
}

/* the shared files of the map being loaded: their cache files are kept for it */
static char shared_kept_keys[SHARED_FILES][32];

/* where a shared file is: beside the map, the folder above it, then (the stock one) the game's maps folder */
static void shared_find(
	char const *map_path,
	char const *name,
	boolean stock,
	char *found_path)
{
	char folder[256];
	char *slash;
	short k;

	found_path[0] = 0;
	csstrncpy(folder, map_path, sizeof(folder) - 1);
	folder[sizeof(folder) - 1] = 0;
	for (k = 0; k < 3; k++)
	{
		HANDLE found;

		if (k < 2)
		{
			slash = strrchr(folder, '\\');
			if (!slash)
				continue;
			*slash = 0;
		}
		else if (stock)
		{
			char const *maps = cache_files_map_directory();
			long length = (long)strlen(maps);

			if (length < 2 || length >= (long)sizeof(folder))
				break;
			strcpy(folder, maps);
			if (folder[length - 1] == '\\')
				folder[length - 1] = 0;
		}
		else
			break;
		if ((int)strlen(folder) + 1 + (int)strlen(name) + 8 >= 256)
			continue;
		sprintf(found_path, "%s\\%s.shared", folder, name);
		found = CreateFileA(found_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
		if (found != INVALID_HANDLE_VALUE)
		{
			CloseHandle(found);
			return;
		}
		found_path[0] = 0;
	}
}

/* whether the file at path is that version of a shared file, stored unpacked (its size is its length) */
static boolean shared_stored_unpacked(
	char const *path,
	unsigned long checksum,
	long length)
{
	struct cache_file_header header;
	unsigned long got = 0;
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	if ((long)GetFileSize(file, NULL) != length || !ReadFile(file, &header, sizeof(header), &got, NULL))
		got = 0;
	CloseHandle(file);
	return got == sizeof(header) && header.file_length == length && header.checksum == checksum;
}

/* a name a shared file may have: 1 to 30 characters, none a path's */
static boolean shared_name_valid(
	char const *name)
{
	short k;

	if (!memchr(name, 0, 32) || !name[0] || strlen(name) > 30)
		return FALSE;
	for (k = 0; name[k]; k++)
	{
		if (strchr("\\/:*?\"<>|", name[k]) || name[k] < ' ')
			return FALSE;
	}
	return TRUE;
}

/* a folder map's shared references, and where its shared files are (looked up again 2 seconds on) */
static boolean shared_reference_get(
	const char *map_name)
{
	extern boolean hmp_map_file_path(char const *level_name, char *path, long size);
	struct cache_file_header header;
	struct cache_file_shared_reference reference;
	char path[256];
	unsigned long now = system_milliseconds();
	unsigned long got = 0;
	HANDLE file;
	short k;

	if (!map_name || !map_name[0] || map_name[0] == '*')
		return FALSE;
	if (!_stricmp(shared_lookup.map_name, map_name) && now - shared_lookup.time < 2000)
		return shared_lookup.has;
	csmemset(&shared_lookup, 0, sizeof(shared_lookup));
	csstrncpy(shared_lookup.map_name, map_name, sizeof(shared_lookup.map_name) - 1);
	shared_lookup.time = now;
	/* (a folder map's file, or the maps folder's: the stock maps drawing on stock.shared) */
	if (map_name[0] == '+')
	{
		if (!hmp_map_file_path(map_name, path, sizeof(path)))
			return FALSE;
	}
	else
	{
		cache_file_get_map_path(tag_name_strip_path(map_name), path);
		if (!path[0])
			return FALSE;
	}
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	if (!ReadFile(file, &header, sizeof(header), &got, NULL))
		got = 0;
	CloseHandle(file);
	if (got != sizeof(header))
		return FALSE;
	csmemcpy(&reference, (byte const *)&header + SHARED_REFERENCE_OFFSET, sizeof(reference));
	if (reference.signature != SHARED_REFERENCE_SIGNATURE || (reference.version != 1 && reference.version != 2))
		return FALSE;
	if (reference.version == 1)
		csmemset(reference.stock_name, 0, sizeof(reference.stock_name));
	for (k = 0; k < SHARED_FILES; k++)
	{
		char const *name = k ? reference.stock_name : reference.name;

		if (!name[0] && memchr(name, 0, 32))
			continue;
		if (!shared_name_valid(name))
			return FALSE;
		shared_lookup.files[k].key[0] = '*';
		csstrncpy(shared_lookup.files[k].key + 1, name, sizeof(shared_lookup.files[k].key) - 2);
		shared_lookup.files[k].checksum = k ? reference.stock_checksum : reference.checksum;
		shared_lookup.files[k].length = k ? reference.stock_length : reference.length;
		shared_find(path, name, (boolean)k, shared_lookup.files[k].path);
		if (!shared_lookup.files[k].path[0])
		{
			platform_log("shared: %s draws on %s.shared, which is neither beside it nor in the folder above%s", map_name,
				name, k ? " nor in the maps folder" : "");
		}
		else
		{
			shared_lookup.files[k].unpacked = shared_stored_unpacked(shared_lookup.files[k].path,
				shared_lookup.files[k].checksum, shared_lookup.files[k].length);
		}
		shared_lookup.has = TRUE;
	}
	return shared_lookup.has;
}

static boolean shared_file_path(
	const char *key,
	char *path)
{
	short k;

	for (k = 0; shared_lookup.has && k < SHARED_FILES; k++)
	{
		if (shared_lookup.files[k].key[0] && !_stricmp(key, shared_lookup.files[k].key) && shared_lookup.files[k].path[0])
		{
			strcpy(path, shared_lookup.files[k].path);
			return TRUE;
		}
	}
	return FALSE;
}

/* the cache file holding a shared file the looked-up map draws on, the right version, or NONE */
static short shared_cached(
	short k)
{
	short map_file_index = cached_map_files_find_map(shared_lookup.files[k].key);
	struct cached_map_file *map_file;

	if (map_file_index == NONE)
		return NONE;
	map_file = cached_map_file_get(map_file_index);
	if (map_file->header.checksum == shared_lookup.files[k].checksum &&
		map_file->header.file_length == shared_lookup.files[k].length)
	{
		return map_file_index;
	}
	/* (another version: copied again) */
	if (map_file_index != cache_file_globals.open_map_file_index && !shared_slot_kept(map_file_index) &&
		map_file_index != cache_file_globals.copying_to_map_file_index)
	{
		platform_log("shared: %s is cached in another version: copied again", shared_lookup.files[k].key + 1);
		map_file->header.name[0] = 0;
	}
	return NONE;
}

/* TRUE if a map draws on a shared file that isn't cached yet (its cache file name in key[32]): it's precached first
(cache_files.c). The shared files a map asked about draws on keep their cache files */
boolean cache_files_shared_wanted(
	const char *map_name,
	char *key)
{
	short k;

	if (!shared_reference_get(map_name))
	{
		/* (a map that draws on none: the shared files kept for an earlier one may go) */
		if (map_name && cached_map_files_find_map(tag_name_strip_path(map_name)) != NONE)
			csmemset(shared_kept_keys, 0, sizeof(shared_kept_keys));
		return FALSE;
	}
	for (k = 0; k < SHARED_FILES; k++)
	{
		/* (one read where it is needs no cache file kept for it) */
		if (shared_lookup.files[k].unpacked)
			shared_kept_keys[k][0] = 0;
		else
			csstrncpy(shared_kept_keys[k], shared_lookup.files[k].key, sizeof(shared_kept_keys[k]) - 1);
	}
	for (k = 0; k < SHARED_FILES; k++)
	{
		if (shared_lookup.files[k].key[0] && !shared_lookup.files[k].unpacked && shared_cached(k) == NONE)
		{
			if (key)
				csstrncpy(key, shared_lookup.files[k].key, 31);
			return TRUE;
		}
	}
	return FALSE;
}

/* what the last map opened drew on, let go (its own handles: a cache file's stays open) */
static void shared_open_close(
	void)
{
	short k;

	for (k = 0; k < SHARED_FILES; k++)
	{
		if (shared_open_file[k] != INVALID_HANDLE_VALUE && shared_open_map_file_index[k] == NONE)
			CloseHandle(shared_open_file[k]);
		shared_open_file[k] = INVALID_HANDLE_VALUE;
		shared_open_map_file_index[k] = NONE;
		shared_open_length[k] = 0;
	}
}

/* a map opened: the shared files it draws on, each read where it is (stored unpacked) or from its cache file. FALSE: it
draws on one that can't be read */
static boolean cache_files_shared_open(
	const char *map_name)
{
	short k;

	shared_open_close();
	if (!shared_reference_get(map_name))
		return TRUE;
	for (k = 0; k < SHARED_FILES; k++)
	{
		if (!shared_lookup.files[k].key[0])
			continue;
		if (shared_lookup.files[k].unpacked)
		{
			shared_open_file[k] = CreateFileA(shared_lookup.files[k].path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
				FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED, NULL);
			if (shared_open_file[k] != INVALID_HANDLE_VALUE)
			{
				shared_open_length[k] = shared_lookup.files[k].length;
				platform_log("shared: %s opened with %s.shared (read where it is)", map_name, shared_lookup.files[k].key + 1);
				continue;
			}
		}
		else
		{
			shared_open_map_file_index[k] = shared_cached(k);
			if (shared_open_map_file_index[k] != NONE)
			{
				shared_open_file[k] = cached_map_file_get_handle(shared_open_map_file_index[k]);
				shared_open_length[k] = cached_map_file_get(shared_open_map_file_index[k])->header.file_length;
				platform_log("shared: %s opened with %s.shared (cache file %d)", map_name, shared_lookup.files[k].key + 1,
					(int)shared_open_map_file_index[k]);
				continue;
			}
		}
		error(_error_silent, "the map '%s' draws on %s.shared, which can't be read", map_name, shared_lookup.files[k].key + 1);
		platform_log("shared: %s draws on %s.shared, which can't be read: not opened", map_name, shared_lookup.files[k].key + 1);
		shared_open_close();
		return FALSE;
	}
	return TRUE;
}

/* a cache file not to copy another map into: a shared file of the map open, or of the map being loaded */
static boolean shared_slot_kept(
	short map_file_index)
{
	char const *name = cached_map_file_get(map_file_index)->header.name;
	short k;

	for (k = 0; k < SHARED_FILES; k++)
	{
		if (map_file_index == shared_open_map_file_index[k])
			return TRUE;
		if (shared_kept_keys[k][0] && !_stricmp(name, shared_kept_keys[k]))
			return TRUE;
	}
	return FALSE;
}

/* ---------- oxbox: checking a folder map before it's loaded (port/linux/game/hmp_maps.c's map checks) */

#include "../../port/third_party/zlib/zlib_prefixed.h"
#ifndef FILE_SHARE_READ
#define FILE_SHARE_READ 0x00000001
#endif
#ifndef ERROR_SHARING_VIOLATION
#define ERROR_SHARING_VIOLATION 32L
#endif
#define CACHE_FILE_HEADER_SIGNATURE 'head'           /* (cache_files.c's) */
#define CACHE_FILE_FOOTER_SIGNATURE 'foot'

/* the quick check, from the map's header (2 KB): NULL when nothing's wrong, else what is. A map with a problem here
fails every time it's loaded (the damaged disc error, at the end of the lobby's countdown) */
char const *cache_files_map_problem(
	char const *map_name)
{
	struct cache_file_header header;
	char path[256];
	HANDLE file;
	unsigned long bytes_read = 0, size;

	cache_file_get_map_path(map_name, path);
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_SHARING_VIOLATION ? NULL : "its .map file can't be opened";
	size = GetFileSize(file, NULL);
	if (!ReadFile(file, &header, sizeof(header), &bytes_read, NULL))
		bytes_read = 0;
	CloseHandle(file);
	if (bytes_read != sizeof(header))
		return "its .map file is shorter than a map's header";
	if (!cache_file_header_verify(&header, path, FALSE))
		return header.header_signature == CACHE_FILE_HEADER_SIGNATURE && header.version != 5 ?
			"it's a map for another version of Halo" : "its .map file isn't a map (its header)";
	if (header.scenario_type != _scenario_type_multiplayer)
		return "it isn't a multiplayer map";
	if (size <= sizeof(header) + 64)
		return "its .map file is cut short (no map after the header)";
	if (cached_map_files_find_free_map(header.file_length, header.scenario_type) == NONE)
		return "it's too big for the Xbox's map cache";
	return NULL;
}

/* the full check (opence.ini map_check = 1; its own thread, hmp_maps.c): the whole map read and unpacked, as loading
it does, without writing it anywhere. work: 128 KB of memory for it. NULL when it unpacked whole, else what failed */
static unsigned char *cache_files_check_heap;
static unsigned long cache_files_check_heap_used, cache_files_check_heap_size;

static voidpf cache_files_check_alloc(voidpf opaque, uInt items, uInt size)
{
	unsigned long bytes = ((unsigned long)items * size + 15) & ~15ul;
	voidpf p;

	(void)opaque;
	if (cache_files_check_heap_used + bytes > cache_files_check_heap_size)
		return Z_NULL;
	p = cache_files_check_heap + cache_files_check_heap_used;
	cache_files_check_heap_used += bytes;
	return p;
}

static void cache_files_check_free(voidpf opaque, voidpf address)
{
	(void)opaque;
	(void)address;
}

char const *cache_files_map_full_check(
	char const *path,
	void *work,
	unsigned long work_size,
	volatile long const *stop)
{
	enum { IN_SIZE = 32 * 1024, OUT_SIZE = 32 * 1024 };
	unsigned char *in = (unsigned char *)work, *out = in + IN_SIZE;
	struct cache_file_header header;
	z_stream stream;
	HANDLE file;
	unsigned long bytes_read = 0, expected;
	char const *why = NULL;
	int result = Z_OK;

	if (work_size < IN_SIZE + OUT_SIZE + 48 * 1024)
		return "no memory to check it";
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return GetLastError() == ERROR_SHARING_VIOLATION ? "busy (being loaded): not checked" : "its .map file can't be opened";
	if (!ReadFile(file, &header, sizeof(header), &bytes_read, NULL) || bytes_read != sizeof(header) ||
		header.header_signature != CACHE_FILE_HEADER_SIGNATURE || header.footer_signature != CACHE_FILE_FOOTER_SIGNATURE ||
		header.file_length <= (long)sizeof(header))
	{
		CloseHandle(file);
		return "its .map file isn't a map (its header)";
	}
	expected = (unsigned long)header.file_length - sizeof(header);
	cache_files_check_heap = out + OUT_SIZE;
	cache_files_check_heap_size = work_size - IN_SIZE - OUT_SIZE;
	cache_files_check_heap_used = 0;
	memset(&stream, 0, sizeof(stream));
	stream.zalloc = cache_files_check_alloc;
	stream.zfree = cache_files_check_free;
	if (inflateInit(&stream) != Z_OK)
	{
		CloseHandle(file);
		return "no memory to check it";
	}
	while (result != Z_STREAM_END && !why)
	{
		if (stop && *stop)
		{
			why = "stopped";
			break;
		}
		if (!stream.avail_in)
		{
			if (!ReadFile(file, in, IN_SIZE, &bytes_read, NULL))
			{
				why = "a read of its .map file failed (a bad spot on the drive?)";
				break;
			}
			if (!bytes_read)
			{
				why = "its .map file ends before the map does (cut short: incomplete?)";
				break;
			}
			stream.next_in = in;
			stream.avail_in = (uInt)bytes_read;
		}
		stream.next_out = out;
		stream.avail_out = OUT_SIZE;
		result = inflate(&stream, Z_NO_FLUSH);
		Sleep(1);   /* (gives way to the game after each 32 KB it unpacks: the thread runs at the game's priority) */
		if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR)
			why = "its packed map data is damaged (it doesn't unpack)";
		else if (stream.total_out > expected)
			why = "it unpacks to more than its header says";
	}
	if (!why && stream.total_out != expected)
		why = "it unpacks to less than its header says (incomplete?)";
	inflateEnd(&stream);
	CloseHandle(file);
	return why;
}

/* the map file's path (hmp_maps.c's full check: worked out on Halo's thread) */
void cache_files_map_file_path(
	char const *map_name,
	char *path)
{
	cache_file_get_map_path(map_name, path);
}
#endif
