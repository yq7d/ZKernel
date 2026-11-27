#pragma once
// ipv4 networking: one interface, ethernet, arp, icmp, udp, tcp, dhcp.
// addresses and ports are kept in host byte order, the wire code swaps them
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>

struct vnode;

struct netif {
    uint8_t mac[6];
    uint32_t ip, mask, gw, dns;
    bool up;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};

extern struct netif netif;

static inline uint16_t htons(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
static inline uint32_t htonl(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
#define ntohs htons
#define ntohl htonl

#define IP_ICMP 1
#define IP_TCP  6
#define IP_UDP  17

void net_init(void);
bool net_is_thread(void);
void net_input(const uint8_t *frame, size_t len);
uint16_t inet_checksum(const void *data, size_t len, uint32_t sum);
// payload is the ip payload, the header is added here
int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);
uint32_t ip_source_for(uint32_t dst);
// round trip in ms, or an error
int64_t net_ping(uint32_t ip, uint64_t timeout_ms);

int udp_send(uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len);
void udp_input(uint32_t src, uint32_t dst, const uint8_t *pkt, size_t len);

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_timer(void);

// socket.c
void sock_udp_deliver(uint16_t port, uint32_t src, uint16_t sport, const uint8_t *data, size_t len);
uint16_t sock_ephemeral_port(void);

// the network card in use
struct nic_ops {
    const char *name;
    int (*send)(const void *frame, size_t len);
    int (*poll)(void);
};
extern const struct nic_ops *nic;

// each sets nic and netif.mac when it finds its card
bool virtio_net_init(void);
bool e1000_init(void);

// socket syscalls coming from linux programs, by linux syscall number
int64_t linux_socketcall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);
// the zos ones, from proc/syscall.c
int64_t sys_socket(int domain, int type);
int64_t sys_connect(int fd, uint64_t uaddr, uint32_t len);
int64_t sys_bind(int fd, uint64_t uaddr, uint32_t len);
int64_t sys_sendto(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint32_t alen);
int64_t sys_recvfrom(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint64_t ualen);
int64_t sys_getsockname(int fd, uint64_t uaddr, uint64_t ualen, bool peer);
int64_t sys_shutdown(int fd, int how);
// set from fcntl
void sock_set_nonblock(struct vnode *v, bool on);
