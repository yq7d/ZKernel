#include "zk.h"

int main(int argc, char **argv)
{
    bool newline = true;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        i++;
    }
    for (; i < argc; i++)
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (newline)
        printf("\n");
    return 0;
}
