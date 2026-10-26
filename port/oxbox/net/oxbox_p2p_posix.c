/*
OXBOX_P2P_POSIX.C

What internet play (port/linux/src/p2p.c, p2p_signal.c, p2p_lobby.c, p2p_crypto.c: OpenCE's, as our fork has it) asks
of the platform (port/linux/src/posix.h, and the pthread calls of p2p_internal.h), for the Xbox: lwIP's sockets
(oxbox_lwip.c), kernel semaphores and XAPI's threads, and random bytes for its keys. Only in the builds with internet
play (OXBOX_NET_P2P), which have our own network stack (OXBOX_NET_LWIP).

- Sockets are lwIP's descriptors as they are (internet play's own: its tunnel, the brokers, its stand-ins for peers),
  not the game's (SOCKET values: oxbox_lwip_ws.c, which tells internet play about them). Addresses are Winsock's
  (sockaddr_in with a 16-bit family), errors Winsock's codes.
- Random bytes: the keys a console makes each run (its identity, invites' tokens, the tunnel's nonces) must not be
  guessable. The Xbox has no random number instruction, so a pool (BLAKE2b) gathers timings that no one else sees:
  the processor's cycle counter around memory reads that the GPU's screen refresh and the interrupts slow at random,
  and the arrival times of the card's frames (oxbox_entropy_frame, from nvnetdrv_lwip.c), at the cycle. Each request
  is answered from ChaCha20 with a key made from the pool and the last key, then replaced (fast key erasure).
- UPnP (the router asked to forward the tunnel's port) is oxbox_upnp.c's. No invite links (no clipboard, no command
  line).
*/

#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "oxbox_kernel.h"
#include "monocypher.h"

#if defined(OXBOX_NET_P2P)

#include "p2p/pthread.h"   /* (internet play's view of these calls) */

int halo_linux_snprintf(char *buffer, size_t count, const char *format, ...);

void oxbox_lwip_start(void);
int oxbox_lwip_address(unsigned long *address, int *dhcp);
const unsigned char *oxbox_lwip_mac(void);
int oxbox_lwip_wsa_error(void);
void oxbox_lwip_set_errno(int code);

int NTAPI SwitchToThread(void);

#define THREAD_PRIORITY_ABOVE_NORMAL 1

/* ---------- the lock and the thread (p2p_internal.h's pthread calls) */

static PKSEMAPHORE mutex_semaphore(pthread_mutex_t *mutex)
{
	typedef char storage_fits[sizeof(mutex->storage) >= sizeof(KSEMAPHORE) ? 1 : -1];

	(void)sizeof(storage_fits);
	if (mutex->state != 2)
	{
		/* (made by whichever thread takes it first; another waits for that) */
		if (__sync_bool_compare_and_swap(&mutex->state, 0, 1))
		{
			KeInitializeSemaphore((PKSEMAPHORE)mutex->storage, 1, 1);
			__sync_synchronize();
			mutex->state = 2;
		}
		else
		{
			while (mutex->state != 2)
				Sleep(1);
		}
	}
	return (PKSEMAPHORE)mutex->storage;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	KeWaitForSingleObject(mutex_semaphore(mutex), Executive, KernelMode, FALSE, NULL);
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	KeReleaseSemaphore(mutex_semaphore(mutex), 0, 1, FALSE);
	return 0;
}

struct thread_start
{
	void *(*start)(void *);
	void *argument;
};

static DWORD NTAPI thread_trampoline(void *parameter)
{
	struct thread_start start = *(struct thread_start *)parameter;

	free(parameter);
	start.start(start.argument);
	return 0;
}

/* internet play's thread: above the game's (it forwards what peers send the game, and KCP's clock wants it every
10 ms), below lwIP's own */
int pthread_create(pthread_t *thread, const pthread_attr_t *attributes, void *(*start)(void *), void *argument)
{
	struct thread_start *parameter = malloc(sizeof(*parameter));
	HANDLE handle;

	(void)attributes;
	if (!parameter)
		return -1;
	parameter->start = start;
	parameter->argument = argument;
	handle = CreateThread(NULL, 0x10000, (PVOID)thread_trampoline, parameter, 0, NULL);
	if (!handle)
	{
		free(parameter);
		return -1;
	}
	SetThreadPriority(handle, THREAD_PRIORITY_ABOVE_NORMAL);
	*thread = (pthread_t)handle;
	return 0;
}

int pthread_detach(pthread_t thread)
{
	CloseHandle((HANDLE)thread);
	return 0;
}

/* ---------- sockets (posix.h's: Winsock's addresses and error codes) */

struct ws_sockaddr_in
{
	short sin_family;
	unsigned short sin_port;
	unsigned long sin_addr;
	char sin_zero[8];
};

#define WS_SOL_SOCKET 0xFFFF
#define WS_SO_REUSEADDR 0x0004
#define WS_SO_KEEPALIVE 0x0008
#define WS_SO_BROADCAST 0x0020
#define WS_SO_SNDBUF 0x1001
#define WS_SO_RCVBUF 0x1002
#define WS_SO_ERROR 0x1007
#define WS_SO_TYPE 0x1008
#define WS_IPPROTO_TCP 6
#define WS_TCP_NODELAY 0x0001

static int to_lwip(const void *name, int length, struct sockaddr_in *address)
{
	const struct ws_sockaddr_in *ws = (const struct ws_sockaddr_in *)name;

	if (!name || length < 8)
	{
		oxbox_lwip_set_errno(EFAULT);
		return 0;
	}
	memset(address, 0, sizeof(*address));
	address->sin_len = sizeof(*address);
	address->sin_family = AF_INET;
	address->sin_port = ws->sin_port;
	address->sin_addr.s_addr = ws->sin_addr;
	return 1;
}

static void to_ws(const struct sockaddr_in *address, void *name, int *length)
{
	struct ws_sockaddr_in ws;

	if (!name || !length)
		return;
	memset(&ws, 0, sizeof(ws));
	ws.sin_family = AF_INET;
	ws.sin_port = address->sin_port;
	ws.sin_addr = address->sin_addr.s_addr;
	memcpy(name, &ws, *length < (int)sizeof(ws) ? (size_t)*length : sizeof(ws));
	*length = sizeof(ws);
}

int posix_socket_last_error(void)
{
	return oxbox_lwip_wsa_error();
}

int posix_socket(int family, int type, int protocol)
{
	(void)family;
	(void)protocol;
	oxbox_lwip_start();
	return lwip_socket(AF_INET, type == 2 ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int posix_socket_close(int socket)
{
	return lwip_close(socket);
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_in local;

	if (!to_lwip(address, address_length, &local))
		return -1;
	return lwip_bind(socket, (struct sockaddr *)&local, sizeof(local));
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	struct sockaddr_in remote;

	if (!to_lwip(address, address_length, &remote))
		return -1;
	return lwip_connect(socket, (struct sockaddr *)&remote, sizeof(remote));
}

int posix_socket_listen(int socket, int backlog)
{
	return lwip_listen(socket, backlog);
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_in remote;
	socklen_t size = sizeof(remote);
	int accepted = lwip_accept(socket, (struct sockaddr *)&remote, &size);

	if (accepted >= 0)
		to_ws(&remote, address, address_length);
	return accepted;
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	(void)flags;
	return lwip_send(socket, buffer, (size_t)length, 0);
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address, int address_length)
{
	struct sockaddr_in remote;

	(void)flags;
	if (!to_lwip(address, address_length, &remote))
		return -1;
	return lwip_sendto(socket, buffer, (size_t)length, 0, (struct sockaddr *)&remote, sizeof(remote));
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	(void)flags;
	return lwip_recv(socket, buffer, (size_t)length, 0);
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length)
{
	struct sockaddr_in remote;
	socklen_t size = sizeof(remote);
	int got;

	(void)flags;
	memset(&remote, 0, sizeof(remote));
	got = lwip_recvfrom(socket, buffer, (size_t)length, 0, (struct sockaddr *)&remote, &size);
	if (got >= 0)
		to_ws(&remote, address, address_length);
	return got;
}

int posix_socket_shutdown(int socket, int how)
{
	return lwip_shutdown(socket, how);
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags = lwip_fcntl(socket, F_GETFL, 0);

	if (flags < 0)
		return -1;
	return lwip_fcntl(socket, F_SETFL, nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}

int posix_socket_bytes_available(int socket, unsigned long *count)
{
	int available = 0;
	int result = lwip_ioctl(socket, FIONREAD, &available);

	if (result >= 0 && count)
		*count = (unsigned long)available;
	return result;
}

int posix_socket_set_nodelay(int socket)
{
	int on = 1;

	return lwip_setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int number = value && length >= 4 ? *(const int *)value : (value && length >= 1 ? *(const unsigned char *)value : 0);

	if (level == WS_SOL_SOCKET)
	{
		switch (name)
		{
		case WS_SO_BROADCAST:
			return lwip_setsockopt(socket, SOL_SOCKET, SO_BROADCAST, &number, sizeof(number));
		case WS_SO_REUSEADDR:
			return lwip_setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &number, sizeof(number));
		case WS_SO_KEEPALIVE:
			return lwip_setsockopt(socket, SOL_SOCKET, SO_KEEPALIVE, &number, sizeof(number));
		case WS_SO_RCVBUF:
			return lwip_setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &number, sizeof(number));
		case WS_SO_SNDBUF:
			/* (no such limit in lwIP: TCP_SND_BUF, lwipopts.h) */
			return 0;
		}
	}
	else if (level == WS_IPPROTO_TCP && name == WS_TCP_NODELAY)
	{
		return lwip_setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &number, sizeof(number));
	}
	oxbox_lwip_set_errno(ENOPROTOOPT);
	return -1;
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int number = 0, result = 0;
	socklen_t size = sizeof(number);

	if (!value || !length || *length < 4)
	{
		oxbox_lwip_set_errno(EFAULT);
		return -1;
	}
	if (level == WS_SOL_SOCKET && name == WS_SO_TYPE)
	{
		result = lwip_getsockopt(socket, SOL_SOCKET, SO_TYPE, &number, &size);
		number = number == SOCK_DGRAM ? 2 : 1;
	}
	else if (level == WS_SOL_SOCKET && name == WS_SO_ERROR)
	{
		/* (lwIP's errno: whoever reads it maps it, as posix_socket_last_error does) */
		result = lwip_getsockopt(socket, SOL_SOCKET, SO_ERROR, &number, &size);
		if (result >= 0 && number)
		{
			oxbox_lwip_set_errno(number);
			number = oxbox_lwip_wsa_error();
		}
	}
	else
	{
		oxbox_lwip_set_errno(ENOPROTOOPT);
		return -1;
	}
	if (result < 0)
		return result;
	*(int *)value = number;
	*length = 4;
	return 0;
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_in local;
	socklen_t size = sizeof(local);
	int result = lwip_getsockname(socket, (struct sockaddr *)&local, &size);

	if (result >= 0)
		to_ws(&local, address, address_length);
	return result;
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_in remote;
	socklen_t size = sizeof(remote);
	int result = lwip_getpeername(socket, (struct sockaddr *)&remote, &size);

	if (result >= 0)
		to_ws(&remote, address, address_length);
	return result;
}

/* over lwIP's poll: each list rewritten to hold its ready descriptors, in the order given */
#define MAXIMUM_POLLED 192

int posix_socket_select(int *read, int *read_count, int *write, int *write_count, int *error, int *error_count,
	long timeout_seconds, long timeout_microseconds, int infinite)
{
	struct pollfd polled[MAXIMUM_POLLED];
	int count = 0, index, result, kept;
	int timeout = infinite ? -1 : (int)(timeout_seconds * 1000 + (timeout_microseconds + 999) / 1000);

	(void)error;
	for (index = 0; read && index < *read_count && count < MAXIMUM_POLLED; index++)
	{
		polled[count].fd = read[index];
		polled[count].events = POLLIN;
		polled[count++].revents = 0;
	}
	for (index = 0; write && index < *write_count; index++)
	{
		int known;

		for (known = 0; known < count && polled[known].fd != write[index]; known++)
			;
		if (known == count)
		{
			if (count == MAXIMUM_POLLED)
				continue;
			polled[count].fd = write[index];
			polled[count].events = 0;
			polled[count++].revents = 0;
		}
		polled[known].events |= POLLOUT;
	}
	if (error_count)
		*error_count = 0;
	if (!count)
	{
		if (timeout > 0)
			Sleep((DWORD)timeout);
		if (read_count)
			*read_count = 0;
		if (write_count)
			*write_count = 0;
		return 0;
	}
	result = lwip_poll(polled, (nfds_t)count, timeout);
	if (result < 0)
		return -1;
	/* (a descriptor's error or hang-up counts as ready to read or write: the call that follows says what it was) */
	for (index = 0, kept = 0; read && index < *read_count; index++)
	{
		int known;

		for (known = 0; known < count && polled[known].fd != read[index]; known++)
			;
		if (known < count && (polled[known].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)))
			read[kept++] = read[index];
	}
	if (read_count)
		*read_count = kept;
	for (index = 0, kept = 0; write && index < *write_count; index++)
	{
		int known;

		for (known = 0; known < count && polled[known].fd != write[index]; known++)
			;
		if (known < count && (polled[known].revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
			write[kept++] = write[index];
	}
	if (write_count)
		*write_count = kept;
	return (read_count ? *read_count : 0) + (write_count ? *write_count : 0);
}

unsigned long posix_local_ipv4_address(void)
{
	unsigned long address = 0;
	int dhcp;

	oxbox_lwip_address(&address, &dhcp);
	return address;
}

/* a name (through lwIP's DNS: netconn_gethostbyname waits on the stack's thread, safe from any thread) or a dotted
quad; 0 if it can't be found */
unsigned long posix_resolve_ipv4(const char *host)
{
	ip_addr_t address;
	unsigned long dotted = inet_addr(host);

	if (dotted != INADDR_NONE)
		return dotted;
	if (netconn_gethostbyname(host, &address) != ERR_OK)
		return 0;
	return ip4_addr_get_u32(ip_2_ip4(&address));
}

/* ---------- random bytes */

/* the card's frames' arrival times, at the cycle (nvnetdrv_lwip.c, from its DPC: a few instructions, no lock: a lost
update loses nothing but some of what it gathers) */
static volatile unsigned long long frame_entropy;
static volatile unsigned long frame_count;

static unsigned long long cycles(void)
{
	unsigned long low, high;

	__asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
	return (unsigned long long)high << 32 | low;
}

void oxbox_entropy_frame(void)
{
	unsigned long long now = cycles();

	frame_entropy = (frame_entropy << 7 | frame_entropy >> 57) ^ now;
	frame_count++;
}

static struct
{
	int seeded;
	unsigned char key[32];
	unsigned long long requests;
} generator;

static pthread_mutex_t generator_lock = PTHREAD_MUTEX_INITIALIZER;

/* the first time: 4096 timings of reads across memory the GPU shares (its screen refresh and the interrupts make each
take a little longer or shorter, by chance), with what else differs from console to console and boot to boot */
static void generator_seed(crypto_blake2b_ctx *pool)
{
	enum { TOUCHED_SIZE = 256 * 1024 };
	/* (larger than the processor's caches, so that the reads go to memory; the kernel's pages, given back at once) */
	unsigned char *touched = MmAllocateContiguousMemoryEx(TOUCHED_SIZE, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
	unsigned long position = 0;
	int index;

	for (index = 0; index < 4096; index++)
	{
		unsigned long long before = cycles(), after;
		int step;

		for (step = 0; touched && step < 16; step++)
		{
			position = (position * 1103515245u + 12345u + (unsigned long)before) & (TOUCHED_SIZE - 1);
			touched[position] ^= (unsigned char)before;
		}
		if ((index & 255) == 255)
			SwitchToThread();
		after = cycles();
		before = after - before;
		crypto_blake2b_update(pool, (const uint8_t *)&before, sizeof(before));
		crypto_blake2b_update(pool, (const uint8_t *)&after, sizeof(after));
	}
	if (touched)
	{
		crypto_blake2b_update(pool, touched, 4096);
		MmFreeContiguousMemory(touched);
	}
	{
		struct
		{
			unsigned long tick, time, address, stack;
			unsigned char mac[6];
		} facts;

		memset(&facts, 0, sizeof(facts));
		facts.tick = KeTickCount;
		facts.time = GetTickCount();
		oxbox_lwip_address(&facts.address, (int *)&facts.stack);
		facts.stack = (unsigned long)&facts;
		if (oxbox_lwip_mac())
			memcpy(facts.mac, oxbox_lwip_mac(), 6);
		crypto_blake2b_update(pool, (const uint8_t *)&facts, sizeof(facts));
	}
}

void posix_random_bytes(void *buffer, unsigned long size)
{
	static const uint8_t nonce[8];
	unsigned char *out = buffer;
	crypto_blake2b_ctx pool;

	pthread_mutex_lock(&generator_lock);
	/* the next key: the last one, the frames' timings since, this moment and the request's number */
	crypto_blake2b_init(&pool, sizeof(generator.key));
	if (!generator.seeded)
	{
		generator_seed(&pool);
		generator.seeded = 1;
	}
	{
		struct
		{
			unsigned long long frames, now, requests;
			unsigned long frame_count;
		} fresh;

		fresh.frames = frame_entropy;
		fresh.frame_count = frame_count;
		fresh.now = cycles();
		fresh.requests = ++generator.requests;
		crypto_blake2b_update(&pool, generator.key, sizeof(generator.key));
		crypto_blake2b_update(&pool, (const uint8_t *)&fresh, sizeof(fresh));
	}
	crypto_blake2b_final(&pool, generator.key);
	/* the answer from ChaCha20 under it, at block 1 on; block 0 is the key after it (the one used is gone) */
	while (size)
	{
		unsigned char block[64];
		unsigned long take = size < 32 ? size : 32;

		crypto_chacha20_djb(block, NULL, sizeof(block), generator.key, nonce, 0);
		memcpy(out, block + 32, take);
		memcpy(generator.key, block, 32);
		crypto_wipe(block, sizeof(block));
		out += take;
		size -= take;
	}
	pthread_mutex_unlock(&generator_lock);
}

/* lwIP's LWIP_RAND (lwipopts.h) */
unsigned int oxbox_lwip_random(void)
{
	unsigned int value;

	posix_random_bytes(&value, sizeof(value));
	return value;
}

/* ---------- what the Xbox doesn't have */

/* (UPnP: oxbox_upnp.c) */

int posix_command_line_argument(int index, char *buffer, unsigned long size)
{
	(void)index;
	if (size)
		buffer[0] = 0;
	return 0;
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	(void)scheme;
	(void)description;
	return 0;
}

/* (no secret of a user's to hand invites between copies of the game: there is one copy) */
int posix_user_secret(unsigned char *secret, int size)
{
	(void)secret;
	(void)size;
	return 0;
}

#endif
