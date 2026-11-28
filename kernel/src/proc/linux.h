#pragma once
// running linux x86_64 programs. they come in through the syscall
// instruction, zos programs use int 0x80, so the two never mix.
//
// linux programs see the tree under /compat/linux first: a path that
// exists there wins, anything else falls through to the real one
#include <stdbool.h>
#include <stdint.h>

#define LINUX_ROOT "/compat/linux"
// the read only programs bundled in the iso, tried after LINUX_ROOT
#define APPS_ROOT  "/compat/apps"

void linux_init(void);
// absolute normalized path in, the path a linux program means out (ZK_PATH_MAX)
void linux_resolve(const char *path, char *out, bool creating);
// syscall_entry.asm
void syscall_entry(void);
extern uint64_t syscall_kstack;
