/*
HMP_ONLINE.C

ONLINE PLAY (the online test builds with OXBOX_NET_P2P): a Multiplayer item under SYSTEM LINK PLAY that opens the same
screens, with ONLINE GAMES at the top of the games list, for internet play (port/linux/src/p2p*.c) in place of the LAN.

- The item: a copy of SYSTEM LINK PLAY's, made from the same definition by its index alone (without the datum's salt:
  tag lookups take either, and the copy has its own, as END GAME's item in hmp_game.c), linked after it, a row lower,
  with EDIT GAMETYPES a row lower too. Its text and its description (the list's extended description) are ours.
- A on either item sets the mode before the item's own handler (display_error_if_no_network_connection) opens the
  start-to-join screen: SYSTEM LINK PLAY is the LAN alone (no online games listed, a hosted game isn't listed
  online), ONLINE PLAY the internet alone (no LAN games; a hosted game is listed for other online Xboxes).
- The games list in ONLINE PLAY: the header is ONLINE GAMES (a picture made of ui.map's header letters,
  mine/mkheader.py, from the XBE's file: hmp_online_picture). Other Xboxes' games listed online (p2p_lobby.c, through
  OpenCE's brokers, on the Xboxes' own topics) are rows named "@ <name>". Choosing one reaches its host through
  internet play (p2p_join_invite: the invite its listing holds), and the row says "@ JOINING..."; once the tunnel
  reaches the host, its game answers the list's searches as a game on the LAN does, its own row takes the online
  one's place (selected), and A joins it as any system link game. A host not reached in 90 seconds is marked failed
  (its row comes back, last). Games of consoles reached through internet play show in ONLINE PLAY alone; the LAN's in
  SYSTEM LINK PLAY alone.

opence.ini online = 0 turns internet play off (port/oxbox/net/oxbox_lwip_ws.c): no ONLINE PLAY item then.
*/

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_NET_P2P)

#include "cseries.h"
#include "tag_files/tag_groups.h"
#include "interface/ui_widget.h"
#include "interface/event_manager.h"
#include "bitmaps/bitmap_group.h"
#include "hmp.h"
#include "../src/p2p.h"

#include <string.h>
#include <xtl.h>

void platform_log(char const *format, ...);

/* ---------- constants */

enum
{
	/* the list's rows (MAXIMUM_NETWORK_ADVERTISED_GAMES) */
	MAXIMUM_ROWS = 9,
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
#define GAMETYPES_ITEM "edit_gametypes"
#define FOUND_GAMES_HEADER "ui\\shell\\main_menu\\multiplayer_type_select\\connected\\server_list\\header_found_games"

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
	struct
	{
		wchar_t *text;
		short string_list_index;
	} text_box;
	byte list_parameters[0x50 - 0x44];
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

/* ---------- globals */

static struct online_row online_rows[MAXIMUM_ROWS];
static struct p2p_listing online_games[MAXIMUM_ROWS];

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
	item->text_box.string_list_index = _string_online_item;
	if (gametypes)
		gametypes->vertical_offset += step;
	online.item[local] = item->definition_tag_index;
	if (online.focus[local])
	{
		online.focus[local] = FALSE;
		list->focused_child = item;
	}
}

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

/* a local player's top menu is about to get an event (hmp_options_dispatch): A or Start on SYSTEM LINK PLAY or ONLINE
PLAY sets the mode; the item's own handler runs after (never taken here) */
void hmp_online_dispatch(struct widget_instance *root, struct event_record *event)
{
	struct widget_instance *list, *item;
	short local;

	if (!root || event->type != 3 || event->data.button.value != 1 ||
		(event->data.button.index != 0 && event->data.button.index != 12) || !name_is(root, MULTIPLAYER_SCREEN))
		return;
	for (local = 0; local < 4 && online.screen[local] != root; local++)
		;
	if (local > 3)
		return;
	list = root->focused_child;
	item = list ? list->focused_child : NULL;
	if (!item)
		return;
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
has the picture: mine/mkheader.py); NULL: the widget's own */
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

/* the list's rows for the mode (displayed: the advertised games the list shows, count of them, of maximum; available:
all of them, available_count): SYSTEM LINK PLAY keeps the LAN's games, ONLINE PLAY those of consoles reached through
internet play, then the games listed online. Returns the row count. select: set to the row of a game whose host was
just reached (its own row now), else left as it is */
int oxbox_online_add_rows(void **displayed, int count, int maximum, void const *available, int available_count,
	short *select)
{
	int game_count, index, kept, row_count = 0;

	(void)available;
	(void)available_count;

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
	if (!online.mode)
		return count;
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
				if (select)
					*select = (short)index;
				online.joining = FALSE;
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
	game_count = p2p_lobby_games(online_games, MAXIMUM_ROWS);
	for (index = 0; index < game_count && count < maximum; index++)
	{
		struct p2p_listing const *game = &online_games[index];
		struct online_row *row = &online_rows[row_count];
		int known;

		/* (not this console's own game, nor one with a password: an Xbox hosts none) */
		if (game->locked || !memcmp(game->identifier, p2p_identifier(), sizeof(game->identifier)))
			continue;
		/* (a host reached already: its own row is in the list. Only a row the list shows: a host on this LAN that
		is listed online too is the LAN's in SYSTEM LINK PLAY, and its online row stands in ONLINE PLAY) */
		for (known = 0; known < count; known++)
		{
			struct online_row const *shown = (struct online_row const *)displayed[known];

			if (shown->valid && !memcmp(shown->identifier, game->identifier, sizeof(game->identifier)))
				break;
		}
		if (known < count)
			continue;
		memset(row, 0, sizeof(*row));
		row->xnaddr_size = 12;
		memcpy(row->identifier, game->identifier, sizeof(row->identifier));
		row->update_time = system_milliseconds();
		if (online.joining && !memcmp(game->identifier, online.identifier, sizeof(online.identifier)))
			row_name(row->game_name, NUMBEROF(row->game_name), "@ JOINING...", NULL);
		else
			row_name(row->game_name, NUMBEROF(row->game_name), game->failed ? "@ (failed) " : "@ ", game->name);
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
		displayed[count++] = row;
		row_count++;
	}
	return count;
}

/* A on a row: an online game's host is reached (TRUE: the row was an online game's, handled here) */
boolean oxbox_online_row_join(void const *server)
{
	struct online_row const *row = (struct online_row const *)server;
	int index;

	if (row < online_rows || row >= online_rows + MAXIMUM_ROWS)
		return FALSE;
	for (index = 0; index < MAXIMUM_ROWS; index++)
	{
		struct p2p_listing const *game = &online_games[index];

		if (!memcmp(game->identifier, row->identifier, sizeof(row->identifier)) && game->invite[0])
		{
			if (p2p_join_invite(game->invite))
			{
				memcpy(online.identifier, row->identifier, sizeof(online.identifier));
				online.joining = TRUE;
				online.time = system_milliseconds();
				platform_log("online: reaching \"%s\" (%s, %d of %d players)", game->name, game->map,
					game->player_count, game->maximum_player_count);
			}
			return TRUE;
		}
	}
	return TRUE;
}

#endif
