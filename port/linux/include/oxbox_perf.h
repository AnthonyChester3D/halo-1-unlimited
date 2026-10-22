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
  from the mark before it (mark). */

#ifndef OXBOX_PERF_H
#define OXBOX_PERF_H

struct profile_section;

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
	_oxbox_perf_draw_hud,
	_oxbox_perf_draw_other,
	_oxbox_perf_draw_end,                   /* (the window's end: the rest, and the window's counts) */
	NUMBER_OF_OXBOX_PERF_STEPS
};

extern struct oxbox_perf_hooks *oxbox_perf;

#define OXBOX_PERF_MARK(step) { if (oxbox_perf) oxbox_perf->mark(step); }

#endif
