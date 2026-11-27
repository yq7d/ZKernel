#include "tests/tests.h"
#include "fs/vfs.h"
#include "kprintf.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "proc/proc.h"
#include "sched/sched.h"

bool test_proc(void)
{
    int passed = 0, failed = 0;
#define CHECK(cond) do { if (cond) passed++; else { failed++; \
        pr_err("proc test failed: %s (line %d)\n", #cond, __LINE__); } } while (0)

    struct zk_stat st;
    if (vfs_stat("/bin/spin", &st) != 0) {
        pr_warn("proc: /bin/spin missing, skipping tests\n");
        return true;
    }

    struct spawn_opts o = { 0 };
    char *args[] = { "spin", "quiet", "0", NULL };

    // warm up once so lazily grown kernel structures don't count as a leak
    struct thread *t = proc_spawn("/bin/spin", args, NULL, &o, NULL);
    CHECK(t && thread_join(t) == 0);

    size_t before = pmm_free_count();
    int ok = 0;
    for (int i = 0; i < 20; i++) {
        t = proc_spawn("/bin/spin", args, NULL, &o, NULL);
        if (t && thread_join(t) == 0)
            ok++;
    }
    CHECK(ok == 20);
    CHECK(pmm_free_count() == before);

    int err = 0;
    CHECK(proc_spawn("/nope", args, NULL, &o, &err) == NULL && err == ZK_ENOENT);
    CHECK(proc_spawn("/bin", args, NULL, &o, &err) == NULL && err == ZK_EISDIR);

    // garbage isn't an executable
    struct file *f;
    if (vfs_open("/tmp/junk", O_WRONLY | O_CREAT, &f) == 0) {
        file_write(f, "not an elf at all", 17);
        file_put(f);
    }
    CHECK(proc_spawn("/tmp/junk", args, NULL, &o, &err) == NULL && err == ZK_EACCES);
    vfs_chmod("/tmp/junk", 0755);
    CHECK(proc_spawn("/tmp/junk", args, NULL, &o, &err) == NULL && err == ZK_ENOEXEC);
    vfs_unlink("/tmp/junk");
    CHECK(pmm_free_count() == before);

    // console fds are refcounted and handed back
    struct spawn_opts c;
    proc_console_opts(&c);
    struct file *con = c.std[1];
    t = proc_spawn("/bin/spin", args, NULL, &c, NULL);
    CHECK(t && con->refs == 2);
    CHECK(t && thread_join(t) == 0);
    CHECK(con->refs == 1);
    proc_put_console(&c);

    if (failed)
        pr_err("proc: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("proc: all %d tests passed\n", passed);
    return failed == 0;
#undef CHECK
}
