#include "zk.h"

// one line of a colon separated file into fields, in place
static int split(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++) {
        if (*p == ':') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

// calls fn on every line, stops when it returns true
static bool each_line(const char *path, bool (*fn)(char *line, void *ctx), void *ctx)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct lreader *r = lr_open(fd);
    char *line;
    bool hit = false;
    while (!hit && lr_getline(r, &line) >= 0)
        if (line[0] && line[0] != '#')
            hit = fn(line, ctx);
    lr_close(r);
    close(fd);
    return hit;
}

struct pw_find {
    const char *name;
    long uid;
    struct passwd *out;
};

static bool pw_match(char *line, void *ctx)
{
    struct pw_find *q = ctx;
    char *f[7];
    if (split(line, f, 7) < 7)
        return false;
    uint32_t uid = (uint32_t)strtol(f[2], NULL, 10);
    if ((q->name && strcmp(f[0], q->name) != 0) || (!q->name && uid != q->uid))
        return false;
    strlcpy(q->out->name, f[0], sizeof q->out->name);
    q->out->uid = uid;
    q->out->gid = (uint32_t)strtol(f[3], NULL, 10);
    strlcpy(q->out->gecos, f[4], sizeof q->out->gecos);
    strlcpy(q->out->home, f[5], sizeof q->out->home);
    strlcpy(q->out->shell, f[6], sizeof q->out->shell);
    return true;
}

int getpwnam(const char *name, struct passwd *pw)
{
    struct pw_find q = { name, 0, pw };
    return each_line("/etc/passwd", pw_match, &q) ? 0 : ZK_ENOENT;
}

int getpwuid(uint32_t uid, struct passwd *pw)
{
    struct pw_find q = { NULL, uid, pw };
    return each_line("/etc/passwd", pw_match, &q) ? 0 : ZK_ENOENT;
}

struct gr_find {
    const char *name;
    long gid;
    char *out;
    size_t size;
};

static bool gr_match(char *line, void *ctx)
{
    struct gr_find *q = ctx;
    char *f[4];
    if (split(line, f, 4) < 3)
        return false;
    if (q->name ? strcmp(f[0], q->name) == 0 : strtol(f[2], NULL, 10) == q->gid) {
        q->gid = strtol(f[2], NULL, 10);
        if (q->out)
            strlcpy(q->out, f[0], q->size);
        return true;
    }
    return false;
}

long group_id(const char *name)
{
    struct gr_find q = { name, -1, NULL, 0 };
    return each_line("/etc/group", gr_match, &q) ? q.gid : -1;
}

const char *group_name(uint32_t gid, char *buf, size_t size)
{
    struct gr_find q = { NULL, gid, buf, size };
    if (!each_line("/etc/group", gr_match, &q))
        snprintf(buf, size, "%u", gid);
    return buf;
}

struct member_find {
    const char *user;
    uint32_t *out;
    int n, max;
};

static bool member_match(char *line, void *ctx)
{
    struct member_find *q = ctx;
    char *f[4];
    if (split(line, f, 4) < 4)
        return false;
    for (char *p = f[3]; *p && q->n < q->max; ) {
        char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        uint32_t gid = (uint32_t)strtol(f[2], NULL, 10);
        bool dup = false;
        for (int i = 0; i < q->n; i++)
            dup |= q->out[i] == gid;
        if (!dup && len == strlen(q->user) && strncmp(p, q->user, len) == 0)
            q->out[q->n++] = gid;
        if (!end)
            break;
        p = end + 1;
    }
    return false;
}

int user_groups(const char *user, uint32_t primary, uint32_t *out, int max)
{
    struct member_find q = { user, out, 0, max };
    if (max > 0)
        out[q.n++] = primary;
    each_line("/etc/group", member_match, &q);
    return q.n;
}

int become_user(const struct passwd *pw)
{
    uint32_t groups[ZK_NGROUPS];
    int n = user_groups(pw->name, pw->gid, groups, ZK_NGROUPS);
    int err = setgroups((size_t)n, groups);
    if (!err)
        err = setgid(pw->gid);
    if (!err)
        err = setuid(pw->uid);
    if (err)
        return err;
    setenv("HOME", pw->home, 1);
    setenv("USER", pw->name, 1);
    setenv("LOGNAME", pw->name, 1);
    setenv("SHELL", pw->shell, 1);
    return 0;
}

#define ROUNDS 5000

void hash_password(const char *password, const char *salt, char *out, size_t size)
{
    uint8_t h[32];
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, salt, strlen(salt));
    sha256_update(&s, password, strlen(password));
    sha256_final(&s, h);
    for (int i = 1; i < ROUNDS; i++) {
        sha256_init(&s);
        sha256_update(&s, h, sizeof h);
        sha256_update(&s, salt, strlen(salt));
        sha256_update(&s, password, strlen(password));
        sha256_final(&s, h);
    }
    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", h[i]);
    snprintf(out, size, "$5z$%s$%s", salt, hex);
}

struct shadow_check {
    const char *user, *password;
    bool ok;
};

static bool shadow_match(char *line, void *ctx)
{
    struct shadow_check *q = ctx;
    char *f[3];
    if (split(line, f, 3) < 2 || strcmp(f[0], q->user) != 0)
        return false;
    // $5z$salt$hash
    char salt[64];
    if (strncmp(f[1], "$5z$", 4) != 0)
        return true;    // locked or unknown scheme
    char *end = strchr(f[1] + 4, '$');
    if (!end || (size_t)(end - (f[1] + 4)) >= sizeof salt)
        return true;
    memcpy(salt, f[1] + 4, (size_t)(end - (f[1] + 4)));
    salt[end - (f[1] + 4)] = '\0';
    char want[128];
    hash_password(q->password, salt, want, sizeof want);
    // compare everything so the time doesn't give away how much matched
    size_t n = strlen(want);
    int diff = strlen(f[1]) != n;
    for (size_t i = 0; i < n && f[1][i]; i++)
        diff |= want[i] ^ f[1][i];
    q->ok = diff == 0;
    return true;
}

bool check_password(const char *user, const char *password)
{
    struct shadow_check q = { user, password, false };
    each_line("/etc/shadow", shadow_match, &q);
    return q.ok;
}

long read_password(const char *prompt, char *buf, size_t size)
{
    int fd = open("/dev/tty", O_RDWR);
    int in = fd >= 0 ? fd : 0;
    uint32_t mode = ZK_TTY_COOKED;
    bool tty = ioctl(in, TIOCGMODE, (uint64_t)&mode) == 0;
    if (tty)
        ioctl(in, TIOCSMODE, mode & ~ZK_TTY_ECHO);
    dprintf(in == 0 ? 2 : in, "%s", prompt);
    size_t n = 0;
    char c;
    while (read(in, &c, 1) == 1 && c != '\n')
        if (n + 1 < size)
            buf[n++] = c;
    buf[n] = '\0';
    if (tty)
        ioctl(in, TIOCSMODE, mode);
    dprintf(in == 0 ? 2 : in, "\n");
    if (fd >= 0)
        close(fd);
    return (long)n;
}
