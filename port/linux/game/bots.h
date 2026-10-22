/*
BOTS.H (oxbox)

The host's bots (bots.c), and the host's side of them in
network_server_manager.c.
*/

#ifndef __BOTS_H
#define __BOTS_H
#pragma once

struct network_player;
struct player_action;

/* bots.c */
void bots_update(void);
long bots_pick_leaver(struct network_player *players, long count);
boolean bots_player_action(long absolute_player_index, struct player_action *action);
boolean bots_lobby_button(short button);
void bots_render(void);

/* network_server_manager.c */
short network_game_server_bot_count(short *people);
boolean network_game_server_bots_held(void);
boolean network_game_server_add_bot(void);
boolean network_game_server_remove_bot(long slot);
void network_game_server_set_bot_mode(short value);
boolean network_game_server_bots_in_lobby(void);

#endif
