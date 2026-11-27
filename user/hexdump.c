// hexdump [-n bytes] [file], canonical hex+ascii
#include "zk.h"

int main(int argc, char **argv)
{
    long limit = -1;
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        limit = strtol(argv[i + 1], NULL, 0);
        i += 2;
    }
    int fd = i < argc ? open_arg("hexdump", argv[i]) : 0;
    if (fd < 0)
        return 1;
    uint8_t line[16];
    uint64_t off = 0;
    for (;;) {
        size_t want = 16;
        if (limit >= 0 && (uint64_t)limit - off < want)
            want = (size_t)((uint64_t)limit - off);
        size_t got = 0;
        while (got < want) {
            long n = read(fd, line + got, want - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (got == 0)
            break;
        printf("%08lx  ", off);
        for (size_t k = 0; k < 16; k++) {
            if (k < got)
                printf("%02x ", line[k]);
            else
                printf("   ");
            if (k == 7)
                printf(" ");
        }
        printf(" |");
        for (size_t k = 0; k < got; k++)
            printf("%c", isprint(line[k]) ? line[k] : '.');
        printf("|\n");
        off += got;
        if (got < 16)
            break;
    }
    printf("%08lx\n", off);
    return 0;
}
