/* download.c: the installer's downloads (download.h): nxdk's network (lwIP, its NIC driver), and Mbed TLS for HTTPS
(port/third_party/mbedtls, as tls_config.h has it) */
#include "download.h"

#include <lwip/dhcp.h>
#include <lwip/netdb.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <nvnetdrv.h>
#include <nxdk/configsector.h>
#include <nxdk/net.h>
#include <xboxkrnl/xboxkrnl.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "psa/crypto.h"
#include "mbedtls/psa_util.h"

#include "sha256.h"
#include "text.h"

#define USER_AGENT "Halo1Unlimited-installer"
#define MOST_REDIRECTS 8
#define TIMEOUT_MS 20000
#define MOST_HEADER 16384
#define MOST_URL 4096

extern struct netif *g_pnetif;
extern unsigned char debug_flags; /* (lwIP's debug output: off) */

/* ---- what Mbed TLS asks of the platform (tls_config.h) ---- */

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)KeTickCount;
}

/* randomness: the processor's cycle counter read around work whose time varies (the caches, the interrupts, the
network's own thread), many times, hashed. Nothing downloaded is secret, so this only has to be unpredictable enough
for the handshake's keys */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
	static unsigned long long counter;
	unsigned long long samples[64];
	unsigned char digest[32];
	size_t done = 0;
	(void)data;
	while (done < len)
	{
		int i;
		for (i = 0; i < 64; i++)
		{
			unsigned long long start = __builtin_readcyclecounter();
			volatile unsigned long spin = 0;
			unsigned long n = (unsigned long)(start ^ (start >> 7)) & 255;
			while (n--)
				spin += n;
			samples[i] = __builtin_readcyclecounter() - start + (start << 20) + KeTickCount + ++counter;
		}
		sha256((unsigned char const *)samples, sizeof(samples), digest);
		for (i = 0; i < 32 && done < len; i++)
			output[done++] = digest[i];
	}
	*olen = len;
	return 0;
}

/* ---- the network ---- */

static unsigned long now_ms(void)
{
	return KeTickCount;
}

static void sleep_ms(unsigned long ms)
{
	LARGE_INTEGER duration;
	duration.QuadPart = -10000LL * (LONGLONG)ms;
	KeDelayExecutionThread(KernelMode, FALSE, &duration);
}

static void step(struct download_report const *report, char const *format, ...)
{
	char text[256];
	va_list list;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	if (report && report->step)
		report->step(report->context, text);
}

int download_network(struct download_report const *report, char *address, char *error)
{
	static int started, result;
	static char started_address[32], started_error[256];
	nxdk_network_config_sector_t settings;
	nx_net_parameters_t dhcp;
	int r, waited;

	if (started)
	{
		text_print(address, 32, "%s", started_address);
		text_print(error, 256, "%s", started_error);
		return result;
	}
	started = 1;
	debug_flags = 0;
	/* (the dashboard's network settings; DHCP if it has none saved) */
	memset(&dhcp, 0, sizeof(dhcp));
	dhcp.ipv4_mode = NX_NET_DHCP;
	dhcp.ipv6_mode = NX_NET_DHCP;
	if (nxLoadNetworkConfig(&settings))
	{
		step(report, "Starting the network (the dashboard's settings: %s)...",
			settings.dhcpFlags & NXDK_NETWORK_CONFIG_MANUAL_IP ? "a fixed address" : "automatic");
		r = nxNetInit(NULL);
	}
	else
	{
		step(report, "Starting the network (automatic address)...");
		r = nxNetInit(&dhcp);
	}
	/* (nxdk gives DHCP 10 seconds; it goes on trying meanwhile, so another 20, with a cable in) */
	for (waited = 0; r == -2 && waited < 20 && nvnetdrv_is_link_up(); waited++)
	{
		if (!waited)
			step(report, "Still waiting for an address from the router...");
		sleep_ms(1000);
		if (dhcp_supplied_address(g_pnetif))
			r = 0;
	}
	if (r)
	{
		if (!nvnetdrv_is_link_up())
			text_print(started_error, sizeof(started_error), "no network cable: the Xbox isn't connected to a network");
		else if (r == -2)
			text_print(started_error, sizeof(started_error), "no address from the router (DHCP): check the network");
		else
			text_print(started_error, sizeof(started_error), "the network didn't start (%d)", r);
		result = 1;
	}
	else
	{
		text_print(started_address, sizeof(started_address), "%s", ip4addr_ntoa(netif_ip4_addr(g_pnetif)));
		result = 0;
	}
	text_print(address, 32, "%s", started_address);
	text_print(error, 256, "%s", started_error);
	return result;
}

/* ---- a connection: a socket, and TLS over it for https ---- */

struct connection
{
	int socket;
	int tls;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config config;
	/* (the bytes read past the headers, read first) */
	unsigned char *pending;
	unsigned long pending_size;
};


/* waits until the socket can be read (or written): 1, else 0 when the time's up */
static int wait_socket(int s, int writing, unsigned long ms)
{
	fd_set set;
	struct timeval timeout;
	FD_ZERO(&set);
	FD_SET(s, &set);
	timeout.tv_sec = (long)(ms / 1000);
	timeout.tv_usec = (long)(ms % 1000) * 1000;
	return select(s + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &timeout) > 0;
}

static int bio_send(void *context, unsigned char const *data, size_t size)
{
	int s = ((struct connection *)context)->socket, n;
	if (!wait_socket(s, 1, TIMEOUT_MS))
		return MBEDTLS_ERR_SSL_TIMEOUT;
	n = send(s, data, size, 0);
	if (n < 0)
		return errno == EWOULDBLOCK || errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	return n;
}

static int bio_receive(void *context, unsigned char *data, size_t size, uint32_t timeout)
{
	int s = ((struct connection *)context)->socket, n;
	if (!wait_socket(s, 0, timeout ? timeout : TIMEOUT_MS))
		return MBEDTLS_ERR_SSL_TIMEOUT;
	n = recv(s, data, size, 0);
	if (n < 0)
		return errno == EWOULDBLOCK || errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	return n;
}

static void connection_close(struct connection *c)
{
	if (c->tls)
	{
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->config);
		c->tls = 0;
	}
	if (c->socket >= 0)
		closesocket(c->socket);
	c->socket = -1;
}

static int connection_open(struct connection *c, char const *host, unsigned short port, int tls,
	struct download_report const *report, char *error)
{
	static int psa_ready;
	struct addrinfo hints, *found = NULL;
	struct sockaddr_in to;
	char port_text[8];
	unsigned long start;
	int r, problem = 0;
	socklen_t problem_size = sizeof(problem);

	memset(c, 0, sizeof(*c));
	c->socket = -1;
	step(report, "Looking up %s...", host);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	text_print(port_text, sizeof(port_text), "%u", port);
	if (getaddrinfo(host, port_text, &hints, &found) || !found)
	{
		text_print(error, 256, "couldn't find %s (DNS): check the network's settings", host);
		return 1;
	}
	memcpy(&to, found->ai_addr, sizeof(to));
	freeaddrinfo(found);
	step(report, "Connecting to %s (%s)...", host, inet_ntoa(to.sin_addr));
	c->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (c->socket < 0)
	{
		text_print(error, 256, "no socket (%d)", errno);
		return 1;
	}
	fcntl(c->socket, F_SETFL, O_NONBLOCK);
	start = now_ms();
	r = connect(c->socket, (struct sockaddr *)&to, sizeof(to));
	if (r && errno != EINPROGRESS)
	{
		text_print(error, 256, "couldn't connect to %s (%d)", host, errno);
		connection_close(c);
		return 1;
	}
	if (r && (!wait_socket(c->socket, 1, TIMEOUT_MS) ||
		getsockopt(c->socket, SOL_SOCKET, SO_ERROR, &problem, &problem_size) || problem))
	{
		text_print(error, 256, problem ? "couldn't connect to %s (%d)" : "couldn't connect to %s: no answer in %lu s",
			host, problem ? (unsigned long)problem : (now_ms() - start) / 1000);
		connection_close(c);
		return 1;
	}
	if (!tls)
		return 0;
	/* TLS: the server's certificate isn't checked (download.h has why) */
	if (!psa_ready)
	{
		if (psa_crypto_init() != PSA_SUCCESS)
		{
			text_print(error, 256, "the secure connection's randomness didn't start");
			connection_close(c);
			return 1;
		}
		psa_ready = 1;
	}
	c->tls = 1;
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->config);
	if ((r = mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
		MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		goto failed;
	mbedtls_ssl_conf_authmode(&c->config, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&c->config, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
	mbedtls_ssl_conf_read_timeout(&c->config, TIMEOUT_MS);
	if ((r = mbedtls_ssl_setup(&c->ssl, &c->config)) != 0 || (r = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0)
		goto failed;
	mbedtls_ssl_set_bio(&c->ssl, c, bio_send, NULL, bio_receive);
	step(report, "Securing the connection (TLS)...");
	start = now_ms();
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
	{
		if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
			goto failed;
		if (now_ms() - start > TIMEOUT_MS)
		{
			r = MBEDTLS_ERR_SSL_TIMEOUT;
			goto failed;
		}
	}
	step(report, "  %s, %s", mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
	return 0;
failed:
	text_print(error, 256, r == MBEDTLS_ERR_SSL_TIMEOUT ? "the secure connection to %s timed out" :
		"the secure connection to %s failed (TLS error -0x%04x)", host, (unsigned)-r);
	connection_close(c);
	return 1;
}

static int connection_write(struct connection *c, char const *data, unsigned long size)
{
	unsigned long done = 0, start = now_ms();
	while (done < size)
	{
		int n;
		if (now_ms() - start > TIMEOUT_MS)
			return 1;
		if (c->tls)
			n = mbedtls_ssl_write(&c->ssl, (unsigned char const *)data + done, size - done);
		else
			n = bio_send(c, (unsigned char const *)data + done, size - done);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (n <= 0)
			return 1;
		done += (unsigned long)n;
	}
	return 0;
}

/* some bytes: how many, 0 at the end, -1 if it went wrong (timed out, or broken) */
static int connection_read(struct connection *c, unsigned char *data, unsigned long size)
{
	unsigned long start = now_ms();
	if (c->pending_size)
	{
		unsigned long n = c->pending_size < size ? c->pending_size : size;
		memcpy(data, c->pending, n);
		c->pending += n;
		c->pending_size -= n;
		return (int)n;
	}
	for (;;)
	{
		int n;
		if (c->tls)
		{
			n = mbedtls_ssl_read(&c->ssl, data, size);
			/* (the end: told, or the connection just closed) */
			if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
				return 0;
		}
		else
		{
			n = bio_receive(c, data, size, TIMEOUT_MS);
			if (n == 0)
				return 0;
		}
		if (n > 0)
			return n;
		if (n == 0)
			return 0;
		if ((n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE &&
			n != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) || now_ms() - start > TIMEOUT_MS)
			return -1;
	}
}

/* ---- HTTP ---- */

struct url
{
	int tls;
	char host[256];
	unsigned short port;
	char path[MOST_URL];
};

static int parse_url(char const *text, struct url *u)
{
	char const *host, *end, *colon;
	size_t n;
	memset(u, 0, sizeof(*u));
	if (!_strnicmp(text, "https://", 8))
		u->tls = 1, u->port = 443, host = text + 8;
	else if (!_strnicmp(text, "http://", 7))
		u->tls = 0, u->port = 80, host = text + 7;
	else
		return 1;
	end = host + strcspn(host, "/?#");
	colon = memchr(host, ':', (size_t)(end - host));
	n = (size_t)((colon ? colon : end) - host);
	if (!n || n >= sizeof(u->host))
		return 1;
	memcpy(u->host, host, n);
	if (colon)
	{
		long port = strtol(colon + 1, NULL, 10);
		if (port <= 0 || port > 65535)
			return 1;
		u->port = (unsigned short)port;
	}
	if (strlen(end) + 2 > sizeof(u->path))
		return 1;
	text_print(u->path, sizeof(u->path), "%s%s", *end == '/' ? "" : "/", end);
	return 0;
}

/* a header's value, if this line is it ("Name: value"), else NULL */
static char const *header_value(char const *line, char const *name)
{
	size_t n = strlen(name);
	if (_strnicmp(line, name, n) || line[n] != ':')
		return NULL;
	line += n + 1;
	while (*line == ' ' || *line == '\t')
		line++;
	return line;
}

struct body
{
	download_write_t put;
	void *context;
	unsigned long done, total, most, reported;
	struct download_report const *report;
};

static int body_put(struct body *b, unsigned char const *data, unsigned long size, char *error)
{
	if (size > b->most - b->done)
	{
		text_print(error, 256, "the file is bigger than %lu MB: not what's expected", b->most / (1024 * 1024));
		return 1;
	}
	if (b->put(b->context, data, size))
	{
		text_print(error, 256, "couldn't write what was downloaded");
		return 1;
	}
	b->done += size;
	if (b->report && b->report->progress && (b->done - b->reported >= 32768 || b->done == b->total))
	{
		b->reported = b->done;
		b->report->progress(b->report->context, b->done, b->total);
	}
	return 0;
}

/* a line of a chunked body ("1a2b;..." then CRLF): the chunk's size, or -1 */
static long chunk_line(struct connection *c)
{
	char line[64];
	unsigned long n = 0;
	unsigned char ch;
	for (;;)
	{
		if (connection_read(c, &ch, 1) != 1)
			return -1;
		if (ch == '\n')
			break;
		if (n < sizeof(line) - 1)
			line[n++] = (char)ch;
	}
	line[n] = 0;
	return strtol(line, NULL, 16);
}

/* one GET: the body to put() on 200 (0), the place it moved to on a redirect (2, location), else 1 and why */
static int get(struct url const *u, struct body *b, char *location, struct download_report const *report, char *error)
{
	static struct connection c;
	static char head[MOST_HEADER + 1];
	static unsigned char buffer[16384];
	char request[MOST_URL + 512], *line, *next, *end_of_head;
	unsigned long have = 0, content_length = 0;
	int status = 0, chunked = 0, has_length = 0, result = 1;

	if (connection_open(&c, u->host, u->port, u->tls, report, error))
		return 1;
	text_print(request, sizeof(request), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\n"
		"Connection: close\r\n\r\n", u->path, u->host, USER_AGENT);
	if (connection_write(&c, request, strlen(request)))
	{
		text_print(error, 256, "couldn't send the request to %s", u->host);
		goto done;
	}
	/* the headers */
	for (;;)
	{
		int n;
		head[have] = 0;
		if ((end_of_head = strstr(head, "\r\n\r\n")) != NULL)
			break;
		if (have == MOST_HEADER)
		{
			text_print(error, 256, "%s's answer is too long", u->host);
			goto done;
		}
		n = connection_read(&c, (unsigned char *)head + have, MOST_HEADER - have);
		if (n <= 0)
		{
			text_print(error, 256, n ? "%s stopped answering" : "%s closed the connection", u->host);
			goto done;
		}
		have += (unsigned long)n;
	}
	*end_of_head = 0;
	c.pending = (unsigned char *)end_of_head + 4;
	c.pending_size = have - (unsigned long)(c.pending - (unsigned char *)head);
	if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1)
	{
		text_print(error, 256, "%s's answer isn't HTTP", u->host);
		goto done;
	}
	location[0] = 0;
	for (line = strstr(head, "\r\n"); line; line = next)
	{
		char const *value;
		line += 2;
		next = strstr(line, "\r\n");
		if (next)
			*next = 0;
		if ((value = header_value(line, "Content-Length")) != NULL)
			content_length = strtoul(value, NULL, 10), has_length = 1;
		else if ((value = header_value(line, "Transfer-Encoding")) != NULL)
			chunked = !_strnicmp(value, "chunked", 7);
		else if ((value = header_value(line, "Location")) != NULL)
			text_print(location, MOST_URL, "%s", value);
		if (next)
			*next = '\r';
	}
	if (status >= 300 && status < 400 && location[0])
	{
		result = 2;
		goto done;
	}
	if (status != 200)
	{
		char *eol = strstr(head, "\r\n");
		if (eol)
			*eol = 0;
		text_print(error, 256, status == 404 ? "%s has no such file (404): no release with it published yet" :
			"%s answered \"%.60s\"", u->host, head);
		goto done;
	}
	b->total = has_length && !chunked ? content_length : 0;
	if (b->total > b->most)
	{
		text_print(error, 256, "the file is %lu MB: bigger than expected", b->total / (1024 * 1024));
		goto done;
	}
	step(report, b->total ? "Downloading %lu KB..." : "Downloading...", b->total / 1024);
	if (chunked)
	{
		long size;
		while ((size = chunk_line(&c)) > 0)
		{
			while (size > 0)
			{
				int n = connection_read(&c, buffer, (unsigned long)size < sizeof(buffer) ? (unsigned long)size :
					sizeof(buffer));
				if (n <= 0)
				{
					text_print(error, 256, "the download stopped after %lu KB", b->done / 1024);
					goto done;
				}
				if (body_put(b, buffer, (unsigned long)n, error))
					goto done;
				size -= n;
			}
			if (chunk_line(&c) != 0) /* (the CRLF after the chunk) */
			{
				text_print(error, 256, "the download's chunks don't read");
				goto done;
			}
		}
		if (size < 0)
		{
			text_print(error, 256, "the download stopped after %lu KB", b->done / 1024);
			goto done;
		}
	}
	else
	{
		for (;;)
		{
			unsigned long want = sizeof(buffer);
			int n;
			if (has_length)
			{
				if (b->done == content_length)
					break;
				if (content_length - b->done < want)
					want = content_length - b->done;
			}
			n = connection_read(&c, buffer, want);
			if (n == 0 && !has_length)
				break;
			if (n <= 0)
			{
				text_print(error, 256, "the download stopped after %lu KB", b->done / 1024);
				goto done;
			}
			if (body_put(b, buffer, (unsigned long)n, error))
				goto done;
		}
	}
	if (report && report->progress)
		report->progress(report->context, b->done, b->total);
	result = 0;
done:
	connection_close(&c);
	return result;
}

int download(char const *url, unsigned long most, download_write_t put, void *write_context,
	struct download_report const *report, unsigned long *size, char *error)
{
	static struct url u;
	static char current[MOST_URL], location[MOST_URL];
	struct body b;
	int redirects;

	text_print(current, sizeof(current), "%s", url);
	for (redirects = 0; redirects <= MOST_REDIRECTS; redirects++)
	{
		int r;
		if (parse_url(current, &u))
		{
			text_print(error, 256, "not an address it can fetch: %.120s", current);
			return 1;
		}
		memset(&b, 0, sizeof(b));
		b.put = put, b.context = write_context, b.most = most, b.report = report;
		r = get(&u, &b, location, report, error);
		if (r == 0)
		{
			*size = b.done;
			return 0;
		}
		if (r == 1)
			return 1;
		/* moved: to another address ("https://..."), or another place on this server ("/...") */
		if (!_strnicmp(location, "https://", 8) || !_strnicmp(location, "http://", 7))
			text_print(current, sizeof(current), "%s", location);
		else if (location[0] == '/' && location[1] == '/')
			text_print(current, sizeof(current), "%s:%s", u.tls ? "https" : "http", location);
		else if (location[0] == '/')
			text_print(current, sizeof(current), "%s://%s:%u%s", u.tls ? "https" : "http", u.host, u.port, location);
		else
		{
			text_print(error, 256, "%s sent it somewhere it can't follow", u.host);
			return 1;
		}
		step(report, "  (it's on %.60s)", strstr(current, "://") + 3);
	}
	text_print(error, 256, "too many redirects");
	return 1;
}
