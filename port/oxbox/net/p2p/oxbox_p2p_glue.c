/*
OXBOX_P2P_GLUE.C

The rest of what internet play (port/linux/src/p2p*.c) calls on the PC that the Xbox answers here (OXBOX_NET_P2P
builds only):
- WebRTC (p2p_webrtc.c on the PC: browsers joining over DTLS) and Discord (p2p_discord.c): none. A console says it has
  no WebRTC, and its tunnel is plain UDP, as two PC builds' is.
- The brokers' list (network.brokers_file, beside config.toml on the PC): opence.ini's online_brokers if set, else
  brokers.txt beside the XBE if there is one, else OpenCE's list (port/assets/network/brokers.txt as our fork has it).
- atexit: the Xbox's game never exits to a desktop (the console reboots), so what a PC does then never runs here.
*/

#include "platform.h"
#include "port_config.h"
#include "p2p_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- WebRTC: none */

int p2p_webrtc_ready(void)
{
	return 1;
}

int p2p_webrtc_describe(const unsigned char *identifier, int proven, struct p2p_webrtc *local,
	struct p2p_candidate *candidates, int maximum_count)
{
	(void)identifier;
	(void)proven;
	memset(local, 0, sizeof(*local));
	local->kind = _p2p_webrtc_none;
	return p2p_local_candidates(candidates, maximum_count);
}

void p2p_webrtc_new_request(void)
{
}

int p2p_webrtc_offered(int connection, int peer, const unsigned char *secret, const struct p2p_webrtc *remote,
	const struct p2p_candidate *candidates, int count, int is_host)
{
	(void)connection;
	(void)peer;
	(void)secret;
	(void)remote;
	(void)candidates;
	(void)count;
	(void)is_host;
	/* (a browser's tunnel would need DTLS: the console reaches native builds' plain UDP tunnels only) */
	return -1;
}

void p2p_webrtc_close(int connection)
{
	(void)connection;
}

void p2p_webrtc_send(int connection, const void *packet, int size)
{
	(void)connection;
	(void)packet;
	(void)size;
}

int p2p_webrtc_received(const unsigned char *packet, int size, unsigned long address, unsigned short port)
{
	(void)packet;
	(void)size;
	(void)address;
	(void)port;
	return 0;
}

int p2p_webrtc_update(void)
{
	return 0;
}

/* ---------- Discord: none */

void p2p_discord_update(void)
{
}

void p2p_discord_user(char *id, int id_size, char *name, int name_size)
{
	if (id_size > 0)
		id[0] = 0;
	if (name_size > 0)
		name[0] = 0;
}

void p2p_discord_set_hosting(const char *secret, int player_count, int maximum_player_count)
{
	(void)secret;
	(void)player_count;
	(void)maximum_player_count;
}

/* ---------- files: the brokers' list */

/* OpenCE's brokers (port/assets/network/brokers.txt in our fork) */
static const char default_brokers[] =
	"opence.milenko.org:1883\n"
	"broker.emqx.io:1883\n"
	"broker.hivemq.com:1883\n"
	"test.mosquitto.org:1883\n";

/* opence.ini's online_brokers (oxbox_entry.c): host:port, host:port ... */
char oxbox_online_brokers[256];

void config_folder(char *path, size_t size)
{
	snprintf(path, size, "D:\\");
}

/* a file read whole (with an end), freed by the caller; NULL if it can't be read */
char *config_file_read(const char *path, size_t *size)
{
	FILE *file;
	char *text = NULL;
	long length;
	size_t name_length = strlen(path);
	int brokers = name_length >= 11 && !strcmp(path + name_length - 11, "brokers.txt");

	file = brokers && oxbox_online_brokers[0] ? NULL : fopen(path, "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		length = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (length >= 0 && length < 64 * 1024 && (text = malloc((size_t)length + 1)) != NULL)
		{
			if (fread(text, 1, (size_t)length, file) != (size_t)length)
			{
				free(text);
				text = NULL;
			}
			else
			{
				text[length] = 0;
				if (size)
					*size = (size_t)length;
			}
		}
		fclose(file);
		if (text)
			return text;
	}
	/* opence.ini's brokers, else (no brokers.txt beside the XBE) OpenCE's */
	if (brokers)
	{
		const char *list = oxbox_online_brokers[0] ? oxbox_online_brokers : default_brokers;

		length = (long)strlen(list);
		text = malloc((size_t)length + 1);
		if (text)
		{
			memcpy(text, list, (size_t)length + 1);
			if (size)
				*size = (size_t)length;
		}
	}
	return text;
}

/* ---------- atexit: never */

int atexit(void (*function)(void))
{
	(void)function;
	return 0;
}

/* ---------- bcmp: what the compiler makes of memcmp(...) == 0 in internet play's code (the C library here has none) */

int bcmp(const void *first, const void *second, size_t size)
{
	return memcmp(first, second, size);
}

/* ---------- the online test's line (OXBOX_NET_TEST: port/oxbox/net/oxbox_lwip.c's test thread, each second)

What internet play is doing, on the main menu: a broker reached, the Xbox games listed online, and (opence.ini
net_test_join = 1) the first one joined, as the System Link list's online rows will: its host reached through the
tunnel, after which its game is in the System Link list as if on this LAN. */

#if defined(OXBOX_NET_TEST)

int p2p_signal_connected(void);

void oxbox_p2p_test_status(char *line, int size, int join)
{
	static int browsing, joined;
	static unsigned char joined_identifier[6];
	static char joined_name[P2P_LISTING_NAME_SIZE + 1];
	struct p2p_listing games[4];
	unsigned long address = 0;
	int count;

	if (!browsing)
	{
		p2p_lobby_browse(1);
		browsing = 1;
	}
	count = p2p_lobby_games(games, 4);
	if (join && !joined && count > 0 && p2p_join_invite(games[0].invite))
	{
		joined = 1;
		memcpy(joined_identifier, games[0].identifier, sizeof(joined_identifier));
		memcpy(joined_name, games[0].name, sizeof(joined_name));
		platform_log("p2p test: joining \"%s\" on %s (%s, %d of %d players)", games[0].name, games[0].map,
			games[0].gametype, games[0].player_count, games[0].maximum_player_count);
	}
	if (joined)
	{
		int reached = p2p_peer_reached(joined_identifier, &address);

		snprintf(line, (size_t)size, "ONLINE: %s %s", joined_name, reached ? "reached" : "reaching...");
	}
	else
	{
		snprintf(line, (size_t)size, "ONLINE: broker %s, %d Xbox game%s listed", p2p_signal_connected() ? "ok" : "not yet",
			count, count == 1 ? "" : "s");
	}
}

#endif
