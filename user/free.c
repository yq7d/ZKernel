// free [-h]
#include "cat_proc.h"

static uint64_t field(const char *text, const char *name)
{
    const char *p = strstr(text, name);
    if (!p)
        return 0;
    p += strlen(name);
    return (uint64_t)strtol(p, NULL, 10) * 1024;
}

int main(int argc, char **argv)
{
    bool human = argc > 1 && strcmp(argv[1], "-h") == 0;
    char buf[1024];
    if (slurp("/proc/meminfo", buf, sizeof buf) < 0)
        return 1;
    uint64_t total = field(buf, "MemTotal:"), freeb = field(buf, "MemFree:");
    uint64_t v[3] = { total, total - freeb, freeb };
    printf("%14s %10s %10s\nMem:  ", "total", "used", "free");
    for (int i = 0; i < 3; i++) {
        char s[16];
        if (human)
            format_size(v[i], s, sizeof s);
        else
            snprintf(s, sizeof s, "%lu", v[i] / 1024);
        printf("%*s ", i == 0 ? 8 : 10, s);
    }
    printf("\n");
    return 0;
}
