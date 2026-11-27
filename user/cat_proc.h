#pragma once
#include "zk.h"

// copies a file to stdout, for the little /proc readers
static inline int cat_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(2, "%s: %s\n", path, strerror(fd));
        return 1;
    }
    char buf[4096];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        write_all(1, buf, (size_t)n);
    close(fd);
    return 0;
}

// reads a whole small file into buf
static inline long slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    long total = 0, n;
    while ((size_t)total + 1 < size && (n = read(fd, buf + total, size - 1 - (size_t)total)) > 0)
        total += n;
    buf[total] = '\0';
    close(fd);
    return total;
}
