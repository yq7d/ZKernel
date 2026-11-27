// mount, or mount [-t type] source target
#include "cat_proc.h"

int main(int argc, char **argv)
{
    if (argc == 1)
        return cat_file("/proc/mounts");
    const char *type = "fat";
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-t") == 0) {
        type = argv[i + 1];
        i += 2;
    }
    if (argc - i != 2) {
        dprintf(2, "usage: mount [-t type] source target\n");
        return 2;
    }
    int err = mount(argv[i], argv[i + 1], type);
    if (err) {
        dprintf(2, "mount: %s on %s: %s\n", argv[i], argv[i + 1], strerror(err));
        return 1;
    }
    return 0;
}
