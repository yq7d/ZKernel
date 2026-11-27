#pragma once
// shared between the kernel and user programs
//
// int 0x80, number in rax, args in rdi rsi rdx r10 r8 r9, result in rax.
// negative results are errors

#define ZK_SYSCALL_VECTOR 0x80

#define SYS_EXIT        0    // (int code)
#define SYS_WRITE       1    // (int fd, const void *buf, size_t len) -> written
#define SYS_READ        2    // (int fd, void *buf, size_t len) -> read, blocks for at least 1
#define SYS_GETPID      3    // () -> id
#define SYS_SLEEP_MS    4    // (uint64_t ms)
#define SYS_YIELD       5    // ()
#define SYS_UPTIME_MS   6    // () -> ms since boot
#define SYS_ALLOC_PAGES 7    // (size_t count) -> address of zeroed pages

#define ZK_EFAULT  (-1)
#define ZK_EINVAL  (-2)
#define ZK_ENOMEM  (-3)
#define ZK_ENOSYS  (-4)
