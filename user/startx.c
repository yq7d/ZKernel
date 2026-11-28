// startx: x11 from alpine packages on the framebuffer, run from a text shell
#include "zk.h"

#define SH    "/compat/linux/bin/sh"
#define XORG  "/compat/linux/usr/bin/Xorg"
#define XINIT "/usr/lib/zos/xinit.sh"

int main(void)
{
    struct zk_stat st;
    if (stat(XORG, &st) != 0 || stat(SH, &st) != 0) {
        printf("startx: xorg isn't installed, get it with:\n"
               "  sudo pkg install xorg-server xf86-video-fbdev xf86-input-evdev twm xterm xsetroot font-misc-misc\n");
        return 1;
    }
    char *argv[] = { "sh", XINIT, NULL };
    int err = execv(SH, argv);
    printf("startx: can't run %s: %s\n", SH, strerror(err));
    return 1;
}
