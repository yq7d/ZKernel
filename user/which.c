#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: which name...\n");
        return 2;
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        bool found = false;
        const char *p = path;
        while (*p && !found) {
            const char *end = strchr(p, ':');
            size_t n = end ? (size_t)(end - p) : strlen(p);
            char full[ZK_PATH_MAX];
            snprintf(full, sizeof full, "%.*s/%s", (int)n, p, argv[i]);
            struct zk_stat st;
            if (n && stat(full, &st) == 0 && st.type == ZK_TYPE_FILE) {
                printf("%s\n", full);
                found = true;
            }
            p += n;
            if (*p == ':')
                p++;
        }
        if (!found)
            rc = 1;
    }
    return rc;
}
