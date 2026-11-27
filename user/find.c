// find [path] [-name pattern] [-type f|d], * and ? in patterns
#include "zk.h"

static const char *name_pat;
static int want_type;

static bool glob(const char *p, const char *s)
{
    if (!*p)
        return !*s;
    if (*p == '*')
        return glob(p + 1, s) || (*s && glob(p, s + 1));
    if (*s && (*p == '?' || *p == *s))
        return glob(p + 1, s + 1);
    return false;
}

static void walk(const char *path, const char *name)
{
    struct zk_stat st;
    if (stat(path, &st) != 0)
        return;
    bool ok = (!name_pat || glob(name_pat, name)) && (!want_type || (int)st.type == want_type);
    if (ok)
        printf("%s\n", path);
    if (st.type != ZK_TYPE_DIR)
        return;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    struct zk_dirent d;
    while (readdir(fd, &d) > 0) {
        char child[ZK_PATH_MAX];
        snprintf(child, sizeof child, "%s%s%s", path,
                 path[strlen(path) - 1] == '/' ? "" : "/", d.name);
        walk(child, d.name);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    const char *start = ".";
    int i = 1;
    if (i < argc && argv[i][0] != '-')
        start = argv[i++];
    for (; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "-name") == 0)
            name_pat = argv[i + 1];
        else if (strcmp(argv[i], "-type") == 0)
            want_type = argv[i + 1][0] == 'd' ? ZK_TYPE_DIR : ZK_TYPE_FILE;
        else {
            dprintf(2, "usage: find [path] [-name pattern] [-type f|d]\n");
            return 2;
        }
    }
    walk(start, basename_of(start));
    return 0;
}
