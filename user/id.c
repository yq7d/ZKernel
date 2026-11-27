#include "zk.h"

int main(void)
{
    struct zk_cred c;
    getcred(&c);
    struct passwd pw;
    char g[32];
    printf("uid=%u", c.uid);
    if (getpwuid(c.uid, &pw) == 0)
        printf("(%s)", pw.name);
    printf(" gid=%u(%s)", c.gid, group_name(c.gid, g, sizeof g));
    if (c.euid != c.uid) {
        printf(" euid=%u", c.euid);
        if (getpwuid(c.euid, &pw) == 0)
            printf("(%s)", pw.name);
    }
    if (c.ngroups) {
        printf(" groups=");
        for (uint32_t i = 0; i < c.ngroups; i++)
            printf("%s%u(%s)", i ? "," : "", c.groups[i], group_name(c.groups[i], g, sizeof g));
    }
    printf("\n");
    return 0;
}
