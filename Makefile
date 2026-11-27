# make            build build/zkernel.iso
# make run        boot in qemu (bios)
# make run-uefi   boot in qemu (uefi)
# make debug      boot paused, gdb on :1234
# make disk       fresh 64 MiB fat32 disk image (wipes the old one)
# make clean      everything but the disk image
#
# kernel cmdline: make run KCMDLINE="fault=pf"
#   init=/bin/sh (default) or init=kshell for the built in kernel shell

BUILD   := build
KCMDLINE ?=
KERNEL  := $(BUILD)/kernel.elf
ISO     := $(BUILD)/zkernel.iso
LIMINE  := /usr/share/limine

CC      := gcc
LD      := ld
AS      := nasm

# no sse/fpu (state isn't saved on interrupts yet), no red zone
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra \
           -ffreestanding -fno-stack-protector -fno-stack-check \
           -fno-PIC -fno-lto -fno-omit-frame-pointer -fstack-clash-protection \
           -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
           -mno-red-zone -mcmodel=kernel \
           -Ikernel/include -Ikernel/src -MMD -MP
LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
           -z noexecstack -T kernel/linker.ld
ASFLAGS := -f elf64 -g -F dwarf

C_SRCS   := $(shell find kernel/src -name '*.c')
ASM_SRCS := $(shell find kernel/src -name '*.asm')
OBJS     := $(C_SRCS:%.c=$(BUILD)/%.o) $(ASM_SRCS:%.asm=$(BUILD)/%.asm.o)

# user programs, fpu/sse state is saved per thread
USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra \
               -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-lto \
               -m64 -march=x86-64 \
               -Iuser/lib -Ikernel/src -Ikernel/include -MMD -MP
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
                -z noexecstack -T user/linker.ld
USER_PROGS   := hello spin fault guess sh ls cat echo mkdir rm wc ps kill utest \
                cp mv rmdir touch stat head tail grep sort uniq tee cut tr seq yes \
                basename dirname hexdump cmp sleep true false dmesg lspci uname \
                uptime free clear date df mount umount sync reboot poweroff which \
                env pwd hostname du find xargs printf
USER_BINS    := $(USER_PROGS:%=$(BUILD)/user/%.elf)
USER_LIB     := $(BUILD)/user/lib/crt0.asm.o $(BUILD)/user/lib/zk.o $(BUILD)/user/lib/malloc.o $(BUILD)/user/lib/env.o $(BUILD)/user/lib/util.o \
                $(BUILD)/user/klib/printf.o $(BUILD)/user/klib/string.o \
                $(BUILD)/user/klib/time.o

DISK := disk.img

.PHONY: all run run-uefi debug clean disk FORCE
all: $(ISO) $(DISK)

# made once and kept, it's the persistent storage. mounted on /mnt
$(DISK):
	dd if=/dev/zero of=$@ bs=1M count=64 status=none
	mkfs.fat -F 32 -n ZKDISK $@ >/dev/null
	printf 'this file lives on the fat32 disk image\n' | mcopy -i $@ - ::readme.txt

disk:
	rm -f $(DISK)
	$(MAKE) $(DISK)

# only rewritten when the output changes, so the iso rebuilds on cmdline change
$(BUILD)/limine.conf: limine.conf FORCE
	@mkdir -p $(BUILD)
	@sed 's|@CMDLINE@|$(KCMDLINE)|' $< > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -MD $(@:.o=.d) -MP $< -o $@

$(BUILD)/user/%.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/klib/%.o: kernel/src/lib/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(BUILD)/user/%.asm.o: user/%.asm
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD)/user/%.elf: $(BUILD)/user/%.o $(USER_LIB) user/linker.ld
	$(LD) $(USER_LDFLAGS) $(USER_LIB) $< -o $@

# rootfs/ plus the programs in /bin
INITRD       := $(BUILD)/initrd.tar
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

$(INITRD): $(USER_BINS) $(ROOTFS_FILES)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin
	[ -d rootfs ] && cp -r rootfs/. $(BUILD)/rootfs/ || true
	for p in $(USER_PROGS); do strip -o $(BUILD)/rootfs/bin/$$p $(BUILD)/user/$$p.elf; done
	tar --format=ustar --owner=0 --group=0 -cf $@ -C $(BUILD)/rootfs .

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO): $(KERNEL) $(INITRD) $(BUILD)/limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/zkernel
	cp $(INITRD) $(BUILD)/iso_root/boot/initrd.tar
	cp $(BUILD)/limine.conf $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	   $(LIMINE)/limine-uefi-cd.bin $(BUILD)/iso_root/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(BUILD)/iso_root/EFI/BOOT/
	xorriso -as mkisofs -quiet -R -r -J \
	    -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 \
	    -boot-info-table -hfsplus -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part \
	    --efi-boot-image --protective-msdos-label \
	    $(BUILD)/iso_root -o $@
	limine bios-install $@ 2>/dev/null

run: $(ISO) $(DISK)
	./scripts/vm.sh --disk $(DISK) $(ISO)

run-uefi: $(ISO) $(DISK)
	./scripts/vm.sh --uefi --disk $(DISK) $(ISO)

debug: $(ISO) $(DISK)
	./scripts/vm.sh --debug --disk $(DISK) $(ISO)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(wildcard $(BUILD)/user/*.d $(BUILD)/user/*/*.d)
