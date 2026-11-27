// tiny built-in shell for poking at the kernel
#include "shell.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/power.h"
#include "cmdline.h"
#include "console/console.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "lib/printf.h"
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
    machine_reboot();
}

static void cmd_poweroff(int argc, char **argv)
{
    (void)argc; (void)argv;
    machine_poweroff();
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

// relative names are looked up in /bin
static int resolve(const char *name, char *out)
{
    if (!strchr(name, '/')) {
        char tmp[ZK_PATH_MAX];
        snprintf(tmp, sizeof tmp, "/bin/%s", name);
        return vfs_normalize("/", tmp, out);
    }
    return vfs_normalize("/", name, out);
}

static void cmd_ls(int argc, char **argv)
{
    char path[ZK_PATH_MAX];
    int err = vfs_normalize("/", argc > 1 ? argv[1] : "/", path);
    struct file *f = NULL;
    if (!err)
        err = vfs_open(path, O_RDONLY, &f);
    if (err) {
        kprintf("ls: %s\n", proc_strerror(err));
        return;
    }
    struct zk_dirent d;
    while ((err = file_readdir(f, &d)) > 0) {
        char child[ZK_PATH_MAX];
        struct zk_stat st = { 0 };
        snprintf(child, sizeof child, "%s/%s", strcmp(path, "/") ? path : "", d.name);
        vfs_stat(child, &st);
        kprintf("  %-20s %s%8lu\n", d.name, d.type == ZK_TYPE_DIR ? "dir " :
                d.type == ZK_TYPE_DEV ? "dev " : "    ", st.size);
    }
    if (err < 0)
        kprintf("ls: %s\n", proc_strerror(err));
    file_put(f);
}

static void cmd_cat(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char path[ZK_PATH_MAX];
        void *data;
        size_t size;
        int err = vfs_normalize("/", argv[i], path);
        if (!err)
            err = vfs_read_all(path, &data, &size, 1 << 20);
        if (err) {
            kprintf("cat: %s: %s\n", argv[i], proc_strerror(err));
            continue;
        }
        kprintf("%.*s", (int)size, (const char *)data);
        kfree(data);
    }
}

static void cmd_progs(int argc, char **argv)
{
    (void)argc; (void)argv;
    char *args[] = { "ls", "/bin" };
    cmd_ls(2, args);
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

    char path[ZK_PATH_MAX];
    int err = resolve(argv[1], path);
    if (err) {
        kprintf("%s: %s\n", argv[1], proc_strerror(err));
        return;
    }

    struct spawn_opts o;
    proc_console_opts(&o);
    o.detached = background;
    // a background process can finish and be freed right away, so grab
    // its id before it gets a chance to run
    uint64_t flags = irq_save();
    char *args[MAX_ARGS + 1];
    for (int i = 1; i < argc; i++)
        args[i - 1] = argv[i];
    args[argc - 1] = NULL;
    struct thread *t = proc_spawn(path, args, proc_default_env, &o, &err);
    uint64_t id = t ? t->id : 0;
    irq_restore(flags);
    proc_put_console(&o);

    if (!t) {
        kprintf("%s: %s\n", argv[1], proc_strerror(err));
        return;
    }
    if (background) {
        kprintf("[%lu] %s\n", id, argv[1]);
        return;
    }
    console_tty.fg = id;
    tty_console_route(true);
    int code = thread_join(t);
    tty_console_route(false);
    console_tty.fg = 0;
    if (code)
        kprintf("%s exited with %d\n", argv[1], code);
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
    { "ls",      "ls [dir]",                    cmd_ls },
    { "cat",     "cat <file...>",               cmd_cat },
    { "progs",   "list user programs",          cmd_progs },
    { "run",     "run <prog> [args] [&]",       cmd_run },
    { "cmdline", "kernel command line",         cmd_cmdline },
    { "crash",   "crash <type>, test a fault",  cmd_crash },
    { "reboot",  "reboot the machine",          cmd_reboot },
    { "poweroff", "turn the machine off",       cmd_poweroff },
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
