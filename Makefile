CROSS ?= aarch64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)gcc
AR := $(CROSS)ar
OBJCOPY := $(CROSS)objcopy

BUILD := build
KBUILD := $(BUILD)/kernel
UBUILD := $(BUILD)/user
EMBED := $(BUILD)/embed
TCCRT := $(BUILD)/tccrt

CFLAGS_COMMON := -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -nostartfiles -Wall -Wextra -O2 -mcpu=cortex-a53 -MMD -MP
KERNEL_DEFS ?=
CFLAGS_KERNEL := $(CFLAGS_COMMON) -std=c11 -Iinclude $(KERNEL_DEFS)
CFLAGS_USER := $(CFLAGS_COMMON) -std=c11 -Iuser -Iinclude
ASFLAGS := -mcpu=cortex-a53
LDFLAGS_KERNEL := -nostdlib -static -Wl,--build-id=none -Wl,-T,linker.ld
LDFLAGS_USER := -nostdlib -static -Wl,--build-id=none -Wl,-T,user/user.ld

USER_PROGS := init sh ls cat echo clear nano elfinfo mkdir rmdir rm pwd touch cp mv sleep kill mount umount ln chmod mkfs.ext4 fsck.ext4 tcc ar ranlib as
USER_ELFS := $(addprefix $(UBUILD)/,$(addsuffix .elf,$(USER_PROGS)))
EMBED_OBJS := $(addprefix $(EMBED)/,$(addsuffix .o,$(USER_PROGS)))
DYN_LOADER_ELF := $(UBUILD)/ld-furios.elf
DYN_LOADER_EMBED := $(EMBED)/ld-furios.o
USER_ELFS += $(DYN_LOADER_ELF)
EMBED_OBJS += $(DYN_LOADER_EMBED)
TCCRT_BINS := \
	$(TCCRT)/crt1.o \
	$(TCCRT)/crti.o \
	$(TCCRT)/crtn.o \
	$(TCCRT)/crtbegin.o \
	$(TCCRT)/crtend.o \
	$(TCCRT)/libc.a \
	$(TCCRT)/libtcc1.a
TCCRT_EMBED_OBJS := \
	$(EMBED)/tccrt_crt1.o \
	$(EMBED)/tccrt_crti.o \
	$(EMBED)/tccrt_crtn.o \
	$(EMBED)/tccrt_crtbegin.o \
	$(EMBED)/tccrt_crtend.o \
	$(EMBED)/tccrt_libc.o \
	$(EMBED)/tccrt_libtcc1.o
SDK_EMBED_OBJS := \
	$(EMBED)/tccdefs.o

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
	$(UBUILD)/io.o \
	$(UBUILD)/alloc.o \
	$(UBUILD)/posix.o \
	$(UBUILD)/stdio.o

all: $(BUILD)/kernel.elf

$(BUILD)/kernel.elf: $(KERNEL_OBJS) $(EMBED_OBJS) $(TCCRT_EMBED_OBJS) $(SDK_EMBED_OBJS) linker.ld | $(BUILD)
	$(LD) $(LDFLAGS_KERNEL) -o $@ $(KERNEL_OBJS) $(EMBED_OBJS) $(TCCRT_EMBED_OBJS) $(SDK_EMBED_OBJS)

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

# Dynamic loader is linked at a dedicated high user VA to avoid overlap
# when it maps target ET_EXEC images at 0x00400000.
$(UBUILD)/ld-furios.elf: $(UBUILD)/ld-furios.prog.o $(USER_LIB_OBJS) user/ld-furios.ld | $(UBUILD)
	$(LD) -nostdlib -static -Wl,--build-id=none -Wl,-T,user/ld-furios.ld -o $@ $< $(USER_LIB_OBJS)

# TinyCC is self-contained and links with its own libc shim.
$(UBUILD)/tcc.elf: $(UBUILD)/tcc.prog.o $(UBUILD)/crt0.o user/user.ld | $(UBUILD)
	$(LD) $(LDFLAGS_USER) -o $@ $(UBUILD)/tcc.prog.o $(UBUILD)/crt0.o -lgcc

$(TCCRT)/crt1.o: user/tccrt/crt1.S | $(TCCRT)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(TCCRT)/crti.o: user/tccrt/crti.S | $(TCCRT)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(TCCRT)/crtn.o: user/tccrt/crtn.S | $(TCCRT)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(TCCRT)/crtbegin.o: user/tccrt/crtbegin.S | $(TCCRT)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(TCCRT)/crtend.o: user/tccrt/crtend.S | $(TCCRT)
	$(CC) $(ASFLAGS) -c -o $@ $<

$(TCCRT)/libc.a: $(UBUILD)/syscall.o $(UBUILD)/string.o $(UBUILD)/io.o $(UBUILD)/alloc.o $(UBUILD)/posix.o $(UBUILD)/stdio.o | $(TCCRT)
	$(AR) rcs $@ $^

$(TCCRT)/libtcc1.a: | $(TCCRT)
	LIBGCC_FILE="$$( $(CC) -print-libgcc-file-name )"; \
	cp "$$LIBGCC_FILE" $@

$(EMBED)/%.o: $(UBUILD)/%.elf | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_crt1.o: $(TCCRT)/crt1.o | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_crti.o: $(TCCRT)/crti.o | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_crtn.o: $(TCCRT)/crtn.o | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_crtbegin.o: $(TCCRT)/crtbegin.o | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_crtend.o: $(TCCRT)/crtend.o | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_libc.o: $(TCCRT)/libc.a | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccrt_libtcc1.o: $(TCCRT)/libtcc1.a | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(EMBED)/tccdefs.o: third_party/tinycc/include/tccdefs.h | $(EMBED)
	$(OBJCOPY) -I binary -O elf64-littleaarch64 -B aarch64 $< $@

$(KBUILD) $(UBUILD) $(EMBED) $(TCCRT) $(BUILD):
	mkdir -p $@

userspace: $(USER_ELFS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean userspace

# Ensure kernel embeds latest userspace binaries.
$(BUILD)/kernel.elf: $(USER_ELFS) $(TCCRT_BINS)
$(EMBED_OBJS): $(USER_ELFS)
$(TCCRT_EMBED_OBJS): $(TCCRT_BINS)
$(SDK_EMBED_OBJS): third_party/tinycc/include/tccdefs.h

-include $(wildcard $(KBUILD)/*.d) $(wildcard $(UBUILD)/*.d)
