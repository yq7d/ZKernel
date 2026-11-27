#include "zk.h"

int main(void)
{
    struct zk_netinfo ni;
    if (netinfo(&ni) < 0 || (!ni.up && !ni.mac[0] && !ni.mac[1])) {
        printf("no network interface\n");
        return 1;
    }
    char ip[16], mask[16], gw[16], dns[16], rx[16], tx[16];
    inet_format(ni.ip, ip, sizeof ip);
    inet_format(ni.mask, mask, sizeof mask);
    inet_format(ni.gw, gw, sizeof gw);
    inet_format(ni.dns, dns, sizeof dns);
    format_size(ni.rx_bytes, rx, sizeof rx);
    format_size(ni.tx_bytes, tx, sizeof tx);
    printf("eth0: %s\n", ni.up ? "up" : "down");
    printf("    ether %02x:%02x:%02x:%02x:%02x:%02x\n", ni.mac[0], ni.mac[1], ni.mac[2],
           ni.mac[3], ni.mac[4], ni.mac[5]);
    printf("    inet %s  netmask %s\n", ip, mask);
    printf("    gateway %s  dns %s\n", gw, dns);
    printf("    rx %lu packets (%s)  tx %lu packets (%s)\n", ni.rx_packets, rx, ni.tx_packets, tx);
    return 0;
}
