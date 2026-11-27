// xargs command [args], one run with every word from stdin appended
#include "zk.h"

int main(int argc, char **argv)
{
    char *args[64];
    int n = 0;
    const char *cmd = argc > 1 ? argv[1] : "echo";
    for (int i = 1; i < argc && n < 64 - 1; i++)
        args[n++] = argv[i];
    if (argc == 1)
        args[n++] = "echo";

    struct lreader *r = lr_open(0);
    char *line;
    while (lr_getline(r, &line) >= 0) {
        char *p = line;
        while (*p && n < 64 - 1) {
            while (isspace(*p))
                p++;
            if (!*p)
                break;
            char *start = p;
            while (*p && !isspace(*p))
                p++;
            if (*p)
                *p++ = '\0';
            args[n++] = strdup(start);
        }
    }
    args[n] = NULL;
    char path[ZK_PATH_MAX];
    if (strchr(cmd, '/'))
        strlcpy(path, cmd, sizeof path);
    else
        snprintf(path, sizeof path, "/bin/%s", cmd);
    int err = execv(path, args);
    dprintf(2, "xargs: %s: %s\n", cmd, strerror(err));
    return 127;
}
