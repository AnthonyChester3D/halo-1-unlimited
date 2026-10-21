/*
PHYSICAL_MEMORY_MAP.C

symbols in this file:
001AD660 00f0:
	_physical_memory_allocate (0000)
001AD750 00a0:
	_physical_memory_verify (0000)
001AD7F0 0040:
	_physical_memory_free (0000)
001AD830 0010:
	_physical_memory_get_game_state_base_address (0000)
001AD840 0010:
	_physical_memory_get_tag_cache_base_address (0000)
001AD850 0010:
	_physical_memory_get_texture_cache_base_address (0000)
001AD860 0010:
	_physical_memory_get_sound_cache_base_address (0000)
002A76A0 0035:
	??_C@_0DF@JIMKNOMI@physical_memory_map_globals?4soun@ (0000)
002A76D8 0037:
	??_C@_0DH@HFDEEEDC@physical_memory_map_globals?4text@ (0000)
002A7710 005a:
	??_C@_0FK@BLCIEIND@?$CIunsigned?5long?$CJphysical_memory_m@ (0000)
002A7770 005c:
	??_C@_0FM@EHFFGLDK@?$CIunsigned?5long?$CJphysical_memory_m@ (0000)
002A77CC 002b:
	??_C@_0CL@BKHLOEEB@c?3?2halo?2SOURCE?2cache?2physical_me@ (0000)
002A77F8 001e:
	??_C@_0BO@DKDCAKMK@page_status?5?$DN?$DN?5PAGE_READWRITE?$AA@ (0000)
004D1074 0010:
	_physical_memory_map_globals (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries_windows.h"
#include "cache/physical_memory_map.h"

/* ---------- constants */

/* the native builds' larger game state, placed above the tag cache
(halo_port_capacity.h); the verified part is the CPU part, as on the Xbox */
#define GAME_STATE_BASE_ADDRESS HALO_PORT_GAME_STATE_BASE_ADDRESS
#define GAME_STATE_SIZE HALO_PORT_GAME_STATE_SIZE
#define GAME_STATE_VERIFY_SIZE HALO_PORT_GAME_STATE_CPU_SIZE
#define TAG_CACHE_BASE_ADDRESS 0x803A6000
/* port: the native builds' larger texture cache (halo_port_capacity.h) */
#define TEXTURE_CACHE_SIZE HALO_PORT_TEXTURE_CACHE_SIZE
/* port: and their larger sound cache (halo_port_capacity.h) */
#define SOUND_CACHE_SIZE HALO_PORT_SOUND_CACHE_SIZE

/* ---------- macros */

/* ---------- structures */

struct physical_memory_map_globals
{
	void *game_state_base_address;
	void *tag_cache_base_address;
	void *texture_cache_base_address;
	void *sound_cache_base_address;
};

/* ---------- prototypes */

/* ---------- globals */

static struct physical_memory_map_globals physical_memory_map_globals;

/* port: the caches' sizes (oxbox: chosen at start-up, physical_memory_allocate) */
static long physical_memory_texture_cache_size = TEXTURE_CACHE_SIZE;
static long physical_memory_sound_cache_size = SOUND_CACHE_SIZE;

/* ---------- public code */

void physical_memory_allocate(
	void)
{
	physical_memory_map_globals.game_state_base_address = XPhysicalAlloc(GAME_STATE_SIZE, GAME_STATE_BASE_ADDRESS & 0x7FFFFFFF, 0, PAGE_READWRITE);
#line 46 "c:\\halo\\SOURCE\\cache\\physical_memory_map.c"
	match_assert(__FILE__, __LINE__, (unsigned long)physical_memory_map_globals.game_state_base_address==GAME_STATE_BASE_ADDRESS);

	physical_memory_map_globals.tag_cache_base_address = XPhysicalAlloc(TAG_CACHE_SIZE, TAG_CACHE_BASE_ADDRESS & 0x7FFFFFFF, 0, PAGE_READWRITE);
#line 50 "c:\\halo\\SOURCE\\cache\\physical_memory_map.c"
	match_assert(__FILE__, __LINE__, (unsigned long)physical_memory_map_globals.tag_cache_base_address==TAG_CACHE_BASE_ADDRESS);

#if defined(HALO_XBOX_TARGET)
	/* oxbox: a console with 128 MB (a devkit, or an upgraded and modded retail Xbox) gives the texture cache
	most of its extra 64 MB and the sound cache 8 MB; a stock 64 MB console keeps the sizes the build was made
	for (halo_port_capacity.h). The cache needs one contiguous block: the largest that the console has, of
	60 MB down to 32 MB, 4 MB at a time (the top of memory holds the graphics chip's own tables) */
	{
		MEMORYSTATUS status;
		void platform_log(char const *format, ...);

		GlobalMemoryStatus(&status);
#if defined(OXBOX_TEST_TEXTURE_CACHE_KB)
		/* (test builds only: a smaller cache, the sizes' plumbing checked on a 64 MB console) */
		physical_memory_texture_cache_size = OXBOX_TEST_TEXTURE_CACHE_KB * 1024L;
#endif
#if defined(OXBOX_TEST_AS_128MB)
		/* (test builds only: the 128 MB attempts on a 64 MB console, which fail, and fall back) */
		status.dwTotalPhys = 128UL << 20;
#endif
		if (status.dwTotalPhys >= 100UL << 20)
		{
			long size;

			for (size = 0x3C00000; size >= 0x2000000 && !physical_memory_map_globals.texture_cache_base_address;
				size -= 0x400000)
			{
				physical_memory_map_globals.texture_cache_base_address =
					XPhysicalAlloc(size, -1, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
				if (physical_memory_map_globals.texture_cache_base_address)
					physical_memory_texture_cache_size = size;
			}
			physical_memory_map_globals.sound_cache_base_address = XPhysicalAlloc(0x800000, -1, 0, PAGE_READWRITE);
			if (physical_memory_map_globals.sound_cache_base_address)
				physical_memory_sound_cache_size = 0x800000;
		}
		if (!physical_memory_map_globals.texture_cache_base_address)
		{
			physical_memory_map_globals.texture_cache_base_address =
				XPhysicalAlloc(physical_memory_texture_cache_size, -1, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
		}
		if (!physical_memory_map_globals.sound_cache_base_address)
		{
			physical_memory_map_globals.sound_cache_base_address =
				XPhysicalAlloc(physical_memory_sound_cache_size, -1, 0, PAGE_READWRITE);
		}
		GlobalMemoryStatus(&status);
		platform_log("memory: %lu MB console: texture cache %ld KB, sound cache %ld KB, %lu KB free",
			(status.dwTotalPhys + (1UL << 20) - 1) >> 20, physical_memory_texture_cache_size >> 10,
			physical_memory_sound_cache_size >> 10, status.dwAvailPhys >> 10);
	}
#else
	physical_memory_map_globals.texture_cache_base_address = XPhysicalAlloc(physical_memory_texture_cache_size, -1, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
#endif
#line 55 "c:\\halo\\SOURCE\\cache\\physical_memory_map.c"
	match_assert(__FILE__, __LINE__, physical_memory_map_globals.texture_cache_base_address);

#if !defined(HALO_XBOX_TARGET)
	physical_memory_map_globals.sound_cache_base_address = XPhysicalAlloc(physical_memory_sound_cache_size, -1, 0, PAGE_READWRITE);
#endif
#line 58 "c:\\halo\\SOURCE\\cache\\physical_memory_map.c"
	match_assert(__FILE__, __LINE__, physical_memory_map_globals.sound_cache_base_address);

	return;
}

void physical_memory_verify(
	void)
{
	byte *address;
	unsigned long page_status;

	for (address = physical_memory_map_globals.tag_cache_base_address;
		address < (byte *)physical_memory_map_globals.tag_cache_base_address + TAG_CACHE_SIZE;
		address += 0x1000)
	{
		page_status = XQueryMemoryProtect(address);
#line 77 "c:\\halo\\SOURCE\\cache\\physical_memory_map.c"
		match_assert(__FILE__, __LINE__, page_status == PAGE_READWRITE);
	}

	for (address = physical_memory_map_globals.game_state_base_address;
		address < (byte *)physical_memory_map_globals.game_state_base_address + GAME_STATE_VERIFY_SIZE;
		address += 0x1000)
	{
		page_status = XQueryMemoryProtect(address);
#line 86 "c:\\halo\\SOURCE\\cache\\physical_memory_map.c"
		match_assert(__FILE__, __LINE__, page_status == PAGE_READWRITE);
	}

	return;
}

void physical_memory_free(
	void)
{
	if (physical_memory_map_globals.game_state_base_address)
		XPhysicalFree(physical_memory_map_globals.game_state_base_address);
	if (physical_memory_map_globals.tag_cache_base_address)
		XPhysicalFree(physical_memory_map_globals.tag_cache_base_address);
	if (physical_memory_map_globals.texture_cache_base_address)
		XPhysicalFree(physical_memory_map_globals.texture_cache_base_address);
	if (physical_memory_map_globals.sound_cache_base_address)
		XPhysicalFree(physical_memory_map_globals.sound_cache_base_address);

	return;
}

long physical_memory_get_texture_cache_size(
	void)
{
	return physical_memory_texture_cache_size;
}

long physical_memory_get_sound_cache_size(
	void)
{
	return physical_memory_sound_cache_size;
}

void *physical_memory_get_game_state_base_address(
	void)
{
	return physical_memory_map_globals.game_state_base_address;
}

void *physical_memory_get_tag_cache_base_address(
	void)
{
	return physical_memory_map_globals.tag_cache_base_address;
}

void *physical_memory_get_texture_cache_base_address(
	void)
{
	return physical_memory_map_globals.texture_cache_base_address;
}

void *physical_memory_get_sound_cache_base_address(
	void)
{
	return physical_memory_map_globals.sound_cache_base_address;
}

/* ---------- private code */
