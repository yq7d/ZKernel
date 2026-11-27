// sleep seconds, fractions allowed
#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: sleep seconds\n");
        return 2;
    }
    char *end;
    uint64_t ms = (uint64_t)strtol(argv[1], &end, 10) * 1000;
    if (*end == '.') {
        uint64_t scale = 100;
        for (end++; isdigit(*end) && scale; end++, scale /= 10)
            ms += (uint64_t)(*end - '0') * scale;
    }
    sleep_ms(ms);
    return 0;
}
