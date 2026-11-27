// ping [-c count] host
#include "zk.h"

int main(int argc, char **argv)
{
    int count = 4, i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-c") == 0) {
        count = atoi(argv[i + 1]);
        i += 2;
    }
    if (i >= argc) {
        dprintf(2, "usage: ping [-c count] host\n");
        return 2;
    }
    uint32_t ip;
    int err = resolve(argv[i], &ip);
    if (err) {
        dprintf(2, "ping: %s: %s\n", argv[i], strerror(err));
        return 1;
    }
    char addr[16];
    inet_format(ip, addr, sizeof addr);
    printf("PING %s (%s)\n", argv[i], addr);
    int ok = 0;
    for (int n = 1; n <= count; n++) {
        long rtt = ping(ip, 2000);
        if (rtt >= 0) {
            printf("reply from %s: seq=%d time=%ld ms\n", addr, n, rtt);
            ok++;
        } else {
            printf("seq=%d: %s\n", n, strerror((int)rtt));
        }
        if (n < count)
            sleep_ms(rtt >= 0 && rtt < 1000 ? (uint64_t)(1000 - rtt) : 0);
    }
    printf("%d sent, %d received\n", count, ok);
    return ok ? 0 : 1;
}
