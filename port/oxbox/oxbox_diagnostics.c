/* oxbox_diagnostics.c: what the log says for testers and developers, beyond the perf log (oxbox_perf.c)

- Limits: the first time in a map that one of the game state's pools is full (a datum it couldn't make: data.c),
  or the sound cache or the texture cache couldn't fit something, a "limit:" line says which, and where the
  player was. When the map ends, how many times each was full. Always on: nothing runs until a limit is hit.
- Bookmarks: both thumbsticks clicked together write a "bookmark N:" line (where, when, the frame rate, the memory
  free) and show "Bookmark N" on the screen, so what a tester saw can be found in the log. Always on: one look at
  the first controller's two buttons a frame.
- Pools (opence.ini log_pools = 1): every 10 seconds of a game, how full the game state's pools have been this
  map (saved games/game_state.c's game_state_report_pools). Off: nothing runs.
- Crashes: the call chain a crash left on the stack, as addresses of the game's code ("crash:" and "halt:" lines,
  oxbox_entry.c and main.c), to look up in the build's oxbox.elf (llvm-addr2line). */

#include "cseries.h"
#include "game/game.h"
#include "tag_files/tag_files.h"
#include "game/players.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "input/input.h"
#include "interface/hud_messaging.h"

#include <string.h>
#include <xtl.h>

void platform_log(char const *format, ...);
int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);

int oxbox_setting_log_pools = 0;   /* log_pools = 1: the pools' use every 10 seconds of a game */

/* ---------- where the player is */

/* the map, its BSP, the first player's place and how long the game has run: "a10, BSP 2, (12.3 -4.5 6.7), 312 s" */
static void oxbox_where(char *text, int size)
{
	char const *map = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "no map";
	char const *leaf = strrchr(map, '\\');
	long player_index = local_player_get_player_index(0);
	real_point3d origin;
	boolean placed = FALSE;

	if (player_index != NONE)
	{
		struct player_datum *player = player_get(player_index);

		if (player && player->unit_index != NONE)
		{
			object_get_origin(player->unit_index, &origin);
			placed = TRUE;
		}
	}
	if (placed)
		halo_linux_snprintf(text, size, "%s, BSP %d, (%.1f %.1f %.1f), %ld s", leaf ? leaf + 1 : map,
			(int)global_structure_bsp_index, origin.x, origin.y, origin.z, (long)(game_time_get() / 30));
	else
		halo_linux_snprintf(text, size, "%s, BSP %d, no player, %ld s", leaf ? leaf + 1 : map,
			(int)global_structure_bsp_index, (long)(game_time_get() / 30));
}

/* ---------- limits */

#define LIMIT_KINDS 24

static struct
{
	char name[32];
	long limit, times;
} oxbox_limits[LIMIT_KINDS];
static int oxbox_limit_count;

void oxbox_limit_hit(char const *what, long limit)
{
	int k;
	char where[128];

	for (k = 0; k < oxbox_limit_count && strcmp(oxbox_limits[k].name, what); k++)
		;
	if (k < oxbox_limit_count)
	{
		oxbox_limits[k].times++;
		return;
	}
	if (oxbox_limit_count == LIMIT_KINDS)
		return;
	strncpy(oxbox_limits[k].name, what, sizeof(oxbox_limits[k].name) - 1);
	oxbox_limits[k].limit = limit;
	oxbox_limits[k].times = 1;
	oxbox_limit_count++;
	oxbox_where(where, sizeof(where));
	if (limit > 0)
		platform_log("limit: %s is full (%ld): first at %s", what, limit, where);
	else
		platform_log("limit: %s is full: first at %s", what, where);
}

/* a map ended (or the next is starting): how many times each limit was hit in it, and a clean slate */
void oxbox_limits_new_map(void)
{
	int k;
	for (k = 0; k < oxbox_limit_count; k++)
		platform_log("limit: %s was full %ld time%s in that map", oxbox_limits[k].name, oxbox_limits[k].times,
			oxbox_limits[k].times == 1 ? "" : "s");
	oxbox_limit_count = 0;
}

/* ---------- bookmarks */

static void oxbox_bookmark(void)
{
	static int number;
	char where[128];
	MEMORYSTATUS memory;
	wchar_t message[32];
	int k;
	char ascii[32];

	number++;
	oxbox_where(where, sizeof(where));
	GlobalMemoryStatus(&memory);
	{
		extern float oxbox_diagnostics_fps;
		platform_log("bookmark %d: %s, %.1f frames a second, %lu KB free", number, where, oxbox_diagnostics_fps,
			(unsigned long)(memory.dwAvailPhys / 1024));
	}
	halo_linux_snprintf(ascii, sizeof(ascii), "Bookmark %d", number);
	for (k = 0; ascii[k] && k < 31; k++)
		message[k] = (wchar_t)ascii[k];
	message[k] = 0;
	if (game_in_progress() && local_player_get_player_index(0) != NONE)
		hud_print_message(0, message);
}

float oxbox_diagnostics_fps;

/* each frame (main.c): the bookmark buttons, the frame rate, and the pools' log when it's on */
void oxbox_diagnostics_frame(void)
{
	static unsigned long second_start, frames;
	unsigned long now = GetTickCount();
	struct gamepad_state const *pad = input_has_gamepad(0) ? input_get_gamepad_state(0) : NULL;

	frames++;
	if (now - second_start >= 1000)
	{
		oxbox_diagnostics_fps = second_start ? frames * 1000.0f / (float)(now - second_start) : 0.0f;
		second_start = now;
		frames = 0;
	}
	/* (both sticks in: when the second of them goes down) */
	if (pad && pad->buttons[_gamepad_binary_button_left_thumb] && pad->buttons[_gamepad_binary_button_right_thumb] &&
		(pad->buttons[_gamepad_binary_button_left_thumb] == 1 || pad->buttons[_gamepad_binary_button_right_thumb] == 1))
	{
		oxbox_bookmark();
	}
	if (oxbox_setting_log_pools)
	{
		void game_state_report_pools(void);

		game_state_report_pools();
	}
}

/* ---------- crashes */

/* the addresses of the game's code on the stack from start up to its base (the thread's: fs:[4]), the call chain a
crash or a halt left there (with some of the code's other addresses among them), a line at a time */
void oxbox_log_stack(char const *why, unsigned long const *start)
{
	extern char __oxbox_image_end[];
	unsigned long base, const *p;
	char line[200];
	int length = 0, found = 0;

	__asm__ volatile ("movl %%fs:4, %0" : "=r"(base));
	if (base <= (unsigned long)start || base - (unsigned long)start > 0x80000)
		base = (unsigned long)start + 0x4000;
	for (p = start; (unsigned long)p < base && found < 48; p++)
	{
		unsigned long v = *p;
		if (v < 0x11000 || v >= (unsigned long)__oxbox_image_end)
			continue;
		length += halo_linux_snprintf(line + length, sizeof(line) - length, " %08lx", v);
		found++;
		if (found % 12 == 0)
		{
			platform_log("%s: stack%s", why, line);
			length = 0;
		}
	}
	if (length)
		platform_log("%s: stack%s", why, line);
}
