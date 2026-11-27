#include "zk.h"

int main(void)
{
    int err = reboot(ZK_REBOOT);
    dprintf(2, "reboot: %s\n", strerror(err));
    return 1;
}
