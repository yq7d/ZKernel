// fetch [-o file] [-q] url, http only
#include "zk.h"

static uint64_t last_draw;

static void progress(struct http_progress *p)
{
    uint64_t now = uptime_ms();
    if (now - last_draw < 200 && (!p->total || p->done < p->total))
        return;
    last_draw = now;
    char done[16], total[16];
    format_size(p->done, done, sizeof done);
    if (p->total) {
        format_size(p->total, total, sizeof total);
        int pct = (int)(p->done * 100 / p->total);
        char bar[31];
        for (int i = 0; i < 30; i++)
            bar[i] = i < pct * 30 / 100 ? '#' : '.';
        bar[30] = '\0';
        dprintf(2, "\r[%s] %3d%% %s/%s ", bar, pct, done, total);
    } else {
        dprintf(2, "\r%s ", done);
    }
}

int main(int argc, char **argv)
{
    const char *out = NULL, *url = NULL;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(argv[i], "-q") == 0)
            quiet = true;
        else
            url = argv[i];
    }
    if (!url) {
        dprintf(2, "usage: fetch [-o file] [-q] url\n");
        return 2;
    }
    char name[ZK_PATH_MAX];
    if (!out) {
        const char *slash = strrchr(url, '/');
        strlcpy(name, slash && slash[1] ? slash + 1 : "index.html", sizeof name);
        out = name;
    }
    int fd = strcmp(out, "-") == 0 ? 1 : open(out, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        dprintf(2, "fetch: %s: %s\n", out, strerror(fd));
        return 1;
    }
    struct http_progress p = { 0, 0, 0, quiet || fd == 1 ? NULL : progress, NULL };
    int err = http_get(url, fd, &p);
    if (fd != 1)
        close(fd);
    if (!quiet && fd != 1)
        dprintf(2, "\n");
    if (err) {
        dprintf(2, "fetch: %s: %s", url, strerror(err));
        if (p.status)
            dprintf(2, " (http %d)", p.status);
        dprintf(2, "\n");
        if (fd != 1)
            unlink(out);
        return 1;
    }
    if (!quiet && fd != 1) {
        char size[16];
        format_size(p.done, size, sizeof size);
        dprintf(2, "saved %s (%s)\n", out, size);
    }
    return 0;
}
