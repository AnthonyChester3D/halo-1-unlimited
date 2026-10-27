/*
HMP_ONLINE.C

ONLINE PLAY (the online test builds with OXBOX_NET_P2P): a Multiplayer item under SYSTEM LINK PLAY that opens the same
screens, with ONLINE GAMES at the top of the games list, for internet play (port/linux/src/p2p*.c) in place of the LAN.

- The item: a copy of SYSTEM LINK PLAY's, made from the same definition by its index alone (without the datum's salt:
  tag lookups take either, and the copy has its own, as END GAME's item in hmp_game.c), linked after it, a row lower,
  with EDIT GAMETYPES and the line above it a row lower too. Its text and its description (the list's extended
  description) are ours.
- A on either item sets the mode before the item's own handler (display_error_if_no_network_connection) opens the
  start-to-join screen: SYSTEM LINK PLAY is the LAN alone (no online games listed, a hosted game isn't listed
  online), ONLINE PLAY the internet alone (no LAN games; a hosted game is listed for other online Xboxes).
- The games list in ONLINE PLAY: the header is ONLINE GAMES (a picture made of ui.map's header letters,
  pictures.c, from the XBE's file: hmp_online_picture). Other Xboxes' games listed online (p2p_lobby.c, through
  OpenCE's brokers, on the Xboxes' own topics; with crossplay, OXBOX_CROSSPLAY, the PC builds' games too) are rows
  of their own. Choosing one reaches its host through internet play (p2p_join_invite: the invite its listing holds),
  and the row says "JOINING..."; once the tunnel reaches the host, its game answers the list's searches as a game on
  the LAN does, its own row takes the online one's place (selected), and A joins it as any system link game. A host
  not reached in 90 seconds is marked failed ("(failed) <name>"). Games of consoles reached through internet play
  show in ONLINE PLAY alone; the LAN's in SYSTEM LINK PLAY alone.
- Either list shows only games this Xbox can join (network_client_manager.c's advertisements, p2p_lobby.c's
  listings: another network version, a password, or a PC's game on a map the Xbox doesn't have, for more than 16
  players or playing an OpenCE gametype isn't kept: oxbox_online_pc_game_listed). Xboxes' games come first, then PCs'.
  Each row's name is a few spaces in; an Xbox host's row has an Xbox there (the system link lobby's connected console,
  smaller: hmp_online_text_drawn). The list has Halo's nine rows; with more games, up and down past its first or last
  row scroll it (games_list_scroll), and the games listed online are kept while the menus are up
  (hmp_online_release).

opence.ini online = 0 turns internet play off (port/oxbox/net/oxbox_lwip_ws.c): no ONLINE PLAY item then.
*/

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_NET_P2P)

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "interface/ui_widget.h"
#include "interface/event_manager.h"
#include "bitmaps/bitmap_group.h"
#include "text/draw_string.h"
#include "hmp.h"
#include "../src/p2p.h"

#include <string.h>
#include <xtl.h>

void platform_log(char const *format, ...);

/* ---------- constants */

enum
{
	/* the list's rows (MAXIMUM_NETWORK_ADVERTISED_GAMES); the games listed online it scrolls through (p2p_lobby.c's
	MAXIMUM_GAMES); all its rows */
	VISIBLE_ROWS = 9,
	MAXIMUM_LISTED = 32,
	MAXIMUM_ROWS = VISIBLE_ROWS + MAXIMUM_LISTED,
	/* a row's name is this many spaces in: an Xbox host's has an Xbox there */
	ROW_INDENT = 7,
	/* the Xbox: the system link lobby's connected console (XBOX_PICTURE's second picture, its 110 x 58 at the top
	left), drawn smaller beside a row's name */
	XBOX_PICTURE_INDEX = 1,
	XBOX_PICTURE_WIDTH = 110,
	XBOX_PICTURE_HEIGHT = 58,
	XBOX_ICON_WIDTH = 38,
	XBOX_ICON_HEIGHT = 20,
	/* milliseconds: rows scrolled while up or down is held, one each */
	SCROLL_REPEAT = 180,
	/* (milliseconds) a reached host's row shown before it is joined (oxbox_online_join_reached) */
	JOIN_SETTLE = 600,
	/* the menus' button events (input.h's d-pad, then the left stick's directions) */
	BUTTON_DPAD_UP = 8,
	BUTTON_DPAD_DOWN = 9,
	BUTTON_STICK_UP = 16,
	BUTTON_STICK_DOWN = 17,
	/* milliseconds: how long a host is reached for (p2p.c's JOIN_TIMEOUT) */
	JOIN_TIMEOUT = 90000,
	/* our texts (hmp.h's HMP_ONLINE_STRING_BASE on) */
	_string_online_item = HMP_ONLINE_STRING_BASE,
	_string_online_description,
	NUMBER_OF_ONLINE_STRINGS = 2,
	/* the Multiplayer list's items, as its extended description counts them, and their pictures (mp_options) */
	MULTIPLAYER_SYSTEM_LINK_INDEX = 2,
	MULTIPLAYER_ONLINE_INDEX = 3,
	/* the ONLINE GAMES header: 512 x 64 DXT3, as the SYSTEM LINK GAMES one; a fake bitmap's number (hmp_maps.c) */
	HEADER_WIDTH = 512,
	HEADER_HEIGHT = 64,
	HEADER_SIZE = HEADER_WIDTH * HEADER_HEIGHT,
	HEADER_FAKE_INDEX = 0xfe00,
};

#define MULTIPLAYER_SCREEN "multiplayer_type_select_screen"
#define SYSTEM_LINK_ITEM "multiplayer_type_conn_item"
/* (a widget's name is its definition's own: EDIT GAMETYPES' and the line's, not the names the screen gives them,
"edit_gametypes" and "blue line") */
#define GAMETYPES_ITEM "multiplayer_type_gametypes_item"
#define DIVIDER "blueline"
#define FOUND_GAMES_HEADER "ui\\shell\\main_menu\\multiplayer_type_select\\connected\\server_list\\header_found_games"
#define SERVER_LIST_SCREEN "server_list_screen"
#define XBOX_PICTURE "ui\\shell\\bitmaps\\console_grafix"

static char const *const online_strings[NUMBER_OF_ONLINE_STRINGS] =
{
	"ONLINE PLAY",
	"Play over the internet with \r\nother Xboxes running Halo 1 \r\nUnlimited: up to 16 players.",
};

/* ---------- structures */

/* (ui_widget.c's, as hmp_game.c has it) */
struct widget_animation_data
{
	short current_frame_index;
	short first_frame_index;
	short last_frame_index;
	short number_of_sprite_frames;
};
struct widget_instance
{
	long definition_tag_index;
	char const *name;
	short local_player_index;
	short horizontal_offset;
	short vertical_offset;
	short type;
	boolean visible;
	boolean render_regardless_of_controller_index;
	boolean disabled;
	boolean pause_game_time;
	boolean delete_recursion_lock;
	boolean widget_is_error_dialog;
	boolean close_if_local_player_controller_present;
	byte pad17;
	long creation_time;
	unsigned long milliseconds_to_auto_close;
	unsigned long auto_close_fade_time;
	real alpha_modifier;
	struct widget_instance *previous;
	struct widget_instance *next;
	struct widget_instance *parent;
	struct widget_instance *child;
	struct widget_instance *focused_child;
	union
	{
		struct
		{
			wchar_t *text;
			short string_list_index;
		} text_box;
		struct
		{
			short selected_index;
			short last_list_tab_direction;
			void *list_items;
			word number_of_items;
			struct widget_instance *extended_description;
			wchar_t *item_text;
		} list;
	} parameters;
	struct widget_animation_data animation;
};
typedef char verify_hmp_online_widget_instance_size[sizeof(struct widget_instance) == 0x58 ? 1 : -1];

/* an advertised game's record, as the list shows it (network_client_manager.c's struct network_advertised_game) */
struct online_row
{
	unsigned char key_id[8];
	unsigned char key[16];
	/* (its XNADDR: a host's abEnet is its internet play identifier, port/oxbox/net/oxbox_lwip_ws.c) */
	unsigned char xnaddr_size;
	unsigned char xnaddr_flags;
	unsigned char identifier[6];
	unsigned long ina;
	unsigned char nonce[8];
	unsigned long update_time;
	wchar_t game_name[16];
	long padding50;
	char map_name[0x80];
	short engine_type;
	short machine_count;
	unsigned short player_count;
	short maximum_player_count;
	short score_limit;
	short platform;
	unsigned char open;
	unsigned char valid;
	unsigned char has_teams;
	unsigned char oddball_variant;
};

typedef char online_row_size_assert[sizeof(struct online_row) == 0xE4 ? 1 : -1];
typedef char online_row_identifier_assert[offsetof(struct online_row, identifier) == 0x1A ? 1 : -1];
typedef char online_row_name_assert[offsetof(struct online_row, game_name) == 0x30 ? 1 : -1];
typedef char online_row_engine_assert[offsetof(struct online_row, engine_type) == 0xD4 ? 1 : -1];
typedef char online_row_platform_assert[offsetof(struct online_row, platform) == 0xDE ? 1 : -1];

/* ---------- prototypes */

struct widget_instance *ui_widget_load_by_name_or_tag(char const *name, long tag_index, struct widget_instance *parent,
	short local_player_index, long topmost_tag, long parent_tag, short child_index);
struct widget_instance *ui_widget_get_active_widget(short local_player_index);
int oxbox_xbe_online_header_read(void *buffer, unsigned long size);   /* (oxbox_entry.c: the XBE's ONLINE GAMES) */
void p2p_set_hosting_allowed(int allowed);

extern int oxbox_online;                                               /* (opence.ini online, oxbox_lwip_ws.c) */
boolean network_game_client_advertised_index_on_xbox(long game_index);   /* (network_client_manager.c) */
struct bitmap_data *bitmap_group_try_and_get_bitmap(long bitmap_group_index, short bitmap_index);
unsigned long system_milliseconds(void);
char const *tag_get_name(long tag_index);
void ui_play_audio_feedback_sound(short audio_feedback);
void *__stdcall LocalAlloc(unsigned int flags, unsigned int bytes);
void *__stdcall LocalFree(void *memory);

/* ---------- globals */

/* the games listed online as the list has them, while the menus are up (hmp_online_release frees them for a game) */
struct online_listed
{
	struct online_row rows[MAXIMUM_LISTED];
	struct p2p_listing games[MAXIMUM_LISTED];
};
static struct online_listed *online_listed;

static struct
{
	/* ONLINE PLAY chosen (else SYSTEM LINK PLAY: the LAN) */
	boolean mode;
	boolean browsing;
	/* a host being reached (its game's row: "JOINING..."), since when */
	boolean joining;
	unsigned char identifier[6];
	unsigned long time;
	/* each local player's Multiplayer screen, patched, and its ONLINE PLAY item's datum */
	struct widget_instance *screen[4];
	long screen_time[4];
	long item[4];
	/* the Multiplayer screen comes back (B from the games list): ONLINE PLAY gets the focus */
	boolean focus[4];
	wchar_t strings[NUMBER_OF_ONLINE_STRINGS][128];
	struct bitmap_data header;
	/* the games list: all its rows (the LAN's or the reached hosts', then those listed online; Xboxes' first), which
	an Xbox hosts, the first shown, and the list's own rows (Halo's displayed servers) and how many are shown */
	void *rows[MAXIMUM_ROWS];
	boolean on_xbox[MAXIMUM_ROWS];
	int row_count;
	int top;
	void **shown;
	int shown_count;
	/* each controller's last row scrolled (up or down held) */
	unsigned long scrolled[4];
	/* the Xbox's picture: its tag, as found for the list made then */
	long picture_tag;
	unsigned long picture_time;
	/* the controller whose A started the reaching (the games list's last A), and whether its host's own row, once
	reached, is joined at once (oxbox_online_join_reached): one A joins */
	short join_controller;
	boolean join_reached;
	/* ... its row (as shown) and when it was reached */
	short join_row;
	unsigned long join_time;
	/* the A is due: the games list's next empty event is made it (hmp_online_event) */
	boolean join_due;
} online;

/* ---------- private code */

static boolean name_is(struct widget_instance *w, char const *n) { return w->name && !strcmp(w->name, n); }

static void row_name(wchar_t *name, int size, char const *prefix, char const *text)
{
	int length = 0;

	while (*prefix && length < size - 1)
		name[length++] = (wchar_t)(unsigned char)*prefix++;
	while (text && *text && length < size - 1)
		name[length++] = (wchar_t)(unsigned char)*text++;
	name[length] = 0;
}

static void set_mode(boolean on)
{
	if (online.mode != on)
		platform_log("online: %s", on ? "ONLINE PLAY (internet games; a hosted game is listed online)" :
			"SYSTEM LINK PLAY (the LAN alone)");
	online.mode = on;
	/* (a game hosted now: joinable and listed from the internet in ONLINE PLAY alone) */
	p2p_set_hosting_allowed(on);
	p2p_set_hosting_public(on);
}

/* the Multiplayer screen (each local player's): ONLINE PLAY after SYSTEM LINK PLAY */
static void multiplayer_patch(short local, struct widget_instance *root)
{
	struct widget_instance *list = NULL, *c, *link = NULL, *gametypes = NULL, *item;
	short step;

	online.screen[local] = root;
	online.screen_time[local] = root->creation_time;
	online.item[local] = 0;
	/* (the games list opened from here starts at its top) */
	online.top = 0;
	if (!oxbox_online)
		return;
	for (c = root->child; c && !list; c = c->next)
		if (c->type == 3)
			list = c;
	for (c = list ? list->child : NULL; c; c = c->next)
	{
		if (name_is(c, SYSTEM_LINK_ITEM))
			link = c;
		else if (name_is(c, GAMETYPES_ITEM))
			gametypes = c;
	}
	if (!link || !link->previous)
	{
		platform_log("online: the Multiplayer screen has no SYSTEM LINK PLAY to put ONLINE PLAY under");
		return;
	}
	item = ui_widget_load_by_name_or_tag(NULL, link->definition_tag_index & 0xffff, list, local, NONE, NONE, NONE);
	if (!item)
		return;
	/* (made as the list's child; moved to just after SYSTEM LINK PLAY) */
	if (item->previous)
		item->previous->next = item->next;
	else if (list->child == item)
		list->child = item->next;
	if (item->next)
		item->next->previous = item->previous;
	item->previous = link;
	item->next = link->next;
	if (link->next)
		link->next->previous = item;
	link->next = item;
	step = (short)(link->vertical_offset - link->previous->vertical_offset);
	if (step <= 0 || step > 80)
		step = 33;
	item->horizontal_offset = link->horizontal_offset;
	item->vertical_offset = (short)(link->vertical_offset + step);
	item->parameters.text_box.string_list_index = _string_online_item;
	if (gametypes)
		gametypes->vertical_offset += step;
	/* (and the line between SYSTEM LINK PLAY and EDIT GAMETYPES, the screen's own child: under ONLINE PLAY now) */
	for (c = root->child; c; c = c->next)
		if (name_is(c, DIVIDER))
			c->vertical_offset += step;
	online.item[local] = item->definition_tag_index;
	if (online.focus[local])
	{
		online.focus[local] = FALSE;
		list->focused_child = item;
	}
}

#if defined(OXBOX_OFFLINE_SYSTEM_LINK)
/* ONLINE PLAY chosen: the games list is the internet's (net_status.c says why none show up) */
boolean hmp_online_chosen(void)
{
	return online.mode;
}
#endif

/* ---------- public code: the Multiplayer screen */

/* each tick, each local player's (hmp_game_tick) */
void hmp_online_tick(short local)
{
	struct widget_instance *root = ui_widget_get_active_widget(local);

	if (local < 0 || local > 3 || !root || !name_is(root, MULTIPLAYER_SCREEN))
		return;
	if (root == online.screen[local] && root->creation_time == online.screen_time[local])
		return;
	multiplayer_patch(local, root);
}

/* the games list's screen gets up or down: past its first or last row the list scrolls (TRUE: the event is taken),
and from the very last (or first) row it goes round to the first (last) as Halo's lists do (Halo's own handler then
moves the focus) */
static boolean games_list_scroll(struct widget_instance *root, struct event_record *event)
{
	short button = event->data.button.index;
	boolean up = button == BUTTON_DPAD_UP || button == BUTTON_STICK_UP;
	boolean down = button == BUTTON_DPAD_DOWN || button == BUTTON_STICK_DOWN;
	short controller = event->controller_index >= 0 && event->controller_index < 4 ? event->controller_index : 0;
	unsigned long now = system_milliseconds();
	struct widget_instance *list;
	short selected;
	int step;

	if ((!up && !down) || online.row_count <= VISIBLE_ROWS || event->data.button.value < 1)
		return FALSE;
	for (list = root->child; list && list->type != 3; list = list->next)
		;
	if (!list || !online.shown || list->parameters.list.list_items != (void *)online.shown)
		return FALSE;
	selected = list->parameters.list.selected_index;
	if (down && selected == VISIBLE_ROWS - 1)
	{
		if (online.top + VISIBLE_ROWS >= online.row_count)
		{
			/* (round to the first: on a press, not while held) */
			if (event->data.button.value != 1)
				return TRUE;
			online.top = 0;
			return FALSE;
		}
		step = 1;
	}
	else if (up && selected == 0)
	{
		if (online.top <= 0)
		{
			if (event->data.button.value != 1)
				return TRUE;
			online.top = online.row_count - VISIBLE_ROWS;
			return FALSE;
		}
		step = -1;
	}
	else
		return FALSE;
	if (event->data.button.value == 1 || now - online.scrolled[controller] >= SCROLL_REPEAT)
	{
		online.top += step;
		online.scrolled[controller] = now;
		ui_play_audio_feedback_sound(1);
	}
	return TRUE;
}

/* a local player's top menu is about to get an event (hmp_options_dispatch): A or Start on SYSTEM LINK PLAY or ONLINE
PLAY sets the mode (the item's own handler runs after); up and down in the games list scroll it. TRUE: taken */
boolean hmp_online_dispatch(struct widget_instance *root, struct event_record *event)
{
	struct widget_instance *list, *item;
	short local;

	if (!root || event->type != 3)
		return FALSE;
	if (name_is(root, SERVER_LIST_SCREEN))
	{
		/* (the controller of an A on a row: the one that joins once its host is reached) */
		if (event->data.button.index == 0 && event->data.button.value == 1)
			online.join_controller = event->controller_index >= 0 && event->controller_index < 4 ?
				event->controller_index : 0;
		return games_list_scroll(root, event);
	}
	if (event->data.button.value != 1 || (event->data.button.index != 0 && event->data.button.index != 12) ||
		!name_is(root, MULTIPLAYER_SCREEN))
	{
		return FALSE;
	}
	for (local = 0; local < 4 && online.screen[local] != root; local++)
		;
	if (local > 3)
		return FALSE;
	list = root->focused_child;
	item = list ? list->focused_child : NULL;
	if (!item)
		return FALSE;
	if (online.item[local] && item->definition_tag_index == online.item[local])
	{
		set_mode(TRUE);
		online.focus[local] = TRUE;
	}
	else if (name_is(item, SYSTEM_LINK_ITEM))
	{
		set_mode(FALSE);
		online.focus[local] = FALSE;
	}
	else
		online.focus[local] = FALSE;
	return FALSE;
}

/* the Multiplayer list's extended description (multiplayer_type_menu_update_extended_description, after it set the
picture and text to the focused item's count, index): ONLINE PLAY's, and the items' after it as without it */
void hmp_online_multiplayer_description(struct widget_instance *list, short index, short *picture, short *text)
{
	struct widget_instance *c;
	boolean ours = FALSE;
	short local;

	/* (the screen's local player: the menus' widgets have none of their own) */
	for (local = 0; local < 4 && (!list || online.screen[local] != list->parent); local++)
		;
	if (local > 3 || !online.item[local] || index < MULTIPLAYER_ONLINE_INDEX)
		return;
	for (c = list->child; c && !ours; c = c->next)
		ours = c->definition_tag_index == online.item[local];
	if (!ours)
		return;
	if (index == MULTIPLAYER_ONLINE_INDEX)
	{
		/* (SYSTEM LINK PLAY's picture, our text) */
		*picture = MULTIPLAYER_SYSTEM_LINK_INDEX;
		*text = _string_online_description;
	}
	else
		*picture = *text = (short)(index - 1);
}

wchar_t *hmp_online_string(short index)
{
	wchar_t *d;
	char const *a;

	if (index < HMP_ONLINE_STRING_BASE || index >= HMP_ONLINE_STRING_BASE + NUMBER_OF_ONLINE_STRINGS)
		return NULL;
	d = online.strings[index - HMP_ONLINE_STRING_BASE];
	if (!d[0])
		for (a = online_strings[index - HMP_ONLINE_STRING_BASE]; (*d++ = (unsigned char)*a++) != 0; )
			;
	return online.strings[index - HMP_ONLINE_STRING_BASE];
}

/* ---------- public code: the games list's header */

/* a widget's picture (custom_edition_maps_picture): ONLINE GAMES in place of SYSTEM LINK GAMES in ONLINE PLAY (the XBE
has the picture: installer/pictures.c); NULL: the widget's own */
struct bitmap_data *hmp_online_picture(long bitmap_tag_index, short *frame_index)
{
	if (!online.mode || bitmap_tag_index == NONE || *frame_index != 0 ||
		csstrcasecmp(tag_get_name(bitmap_tag_index), FOUND_GAMES_HEADER))
		return NULL;
	if (!online.header.signature)
	{
		/* (as Halo's: DXT3, its registration point at the top left) */
		hmp_bitmap_setup(&online.header, HEADER_FAKE_INDEX, HEADER_WIDTH, HEADER_HEIGHT, 15, 0x83, HEADER_SIZE);
		online.header.registration_point.x = 0;
		online.header.registration_point.y = 0;
	}
	return &online.header;
}

/* its texels (hmp_texture_read, for its fake bitmap): from the XBE's file, straight into the texture cache; FALSE
for another fake bitmap */
boolean hmp_online_picture_read(long fake_index, void *buffer, long size)
{
	if (fake_index != HEADER_FAKE_INDEX)
		return FALSE;
	if (size != HEADER_SIZE || !oxbox_xbe_online_header_read(buffer, (unsigned long)size))
		memset(buffer, 0, size);
	return TRUE;
}

/* ---------- public code: the games list's rows */

/* the Xbox's stock multiplayer levels (hmp_maps.c's), by name: the maps of PC games it can play */
static char const *const stock_maps[] =
{
	"beavercreek", "sidewinder", "damnation", "ratrace", "prisoner", "hangemhigh", "chillout", "carousel",
	"boardingaction", "bloodgulch", "wizard", "putput", "longest",
};

/* whether a PC build's game (version 38 or 39) is one this Xbox can join, so the games list shows it (a listing's or
an advertisement's): on one of the Xbox's stock levels (by its name, or its path's last part: not a Custom Edition map,
nor one of the PC's own levels), for at most 16 players, playing no OpenCE gametype (the game's kind: its mode and
Hardcore, 0 for none) */
int oxbox_online_stock_map(char const *map);

int oxbox_online_pc_game_listed(char const *map, long maximum_players, long kind)
{
	return map && kind == 0 && maximum_players <= 16 && oxbox_online_stock_map(map);
}

/* whether a map is one of the Xbox's 13 multiplayer levels (by its name, or its path's last part), and no folder map
("+folder", "+pack/folder": hmp_maps.c), whatever its name: one a PC has too. The lobby kinds
(network_server_manager.c's oxbox_crossplay_hosting) and the games list (oxbox_online_pc_game_listed) */
int oxbox_online_stock_map(char const *map)
{
	char const *name = map, *c;
	int index;

	if (!map || map[0] == '+')
		return FALSE;
	for (c = map; *c; c++)
		if (*c == '\\' || *c == '/')
			name = c + 1;
	for (index = 0; index < (int)NUMBEROF(stock_maps); index++)
		if (!csstrcasecmp(name, stock_maps[index]))
			return TRUE;
	return FALSE;
}

/* the games listed online, freed with the menus' memory (hmp_menu_memory_release: a game map loaded) */
void hmp_online_release(void)
{
	if (online_listed)
		LocalFree(online_listed);
	online_listed = NULL;
	online.row_count = 0;
	online.shown = NULL;
	online.shown_count = 0;
}

/* a row of the games listed online (the list's rows, as an advertised game's record) */
static int listed_rows(int known_count, void **known)
{
	int game_count, index, row_count = 0;

	if (!online_listed)
	{
		online_listed = (struct online_listed *)LocalAlloc(0x40 /* LMEM_ZEROINIT */, sizeof(*online_listed));
		if (!online_listed)
			return 0;
	}
	game_count = p2p_lobby_games(online_listed->games, MAXIMUM_LISTED);
	for (index = 0; index < game_count; index++)
	{
		struct p2p_listing *game = &online_listed->games[index];
		struct online_row *row = &online_listed->rows[row_count];
		int known_index;

		/* (not this console's own game, nor one with a password: an Xbox has no way to give one) */
		if (game->locked || !memcmp(game->identifier, p2p_identifier(), sizeof(game->identifier)))
			continue;
		/* (a host reached already: its own row is in the list. Only a row the list shows: a host on this LAN that
		is listed online too is the LAN's in SYSTEM LINK PLAY, and its online row stands in ONLINE PLAY) */
		for (known_index = 0; known_index < known_count; known_index++)
		{
			struct online_row const *shown = (struct online_row const *)known[known_index];

			if (shown->valid && !memcmp(shown->identifier, game->identifier, sizeof(game->identifier)))
				break;
		}
		if (known_index < known_count)
			continue;
		/* (kept where its row is: game i's row is row i) */
		if (row_count != index)
			online_listed->games[row_count] = *game;
		game = &online_listed->games[row_count];
		memset(row, 0, sizeof(*row));
		row->xnaddr_size = 12;
		memcpy(row->identifier, game->identifier, sizeof(row->identifier));
		row->update_time = system_milliseconds();
		if (online.joining && !memcmp(game->identifier, online.identifier, sizeof(online.identifier)))
			row_name(row->game_name, NUMBEROF(row->game_name), "JOINING...", NULL);
		else
			row_name(row->game_name, NUMBEROF(row->game_name), game->failed ? "(failed) " : "", game->name);
		strncpy(row->map_name, game->map, sizeof(row->map_name) - 1);
		row->engine_type = game->engine_type;
		row->machine_count = game->player_count;
		row->player_count = game->player_count;
		row->maximum_player_count = game->maximum_player_count;
		/* (the Xbox's platform; "open": joinable now) */
		row->platform = 0;
		row->open = game->open ? TRUE : FALSE;
		row->valid = TRUE;
		row->has_teams = game->has_teams ? TRUE : FALSE;
		row_count++;
	}
	return row_count;
}

/* whether an Xbox hosts the game of one of the list's rows: an advertised game's (by its place among available, the
client's), or one listed online */
static boolean row_on_xbox(void const *row, void const *available, int available_count)
{
	struct online_row const *advertised = (struct online_row const *)available;
	struct online_row const *as_row = (struct online_row const *)row;

	if (as_row >= advertised && as_row < advertised + available_count)
		return network_game_client_advertised_index_on_xbox((long)(as_row - advertised));
#if defined(OXBOX_CROSSPLAY)
	if (online_listed && as_row >= online_listed->rows && as_row < online_listed->rows + MAXIMUM_LISTED)
		return online_listed->games[as_row - online_listed->rows].on_xbox != 0;
#endif
	/* (the Xboxes' own games alone are listed without crossplay) */
	return TRUE;
}

/* the list's rows for the mode (displayed: the advertised games the list shows, count of them, of maximum; available:
all of them, available_count): SYSTEM LINK PLAY keeps the LAN's games, ONLINE PLAY those of consoles reached through
internet play, then the games listed online; Xboxes' games first, then PCs' (crossplay). The list shows maximum of them
from online.top (games_list_scroll), into displayed. Returns the count shown. select: set to the row (as shown) of a
game whose host was just reached (its own row now), else left as it is */
int oxbox_online_add_rows(void **displayed, int count, int maximum, void const *available, int available_count,
	short *select)
{
	void *all[MAXIMUM_ROWS];
	int index, kept, all_count, reached = NONE;

	/* (the list's own rows: the LAN's, or the internet's) */
	for (index = 0, kept = 0; index < count; index++)
	{
		struct online_row const *row = (struct online_row const *)displayed[index];
		unsigned long address;
		boolean peer = p2p_peer_address(row->identifier, &address) != 0;

		if (peer == online.mode)
			displayed[kept++] = displayed[index];
	}
	count = kept;
	for (index = 0; index < count; index++)
		all[index] = displayed[index];
	all_count = count;
	if (online.mode)
	{
		if (!online.browsing)
		{
			/* (the listings are gathered from now on: ONLINE PLAY's first list) */
			p2p_lobby_browse(TRUE);
			online.browsing = TRUE;
		}
		if (online.joining)
		{
			unsigned long address;

			/* the host reached: its own row, selected */
			for (index = 0; index < count; index++)
			{
				struct online_row const *row = (struct online_row const *)displayed[index];

				if (row->valid && !memcmp(row->identifier, online.identifier, sizeof(online.identifier)))
				{
					reached = index;
					online.joining = FALSE;
					/* (an open game: joined at once, as if A were pressed on it) */
					online.join_reached = row->open ? TRUE : FALSE;
					break;
				}
			}
			if (online.joining && !p2p_peer_reached(online.identifier, &address) &&
				system_milliseconds() - online.time > JOIN_TIMEOUT)
			{
				p2p_lobby_mark_failed(online.identifier);
				online.joining = FALSE;
			}
		}
		kept = listed_rows(count, displayed);
		for (index = 0; index < kept && all_count < MAXIMUM_ROWS; index++)
			all[all_count++] = &online_listed->rows[index];
	}
	/* Xboxes' games first, each in its place (the reached host's row followed) */
	online.row_count = 0;
	for (index = 0; index < all_count; index++)
	{
		if (row_on_xbox(all[index], available, available_count))
		{
			if (index == reached)
				reached = online.row_count + MAXIMUM_ROWS;
			online.on_xbox[online.row_count] = TRUE;
			online.rows[online.row_count++] = all[index];
		}
	}
	for (index = 0; index < all_count; index++)
	{
		if (!row_on_xbox(all[index], available, available_count))
		{
			if (index == reached)
				reached = online.row_count + MAXIMUM_ROWS;
			online.on_xbox[online.row_count] = FALSE;
			online.rows[online.row_count++] = all[index];
		}
	}
	/* (the first shown: the reached host's row in sight; never past the last screenful) */
	if (reached != NONE)
	{
		reached -= MAXIMUM_ROWS;
		if (reached < online.top)
			online.top = reached;
		else if (reached >= online.top + maximum)
			online.top = reached - maximum + 1;
		if (select)
			*select = (short)(reached - online.top);
		online.join_row = (short)(reached - online.top);
		online.join_time = system_milliseconds();
	}
	if (online.top > online.row_count - maximum)
		online.top = online.row_count - maximum;
	if (online.top < 0)
		online.top = 0;
	online.shown = displayed;
	online.shown_count = MIN(maximum, online.row_count - online.top);
	for (index = 0; index < online.shown_count; index++)
		displayed[index] = online.rows[online.top + index];
	return online.shown_count;
}

/* the games list's data input, each update (after oxbox_online_add_rows selected a reached host's row): once the
list has had JOIN_SETTLE to show that row, highlighted, and the player hasn't moved off it, an A is posted for the
controller that pressed A on its listing, which joins the game as a press on the row would (an open game). One A
joins a game listed online: it took two, one to reach the host, one on its own row once reached */
void oxbox_online_join_reached(struct widget_instance *list)
{
	if (!online.join_reached || system_milliseconds() - online.join_time < JOIN_SETTLE)
		return;
	online.join_reached = FALSE;
	if (!list || !list->focused_child || list->parameters.list.selected_index != online.join_row)
	{
		platform_log("online: the host is reached (its row not selected: not joined at once)");
		return;
	}
	platform_log("online: the host is reached: joining its game");
	online.join_due = TRUE;
}

/* a local player's top menu is about to get an event (hmp_options_dispatch, first): the games list's empty event
(each update's, for its animations) is made the A that joins a reached host's game (oxbox_online_join_reached). An
event posted from the list's data input would be lost: the menus flush the events once they have taken theirs */
void hmp_online_event(struct widget_instance *root, struct event_record *event)
{
	if (!online.join_due || !root || event->type != 0 || !name_is(root, SERVER_LIST_SCREEN))
		return;
	online.join_due = FALSE;
	event->type = 3;
	event->controller_index = online.join_controller;
	event->data.button.index = 0;
	event->data.button.value = 1;
}

/* a row's text as the list writes it (text: up to length characters): its name ROW_INDENT spaces in, where an Xbox
host's has an Xbox (hmp_online_text_drawn) */
void oxbox_online_row_text(wchar_t *text, int length)
{
	int size, index;

	if (!text)
		return;
	for (size = 0; text[size] && size < length; size++)
		;
	if (size + ROW_INDENT > length)
		size = length - ROW_INDENT;
	for (index = size - 1; index >= 0; index--)
		text[index + ROW_INDENT] = text[index];
	for (index = 0; index < ROW_INDENT; index++)
		text[index] = L' ';
	text[size + ROW_INDENT] = 0;
}

/* a text box was drawn (ui_widget.c, in bounds, its font and colour still set): a games list row's Xbox, if an Xbox
hosts its game, in its indent, in the text's alpha */
void hmp_online_text_drawn(struct widget_instance *widget, rectangle2d const *bounds)
{
	struct widget_instance *list = widget ? widget->parent : NULL, *c;
	struct bitmap_data *bitmap;
	rectangle2d text, cursor, icon, picture;
	real_argb_color color;
	unsigned long a8;
	wchar_t const *string;
	short middle;
	int index = 0;

	if (!list || !online.shown || list->type != 3 || list->parameters.list.list_items != (void *)online.shown)
		return;
	for (c = list->child; c && c != widget; c = c->next)
		index++;
	if (!c || index >= online.shown_count || online.top + index >= online.row_count || !online.on_xbox[online.top + index])
		return;
	string = widget->parameters.text_box.text;
	if (!string || !string[0])
		return;
	if (list->creation_time != (long)online.picture_time || !online.picture_tag)
	{
		online.picture_tag = tag_loaded('bitm', XBOX_PICTURE);
		online.picture_time = (unsigned long)list->creation_time;
	}
	if (online.picture_tag == NONE ||
		!(bitmap = bitmap_group_try_and_get_bitmap(online.picture_tag, XBOX_PICTURE_INDEX)))
	{
		return;
	}
	draw_unicode_string_compute_bounds(bounds, string, &text, &cursor);
	middle = draw_unicode_string_capital_middle(bounds, string);
	/* (the text's bounds start at its first letter's ink, past the spaces) */
	icon.x1 = (short)(text.x0 - 4);
	icon.x0 = (short)(icon.x1 - XBOX_ICON_WIDTH);
	icon.y0 = (short)(middle - XBOX_ICON_HEIGHT / 2);
	icon.y1 = (short)(icon.y0 + XBOX_ICON_HEIGHT);
	picture.x0 = 0;
	picture.y0 = 0;
	picture.x1 = XBOX_PICTURE_WIDTH;
	picture.y1 = XBOX_PICTURE_HEIGHT;
	draw_string_get_color(&color);
	a8 = (unsigned long)(long)(color.alpha * 255.0f + 0.5f);
	draw_bitmap_in_rect(bitmap, &icon, &picture, NULL, (a8 > 255 ? 255 : a8) << 24 | 0xffffff, NULL, TRUE);
}

/* A on a row: an online game's host is reached (TRUE: the row was an online game's, handled here) */
boolean oxbox_online_row_join(void const *server)
{
	struct online_row const *row = (struct online_row const *)server;
	struct p2p_listing const *game;

	if (!online_listed || row < online_listed->rows || row >= online_listed->rows + MAXIMUM_LISTED)
	{
		platform_log("online: A on a reached host's row (%s)", row && row->valid ? "valid" : "not valid");
		return FALSE;
	}
	game = &online_listed->games[row - online_listed->rows];
	if (!memcmp(game->identifier, row->identifier, sizeof(row->identifier)) && game->invite[0] &&
		p2p_join_invite(game->invite))
	{
		memcpy(online.identifier, row->identifier, sizeof(online.identifier));
		online.joining = TRUE;
		online.time = system_milliseconds();
		platform_log("online: reaching \"%s\" (%s, %d of %d players)%s", game->name, game->map,
			game->player_count, game->maximum_player_count,
#if defined(OXBOX_CROSSPLAY)
			!game->on_xbox ? " (a PC game: crossplay experiment)" :
#endif
			"");
	}
	return TRUE;
}

#endif
