/* oxbox_net_status.h: telling the player the Xbox isn't on a network (oxbox, the builds with
OXBOX_OFFLINE_SYSTEM_LINK: port/linux/game/net_status.c).

Those builds let a console host a system link game, or look for games, without a network (network_client_manager.c),
so Halo's "Network connection lost" box doesn't come up any more to say why no games show up. net_status.c says it
instead: in the games list while it lists none, in the lobby while hosting, and on the HUD of a game it hosts.

The hook sites, each inside #if defined(OXBOX_OFFLINE_SYSTEM_LINK):
- main.c main_loop, once a frame (net_status_update): the network looked at, the HUD's messages, internet play told
  when the network is back.
- ui_widget_game_data_input_functions.c server_list_menu_update: the games list's message (Halo's "Looking for
  games..." text box) is NET_STATUS_STRING_INDEX while net_status_server_list says why no games can show up; the
  text box asks hmp_string (hmp_maps.c) for it, which asks net_status_server_list_message.
- interface.c interface_draw_fullscreen_overlays (net_status_render): the lobby's line, beside the bots label.
- hmp_online.c: hmp_online_chosen, whether the list is ONLINE PLAY's (internet games) or SYSTEM LINK PLAY's.
- port/linux/src/p2p.c: p2p_network_changed, internet play reconnecting at once when the network is back. */

#ifndef OXBOX_NET_STATUS_H
#define OXBOX_NET_STATUS_H

/* the games list's message with why no games show up (hmp.h's HMP_NET_STATUS_STRING) */
#define NET_STATUS_STRING_INDEX 0x70A0

/* once a frame (main.c) */
void net_status_update(void);
/* the games list lists no game, its message shown for that long: TRUE when the message should say why
(NET_STATUS_STRING_INDEX), FALSE for Halo's own */
boolean net_status_server_list(unsigned long milliseconds_shown);
/* the message: Halo's own "Looking for games..." from its string list, then why */
wchar_t *net_status_server_list_message(long string_list_tag_index);
/* the lobby's line (interface.c, after the bots label) */
void net_status_render(void);

#if defined(OXBOX_NET_P2P)
/* hmp_online.c: ONLINE PLAY chosen (the games list is the internet's), else SYSTEM LINK PLAY */
boolean hmp_online_chosen(void);
#endif

#endif
