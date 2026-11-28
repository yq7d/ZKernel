#pragma once
// file mappings filled in on first touch, from a page cache that read only
// mappings share. writable private mappings copy on their first write
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct address_space;
struct vnode;

struct fmap {
    uint64_t start, end;
    struct vnode *v;
    uint64_t off;       // file offset of start
    int prot;           // PROT_* bits, 0 for no access
    char name[48];      // the file, for crash reports
    struct fmap *next;
};

int filemap_add(struct address_space *as, uint64_t start, uint64_t len, struct vnode *v,
                uint64_t off, int prot, const char *path);
// "libfoo.so+0x1234" (a file offset) for an address in a file mapping
bool filemap_describe(struct address_space *as, uint64_t addr, char *out, size_t size);
// forgets [start, start + len), the pages themselves are the caller's to unmap
void filemap_remove(struct address_space *as, uint64_t start, uint64_t len);
void filemap_protect(struct address_space *as, uint64_t start, uint64_t len, int prot);
bool filemap_covers(struct address_space *as, uint64_t addr);
// a missing page at addr: maps it if a file mapping covers it and allows the access
bool filemap_fault(struct address_space *as, uint64_t addr, bool write);
bool filemap_clone(struct address_space *dst, struct address_space *src);
void filemap_destroy(struct address_space *as);
