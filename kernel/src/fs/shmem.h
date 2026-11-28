#pragma once
// page backed memory files: memfd_create and shared anonymous mappings.
// mappings take a reference on each page, so they outlive the file
#include <stdbool.h>
#include <stdint.h>

struct vnode;

struct vnode *shmem_new(uint64_t size);
bool shmem_is(struct vnode *v);
// memfd seals, F_SEAL_* bits. adding fails once F_SEAL_SEAL is set
int shmem_add_seals(struct vnode *v, uint32_t seals);
uint32_t shmem_seals(struct vnode *v);
// for memfd_create(MFD_ALLOW_SEALING), the only kind that starts sealable
void shmem_allow_sealing(struct vnode *v);
// physical page index of v, made (zeroed) if it isn't there yet. 0 when
// out of memory
uint64_t shmem_page(struct vnode *v, uint64_t index);
