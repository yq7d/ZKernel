#include "zk.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        dprintf(2, "usage: cmp file1 file2\n");
        return 2;
    }
    int a = open_arg("cmp", argv[1]), b = open_arg("cmp", argv[2]);
    if (a < 0 || b < 0)
        return 2;
    struct lreader *ra = lr_open(a), *rb = lr_open(b);
    uint64_t byte = 1, line = 1;
    for (;;) {
        if (ra->pos == ra->len) {
            long n = read(a, ra->buf, sizeof ra->buf);
            ra->pos = 0;
            ra->len = n > 0 ? (size_t)n : 0;
        }
        if (rb->pos == rb->len) {
            long n = read(b, rb->buf, sizeof rb->buf);
            rb->pos = 0;
            rb->len = n > 0 ? (size_t)n : 0;
        }
        bool ea = ra->pos == ra->len, eb = rb->pos == rb->len;
        if (ea || eb) {
            if (ea && eb)
                return 0;
            printf("cmp: EOF on %s\n", ea ? argv[1] : argv[2]);
            return 1;
        }
        char x = ra->buf[ra->pos++], y = rb->buf[rb->pos++];
        if (x != y) {
            printf("%s %s differ: byte %lu, line %lu\n", argv[1], argv[2], byte, line);
            return 1;
        }
        if (x == '\n')
            line++;
        byte++;
    }
}
