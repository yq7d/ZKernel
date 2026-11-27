// uname [-a] [-r] [-s] [-m] [-n]
#include "cat_proc.h"

int main(int argc, char **argv)
{
    bool all = false, rel = false, sys = false, mach = false, node = false;
    for (int i = 1; i < argc; i++) {
        for (const char *p = argv[i] + (argv[i][0] == '-'); *p; p++) {
            if (*p == 'a') all = true;
            else if (*p == 'r') rel = true;
            else if (*p == 's') sys = true;
            else if (*p == 'm') mach = true;
            else if (*p == 'n') node = true;
        }
    }
    if (!all && !rel && !mach && !node)
        sys = true;

    char version[128], host[64];
    if (slurp("/proc/version", version, sizeof version) < 0)
        strlcpy(version, "ZKernel version ?", sizeof version);
    if (slurp("/etc/hostname", host, sizeof host) <= 0)
        strlcpy(host, "zkernel", sizeof host);
    for (char *p = host; *p; p++)
        if (*p == '\n')
            *p = '\0';
    // "ZKernel version X ..."
    char release[32] = "?";
    char *v = strstr(version, "version ");
    if (v) {
        v += 8;
        size_t n = 0;
        while (v[n] && v[n] != ' ' && n + 1 < sizeof release)
            n++;
        memcpy(release, v, n);
        release[n] = '\0';
    }

    bool first = true;
#define PART(cond, s) do { if (cond) { printf("%s%s", first ? "" : " ", s); first = false; } } while (0)
    PART(all || sys, "ZKernel");
    PART(all || node, host);
    PART(all || rel, release);
    PART(all || mach, "x86_64");
    printf("\n");
    return 0;
}
