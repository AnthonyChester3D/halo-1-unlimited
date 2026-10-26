/*
OXBOX_UPNP.C

UPnP for internet play on the Xbox (port/linux/src/posix.h's posix_upnp_forward_udp and posix_upnp_stop_forwarding_udp,
which p2p.c calls on a thread of its own): the home router, asked with the UPnP Internet Gateway Device protocol, forwards
a UDP port of its internet address to the tunnel's socket on this console, so that a peer whose NAT is too strict for
hole punching can still reach it there. What OpenCE's posix_upnp.c does with miniupnpc on the PC, done here in a few
hundred lines on lwIP's sockets (miniupnpc wants a POSIX system; the Xbox has none):

- the router is looked for with an SSDP search (a multicast M-SEARCH on the LAN: lwIP sends to a multicast address
  without IGMP, and the answers come back unicast), for two and a half seconds, until one is found, and then kept;
- its description (HTTP) names its WANIPConnection or WANPPPConnection service's control URL; the first that says its
  internet address is used;
- forwarding: AddPortMapping (SOAP over HTTP), for an hour (p2p.c renews it, asking for the same port), or a
  permanent one where the router takes no other (error 725), another port where the router has that one forwarded to
  another machine (718), the same port as this console's where the router wants that (724);
- a router whose internet address is a private one (behind another NAT, such as a carrier's) can't help, and isn't
  asked.

Nothing is kept in memory but the router's address and control URL, and nothing is taken from the heap: the
description is read a piece at a time through a window on the thread's stack. One thread calls these at a time
(p2p.c, on a thread of its own). Only in the builds with internet play (OXBOX_NET_P2P).
*/

#include "lwip/opt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include <stdlib.h>
#include <string.h>

#if defined(OXBOX_NET_P2P)

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);
void platform_log(char const *format, ...);
void oxbox_lwip_start(void);
void posix_random_bytes(void *buffer, unsigned long size);

/* ---------- constants */

enum
{
	/* milliseconds: the router is looked for; an HTTP request waits for its answer */
	DISCOVERY_TIME = 2500,
	HTTP_TIME = 5000,
	/* a router's description is read through a window of this size (on the stack: a heap block this big would stay
	committed after it is freed), and a SOAP answer whole */
	DESCRIPTION_WINDOW = 4096,
	ANSWER_SIZE = 3072,
	/* WAN connection services kept from a description */
	MAXIMUM_SERVICES = 4,
	/* routers whose descriptions are tried (the first answers of the search) */
	MAXIMUM_LOCATIONS = 4,
	/* ports tried when the router forwards the one asked for to another machine */
	PORT_ATTEMPTS = 8,
	/* UPnP's errors (UPnP IGD WANIPConnection) */
	ERROR_CONFLICT = 718,
	ERROR_SAME_PORT_VALUES_REQUIRED = 724,
	ERROR_ONLY_PERMANENT_LEASES = 725,
};

#define FORWARDING_DESCRIPTION "Halo internet play"
#define LEASE_SECONDS "3600"
#define SSDP_ADDRESS "239.255.255.250"
#define SSDP_PORT 1900

static char const *const search_targets[] =
{
	"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
	"urn:schemas-upnp-org:service:WANIPConnection:1",
	"urn:schemas-upnp-org:device:InternetGatewayDevice:2",
	"urn:schemas-upnp-org:service:WANPPPConnection:1",
};

/* ---------- globals */

static struct
{
	/* the router found (kept until a request to it fails to connect) */
	int found;
	unsigned long address;              /* (network byte order) */
	unsigned short port;                /* (host byte order) */
	char control_path[256];
	char service[80];
	/* this console's address as the router sees it (dotted) */
	char lan_address[16];
	/* the router takes only permanent forwardings */
	int permanent_only;
} upnp;

/* ---------- private code: text */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* text's first place (case aside) where key is, or NULL */
static char const *find_text(char const *text, char const *key)
{
	size_t length = strlen(key);

	for (; *text; text++)
	{
		size_t i;

		for (i = 0; i < length && text[i] && lower((unsigned char)text[i]) == lower((unsigned char)key[i]); i++)
			;
		if (i == length)
			return text;
	}
	return NULL;
}

/* an HTTP header's value (name without its colon), trimmed, into value; 1 if there was one */
static int header_value(char const *headers, char const *name, char *value, int size)
{
	char const *line = headers;
	size_t length = strlen(name);

	while (line && *line)
	{
		if (!strncasecmp(line, name, length) && line[length] == ':')
		{
			char const *start = line + length + 1;
			int count = 0;

			while (*start == ' ' || *start == '\t')
				start++;
			while (start[count] && start[count] != '\r' && start[count] != '\n' && count < size - 1)
				count++;
			while (count > 0 && (start[count - 1] == ' ' || start[count - 1] == '\t'))
				count--;
			memcpy(value, start, (size_t)count);
			value[count] = 0;
			return 1;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return 0;
}

/* an XML element's text (the first <tag> from start, before end if end isn't NULL), into value; 1 if found */
static int element_text(char const *start, char const *end, char const *tag, char *value, int size)
{
	char open[48];
	char const *found, *close;
	int count;

	halo_linux_snprintf(open, sizeof(open), "<%s>", tag);
	found = find_text(start, open);
	if (!found || (end && found >= end))
	{
		/* (a namespace prefix: <u:tag> or <m:tag>, as some routers answer) */
		halo_linux_snprintf(open, sizeof(open), ":%s>", tag);
		found = find_text(start, open);
		if (!found || (end && found >= end))
			return 0;
	}
	found += strlen(open);
	close = strchr(found, '<');
	if (!close)
		return 0;
	while (found < close && (*found == ' ' || *found == '\r' || *found == '\n' || *found == '\t'))
		found++;
	count = (int)(close - found);
	while (count > 0 && (found[count - 1] == ' ' || found[count - 1] == '\r' || found[count - 1] == '\n' ||
		found[count - 1] == '\t'))
		count--;
	if (count >= size)
		count = size - 1;
	memcpy(value, found, (size_t)count);
	value[count] = 0;
	return 1;
}

/* "http://a.b.c.d[:port]/path": the address (network byte order), port and path; 1 if it is one (a router names
itself by its address) */
static int parse_url(char const *url, unsigned long *address, unsigned short *port, char *path, int size)
{
	char host[64];
	char const *at = url, *slash;
	int count = 0;

	if (strncasecmp(at, "http://", 7))
		return 0;
	at += 7;
	while (at[count] && at[count] != ':' && at[count] != '/' && count < (int)sizeof(host) - 1)
	{
		host[count] = at[count];
		count++;
	}
	host[count] = 0;
	*address = ipaddr_addr(host);
	if (*address == IPADDR_NONE || *address == 0)
		return 0;
	at += count;
	*port = 80;
	if (*at == ':')
		*port = (unsigned short)strtoul(at + 1, NULL, 10);
	slash = strchr(at, '/');
	halo_linux_snprintf(path, (size_t)size, "%s", slash ? slash : "/");
	return *port != 0;
}

static int is_private(unsigned long address)
{
	unsigned char const *b = (unsigned char const *)&address;

	return b[0] == 10 || b[0] == 127 || b[0] == 0 || (b[0] == 172 && (b[1] & 0xF0) == 16) ||
		(b[0] == 192 && b[1] == 168) || (b[0] == 100 && (b[1] & 0xC0) == 64) || (b[0] == 169 && b[1] == 254);
}

static void dotted(unsigned long address, char *text, int size)
{
	unsigned char const *b = (unsigned char const *)&address;

	halo_linux_snprintf(text, (size_t)size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ---------- private code: sockets */

/* whether the socket gets something to read (or, for write, can be written: a connection made) in ms */
static int wait_socket(int socket, int write, int ms)
{
	struct pollfd polled;

	polled.fd = socket;
	polled.events = write ? POLLOUT : POLLIN;
	polled.revents = 0;
	return lwip_poll(&polled, 1, ms) > 0 && (polled.revents & (write ? POLLOUT : POLLIN | POLLHUP));
}

/* a chunked HTTP body decoded where it is: its length */
static int dechunk(char *body, int length)
{
	char *read = body, *write = body, *end = body + length;

	while (read < end)
	{
		unsigned long size = strtoul(read, NULL, 16);
		char *line = strstr(read, "\r\n");

		if (!line || size == 0)
			break;
		read = line + 2;
		if (read + size > end)
			size = (unsigned long)(end - read);
		memmove(write, read, size);
		write += size;
		read += size;
		if (read + 2 <= end && read[0] == '\r')
			read += 2;
	}
	*write = 0;
	return (int)(write - body);
}

/* a connection to the router (address, network byte order; port) with the request sent: the socket, or -1. lan:
this console's address on the connection */
static int http_open(unsigned long address, unsigned short port, char const *request, int request_length,
	unsigned long *lan)
{
	struct sockaddr_in remote, local;
	socklen_t local_size = sizeof(local);
	int socket = lwip_socket(AF_INET, SOCK_STREAM, 0);
	int sent = 0, nonblocking = 1, socket_error = 0;
	socklen_t error_size = sizeof(socket_error);

	if (socket < 0)
		return -1;
	memset(&remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_port = lwip_htons(port);
	remote.sin_addr.s_addr = address;
	lwip_ioctl(socket, FIONBIO, &nonblocking);
	/* (connected, or refused: the socket can be written either way; SO_ERROR says which) */
	lwip_connect(socket, (struct sockaddr *)&remote, sizeof(remote));
	if (!wait_socket(socket, 1, HTTP_TIME) ||
		lwip_getsockopt(socket, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) || socket_error)
	{
		lwip_close(socket);
		return -1;
	}
	nonblocking = 0;
	lwip_ioctl(socket, FIONBIO, &nonblocking);
	if (lan && !lwip_getsockname(socket, (struct sockaddr *)&local, &local_size))
		*lan = local.sin_addr.s_addr;
	while (sent < request_length)
	{
		int count = lwip_send(socket, request + sent, (size_t)(request_length - sent), 0);

		if (count <= 0)
		{
			lwip_close(socket);
			return -1;
		}
		sent += count;
	}
	return socket;
}

/* what the socket has to read, after the size already in buffer (of size bytes, kept terminated): the new length;
the same at the end of the answer (closed, or nothing for HTTP_TIME) */
static int http_read(int socket, char *buffer, int length, int size)
{
	if (length < size - 1 && wait_socket(socket, 0, HTTP_TIME))
	{
		int count = lwip_recv(socket, buffer + length, (size_t)(size - 1 - length), 0);

		if (count > 0)
			length += count;
	}
	buffer[length] = 0;
	return length;
}

/* an HTTP request to the router (address, network byte order; port): the answer's status (200 OK), its body (from
answer's start, terminated) in answer; 0 if there was no answer */
static int http_request(unsigned long address, unsigned short port, char const *request, int request_length,
	char *answer, int size)
{
	int socket = http_open(address, port, request, request_length, NULL);
	int got = 0, status = 0;
	char *body;

	answer[0] = 0;
	if (socket < 0)
		return 0;
	for (;;)
	{
		int before = got;

		got = http_read(socket, answer, got, size);
		if (got == before)
			break;
	}
	lwip_close(socket);
	if (!strncmp(answer, "HTTP/1.", 7) && got > 12)
		status = atoi(answer + 9);
	body = strstr(answer, "\r\n\r\n");
	if (body)
	{
		char encoding[32];
		int chunked;

		*body = 0;
		chunked = header_value(strchr(answer, '\n') ? strchr(answer, '\n') + 1 : answer, "Transfer-Encoding",
			encoding, sizeof(encoding)) && !strncasecmp(encoding, "chunked", 7);
		body += 4;
		got = (int)(answer + got - body);
		memmove(answer, body, (size_t)got + 1);
		if (chunked)
			dechunk(answer, got);
	}
	else
	{
		answer[0] = 0;
	}
	return status;
}

/* a SOAP action of the router's service, with its arguments (XML): the HTTP status (200: done), the answer's body in
answer; *error: UPnP's error code (0 none) */
static int soap(char const *action, char const *arguments, char *answer, int size, int *error)
{
	char body[1024], request[1700], code[16];
	int body_length, request_length, status;

	body_length = halo_linux_snprintf(body, sizeof(body),
		"<?xml version=\"1.0\"?>\r\n"
		"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
		"<u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
		action, upnp.service, arguments, action);
	request_length = halo_linux_snprintf(request, sizeof(request),
		"POST %s HTTP/1.1\r\n"
		"Host: %u.%u.%u.%u:%u\r\n"
		"Content-Type: text/xml; charset=\"utf-8\"\r\n"
		"SOAPAction: \"%s#%s\"\r\n"
		"Content-Length: %d\r\n"
		"Connection: close\r\n\r\n%s",
		upnp.control_path, (unsigned)(upnp.address & 0xFF), (unsigned)(upnp.address >> 8 & 0xFF),
		(unsigned)(upnp.address >> 16 & 0xFF), (unsigned)(upnp.address >> 24), (unsigned)upnp.port, upnp.service,
		action, body_length, body);
	if (body_length <= 0 || body_length >= (int)sizeof(body) || request_length <= 0 ||
		request_length >= (int)sizeof(request))
		return 0;
	status = http_request(upnp.address, upnp.port, request, request_length, answer, size);
	*error = 0;
	if (status != 200 && element_text(answer, NULL, "errorCode", code, sizeof(code)))
		*error = atoi(code);
	return status;
}

/* the router's internet address (network byte order), or 0 */
static unsigned long external_address(void)
{
	char answer[ANSWER_SIZE], text[32];
	int error;

	if (soap("GetExternalIPAddress", "", answer, sizeof(answer), &error) != 200 ||
		!element_text(answer, NULL, "NewExternalIPAddress", text, sizeof(text)))
		return 0;
	return ipaddr_addr(text) == IPADDR_NONE ? 0 : ipaddr_addr(text);
}

/* ---------- private code: finding the router */

/* a WAN connection service of the router's (from its description): the service's type and control URL */
struct wan_service
{
	char type[80];
	char control[192];
};

/* the WAN connection services a router's description names (read a piece at a time, each <service> element as it
comes: no buffer for the whole of it), at most maximum of them: how many */
static int description_services(int socket, struct wan_service *services, int maximum)
{
	char window[DESCRIPTION_WINDOW];
	int length = 0, count = 0, ended = 0;
	char *body = NULL;

	window[0] = 0;
	/* (the headers: the status, 200; HTTP/1.0 asked, so the body isn't chunked) */
	while (!body)
	{
		int before = length;

		length = http_read(socket, window, length, sizeof(window));
		body = strstr(window, "\r\n\r\n");
		if (!body && length == before)
			return 0;
	}
	if (strncmp(window, "HTTP/1.", 7) || atoi(window + 9) != 200)
		return 0;
	body += 4;
	length = (int)(window + length - body);
	memmove(window, body, (size_t)length + 1);
	while (count < maximum)
	{
		char const *start = find_text(window, "<service>");
		char const *end = start ? find_text(start, "</service>") : NULL;

		if (start && end)
		{
			struct wan_service *service = &services[count];

			if (element_text(start, end, "serviceType", service->type, sizeof(service->type)) &&
				(find_text(service->type, ":service:WANIPConnection:") ||
				find_text(service->type, ":service:WANPPPConnection:")) &&
				element_text(start, end, "controlURL", service->control, sizeof(service->control)))
				count++;
			end += strlen("</service>");
			length -= (int)(end - window);
			memmove(window, end, (size_t)length + 1);
			continue;
		}
		if (ended)
			break;
		/* (keep the element begun, or the last bytes, which may be the start of one; an element bigger than the
		window is dropped) */
		{
			int keep_from = start && start > window ? (int)(start - window) : start ? 0 : length > 16 ? length - 16 : 0;

			if (start == window && length >= (int)sizeof(window) - 1)
				keep_from = length - 16;
			length -= keep_from;
			memmove(window, window + keep_from, (size_t)length + 1);
		}
		{
			int before = length;

			length = http_read(socket, window, length, sizeof(window));
			ended = length == before;
		}
	}
	return count;
}

/* a router's description (from its location): its WAN connection service that says the router's internet address
(WANIPConnection first, then WANPPPConnection), kept in upnp; 1 if there was one */
static int read_description(char const *location)
{
	struct wan_service services[MAXIMUM_SERVICES];
	char path[256], request[384];
	unsigned long address, lan = 0;
	unsigned short port;
	int socket, length, count, kind, index;

	if (!parse_url(location, &address, &port, path, sizeof(path)))
		return 0;
	length = halo_linux_snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %u.%u.%u.%u:%u\r\n\r\n", path,
		(unsigned)(address & 0xFF), (unsigned)(address >> 8 & 0xFF), (unsigned)(address >> 16 & 0xFF),
		(unsigned)(address >> 24), (unsigned)port);
	socket = http_open(address, port, request, length, &lan);
	if (socket < 0)
		return 0;
	count = description_services(socket, services, MAXIMUM_SERVICES);
	lwip_close(socket);
	if (!lan)
		return 0;
	dotted(lan, upnp.lan_address, sizeof(upnp.lan_address));
	for (kind = 0; kind < 2; kind++)
	{
		for (index = 0; index < count; index++)
		{
			struct wan_service const *service = &services[index];

			if (!find_text(service->type, kind ? ":service:WANPPPConnection:" : ":service:WANIPConnection:"))
				continue;
			halo_linux_snprintf(upnp.service, sizeof(upnp.service), "%s", service->type);
			upnp.address = address;
			upnp.port = port;
			if (!strncasecmp(service->control, "http://", 7))
			{
				if (!parse_url(service->control, &upnp.address, &upnp.port, upnp.control_path,
					sizeof(upnp.control_path)))
					continue;
			}
			else
			{
				halo_linux_snprintf(upnp.control_path, sizeof(upnp.control_path), "%s%s",
					service->control[0] == '/' ? "" : "/", service->control);
			}
			if (external_address())
				return 1;
		}
	}
	return 0;
}

/* the router, looked for (SSDP) until it is found; 1 if there is one */
static int find_router(char *error, int error_size)
{
	char locations[MAXIMUM_LOCATIONS][160];
	int location_count = 0, socket, index;
	struct sockaddr_in local, group;
	unsigned long start;

	if (upnp.found)
		return 1;
	socket = lwip_socket(AF_INET, SOCK_DGRAM, 0);
	if (socket < 0)
	{
		halo_linux_snprintf(error, (size_t)error_size, "no socket for the UPnP search");
		return 0;
	}
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	lwip_bind(socket, (struct sockaddr *)&local, sizeof(local));
	memset(&group, 0, sizeof(group));
	group.sin_family = AF_INET;
	group.sin_port = lwip_htons(SSDP_PORT);
	group.sin_addr.s_addr = ipaddr_addr(SSDP_ADDRESS);
	for (index = 0; index < (int)(sizeof(search_targets) / sizeof(search_targets[0])); index++)
	{
		char search[256];
		int length = halo_linux_snprintf(search, sizeof(search),
			"M-SEARCH * HTTP/1.1\r\nHOST: " SSDP_ADDRESS ":1900\r\nST: %s\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n\r\n",
			search_targets[index]);

		lwip_sendto(socket, search, (size_t)length, 0, (struct sockaddr *)&group, sizeof(group));
	}
	start = sys_now();
	while (location_count < MAXIMUM_LOCATIONS && sys_now() - start < DISCOVERY_TIME)
	{
		char answer[1024], location[160];
		int got, known;

		if (!wait_socket(socket, 0, (int)(DISCOVERY_TIME - (sys_now() - start))))
			break;
		got = lwip_recvfrom(socket, answer, sizeof(answer) - 1, 0, NULL, NULL);
		if (got <= 0)
			continue;
		answer[got] = 0;
		if (strncmp(answer, "HTTP/1.", 7) || !header_value(answer, "LOCATION", location, sizeof(location)))
			continue;
		for (known = 0; known < location_count && strcmp(locations[known], location); known++)
			;
		if (known == location_count)
			halo_linux_snprintf(locations[location_count++], sizeof(locations[0]), "%s", location);
	}
	lwip_close(socket);
	if (!location_count)
	{
		halo_linux_snprintf(error, (size_t)error_size, "no UPnP router on this network");
		return 0;
	}
	for (index = 0; index < location_count && !upnp.found; index++)
	{
		if (read_description(locations[index]))
			upnp.found = 1;
	}
	if (!upnp.found)
	{
		halo_linux_snprintf(error, (size_t)error_size, "the UPnP router has no internet connection service to ask");
		return 0;
	}
	platform_log("Internet play: UPnP: the router is %u.%u.%u.%u:%u (%s)", (unsigned)(upnp.address & 0xFF),
		(unsigned)(upnp.address >> 8 & 0xFF), (unsigned)(upnp.address >> 16 & 0xFF), (unsigned)(upnp.address >> 24),
		(unsigned)upnp.port, upnp.service);
	return 1;
}

static int add_forwarding(char const *external_port, char const *internal_port, int *error)
{
	char arguments[640], answer[ANSWER_SIZE];
	int status;

	for (;;)
	{
		halo_linux_snprintf(arguments, sizeof(arguments),
			"<NewRemoteHost></NewRemoteHost><NewExternalPort>%s</NewExternalPort><NewProtocol>UDP</NewProtocol>"
			"<NewInternalPort>%s</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
			"<NewPortMappingDescription>" FORWARDING_DESCRIPTION "</NewPortMappingDescription>"
			"<NewLeaseDuration>%s</NewLeaseDuration>",
			external_port, internal_port, upnp.lan_address, upnp.permanent_only ? "0" : LEASE_SECONDS);
		status = soap("AddPortMapping", arguments, answer, sizeof(answer), error);
		if (status == 200)
		{
			*error = 0;
			return 1;
		}
		if (*error != ERROR_ONLY_PERMANENT_LEASES || upnp.permanent_only)
			return 0;
		upnp.permanent_only = 1;
	}
}

/* ---------- public code (posix.h) */

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, unsigned long *external,
	unsigned short *external_port, char *error, int error_size)
{
	char internal_text[8], external_text[8];
	unsigned short try_port = lwip_ntohs(preferred_port ? preferred_port : port);
	unsigned long address;
	int attempt, code = 0, done = 0;

	oxbox_lwip_start();
	if (!find_router(error, error_size))
		return 0;
	address = external_address();
	if (!address)
	{
		/* (the router went away, or this console moved: looked for again next time) */
		upnp.found = 0;
		halo_linux_snprintf(error, (size_t)error_size, "the router did not say its internet address");
		return 0;
	}
	if (is_private(address))
	{
		halo_linux_snprintf(error, (size_t)error_size,
			"the router's internet address is a private one: another NAT is beyond it");
		return 0;
	}
	halo_linux_snprintf(internal_text, sizeof(internal_text), "%u", (unsigned)lwip_ntohs(port));
	for (attempt = 0; attempt < PORT_ATTEMPTS && !done; attempt++)
	{
		halo_linux_snprintf(external_text, sizeof(external_text), "%u", (unsigned)try_port);
		done = add_forwarding(external_text, internal_text, &code);
		if (done)
			break;
		if (code == ERROR_SAME_PORT_VALUES_REQUIRED && try_port != lwip_ntohs(port))
		{
			try_port = lwip_ntohs(port);
			continue;
		}
		if (code != ERROR_CONFLICT)
			break;
		posix_random_bytes(&try_port, sizeof(try_port));
		try_port = (unsigned short)(49152 + try_port % 16384);
	}
	if (!done)
	{
		halo_linux_snprintf(error, (size_t)error_size, "the router refused to forward a port (%d)", code);
		return 0;
	}
	*external = address;
	*external_port = lwip_htons(try_port);
	return 1;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	char arguments[256], answer[ANSWER_SIZE];
	int error;

	if (!upnp.found)
		return;
	halo_linux_snprintf(arguments, sizeof(arguments),
		"<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
		(unsigned)lwip_ntohs(external_port));
	soap("DeletePortMapping", arguments, answer, sizeof(answer), &error);
}

#endif
