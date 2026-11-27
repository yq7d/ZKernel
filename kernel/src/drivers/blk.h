#pragma once
// block devices: 512 byte sectors, a shared write-through cache of 4k blocks
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fs/vfs.h"

#define SECTOR_SIZE 512

struct blkdev {
    char name[16];
    uint64_t sectors;
    int (*read)(struct blkdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf);
    void *priv;
    struct vnode node;    // /dev/<name>
};

// adds /dev/<name>
void blk_register(struct blkdev *d);
// the device behind a /dev path, NULL if it isn't one
struct blkdev *blk_find(const char *path);
int blk_count(void);
struct blkdev *blk_get(int i);

// byte granular, through the cache
int blk_read(struct blkdev *d, uint64_t off, void *buf, size_t len);
int blk_write(struct blkdev *d, uint64_t off, const void *buf, size_t len);
// drops cached blocks of d
void blk_invalidate(struct blkdev *d);

struct blk_stats {
    uint64_t hits, misses, writes;
};
void blk_get_stats(struct blk_stats *s);
