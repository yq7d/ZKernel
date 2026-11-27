#include "zk.h"

static const char *state(uint32_t s)
{
    static const char *const names[] = { "ready", "running", "blocked", "sleeping", "dead" };
    return s < 5 ? names[s] : "?";
}

int main(void)
{
    struct zk_procinfo info[64];
    int n = ps(info, 64);
    if (n < 0) {
        dprintf(2, "ps: %s\n", strerror(n));
        return 1;
    }
    printf("  %-5s %-5s %-5s %-9s %-8s %s\n", "PID", "PPID", "PGID", "STATE", "CPU", "NAME");
    for (int i = n - 1; i >= 0; i--)
        printf("  %-5lu %-5lu %-5lu %-9s %3lu.%03lus %s\n", info[i].pid, info[i].ppid,
               info[i].pgid, state(info[i].state), info[i].cpu_ms / 1000,
               info[i].cpu_ms % 1000, info[i].name);
    return 0;
}
