/*
OXBOX_PORT_GAME.C

The hooks of the port's PC-only game units, which the Xbox build leaves out (gen_xbox.py's PC_ONLY): Custom
Edition maps (custom_edition_*.c: 6.7 MB of tables), the tag validator (tag_validate*.c: 3 MB) and the PC
version's map sharing (map_share*.c). Each answers as if the feature found nothing: no Custom Edition map is
present or loaded, every Xbox map's tags pass, no download is in progress.
*/

#include "platform.h"

typedef unsigned char boolean;

/* ---------- Custom Edition maps */

boolean custom_edition_cache_playable(char const *level_name) { return 0; }
boolean custom_edition_cache_present(char const *level_name, unsigned long version, char *message, long message_size)
{
	if (message && message_size > 0) message[0] = 0;
	return 0;
}
void custom_edition_cache_read(long tag_index, long offset, long size, void *buffer) { }
boolean custom_edition_cache_refuse(void const *header, char const *build, char const *path) { return 0; }
boolean custom_edition_cache_relies_on(short behaviour) { return 0; }
void *custom_edition_cache_tags_load(char const *map_name, void *header) { return NULL; }
boolean custom_edition_cache_tags_loaded(void) { return 0; }
void custom_edition_cache_tags_unload(void) { }
boolean custom_edition_level_name(char const *level_name) { return 0; }
boolean custom_edition_map_file_present(char const *map_name) { return 0; }
unsigned long custom_edition_map_version(char const *level_name) { return 0; }
short custom_edition_part_palette(void const *vertex_buffer, unsigned char const **nodes) { return 0; }
unsigned char *custom_edition_sounds_encode(unsigned char const *data, long data_bytes, short compression,
	boolean big_endian, long channels, long rate, long encoded_rate, unsigned long *encoded_bytes)
{
	if (encoded_bytes) *encoded_bytes = 0;
	return NULL;
}
boolean custom_edition_structure_bsp_load(void *structure_bsp) { return 1; }
boolean custom_edition_structure_bsp_reference_valid(void const *reference) { return 1; }
void custom_edition_structure_bsp_unload(void) { }
boolean custom_edition_vehicle_placement_allowed(void const *placement) { return 1; }
boolean custom_edition_vehicles_by_placement(void) { return 0; }

/* ---------- the tag validator: an Xbox map's tags as the Xbox takes them */

boolean tag_validate_structure_bsp(long tag_index, void *base, long size) { return 1; }
boolean tag_validate_tags(void *tag_header, long tag_data_size, long file_length, char const *map_name) { return 1; }

/* (the menus' maps and map sharing: port/linux/game/hmp_maps.c and hmp_share.c, Halo Universal's) */


/* ---------- frame interpolation (render_interpolation.c): off on the Xbox (display.interpolation false, frames
at the game's 30 ticks), so the unit (33 KB of records) is left out and each hook answers as it does when off:
no blended camera, objects or first-person pose, game time on the tick. The PC's direct camera
(display.direct_camera) is off too: the observer's camera, as the Xbox game. */

void render_interpolation_tick(void) { }
void render_interpolation_reset(void) { }
void render_interpolation_frame_begin(void) { }
void render_interpolation_frame_end(void) { }
float render_interpolation_fraction(void) { return 1.0f; }
void *render_interpolation_object_node_matrices(long object_index) { return NULL; }
void render_interpolation_correct_object(long object_index, void const *offset) { }
void const *render_interpolation_camera(short local_player_index, void const *observer) { return observer; }
void render_interpolation_first_person(short local_player_index, void *node_matrices, short node_count,
	void const *camera) { }
float render_interpolation_game_time_sec(long ticks) { return (float)ticks * (1.0f / 30.0f); }

/* ---------- the PC version's menus (menu_functions.c, menu_tags.c: ~90 KB): the Xbox build uses the Xbox's own
(display.menus "xbox"), for which those units load nothing and answer that no widget is theirs. Left out, these
answer the same. */

short players_port_local_player_count(void);

void menu_tags_loaded(char const *map_name) { }
void menu_tags_unloaded(void) { }
boolean pc_menu_tag(long tag_index) { return 0; }
boolean pc_menu_frame_placement(void const *bitmap, short *x, short *y, short *width, short *height) { return 0; }
char const *pc_menus_screen(char const *name) { return name; }
char const *pc_menus_root_name(void) { return "ui\\shell\\main_menu\\main_menu"; }
boolean pc_menu_event_function_invoke(void *widget, void *event, long function_index, boolean *widget_deleted)
{
	return 0;
}
void pc_menu_game_data_function_invoke(void *widget, long function) { }
void menu_functions_text_box_drawn(void *widget, void const *bounds) { }
/* (two or more players on this console: each controller keeps to its own player's menus; the PC lobby's own
counts are always 0 here) */
unsigned char pc_menu_split_players(void) { return players_port_local_player_count() >= 2; }

/* ---------- OpenCE's PC extras, left out of the Xbox build (test build 14, gen_xbox.py's PC_ONLY): each answers as
its feature does when switched off, which the Xbox build's config already had them */

typedef unsigned short word;
typedef float real;

/* first-person legs (Video Setup > Legs) and the mesh reader they use */
boolean first_person_legs_wanted(long object_index) { return 0; }
void first_person_legs_reset(void) { }
void const *first_person_legs_node_matrices(long object_index, void const *nodes) { return nodes; }
void const *object_mesh_get(long object_index) { return NULL; }
void object_mesh_reset(void) { }

/* shield glow and shield color (Video Setup) */
void shield_glow_update(void) { }
void const *shield_color_colors(long unit_index, void const *colors) { return colors; }
boolean shield_color_override(void const *colors, void *chosen) { return 0; }
void shield_color_apply(void const *chosen, void const *original, void *result) { }

/* field of view and viewmodel settings: the stock view, the weapon drawn, no shield flare on the arms (the
port's defaults) */
real render_fov_vertical(short local_player_index, real native_vertical_field_of_view) { return native_vertical_field_of_view; }
real render_fov_reticle_scale(short local_player_index) { return 1.0f; }
void viewmodel_projection_begin(void) { }
void viewmodel_projection_end(void) { }
boolean viewmodel_is_visible(void) { return 1; }
boolean viewmodel_shield_is_visible(void) { return 0; }
boolean viewmodel_draws_geometry(boolean first_person) { return 1; }

/* sound tag files over a map's (audio.loose_sounds) */
void loose_sounds_tags_loaded(void) { }
void loose_sounds_tags_unloaded(void) { }
boolean loose_sounds_read(void const *permutation, void *buffer) { return 0; }
void loose_sounds_reload(void) { }
void loose_sounds_enable(boolean enabled) { }

/* vote kick (none on the Xbox: network.votekick was false); the entry sizes are its messages' */
void network_votekick_machine_joined(long machine_index) { }
boolean network_votekick_kept_out(unsigned long address, char const *hardware_id) { return 0; }
void network_votekick_handle_request(long machine_index, void const *entries) { }
word network_votekick_request_entry_size(void) { return 4; }
void network_votekick_new_game(void) { }
void network_votekick_host_tick(void) { }
void network_votekick_handle_status(void const *entries) { }
word network_votekick_status_entry_size(void) { return 8; }
boolean network_votekick_get_status(void *status) { return 0; }
short network_votekick_matching_player_names(char const *text, void *names, short maximum) { return 0; }
boolean network_votekick_player_named(char const *text) { return 0; }
boolean network_votekick_host_kick(short player_index, boolean ban) { return 0; }
boolean network_votekick_host(void) { return 0; }
boolean network_votekick_available(void) { return 0; }
boolean network_votekick_request(short player_index) { return 0; }

/* voice chat (none on the Xbox): its messages, should one come, are taken and dropped (the type is the third byte
of the distributed header: _distributed_message_voice_up/down/config, 80-82) */
boolean network_voice_handles_message(word const *message, word size)
{
	unsigned char type = size >= 8 ? ((unsigned char const *)message)[2] : 0;
	return type >= 80 && type <= 82;
}
void network_voice_handle_message(long machine_index, word const *message, word size) { }
void network_voice_machine_joined(long machine_index) { }
void network_voice_update(void) { }
boolean network_voice_machine_speaking(long machine_index) { return 0; }
boolean network_voice_machine_muted(long machine_index) { return 0; }
void network_voice_mute_machine(long machine_index, boolean muted) { }
boolean network_voice_available(void) { return 0; }
void network_voice_draw_icon(void const *bounds, long machine_index) { }

/* the high-res HUD's tags (no high-res HUD on the Xbox) */
void hud_hires_tags_loaded(void) { }
void hud_hires_tags_unloaded(void) { }
void const *hud_hires_sprite_bitmap(void const *bitmap, short sequence_index) { return bitmap; }
long hud_hires_asset_at(unsigned long address, long width, long height) { return -1; }

/* collision's packed copy of the objects' bounding spheres (a PC speed-up, 48 KB): collision reads each object's
own, as stock */
void object_bounds_cache_update(long object_index, void const *center, real radius) { }
void object_bounds_cache_invalidate(void) { }
boolean object_bounds_cache_out_of_reach(long object_index, void const *point, real radius) { return 0; }
