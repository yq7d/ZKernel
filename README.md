# ZKernel
A kernel made from scratch. Semi assisted with A.I
## What is ZKernel?
originally started in **2023**, it was made simply because I was very bored and was testing my skills of c and ASM. 
It is now being continued as a side project sometimes receiving updates

## What it can do
x86_64, boots with Limine (BIOS and UEFI), single cpu.

- **memory:** physical page allocator, per process address spaces, copy on write, kernel heap, guard pages on kernel stacks
- **processes:** preemptive scheduler, ring 3 user programs (static ELF), `fork`, `exec`, `spawn`, `wait`, environment variables, ctrl+c kills the foreground job
- **files:** VFS with mounts, ramfs root unpacked from an initrd, FAT32 (read/write, long names) on a disk, `/proc`, `/dev`, pipes
- **drivers:** framebuffer console, PS/2 keyboard, serial (input and output), PIT timer, CMOS clock, PCI, virtio-blk
- **userland:** a shell (`/bin/sh`) with pipes, redirects, background jobs, `;` `&&` `||`, variables, and about 50 commands: `ls cp mv rm mkdir cat grep sort head tail find df du mount ps kill`...
- **tests:** kernel self tests run on every boot, `utest` tests the user side (malloc, fork, exec, fpu, files)

## Building and running
Arch: `./scripts/setup.sh` installs everything. Elsewhere you need gcc, nasm, ld, xorriso, limine, qemu, mtools and dosfstools.

```
make              # build/zkernel.iso, plus disk.img the first time
make run          # boot in qemu (bios)
make run-uefi     # boot in qemu (uefi)
make debug        # wait for gdb on :1234
make disk         # wipe disk.img and make a fresh one
make clean        # everything except disk.img
```

`disk.img` is a 64 MiB FAT32 image that shows up as `/mnt`. It survives rebuilds, so it's where stuff you want to keep goes. You can copy files onto it from the host with `mcopy -i disk.img file ::`.

Kernel command line options go through `KCMDLINE`:

```
make run KCMDLINE="init=kshell"        # built in kernel shell instead of /bin/sh
make run KCMDLINE="init=/mnt/bin/init" # run something else as the first process
make run KCMDLINE="fault=pf"           # crash on purpose to test the fault handler
```

## Layout
```
kernel/src/arch    gdt, idt, interrupts, fpu, power
kernel/src/mm      physical memory, paging, heap
kernel/src/sched   scheduler, mutexes, semaphores
kernel/src/proc    processes, elf loader, syscalls
kernel/src/fs      vfs, ramfs, fat32, procfs, pipes, initrd
kernel/src/drivers console, keyboard, serial, timer, rtc, pci, virtio-blk, tty
kernel/include/zk  syscall numbers and structs shared with user programs
user/              user programs, one .c file each
user/lib           the small libc they link against
rootfs/            files copied into / at boot (etc/motd, etc/profile...)
```

## Writing a program for it
Make `user/hello2.c`:

```c
#include "zk.h"

int main(int argc, char **argv)
{
    printf("hi from %s\n", argv[0]);
    return 0;
}
```

Add `hello2` to `USER_PROGS` in the Makefile, run `make run`, and type `hello2`. The syscalls are listed in `kernel/include/zk/syscall.h`, and `user/lib/zk.h` has the C side of them.

## Why is it A.I assisted (grrrrrrrrrrrrrrrrrrr)
Well, honestly I **don't** care. I dont care if my stuff is A.I assisted. I see no wrong in using A.I to assist me. Furthermore, 
if you hate on A.I for "water usage" and so you come over to me and complain because im using it to create a kernel, just note:
The stuff you see on instagram reels and tiktok is not really truthful. 

## Is it stable?
i doubt it ever will be

## CONTACT INFO:
Discord: x.monolith
Website: https://imzire.space

side thing:
*if you arent using A.I to assist you in 2026, youre falling behind.*
