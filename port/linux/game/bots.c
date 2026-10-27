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
below). Milestone 4 (29): they fight. Milestone 5 (36): they crouch, jump and
crouch-jump, and step off ledges (moving like a player). Milestone 6 (37): the
gametype's objective. Milestone 7 (38): the difficulty (Black in the lobby),
power items, backing off when hurt, teammates. Milestone 8 (39): vehicles.

Test builds start in Custom with OXBOX_EXTRA=-DOXBOX_TEST_BOTS=<count> (and
the bot log on, and logs of each bot's goal, jumps, searches that end short,
and the waypoints); -DOXBOX_TEST_BOT_KILLS adds the host's player killing a
bot every 20 seconds.
*/

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
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
#include "units/unit_definitions.h"
#include "units/vehicles.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "physics/collisions.h"
#include "physics/collision_bsp.h"
#include "physics/collision_usage.h"
#include "physics/physics.h"
#include "ai/path.h"
#include "game/game_allegiance.h"
#include "items/items.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/equipment_definitions.h"
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

/* how well bots fight (milestone 7): the host picks it in the lobby with Black */
enum bots_difficulty
{
	_bots_difficulty_easy,
	_bots_difficulty_normal,
	_bots_difficulty_hard,
	_bots_difficulty_legendary,
	NUMBER_OF_BOTS_DIFFICULTIES
};

struct bots_skill
{
	char const *name;
	short reaction_ticks;                   /* before the first shot at a new sighting (+ up to 4) */
	real aim_error;                         /* radians, at the start of a sighting */
	real aim_settle;                        /* of the error, kept each tick */
	real aim_turn;                          /* radians a tick (the pitch two thirds of it) */
	real sight_range;
	real sight_angle;                       /* radians, half the cone it notices enemies in */
	long grenade_ticks;                     /* between grenades (+ up to 6 seconds) */
	real fire_tolerance;                    /* how well aimed before it fires (x the normal) */
};

static struct bots_skill const bots_skills[NUMBER_OF_BOTS_DIFFICULTIES] =
{
	{ "Easy", 16, 0.22f, 0.95f, 0.18f, 35.f, 0.70f, 14 * 30, 1.5f },
	{ "Normal", 8, 0.12f, 0.92f, 0.30f, 45.f, 0.87f, 8 * 30, 1.0f },
	{ "Hard", 5, 0.07f, 0.88f, 0.40f, 50.f, 1.00f, 6 * 30, 0.85f },
	{ "Legendary", 3, 0.04f, 0.84f, 0.50f, 55.f, 1.10f, 5 * 30, 0.75f },
};

static struct
{
	unsigned long last_change;
	short mode;
	short custom_count;
	char suggested_map[128];
	short suggested_players;
	short difficulty;
} bots_globals =
#if defined(OXBOX_TEST_BOTS)
	{ 0, _bots_mode_custom, OXBOX_TEST_BOTS, "", 0, _bots_difficulty_normal };
#else
	{ 0, _bots_mode_off, 4, "", 0, _bots_difficulty_normal };
#endif

/* the lobby's setting (NETWORK_GAME_BOT_MODE): the mode + 1, and the difficulty in the high byte */
#define BOTS_SETTING(mode, difficulty) ((short)((mode) + 1 + ((difficulty) << 8)))
#if defined(OXBOX_CROSSPLAY)
/* oxbox: how long the host has to press Y again, once told that bots drop the lobby's PC players */
#define BOTS_CONFIRM_MILLISECONDS 20000
#endif

static struct bots_skill const *bots_skill(
	void)
{
	return &bots_skills[bots_globals.difficulty >= 0 && bots_globals.difficulty < NUMBER_OF_BOTS_DIFFICULTIES ?
		bots_globals.difficulty : _bots_difficulty_normal];
}

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
	long jumps, crouch_jumps, jumps_landed, crouches, straight_up, straight_down, straight_done, overhangs;
	unsigned long long probe_cycles;
	boolean body_logged;                    /* the bots' reach said in the log, for this map */

	/* the game types' spots of this map: the flags' bases, the race's flags (bots_route_build) */
	boolean ctf_base_valid[2];
	real_point3d ctf_base[2];
	short race_flag_count;
	short race_flag_numbers[32];
	real_point3d race_flags[32];
	long goal_counts[10];                   /* bots after each kind of goal (the 10-second report) */
	long vehicle_entries, vehicle_exits, driving_ticks, gunning_ticks, vehicle_reverses;
	long vehicle_firing_ticks, crew_waits, scouts, evades, weapon_switches, weapon_switches_done, reloads;
	long crew_gave_up, seat_misses, went_around;
	struct { long vehicle, until; } crewless[8];  /* vehicles whose driver waited for a gunner in vain */
	long items_taken, retreats, regroups;
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

	/* moving like a player (milestone 5, bots_move_skills): crouching under what's too low to stand under,
	jumping (and crouch-jumping) onto what's too high to step onto, and hops finished straight up a ledge or off it */
	boolean jump_crouch;                    /* this jump crouches in the air (the feet come up by the crouch) */
	long jump_time;                         /* when the bot left the ground (0: not a jump of its own) */
	real jump_start_z;
	real jump_start_x, jump_start_y;
	real jump_goal;                         /* how high the top of what it jumps onto (or over) is */
	long jump_wait_until;                   /* no new jump before this tick */
	short crouch_ticks;                     /* crouching for this many more ticks */
	short push_ticks;                       /* trying to move, and not getting anywhere, for this many ticks */
	long probe_time;                        /* the next look ahead */
	boolean straight_after;                 /* the hop's corners end below (or above) its waypoint: then straight for
	                                           it, up the ledge or off it */
	boolean straight;                       /* finishing the hop straight */
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

	/* the game type's objective (milestone 6, bots_objective) */
	short goal_kind;                        /* none, touch the point, or hold around it */
	short goal_reason;                      /* what for (the log's counts) */
	real_point3d goal_point;
	real goal_radius;                       /* holding: this close to the point */
	long goal_object;                       /* what the point follows (a flag, a ball, a carrier's unit), NONE */
	long goal_hunt_unit;                    /* an enemy carrying an objective: seen first */
	boolean goal_urgent;                    /* carrying the objective: on while fighting, no weapon pickups */
	boolean goal_visible;                   /* in a straight line, near */
	short goal_waypoint;                    /* the route's waypoint nearest the point */
	long goal_update_time;
	long goal_look_time;
	long goal_plan_time;
	long goal_direct_since;                 /* walking straight for the point since this tick (0: not) */
	long goal_blocked_until;                /* no straight walk for it before this tick */
	real_point3d hold_point;                /* where the bot holding a spot walks to next */
	long hold_until;
	short touch_ticks;                      /* standing on a ball that wasn't picked up: the action button */

	/* fighting smarter (milestone 7) */
	long item_time;                         /* the next look for a power item */
	long item_object;                       /* the one it's going for, NONE */
	long item_until;
	long retreat_until;                     /* backing off (hurt) until this tick */

	/* vehicles (milestone 8) */
	long vehicle_object;                    /* the vehicle it's going to get into, NONE */
	short vehicle_seat;                     /* the seat it wants: driver, gunner */
	long vehicle_until;
	long vehicle_time;                      /* the next look for one */
	long vehicle_left_time;                 /* when it last got out (no new one for a while) */
	short seat_kind;                        /* the seat it's in now: none, driver, gunner, other */
	boolean driving;                        /* this tick: it steers a vehicle */
	boolean driver_aims;                    /* ... whose guns are the driver's (a Ghost, a Banshee, a Scorpion) */
	boolean flying;                         /* ... that flies */
	boolean press_action;                   /* the action button this tick (in, or out) */
	boolean exiting;                        /* getting out (counted once) */
	boolean drive_hold;                     /* this tick: the driver waits for its crew */
	long crew_wait_since;                   /* the driver's been waiting for its crew since (0: not) */

	/* build 40 */
	long scout_until;                       /* a sniper looking out over the map until this tick */
	long scout_next;                        /* no new look before this tick */
	real scout_center;                      /* the way it looks (a yaw), sweeping either side */
	short zoom;                             /* the zoom level it wants (NONE: none) */
	long evade_until;                       /* getting out of a vehicle's way until this tick */
	real evade_i, evade_j;
	long evade_time;                        /* the next look for vehicles coming at it */
	long around_until;                      /* going round a parked vehicle until this tick */
	real around_i, around_j;
	long reload_next;                       /* no reload press out of a fight before this tick */
	short last_weapon_slot;                 /* the weapon in hand last tick (the log's switches) */
	short last_wanted_weapon;
	long drive_progress_time;
	real_point3d drive_progress_point;
	short drive_stuck;
	long reverse_until;
	long idle_since;                        /* in a seat with nothing to do since */
};

static struct bots_brain bots_brains[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];

/* the objectives section's (milestone 6), and the vehicles' (milestone 8) */
enum
{
	_bots_seat_none,
	_bots_seat_driver,
	_bots_seat_gunner,
	_bots_seat_other,
};

#define BOTS_DRIVE_ARRIVAL 3.f                  /* a driving bot's at a waypoint this close */

enum
{
	_bots_goal_none,
	_bots_goal_touch,
	_bots_goal_hold,
};

enum
{
	_bots_reason_none,
	_bots_reason_carry,                     /* carrying an objective (home with the flag, round the map with the ball) */
	_bots_reason_take,                      /* going for an objective on the ground */
	_bots_reason_return,                    /* its team's flag, to send home */
	_bots_reason_hunt,                      /* an enemy carrying an objective */
	_bots_reason_escort,                    /* a teammate carrying one */
	_bots_reason_defend,                    /* its team's flag base */
	_bots_reason_hill,                      /* king of the hill, or a race's flag */
	_bots_reason_item,                      /* a power weapon or a powerup (milestone 7) */
	_bots_reason_vehicle,                   /* a vehicle to get into (milestone 8) */
	NUMBER_OF_BOTS_REASONS
};

static boolean bots_carries_objective(struct unit_datum const *unit);
/* a vehicle's seats: the driver's, a gunner's apart from it, the others (passengers), and who's in them */
struct bots_seats
{
	short driver, gunner;                   /* seat indices, NONE for none */
	long driver_unit, gunner_unit;          /* who's in them, NONE */
	short passengers, passengers_free;
};
static void bots_vehicle_seats(long vehicle_index, struct bots_seats *seats);
static long bots_unit_team(long unit_index);
short vehicle_bot_type(long vehicle_index);  /* vehicles.c's (port: the bots') */
static boolean bots_goal_walk(struct bots_brain *brain, real_point3d const *position, long now);
static boolean bots_route_to(struct bots_brain *brain, real_point3d const *position, short waypoint, long now);

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

/* ---------- moving like a player (milestone 5)

Halo's AI search knows the floor, not jumps or ceilings: it routes under a
ledge it can't climb, and through gaps a standing biped doesn't fit. So the bots
look ahead as they walk (bots_move_skills), with a few rays from their feet the
way they're going:

- Something at the knees that stops the bot (4 ticks pushing at it without
  getting anywhere: a biped walks up stairs and over a curb on its own): how
  high its top is, a little past its face. Up to the biped's jump height, a
  jump; up to its jump height plus the crouch's (a crouch in the air pulls the
  feet up by the difference between standing and crouching), a crouch-jump. The
  bot keeps pushing in the air, so it lands on top. Only with room to land
  (at least a crouched biped's height), never over a railing onto a drop (unless
  the hop means to go down), a slope's face (a ramp) never counts, and nothing
  past the reach of a crouch-jump.
- Nothing at the knees, something at the head: crouch, if a crouched biped fits.
  A crouched bot stays crouched while the ceiling over it is too low to stand.

The reach comes from the biped: its jump speed and Halo's gravity give the jump
height (v^2 / 2g); a safe drop is shorter than the globals' falling damage
starts at. The searches use them too: a hop whose corners end at the foot of a
ledge the bot can crouch-jump (or at the edge of a drop it survives), within 4.5
world units of the waypoint, is finished straight for the waypoint, and the
look-ahead does the jump. */

#define BOTS_PROBE_FLAGS (FLAG(_collision_test_front_facing_surfaces_bit) | FLAG(_collision_test_structure_bit) | \
	FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_scenery_bit) | \
	FLAG(_collision_test_objects_machines_bit))
#define BOTS_PROBE_AHEAD 0.8f               /* the look-ahead past the biped's radius */
#define BOTS_KNEE_HEIGHT 0.2f               /* lower than this a biped steps onto */
#define BOTS_JUMP_DISTANCE 0.3f             /* jump when the face is this close (past the radius) */
#define BOTS_REACH_SAFETY 0.93f             /* of the jump heights, for the ones it tries */
#define BOTS_STRAIGHT_RANGE 4.5f            /* a hop's corners ending this close to its waypoint can finish straight */

/* the globals' falling damage (bipeds.c's game_globals_falling_damage) */
struct bots_falling_damage
{
	long falling_unused[2];
	real falling_distance_lower_bound;
	real falling_distance_upper_bound;
	struct tag_reference falling_damage;
	long terminal_velocity_unused[2];
	real maximum_distance;
	struct tag_reference maximum_distance_damage;
	struct tag_reference vehicle_hit_environment_damage_effect;
	struct tag_reference vehicle_killed_unit_damage_effect;
	struct tag_reference vehicle_collision_damage;
	struct tag_reference flaming_death_damage;
	long unused7c[4];
	real runtime_maximum_falling_velocity;
	real runtime_minimum_damage_velocity;
	real runtime_maximum_damage_velocity;
};

/* what a biped can do: its size, and how high it jumps */
struct bots_body
{
	real radius;
	real standing;                          /* its collision height, standing and crouched */
	real crouching;
	real jump;                              /* how high its feet go in a jump */
	real crouch_jump;                       /* ... with a crouch in the air */
	real safe_drop;                         /* a fall that doesn't hurt */
};

static void bots_body_get(
	long unit_index,
	struct bots_body *body)
{
	struct biped_datum *biped = biped_get(unit_index);
	struct biped_definition *definition = biped_definition_get(biped->definition_index);
	struct game_globals *globals = scenario_get_game_globals();
	real gravity = global_gravity > 0.0001f ? global_gravity : 0.0001f;
	real velocity = definition->biped.jump_velocity;

	body->radius = definition->biped.collision_radius > 0.05f && definition->biped.collision_radius < 1.f ?
		definition->biped.collision_radius : BOTS_PATHFINDING_RADIUS;
	body->standing = definition->biped.collision_height_standing > 0.1f ?
		definition->biped.collision_height_standing : 0.7f;
	body->crouching = definition->biped.collision_height_crouching > 0.1f &&
		definition->biped.collision_height_crouching <= body->standing ?
		definition->biped.collision_height_crouching : body->standing;
	body->jump = velocity > 0.f ? velocity * velocity / (2.f * gravity) : 0.f;
	if (body->jump > 3.f)
		body->jump = 3.f;
	body->crouch_jump = body->jump + body->standing - body->crouching;
	body->safe_drop = 1.5f;
	if (globals && globals->falling_damage.count > 0)
	{
		struct bots_falling_damage const *falling = (struct bots_falling_damage const *)
			tag_block_get_element_with_size(&globals->falling_damage, 0, sizeof(struct bots_falling_damage));

		if (falling->falling_distance_lower_bound > 0.3f && falling->falling_distance_lower_bound < 20.f)
			body->safe_drop = falling->falling_distance_lower_bound * 0.9f;
	}
}

/* a ray against what a biped walks into and stands on (the level, scenery and machines; not players or vehicles):
how far along it the first hit is (0 to 1; 1 for none), and the hit's plane */
static real bots_ray(
	real_point3d const *origin,
	real_vector3d const *vector,
	long ignore_object_index,
	real_plane3d *plane)
{
	struct collision_result collision;
	real t = 1.f;

	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return 0.f;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_bipeds;
	if (collision_test_vector(BOTS_PROBE_FLAGS, origin, vector, ignore_object_index, &collision))
	{
		t = collision.t;
		if (plane)
			*plane = collision.plane;
	}
	--global_current_collision_user_depth;
	return t;
}

/* the height of what's under a point, looking down from a height above the feet to a depth below them (relative to
the feet); FALSE when there's nothing in that range */
static boolean bots_height_at(
	real_point3d const *feet,
	real i,
	real j,
	real from,
	real to,
	long ignore_object_index,
	real *height)
{
	real_point3d origin;
	real_vector3d vector;
	real t;

	origin.x = feet->x + i;
	origin.y = feet->y + j;
	origin.z = feet->z + from;
	vector.i = 0.f;
	vector.j = 0.f;
	vector.k = to - from;
	t = bots_ray(&origin, &vector, ignore_object_index, NULL);
	if (t >= 1.f)
		return FALSE;
	*height = from + (to - from) * t;
	return TRUE;
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
	bots_route.body_logged = FALSE;
	csmemset(bots_route.crewless, 0, sizeof(bots_route.crewless));
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
	/* the game types' spots: each team's flag base, and the race's flags (their numbers: the flags' team index) */
	bots_route.ctf_base_valid[0] = bots_route.ctf_base_valid[1] = FALSE;
	bots_route.race_flag_count = 0;
	for (index = 0; index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag const *flag = TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index,
			struct scenario_netgame_flag);

		if (flag->type == _netgame_flag_ctf_flag && flag->team_index >= 0 && flag->team_index < 2 &&
			!bots_route.ctf_base_valid[flag->team_index])
		{
			bots_route.ctf_base_valid[flag->team_index] = TRUE;
			bots_route.ctf_base[flag->team_index] = flag->position;
		}
		else if (flag->type == _netgame_flag_race_track && bots_route.race_flag_count < NUMBEROF(bots_route.race_flags))
		{
			bots_route.race_flag_numbers[bots_route.race_flag_count] = flag->team_index;
			bots_route.race_flags[bots_route.race_flag_count++] = flag->position;
		}
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
#if defined(OXBOX_TEST_BOTS)
	for (from = 0; from < bots_route.waypoint_count; from++)
	{
		bots_log("bots: waypoint %d at (%.1f %.1f %.1f), %d links", (int)from, bots_route.waypoints[from].point.x,
			bots_route.waypoints[from].point.y, bots_route.waypoints[from].point.z, (int)bots_route.waypoints[from].link_count);
	}
#endif
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
	brain->goal_object = NONE;
	brain->goal_hunt_unit = NONE;
	brain->goal_waypoint = NONE;
	brain->item_object = NONE;
	brain->vehicle_object = NONE;
	brain->zoom = NONE;
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

	brain->straight = brain->straight_after = FALSE;
	/* (an objective first: the route to the waypoint nearest it) */
	if (brain->goal_kind && brain->goal_waypoint != NONE && bots_route_to(brain, position, brain->goal_waypoint, now))
		return TRUE;
	/* (team games with nothing else to do: now and then, toward a teammate, a person first) */
	if (!brain->goal_kind && game_engine_get_variant() && game_engine_get_variant()->universal_variant.teams &&
		bots_random() % 10 < 4)
	{
		struct player_datum *self = NULL, *mate = NULL;
		long index;

		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct player_datum *other = bots_player_at(index);

			if (other && other->unit_index == brain->unit_index)
				self = other;
		}
		for (index = 0; self && index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct player_datum *other = bots_player_at(index);
			real_point3d other_position;

			if (!other || other == self || other->team_index != self->team_index || other->unit_index == NONE ||
				!object_try_and_get(other->unit_index))
			{
				continue;
			}
			object_get_origin(other->unit_index, &other_position);
			if (bots_distance3d(position, &other_position) < 8.f)
				continue;
			if (!mate || (other->network_player_data.controller_index < NETWORK_BOT_CONTROLLER_FIRST &&
				mate->network_player_data.controller_index >= NETWORK_BOT_CONTROLLER_FIRST))
			{
				mate = other;
			}
		}
		if (mate)
		{
			real_point3d mate_position;
			short best = NONE;
			real best_distance = REAL_MAX;

			object_get_origin(mate->unit_index, &mate_position);
			for (index = 0; index < bots_route.waypoint_count; index++)
			{
				real distance = bots_weighted_distance(&mate_position, &bots_route.waypoints[index].point);

				if (!bots_route.waypoints[index].teleport_source && distance < best_distance)
				{
					best = (short)index;
					best_distance = distance;
				}
			}
			if (best != NONE && bots_route_to(brain, position, best, now))
			{
				bots_route.regroups++;
				return TRUE;
			}
		}
	}
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
	brain->straight = brain->straight_after = FALSE;
	/* (not straight away: a bot somewhere its nearest waypoints are behind walls would spend every tick's search) */
	if (brain->plan_retry_time < now + BOTS_HOP_FAILED_RETRY_TICKS)
		brain->plan_retry_time = now + BOTS_HOP_FAILED_RETRY_TICKS;
}

/* the hop finished straight for its waypoint, up a ledge or off one */
static void bots_go_straight(
	struct bots_brain *brain,
	real_point3d const *position,
	real_point3d const *waypoint)
{
	if (!brain->straight)
	{
		if (waypoint->z > position->z)
			bots_route.straight_up++;
		else
			bots_route.straight_down++;
	}
	brain->straight = TRUE;
	brain->path_valid = FALSE;
	brain->wants_search = FALSE;
	/* (a short time to make it: a ledge it can't get up, or a drop that isn't there, gives up soon) */
	brain->hop_deadline = game_time_get() + (long)((2.f + bots_distance3d(position, waypoint)) * BOTS_TICKS_PER_SECOND);
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
	brain->straight = FALSE;
	brain->straight_after = FALSE;
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
	if (closest > BOTS_ARRIVAL_DISTANCE && bots_distance2d(&result.endpoint.point, &waypoint->point) < BOTS_STRAIGHT_RANGE)
	{
		/* the corners end at the foot of a ledge the bot can crouch-jump onto, or at the edge of a drop it survives:
		they, then straight for the waypoint (the look-ahead jumps) */
		struct bots_body body;
		real rise = waypoint->point.z - result.endpoint.point.z;

		bots_body_get(unit_index, &body);
		if ((rise > BOTS_KNEE_HEIGHT && rise <= body.crouch_jump * BOTS_REACH_SAFETY) ||
			(rise < -0.5f && -rise <= body.safe_drop))
		{
			/* (and nothing in the way above the ledge's top, or at the waist over the edge: a wall or a railing
			there makes it no ledge to jump, or no edge to step off) */
			real_point3d over = result.endpoint.point;
			real_vector3d across;

			over.z += (rise > 0.f ? rise : 0.f) + 0.45f;
			across.i = waypoint->point.x - result.endpoint.point.x;
			across.j = waypoint->point.y - result.endpoint.point.y;
			across.k = 0.f;
			if (bots_ray(&over, &across, unit_index, NULL) < 1.f)
				goto not_straight;
			if (rise < 0.f)
			{
				/* (a drop: open above the waypoint, not a floor over it the bot would walk along) */
				real_point3d above = waypoint->point;
				real_vector3d down;

				above.z = result.endpoint.point.z + 0.5f;
				down.i = 0.f;
				down.j = 0.f;
				down.k = waypoint->point.z + 0.3f - above.z;
				if (bots_ray(&above, &down, unit_index, NULL) < 1.f)
					goto not_straight;
			}
			brain->straight_after = TRUE;
			brain->hop_stalls = 0;
			if (bots_distance2d(&start, &result.endpoint.point) < 2.f * BOTS_STEP_DISTANCE)
			{
				/* (already there) */
				bots_go_straight(brain, &start, &waypoint->point);
				return;
			}
			brain->path_valid = TRUE;
			brain->path_finishes = result.steps_finish_path;
			brain->step_count = result.step_count > 4 ? 4 : result.step_count;
			for (index = 0; index < brain->step_count; index++)
				brain->steps[index] = result.steps[index].point;
			brain->step_index = 0;
			return;
		}
	}
not_straight:
#if defined(OXBOX_TEST_BOTS)
	{
		/* (test builds: where the searches that won't finish end, for the jumps and ledges) */
		static long logged;
		static char logged_map[64];

		if (csstrncmp(logged_map, bots_route.map, sizeof(logged_map) - 1))
		{
			csstrncpy(logged_map, bots_route.map, sizeof(logged_map) - 1);
			logged = 0;
		}
		if (closest > BOTS_ARRIVAL_DISTANCE && !brain->straight_after &&
			(closest > bots_distance3d(&start, &waypoint->point) - 1.f ||
				(bots_distance2d(&result.endpoint.point, &waypoint->point) < 2.f &&
					(real)fabs(result.endpoint.point.z - waypoint->point.z) > 1.5f)) && logged++ < 60)
		{
			bots_log("bots: short search: from (%.2f %.2f %.2f) for (%.2f %.2f %.2f), ends at (%.2f %.2f %.2f): %.2f away, "
				"%.2f across, %.2f up", start.x, start.y, start.z, waypoint->point.x, waypoint->point.y, waypoint->point.z,
				result.endpoint.point.x, result.endpoint.point.y, result.endpoint.point.z, closest,
				bots_distance2d(&result.endpoint.point, &waypoint->point), waypoint->point.z - result.endpoint.point.z);
		}
	}
#endif
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
#if defined(OXBOX_TEST_BOTS)
	{
		static short reports;

		if (reports++ % 3 == 0)
		{
			struct object_iterator iterator;

			object_iterator_new(&iterator, _object_mask_vehicle, 0);
			while (object_iterator_next(&iterator))
			{
				struct object_datum *vehicle = object_get(iterator.index);
				struct bots_seats seats;

				bots_vehicle_seats(iterator.index, &seats);
				bots_log("bots: vehicle %lx type %d at (%.1f %.1f %.1f) up %.2f, driver %s, gunner %s, %d of %d other seats taken",
					iterator.index, (int)vehicle_bot_type(iterator.index), vehicle->object.position.x,
					vehicle->object.position.y, vehicle->object.position.z, vehicle->object.up.k,
					seats.driver == NONE ? "-" : seats.driver_unit == NONE ? "empty" : "in",
					seats.gunner == NONE ? "-" : seats.gunner_unit == NONE ? "empty" : "in",
					(int)(seats.passengers - seats.passengers_free), (int)seats.passengers);
			}
		}
	}
#endif
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
#if defined(OXBOX_TEST_BOTS)
		for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
		{
			struct bots_brain const *brain = &bots_brains[index];

			if (brain->unit_index == NONE || brain->active_time < now_ticks - BOTS_TICKS_PER_SECOND || !brain->goal_reason)
				continue;
			bots_log("bots: slot %d: goal %d (reason %d) at (%.1f %.1f %.1f), %.1f away, %s, %s, route %d/%d to %d, from (%.1f %.1f %.1f)",
				(int)index, (int)brain->goal_kind, (int)brain->goal_reason, brain->goal_point.x, brain->goal_point.y,
				brain->goal_point.z, bots_distance3d(&brain->last_position, &brain->goal_point),
				brain->goal_visible ? "in sight" : "out of sight", brain->goal_direct_since ? "straight" : "by route",
				(int)brain->route_position, (int)brain->route_count, (int)brain->destination,
				brain->last_position.x, brain->last_position.y, brain->last_position.z);
		}
#endif
		bots_log("bots 10s: %d planless, %d waiting for a search, %d without corners, %d stuck; "
			"longest since a waypoint %ld s", planless, waiting, straight, stuck, slowest / BOTS_TICKS_PER_SECOND);
		bots_log("bots 10s: fighting: %ld sightings, %ld ticks firing, %ld grenades, %ld melee, %ld weapon pickups; "
			"longest walk %.2f ms (search %.2f ms), longest fight %.2f ms",
			bots_route.sightings, bots_route.firing_ticks, bots_route.grenades, bots_route.melees, bots_route.pickups,
			(double)bots_route.walk_cycles_maximum / 733000.0, (double)bots_route.search_cycles_maximum / 733000.0,
			(double)bots_route.fight_cycles_maximum / 733000.0);
		bots_log("bots 10s: moving: %ld jumps (%ld crouch-jumps), %ld got over, %ld crouched under something; "
			"hops finished straight: %ld up a ledge, %ld off one, %ld reached; %ld under a ramp; look-ahead %.2f ms a tick",
			bots_route.jumps, bots_route.crouch_jumps, bots_route.jumps_landed, bots_route.crouches,
			bots_route.straight_up, bots_route.straight_down, bots_route.straight_done, bots_route.overhangs,
			(double)bots_route.probe_cycles / 733000.0 / (double)bots_route.ticks);
		if (game_engine_get_variant() && game_engine_get_variant()->game_engine_index != game_engine_slayer)
		{
			long const *counts = bots_route.goal_counts;

			/* (a count a third of a second: bot-seconds x 3) */
			bots_log("bots 10s: objectives (bot-seconds): %ld carrying, %ld taking, %ld returning, %ld hunting a carrier, "
				"%ld escorting, %ld defending, %ld on the hill or a race's flag, %ld with none",
				counts[_bots_reason_carry] / 3, counts[_bots_reason_take] / 3, counts[_bots_reason_return] / 3,
				counts[_bots_reason_hunt] / 3, counts[_bots_reason_escort] / 3, counts[_bots_reason_defend] / 3,
				counts[_bots_reason_hill] / 3, counts[_bots_reason_none] / 3);
		}
		bots_log("bots 10s: %s: %ld bot-seconds after power items (%ld taken), %ld backed off hurt, %ld headed for a teammate",
			bots_skill()->name, bots_route.goal_counts[_bots_reason_item] / 3, bots_route.items_taken, bots_route.retreats,
			bots_route.regroups);
		bots_route.items_taken = bots_route.retreats = bots_route.regroups = 0;
		bots_log("bots 10s: vehicles: %ld got in, %ld got out, %.1f bot-seconds driving, %.1f gunning, %ld reversed; "
			"%ld bot-seconds going for one", bots_route.vehicle_entries, bots_route.vehicle_exits,
			(double)bots_route.driving_ticks / BOTS_TICKS_PER_SECOND, (double)bots_route.gunning_ticks / BOTS_TICKS_PER_SECOND,
			bots_route.vehicle_reverses, bots_route.goal_counts[_bots_reason_vehicle] / 3);
		bots_route.vehicle_entries = bots_route.vehicle_exits = bots_route.driving_ticks = bots_route.gunning_ticks = 0;
		bots_route.vehicle_reverses = 0;
		bots_log("bots 10s: %.1f bot-seconds firing from vehicles, %ld drivers waited for a crew (%ld in vain); %ld weapon "
			"switches (%ld done), %ld reloads; %ld sniper looks; %ld got out of a vehicle's way",
			(double)bots_route.vehicle_firing_ticks / BOTS_TICKS_PER_SECOND, bots_route.crew_waits, bots_route.crew_gave_up,
			bots_route.weapon_switches, bots_route.weapon_switches_done, bots_route.reloads, bots_route.scouts,
			bots_route.evades);
		bots_log("bots 10s: %ld seats not got in time; %ld went round a parked vehicle", bots_route.seat_misses,
			bots_route.went_around);
		bots_route.crew_gave_up = bots_route.seat_misses = bots_route.went_around = 0;
		bots_route.vehicle_firing_ticks = bots_route.crew_waits = bots_route.weapon_switches = bots_route.reloads = 0;
		bots_route.weapon_switches_done = 0;
		bots_route.scouts = bots_route.evades = 0;
		csmemset(bots_route.goal_counts, 0, sizeof(bots_route.goal_counts));
		bots_route.jumps = bots_route.crouch_jumps = bots_route.jumps_landed = bots_route.crouches = 0;
		bots_route.straight_up = bots_route.straight_down = bots_route.straight_done = bots_route.overhangs = 0;
		bots_route.probe_cycles = 0;
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
	if (brain->straight)
		bots_route.straight_done++;
	brain->straight = brain->straight_after = FALSE;
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
		brain->straight = brain->straight_after = FALSE;
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

	/* an objective close by (or a spot to hold): straight for it, no route */
	if (bots_goal_walk(brain, &position, now))
		return;

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
		(!bots_teleport_hop(brain) &&
			bots_distance2d(&position, &waypoint->point) < (brain->driving ? BOTS_DRIVE_ARRIVAL : BOTS_ARRIVAL_DISTANCE) &&
			(real)fabs(position.z - waypoint->point.z) < (brain->driving ? 3.f : 1.5f)))
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
			/* (corners to the foot of a ledge, or the edge of a drop: then straight on) */
			if (brain->straight_after)
				bots_go_straight(brain, &position, hop_point);
			else
				bots_want_search(brain, now);
		}
	}
	else if (!brain->straight)
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
				/* (a crouch-jump: the most a biped climbs) */
				brain->jump = TRUE;
				brain->jump_crouch = TRUE;
				brain->jump_time = now;
				brain->jump_start_z = position.z;
				brain->jump_goal = 0.f;
				break;
			case 2:
				brain->sidestep_until = now + BOTS_SIDESTEP_TICKS;
				brain->sidestep_sign = (bots_random() & 1) ? 1.f : -1.f;
				brain->jump = TRUE;
				brain->jump_crouch = TRUE;
				brain->jump_time = now;
				brain->jump_start_z = position.z;
				brain->jump_goal = 0.f;
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

/* the object an object rides in at the top (itself when it's in nothing) */
static long bots_ultimate_parent(
	long object_index)
{
	long count;

	for (count = 0; count < 4 && object_get(object_index)->object.parent_object_index != NONE; count++)
		object_index = object_get(object_index)->object.parent_object_index;
	return object_index;
}

/* whether a bot sees a target from its eye: from inside a vehicle the ray starts past the vehicle (its own
vehicle blocked every look, so drivers and gunners never fired), and hitting the target's own vehicle counts as
seeing the target */
static boolean bots_can_see(
	real_point3d const *eye,
	real_point3d const *center,
	long viewer_unit,
	long target_unit)
{
	struct collision_result collision;
	real_point3d from = *eye;
	real_vector3d vector;
	long viewer_vehicle = bots_ultimate_parent(viewer_unit);
	long target_vehicle = bots_ultimate_parent(target_unit);
	real length;
	boolean blocked;

	vector.i = center->x - from.x;
	vector.j = center->y - from.y;
	vector.k = center->z - from.z;
	length = (real)sqrt(vector.i * vector.i + vector.j * vector.j + vector.k * vector.k);
	if (length < 0.01f)
		return TRUE;
	if (viewer_vehicle != viewer_unit)
	{
		real skip = object_get(viewer_vehicle)->object.bounding_sphere_radius + 0.2f;

		if (skip > 0.5f * length)
			skip = 0.5f * length;
		from.x += vector.i * skip / length;
		from.y += vector.j * skip / length;
		from.z += vector.k * skip / length;
		vector.i = center->x - from.x;
		vector.j = center->y - from.y;
		vector.k = center->z - from.z;
	}
	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return FALSE;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ai_lineofsight;
	blocked = collision_test_vector(_collision_test_for_line_of_sight_flags, &from, &vector,
		viewer_vehicle != viewer_unit ? viewer_vehicle : viewer_unit, &collision);
	--global_current_collision_user_depth;
	/* (object_index is only set for an object's hit: the level's leaves it as it was) */
	if (blocked && collision.type == _collision_result_object && collision.object_index != NONE &&
		(collision.object_index == target_unit || collision.object_index == target_vehicle ||
			bots_ultimate_parent(collision.object_index) == target_vehicle))
	{
		blocked = FALSE;
	}
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
		if (distance > bots_skill()->sight_range * (brain->zoom != NONE ? 1.8f : 1.f) || distance < 0.01f)
			continue;
		angle = (real)acos(PIN((di * facing_i + dj * facing_j + dk * facing_k) / distance, -1.f, 1.f));
		noticed = angle < bots_skill()->sight_angle || (distance < BOTS_SIDE_SIGHT_RANGE && angle < 2.f * bots_skill()->sight_angle) ||
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
		/* (an enemy carrying an objective first) */
		if (other->unit_index == brain->goal_hunt_unit)
			score *= 0.3f;
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
		struct player_datum *candidate = bots_player_at(candidates[index]);

		if (candidate && candidate->unit_index != NONE &&
			bots_can_see(&eye, &candidate_centers[index], player->unit_index, candidate->unit_index))
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

	brain->straight = brain->straight_after = FALSE;
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

/* a weapon's rounds: in its magazine, and the rest it carries (FALSE: it has no magazine, as a plasma weapon) */
static boolean bots_weapon_rounds(
	long weapon_index,
	long *loaded,
	long *total)
{
	struct weapon_datum *weapon;

	if (weapon_index == NONE || !object_try_and_get(weapon_index))
		return FALSE;
	weapon = weapon_get(weapon_index);
	if (!weapon_definition_get(weapon->definition_index)->weapon.magazines.count)
		return FALSE;
	*loaded = weapon->weapon.magazines[0].rounds_loaded;
	*total = weapon->weapon.magazines[0].rounds_total;
	return TRUE;
}

static long bots_weapon_magazine_size(
	long weapon_index)
{
	struct weapon_definition *definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);

	return definition->weapon.magazines.count ? TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, 0,
		struct weapon_magazine_definition)->rounds_loaded_maximum : 0;
}

/* Snipers (build 40): a bot holding a weapon that zooms 4x or more, with nothing
to fight and no objective in a hurry, now and then (every 15-30 seconds) stops
for 4-7 seconds, zooms in and sweeps its view across the longest open sight line
it finds; zoomed it sees enemies 1.8 times as far. In a fight, it zooms in on a
target more than 15 units away (the second zoom past 40). */
static void bots_scout(
	struct bots_brain *brain,
	struct unit_datum const *unit,
	long weapon_index,
	real distance,
	long now)
{
	struct weapon_definition *definition;
	long index;

	if (weapon_index == NONE || !object_try_and_get(weapon_index) || brain->seat_kind != _bots_seat_none)
	{
		brain->scout_until = 0;
		return;
	}
	definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
	if (definition->weapon.zoom_level_count <= 0 || definition->weapon.zoom_magnification_maximum < 4.f)
	{
		brain->scout_until = 0;
		return;
	}
	if (brain->target_visible)
	{
		brain->scout_until = 0;
		if (distance > 15.f)
			brain->zoom = distance > 40.f && definition->weapon.zoom_level_count > 1 ? 1 : 0;
		return;
	}
	if (brain->goal_urgent || brain->pickup_object != NONE || brain->goal_reason == _bots_reason_hill ||
		brain->goal_reason == _bots_reason_take || brain->goal_reason == _bots_reason_return ||
		brain->goal_reason == _bots_reason_vehicle || brain->target != NONE)
	{
		brain->scout_until = 0;
		return;
	}
	if (!brain->scout_until && now >= brain->scout_next)
	{
		/* (the longest open sight line of 4 around it, at eye height: 4 rays, once in a while) */
		real_point3d eye;
		real best = 0.f;

		unit_get_camera_position(brain->unit_index, &eye);
		brain->scout_center = brain->yaw;
		for (index = 0; index < 4; index++)
		{
			real yaw = brain->yaw + BOTS_PI / 4.f + (real)index * BOTS_PI / 2.f;
			real_vector3d vector;
			real t;

			vector.i = 45.f * (real)cos(yaw);
			vector.j = 45.f * (real)sin(yaw);
			vector.k = 0.f;
			t = bots_ray(&eye, &vector, brain->unit_index, NULL);
			if (t > best)
			{
				best = t;
				brain->scout_center = yaw;
			}
		}
		if (best > 0.4f)
		{
			brain->scout_until = now + 4 * BOTS_TICKS_PER_SECOND + (long)(bots_random() % (3 * BOTS_TICKS_PER_SECOND));
			bots_route.scouts++;
		}
		brain->scout_next = now + 15 * BOTS_TICKS_PER_SECOND + (long)(bots_random() % (15 * BOTS_TICKS_PER_SECOND));
	}
	if (brain->scout_until && now < brain->scout_until)
	{
		/* (still, zoomed, a slow sweep of a quarter turn either side) */
		real sweep = (real)sin((real)(now % (4 * BOTS_TICKS_PER_SECOND)) * 2.f * BOTS_PI / (4.f * BOTS_TICKS_PER_SECOND));

		brain->zoom = 0;
		brain->move_i = brain->move_j = 0.f;
		brain->walk_yaw = brain->scout_center + 0.8f * sweep;
		brain->walk_has_yaw = TRUE;
		brain->arrival_time = now;
		brain->progress_time = now;
		object_get_origin(brain->unit_index, &brain->progress_point);
		if (brain->hop_deadline)
			brain->hop_deadline++;
	}
	else
	{
		brain->scout_until = 0;
	}
}

/* Vehicles coming at it (build 40): on foot, every other tick, an enemy's
vehicle moving faster than 1.5 units a second whose path passes within its
size plus a unit of the bot in the next 1.5 seconds: the bot runs to the side
away from its path for a second, and jumps when it's about to be hit. */
static void bots_evade(
	struct bots_brain *brain,
	struct player_datum const *player,
	long unit_index,
	long now)
{
	if (brain->seat_kind != _bots_seat_none)
		return;
	if (now >= brain->evade_time)
	{
		struct object_iterator iterator;
		real_point3d position;

		brain->evade_time = now + 2;
		object_get_origin(unit_index, &position);
		object_iterator_new(&iterator, _object_mask_vehicle, 0);
		while (object_iterator_next(&iterator))
		{
			struct object_datum *vehicle = object_get(iterator.index);
			real_vector3d const *v = &vehicle->object.translational_velocity;
			real speed_squared = v->i * v->i + v->j * v->j;
			real pi, pj, t, ci, cj, miss;
			struct bots_seats seats;
			long team;

			if (speed_squared < 0.05f * 0.05f || vehicle->object.parent_object_index != NONE)
				continue;
			pi = position.x - vehicle->object.position.x;
			pj = position.y - vehicle->object.position.y;
			if (pi * pi + pj * pj > 20.f * 20.f || (real)fabs(position.z - vehicle->object.position.z) > 3.f)
				continue;
			t = (pi * v->i + pj * v->j) / speed_squared;
			if (t <= 0.f || t > 1.5f * BOTS_TICKS_PER_SECOND)
				continue;
			ci = pi - v->i * t;
			cj = pj - v->j * t;
			miss = (real)sqrt(ci * ci + cj * cj);
			if (miss > vehicle->object.bounding_sphere_radius + 1.f)
				continue;
			bots_vehicle_seats(iterator.index, &seats);
			team = bots_unit_team(seats.driver_unit);
			if (seats.driver_unit == NONE || team == NONE || !game_team_is_enemy((short)player->team_index, (short)team))
				continue;
			/* (away from its path: the side the bot's already on) */
			{
				real speed = (real)sqrt(speed_squared);
				real side_i = -v->j / speed, side_j = v->i / speed;

				if (ci * side_i + cj * side_j < 0.f)
				{
					side_i = -side_i;
					side_j = -side_j;
				}
				if (now >= brain->evade_until)
					bots_route.evades++;
				brain->evade_i = side_i;
				brain->evade_j = side_j;
				brain->evade_until = now + BOTS_TICKS_PER_SECOND;
				if (t < 6.f && now >= brain->jump_wait_until)
				{
					brain->jump = TRUE;
					brain->jump_crouch = TRUE;
					brain->jump_time = now;
					brain->jump_start_z = position.z;
					brain->jump_start_x = position.x;
					brain->jump_start_y = position.y;
					brain->jump_goal = 0.f;
					brain->jump_wait_until = now + BOTS_TICKS_PER_SECOND;
				}
			}
			break;
		}
	}
	if (now < brain->evade_until)
	{
		brain->move_i = brain->evade_i;
		brain->move_j = brain->evade_j;
		return;
	}
	/* a parked vehicle (not one it's going for) in its way: round it, to the side away from its middle */
	if (now >= brain->around_until && (now & 1))
	{
		real length = (real)sqrt(brain->move_i * brain->move_i + brain->move_j * brain->move_j);
		struct object_iterator iterator;
		real_point3d position;

		if (length > 0.3f)
		{
			real mi = brain->move_i / length, mj = brain->move_j / length;

			object_get_origin(unit_index, &position);
			object_iterator_new(&iterator, _object_mask_vehicle, 0);
			while (object_iterator_next(&iterator))
			{
				struct object_datum *vehicle = object_get(iterator.index);
				real radius = vehicle->object.bounding_sphere_radius;
				real di = vehicle->object.position.x - position.x, dj = vehicle->object.position.y - position.y;
				real along = di * mi + dj * mj, across = mi * dj - mj * di;

				if (iterator.index == brain->vehicle_object || vehicle->object.parent_object_index != NONE)
					continue;
				radius = 0.7f * (radius < 4.f ? radius : 4.f) + 0.4f;
				if (along <= 0.f || along > radius + 1.5f || (real)fabs(across) > radius ||
					(real)fabs(vehicle->object.position.z - position.z) > 2.5f)
				{
					continue;
				}
				/* (left of the way: round it to the right, and the other way) */
				brain->around_i = across > 0.f ? mj : -mj;
				brain->around_j = across > 0.f ? -mi : mi;
				brain->around_i = 0.95f * brain->around_i + 0.3f * mi;
				brain->around_j = 0.95f * brain->around_j + 0.3f * mj;
				brain->around_until = now + 12;
				bots_route.went_around++;
				break;
			}
		}
	}
	if (now < brain->around_until)
	{
		brain->move_i = brain->around_i;
		brain->move_j = brain->around_j;
	}
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
			brain->target_sighted_time = now + bots_skill()->reaction_ticks + (long)(bots_random() % 5);
			brain->aim_error_yaw = bots_skill()->aim_error * bots_random_signed();
			brain->aim_error_pitch = 0.5f * bots_skill()->aim_error * bots_random_signed();
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
		brain->wanted_weapon = best != NONE && best != unit->unit.current_weapon_index && best_score > current_score * 1.1f ?
			best : NONE;
		{
			/* (the one in the pocket when the one in hand is out of ammunition, or empty with an enemy close: a switch
			is quicker than a reload) */
			short current = unit->unit.current_weapon_index;
			short slot;

			for (slot = 0; brain->wanted_weapon == NONE && slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			{
				long loaded, total, other_loaded, other_total;

				if (slot == current || current < 0 || current >= MAXIMUM_WEAPONS_PER_UNIT ||
					!bots_weapon_rounds(unit->unit.weapon_object_indices[current], &loaded, &total) ||
					!bots_weapon_rounds(unit->unit.weapon_object_indices[slot], &other_loaded, &other_total))
				{
					continue;
				}
				if ((loaded + total == 0 && other_loaded + other_total > 0) ||
					(loaded == 0 && other_loaded > 0 && brain->target_visible && distance < 10.f))
				{
					brain->wanted_weapon = slot;
				}
			}
		}
		if (brain->wanted_weapon != NONE && unit->object.parent_object_index == NONE &&
			brain->wanted_weapon != brain->last_wanted_weapon)
		{
			bots_route.weapon_switches++;
		}
		brain->last_wanted_weapon = brain->wanted_weapon;
		/* (carrying a flag or a ball: a switch would drop it; in a seat: the seat's weapon is the vehicle's) */
		if (bots_carries_objective(unit) || unit->object.parent_object_index != NONE)
			brain->wanted_weapon = NONE;
	}
	if (unit->object.parent_object_index != NONE)
		brain->wanted_weapon = NONE;
	if (brain->wanted_weapon != NONE && brain->wanted_weapon != unit->unit.current_weapon_index)
		action->desired_weapon_index = brain->wanted_weapon;
	weapon_index = unit->unit.current_weapon_index >= 0 && unit->unit.current_weapon_index < MAXIMUM_WEAPONS_PER_UNIT ?
		unit->unit.weapon_object_indices[unit->unit.current_weapon_index] : NONE;
	{
		/* (out of a fight with a magazine under half: a reload, as a player would) */
		long loaded, total, maximum;

		if (brain->target == NONE && now >= brain->reload_next && brain->seat_kind == _bots_seat_none &&
			bots_weapon_rounds(weapon_index, &loaded, &total) && total > 0 &&
			(maximum = bots_weapon_magazine_size(weapon_index)) > 0 && loaded * 2 < maximum)
		{
			/* (a press, then 3 seconds for the reload to run) */
			action->control_flags |= FLAG(_unit_control_weapon_reload_bit);
			brain->reload_next = now + 3 * BOTS_TICKS_PER_SECOND;
			bots_route.reloads++;
		}
	}
	if (unit->unit.current_weapon_index != brain->last_weapon_slot)
	{
		if (brain->wanted_weapon != NONE && unit->unit.current_weapon_index == brain->wanted_weapon)
			bots_route.weapon_switches_done++;
		brain->last_weapon_slot = unit->unit.current_weapon_index;
	}
	/* a sniper's zoom: out at a distance, looking out over the map or shooting far */
	brain->zoom = NONE;
	bots_scout(brain, unit, weapon_index, distance, now);
	action->desired_zoom_level = brain->zoom;

	/* aiming and firing (not a Warthog's driver: it steers) */
	if (brain->target_visible && (!brain->driving || brain->driver_aims))
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
		brain->aim_error_yaw = brain->aim_error_yaw * bots_skill()->aim_settle + 0.004f * bots_random_signed();
		brain->aim_error_pitch = brain->aim_error_pitch * bots_skill()->aim_settle + 0.002f * bots_random_signed();
		yaw_error = bots_angle_difference(aim_yaw, brain->yaw);
		pitch_error = aim_pitch - brain->pitch;
		brain->yaw += PIN(yaw_error, -bots_skill()->aim_turn, bots_skill()->aim_turn);
		brain->pitch += PIN(pitch_error, -bots_skill()->aim_turn * 0.67f, bots_skill()->aim_turn * 0.67f);
		tolerance = (real)atan2(0.35f * bots_skill()->fire_tolerance, distance > 0.5f ? distance : 0.5f);
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
			!brain->goal_urgent &&
			(unit->unit.grenade_counts[0] > 0 || unit->unit.grenade_counts[1] > 0))
		{
			/* (a grenade: thrown a little high, for its arc; of the other kind when the one in hand has run out) */
			short current = unit->unit.current_grenade_index;

			if (current < 0 || current >= NUMBER_OF_UNIT_GRENADE_TYPES || unit->unit.grenade_counts[current] <= 0)
				action->desired_grenade_index = unit->unit.grenade_counts[0] > 0 ? 0 : 1;
			action->control_flags |= FLAG(_unit_control_throw_grenade_bit) | FLAG(_unit_control_weapon_secondary_trigger_bit);
			brain->pitch += 0.12f;
			brain->grenade_time = now + bots_skill()->grenade_ticks + (long)(bots_random() % (6 * BOTS_TICKS_PER_SECOND));
			bots_route.grenades++;
		}
		else if (now >= brain->target_sighted_time && weapon_index != NONE && object_try_and_get(weapon_index))
		{
			if (brain->seat_kind != _bots_seat_none)
				bots_route.vehicle_firing_ticks++;
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
			if (unit->object.shield_vitality < 0.05f && unit->object.body_vitality < 0.5f && !brain->goal_urgent &&
				now >= brain->retreat_until + 3 * BOTS_TICKS_PER_SECOND)
			{
				/* (hurt: back off for a while) */
				brain->retreat_until = now + 2 * BOTS_TICKS_PER_SECOND;
				bots_route.retreats++;
			}
			if (now < brain->retreat_until)
			{
				move_i = -to_i * 0.8f - to_j * brain->strafe_sign * 0.5f;
				move_j = -to_j * 0.8f + to_i * brain->strafe_sign * 0.5f;
			}
			else if (brain->goal_urgent || brain->goal_reason == _bots_reason_take || brain->goal_reason == _bots_reason_return)
			{
				/* (carrying an objective, or about to: on for it, a little strafe) */
				move_i = brain->move_i - to_j * brain->strafe_sign * 0.25f;
				move_j = brain->move_j + to_i * brain->strafe_sign * 0.25f;
			}
			else if (distance > preferred)
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
		if (brain->target != NONE && !brain->chasing && now - brain->target_seen_time >= BOTS_TICKS_PER_SECOND &&
			!brain->goal_kind && brain->seat_kind == _bots_seat_none)
		{
			brain->chasing = TRUE;
			bots_head_for(brain, &position, &brain->target_position, now);
		}
		/* a better weapon nearby, out of a fight (and not on the way to an objective) */
		if (brain->target == NONE && now >= brain->pickup_time && !brain->goal_urgent && brain->seat_kind == _bots_seat_none &&
			(!brain->goal_kind || brain->goal_reason == _bots_reason_defend || brain->goal_reason == _bots_reason_item))
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

/* ---------- objectives (milestone 6)

What the game type asks for, on top of fighting and walking the map. Every
third of a second each bot picks a goal (bots_objective): a point to touch, or
a spot to hold. Within 12 world units, on the same floor and in a straight line
(or once at the waypoint nearest it), the bot walks straight for it
(bots_goal_walk); further away, its route goes to the waypoint nearest it.

- Capture the flag: a bot carrying the enemy flag takes it home to its own
  flag's base. A team's flag dropped somewhere: its bots go and touch it (it
  goes home); carried off: they hunt the carrier (seen first in a fight). Every
  third bot of a team defends (holds its base while its flag is home); the
  others go for the enemy flag, or escort the teammate carrying it. With one
  flag (assault), the flag's team defends and the other attacks.
- Oddball: the bot carrying a ball keeps moving round the map (no stopping to
  fight, no weapon pickups); the others go for a ball on the ground, hunt the
  enemy carrying one, or escort the teammate who has it.
- King of the hill: everyone holds the hill the game counts now (it moves with
  a moving hill).
- Race: the nearest flag the bot can touch now (the next one, or any one left
  this lap, by the race's type).

A bot carrying an objective never switches weapons or throws grenades (a switch
would drop it). Slayer has no goal. */

#define BOTS_GOAL_TICKS 10
#define BOTS_GOAL_NEAR 12.f                 /* walking straight for a goal this close (and in sight) */
#define BOTS_GOAL_DIRECT_TICKS (8 * BOTS_TICKS_PER_SECOND)
#define BOTS_GOAL_BLOCKED_TICKS (5 * BOTS_TICKS_PER_SECOND)

/* the race's and the hill's (game_engine_race.c, game_engine_king.c) */
boolean game_engine_race_bot_can_touch(long player_index, long flag_number);
boolean game_engine_king_bot_hill(real_point3d *center, real *radius);

/* whose unit carries a weapon (an absolute player index, NONE for none) */
static long bots_carrier(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_get(weapon_index);
	struct unit_datum *unit;

	if (weapon->object.parent_object_index == NONE ||
		!(unit = (struct unit_datum *)object_try_and_get(weapon->object.parent_object_index)) ||
		!TEST_FLAG(_object_mask_unit, unit->object.type) || unit->unit.player_index == NONE)
	{
		return NONE;
	}
	return DATUM_INDEX_TO_ABSOLUTE_INDEX(unit->unit.player_index);
}

/* whether the unit carries an objective (a flag, a ball) */
static boolean bots_carries_objective(
	struct unit_datum const *unit)
{
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		if (weapon_index != NONE && object_try_and_get(weapon_index) && weapon_is_flag(weapon_index))
			return TRUE;
	}
	return FALSE;
}

static void bots_set_goal(
	struct bots_brain *brain,
	short kind,
	short reason,
	real_point3d const *point,
	real radius,
	long object_index)
{
	if (brain->goal_kind != kind || bots_distance3d(point, &brain->goal_point) > 3.f)
	{
		/* (a new goal, or one gone far: a route to it now) */
		brain->goal_plan_time = 0;
		brain->goal_direct_since = 0;
		brain->hold_until = 0;
	}
	brain->goal_kind = kind;
	brain->goal_reason = reason;
	brain->goal_point = *point;
	brain->goal_radius = radius;
	brain->goal_object = object_index;
}

/* the bot's place among its team's bots (0 first): every third defends */
static short bots_team_rank(
	long absolute_player_index,
	struct player_datum const *player)
{
	short rank = 0;
	long index;

	for (index = 0; index < absolute_player_index; index++)
	{
		struct player_datum *other = bots_player_at(index);

		if (other && other->team_index == player->team_index &&
			other->network_player_data.controller_index >= NETWORK_BOT_CONTROLLER_FIRST)
		{
			rank++;
		}
	}
	return rank;
}

/* game_engine_ctf.c's (port: the bots') */
boolean game_engine_ctf_bot_flag(long team_index, real_point3d *spot, long *weapon_index, boolean *at_home);

static void bots_goal_ctf(
	struct bots_brain *brain,
	long absolute_player_index,
	struct player_datum const *player,
	struct unit_datum const *unit,
	real_point3d const *position)
{
	long flags[2] = { NONE, NONE };
	long carriers[2] = { NONE, NONE };
	real_point3d flag_points[2], spots[2];
	boolean at_home[2] = { FALSE, FALSE }, spot_valid[2];
	short team = (short)player->team_index;
	short other = (short)(1 - team);
	boolean defender;
	short index;

	if (team < 0 || team > 1)
		return;
	/* (the game's own: each team's flag spot, where its flag goes home to and where it scores, its flag, and
	whether the flag's at home: untouched since it was put there. A map with more than one flag spot a team, or a
	game that swaps them, threw the bots' own guess off: they kept going back to a flag that was home) */
	for (index = 0; index < 2; index++)
	{
		spot_valid[index] = game_engine_ctf_bot_flag(index, &spots[index], &flags[index], &at_home[index]);
		if (flags[index] != NONE)
		{
			item_get_position_even_if_in_inventory(flags[index], &flag_points[index]);
			carriers[index] = bots_carrier(flags[index]);
		}
	}
	/* carrying the enemy flag: home with it */
	if (flags[other] != NONE && carriers[other] == absolute_player_index && spot_valid[team])
	{
		bots_set_goal(brain, _bots_goal_touch, _bots_reason_carry, &spots[team], 0.f, NONE);
		brain->goal_urgent = TRUE;
		return;
	}
	defender = flags[other] == NONE ? TRUE : flags[team] == NONE ? FALSE :
		bots_team_rank(absolute_player_index, player) % 3 == 1;
	/* its team's flag away from home: hunt the carrier, or touch the flag (the defenders, and anyone near) */
	if (flags[team] != NONE && !at_home[team])
	{
		if (carriers[team] != NONE)
		{
			struct player_datum *carrier = bots_player_at(carriers[team]);

			if (carrier && carrier->unit_index != NONE && (defender || bots_distance3d(position, &flag_points[team]) < 30.f ||
				flags[other] == NONE))
			{
				object_get_origin(carrier->unit_index, &flag_points[team]);
				bots_set_goal(brain, _bots_goal_touch, _bots_reason_hunt, &flag_points[team], 0.f, carrier->unit_index);
				brain->goal_hunt_unit = carrier->unit_index;
				return;
			}
		}
		else if (defender || bots_distance3d(position, &flag_points[team]) < 25.f || flags[other] == NONE)
		{
			bots_set_goal(brain, _bots_goal_touch, _bots_reason_return, &flag_points[team], 0.f, flags[team]);
			return;
		}
	}
	if (defender && spot_valid[team])
	{
		bots_set_goal(brain, _bots_goal_hold, _bots_reason_defend, &spots[team], 6.f, NONE);
		return;
	}
	if (flags[other] == NONE)
		return;
	/* attacking: the enemy flag, or escorting the teammate who has it */
	if (carriers[other] != NONE)
	{
		struct player_datum *carrier = bots_player_at(carriers[other]);

		if (carrier && carrier->unit_index != NONE)
		{
			object_get_origin(carrier->unit_index, &flag_points[other]);
			bots_set_goal(brain, _bots_goal_hold, _bots_reason_escort, &flag_points[other], 5.f, carrier->unit_index);
		}
		return;
	}
	bots_set_goal(brain, _bots_goal_touch, _bots_reason_take, &flag_points[other], 0.f, flags[other]);
	(void)unit;
}

static void bots_goal_oddball(
	struct bots_brain *brain,
	long absolute_player_index,
	struct player_datum const *player,
	struct unit_datum const *unit,
	real_point3d const *position)
{
	struct object_iterator iterator;
	boolean teams = game_engine_get_variant()->universal_variant.teams;
	long best = NONE;
	short best_reason = _bots_reason_none;
	real best_distance = REAL_MAX;
	real_point3d best_point;
	long best_object = NONE;

	if (bots_carries_objective(unit))
	{
		/* (round the map with it: the walk's own destinations) */
		brain->goal_urgent = TRUE;
		brain->goal_reason = _bots_reason_carry;
		return;
	}
	object_iterator_new(&iterator, _object_mask_weapon, 0);
	while (object_iterator_next(&iterator))
	{
		long carrier_index;
		real_point3d point;
		short reason;
		long object_index = iterator.index;
		real distance;

		if (!weapon_is_flag(iterator.index))
			continue;
		item_get_position_even_if_in_inventory(iterator.index, &point);
		carrier_index = bots_carrier(iterator.index);
		if (carrier_index == NONE)
		{
			reason = _bots_reason_take;
		}
		else
		{
			struct player_datum *carrier = bots_player_at(carrier_index);

			if (!carrier || carrier->unit_index == NONE)
				continue;
			object_index = carrier->unit_index;
			object_get_origin(carrier->unit_index, &point);
			reason = teams && carrier->team_index == player->team_index ? _bots_reason_escort : _bots_reason_hunt;
		}
		distance = bots_distance3d(position, &point);
		if (distance < best_distance)
		{
			best = iterator.index;
			best_reason = reason;
			best_distance = distance;
			best_point = point;
			best_object = object_index;
		}
	}
	if (best == NONE)
		return;
	if (best_reason == _bots_reason_escort)
	{
		bots_set_goal(brain, _bots_goal_hold, best_reason, &best_point, 5.f, best_object);
	}
	else
	{
		bots_set_goal(brain, _bots_goal_touch, best_reason, &best_point, 0.f, best_object);
		if (best_reason == _bots_reason_hunt)
			brain->goal_hunt_unit = best_object;
	}
	(void)absolute_player_index;
}

static void bots_goal_race(
	struct bots_brain *brain,
	long absolute_player_index,
	struct player_datum const *player,
	real_point3d const *position)
{
	long player_index = DATUM_INDEX_NEW(absolute_player_index, ((struct datum_header *)player)->identifier);
	short best = NONE;
	real best_distance = REAL_MAX;
	short index;

	for (index = 0; index < bots_route.race_flag_count; index++)
	{
		real distance = bots_distance3d(position, &bots_route.race_flags[index]);

		if (distance < best_distance && game_engine_race_bot_can_touch(player_index, bots_route.race_flag_numbers[index]))
		{
			best = index;
			best_distance = distance;
		}
	}
	if (best != NONE)
		bots_set_goal(brain, _bots_goal_touch, _bots_reason_hill, &bots_route.race_flags[best], 0.f, NONE);
}

/* ---------- fighting smarter (milestone 7)

Out of an objective (Slayer, a defender, nothing else to do), every 3 seconds
a bot looks for something worth going out of its way for, within 30 world units:
a powerup (overshield, camouflage; a health pack when its body is hurt), or a
weapon on the ground much better than both of its own (a rocket launcher, a
sniper rifle). Worth over distance; the walk to it is an objective's
(bots_goal_walk), and the weapon pickup picks it up there. A bot whose shield
is gone and whose body is under half backs off from what it fights for 2
seconds (and goes for a health pack, when there's one). In team games with
nothing else to do, a bot now and then heads for a teammate (a person first)
rather than an empty part of the map. */

#define BOTS_ITEM_RANGE 20.f
#define BOTS_ITEM_LOOK_TICKS (3 * BOTS_TICKS_PER_SECOND)
#define BOTS_ITEM_REST_TICKS (15 * BOTS_TICKS_PER_SECOND)  /* after going for one: none for a while */
#define BOTS_ITEM_TICKS (12 * BOTS_TICKS_PER_SECOND)

/* the worth of an item on the ground for this bot (0: none) */
static real bots_item_worth(
	struct unit_datum const *unit,
	long object_index,
	real best_carried)
{
	struct object_datum *object = object_get(object_index);

	if (object->object.parent_object_index != NONE)
		return 0.f;
	if (object->object.type == _object_type_equipment)
	{
		struct equipment_definition *definition = equipment_definition_get(object->definition_index);

		switch (definition->equipment.powerup_type)
		{
		case _equipment_powerup_overshield: return unit->object.shield_vitality > 1.01f ? 0.f : 100.f;
		case _equipment_powerup_active_camouflage:
			/* (not while its own camouflage lasts: one bot stood on a new one waiting for its own to run out) */
			return TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit) || unit->unit.active_camouflage > 0.05f ? 0.f : 80.f;
		case _equipment_powerup_health: return unit->object.body_vitality < 0.6f ? 120.f * (1.f - unit->object.body_vitality) : 0.f;
		}
		return 0.f;
	}
	if (object->object.type == _object_type_weapon && !weapon_is_flag(object_index))
	{
		real score = bots_weapon_score(object_index, 10.f);
		short slot;

		/* (not one like a weapon it has: walking over that only takes its ammunition) */
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long carried = unit->unit.weapon_object_indices[slot];

			if (carried != NONE && object_try_and_get(carried) && object_get(carried)->definition_index == object->definition_index)
				return 0.f;
		}
		return score > best_carried * 2.f && score > 1.f ? 60.f : 0.f;
	}
	return 0.f;
}

/* a power item worth the walk (NONE: none) */
static long bots_find_item(
	struct unit_datum const *unit,
	real_point3d const *position)
{
	struct object_iterator iterator;
	real best_carried = 0.f, best_value = 0.f;
	long best = NONE;
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		if (weapon_index != NONE && object_try_and_get(weapon_index))
		{
			real score = bots_weapon_score(weapon_index, 10.f);

			if (score > best_carried)
				best_carried = score;
		}
	}
	object_iterator_new(&iterator, _object_mask_equipment | _object_mask_weapon, 0);
	while (object_iterator_next(&iterator))
	{
		real_point3d origin;
		real distance, value;

		object_get_origin(iterator.index, &origin);
		distance = bots_distance3d(position, &origin);
		if (distance > BOTS_ITEM_RANGE)
			continue;
		{
			/* (one bot an item: this one's looking, so it has none) */
			long index;

			for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
			{
				if (bots_brains[index].item_object == iterator.index && bots_brains[index].unit_index != NONE)
				{
					break;
				}
			}
			if (index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS)
				continue;
		}
		value = bots_item_worth(unit, iterator.index, best_carried);
		if (value <= 0.f)
			continue;
		value /= 5.f + distance;
		if (value > best_value)
		{
			best = iterator.index;
			best_value = value;
		}
	}
	return best;
}

/* out of an objective: an item worth the walk */
static void bots_goal_item(
	struct bots_brain *brain,
	struct unit_datum const *unit,
	real_point3d const *position,
	long now)
{
	real_point3d origin;

	if (brain->item_object != NONE &&
		(!object_try_and_get(brain->item_object) || object_get(brain->item_object)->object.parent_object_index != NONE ||
			now > brain->item_until))
	{
		if (!object_try_and_get(brain->item_object) || object_get(brain->item_object)->object.parent_object_index != NONE)
			bots_route.items_taken++;
		brain->item_object = NONE;
		brain->item_time = now + BOTS_ITEM_REST_TICKS;
	}
	if (unit->object.parent_object_index != NONE)
	{
		/* (in a vehicle: nothing to pick up) */
		brain->item_object = NONE;
		return;
	}
	if (brain->item_object == NONE && now >= brain->item_time)
	{
		brain->item_time = now + BOTS_ITEM_LOOK_TICKS;
		brain->item_object = bots_find_item(unit, position);
		brain->item_until = now + BOTS_ITEM_TICKS;
	}
	if (brain->item_object == NONE)
		return;
	object_get_origin(brain->item_object, &origin);
	bots_set_goal(brain, _bots_goal_touch, _bots_reason_item, &origin, 0.f, brain->item_object);
}

/* ---------- vehicles (milestone 8)

A bot on foot looks for a vehicle every 2 seconds (not while carrying an
objective, holding a hill or defending): an empty one within 15 world units to
drive when where it's going is more than 25 away; a teammate's Warthog with
its gun free to man; a turret to man when it has seen an enemy in the last 5
seconds. The walk to it is an objective's, to the side of the vehicle the seat
is on; there the game offers the seat (the action button's "enter"), and the bot
presses the button. 10 seconds without getting in: given up.

Crews (build 40): a teammate's free gun is what a bot wants most (within 25),
then a vehicle to drive, then a teammate's passenger seat (within 12). A
Warthog's driver waits for its gunner before it goes anywhere (20 seconds, or 6
with no teammate on foot within 25; then it gets out, and no bot drives that
Warthog for 30 seconds); a bot only takes a Warthog's wheel with a teammate on
foot within 20 of it, and any driver waits up to 8 seconds while
teammates are on their way to its seats. In free for all a bot doesn't drive a
vehicle with a separate gun. Passengers stay in while there's a driver. A bot in
a vehicle sees past its own vehicle (it used to block every look, so drivers
and gunners never fired), and an enemy's vehicle counts as seeing the enemy.

Driving: Halo's vehicles steer toward where the camera faces, so the bot faces
the way its route goes (its waypoints, 3 world units counting as there) and
holds the throttle forward (slower into sharp turns). A Ghost's, a Banshee's or
a Scorpion's driver also aims its guns at what it fights (and so drives at it);
a Warthog's driver keeps to its route. A Banshee climbs or dives toward a
waypoint's height plus 2. Stuck for 3 seconds: reverse for 1.5; stuck twice, or
the vehicle on its side: out. Gunning (a Warthog's gun, a turret): the fight's
aiming and firing, no walking; out when the Warthog has had no driver for 5
seconds, or a turret has had nothing to shoot for 15. A passenger seat: out at
once. */

#define BOTS_VEHICLE_RANGE 15.f
#define BOTS_SEAT_RANGE 25.f                    /* a teammate's free gun this far away */
#define BOTS_PASSENGER_RANGE 12.f
#define BOTS_VEHICLE_TRIP 25.f                  /* driving only for a trip longer than this */
#define BOTS_CREW_WAIT_TICKS (20 * BOTS_TICKS_PER_SECOND)      /* a Warthog's driver waits this long for a gunner */
#define BOTS_PASSENGER_WAIT_TICKS (8 * BOTS_TICKS_PER_SECOND)  /* ... and this long for others on their way */
#define BOTS_VEHICLE_LOOK_TICKS (2 * BOTS_TICKS_PER_SECOND)
#define BOTS_VEHICLE_TRY_TICKS (10 * BOTS_TICKS_PER_SECOND)
#define BOTS_VEHICLE_REST_TICKS (15 * BOTS_TICKS_PER_SECOND)     /* after a seat it didn't get: none for a while */

/* vehicles.c's (port: the bots') */
short vehicle_bot_type(long vehicle_index);
#define BOTS_VEHICLE_TYPE_ALIEN_FIGHTER 5
#define BOTS_VEHICLE_TYPE_TURRET 6

static short bots_seat_kind(
	long vehicle_index,
	short seat_index)
{
	if (seat_index < 0)
		return _bots_seat_none;
	if (unit_seat_is_driver(vehicle_index, seat_index))
		return _bots_seat_driver;
	if (unit_seat_is_gunner(vehicle_index, seat_index))
		return _bots_seat_gunner;
	return _bots_seat_other;
}

/* whether a vehicle has a gunner's seat apart from its driver's (a Warthog: the driver doesn't aim the gun) */
static boolean bots_vehicle_has_gunner_seat(
	long vehicle_index)
{
	struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);
	short seat;

	for (seat = 0; seat < definition->unit.seats.count; seat++)
	{
		if (unit_seat_is_gunner(vehicle_index, seat) && !unit_seat_is_driver(vehicle_index, seat))
			return TRUE;
	}
	return FALSE;
}

/* a vehicle's seats (struct bots_seats, up with the prototypes) */
static void bots_vehicle_seats(
	long vehicle_index,
	struct bots_seats *seats)
{
	struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);
	struct object_iterator units;
	struct unit_datum *rider;
	long riders[16];
	short seat;

	for (seat = 0; seat < NUMBEROF(riders); seat++)
		riders[seat] = NONE;
	object_iterator_new(&units, _object_mask_unit, 0);
	while ((rider = object_iterator_next(&units)) != NULL)
	{
		if (rider->object.parent_object_index == vehicle_index && rider->unit.parent_seat_index >= 0 &&
			rider->unit.parent_seat_index < NUMBEROF(riders))
		{
			riders[rider->unit.parent_seat_index] = units.index;
		}
	}
	seats->driver = seats->gunner = NONE;
	seats->driver_unit = seats->gunner_unit = NONE;
	seats->passengers = seats->passengers_free = 0;
	for (seat = 0; seat < definition->unit.seats.count && seat < NUMBEROF(riders); seat++)
	{
		short kind = bots_seat_kind(vehicle_index, seat);

		if (kind == _bots_seat_driver && seats->driver == NONE)
		{
			seats->driver = seat;
			seats->driver_unit = riders[seat];
		}
		else if (kind == _bots_seat_gunner && seats->gunner == NONE)
		{
			seats->gunner = seat;
			seats->gunner_unit = riders[seat];
		}
		else if (kind == _bots_seat_other)
		{
			seats->passengers++;
			if (riders[seat] == NONE)
				seats->passengers_free++;
		}
	}
}

/* a unit's player's team (NONE: no player's) */
static long bots_unit_team(
	long unit_index)
{
	struct unit_datum *unit = unit_index != NONE ? (struct unit_datum *)object_try_and_get(unit_index) : NULL;
	struct player_datum *player = unit && unit->unit.player_index != NONE ?
		bots_player_at(DATUM_INDEX_TO_ABSOLUTE_INDEX(unit->unit.player_index)) : NULL;

	return player ? player->team_index : NONE;
}

/* how many other bots are going for a vehicle's seats of a kind */
static short bots_seat_takers(
	struct bots_brain const *brain,
	long vehicle_index,
	short seat_kind)
{
	short count = 0;
	long index;

	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct bots_brain const *other = &bots_brains[index];

		if (other != brain && other->unit_index != NONE && other->vehicle_object == vehicle_index &&
			(seat_kind == NONE || other->vehicle_seat == seat_kind))
		{
			count++;
		}
	}
	return count;
}

#define BOTS_CREWLESS_TICKS (30 * BOTS_TICKS_PER_SECOND)

static void bots_crewless_add(
	long vehicle_index,
	long now)
{
	short index, oldest = 0;

	for (index = 0; index < NUMBEROF(bots_route.crewless); index++)
	{
		if (bots_route.crewless[index].vehicle == vehicle_index || bots_route.crewless[index].until < now)
		{
			oldest = index;
			break;
		}
		if (bots_route.crewless[index].until < bots_route.crewless[oldest].until)
			oldest = index;
	}
	bots_route.crewless[oldest].vehicle = vehicle_index;
	bots_route.crewless[oldest].until = now + BOTS_CREWLESS_TICKS;
}

static boolean bots_crewless(
	long vehicle_index,
	long now)
{
	short index;

	for (index = 0; index < NUMBEROF(bots_route.crewless); index++)
	{
		/* (a time more than the wait away: an earlier game's) */
		if (bots_route.crewless[index].vehicle == vehicle_index && bots_route.crewless[index].until > now &&
			bots_route.crewless[index].until - now <= BOTS_CREWLESS_TICKS)
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* whether a teammate on foot (not the bot itself, not one guarding or carrying an objective) is within a distance:
someone to man a gun */
static boolean bots_teammate_near(
	long absolute_player_index,
	struct player_datum const *player,
	real_point3d const *position,
	real range)
{
	long index;

	for (index = 0; index < HALO_PORT_MAXIMUM_NETWORK_PLAYERS; index++)
	{
		struct player_datum *other = index != absolute_player_index ? bots_player_at(index) : NULL;
		struct unit_datum *unit;
		real_point3d origin;

		if (!other || other->team_index != player->team_index || other->unit_index == NONE ||
			!(unit = (struct unit_datum *)object_try_and_get(other->unit_index)) ||
			unit->object.parent_object_index != NONE || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		{
			continue;
		}
		if (bots_brains[index].unit_index == other->unit_index &&
			(bots_brains[index].goal_reason == _bots_reason_carry || bots_brains[index].goal_reason == _bots_reason_defend ||
				bots_brains[index].goal_reason == _bots_reason_hill ||
				(bots_brains[index].vehicle_object != NONE && bots_brains[index].vehicle_seat == _bots_seat_driver)))
		{
			continue;
		}
		object_get_origin(other->unit_index, &origin);
		if (bots_distance3d(position, &origin) < range)
			return TRUE;
	}
	return FALSE;
}

/* a vehicle worth getting into (NONE: none), and the seat: a teammate's free gun first (a Warthog's no use without
its gunner), an empty vehicle to drive on a long trip, a turret after a fight, a teammate's passenger seat (a
Scorpion's sides) last */
static long bots_find_vehicle(
	struct bots_brain *brain,
	struct player_datum const *player,
	real_point3d const *position,
	boolean long_trip,
	long now,
	short *seat_wanted)
{
	struct object_iterator iterator;
	boolean teams = game_engine_get_variant() && game_engine_get_variant()->universal_variant.teams;
	boolean fighting = brain->target != NONE && now - brain->target_seen_time < 5 * BOTS_TICKS_PER_SECOND;
	real best_score = 0.f;
	long best = NONE;

	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *vehicle = object_get(iterator.index);
		boolean turret = vehicle_bot_type(iterator.index) == BOTS_VEHICLE_TYPE_TURRET;
		boolean still = vehicle->object.translational_velocity.i * vehicle->object.translational_velocity.i +
			vehicle->object.translational_velocity.j * vehicle->object.translational_velocity.j < 0.01f;
		boolean mate_drives;
		struct bots_seats seats;
		real distance, score;

		if (vehicle->object.parent_object_index != NONE || vehicle->object.up.k < 0.6f ||
			TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) ||
			(real)fabs(vehicle->object.position.z - position->z) > 2.f)
		{
			continue;
		}
		distance = bots_distance2d(position, &vehicle->object.position);
		if (distance > BOTS_SEAT_RANGE)
			continue;
		bots_vehicle_seats(iterator.index, &seats);
		mate_drives = teams && seats.driver_unit != NONE && bots_unit_team(seats.driver_unit) == player->team_index;
		if (turret)
		{
			/* (a turret: manned when there's been a fight; its seat, the driver's or the gunner's) */
			short kind = seats.driver != NONE ? _bots_seat_driver : _bots_seat_gunner;
			long rider = seats.driver != NONE ? seats.driver_unit : seats.gunner_unit;

			score = 3.f - distance / 30.f;
			if (fighting && distance < BOTS_VEHICLE_RANGE && (seats.driver != NONE || seats.gunner != NONE) && rider == NONE &&
				!bots_seat_takers(brain, iterator.index, NONE) && score > best_score)
			{
				best = iterator.index;
				best_score = score;
				*seat_wanted = kind;
			}
			continue;
		}
		/* a teammate's free gun (driving, or waiting for its gunner) */
		score = 4.f - distance / 30.f;
		if (seats.gunner != NONE && seats.gunner_unit == NONE && mate_drives &&
			!bots_seat_takers(brain, iterator.index, _bots_seat_gunner) && score > best_score)
		{
			best = iterator.index;
			best_score = score;
			*seat_wanted = _bots_seat_gunner;
			continue;
		}
		/* an empty one to drive (one with a separate gun only in a team game, with a teammate near to man it, and
		not one whose last driver waited for a gunner in vain) */
		score = 2.f - distance / 30.f;
		if (seats.driver != NONE && seats.driver_unit == NONE && long_trip && still && distance < BOTS_VEHICLE_RANGE &&
			(seats.gunner == NONE || (teams && !bots_crewless(iterator.index, now) &&
				bots_teammate_near((long)(brain - bots_brains), player, &vehicle->object.position, 20.f))) &&
			!bots_seat_takers(brain, iterator.index, _bots_seat_driver) && score > best_score)
		{
			best = iterator.index;
			best_score = score;
			*seat_wanted = _bots_seat_driver;
			continue;
		}
		/* a teammate's passenger seat, close by */
		score = 1.f - distance / 30.f;
		if (seats.passengers_free > bots_seat_takers(brain, iterator.index, _bots_seat_other) && mate_drives &&
			distance < BOTS_PASSENGER_RANGE && score > best_score)
		{
			best = iterator.index;
			best_score = score;
			*seat_wanted = _bots_seat_other;
		}
	}
	return best;
}

/* the bot's vehicle, this tick: in one (what to do there), or going for one (a goal) */
static void bots_vehicle_update(
	struct bots_brain *brain,
	struct player_datum const *player,
	struct unit_datum *unit,
	long now)
{
	long vehicle_index = unit->object.parent_object_index;
	struct object_datum *vehicle = vehicle_index != NONE ? object_try_and_get(vehicle_index) : NULL;
	real_point3d position;

	brain->driving = FALSE;
	brain->drive_hold = FALSE;
	brain->press_action = FALSE;
	brain->seat_kind = _bots_seat_none;
	if (vehicle && !TEST_FLAG(_object_mask_vehicle, vehicle->object.type))
		vehicle = NULL;
	if (vehicle)
	{
		long since = brain->idle_since;

		brain->vehicle_object = NONE;
		brain->seat_kind = bots_seat_kind(vehicle_index, unit->unit.parent_seat_index);
		if (!brain->drive_progress_time)
		{
			/* (just in) */
			bots_route.vehicle_entries++;
			brain->drive_progress_time = now;
			brain->drive_progress_point = vehicle->object.position;
			brain->drive_stuck = 0;
			brain->idle_since = now;
			brain->route_count = 0;
			brain->path_valid = FALSE;
		}
		switch (brain->seat_kind)
		{
		case _bots_seat_driver:
		{
			boolean turret = vehicle_bot_type(vehicle_index) == BOTS_VEHICLE_TYPE_TURRET;

			if (turret)
			{
				/* (a turret's seat: gunning) */
				brain->seat_kind = _bots_seat_gunner;
				bots_route.gunning_ticks++;
				brain->arrival_time = now;
				if (brain->target_visible)
					brain->idle_since = now;
				if (now - since > 15 * BOTS_TICKS_PER_SECOND)
					brain->press_action = TRUE;
				break;
			}
			brain->driving = TRUE;
			brain->driver_aims = !bots_vehicle_has_gunner_seat(vehicle_index);
			brain->flying = vehicle_bot_type(vehicle_index) == BOTS_VEHICLE_TYPE_ALIEN_FIGHTER;
			bots_route.driving_ticks++;
			if (vehicle->object.up.k < 0.3f)
			{
				brain->press_action = TRUE;
				break;
			}
			{
				/* the crew first: a Warthog waits for its gunner (20 seconds, then out: nobody's coming), and any
				vehicle a little for teammates on their way to its seats */
				struct bots_seats seats;
				boolean teams = game_engine_get_variant() && game_engine_get_variant()->universal_variant.teams;
				boolean need_gunner, coming;

				bots_vehicle_seats(vehicle_index, &seats);
				need_gunner = teams && seats.gunner != NONE && seats.gunner_unit == NONE;
				coming = bots_seat_takers(brain, vehicle_index, NONE) > 0;
				if (need_gunner || coming)
				{
					if (!brain->crew_wait_since)
					{
						brain->crew_wait_since = now;
						bots_route.crew_waits++;
					}
					if (need_gunner && (now - brain->crew_wait_since > BOTS_CREW_WAIT_TICKS ||
						(!coming && now - brain->crew_wait_since > 6 * BOTS_TICKS_PER_SECOND &&
							!bots_teammate_near((long)(brain - bots_brains), player, &vehicle->object.position, 25.f))))
					{
						/* (nobody's coming: out, and the vehicle left a while) */
						bots_crewless_add(vehicle_index, now);
						if (!brain->exiting)
							bots_route.crew_gave_up++;
						brain->press_action = TRUE;
						break;
					}
					if (need_gunner || now - brain->crew_wait_since < BOTS_PASSENGER_WAIT_TICKS)
					{
						brain->drive_hold = TRUE;
						brain->arrival_time = now;
						brain->drive_progress_time = now;
						brain->drive_progress_point = vehicle->object.position;
						if (brain->hop_deadline)
							brain->hop_deadline++;
						break;
					}
				}
				else
				{
					brain->crew_wait_since = 0;
				}
			}
			if (now - brain->drive_progress_time >= 3 * BOTS_TICKS_PER_SECOND && now >= brain->reverse_until)
			{
				if (bots_distance3d(&vehicle->object.position, &brain->drive_progress_point) < 1.5f)
				{
					if (++brain->drive_stuck > 2)
						brain->press_action = TRUE;
					else
					{
						brain->reverse_until = now + BOTS_TICKS_PER_SECOND * 3 / 2;
						bots_route.vehicle_reverses++;
					}
				}
				else
				{
					brain->drive_stuck = 0;
				}
				brain->drive_progress_time = now;
				brain->drive_progress_point = vehicle->object.position;
			}
			break;
		}
		case _bots_seat_gunner:
		{
			struct unit_definition *definition = unit_definition_get(vehicle->definition_index);
			boolean driver = FALSE;
			short seat;

			bots_route.gunning_ticks++;
			brain->arrival_time = now;
			for (seat = 0; seat < definition->unit.seats.count; seat++)
			{
				if (unit_seat_is_driver(vehicle_index, seat) && unit_seat_filled(vehicle_index, seat))
					driver = TRUE;
			}
			if (driver || brain->target_visible)
				brain->idle_since = now;
			if (now - since > 5 * BOTS_TICKS_PER_SECOND)
				brain->press_action = TRUE;
			break;
		}
		default:
		{
			/* a passenger: in while the vehicle has a driver (3 seconds without one: out) */
			struct bots_seats seats;

			brain->arrival_time = now;
			bots_vehicle_seats(vehicle_index, &seats);
			if (seats.driver_unit != NONE)
				brain->idle_since = now;
			if (now - since > 3 * BOTS_TICKS_PER_SECOND || vehicle->object.up.k < 0.3f)
				brain->press_action = TRUE;
			break;
		}
		}
		if (brain->press_action)
		{
			if (!brain->exiting)
				bots_route.vehicle_exits++;
			brain->exiting = TRUE;
			brain->vehicle_left_time = now;
		}
		return;
	}
	brain->exiting = FALSE;
	brain->drive_progress_time = 0;
	brain->reverse_until = 0;
	brain->crew_wait_since = 0;
	object_get_origin(player->unit_index, &position);
	/* going for one: still worth it? */
	if (brain->vehicle_object != NONE)
	{
		struct object_datum *target = object_try_and_get(brain->vehicle_object);

		if (!target || !TEST_FLAG(_object_mask_vehicle, target->object.type) || now > brain->vehicle_until ||
			target->object.up.k < 0.6f || bots_distance2d(&position, &target->object.position) > BOTS_SEAT_RANGE + 5.f)
		{
			if (now > brain->vehicle_until)
			{
				brain->vehicle_time = now + BOTS_VEHICLE_REST_TICKS;
				bots_route.seat_misses++;
			}
			brain->vehicle_object = NONE;
		}
		else if (player->action_result == _player_action_result_enter_vehicle &&
			player->action_object_index == brain->vehicle_object &&
			(bots_seat_kind(brain->vehicle_object, player->action_seat_index) == brain->vehicle_seat ||
				(brain->vehicle_seat == _bots_seat_other &&
					bots_seat_kind(brain->vehicle_object, player->action_seat_index) == _bots_seat_gunner)))
		{
			/* (the game offers the seat: in) */
			brain->press_action = TRUE;
		}
	}
	if (brain->vehicle_object == NONE && now >= brain->vehicle_time &&
		now - brain->vehicle_left_time > 10 * BOTS_TICKS_PER_SECOND && !bots_carries_objective(unit) &&
		brain->goal_reason != _bots_reason_hill && brain->goal_reason != _bots_reason_defend &&
		brain->goal_reason != _bots_reason_carry)
	{
		/* a look for one: a long trip (its goal, or its route's destination, far) */
		real trip = 0.f;

		brain->vehicle_time = now + BOTS_VEHICLE_LOOK_TICKS;
		if (brain->goal_kind)
			trip = bots_distance3d(&position, &brain->goal_point);
		else if (brain->route_position < brain->route_count)
			trip = bots_distance3d(&position, &bots_route.waypoints[brain->route[brain->route_count - 1]].point);
		brain->vehicle_object = bots_find_vehicle(brain, player, &position, trip > BOTS_VEHICLE_TRIP, now,
			&brain->vehicle_seat);
		brain->vehicle_until = now + BOTS_VEHICLE_TRY_TICKS + (brain->vehicle_seat == _bots_seat_gunner ?
			5 * BOTS_TICKS_PER_SECOND : 0);
	}
	if (brain->vehicle_object != NONE)
	{
		/* (walk to the seat's side: the driver's on the left, the gunner at the back) */
		struct object_datum *target = object_get(brain->vehicle_object);
		real_point3d side = target->object.position;
		real left_i = -target->object.forward.j, left_j = target->object.forward.i;

		if (brain->vehicle_seat == _bots_seat_gunner)
		{
			side.x -= target->object.forward.i * 1.2f;
			side.y -= target->object.forward.j * 1.2f;
		}
		else if (brain->vehicle_seat == _bots_seat_other)
		{
			/* (at the vehicle: the nearest seat's offered) */
		}
		else if (vehicle_bot_type(brain->vehicle_object) != BOTS_VEHICLE_TYPE_TURRET)
		{
			side.x += left_i * 1.0f;
			side.y += left_j * 1.0f;
		}
		bots_set_goal(brain, _bots_goal_touch, _bots_reason_vehicle, &side, 0.f, NONE);
	}
}

/* the bot's goal for the game type (every BOTS_GOAL_TICKS; a goal that follows an object follows it every tick) */
static void bots_objective(
	struct bots_brain *brain,
	long absolute_player_index,
	struct player_datum const *player,
	struct unit_datum const *unit,
	long now)
{
	real_point3d position;
	short engine = game_engine_get_variant() ? game_engine_get_variant()->game_engine_index : 0;

	if (now < brain->goal_update_time)
	{
		if (brain->goal_kind && brain->goal_object != NONE)
		{
			if (!object_try_and_get(brain->goal_object))
				brain->goal_update_time = 0;
			else if (TEST_FLAG(_object_mask_weapon, object_get(brain->goal_object)->object.type))
				item_get_position_even_if_in_inventory(brain->goal_object, &brain->goal_point);
			else
				object_get_origin(brain->goal_object, &brain->goal_point);
		}
		if (now < brain->goal_update_time)
			return;
	}
	brain->goal_update_time = now + BOTS_GOAL_TICKS;
	object_get_origin(player->unit_index, &position);
	brain->goal_kind = _bots_goal_none;
	brain->goal_reason = _bots_reason_none;
	brain->goal_urgent = FALSE;
	brain->goal_object = NONE;
	brain->goal_hunt_unit = NONE;
	if (!game_engine_running())
		return;
	switch (engine)
	{
	case game_engine_ctf:
		bots_goal_ctf(brain, absolute_player_index, player, unit, &position);
		break;
	case game_engine_oddball:
		bots_goal_oddball(brain, absolute_player_index, player, unit, &position);
		break;
	case game_engine_king:
	{
		real_point3d center;
		real radius;

		if (game_engine_king_bot_hill(&center, &radius))
		{
			radius = radius > 1.f ? radius * 0.7f : 0.7f;
			bots_set_goal(brain, _bots_goal_hold, _bots_reason_hill, &center, radius > 4.f ? 4.f : radius, NONE);
		}
		break;
	}
	case game_engine_race:
		bots_goal_race(brain, absolute_player_index, player, &position);
		break;
	}
	if ((brain->goal_kind == _bots_goal_none && brain->goal_reason == _bots_reason_none) ||
		brain->goal_reason == _bots_reason_defend)
	{
		bots_goal_item(brain, unit, &position, now);
	}
	if (brain->vehicle_object != NONE)
		brain->goal_reason = _bots_reason_vehicle;
	if (brain->goal_kind == _bots_goal_none)
		brain->goal_waypoint = NONE;
	if (brain->goal_reason >= 0 && brain->goal_reason < NUMBEROF(bots_route.goal_counts))
		bots_route.goal_counts[brain->goal_reason]++;
}

/* the route to a waypoint (FALSE: not reachable, or the bot is on it) */
static boolean bots_route_to(
	struct bots_brain *brain,
	real_point3d const *position,
	short waypoint,
	long now)
{
	real cost[BOTS_WAYPOINT_MAXIMUM];
	short previous[BOTS_WAYPOINT_MAXIMUM];
	short route[BOTS_WAYPOINT_MAXIMUM];
	short length = 0;
	short index;
	short node;
	short at;

	if (waypoint == NONE)
		return FALSE;
	at = bots_explore(brain, position, now, cost, previous);
	if (cost[waypoint] == REAL_MAX || waypoint == at)
		return FALSE;
	for (node = waypoint; node != NONE && length < bots_route.waypoint_count; node = previous[node])
		route[length++] = node;
	brain->route_count = 0;
	for (index = length - 1; index >= 0 && brain->route_count < BOTS_ROUTE_MAXIMUM; index--)
		brain->route[brain->route_count++] = route[index];
	brain->route_position = 0;
	brain->destination = waypoint;
	brain->from_waypoint = at;
	brain->path_valid = FALSE;
	brain->wants_search = FALSE;
	brain->straight = brain->straight_after = FALSE;
	brain->hop_closest = REAL_MAX;
	brain->hop_stalls = 0;
	brain->hop_deadline = 0;
	return TRUE;
}

/* walking for the goal: straight for it when it's near (or holding a spot), TRUE; else the route's for it, FALSE */
static boolean bots_goal_walk(
	struct bots_brain *brain,
	real_point3d const *position,
	long now)
{
	real_point3d const *toward;
	real distance, rise, di, dj, length;
	boolean at_waypoint;

	if (!brain->goal_kind || !bots_route.waypoint_count)
		return FALSE;
	distance = bots_distance2d(position, &brain->goal_point);
	rise = (real)fabs(brain->goal_point.z - position->z);
	if (brain->goal_kind == _bots_goal_hold && distance < brain->goal_radius && rise < 1.5f)
	{
		/* holding: about the spot, a new place in it every 2 or 3 seconds */
		if (now >= brain->hold_until || bots_distance2d(position, &brain->hold_point) < 0.5f)
		{
			real angle = (real)(bots_random() % 628) / 100.f;
			real reach = brain->goal_radius * 0.6f * (real)(bots_random() % 100) / 100.f;

			brain->hold_point = brain->goal_point;
			brain->hold_point.x += reach * (real)cos(angle);
			brain->hold_point.y += reach * (real)sin(angle);
			brain->hold_until = now + 2 * BOTS_TICKS_PER_SECOND + (long)(bots_random() % BOTS_TICKS_PER_SECOND);
		}
		toward = &brain->hold_point;
		brain->goal_direct_since = 0;
	}
	else
	{
		if (now >= brain->goal_look_time)
		{
			real_point3d from = *position, to = brain->goal_point;

			brain->goal_look_time = now + BOTS_GOAL_TICKS;
			from.z += 0.4f;
			to.z += 0.3f;
			/* (a clear way for a biped: the level, scenery and machines; not the objective itself, or a player. Very
			close on the same floor, straight anyway) */
			brain->goal_visible = distance < BOTS_GOAL_NEAR && rise < 1.5f;
			if (brain->goal_visible)
			{
				/* (room for the whole biped: at the knees and at the head; under a ramp, or past a low overhang, no) */
				real_vector3d way;

				way.i = to.x - from.x;
				way.j = to.y - from.y;
				way.k = to.z - from.z;
				from.z = position->z + 0.25f;
				to.z = brain->goal_point.z + 0.25f;
				{
					/* (at the knees, something just in front of the goal counts as clear: what it stands on, a
					flag's stand; the look-ahead jumps onto it) */
					real t = bots_ray(&from, &way, brain->unit_index, NULL);
					real length = (real)sqrt(way.i * way.i + way.j * way.j + way.k * way.k);

					brain->goal_visible = t >= 1.f || (1.f - t) * length < 1.5f;
				}
				from.z = position->z + 0.65f;
				to.z = brain->goal_point.z + 0.65f;
				brain->goal_visible = brain->goal_visible && bots_ray(&from, &way, brain->unit_index, NULL) >= 1.f;
			}
		}
		at_waypoint = brain->goal_waypoint != NONE &&
			bots_distance2d(position, &bots_route.waypoints[brain->goal_waypoint].point) < 2.f &&
			(real)fabs(position->z - bots_route.waypoints[brain->goal_waypoint].point.z) < 1.5f;
		if (now < brain->goal_blocked_until || !(brain->goal_visible || (at_waypoint && distance < BOTS_GOAL_NEAR)) ||
			(brain->goal_kind == _bots_goal_touch && distance < 0.2f && rise < 1.f))
		{
			/* far, or out of sight: the route to the waypoint nearest the goal */
			brain->goal_direct_since = 0;
			if (now >= brain->goal_plan_time)
			{
				/* (the waypoint nearest the goal of those the bot can reach; a little for the way there too) */
				real cost[BOTS_WAYPOINT_MAXIMUM];
				short previous[BOTS_WAYPOINT_MAXIMUM];
				short best = NONE;
				real best_distance = REAL_MAX;
				short index;
				short at;

				brain->goal_plan_time = now + BOTS_TICKS_PER_SECOND;
				at = bots_explore(brain, position, now, cost, previous);
				for (index = 0; index < bots_route.waypoint_count; index++)
				{
					real weighted;

					if (bots_route.waypoints[index].teleport_source || (cost[index] == REAL_MAX && index != at))
						continue;
					weighted = bots_weighted_distance(&brain->goal_point, &bots_route.waypoints[index].point) +
						(index == at ? 0.f : 0.05f * cost[index]);
					if (weighted < best_distance)
					{
						best = index;
						best_distance = weighted;
					}
				}
				{
					/* (the links don't reach near it, yet: straight for the waypoint nearest it, by Halo's search alone,
					as for a waypoint the links don't reach) */
					short nearest = NONE;
					real nearest_distance = REAL_MAX;

					for (index = 0; index < bots_route.waypoint_count; index++)
					{
						real weighted;

						if (bots_route.waypoints[index].teleport_source || bots_avoided(brain, index, now))
							continue;
						weighted = bots_weighted_distance(&brain->goal_point, &bots_route.waypoints[index].point);
						if (weighted < nearest_distance)
						{
							nearest = index;
							nearest_distance = weighted;
						}
					}
					if (nearest != NONE && nearest != at && nearest != best &&
						(best == NONE || best == at || best_distance > nearest_distance + 10.f))
					{
						brain->goal_waypoint = nearest;
						if (brain->destination != nearest || brain->route_position >= brain->route_count)
						{
							brain->route[0] = nearest;
							brain->route_count = 1;
							brain->route_position = 0;
							brain->destination = nearest;
							brain->from_waypoint = at;
							brain->path_valid = FALSE;
							brain->wants_search = FALSE;
							brain->straight = brain->straight_after = FALSE;
							brain->hop_closest = REAL_MAX;
							brain->hop_stalls = 0;
							brain->hop_deadline = 0;
						}
						return FALSE;
					}
				}
				brain->goal_waypoint = best;
				if (best != NONE && best != at && (brain->destination != best || brain->route_position >= brain->route_count))
					bots_route_to(brain, position, best, now);
			}
			return FALSE;
		}
		/* near and in sight: straight for it (not for ever: a wall in the way, or a second pushing at something
		without getting anywhere, gives up for a while) */
		if (!brain->goal_direct_since)
			brain->goal_direct_since = now;
		else if (now - brain->goal_direct_since > BOTS_GOAL_DIRECT_TICKS || brain->push_ticks > BOTS_TICKS_PER_SECOND)
		{
			brain->goal_blocked_until = now + BOTS_GOAL_BLOCKED_TICKS;
			brain->goal_direct_since = 0;
			return FALSE;
		}
		toward = &brain->goal_point;
	}
	/* (standing on a ball that isn't picked up on its own: the action button, after a third of a second) */
	if (brain->goal_kind == _bots_goal_touch && brain->goal_reason == _bots_reason_take && distance < 0.6f && rise < 1.f)
		brain->touch_ticks++;
	else
		brain->touch_ticks = 0;
	/* (not trapped while it goes for its goal) */
	brain->arrival_time = now;
	brain->progress_point = *position;
	brain->progress_time = now;
	brain->route_count = 0;
	brain->path_valid = FALSE;
	di = toward->x - position->x;
	dj = toward->y - position->y;
	length = (real)sqrt(di * di + dj * dj);
	if (length < 0.05f)
		return TRUE;
	brain->move_i = di / length;
	brain->move_j = dj / length;
	brain->walk_yaw = (real)atan2(dj, di);
	brain->walk_has_yaw = TRUE;
	return TRUE;
}

/* this tick's crouch and jump for the way the bot moves (the look-ahead: the movement section's top) */
static void bots_move_skills(
	struct bots_brain *brain,
	long unit_index)
{
	long now = game_time_get();
	struct biped_datum *biped = biped_get(unit_index);
	boolean airborne = TEST_FLAG(biped->biped.flags, _biped_airborne_bit);
	struct bots_body body;
	real_point3d feet, origin;
	real_vector3d vector;
	real_plane3d plane;
	real move_length = (real)sqrt(brain->move_i * brain->move_i + brain->move_j * brain->move_j);
	real di, dj, length, t_knee, t_head;
	unsigned long long cycles;

	if (brain->crouch_ticks > 0)
		brain->crouch_ticks--;
	if (airborne)
	{
		/* (a crouch-jump crouches on the way up, and stays crouched until it lands) */
		if (brain->jump_time && brain->jump_crouch)
			brain->crouch_ticks = 2;
		return;
	}
	object_get_origin(unit_index, &feet);
	if (brain->jump_time && now - brain->jump_time > 2)
	{
		/* (landed: higher, as it meant to?) */
		/* (over it: higher, or on the far side of something thin) */
		if (brain->jump_goal > 0.f && (feet.z > brain->jump_start_z + 0.6f * brain->jump_goal ||
			(feet.x - brain->jump_start_x) * (feet.x - brain->jump_start_x) +
				(feet.y - brain->jump_start_y) * (feet.y - brain->jump_start_y) > 0.6f * 0.6f))
		{
			bots_route.jumps_landed++;
		}
#if defined(OXBOX_TEST_BOTS)
		{
			static long logged;

			if (logged++ < 80)
				bots_log("bots: jump landed at (%.2f %.2f %.2f): %.2f up (meant %.2f), %ld ticks", feet.x, feet.y, feet.z,
					feet.z - brain->jump_start_z, brain->jump_goal, now - brain->jump_time);
		}
#endif
		brain->jump_time = 0;
		brain->jump_crouch = FALSE;
	}
	{
		/* (trying to move, and not getting anywhere: pushing at something) */
		real speed = move_length > 0.01f ? (biped->object.translational_velocity.i * brain->move_i +
			biped->object.translational_velocity.j * brain->move_j) / move_length : 0.f;

		if (move_length >= 0.5f && speed < 0.02f)
			brain->push_ticks++;
		else
			brain->push_ticks = 0;
	}
	if (move_length < 0.3f || now < brain->probe_time)
		return;
	cycles = bots_cycles();
	bots_body_get(unit_index, &body);
	if (!bots_route.body_logged && bots_log_enabled())
	{
		bots_route.body_logged = TRUE;
		bots_log("bots: a bot's body: radius %.3f, height %.3f standing, %.3f crouched; jumps %.3f, crouch-jumps %.3f; "
			"falls %.2f safely", body.radius, body.standing, body.crouching, body.jump, body.crouch_jump, body.safe_drop);
	}
	di = brain->move_i / move_length;
	dj = brain->move_j / move_length;
	length = body.radius + BOTS_PROBE_AHEAD;
	vector.i = di * length;
	vector.j = dj * length;
	vector.k = 0.f;
	origin = feet;
	origin.z += BOTS_KNEE_HEIGHT;
	plane.n.k = 0.f;
	t_knee = bots_ray(&origin, &vector, unit_index, &plane);
	/* (looking every 3 ticks; every tick with something close ahead) */
	brain->probe_time = now + (t_knee < 1.f ? 1 : 3);
#if defined(OXBOX_TEST_BOTS)
	if (brain->push_ticks == 8)
	{
		/* (test builds: a bot pushing at something it doesn't jump: what its rays see) */
		static long logged;
		real top_test = -9.f, land_test = -9.f, head_test;
		real_point3d head = feet;

		head.z += body.standing - 0.03f;
		head_test = bots_ray(&head, &vector, unit_index, NULL);
		if (t_knee < 1.f)
		{
			bots_height_at(&feet, di * (t_knee * length + 0.05f), dj * (t_knee * length + 0.05f), body.crouch_jump + 0.05f,
				BOTS_KNEE_HEIGHT * 0.5f, unit_index, &top_test);
			bots_height_at(&feet, di * (t_knee * length + body.radius + 0.15f), dj * (t_knee * length + body.radius + 0.15f),
				body.crouch_jump + 0.1f, -0.3f, unit_index, &land_test);
		}
		if (logged++ < 60)
		{
			bots_log("bots: pushing at (%.2f %.2f %.2f) heading %.2f %.2f: knee %.2f (normal k %.2f), head %.2f, top %.2f, land %.2f",
				feet.x, feet.y, feet.z, di, dj, t_knee * length, plane.n.k, head_test * length, top_test, land_test);
		}
	}
#endif
	if (t_knee < 1.f && plane.n.k < -0.1f && brain->push_ticks == 10)
	{
		/* (the underside of a ramp or a stair: Halo's search knows the floor under it, not the ceiling; a spot to
		go around from now on, and a search from here) */
		real_point3d ahead = feet;

		ahead.x += di * length;
		ahead.y += dj * length;
		bots_stuck_log_add(&feet, &ahead);
		bots_stuck_log_add(&feet, &ahead);
		brain->path_valid = FALSE;
		bots_route.overhangs++;
	}
	if (t_knee < 1.f && plane.n.k < 0.6f)
	{
		/* at the knees: what's its top, and is there room on it? */
		real face = t_knee * length;
		real reach = body.crouch_jump * BOTS_REACH_SAFETY;
		real top, land, room;
		boolean standing_room;

		origin.z = feet.z + reach + 0.05f;
		if (now >= brain->jump_wait_until && face - body.radius < BOTS_JUMP_DISTANCE && brain->push_ticks >= 4 &&
			bots_ray(&origin, &vector, unit_index, NULL) >= 1.f &&
			bots_height_at(&feet, di * (face + 0.05f), dj * (face + 0.05f), reach + 0.05f, BOTS_KNEE_HEIGHT * 0.5f,
				unit_index, &top) &&
			top > BOTS_KNEE_HEIGHT * 0.75f &&
			bots_height_at(&feet, di * (face + body.radius + 0.15f), dj * (face + body.radius + 0.15f), top + 0.1f,
				brain->straight ? -body.safe_drop : -0.3f, unit_index, &land))
		{
			/* (room to land: up from the top, past the face) */
			origin.x = feet.x + di * (face + body.radius + 0.15f);
			origin.y = feet.y + dj * (face + body.radius + 0.15f);
			origin.z = feet.z + land + 0.05f;
			vector.i = 0.f;
			vector.j = 0.f;
			vector.k = body.standing;
			room = bots_ray(&origin, &vector, unit_index, NULL) * body.standing;
			standing_room = room >= body.standing - 0.01f;
			if (room >= body.crouching + 0.02f)
			{
				if (top <= body.jump * BOTS_REACH_SAFETY && standing_room)
				{
					brain->jump = TRUE;
					brain->jump_crouch = FALSE;
					bots_route.jumps++;
				}
				else if (top <= reach)
				{
					brain->jump = TRUE;
					brain->jump_crouch = TRUE;
					bots_route.jumps++;
					bots_route.crouch_jumps++;
				}
				if (brain->jump)
				{
#if defined(OXBOX_TEST_BOTS)
					static long logged;

					if (logged++ < 80)
						bots_log("bots: %s at (%.2f %.2f %.2f) heading %.2f %.2f: face %.2f, top %.2f, land %.2f, room %.2f, "
							"speed %.3f", brain->jump_crouch ? "crouch-jump" : "jump", feet.x, feet.y, feet.z, di, dj, face, top,
							land, room, (real)sqrt(biped->object.translational_velocity.i * biped->object.translational_velocity.i +
								biped->object.translational_velocity.j * biped->object.translational_velocity.j));
#endif
					brain->jump_time = now;
					brain->jump_start_z = feet.z;
					brain->jump_start_x = feet.x;
					brain->jump_start_y = feet.y;
					brain->jump_goal = top;
					brain->jump_wait_until = now + BOTS_TICKS_PER_SECOND / 2;
				}
			}
		}
	}
	else if (t_knee >= 1.f)
	{
		/* nothing at the knees: something at the head? Crouch, if a crouched biped fits */
		origin = feet;
		origin.z += body.standing - 0.03f;
		t_head = bots_ray(&origin, &vector, unit_index, NULL);
		if (t_head < 1.f && body.crouching < body.standing - 0.05f)
		{
			origin.z = feet.z + body.crouching - 0.03f;
			if (bots_ray(&origin, &vector, unit_index, NULL) >= 1.f)
			{
				if (brain->crouch_ticks <= 0)
					bots_route.crouches++;
				brain->crouch_ticks = 20;
			}
		}
	}
	if (brain->crouch_ticks > 0 && brain->crouch_ticks < 4 && biped->biped.crouch > 0.f)
	{
		/* (still under something too low to stand: stay down) */
		origin = feet;
		origin.z += body.crouching;
		vector.i = 0.f;
		vector.j = 0.f;
		vector.k = body.standing - body.crouching + 0.02f;
		if (bots_ray(&origin, &vector, unit_index, NULL) < 1.f)
			brain->crouch_ticks = 10;
	}
	bots_route.probe_cycles += bots_cycles() - cycles;
}

/* the bot's action from its walk and its fight: facing (turning at a person's speed), throttle, jump, crouch */
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
	if (brain->crouch_ticks > 0)
		action->control_flags |= FLAG(_unit_control_crouch_modifier_bit);
	if (brain->touch_ticks > 10 && brain->touch_ticks <= 25)
		action->control_flags |= FLAG(_unit_control_swap_weapons_bit);
	if (brain->driving)
	{
		/* (a vehicle steers toward where the camera faces: the route's way, unless its driver aims its guns; the
		throttle forward, slower into a sharp turn, back when stuck) */
		real turn;

		if (!brain->driver_aims || !brain->target_visible)
		{
			if (brain->walk_has_yaw)
				brain->yaw += PIN(bots_angle_difference(brain->walk_yaw, brain->yaw), -BOTS_TURN_PER_TICK, BOTS_TURN_PER_TICK);
			action->desired_facing.yaw = brain->yaw;
		}
		turn = (real)fabs(bots_angle_difference(brain->walk_yaw, brain->yaw));
		action->throttle.i = brain->walk_has_yaw || brain->target_visible ? (turn > 1.2f ? 0.4f : 1.f) : 0.f;
		if (brain->drive_hold)
			action->throttle.i = 0.f;
		action->throttle.j = 0.f;
		if (game_time_get() < brain->reverse_until)
			action->throttle.i = -1.f;
		if (brain->flying && !brain->target_visible && brain->route_position < brain->route_count)
		{
			/* (a Banshee: toward the waypoint's height, and 2 over it) */
			real_point3d const *waypoint = &bots_route.waypoints[brain->route[brain->route_position]].point;
			real dx = waypoint->x - brain->last_position.x, dy = waypoint->y - brain->last_position.y;

			action->desired_facing.pitch = PIN((real)atan2(waypoint->z + 2.f - brain->last_position.z,
				sqrt(dx * dx + dy * dy) + 1.f), -0.5f, 0.5f);
		}
	}
	if (brain->press_action)
		action->control_flags |= FLAG(_unit_control_action_bit);
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
	network_game_server_set_bot_mode(BOTS_SETTING(bots_globals.mode, bots_globals.difficulty));
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

#if defined(OXBOX_SHOWCASE)
/* the showcase screensaver's bots (showcase.c): that many (0 to 15), at Normal,
while it runs; -1: the host's own setting back */
void bots_showcase(
	short count)
{
	static boolean saved = FALSE;
	static short saved_mode, saved_count, saved_difficulty;

	if (count >= 0)
	{
		if (!saved)
		{
			saved = TRUE;
			saved_mode = bots_globals.mode;
			saved_count = bots_globals.custom_count;
			saved_difficulty = bots_globals.difficulty;
		}
		bots_globals.mode = _bots_mode_custom;
		bots_globals.custom_count = count > NETWORK_BOT_MAXIMUM_COUNT ? NETWORK_BOT_MAXIMUM_COUNT : count;
		bots_globals.difficulty = _bots_difficulty_normal;
	}
	else if (saved)
	{
		saved = FALSE;
		bots_globals.mode = saved_mode;
		bots_globals.custom_count = saved_count;
		bots_globals.difficulty = saved_difficulty;
	}
}
#endif

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

				bots_objective(brain, absolute_player_index, player, unit, now);
				bots_vehicle_update(brain, player, unit, now);
				if (brain->seat_kind == _bots_seat_none || brain->driving)
					bots_walk(brain, absolute_player_index, player->unit_index, action);
				else
					brain->move_i = brain->move_j = 0.f;
				fight_start = bots_cycles();
				bots_fight(brain, absolute_player_index, player, unit, action);
				bots_evade(brain, player, player->unit_index, now);
				if (brain->seat_kind == _bots_seat_none)
					bots_move_skills(brain, player->unit_index);
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
#if defined(OXBOX_CROSSPLAY)
		/* oxbox, lobby kinds: bots turned on in a crossplay lobby with a PC in it make it the Xboxes' alone, which
		drops the PC (network_server_manager.c): said first, and Y again within BOTS_CONFIRM_MILLISECONDS turns them
		on */
		{
			extern boolean oxbox_crossplay_lobby_has_pc(void);
			static unsigned long asked_time;
			unsigned long now = system_milliseconds();

			if (bots_globals.mode == _bots_mode_off && oxbox_crossplay_lobby_has_pc() &&
				(!asked_time || now - asked_time > BOTS_CONFIRM_MILLISECONDS))
			{
				asked_time = now;
				display_error_text_deferred(L"PC players can't play\r\nwith bots: turning them\r\non drops them from the\r\nlobby. Press Y again\r\nto add bots.",
					NONE);
				return TRUE;
			}
			asked_time = 0;
		}
#endif
		bots_globals.mode = (short)((bots_globals.mode + 1) % NUMBER_OF_BOTS_MODES);
		network_game_server_set_bot_mode(BOTS_SETTING(bots_globals.mode, bots_globals.difficulty));
		ui_play_audio_feedback_sound(1);
		return TRUE;
	}
	if (button == _gamepad_analog_button_black && bots_globals.mode != _bots_mode_off)
	{
		/* (Black: how well they fight) */
		bots_globals.difficulty = (short)((bots_globals.difficulty + 1) % NUMBER_OF_BOTS_DIFFICULTIES);
		network_game_server_set_bot_mode(BOTS_SETTING(bots_globals.mode, bots_globals.difficulty));
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
	short difficulty;
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
#if defined(OXBOX_SHOWCASE)
	/* (not over the showcase screensaver's black: showcase.c) */
	{
		extern boolean showcase_active(void);

		if (showcase_active())
			return;
	}
#endif
	game = network_game_client_get_game(client);
	mode = host ? bots_globals.mode : (short)((NETWORK_GAME_BOT_MODE(game) & 0xFF) - 1);
	difficulty = host ? bots_globals.difficulty : (short)((NETWORK_GAME_BOT_MODE(game) >> 8) & 0xFF);
	if (difficulty < 0 || difficulty >= NUMBER_OF_BOTS_DIFFICULTIES)
		difficulty = _bots_difficulty_normal;
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
		halo_linux_snprintf(text, sizeof(text), "Y: Custom %d LT/RT  %s", (int)bots_globals.custom_count,
			bots_skills[difficulty].name);
	else
		halo_linux_snprintf(text, sizeof(text), host ? "Y: Bots %s (%d)  %s" : "Bots: %s (%d)  %s", bots_mode_names[mode],
			(int)bots, bots_skills[difficulty].name);
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
