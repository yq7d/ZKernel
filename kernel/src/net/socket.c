// ipv4 sockets as files: tcp streams and udp datagrams
#include "net/net.h"
#include "net/tcp.h"
#include "arch/x86_64/cpu.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "proc/sys.h"
#include "sched/sched.h"

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000
#define MSG_DONTWAIT  0x40
#define MAX_QUEUED  64
#define UDP_MAX     1472

struct sockaddr_in {
    uint16_t family;
    uint16_t port;     // network order
    uint32_t addr;     // network order
    uint8_t zero[8];
} __attribute__((packed));

struct dgram {
    uint32_t src;
    uint16_t sport;
    size_t len;
    struct dgram *next;
    uint8_t data[];
};

struct sock {
    struct vnode v;
    int type;
    bool nonblock;
    struct tcb *tcb;
    uint16_t lport;
    uint32_t peer_ip;
    uint16_t peer_port;
    bool has_peer;
    struct dgram *qhead, *qtail;
    int qlen;
    struct waitq wq;
    struct sock *next;
};

static struct sock *udp_socks;
static uint16_t next_port = 49152;
static const struct vnode_ops sock_ops;

static struct sock *sock_of(struct file *f)
{
    return f && f->v->ops == &sock_ops ? (struct sock *)f->v : NULL;
}

static bool udp_port_used(uint16_t port)
{
    for (struct sock *s = udp_socks; s; s = s->next)
        if (s->lport == port)
            return true;
    return false;
}

uint16_t sock_ephemeral_port(void)
{
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_port++;
        if (next_port == 0)
            next_port = 49152;
        if (p >= 49152 && !udp_port_used(p) && !tcp_port_used(p))
            return p;
    }
    return 0;
}

void sock_udp_deliver(uint16_t port, uint32_t src, uint16_t sport, const uint8_t *data, size_t len)
{
    uint64_t flags = irq_save();
    struct sock *s = udp_socks;
    while (s && s->lport != port)
        s = s->next;
    if (!s || s->qlen >= MAX_QUEUED || (s->has_peer && (src != s->peer_ip || sport != s->peer_port))) {
        irq_restore(flags);
        return;
    }
    struct dgram *d = kmalloc(sizeof(*d) + len);
    if (!d) {
        irq_restore(flags);
        return;
    }
    d->src = src;
    d->sport = sport;
    d->len = len;
    d->next = NULL;
    memcpy(d->data, data, len);
    if (s->qtail)
        s->qtail->next = d;
    else
        s->qhead = d;
    s->qtail = d;
    s->qlen++;
    waitq_wake_all(&s->wq);
    poll_notify();
    irq_restore(flags);
}

void sock_set_nonblock(struct vnode *v, bool on)
{
    if (v->ops == &sock_ops)
        ((struct sock *)v)->nonblock = on;
}

static void bind_udp(struct sock *s, uint16_t port)
{
    uint64_t flags = irq_save();
    s->lport = port ? port : sock_ephemeral_port();
    s->next = udp_socks;
    udp_socks = s;
    irq_restore(flags);
}

static int64_t udp_recv(struct sock *s, void *buf, size_t len, bool nonblock,
                        uint32_t *src, uint16_t *sport)
{
    uint64_t flags = irq_save();
    while (!s->qhead) {
        if (nonblock || s->nonblock) {
            irq_restore(flags);
            return ZK_EAGAIN;
        }
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&s->wq);
    }
    struct dgram *d = s->qhead;
    s->qhead = d->next;
    if (!s->qhead)
        s->qtail = NULL;
    s->qlen--;
    irq_restore(flags);
    size_t n = d->len < len ? d->len : len;
    memcpy(buf, d->data, n);
    if (src)
        *src = d->src;
    if (sport)
        *sport = d->sport;
    kfree(d);
    return (int64_t)n;
}

static int64_t s_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct sock *s = (struct sock *)v;
    if (s->type == SOCK_STREAM)
        return s->tcb ? tcp_recv(s->tcb, buf, len, s->nonblock) : ZK_ENOTCONN;
    return udp_recv(s, buf, len, false, NULL, NULL);
}

static int64_t s_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct sock *s = (struct sock *)v;
    if (s->type == SOCK_STREAM)
        return s->tcb ? tcp_send(s->tcb, buf, len, s->nonblock) : ZK_ENOTCONN;
    if (!s->has_peer)
        return ZK_ENOTCONN;
    if (len > UDP_MAX)
        return ZK_EINVAL;
    int err = udp_send(s->lport, s->peer_ip, s->peer_port, buf, len);
    return err ? err : (int64_t)len;
}

static int s_poll(struct vnode *v)
{
    struct sock *s = (struct sock *)v;
    if (s->type == SOCK_STREAM)
        return s->tcb ? tcp_poll(s->tcb) : POLLOUT;
    return POLLOUT | (s->qhead ? POLLIN : 0);
}

static void s_release(struct vnode *v)
{
    struct sock *s = (struct sock *)v;
    if (s->tcb)
        tcp_release(s->tcb);
    uint64_t flags = irq_save();
    for (struct sock **pp = &udp_socks; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    irq_restore(flags);
    while (s->qhead) {
        struct dgram *d = s->qhead;
        s->qhead = d->next;
        kfree(d);
    }
    kfree(s);
}

static const struct vnode_ops sock_ops = {
    .read = s_read, .write = s_write, .poll = s_poll, .release = s_release,
};

int64_t sys_socket(int domain, int type)
{
    if (domain != AF_INET)
        return ZK_EINVAL;
    int kind = type & 0xf;
    if (kind != SOCK_STREAM && kind != SOCK_DGRAM)
        return ZK_EINVAL;
    struct sock *s = kzalloc(sizeof(*s));
    if (!s)
        return ZK_ENOMEM;
    s->v = (struct vnode){ .type = VNODE_SOCK, .refs = 1, .ino = vfs_next_ino(),
                           .mode = 0777, .ops = &sock_ops };
    s->type = kind;
    s->nonblock = type & SOCK_NONBLOCK;
    struct file *f = file_new(&s->v, O_RDWR);
    vnode_put(&s->v);
    if (!f)
        return ZK_ENOMEM;
    struct proc *p = proc_current();
    int fd = proc_fd_alloc(p, f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (type & SOCK_CLOEXEC)
        p->cloexec |= 1U << fd;
    return fd;
}

static int get_addr(uint64_t uaddr, uint32_t len, uint32_t *ip, uint16_t *port)
{
    struct sockaddr_in a;
    if (len < sizeof a)
        return ZK_EINVAL;
    if (copy_in(&a, uaddr, sizeof a))
        return ZK_EFAULT;
    if (a.family != AF_INET)
        return ZK_EINVAL;
    *ip = ntohl(a.addr);
    *port = ntohs(a.port);
    return 0;
}

static int put_addr(uint64_t uaddr, uint64_t ualen, uint32_t ip, uint16_t port)
{
    if (!uaddr)
        return 0;
    uint32_t len;
    if (copy_in(&len, ualen, 4))
        return ZK_EFAULT;
    struct sockaddr_in a = { AF_INET, htons(port), htonl(ip), { 0 } };
    uint32_t n = len < sizeof a ? len : sizeof a;
    if (copy_out(uaddr, &a, n))
        return ZK_EFAULT;
    len = sizeof a;
    return copy_out(ualen, &len, 4);
}

int64_t sys_connect(int fd, uint64_t uaddr, uint32_t len)
{
    struct sock *s = sock_of(proc_fd_get(proc_current(), fd));
    if (!s)
        return ZK_ENOTSOCK;
    uint32_t ip;
    uint16_t port;
    int err = get_addr(uaddr, len, &ip, &port);
    if (err)
        return err;
    if (s->type == SOCK_DGRAM) {
        if (!s->lport)
            bind_udp(s, 0);
        s->peer_ip = ip;
        s->peer_port = port;
        s->has_peer = true;
        return 0;
    }
    if (s->tcb) {
        // asked again after a non-blocking connect
        return tcp_wait_connected(s->tcb, s->nonblock) == ZK_EINPROGRESS ? ZK_EINPROGRESS : tcp_error(s->tcb);
    }
    if (!netif.up)
        return ZK_ENETUNREACH;
    s->tcb = tcp_open(ip, port, s->lport ? s->lport : sock_ephemeral_port(), &err);
    if (!s->tcb)
        return err;
    return tcp_wait_connected(s->tcb, s->nonblock);
}

int64_t sys_bind(int fd, uint64_t uaddr, uint32_t len)
{
    struct sock *s = sock_of(proc_fd_get(proc_current(), fd));
    if (!s)
        return ZK_ENOTSOCK;
    uint32_t ip;
    uint16_t port;
    int err = get_addr(uaddr, len, &ip, &port);
    if (err)
        return err;
    if (s->lport)
        return ZK_EINVAL;
    if (port && (udp_port_used(port) || tcp_port_used(port)))
        return ZK_EADDRINUSE;
    if (port && port < 1024 && cred_current()->euid != 0)
        return ZK_EACCES;
    if (s->type == SOCK_DGRAM)
        bind_udp(s, port);
    else
        s->lport = port ? port : sock_ephemeral_port();
    return 0;
}

int64_t sys_sendto(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint32_t alen)
{
    struct file *f = proc_fd_get(proc_current(), fd);
    struct sock *s = sock_of(f);
    if (!s)
        return ZK_ENOTSOCK;
    if (!user_ok(buf, len, false))
        return ZK_EFAULT;
    if (s->type == SOCK_STREAM || !uaddr)
        return s->type == SOCK_STREAM && s->tcb ?
               tcp_send(s->tcb, (const void *)buf, len, s->nonblock || (flags & MSG_DONTWAIT)) :
               file_write(f, (const void *)buf, len);
    uint32_t ip;
    uint16_t port;
    int err = get_addr(uaddr, alen, &ip, &port);
    if (err)
        return err;
    if (len > UDP_MAX)
        return ZK_EINVAL;
    if (!s->lport)
        bind_udp(s, 0);
    err = udp_send(s->lport, ip, port, (const void *)buf, len);
    return err ? err : (int64_t)len;
}

int64_t sys_recvfrom(int fd, uint64_t buf, size_t len, int flags, uint64_t uaddr, uint64_t ualen)
{
    struct sock *s = sock_of(proc_fd_get(proc_current(), fd));
    if (!s)
        return ZK_ENOTSOCK;
    if (!user_ok(buf, len, true))
        return ZK_EFAULT;
    bool nb = s->nonblock || (flags & MSG_DONTWAIT);
    if (s->type == SOCK_STREAM) {
        if (!s->tcb)
            return ZK_ENOTCONN;
        int64_t n = tcp_recv(s->tcb, (void *)buf, len, nb);
        if (n >= 0 && uaddr) {
            uint32_t lip, rip;
            uint16_t lp, rp;
            tcp_addrs(s->tcb, &lip, &lp, &rip, &rp);
            put_addr(uaddr, ualen, rip, rp);
        }
        return n;
    }
    if (!s->lport)
        bind_udp(s, 0);
    uint32_t src;
    uint16_t sport;
    int64_t n = udp_recv(s, (void *)buf, len, nb, &src, &sport);
    if (n >= 0) {
        int err = put_addr(uaddr, ualen, src, sport);
        if (err)
            return err;
    }
    return n;
}

int64_t sys_getsockname(int fd, uint64_t uaddr, uint64_t ualen, bool peer)
{
    struct sock *s = sock_of(proc_fd_get(proc_current(), fd));
    if (!s)
        return ZK_ENOTSOCK;
    uint32_t lip = netif.ip, rip = s->peer_ip;
    uint16_t lport = s->lport, rport = s->peer_port;
    if (s->tcb)
        tcp_addrs(s->tcb, &lip, &lport, &rip, &rport);
    if (peer && !s->tcb && !s->has_peer)
        return ZK_ENOTCONN;
    return put_addr(uaddr, ualen, peer ? rip : lip, peer ? rport : lport);
}

int64_t sys_shutdown(int fd, int how)
{
    struct sock *s = sock_of(proc_fd_get(proc_current(), fd));
    if (!s)
        return ZK_ENOTSOCK;
    if (s->tcb && how >= 1)
        tcp_shutdown(s->tcb);
    return 0;
}

static int64_t sock_error(int fd, uint64_t uval, uint64_t ulen, bool linux_abi);

struct msghdr {
    uint64_t name;
    uint32_t namelen, pad;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control, controllen;
    int32_t flags, pad2;
};

static int64_t msg(int fd, uint64_t umsg, int flags, bool send)
{
    struct msghdr m;
    if (copy_in(&m, umsg, sizeof m))
        return ZK_EFAULT;
    if (m.iovlen == 0)
        return 0;
    uint64_t iov[2];
    if (copy_in(iov, m.iov, sizeof iov))
        return ZK_EFAULT;
    // one buffer is all anything here uses
    if (send)
        return sys_sendto(fd, iov[0], iov[1], flags, m.name, m.namelen);
    uint64_t lenp = umsg + 8;
    int64_t n = sys_recvfrom(fd, iov[0], iov[1], flags, m.name, m.name ? lenp : 0);
    if (n >= 0) {
        uint64_t zero = 0;
        copy_out(umsg + 40, &zero, 8);    // no control data
    }
    return n;
}

int64_t linux_socketcall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    switch (nr) {
    case 41: return (int)a1 == AF_INET ? sys_socket((int)a1, (int)a2) : -1000 - 97;    // EAFNOSUPPORT
    case 42: return sys_connect((int)a1, a2, (uint32_t)a3);
    case 43: return ZK_ENOSYS;    // accept, no listening sockets
    case 44: return sys_sendto((int)a1, a2, a3, (int)a4, a5, (uint32_t)a6);
    case 45: return sys_recvfrom((int)a1, a2, a3, (int)a4, a5, a6);
    case 46: return msg((int)a1, a2, (int)a3, true);
    case 47: return msg((int)a1, a2, (int)a3, false);
    case 48: return sys_shutdown((int)a1, (int)a2);
    case 49: return sys_bind((int)a1, a2, (uint32_t)a3);
    case 50: return ZK_ENOSYS;    // listen
    case 51: return sys_getsockname((int)a1, a2, a3, false);
    case 52: return sys_getsockname((int)a1, a2, a3, true);
    case 53: return ZK_ENOSYS;    // socketpair
    case 54: return 0;            // setsockopt
    case 55: return sock_error((int)a1, a4, a5, a3 == 4 && a2 == 1);
    }
    return ZK_ENOSYS;
}

// getsockopt: SO_ERROR is the one that matters, after a non-blocking connect
static int64_t sock_error(int fd, uint64_t uval, uint64_t ulen, bool is_so_error)
{
    struct sock *s = sock_of(proc_fd_get(proc_current(), fd));
    if (!s)
        return ZK_ENOTSOCK;
    int32_t v = 0;
    if (is_so_error && s->tcb) {
        int err = tcp_error(s->tcb);
        // linux errno numbers, the caller expects those in the value
        static const int16_t map[] = { [-ZK_ECONNREFUSED] = 111, [-ZK_ETIMEDOUT] = 110,
                                       [-ZK_ECONNRESET] = 104, [-ZK_EHOSTUNREACH] = 113,
                                       [-ZK_ENETUNREACH] = 101 };
        v = err < 0 && -err < (int)(sizeof map / sizeof map[0]) ? map[-err] : (err ? 5 : 0);
    }
    uint32_t len = 4;
    if (copy_out(uval, &v, 4) || copy_out(ulen, &len, 4))
        return ZK_EFAULT;
    return 0;
}
