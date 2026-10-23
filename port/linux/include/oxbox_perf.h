/* oxbox_perf.h: the hooks of the developers' performance and memory log (oxbox: port/oxbox/oxbox_perf.c).

The log's code, data and buffers are in an XBE section of their own (OXPERF), which isn't loaded at boot. With
perf_log = 1 in opence.ini, start-up loads it (oxbox_sdk.c oxbox_perf_load) and points oxbox_perf at its hooks;
otherwise oxbox_perf stays NULL and each hook site is a test of it.

The hook sites (test build 32):
- main_loop, once a frame (frame).
- The game's own profile sections (profile.h's profile_enter and profile_exit: the retail profiler never runs on
  the Xbox, and on the Xbox its test of profile_global_enable is this test instead) (enter, exit).
- The steps of a game tick (game.c game_tick, its profile_scope) and of a player's render window (render.c
  render_window, render_objects.c render_objects), in order: each OXBOX_PERF_MARK ends the step it names, timed
  from the mark before it (mark).

Test build 33 (phase 3):
- The HUD's parts (interface.c, hud.c, hud_unit.c, hud_weapon.c): more marks.
- Objects (objects.c objects_update, object_update, object_translate): the steps of each object's update end with
  OXBOX_PERF_OBJECT_MARK, inline (the cycle counter, a subtraction and two stores: well under a microsecond an
  object), and the objects' types, the children and the created objects go to the object hook.
- The weather's counts (weather_particle_systems.c, a type at a time: the weather hook).

Test build 34: the camera tour. main.c main_game_render asks the camera hook for each local player's view: the
observer's camera, or the tour's while a tour flies that view. */

#ifndef OXBOX_PERF_H
#define OXBOX_PERF_H

struct profile_section;
struct observer_result;

struct oxbox_perf_hooks
{
	/* once a frame, at its end (main_loop): the frame's probe marks (main.c's frame_probe_marks, LARGE_INTEGERs:
	start, input, network, menus, network end, ticks, camera, drawing, gpu wait, present) */
	void (*frame)(void const *marks, long mark_count);
	/* a profile section's start and end (profile.h) */
	void (*enter)(struct profile_section *section);
	void (*exit)(struct profile_section *section);
	/* the end of a step (enum oxbox_perf_step) */
	void (*mark)(long step);
	/* objects_update's loop: a top-level object's update about to start (its absolute index), or the loop's end
	(_oxbox_perf_object_event_loop_end); and an object's children's update starting and ending, a created object's
	update starting */
	void (*object)(long type_or_event);
	/* the weather, a particle type drawn: its particles (updated this frame), the sprites built, the boxes and the
	visible weather polyhedra */
	void (*weather)(long particles, long sprites, long boxes, long polyhedra);
	/* a local player's view about to be drawn (test build 34): the camera to draw it from, the observer's or the
	camera tour's */
	struct observer_result const *(*camera)(short local_player_index, struct observer_result const *observer);
	/* (OXBOX_PERF_OBJECT_MARK's and OXBOX_PERF_TRANSLATE's state: the log adds these up a frame and clears them) */
	unsigned long object_last;
	unsigned long object_cycles[16];
	unsigned long object_counts[16];
	unsigned long translate_start, translate_cycles, translate_count;
	/* (while object_translate runs: structure_clusters_in_sphere's queries and the clusters they found) */
	unsigned long translate_active, translate_cluster_queries, translate_clusters;
};

/* the steps OXBOX_PERF_MARK ends (a game tick's start is its profile section game_update's start) */
enum oxbox_perf_step
{
	/* game_tick */
	_oxbox_perf_tick_units,
	_oxbox_perf_tick_ai,                    /* (a distributed client: driving the host's actors) */
	_oxbox_perf_tick_players_before,
	_oxbox_perf_tick_effects,
	_oxbox_perf_tick_antennas,
	_oxbox_perf_tick_first_person,
	_oxbox_perf_tick_game_engine,
	_oxbox_perf_tick_hs,
	_oxbox_perf_tick_recorded_animations,
	_oxbox_perf_tick_objects,
	_oxbox_perf_tick_players_after,
	_oxbox_perf_tick_hud,
	_oxbox_perf_tick_player_effect,
	_oxbox_perf_tick_other,                 /* (cheats, quitting players, allegiances, the editor) */
	/* render_window */
	_oxbox_perf_draw_start,                 /* (times nothing: the window's first mark) */
	_oxbox_perf_draw_visibility,
	_oxbox_perf_draw_sky,                   /* (and the window's setup) */
	_oxbox_perf_draw_first_person_weapon,   /* (its update before the objects, and its draw among them) */
	_oxbox_perf_draw_lights,
	_oxbox_perf_draw_objects,
	_oxbox_perf_draw_lightmaps,
	_oxbox_perf_draw_lens_flares,
	_oxbox_perf_draw_object_shadows,
	_oxbox_perf_draw_diffuse_lights,
	_oxbox_perf_draw_decals,
	_oxbox_perf_draw_diffuse_textures,
	_oxbox_perf_draw_specular_lights,
	_oxbox_perf_draw_reflections,
	_oxbox_perf_draw_structure_transparent,
	_oxbox_perf_draw_fog,
	_oxbox_perf_draw_weather,
	_oxbox_perf_draw_particles,
	_oxbox_perf_draw_contrails,
	_oxbox_perf_draw_transparent,           /* (the sorted transparent list) */
	_oxbox_perf_draw_detail_objects,
	_oxbox_perf_draw_other,
	_oxbox_perf_draw_end,                   /* (the window's end: the rest, and the window's counts) */
	/* the HUD (test build 33): interface_draw_screen's parts, then render_window's last two */
	_oxbox_perf_hud_screen_effect,
	_oxbox_perf_hud_players,                /* (team indicators, names) */
	_oxbox_perf_hud_motion_sensor,          /* (its update and its draw) */
	_oxbox_perf_hud_crosshair,
	_oxbox_perf_hud_weapon,
	_oxbox_perf_hud_unit,
	_oxbox_perf_hud_nav_damage,             /* (nav points, damage indicators) */
	_oxbox_perf_hud_messages,
	_oxbox_perf_hud_other,                  /* (the HUD's start and end, the temporary HUD) */
	_oxbox_perf_hud_game_engine,            /* (game_engine_post_rasterize: scores and the like) */
	_oxbox_perf_draw_screen_flash,
	_oxbox_perf_draw_ui_widgets,
	NUMBER_OF_OXBOX_PERF_STEPS
};

/* the steps of objects_update and object_update (test build 33): OXBOX_PERF_OBJECT_MARK ends one, timed from the
mark before it (objects_update's profile section's start is the first) */
enum oxbox_perf_object_step
{
	_oxbox_perf_object_activation,          /* (comparing the active clusters) */
	_oxbox_perf_object_activation_changed,  /* (activating and deactivating when they changed: counted) */
	_oxbox_perf_object_type_update,         /* (counted: every object update) */
	_oxbox_perf_object_damage,
	_oxbox_perf_object_export_functions,
	_oxbox_perf_object_node_matrices,
	_oxbox_perf_object_functions_colors,
	_oxbox_perf_object_connect_lights,
	_oxbox_perf_object_postprocess,         /* (and the children's update's own overhead) */
	_oxbox_perf_object_create_delete,
	_oxbox_perf_object_garbage,
	NUMBER_OF_OXBOX_PERF_OBJECT_STEPS
};

/* the object hook's events (besides an object's absolute index, 0 and up) */
enum
{
	_oxbox_perf_object_event_loop_end = -1,
	_oxbox_perf_object_event_children_begin = -2,
	_oxbox_perf_object_event_children_end = -3,
	_oxbox_perf_object_event_created = -4
};

extern struct oxbox_perf_hooks *oxbox_perf;

#define OXBOX_PERF_MARK(step) { if (oxbox_perf) oxbox_perf->mark(step); }
#define OXBOX_PERF_OBJECT(type_or_event) { if (oxbox_perf) oxbox_perf->object(type_or_event); }

static __inline__ unsigned long oxbox_perf_cycles(void)
{
	unsigned long low, high;

	__asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
	return low;
}

#define OXBOX_PERF_OBJECT_MARK(step) \
	{ struct oxbox_perf_hooks *oxbox_perf_p = oxbox_perf; if (oxbox_perf_p) { unsigned long oxbox_perf_t = oxbox_perf_cycles(); \
		oxbox_perf_p->object_cycles[step] += oxbox_perf_t - oxbox_perf_p->object_last; oxbox_perf_p->object_last = oxbox_perf_t; } }
#define OXBOX_PERF_OBJECT_MARK_COUNTED(step) \
	{ struct oxbox_perf_hooks *oxbox_perf_p = oxbox_perf; if (oxbox_perf_p) { unsigned long oxbox_perf_t = oxbox_perf_cycles(); \
		oxbox_perf_p->object_cycles[step] += oxbox_perf_t - oxbox_perf_p->object_last; oxbox_perf_p->object_last = oxbox_perf_t; \
		oxbox_perf_p->object_counts[step]++; } }
/* object_translate's start and end: its own time, all it calls included, apart from the object steps */
#define OXBOX_PERF_TRANSLATE_START() \
	{ if (oxbox_perf) { oxbox_perf->translate_active = 1; oxbox_perf->translate_start = oxbox_perf_cycles(); } }
#define OXBOX_PERF_TRANSLATE_END() \
	{ struct oxbox_perf_hooks *oxbox_perf_p = oxbox_perf; if (oxbox_perf_p) { \
		oxbox_perf_p->translate_cycles += oxbox_perf_cycles() - oxbox_perf_p->translate_start; oxbox_perf_p->translate_count++; \
		oxbox_perf_p->translate_active = 0; } }
/* structure_clusters_in_sphere's result: the clusters an object_translate touches (test build 33) */
#define OXBOX_PERF_TRANSLATE_CLUSTERS(count) \
	{ struct oxbox_perf_hooks *oxbox_perf_p = oxbox_perf; if (oxbox_perf_p && oxbox_perf_p->translate_active) { \
		oxbox_perf_p->translate_cluster_queries++; oxbox_perf_p->translate_clusters += (count); } }
#define OXBOX_PERF_WEATHER(particles, sprites, boxes, polyhedra) \
	{ if (oxbox_perf) oxbox_perf->weather(particles, sprites, boxes, polyhedra); }

#endif
