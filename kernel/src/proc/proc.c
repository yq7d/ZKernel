#include "proc/proc.h"
#include "proc/elf.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/interrupts.h"
#include "drivers/pit.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"

#define USER_STACK_TOP   0x00007ffffff00000UL
#define USER_STACK_PAGES 16
#define USER_HEAP_BASE   0x0000100000000000UL
#define USER_MIN_ADDR    0x10000UL    // keep null pointer derefs faulting

extern void enter_user(uint64_t entry, uint64_t rsp, uint64_t argc, uint64_t argv,
                       uint64_t envp) __attribute__((noreturn));
extern void return_to_user(struct interrupt_frame *f) __attribute__((noreturn));

struct user_start {
    uint64_t entry, rsp, argc, argv, envp;
};

char *proc_default_env[] = { "PATH=/bin", "HOME=/", "SHELL=/bin/sh", "TERM=zkernel", NULL };

// every live or unreaped process, interrupts off to touch it
static struct proc *procs;

// copy into user memory through the hhdm, the space isn't active yet
static bool copy_to_user(struct address_space *as, uint64_t dst, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys;
        if (!vmm_translate_user(as, dst, &phys))
            return false;
        size_t chunk = PAGE_SIZE - (dst % PAGE_SIZE);
        if (chunk > len)
            chunk = len;
        memcpy(phys_to_virt(phys), s, chunk);
        dst += chunk;
        s += chunk;
        len -= chunk;
    }
    return true;
}

static bool map_zeroed(struct address_space *as, uint64_t start, uint64_t end, uint64_t flags)
{
    for (uint64_t v = ALIGN_DOWN(start, PAGE_SIZE); v < end; v += PAGE_SIZE) {
        if (vmm_translate_user(as, v, NULL))
            continue;    // segments can share a page
        uint64_t p = pmm_alloc_zeroed();
        if (!p)
            return false;
        if (!vmm_map_user(as, v, p, flags)) {
            pmm_free(p);
            return false;
        }
    }
    return true;
}

static int load_elf(struct address_space *as, const uint8_t *data, size_t size, uint64_t *entry)
{
    const struct elf64_ehdr *eh = (const void *)data;
    if (size < sizeof(*eh) || eh->magic != ELF_MAGIC || eh->class != ELFCLASS64 ||
        eh->data != ELFDATA2LSB || eh->type != ET_EXEC || eh->machine != EM_X86_64 ||
        eh->phentsize != sizeof(struct elf64_phdr) ||
        eh->phoff > size || eh->phnum > (size - eh->phoff) / sizeof(struct elf64_phdr))
        return ZK_ENOEXEC;

    const struct elf64_phdr *ph = (const void *)(data + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;

        uint64_t end = p->vaddr + p->memsz;
        if (p->filesz > p->memsz || end < p->vaddr || p->vaddr < USER_MIN_ADDR ||
            end > USER_HEAP_BASE || p->offset > size || p->filesz > size - p->offset)
            return ZK_ENOEXEC;

        uint64_t flags = 0;
        if (p->flags & PF_W)
            flags |= PTE_WRITE;
        if (!(p->flags & PF_X))
            flags |= PTE_NX;

        if (!map_zeroed(as, p->vaddr, end, flags) ||
            !copy_to_user(as, p->vaddr, data + p->offset, p->filesz))
            return ZK_ENOMEM;
    }

    *entry = eh->entry;
    if (*entry < USER_MIN_ADDR || *entry >= USER_HEAP_BASE)
        return ZK_ENOEXEC;
    return 0;
}

static int push_strings(struct address_space *as, uint64_t *sp, int n, char **strs,
                        uint64_t *ptrs, size_t *total)
{
    for (int i = n - 1; i >= 0; i--) {
        size_t len = strlen(strs[i]) + 1;
        *total += len;
        if (*total > PROC_MAX_ARG_BYTES)
            return ZK_E2BIG;
        *sp -= len;
        if (!copy_to_user(as, *sp, strs[i], len))
            return ZK_EFAULT;
        ptrs[i] = *sp;
    }
    ptrs[n] = 0;
    return 0;
}

static int count_strv(char **v, int max)
{
    int n = 0;
    while (v && v[n]) {
        if (++n > max)
            return -1;
    }
    return n;
}

// strings at the top of the stack, then argv[] and envp[] below them
static int setup_stack(struct address_space *as, char **argv, char **envp,
                       struct user_start *s)
{
    uint64_t bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!map_zeroed(as, bottom, USER_STACK_TOP, PTE_WRITE | PTE_NX))
        return ZK_ENOMEM;
    int argc = count_strv(argv, PROC_MAX_ARGS);
    int envc = count_strv(envp, PROC_MAX_ENV);
    if (argc < 0 || envc < 0)
        return ZK_E2BIG;

    uint64_t sp = USER_STACK_TOP;
    uint64_t aptrs[PROC_MAX_ARGS + 1], eptrs[PROC_MAX_ENV + 1];
    size_t total = 0;
    int err = push_strings(as, &sp, envc, envp, eptrs, &total);
    if (!err)
        err = push_strings(as, &sp, argc, argv, aptrs, &total);
    if (err)
        return err;

    sp = ALIGN_DOWN(sp, 16);
    sp -= (uint64_t)(argc + 1 + envc + 1) * sizeof(uint64_t);
    sp = ALIGN_DOWN(sp, 16);
    uint64_t uargv = sp, uenvp = sp + (uint64_t)(argc + 1) * sizeof(uint64_t);
    if (!copy_to_user(as, uargv, aptrs, (size_t)(argc + 1) * sizeof(uint64_t)) ||
        !copy_to_user(as, uenvp, eptrs, (size_t)(envc + 1) * sizeof(uint64_t)))
        return ZK_EFAULT;

    s->argc = (uint64_t)argc;
    s->argv = uargv;
    s->envp = uenvp;
    s->rsp = sp;    // 16 byte aligned, _start calls main from here
    return 0;
}

// new address space with the program loaded and its stack set up
static int build_image(const char *path, char **argv, char **envp,
                       struct address_space **as_out, struct user_start *s)
{
    void *data;
    size_t size;
    int err = vfs_read_all(path, &data, &size, PROC_MAX_EXEC);
    if (err)
        return err;

    struct address_space *as = vmm_create_space();
    if (!as) {
        kfree(data);
        return ZK_ENOMEM;
    }
    as->heap_top = USER_HEAP_BASE;
    err = load_elf(as, data, size, &s->entry);
    kfree(data);
    if (!err)
        err = setup_stack(as, argv, envp, s);
    if (err) {
        vmm_destroy_space(as);
        return err;
    }
    *as_out = as;
    return 0;
}

static void user_thread(void *arg)
{
    struct user_start s = *(struct user_start *)arg;
    kfree(arg);
    enter_user(s.entry, s.rsp, s.argc, s.argv, s.envp);
}

static void close_fds(struct proc *p)
{
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (p->fds[i]) {
            file_put(p->fds[i]);
            p->fds[i] = NULL;
        }
    }
}

static const char *base_name(const char *path)
{
    const char *name = strrchr(path, '/');
    return name ? name + 1 : path;
}

// starts p's thread and makes it visible. fpu NULL gives a clean fpu state
static struct thread *start_proc(struct proc *p, const char *name, thread_fn fn, void *arg,
                                 bool joinable, struct address_space *as, uint64_t pgid,
                                 bool copy_fpu)
{
    // nothing may look at p before its ids are set
    uint64_t flags = irq_save();
    struct thread *t = thread_create_with(name, fn, arg, joinable, as, p);
    if (t) {
        p->thread = t;
        p->pid = t->id;
        p->pgid = pgid ? pgid : p->pid;
        p->list_next = procs;
        procs = p;
        if (copy_fpu)
            fpu_save(t->fpu);    // the caller's user state, the kernel never touches it
    }
    irq_restore(flags);
    return t;
}

struct thread *proc_spawn(const char *path, char **argv, char **envp,
                          const struct spawn_opts *o, int *err)
{
    int dummy;
    if (!err)
        err = &dummy;

    struct user_start *s = kzalloc(sizeof(*s));
    struct proc *p = kzalloc(sizeof(*p));
    struct address_space *as = NULL;
    *err = ZK_ENOMEM;
    if (s && p)
        *err = build_image(path, argv, envp, &as, s);
    if (*err)
        goto fail;

    const char *cwd = o->cwd ? o->cwd : o->parent ? o->parent->cwd : "/";
    strlcpy(p->cwd, cwd, sizeof p->cwd);
    p->parent = o->parent;
    for (int i = 0; i < 3; i++) {
        if (o->std[i]) {
            file_get(o->std[i]);
            p->fds[i] = o->std[i];
        }
    }

    bool joinable = o->parent || !o->detached;
    struct thread *t = start_proc(p, base_name(path), user_thread, s, joinable, as,
                                  o->pgid, false);
    if (t)
        return t;
    close_fds(p);
    *err = ZK_ENOMEM;
fail:
    kfree(s);
    kfree(p);
    if (as)
        vmm_destroy_space(as);
    return NULL;
}

static void fork_child(void *arg)
{
    struct interrupt_frame f = *(struct interrupt_frame *)arg;
    kfree(arg);
    return_to_user(&f);
}

int64_t proc_fork(struct interrupt_frame *frame)
{
    struct proc *me = proc_current();
    struct thread *cur = thread_current();
    struct interrupt_frame *f = kmalloc(sizeof(*f));
    struct proc *p = kzalloc(sizeof(*p));
    struct address_space *as = f && p ? vmm_clone_space(cur->as) : NULL;
    if (!as) {
        kfree(f);
        kfree(p);
        return ZK_ENOMEM;
    }
    *f = *frame;
    f->rax = 0;    // fork returns 0 in the child

    strlcpy(p->cwd, me->cwd, sizeof p->cwd);
    p->parent = me;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (me->fds[i]) {
            file_get(me->fds[i]);
            p->fds[i] = me->fds[i];
        }
    }

    struct thread *t = start_proc(p, cur->name, fork_child, f, true, as, me->pgid, true);
    if (!t) {
        close_fds(p);
        kfree(p);
        kfree(f);
        vmm_destroy_space(as);
        return ZK_ENOMEM;
    }
    return (int64_t)p->pid;
}

int proc_exec(const char *path, char **argv, char **envp, void *release)
{
    struct thread *t = thread_current();
    struct user_start s;
    struct address_space *as;
    int err = build_image(path, argv, envp, &as, &s);
    if (err)
        return err;

    // past this point there's no going back
    uint64_t flags = irq_save();
    struct address_space *old = t->as;
    t->as = as;
    vmm_activate(as);
    strlcpy(t->name, base_name(path), sizeof t->name);
    irq_restore(flags);
    vmm_destroy_space(old);
    kfree(release);

    fpu_init_state(t->fpu);
    fpu_restore(t->fpu);
    enter_user(s.entry, s.rsp, s.argc, s.argv, s.envp);
}

void proc_console_opts(struct spawn_opts *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < 3; i++)
        o->std[i] = file_new(&tty_vnode, O_RDWR);
}

void proc_put_console(struct spawn_opts *o)
{
    for (int i = 0; i < 3; i++)
        if (o->std[i])
            file_put(o->std[i]);
}

struct proc *proc_current(void)
{
    return thread_current()->proc;
}

bool proc_killed(void)
{
    struct proc *p = proc_current();
    return p && p->killed;
}

// interrupts off
static void kill_locked(struct proc *p)
{
    p->killed = true;
    waitq_wake_all(&p->child_wq);
}

int proc_kill(uint64_t pid)
{
    int err = ZK_ESRCH;
    uint64_t flags = irq_save();
    for (struct proc *p = procs; p; p = p->list_next) {
        if (p->pid == pid && p->thread->state != THREAD_DEAD) {
            kill_locked(p);
            err = 0;
        }
    }
    irq_restore(flags);
    return err;
}

int proc_kill_group(uint64_t pgid)
{
    int n = 0;
    uint64_t flags = irq_save();
    for (struct proc *p = procs; p; p = p->list_next) {
        if (p->pgid == pgid && p->thread->state != THREAD_DEAD) {
            kill_locked(p);
            n++;
        }
    }
    irq_restore(flags);
    return n;
}

void proc_check_killed(void)
{
    if (proc_killed()) {
        sti();
        proc_exit(ZK_EXIT_KILLED);
    }
}

void proc_exit(int code)
{
    struct proc *p = proc_current();
    close_fds(p);    // can block, so before interrupts go off

    cli();
    for (struct proc *c = procs; c; c = c->list_next) {
        if (c->parent != p)
            continue;
        c->parent = NULL;
        thread_detach(c->thread);
    }
    if (p->parent)
        waitq_wake_all(&p->parent->child_wq);
    thread_exit(code);
}

int64_t proc_wait(int64_t pid, int *status, int flags)
{
    struct proc *me = proc_current();
    uint64_t flags_irq = irq_save();
    for (;;) {
        bool any = false;
        struct proc *found = NULL;
        for (struct proc *c = procs; c; c = c->list_next) {
            if (c->parent != me || (pid > 0 && c->pid != (uint64_t)pid))
                continue;
            any = true;
            if (c->thread->state == THREAD_DEAD) {
                found = c;
                break;
            }
        }
        if (!any) {
            irq_restore(flags_irq);
            return ZK_ECHILD;
        }
        if (found) {
            int64_t id = (int64_t)found->pid;
            found->parent = NULL;
            irq_restore(flags_irq);
            int code = thread_join(found->thread);    // frees found
            if (status)
                *status = code;
            return id;
        }
        if (flags & WNOHANG) {
            irq_restore(flags_irq);
            return 0;
        }
        if (me->killed) {
            irq_restore(flags_irq);
            return ZK_EINTR;
        }
        waitq_sleep(&me->child_wq);
    }
}

// from the scheduler when the thread gets freed
void proc_free(struct proc *p)
{
    uint64_t flags = irq_save();
    for (struct proc **pp = &procs; *pp; pp = &(*pp)->list_next) {
        if (*pp == p) {
            *pp = p->list_next;
            break;
        }
    }
    irq_restore(flags);
    close_fds(p);    // only a proc that never ran still has some
    kfree(p);
}

int proc_fd_alloc(struct proc *p, struct file *f)
{
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            return i;
        }
    }
    return ZK_EMFILE;
}

struct file *proc_fd_get(struct proc *p, int fd)
{
    if (fd < 0 || fd >= PROC_MAX_FDS)
        return NULL;
    return p->fds[fd];
}

int proc_snapshot(struct zk_procinfo *out, int max)
{
    int n = 0;
    uint64_t flags = irq_save();
    for (struct proc *p = procs; p && n < max; p = p->list_next) {
        struct zk_procinfo *i = &out[n++];
        i->pid = p->pid;
        i->ppid = p->parent ? p->parent->pid : 0;
        i->pgid = p->pgid;
        i->cpu_ms = p->thread->cpu_ticks * 1000 / TIMER_HZ;
        i->state = p->thread->state;
        i->pad = 0;
        strlcpy(i->name, p->thread->name, sizeof i->name);
    }
    irq_restore(flags);
    return n;
}

const char *proc_strerror(int err)
{
    static const char *const names[] = {
        [0] = "success",
        [-ZK_EFAULT] = "bad address",
        [-ZK_EINVAL] = "invalid argument",
        [-ZK_ENOMEM] = "out of memory",
        [-ZK_ENOSYS] = "not implemented",
        [-ZK_ENOENT] = "no such file or directory",
        [-ZK_EBADF] = "bad file descriptor",
        [-ZK_EEXIST] = "already exists",
        [-ZK_ENOTDIR] = "not a directory",
        [-ZK_EISDIR] = "is a directory",
        [-ZK_ENOTEMPTY] = "directory not empty",
        [-ZK_EMFILE] = "too many open files",
        [-ZK_ECHILD] = "no child processes",
        [-ZK_ENOEXEC] = "not an executable",
        [-ZK_ENAMETOOLONG] = "name too long",
        [-ZK_EPIPE] = "broken pipe",
        [-ZK_EINTR] = "interrupted",
        [-ZK_ESRCH] = "no such process",
        [-ZK_EFBIG] = "file too big",
        [-ZK_EPERM] = "not permitted",
        [-ZK_E2BIG] = "argument list too long",
        [-ZK_EBUSY] = "busy",
        [-ZK_EXDEV] = "cross-device link",
        [-ZK_ENOSPC] = "no space left on device",
        [-ZK_EIO] = "i/o error",
        [-ZK_EROFS] = "read-only file system",
        [-ZK_ENODEV] = "no such device",
    };
    if (err > 0 || -err > ZK_ERRNO_MAX)
        return "unknown error";
    return names[-err];
}
