#include "zk.h"

int main(void)
{
    struct passwd pw;
    uint32_t uid = geteuid();
    if (getpwuid(uid, &pw) == 0)
        printf("%s\n", pw.name);
    else
        printf("%u\n", uid);
    return 0;
}
