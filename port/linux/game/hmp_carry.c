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
- Split-screen co-op: the same per local player. Network co-op isn't carried (a script's map_name doesn't switch a
  network game's map).
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

#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);

#define CARRY_PROFILE_NAME "hmp_carry"
#define CARRY_PATH 128
#define CARRY_PLAYERS 4                     /* (MAXIMUM_NUMBER_OF_LOCAL_PLAYERS) */

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

void hmp_carry_record(void)
{
	short local_player_index, recorded = 0;
	hmp_carry_clear();
	/* (malloc: in the game's code it's the game's own allocator, as free is; calloc isn't) */
	if (game_engine_running() || !(carry = malloc(sizeof(*carry))))
		return;
	csmemset(carry, 0, sizeof(*carry));
	for (local_player_index = 0; local_player_index < CARRY_PLAYERS; local_player_index++)
		carry->made[local_player_index] = carry->made_for_old[local_player_index] = NONE;
	for (local_player_index = 0; local_player_index < CARRY_PLAYERS; local_player_index++)
	{
		struct carry_player *p = &carry->players[local_player_index];
		long player_index = local_player_get_player_index(local_player_index);
		struct unit_datum *unit;
		short slot;
		if (player_index == NONE || player_get(player_index)->unit_index == NONE)
			continue;
		unit = unit_get(player_get(player_index)->unit_index);
		if (TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
			continue;
		p->present = TRUE;
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
		recorded++;
		platform_log("hmp carry: player %d recorded: %d weapons (number %d in hand), %s%s", (int)local_player_index,
			(int)p->weapon_count, (int)p->in_hand, p->vehicle[0] ? "in " : "on foot", p->vehicle);
	}
	if (!recorded)
		hmp_carry_clear();
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

boolean hmp_carry_spawned(short local_player_index, long unit_index, real_point3d const *position, real facing,
	char const *profile_name)
{
	struct carry_player p;
	struct unit_datum *unit;
	short k, hand_slot = NONE;
	if (!carry || local_player_index < 0 || local_player_index >= CARRY_PLAYERS ||
		!carry->players[local_player_index].present)
		return FALSE;
	/* (the first spawn uses the record up, whatever its profile) */
	p = carry->players[local_player_index];
	carry->players[local_player_index].present = FALSE;
	if (!profile_name || _strnicmp(profile_name, CARRY_PROFILE_NAME, 32) || unit_index == NONE)
		return FALSE;
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
	platform_log("hmp carry: player %d given what was carried (%d weapons%s%s)", (int)local_player_index,
		(int)p.weapon_count, p.vehicle[0] ? ", in " : "", p.vehicle);
	return TRUE;
}

#endif

