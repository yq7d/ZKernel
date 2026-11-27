#pragma once
// futex wait queues, keyed by address space and user address
#include <stdint.h>

struct address_space;

#define FUTEX_BITSET_ANY 0xffffffffu

// sleeps while *uaddr == val. timeout_ms < 0 waits forever.
// 0, ZK_EAGAIN (value changed), ZK_ETIMEDOUT, ZK_EINTR or ZK_EFAULT
int64_t futex_wait(uint64_t uaddr, uint32_t val, int64_t timeout_ms, uint32_t bitset);
// wakes up to n waiters, returns how many
int64_t futex_wake(struct address_space *as, uint64_t uaddr, int n, uint32_t bitset);
// wakes up to n on uaddr, moves up to n2 more over to uaddr2
int64_t futex_requeue(uint64_t uaddr, int n, uint64_t uaddr2, int n2);
