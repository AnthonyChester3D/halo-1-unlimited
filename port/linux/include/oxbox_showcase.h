/* oxbox_showcase.h: the showcase screensaver (oxbox, the builds with OXBOX_SHOWCASE: port/linux/game/showcase.c).

When the main menu has waited its attract time with nobody pressing anything, the Xbox hosts a closed system link
game with bots on a folder map that has a showcase.txt (Halo Map Porter writes one beside the map's info.txt and
preview.bin) and flies that file's camera shots over it while the bots play, instead of an attract movie. Any
button ends it and brings the main menu back.

The hook sites, each inside #if defined(OXBOX_SHOWCASE):
- attract_mode.c: attract_mode_start (showcase_start, else the movie as before) and the time it waits
  (showcase_idle_milliseconds: opence.ini's showcase_seconds).
- main.c main_loop, once a frame before the menus (showcase_update).
- observer.c observer_get_camera: the local player's camera while it flies (showcase_camera): the view drawn, and
  the sound and the fog, follow it. (The effects and particles shown are the ones where the local player can see:
  showcase.c puts the watching player, who has no unit, in the camera's cluster.)
- render.c render_window: no HUD (showcase_active), and its fade to black over everything (showcase_render).
- players.c: the local player isn't spawned (it only watches).
- ui_widget.c: no pause menu (a press ends the showcase instead).
- network_server_message_handler.c and network_server_manager.c: the game isn't advertised, listed or joinable.
- hmp_stats.c: its games aren't counted as plays.
- bots.c: bots_showcase, the bots it asks for, and the host's own bot setting back afterwards; and no lobby bots
  line over its black. */

#ifndef OXBOX_SHOWCASE_H
#define OXBOX_SHOWCASE_H

struct observer_result;

/* the main menu's attract time is up: TRUE when the showcase starts (or is already running), FALSE for the movie
(it's off, no folder map has a showcase.txt, or hosting failed a moment ago) */
boolean showcase_start(void);
/* how long the main menu waits for a press before the attract time is up (opence.ini's showcase_seconds; Halo's
own 75 seconds) */
unsigned long showcase_idle_milliseconds(void);
/* once a frame (main.c), before the menus see the frame's presses */
void showcase_update(boolean main_menu_loaded);
/* from its start until the main menu is back */
boolean showcase_active(void);
/* the camera a local player's view and sound use: the showcase's while it flies, else the observer's */
struct observer_result const *showcase_camera(short local_player_index, struct observer_result const *camera);
/* its fade to black, after the HUD and the menus are drawn */
void showcase_render(void);

/* bots.c: the showcase's bots (a count: custom, at normal), or 0: the host's own setting back */
void bots_showcase(short count);

#endif
