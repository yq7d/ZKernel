// first process, runs as root: /etc/rc, then the desktop as the user named
// in /etc/autologin. leaving the desktop drops to a text shell, exiting that
// shell starts the desktop again
#include "zk.h"

static struct passwd user;

static size_t strcspn_ws(const char *s)
{
    size_t n = 0;
    while (s[n] && !isspace(s[n]))
        n++;
    return n;
}
static bool have_user;

static long start(char **argv, bool as_user)
{
    long pid = fork();
    if (pid == 0) {
        if (as_user && have_user) {
            if (become_user(&user) != 0) {
                dprintf(2, "init: can't become %s\n", user.name);
                exit(1);
            }
            chdir(user.home);
        }
        int err = execv(argv[0], argv);
        dprintf(2, "init: %s: %s\n", argv[0], strerror(err));
        exit(127);
    }
    return pid;
}

static int run(char **argv, bool as_user)
{
    long pid = start(argv, as_user);
    if (pid < 0) {
        dprintf(2, "init: fork: %s\n", strerror((int)pid));
        return -1;
    }
    int status = -1;
    while (wait(pid, &status, 0) == ZK_EINTR)
        ;
    return status;
}

static void copy_file(const char *from, const char *to, uint32_t mode)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    char buf[1024];
    long n;
    while (out >= 0 && (n = read(in, buf, sizeof buf)) > 0)
        write_all(out, buf, (size_t)n);
    close(in);
    if (out >= 0) {
        close(out);
        chmod(to, mode);
    }
}

static void setup_user(void)
{
    char name[32] = "zos";
    int fd = open("/etc/autologin", O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, name, sizeof name - 1);
        close(fd);
        name[n > 0 ? n : 0] = '\0';
        char *nl = strchr(name, '\n');
        if (nl)
            *nl = '\0';
    }
    if (getpwnam(name, &user) != 0) {
        dprintf(2, "init: no user %s, staying root\n", name);
        return;
    }
    have_user = true;

    // a fresh home gets the skeleton files
    struct zk_stat st;
    if (stat(user.home, &st) != 0) {
        mkdir("/home");
        mkdir(user.home);
        chown(user.home, user.uid, user.gid);
        chmod(user.home, 0755);
    }
    char marker[ZK_PATH_MAX];
    snprintf(marker, sizeof marker, "%s/.config", user.home);
    if (stat(marker, &st) != 0) {
        char *cp[] = { "/bin/cp", "-r", "/etc/skel/.", user.home, NULL };
        run(cp, false);
        char *own[] = { "/bin/chown", "-R", NULL, user.home, NULL };
        char spec[48];
        snprintf(spec, sizeof spec, "%u:%u", user.uid, user.gid);
        own[2] = spec;
        run(own, false);
    }
}

int main(void)
{
    setenv("PATH", "/bin:/usr/local/bin:/compat/linux/bin:/compat/linux/usr/bin:/compat/apps/bin:/compat/apps/usr/bin", 1);
    struct zk_stat st;
    if (stat("/etc/rc", &st) == 0) {
        char *rc[] = { "/bin/sh", "/etc/rc", NULL };
        run(rc, false);
    }
    // accounts, passwords and the hostname kept on disk replace the defaults
    static const struct { const char *name; uint32_t mode; } kept[] = {
        { "passwd", 0644 }, { "group", 0644 }, { "shadow", 0600 }, { "sudoers", 0440 },
        { "hostname", 0644 }, { "autologin", 0644 },
    };
    for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) {
        char from[64], to[64];
        snprintf(from, sizeof from, "/usr/local/etc/%s", kept[i].name);
        snprintf(to, sizeof to, "/etc/%s", kept[i].name);
        if (stat(from, &st) == 0)
            copy_file(from, to, kept[i].mode);
    }
    setup_user();

    char *zwm[] = { "/bin/zwm", NULL };
    char *x11[] = { "/bin/startx", NULL };
    char *shell[] = { "/bin/sh", NULL };
    // "text" on the kernel command line skips the desktop
    char cmdline[256] = "";
    int cfd = open("/proc/cmdline", O_RDONLY);
    if (cfd >= 0) {
        long n = read(cfd, cmdline, sizeof cmdline - 1);
        cmdline[n > 0 ? n : 0] = '\0';
        close(cfd);
    }
    bool gui = true, want_zwm = false;
    for (char *w = cmdline; *w; ) {
        while (isspace(*w))
            w++;
        size_t len = strcspn_ws(w);
        if (len == 4 && strncmp(w, "text", 4) == 0)
            gui = false;
        if (len == 3 && strncmp(w, "zwm", 3) == 0)
            want_zwm = true;
        w += len;
    }
    // x with twm once xorg is installed, "zwm" on the command line keeps zwm
    struct zk_stat xst;
    bool use_x = !want_zwm && (stat("/compat/linux/usr/bin/Xorg", &xst) == 0 ||
                               stat("/compat/apps/usr/bin/Xorg", &xst) == 0);
    for (;;) {
        if (gui && use_x) {
            uint64_t began = uptime_ms();
            int status = run(x11, true);
            if (status != 0 && uptime_ms() - began < 15000) {
                printf("init: x didn't start (see /tmp/Xorg.0.log), using zwm\n");
                use_x = false;
            }
        }
        if (gui && !use_x) {
            uint64_t began = uptime_ms();
            int status = run(zwm, true);
            // it couldn't even start, no point trying again
            if (status != 0 && uptime_ms() - began < 3000) {
                printf("init: the desktop failed to start, using the text console\n");
                gui = false;
            }
        }
        if (gui)
            printf("type exit to go back to the desktop\n");
        run(shell, true);
    }
}
