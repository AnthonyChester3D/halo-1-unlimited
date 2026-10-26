/*
OXBOX_LWIP_WS.C

The online builds' XNet (OXBOX_NET_LWIP only): the XNet and Winsock calls Halo's game code and our map sharing make
(transport_endpoint_*_winsock.c, network_*.c, hmp_share.c), answered by lwIP (oxbox_lwip.c) in place of the retail
XNet inside Halo's XBE. retail.ld PROVIDEs the retail addresses of these names; defining them here takes their place.

What changes for the game:
- Addresses are plain IP. A console's XNADDR carries its lwIP address (ina) and its card's MAC (internet play's
  builds: its internet play identifier, which peers' virtual addresses map from); XNetXnAddrToInAddr gives back the
  ina, and XNet's keys (one a game, to encrypt it) are accepted and do nothing. These consoles talk to each other
  only, never to a console running an XNet build.
- Internet play (OXBOX_NET_P2P): consoles reached over the internet are on this LAN for the game (below).
- SOCKET values are lwIP's descriptors plus one (Halo's code knows only INVALID_SOCKET as "no socket").
- Winsock's structures (16-bit sin_family, fd_set's list of sockets) are converted to and from lwIP's.
- WSAGetLastError is the thread's last error (GetLastError, as XNet's is: oxbox_net.c saves and restores it around its
  log), from lwIP's errno, which is per thread too (lwipopts.h).
*/

#include "lwip/opt.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

#include "oxbox_kernel.h"

#if defined(OXBOX_NET_LWIP)

#define WINAPI __attribute__((stdcall))

typedef unsigned int WS_SOCKET;
#define WS_INVALID_SOCKET ((WS_SOCKET)~0)
#define WS_SOCKET_ERROR (-1)

/* Winsock's (the XDK's) layouts */
struct ws_sockaddr_in
{
	short sin_family;
	unsigned short sin_port;
	unsigned long sin_addr;
	char sin_zero[8];
};

#define WS_FD_SETSIZE 64
struct ws_fd_set
{
	unsigned int fd_count;
	WS_SOCKET fd_array[WS_FD_SETSIZE];
};

struct ws_timeval
{
	long tv_sec, tv_usec;
};

typedef struct
{
	unsigned char bSizeOfStruct, bFlags, abEnet[6];
	unsigned long ina;
} ws_xnaddr;

/* Winsock's option and error numbers */
#define WS_SOL_SOCKET 0xFFFF
#define WS_SO_REUSEADDR 0x0004
#define WS_SO_KEEPALIVE 0x0008
#define WS_SO_BROADCAST 0x0020
#define WS_SO_LINGER 0x0080
#define WS_SO_SNDBUF 0x1001
#define WS_SO_RCVBUF 0x1002
#define WS_SO_ERROR 0x1007
#define WS_SO_TYPE 0x1008
#define WS_IPPROTO_TCP 6
#define WS_TCP_NODELAY 0x0001
#define WS_FIONBIO 0x8004667EUL
#define WS_FIONREAD 0x4004667FUL

#define WSABASEERR 10000
#define WSAEINTR (WSABASEERR + 4)
#define WSAEBADF (WSABASEERR + 9)
#define WSAEACCES (WSABASEERR + 13)
#define WSAEFAULT (WSABASEERR + 14)
#define WSAEINVAL (WSABASEERR + 22)
#define WSAEMFILE (WSABASEERR + 24)
#define WSAEWOULDBLOCK (WSABASEERR + 35)
#define WSAEINPROGRESS (WSABASEERR + 36)
#define WSAEALREADY (WSABASEERR + 37)
#define WSAENOTSOCK (WSABASEERR + 38)
#define WSAEDESTADDRREQ (WSABASEERR + 39)
#define WSAEMSGSIZE (WSABASEERR + 40)
#define WSAENOPROTOOPT (WSABASEERR + 42)
#define WSAEADDRINUSE (WSABASEERR + 48)
#define WSAEADDRNOTAVAIL (WSABASEERR + 49)
#define WSAENETDOWN (WSABASEERR + 50)
#define WSAENETUNREACH (WSABASEERR + 51)
#define WSAECONNABORTED (WSABASEERR + 53)
#define WSAECONNRESET (WSABASEERR + 54)
#define WSAENOBUFS (WSABASEERR + 55)
#define WSAEISCONN (WSABASEERR + 56)
#define WSAENOTCONN (WSABASEERR + 57)
#define WSAESHUTDOWN (WSABASEERR + 58)
#define WSAETIMEDOUT (WSABASEERR + 60)
#define WSAECONNREFUSED (WSABASEERR + 61)
#define WSAEHOSTUNREACH (WSABASEERR + 65)
#define WSANOTINITIALISED (WSABASEERR + 93)

void oxbox_lwip_start(void);
int oxbox_lwip_address(unsigned long *address, int *dhcp);
const unsigned char *oxbox_lwip_mac(void);
int oxbox_lwip_card_up(void);

/* ---------- per-thread errors (lwIP's errno, and WSAGetLastError) */

#define THREAD_SLOTS 16

static struct { void *thread; int lwip_errno; } thread_errors[THREAD_SLOTS];

static void *current_thread(void)
{
	void *thread;

	/* (the KPCR's PrcbData.CurrentThread) */
	__asm__ __volatile__("movl %%fs:0x28, %0" : "=r"(thread));
	return thread;
}

static int thread_slot(void)
{
	void *thread = current_thread();
	int index, free_index = -1;
	KIRQL irql;

	for (index = 0; index < THREAD_SLOTS; index++)
	{
		if (thread_errors[index].thread == thread)
			return index;
	}
	irql = KeRaiseIrqlToDpcLevel();
	for (index = 0; index < THREAD_SLOTS; index++)
	{
		if (thread_errors[index].thread == thread)
			break;
		if (!thread_errors[index].thread && free_index < 0)
			free_index = index;
	}
	if (index == THREAD_SLOTS)
	{
		/* (a thread more than the slots hold: the last slot, shared) */
		index = free_index >= 0 ? free_index : THREAD_SLOTS - 1;
		thread_errors[index].thread = thread;
	}
	KfLowerIrql(irql);
	return index;
}

int *oxbox_lwip_errno(void)
{
	return &thread_errors[thread_slot()].lwip_errno;
}

static int wsa_error_from_errno(int code);

/* (internet play's own sockets, which answer with lwIP's errors as Winsock's: oxbox_p2p_posix.c) */
int oxbox_lwip_wsa_error(void)
{
	return wsa_error_from_errno(thread_errors[thread_slot()].lwip_errno);
}

void oxbox_lwip_set_errno(int code)
{
	thread_errors[thread_slot()].lwip_errno = code;
}

static int wsa_error_from_errno(int code)
{
	switch (code)
	{
	case 0: return 0;
	case EINTR: return WSAEINTR;
	case EBADF: return WSAENOTSOCK;
	case EACCES: return WSAEACCES;
	case EFAULT: return WSAEFAULT;
	case EINVAL: return WSAEINVAL;
	case ENFILE: case EMFILE: return WSAEMFILE;
	case EWOULDBLOCK: return WSAEWOULDBLOCK;
	case EINPROGRESS: return WSAEWOULDBLOCK;   /* (Winsock's non-blocking connect) */
	case EALREADY: return WSAEALREADY;
	case ENOTSOCK: return WSAENOTSOCK;
	case EDESTADDRREQ: return WSAEDESTADDRREQ;
	case EMSGSIZE: return WSAEMSGSIZE;
	case ENOPROTOOPT: return WSAENOPROTOOPT;
	case EADDRINUSE: return WSAEADDRINUSE;
	case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
	case ENETDOWN: return WSAENETDOWN;
	case ENETUNREACH: return WSAENETUNREACH;
	case ECONNABORTED: return WSAECONNABORTED;
	case ECONNRESET: return WSAECONNRESET;
	case ENOBUFS: case ENOMEM: return WSAENOBUFS;
	case EISCONN: return WSAEISCONN;
	case ENOTCONN: return WSAENOTCONN;
	case ESHUTDOWN: return WSAESHUTDOWN;
	case ETIMEDOUT: return WSAETIMEDOUT;
	case ECONNREFUSED: return WSAECONNREFUSED;
	case EHOSTUNREACH: return WSAEHOSTUNREACH;
	default: return WSAEINVAL;
	}
}

/* a failed call's error, for WSAGetLastError (test builds: logged, but for "would block") */
#define failed(result) failed_in(__func__, (int)(s), (result))   /* (s: the function's socket) */

static const char getpeername_name[] = "halo_ws_getpeername";

static int failed_in(const char *function, int s, int result)
{
	int slot = thread_slot();

	if (result < 0)
	{
		int wsa_error = wsa_error_from_errno(thread_errors[slot].lwip_errno);

		SetLastError((DWORD)wsa_error);
#if defined(OXBOX_NET_TEST)
		{
			static int logged;

			/* (not "would block", nor getpeername on an unconnected socket, which oxbox_net.c's log asks of all) */
			if (wsa_error != WSAEWOULDBLOCK && !(wsa_error == WSAENOTCONN && function == getpeername_name) &&
				logged++ < 200)
				platform_log("net: %s(%d) failed: errno %d, Winsock error %d", function, s, thread_errors[slot].lwip_errno,
					wsa_error);
		}
#endif
	}
	return result;
}

static int fail_with(int wsa_error)
{
	SetLastError((DWORD)wsa_error);
	return WS_SOCKET_ERROR;
}

int WINAPI WSAGetLastError(void)
{
	return (int)GetLastError();
}

/* ---------- addresses */

static int descriptor(WS_SOCKET s)
{
	return s == WS_INVALID_SOCKET || s == 0 ? -1 : (int)s - 1;
}

static int to_lwip_address(const void *name, int length, struct sockaddr_in *address)
{
	const struct ws_sockaddr_in *ws = (const struct ws_sockaddr_in *)name;

	if (!name || length < (int)sizeof(struct ws_sockaddr_in) - 8)
		return 0;
	memset(address, 0, sizeof(*address));
	address->sin_len = sizeof(*address);
	address->sin_family = AF_INET;
	address->sin_port = ws->sin_port;
	address->sin_addr.s_addr = ws->sin_addr;
	return 1;
}

static void to_ws_address(const struct sockaddr_in *address, void *name, int *length)
{
	struct ws_sockaddr_in ws;

	if (!name || !length)
		return;
	memset(&ws, 0, sizeof(ws));
	ws.sin_family = 2;
	ws.sin_port = address->sin_port;
	ws.sin_addr = address->sin_addr.s_addr;
	memcpy(name, &ws, *length < (int)sizeof(ws) ? (size_t)*length : sizeof(ws));
	*length = sizeof(ws);
}

/* ---------- internet play (OXBOX_NET_P2P: OpenCE's, port/linux/src/p2p.c, as xnet.c calls it on the PC)

Consoles that reached each other through internet play are on this LAN for the game: each peer has a virtual address
(100.64.0.0/10) that its XNADDR maps to (XNetXnAddrToInAddr). The game's datagrams to a peer, and its broadcasts, go
onto the tunnel at once (p2p_send_datagram); its connections to a peer, and datagrams from a socket not bound yet, go to
the peer's stand-ins on 127.0.0.1, and what comes from a stand-in comes from the peer (p2p_incoming). Internet play is
told the ports of the game's sockets: peers reach those alone. */

#if defined(OXBOX_NET_P2P)

void p2p_initialize(unsigned long local_address);
const unsigned char *p2p_identifier(void);
int p2p_peer_address(const unsigned char *identifier, unsigned long *address);
int p2p_outgoing(int stream, int socket, unsigned long *address, unsigned short *port);
int p2p_incoming(int stream, unsigned long *address, unsigned short *port);
int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum_count);
int p2p_send_datagram(unsigned short source_port, unsigned long address, unsigned short port, const void *data,
	int size);
int p2p_broadcast_datagram(unsigned short source_port, unsigned short port, const void *data, int size);
void p2p_socket_port(int socket, int stream, int listening, unsigned short port);
void p2p_port_taken(int stream, unsigned short port);
void p2p_socket_closed(int socket, unsigned short datagram_port);
void p2p_set_hosting_allowed(int allowed);
void p2p_set_hosting_public(int public);
void oxbox_config_set(const char *name, const char *value);
int oxbox_lwip_wait_up(unsigned long milliseconds);

/* opence.ini's online (oxbox_entry.c): on unless it says 0 */
int oxbox_online = 1;

#define PEER_BROADCAST_TARGETS 15

static unsigned long loopback_address(void)
{
	return PP_HTONL(0x7F000001UL);
}

static int is_virtual_address(unsigned long address)
{
	return (lwip_ntohl(address) & 0xFFC00000UL) == 0x64400000UL;
}

static int is_stream(int d)
{
	int type = SOCK_STREAM;
	socklen_t size = sizeof(type);

	lwip_getsockopt(d, SOL_SOCKET, SO_TYPE, &type, &size);
	return type == SOCK_STREAM;
}

/* the local address and port a socket is bound to (network byte order; port 0: not yet) */
static unsigned short bound_port(int d, unsigned long *address)
{
	struct sockaddr_in local;
	socklen_t size = sizeof(local);

	if (lwip_getsockname(d, (struct sockaddr *)&local, &size) < 0)
		return 0;
	if (address)
		*address = local.sin_addr.s_addr;
	return local.sin_port;
}

/* tells internet play the port the game's socket has (bound, given one, or listening: hosting) */
static void note_socket_port(WS_SOCKET s, int listening)
{
	unsigned long address = 0;
	unsigned short port = bound_port(descriptor(s), &address);

	if (!port)
		return;
	/* (one bound to this console alone is not for peers to reach, and its port is no stand-in's) */
	if (address == loopback_address())
		p2p_port_taken(is_stream(descriptor(s)), port);
	else
		p2p_socket_port((int)s, is_stream(descriptor(s)), listening, port);
}

/* a destination that is a peer's goes to its stand-in (1); -1 (WSAEHOSTUNREACH) if it is a peer's that can't be
reached now; 0 if it isn't a peer's. connecting: the game's socket (UDP) connected to it, else -1 */
static int peer_outgoing(int stream, int connecting, struct sockaddr_in *address)
{
	unsigned long ip = address->sin_addr.s_addr;
	unsigned short port = address->sin_port;
	int result = p2p_outgoing(stream, connecting, &ip, &port);

	if (result > 0)
	{
		address->sin_addr.s_addr = ip;
		address->sin_port = port;
	}
	return result;
}

/* traffic from a peer's stand-in comes from the peer */
static void peer_incoming(int stream, struct sockaddr_in *address)
{
	unsigned long ip = address->sin_addr.s_addr;
	unsigned short port = address->sin_port;

	if (p2p_incoming(stream, &ip, &port))
	{
		address->sin_addr.s_addr = ip;
		address->sin_port = port;
	}
}

/* once, as the game starts its networking (XNetStartup, WSAStartup): lwIP up, then internet play (opence.ini's online,
on unless 0: every system link game this console hosts is listed for other Xboxes online, port/linux/src/p2p_lobby.c) */
static void internet_play_start(void)
{
	static int started;

	if (started)
		return;
	started = 1;
	oxbox_lwip_start();
	if (!oxbox_lwip_wait_up(3000))
	{
		platform_log("p2p: lwIP didn't start: no internet play");
		return;
	}
	if (oxbox_online)
	{
		oxbox_config_set("network.online", "true");
		oxbox_config_set("network.public_lobby", "true");
	}
	p2p_initialize(loopback_address());
	p2p_set_hosting_allowed(1);
	p2p_set_hosting_public(1);
	platform_log("p2p: internet play %s",
		oxbox_online ? "on (opence.ini online = 0 turns it off)" : "off (opence.ini online = 0)");
}

#endif

/* ---------- Winsock */

WS_SOCKET WINAPI retail_ws_socket(int family, int type, int protocol)
{
	int s;

	(void)family;
	(void)protocol;
	oxbox_lwip_start();
	s = lwip_socket(AF_INET, type == 2 ? SOCK_DGRAM : SOCK_STREAM, 0);
	if (s < 0)
	{
		failed(s);
		return WS_INVALID_SOCKET;
	}
	return (WS_SOCKET)(s + 1);
}

int WINAPI retail_ws_bind(WS_SOCKET s, const void *name, int length)
{
	struct sockaddr_in address;
	int result;

	if (!to_lwip_address(name, length, &address))
		return fail_with(WSAEFAULT);
	result = failed(lwip_bind(descriptor(s), (struct sockaddr *)&address, sizeof(address)));
#if defined(OXBOX_NET_P2P)
	if (result == 0)
		note_socket_port(s, 0);
#endif
	return result;
}

int WINAPI retail_ws_connect(WS_SOCKET s, const void *name, int length)
{
	struct sockaddr_in address;
	int result;

	if (!to_lwip_address(name, length, &address))
		return fail_with(WSAEFAULT);
#if defined(OXBOX_NET_P2P)
	/* a peer's port: its stand-in */
	if (is_virtual_address(address.sin_addr.s_addr) &&
		peer_outgoing(is_stream(descriptor(s)), (int)s, &address) < 0)
	{
		return fail_with(WSAEHOSTUNREACH);
	}
#endif
	result = lwip_connect(descriptor(s), (struct sockaddr *)&address, sizeof(address));
#if defined(OXBOX_NET_P2P)
	{
		/* (the port it was given: a connection under way has one too; the error kept across the note) */
		int saved = *oxbox_lwip_errno();

		note_socket_port(s, 0);
		*oxbox_lwip_errno() = saved;
	}
#endif
	return failed(result);
}

int WINAPI halo_ws_listen(WS_SOCKET s, int backlog)
{
	int result = failed(lwip_listen(descriptor(s), backlog));

#if defined(OXBOX_NET_P2P)
	/* (it listens while it hosts) */
	if (result == 0)
		note_socket_port(s, 1);
#endif
	return result;
}

WS_SOCKET WINAPI halo_ws_accept(WS_SOCKET s, void *name, int *length)
{
	struct sockaddr_in address;
	socklen_t size = sizeof(address);
	int accepted = lwip_accept(descriptor(s), (struct sockaddr *)&address, &size);

	if (accepted < 0)
	{
		failed(accepted);
		return WS_INVALID_SOCKET;
	}
#if defined(OXBOX_NET_P2P)
	peer_incoming(1, &address);
#endif
	to_ws_address(&address, name, length);
	return (WS_SOCKET)(accepted + 1);
}

int WINAPI retail_ws_send(WS_SOCKET s, const char *buffer, int length, int flags)
{
	(void)flags;
	return failed(lwip_send(descriptor(s), buffer, (size_t)length, 0));
}

int WINAPI retail_ws_recv(WS_SOCKET s, char *buffer, int length, int flags)
{
	int got;

	(void)flags;
	got = lwip_recv(descriptor(s), buffer, (size_t)length, 0);
#if defined(OXBOX_NET_TEST)
	if (!got)
		platform_log("net: recv(%u, %d bytes) = 0: the other end closed", s, length);
#endif
	return failed(got);
}

#if defined(OXBOX_NET_P2P)
/* a broadcast: on this LAN, and to every peer (onto the tunnel at once, from the socket's port; through the stand-ins
while it has none) */
static int broadcast_to_peers(WS_SOCKET s, const char *buffer, int length, struct sockaddr_in *address)
{
	unsigned short source_port = bound_port(descriptor(s), NULL);
	int result = failed(lwip_sendto(descriptor(s), buffer, (size_t)length, 0, (struct sockaddr *)address,
		sizeof(*address)));
	DWORD error = result < 0 ? GetLastError() : 0;

	if (!source_port)
	{
		/* (the send above bound it) */
		source_port = bound_port(descriptor(s), NULL);
		if (source_port)
			note_socket_port(s, 0);
	}
	if (source_port)
	{
		if (p2p_broadcast_datagram(source_port, address->sin_port, buffer, length) > 0 && result < 0)
			result = length;
	}
	else
	{
		unsigned long targets[PEER_BROADCAST_TARGETS];
		unsigned short ports[PEER_BROADCAST_TARGETS];
		int count = p2p_broadcast_targets(address->sin_port, targets, ports, PEER_BROADCAST_TARGETS);
		int index;

		for (index = 0; index < count; index++)
		{
			struct sockaddr_in target = *address;

			target.sin_addr.s_addr = targets[index];
			target.sin_port = ports[index];
			if (lwip_sendto(descriptor(s), buffer, (size_t)length, 0, (struct sockaddr *)&target, sizeof(target)) >= 0 &&
				result < 0)
				result = length;
		}
	}
	if (result < 0)
		SetLastError(error);
	return result;
}
#endif

int WINAPI retail_ws_sendto(WS_SOCKET s, const char *buffer, int length, int flags, const void *to, int to_length)
{
	struct sockaddr_in address;
#if defined(OXBOX_NET_P2P)
	unsigned short source_port;
	int result;
#endif

	(void)flags;
	if (!to)
		return failed(lwip_send(descriptor(s), buffer, (size_t)length, 0));
	if (!to_lwip_address(to, to_length, &address))
		return fail_with(WSAEFAULT);
#if defined(OXBOX_NET_P2P)
	if (address.sin_addr.s_addr == PP_HTONL(0xFFFFFFFFUL))
		return broadcast_to_peers(s, buffer, length, &address);
	source_port = bound_port(descriptor(s), NULL);
	if (is_virtual_address(address.sin_addr.s_addr))
	{
		/* a peer's: onto the tunnel at once, from the socket's port (one not bound yet goes through a stand-in,
		which the send binds it for) */
		switch (source_port ? p2p_send_datagram(source_port, address.sin_addr.s_addr, address.sin_port, buffer, length) : 0)
		{
		case 1:
			return length;
		case -1:
			return fail_with(WSAEHOSTUNREACH);
		}
		if (peer_outgoing(0, -1, &address) < 0)
			return fail_with(WSAEHOSTUNREACH);
	}
	result = failed(lwip_sendto(descriptor(s), buffer, (size_t)length, 0, (struct sockaddr *)&address, sizeof(address)));
	if (!source_port)
	{
		DWORD error = GetLastError();

		note_socket_port(s, 0);
		SetLastError(error);
	}
	return result;
#else
	return failed(lwip_sendto(descriptor(s), buffer, (size_t)length, 0, (struct sockaddr *)&address, sizeof(address)));
#endif
}

int WINAPI retail_ws_recvfrom(WS_SOCKET s, char *buffer, int length, int flags, void *from, int *from_length)
{
	struct sockaddr_in address;
	socklen_t size = sizeof(address);
	int got;

	(void)flags;
	memset(&address, 0, sizeof(address));
	got = lwip_recvfrom(descriptor(s), buffer, (size_t)length, 0, (struct sockaddr *)&address, &size);
	if (got >= 0)
	{
#if defined(OXBOX_NET_P2P)
		peer_incoming(0, &address);
#endif
		to_ws_address(&address, from, from_length);
	}
	return failed(got);
}

int WINAPI retail_ws_closesocket(WS_SOCKET s)
{
#if defined(OXBOX_NET_P2P)
	int d = descriptor(s);

	if (d >= 0)
		p2p_socket_closed((int)s, is_stream(d) ? 0 : bound_port(d, NULL));
#endif
	return failed(lwip_close(descriptor(s)));
}

int WINAPI halo_ws_ioctlsocket(WS_SOCKET s, long command, unsigned long *argument)
{
	if ((unsigned long)command == WS_FIONBIO)
	{
		int flags = lwip_fcntl(descriptor(s), F_GETFL, 0);

		if (flags < 0)
			return failed(flags);
		return failed(lwip_fcntl(descriptor(s), F_SETFL, argument && *argument ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)));
	}
	if ((unsigned long)command == WS_FIONREAD)
	{
		int available = 0, result = lwip_ioctl(descriptor(s), FIONREAD, &available);

		if (result >= 0 && argument)
			*argument = (unsigned long)available;
		return failed(result);
	}
	return fail_with(WSAEINVAL);
}

int WINAPI halo_ws_setsockopt(WS_SOCKET s, int level, int name, const char *value, int length)
{
	int number = value && length >= 4 ? *(const int *)value : (value && length >= 1 ? *(const unsigned char *)value : 0);

	if (level == WS_SOL_SOCKET)
	{
		switch (name)
		{
		case WS_SO_BROADCAST:
			return failed(lwip_setsockopt(descriptor(s), SOL_SOCKET, SO_BROADCAST, &number, sizeof(number)));
		case WS_SO_REUSEADDR:
			return failed(lwip_setsockopt(descriptor(s), SOL_SOCKET, SO_REUSEADDR, &number, sizeof(number)));
		case WS_SO_KEEPALIVE:
			return failed(lwip_setsockopt(descriptor(s), SOL_SOCKET, SO_KEEPALIVE, &number, sizeof(number)));
		case WS_SO_RCVBUF:
			/* (lwIP's receive limit; its window is fixed: lwipopts.h) */
			return failed(lwip_setsockopt(descriptor(s), SOL_SOCKET, SO_RCVBUF, &number, sizeof(number)));
		case WS_SO_SNDBUF:
		case WS_SO_LINGER:
			/* (no such setting in lwIP: accepted) */
			return 0;
		}
	}
	else if (level == WS_IPPROTO_TCP && name == WS_TCP_NODELAY)
	{
		return failed(lwip_setsockopt(descriptor(s), IPPROTO_TCP, TCP_NODELAY, &number, sizeof(number)));
	}
	return fail_with(WSAENOPROTOOPT);
}

int WINAPI halo_ws_getsockopt(WS_SOCKET s, int level, int name, char *value, int *length)
{
	int number = 0, result = 0;
	socklen_t size = sizeof(number);

	if (!value || !length || *length < 4)
		return fail_with(WSAEFAULT);
	if (level == WS_SOL_SOCKET)
	{
		switch (name)
		{
		case WS_SO_ERROR:
			result = lwip_getsockopt(descriptor(s), SOL_SOCKET, SO_ERROR, &number, &size);
			number = wsa_error_from_errno(number);
			break;
		case WS_SO_TYPE:
			result = lwip_getsockopt(descriptor(s), SOL_SOCKET, SO_TYPE, &number, &size);
			number = number == SOCK_DGRAM ? 2 : 1;
			break;
		case WS_SO_BROADCAST:
			result = lwip_getsockopt(descriptor(s), SOL_SOCKET, SO_BROADCAST, &number, &size);
			break;
		case WS_SO_RCVBUF:
			result = lwip_getsockopt(descriptor(s), SOL_SOCKET, SO_RCVBUF, &number, &size);
			break;
		case WS_SO_SNDBUF:
			number = TCP_SND_BUF;
			break;
		default:
			return fail_with(WSAENOPROTOOPT);
		}
	}
	else if (level == WS_IPPROTO_TCP && name == WS_TCP_NODELAY)
	{
		result = lwip_getsockopt(descriptor(s), IPPROTO_TCP, TCP_NODELAY, &number, &size);
	}
	else
	{
		return fail_with(WSAENOPROTOOPT);
	}
	if (result < 0)
		return failed(result);
	*(int *)value = number;
	*length = 4;
	return 0;
}

int WINAPI halo_ws_getsockname(WS_SOCKET s, void *name, int *length)
{
	struct sockaddr_in address;
	socklen_t size = sizeof(address);
	int result = lwip_getsockname(descriptor(s), (struct sockaddr *)&address, &size);

	if (result >= 0)
		to_ws_address(&address, name, length);
	return failed(result);
}

int WINAPI halo_ws_getpeername(WS_SOCKET s, void *name, int *length)
{
	struct sockaddr_in address;
	socklen_t size = sizeof(address);
	int result = lwip_getpeername(descriptor(s), (struct sockaddr *)&address, &size);

	if (result >= 0)
	{
#if defined(OXBOX_NET_P2P)
		peer_incoming(1, &address);
#endif
		to_ws_address(&address, name, length);
	}
	return failed_in(getpeername_name, (int)s, result);
}

static int to_lwip_set(const struct ws_fd_set *ws, fd_set *set, int *maximum)
{
	unsigned int index;

	FD_ZERO(set);
	if (!ws)
		return 0;
	for (index = 0; index < ws->fd_count && index < WS_FD_SETSIZE; index++)
	{
		int d = descriptor(ws->fd_array[index]);

		if (d >= 0 && d < FD_SETSIZE)
		{
			FD_SET(d, set);
			if (d > *maximum)
				*maximum = d;
		}
	}
	return 1;
}

static int from_lwip_set(struct ws_fd_set *ws, const fd_set *set)
{
	unsigned int index, kept = 0;

	if (!ws)
		return 0;
	for (index = 0; index < ws->fd_count && index < WS_FD_SETSIZE; index++)
	{
		int d = descriptor(ws->fd_array[index]);

		if (d >= 0 && d < FD_SETSIZE && FD_ISSET(d, set))
			ws->fd_array[kept++] = ws->fd_array[index];
	}
	ws->fd_count = kept;
	return (int)kept;
}

int WINAPI halo_ws_select(int count, struct ws_fd_set *reading, struct ws_fd_set *writing, struct ws_fd_set *errors,
	const struct ws_timeval *timeout)
{
	fd_set r, w, e;
	struct timeval t;
	int maximum = -1, result;

	(void)count;
	to_lwip_set(reading, &r, &maximum);
	to_lwip_set(writing, &w, &maximum);
	to_lwip_set(errors, &e, &maximum);
	if (timeout)
	{
		t.tv_sec = timeout->tv_sec;
		t.tv_usec = timeout->tv_usec;
	}
	if (maximum < 0)
	{
		/* (no sockets: a wait) */
		if (timeout)
			Sleep((DWORD)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000));
		return 0;
	}
	result = lwip_select(maximum + 1, reading ? &r : NULL, writing ? &w : NULL, errors ? &e : NULL, timeout ? &t : NULL);
	if (result < 0)
		return failed_in(__func__, maximum, result);
	return from_lwip_set(reading, &r) + from_lwip_set(writing, &w) + from_lwip_set(errors, &e);
}

int WINAPI __WSAFDIsSet(WS_SOCKET s, struct ws_fd_set *set)
{
	unsigned int index;

	for (index = 0; set && index < set->fd_count && index < WS_FD_SETSIZE; index++)
	{
		if (set->fd_array[index] == s)
			return 1;
	}
	return 0;
}

int WINAPI WSAStartup(unsigned short version, void *data)
{
	(void)version;
	(void)data;
	oxbox_lwip_start();
#if defined(OXBOX_NET_P2P)
	internet_play_start();
#endif
	return 0;
}

int WINAPI WSACleanup(void)
{
	return 0;
}

/* ---------- XNet */

int WINAPI XNetStartup(const void *parameters)
{
	(void)parameters;
	oxbox_lwip_start();
#if defined(OXBOX_NET_P2P)
	internet_play_start();
#endif
	return 0;
}

int WINAPI XNetCleanup(void)
{
	return 0;
}

/* the console's address: its lwIP address (0 until DHCP, or the link-local fallback, has one) and its card's MAC;
"Ethernet" (2), plus "DHCP" (8) when DHCP gave it, never pending (the game waits only 10 seconds at start-up, and
gets the address again when it hosts: transport_get_xnaddr) */
unsigned long WINAPI XNetGetTitleXnAddr(void *xnaddr)
{
	ws_xnaddr *address = (ws_xnaddr *)xnaddr;
	unsigned long ina = 0;
	int dhcp = 0;

	oxbox_lwip_start();
	oxbox_lwip_address(&ina, &dhcp);
	memset(address, 0, sizeof(*address));
	address->bSizeOfStruct = sizeof(*address);
#if defined(OXBOX_NET_P2P)
	/* (internet play's identifier, which peers' XNADDRs map from: XNetXnAddrToInAddr) */
	memcpy(address->abEnet, p2p_identifier(), 6);
#else
	if (oxbox_lwip_mac())
		memcpy(address->abEnet, oxbox_lwip_mac(), 6);
#endif
	address->ina = ina;
	return 0x02 | (dhcp ? 0x08 : 0);
}

int WINAPI retail_XNetXnAddrToInAddr(const void *xnaddr, const void *key_id, unsigned long *in_addr)
{
	const ws_xnaddr *address = (const ws_xnaddr *)xnaddr;

	(void)key_id;
	if (!address || !in_addr)
		return WSAEINVAL;
#if defined(OXBOX_NET_P2P)
	/* an internet play peer's XNADDR carries its identifier: its virtual address */
	if (p2p_peer_address(address->abEnet, in_addr))
		return 0;
#endif
	*in_addr = address->ina;
	return address->ina ? 0 : WSAEINVAL;
}

#if defined(OXBOX_NET_P2P)
void posix_random_bytes(void *buffer, unsigned long size);   /* (oxbox_p2p_posix.c: unguessable) */

static void random_bytes(unsigned char *buffer, unsigned int length)
{
	posix_random_bytes(buffer, length);
}
#else
static void random_bytes(unsigned char *buffer, unsigned int length)
{
	static unsigned long long state;
	unsigned int index;

	if (!state)
	{
		unsigned long lo, hi;

		__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
		state = ((unsigned long long)hi << 32 | lo) ^ 0x9E3779B97F4A7C15ULL;
	}
	for (index = 0; index < length; index++)
	{
		unsigned long lo, hi;

		__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
		state ^= lo;
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		buffer[index] = (unsigned char)(state >> 33);
	}
}
#endif

int WINAPI XNetRandom(unsigned char *buffer, unsigned int length)
{
	random_bytes(buffer, length);
	return 0;
}

int WINAPI retail_XNetCreateKey(void *key_id, void *key)
{
	random_bytes((unsigned char *)key_id, 8);
	random_bytes((unsigned char *)key, 16);
	return 0;
}

int WINAPI retail_XNetRegisterKey(const void *key_id, const void *key)
{
	(void)key_id;
	(void)key;
	return 0;
}

int WINAPI retail_XNetUnregisterKey(const void *key_id)
{
	(void)key_id;
	return 0;
}

/* the cable: the kernel's view of the card's link (XNET_ETHERNET_LINK_*) */
unsigned long WINAPI XNetGetEthernetLinkStatus(void)
{
	return PhyGetLinkState(oxbox_lwip_card_up() ? FALSE : TRUE);
}

#endif
