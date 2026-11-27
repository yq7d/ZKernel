// sockets, dns and a small http client for zos programs
#include "zk.h"

static inline long syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "memory");
    return ret;
}

int socket(int domain, int type) { return (int)syscall6(SYS_SOCKET, domain, type, 0, 0, 0, 0); }
int connect(int fd, const struct zk_sockaddr_in *a) { return (int)syscall6(SYS_CONNECT, fd, (long)a, sizeof *a, 0, 0, 0); }
int bind(int fd, const struct zk_sockaddr_in *a) { return (int)syscall6(SYS_BIND, fd, (long)a, sizeof *a, 0, 0, 0); }
long sendto(int fd, const void *buf, size_t len, const struct zk_sockaddr_in *a)
{
    return syscall6(SYS_SENDTO, fd, (long)buf, (long)len, 0, (long)a, a ? sizeof *a : 0);
}
long recvfrom(int fd, void *buf, size_t len, struct zk_sockaddr_in *a)
{
    uint32_t alen = sizeof *a;
    return syscall6(SYS_RECVFROM, fd, (long)buf, (long)len, 0, (long)a, a ? (long)&alen : 0);
}
int shutdown(int fd, int how) { return (int)syscall6(SYS_SHUTDOWN, fd, how, 0, 0, 0, 0); }
int netinfo(struct zk_netinfo *ni) { return (int)syscall6(SYS_NETINFO, (long)ni, 0, 0, 0, 0, 0); }
long ping(uint32_t ip, uint64_t timeout_ms) { return syscall6(SYS_PING, ip, (long)timeout_ms, 0, 0, 0, 0); }

uint16_t net16(uint16_t v) { return (uint16_t)(v << 8 | v >> 8); }
uint32_t net32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

bool inet_parse(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        if (!isdigit(*s))
            return false;
        long n = strtol(s, (char **)&s, 10);
        if (n < 0 || n > 255)
            return false;
        v = v << 8 | (uint32_t)n;
        if (part < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *ip = v;
    return true;
}

void inet_format(uint32_t ip, char *out, size_t size)
{
    snprintf(out, size, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static bool from_hosts(const char *name, uint32_t *ip)
{
    int fd = open("/etc/hosts", O_RDONLY);
    if (fd < 0)
        return false;
    struct lreader *r = lr_open(fd);
    char *line;
    bool found = false;
    while (!found && lr_getline(r, &line) >= 0) {
        char *p = line;
        while (isspace(*p))
            p++;
        if (*p == '#' || !*p)
            continue;
        char *addr = p;
        while (*p && !isspace(*p))
            p++;
        if (!*p)
            continue;
        *p++ = '\0';
        uint32_t a;
        if (!inet_parse(addr, &a))
            continue;
        while (*p) {
            while (isspace(*p))
                p++;
            char *n = p;
            while (*p && !isspace(*p))
                p++;
            if (*p)
                *p++ = '\0';
            if (*n && strcasecmp(n, name) == 0) {
                *ip = a;
                found = true;
                break;
            }
        }
    }
    lr_close(r);
    close(fd);
    return found;
}

static uint32_t nameserver(void)
{
    uint32_t ip = 0;
    int fd = open("/etc/resolv.conf", O_RDONLY);
    if (fd < 0)
        return 0;
    struct lreader *r = lr_open(fd);
    char *line;
    while (!ip && lr_getline(r, &line) >= 0) {
        if (strncmp(line, "nameserver", 10) != 0)
            continue;
        char *p = line + 10;
        while (isspace(*p))
            p++;
        char *end = p;
        while (*end && !isspace(*end))
            end++;
        *end = '\0';
        inet_parse(p, &ip);
    }
    lr_close(r);
    close(fd);
    return ip;
}

// skips a possibly compressed name in a dns message
static int skip_name(const uint8_t *m, int len, int off)
{
    while (off < len) {
        uint8_t l = m[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xc0) == 0xc0)
            return off + 2;
        off += l + 1;
    }
    return -1;
}

static int dns_query(uint32_t server, const char *name, uint32_t *ip)
{
    uint8_t q[512];
    uint16_t id;
    getrandom(&id, sizeof id);
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;    // recursion desired
    q[5] = 1;       // one question
    int n = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int l = dot ? (int)(dot - p) : (int)strlen(p);
        if (l == 0 || l > 63 || n + l + 6 > (int)sizeof q)
            return ZK_EINVAL;
        q[n++] = (uint8_t)l;
        memcpy(q + n, p, (size_t)l);
        n += l;
        p += l;
        if (*p == '.')
            p++;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;    // type A
    q[n++] = 0; q[n++] = 1;    // class IN

    int fd = socket(AF_INET, SOCK_DGRAM);
    if (fd < 0)
        return fd;
    struct zk_sockaddr_in to = { AF_INET, net16(53), net32(server), { 0 } };
    int err = ZK_ETIMEDOUT;
    for (int tries = 0; tries < 3 && err == ZK_ETIMEDOUT; tries++) {
        sendto(fd, q, (size_t)n, &to);
        struct zk_pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0)
            continue;
        uint8_t r[1024];
        long got = recvfrom(fd, r, sizeof r, NULL);
        if (got < 12 || r[0] != q[0] || r[1] != q[1])
            continue;
        if ((r[3] & 0x0f) != 0) {
            err = ZK_ENOENT;    // nxdomain and friends
            break;
        }
        int qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7];
        int off = 12;
        for (int i = 0; i < qd && off > 0; i++) {
            off = skip_name(r, (int)got, off);
            off += 4;
        }
        err = ZK_ENOENT;
        // cname records come first, the first A record is the answer
        for (int i = 0; i < an && off > 0 && off < got; i++) {
            off = skip_name(r, (int)got, off);
            if (off < 0 || off + 10 > got)
                break;
            int type = r[off] << 8 | r[off + 1];
            int rdlen = r[off + 8] << 8 | r[off + 9];
            off += 10;
            if (type == 1 && rdlen == 4 && off + 4 <= got) {
                *ip = (uint32_t)r[off] << 24 | (uint32_t)r[off + 1] << 16 |
                      (uint32_t)r[off + 2] << 8 | r[off + 3];
                err = 0;
                break;
            }
            off += rdlen;
        }
    }
    close(fd);
    return err;
}

int resolve(const char *host, uint32_t *ip)
{
    if (inet_parse(host, ip))
        return 0;
    if (strcmp(host, "localhost") == 0) {
        *ip = 0x7f000001;
        return 0;
    }
    if (from_hosts(host, ip))
        return 0;
    uint32_t ns = nameserver();
    if (!ns)
        return ZK_ENETUNREACH;
    return dns_query(ns, host, ip);
}

int tcp_dial(const char *host, uint16_t port)
{
    uint32_t ip;
    int err = resolve(host, &ip);
    if (err)
        return err;
    int fd = socket(AF_INET, SOCK_STREAM);
    if (fd < 0)
        return fd;
    struct zk_sockaddr_in a = { AF_INET, net16(port), net32(ip), { 0 } };
    err = connect(fd, &a);
    if (err) {
        close(fd);
        return err;
    }
    return fd;
}

// http

struct conn {
    int fd;
    uint8_t buf[8192];
    size_t pos, len;
};

static long c_fill(struct conn *c)
{
    if (c->pos < c->len)
        return (long)(c->len - c->pos);
    long n = read(c->fd, c->buf, sizeof c->buf);
    c->pos = 0;
    c->len = n > 0 ? (size_t)n : 0;
    return n;
}

static long c_line(struct conn *c, char *out, size_t size)
{
    size_t n = 0;
    for (;;) {
        if (c_fill(c) <= 0)
            return n ? (long)n : -1;
        char ch = (char)c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (ch != '\r' && n + 1 < size)
            out[n++] = ch;
    }
    out[n] = '\0';
    return (long)n;
}

// copies len bytes, or until eof with len -1
static long c_copy(struct conn *c, long len, int out, struct http_progress *pr)
{
    long total = 0;
    while (len < 0 || total < len) {
        long avail = c_fill(c);
        if (avail <= 0)
            return len < 0 ? total : ZK_EIO;
        if (len >= 0 && avail > len - total)
            avail = len - total;
        long w = write_all(out, c->buf + c->pos, (size_t)avail);
        if (w < 0)
            return w;
        c->pos += (size_t)avail;
        total += avail;
        if (pr) {
            pr->done += (uint64_t)avail;
            if (pr->fn)
                pr->fn(pr);
        }
    }
    return total;
}

static bool split_url(const char *url, char *host, size_t hsize, uint16_t *port, char *path, size_t psize)
{
    if (strncmp(url, "http://", 7) != 0)
        return false;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl == 0 || hl >= hsize)
        return false;
    memcpy(host, h, hl);
    host[hl] = '\0';
    *port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)strtol(colon + 1, NULL, 10);
    }
    strlcpy(path, slash ? slash : "/", psize);
    return true;
}

int http_get(const char *url, int out, struct http_progress *pr)
{
    char cur[1024];
    strlcpy(cur, url, sizeof cur);
    for (int redirects = 0; redirects < 5; redirects++) {
        char host[256], path[768];
        uint16_t port;
        if (!split_url(cur, host, sizeof host, &port, path, sizeof path))
            return strncmp(cur, "https://", 8) == 0 ? ZK_ENOSYS : ZK_EINVAL;
        int fd = tcp_dial(host, port);
        if (fd < 0)
            return fd;
        char req[1200];
        int n = snprintf(req, sizeof req,
                         "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: zos-fetch/1.0\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
        if (write_all(fd, req, (size_t)n) < 0) {
            close(fd);
            return ZK_EIO;
        }

        struct conn *c = malloc(sizeof *c);
        if (!c) {
            close(fd);
            return ZK_ENOMEM;
        }
        c->fd = fd;
        c->pos = c->len = 0;
        char line[1024];
        int status = 0;
        long length = -1;
        bool chunked = false;
        char location[1024] = "";
        if (c_line(c, line, sizeof line) < 0 || strncmp(line, "HTTP/1.", 7) != 0) {
            free(c);
            close(fd);
            return ZK_EIO;
        }
        status = (int)strtol(line + 9, NULL, 10);
        while (c_line(c, line, sizeof line) > 0) {
            if (strncasecmp(line, "content-length:", 15) == 0)
                length = strtol(line + 15, NULL, 10);
            else if (strncasecmp(line, "transfer-encoding:", 18) == 0 && strstr(line, "chunked"))
                chunked = true;
            else if (strncasecmp(line, "location:", 9) == 0) {
                const char *v = line + 9;
                while (*v == ' ')
                    v++;
                strlcpy(location, v, sizeof location);
            }
        }
        if (pr) {
            pr->status = status;
            pr->total = length > 0 ? (uint64_t)length : 0;
            pr->done = 0;
        }
        if (status >= 300 && status < 400 && location[0]) {
            free(c);
            close(fd);
            if (location[0] == '/') {
                char base[300];
                snprintf(base, sizeof base, "http://%s:%u", host, port);
                snprintf(cur, sizeof cur, "%s%s", base, location);
            } else {
                strlcpy(cur, location, sizeof cur);
            }
            continue;
        }
        long r = 0;
        if (status != 200) {
            r = ZK_ENOENT;
        } else if (chunked) {
            for (;;) {
                if (c_line(c, line, sizeof line) < 0) {
                    r = ZK_EIO;
                    break;
                }
                long size = strtol(line, NULL, 16);
                if (size <= 0)
                    break;
                r = c_copy(c, size, out, pr);
                if (r < 0)
                    break;
                c_line(c, line, sizeof line);
            }
        } else {
            r = c_copy(c, length, out, pr);
        }
        free(c);
        close(fd);
        return r < 0 ? (int)r : 0;
    }
    return ZK_ELOOP;
}
