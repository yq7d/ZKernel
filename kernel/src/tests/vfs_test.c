#include "tests/tests.h"
#include "fs/vfs.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"

static int passed, failed;

#define CHECK(cond) do {                                          \
        if (cond) passed++;                                       \
        else { failed++; pr_err("vfs test failed: %s (line %d)\n", #cond, __LINE__); } \
    } while (0)

static bool norm_is(const char *cwd, const char *path, const char *want)
{
    char out[ZK_PATH_MAX];
    return vfs_normalize(cwd, path, out) == 0 && strcmp(out, want) == 0;
}

static void test_paths(void)
{
    char out[ZK_PATH_MAX];
    CHECK(norm_is("/", "/", "/"));
    CHECK(norm_is("/", "a/b", "/a/b"));
    CHECK(norm_is("/x/y", "../z", "/x/z"));
    CHECK(norm_is("/x", "./a//b/./", "/x/a/b"));
    CHECK(norm_is("/", "../../..", "/"));
    CHECK(norm_is("/a/b", "/c/../d", "/d"));
    CHECK(vfs_normalize("/", "", out) == ZK_ENOENT);

    char longname[ZK_NAME_MAX + 2];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    CHECK(vfs_normalize("/", longname, out) == ZK_ENAMETOOLONG);
}

static void test_files(void)
{
    struct file *f;
    struct zk_stat st;

    CHECK(vfs_mkdir("/tmp/vt") == 0);
    CHECK(vfs_mkdir("/tmp/vt") == ZK_EEXIST);
    CHECK(vfs_open("/tmp/vt/a", O_RDONLY, &f) == ZK_ENOENT);
    CHECK(vfs_open("/tmp/vt", O_WRONLY, &f) == ZK_EISDIR);

    CHECK(vfs_open("/tmp/vt/a", O_RDWR | O_CREAT, &f) == 0);
    CHECK(file_write(f, "hello world", 11) == 11);
    CHECK(file_seek(f, 6, SEEK_SET) == 6);
    char buf[32] = { 0 };
    CHECK(file_read(f, buf, sizeof buf) == 5 && memcmp(buf, "world", 5) == 0);
    CHECK(file_read(f, buf, sizeof buf) == 0);
    // a hole reads back as zeros
    CHECK(file_seek(f, 20, SEEK_SET) == 20);
    CHECK(file_write(f, "!", 1) == 1);
    CHECK(file_seek(f, 11, SEEK_SET) == 11);
    CHECK(file_read(f, buf, 10) == 10 && buf[0] == 0 && buf[8] == 0 && buf[9] == '!');
    file_put(f);

    CHECK(vfs_stat("/tmp/vt/a", &st) == 0 && st.type == ZK_TYPE_FILE && st.size == 21);
    CHECK(vfs_open("/tmp/vt/a", O_WRONLY | O_TRUNC, &f) == 0);
    file_put(f);
    CHECK(vfs_stat("/tmp/vt/a", &st) == 0 && st.size == 0);

    CHECK(vfs_open("/tmp/vt/a", O_WRONLY | O_APPEND, &f) == 0);
    file_write(f, "ab", 2);
    file_seek(f, 0, SEEK_SET);
    file_write(f, "cd", 2);
    file_put(f);
    void *data;
    size_t size;
    CHECK(vfs_read_all("/tmp/vt/a", &data, &size, 100) == 0 && size == 4 &&
          memcmp(data, "abcd", 4) == 0);
    if (size == 4)
        kfree(data);

    // file used as a directory
    CHECK(vfs_mkdir("/tmp/vt/a/b") == ZK_ENOTDIR);
    CHECK(vfs_open("/tmp/vt/a/b", O_RDONLY, &f) == ZK_ENOTDIR);

    CHECK(vfs_mkdir("/tmp/vt/sub") == 0);
    CHECK(vfs_open("/tmp/vt", O_RDONLY, &f) == 0);
    struct zk_dirent d;
    CHECK(file_readdir(f, &d) == 1 && strcmp(d.name, "a") == 0 && d.type == ZK_TYPE_FILE);
    CHECK(file_readdir(f, &d) == 1 && strcmp(d.name, "sub") == 0 && d.type == ZK_TYPE_DIR);
    CHECK(file_readdir(f, &d) == 0);
    file_put(f);

    CHECK(vfs_unlink("/tmp/vt") == ZK_ENOTEMPTY);
    CHECK(vfs_unlink("/tmp/vt/sub") == 0);

    // an open file outlives its name
    CHECK(vfs_open("/tmp/vt/a", O_RDONLY, &f) == 0);
    CHECK(vfs_unlink("/tmp/vt/a") == 0);
    CHECK(vfs_stat("/tmp/vt/a", &st) == ZK_ENOENT);
    CHECK(file_read(f, buf, sizeof buf) == 4);
    file_put(f);

    CHECK(vfs_unlink("/tmp/vt") == 0);
    CHECK(vfs_unlink("/") == ZK_EPERM);
}

static void test_pipe(void)
{
    struct file *rd, *wr;
    CHECK(pipe_create(&rd, &wr) == 0);
    CHECK(file_write(wr, "abc", 3) == 3);
    char buf[8];
    CHECK(file_read(rd, buf, sizeof buf) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(file_write(rd, "x", 1) == ZK_EBADF);
    file_put(wr);
    CHECK(file_read(rd, buf, sizeof buf) == 0);    // eof
    file_put(rd);

    CHECK(pipe_create(&rd, &wr) == 0);
    file_put(rd);
    CHECK(file_write(wr, "x", 1) == ZK_EPIPE);
    file_put(wr);
}

bool test_vfs(void)
{
    passed = failed = 0;
    size_t before = ramfs_bytes_used();
    test_paths();
    test_files();
    test_pipe();
    CHECK(ramfs_bytes_used() == before);

    if (failed)
        pr_err("vfs: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("vfs: all %d tests passed\n", passed);
    return failed == 0;
}
