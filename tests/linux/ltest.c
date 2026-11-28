// built with the host's gcc -static, runs under the linux layer
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int passed, failed;
#define CHECK(c) do { if (c) passed++; else { failed++; printf("  FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static volatile sig_atomic_t got, got_pid, got_chld;
static volatile int on_alt;
static char altstack[64 * 1024];
static sigjmp_buf segv_jmp;

static void on_usr(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    got = sig;
    got_pid = si->si_pid;
    // the handler's own fpu use mustn't leak into the interrupted code
    volatile double d = 3.0;
    d = d * d * d;
    char local;
    on_alt = &local >= altstack && &local < altstack + sizeof altstack;
}

static void on_chld(int sig) { (void)sig; got_chld++; }
static void on_segv(int sig) { (void)sig; siglongjmp(segv_jmp, 1); }

static void handle(int sig, void (*fn)(int, siginfo_t *, void *), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | flags;
    sigaction(sig, &sa, NULL);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static int ready_flag;
static __thread int tls_var = 5;
static sem_t sem;
static pthread_barrier_t barrier;
static volatile pid_t worker_tid;
static volatile int worker_got;

static void *count_up(void *arg)
{
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
        if (i % 1000 == 0)
            sched_yield();
    }
    // every thread has its own copy
    tls_var += (int)(long)arg;
    return (void *)(long)tls_var;
}

static void *wait_ready(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!ready_flag)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    sem_post(&sem);
    return NULL;
}

static void *at_barrier(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&barrier);
    return NULL;
}

static void on_worker_sig(int sig) { (void)sig; worker_got = (int)syscall(SYS_gettid); }

static void *sig_worker(void *arg)
{
    (void)arg;
    worker_tid = (pid_t)syscall(SYS_gettid);
    while (!worker_got)
        usleep(1000);
    return NULL;
}

static void *exits_all(void *arg)
{
    (void)arg;
    usleep(20000);
    exit(7);
}

static void *blocks_forever(void *arg)
{
    (void)arg;
    for (;;)
        pause();
}

static void test_threads(void)
{
    // a mutex keeps the count right, tls is per thread, join gets the result
    pthread_t t[4];
    counter = 0;
    for (long i = 0; i < 4; i++)
        CHECK(pthread_create(&t[i], NULL, count_up, (void *)(i + 1)) == 0);
    int sum = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        CHECK(pthread_join(t[i], &r) == 0);
        sum += (int)(long)r;
    }
    CHECK(counter == 80000);
    CHECK(sum == 5 * 4 + 1 + 2 + 3 + 4);
    CHECK(tls_var == 5);

    // condition variables and semaphores
    sem_init(&sem, 0, 0);
    ready_flag = 0;
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, wait_ready, NULL);
    usleep(20000);
    pthread_mutex_lock(&lock);
    ready_flag = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < 3; i++)
        sem_wait(&sem);
    for (int i = 0; i < 3; i++)
        CHECK(pthread_join(t[i], NULL) == 0);

    // a timed wait times out
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50 * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&lock);
    CHECK(pthread_cond_timedwait(&cond, &lock, &ts) == ETIMEDOUT);
    pthread_mutex_unlock(&lock);

    // barriers
    pthread_barrier_init(&barrier, NULL, 4);
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, at_barrier, NULL);
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 3; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    pthread_barrier_destroy(&barrier);

    // threads have their own ids, the process keeps one pid
    CHECK(syscall(SYS_gettid) == getpid());

    // pthread_kill reaches that one thread
    signal(SIGUSR1, on_worker_sig);
    worker_got = 0;
    worker_tid = 0;
    pthread_create(&t[0], NULL, sig_worker, NULL);
    while (!worker_tid)
        usleep(1000);
    CHECK(pthread_kill(t[0], SIGUSR1) == 0);
    CHECK(pthread_join(t[0], NULL) == 0);
    CHECK(worker_got == worker_tid && worker_tid != getpid());
    signal(SIGUSR1, SIG_DFL);

    // exit from any thread ends the whole process, even with threads stuck
    int status;
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t a, b;
        pthread_create(&b, NULL, blocks_forever, NULL);
        pthread_create(&a, NULL, exits_all, NULL);
        for (;;)
            pause();
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7);

    // a fatal signal takes every thread down
    pid = fork();
    if (pid == 0) {
        pthread_t a;
        pthread_create(&a, NULL, blocks_forever, NULL);
        for (;;)
            pause();
    }
    usleep(30000);
    kill(pid, SIGTERM);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);

    // the main thread leaving with pthread_exit lets the rest finish
    pid = fork();
    if (pid == 0) {
        pthread_t a;
        pthread_create(&a, NULL, exits_all, NULL);
        pthread_exit(NULL);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7);

    // fork from a threaded process gives a child with one thread
    pthread_create(&t[0], NULL, wait_ready, NULL);
    pid = fork();
    if (pid == 0)
        _exit(3);
    CHECK(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 3);
    sem_wait(&sem);
    pthread_join(t[0], NULL);

    // exec from a threaded process
    pthread_create(&t[0], NULL, blocks_forever, NULL);
    pid = fork();
    if (pid == 0) {
        pthread_t a;
        pthread_create(&a, NULL, blocks_forever, NULL);
        char *args[] = { "true", NULL };
        execv("/bin/true", args);
        _exit(99);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int send_fd(int sock, int fd)
{
    char byte = 'F';
    struct iovec iov = { &byte, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return (int)sendmsg(sock, &m, 0);
}

static int recv_fd(int sock)
{
    char byte;
    struct iovec iov = { &byte, 1 };
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.buf, .msg_controllen = sizeof u.buf };
    if (recvmsg(sock, &m, 0) != 1 || byte != 'F')
        return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (!c || c->cmsg_type != SCM_RIGHTS)
        return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof fd);
    return fd;
}

static void test_unix(void)
{
    // socketpair, both ways, then eof
    int sv[2];
    char buf[64];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CHECK(write(sv[0], "ping", 4) == 4 && read(sv[1], buf, sizeof buf) == 4 && memcmp(buf, "ping", 4) == 0);
    CHECK(send(sv[1], "pong", 4, 0) == 4 && recv(sv[0], buf, sizeof buf, 0) == 4);
    CHECK(recv(sv[0], buf, sizeof buf, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    close(sv[1]);
    CHECK(read(sv[0], buf, sizeof buf) == 0);
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(sv[0], "x", 1) == -1 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(sv[0]);

    // datagrams keep their boundaries
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0);
    send(sv[0], "one", 3, 0);
    send(sv[0], "three", 5, 0);
    CHECK(recv(sv[1], buf, sizeof buf, 0) == 3 && recv(sv[1], buf, sizeof buf, 0) == 5);
    close(sv[0]);
    close(sv[1]);

    // a server on a path, a client in another process
    const char *path = "/tmp/ltest.sock";
    unlink(path);
    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, path);
    CHECK(srv >= 0 && bind(srv, (struct sockaddr *)&a, sizeof a) == 0);
    CHECK(bind(socket(AF_UNIX, SOCK_STREAM, 0), (struct sockaddr *)&a, sizeof a) == -1 && errno == EADDRINUSE);
    CHECK(listen(srv, 8) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISSOCK(st.st_mode));
    pid_t pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof a) != 0)
            _exit(1);
        // a megabyte, more than the queue holds, so the writer has to wait
        static char big[1 << 20];
        for (size_t i = 0; i < sizeof big; i++)
            big[i] = (char)(i * 7);
        size_t off = 0;
        while (off < sizeof big) {
            ssize_t n = write(c, big + off, sizeof big - off);
            if (n <= 0)
                _exit(2);
            off += (size_t)n;
        }
        _exit(0);
    }
    struct pollfd pf = { srv, POLLIN, 0 };
    CHECK(poll(&pf, 1, 2000) == 1 && (pf.revents & POLLIN));
    int conn = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
    CHECK(conn >= 0);
    struct ucred cr;
    socklen_t crl = sizeof cr;
    CHECK(getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cr, &crl) == 0 && cr.pid == pid);
    static char got[1 << 20];
    size_t total = 0;
    ssize_t n;
    while ((n = read(conn, got + total, sizeof got - total)) > 0)
        total += (size_t)n;
    int good = total == sizeof got;
    for (size_t i = 0; good && i < sizeof got; i++)
        good = got[i] == (char)(i * 7);
    CHECK(good);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(conn);
    close(srv);
    // the file stays, but nobody's listening any more
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == -1 && errno == ECONNREFUSED);
    close(c);
    unlink(path);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof a) == -1 && errno == ENOENT);
    close(c);

    // the abstract namespace, and getsockname
    srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un ab = { .sun_family = AF_UNIX };
    memcpy(ab.sun_path, "\0ltest-abstract", 15);
    socklen_t al = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 15);
    CHECK(bind(srv, (struct sockaddr *)&ab, al) == 0 && listen(srv, 1) == 0);
    c = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(connect(c, (struct sockaddr *)&ab, al) == 0);
    conn = accept(srv, NULL, NULL);
    struct sockaddr_un got_a;
    socklen_t gl = sizeof got_a;
    CHECK(conn >= 0 && getsockname(conn, (struct sockaddr *)&got_a, &gl) == 0 &&
          gl == al && memcmp(got_a.sun_path, ab.sun_path, 15) == 0);
    close(c);
    close(conn);
    close(srv);

    // passing a pipe to another process
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int fd = recv_fd(sv[1]);
        if (fd < 0)
            _exit(1);
        write(fd, "through a passed fd", 19);
        _exit(0);
    }
    close(sv[1]);
    int p[2];
    pipe(p);
    CHECK(send_fd(sv[0], p[1]) == 1);
    close(p[1]);
    memset(buf, 0, sizeof buf);
    CHECK(read(p[0], buf, sizeof buf) == 19 && strcmp(buf, "through a passed fd") == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);
    close(p[0]);
    close(sv[0]);

    // O_NONBLOCK on a pipe
    pipe(p);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    CHECK(fcntl(p[0], F_GETFL) & O_NONBLOCK);
    CHECK(read(p[0], buf, 1) == -1 && errno == EAGAIN);
    close(p[0]);
    close(p[1]);
    CHECK(pipe2(p, O_NONBLOCK) == 0 && read(p[0], buf, 1) == -1 && errno == EAGAIN);
    close(p[0]);
    close(p[1]);

    // lots of fds at once
    int many[200], nmany = 0;
    for (int i = 0; i < 200; i++) {
        many[i] = dup(1);
        if (many[i] >= 0)
            nmany++;
    }
    CHECK(nmany == 200);
    for (int i = 0; i < 200; i++)
        close(many[i]);
}

static volatile int alarms;
static void on_alarm(int sig) { (void)sig; alarms++; }

static void test_events(void)
{
    // eventfd counts up and reads back the total
    int e = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    uint64_t v = 3;
    CHECK(e >= 0 && write(e, &v, 8) == 8 && write(e, &v, 8) == 8);
    CHECK(read(e, &v, 8) == 8 && v == 6);
    CHECK(read(e, &v, 8) == -1 && errno == EAGAIN);

    // timerfd ticks
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 20 * 1000000 }, { 0, 20 * 1000000 } };
    CHECK(t >= 0 && timerfd_settime(t, 0, &its, NULL) == 0);
    usleep(110000);
    CHECK(read(t, &v, 8) == 8 && v >= 3 && v <= 7);

    // epoll over a pipe, the eventfd and the timer
    int p[2];
    pipe(p);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 1 };
    CHECK(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0);
    ev.data.u32 = 2;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) == 0);
    ev.data.u32 = 3;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) == -1 && errno == EEXIST);
    struct epoll_event got[8];
    read(t, &v, 8);
    its.it_interval.tv_nsec = 0;
    its.it_value.tv_nsec = 30 * 1000000;
    timerfd_settime(t, 0, &its, NULL);
    // only the timer, after it fires
    int n = epoll_wait(ep, got, 8, 1000);
    CHECK(n == 1 && got[0].data.u32 == 3);
    read(t, &v, 8);
    CHECK(epoll_wait(ep, got, 8, 0) == 0);
    write(p[1], "x", 1);
    v = 1;
    write(e, &v, 8);
    n = epoll_wait(ep, got, 8, 1000);
    int seen = 0;
    for (int i = 0; i < n; i++)
        seen |= 1 << got[i].data.u32;
    CHECK(n == 2 && seen == ((1 << 1) | (1 << 2)));
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) == 0);
    n = epoll_wait(ep, got, 8, 0);
    CHECK(n == 1 && got[0].data.u32 == 2);
    // a closed fd drops out by itself
    close(e);
    CHECK(epoll_wait(ep, got, 8, 0) == 0);
    close(ep);
    close(t);

    // select
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    struct timeval tv = { 1, 0 };
    CHECK(select(p[0] + 1, &rd, NULL, NULL, &tv) == 1 && FD_ISSET(p[0], &rd));
    char c;
    read(p[0], &c, 1);
    FD_ZERO(&rd);
    FD_SET(p[0], &rd);
    tv = (struct timeval){ 0, 30000 };
    CHECK(select(p[0] + 1, &rd, NULL, NULL, &tv) == 0);
    close(p[0]);
    close(p[1]);

    // signalfd takes blocked signals as data
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, NULL);
    int sf = signalfd(-1, &m, SFD_CLOEXEC);
    raise(SIGUSR2);
    struct signalfd_siginfo si;
    CHECK(sf >= 0 && read(sf, &si, sizeof si) == sizeof si && si.ssi_signo == SIGUSR2 &&
          (pid_t)si.ssi_pid == getpid());
    close(sf);
    sigprocmask(SIG_UNBLOCK, &m, NULL);

    // alarm and setitimer
    signal(SIGALRM, on_alarm);
    alarms = 0;
    alarm(1);
    CHECK(alarm(1) == 1);
    alarm(0);
    struct itimerval it = { { 0, 20000 }, { 0, 20000 } };
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
    while (alarms < 3)
        pause();
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_REAL, &it, NULL);
    CHECK(alarms >= 3);
    signal(SIGALRM, SIG_DFL);

    // memfd: shared between a mapping here and one in a child
    int mf = memfd_create("ltest", MFD_CLOEXEC);
    CHECK(mf >= 0 && ftruncate(mf, 8192) == 0);
    struct stat st;
    CHECK(fstat(mf, &st) == 0 && st.st_size == 8192);
    char *sh = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
    CHECK(sh != MAP_FAILED);
    pid_t pid = fork();
    if (pid == 0) {
        char *c2 = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, mf, 0);
        strcpy(c2 + 4096, "from the child");
        sh[0] = 'Z';
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    CHECK(strcmp(sh + 4096, "from the child") == 0 && sh[0] == 'Z');
    char rb[16] = { 0 };
    CHECK(pread(mf, rb, 14, 4096) == 14 && strcmp(rb, "from the child") == 0);
    munmap(sh, 8192);
    close(mf);

    // shared anonymous memory survives fork as shared
    int *cnt = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *cnt = 0;
    pid = fork();
    if (pid == 0) {
        *cnt = 41;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(*cnt == 41);
    // private anonymous memory doesn't
    int *priv = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *priv = 1;
    pid = fork();
    if (pid == 0) {
        *priv = 2;
        _exit(0);
    }
    waitpid(pid, &status, 0);
    CHECK(*priv == 1);
    munmap(cnt, 4096);
    munmap(priv, 4096);
}

static void test_signals(void)
{
    // a handler runs, with siginfo, and the fpu state survives it
    handle(SIGUSR1, on_usr, 0);
    volatile double x = 1.25;
    got = 0;
    raise(SIGUSR1);
    CHECK(got == SIGUSR1 && got_pid == getpid());
    CHECK(x * 4 == 5.0);

    // blocked signals wait until they're unblocked
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &old);
    got = 0;
    raise(SIGUSR1);
    CHECK(got == 0);
    sigset_t pend;
    CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1));
    sigprocmask(SIG_SETMASK, &old, NULL);
    CHECK(got == SIGUSR1);

    // a fault goes to the handler, which jumps out of it
    signal(SIGSEGV, on_segv);
    int jumped = 0;
    if (sigsetjmp(segv_jmp, 1) == 0)
        *(volatile int *)8 = 1;
    else
        jumped = 1;
    CHECK(jumped);
    signal(SIGSEGV, SIG_DFL);

    // ignored signals do nothing
    signal(SIGTERM, SIG_IGN);
    CHECK(kill(getpid(), SIGTERM) == 0);
    signal(SIGTERM, SIG_DFL);

    // default actions kill, and wait says which signal did it
    int status;
    pid_t pid = fork();
    if (pid == 0) {
        for (;;)
            pause();
    }
    usleep(20000);
    CHECK(kill(pid, SIGTERM) == 0);
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    pid = fork();
    if (pid == 0)
        abort();
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
          WTERMSIG(status) == SIGABRT && WCOREDUMP(status));
    pid = fork();
    if (pid == 0)
        *(volatile int *)0 = 1;
    CHECK(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);

    // SIGCHLD when a child exits
    signal(SIGCHLD, on_chld);
    got_chld = 0;
    pid = fork();
    if (pid == 0)
        _exit(0);
    while (!got_chld)
        usleep(1000);
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    signal(SIGCHLD, SIG_DFL);

    // a blocking read gives up with EINTR, or carries on with SA_RESTART
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR1);
        usleep(50000);
        kill(getppid(), SIGUSR1);
        usleep(50000);
        write(fds[1], "x", 1);
        _exit(0);
    }
    char c;
    got = 0;
    errno = 0;
    CHECK(read(fds[0], &c, 1) == -1 && errno == EINTR && got == SIGUSR1);
    handle(SIGUSR1, on_usr, SA_RESTART);
    got = 0;
    CHECK(read(fds[0], &c, 1) == 1 && c == 'x' && got == SIGUSR1);
    waitpid(pid, NULL, 0);
    close(fds[0]);
    close(fds[1]);

    // sigsuspend waits with a temporary mask, then puts the old one back
    sigprocmask(SIG_BLOCK, &set, &old);
    pid = fork();
    if (pid == 0) {
        usleep(30000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    sigset_t none;
    sigemptyset(&none);
    got = 0;
    CHECK(sigsuspend(&none) == -1 && errno == EINTR && got == SIGUSR1);
    sigset_t now;
    sigprocmask(SIG_SETMASK, NULL, &now);
    CHECK(sigismember(&now, SIGUSR1));
    sigprocmask(SIG_SETMASK, &old, NULL);
    waitpid(pid, NULL, 0);

    // handlers can run on their own stack
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    CHECK(sigaltstack(&ss, NULL) == 0);
    handle(SIGUSR2, on_usr, SA_ONSTACK);
    on_alt = 0;
    raise(SIGUSR2);
    CHECK(on_alt);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

#define HAS(bits, n) ((bits)[(n) / 8] & (1 << ((n) % 8)))

static void test_devices(void)
{
    // fbdev: modes and a mapping of the screen
    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    CHECK(fb >= 0);
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    CHECK(ioctl(fb, FBIOGET_VSCREENINFO, &var) == 0 && var.xres > 0 && var.bits_per_pixel == 32);
    CHECK(ioctl(fb, FBIOPUT_VSCREENINFO, &var) == 0);
    CHECK(ioctl(fb, FBIOGET_FSCREENINFO, &fix) == 0 && fix.line_length >= var.xres * 4 &&
          fix.smem_len >= fix.line_length * var.yres && fix.visual == FB_VISUAL_TRUECOLOR);
    uint32_t *px = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    CHECK(px != MAP_FAILED);
    if (px != MAP_FAILED) {
        px[0] = 0x00ff8800;
        CHECK(px[0] == 0x00ff8800);
        CHECK(munmap(px, fix.smem_len) == 0);
    }
    close(fb);

    // evdev
    int kb = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
    int ms = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    CHECK(kb >= 0 && ms >= 0);
    char name[64] = "";
    CHECK(ioctl(kb, EVIOCGNAME(sizeof name), name) > 0 && strstr(name, "keyboard"));
    int ver = 0;
    CHECK(ioctl(kb, EVIOCGVERSION, &ver) == 0 && ver == EV_VERSION);
    uint8_t ev[4] = { 0 }, keys[96] = { 0 }, rel[2] = { 0 };
    CHECK(ioctl(kb, EVIOCGBIT(0, sizeof ev), ev) > 0 && HAS(ev, EV_KEY) && !HAS(ev, EV_REL));
    CHECK(ioctl(kb, EVIOCGBIT(EV_KEY, sizeof keys), keys) > 0 && HAS(keys, KEY_A) &&
          HAS(keys, KEY_LEFTMETA) && !HAS(keys, BTN_LEFT));
    memset(ev, 0, sizeof ev);
    memset(keys, 0, sizeof keys);
    CHECK(ioctl(ms, EVIOCGBIT(0, sizeof ev), ev) > 0 && HAS(ev, EV_REL));
    CHECK(ioctl(ms, EVIOCGBIT(EV_KEY, sizeof keys), keys) > 0 && HAS(keys, BTN_LEFT));
    CHECK(ioctl(ms, EVIOCGBIT(EV_REL, sizeof rel), rel) > 0 && HAS(rel, REL_X) && HAS(rel, REL_Y));
    struct input_id id;
    CHECK(ioctl(ms, EVIOCGID, &id) == 0 && id.bustype == BUS_I8042);
    int clk = CLOCK_MONOTONIC;
    CHECK(ioctl(kb, EVIOCSCLOCKID, &clk) == 0);
    struct input_event ie;
    CHECK(read(kb, &ie, sizeof ie) == -1 && errno == EAGAIN);
    struct stat a, b;
    CHECK(fstat(kb, &a) == 0 && fstat(ms, &b) == 0 && S_ISCHR(a.st_mode) &&
          major(a.st_rdev) == 13 && a.st_rdev != b.st_rdev);
    struct pollfd pf = { kb, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 0);
    close(kb);
    close(ms);

    // the console as a vt
    int vt = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    CHECK(vt >= 0);
    struct vt_stat vs;
    CHECK(ioctl(vt, VT_GETSTATE, &vs) == 0 && vs.v_active >= 1);
    int n = -1, mode = -1;
    CHECK(ioctl(vt, VT_OPENQRY, &n) == 0 && n >= 1);
    CHECK(ioctl(vt, KDGETMODE, &mode) == 0 && mode == KD_TEXT);
    CHECK(ioctl(vt, KDSETMODE, KD_GRAPHICS) == 0 && ioctl(vt, KDGETMODE, &mode) == 0 && mode == KD_GRAPHICS);
    CHECK(ioctl(vt, KDSETMODE, KD_TEXT) == 0);
    CHECK(ioctl(vt, KDGKBMODE, &mode) == 0);
    struct vt_mode vm = { .mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2 };
    CHECK(ioctl(vt, VT_SETMODE, &vm) == 0);
    memset(&vm, 0, sizeof vm);
    CHECK(ioctl(vt, VT_GETMODE, &vm) == 0 && vm.mode == VT_PROCESS && vm.relsig == SIGUSR1);
    vm.mode = VT_AUTO;
    ioctl(vt, VT_SETMODE, &vm);
    close(vt);
}

static void test_pty(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0);
    char *name = ptsname(m) ? strdup(ptsname(m)) : NULL;
    CHECK(name && strncmp(name, "/dev/pts/", 9) == 0);
    int s = name ? open(name, O_RDWR | O_NOCTTY) : -1;
    CHECK(s >= 0 && isatty(s));
    char buf[16] = { 0 };
    CHECK(write(s, "hey", 3) == 3 && read(m, buf, sizeof buf) >= 3 && memcmp(buf, "hey", 3) == 0);
    struct winsize ws = { .ws_row = 30, .ws_col = 100 };
    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0);
    memset(&ws, 0, sizeof ws);
    CHECK(ioctl(s, TIOCGWINSZ, &ws) == 0 && ws.ws_col == 100);
    int m2 = posix_openpt(O_RDWR);
    char *n2 = m2 >= 0 && ptsname(m2) ? strdup(ptsname(m2)) : NULL;
    CHECK(n2 && name && strcmp(n2, name) != 0);
    close(m2);
    struct stat st;
    CHECK(n2 && stat(n2, &st) == -1);    // gone with its master
    free(n2);
    close(s);
    CHECK(name && stat(name, &st) == 0);     // the slave is still open
    close(m);
    free(name);
}

static int slurp(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, size - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    return n;
}

static void test_sysfs(void)
{
    char buf[512];
    CHECK(slurp("/sys/class/input/event0/dev", buf, sizeof buf) > 0 && strcmp(buf, "13:64\n") == 0);
    CHECK(slurp("/sys/class/input/event0/device/name", buf, sizeof buf) > 0 && strstr(buf, "keyboard"));
    CHECK(slurp("/sys/dev/char/13:65/device/capabilities/rel", buf, sizeof buf) > 0 && strcmp(buf, "103\n") == 0);
    CHECK(slurp("/sys/class/input/input0/capabilities/ev", buf, sizeof buf) > 0 && strcmp(buf, "100003\n") == 0);
    char real[256];
    CHECK(realpath("/sys/class/input/event1", real) &&
          strcmp(real, "/sys/devices/platform/i8042/serio1/input/input1/event1") == 0);
    struct fb_var_screeninfo var;
    int fb = open("/dev/fb0", O_RDONLY);
    if (fb >= 0) {
        ioctl(fb, FBIOGET_VSCREENINFO, &var);
        close(fb);
        unsigned w = 0, h = 0;
        CHECK(slurp("/sys/class/graphics/fb0/virtual_size", buf, sizeof buf) > 0 &&
              sscanf(buf, "%u,%u", &w, &h) == 2 && w == var.xres && h == var.yres);
    }
    DIR *d = opendir("/sys/bus/pci/devices");
    CHECK(d != NULL);
    int devs = 0, good = 0;
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        devs++;
        char p[300];
        unsigned vendor = 0;
        unsigned char cfg[256];
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", e->d_name);
        slurp(p, buf, sizeof buf);
        sscanf(buf, "%x", &vendor);
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/config", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd >= 0 && read(fd, cfg, 256) == 256 && (cfg[0] | cfg[1] << 8) == vendor && vendor)
            good++;
        close(fd);
    }
    if (d)
        closedir(d);
    CHECK(devs > 0 && good == devs);
    CHECK(slurp("/sys/devices/system/cpu/online", buf, sizeof buf) > 0 && strcmp(buf, "0\n") == 0);
    CHECK(open("/sys/class/input/event0/dev", O_WRONLY) == -1);
}

// ltest evdev N: prints the next N events from a device, for poking by hand
static int dump_events(const char *dev, int n)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    struct input_event ie;
    while (n > 0 && read(fd, &ie, sizeof ie) == sizeof ie) {
        printf("type %d code %d value %d\n", ie.type, ie.code, ie.value);
        if (ie.type != EV_SYN)
            n--;
    }
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "evdev") == 0)
        return dump_events(argv[2], atoi(argv[3]));
    printf("ltest: glibc %s, pid %d\n", "static", getpid());
    struct utsname u;
    CHECK(uname(&u) == 0 && strcmp(u.sysname, "Linux") == 0);

    char *big = malloc(10 << 20);
    CHECK(big != NULL);
    memset(big, 7, 10 << 20);
    CHECK(big[(10 << 20) - 1] == 7);
    free(big);

    void *m = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    ((char *)m)[12345] = 1;
    CHECK(munmap(m, 1 << 20) == 0);

    int fd = open("/tmp/ltest.txt", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, "hello linux\n", 12) == 12);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    char buf[32] = { 0 };
    CHECK(read(fd, buf, sizeof buf) == 12 && strcmp(buf, "hello linux\n") == 0);
    close(fd);
    struct stat st;
    CHECK(stat("/tmp/ltest.txt", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 12);
    FILE *f = fopen("/tmp/ltest.txt", "r");
    CHECK(f && fgets(buf, sizeof buf, f) && strcmp(buf, "hello linux\n") == 0);
    if (f)
        fclose(f);
    CHECK(unlink("/tmp/ltest.txt") == 0);
    CHECK(open("/nope", O_RDONLY) < 0 && errno == ENOENT);

    DIR *d = opendir("/bin");
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        n++;
    if (d)
        closedir(d);
    CHECK(n > 10);

    pid_t pid = fork();
    if (pid == 0)
        _exit(42);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 42);

    pid = fork();
    if (pid == 0) {
        char *args[] = { "echo", "exec from a linux program works", NULL };
        execv("/bin/echo", args);
        _exit(99);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0);

    double x = 1.5;
    for (int i = 0; i < 10; i++)
        x *= 1.5;
    char num[64];
    snprintf(num, sizeof num, "%.3f", x);
    CHECK(strcmp(num, "86.498") == 0);

    struct timespec ts;
    CHECK(clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec > 1600000000);
    char cwd[256];
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK(argc >= 1 && argv[0] != NULL);

    test_signals();
    test_threads();
    test_unix();
    test_events();
    test_devices();
    test_sysfs();
    test_pty();

    printf("ltest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
