// tiny built-in shell for poking at the kernel
#include "shell.h"
#include "arch/x86_64/cpu.h"
#include "cmdline.h"
#include "console/console.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "drivers/ps2.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include "tests/tests.h"

#define LINE_MAX    128
#define MAX_ARGS    16
#define HISTORY     16
#define PROMPT      "\e[1;32mzk\e[0m> "

static char history[HISTORY][LINE_MAX];
static int history_count;

struct command {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
};

static void cmd_help(int argc, char **argv);

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_clear();
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint64_t ms = timer_uptime_ms();
    kprintf("up %lu.%03lus (%lu ticks at %d Hz)\n",
            ms / 1000, ms % 1000, timer_ticks(), TIMER_HZ);
}

static void cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t total = pmm_total_pages(), free = pmm_free_count();
    kprintf("physical: %zu MiB total, %zu MiB used, %zu MiB free\n",
            total * PAGE_SIZE >> 20, (total - free) * PAGE_SIZE >> 20,
            free * PAGE_SIZE >> 20);

    struct heap_stats s;
    heap_get_stats(&s);
    kprintf("heap:     %zu KiB mapped, %zu bytes used, %zu blocks (%zu free)\n",
            s.mapped >> 10, s.used, s.blocks, s.free_blocks);
}

static void cmd_sleep(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: sleep <ms>\n");
        return;
    }
    uint64_t ms = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
        ms = ms * 10 + (uint64_t)(*p - '0');
    timer_sleep_ms(ms);
}

static void cmd_cmdline(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s\n", cmdline_raw());
}

static void cmd_crash(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: crash <de|ud|bp|gp|pf|df|stack|wp|nx|dfree|panic>\n");
        return;
    }
    trigger_fault(argv[1]);
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("rebooting...\n");
    ps2_reboot();

    // controller didn't do it, triple fault instead
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(empty));
    for (;;)
        __asm__ volatile ("hlt");
}

static void cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct thread_info info[64];
    int n = sched_snapshot(info, 64);
    kprintf("  %-4s %-16s %-9s %-6s %s\n", "ID", "NAME", "STATE", "MODE", "CPU");
    for (int i = n - 1; i >= 0; i--) {
        kprintf("  %-4lu %-16s %-9s %-6s %lu.%03lus\n", info[i].id, info[i].name,
                thread_state_name(info[i].state), info[i].user ? "user" : "kernel",
                info[i].cpu_ticks / TIMER_HZ, info[i].cpu_ticks % TIMER_HZ);
    }
}

static void demo_thread(void *arg)
{
    int id = (int)(uint64_t)arg;
    for (int i = 1; i <= 3; i++) {
        kprintf("  [demo %d] tick %d\n", id, i);
        thread_sleep_ms((uint64_t)(100 * id));
    }
}

static void cmd_demo(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct thread *t[3];
    for (int i = 0; i < 3; i++)
        t[i] = thread_create("demo", demo_thread, (void *)(uint64_t)(i + 1), true);
    for (int i = 0; i < 3; i++)
        thread_join(t[i]);
    kprintf("  all demo threads finished\n");
}

static void cmd_progs(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (const struct program *p = program_list(); p->name; p++)
        kprintf("  %-8s %zu bytes\n", p->name, p->size);
}

// run <prog> [args...] [&]
static void cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: run <program> [args...] [&], see progs\n");
        return;
    }
    bool background = strcmp(argv[argc - 1], "&") == 0;
    if (background)
        argc--;

    const struct program *p = program_find(argv[1]);
    if (!p) {
        kprintf("%s: no such program\n", argv[1]);
        return;
    }
    // a background process can finish and be freed right away, so grab
    // its id before it gets a chance to run
    const char *err;
    uint64_t flags = irq_save();
    struct thread *t = proc_spawn(p, argc - 1, argv + 1, background, &err);
    uint64_t id = t ? t->id : 0;
    irq_restore(flags);

    if (!t) {
        kprintf("%s: %s\n", argv[1], err);
        return;
    }
    if (background) {
        kprintf("[%lu] %s\n", id, p->name);
        return;
    }
    int code = thread_join(t);
    if (code)
        kprintf("%s exited with %d\n", p->name, code);
}

static const struct command commands[] = {
    { "help",    "list commands",               cmd_help },
    { "clear",   "clear the screen",            cmd_clear },
    { "echo",    "print the arguments",         cmd_echo },
    { "uptime",  "time since boot",             cmd_uptime },
    { "mem",     "memory usage",                cmd_mem },
    { "sleep",   "sleep <ms>",                  cmd_sleep },
    { "ps",      "list threads",                cmd_ps },
    { "demo",    "run three threads at once",   cmd_demo },
    { "progs",   "list user programs",          cmd_progs },
    { "run",     "run <prog> [args] [&]",       cmd_run },
    { "cmdline", "kernel command line",         cmd_cmdline },
    { "crash",   "crash <type>, test a fault",  cmd_crash },
    { "reboot",  "reboot the machine",          cmd_reboot },
    { NULL, NULL, NULL },
};

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (const struct command *c = commands; c->name; c++)
        kprintf("  %-8s %s\n", c->name, c->help);
}

static void run_line(char *line)
{
    char *argv[MAX_ARGS];
    int argc = 0;

    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ')
            *p++ = '\0';
        if (!*p)
            break;
        argv[argc++] = p;
        while (*p && *p != ' ')
            p++;
    }
    if (argc == 0)
        return;

    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(argv[0], c->name) == 0) {
            c->fn(argc, argv);
            return;
        }
    }
    kprintf("%s: unknown command, try help\n", argv[0]);
}

static void erase_line(size_t len)
{
    while (len--)
        kprintf("\b \b");
}

// returns false on ctrl+c
static bool read_line(char *buf)
{
    size_t len = 0;
    int hist_pos = history_count;
    buf[0] = '\0';

    for (;;) {
        console_cursor_show();
        int c = keyboard_getc();
        console_cursor_hide();

        if (c == '\n') {
            kprintf("\n");
            return true;
        }
        if (c == 3) {    // ctrl+c
            kprintf("^C\n");
            return false;
        }
        if (c == 12) {   // ctrl+l
            console_clear();
            kprintf(PROMPT "%s", buf);
            continue;
        }
        if (c == '\b') {
            if (len > 0) {
                buf[--len] = '\0';
                kprintf("\b \b");
            }
            continue;
        }
        if (c == KEY_UP || c == KEY_DOWN) {
            int oldest = history_count > HISTORY ? history_count - HISTORY : 0;
            int next = hist_pos + (c == KEY_UP ? -1 : 1);
            if (next < oldest || next > history_count)
                continue;
            hist_pos = next;
            erase_line(len);
            const char *src = hist_pos == history_count ? ""
                            : history[hist_pos % HISTORY];
            strlcpy(buf, src, LINE_MAX);
            len = strlen(buf);
            kprintf("%s", buf);
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len + 1 < LINE_MAX) {
            buf[len++] = (char)c;
            buf[len] = '\0';
            kprintf("%c", c);
        }
    }
}

void shell_run(void)
{
    char line[LINE_MAX];
    kprintf("\ntype \e[1mhelp\e[0m for commands\n");

    for (;;) {
        kprintf(PROMPT);
        if (!read_line(line) || !line[0])
            continue;

        // ring of the last HISTORY lines, skipping repeats
        if (history_count == 0 || strcmp(history[(history_count - 1) % HISTORY], line) != 0)
            strlcpy(history[history_count++ % HISTORY], line, LINE_MAX);
        run_line(line);
    }
}
