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

It also asks OpenCE's master servers for their lists of dedicated servers (the online games list's; oxbox_sdk.c
oxbox_masters_frame: while the list is open, every 30 seconds), which list there and not with the brokers since
OpenCE's network version 39 (OpenCE's docs/master_protocol.md, "The list"): GET /v1/servers over HTTPS (the masters
send plain HTTP to HTTPS), each list taken only if its Ed25519 signature checks with the master's key pinned here, its
key id is that key's, and (when the Xbox's clock is about right) it's at most 5 minutes old; its servers' own signed
listings go to the games list as the brokers' do (p2p_lobby.c p2p_lobby_master_heard, which checks each again). The
connection's certificate isn't checked: the list's signature is what's trusted.
*/

#include "lwip/opt.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "oxbox_kernel.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

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
extern volatile long oxbox_update_found_release, oxbox_update_found_network, oxbox_update_found_build;
/* (p2p_crypto.c, p2p.c, p2p_lobby.c: the masters' lists) */
void p2p_sha256(const void *data, int size, unsigned char *digest);
void p2p_hex(const unsigned char *bytes, int size, char *text);
int p2p_ed25519_verify(const unsigned char *public_key, const void *message, int size, const unsigned char *signature);
int p2p_lobby_master_heard(const unsigned char *payload, int size);

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
	volatile long *stop;               /* (its thread's: update.stop, or masters.stop) */
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
static int update_wait(struct update_connection const *c, int writing, unsigned long ms)
{
	unsigned long start = GetTickCount();
	int s = c->socket;

	while (!*c->stop && GetTickCount() - start < ms)
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
	struct update_connection *c = (struct update_connection *)context;
	int s = c->socket, n;

	if (!update_wait(c, 1, UPDATE_TIMEOUT_MS))
		return MBEDTLS_ERR_SSL_TIMEOUT;
	n = lwip_send(s, data, size, 0);
	return n < 0 ? MBEDTLS_ERR_SSL_WANT_WRITE : n;
}

static int update_receive(void *context, unsigned char *data, size_t size, uint32_t timeout)
{
	struct update_connection *c = (struct update_connection *)context;
	int s = c->socket, n;

	if (!update_wait(c, 0, timeout ? timeout : UPDATE_TIMEOUT_MS))
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

/* connected (TLS over it when tls): 0, else 1 (said in the log); stop: its thread's word to give up */
static int update_open(struct update_connection *c, char const *host, unsigned short port, int tls,
	volatile long *stop)
{
	struct hostent *found;
	struct sockaddr_in to;
	unsigned long start;
	int r, flags;

	memset(c, 0, sizeof(*c));
	c->socket = -1;
	c->stop = stop;
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
	if (r && !update_wait(c, 1, UPDATE_TIMEOUT_MS))
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
		if ((r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) || *c->stop ||
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
			if (*c->stop || GetTickCount() - start > UPDATE_TIMEOUT_MS)
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
			n != MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) || *c->stop || GetTickCount() - start > UPDATE_TIMEOUT_MS)
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
	if (update_open(&c, host, port, tls, &update.stop))
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
		oxbox_update_found_build = (long)update_u32(update.body + 124);
		platform_log("update: the latest package is release %ld build %ld (network version %ld) (%lu ms)",
			oxbox_update_found_release, oxbox_update_found_build, oxbox_update_found_network, GetTickCount() - start);
	}
	update.running = 0;
	return 0;
}

/* ---------- the master servers' lists */

enum
{
	MASTERS_MOST_BODY = 256 * 1024,    /* (a list of 512 servers, the most a master gives, is some 350 KB: 45 are 25 KB) */
	MASTERS_FRESH = 300,               /* seconds: OpenCE's FRESH_SECONDS */
	MASTERS_CLOCK_WRONG = 86400,       /* an Xbox clock further off than this from a list's time isn't trusted */
	MASTERS_LISTING_MOST = 300         /* bytes: a listing is 140 to 268 */
};

/* OpenCE's official masters, as master_list.c's master_list_read_defaults has them */
static struct
{
	char const *host;
	char const *key;                   /* (its Ed25519 public key, hex) */
} const master_servers[] =
{
	{ "master.opencommunityedition.com", "89f4eda71229f11f3538679f49ef7fb0c2b407b380093f7f4fe02105b92225aa" },
	{ "ocemaster.halobase.net", "1d018c4d1e182585948cd807f19133122c1a1484a74dcffa7792d18012573459" },
	{ "masterce.unicade.io", "22aa04b7cf041af39c2e9765b5da10989254ed49fa23b730cd90e6d36a0a411a" },
};

static struct
{
	HANDLE thread;
	volatile long stop, running;
	int network_version;
	char reply[UPDATE_HEADER + 1];
	char headers[UPDATE_HEADER + 1];
} masters;

static int masters_hex_digit(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* base64 (RFC 4648, with = padding) up to its end or a '"': the bytes' count, -1 if it isn't base64 or won't fit */
static int masters_base64(char const *text, unsigned char *out, int most)
{
	unsigned long bits = 0;
	int count = 0, have = 0;

	for (; *text && *text != '"' && *text != '\r' && *text != '\n'; text++)
	{
		int c = (unsigned char)*text, value;

		if (c == '=')
			break;
		value = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ?
			c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
		if (value < 0)
			return -1;
		bits = bits << 6 | (unsigned long)value;
		have += 6;
		if (have >= 8)
		{
			have -= 8;
			if (count >= most)
				return -1;
			out[count++] = (unsigned char)(bits >> have);
		}
	}
	return count;
}

/* a header of the reply's (name with its colon, lowercase), copied into value: 1, else 0 */
static int masters_header(char const *name, char *value, int size)
{
	char *found;

	strcpy(masters.headers, masters.reply);
	found = update_header(masters.headers, name);
	if (!found || (int)strlen(found) >= size)
		return 0;
	strcpy(value, found);
	return 1;
}

/* a number after "name": in the text from start to end (the body's master object), else -1 */
static long masters_number(char const *start, char const *end, char const *name)
{
	char const *at = strstr(start, name);

	if (!at || at >= end)
		return -1;
	at += strlen(name);
	while (*at == ' ')
		at++;
	return *at >= '0' && *at <= '9' ? (long)strtoul(at, NULL, 10) : -1;
}

/* one master's list asked for, checked, and its servers given to the games list */
static void masters_ask(int index)
{
	struct update_connection c;
	char const *host = master_servers[index].host;
	unsigned char key[32], digest[32], signature[64 + 4], listing[MASTERS_LISTING_MOST];
	char request[512], key_id[17], said_signature[160], said_key[40];
	char *body = NULL, *end, *object, *object_end, *at;
	unsigned long start = GetTickCount(), body_size = 0, body_room = 0;
	int size = 0, n, status, digit, listed = 0, taken = 0;
	long list_time;
	long now;

	for (digit = 0; digit < 32; digit++)
		key[digit] = (unsigned char)(masters_hex_digit(master_servers[index].key[2 * digit]) << 4 |
			masters_hex_digit(master_servers[index].key[2 * digit + 1]));
	p2p_sha256(key, 32, digest);
	p2p_hex(digest, 8, key_id);
	if (update_open(&c, host, 443, 1, &masters.stop))
		return;
	/* (HTTP/1.0: the whole body, then the connection closed: no chunks; no gzip asked for) */
	n = halo_linux_snprintf(request, sizeof(request),
		"GET /v1/servers?network_version=%d HTTP/1.0\r\nHost: %s\r\nUser-Agent: Halo1Unlimited\r\n"
		"Accept: application/json\r\n\r\n", masters.network_version, host);
	if (n <= 0 || n >= (int)sizeof(request) || update_write(&c, request, (unsigned long)n))
	{
		platform_log("masters: the request to %s didn't go", host);
		update_close(&c);
		return;
	}
	for (;;)
	{
		n = update_read(&c, (unsigned char *)masters.reply + size, UPDATE_HEADER - size);
		if (n <= 0)
			break;
		size += n;
		masters.reply[size] = 0;
		if (strstr(masters.reply, "\r\n\r\n") || size >= UPDATE_HEADER)
			break;
	}
	masters.reply[size] = 0;
	end = strstr(masters.reply, "\r\n\r\n");
	if (!end || strncmp(masters.reply, "HTTP/1.", 7) || size < 12)
	{
		platform_log("masters: no reply from %s", host);
		update_close(&c);
		return;
	}
	status = (int)strtoul(masters.reply + 9, NULL, 10);
	{
		int body_at = (int)(end + 4 - masters.reply), have = size - body_at;

		*end = 0;
		if (status != 200 || !masters_header("signature:", said_signature, sizeof(said_signature)) ||
			!masters_header("signature-key:", said_key, sizeof(said_key)))
		{
			platform_log("masters: %s says %d%s", host, status, status == 200 ? " with no signature" : "");
			update_close(&c);
			return;
		}
		body_room = 32 * 1024;
		body = (char *)malloc(body_room + 1);
		if (!body)
		{
			platform_log("masters: no memory for %s's list", host);
			update_close(&c);
			return;
		}
		if (have > 0)
		{
			memcpy(body, masters.reply + body_at, (size_t)have);
			body_size = (unsigned long)have;
		}
		for (;;)
		{
			if (body_size == body_room)
			{
				char *bigger;

				if (body_room >= MASTERS_MOST_BODY)
				{
					platform_log("masters: %s's list is over %d KB: not taken", host, MASTERS_MOST_BODY / 1024);
					free(body);
					update_close(&c);
					return;
				}
				bigger = (char *)realloc(body, body_room * 2 + 1);
				if (!bigger)
				{
					platform_log("masters: no memory for %s's list (%lu KB)", host, body_room * 2 / 1024);
					free(body);
					update_close(&c);
					return;
				}
				body = bigger;
				body_room *= 2;
			}
			n = update_read(&c, (unsigned char *)body + body_size, body_room - body_size);
			if (n < 0)
			{
				platform_log("masters: %s's list didn't all come", host);
				free(body);
				update_close(&c);
				return;
			}
			if (n == 0)
				break;
			body_size += (unsigned long)n;
		}
		body[body_size] = 0;
	}
	update_close(&c);
	/* the checks: the master's key id, its signature of the body, a list of protocol 1 by it, and fresh */
	if (strcmp(said_key, key_id) || strncmp(said_signature, "ed25519=", 8) ||
		masters_base64(said_signature + 8, signature, sizeof(signature)) != 64 ||
		!p2p_ed25519_verify(key, body, (int)body_size, signature))
	{
		platform_log("masters: %s's list isn't signed by its key (%s): not taken", host, said_key);
		free(body);
		return;
	}
	object = strstr(body, "\"master\":{");
	object_end = object ? strchr(object, '}') : NULL;
	if (!strstr(body, "\"protocol\":1") || !object || !object_end)
	{
		platform_log("masters: %s's answer isn't a list of protocol 1", host);
		free(body);
		return;
	}
	{
		char id_field[40];

		halo_linux_snprintf(id_field, sizeof(id_field), "\"key_id\":\"%s\"", key_id);
		at = strstr(object, id_field);
		list_time = masters_number(object, object_end, "\"time\":");
		if (!at || at > object_end || list_time < 0)
		{
			platform_log("masters: %s's list isn't its own", host);
			free(body);
			return;
		}
	}
	now = (long)time(NULL);
	if (now - list_time > MASTERS_FRESH || list_time - now > MASTERS_FRESH)
	{
		/* (an Xbox's clock that's far off, unset or wrong, can't say: taken; one about right says it's stale) */
		if (now - list_time < MASTERS_CLOCK_WRONG && list_time - now < MASTERS_CLOCK_WRONG)
		{
			platform_log("masters: %s's list is %ld seconds from this Xbox's clock: not taken", host, now - list_time);
			free(body);
			return;
		}
		platform_log("masters: this Xbox's clock is %ld seconds from %s's (its list taken all the same)",
			now - list_time, host);
	}
	/* each server's listing: the host-signed bytes the brokers would carry (base64) */
	for (at = body; (at = strstr(at, "\"listing\":\"")) != NULL; )
	{
		at += 11;
		n = masters_base64(at, listing, sizeof(listing));
		listed++;
		if (n > 0 && p2p_lobby_master_heard(listing, n))
			taken++;
		at = strchr(at, '"');
		if (!at)
			break;
	}
	platform_log("masters: %s lists %d servers, %d of them for this Xbox (%lu ms)", host, listed, taken,
		GetTickCount() - start);
	free(body);
}

static DWORD NTAPI masters_thread(PVOID parameter)
{
	int index;

	(void)parameter;
	for (index = 0; index < (int)(sizeof(master_servers) / sizeof(master_servers[0])) && !masters.stop; index++)
		masters_ask(index);
	masters.running = 0;
	return 0;
}

/* the masters asked on their own thread (network_version: the lists' version wanted): 1, else 0 */
int oxbox_masters_start(int network_version)
{
	unsigned long address;
	int dhcp;

	if (masters.running || update.running)
		return masters.running != 0;
	if (!oxbox_lwip_address(&address, &dhcp))
		return 0;
	if (masters.thread)
	{
		CloseHandle(masters.thread);
		masters.thread = NULL;
	}
	masters.network_version = network_version;
	masters.stop = 0;
	masters.running = 1;
	masters.thread = CreateThread(NULL, UPDATE_STACK, (PVOID)masters_thread, NULL, 0, NULL);
	if (!masters.thread)
	{
		masters.running = 0;
		return 0;
	}
	return 1;
}

int oxbox_masters_running(void)
{
	return masters.running != 0;
}

void oxbox_masters_stop(void)
{
	masters.stop = 1;
}

/* the thread's handle let go and Mbed TLS's randomness freed (the update check not running), before an unload */
void oxbox_masters_done(void)
{
	if (masters.thread)
		CloseHandle(masters.thread);
	masters.thread = NULL;
	if (update.psa_ready && !update.running)
		mbedtls_psa_crypto_free();
	if (!update.running)
		update.psa_ready = 0;
}

/* ---------- the game's calls (oxbox_sdk.c) */

/* the check started on its own thread: 1, else 0 (the network isn't up yet) */
int oxbox_update_check_start(char const *url)
{
	unsigned long address;
	int dhcp;

	if (update.running)
		return 1;
	if (masters.running || !oxbox_lwip_address(&address, &dhcp))
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
	if (update.psa_ready && !masters.running)
		mbedtls_psa_crypto_free();
	if (!masters.running)
		update.psa_ready = 0;
}
