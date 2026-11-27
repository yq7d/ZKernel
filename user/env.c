// env, or env NAME=value... command [args]
#include "zk.h"

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && strchr(argv[i], '='); i++) {
        char *eq = strchr(argv[i], '=');
        *eq = '\0';
        setenv(argv[i], eq + 1, 1);
    }
    if (i == argc) {
        for (char **e = environ; e && *e; e++)
            printf("%s\n", *e);
        return 0;
    }
    char path[ZK_PATH_MAX];
    if (strchr(argv[i], '/'))
        strlcpy(path, argv[i], sizeof path);
    else
        snprintf(path, sizeof path, "/bin/%s", argv[i]);
    int err = execve(path, argv + i, environ);
    dprintf(2, "env: %s: %s\n", argv[i], strerror(err));
    return 127;
}
