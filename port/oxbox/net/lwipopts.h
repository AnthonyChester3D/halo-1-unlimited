/*
LWIPOPTS.H

lwIP's settings for the Xbox (port/third_party/lwip, port/third_party/nxdk_net): IPv4 only, DHCP with a link-local
fallback (a LAN with no router: system link), DNS, UDP, TCP and the BSD socket calls (lwip_socket ...), sized for a
console with 64 MB: buffers come from the kernel's pool as they're needed (MEMP_MEM_MALLOC), not reserved up front.
*/

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* threads: the stack's own (tcpip), the callers' through the socket calls */
#define NO_SYS 0
#define LWIP_TCPIP_CORE_LOCKING 1
#define LWIP_TCPIP_CORE_LOCKING_INPUT 0
#define SYS_LIGHTWEIGHT_PROT 1
#define LWIP_COMPAT_MUTEX_ALLOWED
#define TCPIP_THREAD_STACKSIZE 0x4000
#define TCPIP_THREAD_PRIO THREAD_PRIORITY_HIGHEST   /* (above the game's thread: sys_arch.c) */
#define TCPIP_MBOX_SIZE 64
#define DEFAULT_THREAD_STACKSIZE 0x4000
#if defined(OXBOX_NET_P2P)
#define DEFAULT_UDP_RECVMBOX_SIZE 64    /* (internet play's tunnel: every peer's packets) */
#else
#define DEFAULT_UDP_RECVMBOX_SIZE 32
#endif
#define DEFAULT_TCP_RECVMBOX_SIZE 32
#define DEFAULT_RAW_RECVMBOX_SIZE 8
#define DEFAULT_ACCEPTMBOX_SIZE 8

/* memory: the kernel's pool (sys_arch.c), as needed */
#define MEM_LIBC_MALLOC 0
#define MEM_CUSTOM_ALLOCATOR 1
#define MEM_CUSTOM_FREE nxdk_lwip_free
#define MEM_CUSTOM_MALLOC nxdk_lwip_malloc
#define MEM_CUSTOM_CALLOC nxdk_lwip_calloc
#define MEMP_MEM_MALLOC 1
#define MEM_ALIGNMENT 4
#define PBUF_POOL_SIZE 32           /* (also the card's send queue: nvnetdrv_lwip.c) */
#define RX_BUFF_CNT 64              /* the card's receive buffers, 2 KB each (nvnetdrv_lwip.c): xemu stops receiving for good once they run out (its card never asks for the queued frames again) */
#define OXBOX_RX_COPY 1             /* frames copied out of them at once (nvnetdrv_lwip.c) */
#if defined(OXBOX_NET_P2P)
/* (internet play's too: its tunnel, the brokers, and stand-ins for 15 peers' ports and connections: p2p.c) */
#define MEMP_NUM_NETCONN 128
#define MEMP_NUM_UDP_PCB 80
#define MEMP_NUM_TCP_PCB 80
#define MEMP_NUM_TCP_PCB_LISTEN 36
#else
#define MEMP_NUM_NETCONN 40         /* (a host of 16: a connection each, map sharing's, the datagram sockets) */
#define MEMP_NUM_UDP_PCB 8
#define MEMP_NUM_TCP_PCB 40
#define MEMP_NUM_TCP_PCB_LISTEN 4
#endif

/* the protocols */
#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define ETH_PAD_SIZE 0
#define IP_REASSEMBLY 1
#define IP_FRAG 1
#define IP_DEFAULT_TTL 64
#define LWIP_ICMP 1
#define LWIP_RAW 0
#define LWIP_IGMP 0
#define LWIP_UDP 1
#define LWIP_TCP 1
#define TCP_MSS 1460
#define TCP_WND (12 * TCP_MSS)      /* (map sharing's downloads; RX_BUFF_CNT holds a window) */
#define TCP_SND_BUF (12 * TCP_MSS)
#define LWIP_TCP_KEEPALIVE 1
#define LWIP_ALTCP 0

/* addresses: DHCP, then a link-local address (169.254.x.y) when no DHCP server answers; DNS servers from DHCP */
#define LWIP_DHCP 1
#define LWIP_AUTOIP 1
#define LWIP_DHCP_AUTOIP_COOP 1
#define LWIP_DHCP_AUTOIP_COOP_TRIES 3
#define LWIP_DNS 1
#define DNS_MAX_SERVERS 2
#define LWIP_SINGLE_NETIF 0
#define LWIP_HAVE_LOOPIF 1          /* 127.0.0.1: a host joins its own game through it (XNet had one) */
#define LWIP_NETIF_LOOPBACK 1       /* (and the console's own address) */
#define LWIP_LOOPBACK_MAX_PBUFS 64
#define LWIP_NETIF_HOSTNAME 1
#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
/* (consoles on this wire outside our subnet, sent to directly: oxbox_lwip.c) */
struct netif;
struct ip4_addr;
const struct ip4_addr *oxbox_lwip_onlink_gateway(struct netif *netif, const struct ip4_addr *destination);
#define LWIP_HOOK_ETHARP_GET_GW(netif, destination) oxbox_lwip_onlink_gateway((netif), (destination))

/* random numbers for DNS's query ids and source ports, DHCP's, and the first local ports: the internet play builds'
generator (oxbox_p2p_posix.c; the others have none, and lwIP counts) */
#if defined(OXBOX_NET_P2P)
unsigned int oxbox_lwip_random(void);
#define LWIP_RAND() oxbox_lwip_random()
#endif

/* the calls: netconn and BSD sockets under lwip_ names only (the game's own socket names are the XDK's) */
#define LWIP_NETCONN 1
#define LWIP_SOCKET 1
#define LWIP_COMPAT_SOCKETS 0
#define LWIP_POSIX_SOCKETS_IO_NAMES 0
#define LWIP_SOCKET_SELECT 1
#define LWIP_SOCKET_POLL 1
#define LWIP_SO_RCVTIMEO 1
#define LWIP_SO_SNDTIMEO 1
#define LWIP_SO_RCVBUF 1            /* (also FIONREAD) */
#define SO_REUSE 1
#define LWIP_TIMEVAL_PRIVATE 0      /* (the C library's struct timeval: arch/cc.h) */
#define LWIP_PROVIDE_ERRNO 1
/* errno, a thread's own (oxbox_lwip_ws.c): the game's thread and map sharing's both make socket calls */
int *oxbox_lwip_errno(void);
#define errno (*oxbox_lwip_errno())

/* no statistics or debug output (the test builds: statistics, for their log: oxbox_lwip.c) */
#if defined(OXBOX_NET_TEST)
#define LWIP_STATS 1
#define MEM_STATS 1
#define MEMP_STATS 0
#define SYS_STATS 0
#else
#define LWIP_STATS 0
#endif
#define LWIP_STATS_DISPLAY 0
#define LWIP_DEBUG 0

#endif
