#include "tests/tests.h"
#include "kprintf.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "sched/sync.h"

static int passed, failed;

#define CHECK(cond) do {                                          \
        if (cond) passed++;                                       \
        else { failed++; pr_err("sched test failed: %s (line %d)\n", #cond, __LINE__); } \
    } while (0)

// mutex: several threads bump a shared counter, yielding inside the lock
static struct mutex counter_lock = MUTEX_INIT;
static int counter;

static void counter_thread(void *arg)
{
    for (int i = 0; i < 500; i++) {
        mutex_lock(&counter_lock);
        int v = counter;
        if (i % 50 == 0)
            thread_yield();    // without the mutex this loses updates
        counter = v + 1;
        mutex_unlock(&counter_lock);
    }
    thread_exit((int)(uint64_t)arg);
}

// sleep: wake order follows the sleep length, not the start order
static struct mutex order_lock = MUTEX_INIT;
static int order[3], order_n;

static void sleeper_thread(void *arg)
{
    int ms = (int)(uint64_t)arg;
    thread_sleep_ms((uint64_t)ms);
    mutex_lock(&order_lock);
    order[order_n++] = ms;
    mutex_unlock(&order_lock);
}

// preemption: this never yields, the timer has to take the cpu away
static volatile bool stop_spinning;
static volatile uint64_t spins;

static void spinner_thread(void *arg)
{
    (void)arg;
    while (!stop_spinning)
        spins++;
}

// semaphore: producer/consumer handoff
static struct semaphore items = SEMAPHORE_INIT(0);
static int consumed;

static void consumer_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20; i++) {
        sem_down(&items);
        consumed++;
    }
}

static void exit_thread(void *arg)
{
    (void)arg;
}

bool test_sched(void)
{
    passed = failed = 0;

    counter = 0;
    struct thread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = thread_create("counter", counter_thread, (void *)(uint64_t)(i + 10), true);
    int codes = 0;
    for (int i = 0; i < 4; i++)
        codes += thread_join(t[i]);
    CHECK(counter == 2000);
    CHECK(codes == 10 + 11 + 12 + 13);

    order_n = 0;
    struct thread *s[3];
    s[0] = thread_create("sleep30", sleeper_thread, (void *)30UL, true);
    s[1] = thread_create("sleep10", sleeper_thread, (void *)10UL, true);
    s[2] = thread_create("sleep20", sleeper_thread, (void *)20UL, true);
    for (int i = 0; i < 3; i++)
        thread_join(s[i]);
    CHECK(order_n == 3 && order[0] == 10 && order[1] == 20 && order[2] == 30);

    stop_spinning = false;
    spins = 0;
    struct thread *sp = thread_create("spinner", spinner_thread, NULL, true);
    thread_sleep_ms(30);
    stop_spinning = true;    // only reachable if we got the cpu back
    thread_join(sp);
    CHECK(spins > 0);

    consumed = 0;
    struct thread *c = thread_create("consumer", consumer_thread, NULL, true);
    for (int i = 0; i < 20; i++) {
        sem_up(&items);
        if (i % 5 == 0)
            thread_sleep_ms(1);
    }
    thread_join(c);
    CHECK(consumed == 20);

    // detached threads get their stacks back through the idle thread
    size_t before = pmm_free_count();
    for (int i = 0; i < 50; i++)
        CHECK(thread_create("short", exit_thread, NULL, false) != NULL);
    thread_sleep_ms(20);
    thread_create("reap", exit_thread, NULL, false);    // creating also reaps
    thread_sleep_ms(5);
    size_t after = pmm_free_count();
    CHECK(after + 32 >= before);    // one stack is 16 pages, allow two in flight

    if (failed)
        pr_err("sched: %d/%d tests passed\n", passed, passed + failed);
    else
        pr_ok("sched: all %d tests passed\n", passed);
    return failed == 0;
}
