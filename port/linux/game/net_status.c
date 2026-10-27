/*
NET_STATUS.C (oxbox)

Telling the player when the Xbox isn't on a network, in the builds with OXBOX_OFFLINE_SYSTEM_LINK. Those let a console
host a system link game, or look for games, without a network (network_client_manager.c), so Halo's "Network
connection lost" box no longer comes up to say why no games show up. This says it instead, where the player looks:

- The games list (SYSTEM LINK PLAY's and ONLINE PLAY's), while it lists no game: Halo's own "Looking for games..." and
  "Press A or Y to start your own game", then, in the same text box (its font, colour and fades), why no game can show
  up (server_list_menu_update picks the message, NET_STATUS_STRING_INDEX):
  - no network connection: the network cable is out, or what it's plugged into is off (the card has no link);
  - connecting to the network: the cable is in, but the Xbox has no address yet (DHCP; link-local after about 20
    seconds without an answer, which is enough for system link);
  - ONLINE PLAY only: no router (a link-local address: nothing on the network gave the Xbox an address, so no
    internet); connecting to the online servers (internet play's brokers, p2p_signal.c), for the list's first ten
    seconds or the network's; then that it can't reach them.
- The lobby, hosting a system link game: "No network connection" in the countdown bar, left of the bots label (not
  while the countdown runs: it's shown there).
- A system link game it hosts: the network going (the other consoles drop out; the game goes on here) and coming back,
  as a message on each local player's HUD.
- The network back (the cable in, or an address after none or a link-local one): internet play asks its public address
  again and reconnects to the brokers now (p2p_network_changed), not when their retry time comes round, which is two
  minutes once they've failed a few times.

A console that joined another's game is told by Halo itself (network_game_client_network_lost: its "Network connection
lost" box). The network is looked at twice a second: the cable (transport_network_available, the card's link) and the
address (lwIP's, in the builds with OXBOX_NET_LWIP). Changes are logged (oxbox_log.txt, "net status:").
*/

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_OFFLINE_SYSTEM_LINK)

#include "cseries.h"
#include "cseries/cseries_windows.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "interface/ui_widget.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "rasterizer/rasterizer.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/text_group.h"
#include "hmp.h"
#include "oxbox_net_status.h"

void platform_log(char const *format, ...);
boolean transport_network_available(void);   /* (the card's link: transport_endpoint_set_winsock.c) */
#if defined(OXBOX_NET_LWIP)
int oxbox_lwip_address(unsigned long *address, int *dhcp);   /* (oxbox_lwip.c) */
#endif
#if defined(OXBOX_NET_P2P)
int p2p_signal_connected(void);      /* (p2p_signal.c: a broker is connected) */
void p2p_network_changed(void);      /* (p2p.c) */
#endif
#if defined(OXBOX_SHOWCASE)
boolean showcase_active(void);       /* (showcase.c: its game is the console's alone) */
#endif

typedef char net_status_string_index_check[NET_STATUS_STRING_INDEX == HMP_NET_STATUS_STRING ? 1 : -1];

/* ---------- constants */

enum
{
	NET_STATUS_POLL_MILLISECONDS = 500,
	NET_STATUS_CONNECTING_MILLISECONDS = 10000,   /* the online servers: "Connecting" that long, then "Can't reach" */
	NET_STATUS_PREGAME_STATE = 2,                 /* (network_client_manager.c's _network_game_client_state_pregame) */
	NET_STATUS_MESSAGE_LENGTH = 512,
};

enum net_status
{
	_net_status_connected,
	_net_status_no_connection,   /* no link: the cable out, or what it's plugged into off */
	_net_status_no_address,      /* a link, no address yet */
	_net_status_link_local,      /* a link-local address: no DHCP server answered (no router) */
	NUMBER_OF_NET_STATUSES
};

enum net_status_message
{
	_message_none,
	_message_no_connection,
	_message_online_no_connection,
	_message_no_address,
	_message_no_router,
	_message_connecting_online,
	_message_online_unreachable,
	NUMBER_OF_NET_STATUS_MESSAGES
};

static char const *const net_status_names[NUMBER_OF_NET_STATUSES] =
{
	"connected",
	"no network connection (no link: the cable)",
	"connected, no address yet",
	"connected, a link-local address (no DHCP server: no router)",
};

/* each line short enough for the games list's text box (22 characters: its widget ends 227 pixels after the text's
start), and its name for the log */
static char const *const net_status_messages[NUMBER_OF_NET_STATUS_MESSAGES] =
{
	"",
	"No network connection.\r\nOther Xboxes' games\r\ncan't show up: check\r\nthe network cable.",
	"No network connection.\r\nOnline games can't\r\nshow up: check the\r\nnetwork cable.",
	"Connecting to the\r\nnetwork...",
	"No router answered, so\r\nthis Xbox can't reach\r\nthe internet.",
	"Connecting to the\r\nonline servers...",
	"Can't reach the online\r\nservers right now:\r\ncheck the internet\r\nconnection.",
};
static char const *const net_status_message_names[NUMBER_OF_NET_STATUS_MESSAGES] =
{
	"Halo's own",
	"no network connection",
	"no network connection (ONLINE PLAY)",
	"connecting to the network",
	"no router (ONLINE PLAY)",
	"connecting to the online servers",
	"can't reach the online servers",
};

/* ---------- globals */

static struct
{
	boolean known;
	short status;
	/* when the network was last looked at, and when it was last usable for internet play (an address, not link-local)
	after not being */
	unsigned long checked;
	unsigned long usable_time;
	/* the games list's message, as last picked */
	short message;
	wchar_t text[NET_STATUS_MESSAGE_LENGTH];
} net_status;

/* ---------- private code */

static boolean usable(short status)
{
	return status == _net_status_connected;
}

/* a system link game this console hosts for others, under way: its players are told on the HUD */
static boolean hosting_system_link_game(void)
{
	if (!game_engine || !global_network_game_server_get() || network_game_is_splitscreen_local())
		return FALSE;
#if defined(OXBOX_SHOWCASE)
	if (showcase_active())
		return FALSE;
#endif
	return TRUE;
}

static void hud_message(wchar_t const *text)
{
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		if (local_player_get_player_index(local_player_index) != NONE)
			hud_print_message(local_player_index, text);
	}
}

static void net_status_changed(short was, short status)
{
	boolean linked = status != _net_status_no_connection;
	boolean was_linked = was != _net_status_no_connection;

	platform_log("net status: %s (was %s)", net_status_names[status], net_status_names[was]);
	if (linked != was_linked && hosting_system_link_game())
	{
		hud_message(linked ? L"Network connected" : L"Network connection lost: other Xboxes can't reach this game");
	}
	if (usable(status) && !usable(was))
	{
		net_status.usable_time = system_milliseconds();
#if defined(OXBOX_NET_P2P)
		/* (internet play: its public address and brokers anew, now) */
		p2p_network_changed();
#endif
	}
}

static void net_status_poll(void)
{
	unsigned long now = system_milliseconds();
	short status = _net_status_connected;

	if (net_status.known && now - net_status.checked < NET_STATUS_POLL_MILLISECONDS)
		return;
	net_status.checked = now;
	if (!transport_network_available())
	{
		status = _net_status_no_connection;
	}
#if defined(OXBOX_NET_LWIP)
	else
	{
		unsigned long address = 0;
		int dhcp = 0;

		if (!oxbox_lwip_address(&address, &dhcp))
			status = _net_status_no_address;
		/* (169.254.x.y, in network order) */
		else if (!dhcp && (address & 0xFFFF) == 0xFEA9)
			status = _net_status_link_local;
	}
#endif
	if (!net_status.known)
	{
		platform_log("net status: %s", net_status_names[status]);
		net_status.usable_time = now;
	}
	else if (status != net_status.status)
	{
		net_status_changed(net_status.status, status);
	}
	net_status.status = status;
	net_status.known = TRUE;
}

/* ---------- public code */

void net_status_update(void)
{
	net_status_poll();
}

boolean net_status_server_list(unsigned long milliseconds_shown)
{
	boolean online = FALSE;
	short message = _message_none;

	net_status_poll();
#if defined(OXBOX_NET_P2P)
	online = hmp_online_chosen();
#endif
	switch (net_status.status)
	{
	case _net_status_no_connection:
		message = online ? _message_online_no_connection : _message_no_connection;
		break;
	case _net_status_no_address:
		message = _message_no_address;
		break;
	case _net_status_link_local:
		/* (enough for system link: the consoles on the cable reach each other so) */
		message = online ? _message_no_router : _message_none;
		break;
	default:
#if defined(OXBOX_NET_P2P)
		if (online && !p2p_signal_connected())
		{
			unsigned long since_usable = system_milliseconds() - net_status.usable_time;

			message = milliseconds_shown < NET_STATUS_CONNECTING_MILLISECONDS ||
				since_usable < NET_STATUS_CONNECTING_MILLISECONDS ? _message_connecting_online :
				_message_online_unreachable;
		}
#endif
		break;
	}
	if (message != net_status.message)
	{
		platform_log("net status: the games list: %s", net_status_message_names[message]);
		net_status.message = message;
	}
	return message != _message_none;
}

wchar_t *net_status_server_list_message(long string_list_tag_index)
{
	wchar_t const *own = string_list_tag_index != NONE ? unicode_string_list_get_string(string_list_tag_index, 1) : L"";
	char const *why = net_status_messages[net_status.message];
	long length = 0;

	/* Halo's "Looking for games...", "Press A or Y to start your own game", then a line's space and why */
	while (own && *own && length < NET_STATUS_MESSAGE_LENGTH - 5)
		net_status.text[length++] = *own++;
	if (*why)
	{
		net_status.text[length++] = L'\r';
		net_status.text[length++] = L'\n';
		net_status.text[length++] = L'\r';
		net_status.text[length++] = L'\n';
	}
	while (*why && length < NET_STATUS_MESSAGE_LENGTH - 1)
		net_status.text[length++] = (wchar_t)(unsigned char)*why++;
	net_status.text[length] = 0;
	return net_status.text;
}

void net_status_render(void)
{
	struct network_game_client *client = global_network_game_client_get();
	real_argb_color color = { 1.0f, 1.0f, 0.65f, 0.2f };
	static wchar_t const text[] = L"No network connection";
	rectangle2d bounds;
	long font;

	if (!client || game_engine || !global_network_game_server_get() || network_game_is_splitscreen_local() ||
		network_game_client_get_state(client, NULL) != NET_STATUS_PREGAME_STATE || !ui_widget_lobby_showing())
	{
		return;
	}
#if defined(OXBOX_SHOWCASE)
	if (showcase_active())
		return;
#endif
	if (!net_status.known || net_status.status != _net_status_no_connection ||
		network_game_client_get_seconds_to_game_start(client) >= 0 ||
		(font = tag_loaded('font', "ui\\large_ui")) == NONE)
	{
		return;
	}
	/* (in the countdown bar, left; the bots label is right-aligned at its other end: bots.c) */
	draw_string_set_draw_mode(font, NONE, 0, 0, &color);
	draw_string_set_tab_stops(NULL, 0);
	bounds.y0 = 82;
	bounds.x0 = 80;
	bounds.y1 = 106;
	bounds.x1 = 330;
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}

#endif
