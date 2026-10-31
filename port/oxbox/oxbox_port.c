/*
OXBOX_PORT.C

The native ports' platform hooks that OpenCE's game code calls (port/linux/src: SDL, OpenGL, internet play,
voice chat, the PC menus, high-resolution text and HUD, mouse and touch input, Custom Edition maps), answered
as the Xbox: a 640x480 screen, a gamepad, system link only, the Xbox's own menus. Settings are the ports'
defaults with the console's values where those differ (oxbox_config_table.h, from port_config.c).
*/

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- settings */

enum { _oxbox_config_boolean, _oxbox_config_integer, _oxbox_config_real, _oxbox_config_string };

static const struct { const char *name; int type; const char *value; } config_settings[] =
{
#include "../oxbox_config_table.h"
};

#if defined(OXBOX_NET_P2P)
/* settings opence.ini sets (oxbox_entry.c: the online test builds' online, online_stun ...), as the table has them:
true / false, a number, or a string in quotes */
static struct { const char *name; char value[192]; } config_overrides[8];

void oxbox_config_set(const char *name, const char *value)
{
	unsigned int index;

	for (index = 0; index < sizeof(config_overrides) / sizeof(config_overrides[0]); index++)
	{
		if (!config_overrides[index].name || !strcmp(config_overrides[index].name, name))
		{
			config_overrides[index].name = name;
			strncpy(config_overrides[index].value, value, sizeof(config_overrides[index].value) - 1);
			return;
		}
	}
}

#endif

static const char *config_value(const char *name)
{
	unsigned int index;

#if defined(OXBOX_NET_P2P)
	for (index = 0; index < sizeof(config_overrides) / sizeof(config_overrides[0]) && config_overrides[index].name;
		index++)
	{
		if (!strcmp(config_overrides[index].name, name))
			return config_overrides[index].value;
	}
#endif
	for (index = 0; index < sizeof(config_settings) / sizeof(config_settings[0]); index++)
		if (!strcmp(config_settings[index].name, name))
			return config_settings[index].value;
	return NULL;
}

int config_boolean(const char *name)
{
	const char *value = config_value(name);

	return value && !strcmp(value, "true");
}

long config_integer(const char *name)
{
	const char *value = config_value(name);

	return value ? strtol(value, NULL, 10) : 0;
}

double config_real(const char *name)
{
	const char *value = config_value(name);

	return value ? strtod(value, NULL) : 0.0;
}

/* never NULL: a string setting's text without its quotes */
const char *config_string(const char *name)
{
	static char strings[8][256];
	static int next;
	const char *value = config_value(name);
	char *out;
	size_t length;

	if (!value)
		return "";
	out = strings[next++ & 7];
	if (value[0] == '"')
	{
		length = strlen(value);
		if (length >= 2)
			length -= 2;
		if (length > 255)
			length = 255;
		memcpy(out, value + 1, length);
		out[length] = 0;
	}
	else
	{
		strncpy(out, value, 255);
		out[255] = 0;
	}
	return out;
}

int config_text(const char *name, char *text, size_t size)
{
	const char *value = config_value(name);

	if (!value || !size)
		return 0;
	strncpy(text, value[0] == '"' ? config_string(name) : value, size - 1);
	text[size - 1] = 0;
	return 1;
}

int config_default(const char *name, char *text, size_t size)
{
	return config_text(name, text, size);
}

int config_write(const char *name, const char *value) { return 0; }
int config_write_boolean(const char *name, int value) { return 0; }
unsigned long config_changes(void) { return 0; }

/* ---------- the screen: the Xbox's 640x480 */

long halo_screen_width(void) { return 640; }
long halo_screen_commit(void) { return 640; }
float halo_screen_scale(void) { return 1.0f; }
void halo_screen_ui_offset(unsigned char centered) { }
void halo_screen_ui_offset_view(long view_width) { }
long halo_screen_ui_offset_current(void) { return 0; }
void halo_screen_anti_alias(short x0, short y0, short x1, short y1) { }
long halo_shadow_map_scale(void) { return 1; }
int halo_interpolation_enabled(void) { return 0; }
void halo_vertex_shader_lighting(unsigned long handle) { }
volatile unsigned int *d3d_find_flipcount(void) { return NULL; }

/* ---------- input: the gamepad alone */

unsigned long halo_keyboard_actions(short controller_index) { return 0; }
void halo_input_name(int input, char *name, size_t size) { if (size) name[0] = 0; }
int halo_linux_mouse_aiming(short gamepad_index) { return 0; }
int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch) { return 0; }
int halo_linux_touch_aiming(short gamepad_index) { return 0; }
int halo_linux_touch_look(short gamepad_index, float *yaw, float *pitch, float *gyro_yaw, float *gyro_pitch) { return 0; }
int halo_linux_touch_move(short controller_index, float *forward, float *strafe) { return 0; }
int halo_push_to_talk_held(void) { return 0; }
int halo_ui_pointer_update(int menus_active, void *pointer) { return 0; }
int halo_scoreboard_pointer_update(int offered, void *pointer) { return -1; }   /* (no pointer on this platform) */
void test_input_hold_action(int hold) { }
void platform_binding_capture_begin(void) { }
int platform_binding_capture_poll(int *input) { return 0; }
void platform_scoreboard_scroll(int open, long *notches, long *pages) { if (notches) *notches = 0; if (pages) *pages = 0; }
void platform_text_field(int typing, int password) { }
void platform_text_typing(int typing) { }
int platform_clipboard_get(char *text, int size) { if (size > 0) text[0] = 0; return 0; }
void platform_clipboard_set(char const *text) { }

/* ---------- the PC version's menus, high-resolution text and HUD: none (the Xbox's own) */

const void *halo_menus_load(void) { return NULL; }
void halo_menus_art_forget(void) { }
void halo_menus_art_register(void const *texture, char const *png) { }
void halo_menus_log(char const *file, long line, char const *message, char const *detail)
{
	platform_log("menus: %s:%ld %s %s", file, line, message, detail ? detail : "");
}

long halo_menus_utf16(char const *utf8, unsigned short *out, long capacity)
{
	long count = 0;

	while (*utf8 && count < capacity - 1)
	{
		unsigned char c = (unsigned char)*utf8++;
		unsigned long code = c;

		if (c >= 0xC0 && (*utf8 & 0xC0) == 0x80)
		{
			if (c < 0xE0) code = ((c & 0x1F) << 6) | (*utf8++ & 0x3F);
			else if ((utf8[1] & 0xC0) == 0x80)
			{
				code = ((c & 0x0F) << 12) | ((utf8[0] & 0x3F) << 6) | (utf8[1] & 0x3F);
				utf8 += 2;
			}
		}
		out[count++] = (unsigned short)code;
	}
	if (capacity > 0)
		out[count] = 0;
	return count;
}

int text_hires_covers(long font, unsigned long code) { return 0; }
long text_hires_font(char const *tag_name, float cap_height, short const *letter_widths, float oversample) { return -1; }
int text_hires_glyph(long font, unsigned long code, void *glyph) { return 0; }
void text_hires_register_atlas(const unsigned long *texture, unsigned long width, unsigned long height) { }
void text_hires_set_reset_hook(void (*hook)(void)) { }

long hud_hires_asset_count(void) { return 0; }
long hud_hires_asset_bitmap(long asset) { return -1; }
long hud_hires_asset_fits(long asset, long width, long height) { return 0; }
unsigned long hud_hires_asset_sprites(long asset) { return 0; }
char const *hud_hires_asset_tag(long asset) { return ""; }
void hud_hires_register_placeholder(long asset, unsigned long const *texture) { }
int hud_hires_sprites_drawable(long asset, unsigned long address, unsigned long level0_size) { return 0; }

/* ---------- Custom Edition maps: not on the Xbox (its maps only) */

void *halo_custom_edition_tag_cache(void) { return NULL; }
void halo_custom_edition_texels_channels(const void *texels, unsigned char channel_order) { }
void halo_custom_edition_texels_forget(void) { }
const char *platform_custom_edition_root(void) { return ""; }

/* ---------- the window, display and audio devices */

int platform_display_resolutions(long *widths, long *heights, int maximum)
{
	if (maximum < 1)
		return 0;
	widths[0] = 640;
	heights[0] = 480;
	return 1;
}

int platform_window_sizes(long *widths, long *heights, int maximum) { return platform_display_resolutions(widths, heights, maximum); }
void platform_display_apply(void) { }
int platform_audio_devices(int recording, void *names, int maximum) { return 0; }
void platform_request_quit(void) { platform_log("quit requested"); }
void platform_show_message(char const *title, char const *message) { platform_log("%s: %s", title, message); }

/* positioned stereo for streams: the Xbox's DirectSound positions them itself */
void dsound_sdl_stream_set_stereo_position(void *stream, BOOL positioned, float pan, float gain) { }

/* ---------- internet play (p2p.c) and voice chat (voice_audio.c): system link only, no voice (the online test
builds with OXBOX_NET_P2P have internet play: OpenCE's, port/oxbox/net) */

#if !defined(OXBOX_NET_P2P)
void p2p_blake2b_256(const void *data, unsigned long size, unsigned char *digest) { memset(digest, 0, 32); }
void p2p_discord_identity(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0) id[0] = 0;
	if (name_size > 0) name[0] = 0;
}
#endif
void p2p_discord_sanitize(char *destination, int size, const char *source, int name)
{
	if (size > 0) { strncpy(destination, source, size - 1); destination[size - 1] = 0; }
}
#if !defined(OXBOX_NET_P2P)
void p2p_hardware_id(char *hex, int size) { if (size > 0) hex[0] = 0; }
void p2p_hardware_id_sanitize(char *destination, int size, const char *source)
{
	if (size > 0) { strncpy(destination, source, size - 1); destination[size - 1] = 0; }
}
int p2p_invite_link(char *link, int size) { if (size > 0) link[0] = 0; return 0; }
int p2p_join_invite(const char *text) { return 0; }
int p2p_listing_unlock(void *listing, const char *password) { return 0; }
void p2p_lobby_browse(int on) { }
int p2p_lobby_games(void *games, int maximum_count) { return 0; }
void p2p_lobby_mark_failed(const unsigned char *identifier) { }
void p2p_lobby_refresh(void) { }
int p2p_peer_address(const unsigned char *identifier, unsigned long *address) { return 0; }
unsigned long p2p_peer_endpoint_address(unsigned long virtual_address) { return 0; }
int p2p_peer_key_hash(unsigned long virtual_address, unsigned char *identifier, unsigned char *key_hash) { return 0; }
int p2p_peer_reached(const unsigned char *identifier, unsigned long *address) { return 0; }
int p2p_reach_player(const unsigned char *key_hash) { return 0; }
void p2p_set_answering_players(int on) { }
void p2p_set_game_listing(const char *name, const char *map, const char *gametype, int engine_type, int kind,
	int open, int in_progress, int has_teams) { }
void p2p_set_game_player_counts(int count, int maximum) { }
void p2p_set_hosting_allowed(int allowed) { }
void p2p_set_hosting_password(const char *password) { }
void p2p_set_hosting_public(int is_public) { }
#endif

int voice_audio_encode(const float *frame, int bitrate, unsigned char *packet, int maximum) { return 0; }
void voice_audio_forget(int speaker) { }
void voice_audio_forget_all(void) { }
float voice_audio_level(const float *frame) { return 0.0f; }
int voice_audio_microphone(int open) { return 0; }
void voice_audio_play(int speaker, unsigned short sequence, const unsigned char *packet, int length, float gain,
	float pan) { }
int voice_audio_read_frame(float *frame) { return 0; }
void voice_audio_set_volume(float volume) { }

#if !defined(OXBOX_NET_P2P)
/* random bytes for voice keys (unused: no voice); XNet's generator (internet play's builds: their own,
port/oxbox/net/oxbox_p2p_posix.c) */
INT WINAPI XNetRandom(BYTE *buffer, UINT size);

void posix_random_bytes(void *buffer, unsigned long size)
{
	XNetRandom(buffer, size);
}
#endif
