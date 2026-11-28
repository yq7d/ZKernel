#include "proc/proc.h"
#include "proc/elf.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/interrupts.h"
#include "drivers/pit.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "lib/random.h"
#include "lib/string.h"
#include "proc/linux.h"
#include "proc/futex.h"
#include "proc/sys.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"

// user space layout
//   0000000000400000  fixed address executables
//   0000100000000000  heap from alloc_pages, growing up
//   0000555555554000  position independent executables
//   00007f0000000000  mmap, growing down
//   00007fc000000000  the dynamic loader
//   00007ffffff00000  top of the stack, which grows down on faults
#define USER_STACK_TOP   0x00007ffffff00000UL
#define USER_STACK_PAGES 16
#define USER_STACK_MAX   (8UL << 20)
#define USER_HEAP_BASE   0x0000100000000000UL
#define PIE_BASE         0x0000555555554000UL
#define USER_MMAP_TOP    0x00007f0000000000UL
#define INTERP_BASE      0x00007fc000000000UL
#define USER_MIN_ADDR    0x10000UL    // keep null pointer derefs faulting

extern void enter_user(uint64_t entry, uint64_t rsp, uint64_t argc, uint64_t argv,
                       uint64_t envp) __attribute__((noreturn));
extern void return_to_user(struct interrupt_frame *f) __attribute__((noreturn));

struct user_start {
    uint64_t entry, rsp, argc, argv, envp;
};

char *proc_default_env[] = { "PATH=/bin", "HOME=/", "SHELL=/bin/sh", "TERM=zkernel", NULL };

// kernel threads and whatever the kernel starts directly
static const struct zk_cred root_cred;

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

// segments sharing a page get the union of their permissions
static bool map_zeroed(struct address_space *as, uint64_t start, uint64_t end, uint64_t flags)
{
    for (uint64_t v = ALIGN_DOWN(start, PAGE_SIZE); v < end; v += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_translate_user(as, v, &phys)) {
            if (flags & PTE_WRITE)
                vmm_protect_user(as, v, PTE_WRITE | (flags & PTE_NX));
            else if (!(flags & PTE_NX))
                vmm_protect_user(as, v, 0);
            continue;
        }
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

struct elf_info {
    uint64_t entry;
    uint64_t base;       // load bias, 0 for fixed address executables
    uint64_t phdr, phnum;
    uint64_t end;        // highest address used
    bool native;         // carries the ZOS note
    char interp[ZK_PATH_MAX];
};

#define NOTE_ZOS 0x534f5a    // "ZOS"

static bool has_zos_note(const uint8_t *data, size_t size, const struct elf64_phdr *p)
{
    if (p->offset > size || p->filesz > size - p->offset)
        return false;
    const uint8_t *n = data + p->offset, *end = n + p->filesz;
    while (n + 12 <= end) {
        uint32_t namesz = *(const uint32_t *)n, descsz = *(const uint32_t *)(n + 4);
        const uint8_t *name = n + 12;
        if (name + namesz > end)
            break;
        if (namesz == 4 && memcmp(name, "ZOS", 4) == 0)
            return true;
        n = name + ALIGN_UP(namesz, 4) + ALIGN_UP(descsz, 4);
    }
    return false;
}

static int load_elf(struct address_space *as, const uint8_t *data, size_t size,
                    uint64_t base, struct elf_info *info)
{
    const struct elf64_ehdr *eh = (const void *)data;
    if (size < sizeof(*eh) || eh->magic != ELF_MAGIC || eh->class != ELFCLASS64 ||
        eh->data != ELFDATA2LSB || eh->machine != EM_X86_64 ||
        (eh->type != ET_EXEC && eh->type != ET_DYN) ||
        eh->phentsize != sizeof(struct elf64_phdr) ||
        eh->phoff > size || eh->phnum > (size - eh->phoff) / sizeof(struct elf64_phdr))
        return ZK_ENOEXEC;
    if (eh->type == ET_EXEC)
        base = 0;

    memset(info, 0, sizeof(*info));
    info->base = base;
    info->phnum = eh->phnum;
    const struct elf64_phdr *ph = (const void *)(data + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type == PT_INTERP && p->filesz < sizeof info->interp &&
            p->offset < size && p->filesz <= size - p->offset) {
            memcpy(info->interp, data + p->offset, p->filesz);
            info->interp[p->filesz] = '\0';
        }
        if (p->type == PT_NOTE && has_zos_note(data, size, p))
            info->native = true;
        if (p->type == PT_PHDR)
            info->phdr = base + p->vaddr;
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;

        uint64_t start = base + p->vaddr, end = start + p->memsz;
        if (p->filesz > p->memsz || end < start || start < USER_MIN_ADDR ||
            end > USER_STACK_TOP - USER_STACK_MAX || p->offset > size || p->filesz > size - p->offset)
            return ZK_ENOEXEC;

        uint64_t flags = 0;
        if (p->flags & PF_W)
            flags |= PTE_WRITE;
        if (!(p->flags & PF_X))
            flags |= PTE_NX;

        if (!map_zeroed(as, start, end, flags) ||
            !copy_to_user(as, start, data + p->offset, p->filesz))
            return ZK_ENOMEM;
        if (end > info->end)
            info->end = end;
        // the headers are usually inside the first segment
        if (!info->phdr && eh->phoff >= p->offset && eh->phoff < p->offset + p->filesz)
            info->phdr = start + (eh->phoff - p->offset);
    }

    info->entry = base + eh->entry;
    if (info->entry < USER_MIN_ADDR || info->entry >= info->end || !info->end)
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

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_EXECFN 31

// the standard layout: strings at the top, then argc, argv[], envp[] and the
// aux vector from rsp upwards. ZOS programs also get argc/argv/envp in registers
static int setup_stack(struct address_space *as, char **argv, char **envp,
                       const char *path, const struct elf_info *prog,
                       uint64_t interp_base, const struct zk_cred *cred, struct user_start *s)
{
    uint64_t initial = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!map_zeroed(as, initial, USER_STACK_TOP, PTE_WRITE | PTE_NX))
        return ZK_ENOMEM;
    as->stack_top = USER_STACK_TOP;
    as->stack_low = USER_STACK_TOP - USER_STACK_MAX;

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

    size_t plen = strlen(path) + 1;
    sp -= plen;
    uint64_t execfn = sp;
    if (!copy_to_user(as, sp, path, plen))
        return ZK_EFAULT;
    uint8_t rnd[16];
    random_bytes(rnd, sizeof rnd);
    sp = ALIGN_DOWN(sp, 16) - 16;
    uint64_t random_at = sp;
    if (!copy_to_user(as, sp, rnd, sizeof rnd))
        return ZK_EFAULT;

    uint64_t aux[] = {
        AT_PHDR, prog->phdr, AT_PHENT, sizeof(struct elf64_phdr), AT_PHNUM, prog->phnum,
        AT_PAGESZ, PAGE_SIZE, AT_BASE, interp_base, AT_ENTRY, prog->entry,
        AT_UID, cred->uid, AT_EUID, cred->euid, AT_GID, cred->gid, AT_EGID, cred->egid,
        AT_SECURE, cred->uid != cred->euid || cred->gid != cred->egid,
        AT_HWCAP, 0, AT_CLKTCK, 100, AT_RANDOM, random_at, AT_EXECFN, execfn,
        AT_NULL, 0,
    };
    size_t words = 1 + (size_t)(argc + 1) + (size_t)(envc + 1) + sizeof aux / 8;
    sp -= words * 8;
    sp = ALIGN_DOWN(sp, 16);
    uint64_t at = sp;
    uint64_t argc64 = (uint64_t)argc;
    if (!copy_to_user(as, at, &argc64, 8) ||
        !copy_to_user(as, at + 8, aptrs, (size_t)(argc + 1) * 8) ||
        !copy_to_user(as, at + 8 + (uint64_t)(argc + 1) * 8, eptrs, (size_t)(envc + 1) * 8) ||
        !copy_to_user(as, at + 8 + (uint64_t)(argc + envc + 2) * 8, aux, sizeof aux))
        return ZK_EFAULT;

    s->argc = argc64;
    s->argv = at + 8;
    s->envp = at + 8 + (uint64_t)(argc + 1) * 8;
    s->rsp = at;
    return 0;
}

// path to the file, through /compat/linux for linux programs
static int read_exec(const char *path, struct zk_stat *st, void **data, size_t *size)
{
    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (err)
        return err;
    if (v->type == VNODE_DIR)
        err = ZK_EISDIR;
    else if (v->type != VNODE_FILE)
        err = ZK_EACCES;
    else
        err = vfs_permission(v, MAY_EXEC);
    st->uid = v->uid;
    st->gid = v->gid;
    st->mode = v->mode;
    vnode_put(v);
    if (err)
        return err;
    return vfs_read_all(path, data, size, PROC_MAX_EXEC);
}

static int build_image(const char *path, char **argv, char **envp,
                       const struct zk_cred *cred, struct address_space **as_out,
                       struct user_start *s, struct zk_stat *st, int *abi);

// "#!interp [arg]" scripts run as interp [arg] script args...
static int run_script(const char *path, const char *text, size_t size, char **argv,
                      char **envp, const struct zk_cred *cred,
                      struct address_space **as_out, struct user_start *s,
                      struct zk_stat *st, int *abi, int depth)
{
    if (depth > 4)
        return ZK_ELOOP;
    char line[ZK_PATH_MAX];
    size_t n = 0;
    for (size_t i = 2; i < size && text[i] != '\n' && n + 1 < sizeof line; i++)
        line[n++] = text[i];
    line[n] = '\0';
    char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    char *interp = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    char *arg = NULL;
    if (*p) {
        *p++ = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        // linux passes the rest as one argument, trailing blanks dropped
        size_t len = strlen(p);
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\r'))
            p[--len] = '\0';
        if (len)
            arg = p;
    }
    if (!interp[0])
        return ZK_ENOEXEC;

    // scripts from linux packages mean the linux tree's interpreter
    char ipath[ZK_PATH_MAX];
    size_t rl = strlen(LINUX_ROOT), al = strlen(APPS_ROOT);
    if ((strncmp(path, LINUX_ROOT, rl) == 0 && path[rl] == '/') ||
        (strncmp(path, APPS_ROOT, al) == 0 && path[al] == '/'))
        linux_resolve(interp, ipath, false);
    else
        strlcpy(ipath, interp, sizeof ipath);

    int argc = 0;
    while (argv && argv[argc])
        argc++;
    char **nargv = kmalloc(sizeof(char *) * (size_t)(argc + 4));
    if (!nargv)
        return ZK_ENOMEM;
    int k = 0;
    nargv[k++] = interp;
    if (arg)
        nargv[k++] = arg;
    nargv[k++] = (char *)path;
    for (int i = 1; i < argc; i++)
        nargv[k++] = argv[i];
    nargv[k] = NULL;

    void *data;
    size_t dsize;
    int err = read_exec(ipath, st, &data, &dsize);
    if (!err && dsize > 2 && ((char *)data)[0] == '#' && ((char *)data)[1] == '!') {
        err = run_script(ipath, data, dsize, nargv, envp, cred, as_out, s, st, abi, depth + 1);
        kfree(data);
    } else if (!err) {
        kfree(data);
        err = build_image(ipath, nargv, envp, cred, as_out, s, st, abi);
    }
    kfree(nargv);
    return err;
}

// new address space with the program (and its interpreter) loaded and the
// stack set up. *abi says what kind of program it is
static int build_image(const char *path, char **argv, char **envp,
                       const struct zk_cred *cred, struct address_space **as_out,
                       struct user_start *s, struct zk_stat *st, int *abi)
{
    void *data;
    size_t size;
    int err = read_exec(path, st, &data, &size);
    if (err)
        return err;
    if (size > 2 && ((char *)data)[0] == '#' && ((char *)data)[1] == '!') {
        // scripts never run set-id, the interpreter's own bits apply
        err = run_script(path, data, size, argv, envp, cred, as_out, s, st, abi, 0);
        kfree(data);
        return err;
    }

    struct address_space *as = vmm_create_space();
    if (!as) {
        kfree(data);
        return ZK_ENOMEM;
    }
    as->heap_top = USER_HEAP_BASE;
    as->mmap_next = USER_MMAP_TOP;

    struct elf_info prog, interp;
    uint64_t interp_base = 0, entry;
    err = load_elf(as, data, size, PIE_BASE, &prog);
    kfree(data);
    *abi = prog.native ? ABI_ZOS : ABI_LINUX;
    entry = prog.entry;

    if (!err && prog.interp[0]) {
        // the dynamic loader lives in the linux tree
        char ipath[ZK_PATH_MAX];
        linux_resolve(prog.interp, ipath, false);
        struct zk_stat ist;
        err = read_exec(ipath, &ist, &data, &size);
        if (!err) {
            err = load_elf(as, data, size, INTERP_BASE, &interp);
            kfree(data);
            interp_base = INTERP_BASE;
            entry = interp.entry;
        }
        *abi = ABI_LINUX;
    }
    if (!err) {
        as->brk_start = as->brk = ALIGN_UP(prog.end, PAGE_SIZE);
        struct zk_cred c = *cred;
        if (st->mode & ZK_S_ISUID)
            c.euid = st->uid;
        if (st->mode & ZK_S_ISGID)
            c.egid = st->gid;
        err = setup_stack(as, argv, envp, path, &prog, interp_base, &c, s);
    }
    if (err) {
        vmm_destroy_space(as);
        return err;
    }
    s->entry = entry;
    // linux's _start reads everything off the stack, and takes rdx as a
    // function to run at exit, so registers have to be zero for it
    if (*abi == ABI_LINUX)
        s->argc = s->argv = s->envp = 0;
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

// set-user-id and set-group-id programs run as the file's owner
static void apply_setid(struct zk_cred *c, const struct zk_stat *st)
{
    if (st->mode & ZK_S_ISUID)
        c->euid = c->suid = st->uid;
    if (st->mode & ZK_S_ISGID)
        c->egid = c->sgid = st->gid;
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
        p->threads = t;
        p->nthreads = 1;
        p->pid = t->id;
        p->pgid = pgid ? pgid : p->pid;
        p->list_next = procs;
        procs = p;
        if (copy_fpu) {
            fpu_save(t->fpu);    // the caller's user state, the kernel never touches it
            t->fs_base = thread_current()->fs_base;
        }
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
    struct zk_stat st;
    int abi = ABI_ZOS;
    p->cred = o->parent ? o->parent->cred : root_cred;
    *err = ZK_ENOMEM;
    if (s && p)
        *err = build_image(path, argv, envp, &p->cred, &as, s, &st, &abi);
    if (*err)
        goto fail;
    apply_setid(&p->cred, &st);
    p->abi = abi;
    strlcpy(p->exe, path, sizeof p->exe);
    // the terminal on stdin becomes the controlling one, else the parent's
    if (o->std[0] && tty_of(o->std[0]->v))
        p->ctty = o->std[0]->v;
    else if (o->parent)
        p->ctty = o->parent->ctty;
    if (p->ctty)
        vnode_get(p->ctty);

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
    uint64_t irq = irq_save();
    struct thread *t = start_proc(p, base_name(path), user_thread, s, joinable, as,
                                  o->pgid, false);
    if (t)
        t->altstack_flags = SS_DISABLE;
    irq_restore(irq);
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

int64_t proc_fork(struct interrupt_frame *frame, uint64_t new_rsp, uint64_t child_tid)
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
    if (new_rsp)
        f->rsp = new_rsp;
    as->brk_start = cur->as->brk_start;
    as->brk = cur->as->brk;
    as->mmap_next = cur->as->mmap_next;
    as->stack_low = cur->as->stack_low;
    as->stack_top = cur->as->stack_top;

    strlcpy(p->cwd, me->cwd, sizeof p->cwd);
    strlcpy(p->exe, me->exe, sizeof p->exe);
    p->parent = me;
    p->cred = me->cred;
    p->abi = me->abi;
    memcpy(p->cloexec, me->cloexec, sizeof p->cloexec);
    p->ctty = me->ctty;
    if (p->ctty)
        vnode_get(p->ctty);
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (me->fds[i]) {
            file_get(me->fds[i]);
            p->fds[i] = me->fds[i];
        }
    }

    // the child's tid goes into its memory before it can run
    uint64_t flags = irq_save();
    struct thread *t = start_proc(p, cur->name, fork_child, f, true, as, me->pgid, true);
    if (t)
        signal_fork(p, t, me, cur);
    if (t && child_tid && vmm_user_range_ok(as, child_tid, 4, true)) {
        uint64_t phys;
        uint32_t tid = (uint32_t)t->id;
        vmm_cow_fault(as, child_tid);
        if (vmm_translate_user(as, child_tid, &phys) && (child_tid & (PAGE_SIZE - 1)) <= PAGE_SIZE - 4)
            memcpy(phys_to_virt(phys), &tid, 4);
    }
    irq_restore(flags);
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
    struct zk_stat st;
    struct proc *me = proc_current();
    int abi;
    // the new image starts with one thread. exec from a thread other than
    // the main one would need the pid to move over, which isn't done
    if (t->extra)
        return ZK_EBUSY;
    int err = build_image(path, argv, envp, &me->cred, &as, &s, &st, &abi);
    if (err)
        return err;
    if (me->nthreads > 1) {
        uint64_t irq = irq_save();
        me->kill_others = true;
        for (struct thread *o = me->threads; o; o = o->tg_next)
            if (o != t)
                thread_interrupt(o);
        while (me->nthreads > 1)
            waitq_sleep(&me->thread_wq);
        me->kill_others = false;
        irq_restore(irq);
    }
    apply_setid(&me->cred, &st);
    me->abi = abi;
    strlcpy(me->exe, path, sizeof me->exe);
    // close on exec
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (me->fds[i] && proc_get_cloexec(me, i)) {
            file_put(me->fds[i]);
            me->fds[i] = NULL;
        }
    }
    memset(me->cloexec, 0, sizeof me->cloexec);
    signal_exec(me, t);
    t->clear_tid = 0;
    t->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);

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

const struct zk_cred *cred_current(void)
{
    struct proc *p = proc_current();
    return p ? &p->cred : &root_cred;
}

bool cred_in_group(const struct zk_cred *c, uint32_t gid)
{
    if (c->egid == gid)
        return true;
    for (uint32_t i = 0; i < c->ngroups && i < ZK_NGROUPS; i++)
        if (c->groups[i] == gid)
            return true;
    return false;
}

bool proc_killed(void)
{
    return signal_pending(thread_current());
}

bool proc_thread_doomed(void)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    return p && (p->killed || p->group_exit || (p->kill_others && t != p->thread));
}

struct thread *proc_find_thread(uint64_t tid)
{
    for (struct proc *p = procs; p; p = p->list_next)
        for (struct thread *t = p->threads; t; t = t->tg_next)
            if (t->id == tid && t->state != THREAD_DEAD)
                return t;
    return NULL;
}

int64_t proc_clone_thread(struct interrupt_frame *frame, uint64_t flags, uint64_t sp,
                          uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    struct proc *me = proc_current();
    struct thread *cur = thread_current();
    struct interrupt_frame *f = kmalloc(sizeof(*f));
    if (!f)
        return ZK_ENOMEM;
    *f = *frame;
    f->rax = 0;
    if (sp)
        f->rsp = sp;

    // nothing runs until it's all set up
    uint64_t irq = irq_save();
    if (me->group_exit || me->kill_others) {
        irq_restore(irq);
        kfree(f);
        return ZK_EAGAIN;
    }
    struct thread *t = thread_create_with(cur->name, fork_child, f, false, cur->as, me);
    if (!t) {
        irq_restore(irq);
        kfree(f);
        return ZK_ENOMEM;
    }
    t->extra = true;
    fpu_save(t->fpu);    // the caller's user state
    t->fs_base = (flags & CLONE_SETTLS) ? tls : cur->fs_base;
    t->sig_blocked = cur->sig_blocked;
    t->altstack_flags = SS_DISABLE;
    if (flags & CLONE_CHILD_CLEARTID)
        t->clear_tid = ctid;
    t->tg_next = me->threads;
    me->threads = t;
    me->nthreads++;
    // the ids land before either thread can look at them
    uint32_t tid = (uint32_t)t->id;
    if (flags & CLONE_PARENT_SETTID)
        copy_out(ptid, &tid, 4);
    if (flags & CLONE_CHILD_SETTID)
        copy_out(ctid, &tid, 4);
    irq_restore(irq);
    return tid;
}

static bool alive(struct proc *p)
{
    return p->thread && p->thread->state != THREAD_DEAD;
}

int proc_kill(uint64_t pid)
{
    int err = ZK_ESRCH;
    uint64_t flags = irq_save();
    for (struct proc *p = procs; p; p = p->list_next) {
        if (p->pid == pid && alive(p)) {
            signal_send(p, SIGKILL, NULL);
            err = 0;
        }
    }
    irq_restore(flags);
    return err;
}

int proc_signal_group(uint64_t pgid, int sig)
{
    int n = 0;
    struct ksiginfo si = { .code = SI_KERNEL };
    uint64_t flags = irq_save();
    for (struct proc *p = procs; p; p = p->list_next) {
        if (p->pgid == pgid && alive(p)) {
            signal_send(p, sig, &si);
            n++;
        }
    }
    irq_restore(flags);
    return n;
}

int proc_kill_group(uint64_t pgid)
{
    return proc_signal_group(pgid, SIGKILL);
}

static bool may_signal(const struct zk_cred *c, const struct proc *p)
{
    return c->euid == 0 || c->uid == p->cred.uid || c->euid == p->cred.uid ||
           c->uid == p->cred.suid || c->euid == p->cred.suid;
}

int proc_signal(int64_t pid, int sig, int code)
{
    if (sig < 0 || sig > NSIG)
        return ZK_EINVAL;
    struct proc *me = proc_current();
    const struct zk_cred *c = cred_current();
    struct ksiginfo si = { .code = code, .pid = me ? (int)me->pid : 0, .uid = c->uid };
    bool found = false, allowed = false;
    uint64_t flags = irq_save();
    for (struct proc *p = procs; p; p = p->list_next) {
        if (!alive(p))
            continue;
        bool hit = pid > 0 ? p->pid == (uint64_t)pid
                 : pid == 0 ? me && p->pgid == me->pgid
                 : pid == -1 ? p->pid != 1 && p != me
                 : p->pgid == (uint64_t)-pid;
        if (!hit)
            continue;
        found = true;
        if (!may_signal(c, p))
            continue;
        allowed = true;
        if (sig)
            signal_send(p, sig, &si);
    }
    irq_restore(flags);
    if (!found)
        return ZK_ESRCH;
    return allowed ? 0 : ZK_EPERM;
}

void proc_exit_signal(int sig, bool core)
{
    struct proc *p = proc_current();
    if (!p->group_exit) {
        p->death_sig = sig;
        p->core = core;
    }
    proc_exit(128 + sig);
}

// the last thread is gone, take the process down with this one
__attribute__((noreturn)) static void finish(struct proc *p, int code)
{
    ktimer_cancel(&p->alarm);
    close_fds(p);    // can block, so before interrupts go off

    cli();
    for (struct proc *c = procs; c; c = c->list_next) {
        if (c->parent != p)
            continue;
        c->parent = NULL;
        thread_detach(c->thread);
    }
    if (p->parent) {
        struct ksiginfo si = {
            .code = p->death_sig ? (p->core ? CLD_DUMPED : CLD_KILLED) : CLD_EXITED,
            .pid = (int)p->pid, .uid = p->cred.uid,
            .status = p->death_sig ? p->death_sig : code & 0xff,
        };
        signal_send(p->parent, SIGCHLD, &si);
        waitq_wake_all(&p->parent->child_wq);
    }
    thread_exit(code);
}

void proc_thread_exit(int code)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;

    if (t->extra) {
        // pthread_join waits on this
        if (t->clear_tid) {
            uint32_t zero = 0;
            if (copy_out(t->clear_tid, &zero, 4) == 0)
                futex_wake(t->as, t->clear_tid, 1, FUTEX_BITSET_ANY);
        }
        cli();
        for (struct thread **pp = &p->threads; *pp; pp = &(*pp)->tg_next) {
            if (*pp == t) {
                *pp = t->tg_next;
                break;
            }
        }
        p->nthreads--;
        waitq_wake_all(&p->thread_wq);
        thread_exit(0);
    }

    // the main thread holds the process up until the others are gone
    cli();
    if (p->nthreads > 1)
        p->main_gone = true;
    while (p->nthreads > 1)
        waitq_sleep(&p->thread_wq);
    sti();
    finish(p, p->group_exit ? p->group_code : code);
}

void proc_exit(int code)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    uint64_t irq = irq_save();
    if (!p->group_exit) {
        p->group_exit = true;
        p->group_code = code;
    }
    for (struct thread *o = p->threads; o; o = o->tg_next)
        if (o != t)
            thread_interrupt(o);
    irq_restore(irq);
    proc_thread_exit(code);
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
            int extra = found->death_sig << 16 | (found->core ? 1 << 24 : 0);
            found->parent = NULL;
            irq_restore(flags_irq);
            int code = thread_join(found->thread);    // frees found
            if (status)
                *status = (code & 0xffff) | extra;
            return id;
        }
        if (flags & WNOHANG) {
            irq_restore(flags_irq);
            return 0;
        }
        if (proc_killed()) {
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
    if (p->ctty)
        vnode_put(p->ctty);
    kfree(p);
}

int proc_fd_alloc_from(struct proc *p, struct file *f, int min)
{
    for (int i = min < 0 ? 0 : min; i < PROC_MAX_FDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            proc_set_cloexec(p, i, false);
            return i;
        }
    }
    return ZK_EMFILE;
}

int proc_fd_alloc(struct proc *p, struct file *f)
{
    return proc_fd_alloc_from(p, f, 0);
}

void proc_set_cloexec(struct proc *p, int fd, bool on)
{
    if (fd < 0 || fd >= PROC_MAX_FDS)
        return;
    if (on)
        p->cloexec[fd / 64] |= 1UL << (fd % 64);
    else
        p->cloexec[fd / 64] &= ~(1UL << (fd % 64));
}

bool proc_get_cloexec(struct proc *p, int fd)
{
    return fd >= 0 && fd < PROC_MAX_FDS && (p->cloexec[fd / 64] >> (fd % 64)) & 1;
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
        i->uid = p->cred.uid;
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
        [-ZK_EACCES] = "permission denied",
        [-ZK_ELOOP] = "too many symlinks",
        [-ZK_ENOTTY] = "not a terminal",
        [-ZK_EAGAIN] = "try again",
        [-ZK_ENOTSOCK] = "not a socket",
        [-ZK_ECONNREFUSED] = "connection refused",
        [-ZK_ETIMEDOUT] = "timed out",
        [-ZK_ENETUNREACH] = "network unreachable",
        [-ZK_EADDRINUSE] = "address in use",
        [-ZK_ENOTCONN] = "not connected",
        [-ZK_ECONNRESET] = "connection reset",
        [-ZK_EHOSTUNREACH] = "host unreachable",
        [-ZK_EINPROGRESS] = "in progress",
    };
    if (err > 0 || -err > ZK_ERRNO_MAX)
        return "unknown error";
    return names[-err];
}
