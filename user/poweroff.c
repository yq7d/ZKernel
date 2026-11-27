#include "zk.h"

int main(void)
{
    int err = reboot(ZK_POWEROFF);
    dprintf(2, "poweroff: %s\n", strerror(err));
    return 1;
}
