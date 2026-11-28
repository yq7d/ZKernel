// xrun prog [args]: runs an x program from anywhere. under x it just runs
// it. from zwm it asks init for an x session with the program in it and
// closes zwm, init brings zwm back once the program exits. from a text
// console it starts x itself. installed as firefox too
#include "zk.h"

#define REQUEST "/tmp/.zos-xsession"
#define ZWM_PID "/tmp/.zwm.pid"

static const char *const roots[] = { "/compat/linux", "/compat/apps" };
static const char *const dirs[] = { "/usr/bin", "/bin", "/usr/local/bin" };

// the linux program behind a name, in either tree
static bool find(const char *name, char *out, size_t size)
{
    struct zk_stat st;
    if (name[0] == '/') {
        for (int r = 0; r < 2; r++) {
            snprintf(out, size, "%s%s", roots[r], name);
            if (stat(out, &st) == 0)
                return true;
        }
        return false;
    }
    for (int r = 0; r < 2; r++) {
        for (int d = 0; d < 3; d++) {
            snprintf(out, size, "%s%s/%s", roots[r], dirs[d], name);
            if (stat(out, &st) == 0)
                return true;
        }
    }
    return false;
}

static long zwm_pid(void)
{
    char buf[32] = "";
    int fd = open(ZWM_PID, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = '\0';
    long pid = 0;
    for (char *p = buf; *p >= '0' && *p <= '9'; p++)
        pid = pid * 10 + (*p - '0');
    return pid > 0 ? pid : -1;
}

int main(int argc, char **argv)
{
    // called as firefox: the program is our own name
    const char *base = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    int first = strcmp(base, "xrun") == 0 ? 1 : 0;
    if (argc <= first) {
        printf("usage: xrun program [args]\n");
        return 1;
    }
    char path[ZK_PATH_MAX];
    if (!find(argv[first], path, sizeof path)) {
        printf("xrun: no %s in /compat/linux or /compat/apps\n", argv[first]);
        return 1;
    }

    if (getenv("DISPLAY")) {
        int err = execv(path, argv + first);
        printf("xrun: %s: %s\n", path, strerror(err));
        return 1;
    }

    // the command line, for the x session to run
    char cmd[1024] = "";
    size_t n = 0;
    for (int i = first; i < argc && n < sizeof cmd - 2; i++)
        n += (size_t)snprintf(cmd + n, sizeof cmd - n, "%s%s", i > first ? " " : "", argv[i]);

    long pid = zwm_pid();
    if (pid > 0) {
        int fd = open(REQUEST, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            printf("xrun: can't write %s\n", REQUEST);
            return 1;
        }
        dprintf(fd, "%s\n", cmd);
        close(fd);
        printf("switching to x for %s, zwm comes back when it closes\n", cmd);
        unlink(ZWM_PID);
        kill(pid);
        return 0;
    }

    // a text console: x right here
    setenv("ZOS_XAPP", cmd, 1);
    char *sx[] = { "startx", NULL };
    int err = execv("/bin/startx", sx);
    printf("xrun: /bin/startx: %s\n", strerror(err));
    return 1;
}
