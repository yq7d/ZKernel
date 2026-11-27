#include "proc/proc.h"
#include "proc/elf.h"
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

extern void enter_user(uint64_t entry, uint64_t rsp, uint64_t argc, uint64_t argv)
    __attribute__((noreturn));

extern const uint8_t prog_hello_start[], prog_hello_end[];
extern const uint8_t prog_spin_start[], prog_spin_end[];
extern const uint8_t prog_fault_start[], prog_fault_end[];
extern const uint8_t prog_guess_start[], prog_guess_end[];

#define PROGRAM(n) { #n, prog_##n##_start, 0 }

static struct program table[] = {
    PROGRAM(hello),
    PROGRAM(spin),
    PROGRAM(fault),
    PROGRAM(guess),
    { NULL, NULL, 0 },
};

// sizes aren't constant expressions, fill them in on first use
static void fill_sizes(void)
{
    static bool done;
    if (done)
        return;
    table[0].size = (size_t)(prog_hello_end - prog_hello_start);
    table[1].size = (size_t)(prog_spin_end - prog_spin_start);
    table[2].size = (size_t)(prog_fault_end - prog_fault_start);
    table[3].size = (size_t)(prog_guess_end - prog_guess_start);
    done = true;
}

const struct program *program_list(void)
{
    fill_sizes();
    return table;
}

const struct program *program_find(const char *name)
{
    fill_sizes();
    for (const struct program *p = table; p->name; p++)
        if (strcmp(p->name, name) == 0)
            return p;
    return NULL;
}

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

static bool load_elf(struct address_space *as, const uint8_t *data, size_t size,
                     uint64_t *entry, const char **err)
{
    const struct elf64_ehdr *eh = (const void *)data;
    if (size < sizeof(*eh) || eh->magic != ELF_MAGIC || eh->class != ELFCLASS64 ||
        eh->data != ELFDATA2LSB || eh->type != ET_EXEC || eh->machine != EM_X86_64 ||
        eh->phentsize != sizeof(struct elf64_phdr) ||
        eh->phoff > size || eh->phnum > (size - eh->phoff) / sizeof(struct elf64_phdr)) {
        *err = "not a valid x86_64 executable";
        return false;
    }

    const struct elf64_phdr *ph = (const void *)(data + eh->phoff);
    for (int i = 0; i < eh->phnum; i++) {
        const struct elf64_phdr *p = &ph[i];
        if (p->type != PT_LOAD || p->memsz == 0)
            continue;

        uint64_t end = p->vaddr + p->memsz;
        if (p->filesz > p->memsz || end < p->vaddr || p->vaddr < USER_MIN_ADDR ||
            end > USER_HEAP_BASE || p->offset > size || p->filesz > size - p->offset) {
            *err = "bad program segment";
            return false;
        }

        uint64_t flags = 0;
        if (p->flags & PF_W)
            flags |= PTE_WRITE;
        if (!(p->flags & PF_X))
            flags |= PTE_NX;

        if (!map_zeroed(as, p->vaddr, end, flags) ||
            !copy_to_user(as, p->vaddr, data + p->offset, p->filesz)) {
            *err = "out of memory";
            return false;
        }
    }

    *entry = eh->entry;
    if (*entry < USER_MIN_ADDR || *entry >= USER_HEAP_BASE) {
        *err = "bad entry point";
        return false;
    }
    return true;
}

// argv strings and pointers go at the top of the stack
static bool setup_stack(struct address_space *as, int argc, char **argv,
                        uint64_t *rsp_out, uint64_t *argv_out)
{
    uint64_t bottom = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    if (!map_zeroed(as, bottom, USER_STACK_TOP, PTE_WRITE | PTE_NX))
        return false;

    uint64_t sp = USER_STACK_TOP;
    uint64_t ptrs[16];
    if (argc > 15)
        argc = 15;

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if (len > 1024)
            return false;
        sp -= len;
        if (!copy_to_user(as, sp, argv[i], len))
            return false;
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;

    sp = ALIGN_DOWN(sp, 16);
    sp -= (uint64_t)(argc + 1) * sizeof(uint64_t);
    sp = ALIGN_DOWN(sp, 16);
    if (!copy_to_user(as, sp, ptrs, (size_t)(argc + 1) * sizeof(uint64_t)))
        return false;

    *argv_out = sp;
    *rsp_out = sp;    // 16 byte aligned, _start calls main from here
    return true;
}

struct user_start {
    uint64_t entry, rsp, argc, argv;
};

static void user_thread(void *arg)
{
    struct user_start s = *(struct user_start *)arg;
    kfree(arg);
    enter_user(s.entry, s.rsp, s.argc, s.argv);
}

struct thread *proc_spawn(const struct program *prog, int argc, char **argv,
                          bool detached, const char **err)
{
    const char *dummy;
    if (!err)
        err = &dummy;
    *err = "out of memory";

    struct address_space *as = vmm_create_space();
    if (!as)
        return NULL;
    as->heap_top = USER_HEAP_BASE;

    struct user_start *s = kzalloc(sizeof(*s));
    bool ok = s && load_elf(as, prog->data, prog->size, &s->entry, err);
    if (ok && !setup_stack(as, argc, argv, &s->rsp, &s->argv)) {
        *err = "can't set up the stack";
        ok = false;
    }
    if (!ok) {
        kfree(s);
        vmm_destroy_space(as);
        return NULL;
    }
    s->argc = (uint64_t)argc;

    struct thread *t = thread_create_with(prog->name, user_thread, s, !detached, as);
    if (!t) {
        kfree(s);
        vmm_destroy_space(as);
    }
    return t;
}
