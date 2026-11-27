// unix domain sockets: stream, datagram and seqpacket, bound to a path or
// to the abstract namespace, socketpair, fd passing (SCM_RIGHTS) and peer
// credentials. what x11, wayland and d-bus talk over.
//
// everything here runs with interrupts off while it touches socket state,
// which on one cpu is the whole locking story
#include "net/net.h"
#include "net/unix.h"
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/linux.h"
#include "proc/proc.h"
#include "proc/sys.h"
#include "sched/sched.h"

#define AF_UNIX        1
#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

#define MSG_PEEK     0x2
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40
#define MSG_EOR      0x80
#define MSG_WAITALL  0x100
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define MSG_CTRUNC   0x8

#define SOL_SOCKET     1
#define SCM_RIGHTS     1
#define SCM_CREDENTIALS 2

#define SO_TYPE       3
#define SO_ERROR      4
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_PASSCRED   16
#define SO_PEERCRED   17
#define SO_ACCEPTCONN 30
#define SO_DOMAIN     39

#define SUN_PATH 108
#define QUEUE_MAX (256 * 1024)
#define MAX_FDS_PER_MSG 253
#define MAX_MSG (256 * 1024)

#define LE(n) (-1000 - (n))    // linux errno straight through
#define LEOPNOTSUPP    LE(95)
#define LEAFNOSUPPORT  LE(97)
#define LEPROTOTYPE    LE(91)
#define LEISCONN       LE(106)
#define LEDESTADDRREQ  LE(89)
#define LEMSGSIZE      LE(90)
#define LENOPROTOOPT   LE(92)

enum { US_NEW, US_LISTEN, US_CONNECTED, US_DISCONNECTED };

struct ucred {
    int32_t pid;
    uint32_t uid, gid;
};

struct seg {
    size_t len, off;
    int nfds;
    struct file **fds;
    struct ucred cred;
    char from[SUN_PATH + 1];    // sender's address, for datagrams
    int fromlen;
    struct seg *next;
    uint8_t data[];
};

struct usock {
    struct vnode v;
    int type;
    int state;
    bool nonblock;
    bool rd_shut, wr_shut;      // this side
    bool peer_wr_shut;          // the other side won't send more: eof here
    struct usock *peer;         // connected, cleared when the peer goes
    struct seg *qh, *qt;
    size_t qbytes;

    struct usock *pending;      // listening: connected, not yet accepted
    struct usock *pnext;
    int npending, backlog;

    char name[SUN_PATH + 1];    // bound address, name[0] == 0 is abstract
    int namelen;                // 0 when unbound
    struct vnode *node;         // filesystem entry for a path bind
    char dgram_peer[SUN_PATH + 1];    // connected datagram socket's target
    int dgram_peer_len;

    struct ucred peercred, mycred;
    bool has_peercred;
    struct waitq wq;
    struct usock *abs_next;     // abstract namespace list
};

// the filesystem entry a path bind makes. points at the socket while it's bound
struct unode {
    struct vnode v;
    struct usock *sock;
};

struct sockaddr_un {
    uint16_t family;
    char path[SUN_PATH];
};

static const struct vnode_ops usock_ops;
static const struct vnode_ops unode_ops;
static struct usock *abstract_socks;

static struct usock *usock_of(struct file *f)
{
    return f && f->v->ops == &usock_ops ? (struct usock *)f->v : NULL;
}

bool unix_is_socket(struct file *f)
{
    return usock_of(f) != NULL;
}

static struct ucred my_cred(void)
{
    struct proc *p = proc_current();
    const struct zk_cred *c = cred_current();
    return (struct ucred){ .pid = p ? (int)p->pid : 0, .uid = c->euid, .gid = c->egid };
}

static void wake(struct usock *s)
{
    if (s)
        waitq_wake_all(&s->wq);
    poll_notify();
}

static struct usock *new_usock(int type)
{
    struct usock *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->v = (struct vnode){ .type = VNODE_SOCK, .refs = 1, .ino = vfs_next_ino(),
                           .mode = 0777, .ops = &usock_ops };
    s->type = type;
    s->wq = (struct waitq)WAITQ_INIT;
    s->mycred = my_cred();
    return s;
}

static void free_seg(struct seg *g)
{
    for (int i = 0; i < g->nfds; i++)
        if (g->fds[i])
            file_put(g->fds[i]);
    kfree(g->fds);
    kfree(g);
}

static int install(struct usock *s, int flags)
{
    struct file *f = file_new(&s->v, O_RDWR | ((flags & SOCK_NONBLOCK) ? O_NONBLOCK : 0));
    vnode_put(&s->v);
    if (!f)
        return ZK_ENOMEM;
    struct proc *p = proc_current();
    int fd = proc_fd_alloc(p, f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (flags & SOCK_CLOEXEC)
        proc_set_cloexec(p, fd, true);
    s->nonblock = flags & SOCK_NONBLOCK;
    return fd;
}

// interrupts off. the peer is going away
static void disconnect(struct usock *s)
{
    struct usock *p = s->peer;
    if (p) {
        p->peer = NULL;
        p->peer_wr_shut = true;
        if (p->state == US_CONNECTED)
            p->state = US_DISCONNECTED;
        wake(p);
    }
    s->peer = NULL;
}

static void unbind(struct usock *s)
{
    if (s->node) {
        ((struct unode *)s->node)->sock = NULL;
        vnode_put(s->node);
        s->node = NULL;
    }
    if (s->namelen && s->name[0] == '\0') {
        for (struct usock **pp = &abstract_socks; *pp; pp = &(*pp)->abs_next) {
            if (*pp == s) {
                *pp = s->abs_next;
                break;
            }
        }
    }
}

static void u_release(struct vnode *v)
{
    struct usock *s = (struct usock *)v;
    uint64_t flags = irq_save();
    disconnect(s);
    unbind(s);
    struct usock *pend = s->pending;
    s->pending = NULL;
    struct seg *q = s->qh;
    s->qh = s->qt = NULL;
    irq_restore(flags);

    // connections nobody accepted get dropped, their clients see eof
    while (pend) {
        struct usock *n = pend->pnext;
        vnode_put(&pend->v);
        pend = n;
    }
    while (q) {
        struct seg *n = q->next;
        free_seg(q);
        q = n;
    }
    kfree(s);
}

static void n_release(struct vnode *v)
{
    kfree(v);
}

static const struct vnode_ops unode_ops = {
    .release = n_release,
};

// addresses

// sun_path into the key the socket is found by: an absolute path, or the
// abstract name with its leading nul
static int get_name(uint64_t uaddr, uint32_t alen, char *key, int *keylen, char *path)
{
    struct sockaddr_un a;
    if (alen < 2 || alen > sizeof a)
        return ZK_EINVAL;
    memset(&a, 0, sizeof a);
    if (copy_in(&a, uaddr, alen))
        return ZK_EFAULT;
    if (a.family != AF_UNIX)
        return ZK_EINVAL;
    int n = (int)alen - 2;
    if (n <= 0)
        return ZK_EINVAL;
    if (a.path[0] == '\0') {
        memcpy(key, a.path, (size_t)n);
        *keylen = n;
        path[0] = '\0';
        return 0;
    }
    char raw[SUN_PATH + 1];
    memcpy(raw, a.path, (size_t)n);
    raw[n] = '\0';
    char abs[ZK_PATH_MAX];
    int err = vfs_normalize(proc_current()->cwd, raw, abs);
    if (err)
        return err;
    // what the program sees as its address stays what it passed in
    size_t rl = strlen(raw);
    memcpy(key, raw, rl + 1);
    *keylen = (int)rl + 1;
    linux_resolve(abs, path, true);
    return 0;
}

static int put_name(const char *name, int len, uint64_t uaddr, uint64_t ualen)
{
    if (!uaddr)
        return 0;
    uint32_t have;
    if (copy_in(&have, ualen, 4))
        return ZK_EFAULT;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.family = AF_UNIX;
    memcpy(a.path, name, (size_t)len);
    uint32_t full = 2 + (uint32_t)len;
    if (copy_out(uaddr, &a, have < full ? have : full) || copy_out(ualen, &full, 4))
        return ZK_EFAULT;
    return 0;
}

static struct usock *find_abstract(const char *key, int keylen)
{
    for (struct usock *s = abstract_socks; s; s = s->abs_next)
        if (s->namelen == keylen && memcmp(s->name, key, (size_t)keylen) == 0)
            return s;
    return NULL;
}

// the socket bound at a filesystem path, or NULL
static int find_path(const char *path, struct usock **out)
{
    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (err)
        return err;
    if (v->ops != &unode_ops) {
        vnode_put(v);
        return ZK_ECONNREFUSED;
    }
    *out = ((struct unode *)v)->sock;
    vnode_put(v);
    return *out ? 0 : ZK_ECONNREFUSED;
}

static int lookup(uint64_t uaddr, uint32_t alen, struct usock **out)
{
    char key[SUN_PATH + 1], path[ZK_PATH_MAX];
    int keylen;
    int err = get_name(uaddr, alen, key, &keylen, path);
    if (err)
        return err;
    if (key[0] == '\0') {
        *out = find_abstract(key, keylen);
        return *out ? 0 : ZK_ECONNREFUSED;
    }
    return find_path(path, out);
}

static int u_bind(struct usock *s, uint64_t uaddr, uint32_t alen)
{
    if (s->namelen)
        return ZK_EINVAL;
    char key[SUN_PATH + 1], path[ZK_PATH_MAX];
    int keylen;
    int err = get_name(uaddr, alen, key, &keylen, path);
    if (err)
        return err;
    if (key[0] == '\0') {
        if (find_abstract(key, keylen))
            return ZK_EADDRINUSE;
        memcpy(s->name, key, (size_t)keylen);
        s->namelen = keylen;
        s->abs_next = abstract_socks;
        abstract_socks = s;
        return 0;
    }
    struct unode *n = kzalloc(sizeof(*n));
    if (!n)
        return ZK_ENOMEM;
    const struct zk_cred *c = cred_current();
    n->v = (struct vnode){ .type = VNODE_SOCK, .refs = 1, .ino = vfs_next_ino(),
                           .uid = c->euid, .gid = c->egid, .mode = 0777 & ~022,
                           .ops = &unode_ops };
    // an existing file, socket or not, is in the way, like on linux
    err = vfs_mknod(path, &n->v);
    if (err) {
        kfree(n);
        return err == ZK_EEXIST ? ZK_EADDRINUSE : err;
    }
    n->sock = s;
    s->node = &n->v;
    memcpy(s->name, key, (size_t)keylen);
    s->namelen = keylen;
    return 0;
}

// queueing

// interrupts off. room for len more bytes in s's queue
static bool has_room(struct usock *s, size_t len)
{
    return s->qbytes == 0 || s->qbytes + len <= QUEUE_MAX;
}

static void enqueue(struct usock *s, struct seg *g)
{
    g->next = NULL;
    if (s->qt)
        s->qt->next = g;
    else
        s->qh = g;
    s->qt = g;
    s->qbytes += g->len;
    wake(s);
}

static bool blocking(struct usock *s, struct file *f, int flags)
{
    return !(s->nonblock || (flags & MSG_DONTWAIT) || (f && (f->flags & O_NONBLOCK)));
}

static void sigpipe(int flags)
{
    if (!(flags & MSG_NOSIGNAL))
        signal_send(proc_current(), SIGPIPE, NULL);
}

// builds a segment from user iovecs
struct iov {
    uint64_t base, len;
};

static int64_t gather(const struct iov *iov, int niov, size_t total, struct seg **out)
{
    struct seg *g = kzalloc(sizeof(*g) + total);
    if (!g)
        return ZK_ENOMEM;
    size_t at = 0;
    for (int i = 0; i < niov && at < total; i++) {
        size_t n = iov[i].len < total - at ? iov[i].len : total - at;
        if (n && copy_in(g->data + at, iov[i].base, n)) {
            kfree(g);
            return ZK_EFAULT;
        }
        at += n;
    }
    g->len = at;
    *out = g;
    return 0;
}

// pulls the fds out of SCM_RIGHTS control data
static int take_fds(uint64_t control, uint64_t clen, struct seg *g)
{
    uint64_t at = 0;
    while (at + 16 <= clen) {
        struct { uint64_t len; int32_t level, type; } h;
        if (copy_in(&h, control + at, sizeof h))
            return ZK_EFAULT;
        if (h.len < 16 || at + h.len > clen)
            return ZK_EINVAL;
        if (h.level == SOL_SOCKET && h.type == SCM_RIGHTS) {
            int n = (int)((h.len - 16) / 4);
            if (n > MAX_FDS_PER_MSG || g->nfds + n > MAX_FDS_PER_MSG)
                return ZK_EINVAL;
            struct file **fds = kmalloc(sizeof(*fds) * (size_t)(g->nfds + n));
            if (!fds)
                return ZK_ENOMEM;
            for (int i = 0; i < g->nfds; i++)
                fds[i] = g->fds[i];
            kfree(g->fds);
            g->fds = fds;
            for (int i = 0; i < n; i++) {
                int32_t fd;
                if (copy_in(&fd, control + at + 16 + (uint64_t)i * 4, 4))
                    return ZK_EFAULT;
                struct file *f = proc_fd_get(proc_current(), fd);
                if (!f)
                    return ZK_EBADF;
                file_get(f);
                g->fds[g->nfds++] = f;
            }
        }
        // SCM_CREDENTIALS: the kernel fills them in anyway
        at += (h.len + 7) & ~7UL;
    }
    return 0;
}

static int64_t u_send(struct usock *s, struct file *f, const struct iov *iov, int niov,
                      uint64_t control, uint64_t clen, int flags,
                      uint64_t uaddr, uint32_t alen)
{
    size_t total = 0;
    for (int i = 0; i < niov; i++)
        total += iov[i].len;
    if (s->wr_shut) {
        sigpipe(flags);
        return ZK_EPIPE;
    }
    bool stream = s->type == SOCK_STREAM;
    if (!stream && total > MAX_MSG)
        return LEMSGSIZE;

    struct usock *dst = NULL;
    if (s->type == SOCK_DGRAM && (uaddr || !s->peer)) {
        int err;
        if (uaddr)
            err = lookup(uaddr, alen, &dst);
        else if (s->dgram_peer_len)
            err = s->dgram_peer[0] ? find_path(s->dgram_peer, &dst)
                                   : (dst = find_abstract(s->dgram_peer, s->dgram_peer_len)) ? 0 : ZK_ECONNREFUSED;
        else
            return LEDESTADDRREQ;
        if (err)
            return err;
        if (dst->type != SOCK_DGRAM)
            return LEPROTOTYPE;
    } else if (uaddr && s->peer) {
        return LEISCONN;
    }

    // a stream goes out in pieces when it's bigger than the queue
    size_t sent = 0;
    size_t chunk_max = stream ? QUEUE_MAX / 2 : total;
    bool first = true;
    uint64_t irq = irq_save();
    for (;;) {
        struct usock *to = dst ? dst : s->peer;
        if (!to) {
            irq_restore(irq);
            if (sent)
                return (int64_t)sent;
            if (s->type == SOCK_DGRAM || s->state == US_NEW)
                return ZK_ENOTCONN;
            sigpipe(flags);
            return ZK_EPIPE;
        }
        if (to->rd_shut || (!dst && s->peer && s->peer->rd_shut)) {
            irq_restore(irq);
            sigpipe(flags);
            return sent ? (int64_t)sent : ZK_EPIPE;
        }
        size_t want = total - sent < chunk_max ? total - sent : chunk_max;
        if (!has_room(to, want)) {
            if (sent && !blocking(s, f, flags)) {
                irq_restore(irq);
                return (int64_t)sent;
            }
            if (!blocking(s, f, flags)) {
                irq_restore(irq);
                return ZK_EAGAIN;
            }
            if (proc_killed()) {
                irq_restore(irq);
                return sent ? (int64_t)sent : ZK_EINTR;
            }
            waitq_sleep(&to->wq);
            continue;
        }
        irq_restore(irq);

        // copy this piece in with interrupts on, user memory may fault
        struct iov sub[16];
        int nsub = 0;
        size_t skip = sent, left = want;
        for (int i = 0; i < niov && left && nsub < 16; i++) {
            if (skip >= iov[i].len) {
                skip -= iov[i].len;
                continue;
            }
            size_t n = iov[i].len - skip;
            if (n > left)
                n = left;
            sub[nsub++] = (struct iov){ iov[i].base + skip, n };
            skip = 0;
            left -= n;
        }
        struct seg *g;
        int64_t err = gather(sub, nsub, want, &g);
        if (err)
            return sent ? (int64_t)sent : err;
        if (first && control && clen) {
            err = take_fds(control, clen, g);
            if (err) {
                free_seg(g);
                return err;
            }
        }
        first = false;
        g->cred = my_cred();
        memcpy(g->from, s->name, (size_t)s->namelen);
        g->fromlen = s->namelen;

        irq = irq_save();
        to = dst ? dst : s->peer;
        if (!to) {
            irq_restore(irq);
            free_seg(g);
            sigpipe(flags);
            return sent ? (int64_t)sent : ZK_EPIPE;
        }
        enqueue(to, g);
        sent += g->len;
        if (sent >= total)
            break;
    }
    irq_restore(irq);
    return (int64_t)sent;
}

// hands the passed fds to the caller, as much as fits in the control buffer
static uint64_t give_fds(struct seg *g, uint64_t control, uint64_t clen, int flags,
                         bool *ctrunc)
{
    if (!g->nfds)
        return 0;
    int room = clen >= 16 ? (int)((clen - 16) / 4) : 0;
    int n = g->nfds < room ? g->nfds : room;
    if (n < g->nfds)
        *ctrunc = true;
    int32_t fds[MAX_FDS_PER_MSG];
    struct proc *p = proc_current();
    int got = 0;
    for (int i = 0; i < g->nfds; i++) {
        struct file *f = g->fds[i];
        g->fds[i] = NULL;
        if (i >= n) {
            file_put(f);
            continue;
        }
        int fd = proc_fd_alloc(p, f);
        if (fd < 0) {
            file_put(f);
            *ctrunc = true;
            continue;
        }
        if (flags & MSG_CMSG_CLOEXEC)
            proc_set_cloexec(p, fd, true);
        fds[got++] = fd;
    }
    g->nfds = 0;
    if (!got)
        return 0;
    struct { uint64_t len; int32_t level, type; } h = {
        16 + (uint64_t)got * 4, SOL_SOCKET, SCM_RIGHTS,
    };
    copy_out(control, &h, sizeof h);
    copy_out(control + 16, fds, (size_t)got * 4);
    return (h.len + 7) & ~7UL;
}

static int64_t u_recv(struct usock *s, struct file *f, const struct iov *iov, int niov,
                      uint64_t control, uint64_t *clen, int flags, int *mflags,
                      uint64_t uaddr, uint64_t ualen)
{
    size_t total = 0;
    for (int i = 0; i < niov; i++)
        total += iov[i].len;
    if (s->state == US_LISTEN)
        return ZK_ENOTCONN;
    bool stream = s->type == SOCK_STREAM;
    uint64_t cap = clen ? *clen : 0;
    if (clen)
        *clen = 0;

    uint64_t irq = irq_save();
    for (;;) {
        if (s->qh)
            break;
        if (s->rd_shut || s->peer_wr_shut || (stream && s->state == US_DISCONNECTED)) {
            irq_restore(irq);
            return 0;
        }
        if (stream && s->state == US_NEW) {
            irq_restore(irq);
            return ZK_ENOTCONN;
        }
        if (!blocking(s, f, flags)) {
            irq_restore(irq);
            return ZK_EAGAIN;
        }
        if (proc_killed()) {
            irq_restore(irq);
            return ZK_EINTR;
        }
        waitq_sleep(&s->wq);
    }

    // take what's queued off the list first, copying out can fault
    size_t got = 0;
    int vi = 0;
    size_t voff = 0;
    bool ctrunc = false;
    uint64_t cused = 0;
    struct seg *g = s->qh;
    if (uaddr && g)
        put_name(g->from, g->fromlen, uaddr, ualen);
    while (g) {
        // fds stay with the bytes they were sent with
        if (got && g->nfds)
            break;
        size_t avail = g->len - g->off;
        size_t want = total - got;
        size_t n = avail < want ? avail : want;
        size_t done = 0;
        while (done < n && vi < niov) {
            size_t room = iov[vi].len - voff;
            size_t c = n - done < room ? n - done : room;
            if (c && copy_out(iov[vi].base + voff, g->data + g->off + done, c)) {
                irq_restore(irq);
                return got ? (int64_t)got : ZK_EFAULT;
            }
            done += c;
            voff += c;
            if (voff == iov[vi].len) {
                vi++;
                voff = 0;
            }
        }
        got += done;
        if (g->nfds && control && !(flags & MSG_PEEK))
            cused += give_fds(g, control + cused, cap - cused, flags, &ctrunc);
        else if (g->nfds && !(flags & MSG_PEEK))
            ctrunc = true;

        if (flags & MSG_PEEK) {
            if (!stream || got >= total)
                break;
            g = g->next;
            continue;
        }
        if (!stream) {
            if (done < avail)
                *mflags |= MSG_TRUNC;
            s->qh = g->next;
            if (!s->qh)
                s->qt = NULL;
            s->qbytes -= g->len;
            free_seg(g);
            break;
        }
        g->off += done;
        s->qbytes -= done;
        if (g->off < g->len)
            break;
        s->qh = g->next;
        if (!s->qh)
            s->qt = NULL;
        free_seg(g);
        g = s->qh;
        if (got >= total)
            break;
    }
    irq_restore(irq);
    // the sender may be waiting for room
    wake(s->peer);
    wake(s);
    if (ctrunc)
        *mflags |= MSG_CTRUNC;
    if (clen)
        *clen = cused;
    if (!stream && (flags & MSG_TRUNC) && (*mflags & MSG_TRUNC))
        return (int64_t)total;    // what was really there isn't kept, close enough
    return (int64_t)got;
}

// vnode ops, for read, write and poll on the fd

static int64_t u_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct usock *s = (struct usock *)v;
    uint64_t irq = irq_save();
    for (;;) {
        if (s->qh)
            break;
        if (s->rd_shut || s->peer_wr_shut || s->state == US_DISCONNECTED) {
            irq_restore(irq);
            return 0;
        }
        if (s->state != US_CONNECTED && s->type == SOCK_STREAM) {
            irq_restore(irq);
            return ZK_ENOTCONN;
        }
        if (s->nonblock) {
            irq_restore(irq);
            return ZK_EAGAIN;
        }
        if (proc_killed()) {
            irq_restore(irq);
            return ZK_EINTR;
        }
        waitq_sleep(&s->wq);
    }
    size_t got = 0;
    uint8_t *dst = buf;
    while (s->qh && got < len) {
        struct seg *g = s->qh;
        if (got && g->nfds)
            break;
        size_t n = g->len - g->off < len - got ? g->len - g->off : len - got;
        memcpy(dst + got, g->data + g->off, n);
        got += n;
        if (s->type != SOCK_STREAM) {
            s->qh = g->next;
            if (!s->qh)
                s->qt = NULL;
            s->qbytes -= g->len;
            free_seg(g);
            break;
        }
        g->off += n;
        s->qbytes -= n;
        if (g->off < g->len)
            break;
        s->qh = g->next;
        if (!s->qh)
            s->qt = NULL;
        free_seg(g);    // passed fds read with plain read() are dropped
    }
    irq_restore(irq);
    // writers wait on the queue they're filling, which is this one
    wake(s);
    wake(s->peer);
    return (int64_t)got;
}

static int64_t u_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct usock *s = (struct usock *)v;
    if (s->type == SOCK_DGRAM && !s->peer && s->dgram_peer_len == 0)
        return LEDESTADDRREQ;
    size_t sent = 0;
    const uint8_t *src = buf;
    while (sent < len) {
        size_t want = len - sent;
        if (s->type == SOCK_STREAM && want > QUEUE_MAX / 2)
            want = QUEUE_MAX / 2;
        uint64_t irq = irq_save();
        struct usock *to = s->peer;
        if (!to && s->type == SOCK_DGRAM && s->dgram_peer_len) {
            if (s->dgram_peer[0])
                find_path(s->dgram_peer, &to);
            else
                to = find_abstract(s->dgram_peer, s->dgram_peer_len);
        }
        if (!to || s->wr_shut || to->rd_shut) {
            irq_restore(irq);
            if (sent)
                return (int64_t)sent;
            if (s->state == US_NEW)
                return ZK_ENOTCONN;
            sigpipe(0);
            return ZK_EPIPE;
        }
        if (!has_room(to, want)) {
            if (s->nonblock) {
                irq_restore(irq);
                return sent ? (int64_t)sent : ZK_EAGAIN;
            }
            if (proc_killed()) {
                irq_restore(irq);
                return sent ? (int64_t)sent : ZK_EINTR;
            }
            waitq_sleep(&to->wq);
            irq_restore(irq);
            continue;
        }
        struct seg *g = kzalloc(sizeof(*g) + want);
        if (!g) {
            irq_restore(irq);
            return sent ? (int64_t)sent : ZK_ENOMEM;
        }
        memcpy(g->data, src + sent, want);
        g->len = want;
        g->cred = my_cred();
        memcpy(g->from, s->name, (size_t)s->namelen);
        g->fromlen = s->namelen;
        enqueue(to, g);
        irq_restore(irq);
        sent += want;
    }
    return (int64_t)sent;
}

static int u_poll(struct vnode *v)
{
    struct usock *s = (struct usock *)v;
    int r = 0;
    if (s->state == US_LISTEN)
        return s->pending ? POLLIN : 0;
    if (s->qh)
        r |= POLLIN;
    if (s->peer_wr_shut || s->rd_shut)
        r |= POLLIN;
    if (s->type != SOCK_DGRAM && s->state == US_DISCONNECTED)
        r |= POLLIN | POLLHUP;
    if (s->type == SOCK_DGRAM) {
        r |= POLLOUT;
    } else if (s->peer && has_room(s->peer, 1) && !s->wr_shut) {
        r |= POLLOUT;
    } else if (s->state == US_NEW) {
        r |= POLLOUT | POLLHUP;
    }
    return r;
}

static int u_ioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)f;
    struct usock *s = (struct usock *)v;
    if (req == 0x541B) {    // FIONREAD
        int32_t n = 0;
        if (s->qh)
            n = (int32_t)(s->type == SOCK_STREAM ? s->qbytes : s->qh->len - s->qh->off);
        return copy_out(arg, &n, 4);
    }
    if (req == 0x5421) {    // FIONBIO
        int32_t on;
        if (copy_in(&on, arg, 4))
            return ZK_EFAULT;
        s->nonblock = on != 0;
        return 0;
    }
    return ZK_ENOTTY;
}

static const struct vnode_ops usock_ops = {
    .read = u_read, .write = u_write, .poll = u_poll, .ioctl = u_ioctl,
    .release = u_release,
};

void unix_set_nonblock(struct vnode *v, bool on)
{
    if (v->ops == &usock_ops)
        ((struct usock *)v)->nonblock = on;
}

bool unix_vnode(struct vnode *v)
{
    return v->ops == &usock_ops;
}

// syscalls

static int sock_type(int type)
{
    int t = type & 0xf;
    return t == SOCK_STREAM || t == SOCK_DGRAM || t == SOCK_SEQPACKET ? t : -1;
}

int64_t unix_socket(int type, int proto)
{
    if (proto != 0)
        return LEAFNOSUPPORT;
    int t = sock_type(type);
    if (t < 0)
        return LEPROTOTYPE;
    struct usock *s = new_usock(t);
    if (!s)
        return ZK_ENOMEM;
    return install(s, type);
}

int64_t unix_socketpair(int type, int proto, uint64_t usv)
{
    if (proto != 0)
        return LEAFNOSUPPORT;
    int t = sock_type(type);
    if (t < 0)
        return LEPROTOTYPE;
    if (!user_ok(usv, 8, true))
        return ZK_EFAULT;
    struct usock *a = new_usock(t), *b = new_usock(t);
    if (!a || !b) {
        kfree(a);
        kfree(b);
        return ZK_ENOMEM;
    }
    a->peer = b;
    b->peer = a;
    a->state = b->state = US_CONNECTED;
    a->peercred = b->peercred = my_cred();
    a->has_peercred = b->has_peercred = true;
    int fa = install(a, type);
    if (fa < 0) {
        vnode_put(&b->v);
        return fa;
    }
    int fb = install(b, type);
    if (fb < 0) {
        struct proc *p = proc_current();
        file_put(p->fds[fa]);
        p->fds[fa] = NULL;
        return fb;
    }
    int32_t sv[2] = { fa, fb };
    copy_out(usv, sv, 8);
    return 0;
}

static int64_t u_listen(struct usock *s, int backlog)
{
    if (s->type == SOCK_DGRAM)
        return LEOPNOTSUPP;
    if (s->state != US_NEW && s->state != US_LISTEN)
        return ZK_EINVAL;
    if (!s->namelen)
        return ZK_EINVAL;    // linux autobinds, nothing here needs that
    s->state = US_LISTEN;
    s->backlog = backlog < 1 ? 1 : backlog > 4096 ? 4096 : backlog;
    s->mycred = my_cred();
    return 0;
}

static int64_t u_connect(struct usock *s, struct file *f, uint64_t uaddr, uint32_t alen)
{
    if (s->type == SOCK_DGRAM) {
        char key[SUN_PATH + 1], path[ZK_PATH_MAX];
        int keylen;
        int err = get_name(uaddr, alen, key, &keylen, path);
        if (err)
            return err;
        struct usock *dst;
        err = key[0] ? find_path(path, &dst)
                     : (dst = find_abstract(key, keylen)) ? 0 : ZK_ECONNREFUSED;
        if (err)
            return err;
        if (key[0])
            strlcpy(s->dgram_peer, path, sizeof s->dgram_peer);
        else
            memcpy(s->dgram_peer, key, (size_t)keylen);
        s->dgram_peer_len = key[0] ? (int)strlen(path) + 1 : keylen;
        return 0;
    }
    if (s->state == US_CONNECTED || s->state == US_DISCONNECTED)
        return LEISCONN;
    if (s->state == US_LISTEN)
        return ZK_EINVAL;

    uint64_t irq = irq_save();
    for (;;) {
        struct usock *l;
        irq_restore(irq);
        int err = lookup(uaddr, alen, &l);
        irq = irq_save();
        if (err) {
            irq_restore(irq);
            return err;
        }
        if (l->type != s->type) {
            irq_restore(irq);
            return LEPROTOTYPE;
        }
        if (l->state != US_LISTEN) {
            irq_restore(irq);
            return ZK_ECONNREFUSED;
        }
        if (l->npending >= l->backlog) {
            if (!blocking(s, f, 0)) {
                irq_restore(irq);
                return ZK_EAGAIN;
            }
            if (proc_killed()) {
                irq_restore(irq);
                return ZK_EINTR;
            }
            waitq_sleep(&l->wq);
            continue;
        }
        // the server side exists right away, accept just hands it out
        struct usock *srv = new_usock(s->type);
        if (!srv) {
            irq_restore(irq);
            return ZK_ENOMEM;
        }
        memcpy(srv->name, l->name, (size_t)l->namelen);
        srv->namelen = l->namelen;
        srv->peer = s;
        s->peer = srv;
        srv->state = s->state = US_CONNECTED;
        srv->peercred = my_cred();
        srv->has_peercred = true;
        s->peercred = l->mycred;
        s->has_peercred = true;
        struct usock **pp = &l->pending;
        while (*pp)
            pp = &(*pp)->pnext;
        *pp = srv;
        l->npending++;
        wake(l);
        irq_restore(irq);
        return 0;
    }
}

static int64_t u_accept(struct usock *l, struct file *f, uint64_t uaddr, uint64_t ualen,
                        int flags)
{
    if (l->state != US_LISTEN)
        return ZK_EINVAL;
    uint64_t irq = irq_save();
    while (!l->pending) {
        if (!blocking(l, f, 0)) {
            irq_restore(irq);
            return ZK_EAGAIN;
        }
        if (proc_killed()) {
            irq_restore(irq);
            return ZK_EINTR;
        }
        waitq_sleep(&l->wq);
    }
    struct usock *s = l->pending;
    l->pending = s->pnext;
    s->pnext = NULL;
    l->npending--;
    wake(l);
    irq_restore(irq);
    int fd = install(s, flags);
    if (fd < 0)
        return fd;
    // the client's address, unnamed unless it bound one
    struct usock *c = s->peer;
    if (uaddr)
        put_name(c ? c->name : "", c ? c->namelen : 0, uaddr, ualen);
    return fd;
}

static int64_t u_shutdown(struct usock *s, int how)
{
    if (how < 0 || how > 2)
        return ZK_EINVAL;
    uint64_t irq = irq_save();
    if (how != 1)
        s->rd_shut = true;
    if (how != 0) {
        s->wr_shut = true;
        if (s->peer)
            s->peer->peer_wr_shut = true;
    }
    wake(s);
    wake(s->peer);
    irq_restore(irq);
    return 0;
}

static int64_t u_getsockopt(struct usock *s, int level, int opt, uint64_t uval, uint64_t ulen)
{
    if (level != SOL_SOCKET)
        return LENOPROTOOPT;
    uint32_t have;
    if (copy_in(&have, ulen, 4))
        return ZK_EFAULT;
    int32_t v = 0;
    switch (opt) {
    case SO_PEERCRED: {
        if (!s->has_peercred)
            return ZK_ENOTCONN;
        uint32_t n = have < sizeof(struct ucred) ? have : sizeof(struct ucred);
        if (copy_out(uval, &s->peercred, n) || copy_out(ulen, &n, 4))
            return ZK_EFAULT;
        return 0;
    }
    case SO_TYPE:       v = s->type; break;
    case SO_ERROR:      v = 0; break;
    case SO_SNDBUF:
    case SO_RCVBUF:     v = QUEUE_MAX; break;
    case SO_ACCEPTCONN: v = s->state == US_LISTEN; break;
    case SO_DOMAIN:     v = AF_UNIX; break;
    case SO_PASSCRED:   v = 0; break;
    default:            return LENOPROTOOPT;
    }
    uint32_t n = have < 4 ? have : 4;
    if (copy_out(uval, &v, n) || copy_out(ulen, &n, 4))
        return ZK_EFAULT;
    return 0;
}

struct msghdr {
    uint64_t name;
    uint32_t namelen, pad;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control, controllen;
    int32_t flags, pad2;
};

#define MAX_IOV 64

static int get_iov(uint64_t uiov, uint64_t n, struct iov *out)
{
    if (n > MAX_IOV)
        return ZK_EINVAL;
    return n ? copy_in(out, uiov, n * sizeof(*out)) : 0;
}

static int64_t u_sendmsg(struct usock *s, struct file *f, uint64_t umsg, int flags)
{
    struct msghdr m;
    if (copy_in(&m, umsg, sizeof m))
        return ZK_EFAULT;
    struct iov iov[MAX_IOV];
    int err = get_iov(m.iov, m.iovlen, iov);
    if (err)
        return err;
    return u_send(s, f, iov, (int)m.iovlen, m.control, m.controllen, flags,
                  m.name, m.namelen);
}

static int64_t u_recvmsg(struct usock *s, struct file *f, uint64_t umsg, int flags)
{
    struct msghdr m;
    if (copy_in(&m, umsg, sizeof m))
        return ZK_EFAULT;
    struct iov iov[MAX_IOV];
    int err = get_iov(m.iov, m.iovlen, iov);
    if (err)
        return err;
    uint64_t clen = m.controllen;
    int mflags = 0;
    // msg_namelen sits at offset 8 and is in/out
    int64_t r = u_recv(s, f, iov, (int)m.iovlen, m.control, &clen, flags, &mflags,
                       m.name, m.name ? umsg + 8 : 0);
    if (r < 0)
        return r;
    if (!m.name) {
        uint32_t zero = 0;
        copy_out(umsg + 8, &zero, 4);
    }
    if (copy_out(umsg + 40, &clen, 8) || copy_out(umsg + 48, &mflags, 4))
        return ZK_EFAULT;
    return r;
}

int64_t unix_call(struct file *f, uint64_t nr, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    struct usock *s = usock_of(f);
    if (!s)
        return ZK_ENOTSOCK;
    switch (nr) {
    case 42: return u_connect(s, f, a2, (uint32_t)a3);
    case 43: return u_accept(s, f, a2, a3, 0);
    case 288: return u_accept(s, f, a2, a3, (int)a4);
    case 44: {
        struct iov v = { a2, a3 };
        return u_send(s, f, &v, 1, 0, 0, (int)a4, a5, (uint32_t)a6);
    }
    case 45: {
        struct iov v = { a2, a3 };
        int mflags = 0;
        return u_recv(s, f, &v, 1, 0, NULL, (int)a4, &mflags, a5, a6);
    }
    case 46: return u_sendmsg(s, f, a2, (int)a3);
    case 47: return u_recvmsg(s, f, a2, (int)a3);
    case 48: return u_shutdown(s, (int)a2);
    case 49: return u_bind(s, a2, (uint32_t)a3);
    case 50: return u_listen(s, (int)a2);
    case 51: return put_name(s->name, s->namelen, a2, a3);
    case 52: {
        struct usock *p = s->peer;
        if (!p && !s->dgram_peer_len)
            return ZK_ENOTCONN;
        if (p)
            return put_name(p->name, p->namelen, a2, a3);
        return put_name(s->dgram_peer, s->dgram_peer_len, a2, a3);
    }
    case 54: return 0;    // setsockopt, SO_PASSCRED and buffer sizes are fine as is
    case 55: return u_getsockopt(s, (int)a2, (int)a3, a4, a5);
    }
    return ZK_ENOSYS;
}
