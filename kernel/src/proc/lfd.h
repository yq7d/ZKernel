#pragma once
// eventfd, timerfd, signalfd, epoll, select and the interval timer, for
// linux programs. flags are linux's (O_CLOEXEC, O_NONBLOCK)
#include <stddef.h>
#include <stdint.h>

int64_t lfd_eventfd(uint32_t initval, int flags);
int64_t lfd_timerfd_create(int clock, int flags);
int64_t lfd_timerfd_settime(int fd, int flags, uint64_t unew, uint64_t uold);
int64_t lfd_timerfd_gettime(int fd, uint64_t ucur);
int64_t lfd_signalfd(int fd, uint64_t umask, size_t size, int flags);
int64_t lfd_epoll_create(int flags);
int64_t lfd_epoll_ctl(int epfd, int op, int fd, uint64_t uev);
// usigmask is swapped in while waiting, 0 leaves the mask alone
int64_t lfd_epoll_wait(int epfd, uint64_t uevents, int max, int64_t timeout,
                       uint64_t usigmask);
int64_t lfd_select(int nfds, uint64_t urd, uint64_t uwr, uint64_t uex, int64_t timeout_ms);
int64_t lfd_alarm(uint32_t secs);
int64_t lfd_setitimer(int which, uint64_t unew, uint64_t uold);
int64_t lfd_getitimer(int which, uint64_t ucur);
