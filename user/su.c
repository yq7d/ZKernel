// su [user] [-c command]. installed set-user-id root
#include "zk.h"

int main(int argc, char **argv)
{
    const char *target = "root", *cmd = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            cmd = argv[++i];
        else if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0)
            continue;
        else
            target = argv[i];
    }
    struct passwd to;
    if (getpwnam(target, &to) != 0) {
        dprintf(2, "su: unknown user %s\n", target);
        return 1;
    }
    struct zk_cred c;
    getcred(&c);
    if (c.uid != 0) {
        char pw[128];
        read_password("Password: ", pw, sizeof pw);
        bool ok = check_password(target, pw);
        memset(pw, 0, sizeof pw);
        if (!ok) {
            sleep_ms(1000);
            dprintf(2, "su: authentication failure\n");
            return 1;
        }
    }
    setuid(0);
    if (become_user(&to) != 0) {
        dprintf(2, "su: can't switch to %s\n", target);
        return 1;
    }
    chdir(to.home);
    char *sh[] = { to.shell, cmd ? "-c" : NULL, (char *)cmd, NULL };
    int err = execv(to.shell, sh);
    dprintf(2, "su: %s: %s\n", to.shell, strerror(err));
    return 1;
}
