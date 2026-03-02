CROSS ?= aarch64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

BUILD := build
KBUILD := $(BUILD)/kernel
UBUILD := $(BUILD)/user
EMBED := $(BUILD)/embed

CFLAGS_COMMON := -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -nostartfiles -Wall -Wextra -O2 -mcpu=cortex-a53 -MMD -MP
KERNEL_DEFS ?=
CFLAGS_KERNEL := $(CFLAGS_COMMON) -std=c11 -Iinclude $(KERNEL_DEFS)
CFLAGS_USER := $(CFLAGS_COMMON) -std=c11 -Iuser -Iinclude
ASFLAGS := -mcpu=cortex-a53
LDFLAGS_KERNEL := -nostdlib -static -Wl,--build-id=none -Wl,-T,linker.ld
LDFLAGS_USER := -nostdlib -static -Wl,--build-id=none -Wl,-T,user/user.ld

USER_PROGS := init sh ls cat echo clear mkdir rmdir rm pwd touch cp mv sleep kill mount umount ln mkfs.ext4 fsck.ext4
USER_ELFS := $(addprefix $(UBUILD)/,$(addsuffix .elf,$(USER_PROGS)))
EMBED_OBJS := $(addprefix $(EMBED)/,$(addsuffix .o,$(USER_PROGS)))

KERNEL_OBJS := \
	$(KBUILD)/boot.o \
	$(KBUILD)/vectors.o \
	$(KBUILD)/kernel.o \
	$(KBUILD)/trap.o \
	$(KBUILD)/syscall.o \
	$(KBUILD)/task.o \
	$(KBUILD)/pagecache.o \
	$(KBUILD)/ext4.o \
	$(KBUILD)/virtio_blk.o \
	$(KBUILD)/ahci.o \
	$(KBUILD)/nvme.o \
	$(KBUILD)/block_cache.o \
	$(KBUILD)/fs.o \
	$(KBUILD)/pipe.o \
	$(KBUILD)/mmu.o \
	$(KBUILD)/gic.o \
	$(KBUILD)/timer.o \
	$(KBUILD)/pmm.o \
	$(KBUILD)/uart.o \
	$(KBUILD)/string.o \
	$(KBUILD)/print.o \
	$(KBUILD)/user_copy.o

USER_LIB_OBJS := \
	$(UBUILD)/crt0.o \
	$(UBUILD)/syscall.o \
	$(UBUILD)/string.o \
	$(UBUILD)/io.o

all: $(BUILD)/kernel.elf

$(BUILD)/kernel.elf: $(KERNEL_OBJS) $(EMBED_OBJS) linker.ld | $(BUILD)
	$(LD) $(LDFLAGS_KERNEL) -o $@ $(KERNEL_OBJS) $(EMBED_OBJS)

$(KBUILD)/%.o: kernel/%.c | $(KBUILD)
	$(CC) $(CFLAGS_KERNEL) -c -o $@ $<

$(KBUILD)/%.o: kernel/%.S | $(KBUILD)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(UBUILD)/%.o: user/lib/%.c | $(UBUILD)
	$(CC) $(CFLAGS_USER) -c -o $@ $<

$(UBUILD)/crt0.o: user/crt0.S | $(UBUILD)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(UBUILD)/%.prog.o: user/%.c | $(UBUILD)
	$(CC) $(CFLAGS_USER) -c -o $@ $<

$(UBUILD)/%.elf: $(UBUILD)/%.prog.o $(USER_LIB_OBJS) user/user.ld | $(UBUILD)
	$(LD) $(LDFLAGS_USER) -o $@ $< $(USER_LIB_OBJS)

$(EMBED)/%.o: $(UBUILD)/%.elf | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(KBUILD) $(UBUILD) $(EMBED) $(BUILD):
	mkdir -p $@

userspace: $(USER_ELFS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean userspace

# Ensure kernel embeds latest userspace binaries.
$(BUILD)/kernel.elf: $(USER_ELFS)
$(EMBED_OBJS): $(USER_ELFS)

-include $(wildcard $(KBUILD)/*.d) $(wildcard $(UBUILD)/*.d)
