// user space self tests: allocator, fpu, fork/cow, exec, env, files
#include "zk.h"

static int passed, failed;

#define CHECK(cond) do {                                                    \
        if (cond) passed++;                                                 \
        else { failed++; printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } \
    } while (0)

static void test_malloc(void)
{
    // fill, check, free in a scrambled order, repeat. leaks show up as growth
    enum { N = 200 };
    static uint8_t *ptrs[N];
    static size_t sizes[N];
    uint64_t seed = 12345;
    uint8_t *first_round_max = NULL;

    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < N; i++) {
            seed = seed * 6364136223846793005UL + 1442695040888963407UL;
            sizes[i] = (seed >> 40) % 3000 + 1;
            ptrs[i] = malloc(sizes[i]);
            if (!ptrs[i])
                break;
            memset(ptrs[i], i & 0xff, sizes[i]);
            if (round == 0 && ptrs[i] > first_round_max)
                first_round_max = ptrs[i];
        }
        bool ok = true;
        for (int i = 0; i < N; i++) {
            CHECK(((uintptr_t)ptrs[i] & 15) == 0);
            for (size_t k = 0; k < sizes[i]; k += 97)
                if (ptrs[i][k] != (i & 0xff))
                    ok = false;
        }
        CHECK(ok);
        for (int i = 0; i < N; i += 2)
            free(ptrs[i]);
        for (int i = 1; i < N; i += 2)
            free(ptrs[i]);
    }

    // everything got merged back, so one big block fits where the small ones were
    uint8_t *big = malloc(200 * 1024);
    CHECK(big != NULL);
    free(big);

    char *s = malloc(8);
    strlcpy(s, "abcdefg", 8);
    s = realloc(s, 5000);
    CHECK(s && strcmp(s, "abcdefg") == 0);
    s = realloc(s, 16);
    CHECK(s && strcmp(s, "abcdefg") == 0);
    free(s);

    int *z = calloc(100, sizeof(int));
    bool zero = z != NULL;
    for (int i = 0; z && i < 100; i++)
        zero &= z[i] == 0;
    CHECK(zero);
    free(z);
    CHECK(calloc((size_t)1 << 62, 16) == NULL);
    free(NULL);
}

static double work(int seed)
{
    volatile double x = seed;
    for (int i = 0; i < 200000; i++) {
        x = x * 1.0000001 + 0.5;
        if (i % 50000 == 0)
            yield();
    }
    return x;
}

static void test_fpu(void)
{
    double expect = work(3);
    // the child does different float math while the parent keeps going
    long pid = fork();
    if (pid == 0) {
        double v = work(7);
        exit(v > 100000 ? 0 : 1);
    }
    double again = work(3);
    CHECK(again == expect);
    int status = -1;
    CHECK(wait(pid, &status, 0) == pid && status == 0);

    float f = 1.5f;
    for (int i = 0; i < 10; i++)
        f *= 2.0f;
    CHECK(f == 1536.0f);
}

static int global_counter = 5;

static void test_fork(void)
{
    char *heap = malloc(64);
    strlcpy(heap, "parent", 64);
    int local = 10;

    long pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // writes here must not show up in the parent
        global_counter = 99;
        local = 99;
        strlcpy(heap, "child", 64);
        exit(global_counter == 99 && local == 99 && strcmp(heap, "child") == 0 ? 42 : 1);
    }
    int status = -1;
    CHECK(wait(pid, &status, 0) == pid);
    CHECK(status == 42);
    CHECK(global_counter == 5 && local == 10 && strcmp(heap, "parent") == 0);
    free(heap);

    // the child shares the fd table at the time of the fork, and a pipe works across it
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        write(fds[1], "from child", 10);
        exit(0);
    }
    close(fds[1]);
    char buf[32] = { 0 };
    long n = read(fds[0], buf, sizeof buf);
    CHECK(n == 10 && memcmp(buf, "from child", 10) == 0);
    CHECK(read(fds[0], buf, sizeof buf) == 0);
    close(fds[0]);
    CHECK(wait(pid, &status, 0) == pid && status == 0);

    // lots of forks, every one reaped
    for (int i = 0; i < 30; i++) {
        pid = fork();
        if (pid == 0)
            exit(i);
        CHECK(wait(pid, &status, 0) == pid && status == i);
    }
    CHECK(wait(-1, NULL, WNOHANG) == ZK_ECHILD);
}

static void test_exec(void)
{
    long pid = fork();
    if (pid == 0) {
        char *argv[] = { "sh", "-c", "exit 7", NULL };
        execv("/bin/sh", argv);
        exit(100);
    }
    int status = -1;
    CHECK(wait(pid, &status, 0) == pid && status == 7);

    pid = fork();
    if (pid == 0) {
        char *argv[] = { "nope", NULL };
        int err = execv("/bin/does-not-exist", argv);
        exit(err == ZK_ENOENT ? 3 : 4);
    }
    CHECK(wait(pid, &status, 0) == pid && status == 3);

    // env reaches the new program
    pid = fork();
    if (pid == 0) {
        char *argv[] = { "sh", "-c", "exit $ZK_TEST", NULL };
        char *envp[] = { "ZK_TEST=9", "PATH=/bin", NULL };
        execve("/bin/sh", argv, envp);
        exit(100);
    }
    CHECK(wait(pid, &status, 0) == pid && status == 9);
}

static void test_env(void)
{
    CHECK(getenv("PATH") != NULL);
    CHECK(setenv("ZK_A", "1", 1) == 0 && strcmp(getenv("ZK_A"), "1") == 0);
    CHECK(setenv("ZK_A", "2", 0) == 0 && strcmp(getenv("ZK_A"), "1") == 0);
    CHECK(setenv("ZK_A", "3", 1) == 0 && strcmp(getenv("ZK_A"), "3") == 0);
    CHECK(unsetenv("ZK_A") == 0 && getenv("ZK_A") == NULL);
}

static void test_files(void)
{
    int fd = open("/tmp/utest", O_RDWR | O_CREAT | O_TRUNC);
    CHECK(fd >= 0);
    CHECK(write(fd, "0123456789", 10) == 10);
    CHECK(ftruncate(fd, 4) == 0);
    struct zk_stat st;
    CHECK(fstat(fd, &st) == 0 && st.size == 4);
    close(fd);
    CHECK(rename("/tmp/utest", "/tmp/utest2") == 0);
    CHECK(stat("/tmp/utest", &st) == ZK_ENOENT);
    CHECK(stat("/tmp/utest2", &st) == 0 && st.size == 4);
    CHECK(unlink("/tmp/utest2") == 0);
    CHECK(time() > 1600000000);
}

int main(void)
{
    printf("utest: running\n");
    test_malloc();
    test_fpu();
    test_fork();
    test_exec();
    test_env();
    test_files();
    printf("utest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
