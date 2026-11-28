// startx: x11 from alpine packages on the framebuffer, run from a text shell
#include "zk.h"

#define XINIT "/usr/lib/zos/xinit.sh"

// installed with pkg, or bundled in the iso
static const char *const roots[] = { "/compat/linux", "/compat/apps" };

static bool have(const char *rel, char *out, size_t size)
{
    struct zk_stat st;
    for (int i = 0; i < 2; i++) {
        snprintf(out, size, "%s%s", roots[i], rel);
        if (stat(out, &st) == 0)
            return true;
    }
    return false;
}

int main(void)
{
    char sh[ZK_PATH_MAX], xorg[ZK_PATH_MAX];
    if (!have("/usr/bin/Xorg", xorg, sizeof xorg) || !have("/bin/sh", sh, sizeof sh)) {
        printf("startx: xorg isn't installed, get it with:\n"
               "  sudo pkg install xorg-server xf86-video-fbdev xf86-input-evdev twm xterm xsetroot font-misc-misc\n");
        return 1;
    }
    char *argv[] = { "sh", XINIT, NULL };
    int err = execv(sh, argv);
    printf("startx: can't run %s: %s\n", sh, strerror(err));
    return 1;
}
