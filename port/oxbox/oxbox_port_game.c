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

/* ---------- OpenCE's PC hooks after the merge with OpenCE of Oct 31 2026 (the Server branch), answered as the Xbox
plays: stock model detail, no remapped controller, no tone mapping, no Custom Edition bitmaps, no server */
unsigned char const controller_remap_controls[10] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11};
int controller_remap_read(short port, unsigned char *buttons) { return 0; }
int controller_remap_connected(short port) { return 0; }
unsigned int controller_remap_held(short port) { return 0; }
unsigned int controller_remap_pressed(short port, unsigned int mask) { return 0; }
void controller_remap_input_reset(void) { }
struct bitmap_data;
boolean custom_edition_bitmap_drawable(struct bitmap_data *bitmap) { return 1; }
void custom_edition_bitmaps_dispose(void) { }
boolean custom_edition_loaded_reordered_bitmaps_find(void) { return 0; }
void d3d8_gl_set_height_fade(float inverse_softness, float height, float offset, float scale) { }
int halo_chat_touch_layout(void) { return 0; }
void halo_custom_edition_hud_meter(int active) { }
int halo_fixed_step(void) { return 0; }
int halo_linux_debug_camera_input(int *middle, int *up, int *down, float *wheel) { return 0; }
float halo_model_detail_scale(int level) { return 1.0f; }
int halo_model_detail_stock(void) { return 1; }
void halo_screen_tone_map(short x0, short y0, short x1, short y1) { }
unsigned long network_client_timeout_milliseconds(void) { return 15000; }   /* (OpenCE's default, 15 seconds) */
struct icon_hud_element_definition;
boolean hud_hires_button_draw(struct icon_hud_element_definition const *icon, void const *bounds, void const *color)
{
	return 0;
}
struct bitmap_data const *hud_hires_crosshair_bitmap(struct bitmap_data const *bitmap, short sequence_index,
	short frame_index)
{
	return bitmap;
}
boolean object_bounds_cache_missed_by_vector(long object_index, void const *point, void const *vector) { return 0; }
void network_votekick_draw(short local_player_index) { }
int map_share_map_downloaded(char const *map_name) { return 0; }
void master_list_browse(int on) { }
struct widget_instance;
unsigned char pc_menu_screen_reads_every_controller(struct widget_instance const *widget) { return 0; }
unsigned char pc_menu_screen_routes_by_controller(struct widget_instance const *widget) { return 0; }
int player_link_join_fill(unsigned long host_virtual_address, unsigned char *pass, unsigned char *proof) { return 0; }
int player_link_join_refused(int rejection_code) { return 0; }
void player_link_join_reset(void) { }
int player_link_pass_shown(const unsigned char *pass) { return 0; }
int player_link_server_check(const unsigned char *pass, const unsigned char *proof, unsigned long virtual_address,
	char *discord_id, int discord_id_size, char *discord_name, int discord_name_size, unsigned long *created,
	char *why, int why_size)
{
	return 0;
}
long port_mods_at(long position) { return -1; }
long port_mods_count(void) { return 0; }
/* (an address for the log, whole: the Xbox's log stays on the Xbox) */
int halo_linux_snprintf(char *buffer, unsigned int count, char const *format, ...);
const char *log_address_ipv4(unsigned long network_address, unsigned short network_port, char *text, int size)
{
	unsigned char const *b = (unsigned char const *)&network_address;

	halo_linux_snprintf(text, (unsigned int)size, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3],
		(unsigned)(unsigned short)((network_port >> 8) | (network_port << 8)));
	return text;
}
const char *log_address(const unsigned char *bytes, int length, int port, char *text, int size)
{
	if (length == 4)
		halo_linux_snprintf(text, (unsigned int)size, "%u.%u.%u.%u:%d", bytes[0], bytes[1], bytes[2], bytes[3], port);
	else
		halo_linux_snprintf(text, (unsigned int)size, "(IPv6):%d", port);
	return text;
}
/* (no mods on the Xbox: port/linux/src/port_mods.c) */
int port_mods_describe(long index, char *text, unsigned long size, const char *map_name) { return 0; }
long port_mods_file_find(const char *tag_file, const char *map_name) { return -1; }
const char *port_mods_file_mod(long file) { return ""; }
long port_mods_file_order(long file) { return 0; }
const char *port_mods_file_path(long file) { return ""; }
int port_mods_file_read(long file, unsigned long offset, void *buffer, unsigned long size) { return 0; }
int port_mods_file_stamp(long file, unsigned long *size, unsigned long stamp[2]) { return 0; }
int port_mods_list(char *text, unsigned long size) { if (size) text[0] = 0; return 0; }
long port_mods_played(void) { return 0; }
long port_mods_scan(void) { return 0; }
unsigned long port_mods_scans(void) { return 0; }
void render_interpolation_object_point(long object_index, void *point) { }
void tag_validate_custom_edition_note_structure_bsp(void *tag_header, unsigned long tag_data_bytes, long bsp_index,
	void *bsp, unsigned long bsp_bytes, unsigned long bsp_address, char const *map_name)
{
}

/* ---------- the OpenCE merge's PC extras (gen_xbox.py's PC_ONLY: 600 KB of the Xbox's memory), each off */

/* streamer mode's word filter (text_filter*.c): everything drawn as it is */
char const *text_filter_string(char const *string) { return string; }
unsigned short const *text_filter_unicode(unsigned short const *string) { return string; }
void text_filter_suspend(void) { }
void text_filter_resume(void) { }

/* campaign co-op's extra vehicles (coop_vehicles.c): a host of none */
void coop_vehicles_new_game(void) { }
void coop_vehicles_register(long object_index, void const *source) { }
void coop_vehicles_update(void) { }
void coop_vehicles_before_save(void) { }
void coop_vehicles_after_load(void) { }

/* the CPU's structure culling (structure_culling.c): the original walk of the visible surfaces, as before the merge */
boolean structure_culling_traverse(void *structure) { return 0; }
void structure_culling_initialize(void) { }
void structure_culling_dispose(void) { }
void structure_culling_structure_bsp_loaded(void) { }
void structure_culling_set_tests(long tests) { }
void structure_culling_debug(boolean enable) { }
void structure_culling_render_debug(void) { }

/* OpenCE's split-screen bots (local_bots.c; Halo 1 Unlimited's own bots are bots.c): none wanted */
long bots_wanted_count(void) { return 0; }
short bots_teams(void) { return 0; }
int bots_machine_is_bot(long machine_index) { return 0; }
void bots_network_player(long bot_index, void *player) { }
void bots_update_actions(void *actions) { }
void bots_initialize_for_new_map(void) { }
void bots_dispose_from_old_map(void) { }

/* tag mods (mods.c, mod_*.c; their files are port_mods.c's, also stubbed): none */
void mods_tags_loaded(char const *map_name) { }
void mods_tags_unloaded(void) { }
void mods_reload(void) { }
void mods_enable(boolean enabled) { }
void mods_list(void) { }
boolean mod_bitmaps_contain(void const *bitmap) { return 0; }
boolean mod_sounds_read(void const *permutation, void *buffer) { return 0; }

/* text chat (network_chat.c): lines from a PC host are passed over; their entries' sizes are kept, so a stream
holding them still reads */
unsigned short network_chat_line_entry_size(void) { return 68; }     /* (struct distributed_chat_line) */
unsigned short network_chat_config_entry_size(void) { return 4; }    /* (struct distributed_chat_config) */
void network_chat_handle_up(long machine_index, void const *entries, boolean from_stream) { }
void network_chat_handle_down(void const *entries, boolean from_stream) { }
void network_chat_handle_config(void const *entries, boolean from_stream) { }
void network_chat_host_notice(char const *text) { }
boolean network_chat_name_blocked(unsigned short const *name, short length) { return 0; }
boolean network_chat_typing(void) { return 0; }
short network_chat_controller(void) { return -1; }
void network_chat_note_motion_sensor(short local_player_index, short left, short top) { }
void network_chat_draw(short local_player_index) { }
short network_chat_host_mute_minutes(short player_index) { return 0; }
void network_chat_host_mute(short player_index, boolean mute) { }

/* a dedicated server's VPN door and linked accounts (network_link.c): an Xbox is never one, so nothing refused */
void network_link_set_machine_pass(long machine_index, void const *pass, void const *proof, char const *name) { }
void network_link_check_pass(long machine_index, unsigned long address) { }
short network_link_door(void const *game, long machine_index, unsigned long real_address, boolean local) { return -1; }
short network_link_address_limit(void const *game, long machine_index, unsigned long real_address, boolean local)
{
	return -1;
}
boolean network_link_machine_verified(long machine_index, char *discord_id, long discord_id_size, char *discord_name,
	long discord_name_size)
{
	return 0;
}
