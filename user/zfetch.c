// zfetch: system info next to a logo. settings in /etc/zfetch.conf and
// ~/.config/zfetch/zfetch.conf, flags win over both:
//   zfetch [--logo FILE|none] [--fields a,b,c] [--color N]
#include "zk.h"
#include "lib/time.h"

#define MAX_LINES 40

// the badge: each character is a pixel two cells wide.
// # tile, + the Z, - the Z's shadow, . nothing
static const char *const badge[] = {
    "...#########...",
    ".#############.",
    ".##+++++++++##.",
    "###+++++++++-##",
    "#########+++-##",
    "########+++-###",
    "#######+++-####",
    "######+++-#####",
    "#####+++-######",
    "###+++++++++###",
    ".##+++++++++-#.",
    ".#############.",
    "...#########...",
    NULL,
};

static const char *const text_logo[] = {
    "  ______  ____   _____ ",
    " |___  / / __ \\ / ____|",
    "    / / | |  | | (___  ",
    "   / /  | |  | |\\___ \\ ",
    "  / /__ | |__| |____) |",
    " /_____| \\____/|_____/ ",
    NULL,
};

// turns a badge row into colored cells
static void paint_row(const char *row, int tile, char *out, size_t size)
{
    out[0] = '\0';
    for (const char *p = row; *p; p++) {
        char cell[24];
        if (*p == '#')
            snprintf(cell, sizeof cell, "\e[4%dm  ", tile);
        else if (*p == '+')
            snprintf(cell, sizeof cell, "\e[107m  ");
        else if (*p == '-')
            snprintf(cell, sizeof cell, "\e[10%dm  ", tile);
        else
            snprintf(cell, sizeof cell, "\e[0m  ");
        strlcat(out, cell, size);
    }
    strlcat(out, "\e[0m", size);
}

static struct {
    char logo[ZK_PATH_MAX];      // builtin, none, or a file
    int logo_color, label_color, title_color;
    char separator[16];
    char fields[256];
    char labels[24][2][24];      // overrides: field name, label
    int nlabels;
    char custom[8][2][96];       // custom = Label: text
    int ncustom;
} cfg = {
    .logo = "builtin", .logo_color = 4, .label_color = 4, .title_color = 6,
    .separator = ": ",
    .fields = "title line os kernel uptime packages shell wm terminal cpu memory disk ip blank colors",
};

static char lines[MAX_LINES][160];
static int nlines;

static void read_config(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    struct lreader *r = lr_open(fd);
    char *line;
    while (lr_getline(r, &line) >= 0) {
        while (isspace(*line))
            line++;
        if (!*line || *line == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;
        for (char *e = eq; e > key && isspace(e[-1]); )
            *--e = '\0';
        while (isspace(*val))
            val++;
        size_t vl = strlen(val);
        if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
            val[vl - 1] = '\0';
            val++;
        }
        if (strcmp(key, "logo") == 0)
            strlcpy(cfg.logo, val, sizeof cfg.logo);
        else if (strcmp(key, "logo_color") == 0)
            cfg.logo_color = atoi(val);
        else if (strcmp(key, "label_color") == 0)
            cfg.label_color = atoi(val);
        else if (strcmp(key, "title_color") == 0)
            cfg.title_color = atoi(val);
        else if (strcmp(key, "separator") == 0)
            strlcpy(cfg.separator, val, sizeof cfg.separator);
        else if (strcmp(key, "fields") == 0)
            strlcpy(cfg.fields, val, sizeof cfg.fields);
        else if (strncmp(key, "label_", 6) == 0 && cfg.nlabels < 24) {
            strlcpy(cfg.labels[cfg.nlabels][0], key + 6, 24);
            strlcpy(cfg.labels[cfg.nlabels][1], val, 24);
            cfg.nlabels++;
        } else if (strcmp(key, "custom") == 0 && cfg.ncustom < 8) {
            // custom = Label: anything you like
            char *colon = strchr(val, ':');
            if (colon) {
                *colon = '\0';
                strlcpy(cfg.custom[cfg.ncustom][0], val, 96);
                const char *t = colon + 1;
                while (isspace(*t))
                    t++;
                strlcpy(cfg.custom[cfg.ncustom][1], t, 96);
                cfg.ncustom++;
            }
        }
    }
    lr_close(r);
    close(fd);
}

static const char *label(const char *field, const char *def)
{
    for (int i = 0; i < cfg.nlabels; i++)
        if (strcmp(cfg.labels[i][0], field) == 0)
            return cfg.labels[i][1];
    return def;
}

__attribute__((format(printf, 1, 2)))
static void addf(const char *fmt, ...)
{
    if (nlines == MAX_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[nlines++], sizeof lines[0], fmt, ap);
    va_end(ap);
}

static void info(const char *field, const char *def, const char *value)
{
    addf("\e[1;3%dm%s\e[0m%s%s", cfg.label_color, label(field, def), cfg.separator, value);
}

static long slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    long n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    return n;
}

static void chomp(char *s)
{
    char *nl = strchr(s, '\n');
    if (nl)
        *nl = '\0';
}

static bool running(const char *name)
{
    struct zk_procinfo p[64];
    int n = ps(p, 64);
    for (int i = 0; i < n; i++)
        if (strcmp(p[i].name, name) == 0)
            return true;
    return false;
}

static int count_dir(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct zk_dirent d;
    int n = 0;
    while (readdir(fd, &d) > 0)
        n++;
    close(fd);
    return n;
}

static int count_pkgs(void)
{
    int fd = open("/var/lib/pkg/installed", O_RDONLY);
    if (fd < 0)
        return 0;
    struct lreader *r = lr_open(fd);
    char *line;
    int n = 0;
    while (lr_getline(r, &line) >= 0)
        if (line[0] == 'P' && line[1] == ':')
            n++;
    lr_close(r);
    close(fd);
    return n;
}

static void field(const char *f)
{
    char buf[512], v[160];
    if (strcmp(f, "title") == 0) {
        struct passwd pw;
        char host[64] = "zos";
        slurp("/etc/hostname", host, sizeof host);
        chomp(host);
        const char *user = getpwuid(geteuid(), &pw) == 0 ? pw.name : "?";
        addf("\e[1;3%dm%s\e[0m@\e[1;3%dm%s\e[0m", cfg.title_color, user, cfg.title_color, host);
    } else if (strcmp(f, "line") == 0) {
        struct passwd pw;
        char host[64] = "zos";
        slurp("/etc/hostname", host, sizeof host);
        chomp(host);
        size_t n = strlen(getpwuid(geteuid(), &pw) == 0 ? pw.name : "?") + 1 + strlen(host);
        char dash[64];
        if (n >= sizeof dash)
            n = sizeof dash - 1;
        memset(dash, '-', n);
        dash[n] = '\0';
        addf("%s", dash);
    } else if (strcmp(f, "os") == 0) {
        strlcpy(v, "ZOS", sizeof v);
        if (slurp("/etc/os-release", buf, sizeof buf) > 0) {
            char *p = strstr(buf, "PRETTY_NAME=");
            if (p) {
                p += 12;
                if (*p == '"')
                    p++;
                size_t n = 0;
                while (p[n] && p[n] != '"' && p[n] != '\n' && n + 1 < sizeof v)
                    n++;
                memcpy(v, p, n);
                v[n] = '\0';
            }
        }
        strlcat(v, " x86_64", sizeof v);
        info(f, "OS", v);
    } else if (strcmp(f, "kernel") == 0) {
        strlcpy(v, "ZKernel", sizeof v);
        if (slurp("/proc/version", buf, sizeof buf) > 0) {
            char *p = strstr(buf, "version ");
            if (p) {
                p += 8;
                char *sp = strchr(p, ' ');
                if (sp)
                    *sp = '\0';
                snprintf(v, sizeof v, "ZKernel %s", p);
            }
        }
        info(f, "Kernel", v);
    } else if (strcmp(f, "uptime") == 0) {
        uint64_t s = uptime_ms() / 1000;
        if (s >= 3600)
            snprintf(v, sizeof v, "%lu hours, %lu mins", s / 3600, s / 60 % 60);
        else if (s >= 60)
            snprintf(v, sizeof v, "%lu mins", s / 60);
        else
            snprintf(v, sizeof v, "%lu secs", s);
        info(f, "Uptime", v);
    } else if (strcmp(f, "packages") == 0) {
        int alpine = count_pkgs();
        if (alpine)
            snprintf(v, sizeof v, "%d (zos), %d (pkg)", count_dir("/bin"), alpine);
        else
            snprintf(v, sizeof v, "%d (zos)", count_dir("/bin"));
        info(f, "Packages", v);
    } else if (strcmp(f, "shell") == 0) {
        const char *sh = getenv("SHELL");
        info(f, "Shell", sh ? basename_of(sh) : "sh");
    } else if (strcmp(f, "wm") == 0) {
        info(f, "WM", running("zwm") ? "zwm" : "none");
    } else if (strcmp(f, "terminal") == 0) {
        info(f, "Terminal", running("zwm") ? "zwm" : "console");
    } else if (strcmp(f, "cpu") == 0) {
        strlcpy(v, "unknown", sizeof v);
        if (slurp("/proc/cpuinfo", buf, sizeof buf) > 0) {
            char *p = strstr(buf, "model name");
            if (p && (p = strchr(p, ':'))) {
                p++;
                while (*p == ' ')
                    p++;
                strlcpy(v, p, sizeof v);
                chomp(v);
            }
        }
        info(f, "CPU", v);
    } else if (strcmp(f, "memory") == 0) {
        uint64_t total = 0, freeb = 0;
        if (slurp("/proc/meminfo", buf, sizeof buf) > 0) {
            char *p = strstr(buf, "MemTotal:");
            if (p)
                total = (uint64_t)strtol(p + 9, NULL, 10) * 1024;
            p = strstr(buf, "MemFree:");
            if (p)
                freeb = (uint64_t)strtol(p + 8, NULL, 10) * 1024;
        }
        char a[16], b[16];
        format_size(total - freeb, a, sizeof a);
        format_size(total, b, sizeof b);
        snprintf(v, sizeof v, "%s / %s (%lu%%)", a, b, total ? (total - freeb) * 100 / total : 0);
        info(f, "Memory", v);
    } else if (strcmp(f, "disk") == 0) {
        struct zk_statfs st;
        const char *where = statfs("/data", &st) == 0 && strcmp(st.fstype, "tmpfs") != 0 ? "/data" : "/";
        if (statfs(where, &st) == 0) {
            uint64_t total = st.blocks * st.block_size, freeb = st.blocks_free * st.block_size;
            char a[16], b[16];
            format_size(total - freeb, a, sizeof a);
            format_size(total, b, sizeof b);
            snprintf(v, sizeof v, "%s / %s (%s, %s)", a, b, where, st.fstype);
            info(f, "Disk", v);
        }
    } else if (strcmp(f, "ip") == 0) {
        struct zk_netinfo ni;
        if (netinfo(&ni) == 0 && ni.up) {
            inet_format(ni.ip, v, sizeof v);
            info(f, "Local IP", v);
        }
    } else if (strcmp(f, "blank") == 0) {
        addf("%s", "");
    } else if (strcmp(f, "colors") == 0) {
        char row[160] = "";
        for (int i = 0; i < 8; i++) {
            char c[24];
            snprintf(c, sizeof c, "\e[4%dm   ", i);
            strlcat(row, c, sizeof row);
        }
        strlcat(row, "\e[0m", sizeof row);
        addf("%s", row);
        row[0] = '\0';
        for (int i = 0; i < 8; i++) {
            char c[24];
            snprintf(c, sizeof c, "\e[10%dm   ", i);
            strlcat(row, c, sizeof row);
        }
        strlcat(row, "\e[0m", sizeof row);
        addf("%s", row);
    } else if (strcmp(f, "custom") == 0) {
        for (int i = 0; i < cfg.ncustom; i++)
            addf("\e[1;3%dm%s\e[0m%s%s", cfg.label_color, cfg.custom[i][0], cfg.separator,
                 cfg.custom[i][1]);
    }
}

// visible width, ansi escapes don't count
static int width(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        if (*s == '\e') {
            while (*s && !isalpha(*s))
                s++;
            if (!*s)
                break;
            continue;
        }
        w++;
    }
    return w;
}

int main(int argc, char **argv)
{
    read_config("/etc/zfetch.conf");
    const char *home = getenv("HOME");
    if (home) {
        char path[ZK_PATH_MAX];
        snprintf(path, sizeof path, "%s/.config/zfetch/zfetch.conf", home);
        read_config(path);
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--logo") == 0 && i + 1 < argc)
            strlcpy(cfg.logo, argv[++i], sizeof cfg.logo);
        else if (strcmp(argv[i], "--fields") == 0 && i + 1 < argc)
            strlcpy(cfg.fields, argv[++i], sizeof cfg.fields);
        else if (strcmp(argv[i], "--color") == 0 && i + 1 < argc)
            cfg.logo_color = cfg.label_color = atoi(argv[++i]);
        else {
            printf("usage: zfetch [--logo FILE|builtin|text|none] [--fields a,b,c] [--color 0-7]\n"
                   "fields: title line os kernel uptime packages shell wm terminal cpu\n"
                   "        memory disk ip blank colors custom\n");
            return strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }

    char fields[256], *save;
    strlcpy(fields, cfg.fields, sizeof fields);
    for (char *f = strtok_r(fields, " ,", &save); f; f = strtok_r(NULL, " ,", &save))
        field(f);

    // the logo, from a file if one's set
    static char logo_buf[4096], painted[MAX_LINES][512];
    const char *logo[MAX_LINES + 1];
    int nlogo = 0;
    bool colored = false;
    if (strcmp(cfg.logo, "none") == 0) {
        logo[0] = NULL;
    } else if (strcmp(cfg.logo, "text") == 0) {
        while (text_logo[nlogo]) {
            logo[nlogo] = text_logo[nlogo];
            nlogo++;
        }
    } else if (strcmp(cfg.logo, "builtin") == 0 || slurp(cfg.logo, logo_buf, sizeof logo_buf) <= 0) {
        while (badge[nlogo] && nlogo < MAX_LINES) {
            paint_row(badge[nlogo], cfg.logo_color, painted[nlogo], sizeof painted[0]);
            logo[nlogo] = painted[nlogo];
            nlogo++;
        }
        colored = true;
    } else {
        char *p = logo_buf;
        while (*p && nlogo < MAX_LINES) {
            logo[nlogo++] = p;
            char *nl = strchr(p, '\n');
            if (!nl)
                break;
            *nl = '\0';
            p = nl + 1;
        }
    }
    logo[nlogo] = NULL;
    int lw = 0;
    for (int i = 0; i < nlogo; i++)
        if (width(logo[i]) > lw)
            lw = width(logo[i]);

    int rows = nlines > nlogo ? nlines : nlogo;
    for (int i = 0; i < rows; i++) {
        int pad = lw;
        if (i < nlogo) {
            if (colored)
                printf("%s", logo[i]);
            else
                printf("\e[1;3%dm%s\e[0m", cfg.logo_color, logo[i]);
            pad -= width(logo[i]);
        }
        if (nlogo)
            printf("%*s   ", pad, "");
        if (i < nlines)
            printf("%s", lines[i]);
        printf("\n");
    }
    return 0;
}
