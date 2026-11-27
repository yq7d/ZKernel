// built with the host's gcc -static, runs under the linux layer
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

int main(int argc, char **argv)
{
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

    printf("ltest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
