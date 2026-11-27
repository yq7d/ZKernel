#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

struct program {
    const char *name;
    const uint8_t *data;
    size_t size;
};

// built in user programs, the list ends with a NULL name
const struct program *program_list(void);
const struct program *program_find(const char *name);

// load an elf into a new address space and start it in ring 3.
// detached processes clean up after themselves, otherwise thread_join them
// on failure returns NULL and points *err at a reason, if err isn't NULL
struct thread *proc_spawn(const struct program *prog, int argc, char **argv,
                          bool detached, const char **err);

void syscall_init(void);
