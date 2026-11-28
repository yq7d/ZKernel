// tcp, the client side: connect, send and receive with retransmission and
// flow control, window scaling, orderly close. out of order segments are
// kept in the receive ring until the hole before them is filled
#include "net/tcp.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pit.h"
#include "fs/vfs.h"
#include "lib/random.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "sched/sched.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define MSS       1400
#define BUF_SIZE  (64 * 1024)     // send side
#define RX_SIZE   (256 * 1024)
#define RX_SHIFT  3               // window scale we ask for
#define MAX_HOLES 8
#define RTO_MIN   400
#define RTO_MAX   8000
#define MAX_TRIES 9
#define LINGER_MS 30000

enum { CLOSED, SYN_SENT, ESTABLISHED };

struct tcb {
    int state;
    uint32_t lip, rip;
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint32_t rcv_nxt;
    uint8_t *tx, *rx;
    size_t tx_head, tx_len;    // tx holds data from snd_una on
    size_t rx_head, rx_len;
    uint8_t rcv_shift, snd_shift;     // 0 unless both sides agreed
    uint32_t adv_edge;                // right edge of the window last advertised
    struct { uint32_t start, end; } ooo[MAX_HOLES];    // out of order data past rcv_nxt
    int nooo;
    bool closing, fin_sent, fin_acked, fin_rcvd;
    bool released;             // the socket is gone, only the protocol is left
    int err;
    uint64_t rto, deadline, closed_at;
    int tries;
    struct waitq wq;
    struct tcb *next;
};

struct tcp_hdr {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off, flags;
    uint16_t win, csum, urg;
} __attribute__((packed));

static struct tcb *conns;

static bool seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static bool seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static uint16_t window(struct tcb *t)
{
    size_t space = (RX_SIZE - t->rx_len) >> t->rcv_shift;
    return (uint16_t)(space > 65535 ? 65535 : space);
}

static void wake(struct tcb *t)
{
    waitq_wake_all(&t->wq);
    poll_notify();
}

static int send_segment(struct tcb *t, uint32_t seq, uint8_t flags, const uint8_t *data, size_t len)
{
    uint8_t pkt[sizeof(struct tcp_hdr) + 8 + MSS];
    struct tcp_hdr *h = (void *)pkt;
    size_t hl = sizeof *h + ((flags & F_SYN) ? 8 : 0);
    h->sport = htons(t->lport);
    h->dport = htons(t->rport);
    h->seq = htonl(seq);
    h->ack = (flags & F_ACK) ? htonl(t->rcv_nxt) : 0;
    h->off = (uint8_t)((hl / 4) << 4);
    h->flags = flags;
    if (flags & F_SYN) {
        size_t space = RX_SIZE - t->rx_len;
        h->win = htons((uint16_t)(space > 65535 ? 65535 : space));
    } else {
        h->win = htons(window(t));
        t->adv_edge = t->rcv_nxt + ((uint32_t)window(t) << t->rcv_shift);
    }
    h->csum = 0;
    h->urg = 0;
    if (flags & F_SYN) {
        pkt[20] = 2;    // maximum segment size
        pkt[21] = 4;
        pkt[22] = MSS >> 8;
        pkt[23] = MSS & 255;
        pkt[24] = 1;    // nop, then window scale
        pkt[25] = 3;
        pkt[26] = 3;
        pkt[27] = RX_SHIFT;
    }
    for (size_t i = 0; i < len; i++)
        pkt[hl + i] = data[i];
    size_t total = hl + len;
    // pseudo header first
    uint8_t pseudo[12];
    uint32_t s = htonl(t->lip), d = htonl(t->rip);
    memcpy(pseudo, &s, 4);
    memcpy(pseudo + 4, &d, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_TCP;
    pseudo[10] = (uint8_t)(total >> 8);
    pseudo[11] = (uint8_t)total;
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2)
        sum += (uint32_t)(pseudo[i] << 8 | pseudo[i + 1]);
    h->csum = inet_checksum(pkt, total, sum);
    return ip_send(t->rip, IP_TCP, pkt, total);
}

static void send_ack(struct tcb *t)
{
    send_segment(t, t->snd_nxt, F_ACK, NULL, 0);
}

static void arm_timer(struct tcb *t)
{
    t->deadline = timer_uptime_ms() + t->rto;
}

// sends whatever the window allows, then the fin once the data is out
static void output(struct tcb *t)
{
    if (t->state != ESTABLISHED)
        return;
    bool sent = false;
    for (;;) {
        size_t off = t->snd_nxt - t->snd_una;
        if (off >= t->tx_len || off >= t->snd_wnd)
            break;
        size_t n = t->tx_len - off;
        if (n > MSS)
            n = MSS;
        if (n > t->snd_wnd - off)
            n = t->snd_wnd - off;
        uint8_t chunk[MSS];
        for (size_t i = 0; i < n; i++)
            chunk[i] = t->tx[(t->tx_head + off + i) % BUF_SIZE];
        send_segment(t, t->snd_nxt, F_ACK | F_PSH, chunk, n);
        t->snd_nxt += (uint32_t)n;
        sent = true;
    }
    if (t->closing && !t->fin_sent && t->snd_nxt - t->snd_una == t->tx_len) {
        send_segment(t, t->snd_nxt, F_ACK | F_FIN, NULL, 0);
        t->snd_nxt++;
        t->fin_sent = true;
        sent = true;
    }
    if (sent && !t->deadline)
        arm_timer(t);
}

static void fail(struct tcb *t, int err)
{
    t->err = err;
    t->state = CLOSED;
    t->deadline = 0;
    wake(t);
}

static struct tcb *find(uint32_t rip, uint16_t rport, uint16_t lport)
{
    for (struct tcb *t = conns; t; t = t->next)
        if (t->rip == rip && t->rport == rport && t->lport == lport)
            return t;
    return NULL;
}

static void send_reset(uint32_t src, uint32_t dst, const struct tcp_hdr *h, size_t datalen)
{
    struct tcb tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.lip = dst;
    tmp.rip = src;
    tmp.lport = ntohs(h->dport);
    tmp.rport = ntohs(h->sport);
    if (h->flags & F_ACK) {
        send_segment(&tmp, ntohl(h->ack), F_RST, NULL, 0);
    } else {
        tmp.rcv_nxt = ntohl(h->seq) + (uint32_t)datalen + ((h->flags & F_SYN) ? 1 : 0);
        send_segment(&tmp, 0, F_RST | F_ACK, NULL, 0);
    }
}

// window scale option of a syn, -1 if there's none
static int syn_wscale(const struct tcp_hdr *h, size_t hl)
{
    const uint8_t *o = (const uint8_t *)h + sizeof *h, *end = (const uint8_t *)h + hl;
    while (o < end) {
        if (*o == 0)
            break;
        if (*o == 1) {
            o++;
            continue;
        }
        if (o + 1 >= end || o[1] < 2 || o + o[1] > end)
            break;
        if (o[0] == 3 && o[1] == 3)
            return o[2];
        o += o[1];
    }
    return -1;
}

// copies a segment into the ring at its place past rcv_nxt. in order data
// moves rcv_nxt along, along with any out of order data it now touches
static void take_data(struct tcb *t, uint32_t seq, const uint8_t *data, size_t dlen)
{
    // trim what we already have
    if (seq_lt(seq, t->rcv_nxt)) {
        uint32_t old = t->rcv_nxt - seq;
        if (old >= dlen)
            return;
        data += old;
        dlen -= old;
        seq = t->rcv_nxt;
    }
    size_t off = seq - t->rcv_nxt;
    size_t space = RX_SIZE - t->rx_len;
    if (off >= space)
        return;
    if (dlen > space - off)
        dlen = space - off;
    size_t at = t->rx_head + t->rx_len + off;
    for (size_t i = 0; i < dlen; i++)
        t->rx[(at + i) % RX_SIZE] = data[i];
    uint32_t end = seq + (uint32_t)dlen;

    if (off) {
        // remember it, merged with ranges it overlaps
        for (int i = 0; i < t->nooo; i++) {
            if (seq_le(t->ooo[i].start, end) && seq_le(seq, t->ooo[i].end)) {
                if (seq_lt(t->ooo[i].start, seq))
                    seq = t->ooo[i].start;
                if (seq_lt(end, t->ooo[i].end))
                    end = t->ooo[i].end;
                t->ooo[i--] = t->ooo[--t->nooo];
            }
        }
        if (t->nooo < MAX_HOLES)
            t->ooo[t->nooo++] = (typeof(t->ooo[0])){ seq, end };
        return;
    }

    uint32_t nxt = end;
    for (bool more = true; more;) {
        more = false;
        for (int i = 0; i < t->nooo; i++) {
            if (seq_le(t->ooo[i].start, nxt)) {
                if (seq_lt(nxt, t->ooo[i].end))
                    nxt = t->ooo[i].end;
                t->ooo[i--] = t->ooo[--t->nooo];
                more = true;
            }
        }
    }
    t->rx_len += nxt - t->rcv_nxt;
    t->rcv_nxt = nxt;
    wake(t);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    if (len < sizeof(struct tcp_hdr))
        return;
    const struct tcp_hdr *h = (const void *)seg;
    size_t hl = (size_t)(h->off >> 4) * 4;
    if (hl < 20 || hl > len)
        return;
    const uint8_t *data = seg + hl;
    size_t dlen = len - hl;
    uint32_t seq = ntohl(h->seq), ack = ntohl(h->ack);

    uint64_t flags = irq_save();
    struct tcb *t = find(src, ntohs(h->sport), ntohs(h->dport));
    if (!t || t->state == CLOSED) {
        if (!(h->flags & F_RST))
            send_reset(src, dst, h, dlen);
        irq_restore(flags);
        return;
    }

    if (h->flags & F_RST) {
        fail(t, t->state == SYN_SENT ? ZK_ECONNREFUSED : ZK_ECONNRESET);
        irq_restore(flags);
        return;
    }

    if (t->state == SYN_SENT) {
        if ((h->flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == t->iss + 1) {
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_nxt = ack;
            int ws = syn_wscale(h, hl);
            if (ws >= 0) {
                t->snd_shift = (uint8_t)(ws > 14 ? 14 : ws);
                t->rcv_shift = RX_SHIFT;
            }
            t->snd_wnd = ntohs(h->win);    // never scaled in a syn
            t->state = ESTABLISHED;
            t->deadline = 0;
            t->tries = 0;
            t->rto = RTO_MIN * 2;
            send_ack(t);
            output(t);
            wake(t);
        }
        irq_restore(flags);
        return;
    }

    // acks
    if (h->flags & F_ACK) {
        if (seq_lt(t->snd_una, ack) && seq_le(ack, t->snd_nxt)) {
            uint32_t acked = ack - t->snd_una;
            size_t data_acked = acked > t->tx_len ? t->tx_len : acked;
            t->tx_head = (t->tx_head + data_acked) % BUF_SIZE;
            t->tx_len -= data_acked;
            t->snd_una = ack;
            if (t->fin_sent && ack == t->snd_nxt)
                t->fin_acked = true;
            t->tries = 0;
            t->rto = RTO_MIN * 2;
            t->deadline = t->snd_una == t->snd_nxt ? 0 : timer_uptime_ms() + t->rto;
            wake(t);
        }
        t->snd_wnd = (uint32_t)ntohs(h->win) << t->snd_shift;
    }

    bool ack_needed = false;
    if (dlen) {
        if (!t->fin_rcvd)
            take_data(t, seq, data, dlen);
        ack_needed = true;
    }
    if ((h->flags & F_FIN) && seq + (uint32_t)dlen == t->rcv_nxt && !t->fin_rcvd) {
        t->rcv_nxt++;
        t->fin_rcvd = true;
        ack_needed = true;
        wake(t);
    }
    if (ack_needed)
        send_ack(t);
    output(t);
    irq_restore(flags);
}

static void destroy(struct tcb *t)
{
    for (struct tcb **pp = &conns; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    kfree(t->tx);
    kfree(t->rx);
    kfree(t);
}

void tcp_timer(void)
{
    uint64_t now = timer_uptime_ms();
    uint64_t flags = irq_save();
    for (struct tcb *t = conns, *next; t; t = next) {
        next = t->next;
        bool done = t->state == CLOSED || (t->fin_sent && t->fin_acked && t->fin_rcvd);
        if (t->released && (done || now - t->closed_at > LINGER_MS)) {
            destroy(t);
            continue;
        }
        if (!t->deadline || now < t->deadline)
            continue;
        if (++t->tries > MAX_TRIES) {
            fail(t, ZK_ETIMEDOUT);
            continue;
        }
        t->rto = t->rto * 2 > RTO_MAX ? RTO_MAX : t->rto * 2;
        arm_timer(t);
        if (t->state == SYN_SENT) {
            send_segment(t, t->iss, F_SYN, NULL, 0);
        } else {
            // go back to the first unacked byte and send it all again
            t->snd_nxt = t->snd_una;
            if (t->fin_sent && !t->fin_acked)
                t->fin_sent = false;
            if (t->snd_wnd == 0)
                t->snd_wnd = 1;    // probe a closed window
            output(t);
        }
    }
    irq_restore(flags);
}

struct tcb *tcp_open(uint32_t dst, uint16_t dport, uint16_t lport, int *err)
{
    // only the client side exists, so nothing here could ever answer. x
    // clients try localhost:6000 before the server is up and would otherwise
    // sit through every retransmit
    if ((dst >> 24) == 127 || dst == netif.ip) {
        *err = ZK_ECONNREFUSED;
        return NULL;
    }
    struct tcb *t = kzalloc(sizeof(*t));
    uint8_t *tx = kmalloc(BUF_SIZE), *rx = kmalloc(RX_SIZE);
    if (!t || !tx || !rx) {
        kfree(t);
        kfree(tx);
        kfree(rx);
        *err = ZK_ENOMEM;
        return NULL;
    }
    t->tx = tx;
    t->rx = rx;
    t->lip = ip_source_for(dst);
    t->rip = dst;
    t->lport = lport;
    t->rport = dport;
    random_bytes(&t->iss, sizeof t->iss);
    t->snd_una = t->snd_nxt = t->iss + 1;
    t->state = SYN_SENT;
    t->rto = 1000;
    t->snd_wnd = MSS;

    uint64_t flags = irq_save();
    t->next = conns;
    conns = t;
    arm_timer(t);
    irq_restore(flags);
    *err = send_segment(t, t->iss, F_SYN, NULL, 0);
    if (*err == ZK_EHOSTUNREACH || *err == ZK_ENETUNREACH) {
        flags = irq_save();
        t->released = true;
        fail(t, *err);
        irq_restore(flags);
        return NULL;
    }
    *err = 0;
    return t;
}

int tcp_wait_connected(struct tcb *t, bool nonblock)
{
    uint64_t flags = irq_save();
    while (t->state == SYN_SENT) {
        if (nonblock) {
            irq_restore(flags);
            return ZK_EINPROGRESS;
        }
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&t->wq);
    }
    int err = t->state == ESTABLISHED ? 0 : (t->err ? t->err : ZK_ECONNREFUSED);
    irq_restore(flags);
    return err;
}

int tcp_error(struct tcb *t)
{
    return t->state == SYN_SENT ? 0 : t->err;
}

int64_t tcp_send(struct tcb *t, const void *buf, size_t len, bool nonblock)
{
    const uint8_t *src = buf;
    size_t done = 0;
    uint64_t flags = irq_save();
    while (done < len) {
        if (t->err) {
            irq_restore(flags);
            return done ? (int64_t)done : t->err;
        }
        if (t->state != ESTABLISHED || t->closing) {
            irq_restore(flags);
            return done ? (int64_t)done : ZK_EPIPE;
        }
        size_t space = BUF_SIZE - t->tx_len;
        if (!space) {
            if (nonblock) {
                irq_restore(flags);
                return done ? (int64_t)done : ZK_EAGAIN;
            }
            if (proc_killed())
                break;
            waitq_sleep(&t->wq);
            continue;
        }
        size_t n = len - done < space ? len - done : space;
        for (size_t i = 0; i < n; i++)
            t->tx[(t->tx_head + t->tx_len + i) % BUF_SIZE] = src[done + i];
        t->tx_len += n;
        done += n;
        output(t);
    }
    irq_restore(flags);
    return done ? (int64_t)done : ZK_EINTR;
}

int64_t tcp_recv(struct tcb *t, void *buf, size_t len, bool nonblock)
{
    uint8_t *dst = buf;
    uint64_t flags = irq_save();
    while (t->rx_len == 0) {
        if (t->fin_rcvd || t->state == CLOSED) {
            int err = t->err;
            irq_restore(flags);
            return err == ZK_ECONNRESET ? err : 0;
        }
        if (nonblock) {
            irq_restore(flags);
            return ZK_EAGAIN;
        }
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&t->wq);
    }
    size_t before = window(t);
    size_t n = len < t->rx_len ? len : t->rx_len;
    for (size_t i = 0; i < n; i++)
        dst[i] = t->rx[(t->rx_head + i) % RX_SIZE];
    t->rx_head = (t->rx_head + n) % RX_SIZE;
    t->rx_len -= n;
    // tell the peer once a good part of the buffer has opened up again
    uint32_t edge = t->rcv_nxt + (uint32_t)(RX_SIZE - t->rx_len);
    if (t->state == ESTABLISHED && (before < MSS || edge - t->adv_edge >= RX_SIZE / 4))
        send_ack(t);
    irq_restore(flags);
    return (int64_t)n;
}

int tcp_poll(struct tcb *t)
{
    int r = 0;
    if (t->rx_len || t->fin_rcvd || t->state == CLOSED)
        r |= POLLIN;
    if (t->state == ESTABLISHED && t->tx_len < BUF_SIZE && !t->closing)
        r |= POLLOUT;
    if (t->state == CLOSED)
        r |= POLLHUP | POLLOUT;
    return r;
}

void tcp_shutdown(struct tcb *t)
{
    uint64_t flags = irq_save();
    t->closing = true;
    output(t);
    irq_restore(flags);
}

void tcp_release(struct tcb *t)
{
    uint64_t flags = irq_save();
    t->released = true;
    t->closed_at = timer_uptime_ms();
    if (t->state == ESTABLISHED) {
        t->closing = true;
        output(t);
    } else {
        t->state = CLOSED;
    }
    irq_restore(flags);
}

void tcp_addrs(struct tcb *t, uint32_t *lip, uint16_t *lport, uint32_t *rip, uint16_t *rport)
{
    *lip = t->lip;
    *lport = t->lport;
    *rip = t->rip;
    *rport = t->rport;
}

bool tcp_port_used(uint16_t port)
{
    for (struct tcb *t = conns; t; t = t->next)
        if (t->lport == port)
            return true;
    return false;
}
