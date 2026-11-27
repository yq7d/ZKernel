#pragma once
// unix domain sockets, for linux programs
#include <stdbool.h>
#include <stdint.h>

struct file;
struct vnode;

int64_t unix_socket(int type, int proto);
int64_t unix_socketpair(int type, int proto, uint64_t usv);
// the fd based socket calls, by linux syscall number. a1 (the fd) is f
int64_t unix_call(struct file *f, uint64_t nr, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5, uint64_t a6);
bool unix_is_socket(struct file *f);
bool unix_vnode(struct vnode *v);
void unix_set_nonblock(struct vnode *v, bool on);
