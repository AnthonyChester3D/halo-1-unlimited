/*
OXBOX_LWIP.C

The online branch's own network stack (online test build 2, OXBOX_NET_LWIP only): lwIP (port/third_party/lwip) on the
Xbox's network card (nxdk's driver, port/third_party/nxdk_net), in place of Halo's XNet, which has no IP address and
can't reach the internet (online test build 1). XNet and lwIP can't share the card, so this build doesn't start XNet:
Halo's Winsock and XNet calls are answered on lwIP (oxbox_lwip_ws.c), system link and internet play included.

oxbox_lwip_start, from transport_initialize: the stack's thread, the card, DHCP (a link-local 169.254.x.y address
when no DHCP server answers). The test (oxbox_lwip_test_thread, started by oxbox_net_test_start in oxbox_nettest.c)
then tries what online play needs, over lwIP's sockets: DNS, STUN (UDP), an MQTT broker (TCP), a PC on the network.
*/

#include "lwip/opt.h"
#include "lwip/autoip.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"

#include <stdarg.h>

#include "oxbox_kernel.h"

#if defined(OXBOX_NET_LWIP)

err_t nvnetif_init(struct netif *netif);   /* nvnetdrv_lwip.c */

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);

/* oxbox_nettest.c's */
void oxbox_net_test_show(const char *line);
#if defined(OXBOX_NET_P2P)
void oxbox_net_test_status(const char *line);
void oxbox_p2p_test_status(char *line, int size, int join);   /* (p2p/oxbox_p2p_glue.c) */
int oxbox_net_test_join;                                       /* (opence.ini net_test_join) */
#endif
extern int oxbox_net_test_port;
extern unsigned long oxbox_net_test_host, oxbox_net_test_dns;

static struct netif xbox_netif;
static volatile int stack_state;   /* 0 not started, 1 starting, 2 up, -1 failed */

static void show(const char *format, ...)
{
	extern int halo_linux_vsnprintf(char *, size_t, const char *, va_list);
	char line[64];
	va_list arguments;

	va_start(arguments, format);
	halo_linux_vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	oxbox_net_test_show(line);
}

static void status_callback(struct netif *netif)
{
	char address[16], mask[16], gateway[16];

	ip4addr_ntoa_r(netif_ip4_addr(netif), address, sizeof(address));
	ip4addr_ntoa_r(netif_ip4_netmask(netif), mask, sizeof(mask));
	ip4addr_ntoa_r(netif_ip4_gw(netif), gateway, sizeof(gateway));
	platform_log("net: %s, address %s mask %s gateway %s", netif_is_up(netif) ? "up" : "down", address, mask, gateway);
}

static void link_callback(struct netif *netif)
{
	platform_log("net: cable %s", netif_is_link_up(netif) ? "connected" : "unplugged");
}

/* ---------- peers on this wire outside our subnet

System link through a bridge (XLink Kai, or two home networks' switches joined) puts consoles with addresses in
different subnets on one Ethernet. A packet for an address outside the subnet goes to the router, which isn't on the way
to a console on the same wire. So a console seen sending system link traffic (Halo's ports 5150-5152) from a private
or link-local address outside our subnet is kept as on this wire, and sent to directly (LWIP_HOOK_ETHARP_GET_GW: lwIP
asks for its card address with ARP, which it answers whatever the asker's subnet). Internet traffic comes from public
addresses, and through the router: never kept. */

#define ONLINK_PEERS 32

static volatile u32_t onlink_peers[ONLINK_PEERS];
static unsigned onlink_next;

static int private_or_link_local(u32_t address)
{
	unsigned a = address & 0xFF, b = (address >> 8) & 0xFF;

	return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 169 && b == 254);
}

const ip4_addr_t *oxbox_lwip_onlink_gateway(struct netif *netif, const ip4_addr_t *destination)
{
	u32_t address = ip4_addr_get_u32(destination);
	unsigned index;

	(void)netif;
	for (index = 0; index < ONLINK_PEERS; index++)
	{
		if (onlink_peers[index] == address)
			return destination;
	}
	return NULL;
}

#if defined(OXBOX_NET_TEST)
static void log_onlink_peer(void *context)
{
	u32_t source = (u32_t)context;

	platform_log("net: %u.%u.%u.%u is on this wire, outside our subnet: sent to directly", (unsigned)(source & 0xFF),
		(unsigned)(source >> 8 & 0xFF), (unsigned)(source >> 16 & 0xFF), (unsigned)(source >> 24));
}
#endif

/* the card's frames on their way to the stack (from its deferred call): system link senders noted */
static err_t input_with_peers(struct pbuf *p, struct netif *netif)
{
	const u8_t *frame = (const u8_t *)p->payload;

	if (p->len >= 42 && frame[12] == 0x08 && frame[13] == 0x00 && (frame[23] == 17 || frame[23] == 6))
	{
		unsigned header = (frame[14] & 0x0F) * 4;
		u32_t source;

		memcpy(&source, frame + 26, 4);
		if (p->len >= 14 + header + 4)
		{
			unsigned source_port = frame[14 + header] << 8 | frame[15 + header];
			unsigned destination_port = frame[16 + header] << 8 | frame[17 + header];
			u32_t mine = ip4_addr_get_u32(netif_ip4_addr(netif)), mask = ip4_addr_get_u32(netif_ip4_netmask(netif));

			if (((source_port >= 5150 && source_port <= 5152) || (destination_port >= 5150 && destination_port <= 5152)) &&
				private_or_link_local(source) && mine && (source & mask) != (mine & mask))
			{
				unsigned index;

				for (index = 0; index < ONLINK_PEERS && onlink_peers[index] != source; index++)
					;
				if (index == ONLINK_PEERS)
				{
					onlink_peers[onlink_next++ % ONLINK_PEERS] = source;
#if defined(OXBOX_NET_TEST)
					/* (logged from the stack's thread: this is the card's deferred call, where writing a file hangs
					the console) */
					tcpip_try_callback(log_onlink_peer, (void *)source);
#endif
				}
			}
		}
	}
	return tcpip_input(p, netif);
}

/* on the stack's thread, once it runs */
static void stack_ready(void *parameter)
{
	(void)parameter;
	if (!netif_add(&xbox_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, nvnetif_init, input_with_peers))
	{
		platform_log("net: the network card didn't start");
		stack_state = -1;
		return;
	}
	netif_set_hostname(&xbox_netif, "xbox-opence");
	netif_set_status_callback(&xbox_netif, status_callback);
	netif_set_link_callback(&xbox_netif, link_callback);
	netif_set_default(&xbox_netif);
	netif_set_up(&xbox_netif);
#if defined(OXBOX_NET_TEST)
	{
		/* (test builds: opence.ini's net_test_static = a.b.c.d, a fixed address in a.b.c.0/24, no router) */
		extern unsigned long oxbox_net_test_static;

		if (oxbox_net_test_static)
		{
			ip4_addr_t address, mask;

			ip4_addr_set_u32(&address, oxbox_net_test_static);
			IP4_ADDR(&mask, 255, 255, 255, 0);
			netif_set_addr(&xbox_netif, &address, &mask, IP4_ADDR_ANY4);
			platform_log("net: a fixed address (net_test_static)");
			stack_state = 2;
			return;
		}
	}
#endif
	dhcp_start(&xbox_netif);
	platform_log("net: card %02x:%02x:%02x:%02x:%02x:%02x, cable %s, DHCP started", xbox_netif.hwaddr[0],
		xbox_netif.hwaddr[1], xbox_netif.hwaddr[2], xbox_netif.hwaddr[3], xbox_netif.hwaddr[4], xbox_netif.hwaddr[5],
		netif_is_link_up(&xbox_netif) ? "connected" : "unplugged");
	stack_state = 2;
}

/* the console's address (0: none yet), and whether DHCP gave it */
int oxbox_lwip_address(unsigned long *address, int *dhcp)
{
	*address = 0;
	*dhcp = 0;
	if (stack_state != 2)
		return 0;
	*address = ip4_addr_get_u32(netif_ip4_addr(&xbox_netif));
	*dhcp = dhcp_supplied_address(&xbox_netif) != 0;
	return *address != 0;
}

const unsigned char *oxbox_lwip_mac(void)
{
	return stack_state == 2 ? xbox_netif.hwaddr : NULL;
}

int oxbox_lwip_card_up(void)
{
	return stack_state == 2;
}

/* whether the stack is up, waiting for it up to that long */
int oxbox_lwip_wait_up(unsigned long milliseconds)
{
	unsigned long waited = 0;

	while (stack_state == 1 && waited < milliseconds)
	{
		Sleep(10);
		waited += 10;
	}
	return stack_state == 2;
}

void oxbox_lwip_start(void)
{
	if (stack_state)
		return;
	stack_state = 1;
	platform_log("net: starting lwIP (no XNet in this build)");
	tcpip_init(stack_ready, NULL);
}

/* ---------- the test builds' log: lwIP's counters and TCP connections, every 10 seconds */

#include "lwip/stats.h"
#include "lwip/priv/tcp_priv.h"

static void log_tcp_pcb(void *context)
{
	struct tcp_pcb *pcb;
	int count = 0;

	(void)context;
	for (pcb = tcp_active_pcbs; pcb && count < 12; pcb = pcb->next, count++)
	{
		char local[16], remote[16];
		int unsent = 0, unacked = 0;
		struct tcp_seg *seg;

		for (seg = pcb->unsent; seg; seg = seg->next)
			unsent++;
		for (seg = pcb->unacked; seg; seg = seg->next)
			unacked++;
		ip4addr_ntoa_r(ip_2_ip4(&pcb->local_ip), local, sizeof(local));
		ip4addr_ntoa_r(ip_2_ip4(&pcb->remote_ip), remote, sizeof(remote));
		platform_log("net: tcp %s:%u-%s:%u state %d: unsent %d unacked %d (busy %d), snd_wnd %u snd_buf %u queue %u, "
			"rcv_wnd %u, nrtx %d rto %d persist %d, refused %d, ooseq %d", local, pcb->local_port, remote, pcb->remote_port,
			(int)pcb->state, unsent, unacked, pcb->unacked && pcb->unacked->p->ref > 1, (unsigned)pcb->snd_wnd,
			(unsigned)pcb->snd_buf, (unsigned)pcb->snd_queuelen, (unsigned)pcb->rcv_wnd, (int)pcb->nrtx, (int)pcb->rto,
			(int)pcb->persist_backoff, pcb->refused_data != NULL, pcb->ooseq != NULL);
	}
}

void oxbox_lwip_log_stats(void)
{
#if LWIP_STATS
	platform_log("net: link xmit %u recv %u drop %u err %u | ip drop %u | tcp xmit %u recv %u fw %u drop %u memerr %u | "
		"udp xmit %u recv %u drop %u | mem used %u max %u err %u", (unsigned)lwip_stats.link.xmit,
		(unsigned)lwip_stats.link.recv, (unsigned)lwip_stats.link.drop, (unsigned)lwip_stats.link.err,
		(unsigned)lwip_stats.ip.drop, (unsigned)lwip_stats.tcp.xmit, (unsigned)lwip_stats.tcp.recv,
		(unsigned)lwip_stats.tcp.fw, (unsigned)lwip_stats.tcp.drop, (unsigned)lwip_stats.tcp.memerr,
		(unsigned)lwip_stats.udp.xmit, (unsigned)lwip_stats.udp.recv, (unsigned)lwip_stats.udp.drop,
		(unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max, (unsigned)lwip_stats.mem.err);
#endif
	tcpip_callback(log_tcp_pcb, NULL);
}

/* ---------- the test */

static unsigned long ip(unsigned a, unsigned b, unsigned c, unsigned d)
{
	return a | b << 8 | c << 16 | (unsigned long)d << 24;
}

static const char *ip_text(unsigned long address, char *text)
{
	halo_linux_snprintf(text, 16, "%u.%u.%u.%u", (unsigned)(address & 0xFF), (unsigned)(address >> 8 & 0xFF),
		(unsigned)(address >> 16 & 0xFF), (unsigned)(address >> 24));
	return text;
}

static void make_address(struct sockaddr_in *address, unsigned long host, unsigned short port)
{
	memset(address, 0, sizeof(*address));
	address->sin_len = sizeof(*address);
	address->sin_family = AF_INET;
	address->sin_port = lwip_htons(port);
	address->sin_addr.s_addr = host;
}

/* whether a socket is readable (writing: writable) within a time; -1 an error */
static int wait_for(int s, int writing, unsigned long milliseconds)
{
	fd_set set, errors;
	struct timeval timeout;
	int result;

	FD_ZERO(&set);
	FD_SET(s, &set);
	FD_ZERO(&errors);
	FD_SET(s, &errors);
	timeout.tv_sec = (long)(milliseconds / 1000);
	timeout.tv_usec = (long)(milliseconds % 1000 * 1000);
	result = lwip_select(s + 1, writing ? NULL : &set, writing ? &set : NULL, &errors, &timeout);
	if (result < 0 || FD_ISSET(s, &errors))
		return -1;
	return FD_ISSET(s, &set) ? 1 : 0;
}

/* a datagram to a host, and its first answer (the size; 0 none in time; -1 an error) */
static int udp_exchange(unsigned long host, unsigned short port, const void *request, int request_size, void *answer,
	int answer_size, unsigned long milliseconds, int tries)
{
	int s = lwip_socket(AF_INET, SOCK_DGRAM, 0), got = 0, try_index;
	struct sockaddr_in to, from;

	if (s < 0)
		return -1;
	make_address(&to, host, port);
	for (try_index = 0; try_index < tries && got <= 0; try_index++)
	{
		int ready;

		if (lwip_sendto(s, request, request_size, 0, (struct sockaddr *)&to, sizeof(to)) < 0)
		{
			got = -1;
			break;
		}
		while ((ready = wait_for(s, 0, milliseconds)) > 0)
		{
			socklen_t from_size = sizeof(from);

			got = lwip_recvfrom(s, answer, answer_size, 0, (struct sockaddr *)&from, &from_size);
			if (got < 0 || from.sin_addr.s_addr == host)
				break;
			got = 0;
		}
		if (ready < 0)
			got = -1;
	}
	lwip_close(s);
	return got;
}

/* a TCP connection within a time (-1: none; *error errno, 0 for no answer in time) */
static int test_tcp_connect(unsigned long host, unsigned short port, unsigned long milliseconds, int *error)
{
	int s = lwip_socket(AF_INET, SOCK_STREAM, 0), ready;
	struct sockaddr_in to;

	*error = 0;
	if (s < 0)
	{
		*error = errno;
		return -1;
	}
	lwip_fcntl(s, F_SETFL, O_NONBLOCK);
	make_address(&to, host, port);
	if (lwip_connect(s, (struct sockaddr *)&to, sizeof(to)) < 0 && errno != EINPROGRESS)
	{
		*error = errno;
		lwip_close(s);
		return -1;
	}
	ready = wait_for(s, 1, milliseconds);
	if (ready > 0)
	{
		int code = 0;
		socklen_t size = sizeof(code);

		lwip_getsockopt(s, SOL_SOCKET, SO_ERROR, &code, &size);
		if (!code)
			return s;
		*error = code;
	}
	else if (ready < 0)
	{
		*error = errno ? errno : -1;
	}
	lwip_close(s);
	return -1;
}

static int tcp_receive(int s, void *buffer, int size, unsigned long milliseconds)
{
	int got = 0;
	unsigned long start = GetTickCount();

	while (got < size && GetTickCount() - start < milliseconds)
	{
		int ready = wait_for(s, 0, 200), n;

		if (ready < 0)
			break;
		if (!ready)
			continue;
		n = lwip_recv(s, (char *)buffer + got, size - got, 0);
		if (n < 0)
		{
			if (errno == EWOULDBLOCK || errno == EAGAIN)
				continue;
			break;
		}
		if (!n)
			break;
		got += n;
	}
	return got;
}

static unsigned long resolve(const char *name)
{
	struct hostent *host = lwip_gethostbyname(name);
	char a[16];

	if (!host || !host->h_addr_list || !host->h_addr_list[0])
	{
		platform_log("nettest: DNS: %s: no address", name);
		return 0;
	}
	platform_log("nettest: DNS: %s = %s", name, ip_text(*(unsigned long *)host->h_addr_list[0], a));
	return *(unsigned long *)host->h_addr_list[0];
}

unsigned long __stdcall oxbox_lwip_test_thread(void *parameter)
{
	unsigned long start = GetTickCount(), address = 0, stun_host;
	char a[16], b[16], c[16];
	int error;

	(void)parameter;
	/* the address: DHCP's, or link-local after DHCP gives up (about 20 seconds) */
	while (GetTickCount() - start < 30000)
	{
		if (stack_state < 0)
			break;
		if (stack_state == 2)
		{
			address = ip4_addr_get_u32(netif_ip4_addr(&xbox_netif));
			if (address && (dhcp_supplied_address(&xbox_netif) || GetTickCount() - start > 25000))
				break;
		}
		Sleep(250);
	}
	if (stack_state < 0)
	{
		show("NET TEST 2: the network card didn't start");
		show("done (details in oxbox_log.txt)");
		return 0;
	}
	show("NET TEST 2 (own stack)  Xbox %s %s", ip_text(address, a), !address ? "(no address)" :
		dhcp_supplied_address(&xbox_netif) ? "(DHCP)" : (address & 0xFFFF) == ip(169, 254, 0, 0) ? "(link-local)" : "");
	if (!netif_is_link_up(&xbox_netif))
		show("the cable looks unplugged");
	if (!address)
	{
		show("done (details in oxbox_log.txt)");
		return 0;
	}
	{
		const ip_addr_t *dns = dns_getserver(0);

		if (oxbox_net_test_dns)
		{
			ip_addr_t server;

			ip_addr_set_ip4_u32(&server, oxbox_net_test_dns);
			dns_setserver(0, &server);
			dns = dns_getserver(0);
		}
		show("gateway %s  DNS %s", ip_text(ip4_addr_get_u32(netif_ip4_gw(&xbox_netif)), b),
			ip_text(dns ? ip4_addr_get_u32(ip_2_ip4(dns)) : 0, c));
	}

	/* DNS and STUN (UDP) */
	stun_host = resolve("stun.l.google.com");
	if (!stun_host)
	{
		show("DNS: no answer for stun.l.google.com");
	}
	else
	{
		unsigned char request[20], answer[256];
		int got, offset;
		unsigned long mapped = 0;
		unsigned short mapped_port = 0;

		show("DNS: ok");
		memset(request, 0, sizeof(request));
		request[1] = 0x01;
		request[4] = 0x21; request[5] = 0x12; request[6] = 0xA4; request[7] = 0x42;
		*(unsigned long *)(request + 8) = GetTickCount() * 2654435761UL;
		*(unsigned long *)(request + 12) = 0x6F78626F;
		*(unsigned long *)(request + 16) = LWIP_RAND();
		got = udp_exchange(stun_host, 19302, request, sizeof(request), answer, sizeof(answer), 1500, 2);
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
					break;
				}
			}
			offset += 4 + ((length + 3) & ~3);
		}
		if (mapped)
			show("STUN: ok, internet sees %s:%u", ip_text(mapped, a), (unsigned)mapped_port);
		else
			show("STUN %s: %s", ip_text(stun_host, a), got < 0 ? "failed" : got == 0 ? "no answer" : "odd answer");
	}

	/* an MQTT broker (TCP) */
	{
		static const char *const brokers[] = { "opence.milenko.org", "broker.emqx.io" };
		int index, ok = 0;

		for (index = 0; index < 2 && !ok; index++)
		{
			unsigned long broker = resolve(brokers[index]);
			unsigned char packet[64], reply[4];
			char client[24];
			int s, length, size = 0, got;

			if (!broker)
			{
				show("MQTT %s: no address", brokers[index]);
				continue;
			}
			s = test_tcp_connect(broker, 1883, 6000, &error);
			if (s < 0)
			{
				show("MQTT %s: connect %s (%d)", brokers[index], error ? "failed" : "timed out", error);
				continue;
			}
			halo_linux_snprintf(client, sizeof(client), "oxbox-nettest-%04x", (unsigned)(LWIP_RAND() & 0xFFFF));
			length = (int)strlen(client);
			packet[size++] = 0x10;
			packet[size++] = (unsigned char)(10 + 2 + length);
			packet[size++] = 0; packet[size++] = 4;
			memcpy(packet + size, "MQTT", 4); size += 4;
			packet[size++] = 4;
			packet[size++] = 0x02;
			packet[size++] = 0; packet[size++] = 30;
			packet[size++] = 0; packet[size++] = (unsigned char)length;
			memcpy(packet + size, client, length); size += length;
			if (lwip_send(s, packet, size, 0) != size)
			{
				show("MQTT %s: send failed (%d)", brokers[index], errno);
			}
			else
			{
				got = tcp_receive(s, reply, 4, 6000);
				if (got == 4 && reply[0] == 0x20 && reply[3] == 0)
				{
					static const unsigned char disconnect[2] = { 0xE0, 0x00 };

					show("MQTT %s: ok (broker accepted)", brokers[index]);
					lwip_send(s, disconnect, 2, 0);
					ok = 1;
				}
				else
				{
					show("MQTT %s: reply %d bytes", brokers[index], got);
				}
			}
			lwip_close(s);
		}
	}

	/* a PC on the network */
	if (oxbox_net_test_host)
	{
		static const char tcp_hello[] = "OpenCE Xbox TCP test\n", udp_hello[] = "OpenCE Xbox UDP test";
		char line[64], echo[64];
		int s = test_tcp_connect(oxbox_net_test_host, (unsigned short)oxbox_net_test_port, 5000, &error), n;
		size_t used;

		ip_text(oxbox_net_test_host, b);
		if (s < 0)
		{
			halo_linux_snprintf(line, sizeof(line), "PC %s TCP: connect %s (%d)", b, error ? "failed" : "timed out", error);
		}
		else
		{
			lwip_send(s, tcp_hello, sizeof(tcp_hello) - 1, 0);
			n = tcp_receive(s, echo, sizeof(tcp_hello) - 1, 3000);
			halo_linux_snprintf(line, sizeof(line), "PC %s TCP: %s", b, n == (int)sizeof(tcp_hello) - 1 ? "ok" :
				n > 0 ? "partial echo" : "connected, no echo");
			lwip_close(s);
		}
		n = udp_exchange(oxbox_net_test_host, (unsigned short)oxbox_net_test_port, udp_hello, sizeof(udp_hello) - 1, echo,
			sizeof(echo), 1500, 2);
		used = strlen(line);
		halo_linux_snprintf(line + used, sizeof(line) - used, "  UDP: %s", n > 0 ? "ok" : n == 0 ? "no echo" : "failed");
		show("%s", line);
	}
	show("done (details in oxbox_log.txt)");
	/* (then every 10 seconds: lwIP's counters and its TCP connections, for the log; with internet play, its line every
	second) */
	for (;;)
	{
#if defined(OXBOX_NET_P2P)
		int second;

		for (second = 0; second < 10; second++)
		{
			char line[64];

			oxbox_p2p_test_status(line, sizeof(line), oxbox_net_test_join);
			oxbox_net_test_status(line);
			Sleep(1000);
		}
#else
		Sleep(10000);
#endif
		oxbox_lwip_log_stats();
	}
	return 0;
}

#endif
