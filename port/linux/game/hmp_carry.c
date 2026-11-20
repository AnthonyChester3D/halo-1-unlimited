/*
HMP_CARRY.C (oxbox, v1.143)

The player's state carried across a campaign map switch, for Halo Map Porter's split levels: HMP cuts a level too big
for the Xbox into pieces at its BSP switches, each piece its own map, each ending with (map_name "+folder/next").

- Recorded (hmp_carry_record, from main.c's main_change_map_name) when a game's script switches maps (not a game
  started from the main menu), before the old map unloads: each local player with a living body: the weapons held
  (tag path, both magazines' rounds loaded and in reserve, an energy weapon's age and heat, the slot, the one in
  hand), the grenade counts, body and shield vitality, and the vehicle they're in (tag path, the seat's label, its
  vitality). In memory only (allocated while there's something carried), cleared when a game starts from the main
  menu, at Restart mission and back at the main menu (hmp_carry_clear).
- Given (hmp_carry_spawned, from players.c's player spawn) at each local player's first spawn after the switch, when
  the starting profile that spawn uses is named "hmp_carry": the recorded weapons, grenades and vitality in place of
  the profile's, and the vehicle made at the spawn point facing the spawn's way, the player put in the same seat (by
  label; else any seat they can use). Two players carried in one vehicle share the one made for the first. A tag the
  new map doesn't have is left out. The first spawn uses the player's record up either way, so a later respawn (or a
  later piece) gets the profile as normal; a checkpoint revert restores its save, which holds what was given.
- Split-screen co-op: the same per local player.
- Network co-op (v1.144): the host's script's map_name doesn't switch maps there as in a local game: it's asked here
  (hmp_carry_coop_switch_ask, from main.c's main_set_map_name) and done from the main loop (hmp_carry_coop_update):
  every player's state recorded on the host (whose units are everyone's), each keyed by their console and controller,
  then the round ended for everyone and the piece set as the next map, as when a co-op level is won
  (network_game_server_port_cooperative_switch), and started by itself once every console has it (a console without
  it downloads it from the host first, as in a lobby). The record lives through the lobby (hmp_carry_menu) and is given
  at each player's first spawn in that piece, by the host, whose units the clients take. A player who wasn't in the
  last piece (joined since) has no record: the profile's contents.
- The record names the map it's for: given only there (a game started some other way meanwhile gets the profile),
  and freed once every recorded player has had their first spawn.
*/

#if defined(HALO_XBOX_TARGET)

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/vehicle_definitions.h"
#include "main/main.h"
#include "memory/data.h"
#include "networking/network_connection.h"
#include "networking/network_game_globals.h"
#include "networking/network_game_manager.h"

#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);

#define CARRY_PROFILE_NAME "hmp_carry"
#define CARRY_PATH 128
#define CARRY_PLAYERS 16                    /* (a network co-op game's most; split screen has 4) */

struct carry_weapon
{
	char tag[CARRY_PATH];
	short slot;
	short rounds_loaded[2], rounds_total[2];
	real age, heat;
};

struct carry_player
{
	boolean present;
	short machine_index, controller_index;  /* (its key: a network game's console and controller; split screen's
	                                           local player index as the controller) */
	short weapon_count, in_hand;            /* (in_hand: an index in weapons, NONE) */
	struct carry_weapon weapons[MAXIMUM_WEAPONS_PER_UNIT];
	char grenades[NUMBER_OF_UNIT_GRENADE_TYPES];
	real body, shield;
	char vehicle[CARRY_PATH];               /* (empty: on foot) */
	char seat[TAG_STRING_LENGTH + 1];
	long old_vehicle;                       /* (the vehicle's index in the old map: co-op players in one share it) */
	real vehicle_body, vehicle_shield;
};

static struct
{
	boolean network;                        /* (a network co-op host's record: kept through the lobby) */
	char target[256];                       /* (the map it's for) */
	struct carry_player players[CARRY_PLAYERS];
	long made_for_old[CARRY_PLAYERS], made[CARRY_PLAYERS];   /* (the vehicles made, by their old index) */
} *carry;

void hmp_carry_clear(void)
{
	if (carry)
	{
		free(carry);
		carry = NULL;
	}
}

static void carry_tag_name(long tag_index, char *out)
{
	char const *name = tag_index != NONE ? tag_get_name(tag_index) : NULL;
	csstrncpy(out, name ? name : "", CARRY_PATH - 1);
	out[CARRY_PATH - 1] = 0;
}

static void carry_record_player(struct carry_player *p, struct unit_datum *unit, short machine_index,
	short controller_index)
{
	short slot;
	p->present = TRUE;
	p->machine_index = machine_index;
	p->controller_index = controller_index;
	p->in_hand = NONE;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];
		struct carry_weapon *w = &p->weapons[p->weapon_count];
		struct weapon_datum *weapon;
		short m;
		if (weapon_index == NONE || !object_try_and_get_and_verify_type(weapon_index, _object_mask_weapon))
			continue;
		weapon = weapon_get(weapon_index);
		carry_tag_name(weapon->definition_index, w->tag);
		w->slot = slot;
		for (m = 0; m < 2; m++)
		{
			w->rounds_loaded[m] = weapon->weapon.magazines[m].rounds_loaded;
			w->rounds_total[m] = weapon->weapon.magazines[m].rounds_total;
		}
		w->age = weapon->weapon.age;
		w->heat = weapon->weapon.heat;
		if (slot == unit->unit.current_weapon_index)
			p->in_hand = p->weapon_count;
		p->weapon_count++;
	}
	memcpy(p->grenades, unit->unit.grenade_counts, sizeof(p->grenades));
	p->body = unit->object.body_vitality;
	p->shield = unit->object.shield_vitality;
	p->old_vehicle = NONE;
	if (unit->object.parent_object_index != NONE &&
		object_try_and_get_and_verify_type(unit->object.parent_object_index, _object_mask_vehicle))
	{
		struct unit_datum *vehicle = unit_get(unit->object.parent_object_index);
		struct unit_definition *definition = unit_definition_get(vehicle->definition_index);
		carry_tag_name(vehicle->definition_index, p->vehicle);
		if (unit->unit.parent_seat_index >= 0 && unit->unit.parent_seat_index < definition->unit.seats.count)
			csstrncpy(p->seat, TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, unit->unit.parent_seat_index,
				struct unit_seat)->label, TAG_STRING_LENGTH);
		p->old_vehicle = unit->object.parent_object_index;
		p->vehicle_body = vehicle->object.body_vitality;
		p->vehicle_shield = vehicle->object.shield_vitality;
	}
	platform_log("hmp carry: player (console %d, controller %d) recorded: %d weapons (number %d in hand), %s%s",
		(int)machine_index, (int)controller_index, (int)p->weapon_count, (int)p->in_hand, p->vehicle[0] ? "in " : "on foot",
		p->vehicle);
}

/* a new record for target; FALSE when there's no memory for it (malloc: in the game's code it's the game's own
allocator, as free is; calloc isn't) */
static boolean carry_new(char const *target, boolean network)
{
	short k;
	hmp_carry_clear();
	if (!(carry = malloc(sizeof(*carry))))
		return FALSE;
	csmemset(carry, 0, sizeof(*carry));
	carry->network = network;
	csstrncpy(carry->target, target ? target : "", sizeof(carry->target) - 1);
	for (k = 0; k < CARRY_PLAYERS; k++)
		carry->made[k] = carry->made_for_old[k] = NONE;
	return TRUE;
}

static boolean carry_unit_alive(long unit_index)
{
	return unit_index != NONE && !TEST_FLAG(unit_get(unit_index)->object.damage_flags, _object_dead_bit);
}

/* a local game's script switching to target (main.c's main_change_map_name): each local player */
void hmp_carry_record(char const *target)
{
	short local_player_index, recorded = 0;
	if (game_engine_running() || !carry_new(target, FALSE))
		return;
	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index = local_player_get_player_index(local_player_index);
		if (player_index == NONE || !carry_unit_alive(player_get(player_index)->unit_index))
			continue;
		carry_record_player(&carry->players[recorded++], unit_get(player_get(player_index)->unit_index), NONE,
			local_player_index);
	}
	if (!recorded)
		hmp_carry_clear();
}

/* a network co-op host's: every player, by console and controller */
static void carry_record_network(char const *target)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short recorded = 0;
	if (!carry_new(target, TRUE))
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL && recorded < CARRY_PLAYERS)
	{
		if (!carry_unit_alive(player->unit_index))
			continue;
		carry_record_player(&carry->players[recorded++], unit_get(player->unit_index),
			(short)player->network_player_data.machine_index, (short)player->network_player_data.controller_index);
	}
	if (!recorded)
		hmp_carry_clear();
}

/* back at the main menu: nothing carried past it, but a network co-op host's record goes through the lobby (its
round ends there, and the next piece starts from it) */
void hmp_carry_menu(void)
{
	if (carry && carry->network && global_network_game_server_get())
		return;
	hmp_carry_clear();
}

/* ---- network co-op: the host's script's map_name (main.c), done from the main loop ---- */

static char coop_switch_target[256];
static boolean coop_switch_pending;

void hmp_carry_coop_switch_ask(char const *map_name)
{
	csstrncpy(coop_switch_target, map_name, sizeof(coop_switch_target) - 1);
	coop_switch_target[sizeof(coop_switch_target) - 1] = 0;
	coop_switch_pending = TRUE;
}

void hmp_carry_coop_update(void)
{
	void network_game_server_port_cooperative_switch(char const *next_map);
#if defined(OXBOX_COOP_TEST)
	{
		void hmp_carry_coop_test(void);
		hmp_carry_coop_test();
	}
#endif
	if (!coop_switch_pending)
		return;
	coop_switch_pending = FALSE;
	if (!global_network_game_server_get() || !game_in_progress())
		return;
	platform_log("co-op: the host's script switches the game to %s: every console follows (the lobby, then the piece "
		"starts by itself once every console has it)", coop_switch_target);
	carry_record_network(coop_switch_target);
	network_game_server_port_cooperative_switch(coop_switch_target);
}

/* the carried vehicle for this player: made at the spawn (or the one made for a co-op player in it already) */
static long carry_vehicle(struct carry_player const *p, real_point3d const *position, real facing)
{
	struct object_placement_data placement;
	long definition_index = tag_loaded(VEHICLE_DEFINITION_TAG, p->vehicle), vehicle_index;
	short k;
	for (k = 0; k < CARRY_PLAYERS; k++)
		if (carry->made[k] != NONE && carry->made_for_old[k] == p->old_vehicle && p->old_vehicle != NONE)
			return carry->made[k];
	if (definition_index == NONE)
	{
		platform_log("hmp carry: the vehicle %s isn't in this map: left out", p->vehicle);
		return NONE;
	}
	object_placement_data_new(&placement, definition_index, NONE);
	placement.position = *position;
	vector3d_from_angle(&placement.forward, facing);
	placement.up = *global_up3d;
	vehicle_index = object_new(&placement);
	if (vehicle_index == NONE)
		return NONE;
	if (!object_try_and_get_and_verify_type(vehicle_index, _object_mask_vehicle))
	{
		object_delete(vehicle_index);
		return NONE;
	}
	object_get(vehicle_index)->object.body_vitality = p->vehicle_body;
	object_get(vehicle_index)->object.shield_vitality = p->vehicle_shield;
	for (k = 0; k < CARRY_PLAYERS && carry->made[k] != NONE; k++)
		;
	if (k < CARRY_PLAYERS)
	{
		carry->made[k] = vehicle_index;
		carry->made_for_old[k] = p->old_vehicle;
	}
	return vehicle_index;
}

boolean hmp_carry_spawned(long player_index, short local_player_index, long unit_index, real_point3d const *position,
	real facing, char const *profile_name)
{
	struct carry_player p;
	struct unit_datum *unit;
	struct network_game *game = carry && carry->network ? network_game_get_game() : NULL;
	char const *map = carry && carry->network ? (game ? game->map.name : "") : main_get_map_name();
	short k, hand_slot = NONE, machine_index = NONE, controller_index = local_player_index, left = 0;
	if (!carry)
		return FALSE;
	/* (a record for another map than this: a game started some other way since) */
	if (_stricmp(map, carry->target))
	{
		platform_log("hmp carry: the record is for %s, not %s: dropped", carry->target, map);
		hmp_carry_clear();
		return FALSE;
	}
	if (carry->network)
	{
		struct player_datum *player = player_get(player_index);
		machine_index = (short)player->network_player_data.machine_index;
		controller_index = (short)player->network_player_data.controller_index;
	}
	for (k = 0; k < CARRY_PLAYERS && !(carry->players[k].present && carry->players[k].machine_index == machine_index &&
		carry->players[k].controller_index == controller_index); k++)
		;
	if (k == CARRY_PLAYERS)
		return FALSE;
	/* (the first spawn uses the record up, whatever its profile) */
	p = carry->players[k];
	carry->players[k].present = FALSE;
	for (k = 0; k < CARRY_PLAYERS; k++)
		left += carry->players[k].present;
#if defined(OXBOX_COOP_TEST)
	/* (the test: Bungie's levels' starting profiles count as hmp_carry) */
	if (unit_index == NONE)
#else
	if (!profile_name || _strnicmp(profile_name, CARRY_PROFILE_NAME, 32) || unit_index == NONE)
#endif
	{
		if (!left)
			hmp_carry_clear();
		return FALSE;
	}
	unit = unit_get(unit_index);
	unit_delete_all_weapons(unit_index);
	for (k = 0; k < p.weapon_count; k++)
	{
		struct carry_weapon const *w = &p.weapons[k];
		struct object_placement_data placement;
		long definition_index = tag_loaded(WEAPON_DEFINITION_TAG, w->tag), weapon_index;
		short m, slot;
		if (definition_index == NONE)
		{
			platform_log("hmp carry: the weapon %s isn't in this map: left out", w->tag);
			continue;
		}
		object_placement_data_new(&placement, definition_index, unit_index);
		weapon_index = object_new(&placement);
		if (weapon_index == NONE)
			continue;
		if (!object_try_and_get_and_verify_type(weapon_index, _object_mask_weapon))
		{
			object_delete(weapon_index);
			continue;
		}
		for (m = 0; m < 2; m++)
		{
			weapon_get(weapon_index)->weapon.magazines[m].rounds_loaded = w->rounds_loaded[m];
			weapon_get(weapon_index)->weapon.magazines[m].rounds_total = w->rounds_total[m];
		}
		weapon_get(weapon_index)->weapon.age = w->age;
		weapon_get(weapon_index)->weapon.heat = w->heat;
		if (!unit_add_weapon_to_inventory(unit_index, weapon_index, _unit_add_weapon_normal))
		{
			object_delete(weapon_index);
			continue;
		}
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			if (unit->unit.weapon_object_indices[slot] == weapon_index && k == p.in_hand)
				hand_slot = slot;
	}
	memcpy(unit->unit.grenade_counts, p.grenades, sizeof(p.grenades));
	unit->object.body_vitality = p.body;
	unit->object.shield_vitality = p.shield;
	if (p.vehicle[0])
	{
		long vehicle_index = carry_vehicle(&p, position, facing);
		if (vehicle_index != NONE)
		{
			struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);
			if (p.seat[0])
				unit_scripting_enter_vehicle(unit_index, vehicle_index, p.seat);
			/* (the seat isn't there, or is taken: any it can use) */
			for (k = 0; k < definition->unit.seats.count && unit_get(unit_index)->object.parent_object_index == NONE; k++)
				unit_scripting_enter_vehicle(unit_index, vehicle_index,
					TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, k, struct unit_seat)->label);
		}
	}
	/* (the weapon in hand last: entering a seat chooses one) */
	if (hand_slot != NONE)
	{
		player_control_set_desired_weapon(unit_index, hand_slot);
		unit_get(unit_index)->unit.desired_weapon_index = hand_slot;
	}
	platform_log("hmp carry: player (console %d, controller %d) given what was carried (%d weapons%s%s)",
		(int)machine_index, (int)controller_index, (int)p.weapon_count, p.vehicle[0] ? ", in " : "", p.vehicle);
	/* (every record given: the memory back; a later vehicle share has nobody left to share it) */
	if (!left)
		hmp_carry_clear();
	return TRUE;
}

#endif


#if defined(HALO_XBOX_TARGET) && defined(OXBOX_COOP_TEST)
/* (the test build: the host's lobby made co-op on b30; 20 s into the game every player's state set and the map switched
as a piece's script does; 15 s into the next round every player's state reported) */
void hmp_carry_coop_test(void)
{
	extern boolean ui_widget_port_cooperative_level_choose(char const *map_name, short difficulty);
	extern boolean network_game_server_playing(struct network_game_server *server);
	extern boolean network_game_server_game_is_loading(struct network_game_server *server);
	static int stage = 1;                   /* (network_test.c sets up the co-op game: debug.network_test) */
	static unsigned long since;
	struct data_iterator iterator;
	struct player_datum *player;
	unsigned long now = GetTickCount();
	if (!global_network_game_server_get())
		return;
	if (stage == 0 && !network_game_server_playing(global_network_game_server_get()) &&
		!network_game_server_game_is_loading(global_network_game_server_get()))
	{
		static unsigned long lobby_since;
		if (!lobby_since)
			lobby_since = now;
		if (now - lobby_since > 45000 && ui_widget_port_cooperative_level_choose("levels\\b30\\b30", 1))
		{
			platform_log("co-op test: the lobby is co-op on b30");
			stage = 1;
		}
		return;
	}
	if ((stage == 1 || stage == 3) && network_game_server_playing(global_network_game_server_get()) && !since)
		since = now;
	if (stage == 1 && since && now - since > 100000)
	{
		data_iterator_new(&iterator, player_data);
		while ((player = data_iterator_next(&iterator)) != NULL)
			if (player->unit_index != NONE)
			{
				struct unit_datum *unit = unit_get(player->unit_index);
				unit->unit.grenade_counts[0] = (char)(2 + player->network_player_data.machine_index);
				unit->unit.grenade_counts[1] = 1;
				unit->object.body_vitality = 0.5f + 0.1f * player->network_player_data.machine_index;
			}
		platform_log("co-op test: every player's state set (grenades 2+console/1, body 0.5+0.1 x console): switching");
		{
			void main_set_map_name(char const *map_name);
			main_set_map_name("levels\\a30\\a30");
		}
		stage = 3;
		since = 0;
		return;
	}
	if (stage == 3 && !since)
	{
		/* (between the rounds: what the precache is doing, every 10 s) */
		static unsigned long said;
		extern boolean cache_files_precache_in_progress(void);
		extern short cache_files_precache_map_status(real *progress);
		extern boolean cache_files_precache_map_loaded(char const *map_name);
		extern char *main_get_multiplayer_map_name(void);
		if (now - said > 10000)
		{
			real progress = 0.0f;
			boolean busy = cache_files_precache_in_progress();
			short status = busy ? cache_files_precache_map_status(&progress) : -1;
			said = now;
			platform_log("co-op test: between rounds: multiplayer map %s (loaded %d), precache %s (status %d, %.2f)",
				main_get_multiplayer_map_name(), (int)cache_files_precache_map_loaded(main_get_multiplayer_map_name()),
				busy ? "busy" : "idle", (int)status, progress);
		}
	}
	if (stage == 3 && since && now - since > 60000)
	{
		stage = 4;
		data_iterator_new(&iterator, player_data);
		while ((player = data_iterator_next(&iterator)) != NULL)
			if (player->unit_index != NONE)
			{
				struct unit_datum *unit = unit_get(player->unit_index);
				platform_log("co-op test: console %d's player now: grenades %d/%d, body %.2f, %d weapons",
					(int)player->network_player_data.machine_index, (int)unit->unit.grenade_counts[0],
					(int)unit->unit.grenade_counts[1], unit->object.body_vitality,
					(int)(unit->unit.weapon_object_indices[0] != NONE) + (int)(unit->unit.weapon_object_indices[1] != NONE));
			}
	}
}
#endif
