# make            build build/zkernel.iso
# make run        boot in qemu (bios)
# make run-uefi   boot in qemu (uefi)
# make debug      boot paused, gdb on :1234
# make clean
#
# kernel cmdline: make run KCMDLINE="fault=pf"

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

# user programs. no sse either, the kernel doesn't save fpu state yet
USER_CFLAGS := -std=gnu11 -O2 -g -Wall -Wextra \
               -ffreestanding -fno-stack-protector -fno-pic -fno-pie -fno-lto \
               -m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
               -Iuser/lib -Ikernel/src -Ikernel/include -MMD -MP
USER_LDFLAGS := -m elf_x86_64 -nostdlib -static -z max-page-size=0x1000 \
                -z noexecstack -T user/linker.ld
USER_PROGS   := hello spin fault guess
USER_BINS    := $(USER_PROGS:%=$(BUILD)/user/%.elf)
USER_LIB     := $(BUILD)/user/lib/crt0.asm.o $(BUILD)/user/lib/zk.o \
                $(BUILD)/user/klib/printf.o $(BUILD)/user/klib/string.o

.PHONY: all run run-uefi debug clean FORCE
all: $(ISO)

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

# user.asm incbins the programs
$(BUILD)/kernel/src/proc/user.asm.o: $(USER_BINS)

$(KERNEL): $(OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO): $(KERNEL) $(BUILD)/limine.conf
	rm -rf $(BUILD)/iso_root
	mkdir -p $(BUILD)/iso_root/boot/limine $(BUILD)/iso_root/EFI/BOOT
	cp $(KERNEL) $(BUILD)/iso_root/boot/zkernel
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

run: $(ISO)
	./scripts/vm.sh $(ISO)

run-uefi: $(ISO)
	./scripts/vm.sh --uefi $(ISO)

debug: $(ISO)
	./scripts/vm.sh --debug $(ISO)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d) $(wildcard $(BUILD)/user/*.d $(BUILD)/user/*/*.d)
