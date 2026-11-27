#include "zk.h"

int main(void)
{
    char cwd[ZK_PATH_MAX];
    int err = getcwd(cwd, sizeof cwd);
    if (err < 0)
        return 1;
    printf("%s\n", cwd);
    return 0;
}
