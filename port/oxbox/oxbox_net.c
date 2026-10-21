/*
OXBOX_NET.C

The game's socket sends and receives, counted (and their errors), with a line in the log every ten seconds of
activity (ten seconds): what a system link game's two channels (the connection's stream, the datagrams) carry on the Xbox.
*/

#include "platform.h"

#include <string.h>

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);

int WINAPI retail_ws_send(SOCKET s, const char *buffer, int length, int flags);
int WINAPI retail_ws_recv(SOCKET s, char *buffer, int length, int flags);
int WINAPI retail_ws_sendto(SOCKET s, const char *buffer, int length, int flags, const struct sockaddr *to, int to_length);
int WINAPI retail_ws_recvfrom(SOCKET s, char *buffer, int length, int flags, struct sockaddr *from, int *from_length);

struct channel_counts
{
	unsigned long calls, bytes, would_block, errors, last_error;
};

static struct channel_counts counts[4];   /* send, recv, sendto, recvfrom */
static unsigned long last_report;

/* the same per socket, with the last address a datagram went to or came from */
int WINAPI halo_ws_getsockname(SOCKET s, struct sockaddr *name, int *length);
int WINAPI halo_ws_getpeername(SOCKET s, struct sockaddr *name, int *length);

struct socket_counts
{
	SOCKET s;
	struct channel_counts c[4];
	unsigned long last_to, last_from;
	unsigned short last_to_port, last_from_port;
};
static struct socket_counts sockets[8];

static struct socket_counts *socket_entry(SOCKET s)
{
	int i;

	for (i = 0; i < 8; i++)
		if (sockets[i].s == s)
			return &sockets[i];
	for (i = 0; i < 8; i++)
		if (!sockets[i].s)
		{
			sockets[i].s = s;
			return &sockets[i];
		}
	return &sockets[7];
}

static const char *address_text(unsigned long address, unsigned short port, char *text)
{
	const unsigned char *b = (const unsigned char *)&address;

	halo_linux_snprintf(text, 24, "%u.%u.%u.%u:%u", b[0], b[1], b[2], b[3], (port >> 8) | ((port & 0xFF) << 8));
	return text;
}

static void report_sockets(void)
{
	int i;

	for (i = 0; i < 8; i++)
	{
		struct socket_counts *k = &sockets[i];
		struct sockaddr_in local, peer;
		int length = sizeof(local), peer_length = sizeof(peer);
		char a[24], b[24], c[24], d[24];

		if (!k->s)
			continue;
		memset(&local, 0, sizeof(local));
		memset(&peer, 0, sizeof(peer));
		halo_ws_getsockname(k->s, (struct sockaddr *)&local, &length);
		if (halo_ws_getpeername(k->s, (struct sockaddr *)&peer, &peer_length))
			memset(&peer, 0, sizeof(peer));
		platform_log("  socket %lu local %s peer %s: send %lu/%lu recv %lu/%lu (wb %lu) sendto %lu/%lu (to %s) "
			"recvfrom %lu/%lu (from %s, wb %lu)", (unsigned long)k->s,
			address_text(local.sin_addr.s_addr, local.sin_port, a), address_text(peer.sin_addr.s_addr, peer.sin_port, b),
			k->c[0].calls, k->c[0].bytes, k->c[1].calls, k->c[1].bytes, k->c[1].would_block,
			k->c[2].calls, k->c[2].bytes, address_text(k->last_to, k->last_to_port, c),
			k->c[3].calls, k->c[3].bytes, address_text(k->last_from, k->last_from_port, d), k->c[3].would_block);
		memset(k->c, 0, sizeof(k->c));
	}
}

static void count_socket(SOCKET s, int which, int result)
{
	struct socket_counts *k = socket_entry(s);

	if (result >= 0)
	{
		k->c[which].calls++;
		k->c[which].bytes += result;
	}
	else
		k->c[which].would_block++;
}

/* all the bytes sent (send, sendto) and received (recv, recvfrom), for main.c's long frames (test build 24) */
unsigned long oxbox_net_bytes_sent, oxbox_net_bytes_received;

static void count(int which, int result)
{
	struct channel_counts *c = &counts[which];
	unsigned long now = GetTickCount();
	int i;

	if (result >= 0)
	{
		c->calls++;
		c->bytes += result;
		if (which == 0 || which == 2)
			oxbox_net_bytes_sent += result;
		else
			oxbox_net_bytes_received += result;
	}
	else
	{
		DWORD error = GetLastError();

		if (error == 10035)   /* WSAEWOULDBLOCK */
			c->would_block++;
		else
		{
			c->errors++;
			c->last_error = error;
		}
		SetLastError(error);
	}
	if (now - last_report >= 10000)
	{
		DWORD error = GetLastError();

		MEMORYSTATUS memory;

		GlobalMemoryStatus(&memory);
		last_report = now;
		platform_log("free %lu KB", memory.dwAvailPhys / 1024);
		platform_log("net %lus: send %lu/%luB wb%lu err%lu(%lu) | recv %lu/%luB wb%lu err%lu(%lu) | sendto %lu/%luB "
			"wb%lu err%lu(%lu) | recvfrom %lu/%luB wb%lu err%lu(%lu)", now / 1000,
			counts[0].calls, counts[0].bytes, counts[0].would_block, counts[0].errors, counts[0].last_error,
			counts[1].calls, counts[1].bytes, counts[1].would_block, counts[1].errors, counts[1].last_error,
			counts[2].calls, counts[2].bytes, counts[2].would_block, counts[2].errors, counts[2].last_error,
			counts[3].calls, counts[3].bytes, counts[3].would_block, counts[3].errors, counts[3].last_error);
		for (i = 0; i < 4; i++)
			counts[i].calls = counts[i].bytes = counts[i].would_block = counts[i].errors = 0;
		report_sockets();
		SetLastError(error);
	}
}

int WINAPI halo_ws_send(SOCKET s, const char *buffer, int length, int flags)
{
	int result = retail_ws_send(s, buffer, length, flags);

	count_socket(s, 0, result);
	count(0, result);
	return result;
}

static SOCKET udp_sockets[16];

static int is_udp(SOCKET s)
{
	int i;

	for (i = 0; i < 16; i++)
		if (udp_sockets[i] == s)
			return 1;
	return 0;
}

int WINAPI halo_ws_recv(SOCKET s, char *buffer, int length, int flags)
{
	int result = retail_ws_recv(s, buffer, length, flags);

	/* experiment: a connected datagram socket's recv found nothing; is a datagram from another address queued? */
	if (result < 0 && GetLastError() == 10035 && is_udp(s))
	{
		struct sockaddr_in from;
		int from_length = sizeof(from);
		int other = retail_ws_recvfrom(s, buffer, length, flags, (struct sockaddr *)&from, &from_length);
		static int logged;

		if (other >= 0)
		{
			if (logged++ < 10)
			{
				char a[24];

				platform_log("recv found nothing but recvfrom has %d bytes from %s on socket %lu", other,
					address_text(from.sin_addr.s_addr, from.sin_port, a), (unsigned long)s);
			}
			result = other;
		}
		else
			SetLastError(10035);
	}

	count_socket(s, 1, result);
	count(1, result);
	return result;
}

int WINAPI halo_ws_sendto(SOCKET s, const char *buffer, int length, int flags, const struct sockaddr *to, int to_length)
{
	int result = retail_ws_sendto(s, buffer, length, flags, to, to_length);
	struct socket_counts *k = socket_entry(s);

	if (to)
	{
		k->last_to = ((const struct sockaddr_in *)to)->sin_addr.s_addr;
		k->last_to_port = ((const struct sockaddr_in *)to)->sin_port;
	}
	count_socket(s, 2, result);
	count(2, result);
	return result;
}

int WINAPI halo_ws_recvfrom(SOCKET s, char *buffer, int length, int flags, struct sockaddr *from, int *from_length)
{
	int result = retail_ws_recvfrom(s, buffer, length, flags, from, from_length);
	struct socket_counts *k = socket_entry(s);

	if (result >= 0 && from)
	{
		k->last_from = ((struct sockaddr_in *)from)->sin_addr.s_addr;
		k->last_from_port = ((struct sockaddr_in *)from)->sin_port;
	}
	count_socket(s, 3, result);
	count(3, result);
	return result;
}

/* ---------- XNet's keys, logged: each registered key holds the security associations its consoles talk over */

INT WINAPI retail_XNetCreateKey(XNKID *key_id, XNKEY *key);
INT WINAPI retail_XNetRegisterKey(const XNKID *key_id, const XNKEY *key);
INT WINAPI retail_XNetUnregisterKey(const XNKID *key_id);
INT WINAPI retail_XNetXnAddrToInAddr(const XNADDR *xnaddr, const XNKID *key_id, IN_ADDR *in_addr);

static unsigned long key_text(const XNKID *key_id)
{
	return key_id ? *(const unsigned long *)key_id : 0;
}

INT WINAPI XNetCreateKey(XNKID *key_id, XNKEY *key)
{
	INT result = retail_XNetCreateKey(key_id, key);

	platform_log("XNetCreateKey: kid %08lx = %d", key_text(key_id), result);
	return result;
}

INT WINAPI XNetRegisterKey(const XNKID *key_id, const XNKEY *key)
{
	INT result = retail_XNetRegisterKey(key_id, key);

	platform_log("XNetRegisterKey(kid %08lx) = %d", key_text(key_id), result);
	return result;
}

INT WINAPI XNetUnregisterKey(const XNKID *key_id)
{
	INT result = retail_XNetUnregisterKey(key_id);

	platform_log("XNetUnregisterKey(kid %08lx) = %d", key_text(key_id), result);
	return result;
}

INT WINAPI XNetXnAddrToInAddr(const XNADDR *xnaddr, const XNKID *key_id, IN_ADDR *in_addr)
{
	INT result = retail_XNetXnAddrToInAddr(xnaddr, key_id, in_addr);
	const unsigned char *b = (const unsigned char *)in_addr;

	platform_log("XNetXnAddrToInAddr(kid %08lx) = %d: %u.%u.%u.%u", key_text(key_id), result, b[0], b[1], b[2], b[3]);
	return result;
}

/* ---------- sockets made, bound, connected and closed, logged */

SOCKET WINAPI retail_ws_socket(int family, int type, int protocol);
int WINAPI retail_ws_bind(SOCKET s, const struct sockaddr *name, int length);
int WINAPI retail_ws_connect(SOCKET s, const struct sockaddr *name, int length);
int WINAPI retail_ws_closesocket(SOCKET s);

SOCKET WINAPI halo_ws_socket(int family, int type, int protocol)
{
	SOCKET s = retail_ws_socket(family, type, protocol);

	platform_log("socket(%d, %s) = %lu", family, type == 2 ? "udp" : "tcp", (unsigned long)s);
	if (type == 2)
	{
		int i;

		for (i = 0; i < 16; i++)
			if (!udp_sockets[i] || udp_sockets[i] == s)
			{
				udp_sockets[i] = s;
				break;
			}
	}
	return s;
}

int WINAPI halo_ws_bind(SOCKET s, const struct sockaddr *name, int length)
{
	int result = retail_ws_bind(s, name, length);
	char a[24];

	platform_log("bind(%lu, %s) = %d (%lu)", (unsigned long)s, address_text(((const struct sockaddr_in *)name)->sin_addr.s_addr,
		((const struct sockaddr_in *)name)->sin_port, a), result, result ? GetLastError() : 0);
	return result;
}

int WINAPI halo_ws_connect(SOCKET s, const struct sockaddr *name, int length)
{
	int result = retail_ws_connect(s, name, length);
	char a[24];

	platform_log("connect(%lu, %s) = %d (%lu)", (unsigned long)s, address_text(((const struct sockaddr_in *)name)->sin_addr.s_addr,
		((const struct sockaddr_in *)name)->sin_port, a), result, result ? GetLastError() : 0);
	return result;
}

int WINAPI halo_ws_closesocket(SOCKET s)
{
	int i;

	for (i = 0; i < 8; i++)
		if (sockets[i].s == s)
			memset(&sockets[i], 0, sizeof(sockets[i]));
	for (i = 0; i < 16; i++)
		if (udp_sockets[i] == s)
			udp_sockets[i] = 0;
	platform_log("closesocket(%lu)", (unsigned long)s);
	return retail_ws_closesocket(s);
}
