#pragma once
#include <stdint.h>

#define ELF_MAGIC     0x464C457FU    // "\x7fELF"
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define ET_EXEC       2
#define EM_X86_64     62
#define PT_LOAD       1
#define PF_X          1
#define PF_W          2

struct elf64_ehdr {
    uint32_t magic;
    uint8_t  class;
    uint8_t  data;
    uint8_t  version;
    uint8_t  osabi;
    uint8_t  pad[8];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));
