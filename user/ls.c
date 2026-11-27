// ls [-l] [-a] [-h] [path...]
#include "zk.h"
#include "lib/time.h"

static bool opt_long, opt_all, opt_human;

struct ent {
    char name[ZK_NAME_MAX + 1];
    struct zk_stat st;
};

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct ent *)a)->name, ((const struct ent *)b)->name);
}

static void print_ent(const struct ent *e)
{
    const char *color = e->st.type == ZK_TYPE_DIR ? "\e[1;34m" :
                        e->st.type == ZK_TYPE_DEV ? "\e[33m" : "";
    const char *suffix = e->st.type == ZK_TYPE_DIR ? "/" : "";
    if (!opt_long) {
        printf("%s%s\e[0m%s\n", color, e->name, suffix);
        return;
    }
    char type = e->st.type == ZK_TYPE_DIR ? 'd' : e->st.type == ZK_TYPE_DEV ? 'c' :
                e->st.type == ZK_TYPE_PIPE ? 'p' : '-';
    char size[16];
    if (opt_human)
        format_size(e->st.size, size, sizeof size);
    else
        snprintf(size, sizeof size, "%lu", e->st.size);
    struct datetime d;
    epoch_to_datetime(e->st.mtime, &d);
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    printf("%c %9s %s %2d %02d:%02d %s%s\e[0m%s\n", type, size,
           mon[(d.month - 1) % 12], d.day, d.hour, d.min, color, e->name, suffix);
}

static int list(const char *path, bool header)
{
    struct zk_stat st;
    int err = stat(path, &st);
    if (err) {
        dprintf(2, "ls: %s: %s\n", path, strerror(err));
        return 1;
    }
    if (st.type != ZK_TYPE_DIR) {
        struct ent e;
        strlcpy(e.name, path, sizeof e.name);
        e.st = st;
        print_ent(&e);
        return 0;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }
    size_t n = 0, cap = 32;
    struct ent *ents = malloc(cap * sizeof(*ents));
    struct zk_dirent d;
    while (ents && readdir(fd, &d) > 0) {
        if (d.name[0] == '.' && !opt_all)
            continue;
        if (n == cap) {
            cap *= 2;
            struct ent *e = realloc(ents, cap * sizeof(*ents));
            if (!e)
                break;
            ents = e;
        }
        char child[ZK_PATH_MAX];
        snprintf(child, sizeof child, "%s%s%s", path,
                 path[strlen(path) - 1] == '/' ? "" : "/", d.name);
        strlcpy(ents[n].name, d.name, sizeof ents[n].name);
        memset(&ents[n].st, 0, sizeof ents[n].st);
        ents[n].st.type = d.type;
        stat(child, &ents[n].st);
        n++;
    }
    close(fd);
    if (!ents)
        return 1;
    qsort(ents, n, sizeof(*ents), by_name);
    if (header)
        printf("%s:\n", path);
    for (size_t i = 0; i < n; i++)
        print_ent(&ents[i]);
    free(ents);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'l') opt_long = true;
            else if (*p == 'a') opt_all = true;
            else if (*p == 'h') opt_human = true;
            else {
                dprintf(2, "usage: ls [-lah] [path...]\n");
                return 2;
            }
        }
    }
    if (first == argc)
        return list(".", false);
    int rc = 0;
    for (int i = first; i < argc; i++) {
        rc |= list(argv[i], argc - first > 1);
        if (argc - first > 1 && i + 1 < argc)
            printf("\n");
    }
    return rc;
}
