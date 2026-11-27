#include "zk.h"

int main(int argc, char **argv)
{
    printf("hello from user mode! pid %ld, uptime %lu ms\n", getpid(), uptime_ms());

    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);

    char *buf = malloc(64);
    strlcpy(buf, "heap works too", 64);
    printf("%s (%p)\n", buf, (void *)buf);
    return 0;
}
