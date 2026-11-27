// things a user program must not be able to do. each should get it killed
#include "zk.h"

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";

    if (strcmp(what, "kread") == 0) {
        printf("reading kernel memory...\n");
        printf("%d\n", *(volatile int *)0xffffffff80000000UL);
    } else if (strcmp(what, "kwrite") == 0) {
        printf("writing kernel memory...\n");
        *(volatile int *)0xffffffff80000000UL = 1;
    } else if (strcmp(what, "cli") == 0) {
        printf("disabling interrupts...\n");
        __asm__ volatile ("cli");
    } else if (strcmp(what, "null") == 0) {
        printf("null pointer...\n");
        *(volatile int *)0 = 1;
    } else if (strcmp(what, "code") == 0) {
        printf("writing own code...\n");
        *(volatile uint8_t *)main = 0xC3;
    } else if (strcmp(what, "badptr") == 0) {
        // the kernel has to refuse this, not crash
        long r = write(1, (void *)0xffffffff80000000UL, 16);
        printf("write with a kernel pointer returned %ld\n", r);
        return r < 0 ? 0 : 1;
    } else {
        printf("usage: fault <kread|kwrite|cli|null|code|badptr>\n");
        return 1;
    }

    printf("still alive, that's a bug\n");
    return 1;
}
