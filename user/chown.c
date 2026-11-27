// chown [-R] user[:group] file...
#include "zk.h"

static uint32_t uid = UINT32_MAX, gid = UINT32_MAX;
static bool recursive;

static int change(const char *path)
{
    int err = chown(path, uid, gid);
    if (err) {
        dprintf(2, "chown: %s: %s\n", path, strerror(err));
        return 1;
    }
    struct zk_stat st;
    if (!recursive || stat(path, &st) != 0 || st.type != ZK_TYPE_DIR)
        return 0;
    int fd = open(path, O_RDONLY), rc = 0;
    struct zk_dirent d;
    while (fd >= 0 && readdir(fd, &d) > 0) {
        char child[ZK_PATH_MAX];
        snprintf(child, sizeof child, "%s/%s", path, d.name);
        rc |= change(child);
    }
    if (fd >= 0)
        close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-R") == 0) {
        recursive = true;
        first = 2;
    }
    if (argc - first < 2) {
        dprintf(2, "usage: chown [-R] user[:group] file...\n");
        return 2;
    }
    char spec[64];
    strlcpy(spec, argv[first], sizeof spec);
    char *colon = strchr(spec, ':');
    if (colon)
        *colon++ = '\0';
    if (spec[0]) {
        struct passwd pw;
        if (getpwnam(spec, &pw) == 0)
            uid = pw.uid;
        else if (isdigit(spec[0]))
            uid = (uint32_t)strtol(spec, NULL, 10);
        else {
            dprintf(2, "chown: unknown user %s\n", spec);
            return 1;
        }
    }
    if (colon && *colon) {
        long g = group_id(colon);
        if (g < 0 && isdigit(*colon))
            g = strtol(colon, NULL, 10);
        if (g < 0) {
            dprintf(2, "chown: unknown group %s\n", colon);
            return 1;
        }
        gid = (uint32_t)g;
    }
    int rc = 0;
    for (int i = first + 1; i < argc; i++)
        rc |= change(argv[i]);
    return rc;
}
