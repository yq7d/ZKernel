// df [-h]
#include "cat_proc.h"

int main(int argc, char **argv)
{
    bool human = argc > 1 && strcmp(argv[1], "-h") == 0;
    static char buf[4096];
    if (slurp("/proc/mounts", buf, sizeof buf) < 0)
        return 1;
    printf("%-12s %-6s %10s %10s %10s %4s %s\n", "Filesystem", "Type",
           human ? "Size" : "1K-blocks", "Used", "Avail", "Use%", "Mounted on");
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        char src[ZK_PATH_MAX], target[ZK_PATH_MAX];
        char *sp = strchr(line, ' ');
        if (sp) {
            *sp = '\0';
            strlcpy(src, line, sizeof src);
            char *t = sp + 1, *sp2 = strchr(t, ' ');
            if (sp2)
                *sp2 = '\0';
            strlcpy(target, t, sizeof target);
            struct zk_statfs st;
            if (statfs(target, &st) == 0 && st.blocks) {
                uint64_t size = st.blocks * st.block_size;
                uint64_t avail = st.blocks_free * st.block_size;
                uint64_t used = size - avail;
                char a[16], b[16], c[16];
                if (human) {
                    format_size(size, a, sizeof a);
                    format_size(used, b, sizeof b);
                    format_size(avail, c, sizeof c);
                } else {
                    snprintf(a, sizeof a, "%lu", size / 1024);
                    snprintf(b, sizeof b, "%lu", used / 1024);
                    snprintf(c, sizeof c, "%lu", avail / 1024);
                }
                printf("%-12s %-6s %10s %10s %10s %3lu%% %s\n", src, st.fstype, a, b, c,
                       size ? used * 100 / size : 0, target);
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return 0;
}
