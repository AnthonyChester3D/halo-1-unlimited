/*
HMP_ONLINE.C

Internet play's games in the System Link list (the online test builds with OXBOX_NET_P2P): other Xboxes' system link
games listed online (port/linux/src/p2p_lobby.c, through OpenCE's brokers, on the Xboxes' own topics) are shown after
the games on this LAN, their names after "@ ". Choosing one reaches its host through internet play (p2p_join_invite:
the invite its listing holds), and the row says "@ JOINING..."; once the tunnel reaches the host, its game answers the
list's searches as a game on this LAN does, its own row takes the online one's place (selected), and A joins it as any
system link game. A host not reached in 90 seconds is marked failed (its row comes back, last).

Every system link game an online console hosts is listed (port/oxbox/net/oxbox_lwip_ws.c: opence.ini online = 0
turns internet play off).
*/

#if defined(HALO_XBOX_TARGET) && defined(OXBOX_NET_P2P)

#include "cseries.h"
#include "../src/p2p.h"

#include <string.h>

void platform_log(char const *format, ...);

/* ---------- constants */

enum
{
	/* the list's rows (MAXIMUM_NETWORK_ADVERTISED_GAMES) */
	MAXIMUM_ROWS = 9,
	/* milliseconds: how long a host is reached for (p2p.c's JOIN_TIMEOUT) */
	JOIN_TIMEOUT = 90000,
};

/* ---------- structures */

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

/* ---------- globals */

static struct online_row online_rows[MAXIMUM_ROWS];
static struct p2p_listing online_games[MAXIMUM_ROWS];

static struct
{
	boolean browsing;
	/* a host being reached (its game's row: "JOINING..."), since when */
	boolean joining;
	unsigned char identifier[6];
	unsigned long time;
} online;

/* ---------- private code */

static void row_name(wchar_t *name, int size, char const *prefix, char const *text)
{
	int length = 0;

	while (*prefix && length < size - 1)
		name[length++] = (wchar_t)(unsigned char)*prefix++;
	while (text && *text && length < size - 1)
		name[length++] = (wchar_t)(unsigned char)*text++;
	name[length] = 0;
}

/* ---------- public code */

/* the online games after the list's own (displayed: the list's rows, count of them so far, of maximum; available:
the advertised games, available_count of them): the list's row count now. select: set to the row of a game whose host
was just reached (its own row now), else left as it is */
int oxbox_online_add_rows(void **displayed, int count, int maximum, void const *available, int available_count,
	short *select)
{
	struct online_row const *advertised = (struct online_row const *)available;
	int game_count, index, row_count = 0;

	if (!online.browsing)
	{
		/* (the listings are gathered from now on: the list's first showing) */
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
		/* (a host reached already: its own row is in the list) */
		for (known = 0; known < available_count; known++)
		{
			if (advertised[known].valid && !memcmp(advertised[known].identifier, game->identifier,
				sizeof(game->identifier)))
				break;
		}
		if (known < available_count)
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
