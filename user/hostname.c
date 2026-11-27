#include "cat_proc.h"

int main(void)
{
    char host[64];
    if (slurp("/etc/hostname", host, sizeof host) <= 0)
        strlcpy(host, "zkernel\n", sizeof host);
    printf("%s", host);
    if (host[strlen(host) - 1] != '\n')
        printf("\n");
    return 0;
}
