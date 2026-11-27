// sudo [-u user] [-i | -s | command [args]]. installed set-user-id root
#include "zk.h"

#define CACHE_DIR "/run/sudo"
#define CACHE_SECS (5 * 60)

static bool allowed(const char *user, uint32_t uid, uint32_t gid)
{
    if (uid == 0)
        return true;
    int fd = open("/etc/sudoers", O_RDONLY);
    if (fd < 0)
        return false;
    uint32_t groups[ZK_NGROUPS];
    int ng = user_groups(user, gid, groups, ZK_NGROUPS);
    struct lreader *r = lr_open(fd);
    char *line;
    bool ok = false;
    while (!ok && lr_getline(r, &line) >= 0) {
        while (isspace(*line))
            line++;
        char *end = line + strlen(line);
        while (end > line && isspace(end[-1]))
            *--end = '\0';
        if (!*line || *line == '#')
            continue;
        if (*line == '%') {
            long g = group_id(line + 1);
            for (int i = 0; i < ng && g >= 0; i++)
                if (groups[i] == (uint32_t)g)
                    ok = true;
        } else if (strcmp(line, user) == 0) {
            ok = true;
        }
    }
    lr_close(r);
    close(fd);
    return ok;
}

static void cache_path(uint32_t uid, char *out, size_t size)
{
    snprintf(out, size, CACHE_DIR "/%u", uid);
}

static bool recently_authed(uint32_t uid)
{
    char path[64];
    cache_path(uid, path, sizeof path);
    struct zk_stat st;
    if (stat(path, &st) != 0 || st.uid != 0)
        return false;
    long now = time();
    return now >= st.mtime && now - st.mtime < CACHE_SECS;
}

static void remember(uint32_t uid)
{
    mkdir("/run");
    mkdir(CACHE_DIR);
    chmod(CACHE_DIR, 0700);
    char path[64];
    cache_path(uid, path, sizeof path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, "", 0);
        ftruncate(fd, 0);
        close(fd);
    }
}

static bool find_in_path(const char *name, char *out)
{
    if (strchr(name, '/')) {
        strlcpy(out, name, ZK_PATH_MAX);
        return true;
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/bin";
    while (*path) {
        const char *end = strchr(path, ':');
        size_t n = end ? (size_t)(end - path) : strlen(path);
        snprintf(out, ZK_PATH_MAX, "%.*s/%s", (int)n, path, name);
        struct zk_stat st;
        if (n && stat(out, &st) == 0 && st.type == ZK_TYPE_FILE)
            return true;
        path += n;
        if (*path == ':')
            path++;
    }
    return false;
}

int main(int argc, char **argv)
{
    const char *target = "root";
    bool login = false;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
            target = argv[++i];
        else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-s") == 0)
            login = true;
        else if (strcmp(argv[i], "-k") == 0) {
            char path[64];
            cache_path(getuid(), path, sizeof path);
            unlink(path);
            return 0;
        } else {
            dprintf(2, "usage: sudo [-u user] [-i | -s | -k | command [args]]\n");
            return 1;
        }
    }
    if (i == argc && !login) {
        dprintf(2, "usage: sudo [-u user] [-i | -s | -k | command [args]]\n");
        return 1;
    }
    if (geteuid() != 0) {
        dprintf(2, "sudo: must be set-user-id root\n");
        return 1;
    }

    struct zk_cred c;
    getcred(&c);
    struct passwd me, to;
    if (getpwuid(c.uid, &me) != 0) {
        dprintf(2, "sudo: you don't exist in /etc/passwd\n");
        return 1;
    }
    if (getpwnam(target, &to) != 0) {
        dprintf(2, "sudo: unknown user %s\n", target);
        return 1;
    }
    if (!allowed(me.name, c.uid, c.gid)) {
        dprintf(2, "sudo: %s is not in the sudoers file\n", me.name);
        return 1;
    }

    if (c.uid != 0 && !recently_authed(c.uid)) {
        bool ok = false;
        for (int tries = 0; tries < 3 && !ok; tries++) {
            char prompt[64], pw[128];
            snprintf(prompt, sizeof prompt, "[sudo] password for %s: ", me.name);
            if (read_password(prompt, pw, sizeof pw) < 0)
                break;
            ok = check_password(me.name, pw);
            memset(pw, 0, sizeof pw);
            if (!ok)
                dprintf(2, "Sorry, try again.\n");
        }
        if (!ok) {
            dprintf(2, "sudo: 3 incorrect password attempts\n");
            return 1;
        }
        remember(c.uid);
    }

    // drop the caller's identity completely and take on the target's
    setuid(0);
    if (become_user(&to) != 0) {
        dprintf(2, "sudo: can't switch to %s\n", target);
        return 1;
    }
    setenv("SUDO_USER", me.name, 1);

    char path[ZK_PATH_MAX];
    if (login) {
        chdir(to.home);
        char *sh[] = { to.shell, NULL };
        int err = execv(to.shell, sh);
        dprintf(2, "sudo: %s: %s\n", to.shell, strerror(err));
        return 1;
    }
    if (!find_in_path(argv[i], path)) {
        dprintf(2, "sudo: %s: command not found\n", argv[i]);
        return 1;
    }
    int err = execv(path, argv + i);
    dprintf(2, "sudo: %s: %s\n", argv[i], strerror(err));
    return 1;
}
