#include "net/net.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pit.h"
#include "fs/vfs.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/random.h"
#include "lib/string.h"
#include "sched/sched.h"

#define ETH_IP  0x0800
#define ETH_ARP 0x0806
#define ARP_SLOTS 16
#define MTU 1500

struct netif netif;
const struct nic_ops *nic;
static struct thread *net_thread;
static const uint8_t broadcast_mac[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

struct eth_hdr {
    uint8_t dst[6], src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ip_hdr {
    uint8_t vihl, tos;
    uint16_t len, id, frag;
    uint8_t ttl, proto;
    uint16_t csum;
    uint32_t src, dst;
} __attribute__((packed));

struct udp_hdr {
    uint16_t sport, dport, len, csum;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t type, code;
    uint16_t csum, id, seq;
} __attribute__((packed));

static struct {
    uint32_t ip;
    uint8_t mac[6];
    bool valid;
} arp_table[ARP_SLOTS];
static int arp_next;

bool net_is_thread(void)
{
    return thread_current() == net_thread;
}

uint16_t inet_checksum(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)(p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static int eth_send(const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    uint8_t frame[1514];
    if (len > MTU)
        return ZK_EINVAL;
    struct eth_hdr *e = (void *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, netif.mac, 6);
    e->type = htons(type);
    memcpy(frame + sizeof *e, payload, len);
    size_t total = sizeof *e + len;
    if (total < 60) {
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    netif.tx_packets++;
    netif.tx_bytes += total;
    return nic ? nic->send(frame, total) : ZK_ENODEV;
}

// arp

static bool arp_lookup(uint32_t ip, uint8_t *mac)
{
    for (int i = 0; i < ARP_SLOTS; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            memcpy(mac, arp_table[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void arp_store(uint32_t ip, const uint8_t *mac)
{
    uint64_t flags = irq_save();
    for (int i = 0; i < ARP_SLOTS; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            memcpy(arp_table[i].mac, mac, 6);
            irq_restore(flags);
            return;
        }
    }
    int i = arp_next++ % ARP_SLOTS;
    arp_table[i].ip = ip;
    memcpy(arp_table[i].mac, mac, 6);
    arp_table[i].valid = true;
    irq_restore(flags);
}

static void arp_send(uint16_t op, const uint8_t *tha, uint32_t tpa)
{
    struct arp_pkt a = {
        .htype = htons(1), .ptype = htons(ETH_IP), .hlen = 6, .plen = 4, .op = htons(op),
        .spa = htonl(netif.ip), .tpa = htonl(tpa),
    };
    memcpy(a.sha, netif.mac, 6);
    memcpy(a.tha, tha, 6);
    eth_send(op == 1 ? broadcast_mac : tha, ETH_ARP, &a, sizeof a);
}

static void arp_input(const uint8_t *p, size_t len)
{
    if (len < sizeof(struct arp_pkt))
        return;
    const struct arp_pkt *a = (const void *)p;
    if (ntohs(a->ptype) != ETH_IP || a->hlen != 6 || a->plen != 4)
        return;
    uint32_t spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    if (spa)
        arp_store(spa, a->sha);
    if (ntohs(a->op) == 1 && netif.ip && tpa == netif.ip)
        arp_send(2, a->sha, spa);
}

// waits for a reply, unless this is the net thread which has to keep going
static bool arp_resolve(uint32_t ip, uint8_t *mac)
{
    if (arp_lookup(ip, mac))
        return true;
    for (int tries = 0; tries < 3; tries++) {
        arp_send(1, (const uint8_t[6]){ 0 }, ip);
        if (net_is_thread() || !sched_running())
            return false;
        for (int i = 0; i < 20; i++) {
            thread_sleep_ms(10);
            if (arp_lookup(ip, mac))
                return true;
        }
    }
    return false;
}

// ip

uint32_t ip_source_for(uint32_t dst)
{
    (void)dst;
    return netif.ip;
}

int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    static uint16_t ident;
    uint8_t pkt[MTU];
    if (len + sizeof(struct ip_hdr) > MTU)
        return ZK_EINVAL;
    struct ip_hdr *h = (void *)pkt;
    h->vihl = 0x45;
    h->tos = 0;
    h->len = htons((uint16_t)(sizeof *h + len));
    h->id = htons(ident++);
    h->frag = htons(0x4000);    // don't fragment
    h->ttl = 64;
    h->proto = proto;
    h->csum = 0;
    h->src = htonl(netif.ip);
    h->dst = htonl(dst);
    h->csum = inet_checksum(h, sizeof *h, 0);
    memcpy(pkt + sizeof *h, payload, len);

    uint8_t mac[6];
    if (dst == 0xffffffff) {
        memcpy(mac, broadcast_mac, 6);
    } else {
        if (!netif.up)
            return ZK_ENETUNREACH;
        uint32_t hop = (dst & netif.mask) == (netif.ip & netif.mask) ? dst : netif.gw;
        if (!hop)
            return ZK_ENETUNREACH;
        if (!arp_resolve(hop, mac))
            return ZK_EHOSTUNREACH;
    }
    return eth_send(mac, ETH_IP, pkt, sizeof *h + len);
}

// icmp

static volatile uint16_t ping_seq_seen;
static struct waitq ping_wq = WAITQ_INIT;

static void icmp_input(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < sizeof(struct icmp_hdr) || inet_checksum(p, len, 0) != 0)
        return;
    const struct icmp_hdr *h = (const void *)p;
    if (h->type == 8) {    // echo request
        uint8_t reply[MTU];
        if (len > sizeof reply - 20)
            return;
        memcpy(reply, p, len);
        struct icmp_hdr *r = (void *)reply;
        r->type = 0;
        r->csum = 0;
        r->csum = inet_checksum(reply, len, 0);
        ip_send(src, IP_ICMP, reply, len);
    } else if (h->type == 0 && ntohs(h->id) == 0x5a4f) {
        ping_seq_seen = ntohs(h->seq);
        waitq_wake_all(&ping_wq);
    }
}

int64_t net_ping(uint32_t ip, uint64_t timeout_ms)
{
    static uint16_t seq;
    uint8_t pkt[sizeof(struct icmp_hdr) + 32];
    struct icmp_hdr *h = (void *)pkt;
    h->type = 8;
    h->code = 0;
    h->id = htons(0x5a4f);
    h->seq = htons(++seq);
    for (size_t i = sizeof *h; i < sizeof pkt; i++)
        pkt[i] = (uint8_t)i;
    h->csum = 0;
    h->csum = inet_checksum(pkt, sizeof pkt, 0);
    uint16_t want = seq;
    uint64_t start = timer_uptime_ms();
    int err = ip_send(ip, IP_ICMP, pkt, sizeof pkt);
    if (err)
        return err;
    uint64_t flags = irq_save();
    while (ping_seq_seen != want) {
        uint64_t now = timer_uptime_ms();
        if (now - start >= timeout_ms) {
            irq_restore(flags);
            return ZK_ETIMEDOUT;
        }
        waitq_sleep_timeout(&ping_wq, timeout_ms - (now - start));
    }
    irq_restore(flags);
    return (int64_t)(timer_uptime_ms() - start);
}

// udp

int udp_send(uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len)
{
    uint8_t pkt[MTU];
    if (len + sizeof(struct udp_hdr) + sizeof(struct ip_hdr) > MTU)
        return ZK_EINVAL;
    struct udp_hdr *u = (void *)pkt;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons((uint16_t)(sizeof *u + len));
    u->csum = 0;    // optional over ipv4
    memcpy(pkt + sizeof *u, data, len);
    return ip_send(dst, IP_UDP, pkt, sizeof *u + len);
}

static void dhcp_input(const uint8_t *p, size_t len);

void udp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t len)
{
    (void)dst;
    if (len < sizeof(struct udp_hdr))
        return;
    const struct udp_hdr *u = (const void *)p;
    size_t ulen = ntohs(u->len);
    if (ulen < sizeof *u || ulen > len)
        return;
    uint16_t dport = ntohs(u->dport);
    if (dport == 68) {
        dhcp_input(p + sizeof *u, ulen - sizeof *u);
        return;
    }
    sock_udp_deliver(dport, src, ntohs(u->sport), p + sizeof *u, ulen - sizeof *u);
}

void net_input(const uint8_t *frame, size_t len)
{
    if (len < sizeof(struct eth_hdr))
        return;
    netif.rx_packets++;
    netif.rx_bytes += len;
    const struct eth_hdr *e = (const void *)frame;
    const uint8_t *p = frame + sizeof *e;
    len -= sizeof *e;
    uint16_t type = ntohs(e->type);
    if (type == ETH_ARP) {
        arp_input(p, len);
        return;
    }
    if (type != ETH_IP || len < sizeof(struct ip_hdr))
        return;
    const struct ip_hdr *h = (const void *)p;
    size_t hl = (size_t)(h->vihl & 15) * 4;
    size_t total = ntohs(h->len);
    if ((h->vihl >> 4) != 4 || hl < 20 || total > len || total < hl)
        return;
    if (inet_checksum(h, hl, 0) != 0)
        return;
    if (ntohs(h->frag) & 0x3fff)
        return;    // fragments aren't reassembled
    uint32_t src = ntohl(h->src), dst = ntohl(h->dst);
    if (netif.ip && dst != netif.ip && dst != 0xffffffff && (dst | netif.mask) != 0xffffffff)
        return;
    switch (h->proto) {
    case IP_ICMP: icmp_input(src, p + hl, total - hl); break;
    case IP_UDP:  udp_input(src, dst, p + hl, total - hl); break;
    case IP_TCP:  tcp_input(src, dst, p + hl, total - hl); break;
    }
}

// dhcp

struct dhcp_msg {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64], file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

static uint32_t dhcp_xid;
static volatile int dhcp_state;    // 0 waiting for offer, 1 for ack, 2 done
static uint32_t offer_ip, offer_server, offer_mask, offer_gw, offer_dns;
static struct waitq dhcp_wq = WAITQ_INIT;

static void dhcp_send(int type)
{
    struct dhcp_msg m;
    memset(&m, 0, sizeof m);
    m.op = 1;
    m.htype = 1;
    m.hlen = 6;
    m.xid = dhcp_xid;
    m.flags = htons(0x8000);    // replies broadcast, we have no address yet
    memcpy(m.chaddr, netif.mac, 6);
    m.magic = htonl(0x63825363);
    uint8_t *o = m.options;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    if (type == 3) {
        *o++ = 50; *o++ = 4;
        uint32_t ip = htonl(offer_ip);
        memcpy(o, &ip, 4);
        o += 4;
        *o++ = 54; *o++ = 4;
        uint32_t sv = htonl(offer_server);
        memcpy(o, &sv, 4);
        o += 4;
    }
    *o++ = 255;
    udp_send(68, 0xffffffff, 67, &m, (size_t)(o - (uint8_t *)&m));
}

static void dhcp_input(const uint8_t *p, size_t len)
{
    if (len < offsetof(struct dhcp_msg, options) || dhcp_state == 2)
        return;
    const struct dhcp_msg *m = (const void *)p;
    if (m->op != 2 || m->xid != dhcp_xid || ntohl(m->magic) != 0x63825363)
        return;
    int type = 0;
    uint32_t mask = 0, gw = 0, dns = 0, server = 0;
    const uint8_t *o = m->options, *end = p + len;
    while (o < end && *o != 255) {
        if (*o == 0) {
            o++;
            continue;
        }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        uint8_t code = o[0], olen = o[1];
        const uint8_t *v = o + 2;
        uint32_t word = olen >= 4 ? ntohl(*(const uint32_t *)v) : 0;
        if (code == 53 && olen == 1) type = v[0];
        else if (code == 1) mask = word;
        else if (code == 3) gw = word;
        else if (code == 6) dns = word;
        else if (code == 54) server = word;
        o += 2 + olen;
    }
    if (type == 2 && dhcp_state == 0) {
        offer_ip = ntohl(m->yiaddr);
        offer_server = server;
        offer_mask = mask;
        offer_gw = gw;
        offer_dns = dns;
        dhcp_state = 1;
        waitq_wake_all(&dhcp_wq);
    } else if (type == 5 && dhcp_state == 1) {
        if (mask) offer_mask = mask;
        if (gw) offer_gw = gw;
        if (dns) offer_dns = dns;
        dhcp_state = 2;
        waitq_wake_all(&dhcp_wq);
    }
}

static bool dhcp_wait(int state, uint64_t ms)
{
    uint64_t end = timer_uptime_ms() + ms;
    uint64_t flags = irq_save();
    while (dhcp_state < state && timer_uptime_ms() < end)
        waitq_sleep_timeout(&dhcp_wq, end - timer_uptime_ms());
    irq_restore(flags);
    return dhcp_state >= state;
}

static void write_resolv_conf(void)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "nameserver %u.%u.%u.%u\n", netif.dns >> 24,
                     (netif.dns >> 16) & 255, (netif.dns >> 8) & 255, netif.dns & 255);
    struct file *f;
    if (vfs_open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) {
        file_write(f, buf, (size_t)n);
        file_put(f);
    }
}

static int prefix_len(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000) {
        n++;
        mask <<= 1;
    }
    return n;
}

static void configure(void *arg)
{
    (void)arg;
    random_bytes(&dhcp_xid, sizeof dhcp_xid);
    bool ok = false;
    for (int tries = 0; tries < 3 && !ok; tries++) {
        dhcp_state = 0;
        dhcp_send(1);
        if (!dhcp_wait(1, 2000))
            continue;
        dhcp_send(3);
        ok = dhcp_wait(2, 2000);
    }
    if (ok) {
        netif.ip = offer_ip;
        netif.mask = offer_mask ? offer_mask : 0xffffff00;
        netif.gw = offer_gw;
        netif.dns = offer_dns ? offer_dns : offer_gw;
    } else {
        // what qemu's user networking hands out anyway
        netif.ip = 0x0a00020f;
        netif.mask = 0xffffff00;
        netif.gw = 0x0a000202;
        netif.dns = 0x0a000203;
    }
    netif.up = true;
    write_resolv_conf();
    pr_ok("net: %u.%u.%u.%u/%d via %u.%u.%u.%u%s\n", netif.ip >> 24, (netif.ip >> 16) & 255,
          (netif.ip >> 8) & 255, netif.ip & 255, prefix_len(netif.mask),
          netif.gw >> 24, (netif.gw >> 16) & 255, (netif.gw >> 8) & 255, netif.gw & 255,
          ok ? " (dhcp)" : " (no dhcp answer, guessed)");
}

static void net_loop(void *arg)
{
    (void)arg;
    for (;;) {
        nic->poll();
        tcp_timer();
        thread_sleep_ms(1);
    }
}

void net_init(void)
{
    if (!virtio_net_init() && !e1000_init()) {
        pr_warn("net: no network card this kernel knows\n");
        return;
    }
    pr_ok("net: %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n", nic->name, netif.mac[0], netif.mac[1],
          netif.mac[2], netif.mac[3], netif.mac[4], netif.mac[5]);
    net_thread = thread_create("net", net_loop, NULL, false);
    thread_create("dhcp", configure, NULL, false);
}
