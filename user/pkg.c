// pkg, installs alpine linux packages into /compat/linux
//
//   pkg update             fetch the package lists
//   pkg search <words>     look for packages
//   pkg info <name>        details of one package
//   pkg install <names>    install with dependencies
//   pkg remove <names>     uninstall
//   pkg upgrade            bring everything installed up to date
//   pkg list               what's installed
//   pkg files <name>       what a package put where
//   pkg clean              empty the download cache
#include "zk.h"

#define ROOT      "/compat/linux"
#define REPOS     "/etc/pkg/repositories"
#define DB_DIR    "/var/lib/pkg"
#define DB        DB_DIR "/installed"
#define CACHE     "/var/cache/pkg"
#define ARCH      "x86_64"
#define MAX_REPOS 8

struct pkgent {
    char *name, *version, *desc, *deps, *provides, *checksum, *url;
    const char *repo;
    uint64_t size, isize;
};

struct installed {
    char name[96], version[64];
    char *files;    // newline separated, relative to ROOT
    size_t flen, fcap;
    bool keep;
};

static char *repos[MAX_REPOS];
static int nrepos;
static struct pkgent *idx;
static size_t nidx, cidx;
static struct installed *db;
static size_t ndb;
static bool verbose;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dprintf(2, "pkg: ");
    vdprintf(2, fmt, ap);
    dprintf(2, "\n");
    va_end(ap);
    exit(1);
}

static void mkdirs(const char *path)
{
    char buf[ZK_PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(buf);
        *p = '/';
    }
    mkdir(buf);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct zk_stat st;
    fstat(fd, &st);
    uint8_t *buf = malloc(st.size + 1);
    size_t got = 0;
    long n;
    while (buf && got < st.size && (n = read(fd, buf + got, st.size - got)) > 0)
        got += (size_t)n;
    close(fd);
    if (buf)
        buf[got] = '\0';
    *len = got;
    return buf;
}

static int write_file(const char *path, const void *data, size_t len, uint32_t mode)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    long w = len ? write_all(fd, data, len) : 0;
    close(fd);
    chmod(path, mode);
    return w < 0 ? (int)w : 0;
}

static void load_repos(void)
{
    size_t len;
    char *text = (char *)read_file(REPOS, &len);
    if (!text)
        die("no %s", REPOS);
    char *save;
    for (char *line = strtok_r(text, "\n", &save); line && nrepos < MAX_REPOS;
         line = strtok_r(NULL, "\n", &save)) {
        while (isspace(*line))
            line++;
        if (*line && *line != '#')
            repos[nrepos++] = line;
    }
    if (!nrepos)
        die("%s lists no repositories", REPOS);
}

// pulls one entry out of an apkindex, fields point into the text
static void parse_index(char *text, const char *repo)
{
    struct pkgent cur;
    memset(&cur, 0, sizeof cur);
    char *line = text;
    for (;;) {
        char *nl = line ? strchr(line, '\n') : NULL;
        if (nl)
            *nl = '\0';
        if (!line || !*line) {
            if (cur.name && cur.version) {
                if (nidx == cidx) {
                    cidx = cidx ? cidx * 2 : 4096;
                    idx = realloc(idx, cidx * sizeof *idx);
                    if (!idx)
                        die("out of memory");
                }
                cur.repo = repo;
                idx[nidx++] = cur;
            }
            memset(&cur, 0, sizeof cur);
            if (!line || !nl)
                break;
        } else if (line[1] == ':') {
            char *v = line + 2;
            switch (line[0]) {
            case 'P': cur.name = v; break;
            case 'V': cur.version = v; break;
            case 'T': cur.desc = v; break;
            case 'D': cur.deps = v; break;
            case 'p': cur.provides = v; break;
            case 'C': cur.checksum = v; break;
            case 'S': cur.size = (uint64_t)strtol(v, NULL, 10); break;
            case 'I': cur.isize = (uint64_t)strtol(v, NULL, 10); break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void load_indexes(void)
{
    load_repos();
    int loaded = 0;
    for (int i = 0; i < nrepos; i++) {
        char path[ZK_PATH_MAX];
        snprintf(path, sizeof path, DB_DIR "/APKINDEX.%d", i);
        size_t len;
        char *text = (char *)read_file(path, &len);
        if (!text)
            continue;
        parse_index(text, repos[i]);
        loaded++;
    }
    if (!loaded)
        die("no package list, run: sudo pkg update");
    if (loaded < nrepos)
        dprintf(2, "pkg: warning: some package lists are missing, run: sudo pkg update\n");
}

// installed database

static struct installed *find_installed(const char *name)
{
    for (size_t i = 0; i < ndb; i++)
        if (strcmp(db[i].name, name) == 0)
            return &db[i];
    return NULL;
}

static void add_file(struct installed *in, const char *path)
{
    size_t n = strlen(path);
    if (in->flen + n + 2 > in->fcap) {
        in->fcap = in->fcap ? in->fcap * 2 + n : 1024 + n;
        in->files = realloc(in->files, in->fcap);
        if (!in->files)
            die("out of memory");
    }
    memcpy(in->files + in->flen, path, n);
    in->flen += n;
    in->files[in->flen++] = '\n';
    in->files[in->flen] = '\0';
}

static void load_db(void)
{
    size_t len;
    char *text = (char *)read_file(DB, &len);
    if (!text)
        return;
    struct installed *cur = NULL;
    char *save;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == 'P' && line[1] == ':') {
            db = realloc(db, (ndb + 1) * sizeof *db);
            if (!db)
                die("out of memory");
            cur = &db[ndb++];
            memset(cur, 0, sizeof *cur);
            strlcpy(cur->name, line + 2, sizeof cur->name);
        } else if (cur && line[0] == 'V' && line[1] == ':') {
            strlcpy(cur->version, line + 2, sizeof cur->version);
        } else if (cur && line[0] == 'F' && line[1] == ':') {
            add_file(cur, line + 2);
        }
    }
}

static void save_db(void)
{
    mkdirs(DB_DIR);
    char tmp[] = DB ".new";
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        die("can't write %s: %s", tmp, strerror(fd));
    for (size_t i = 0; i < ndb; i++) {
        if (!db[i].name[0])
            continue;
        dprintf(fd, "P:%s\nV:%s\n", db[i].name, db[i].version);
        for (char *f = db[i].files; f && *f; ) {
            char *nl = strchr(f, '\n');
            dprintf(fd, "F:%.*s\n", (int)(nl ? nl - f : (long)strlen(f)), f);
            f = nl ? nl + 1 : f + strlen(f);
        }
        dprintf(fd, "\n");
    }
    close(fd);
    rename(tmp, DB);
}

// resolving names

static struct pkgent *by_name(const char *name)
{
    for (size_t i = 0; i < nidx; i++)
        if (strcmp(idx[i].name, name) == 0)
            return &idx[i];
    return NULL;
}

// a dependency is "name", "so:libfoo.so.1" or "cmd:foo", maybe with a version
// constraint. versions are ignored, the newest in the list wins
static struct pkgent *provider(const char *dep)
{
    char want[160];
    size_t n = 0;
    while (dep[n] && !strchr("<>=~", dep[n]) && n + 1 < sizeof want) {
        want[n] = dep[n];
        n++;
    }
    want[n] = '\0';
    struct pkgent *p = by_name(want);
    if (p)
        return p;
    size_t wl = strlen(want);
    for (size_t i = 0; i < nidx; i++) {
        const char *prov = idx[i].provides;
        while (prov && *prov) {
            while (*prov == ' ')
                prov++;
            if (strncmp(prov, want, wl) == 0 && (prov[wl] == '=' || prov[wl] == ' ' || !prov[wl]))
                return &idx[i];
            while (*prov && *prov != ' ')
                prov++;
        }
    }
    return NULL;
}

static struct pkgent **plan;
static size_t nplan;
static struct pkgent *visiting[128];
static int nvisiting;

static bool planned(struct pkgent *p)
{
    for (size_t i = 0; i < nplan; i++)
        if (plan[i] == p)
            return true;
    return false;
}

static void add_to_plan(struct pkgent *p, bool force, int depth)
{
    if (depth > 100 || nvisiting == 128)
        die("dependencies of %s go too deep", p->name);
    if (planned(p))
        return;
    for (int i = 0; i < nvisiting; i++)
        if (visiting[i] == p)
            return;    // a loop, the first one in wins
    struct installed *in = find_installed(p->name);
    if (in && !force && strcmp(in->version, p->version) == 0)
        return;
    // depth first, so dependencies land before what needs them
    visiting[nvisiting++] = p;
    char deps[2048], *save;
    strlcpy(deps, p->deps ? p->deps : "", sizeof deps);
    for (char *d = strtok_r(deps, " ", &save); d; d = strtok_r(NULL, " ", &save)) {
        if (d[0] == '!')
            continue;    // conflicts
        struct pkgent *q = provider(d);
        if (!q) {
            dprintf(2, "pkg: warning: nothing provides %s (needed by %s)\n", d, p->name);
            continue;
        }
        if (q != p)
            add_to_plan(q, false, depth + 1);
    }
    nvisiting--;
    if (!planned(p)) {
        plan = realloc(plan, (nplan + 1) * sizeof *plan);
        plan[nplan++] = p;
    }
}

// archives

struct tarent {
    char path[ZK_PATH_MAX];
    char link[ZK_PATH_MAX];
    char type;
    uint32_t mode;
    const uint8_t *data;
    size_t size;
};

static uint64_t octal(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

// walks a tar stream, pax and gnu long names included
static void each_tar(const uint8_t *buf, size_t len, void (*fn)(struct tarent *, void *), void *ctx)
{
    size_t off = 0;
    char longname[ZK_PATH_MAX] = "", longlink[ZK_PATH_MAX] = "";
    while (off + 512 <= len) {
        const uint8_t *h = buf + off;
        if (!h[0]) {
            off += 512;
            continue;
        }
        uint64_t size = octal((const char *)h + 124, 12);
        char type = (char)h[156];
        const uint8_t *data = h + 512;
        size_t next = off + 512 + (size_t)((size + 511) & ~511UL);
        if (next > len)
            break;
        if (type == 'x') {
            // "len key=value\n" records
            const char *p = (const char *)data, *end = p + size;
            while (p < end) {
                long rl = strtol(p, NULL, 10);
                if (rl <= 0 || p + rl > end)
                    break;
                const char *kv = strchr(p, ' ');
                if (kv && kv < p + rl) {
                    kv++;
                    size_t vl = (size_t)(p + rl - kv) - 1;
                    if (strncmp(kv, "path=", 5) == 0 && vl - 5 < sizeof longname) {
                        memcpy(longname, kv + 5, vl - 5);
                        longname[vl - 5] = '\0';
                    } else if (strncmp(kv, "linkpath=", 9) == 0 && vl - 9 < sizeof longlink) {
                        memcpy(longlink, kv + 9, vl - 9);
                        longlink[vl - 9] = '\0';
                    }
                }
                p += rl;
            }
        } else if (type == 'L') {
            size_t n = size < sizeof longname ? size : sizeof longname - 1;
            memcpy(longname, data, n);
            longname[n] = '\0';
        } else if (type != 'g') {
            struct tarent e;
            if (longname[0]) {
                strlcpy(e.path, longname, sizeof e.path);
            } else {
                char prefix[156], name[101];
                memcpy(prefix, h + 345, 155);
                prefix[155] = '\0';
                memcpy(name, h, 100);
                name[100] = '\0';
                if (prefix[0])
                    snprintf(e.path, sizeof e.path, "%s/%s", prefix, name);
                else
                    strlcpy(e.path, name, sizeof e.path);
            }
            if (longlink[0]) {
                strlcpy(e.link, longlink, sizeof e.link);
            } else {
                memcpy(e.link, h + 157, 100);
                e.link[100] = '\0';
            }
            e.type = type ? type : '0';
            e.mode = (uint32_t)octal((const char *)h + 100, 8) & 07777;
            e.data = data;
            e.size = (size_t)size;
            fn(&e, ctx);
            longname[0] = longlink[0] = '\0';
        }
        off = next;
    }
}

struct apk {
    uint8_t *tar;        // all members decompressed back to back
    size_t len;
    char sum[64];        // Q1 + base64 sha1 of the control member
    char *post_install, *pre_install, *trigger, *pkginfo;
    size_t post_len, pre_len, trigger_len, pkginfo_len;
};

// apk triggers: a package's .trigger script runs once per transaction with
// the dirs, matching its globs, that packages put files in
#define TRIGGERS DB_DIR "/triggers"
#define MAX_TOUCHED 512
static char *touched[MAX_TOUCHED];
static int ntouched;

static void touch_dir(const char *dir)
{
    for (int i = 0; i < ntouched; i++)
        if (strcmp(touched[i], dir) == 0)
            return;
    if (ntouched < MAX_TOUCHED)
        touched[ntouched++] = strdup(dir);
}

// '*' doesn't cross a '/'
static bool glob_match(const char *g, const char *s)
{
    if (*g == '\0')
        return *s == '\0';
    if (*g == '*') {
        for (const char *p = s;; p++) {
            if (glob_match(g + 1, p))
                return true;
            if (*p == '\0' || *p == '/')
                return false;
        }
    }
    return *g == *s && glob_match(g + 1, s + 1);
}

static int open_apk(const uint8_t *raw, size_t rawlen, struct apk *a)
{
    memset(a, 0, sizeof *a);
    size_t off = 0;
    int member = 0;
    size_t starts[4], ends[4];
    while (off < rawlen && member < 4) {
        long used = gunzip_member(raw + off, rawlen - off, &a->tar, &a->len);
        if (used < 0)
            return (int)used;
        starts[member] = off;
        ends[member] = off + (size_t)used;
        off += (size_t)used;
        member++;
    }
    // signature, control, data. unsigned packages have no signature part
    int control = member == 3 ? 1 : 0;
    if (member <= control)
        return ZK_EINVAL;
    uint8_t digest[20];
    sha1(raw + starts[control], ends[control] - starts[control], digest);
    char b64[40];
    base64(digest, 20, b64);
    snprintf(a->sum, sizeof a->sum, "Q1%s", b64);
    return 0;
}

struct extract {
    struct installed *in;
    struct apk *apk;
    int files;
};

static void extract_one(struct tarent *e, void *ctx)
{
    struct extract *x = ctx;
    char *name = e->path;
    while (name[0] == '.' && name[1] == '/')
        name += 2;
    if (name[0] == '.') {
        // control files
        if (strcmp(name, ".post-install") == 0) {
            x->apk->post_install = (char *)e->data;
            x->apk->post_len = e->size;
        } else if (strcmp(name, ".trigger") == 0) {
            x->apk->trigger = (char *)e->data;
            x->apk->trigger_len = e->size;
        } else if (strcmp(name, ".PKGINFO") == 0) {
            x->apk->pkginfo = (char *)e->data;
            x->apk->pkginfo_len = e->size;
        } else if (strcmp(name, ".pre-install") == 0) {
            x->apk->pre_install = (char *)e->data;
            x->apk->pre_len = e->size;
        }
        return;
    }
    if (!x->in)
        return;
    char path[ZK_PATH_MAX];
    snprintf(path, sizeof path, ROOT "/%s", name);
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/')
        path[--pl] = '\0';
    char *slash = strrchr(path, '/');
    if (slash) {
        *slash = '\0';
        mkdirs(path);
        if (e->type != '5')
            touch_dir(path + strlen(ROOT));
        *slash = '/';
    }
    if (e->type == '5') {
        mkdirs(path);
        chmod(path, e->mode ? e->mode : 0755);
        return;
    }
    if (e->type == '2') {
        unlink(path);
        int err = symlink(e->link, path);
        if (err && verbose)
            dprintf(2, "pkg: symlink %s: %s\n", path, strerror(err));
    } else if (e->type == '0' || e->type == '7') {
        int err = write_file(path, e->data, e->size, e->mode ? e->mode : 0644);
        if (err)
            dprintf(2, "pkg: %s: %s\n", path, strerror(err));
    } else if (e->type == '1') {
        // hard link: copy the file it points at
        char from[ZK_PATH_MAX];
        snprintf(from, sizeof from, ROOT "/%s", e->link);
        size_t len;
        uint8_t *data = read_file(from, &len);
        if (data) {
            write_file(path, data, len, e->mode ? e->mode : 0644);
            free(data);
        }
    } else {
        return;
    }
    add_file(x->in, name);
    x->files++;
}

// runs with the interpreter from its #! line, inside the linux tree
static int run_script_args(const char *script, size_t len, const char *what,
                           const char *pkgname, char **args, int nargs);

static int run_script(const char *script, size_t len, const char *what, const char *pkgname)
{
    return run_script_args(script, len, what, pkgname, NULL, 0);
}

static int run_script_args(const char *script, size_t len, const char *what,
                           const char *pkgname, char **args, int nargs)
{
    if (!script)
        return 0;
    char interp[ZK_PATH_MAX] = "/bin/sh", arg[64] = "";
    if (len > 2 && script[0] == '#' && script[1] == '!') {
        const char *p = script + 2, *end = script + len;
        while (p < end && *p == ' ')
            p++;
        size_t n = 0;
        while (p < end && !isspace(*p) && n + 1 < sizeof interp)
            interp[n++] = *p++;
        interp[n] = '\0';
        while (p < end && *p == ' ')
            p++;
        n = 0;
        while (p < end && !isspace(*p) && n + 1 < sizeof arg)
            arg[n++] = *p++;
        arg[n] = '\0';
    }
    char real[ZK_PATH_MAX];
    snprintf(real, sizeof real, ROOT "%s", interp);
    struct zk_stat st;
    if (stat(real, &st) != 0) {
        if (verbose)
            dprintf(2, "pkg: no %s for the %s %s script\n", interp, pkgname, what);
        return 0;
    }
    char path[] = "/tmp/pkg-script";
    if (write_file(path, script, len, 0755) != 0)
        return -1;
    char **argv = malloc((size_t)(nargs + 4) * sizeof *argv);
    if (!argv)
        return -1;
    int n = 0;
    argv[n++] = interp;
    if (arg[0])
        argv[n++] = arg;
    argv[n++] = path;
    for (int i = 0; i < nargs; i++)
        argv[n++] = args[i];
    argv[n] = NULL;
    long pid = spawn(real, argv, NULL);
    free(argv);
    int status = -1;
    if (pid > 0)
        wait(pid, &status, 0);
    unlink(path);
    if (status != 0)
        dprintf(2, "pkg: %s %s script exited with %d\n", pkgname, what, status);
    return status;
}

static void remove_files(const char *files, const char *keep)
{
    if (!files)
        return;
    char *copy = strdup(files);
    if (!copy)
        return;
    char *save;
    for (char *f = strtok_r(copy, "\n", &save); f; f = strtok_r(NULL, "\n", &save)) {
        if (keep) {
            // still part of the new version
            size_t fl = strlen(f);
            const char *k = keep;
            bool still = false;
            while ((k = strstr(k, f))) {
                if ((k == keep || k[-1] == '\n') && (k[fl] == '\n' || !k[fl])) {
                    still = true;
                    break;
                }
                k += fl;
            }
            if (still)
                continue;
        }
        char path[ZK_PATH_MAX];
        snprintf(path, sizeof path, ROOT "/%s", f);
        unlink(path);
    }
    free(copy);
}

// progress bar for downloads
static const char *dl_name;
static uint64_t dl_last;

static void dl_progress(struct http_progress *p)
{
    uint64_t now = uptime_ms();
    if (now - dl_last < 150 && p->done < p->total)
        return;
    dl_last = now;
    char done[16];
    format_size(p->done, done, sizeof done);
    int pct = p->total ? (int)(p->done * 100 / p->total) : 0;
    dprintf(1, "\r  %-32s %6s %3d%%", dl_name, done, pct);
}

static int download(const char *url, const char *dest, const char *label, uint64_t expect)
{
    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    dl_name = label;
    dl_last = 0;
    struct http_progress p = { 0, expect, 0, dl_progress, NULL };
    int err = http_get(url, fd, &p);
    close(fd);
    dprintf(1, "\r  %-32s %s\n", label, err ? strerror(err) : "done");
    if (err)
        unlink(dest);
    return err;
}

// keeps a package's trigger script and globs for later transactions
static void save_trigger(const char *name, struct apk *a)
{
    char path[ZK_PATH_MAX];
    snprintf(path, sizeof path, TRIGGERS "/%s.trigger", name);
    char globs[1024] = "";
    for (const char *l = a->pkginfo; a->trigger && l && l < a->pkginfo + a->pkginfo_len;) {
        const char *end = l;
        while (end < a->pkginfo + a->pkginfo_len && *end != '\n')
            end++;
        if ((size_t)(end - l) > 11 && strncmp(l, "triggers = ", 11) == 0) {
            size_t n = (size_t)(end - l) - 11;
            if (n >= sizeof globs)
                n = sizeof globs - 1;
            memcpy(globs, l + 11, n);
            globs[n] = '\0';
        }
        l = end + 1;
    }
    if (!a->trigger || !globs[0]) {
        unlink(path);
        return;
    }
    mkdirs(TRIGGERS);
    write_file(path, a->trigger, a->trigger_len, 0755);
    snprintf(path, sizeof path, TRIGGERS "/%s.globs", name);
    write_file(path, globs, strlen(globs), 0644);
}

static void run_triggers(void)
{
    for (size_t i = 0; i < ndb && ntouched; i++) {
        if (!db[i].name[0])
            continue;
        char path[ZK_PATH_MAX];
        size_t glen, slen;
        snprintf(path, sizeof path, TRIGGERS "/%s.globs", db[i].name);
        char *globs = (char *)read_file(path, &glen);
        if (!globs)
            continue;
        snprintf(path, sizeof path, TRIGGERS "/%s.trigger", db[i].name);
        char *script = (char *)read_file(path, &slen);
        char *args[MAX_TOUCHED];
        int nargs = 0;
        for (int t = 0; t < ntouched; t++) {
            char *g = strdup(globs), *save;
            bool hit = false;
            for (char *w = g ? strtok_r(g, " \t\n", &save) : NULL; w && !hit;
                 w = strtok_r(NULL, " \t\n", &save))
                hit = glob_match(w, touched[t]);
            free(g);
            if (hit)
                args[nargs++] = touched[t];
        }
        if (nargs && script)
            run_script_args(script, slen, "trigger", db[i].name, args, nargs);
        free(globs);
        free(script);
    }
}

static void install_one(struct pkgent *p)
{
    char url[1024], file[ZK_PATH_MAX], label[96];
    snprintf(url, sizeof url, "%s/" ARCH "/%s-%s.apk", p->repo, p->name, p->version);
    snprintf(file, sizeof file, CACHE "/%s-%s.apk", p->name, p->version);
    snprintf(label, sizeof label, "%s %s", p->name, p->version);
    struct zk_stat st;
    if (stat(file, &st) != 0 || st.size != p->size) {
        int err = download(url, file, label, p->size);
        // mirrors hiccup now and then, a 404 included
        for (int tries = 1; err && tries < 3; tries++) {
            sleep_ms(1000);
            err = download(url, file, label, p->size);
        }
        if (err)
            die("can't download %s: %s", p->name, strerror(err));
    }

    size_t rawlen;
    uint8_t *raw = read_file(file, &rawlen);
    if (!raw)
        die("can't read %s", file);
    struct apk a;
    int err = open_apk(raw, rawlen, &a);
    if (err)
        die("%s: broken package (%s)", p->name, strerror(err));
    if (p->checksum && strcmp(a.sum, p->checksum) != 0) {
        unlink(file);
        die("%s: checksum mismatch, the download is bad or the list is stale (pkg update)", p->name);
    }

    // scripts first, they decide whether to go ahead
    struct extract x = { NULL, &a, 0 };
    each_tar(a.tar, a.len, extract_one, &x);
    if (a.pre_install)
        run_script(a.pre_install, a.pre_len, "pre-install", p->name);

    struct installed *old = find_installed(p->name);
    struct installed fresh;
    memset(&fresh, 0, sizeof fresh);
    strlcpy(fresh.name, p->name, sizeof fresh.name);
    strlcpy(fresh.version, p->version, sizeof fresh.version);
    x.in = &fresh;
    x.files = 0;
    each_tar(a.tar, a.len, extract_one, &x);
    if (old) {
        remove_files(old->files, fresh.files);
        free(old->files);
        *old = fresh;
    } else {
        db = realloc(db, (ndb + 1) * sizeof *db);
        db[ndb++] = fresh;
    }
    save_db();
    save_trigger(p->name, &a);
    if (a.post_install)
        run_script(a.post_install, a.post_len, "post-install", p->name);
    free(a.tar);
    free(raw);
}

static void need_root(const char *what)
{
    if (geteuid() != 0)
        die("%s needs root, try: sudo pkg %s", what, what);
}

struct find {
    const uint8_t *data;
    size_t size;
};

static void grab(struct tarent *e, void *ctx)
{
    struct find *f = ctx;
    if (strcmp(e->path, "APKINDEX") == 0) {
        f->data = e->data;
        f->size = e->size;
    }
}

// commands

static int cmd_update(void)
{
    need_root("update");
    load_repos();
    mkdirs(DB_DIR);
    mkdirs(CACHE);
    int total = 0;
    for (int i = 0; i < nrepos; i++) {
        char url[512], gz[ZK_PATH_MAX], out[ZK_PATH_MAX];
        snprintf(url, sizeof url, "%s/" ARCH "/APKINDEX.tar.gz", repos[i]);
        snprintf(gz, sizeof gz, CACHE "/APKINDEX.%d.tar.gz", i);
        snprintf(out, sizeof out, DB_DIR "/APKINDEX.%d", i);
        const char *slash = strrchr(repos[i], '/');
        int err = download(url, gz, slash ? slash + 1 : repos[i], 0);
        if (err) {
            dprintf(2, "pkg: %s: %s\n", repos[i], strerror(err));
            continue;
        }
        size_t rawlen;
        uint8_t *raw = read_file(gz, &rawlen);
        uint8_t *tar = NULL;
        size_t tlen = 0;
        size_t off = 0;
        while (raw && off < rawlen) {
            long used = gunzip_member(raw + off, rawlen - off, &tar, &tlen);
            if (used <= 0) {
                if (used < 0)
                    dprintf(2, "pkg: %s: can't unpack the list (%s)\n", repos[i], strerror((int)used));
                break;
            }
            off += (size_t)used;
        }
        free(raw);
        unlink(gz);
        // the file named APKINDEX inside
        struct find f = { NULL, 0 };
        if (tar)
            each_tar(tar, tlen, grab, &f);
        if (!f.data) {
            dprintf(2, "pkg: %s: no APKINDEX in the download\n", repos[i]);
            free(tar);
            continue;
        }
        write_file(out, f.data, f.size, 0644);
        int count = 0;
        for (size_t k = 0; k + 2 < f.size; k++)
            if (f.data[k] == '\n' && f.data[k + 1] == 'P' && f.data[k + 2] == ':')
                count++;
        total += count;
        free(tar);
    }
    printf("%d packages available\n", total);
    return 0;
}

static int cmd_search(int argc, char **argv)
{
    load_indexes();
    load_db();
    int shown = 0;
    for (size_t i = 0; i < nidx; i++) {
        bool all = true;
        for (int k = 0; k < argc && all; k++) {
            bool in_name = strstr(idx[i].name, argv[k]) != NULL;
            bool in_desc = false;
            if (!in_name && idx[i].desc) {
                size_t al = strlen(argv[k]);
                for (const char *d = idx[i].desc; *d && !in_desc; d++)
                    in_desc = strncasecmp(d, argv[k], al) == 0;
            }
            all = in_name || in_desc;
        }
        if (!all)
            continue;
        printf("%s%s\e[0m %s%s - %s\n", find_installed(idx[i].name) ? "\e[1;32m" : "\e[1m",
               idx[i].name, idx[i].version, find_installed(idx[i].name) ? " [installed]" : "",
               idx[i].desc ? idx[i].desc : "");
        shown++;
    }
    if (!shown) {
        printf("nothing found\n");
        return 1;
    }
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    load_indexes();
    load_db();
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct pkgent *p = by_name(argv[i]);
        if (!p) {
            dprintf(2, "pkg: no package %s\n", argv[i]);
            rc = 1;
            continue;
        }
        char size[16], isize[16];
        format_size(p->size, size, sizeof size);
        format_size(p->isize, isize, sizeof isize);
        struct installed *in = find_installed(p->name);
        printf("\e[1m%s\e[0m %s\n  %s\n  download %s, installed %s\n  depends: %s\n  from %s\n  %s\n",
               p->name, p->version, p->desc ? p->desc : "", size, isize, p->deps ? p->deps : "-",
               p->repo, in ? "installed" : "not installed");
    }
    return rc;
}

static int cmd_install(int argc, char **argv, bool reinstall)
{
    need_root("install");
    load_indexes();
    load_db();
    for (int i = 0; i < argc; i++) {
        struct pkgent *p = provider(argv[i]);
        if (!p)
            die("no package %s (pkg search %s)", argv[i], argv[i]);
        add_to_plan(p, reinstall, 0);
    }
    if (!nplan) {
        printf("already installed\n");
        return 0;
    }
    uint64_t dl = 0, inst = 0;
    printf("installing:");
    for (size_t i = 0; i < nplan; i++) {
        printf(" %s", plan[i]->name);
        dl += plan[i]->size;
        inst += plan[i]->isize;
    }
    char a[16], b[16];
    format_size(dl, a, sizeof a);
    format_size(inst, b, sizeof b);
    printf("\n%zu packages, %s to download, %s on disk\n", nplan, a, b);
    mkdirs(ROOT);
    mkdirs(CACHE);
    for (size_t i = 0; i < nplan; i++)
        install_one(plan[i]);
    run_triggers();
    printf("done\n");
    return 0;
}

static int cmd_remove(int argc, char **argv)
{
    need_root("remove");
    load_db();
    int rc = 0;
    for (int i = 0; i < argc; i++) {
        struct installed *in = find_installed(argv[i]);
        if (!in) {
            dprintf(2, "pkg: %s isn't installed\n", argv[i]);
            rc = 1;
            continue;
        }
        remove_files(in->files, NULL);
        printf("removed %s %s\n", in->name, in->version);
        in->name[0] = '\0';
    }
    save_db();
    return rc;
}

static int cmd_upgrade(void)
{
    need_root("upgrade");
    load_indexes();
    load_db();
    for (size_t i = 0; i < ndb; i++) {
        struct pkgent *p = by_name(db[i].name);
        if (p && strcmp(p->version, db[i].version) != 0)
            add_to_plan(p, false, 0);
    }
    if (!nplan) {
        printf("everything is up to date\n");
        return 0;
    }
    printf("upgrading:");
    for (size_t i = 0; i < nplan; i++)
        printf(" %s", plan[i]->name);
    printf("\n");
    for (size_t i = 0; i < nplan; i++)
        install_one(plan[i]);
    run_triggers();
    printf("done\n");
    return 0;
}

static int cmd_list(void)
{
    load_db();
    for (size_t i = 0; i < ndb; i++)
        printf("%s %s\n", db[i].name, db[i].version);
    if (!ndb)
        printf("nothing installed yet, try: sudo pkg install busybox\n");
    return 0;
}

static int cmd_files(int argc, char **argv)
{
    load_db();
    for (int i = 0; i < argc; i++) {
        struct installed *in = find_installed(argv[i]);
        if (!in) {
            dprintf(2, "pkg: %s isn't installed\n", argv[i]);
            return 1;
        }
        for (char *f = in->files; f && *f; ) {
            char *nl = strchr(f, '\n');
            printf(ROOT "/%.*s\n", (int)(nl ? nl - f : (long)strlen(f)), f);
            f = nl ? nl + 1 : f + strlen(f);
        }
    }
    return 0;
}

static int cmd_clean(void)
{
    need_root("clean");
    int fd = open(CACHE, O_RDONLY);
    if (fd < 0)
        return 0;
    struct zk_dirent d;
    char names[64][ZK_NAME_MAX + 1];
    int n = 0;
    while (n < 64 && readdir(fd, &d) > 0)
        strlcpy(names[n++], d.name, sizeof names[0]);
    close(fd);
    for (int i = 0; i < n; i++) {
        char path[ZK_PATH_MAX];
        snprintf(path, sizeof path, CACHE "/%s", names[i]);
        unlink(path);
    }
    printf("removed %d cached files\n", n);
    return 0;
}

static void usage(void)
{
    printf("usage: pkg <command> [args]\n"
           "  update             fetch the package lists\n"
           "  search <words>     look for packages\n"
           "  info <names>       details\n"
           "  install <names>    install, with dependencies\n"
           "  reinstall <names>  install again even if it's there\n"
           "  remove <names>     uninstall\n"
           "  upgrade            update everything installed\n"
           "  list               installed packages\n"
           "  files <name>       files a package installed\n"
           "  clean              empty the download cache\n"
           "packages come from alpine linux (" REPOS ") and go into " ROOT "\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }
    const char *cmd = argv[1];
    argc -= 2;
    argv += 2;
    if (argc && strcmp(argv[0], "-v") == 0) {
        verbose = true;
        argc--;
        argv++;
    }
    if (strcmp(cmd, "update") == 0)
        return cmd_update();
    if (strcmp(cmd, "search") == 0 && argc)
        return cmd_search(argc, argv);
    if (strcmp(cmd, "info") == 0 && argc)
        return cmd_info(argc, argv);
    if ((strcmp(cmd, "install") == 0 || strcmp(cmd, "add") == 0) && argc)
        return cmd_install(argc, argv, false);
    if (strcmp(cmd, "reinstall") == 0 && argc)
        return cmd_install(argc, argv, true);
    if ((strcmp(cmd, "remove") == 0 || strcmp(cmd, "del") == 0) && argc)
        return cmd_remove(argc, argv);
    if (strcmp(cmd, "upgrade") == 0)
        return cmd_upgrade();
    if (strcmp(cmd, "list") == 0)
        return cmd_list();
    if (strcmp(cmd, "files") == 0 && argc)
        return cmd_files(argc, argv);
    if (strcmp(cmd, "clean") == 0)
        return cmd_clean();
    usage();
    return 1;
}
