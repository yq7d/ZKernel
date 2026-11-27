// du [-s] [-h] [path...]
#include "zk.h"

static bool summary, human;

static void show(uint64_t bytes, const char *path)
{
    char s[16];
    if (human)
        format_size(bytes, s, sizeof s);
    else
        snprintf(s, sizeof s, "%lu", (bytes + 1023) / 1024);
    printf("%-8s %s\n", s, path);
}

static uint64_t du(const char *path, int depth)
{
    struct zk_stat st;
    if (stat(path, &st) != 0)
        return 0;
    if (st.type != ZK_TYPE_DIR)
        return st.type == ZK_TYPE_FILE ? st.size : 0;
    uint64_t total = 0;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct zk_dirent d;
        while (readdir(fd, &d) > 0) {
            char child[ZK_PATH_MAX];
            snprintf(child, sizeof child, "%s%s%s", path,
                     path[strlen(path) - 1] == '/' ? "" : "/", d.name);
            total += du(child, depth + 1);
        }
        close(fd);
    }
    if (!summary || depth == 0)
        show(total, path);
    return total;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 's') summary = true;
            else if (*p == 'h') human = true;
        }
    }
    if (i == argc)
        du(".", 0);
    for (; i < argc; i++)
        du(argv[i], 0);
    return 0;
}
