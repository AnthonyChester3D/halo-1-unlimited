/*
OXBOX_NETTEST.C

The online branch's first test (test build O1, OXBOX_NET_TEST only): can Halo's own network stack (the retail XNet
linked inside Halo's XBE) talk to machines that aren't Xboxes? XNet normally only talks to other Xboxes, over its
own secured traffic; OpenCE's internet play needs plain UDP and TCP: DNS, STUN, MQTT brokers, PCs.

Run once, a few seconds after the network starts (transport_initialize), on its own thread, with the result in the
log (oxbox_log.txt, lines "nettest: ...") and on the main menu:
1. DNS: a query (UDP 53) for the STUN server's and the brokers' names, to 8.8.8.8, then 1.1.1.1 (opence.ini
   net_test_dns = a.b.c.d to use another server).
2. STUN (UDP): a binding request to stun.l.google.com:19302: the public address the internet sees.
3. MQTT (TCP 1883): CONNECT to OpenCE's brokers (opence.milenko.org, then broker.emqx.io), waiting for CONNACK.
4. A PC on the network (opence.ini net_test_host = a.b.c.d, net_test_port = 5555): a line over TCP, then a
   datagram over UDP, each echoed back by the listener script that comes with the test build.
XNetStartup gets XNET_STARTUP_BYPASS_SECURITY (a development kit's switch; does retail XNet honour it?) unless
opence.ini says net_test_bypass = 0.
*/

#include "platform.h"

#include <stdarg.h>
#include <string.h>

#if defined(OXBOX_NET_TEST)

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif
#ifndef SO_ERROR
#define SO_ERROR 0x1007
#endif

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);
void platform_log(const char *format, ...);

SOCKET WINAPI retail_ws_socket(int family, int type, int protocol);
int WINAPI retail_ws_connect(SOCKET s, const struct sockaddr *name, int length);
int WINAPI retail_ws_send(SOCKET s, const char *buffer, int length, int flags);
int WINAPI retail_ws_recv(SOCKET s, char *buffer, int length, int flags);
int WINAPI retail_ws_sendto(SOCKET s, const char *buffer, int length, int flags, const struct sockaddr *to, int to_length);
int WINAPI retail_ws_recvfrom(SOCKET s, char *buffer, int length, int flags, struct sockaddr *from, int *from_length);
int WINAPI retail_ws_closesocket(SOCKET s);
int WINAPI halo_ws_select(int count, struct halo_ws_fd_set *reading, struct halo_ws_fd_set *writing,
	struct halo_ws_fd_set *errors, const struct halo_ws_timeval *timeout);
int WINAPI halo_ws_ioctlsocket(SOCKET s, long command, u_long *argument);
int WINAPI halo_ws_getsockopt(SOCKET s, int level, int name, char *value, int *length);

/* opence.ini's (oxbox_entry.c) */
int oxbox_net_test = 1;
int oxbox_net_test_bypass = 1;
unsigned long oxbox_net_test_host;      /* a.b.c.d as the bytes in memory order (network order), 0: none */
unsigned long oxbox_net_test_dns;
unsigned long oxbox_net_test_static;   /* (test 2: a fixed address, net/oxbox_lwip.c) */
int oxbox_net_test_port = 5555;

/* what the main menu shows (interface.c) */
char oxbox_net_test_lines[8][64];
volatile int oxbox_net_test_line_count;

static unsigned long ip(unsigned a, unsigned b, unsigned c, unsigned d)
{
	return a | b << 8 | c << 16 | (unsigned long)d << 24;
}

static unsigned short port_bytes(unsigned short port)
{
	return (unsigned short)(port >> 8 | port << 8);
}

static const char *ip_text(unsigned long address, char *text)
{
	halo_linux_snprintf(text, 16, "%u.%u.%u.%u", (unsigned)(address & 0xFF), (unsigned)(address >> 8 & 0xFF),
		(unsigned)(address >> 16 & 0xFF), (unsigned)(address >> 24));
	return text;
}

static void show(const char *format, ...)
{
	extern int halo_linux_vsnprintf(char *, size_t, const char *, va_list);
	char line[64];
	va_list arguments;

	va_start(arguments, format);
	halo_linux_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("nettest: %s", line);
	if (oxbox_net_test_line_count < 8)
	{
		strcpy(oxbox_net_test_lines[oxbox_net_test_line_count], line);
		oxbox_net_test_line_count = oxbox_net_test_line_count + 1;
	}
}

/* a result line from the online test 2's own stack (net/oxbox_lwip.c) */
void oxbox_net_test_show(const char *line)
{
	show("%s", line);
}

/* the line below them that changes (test 3: internet play, net/p2p/oxbox_p2p_glue.c), logged when it does */
#if defined(OXBOX_CROSSPLAY)
/* a second line that changes: what the last join refused said (the crossplay experiment: network_client_manager.c,
network_client_message_handler.c), where the Xbox shows no message box */
void oxbox_net_test_notice(const char *line)
{
	static int index = -1;

	if (index < 0)
	{
		if (oxbox_net_test_line_count >= 8)
			return;
		index = oxbox_net_test_line_count;
		oxbox_net_test_lines[index][0] = 0;
		oxbox_net_test_line_count = index + 1;
	}
	if (strcmp(oxbox_net_test_lines[index], line))
	{
		platform_log("nettest: %s", line);
		strncpy(oxbox_net_test_lines[index], line, sizeof(oxbox_net_test_lines[index]) - 1);
	}
}
#endif

void oxbox_net_test_status(const char *line)
{
	static int index = -1;

	if (index < 0)
	{
		if (oxbox_net_test_line_count >= 8)
			return;
		index = oxbox_net_test_line_count;
		oxbox_net_test_lines[index][0] = 0;
		oxbox_net_test_line_count = index + 1;
	}
	if (strcmp(oxbox_net_test_lines[index], line))
	{
		platform_log("nettest: %s", line);
		strncpy(oxbox_net_test_lines[index], line, sizeof(oxbox_net_test_lines[index]) - 1);
	}
}

#if !defined(OXBOX_NET_LWIP)
static void make_address(struct sockaddr_in *address, unsigned long host, unsigned short port)
{
	memset(address, 0, sizeof(*address));
	address->sin_family = AF_INET;
	address->sin_port = port_bytes(port);
	address->sin_addr.s_addr = host;
}

/* whether a socket is readable (or, writing, writable) within a time; -1 an error */
static int wait_for(SOCKET s, int writing, unsigned long milliseconds)
{
	struct halo_ws_fd_set set, errors;
	struct halo_ws_timeval timeout;
	int result;

	FD_ZERO(&set);
	FD_SET(s, &set);
	FD_ZERO(&errors);
	FD_SET(s, &errors);
	timeout.tv_sec = (long)(milliseconds / 1000);
	timeout.tv_usec = (long)(milliseconds % 1000 * 1000);
	result = halo_ws_select(0, writing ? NULL : &set, writing ? &set : NULL, &errors, &timeout);
	if (result == SOCKET_ERROR)
		return -1;
	if (FD_ISSET(s, &errors))
		return -1;
	return FD_ISSET(s, &set) ? 1 : 0;
}

/* a datagram to a host and the first answer from it (its size; 0 none in time; -1 an error, *error the code) */
static int udp_exchange(unsigned long host, unsigned short port, const void *request, int request_size, void *answer,
	int answer_size, unsigned long milliseconds, int tries, int *error)
{
	SOCKET s = retail_ws_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in to, from;
	int got = 0, try_index;

	*error = 0;
	if (s == INVALID_SOCKET)
	{
		*error = WSAGetLastError();
		return -1;
	}
	make_address(&to, host, port);
	for (try_index = 0; try_index < tries && got <= 0; try_index++)
	{
		int ready;

		if (retail_ws_sendto(s, (const char *)request, request_size, 0, (const struct sockaddr *)&to, sizeof(to)) ==
			SOCKET_ERROR)
		{
			*error = WSAGetLastError();
			got = -1;
			break;
		}
		while ((ready = wait_for(s, 0, milliseconds)) > 0)
		{
			int from_size = sizeof(from);

			got = retail_ws_recvfrom(s, (char *)answer, answer_size, 0, (struct sockaddr *)&from, &from_size);
			if (got == SOCKET_ERROR)
			{
				*error = WSAGetLastError();
				got = -1;
				break;
			}
			if (from.sin_addr.s_addr == host)
				break;
			got = 0;
		}
		if (ready < 0)
		{
			*error = WSAGetLastError();
			got = -1;
		}
	}
	retail_ws_closesocket(s);
	return got;
}

/* a TCP connection to a host (INVALID_SOCKET: none, *error the code, or 0 for no answer in time) */
static SOCKET tcp_connect(unsigned long host, unsigned short port, unsigned long milliseconds, int *error)
{
	SOCKET s = retail_ws_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in to;
	u_long nonblocking = 1;
	int ready;

	*error = 0;
	if (s == INVALID_SOCKET)
	{
		*error = WSAGetLastError();
		return INVALID_SOCKET;
	}
	halo_ws_ioctlsocket(s, FIONBIO, &nonblocking);
	make_address(&to, host, port);
	if (retail_ws_connect(s, (const struct sockaddr *)&to, sizeof(to)) == SOCKET_ERROR)
	{
		int code = WSAGetLastError();

		if (code != WSAEWOULDBLOCK)
		{
			*error = code;
			retail_ws_closesocket(s);
			return INVALID_SOCKET;
		}
	}
	ready = wait_for(s, 1, milliseconds);
	if (ready <= 0)
	{
		int code = 0, size = sizeof(code);

		if (ready < 0)
			halo_ws_getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&code, &size);
		*error = ready < 0 ? (code ? code : WSAGetLastError()) : 0;
		retail_ws_closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

/* at most size bytes within a time (what came; -1 an error) */
static int tcp_receive(SOCKET s, char *buffer, int size, unsigned long milliseconds, int *error)
{
	int got = 0;
	unsigned long start = GetTickCount();

	*error = 0;
	while (got < size && GetTickCount() - start < milliseconds)
	{
		int ready = wait_for(s, 0, 200), n;

		if (ready < 0)
		{
			*error = WSAGetLastError();
			return got ? got : -1;
		}
		if (!ready)
			continue;
		n = retail_ws_recv(s, buffer + got, size - got, 0);
		if (n == SOCKET_ERROR)
		{
			int code = WSAGetLastError();

			if (code == WSAEWOULDBLOCK)
				continue;
			*error = code;
			return got ? got : -1;
		}
		if (!n)
			break;
		got += n;
	}
	return got;
}

/* ---------- DNS: an A record's address (0: none) */

static unsigned short random_id(void)
{
	return (unsigned short)(GetTickCount() * 2654435761UL >> 16);
}

static unsigned long dns_lookup(unsigned long server, const char *name, int *status, int *error)
{
	unsigned char query[300], answer[512];
	unsigned short id = random_id();
	int size = 0, got, offset, questions, answers, index;
	const char *label = name;

	/* (the header: an id, recursion wanted, one question) */
	query[size++] = (unsigned char)(id >> 8);
	query[size++] = (unsigned char)id;
	query[size++] = 0x01; query[size++] = 0x00;
	query[size++] = 0; query[size++] = 1;
	query[size++] = 0; query[size++] = 0; query[size++] = 0; query[size++] = 0; query[size++] = 0; query[size++] = 0;
	while (*label)
	{
		const char *dot = label;
		int length;

		while (*dot && *dot != '.')
			dot++;
		length = (int)(dot - label);
		query[size++] = (unsigned char)length;
		memcpy(query + size, label, length);
		size += length;
		label = *dot ? dot + 1 : dot;
	}
	query[size++] = 0;
	query[size++] = 0; query[size++] = 1;   /* A */
	query[size++] = 0; query[size++] = 1;   /* IN */
	*status = 0;
	got = udp_exchange(server, 53, query, size, answer, sizeof(answer), 1500, 2, error);
	if (got <= 0)
	{
		*status = got;
		return 0;
	}
	*status = 1;
	if (got < 12 || answer[0] != query[0] || answer[1] != query[1])
		return 0;
	questions = answer[4] << 8 | answer[5];
	answers = answer[6] << 8 | answer[7];
	offset = 12;
	for (index = 0; index < questions && offset < got; index++)
	{
		while (offset < got && answer[offset] && (answer[offset] & 0xC0) != 0xC0)
			offset += answer[offset] + 1;
		offset += offset < got && (answer[offset] & 0xC0) == 0xC0 ? 2 : 1;
		offset += 4;
	}
	for (index = 0; index < answers && offset + 10 <= got; index++)
	{
		int type, length;

		while (offset < got && answer[offset] && (answer[offset] & 0xC0) != 0xC0)
			offset += answer[offset] + 1;
		offset += offset < got && (answer[offset] & 0xC0) == 0xC0 ? 2 : 1;
		if (offset + 10 > got)
			break;
		type = answer[offset] << 8 | answer[offset + 1];
		length = answer[offset + 8] << 8 | answer[offset + 9];
		offset += 10;
		if (type == 1 && length == 4 && offset + 4 <= got)
			return ip(answer[offset], answer[offset + 1], answer[offset + 2], answer[offset + 3]);
		offset += length;
	}
	return 0;
}

/* a name's address from the DNS servers in turn; the server that answered */
static unsigned long resolve(const char *name, unsigned long *servers, int server_count, unsigned long *answered_by)
{
	int index;

	for (index = 0; index < server_count; index++)
	{
		int status, error;
		unsigned long address = dns_lookup(servers[index], name, &status, &error);
		char a[16], b[16];

		if (address)
		{
			platform_log("nettest: DNS %s: %s = %s", ip_text(servers[index], a), name, ip_text(address, b));
			*answered_by = servers[index];
			return address;
		}
		platform_log("nettest: DNS %s: %s: %s (error %d)", ip_text(servers[index], a), name,
			status < 0 ? "sendto/recvfrom failed" : status == 0 ? "no answer in 3 s" : "answer without an address", error);
	}
	return 0;
}

/* ---------- the test */

static unsigned long __stdcall net_test_thread(void *parameter)
{
	XNADDR address;
	unsigned long servers[3];
	int server_count = 0;
	unsigned long dns_server = 0, stun_host, broker;
	char a[16], b[16];
	int error;

	(void)parameter;
	{
		/* (the address, as XNet has it: the status and the structure's bytes, logged as they change, for up to 20
		seconds while there's no address) */
		unsigned char raw[64];
		unsigned long status = 0, last_status = 0xFFFFFFFF, start = GetTickCount();
		int index;

		do
		{
			memset(raw, 0, sizeof(raw));
			status = XNetGetTitleXnAddr((XNADDR *)raw);
			if (status != last_status)
			{
				char text[3 * 36 + 1];

				for (index = 0; index < 36; index++)
					halo_linux_snprintf(text + 3 * index, 4, "%02x ", raw[index]);
				platform_log("nettest: XNetGetTitleXnAddr = %lx after %lu ms: %s", status, GetTickCount() - start, text);
				last_status = status;
			}
			memcpy(&address, raw, sizeof(address));
			if (address.ina.s_addr && GetTickCount() - start >= 2000)
				break;
			Sleep(250);
		}
		while (GetTickCount() - start < 20000);
	}
	show("NET TEST (bypass %s)  Xbox %s", oxbox_net_test_bypass ? "on" : "off", ip_text(address.ina.s_addr, a));
	if (!address.ina.s_addr)
		show("no IP address: Halo's XNet set none");
	if (oxbox_net_test_dns)
		servers[server_count++] = oxbox_net_test_dns;
	servers[server_count++] = ip(8, 8, 8, 8);
	servers[server_count++] = ip(1, 1, 1, 1);

	/* 1-2: DNS and STUN (UDP) */
	stun_host = resolve("stun.l.google.com", servers, server_count, &dns_server);
	if (!stun_host)
	{
		show("DNS: no answer from %d servers (8.8.8.8, 1.1.1.1)", server_count);
	}
	else
	{
		unsigned char request[20], answer[256];
		int got, offset;
		unsigned long mapped = 0;
		unsigned short mapped_port = 0;

		show("DNS %s: ok", ip_text(dns_server, a));
		memset(request, 0, sizeof(request));
		request[1] = 0x01;   /* binding request */
		request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
		*(unsigned long *)(request + 8) = GetTickCount() * 2654435761UL;
		*(unsigned long *)(request + 12) = 0x6F78626F;
		*(unsigned long *)(request + 16) = random_id();
		got = udp_exchange(stun_host, 19302, request, sizeof(request), answer, sizeof(answer), 1500, 2, &error);
		for (offset = 20; got >= 20 && offset + 4 <= got; )
		{
			int type = answer[offset] << 8 | answer[offset + 1], length = answer[offset + 2] << 8 | answer[offset + 3];

			if ((type == 0x0020 || type == 0x0001) && length >= 8 && offset + 12 <= got && answer[offset + 5] == 1)
			{
				mapped_port = (unsigned short)(answer[offset + 6] << 8 | answer[offset + 7]);
				mapped = ip(answer[offset + 8], answer[offset + 9], answer[offset + 10], answer[offset + 11]);
				if (type == 0x0020)
				{
					mapped_port ^= 0x2112;
					mapped ^= ip(0x21, 0x12, 0xA4, 0x42);
				}
				if (type == 0x0020)
					break;
			}
			offset += 4 + ((length + 3) & ~3);
		}
		if (mapped)
			show("STUN: ok, internet sees %s:%u", ip_text(mapped, a), (unsigned)mapped_port);
		else
			show("STUN %s: %s (error %d)", ip_text(stun_host, a), got < 0 ? "failed" : got == 0 ? "no answer" :
				"odd answer", error);
	}

	/* 3: an MQTT broker (TCP) */
	{
		static const char *const brokers[] = { "opence.milenko.org", "broker.emqx.io" };
		int index, ok = 0;

		for (index = 0; index < 2 && !ok; index++)
		{
			SOCKET s;
			unsigned char connect_packet[64], reply[4];
			char client[24];
			int length, size = 0, got;

			broker = dns_server ? resolve(brokers[index], &dns_server, 1, &dns_server) : 0;
			if (!broker)
			{
				platform_log("nettest: MQTT %s: no address", brokers[index]);
				continue;
			}
			s = tcp_connect(broker, 1883, 5000, &error);
			if (s == INVALID_SOCKET)
			{
				show("MQTT %s: connect %s (%d)", brokers[index], error ? "failed" : "timed out", error);
				continue;
			}
			halo_linux_snprintf(client, sizeof(client), "oxbox-nettest-%04x", (unsigned)random_id());
			length = (int)strlen(client);
			connect_packet[size++] = 0x10;
			connect_packet[size++] = (unsigned char)(10 + 2 + length);
			connect_packet[size++] = 0; connect_packet[size++] = 4;
			memcpy(connect_packet + size, "MQTT", 4); size += 4;
			connect_packet[size++] = 4;      /* 3.1.1 */
			connect_packet[size++] = 0x02;   /* clean session */
			connect_packet[size++] = 0; connect_packet[size++] = 30;
			connect_packet[size++] = 0; connect_packet[size++] = (unsigned char)length;
			memcpy(connect_packet + size, client, length); size += length;
			if (retail_ws_send(s, (const char *)connect_packet, size, 0) != size)
			{
				show("MQTT %s: send failed (%d)", brokers[index], WSAGetLastError());
			}
			else
			{
				got = tcp_receive(s, (char *)reply, 4, 5000, &error);
				if (got == 4 && reply[0] == 0x20 && reply[3] == 0)
				{
					static const unsigned char disconnect[2] = { 0xE0, 0x00 };

					show("MQTT %s: ok (broker accepted)", brokers[index]);
					retail_ws_send(s, (const char *)disconnect, 2, 0);
					ok = 1;
				}
				else
				{
					show("MQTT %s: connected, reply %d bytes %02x %02x (%d)", brokers[index], got,
						got > 0 ? reply[0] : 0, got > 3 ? reply[3] : 0, error);
				}
			}
			retail_ws_closesocket(s);
		}
	}

	/* 4: a PC on the network */
	if (oxbox_net_test_host)
	{
		SOCKET s = tcp_connect(oxbox_net_test_host, (unsigned short)oxbox_net_test_port, 5000, &error);
		char reply[64];
		int got;
		char line[64];

		ip_text(oxbox_net_test_host, b);
		if (s == INVALID_SOCKET)
		{
			halo_linux_snprintf(line, sizeof(line), "PC %s TCP: connect %s (%d)", b, error ? "failed" : "timed out", error);
		}
		else
		{
			static const char hello[] = "OpenCE Xbox TCP test\n";

			retail_ws_send(s, hello, sizeof(hello) - 1, 0);
			got = tcp_receive(s, reply, sizeof(hello) - 1, 3000, &error);
			halo_linux_snprintf(line, sizeof(line), "PC %s TCP: %s", b, got == (int)sizeof(hello) - 1 ? "ok" :
				got > 0 ? "partial echo" : "connected, no echo");
			retail_ws_closesocket(s);
		}
		{
			static const char hello[] = "OpenCE Xbox UDP test";
			char echo[64];
			int n = udp_exchange(oxbox_net_test_host, (unsigned short)oxbox_net_test_port, hello, sizeof(hello) - 1, echo,
				sizeof(echo), 1500, 2, &error);
			size_t used = strlen(line);

			halo_linux_snprintf(line + used, sizeof(line) - used, "  UDP: %s", n > 0 ? "ok" : n == 0 ? "no echo" : "failed");
		}
		show("%s", line);
	}
	else
	{
		platform_log("nettest: no net_test_host in opence.ini: the PC test is skipped");
	}
	show("done (details in oxbox_log.txt)");
	return 0;
}

#endif /* !OXBOX_NET_LWIP */

/* transport_initialize's, when the network didn't start (no address in 10 seconds, or none to be had) */
void oxbox_net_test_no_network(unsigned long status)
{
	show("NET TEST: the network didn't start (status %lu)", status);
	show("check the cable and the dashboard's network settings");
}

/* transport_initialize's, once the network is up */
void oxbox_net_test_start(void)
{
	static int started;
	HANDLE thread;

	if (started || !oxbox_net_test)
		return;
	started = 1;
#if defined(OXBOX_NET_LWIP)
	{
		/* (test 2: the own stack's test, net/oxbox_lwip.c) */
		unsigned long __stdcall oxbox_lwip_test_thread(void *parameter);

		thread = CreateThread(NULL, 0x4000, oxbox_lwip_test_thread, NULL, 0, NULL);
	}
#else
	thread = CreateThread(NULL, 0x4000, net_test_thread, NULL, 0, NULL);
#endif
	if (thread)
		CloseHandle(thread);
	else
		platform_log("nettest: no thread");
}

#endif
