#include "zk.h"

int main(void)
{
    uint64_t s = uptime_ms() / 1000;
    printf("up ");
    if (s >= 86400)
        printf("%lu day%s, ", s / 86400, s / 86400 == 1 ? "" : "s");
    printf("%02lu:%02lu:%02lu\n", s / 3600 % 24, s / 60 % 60, s % 60);
    return 0;
}
