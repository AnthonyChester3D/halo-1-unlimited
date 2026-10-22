/*
BOTS.C (oxbox)

Bots for system link games: players of the host's own machine at controller
indices no gamepad has (network_game_manager.h), which the host adds to the
game and drives with an action of its own each tick, as a person's gamepad
would (player_queues_new.c's update_server_next_update). Every machine plays
them as any other player of the host's: their units are the host's, sent to
the clients with the other players' (network_distributed.c).

The host keeps as many bots as it wants (bots_wanted), one change at a time,
while nothing holds them (network_game_server_bots_held: the lobby closed
while the game loads, a person waiting to join, a bot leaving to make room).
A person joining a full game takes a bot's place
(network_game_server_add_player_to_game).

The host picks a mode in the lobby with Y (bots_lobby_button): Off, Suggested
(up to the map's suggested most players: its card's, or 8), Max (16 players)
or Custom (a count of bots, RT more, LT fewer). The mode lasts from game to
game, but is never saved: it is Off again when the console starts. The lobby's
countdown bar says it on every machine (bots_render), from the settings
(NETWORK_GAME_BOT_MODE).

Milestone 1: the bots stand where they spawn. Test builds start in Custom
with OXBOX_EXTRA=-DOXBOX_TEST_BOTS=<count>.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"
#include "networking/network_client_manager.h"
#include "input/input.h"
#include "interface/ui_widget.h"
#include "text/draw_string.h"
#include "rasterizer/rasterizer.h"
#include "tag_files/tag_groups.h"
#include "objects/objects.h"
#include "units/units.h"
#include "bots.h"

#include <math.h>

/* ---------- constants */

enum
{
	/* between two changes to the bots (milliseconds): one at a time, and
	the settings each sends out first */
	BOTS_CHANGE_INTERVAL = 250,
	/* Suggested on a map that says no player count */
	BOTS_SUGGESTED_DEFAULT = 8,
	/* a network game client's lobby (network_game_globals.c's
	_network_game_client_state_pregame) */
	BOTS_CLIENT_STATE_PREGAME = 2,
};

enum bots_mode
{
	_bots_mode_off,
	_bots_mode_suggested,
	_bots_mode_max,
	_bots_mode_custom,
	NUMBER_OF_BOTS_MODES
};

static char const *const bots_mode_names[NUMBER_OF_BOTS_MODES] = { "Off", "Suggested", "Max", "Custom" };

/* ---------- globals */

static struct
{
	unsigned long last_change;
	short mode;
	short custom_count;
	char suggested_map[128];
	short suggested_players;
} bots_globals =
#if defined(OXBOX_TEST_BOTS)
	{ 0, _bots_mode_custom, OXBOX_TEST_BOTS };
#else
	{ 0, _bots_mode_off, 4 };
#endif

/* the map browser's (hmp_browse.c) and the lobby's (network_game_globals.c, hmp_share.c) */
#if defined(HALO_XBOX_TARGET)
short hmp_browse_suggested_players(char const *level_name);
boolean map_share_status_showing(void);
#endif
char const *network_game_current_map_name(void);
int halo_linux_snprintf(char *buffer, size_t count, char const *format, ...);

/* ---------- private code */

/* the most players the game's map suggests (remembered for the map) */
static short bots_suggested_players(
	void)
{
	char const *map = network_game_current_map_name();

	if (!map)
		return BOTS_SUGGESTED_DEFAULT;
	if (csstrncmp(map, bots_globals.suggested_map, sizeof(bots_globals.suggested_map) - 1))
	{
		csstrncpy(bots_globals.suggested_map, map, sizeof(bots_globals.suggested_map) - 1);
		bots_globals.suggested_map[sizeof(bots_globals.suggested_map) - 1] = 0;
#if defined(HALO_XBOX_TARGET)
		bots_globals.suggested_players = hmp_browse_suggested_players(map);
#else
		bots_globals.suggested_players = 0;
#endif
		if (bots_globals.suggested_players <= 0 || bots_globals.suggested_players > HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
			bots_globals.suggested_players = BOTS_SUGGESTED_DEFAULT;
	}
	return bots_globals.suggested_players;
}

/* how many bots the host wants, with this many people in the game */
static short bots_wanted(
	short people)
{
	short wanted = 0;

	switch (bots_globals.mode)
	{
	case _bots_mode_suggested:
		wanted = (short)(bots_suggested_players() - people);
		break;
	case _bots_mode_max:
		wanted = (short)(HALO_PORT_MAXIMUM_NETWORK_PLAYERS - people);
		break;
	case _bots_mode_custom:
		wanted = bots_globals.custom_count;
		break;
	}
	if (wanted > HALO_PORT_MAXIMUM_NETWORK_PLAYERS - people)
		wanted = (short)(HALO_PORT_MAXIMUM_NETWORK_PLAYERS - people);
	if (wanted > NETWORK_BOT_MAXIMUM_COUNT)
		wanted = NETWORK_BOT_MAXIMUM_COUNT;
	if (wanted < 0)
		wanted = 0;
	return wanted;
}

/* the player at a slot of the host's list (its datum), or NULL */
static struct player_datum *bots_player_at(
	long absolute_player_index)
{
	struct player_datum *player;

	if (!player_data || !player_data->valid || absolute_player_index < 0 ||
		absolute_player_index >= player_data->maximum_count)
	{
		return NULL;
	}
	player = (struct player_datum *)((byte *)player_data->data + absolute_player_index * player_data->size);
	return ((struct datum_header *)player)->identifier ? player : NULL;
}

#if defined(OXBOX_TEST_BOTS)
/* test builds: every 20 seconds of a game the host's first player kills a
bot, round the bots in turn; the log says when each bot spawns */
void damage_kill_object_for_player(long object_index, long player_index);

static void bots_test_update(
	void)
{
	static long next_kill_time = 0;
	static long last_units[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	static long kill_turn = 0;
	long index;

	if (!game_in_progress() || !game_engine_running() || !player_data || !player_data->valid)
	{
		next_kill_time = 0;
		return;
	}
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct player_datum *player = bots_player_at(index);
		long unit_index = player && player->network_player_data.controller_index >= NETWORK_BOT_CONTROLLER_FIRST ?
			player->unit_index : NONE;

		if (unit_index != last_units[index] && unit_index != NONE)
			error(_error_silent, "bots test: bot at slot %ld spawned (unit %08lx)", index, unit_index);
		last_units[index] = unit_index;
	}
	if (!next_kill_time)
		next_kill_time = game_time_get() + 20 * 30;
	if (game_time_get() >= next_kill_time)
	{
		long killer_index = local_player_get_player_index(0);

		next_kill_time = game_time_get() + 20 * 30;
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			long slot = (kill_turn + index) % HALO_PORT_MAXIMUM_NETWORK_PLAYERS;
			struct player_datum *player = bots_player_at(slot);

			if (player && player->network_player_data.controller_index >= NETWORK_BOT_CONTROLLER_FIRST &&
				player->unit_index != NONE)
			{
				error(_error_silent, "bots test: the host's player kills the bot at slot %ld", slot);
				damage_kill_object_for_player(player->unit_index, killer_index);
				kill_turn = slot + 1;
				break;
			}
		}
	}
}
#endif

/* ---------- public code */

/* the host, every frame */
void bots_update(
	void)
{
	unsigned long now;
	short people;
	short bots;
	short wanted;

	if (!global_network_game_server_get())
		return;
#if defined(OXBOX_TEST_BOTS)
	bots_test_update();
#endif
	network_game_server_set_bot_mode((short)(bots_globals.mode + 1));
	if (network_game_server_bots_held())
		return;
	now = system_milliseconds();
	if (bots_globals.last_change && now - bots_globals.last_change < BOTS_CHANGE_INTERVAL)
		return;
	bots = network_game_server_bot_count(&people);
	/* (the host's own player first: a lobby being set up has none yet) */
	if (!people)
		return;
	wanted = bots_wanted(people);
	if (bots < wanted)
	{
		if (network_game_server_add_bot())
			bots_globals.last_change = now;
	}
	else if (bots > wanted)
	{
		if (network_game_server_remove_bot(NONE))
			bots_globals.last_change = now;
	}
}

/* the bot to leave: one dead (waiting to respawn, so nobody sees a player
vanish), else the lowest score, else the last; NONE when there is none */
long bots_pick_leaver(
	struct network_player *players,
	long count)
{
	long best_index = NONE;
	boolean best_dead = FALSE;
	long best_score = 0;
	long index;

	for (index = 0; index < count; index++)
	{
		struct network_player *player = &players[index];
		struct player_datum *datum;
		boolean dead = FALSE;
		long score = 0;

		if (!network_player_is_bot(player))
			continue;
		datum = game_in_progress() ? bots_player_at(player->player_list_index) : NULL;
		if (datum)
		{
			long datum_index = DATUM_INDEX_NEW(player->player_list_index, ((struct datum_header *)datum)->identifier);

			dead = datum->unit_index == NONE;
			if (game_engine && game_engine->get_player_score)
				score = game_engine->get_player_score(datum_index, _get_score_individual);
		}
		if (best_index == NONE ||
			(dead && !best_dead) ||
			(dead == best_dead && score <= best_score))
		{
			best_index = index;
			best_dead = dead;
			best_score = score;
		}
	}
	return best_index;
}

/* a bot's action for the host's tick (FALSE: the player is no bot) */
boolean bots_player_action(
	long absolute_player_index,
	struct player_action *action)
{
	struct player_datum *player = bots_player_at(absolute_player_index);
	struct unit_datum *unit;

	if (!player || player->network_player_data.controller_index < NETWORK_BOT_CONTROLLER_FIRST)
		return FALSE;
	csmemset(action, 0, sizeof(*action));
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	/* (milestone 1: standing, facing as it spawned) */
	unit = player->unit_index != NONE ? (struct unit_datum *)object_try_and_get(player->unit_index) : NULL;
	if (unit)
	{
		real_vector3d const *facing = &unit->unit.desired_facing_vector;

		action->desired_facing.yaw = (real)atan2(facing->j, facing->i);
		if (action->desired_facing.yaw < 0.f)
			action->desired_facing.yaw += (real)(2.0 * 3.14159265358979323846);
		action->desired_facing.pitch = 0.f;
	}
	return TRUE;
}

/* a button on the lobby screen (the host's: Y the mode, LT / RT Custom's
count); FALSE leaves it to the screen */
boolean bots_lobby_button(
	short button)
{
	if (!network_game_server_bots_in_lobby())
		return FALSE;
	if (button == _gamepad_analog_button_y)
	{
		bots_globals.mode = (short)((bots_globals.mode + 1) % NUMBER_OF_BOTS_MODES);
		network_game_server_set_bot_mode((short)(bots_globals.mode + 1));
		ui_play_audio_feedback_sound(1);
		return TRUE;
	}
	if (bots_globals.mode == _bots_mode_custom &&
		(button == _gamepad_analog_button_left_trigger || button == _gamepad_analog_button_right_trigger))
	{
		short count = (short)(bots_globals.custom_count + (button == _gamepad_analog_button_right_trigger ? 1 : -1));

		if (count < 0 || count > NETWORK_BOT_MAXIMUM_COUNT)
		{
			ui_play_audio_feedback_sound(4);
			return TRUE;
		}
		bots_globals.custom_count = count;
		ui_play_audio_feedback_sound(1);
		return TRUE;
	}
	return FALSE;
}

/* the lobby's label, in its countdown bar on every machine: "Bots: Suggested
(6)", with the host's buttons on the host's ("Y: Bots ...") */
void bots_render(
	void)
{
	struct network_game_client *client = global_network_game_client_get();
	struct network_game *game;
	boolean host = global_network_game_server_get() != NULL;
	real_argb_color color = { 1.0f, 0.156863f, 0.588235f, 1.0f };
	rectangle2d bounds;
	char text[64];
	wchar_t wide[64];
	short mode;
	short bots = 0;
	long index;
	long font;

	if (!client || game_engine || network_game_client_get_state(client, NULL) != BOTS_CLIENT_STATE_PREGAME ||
		network_game_is_splitscreen_local() || !ui_widget_lobby_showing())
	{
		return;
	}
#if defined(HALO_XBOX_TARGET)
	if (map_share_status_showing())
		return;
#endif
	game = network_game_client_get_game(client);
	mode = host ? bots_globals.mode : (short)(NETWORK_GAME_BOT_MODE(game) - 1);
	if (mode < 0 || mode >= NUMBER_OF_BOTS_MODES || (!host && mode == _bots_mode_off))
		return;
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		if (network_player_is_bot(&game->players[index]))
			bots++;
	}
	if ((font = tag_loaded('font', "ui\\large_ui")) == NONE)
		return;
	if (mode == _bots_mode_off)
		halo_linux_snprintf(text, sizeof(text), "Y: Bots Off");
	else if (host && mode == _bots_mode_custom)
		halo_linux_snprintf(text, sizeof(text), "Y: Bots Custom (%d)  LT- RT+", (int)bots_globals.custom_count);
	else
		halo_linux_snprintf(text, sizeof(text), host ? "Y: Bots %s (%d)" : "Bots: %s (%d)", bots_mode_names[mode], (int)bots);
	for (index = 0; text[index] && index < (long)NUMBEROF(wide) - 1; index++)
		wide[index] = (wchar_t)(unsigned char)text[index];
	wide[index] = 0;
	draw_string_set_draw_mode(font, NONE, 1, 0, &color);
	draw_string_set_tab_stops(NULL, 0);
	bounds.y0 = 82;
	bounds.x0 = 250;
	bounds.y1 = 106;
	bounds.x1 = 565;
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, wide);
}
