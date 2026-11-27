// ustar archive -> ramfs
#include "fs/vfs.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

struct ustar {
    char name[100];
    char mode[8], uid[8], gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char type;
    char link[100];
    char magic[6];
    char version[2];
    char uname[32], gname[32];
    char devmajor[8], devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

_Static_assert(sizeof(struct ustar) == 512, "ustar header is 512 bytes");

static uint64_t octal(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++)
        v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

// mkdir -p for the parents of path
static void make_parents(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        vfs_mkdir(path);
        *p = '/';
    }
}

int initrd_unpack(const void *data, size_t size)
{
    const uint8_t *base = data;
    size_t off = 0;
    int files = 0;

    while (off + 512 <= size) {
        const struct ustar *h = (const void *)(base + off);
        if (!h->name[0])
            break;    // two zero blocks end the archive
        if (memcmp(h->magic, "ustar", 5) != 0) {
            pr_warn("initrd: bad header at offset %zu\n", off);
            break;
        }
        uint64_t fsize = octal(h->size, sizeof h->size);
        off += 512;
        if (fsize > size - off) {
            pr_warn("initrd: truncated archive\n");
            break;
        }

        char raw[ZK_PATH_MAX], path[ZK_PATH_MAX];
        raw[0] = '\0';
        if (h->prefix[0]) {
            size_t n = strnlen(h->prefix, sizeof h->prefix);
            if (n + 2 < sizeof raw) {
                memcpy(raw, h->prefix, n);
                raw[n++] = '/';
                raw[n] = '\0';
            }
        }
        size_t n = strlen(raw);
        size_t nl = strnlen(h->name, sizeof h->name);
        if (n + nl < sizeof raw) {
            memcpy(raw + n, h->name, nl);
            raw[n + nl] = '\0';
        }

        if (vfs_normalize("/", raw, path) == 0 && strcmp(path, "/") != 0) {
            make_parents(path);
            if (h->type == '5') {
                vfs_mkdir(path);
            } else if (h->type == '0' || h->type == '\0') {
                struct file *f;
                if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f) == 0) {
                    if (file_write(f, base + off, fsize) == (int64_t)fsize) {
                        f->v->mtime = (int64_t)octal(h->mtime, sizeof h->mtime);
                        files++;
                    }
                    else
                        pr_warn("initrd: couldn't write %s\n", path);
                    file_put(f);
                }
            }
        }
        off += (fsize + 511) & ~511UL;
    }
    return files;
}
