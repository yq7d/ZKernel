# ZKernel
A kernel made from scratch. Semi assisted with A.I
## What is ZKernel?
originally started in **2023**, it was made simply because I was very bored and was testing my skills of c and ASM. 
It is now being continued as a side project sometimes receiving updates

## ZOS
ZOS is the OS that runs on top of ZKernel. It boots straight into **zwm**, a tiling window manager where every window is a terminal.

| keys | what it does |
|---|---|
| super + enter | new terminal |
| super + q | close the window |
| super + j / k (or arrows) | focus next / previous |
| super + shift + j / k | move the window down / up the stack |
| super + shift + enter | swap with the big (master) window |
| super + h / l | shrink / grow the master area |
| super + f | fullscreen |
| super + 1..9 | switch workspace, add shift to move the window there |
| super + i | zfetch in a new terminal (a binding from `/etc/zwm.conf`) |
| super + shift + r | reload the config |
| super + shift + e | leave the desktop for a text shell (`exit` goes back) |

Alt works as well as super, since a lot of desktops won't let QEMU have the super key. Clicking a window focuses it, and clicking a number in the bar switches workspace. In QEMU click into the window first so it grabs the mouse.

### Logging in, sudo
You're logged in as `zos` (password `zos`). `root`'s password is `root`. Change them with `passwd`.
`zos` is in the `wheel` group, so `sudo` works. Who may use it is in `/etc/sudoers`.

### Customizing
- `~/.config/zwm/zwm.conf`: colors, the background (a color, a gradient, or a `.ppm` picture), gaps, borders, the modifier key, terminal colors, your own key bindings, and programs to open at login. `/etc/zwm.conf` lists every option. Press super + shift + r after changing it.
- `~/.config/zfetch/zfetch.conf`: zfetch's logo (the ZOS badge, the lettering, your own from a text file, or none), colors, which lines to show and in what order, and your own lines. `neofetch` and `fastfetch` run zfetch too.

### Packages
`pkg` installs real [Alpine Linux](https://alpinelinux.org) packages, straight from Alpine's servers:

```
sudo pkg update            # fetch the package lists
pkg search json            # look around
sudo pkg install jq htop   # dependencies come along
pkg list
sudo pkg remove jq
sudo pkg upgrade
```

Every download is checked against the checksum in Alpine's package list. Packages land in `/compat/linux` and run through the Linux compatibility layer, so `jq` just works from the shell.

This uses Alpine and not the Arch AUR because AUR packages are build scripts. They need bash, makepkg and gcc on the machine, and Arch's binaries need glibc's dynamic linker. Alpine ships ready binaries built on musl, which the compatibility layer can load.

### Linux programs
ZOS runs x86_64 Linux binaries, both static ones (glibc or musl) and Alpine's dynamically linked musl ones. Like FreeBSD's Linux layer, Linux programs see `/compat/linux` laid over `/`: a path that exists in there wins.
Signals work like on Linux: handlers (with `SA_SIGINFO`, `SA_RESTART`, `SA_ONSTACK`), masks, `sigsuspend`, `sigaltstack`, faults as catchable `SIGSEGV`/`SIGILL`/`SIGFPE`, `SIGCHLD`, and wait statuses that say which signal killed a child. Ctrl+C sends `SIGINT` and Ctrl+\ sends `SIGQUIT`.
Threads work too: `clone` with `CLONE_THREAD`, futexes, thread local storage, `pthread_kill`, so glibc and musl pthreads (mutexes, condition variables, semaphores, barriers, join) run, and `xz -T4` from Alpine compresses on four threads.
What doesn't work yet: job control (Ctrl+Z, `SIGSTOP`), `alarm`, listening sockets, and anything that reads Linux's own `/proc` files (so `htop` installs but can't show anything).

### Files that stay
Disks are found by filesystem label (see `/etc/fstab`), so it doesn't matter what the disk is called:
- `ZOSDATA` (ext2) goes on `/data`. It holds `/home`, `/compat/linux` (installed packages), `/usr/local` and pkg's database. User accounts, passwords and the hostname are kept in `/usr/local/etc`.
- `ZOSBOOT` (FAT32) goes on `/boot` on an installed system.
- `ZOSSHARE` (FAT32) goes on `/mnt` in the dev disk image. It's easy to reach from the host: `mcopy -i disk.img@@1M file ::`.

Everything else comes fresh from the initrd on each boot. Writes are cached and flushed every second, and `sync` and `poweroff` flush them right away.

### Installing
Boot the ISO (in QEMU, or from a USB stick on a real machine), open a terminal and run:

```
sudo zos-install
```

It lists the disks and asks which one to use, your user name, a password and a name for the computer. Then it erases that disk and sets it up:
- a GPT partition table;
- a 256 MiB EFI partition with Limine, the kernel and the system;
- the rest of the disk as ext2 for your files and packages.

`root` gets the same password as you, `sudo passwd root` changes it. For scripts: `sudo zos-install --disk sda --user bob --password pw --hostname box --yes`.

The installed system boots with **UEFI only**. Legacy BIOS installs aren't done yet.

### Real hardware
Write the ISO to a USB stick with `sudo dd if=build/zkernel.iso of=/dev/sdX bs=4M` (sdX is the stick, double check it), then boot from it.

What ZOS can drive on a real PC:
- **disks:** SATA (AHCI) and NVMe drives with 512 byte sectors, and virtio in VMs
- **network:** Intel e1000 and e1000e cards (8254x, 82574) and virtio-net. Most other cards, including Wi-Fi and newer Intel i219 chips, aren't supported yet
- **input:** PS/2 keyboards and mice. Laptop keyboards and touchpads are usually PS/2 inside. USB keyboards and mice only work if the firmware's "legacy USB" setting emulates PS/2, since there's no USB driver yet
- **screen:** whatever framebuffer the firmware sets up, at its resolution
- **power:** ACPI poweroff and reset
- **cpus:** only the first core is used


## What it can do
x86_64, boots with Limine (BIOS and UEFI), single cpu.

- **memory:** physical page allocator, per process address spaces, copy on write, kernel heap, guard pages on kernel stacks
- **processes:** preemptive scheduler, ring 3 user programs (static ELF), `fork`, `exec`, `spawn`, `wait`, environment variables, signals (ctrl+c sends `SIGINT` to the foreground job), stacks that grow on demand
- **users:** uids, gids, groups, file permissions, set-user-id programs, `sudo`, `su`, `passwd`
- **files:** VFS with mounts and symlinks, ramfs root unpacked from an initrd, ext2 and FAT32 (both read/write, and both can be created with `mkfs.ext2` / `mkfs.fat`), MBR and GPT partitions, bind mounts, `/proc`, `/dev`, pipes, a write-back disk cache
- **network:** virtio-net, ARP, IPv4, ICMP, UDP, TCP, DHCP, DNS, sockets, an HTTP client (`fetch`, `ping`, `host`, `ifconfig`)
- **linux:** a compatibility layer for x86_64 Linux programs (around 160 syscalls), including musl's dynamic loader, signals and threads
- **drivers:** framebuffer console, PS/2 keyboard and mouse, serial (input and output), PIT timer, CMOS clock, PCI, ACPI power off and reset, AHCI, NVMe, virtio-blk, e1000/e1000e, virtio-net
- **graphics and terminals:** `/dev/fb0` that programs can map, raw key and mouse events (`/dev/kbd`, `/dev/mouse`), pseudo terminals, `poll`
- **userland:** `init`, the `zwm` desktop, `zos-install`, `pkg`, `zfetch`, a shell (`/bin/sh`) with pipes, redirects, background jobs, `;` `&&` `||`, variables, `~`, and about 70 commands: `ls cp mv rm mkdir cat grep sort head tail find df du mount ps kill sudo chmod chown`...
- **tests:** kernel self tests run on every boot, `utest` tests the user side (malloc, fork, exec, fpu, files), `ltest` is a static glibc program that tests the Linux layer (files, fork/exec, signals, pthreads)

## Building and running
Arch: `./scripts/setup.sh` installs everything. Elsewhere you need gcc (with static glibc), nasm, ld, xorriso, limine, qemu, mtools, dosfstools, e2fsprogs and sfdisk.

```
make              # build/zkernel.iso, plus disk.img the first time
make run          # boot in qemu (bios)
make run-uefi     # boot in qemu (uefi)
make vbox         # boot in virtualbox
make debug        # wait for gdb on :1234
make disk         # wipe disk.img and make a fresh one
make clean        # everything except disk.img
```

`disk.img` is a 30 GiB sparse image (it only takes the space that is actually used) (64 MiB FAT32 plus ext2, see "Files that stay" above). It survives rebuilds. If yours was made by an older build, run `make disk` once to get the current layout and labels. That wipes it.

`make vbox` (`./scripts/vbox.sh`) makes a VirtualBox VM called ZKernel, or fixes an existing one with `--name`. If VirtualBox boots it and Limine says "This CPU does not support 64-bit mode", the VM's OS type is a 32 bit one: the script sets it to Other/Unknown (64-bit) with long mode on. If it still fails, VT-x/AMD-V is off in the host BIOS, or on Windows, Hyper-V is in the way. `disk.img` is copied to `disk.vdi` the first time, so changes made in VirtualBox stay in that copy.

On Windows, double click `scripts\vbox.bat` and pick the ISO, or run `scripts\vbox.bat zkernel.iso -Disk disk.img`. It does the same setup through `scripts\vbox.ps1` (`-Name`, `-Mem`, `-Cpus`, `-Uefi`, `-NoStart`).

`make run` gives the VM a network card with QEMU's user networking. `./scripts/vm.sh --no-net` leaves it out.

Kernel command line options go through `KCMDLINE`:

```
make run KCMDLINE="text"               # text console instead of the desktop
make run KCMDLINE="init=/bin/sh"       # just a root shell, nothing mounted
make run KCMDLINE="init=kshell"        # built in kernel shell
make run KCMDLINE="init=/mnt/bin/init" # run something else as the first process
make run KCMDLINE="fault=pf"           # crash on purpose to test the fault handler
```

## Layout
```
kernel/src/arch    gdt, idt, interrupts, fpu, acpi, power
kernel/src/mm      physical memory, paging, heap
kernel/src/sched   scheduler, mutexes, semaphores
kernel/src/proc    processes, elf loader, syscalls, the linux layer
kernel/src/fs      vfs, ramfs, ext2, fat32, procfs, pipes, ptys, initrd
kernel/src/net     virtio-net, e1000, ip, arp, icmp, udp, tcp, dhcp, sockets
kernel/src/drivers console, keyboard, mouse, framebuffer, serial, timer, rtc, pci, ahci, nvme, virtio-blk, block cache, tty
kernel/include/zk  syscall numbers and structs shared with user programs
user/              user programs, one .c file each
user/lib           the small libc they link against: gfx, users and passwords, dns and http, gzip
rootfs/            files copied into / at boot (/etc/fstab, zwm.conf, passwd, pkg repositories...)
tests/linux        programs built with the host's gcc to test the linux layer
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

Add `hello2` to `USER_PROGS` in the Makefile, run `make run`, open a terminal and type `hello2`. The syscalls are listed in `kernel/include/zk/syscall.h`, and `user/lib/zk.h` has the C side of them.

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
