// busy loops without ever yielding, so two of these only share the cpu
// because the timer preempts them
#include "zk.h"

int main(int argc, char **argv)
{
    const char *tag = argc > 1 ? argv[1] : "spin";
    int rounds = argc > 2 ? atoi(argv[2]) : 5;

    for (int r = 1; r <= rounds; r++) {
        for (volatile uint64_t i = 0; i < 30000000; i++)
            ;
        printf("[%s] round %d at %lu ms\n", tag, r, uptime_ms());
    }
    return 0;
}
