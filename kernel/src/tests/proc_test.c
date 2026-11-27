#include "tests/tests.h"
#include "kprintf.h"
#include "mm/pmm.h"
#include "proc/proc.h"
#include "sched/sched.h"

bool test_proc(void)
{
    int passed = 0, failed = 0;
#define CHECK(cond) do { if (cond) passed++; else { failed++; \
        pr_err("proc test failed: %s (line %d)\n", #cond, __LINE__); } } while (0)

    const struct program *spin = program_find("spin");
    CHECK(spin != NULL);
    CHECK(program_find("nope") == NULL);
    if (!spin)
        return false;

    // warm up once so lazily grown kernel structures don't count as a leak
    char *args[] = { "spin", "quiet", "0" };
    struct thread *t = proc_spawn(spin, 3, args, false, NULL);
    CHECK(t && thread_join(t) == 0);

    size_t before = pmm_free_count();
    int ok = 0;
    for (int i = 0; i < 20; i++) {
        t = proc_spawn(spin, 3, args, false, NULL);
        if (t && thread_join(t) == 0)
            ok++;
    }
    CHECK(ok == 20);
    CHECK(pmm_free_count() == before);

    // garbage isn't an executable
    struct program junk = { "junk", (const uint8_t *)"not an elf at all", 17 };
    const char *err = NULL;
    CHECK(proc_spawn(&junk, 1, args, false, &err) == NULL && err != NULL);
    CHECK(pmm_free_count() == before);

    if (failed)
        pr_err("proc: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("proc: all %d tests passed\n", passed);
    return failed == 0;
#undef CHECK
}
