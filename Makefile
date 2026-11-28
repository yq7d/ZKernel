# make            build build/zkernel.iso
# make run        boot in qemu (bios)
# make run-uefi   boot in qemu (uefi)
# make vbox       boot in virtualbox (makes or fixes the vm)
# make debug      boot paused, gdb on :1234
# make disk       fresh disk image (wipes the old one): 64 MiB fat, the rest ext2
# make clean      everything but the disk image
#
# bundled linux programs, in the iso as a read only image on /compat/apps.
# downloaded once into .cache/apk. make APPS= leaves them out
#
# kernel cmdline: make run KCMDLINE="fault=pf"
#   init=/bin/init (default, the desktop), init=/bin/sh for text only,
#   init=kshell for the built in kernel shell

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
                env pwd hostname du find xargs printf init zwm \
                id whoami sudo su passwd chmod chown ifconfig ping host fetch pkg zfetch \
                mkfs.fat mkfs.ext2 zos-install startx xrun
USER_BINS    := $(USER_PROGS:%=$(BUILD)/user/%.elf)
USER_LIB     := $(BUILD)/user/lib/crt0.asm.o $(BUILD)/user/lib/zk.o $(BUILD)/user/lib/malloc.o $(BUILD)/user/lib/env.o $(BUILD)/user/lib/util.o \
                $(BUILD)/user/lib/sha256.o $(BUILD)/user/lib/users.o $(BUILD)/user/lib/net.o \
                $(BUILD)/user/lib/inflate.o $(BUILD)/user/lib/fsprobe.o \
                $(BUILD)/user/klib/printf.o $(BUILD)/user/klib/string.o \
                $(BUILD)/user/klib/time.o $(BUILD)/user/klib/font.o \
                $(BUILD)/user/lib/gfx.o

DISK := disk.img

APPS ?= xorg-server xf86-video-fbdev xf86-input-evdev twm xterm xsetroot xeyes \
        font-misc-misc font-dejavu firefox ca-certificates
APPS_IMG := $(if $(strip $(APPS)),$(BUILD)/apps.img)

.PHONY: all run run-uefi vbox debug clean disk FORCE
all: $(ISO) $(DISK)

# made once and kept, it's the persistent storage (see /etc/fstab):
#   partition 1, fat32, /mnt. easy to reach from the host with mtools
#   partition 2, ext2, /data. home, installed packages, /usr/local
DISK_MB := 30720
FAT_MB  := 64
$(DISK):
	dd if=/dev/zero of=$@ bs=1M count=0 seek=$(DISK_MB) status=none
	printf 'label: dos\nstart=2048, size=%d, type=c\nstart=%d, type=83\n' \
	    $$(($(FAT_MB) * 2048)) $$(($(FAT_MB) * 2048 + 2048)) | sfdisk -q $@
	mkfs.fat -F 32 -s 1 -n ZOSSHARE --offset 2048 $@ $$(($(FAT_MB) * 1024)) >/dev/null 2>&1
	mke2fs -q -F -t ext2 -O ^dir_index -L ZOSDATA -E offset=$$(($(FAT_MB) * 1048576 + 1048576)) \
	    $@ $$((($(DISK_MB) - $(FAT_MB) - 1) * 1024))k
	printf 'this file lives on the fat32 partition\n' | mcopy -i $@@@1M - ::readme.txt

disk:
	rm -f $(DISK)
	$(MAKE) $(DISK)

# only rewritten when the output changes, so the iso rebuilds on cmdline change
$(BUILD)/limine.conf: limine.conf FORCE
	@mkdir -p $(BUILD)
	@sed -e 's|@CMDLINE@|$(KCMDLINE)|' \
	     -e '$(if $(APPS_IMG),s|^\(.*\)module_path: boot():/boot/initrd.tar|&\n\1module_path: boot():/boot/apps.img|)' $< > $@.tmp
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

# a static glibc program to test the linux layer with, built by the host's gcc
LTEST := $(BUILD)/ltest
$(LTEST): tests/linux/ltest.c
	@mkdir -p $(BUILD)
	gcc -static -O2 -o $@ $<
	strip $@

# rootfs/ plus the programs in /bin
INITRD       := $(BUILD)/initrd.tar
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)

$(INITRD): $(USER_BINS) $(ROOTFS_FILES) kernel/fonts/ter-v16n.psf $(LTEST) $(KERNEL)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin
	[ -d rootfs ] && cp -r rootfs/. $(BUILD)/rootfs/ || true
	for p in $(USER_PROGS); do strip -o $(BUILD)/rootfs/bin/$$p $(BUILD)/user/$$p.elf; done
	mkdir -p $(BUILD)/rootfs/usr/share/fonts
	cp kernel/fonts/ter-v16n.psf $(BUILD)/rootfs/usr/share/fonts/
	# for testing the linux layer: make ALPINE_ROOT=/path/to/extracted/rootfs
	if [ -n "$(ALPINE_ROOT)" ]; then mkdir -p $(BUILD)/rootfs/compat/linux && \
	    cp -a $(ALPINE_ROOT)/. $(BUILD)/rootfs/compat/linux/; fi
	mkdir -p $(BUILD)/rootfs/usr/lib/zos
	cp $(LTEST) $(BUILD)/rootfs/usr/lib/zos/ltest
	ln -sf xrun $(BUILD)/rootfs/bin/firefox
	ln -sf zfetch $(BUILD)/rootfs/bin/neofetch
	ln -sf zfetch $(BUILD)/rootfs/bin/fastfetch
	chmod 4755 $(BUILD)/rootfs/bin/sudo $(BUILD)/rootfs/bin/su $(BUILD)/rootfs/bin/passwd
	chmod 600 $(BUILD)/rootfs/etc/shadow
	chmod 440 $(BUILD)/rootfs/etc/sudoers
	# what an installed system boots: everything but the installer's payload
	tar --format=ustar --owner=0 --group=0 -cf $(BUILD)/initrd-base.tar -C $(BUILD)/rootfs .
	# the live system carries a copy of itself for zos-install
	mkdir -p $(BUILD)/rootfs/usr/share/zos/install
	cp $(KERNEL) $(BUILD)/rootfs/usr/share/zos/install/zkernel
	cp $(BUILD)/initrd-base.tar $(BUILD)/rootfs/usr/share/zos/install/initrd.tar
	cp $(LIMINE)/BOOTX64.EFI $(BUILD)/rootfs/usr/share/zos/install/
	tar --format=ustar --owner=0 --group=0 -cf $@ -C $(BUILD)/rootfs .

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

# the package list is remembered so changing APPS rebuilds the image
$(BUILD)/apps.list: FORCE
	@mkdir -p $(BUILD)
	@echo '$(strip $(APPS))' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(BUILD)/apps.img: $(BUILD)/apps.list scripts/mkapps.py
	python3 scripts/mkapps.py $@ .cache/apk $(APPS)

$(ISO): $(KERNEL) $(INITRD) $(BUILD)/limine.conf $(APPS_IMG)
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/zkernel
	cp $(INITRD) $(BUILD)/iso_root/boot/initrd.tar
	$(if $(APPS_IMG),cp $(APPS_IMG) $(BUILD)/iso_root/boot/apps.img)
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

vbox: $(ISO) $(DISK)
	./scripts/vbox.sh $(ISO)

debug: $(ISO) $(DISK)
	./scripts/vm.sh --debug --disk $(DISK) $(ISO)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(wildcard $(BUILD)/user/*.d $(BUILD)/user/*/*.d)
