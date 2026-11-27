#include "zk.h"

int main(void)
{
    uint64_t seed = uptime_ms() * 6364136223846793005UL + 1442695040888963407UL;
    int target = (int)((seed >> 33) % 100) + 1;

    printf("guess a number between 1 and 100\n");
    for (int tries = 1;; tries++) {
        printf("> ");
        char line[16];
        if (readline(line, sizeof line) < 0)
            return 1;
        int g = atoi(line);

        if (g < target) {
            printf("higher\n");
        } else if (g > target) {
            printf("lower\n");
        } else {
            printf("got it in %d tries\n", tries);
            return 0;
        }
    }
}
