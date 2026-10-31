/*
OXBOX_UPDATE.C

The game's look for a newer release (opence.ini update_check; oxbox_sdk.c oxbox_update_frame asks and shows the
answer): the first 128 bytes of the project's latest package, its header, which says its release and network version
(port/oxbox/tools/mkpackage.py), fetched over HTTPS from GitHub, as the installer fetches the whole package
(installer/download.c), following GitHub's redirects to its file host.

- Its code (and Mbed TLS's) is an XBE section of its own, OXUPD (link.ld, mkxbe.py), loaded for the check only, in
  the menus, and unloaded after it: a match never carries it.
- Its own thread, at the game's priority; it gives up when told to (a map loading), within a quarter of a second.
- The server's certificate isn't checked (as the installer's isn't: download.h has why). What comes back only decides
  whether the player is asked; the installer installs only a package signed by a key it trusts.
- Nothing is sent but the request: no player name, no hardware, no address of this Xbox's own.
*/

#include "lwip/opt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "oxbox_kernel.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/platform_time.h"
#include "mbedtls/ssl.h"
#include "mbedtls/sha256.h"
#include "psa/crypto.h"
#include "mbedtls/psa_util.h"

/* ---------- the calls it makes into the game's image */

void platform_log(char const *format, ...);
int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);
int oxbox_lwip_address(unsigned long *address, int *dhcp);
DWORD NTAPI GetTickCount(void);
void NTAPI Sleep(DWORD milliseconds);
HANDLE NTAPI CreateThread(PVOID security, SIZE_T stack, PVOID start, PVOID parameter, DWORD flags, DWORD *id);
int NTAPI CloseHandle(HANDLE handle);
/* (oxbox_sdk.c: the answer) */
extern volatile long oxbox_update_found_release, oxbox_update_found_network;

/* ---------- constants */

enum
{
	UPDATE_STACK = 32 * 1024,          /* (Mbed TLS's handshake: its big numbers are on the stack) */
	UPDATE_TIMEOUT_MS = 15000,         /* any one step: looking up, connecting, the handshake, a reply */
	UPDATE_MOST_REDIRECTS = 6,
	UPDATE_HEADER = 8192,              /* a reply's headers, at most */
	UPDATE_WANT = 128                  /* the package's header */
};

/* ---------- globals (all in OXUPD) */

static struct
{
	HANDLE thread;
	volatile long stop, running;
	char url[2048];                    /* (GitHub's file host's addresses are about 900 characters) */
	char reply[UPDATE_HEADER + 1];
	unsigned char body[UPDATE_WANT];
	int psa_ready;
} update;

struct update_connection
{
	int socket;
	int tls;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config config;
};

/* ---------- what Mbed TLS asks of the platform (installer/tls_config.h) */

mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)GetTickCount();
}

/* randomness: the processor's cycle counter read around work whose time varies, hashed (as the installer's: nothing
fetched is secret, so the handshake's keys only have to be unpredictable enough) */
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
			samples[i] = __builtin_readcyclecounter() - start + (start << 20) + GetTickCount() + ++counter;
		}
		mbedtls_sha256((unsigned char const *)samples, sizeof(samples), digest, 0);
		for (i = 0; i < 32 && done < len; i++)
			output[done++] = digest[i];
	}
	*olen = len;
	return 0;
}

/* (the C library calls Mbed TLS makes that the game's doesn't have) */
void explicit_bzero(void *pointer, size_t size)
{
	volatile unsigned char *p = (volatile unsigned char *)pointer;

	while (size--)
		*p++ = 0;
}

int getpid(void)
{
	return 1;
}

/* (a host name that's an address: not one this check meets) */
int inet_pton(int family, char const *text, void *address)
{
	(void)family;
	(void)text;
	(void)address;
	return 0;
}

/* ---------- the connection */

/* waits until the socket can be read (or written), a quarter of a second at a time, giving up when told to: 1 when it
can, else 0 */
static int update_wait(int s, int writing, unsigned long ms)
{
	unsigned long start = GetTickCount();

	while (!update.stop && GetTickCount() - start < ms)
	{
		fd_set set;
		struct timeval timeout;

		FD_ZERO(&set);
		FD_SET(s, &set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 250000;
		if (lwip_select(s + 1, writing ? NULL : &set, writing ? &set : NULL, NULL, &timeout) > 0)
			return 1;
	}
	return 0;
}

static int update_send(void *context, unsigned char const *data, size_t size)
{
	int s = ((struct update_connection *)context)->socket, n;

	if (!update_wait(s, 1, UPDATE_TIMEOUT_MS))
		return MBEDTLS_ERR_SSL_TIMEOUT;
	n = lwip_send(s, data, size, 0);
	return n < 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : n;
}

static int update_receive(void *context, unsigned char *data, size_t size, uint32_t timeout)
{
	int s = ((struct update_connection *)context)->socket, n;

	if (!update_wait(s, 0, timeout ? timeout : UPDATE_TIMEOUT_MS))
		return MBEDTLS_ERR_SSL_TIMEOUT;
	n = lwip_recv(s, data, size, 0);
	return n < 0 ? MBEDTLS_ERR_SSL_WANT_READ : n;
}

static void update_close(struct update_connection *c)
{
	if (c->tls)
	{
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->config);
		c->tls = 0;
	}
	if (c->socket >= 0)
		lwip_close(c->socket);
	c->socket = -1;
}

/* connected (TLS over it when tls): 0, else 1 (said in the log) */
static int update_open(struct update_connection *c, char const *host, unsigned short port, int tls)
{
	struct hostent *found;
	struct sockaddr_in to;
	unsigned long start;
	int r, flags;

	memset(c, 0, sizeof(*c));
	c->socket = -1;
	found = lwip_gethostbyname(host);
	if (!found || !found->h_addr_list || !found->h_addr_list[0])
	{
		platform_log("update: couldn't look up %s", host);
		return 1;
	}
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	memcpy(&to.sin_addr, found->h_addr_list[0], 4);
	c->socket = lwip_socket(AF_INET, SOCK_STREAM, 0);
	if (c->socket < 0)
	{
		platform_log("update: no socket");
		return 1;
	}
	flags = lwip_fcntl(c->socket, F_GETFL, 0);
	lwip_fcntl(c->socket, F_SETFL, flags | O_NONBLOCK);
	r = lwip_connect(c->socket, (struct sockaddr *)&to, sizeof(to));
	if (r && !update_wait(c->socket, 1, UPDATE_TIMEOUT_MS))
	{
		platform_log("update: couldn't connect to %s", host);
		update_close(c);
		return 1;
	}
	{
		int problem = 0;
		socklen_t problem_size = sizeof(problem);

		if (lwip_getsockopt(c->socket, SOL_SOCKET, SO_ERROR, &problem, &problem_size) || problem)
		{
			platform_log("update: couldn't connect to %s (%d)", host, problem);
			update_close(c);
			return 1;
		}
	}
	if (!tls)
		return 0;
	if (!update.psa_ready)
	{
		if (psa_crypto_init() != PSA_SUCCESS)
		{
			platform_log("update: the secure connection's randomness didn't start");
			update_close(c);
			return 1;
		}
		update.psa_ready = 1;
	}
	c->tls = 1;
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->config);
	if ((r = mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
		MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		goto failed;
	mbedtls_ssl_conf_authmode(&c->config, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&c->config, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
	mbedtls_ssl_conf_read_timeout(&c->config, UPDATE_TIMEOUT_MS);
	if ((r = mbedtls_ssl_setup(&c->ssl, &c->config)) != 0 || (r = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0)
		goto failed;
	mbedtls_ssl_set_bio(&c->ssl, c, update_send, NULL, update_receive);
	start = GetTickCount();
	while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0)
	{
		if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || update.stop ||
			GetTickCount() - start > UPDATE_TIMEOUT_MS)
			goto failed;
	}
	return 0;
failed:
	platform_log("update: the secure connection to %s failed (-0x%04x)", host, (unsigned)-r);
	update_close(c);
	return 1;
}

static int update_write(struct update_connection *c, char const *data, unsigned long size)
{
	unsigned long done = 0, start = GetTickCount();

	while (done < size)
	{
		int n = c->tls ? mbedtls_ssl_write(&c->ssl, (unsigned char const *)data + done, size - done) :
			update_send(c, (unsigned char const *)data + done, size - done);

		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
		{
			if (update.stop || GetTickCount() - start > UPDATE_TIMEOUT_MS)
				return 1;
			continue;
		}
		if (n <= 0)
			return 1;
		done += (unsigned long)n;
	}
	return 0;
}

/* some bytes: how many, 0 at the end, -1 if it went wrong */
static int update_read(struct update_connection *c, unsigned char *data, unsigned long size)
{
	unsigned long start = GetTickCount();

	for (;;)
	{
		int n = c->tls ? mbedtls_ssl_read(&c->ssl, data, size) : update_receive(c, data, size, UPDATE_TIMEOUT_MS);

		if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF || n == 0)
			return 0;
		if (n > 0)
			return n;
		if ((n != MBEDTLS_ERR_SSL_WANT_READ && n != MBEDTLS_ERR_SSL_WANT_WRITE &&
			n != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) || update.stop || GetTickCount() - start > UPDATE_TIMEOUT_MS)
			return -1;
	}
}

/* ---------- HTTP */

static int update_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* a header's value in the reply's headers (name with its colon: "location:"), or NULL */
static char *update_header(char *headers, char const *name)
{
	size_t n = strlen(name);
	char *line = headers;

	while (line && *line)
	{
		size_t i;

		for (i = 0; i < n && line[i] && update_lower(line[i]) == name[i]; i++)
			;
		if (i == n)
		{
			char *value = line + n, *end;

			while (*value == ' ' || *value == '\t')
				value++;
			for (end = value; *end && *end != '\r' && *end != '\n'; end++)
				;
			*end = 0;
			return value;
		}
		line = strchr(line, '\n');
		if (line)
			line++;
	}
	return NULL;
}

/* one request: 0 with the body's first bytes in update.body (*got of them), 2 with a redirect in update.url, else 1 */
static int update_get(int *got)
{
	struct update_connection c;
	char host[256], *path, *slash, request[2560], *end, *location;
	unsigned short port = 443;
	int tls, size = 0, n, status;
	char const *rest;

	*got = 0;
	if (!strncmp(update.url, "https://", 8))
		tls = 1, rest = update.url + 8;
	else if (!strncmp(update.url, "http://", 7))
		tls = 0, port = 80, rest = update.url + 7;
	else
		return 1;
	slash = strchr(rest, '/');
	n = slash ? (int)(slash - rest) : (int)strlen(rest);
	if (n <= 0 || n >= (int)sizeof(host))
		return 1;
	memcpy(host, rest, (size_t)n);
	host[n] = 0;
	path = slash ? slash : "/";
	{
		char *colon = strchr(host, ':');

		if (colon)
		{
			*colon = 0;
			port = (unsigned short)strtoul(colon + 1, NULL, 10);
		}
	}
	if (update_open(&c, host, port, tls))
		return 1;
	n = halo_linux_snprintf(request, sizeof(request),
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Halo1Unlimited\r\nRange: bytes=0-%d\r\nAccept: */*\r\n"
		"Connection: close\r\n\r\n", path, host, UPDATE_WANT - 1);
	if (n <= 0 || n >= (int)sizeof(request) || update_write(&c, request, (unsigned long)n))
	{
		platform_log("update: the request to %s didn't go", host);
		update_close(&c);
		return 1;
	}
	/* the headers, and what came after them */
	for (;;)
	{
		n = update_read(&c, (unsigned char *)update.reply + size, UPDATE_HEADER - size);
		if (n <= 0)
			break;
		size += n;
		update.reply[size] = 0;
		if (strstr(update.reply, "\r\n\r\n") || size >= UPDATE_HEADER)
			break;
	}
	update.reply[size] = 0;
	end = strstr(update.reply, "\r\n\r\n");
	if (!end || strncmp(update.reply, "HTTP/1.", 7) || size < 12)
	{
		platform_log("update: no reply from %s", host);
		update_close(&c);
		return 1;
	}
	status = (int)strtoul(update.reply + 9, NULL, 10);
	{
		int body_at = (int)(end + 4 - update.reply), have = size - body_at;

		*end = 0;
		if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
		{
			location = update_header(update.reply, "location:");
			update_close(&c);
			if (!location || strlen(location) >= sizeof(update.url))
				return 1;
			if (location[0] == '/')
			{
				char where[sizeof(update.url)];

				halo_linux_snprintf(where, sizeof(where), "%s://%s%s", tls ? "https" : "http", host, location);
				strcpy(update.url, where);
			}
			else
				strcpy(update.url, location);
			return 2;
		}
		if (status != 200 && status != 206)
		{
			platform_log("update: %s says %d", host, status);
			update_close(&c);
			return 1;
		}
		/* (a chunked reply: its first chunk's size line skipped) */
		{
			char *encoding = update_header(update.reply, "transfer-encoding:");

			if (encoding && update_lower(encoding[0]) == 'c')
			{
				char *line_end = have > 0 ? strstr(end + 4, "\r\n") : NULL;

				if (!line_end)
				{
					update_close(&c);
					return 1;
				}
				have -= (int)(line_end + 2 - (end + 4));
				body_at = (int)(line_end + 2 - update.reply);
			}
		}
		if (have > UPDATE_WANT)
			have = UPDATE_WANT;
		if (have > 0)
			memcpy(update.body, update.reply + body_at, (size_t)have);
		while (have < UPDATE_WANT)
		{
			n = update_read(&c, update.body + have, UPDATE_WANT - have);
			if (n <= 0)
				break;
			have += n;
		}
		*got = have;
	}
	update_close(&c);
	return 0;
}

static unsigned long update_u32(unsigned char const *p)
{
	return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

static DWORD NTAPI update_thread(PVOID parameter)
{
	int redirects, r = 1, got = 0;
	unsigned long start = GetTickCount();

	(void)parameter;
	for (redirects = 0; redirects <= UPDATE_MOST_REDIRECTS && !update.stop; redirects++)
	{
		r = update_get(&got);
		if (r != 2)
			break;
	}
	if (update.stop)
		platform_log("update: the check stopped (a map is loading)");
	else if (r)
		platform_log("update: the check didn't finish (%lu ms)", GetTickCount() - start);
	else if (got < UPDATE_WANT || memcmp(update.body, "OXPK", 4) || update_u32(update.body + 4) < 2)
		platform_log("update: what came isn't a package's header (%d bytes)", got);
	else
	{
		oxbox_update_found_network = (long)update_u32(update.body + 120);
		oxbox_update_found_release = (long)update_u32(update.body + 116);
		platform_log("update: the latest package is release %ld (network version %ld) (%lu ms)",
			oxbox_update_found_release, oxbox_update_found_network, GetTickCount() - start);
	}
	update.running = 0;
	return 0;
}

/* ---------- the game's calls (oxbox_sdk.c) */

/* the check started on its own thread: 1, else 0 (the network isn't up yet) */
int oxbox_update_check_start(char const *url)
{
	unsigned long address;
	int dhcp;

	if (update.running)
		return 1;
	if (!oxbox_lwip_address(&address, &dhcp))
		return 0;
	if (update.thread)
	{
		CloseHandle(update.thread);
		update.thread = NULL;
	}
	strncpy(update.url, url, sizeof(update.url) - 1);
	update.url[sizeof(update.url) - 1] = 0;
	update.stop = 0;
	update.running = 1;
	update.thread = CreateThread(NULL, UPDATE_STACK, (PVOID)update_thread, NULL, 0, NULL);
	if (!update.thread)
	{
		update.running = 0;
		return 0;
	}
	return 1;
}

/* whether the check's thread still runs */
int oxbox_update_check_running(void)
{
	return update.running != 0;
}

/* told to give up (a map is loading); oxbox_update_check_running says when it has */
void oxbox_update_check_stop(void)
{
	update.stop = 1;
}

/* the thread's handle let go and Mbed TLS's randomness freed, before the section is unloaded */
void oxbox_update_check_done(void)
{
	if (update.thread)
		CloseHandle(update.thread);
	update.thread = NULL;
	if (update.psa_ready)
		mbedtls_psa_crypto_free();
	update.psa_ready = 0;
}
