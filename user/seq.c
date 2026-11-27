// seq [first [step]] last
#include "zk.h"

int main(int argc, char **argv)
{
    long first = 1, step = 1, last;
    if (argc == 2) {
        last = strtol(argv[1], NULL, 10);
    } else if (argc == 3) {
        first = strtol(argv[1], NULL, 10);
        last = strtol(argv[2], NULL, 10);
    } else if (argc == 4) {
        first = strtol(argv[1], NULL, 10);
        step = strtol(argv[2], NULL, 10);
        last = strtol(argv[3], NULL, 10);
    } else {
        dprintf(2, "usage: seq [first [step]] last\n");
        return 2;
    }
    if (step == 0)
        return 1;
    for (long v = first; step > 0 ? v <= last : v >= last; v += step)
        printf("%ld\n", v);
    return 0;
}
