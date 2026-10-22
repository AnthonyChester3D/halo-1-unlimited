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

Milestone 3 (test build 28): the bots walk the map (the movement section
below). Test builds start in Custom with OXBOX_EXTRA=-DOXBOX_TEST_BOTS=<count>
(and the bot log on); -DOXBOX_TEST_BOT_KILLS adds the host's player killing a
bot every 20 seconds.
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
#include "units/bipeds.h"
#include "units/biped_definitions.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "physics/collisions.h"
#include "physics/collision_bsp.h"
#include "physics/collision_usage.h"
#include "ai/path.h"
#include "game/game_allegiance.h"
#include "items/items.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
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

void damage_kill_object_for_player(long object_index, long player_index);

#if defined(OXBOX_TEST_BOT_KILLS)
/* test builds: every 20 seconds of a game the host's first player kills a
bot, round the bots in turn; the log says when each bot spawns */

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

/* ---------- movement (milestone 3)

The route layer: the map's own points (the multiplayer spawns, the netgame
equipment's spots, the netgame flags), each dropped onto the ground under it,
merged when closer than a world unit, and linked to its nearest neighbours. A
teleporter's way in also links to its way out: a bot whose route goes through
it walks onto it, and takes the next hop from where it comes out. A bot picks a destination (a waypoint nobody has been to
for a while), plans a route over the links (Dijkstra, from its 3 nearest
waypoints), and walks each hop between waypoints with Halo's AI path search
(path.c): a search gives the next few corners toward the hop's waypoint, or
toward the closest spot it found when the waypoint is past its 1,024 surfaces,
and the bot searches again when it has walked them.

Links start unknown. A hop walked marks its link good; a hop that fails (no
path, no progress, or a bot that gave up) counts against it, and twice makes it
unusable. What the layer learns lasts for the map (a next game on it keeps it).

Searches are the cost: one per tick at most, for all the bots, to the bot that
has waited longest. A bot keeps walking toward its last corner, or straight at
its waypoint, while it waits.

Getting unstuck: a bot that moves less than a world unit in 2 seconds jumps;
2 seconds later, sidesteps; then searches afresh; then gives up on its
destination and picks another; one that reaches no waypoint for 90 seconds is
trapped, and dies (to respawn). With bot_log = 1 in opence.ini, each give-up is
counted in opence_bot_log.txt beside oxbox_log.txt (map, spot, where the bot
was heading), and the host's log has a line every 10 seconds (searches,
arrivals, give-ups, waypoints visited, the bots' time). */

enum
{
	BOTS_WAYPOINT_MAXIMUM = 192,
	BOTS_LINK_MAXIMUM = 8,
	BOTS_LINK_MINIMUM = 2,           /* links a waypoint gets however far its neighbours are */
	BOTS_ROUTE_MAXIMUM = 40,
	BOTS_LINK_FAILURES_UNUSABLE = 2,
	BOTS_LINK_FAILURES_CERTAIN = 4,
	BOTS_EXPLORE_FAILURES_MAXIMUM = 2,      /* straight tries for a waypoint the links don't reach that failed:
	                                           no more tries for the fun of it */
	BOTS_TICKS_PER_SECOND = 30,
	BOTS_PROGRESS_TICKS = 2 * BOTS_TICKS_PER_SECOND,
	BOTS_SIDESTEP_TICKS = 20,
	BOTS_PLAN_RETRY_TICKS = BOTS_TICKS_PER_SECOND,
	BOTS_AVOID_TICKS = 10 * BOTS_TICKS_PER_SECOND,
	BOTS_AVOID_MAXIMUM = 4,
	BOTS_HOP_FAILED_RETRY_TICKS = BOTS_TICKS_PER_SECOND / 3,
	BOTS_TRAPPED_TICKS = 90 * BOTS_TICKS_PER_SECOND,     /* no waypoint reached in this long: trapped */
	BOTS_STUCK_SPOT_MAXIMUM = 48,
	BOTS_REPORT_MILLISECONDS = 10000,
	BOTS_FORGIVE_TICKS = 90 * BOTS_TICKS_PER_SECOND,   /* a link's failures count one fewer this often */
	BOTS_STUCK_LOG_MILLISECONDS = 10000,
};

#define BOTS_LINK_RANGE 40.f
#define BOTS_MERGE_DISTANCE 1.f
#define BOTS_PATHFINDING_RADIUS 0.25f
#define BOTS_ARRIVAL_DISTANCE 0.9f
#define BOTS_STEP_DISTANCE 0.45f
#define BOTS_TURN_PER_TICK 0.35f           /* radians (10 a second) */
#define BOTS_DESTINATION_MINIMUM 6.f        /* a destination's distance at least */
#define BOTS_PROGRESS_DISTANCE 1.f
#define BOTS_TELEPORT_DISTANCE 3.f          /* moved further in a tick: teleported (or respawned) */
#define BOTS_PI 3.14159265f
#define BOTS_HOP_SECONDS_BASE 5.f           /* a hop longer than this plus a second a world unit: given up */
#define BOTS_EXPLORE_RANGE 40.f             /* no route anywhere: straight for a waypoint this close */

struct bots_waypoint
{
	real_point3d point;
	long surface_index;
	long last_visit_time;                   /* game time, any bot's (or a bot heading there) */
	short link_count;
	short links[BOTS_LINK_MAXIMUM];
	byte link_good[BOTS_LINK_MAXIMUM];
	byte link_failures[BOTS_LINK_MAXIMUM];
	boolean visited;                        /* this game */
	byte explore_failures;                  /* straight tries for it that failed, this game */
	boolean teleport_source;                /* a teleporter's way in (walking onto it sends a player away) */
	boolean teleport_target;                /* a teleporter's way out (a pad both ways is both: a player who came
	                                           out on it walks off before it sends anyone back) */
	byte teleport_links;                    /* the links that are a teleporter's (a bit each): walk onto this
	                                           waypoint, come out at the link's */
};

struct bots_stuck_spot
{
	char map[32];
	real_point3d spot;
	real_point3d heading;
	long count;
};

static struct
{
	char map[64];
	long game_time;                         /* the last tick seen: going back is a new game */
	short waypoint_count;
	unsigned long random;
	long search_round;                      /* the round the last search ran in (one a tick, however many ticks a
	                                           frame takes) */
	long rounds;
	long forgive_time;
	long forgive_count;
	struct bots_waypoint waypoints[BOTS_WAYPOINT_MAXIMUM];

	/* the 10-second report (bot_log) */
	unsigned long report_time;
	long searches, searches_failed, arrivals, give_ups, teleports, ticks;
	long failed_no_path, failed_ledge, failed_stall, teleport_hops;
	long sightings, firing_ticks, grenades, melees, pickups;
	unsigned long long search_cycles_maximum, fight_cycles_maximum, walk_cycles_maximum;
	unsigned long long cycles, search_cycles, cycles_tick_maximum, cycles_tick;
	long round_last_index;                  /* a round of bots' actions: one tick's (several are built in a row
	                                           when a frame took several ticks: network_game_server_update_ticks) */
	short walking, walking_this_tick;

	/* where bots gave up (bot_log) */
	short stuck_spot_count;
	boolean stuck_log_dirty;
	unsigned long stuck_log_time;
	struct bots_stuck_spot stuck_spots[BOTS_STUCK_SPOT_MAXIMUM];
} bots_route;

struct bots_brain
{
	long unit_index;                        /* the unit this brain drives (another: a fresh brain) */
	long active_time;
	short destination;
	short from_waypoint;                    /* the hop's start (NONE: wherever the bot was) */
	short route[BOTS_ROUTE_MAXIMUM];
	short route_count;
	short route_position;
	long plan_retry_time;
	short avoid[BOTS_AVOID_MAXIMUM];        /* waypoints the bot keeps away from for a while (a hop that failed) */
	long avoid_until[BOTS_AVOID_MAXIMUM];
	short avoid_next;

	/* the hop: corners from the last search */
	boolean path_valid;
	boolean path_finishes;
	short step_count;
	short step_index;
	real_point3d steps[4];
	real hop_closest;                       /* the last search's closest approach to the waypoint */
	long hop_deadline;                      /* the hop's given up after this tick */
	short hop_stalls;
	boolean wants_search;
	long wants_search_since;

	/* progress and getting unstuck */
	real_point3d progress_point;
	real_point3d progress_target;           /* the corner or waypoint the bot was heading for, and how far */
	real progress_target_distance;
	long progress_time;
	short stuck_level;
	long sidestep_until;
	real sidestep_sign;
	boolean jump;
	real_point3d last_position;
	real yaw;                               /* where the bot faces (what its actions say) */
	real pitch;
	long arrival_time;                      /* the last arrival at a waypoint (or the brain's start) */

	/* this tick's walk (bots_walk): a direction in the world (length 1 at most), and the way it faces */
	real move_i, move_j;
	real walk_yaw;
	boolean walk_has_yaw;

	/* fighting (bots_fight) */
	short target;                           /* the enemy's slot (absolute player index), NONE for none */
	long target_unit;
	boolean target_visible;
	long target_seen_time;                  /* the last tick it was seen */
	long target_sighted_time;               /* when this sighting began: the reaction time counts from it */
	real_point3d target_position;           /* where it was last seen (its body's center) */
	long next_look_time;
	real aim_error_yaw, aim_error_pitch;    /* an aim that settles while the bot keeps aiming */
	boolean aimed;                          /* close enough to fire */
	short burst_ticks;                      /* the trigger held for this many more ticks */
	short pause_ticks;                      /* then released for this many */
	short trigger_ticks;                    /* how long it has been held */
	long grenade_time;                      /* none before this tick */
	long melee_time;
	long weapon_time;                       /* the next weapon choice */
	short wanted_weapon;                    /* the slot to switch to, NONE */
	real strafe_sign;
	long strafe_until;
	boolean chasing;                        /* heading where the target was last seen */

	/* a weapon on the ground worth picking up */
	long pickup_object;
	long pickup_until;
	long pickup_time;                       /* the next look for one */
	short action_ticks;
	long pickup_failed_object;              /* the last one it couldn't get, left alone for a while */
	long pickup_failed_until;
};

static struct bots_brain bots_brains[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];

#if defined(HALO_XBOX_TARGET)
extern int oxbox_setting_bot_log;
int oxbox_write_log_file(char const *name, void const *data, unsigned long size);
void platform_log(char const *format, ...);
#endif

#if defined(HALO_XBOX_TARGET)
#define bots_log platform_log
#else
#define bots_log(...) error(_error_silent, __VA_ARGS__)
#endif

static boolean bots_log_enabled(
	void)
{
#if defined(OXBOX_TEST_BOTS)
	return TRUE;
#elif defined(HALO_XBOX_TARGET)
	return oxbox_setting_bot_log != 0;
#else
	return FALSE;
#endif
}

static unsigned long long bots_cycles(
	void)
{
#if defined(HALO_XBOX_TARGET)
	unsigned long lo, hi;

	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
#else
	return 0;
#endif
}

/* the bots' own random numbers (the game's would change the host's sequence) */
static unsigned long bots_random(
	void)
{
	bots_route.random = bots_route.random * 1664525UL + 1013904223UL;
	return bots_route.random >> 8;
}

static real bots_distance2d(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = b->x - a->x, dy = b->y - a->y;

	return (real)sqrt(dx * dx + dy * dy);
}

static real bots_distance3d(
	real_point3d const *a,
	real_point3d const *b)
{
	real dx = b->x - a->x, dy = b->y - a->y, dz = b->z - a->z;

	return (real)sqrt(dx * dx + dy * dy + dz * dz);
}

/* how far for choosing nearby waypoints: a floor up or down counts extra */
static real bots_weighted_distance(
	real_point3d const *a,
	real_point3d const *b)
{
	return bots_distance2d(a, b) + 2.f * (real)fabs(b->z - a->z);
}

/* the ground under a point (within 3 world units), and its collision surface;
NONE when there is none */
static long bots_ground(
	real_point3d const *point,
	real_point3d *ground)
{
	struct collision_bsp *bsp = global_collision_bsp_get();
	struct collision_bsp_test_vector_result result;
	real_point3d origin;
	real_vector3d vector;
	long surface_index = NONE;

	if (!bsp || global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return NONE;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_bipeds;
	origin = *point;
	origin.z += 0.5f;
	vector.i = 0.f;
	vector.j = 0.f;
	vector.k = -3.5f;
	if (collision_bsp_test_vector(FLAG(_collision_test_front_facing_surfaces_bit), bsp, 0, NULL, &origin, &vector,
		REAL_MAX, &result))
	{
		surface_index = result.surface_index;
		ground->x = origin.x;
		ground->y = origin.y;
		ground->z = origin.z + vector.k * result.t;
	}
	--global_current_collision_user_depth;
	return surface_index;
}

/* a waypoint for a point of the map's (the one already there within a world unit, or a new one); NONE when
there is no ground under it, or no room */
static short bots_route_add(
	real_point3d const *point)
{
	real_point3d ground;
	long surface_index;
	short index;

	if ((surface_index = bots_ground(point, &ground)) == NONE)
		return NONE;
	for (index = 0; index < bots_route.waypoint_count; index++)
	{
		if (bots_distance3d(&bots_route.waypoints[index].point, &ground) < BOTS_MERGE_DISTANCE)
			return index;
	}
	if (bots_route.waypoint_count >= BOTS_WAYPOINT_MAXIMUM)
		return NONE;
	csmemset(&bots_route.waypoints[index], 0, sizeof(bots_route.waypoints[index]));
	bots_route.waypoints[index].point = ground;
	bots_route.waypoints[index].surface_index = surface_index;
	bots_route.waypoints[index].last_visit_time = -100000;
	bots_route.waypoint_count++;
	return index;
}

static short bots_link_index(
	short from,
	short to)
{
	struct bots_waypoint const *waypoint = &bots_route.waypoints[from];
	short index;

	for (index = 0; index < waypoint->link_count; index++)
	{
		if (waypoint->links[index] == to)
			return index;
	}
	return NONE;
}

static void bots_link_add(
	short from,
	short to)
{
	struct bots_waypoint *waypoint = &bots_route.waypoints[from];

	if (from != to && (!waypoint->teleport_source || waypoint->teleport_target) && waypoint->link_count < BOTS_LINK_MAXIMUM &&
		bots_link_index(from, to) == NONE)
	{
		waypoint->links[waypoint->link_count++] = to;
	}
}

/* a link a bot found by walking it: added, in place of an unusable one when the waypoint has no room */
static void bots_link_learn(
	short from,
	short to)
{
	struct bots_waypoint *waypoint = &bots_route.waypoints[from];
	short index;

	if (from == NONE || to == NONE || from == to || (waypoint->teleport_source && !waypoint->teleport_target) ||
		bots_link_index(from, to) != NONE)
		return;
	if (waypoint->link_count < BOTS_LINK_MAXIMUM)
	{
		index = waypoint->link_count++;
	}
	else
	{
		for (index = 0; index < waypoint->link_count; index++)
		{
			if (waypoint->link_failures[index] >= BOTS_LINK_FAILURES_UNUSABLE)
				break;
		}
		if (index == waypoint->link_count)
			return;
	}
	waypoint->links[index] = to;
	waypoint->link_good[index] = TRUE;
	waypoint->link_failures[index] = 0;
}

/* the map's waypoints and their links (no searches: links are tried as bots walk them) */
static void bots_route_build(
	char const *map)
{
	struct scenario *scenario = global_scenario_get();
	long index;
	short from;

	csstrncpy(bots_route.map, map, sizeof(bots_route.map) - 1);
	bots_route.map[sizeof(bots_route.map) - 1] = 0;
	bots_route.waypoint_count = 0;
	if (!scenario)
		return;
	/* the teleporters first (a teleporter's way in links to its way out: the game sends a player to the first
	way out on the same channel, the flags' team index; game_engine.c) */
	for (index = 0; index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag const *flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);
		long target_index;
		short source, target = NONE;

		if (flag->type != _netgame_flag_teleporter_source || (source = bots_route_add(&flag->position)) == NONE)
			continue;
		for (target_index = 0; target_index < scenario->netgame_flags.count; target_index++)
		{
			struct scenario_netgame_flag const *target_flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags,
				target_index, struct scenario_netgame_flag);

			if (target_flag->type == _netgame_flag_teleporter_target && target_flag->team_index == flag->team_index)
			{
				target = bots_route_add(&target_flag->position);
				break;
			}
		}
		if (target == NONE || target == source || bots_route.waypoints[source].link_count >= BOTS_LINK_MAXIMUM)
			continue;
		bots_route.waypoints[target].teleport_target = TRUE;
		bots_route.waypoints[source].teleport_links |= (byte)FLAG(bots_route.waypoints[source].link_count);
		bots_route.waypoints[source].links[bots_route.waypoints[source].link_count++] = target;
		bots_route.waypoints[source].teleport_source = TRUE;
#if defined(OXBOX_TEST_BOTS)
		bots_log("bots: teleporter %d: way in (%.1f %.1f %.1f) on the ground at (%.1f %.1f %.1f), way out at (%.1f %.1f %.1f)",
			(int)flag->team_index, flag->position.x, flag->position.y, flag->position.z,
			bots_route.waypoints[source].point.x, bots_route.waypoints[source].point.y, bots_route.waypoints[source].point.z,
			bots_route.waypoints[target].point.x, bots_route.waypoints[target].point.y, bots_route.waypoints[target].point.z);
#endif
	}
	for (index = 0; index < scenario->players.count; index++)
	{
		bots_route_add(&TAG_BLOCK_GET_ELEMENT(&scenario->players, index,
			struct encounter_player_starting_location)->position);
	}
	for (index = 0; index < scenario->netgame_equipment.count; index++)
	{
		bots_route_add(&TAG_BLOCK_GET_ELEMENT(&scenario->netgame_equipment, index,
			struct scenario_netgame_equipment)->position);
	}
	for (index = 0; index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag const *flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);

		if (flag->type != _netgame_flag_teleporter_source && flag->type != _netgame_flag_teleporter_target)
			bots_route_add(&flag->position);
	}
	/* each waypoint to its nearest (within the range, but at least 2), both ways */
	for (from = 0; from < bots_route.waypoint_count; from++)
	{
		short picked;

		for (picked = 0; picked < BOTS_LINK_MAXIMUM; picked++)
		{
			short best = NONE;
			real best_distance = REAL_MAX;
			short to;

			for (to = 0; to < bots_route.waypoint_count; to++)
			{
				real distance;

				if (to == from || bots_link_index(from, to) != NONE)
					continue;
				distance = bots_weighted_distance(&bots_route.waypoints[from].point, &bots_route.waypoints[to].point);
				if (distance < best_distance)
				{
					best = to;
					best_distance = distance;
				}
			}
			if (best == NONE || (picked >= BOTS_LINK_MINIMUM && best_distance > BOTS_LINK_RANGE))
				break;
			bots_link_add(from, best);
			bots_link_add(best, from);
		}
	}
	if (bots_log_enabled())
	{
		long links = 0, teleporters = 0;

		for (from = 0; from < bots_route.waypoint_count; from++)
		{
			links += bots_route.waypoints[from].link_count;
			teleporters += bots_route.waypoints[from].teleport_links ? 1 : 0;
		}
		bots_log("bots: %s: %d waypoints, %ld links (%ld through teleporters)", bots_route.map,
			bots_route.waypoint_count, links, teleporters);
	}
}

static void bots_brain_reset(
	struct bots_brain *brain,
	long unit_index)
{
	short index;

	csmemset(brain, 0, sizeof(*brain));
	brain->unit_index = unit_index;
	brain->destination = NONE;
	brain->from_waypoint = NONE;
	for (index = 0; index < BOTS_AVOID_MAXIMUM; index++)
		brain->avoid[index] = NONE;
	brain->hop_closest = REAL_MAX;
	brain->yaw = -1.f;
	brain->arrival_time = game_time_get();
	brain->target = NONE;
	brain->target_unit = NONE;
	brain->wanted_weapon = NONE;
	brain->pickup_object = NONE;
	brain->pickup_failed_object = NONE;
	brain->strafe_sign = 1.f;
}

static void bots_stuck_log_write(
	void);

static boolean bots_avoided(
	struct bots_brain const *brain,
	short waypoint,
	long now)
{
	short index;

	for (index = 0; index < BOTS_AVOID_MAXIMUM; index++)
	{
		if (brain->avoid[index] == waypoint && now < brain->avoid_until[index])
			return TRUE;
	}
	return FALSE;
}

static void bots_avoid(
	struct bots_brain *brain,
	short waypoint,
	long now)
{
	if (waypoint == NONE)
		return;
	brain->avoid[brain->avoid_next] = waypoint;
	brain->avoid_until[brain->avoid_next] = now + BOTS_AVOID_TICKS;
	brain->avoid_next = (short)((brain->avoid_next + 1) % BOTS_AVOID_MAXIMUM);
}

/* the route layer for this map and game: built for a new map, its visits
forgotten for a new game */
static void bots_route_update(
	void)
{
	char const *map = network_game_current_map_name();
	long now = game_time_get();
	long index;

	if (!map)
		map = "";
	if (csstrncmp(map, bots_route.map, sizeof(bots_route.map) - 1))
	{
		if (bots_route.stuck_log_dirty)
			bots_stuck_log_write();
		bots_route_build(map);
		bots_route.game_time = now;
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
			bots_brain_reset(&bots_brains[index], NONE);
	}
	else if (now < bots_route.game_time)
	{
		for (index = 0; index < bots_route.waypoint_count; index++)
		{
			bots_route.waypoints[index].last_visit_time = -100000;
			bots_route.waypoints[index].visited = FALSE;
			bots_route.waypoints[index].explore_failures = 0;
		}
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
			bots_brain_reset(&bots_brains[index], NONE);
	}
	bots_route.game_time = now;
	if (now - bots_route.forgive_time >= BOTS_FORGIVE_TICKS || now < bots_route.forgive_time)
	{
		/* (a link walked before that failed while bots crowded it gets another chance; one never walked, a third
		as often) */
		bots_route.forgive_time = now;
		bots_route.forgive_count++;
		for (index = 0; index < bots_route.waypoint_count; index++)
		{
			struct bots_waypoint *waypoint = &bots_route.waypoints[index];
			short link;

			for (link = 0; link < waypoint->link_count; link++)
			{
				if (waypoint->link_failures[link] && (waypoint->link_good[link] || !(bots_route.forgive_count % 3)))
					waypoint->link_failures[link]--;
			}
		}
	}
}

static void bots_link_result(
	short from,
	short to,
	boolean walked,
	boolean certain)
{
	short index;

	if (from == NONE || to == NONE || (index = bots_link_index(from, to)) == NONE)
		return;
	if (walked)
	{
		bots_route.waypoints[from].link_good[index] = TRUE;
		bots_route.waypoints[from].link_failures[index] = 0;
	}
	else if (certain)
	{
		/* (no path, or a ledge: no crowd made that one fail; it takes a few rounds of forgiving) */
		if (bots_route.waypoints[from].link_failures[index] < BOTS_LINK_FAILURES_CERTAIN)
			bots_route.waypoints[from].link_failures[index] = BOTS_LINK_FAILURES_CERTAIN;
	}
	else if (bots_route.waypoints[from].link_failures[index] < 255)
	{
		bots_route.waypoints[from].link_failures[index]++;
	}
}

/* how far every waypoint is from where the bot is, over the usable links (Dijkstra, from its 3 nearest
waypoints): cost REAL_MAX for one it can't reach, and each one's previous on the way */
static short bots_explore(
	struct bots_brain const *brain,
	real_point3d const *position,
	long now,
	real *cost,
	short *previous)
{
	short at = NONE;
	byte done[BOTS_WAYPOINT_MAXIMUM];
	short starts[3] = { NONE, NONE, NONE };
	real start_costs[3] = { REAL_MAX, REAL_MAX, REAL_MAX };
	short count = bots_route.waypoint_count;
	short index;

	for (index = 0; index < count; index++)
	{
		real distance;
		short slot;

		cost[index] = REAL_MAX;
		previous[index] = NONE;
		done[index] = FALSE;
		if (bots_avoided(brain, index, now))
			continue;
		if (bots_distance2d(position, &bots_route.waypoints[index].point) < BOTS_ARRIVAL_DISTANCE &&
			(real)fabs(position->z - bots_route.waypoints[index].point.z) < 1.5f)
		{
			at = index;
		}
		distance = bots_weighted_distance(position, &bots_route.waypoints[index].point);
		/* (another floor's comes after this floor's) */
		if ((real)fabs(position->z - bots_route.waypoints[index].point.z) > 1.5f)
			distance += 100.f;
		for (slot = 0; slot < 3; slot++)
		{
			if (distance < start_costs[slot])
			{
				short move;

				for (move = 2; move > slot; move--)
				{
					starts[move] = starts[move - 1];
					start_costs[move] = start_costs[move - 1];
				}
				starts[slot] = index;
				start_costs[slot] = distance;
				break;
			}
		}
	}
	if (at != NONE)
	{
		/* (on a waypoint: from it alone, so its links count what happens on the way) */
		cost[at] = 0.f;
	}
	else
	{
		for (index = 0; index < 3; index++)
		{
			if (starts[index] != NONE)
				cost[starts[index]] = start_costs[index];
		}
	}
	while (TRUE)
	{
		real best_cost = REAL_MAX;
		struct bots_waypoint const *waypoint;
		short node = NONE;
		short link;

		for (index = 0; index < count; index++)
		{
			if (!done[index] && cost[index] < best_cost)
			{
				best_cost = cost[index];
				node = index;
			}
		}
		if (node == NONE)
			break;
		done[node] = TRUE;
		waypoint = &bots_route.waypoints[node];
		for (link = 0; link < waypoint->link_count; link++)
		{
			short to = waypoint->links[link];
			real link_cost;

			if (waypoint->link_failures[link] >= BOTS_LINK_FAILURES_UNUSABLE || done[to])
				continue;
			link_cost = TEST_FLAG(waypoint->teleport_links, link) ? 1.f :
				bots_distance3d(&waypoint->point, &bots_route.waypoints[to].point);
			if (!waypoint->link_good[link])
				link_cost *= 1.25f;
			if (waypoint->link_failures[link])
				link_cost *= 3.f;
			if (best_cost + link_cost < cost[to])
			{
				cost[to] = best_cost + link_cost;
				previous[to] = node;
			}
		}
	}
	return at;
}

/* a destination the bot can reach that nobody has been to for a while (with a little chance), and the route
there; FALSE when there is none */
static boolean bots_pick_destination(
	struct bots_brain *brain,
	real_point3d const *position,
	long now)
{
	real cost[BOTS_WAYPOINT_MAXIMUM];
	short previous[BOTS_WAYPOINT_MAXIMUM];
	short route[BOTS_WAYPOINT_MAXIMUM];
	short best = NONE;
	long best_score = 0;
	short length = 0;
	short index;
	short node;
	short at;

	at = bots_explore(brain, position, now, cost, previous);
	for (index = 0; index < bots_route.waypoint_count; index++)
	{
		struct bots_waypoint const *waypoint = &bots_route.waypoints[index];
		long score;

		if (cost[index] == REAL_MAX || waypoint->teleport_source ||
			bots_distance2d(position, &waypoint->point) < BOTS_DESTINATION_MINIMUM ||
			bots_avoided(brain, index, now))
		{
			continue;
		}
		score = now - waypoint->last_visit_time + (long)(bots_random() % (20 * BOTS_TICKS_PER_SECOND));
		if (best == NONE || score > best_score)
		{
			best = index;
			best_score = score;
		}
	}
	{
		/* waypoints the links don't reach yet: straight for one not too far now and then, or when nothing else is
		left, by Halo's search alone (it knows the ramps and stairs the links don't); a hop walked from a waypoint
		becomes a link */
		short from = NONE;
		short explore = NONE;
		short candidates = 0;

		for (index = 0; index < bots_route.waypoint_count; index++)
		{
			real distance = bots_distance2d(position, &bots_route.waypoints[index].point);

			if (distance < 2.f * BOTS_ARRIVAL_DISTANCE && (real)fabs(position->z - bots_route.waypoints[index].point.z) < 1.5f)
				from = index;
			if (cost[index] != REAL_MAX || bots_route.waypoints[index].teleport_source ||
				(best != NONE && (bots_route.waypoints[index].visited ||
					bots_route.waypoints[index].explore_failures >= BOTS_EXPLORE_FAILURES_MAXIMUM)) ||
				distance < BOTS_DESTINATION_MINIMUM || distance > BOTS_EXPLORE_RANGE ||
				bots_avoided(brain, index, now))
			{
				continue;
			}
			/* (one of them at random) */
			if (bots_random() % (unsigned long)(++candidates) == 0)
				explore = index;
		}
		if (explore != NONE && (best == NONE || bots_random() % 4 == 0))
		{
			brain->route[0] = explore;
			brain->route_count = 1;
			brain->route_position = 0;
			brain->destination = explore;
			brain->from_waypoint = from;
			brain->path_valid = FALSE;
			brain->wants_search = FALSE;
			brain->hop_closest = REAL_MAX;
			brain->hop_stalls = 0;
			brain->hop_deadline = 0;
			return TRUE;
		}
		if (best == NONE)
			return FALSE;
	}
	/* (others look elsewhere: a bot is heading there) */
	bots_route.waypoints[best].last_visit_time = now;
	for (node = best; node != NONE && length < bots_route.waypoint_count; node = previous[node])
		route[length++] = node;
	brain->route_count = 0;
	for (index = length - 1; index >= 0 && brain->route_count < BOTS_ROUTE_MAXIMUM; index--)
		brain->route[brain->route_count++] = route[index];
	brain->route_position = 0;
	brain->destination = best;
	brain->from_waypoint = at;
	brain->path_valid = FALSE;
	brain->wants_search = FALSE;
	brain->hop_closest = REAL_MAX;
	brain->hop_stalls = 0;
	brain->hop_deadline = 0;
	return TRUE;
}

/* the hop failed: its link counts it, and the bot plans again from where it is */
static void bots_hop_failed(
	struct bots_brain *brain,
	long now,
	boolean certain)
{
	short target = brain->route_position < brain->route_count ? brain->route[brain->route_position] : NONE;
	struct bots_waypoint const *from = brain->from_waypoint != NONE ? &bots_route.waypoints[brain->from_waypoint] : NULL;

	/* (the link's fault only when the bot was still near its start: one that sidestepped off a ledge, or got
	pushed somewhere, had another hop than the link's) */
	if (from && bots_link_index(brain->from_waypoint, target) != NONE &&
		bots_distance2d(&brain->last_position, &from->point) < 3.f && (real)fabs(brain->last_position.z - from->point.z) < 1.5f)
	{
		bots_link_result(brain->from_waypoint, target, FALSE, certain);
	}
	else if (target != NONE)
	{
		/* (no link: the first hop, from wherever the bot was, or one straight for a waypoint the links don't reach;
		another for a while) */
		bots_avoid(brain, target, now);
		if (brain->route_count == 1 && bots_route.waypoints[target].explore_failures < 255)
			bots_route.waypoints[target].explore_failures++;
	}
	brain->route_count = 0;
	brain->path_valid = FALSE;
	brain->wants_search = FALSE;
	/* (not straight away: a bot somewhere its nearest waypoints are behind walls would spend every tick's search) */
	if (brain->plan_retry_time < now + BOTS_HOP_FAILED_RETRY_TICKS)
		brain->plan_retry_time = now + BOTS_HOP_FAILED_RETRY_TICKS;
}

/* a path search for the hop to the bot's waypoint */
static void bots_search(
	struct bots_brain *brain,
	long unit_index,
	long now)
{
	struct bots_waypoint const *waypoint = &bots_route.waypoints[brain->route[brain->route_position]];
	struct path_input input;
	struct path_state state;
	struct path_result result;
	real_point3d start;
	long start_surface_index = biped_find_pathfinding_surface_index(unit_index, &start);
	unsigned long long cycles = bots_cycles();
	real closest;
	boolean found;
	short index;

	brain->wants_search = FALSE;
	if (start_surface_index == NONE)
	{
		/* (in the air, or off the walkable surfaces: straight at the waypoint, and again a little later) */
		brain->path_valid = FALSE;
		return;
	}
	bots_route.search_round = bots_route.rounds;
	bots_route.searches++;
	{
		/* (the biped's own collision radius, and a little room) */
		struct biped_datum *biped = biped_get(unit_index);
		real radius = biped_definition_get(biped->definition_index)->biped.collision_radius + 0.05f;

		if (!(radius >= BOTS_PATHFINDING_RADIUS && radius < 1.f))
			radius = BOTS_PATHFINDING_RADIUS;
		path_input_new(&input, radius, FALSE, unit_index);
	}
	/* (no further than twice the way, and 60 world units: a waypoint it can't reach doesn't search the whole map) */
	{
		real bound = 2.f * bots_distance3d(&start, &waypoint->point) + 10.f;

		path_input_set_search_bounds(&input, bound < 60.f ? bound : 60.f);
	}
	{
		/* the worst spot where bots gave up before near the way there: a cost for the search to go around */
		struct bots_stuck_spot const *hazard = NULL;
		real hazard_score = 0.f;
		real segment_i = waypoint->point.x - start.x, segment_j = waypoint->point.y - start.y;
		real segment_length_squared = segment_i * segment_i + segment_j * segment_j;

		for (index = 0; index < bots_route.stuck_spot_count; index++)
		{
			struct bots_stuck_spot const *spot = &bots_route.stuck_spots[index];
			real t = 0.f, di, dj, distance, score;

			if (spot->count < 2 || (real)fabs(spot->spot.z - start.z) > 3.f ||
				csstrncmp(spot->map, bots_route.map, sizeof(spot->map) - 1))
			{
				continue;
			}
			if (segment_length_squared > 0.01f)
			{
				t = ((spot->spot.x - start.x) * segment_i + (spot->spot.y - start.y) * segment_j) / segment_length_squared;
				t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
			}
			di = start.x + segment_i * t - spot->spot.x;
			dj = start.y + segment_j * t - spot->spot.y;
			distance = (real)sqrt(di * di + dj * dj);
			if (distance > 4.f)
				continue;
			score = (real)spot->count * (4.f - distance);
			if (score > hazard_score)
			{
				hazard = spot;
				hazard_score = score;
			}
		}
		if (hazard)
			path_input_set_attractor(&input, &hazard->spot, 2.f, NONE, 10.f);
	}
	path_input_set_start(&input, &start, start_surface_index);
	path_state_new(&input, &state, NULL);
	/* (any distance: a waypoint past the search's surfaces gives the way to the closest spot it found) */
	path_state_destination(&state, &waypoint->point, waypoint->surface_index, 1000.f);
	found = path_state_find(&state) && path_state_build_path(&state, &result) && result.valid && result.step_count > 0;
#if defined(OXBOX_TEST_BOTS)
	if (!found)
		bots_route.failed_no_path++;
#endif
	cycles = bots_cycles() - cycles;
	bots_route.search_cycles += cycles;
	if (cycles > bots_route.search_cycles_maximum)
		bots_route.search_cycles_maximum = cycles;
	if (!found)
	{
		bots_route.searches_failed++;
		bots_hop_failed(brain, now, TRUE);
		return;
	}
	closest = result.endpoint.target_radius;
	if (closest > BOTS_ARRIVAL_DISTANCE && closest > bots_distance3d(&start, &waypoint->point) - 1.f)
	{
		/* (no closer than where the bot is: a wall, a floor it has no way to) */
		bots_route.searches_failed++;
#if defined(OXBOX_TEST_BOTS)
		bots_route.failed_stall++;
#endif
		bots_hop_failed(brain, now, TRUE);
		return;
	}
	if (closest > BOTS_ARRIVAL_DISTANCE &&
		bots_distance2d(&result.endpoint.point, &waypoint->point) < 2.f &&
		(real)fabs(result.endpoint.point.z - waypoint->point.z) > 1.5f)
	{
		/* (the closest it gets is under the waypoint, or over it: a ledge, a floor it has no way up or down to) */
		bots_route.searches_failed++;
#if defined(OXBOX_TEST_BOTS)
		bots_route.failed_ledge++;
#endif
		bots_hop_failed(brain, now, TRUE);
		return;
	}
	if (closest > BOTS_ARRIVAL_DISTANCE && closest > brain->hop_closest - 0.5f)
	{
		/* (no closer than the last search: twice is a dead end) */
		if (++brain->hop_stalls >= 2)
		{
			bots_route.searches_failed++;
#if defined(OXBOX_TEST_BOTS)
			bots_route.failed_stall++;
#endif
			bots_hop_failed(brain, now, FALSE);
			return;
		}
	}
	else
	{
		brain->hop_stalls = 0;
	}
	if (closest < brain->hop_closest)
		brain->hop_closest = closest;
	brain->path_valid = TRUE;
	brain->path_finishes = result.steps_finish_path;
	brain->step_count = result.step_count;
	if (brain->step_count > 4)
		brain->step_count = 4;
	for (index = 0; index < brain->step_count; index++)
		brain->steps[index] = result.steps[index].point;
	brain->step_index = 0;
}

static void bots_stuck_log_write(
	void)
{
#if defined(HALO_XBOX_TARGET)
	char text[BOTS_STUCK_SPOT_MAXIMUM * 96 + 256];
	byte written[BOTS_STUCK_SPOT_MAXIMUM];
	long length;
	short count;

	length = halo_linux_snprintf(text, sizeof(text),
		"OpenCE bots: spots where bots gave up and picked another destination, most first.\r\n"
		"(Spots are 2 world units apart. A world unit is about 3 m.)\r\n\r\n"
		"map                              times    spot (x y z)                heading for (x y z)\r\n");
	csmemset(written, 0, sizeof(written));
	for (count = 0; count < bots_route.stuck_spot_count; count++)
	{
		struct bots_stuck_spot const *spot = NULL;
		char const *leaf;
		short best = NONE;
		short index;

		for (index = 0; index < bots_route.stuck_spot_count; index++)
		{
			if (!written[index] && (best == NONE || bots_route.stuck_spots[index].count > bots_route.stuck_spots[best].count))
				best = index;
		}
		written[best] = TRUE;
		spot = &bots_route.stuck_spots[best];
		if (length < 0 || length >= (long)sizeof(text) - 100)
			break;
		/* (the map's name without its folders: levels\test\longest\longest is longest) */
		leaf = spot->map;
		for (index = 0; spot->map[index]; index++)
		{
			if (spot->map[index] == '\\')
				leaf = &spot->map[index + 1];
		}
		length += halo_linux_snprintf(text + length, sizeof(text) - length,
			"%-32s %5ld    %7.1f %7.1f %6.1f    %7.1f %7.1f %6.1f\r\n", leaf, spot->count,
			spot->spot.x, spot->spot.y, spot->spot.z, spot->heading.x, spot->heading.y, spot->heading.z);
	}
	if (length > 0)
		oxbox_write_log_file("opence_bot_log.txt", text, (unsigned long)length);
#endif
	bots_route.stuck_log_dirty = FALSE;
	bots_route.stuck_log_time = system_milliseconds();
}

/* a bot gave up here, heading there: kept as a spot searches go around (bots_search), and in the log with bot_log */
static void bots_stuck_log_add(
	real_point3d const *position,
	real_point3d const *heading)
{
	real_point3d spot;
	short index;

	spot.x = 2.f * (real)floor(position->x / 2.f + 0.5f);
	spot.y = 2.f * (real)floor(position->y / 2.f + 0.5f);
	spot.z = 2.f * (real)floor(position->z / 2.f + 0.5f);
	for (index = 0; index < bots_route.stuck_spot_count; index++)
	{
		struct bots_stuck_spot *entry = &bots_route.stuck_spots[index];

		if (!csstrncmp(entry->map, bots_route.map, sizeof(entry->map) - 1) && entry->spot.x == spot.x &&
			entry->spot.y == spot.y && entry->spot.z == spot.z)
		{
			entry->count++;
			entry->heading = *heading;
			break;
		}
	}
	if (index == bots_route.stuck_spot_count && index < BOTS_STUCK_SPOT_MAXIMUM)
	{
		struct bots_stuck_spot *entry = &bots_route.stuck_spots[bots_route.stuck_spot_count++];

		csstrncpy(entry->map, bots_route.map, sizeof(entry->map) - 1);
		entry->map[sizeof(entry->map) - 1] = 0;
		entry->spot = spot;
		entry->heading = *heading;
		entry->count = 1;
	}
	if (!bots_log_enabled())
		return;
	bots_log("bots: a bot gave up at (%.1f %.1f %.1f), heading for (%.1f %.1f %.1f)",
		position->x, position->y, position->z, heading->x, heading->y, heading->z);
	bots_route.stuck_log_dirty = TRUE;
}

/* the host's bot report, every 10 seconds (bot_log), and the stuck spots' file when they changed */
static void bots_report(
	void)
{
	unsigned long now = system_milliseconds();
	short visited = 0;
	short index;

	if (!bots_log_enabled())
		return;
	if (bots_route.stuck_log_dirty && now - bots_route.stuck_log_time >= BOTS_STUCK_LOG_MILLISECONDS)
		bots_stuck_log_write();
	if (!bots_route.report_time)
		bots_route.report_time = now;
	if (now - bots_route.report_time < BOTS_REPORT_MILLISECONDS)
		return;
	bots_route.report_time = now;
	if (!bots_route.ticks || !game_in_progress())
	{
		bots_route.searches = bots_route.searches_failed = bots_route.arrivals = bots_route.give_ups = 0;
		bots_route.teleports = 0;
		bots_route.ticks = 0;
		bots_route.cycles = bots_route.search_cycles = bots_route.cycles_tick_maximum = 0;
		return;
	}
	for (index = 0; index < bots_route.waypoint_count; index++)
		visited += bots_route.waypoints[index].visited ? 1 : 0;
	{
		long now_ticks = game_time_get();
		short planless = 0, waiting = 0, straight = 0, stuck = 0;
		long slowest = 0;

		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct bots_brain const *brain = &bots_brains[index];

			if (brain->unit_index == NONE || brain->active_time < now_ticks - BOTS_TICKS_PER_SECOND)
				continue;
			planless += brain->route_position >= brain->route_count ? 1 : 0;
			waiting += brain->wants_search ? 1 : 0;
			straight += !brain->path_valid && brain->route_position < brain->route_count ? 1 : 0;
			stuck += brain->stuck_level > 0 ? 1 : 0;
			if (now_ticks - brain->arrival_time > slowest)
				slowest = now_ticks - brain->arrival_time;
#if defined(OXBOX_TEST_BOTS)
			if (now_ticks - brain->arrival_time > 20 * BOTS_TICKS_PER_SECOND && brain->route_position >= brain->route_count)
			{
				bots_log("bots: slot %d: %ld s since a waypoint, at (%.1f %.1f %.1f) with nowhere to go", (int)index,
					(now_ticks - brain->arrival_time) / BOTS_TICKS_PER_SECOND,
					brain->last_position.x, brain->last_position.y, brain->last_position.z);
			}
			else if (now_ticks - brain->arrival_time > 20 * BOTS_TICKS_PER_SECOND)
			{
				struct bots_waypoint const *target = &bots_route.waypoints[brain->route[brain->route_position]];

				bots_log("bots: slot %d: %ld s since a waypoint, at (%.1f %.1f %.1f) heading for %d (%.1f %.1f %.1f), "
					"hop %d of %d, corners %d of %d%s, stuck %d", (int)index,
					(now_ticks - brain->arrival_time) / BOTS_TICKS_PER_SECOND,
					brain->last_position.x, brain->last_position.y, brain->last_position.z,
					(int)brain->route[brain->route_position], target->point.x, target->point.y, target->point.z,
					(int)brain->route_position + 1, (int)brain->route_count, (int)brain->step_index,
					(int)(brain->path_valid ? brain->step_count : 0), brain->path_finishes ? "" : " (more)",
					(int)brain->stuck_level);
			}
#endif
		}
		bots_log("bots 10s: %d planless, %d waiting for a search, %d without corners, %d stuck; "
			"longest since a waypoint %ld s", planless, waiting, straight, stuck, slowest / BOTS_TICKS_PER_SECOND);
		bots_log("bots 10s: fighting: %ld sightings, %ld ticks firing, %ld grenades, %ld melee, %ld weapon pickups; "
			"longest walk %.2f ms (search %.2f ms), longest fight %.2f ms",
			bots_route.sightings, bots_route.firing_ticks, bots_route.grenades, bots_route.melees, bots_route.pickups,
			(double)bots_route.walk_cycles_maximum / 733000.0, (double)bots_route.search_cycles_maximum / 733000.0,
			(double)bots_route.fight_cycles_maximum / 733000.0);
		bots_route.walk_cycles_maximum = bots_route.search_cycles_maximum = bots_route.fight_cycles_maximum = 0;
		bots_route.sightings = bots_route.firing_ticks = bots_route.grenades = bots_route.melees = bots_route.pickups = 0;
#if defined(OXBOX_TEST_BOTS)
		bots_log("bots 10s: failed searches: %ld no path, %ld ledges, %ld no closer; %ld bots stepped onto teleporters",
			bots_route.failed_no_path, bots_route.failed_ledge, bots_route.failed_stall, bots_route.teleport_hops);
		bots_route.failed_no_path = bots_route.failed_ledge = bots_route.failed_stall = bots_route.teleport_hops = 0;
#endif
	}
	/* (the Xbox's processor: 733 MHz) */
	bots_log("bots 10s: %d walking, %ld searches (%ld failed), %ld arrivals (%ld by teleporter), %ld give-ups; "
		"%d of %d waypoints visited this game; bots' time %.2f ms a tick (searches %.2f), at most %.2f ms",
		bots_route.walking, bots_route.searches, bots_route.searches_failed, bots_route.arrivals, bots_route.teleports,
		bots_route.give_ups,
		visited, bots_route.waypoint_count,
		(double)bots_route.cycles / 733000.0 / (double)bots_route.ticks,
		(double)bots_route.search_cycles / 733000.0 / (double)bots_route.ticks,
		(double)bots_route.cycles_tick_maximum / 733000.0);
	bots_route.searches = bots_route.searches_failed = bots_route.arrivals = bots_route.give_ups = 0;
	bots_route.teleports = 0;
	bots_route.ticks = 0;
	bots_route.cycles = bots_route.search_cycles = bots_route.cycles_tick_maximum = 0;
}

/* whether this bot has waited longest for a search (of the bots playing now) */
static boolean bots_search_turn(
	struct bots_brain const *brain,
	long now)
{
	long index;

	if (bots_route.search_round == bots_route.rounds)
		return FALSE;
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct bots_brain const *other = &bots_brains[index];

		if (other == brain || !other->wants_search || other->active_time < now - 1)
			continue;
		if (other->wants_search_since < brain->wants_search_since ||
			(other->wants_search_since == brain->wants_search_since && other < brain))
		{
			return FALSE;
		}
	}
	return TRUE;
}

static void bots_want_search(
	struct bots_brain *brain,
	long now)
{
	if (!brain->wants_search)
	{
		brain->wants_search = TRUE;
		brain->wants_search_since = now;
	}
}

/* whether the bot's hop is a teleporter's (from the way in it reached to the way out) */
static boolean bots_teleport_hop(
	struct bots_brain const *brain)
{
	short link;

	if (brain->from_waypoint == NONE || brain->route_position >= brain->route_count)
		return FALSE;
	link = bots_link_index(brain->from_waypoint, brain->route[brain->route_position]);
	return link != NONE && TEST_FLAG(bots_route.waypoints[brain->from_waypoint].teleport_links, link);
}

/* at the hop's waypoint: its link walked, and the next hop (FALSE: nowhere to go on to, for now) */
static boolean bots_arrive(
	struct bots_brain *brain,
	real_point3d const *position,
	long now)
{
	short reached = brain->route[brain->route_position];

	bots_link_result(brain->from_waypoint, reached, TRUE, FALSE);
	bots_link_learn(brain->from_waypoint, reached);
	bots_route.waypoints[reached].last_visit_time = now;
	bots_route.waypoints[reached].visited = TRUE;
	bots_route.arrivals++;
	brain->arrival_time = now;
	brain->from_waypoint = reached;
	brain->route_position++;
	brain->hop_deadline = 0;
#if defined(OXBOX_TEST_BOTS)
	if (bots_teleport_hop(brain))
		bots_route.teleport_hops++;
#endif
	brain->path_valid = FALSE;
	brain->wants_search = FALSE;
	brain->hop_closest = REAL_MAX;
	brain->hop_stalls = 0;
	brain->stuck_level = 0;
	brain->progress_point = *position;
	brain->progress_time = now;
	if (brain->route_position >= brain->route_count && !bots_pick_destination(brain, position, now))
	{
		brain->plan_retry_time = now + BOTS_PLAN_RETRY_TICKS;
		return FALSE;
	}
	return TRUE;
}

/* the bot's move for this tick: the direction to walk (a unit 2d vector, or zero) and the yaw to face */
static void bots_walk(
	struct bots_brain *brain,
	long absolute_player_index,
	long unit_index,
	struct player_action *action)
{
	long now = game_time_get();
	struct bots_waypoint const *waypoint;
	real_point3d position;
	real_point3d const *target;
	real_point3d const *hop_point;
	boolean teleported, teleport_hop;
	real move_i, move_j, length;
	real desired_yaw;
	long index;

	brain->move_i = brain->move_j = 0.f;
	brain->walk_has_yaw = FALSE;
	object_get_origin(unit_index, &position);
	if (brain->unit_index != unit_index)
	{
		bots_brain_reset(brain, unit_index);
		brain->last_position = position;
		brain->progress_point = position;
		brain->progress_time = now;
	}
	brain->active_time = now;
	if (brain->target_visible || brain->pickup_object != NONE)
	{
		/* (fighting, or walking to a weapon: not stuck, and the hop waits) */
		brain->progress_point = position;
		brain->progress_time = now;
		brain->stuck_level = 0;
		if (brain->hop_deadline)
			brain->hop_deadline++;
		if (brain->target_visible)
			brain->arrival_time = now;
	}
	teleported = bots_distance3d(&brain->last_position, &position) > BOTS_TELEPORT_DISTANCE;
	brain->last_position = position;
	if (teleported)
	{
		/* (through a teleporter on the route: at the way out, the next hop; any other, a route from here) */
		brain->path_valid = FALSE;
		brain->progress_point = position;
		brain->progress_time = now;
		if (!(brain->route_position < brain->route_count &&
			bots_distance2d(&position, &bots_route.waypoints[brain->route[brain->route_position]].point) < 2.5f))
		{
			brain->route_count = 0;
		}
		else
		{
			bots_route.teleports++;
		}
	}

	if (brain->route_position >= brain->route_count)
	{
		if (now < brain->plan_retry_time)
			return;
		if (!bots_pick_destination(brain, &position, now))
		{
			brain->plan_retry_time = now + BOTS_PLAN_RETRY_TICKS;
			return;
		}
		teleported = FALSE;
	}
	waypoint = &bots_route.waypoints[brain->route[brain->route_position]];

	/* at the hop's waypoint (or out of the hop's teleporter): the next hop */
	if (teleported ||
		(!bots_teleport_hop(brain) && bots_distance2d(&position, &waypoint->point) < BOTS_ARRIVAL_DISTANCE &&
			(real)fabs(position.z - waypoint->point.z) < 1.5f))
	{
		if (!bots_arrive(brain, &position, now))
			return;
		waypoint = &bots_route.waypoints[brain->route[brain->route_position]];
	}
	/* (a teleporter's hop: onto its way in, the waypoint the bot just reached) */
	teleport_hop = bots_teleport_hop(brain);
	hop_point = teleport_hop ? &bots_route.waypoints[brain->from_waypoint].point : &waypoint->point;

	/* a hop that takes too long (bots pushing each other, or a path the bot can't follow): given up */
	if (!brain->hop_deadline)
	{
		brain->hop_deadline = now + (long)((BOTS_HOP_SECONDS_BASE + bots_distance3d(&position, hop_point)) *
			BOTS_TICKS_PER_SECOND);
	}
	else if (now > brain->hop_deadline)
	{
		bots_route.give_ups++;
		bots_stuck_log_add(&position, hop_point);
		bots_hop_failed(brain, now, FALSE);
		bots_avoid(brain, brain->destination, now);
		brain->stuck_level = 0;
		brain->plan_retry_time = now + BOTS_PLAN_RETRY_TICKS / 2;
		return;
	}

	/* the corners: the next one, and a search when they run out */
	if (teleport_hop)
	{
		brain->path_valid = FALSE;
		brain->wants_search = FALSE;
	}
	else if (brain->path_valid)
	{
		while (brain->step_index < brain->step_count &&
			bots_distance2d(&position, &brain->steps[brain->step_index]) < BOTS_STEP_DISTANCE)
		{
			brain->step_index++;
		}
		if (brain->step_index >= brain->step_count)
		{
			brain->path_valid = FALSE;
			bots_want_search(brain, now);
		}
	}
	else
	{
		bots_want_search(brain, now);
	}
	if (brain->wants_search && bots_search_turn(brain, now))
	{
		bots_search(brain, unit_index, now);
		if (brain->route_position >= brain->route_count)
			return;
		waypoint = &bots_route.waypoints[brain->route[brain->route_position]];
		hop_point = &waypoint->point;
	}
	target = brain->path_valid && brain->step_index < brain->step_count ? &brain->steps[brain->step_index] : hop_point;

	/* getting unstuck: in 2 seconds, less than a world unit moved, or no closer to the same corner (sliding along
	a wall, or bots pushing each other) */
	if (now - brain->progress_time >= BOTS_PROGRESS_TICKS)
	{
		real target_distance = bots_distance3d(&position, target);
		boolean same_target = bots_distance3d(target, &brain->progress_target) < 0.01f;

		if (bots_distance3d(&position, &brain->progress_point) < BOTS_PROGRESS_DISTANCE ||
			(same_target && target_distance > brain->progress_target_distance - 0.5f))
		{
			switch (++brain->stuck_level)
			{
			case 1:
				brain->jump = TRUE;
				break;
			case 2:
				brain->sidestep_until = now + BOTS_SIDESTEP_TICKS;
				brain->sidestep_sign = (bots_random() & 1) ? 1.f : -1.f;
				brain->jump = TRUE;
				break;
			case 3:
				brain->path_valid = FALSE;
				bots_want_search(brain, now);
				break;
			default:
				bots_route.give_ups++;
				bots_stuck_log_add(&position, &waypoint->point);
				bots_hop_failed(brain, now, FALSE);
				/* (and this destination isn't worth it for a while) */
				bots_avoid(brain, brain->destination, now);
				brain->stuck_level = 0;
				break;
			}
		}
		else
		{
			brain->stuck_level = 0;
		}
		brain->progress_point = position;
		brain->progress_target = *target;
		brain->progress_target_distance = target_distance;
		brain->progress_time = now;
	}

	move_i = target->x - position.x;
	move_j = target->y - position.y;
	length = (real)sqrt(move_i * move_i + move_j * move_j);
	if (length < 0.01f)
		return;
	move_i /= length;
	move_j /= length;
	desired_yaw = (real)atan2(move_j, move_i);

	/* another player close ahead: around them, to the right */
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct player_datum *other = index != absolute_player_index ? bots_player_at(index) : NULL;
		real_point3d other_position;
		real di, dj, distance;

		if (!other || other->unit_index == NONE || !object_try_and_get(other->unit_index))
			continue;
		object_get_origin(other->unit_index, &other_position);
		if ((real)fabs(other_position.z - position.z) > 1.f)
			continue;
		di = other_position.x - position.x;
		dj = other_position.y - position.y;
		distance = (real)sqrt(di * di + dj * dj);
		if (distance < 1.2f && distance > 0.01f && (di * move_i + dj * move_j) / distance > 0.6f)
		{
			real right_i = move_j, right_j = -move_i;

			move_i += right_i * 0.8f;
			move_j += right_j * 0.8f;
			break;
		}
	}
	if (now < brain->sidestep_until)
	{
		real side_i = -move_j * brain->sidestep_sign, side_j = move_i * brain->sidestep_sign;

		move_i = move_i * 0.4f + side_i * 0.9f;
		move_j = move_j * 0.4f + side_j * 0.9f;
	}
	length = (real)sqrt(move_i * move_i + move_j * move_j);
	if (length > 1.f)
	{
		move_i /= length;
		move_j /= length;
	}

	brain->move_i = move_i;
	brain->move_j = move_j;
	brain->walk_yaw = desired_yaw;
	brain->walk_has_yaw = TRUE;
}

/* ---------- fighting (milestone 4)

Seeing: every 5 ticks a bot looks for enemies (game_team_is_enemy: the other
team, or everyone in free for all) within 45 world units. One in front (within
50 degrees) is noticed; one to the side when within 8 units, any when within
2.5, or when it hit the bot in the last 2 seconds. The nearest few noticed are
tried with Halo's line of sight ray (structure, vehicles, scenery, machines),
the target kept a little longer than a new one would be picked.

Aiming: a target sighted is fired on after a reaction time (a quarter second
and a bit). The aim starts off by a few degrees and settles while the bot keeps
aiming; it leads the target by the weapon's projectile speed
(weapon_estimate_time_to_target). The bot turns at a capped speed, and fires
only when its facing is close enough for the distance: automatic weapons in
bursts, others tapped at their rate of fire.

Weapons: each one scored by its damage a second (weapon_definition_get_damage_
potential and its rate of fire), less past its aim assist range and much less
inside its AI minimum range (rockets up close); a bot switches to its best for
the target's distance, and walks over to a weapon on the ground within 10 units
that's better than the worse of its two (the stock pickup: the action button
held, a swap, with the weapon to give up in hand).

Grenades at an enemy 2 to 6 units away that it can see, one every 8 to 14
seconds; melee within arm's reach. While it sees its target, a bot goes on
along its route, strafing, and closer or back for its weapon; when it loses
sight for a second, it heads for where it last saw it. */

enum
{
	BOTS_LOOK_TICKS = 5,
	BOTS_REACTION_TICKS = 8,                /* + up to 4 */
	BOTS_TARGET_MEMORY_TICKS = 5 * BOTS_TICKS_PER_SECOND,
	BOTS_ATTACKED_TICKS = 2 * BOTS_TICKS_PER_SECOND,
	BOTS_LOOK_CANDIDATES = 3,
	BOTS_WEAPON_TICKS = BOTS_TICKS_PER_SECOND,
	BOTS_PICKUP_LOOK_TICKS = BOTS_TICKS_PER_SECOND,
	BOTS_PICKUP_TICKS = 5 * BOTS_TICKS_PER_SECOND,
	BOTS_PICKUP_REST_TICKS = 10 * BOTS_TICKS_PER_SECOND, /* after one, none for this long */
	BOTS_GRENADE_TICKS = 8 * BOTS_TICKS_PER_SECOND, /* + up to 6 s */
	BOTS_MELEE_TICKS = BOTS_TICKS_PER_SECOND,
};

#define BOTS_SIGHT_RANGE 45.f
#define BOTS_SIGHT_ANGLE 0.87f              /* radians (50 degrees) */
#define BOTS_SIDE_SIGHT_RANGE 8.f
#define BOTS_CLOSE_SIGHT_RANGE 2.5f
#define BOTS_AIM_TURN_PER_TICK 0.3f         /* radians */
#define BOTS_PITCH_TURN_PER_TICK 0.2f
#define BOTS_AIM_ERROR 0.12f                /* radians, at the start of a sighting */
#define BOTS_AIM_SETTLE 0.92f               /* the error kept each tick */
#define BOTS_MELEE_RANGE 0.9f
#define BOTS_GRENADE_RANGE_MINIMUM 2.f
#define BOTS_GRENADE_RANGE_MAXIMUM 6.f
#define BOTS_PICKUP_RANGE 10.f

/* (the sensible part of a turn: -pi to pi) */
static real bots_angle_difference(
	real to,
	real from)
{
	real turn = to - from;

	while (turn > BOTS_PI)
		turn -= 2.f * BOTS_PI;
	while (turn < -BOTS_PI)
		turn += 2.f * BOTS_PI;
	return turn;
}

static real bots_random_signed(
	void)
{
	return (real)(long)(bots_random() % 2001) / 1000.f - 1.f;
}

/* where an object's body is (its bounding sphere's center) */
static void bots_object_center(
	long object_index,
	real_point3d *center)
{
	struct object_datum *object = object_get(object_index);

	*center = object->object.bounding_sphere_center;
}

/* whether nothing that blocks sight is between two points */
static boolean bots_line_of_sight(
	real_point3d const *from,
	real_point3d const *to,
	long ignore_object_index)
{
	struct collision_result collision;
	real_vector3d vector;
	boolean blocked;

	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return FALSE;
	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
	blocked = collision_test_vector(_collision_test_for_line_of_sight_flags, from, &vector, ignore_object_index, &collision);
	--global_current_collision_user_depth;
	return !blocked;
}

/* a weapon's worth at a distance: its damage a second, less past its range, much less inside its minimum
range; 0 when it can't fire */
static real bots_weapon_score(
	long weapon_index,
	real distance)
{
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	struct weapon_trigger_definition const *trigger;
	real rate_of_fire = 1.f;
	real score, range;

	if (weapon_index == NONE || !object_try_and_get(weapon_index) || !weapon_can_be_fired(weapon_index))
		return 0.f;
	weapon = weapon_get(weapon_index);
	definition = weapon_definition_get(weapon->definition_index);
	if (!definition->weapon.triggers.count)
		return 0.f;
	trigger = TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition);
	if (trigger->projectile.index == NONE)
		return 0.f;
	score = weapon_definition_get_damage_potential(weapon->definition_index, &rate_of_fire);
	if (rate_of_fire < 0.5f)
		rate_of_fire = 0.5f;
	score *= rate_of_fire * (real)(trigger->projectiles_per_shot > 1 ? trigger->projectiles_per_shot : 1);
	if (score <= 0.f)
		score = 1.f;
	range = definition->weapon.aim_assist_parameters.autoaim_distance;
	if (range <= 0.f)
		range = 15.f;
	if (definition->weapon.zoom_magnification_maximum >= 4.f && range < 40.f)
		range = 40.f;
	if (distance < definition->weapon.ai_minimum_target_range)
		score *= 0.05f;
	if (distance > range)
		score *= (range / distance) * (range / distance);
	return score;
}

/* the slot of the unit's best weapon for a distance (NONE: none can fire) */
static short bots_best_weapon(
	struct unit_datum const *unit,
	real distance,
	real *best_score)
{
	short best = NONE;
	short slot;

	*best_score = 0.f;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		real score = bots_weapon_score(unit->unit.weapon_object_indices[slot], distance);

		if (score > *best_score)
		{
			best = slot;
			*best_score = score;
		}
	}
	return best;
}

/* the enemy the bot sees, if any (its slot), and where its body is */
static short bots_look(
	struct bots_brain *brain,
	long absolute_player_index,
	struct player_datum const *player,
	struct unit_datum const *unit,
	long now,
	real_point3d *seen_center)
{
	real_point3d eye;
	real facing_i, facing_j, facing_k;
	short candidates[BOTS_LOOK_CANDIDATES];
	real candidate_scores[BOTS_LOOK_CANDIDATES];
	real_point3d candidate_centers[BOTS_LOOK_CANDIDATES];
	short count = 0;
	long index;

	unit_get_camera_position(player->unit_index, &eye);
	facing_i = (real)(cos(brain->pitch) * cos(brain->yaw));
	facing_j = (real)(cos(brain->pitch) * sin(brain->yaw));
	facing_k = (real)sin(brain->pitch);
	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct player_datum *other = index != absolute_player_index ? bots_player_at(index) : NULL;
		real_point3d center;
		real di, dj, dk, distance, angle, score;
		boolean noticed;
		short attacker;
		short slot;

		if (!other || other->unit_index == NONE || !object_try_and_get(other->unit_index) ||
			!game_team_is_enemy((short)player->team_index, (short)other->team_index))
		{
			continue;
		}
		if (TEST_FLAG(object_get(other->unit_index)->object.damage_flags, _object_dead_bit))
			continue;
		bots_object_center(other->unit_index, &center);
		di = center.x - eye.x;
		dj = center.y - eye.y;
		dk = center.z - eye.z;
		distance = (real)sqrt(di * di + dj * dj + dk * dk);
		if (distance > BOTS_SIGHT_RANGE || distance < 0.01f)
			continue;
		angle = (real)acos(PIN((di * facing_i + dj * facing_j + dk * facing_k) / distance, -1.f, 1.f));
		noticed = angle < BOTS_SIGHT_ANGLE || (distance < BOTS_SIDE_SIGHT_RANGE && angle < 2.f * BOTS_SIGHT_ANGLE) ||
			distance < BOTS_CLOSE_SIGHT_RANGE ||
			(index == brain->target && now - brain->target_seen_time < BOTS_TICKS_PER_SECOND);
		for (attacker = 0; !noticed && attacker < MAXIMUM_ATTACKERS_PER_UNIT; attacker++)
		{
			struct unit_attacker const *record = &unit->unit.attackers[attacker];

			if (record->player_index != NONE && DATUM_INDEX_TO_ABSOLUTE_INDEX(record->player_index) == index &&
				(long)(now - (long)record->game_time_stamp) < BOTS_ATTACKED_TICKS)
			{
				noticed = TRUE;
			}
		}
		if (!noticed)
			continue;
		score = index == brain->target ? distance * 0.6f : distance;
		/* (the nearest few, by score) */
		for (slot = count; slot > 0 && candidate_scores[slot - 1] > score; slot--)
		{
			if (slot < BOTS_LOOK_CANDIDATES)
			{
				candidates[slot] = candidates[slot - 1];
				candidate_scores[slot] = candidate_scores[slot - 1];
				candidate_centers[slot] = candidate_centers[slot - 1];
			}
		}
		if (slot < BOTS_LOOK_CANDIDATES)
		{
			candidates[slot] = (short)index;
			candidate_scores[slot] = score;
			candidate_centers[slot] = center;
			if (count < BOTS_LOOK_CANDIDATES)
				count++;
		}
	}
	for (index = 0; index < count; index++)
	{
		if (bots_line_of_sight(&eye, &candidate_centers[index], player->unit_index))
		{
			*seen_center = candidate_centers[index];
			return candidates[index];
		}
	}
	return NONE;
}

/* a route to the waypoint nearest a point (where a target was last seen); FALSE when none is near */
static boolean bots_head_for(
	struct bots_brain *brain,
	real_point3d const *position,
	real_point3d const *point,
	long now)
{
	real cost[BOTS_WAYPOINT_MAXIMUM];
	short previous[BOTS_WAYPOINT_MAXIMUM];
	short route[BOTS_WAYPOINT_MAXIMUM];
	short best = NONE;
	real best_distance = 8.f;
	short length = 0;
	short index;
	short node;
	short at;

	at = bots_explore(brain, position, now, cost, previous);
	for (index = 0; index < bots_route.waypoint_count; index++)
	{
		real distance;

		if (cost[index] == REAL_MAX || bots_route.waypoints[index].teleport_source)
			continue;
		distance = bots_distance3d(point, &bots_route.waypoints[index].point);
		if (distance < best_distance)
		{
			best = index;
			best_distance = distance;
		}
	}
	if (best == NONE || best == at)
		return FALSE;
	for (node = best; node != NONE && length < bots_route.waypoint_count; node = previous[node])
		route[length++] = node;
	brain->route_count = 0;
	for (index = length - 1; index >= 0 && brain->route_count < BOTS_ROUTE_MAXIMUM; index--)
		brain->route[brain->route_count++] = route[index];
	brain->route_position = 0;
	brain->destination = best;
	brain->from_waypoint = at;
	brain->path_valid = FALSE;
	brain->wants_search = FALSE;
	brain->hop_closest = REAL_MAX;
	brain->hop_stalls = 0;
	brain->hop_deadline = 0;
	return TRUE;
}

/* a weapon on the ground near the bot better than the worse of its own (NONE: none) */
static long bots_find_pickup(
	struct unit_datum const *unit,
	long unit_index,
	real_point3d const *position,
	long ignore_object_index)
{
	struct object_iterator iterator;
	real worst_score = REAL_MAX;
	real best_score;
	long best = NONE;
	short carried = 0;
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];
		real score;

		if (weapon_index == NONE)
			continue;
		carried++;
		score = bots_weapon_score(weapon_index, 10.f);
		if (score < worst_score)
			worst_score = score;
	}
	if (carried < 2)
		worst_score = 0.f;
	best_score = worst_score * 1.3f + 1.f;
	object_iterator_new(&iterator, _object_mask_weapon, 0);
	while (object_iterator_next(&iterator))
	{
		struct weapon_datum *weapon = weapon_get(iterator.index);
		real_point3d origin;
		real score;

		if (iterator.index == ignore_object_index || weapon->object.parent_object_index != NONE ||
			TEST_FLAG(weapon->item.flags, _item_attached_to_unit_bit))
		{
			continue;
		}
		object_get_origin(iterator.index, &origin);
		if (bots_distance2d(position, &origin) > BOTS_PICKUP_RANGE || (real)fabs(origin.z - position->z) > 1.5f)
			continue;
		/* (not one like a weapon it has: walking over that only takes its ammunition) */
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long carried_index = unit->unit.weapon_object_indices[slot];

			if (carried_index != NONE && object_try_and_get(carried_index) &&
				weapon_get(carried_index)->definition_index == weapon->definition_index)
			{
				break;
			}
		}
		if (slot < MAXIMUM_WEAPONS_PER_UNIT)
			continue;
		score = bots_weapon_score(iterator.index, 10.f);
		if (score > best_score)
		{
			best = iterator.index;
			best_score = score;
		}
	}
	if (best != NONE)
	{
		/* (one it can walk straight to: nothing in the way) */
		real_point3d from = *position, to;

		object_get_origin(best, &to);
		from.z += 0.4f;
		to.z += 0.2f;
		if (!bots_line_of_sight(&from, &to, unit_index))
			best = NONE;
	}
	return best;
}

/* the bot's fight this tick: what it sees, where it aims, whether it fires; and its weapons */
static void bots_fight(
	struct bots_brain *brain,
	long absolute_player_index,
	struct player_datum const *player,
	struct unit_datum *unit,
	struct player_action *action)
{
	long now = game_time_get();
	real_point3d position, eye, center;
	short seen = brain->target_visible ? brain->target : NONE;
	long weapon_index;
	real distance = 10.f;

	object_get_origin(player->unit_index, &position);
	unit_get_camera_position(player->unit_index, &eye);
	if (brain->yaw < 0.f)
	{
		brain->yaw = action->desired_facing.yaw;
		brain->pitch = 0.f;
	}

	/* looking (the bots take turns: a fifth of them a tick) */
	if (!brain->next_look_time)
		brain->next_look_time = now + absolute_player_index % BOTS_LOOK_TICKS;
	if (now >= brain->next_look_time)
	{
		brain->next_look_time = now + BOTS_LOOK_TICKS;
		seen = bots_look(brain, absolute_player_index, player, unit, now, &center);
	}
	else if (seen != NONE)
	{
		/* (between looks: following the target it saw) */
		struct player_datum *target = bots_player_at(seen);

		if (!target || target->unit_index != brain->target_unit || !object_try_and_get(target->unit_index) ||
			TEST_FLAG(object_get(target->unit_index)->object.damage_flags, _object_dead_bit))
		{
			seen = NONE;
		}
		else
		{
			bots_object_center(target->unit_index, &center);
		}
	}
	if (seen != NONE)
	{
		struct player_datum *target = bots_player_at(seen);

		if (seen != brain->target || !brain->target_visible)
		{
			/* (a new sighting: a reaction time, and an aim that has to settle) */
			bots_route.sightings++;
			brain->target_sighted_time = now + BOTS_REACTION_TICKS + (long)(bots_random() % 5);
			brain->aim_error_yaw = BOTS_AIM_ERROR * bots_random_signed();
			brain->aim_error_pitch = 0.5f * BOTS_AIM_ERROR * bots_random_signed();
			brain->weapon_time = now;
		}
		brain->target = seen;
		brain->target_unit = target->unit_index;
		brain->target_visible = TRUE;
		brain->target_seen_time = now;
		brain->target_position = center;
		brain->chasing = FALSE;
	}
	else
	{
		struct player_datum *target = brain->target != NONE ? bots_player_at(brain->target) : NULL;

		brain->target_visible = FALSE;
		/* (forgotten after a while, or at once when it died) */
		if (brain->target != NONE &&
			(now - brain->target_seen_time > BOTS_TARGET_MEMORY_TICKS || !target || target->unit_index != brain->target_unit))
		{
			brain->target = NONE;
		}
	}

	/* the weapon for the distance (and a reload or a switch when the one in hand is empty) */
	if (brain->target != NONE)
		distance = bots_distance3d(&eye, &brain->target_position);
	if (now >= brain->weapon_time)
	{
		real best_score;
		short best = bots_best_weapon(unit, distance, &best_score);
		real current_score = bots_weapon_score(unit->unit.weapon_object_indices[unit->unit.current_weapon_index >= 0 &&
			unit->unit.current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT ? unit->unit.current_weapon_index : 0], distance);

		brain->weapon_time = now + BOTS_WEAPON_TICKS;
		brain->wanted_weapon = best != NONE && best != unit->unit.current_weapon_index && best_score > current_score * 1.2f ?
			best : NONE;
	}
	if (brain->wanted_weapon != NONE && brain->wanted_weapon != unit->unit.current_weapon_index)
		action->desired_weapon_index = brain->wanted_weapon;
	weapon_index = unit->unit.current_weapon_index >= 0 && unit->unit.current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;

	/* aiming and firing */
	if (brain->target_visible)
	{
		struct object_datum *target_object = object_get(brain->target_unit);
		real_point3d aim = brain->target_position;
		real di, dj, dk, aim_yaw, aim_pitch, yaw_error, pitch_error, tolerance;

		if (weapon_index != NONE && object_try_and_get(weapon_index))
		{
			struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);

			if (definition->weapon.triggers.count &&
				TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition)->projectile.index != NONE)
			{
				/* (leading: where it will be when the shot gets there) */
				real ticks = weapon_estimate_time_to_target(weapon_index, 0, distance);

				if (ticks > 0.f && ticks < 90.f)
				{
					aim.x += target_object->object.translational_velocity.i * ticks;
					aim.y += target_object->object.translational_velocity.j * ticks;
					aim.z += target_object->object.translational_velocity.k * ticks;
				}
			}
		}
		di = aim.x - eye.x;
		dj = aim.y - eye.y;
		dk = aim.z - eye.z;
		aim_yaw = (real)atan2(dj, di) + brain->aim_error_yaw;
		aim_pitch = (real)atan2(dk, sqrt(di * di + dj * dj)) + brain->aim_error_pitch;
		brain->aim_error_yaw = brain->aim_error_yaw * BOTS_AIM_SETTLE + 0.004f * bots_random_signed();
		brain->aim_error_pitch = brain->aim_error_pitch * BOTS_AIM_SETTLE + 0.002f * bots_random_signed();
		yaw_error = bots_angle_difference(aim_yaw, brain->yaw);
		pitch_error = aim_pitch - brain->pitch;
		brain->yaw += PIN(yaw_error, -BOTS_AIM_TURN_PER_TICK, BOTS_AIM_TURN_PER_TICK);
		brain->pitch += PIN(pitch_error, -BOTS_PITCH_TURN_PER_TICK, BOTS_PITCH_TURN_PER_TICK);
		tolerance = (real)atan2(0.35f, distance > 0.5f ? distance : 0.5f);
		if (tolerance < 0.03f)
			tolerance = 0.03f;
		brain->aimed = (real)fabs(bots_angle_difference(aim_yaw, brain->yaw)) < tolerance &&
			(real)fabs(aim_pitch - brain->pitch) < tolerance;

		if (distance < BOTS_MELEE_RANGE + 0.6f && now >= brain->melee_time)
		{
			/* (within arm's reach) */
			action->control_flags |= FLAG(_unit_control_use_equipment_bit);
			brain->melee_time = now + BOTS_MELEE_TICKS;
			bots_route.melees++;
		}
		else if (now >= brain->target_sighted_time && brain->aimed && weapon_index != NONE &&
			distance > BOTS_GRENADE_RANGE_MINIMUM && distance < BOTS_GRENADE_RANGE_MAXIMUM && now >= brain->grenade_time &&
			(unit->unit.grenade_counts[0] > 0 || unit->unit.grenade_counts[1] > 0))
		{
			/* (a grenade: thrown a little high, for its arc; of the other kind when the one in hand has run out) */
			short current = unit->unit.current_grenade_index;

			if (current < 0 || current >= NUMBER_OF_UNIT_GRENADE_TYPES || unit->unit.grenade_counts[current] <= 0)
				action->desired_grenade_index = unit->unit.grenade_counts[0] > 0 ? 0 : 1;
			action->control_flags |= FLAG(_unit_control_throw_grenade_bit) | FLAG(_unit_control_weapon_secondary_trigger_bit);
			brain->pitch += 0.12f;
			brain->grenade_time = now + BOTS_GRENADE_TICKS + (long)(bots_random() % (6 * BOTS_TICKS_PER_SECOND));
			bots_route.grenades++;
		}
		else if (now >= brain->target_sighted_time && weapon_index != NONE && object_try_and_get(weapon_index))
		{
			/* bursts: an automatic weapon held, others tapped at their rate of fire */
			if (brain->burst_ticks > 0)
			{
				brain->burst_ticks--;
				brain->trigger_ticks++;
				action->control_flags |= FLAG(_unit_control_weapon_primary_trigger_bit);
				action->primary_trigger = brain->trigger_ticks < 255 ? (real)brain->trigger_ticks / 255.f : 1.f;
				bots_route.firing_ticks++;
			}
			else if (brain->pause_ticks > 0)
			{
				brain->pause_ticks--;
				brain->trigger_ticks = 0;
			}
			else if (brain->aimed)
			{
				struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
				real rate_of_fire = definition->weapon.triggers.count ?
					TAG_BLOCK_GET_ELEMENT(&definition->weapon.triggers, 0, struct weapon_trigger_definition)->final_rate_of_fire : 1.f;

				if (rate_of_fire >= 5.f)
				{
					brain->burst_ticks = (short)(12 + bots_random() % 13);
					brain->pause_ticks = (short)(6 + bots_random() % 9);
				}
				else
				{
					brain->burst_ticks = 2;
					brain->pause_ticks = (short)(rate_of_fire > 0.5f ? BOTS_TICKS_PER_SECOND / rate_of_fire : 30.f);
					if (brain->pause_ticks < 4)
						brain->pause_ticks = 4;
				}
			}
		}

		/* moving while fighting: strafing, and the distance for the weapon */
		{
			real to_i = di, to_j = dj, length = (real)sqrt(di * di + dj * dj);
			real move_i, move_j, preferred = 8.f, minimum = 2.f;

			if (weapon_index != NONE && object_try_and_get(weapon_index))
			{
				struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
				real range = definition->weapon.aim_assist_parameters.autoaim_distance;

				if (range > 0.f)
					preferred = 0.6f * range;
				if (definition->weapon.ai_minimum_target_range > minimum)
					minimum = definition->weapon.ai_minimum_target_range;
			}
			if (length > 0.01f)
			{
				to_i /= length;
				to_j /= length;
			}
			if (now >= brain->strafe_until)
			{
				brain->strafe_sign = (bots_random() & 1) ? 1.f : -1.f;
				brain->strafe_until = now + 15 + (long)(bots_random() % 30);
			}
			/* (on along its route while it fights, as a player would, rather than standing in a crowd: the walk,
			a strafe, and closer or back for the weapon) */
			move_i = brain->move_i * 0.7f - to_j * brain->strafe_sign * 0.6f;
			move_j = brain->move_j * 0.7f + to_i * brain->strafe_sign * 0.6f;
			if (distance > preferred)
			{
				move_i += to_i * 0.4f;
				move_j += to_j * 0.4f;
			}
			else if (distance < minimum)
			{
				move_i -= to_i * 0.6f;
				move_j -= to_j * 0.6f;
			}
			brain->move_i = move_i;
			brain->move_j = move_j;
		}
	}
	else
	{
		brain->aimed = FALSE;
		brain->burst_ticks = 0;
		brain->trigger_ticks = 0;
		/* lost sight (for a second: not every time it ducks behind something): where it was last seen */
		if (brain->target != NONE && !brain->chasing && now - brain->target_seen_time >= BOTS_TICKS_PER_SECOND)
		{
			brain->chasing = TRUE;
			bots_head_for(brain, &position, &brain->target_position, now);
		}
		/* a better weapon nearby, out of a fight */
		if (brain->target == NONE && now >= brain->pickup_time)
		{
			brain->pickup_time = now + BOTS_PICKUP_LOOK_TICKS;
			if (brain->pickup_object == NONE)
			{
				brain->pickup_object = bots_find_pickup(unit, player->unit_index, &position,
					now < brain->pickup_failed_until ? brain->pickup_failed_object : NONE);
				brain->pickup_until = now + BOTS_PICKUP_TICKS;
				brain->action_ticks = 0;
			}
		}
		if (brain->pickup_object != NONE)
		{
			struct weapon_datum *weapon = object_try_and_get(brain->pickup_object) ?
				(struct weapon_datum *)object_get_and_verify_type(brain->pickup_object, _object_mask_weapon) : NULL;
			real_point3d origin;

			if (!weapon || weapon->object.parent_object_index != NONE || now > brain->pickup_until)
			{
				if (weapon && weapon->object.parent_object_index == NONE)
				{
					brain->pickup_failed_object = brain->pickup_object;
					brain->pickup_failed_until = now + 20 * BOTS_TICKS_PER_SECOND;
				}
				brain->pickup_object = NONE;
				brain->pickup_time = now + BOTS_PICKUP_REST_TICKS;
			}
			else
			{
				real di, dj, length;

				object_get_origin(brain->pickup_object, &origin);
				di = origin.x - position.x;
				dj = origin.y - position.y;
				length = (real)sqrt(di * di + dj * dj);
				if (length > 0.6f)
				{
					brain->move_i = di / length;
					brain->move_j = dj / length;
					brain->walk_yaw = (real)atan2(dj, di);
					brain->walk_has_yaw = TRUE;
				}
				else
				{
					/* (on it: the worse weapon in hand, then the action button) */
					real worst_score = REAL_MAX;
					short worst = NONE;
					short slot;

					for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
					{
						real score;

						if (unit->unit.weapon_object_indices[slot] == NONE)
							continue;
						score = bots_weapon_score(unit->unit.weapon_object_indices[slot], 10.f);
						if (score < worst_score)
						{
							worst = slot;
							worst_score = score;
						}
					}
					brain->move_i = brain->move_j = 0.f;
					if (worst != NONE && worst != unit->unit.current_weapon_index)
					{
						action->desired_weapon_index = worst;
					}
					else if (brain->action_ticks < 15)
					{
						/* (the action button held: a swap, as players_update_before_game reads it; the action alone
						would reload, or get into a vehicle) */
						action->control_flags |= FLAG(_unit_control_swap_weapons_bit);
						if (!brain->action_ticks)
							bots_route.pickups++;
						brain->action_ticks++;
					}
					else
					{
						brain->pickup_failed_object = brain->pickup_object;
						brain->pickup_failed_until = now + 20 * BOTS_TICKS_PER_SECOND;
						brain->pickup_object = NONE;
						brain->pickup_time = now + BOTS_PICKUP_REST_TICKS;
					}
				}
			}
		}
	}
}

/* the bot's action from its walk and its fight: facing (turning at a person's speed), throttle, jump */
static void bots_act(
	struct bots_brain *brain,
	struct player_action *action)
{
	real forward_i, forward_j, length;

	if (brain->yaw < 0.f)
	{
		brain->yaw = action->desired_facing.yaw;
		brain->pitch = 0.f;
	}
	if (!brain->target_visible)
	{
		if (brain->walk_has_yaw)
			brain->yaw += PIN(bots_angle_difference(brain->walk_yaw, brain->yaw), -BOTS_TURN_PER_TICK, BOTS_TURN_PER_TICK);
		brain->pitch += PIN(-brain->pitch, -BOTS_PITCH_TURN_PER_TICK, BOTS_PITCH_TURN_PER_TICK);
	}
	while (brain->yaw >= 2.f * BOTS_PI)
		brain->yaw -= 2.f * BOTS_PI;
	while (brain->yaw < 0.f)
		brain->yaw += 2.f * BOTS_PI;
	brain->pitch = PIN(brain->pitch, -1.4f, 1.4f);
	action->desired_facing.yaw = brain->yaw;
	action->desired_facing.pitch = brain->pitch;

	/* the throttle is the unit's own: forward and left of where it faces */
	length = (real)sqrt(brain->move_i * brain->move_i + brain->move_j * brain->move_j);
	if (length > 1.f)
	{
		brain->move_i /= length;
		brain->move_j /= length;
	}
	forward_i = (real)cos(brain->yaw);
	forward_j = (real)sin(brain->yaw);
	action->throttle.i = brain->move_i * forward_i + brain->move_j * forward_j;
	action->throttle.j = -brain->move_i * forward_j + brain->move_j * forward_i;
	if (brain->jump)
	{
		action->control_flags |= FLAG(_unit_control_jump_bit);
		brain->jump = FALSE;
	}
}

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
#if defined(OXBOX_TEST_BOT_KILLS)
	bots_test_update();
#endif
	bots_report();
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
	unit = player->unit_index != NONE ? (struct unit_datum *)object_try_and_get(player->unit_index) : NULL;
	if (unit)
	{
		real_vector3d const *facing = &unit->unit.desired_facing_vector;

		action->desired_facing.yaw = (real)atan2(facing->j, facing->i);
		if (action->desired_facing.yaw < 0.f)
			action->desired_facing.yaw += (real)(2.0 * 3.14159265358979323846);
		action->desired_facing.pitch = 0.f;
	}
	/* (milestone 3: walking the map, on foot) */
	if (unit && biped_try_and_get(player->unit_index) &&
		absolute_player_index >= 0 && absolute_player_index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
	{
		long now = game_time_get();
		unsigned long long cycles = bots_cycles();

		if (absolute_player_index <= bots_route.round_last_index)
		{
			if (bots_route.cycles_tick > bots_route.cycles_tick_maximum)
				bots_route.cycles_tick_maximum = bots_route.cycles_tick;
			bots_route.cycles_tick = 0;
			bots_route.ticks++;
			bots_route.rounds++;
			bots_route.walking = bots_route.walking_this_tick;
			bots_route.walking_this_tick = 0;
			bots_route_update();
		}
		bots_route.round_last_index = absolute_player_index;
		if (bots_route.waypoint_count)
		{
			struct bots_brain *brain = &bots_brains[absolute_player_index];

			if (brain->unit_index == player->unit_index && now - brain->arrival_time > BOTS_TRAPPED_TICKS)
			{
				/* trapped (fallen somewhere with no way out): it dies, and respawns */
				real_point3d position;

				object_get_origin(player->unit_index, &position);
				bots_stuck_log_add(&position, &position);
				brain->arrival_time = now;
				damage_kill_object_for_player(player->unit_index,
					DATUM_INDEX_NEW(absolute_player_index, ((struct datum_header *)player)->identifier));
				return TRUE;
			}
			{
				unsigned long long walk_start = bots_cycles(), fight_start, fight_end;

				bots_walk(brain, absolute_player_index, player->unit_index, action);
				fight_start = bots_cycles();
				bots_fight(brain, absolute_player_index, player, unit, action);
				bots_act(brain, action);
				fight_end = bots_cycles();
				if (fight_start - walk_start > bots_route.walk_cycles_maximum)
					bots_route.walk_cycles_maximum = fight_start - walk_start;
				if (fight_end - fight_start > bots_route.fight_cycles_maximum)
					bots_route.fight_cycles_maximum = fight_end - fight_start;
			}
			bots_route.walking_this_tick++;
		}
		cycles = bots_cycles() - cycles;
		bots_route.cycles += cycles;
		bots_route.cycles_tick += cycles;
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
