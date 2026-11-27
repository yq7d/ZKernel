#pragma once
// syscall helpers shared by the zos and linux syscall tables
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "proc/proc.h"

// a NULL terminated user string array, copied into the kernel
struct strv {
    char *ptrs[PROC_MAX_ENV + 1];
    char *buf;
    size_t used;
};

struct exec_args {
    char path[ZK_PATH_MAX];
    struct strv argv, envp;
    char buf[];
};

bool user_ok(uint64_t addr, size_t len, bool write);
int copy_out(uint64_t dst, const void *src, size_t len);
int copy_in(void *dst, uint64_t src, size_t len);
int get_string(uint64_t addr, char *buf, size_t size);
// user path -> absolute normalized path
int get_path(uint64_t addr, char *out);
// path, argv and envp in one allocation, free with kfree
int get_exec_args(uint64_t upath, uint64_t uargv, uint64_t uenvp, struct exec_args **out);

int64_t sys_poll(uint64_t ufds, size_t n, int64_t timeout);
int64_t sys_kill(uint64_t pid);
int64_t sys_setuid(uint32_t uid);
int64_t sys_setgid(uint32_t gid);
int64_t sys_setgroups(size_t n, uint64_t ugroups);
int64_t sys_getrandom(uint64_t ubuf, size_t len);
int64_t sys_pipe(uint64_t ufds);
int64_t sys_statfs(uint64_t upath, uint64_t ust);
