// cut -d c -f N[,N...] [file]
#include "zk.h"

int main(int argc, char **argv)
{
    char delim = '\t';
    bool want[64] = { false };
    bool have_fields = false;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (strncmp(argv[i], "-d", 2) == 0 && argv[i][2]) {
            delim = argv[i][2];
        } else if ((strcmp(argv[i], "-f") == 0 && i + 1 < argc) || strncmp(argv[i], "-f", 2) == 0) {
            const char *list = argv[i][2] ? argv[i] + 2 : argv[++i];
            for (const char *p = list; *p; ) {
                long a = strtol(p, (char **)&p, 10), b = a;
                if (*p == '-') {
                    p++;
                    b = isdigit(*p) ? strtol(p, (char **)&p, 10) : 63;
                }
                for (long f = a; f <= b && f < 64; f++)
                    if (f > 0)
                        want[f] = true;
                if (*p == ',')
                    p++;
                else if (*p)
                    break;
            }
            have_fields = true;
        } else {
            break;
        }
    }
    if (!have_fields) {
        dprintf(2, "usage: cut -d c -f list [file]\n");
        return 2;
    }
    int fd = i < argc ? open_arg("cut", argv[i]) : 0;
    if (fd < 0)
        return 1;
    struct lreader *r = lr_open(fd);
    char *line;
    while (lr_getline(r, &line) >= 0) {
        int field = 1;
        bool first = true;
        char *start = line;
        for (char *p = line;; p++) {
            if (*p == delim || !*p) {
                char end = *p;
                *p = '\0';
                if (field < 64 && want[field]) {
                    printf("%s%s", first ? "" : (char[]){ delim, 0 }, start);
                    first = false;
                }
                if (!end)
                    break;
                field++;
                start = p + 1;
            }
        }
        printf("\n");
    }
    lr_close(r);
    return 0;
}
