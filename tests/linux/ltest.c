// built with the host's gcc -static, runs under the linux layer
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
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

    printf("ltest: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
