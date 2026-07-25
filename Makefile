# ArmOS kernel Makefile

ARMOS_VERSION := 0.7.4

# Local configuration is optional. Command-line assignments retain GNU make's
# highest priority; environment values are restored after the include so the
# repository-wide precedence matches the shell entry points.
ARMOS_CONFIG ?= armos.conf
ifeq ($(origin ARMOS_CONFIG),command line)
ifeq ($(wildcard $(ARMOS_CONFIG)),)
$(error Selected ArmOS configuration does not exist: $(ARMOS_CONFIG))
endif
endif
_ARMOS_ENV_TARGET_ARCH := $(if $(filter environment environment\ override,$(origin TARGET_ARCH)),$(TARGET_ARCH))
_ARMOS_ENV_TARGET_PLATFORM := $(if $(filter environment environment\ override,$(origin TARGET_PLATFORM)),$(TARGET_PLATFORM))
_ARMOS_ENV_CROSS_COMPILE := $(if $(filter environment environment\ override,$(origin CROSS_COMPILE)),$(CROSS_COMPILE))
_ARMOS_ENV_SMP_CPUS := $(if $(filter environment environment\ override,$(origin SMP_CPUS)),$(SMP_CPUS))
-include $(ARMOS_CONFIG)
ifneq ($(_ARMOS_ENV_TARGET_ARCH),)
TARGET_ARCH := $(_ARMOS_ENV_TARGET_ARCH)
endif
ifneq ($(_ARMOS_ENV_TARGET_PLATFORM),)
TARGET_PLATFORM := $(_ARMOS_ENV_TARGET_PLATFORM)
endif
ifneq ($(_ARMOS_ENV_CROSS_COMPILE),)
CROSS_COMPILE := $(_ARMOS_ENV_CROSS_COMPILE)
endif
ifneq ($(_ARMOS_ENV_SMP_CPUS),)
SMP_CPUS := $(_ARMOS_ENV_SMP_CPUS)
endif

TARGET_ARCH ?= arm32
TARGET_PLATFORM ?= qemu-virt

ifneq ($(wildcard $(ARMOS_CONFIG)),)
_ARMOS_CONFIG_STATUS := $(shell ARMOS_CONFIG_LOADED=0 \
	TARGET_ARCH='$(TARGET_ARCH)' TARGET_PLATFORM='$(TARGET_PLATFORM)' \
	ARMOS_CONFIG='$(ARMOS_CONFIG)' bash tools/armos_config.sh --show \
	>/dev/null 2>&1; echo $$?)
ifneq ($(_ARMOS_CONFIG_STATUS),0)
$(error Invalid ArmOS configuration; run ARMOS_CONFIG=$(ARMOS_CONFIG) ./tools/armos_config.sh --show)
endif
endif

ifeq ($(TARGET_ARCH),arm64)
CROSS_COMPILE ?= aarch64-elf-
else
CROSS_COMPILE ?= arm-none-eabi-
endif

CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump
QEMU ?= qemu-system-arm
SMP_CPUS ?= 1
BUILD_DIR = build
IMAGE_DIR ?= $(BUILD_DIR)/images
ARCH_DIR = arch/$(TARGET_ARCH)
ARCH_INCLUDE = $(ARCH_DIR)/include
ASM_OFFSETS_SRC = $(ARCH_DIR)/asm-offsets.c
ASM_OFFSETS_S = $(BUILD_DIR)/asm-offsets.s
ASM_OFFSETS_H = $(BUILD_DIR)/generated/asm-offsets.h
BUILD_CONFIG_STAMP = $(BUILD_DIR)/active-build-config.stamp
TARGET_ARCH_DISPLAY = $(TARGET_ARCH)/$(TARGET_PLATFORM)
TARGET_PLATFORM_DIR = $(subst -,_,$(TARGET_PLATFORM))
PLATFORM_DIR = $(ARCH_DIR)/platform/$(TARGET_PLATFORM_DIR)
PLATFORM_MK = $(PLATFORM_DIR)/platform.mk

ifeq ($(TARGET_ARCH),arm32)
ARCH_CFLAGS = -marm -mfpu=neon-vfpv4 -mfloat-abi=soft \
              -mno-unaligned-access -DARMV7A_KERNEL -DARMOS_ARCH_BITS=32
ASM_OFFSETS_DEPS = include/kernel/task.h \
                   $(ARCH_INCLUDE)/asm/task_context.h
else ifeq ($(TARGET_ARCH),arm64)
ARCH_CFLAGS = -march=armv8-a -mgeneral-regs-only \
              -DARMV8A_KERNEL -DARMOS_ARCH_BITS=64
ASM_OFFSETS_DEPS = $(ARCH_INCLUDE)/asm/user_context.h \
                   $(ARCH_INCLUDE)/asm/exception_frame.h \
                   $(ARCH_INCLUDE)/asm/task_context.h
else
$(error Unsupported TARGET_ARCH '$(TARGET_ARCH)')
endif

ifeq ($(wildcard $(PLATFORM_MK)),)
$(error Unsupported TARGET_PLATFORM '$(TARGET_PLATFORM)' for TARGET_ARCH '$(TARGET_ARCH)')
endif
include $(PLATFORM_MK)

# CORRECTION 2: Flags specifiques pour corriger les operations mathematiques
MATH_FLAGS = -fno-builtin-div -fno-builtin-mod
STACK_PROTECTOR_FLAG ?= -fstack-protector
LINKER_SCRIPT ?= linker.ld

# Flags de compilation
ASFLAGS = -g -I$(ARCH_INCLUDE) -Iinclude -I$(BUILD_DIR)/generated $(PLATFORM_ASFLAGS)

CFLAGS = -std=gnu99 $(ARCH_CFLAGS) $(PLATFORM_CFLAGS) $(MATH_FLAGS) \
         $(REPRO_FLAGS) \
         -DARMOS_VERSION=\"$(ARMOS_VERSION)\" \
         -ffreestanding -nostdlib -nostartfiles -fno-inline \
         -Wall -Wextra -Werror -g -O0 -fno-omit-frame-pointer -Wformat -Wformat-security \
         -fno-builtin $(STACK_PROTECTOR_FLAG) -Wno-unused-function \
         -MMD -MP \
         -fno-pic -fno-pie \
         -I$(ARCH_INCLUDE) \
         -Iinclude
# Flags du linker. Platform --defsym values must precede -T so linker.ld sees
# them while evaluating parametric addresses.
LDFLAGS = $(PLATFORM_LDFLAGS) -T $(LINKER_SCRIPT) -nostdlib -Map=kernel.map

COMMON_TASK_OBJS = \
	kernel/task/current.o \
	kernel/task/task.o \
	kernel/task/kernel_tasks.o \
	kernel/sync/spinlock.o

COMMON_LIB_OBJS = \
	kernel/lib/kprintf.o \
	kernel/lib/string.o \
	kernel/lib/fdt.o \
	kernel/lib/fdt_memory.o \
	kernel/lib/font_meslo_12x24.o \
	kernel/lib/font_meslo_10x20.o \
	kernel/lib/font_meslo_8x16.o \
	kernel/lib/font_spleen_8x16.o \
	kernel/lib/font_spleen_12x24.o \
	kernel/lib/font_vga_8x16.o \
	kernel/lib/divmod.o \
	kernel/lib/debug_print.o \
	kernel/lib/math.o

# Architecture-neutral kernel. Every target links this exact subsystem set.
COMMON_KERNEL_OBJS = \
	kernel/main.o \
	kernel/memory/physical.o \
	kernel/memory/kmalloc.o \
	kernel/memory/detect.o \
	kernel/memory/virtual.o \
	kernel/memory/usercopy.o \
	kernel/process/process.o \
	kernel/process/fork.o \
	kernel/process/exec.o \
	kernel/process/exec_stack.o \
	kernel/process/signal.o \
	kernel/core/coredump.o \
	kernel/core/device_service.o \
	kernel/net/device.o \
	kernel/net/stack.o \
	kernel/net/socket.o \
	kernel/net/armos_socket.o \
	kernel/net/control.o \
	kernel/net/wifi.o \
	kernel/fs/vfs.o \
	kernel/fs/mount.o \
	kernel/fs/fat32.o \
	kernel/fs/fat32_vfs.o \
	kernel/fs/disk_layout.o \
	kernel/fs/ext2_vfs.o \
	kernel/fs/procfs.o \
	kernel/drivers/console.o \
	kernel/drivers/block_device.o \
	kernel/drivers/uart.o \
	kernel/drivers/tty.o \
	kernel/drivers/null.o \
	kernel/drivers/power.o \
	kernel/drivers/usb/core.o \
	kernel/timer/timer.o \
	kernel/syscalls/syscalls.o \
	kernel/syscalls/thread.o \
	kernel/syscalls/file.o \
	kernel/syscalls/shm.o \
	kernel/syscalls/process_syscalls.o

ifeq ($(TARGET_ARCH),arm32)
ARCH_TASK_OBJS = \
	$(ARCH_DIR)/task/task_switch.o \
	$(ARCH_DIR)/task/context_debug.o \
	$(ARCH_DIR)/smp/smp.o
ARCH_KERNEL_OBJS = \
	$(ARCH_DIR)/boot/boot.o \
	$(ARCH_DIR)/cpu/cpu.o \
	$(ARCH_DIR)/mmu/helpers.o \
	$(ARCH_DIR)/mmu/virtual.o \
	$(ARCH_DIR)/mmu/mmu.o \
	$(ARCH_DIR)/mmu/debug.o \
	$(ARCH_DIR)/mmu/tlb.o \
	$(ARCH_DIR)/process/exec.o \
	$(ARCH_DIR)/interrupt/exception.o \
	$(ARCH_DIR)/interrupt/interrupt.o \
	$(ARCH_DIR)/interrupt/irq_return.o \
	$(ARCH_DIR)/syscall/syscall.o
else ifeq ($(TARGET_ARCH),arm64)
CFLAGS += -ffunction-sections -fdata-sections
LDFLAGS += --gc-sections
ARCH_TASK_OBJS = \
	$(ARCH_DIR)/task/task_context.o \
	$(ARCH_DIR)/task/context_switch.o \
	$(ARCH_DIR)/smp/smp.o
ARCH_KERNEL_OBJS = \
	$(ARCH_DIR)/boot/boot.o \
	$(ARCH_DIR)/interrupt/vectors.o \
	$(ARCH_DIR)/interrupt/exception.o \
	$(ARCH_DIR)/cpu/cpu.o \
	$(ARCH_DIR)/mmu/mmu.o \
	$(ARCH_DIR)/mmu/user_vm.o \
	$(ARCH_DIR)/mmu/virtual.o \
	$(ARCH_DIR)/mmu/tlb.o \
	$(ARCH_DIR)/timer/timer.o \
	$(ARCH_DIR)/process/exec.o \
	$(ARCH_DIR)/user/el0.o
endif

# The platform contributes devices; the architecture contributes mechanisms.
KERNEL_OBJS = $(COMMON_KERNEL_OBJS) $(ARCH_KERNEL_OBJS) $(PLATFORM_OBJS)
TASK_OBJS = $(COMMON_TASK_OBJS) $(ARCH_TASK_OBJS)
LIB_OBJ = $(COMMON_LIB_OBJS)
ALL_OBJS = $(KERNEL_OBJS) $(LIB_OBJ) $(TASK_OBJS)
DEPFILES = $(ALL_OBJS:.o=.d)

# Configuration du disque
DISK_IMG     = disk.img
FAT32_IMG    = fat32.img
EXT2_IMG     = ext2.img
IMAGE_SUFFIX ?= $(TARGET_ARCH)-$(TARGET_PLATFORM)
PLATFORM_KERNEL_ELF = $(IMAGE_DIR)/kernel-$(IMAGE_SUFFIX).elf
PLATFORM_KERNEL_BIN = $(IMAGE_DIR)/kernel-$(IMAGE_SUFFIX).bin
PLATFORM_KERNEL_MAP = $(IMAGE_DIR)/kernel-$(IMAGE_SUFFIX).map
PLATFORM_KERNEL_DIS = $(IMAGE_DIR)/kernel-$(IMAGE_SUFFIX).dis
PLATFORM_DISK_IMG   = $(IMAGE_DIR)/disk-$(IMAGE_SUFFIX).img
ARMOS_REPRODUCIBLE_ROOT ?= /usr/src/armos
REPRO_FLAGS = -ffile-prefix-map=$(CURDIR)=$(ARMOS_REPRODUCIBLE_ROOT) \
              -fmacro-prefix-map=$(CURDIR)=$(ARMOS_REPRODUCIBLE_ROOT) \
              -fdebug-prefix-map=$(CURDIR)=$(ARMOS_REPRODUCIBLE_ROOT)
FAT32_SIZE_MB = 64
FAT32_LABEL ?= ARMBOOT
EXT2_SIZE_MB = 512
DISK_RESERVED_MB = 1
DISK_SIZE_MB = $(shell echo $$(($(DISK_RESERVED_MB) + $(EXT2_SIZE_MB) + $(FAT32_SIZE_MB))))
PLATFORM_DISK_SIZE_MB ?= $(DISK_SIZE_MB)
DISK_MB_SECTORS = 2048
DISK_EXT2_START_MB = $(DISK_RESERVED_MB)
DISK_FAT32_START_MB = $(shell echo $$(($(DISK_EXT2_START_MB) + $(EXT2_SIZE_MB))))
DISK_FAT32_FIRST_FAT32_START_MB = $(DISK_RESERVED_MB)
DISK_FAT32_FIRST_EXT2_START_MB = $(shell echo $$(($(DISK_FAT32_FIRST_FAT32_START_MB) + $(FAT32_SIZE_MB))))
DISK_EXT2_START_SECTOR = $(shell echo $$(($(DISK_EXT2_START_MB) * $(DISK_MB_SECTORS))))
DISK_FAT32_START_SECTOR = $(shell echo $$(($(DISK_FAT32_START_MB) * $(DISK_MB_SECTORS))))
DISK_FAT32_FIRST_FAT32_START_SECTOR = $(shell echo $$(($(DISK_FAT32_FIRST_FAT32_START_MB) * $(DISK_MB_SECTORS))))
DISK_FAT32_FIRST_EXT2_START_SECTOR = $(shell echo $$(($(DISK_FAT32_FIRST_EXT2_START_MB) * $(DISK_MB_SECTORS))))
DISK_EXT2_SECTORS = $(shell echo $$(($(EXT2_SIZE_MB) * $(DISK_MB_SECTORS))))
DISK_FAT32_SECTORS = $(shell echo $$(($(FAT32_SIZE_MB) * $(DISK_MB_SECTORS))))
PLATFORM_DISK_LAYOUT ?= ext2-first
PLATFORM_DISK_HIDDEN_BOOT ?= 0
HIDDEN_FAT32_FLAG = $(if $(filter 1 yes true,$(PLATFORM_DISK_HIDDEN_BOOT)),--hidden-fat32,)
USERFS_DIR   = userfs
USERFS_OS_CONF = $(USERFS_DIR)/etc/os.conf
USERLAND_DIR = userland
EXT2_STAGING = /tmp/ext2_staging
PYTHON ?= python3
MBR_TOOL = tools/make_mbr.py
USERFS_FILES := $(shell find $(USERFS_DIR) -type f 2>/dev/null)
USERFS_DIRS  := $(shell find $(USERFS_DIR) -type d 2>/dev/null)
USERFS_LINKS := $(shell find $(USERFS_DIR) -type l 2>/dev/null)
USERFS_BIN_FILES := $(shell find $(USERFS_DIR)/bin -type f 2>/dev/null)

# Cibles
TARGET = kernel
KERNEL_ELF = $(TARGET).elf
KERNEL_BIN = $(TARGET).bin

.PHONY: FORCE config platform-kernel platform-disk

all: platform-kernel platform-disk

config:
	@echo "ArmOS make configuration"
	@echo "  file:            $(if $(wildcard $(ARMOS_CONFIG)),$(ARMOS_CONFIG),defaults and environment)"
	@echo "  TARGET_ARCH:     $(TARGET_ARCH)"
	@echo "  TARGET_PLATFORM: $(TARGET_PLATFORM)"
	@echo "  CROSS_COMPILE:   $(CROSS_COMPILE)"
	@echo "  SMP_CPUS:        $(SMP_CPUS)"
	@echo "  ENABLE_NET:      $(if $(ENABLE_NET),$(ENABLE_NET),no)"
	@echo "  ENABLE_WIFI:     $(if $(ENABLE_WIFI),$(ENABLE_WIFI),no)"
	@echo "  ENABLE_GPU:      $(if $(ENABLE_GPU),$(ENABLE_GPU),no)"
	@echo "  ENABLE_HDMI:     $(if $(ENABLE_HDMI),$(ENABLE_HDMI),platform default)"
	@echo "  HDMI_WIDTH:      $(HDMI_WIDTH)"
	@echo "  HDMI_HEIGHT:     $(HDMI_HEIGHT)"
	@echo "  ENABLE_ILI9341:  $(if $(ENABLE_ILI9341),$(ENABLE_ILI9341),platform default)"
	@echo "  ENABLE_USB:      $(if $(ENABLE_USB),$(ENABLE_USB),platform default)"

# Linkage
$(KERNEL_ELF): $(ALL_OBJS) $(LINKER_SCRIPT)
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@

# Conversion en binaire
$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	$(OBJDUMP) -d $(KERNEL_ELF) > kernel.dis

$(PLATFORM_KERNEL_BIN): $(KERNEL_BIN)
	@mkdir -p $(IMAGE_DIR)
	cp $(KERNEL_ELF) $(PLATFORM_KERNEL_ELF)
	cp $(KERNEL_BIN) $(PLATFORM_KERNEL_BIN)
	cp kernel.map $(PLATFORM_KERNEL_MAP)
	cp kernel.dis $(PLATFORM_KERNEL_DIS)
	@echo "Platform kernel image: $(PLATFORM_KERNEL_BIN)"

platform-kernel: $(PLATFORM_KERNEL_BIN)

$(BUILD_CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@tmp="$@.tmp"; \
	{ \
		echo "TARGET_ARCH=$(TARGET_ARCH)"; \
		echo "TARGET_PLATFORM=$(TARGET_PLATFORM)"; \
		echo "CROSS_COMPILE=$(CROSS_COMPILE)"; \
		echo "CFLAGS=$(CFLAGS)"; \
		echo "LDFLAGS=$(LDFLAGS)"; \
	} > "$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then \
		mv "$$tmp" "$@"; \
	else \
		rm -f "$$tmp"; \
	fi

$(ASM_OFFSETS_H): $(ASM_OFFSETS_SRC) $(ASM_OFFSETS_DEPS) $(BUILD_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR) $(dir $@)
	$(CC) $(CFLAGS) -S $(ASM_OFFSETS_SRC) -o $(ASM_OFFSETS_S)
	@awk '/->/ { \
		line=$$0; \
		sub(/^.*->/, "", line); \
		gsub(/"/, "", line); \
		split(line, f, " "); \
		value=f[2]; \
		sub(/^#/, "", value); \
		printf ".equ %-24s %s\n", f[1] ",", value; \
	}' $(ASM_OFFSETS_S) > $@

%.o: %.c $(BUILD_CONFIG_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(ASM_OFFSETS_H) $(BUILD_CONFIG_STAMP)
	$(AS) $(ASFLAGS) $< -o $@

# Partition FAT32 de boot. Elle reste volontairement vide au build generique :
# les firmwares Raspberry Pi, le kernel et config.txt sont stages ensuite par
# tools/build_pi2_sd.sh. Le root systeme complet vit dans ext2.
$(FAT32_IMG): Makefile
	@echo "=== Creating FAT32 image ($(FAT32_SIZE_MB) MB) ==="
	dd if=/dev/zero of=$(FAT32_IMG) bs=1048576 count=$(FAT32_SIZE_MB) 2>/dev/null
	mkfs.fat -F 32 -n "$(FAT32_LABEL)" $(FAT32_IMG)
	@echo "FAT32 image created"

# Partition ext2 — peuplee avec tout userfs via mke2fs + debugfs.
E2FSPROGS_PREFIX ?= $(shell \
	if command -v brew >/dev/null 2>&1; then \
		brew --prefix e2fsprogs 2>/dev/null; \
	elif [ -d /opt/homebrew/opt/e2fsprogs ]; then \
		echo /opt/homebrew/opt/e2fsprogs; \
	elif [ -d /usr/local/opt/e2fsprogs ]; then \
		echo /usr/local/opt/e2fsprogs; \
	fi)
MKE2FS ?= $(shell command -v mke2fs 2>/dev/null)
DEBUGFS ?= $(shell command -v debugfs 2>/dev/null)
ifeq ($(strip $(MKE2FS)),)
MKE2FS := $(if $(E2FSPROGS_PREFIX),$(E2FSPROGS_PREFIX)/sbin/mke2fs,mke2fs)
endif
ifeq ($(strip $(DEBUGFS)),)
DEBUGFS := $(if $(E2FSPROGS_PREFIX),$(E2FSPROGS_PREFIX)/sbin/debugfs,debugfs)
endif

$(EXT2_IMG): Makefile $(USERFS_DIR) $(USERFS_OS_CONF) $(USERFS_FILES) $(USERFS_DIRS) $(USERFS_LINKS)
	@echo "=== Creating ext2 image ($(EXT2_SIZE_MB) MB) ==="
	@if ! command -v "$(MKE2FS)" >/dev/null 2>&1 || \
	    ! command -v "$(DEBUGFS)" >/dev/null 2>&1; then \
		echo "Error: e2fsprogs tools mke2fs/debugfs not found"; \
		echo "Linux: install the e2fsprogs package (for example: sudo apt install e2fsprogs)"; \
		echo "macOS: run brew install e2fsprogs"; \
		echo "MKE2FS=$(MKE2FS)"; \
		echo "DEBUGFS=$(DEBUGFS)"; \
		exit 1; \
	fi
	dd if=/dev/zero of=$(EXT2_IMG) bs=1048576 count=$(EXT2_SIZE_MB) 2>/dev/null
	$(MKE2FS) -q -t ext2 -F -L OS_EXT2 $(EXT2_IMG)
	@( find $(USERFS_DIR) \( \
	       -path "$(USERFS_DIR)/legacy" -o \
	       -name ".DS_Store" -o -name "._*" -o \
	       -name ".Spotlight-V100" -o -name ".Trashes" \
	   \) -prune -o -type d -print | sort | while read dir; do \
	       if [ "$$dir" != "$(USERFS_DIR)" ]; then \
	           relpath=$$(echo "$$dir" | sed 's|$(USERFS_DIR)/||'); \
	           printf 'mkdir /%s\n' "$$relpath"; \
	           case "$$relpath" in \
	               tmp) mode=040777; uid=0; gid=0 ;; \
	               dev) mode=040755; uid=0; gid=0 ;; \
	               root|root/*) mode=040700; uid=0; gid=0 ;; \
	               home/user|home/user/*) mode=040755; uid=1000; gid=1000 ;; \
	               *) mode=040755; uid=0; gid=0 ;; \
	           esac; \
	           printf 'set_inode_field /%s mode %s\n' "$$relpath" "$$mode"; \
	           printf 'set_inode_field /%s uid %s\n' "$$relpath" "$$uid"; \
	           printf 'set_inode_field /%s gid %s\n' "$$relpath" "$$gid"; \
	       fi; \
	   done; \
	   find $(USERFS_DIR) \( \
	       -path "$(USERFS_DIR)/legacy" -o \
	       -name ".DS_Store" -o -name "._*" -o \
	       -name ".Spotlight-V100" -o -name ".Trashes" \
	   \) -prune -o -type f -print | sort | while read f; do \
	       relpath=$$(echo "$$f" | sed 's|$(USERFS_DIR)/||'); \
	       case "$$relpath" in dev/tty0|dev/tty1|dev/console|dev/fb0|dev/fb1) continue ;; esac; \
	       printf 'write %s /%s\n' "$$f" "$$relpath"; \
	       case "$$relpath" in \
	           sbin/init) mode=0100700 ;; \
	           bin/su) mode=0104755 ;; \
	           bin/*|sbin/*|usr/bin/*|opt/*/bin/*|init.sh) mode=0100755 ;; \
	           home/user/copy_renamed) mode=0100755 ;; \
	           *) mode=0100644 ;; \
	       esac; \
	       case "$$relpath" in \
	           home/user/*) uid=1000; gid=1000 ;; \
	           *) uid=0; gid=0 ;; \
	       esac; \
	       printf 'set_inode_field /%s mode %s\n' "$$relpath" "$$mode"; \
	       printf 'set_inode_field /%s uid %s\n' "$$relpath" "$$uid"; \
	       printf 'set_inode_field /%s gid %s\n' "$$relpath" "$$gid"; \
	   done; \
	   find $(USERFS_DIR) \( \
	       -path "$(USERFS_DIR)/legacy" -o \
	       -name ".DS_Store" -o -name "._*" -o \
	       -name ".Spotlight-V100" -o -name ".Trashes" \
	   \) -prune -o -type l -print | sort | while read l; do \
	       relpath=$$(echo "$$l" | sed 's|$(USERFS_DIR)/||'); \
	       target=$$(readlink "$$l"); \
	       printf 'symlink /%s %s\n' "$$relpath" "$$target"; \
	   done; \
	   printf 'cd /dev\n'; \
	   printf 'mknod console c 5 1\n'; \
	   printf 'set_inode_field console mode 020666\n'; \
	   printf 'set_inode_field console uid 0\n'; \
	   printf 'set_inode_field console gid 0\n'; \
	   printf 'mknod tty0 c 4 0\n'; \
	   printf 'set_inode_field tty0 mode 020666\n'; \
	   printf 'set_inode_field tty0 uid 0\n'; \
	   printf 'set_inode_field tty0 gid 0\n'; \
	   printf 'mknod tty1 c 4 1\n'; \
	   printf 'set_inode_field tty1 mode 020666\n'; \
	   printf 'set_inode_field tty1 uid 0\n'; \
	   printf 'set_inode_field tty1 gid 0\n'; \
	   printf 'mknod null c 1 3\n'; \
	   printf 'set_inode_field null mode 020666\n'; \
	   printf 'set_inode_field null uid 0\n'; \
	   printf 'set_inode_field null gid 0\n'; \
	   printf 'mknod fb0 c 29 0\n'; \
	   printf 'set_inode_field fb0 mode 020666\n'; \
	   printf 'set_inode_field fb0 uid 0\n'; \
	   printf 'set_inode_field fb0 gid 0\n'; \
	   printf 'mknod fb1 c 29 1\n'; \
	   printf 'set_inode_field fb1 mode 020666\n'; \
	   printf 'set_inode_field fb1 uid 0\n'; \
	   printf 'set_inode_field fb1 gid 0\n'; \
	   printf 'quit\n' ) | $(DEBUGFS) -w -f - $(EXT2_IMG) >/dev/null
	$(DEBUGFS) -R 'ls -l /bin' $(EXT2_IMG) >/dev/null
	@echo "ext2 image created"

# Disque final = MBR + partitions alignees. La premiere partition commence a
# 1 MiB (LBA 2048), comme sur un disque Linux classique.
$(DISK_IMG): $(FAT32_IMG) $(EXT2_IMG) $(MBR_TOOL)
	@echo "=== Assembling $(DISK_IMG) (MBR + ext2 + FAT32) ==="
	dd if=/dev/zero of=$(DISK_IMG) bs=1048576 count=$(DISK_SIZE_MB) 2>/dev/null
	$(PYTHON) $(MBR_TOOL) $(DISK_IMG) \
		$(DISK_EXT2_START_SECTOR) $(DISK_EXT2_SECTORS) \
		$(DISK_FAT32_START_SECTOR) $(DISK_FAT32_SECTORS) \
		$(HIDDEN_FAT32_FLAG)
	dd if=$(EXT2_IMG) of=$(DISK_IMG) bs=1048576 seek=$(DISK_EXT2_START_MB) conv=notrunc 2>/dev/null
	dd if=$(FAT32_IMG) of=$(DISK_IMG) bs=1048576 seek=$(DISK_FAT32_START_MB) conv=notrunc 2>/dev/null
	@echo "Disk image $(DISK_IMG) created ($(DISK_SIZE_MB) MB)"

ifeq ($(TARGET_ARCH),arm64)
$(PLATFORM_DISK_IMG): FORCE tools/build_arm64_disk.sh \
		tools/build_arm64_userland.sh userland/Makefile Makefile
	TARGET_PLATFORM="$(TARGET_PLATFORM)" \
	PLATFORM_DISK_LAYOUT="$(PLATFORM_DISK_LAYOUT)" \
	PLATFORM_DISK_HIDDEN_BOOT="$(PLATFORM_DISK_HIDDEN_BOOT)" \
	PLATFORM_DISK_SIZE_MB="$(PLATFORM_DISK_SIZE_MB)" \
	./tools/build_arm64_disk.sh --output $@
else ifeq ($(PLATFORM_DISK_LAYOUT),fat32-first)
$(PLATFORM_DISK_IMG): $(FAT32_IMG) $(EXT2_IMG) $(MBR_TOOL) Makefile $(PLATFORM_MK)
	@mkdir -p $(IMAGE_DIR)
	@echo "=== Assembling $(PLATFORM_DISK_IMG) (MBR + FAT32 boot + ext2 root) ==="
	dd if=/dev/zero of=$(PLATFORM_DISK_IMG) bs=1048576 count=$(DISK_SIZE_MB) 2>/dev/null
	$(PYTHON) $(MBR_TOOL) $(PLATFORM_DISK_IMG) \
		$(DISK_FAT32_FIRST_EXT2_START_SECTOR) $(DISK_EXT2_SECTORS) \
		$(DISK_FAT32_FIRST_FAT32_START_SECTOR) $(DISK_FAT32_SECTORS) \
		--fat32-first $(HIDDEN_FAT32_FLAG)
	dd if=$(FAT32_IMG) of=$(PLATFORM_DISK_IMG) bs=1048576 seek=$(DISK_FAT32_FIRST_FAT32_START_MB) conv=notrunc 2>/dev/null
	dd if=$(EXT2_IMG) of=$(PLATFORM_DISK_IMG) bs=1048576 seek=$(DISK_FAT32_FIRST_EXT2_START_MB) conv=notrunc 2>/dev/null
	@if [ "$(PLATFORM_DISK_SIZE_MB)" -lt "$(DISK_SIZE_MB)" ]; then \
		echo "Error: PLATFORM_DISK_SIZE_MB ($(PLATFORM_DISK_SIZE_MB)) is smaller than disk layout ($(DISK_SIZE_MB))"; \
		exit 1; \
	fi
	@if [ "$(PLATFORM_DISK_SIZE_MB)" != "$(DISK_SIZE_MB)" ]; then \
		dd if=/dev/zero of=$(PLATFORM_DISK_IMG) bs=1048576 seek=$$(($(PLATFORM_DISK_SIZE_MB) - 1)) count=1 conv=notrunc 2>/dev/null; \
		echo "Platform disk image padded to $(PLATFORM_DISK_SIZE_MB) MB"; \
	fi
	@echo "Platform disk image: $(PLATFORM_DISK_IMG)"
else
$(PLATFORM_DISK_IMG): $(DISK_IMG) Makefile $(PLATFORM_MK)
	@mkdir -p $(IMAGE_DIR)
	cp $(DISK_IMG) $(PLATFORM_DISK_IMG)
	@if [ "$(PLATFORM_DISK_SIZE_MB)" -lt "$(DISK_SIZE_MB)" ]; then \
		echo "Error: PLATFORM_DISK_SIZE_MB ($(PLATFORM_DISK_SIZE_MB)) is smaller than disk layout ($(DISK_SIZE_MB))"; \
		exit 1; \
	fi
	@if [ "$(PLATFORM_DISK_SIZE_MB)" != "$(DISK_SIZE_MB)" ]; then \
		dd if=/dev/zero of=$(PLATFORM_DISK_IMG) bs=1048576 seek=$$(($(PLATFORM_DISK_SIZE_MB) - 1)) count=1 conv=notrunc 2>/dev/null; \
		echo "Platform disk image padded to $(PLATFORM_DISK_SIZE_MB) MB"; \
	fi
	@echo "Platform disk image: $(PLATFORM_DISK_IMG)"
endif

platform-disk: $(PLATFORM_DISK_IMG)

# Creer le repertoire userfs avec des fichiers de test
$(USERFS_DIR):
	@echo "Creating $(USERFS_DIR) directory with test files..."
	mkdir -p $(USERFS_DIR)
	echo "Hello from ArmOS ($(TARGET_ARCH_DISPLAY))!" > $(USERFS_DIR)/hello.txt
	echo "This is a test file for the kernel" > $(USERFS_DIR)/test.txt
	echo "ArmOS kernel ($(TARGET_ARCH_DISPLAY))" > $(USERFS_DIR)/readme.txt
	echo "#!/bin/sh" > $(USERFS_DIR)/init.sh
	echo "echo 'System initialization...'" >> $(USERFS_DIR)/init.sh
	echo "echo 'Welcome to ArmOS ($(TARGET_ARCH_DISPLAY))'" >> $(USERFS_DIR)/init.sh
	chmod +x $(USERFS_DIR)/init.sh
	
	# Creer des sous-repertoires
	mkdir -p $(USERFS_DIR)/bin $(USERFS_DIR)/usr/bin
	echo "echo 'Test program running'" > $(USERFS_DIR)/bin/test.sh
	chmod +x $(USERFS_DIR)/bin/test.sh
	echo "Binary placeholder" > $(USERFS_DIR)/usr/bin/hello
	
	mkdir -p $(USERFS_DIR)/tmp
	echo "Temporary files directory" > $(USERFS_DIR)/tmp/README
	
	mkdir -p $(USERFS_DIR)/dev
	touch $(USERFS_DIR)/dev/tty0 $(USERFS_DIR)/dev/tty1 $(USERFS_DIR)/dev/console $(USERFS_DIR)/dev/fb0 $(USERFS_DIR)/dev/fb1
	
	@echo "$(USERFS_DIR) directory created with test files"

$(USERFS_OS_CONF): $(BUILD_CONFIG_STAMP) | $(USERFS_DIR)
	mkdir -p $(dir $@)
	@printf '# ArmOS Configuration\nversion=%s\narchitecture=%s\n' \
		'$(ARMOS_VERSION)' '$(TARGET_ARCH)' > $@

# Generer les donnees userfs
userfs-data: userfs.bin

userfs.bin: $(wildcard userfs/**/*)
	./populate_userfs.sh
	./qemu_loader_method.sh

# Run avec userfs loader
run-userfs: $(KERNEL_BIN)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(QEMU_CPU) \
		-m 2G -smp $(SMP_CPUS) \
		$(QEMU_BOOT_DRIVE) \
		$(QEMU_BOOT_DEVICE) \
		-kernel $(KERNEL_BIN) \
		-nographic

#-device loader,file=userfs.bin,addr=0x50000000
#,virtualization=off,gic-version=2
#-device loader,file=userfs.bin,addr=0x41000000

debug-run-userfs: $(KERNEL_BIN) userfs.bin
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(QEMU_CPU) \
		-m 2G -smp $(SMP_CPUS) \
		$(QEMU_BOOT_DRIVE) \
		$(QEMU_BOOT_DEVICE) \
		-kernel $(KERNEL_BIN) \
		-nographic \
		-device loader,file=userfs.bin,addr=0x50000000 -s -S

# Version alternative plus simple (un seul niveau de fichiers)
disk-simple: $(USERFS_DIR)
	@echo "Creating simple disk image..."
	dd if=/dev/zero of=$(DISK_IMG) bs=1048576 count=$(DISK_SIZE_MB) 2>/dev/null
	mkfs.fat -F 32 -n "OSKERNEL" $(DISK_IMG)
	
	# Copier seulement les fichiers du niveau racine
	@find $(USERFS_DIR) -maxdepth 1 -type f | while read file; do \
		basename_file=$$(basename "$$file"); \
		echo "Copying $$basename_file..."; \
		mcopy -i $(DISK_IMG) "$$file" "::$$basename_file"; \
	done
	@echo "Simple disk created"

# Verifier le contenu du disque
check-disk: $(DISK_IMG)
	@echo "=== ext2 root contents (mounted as /) ==="
	@if [ -x "$(DEBUGFS)" ]; then \
		$(DEBUGFS) -R 'ls -l /' $(EXT2_IMG); \
	else \
		echo "Could not list ext2 root: debugfs not found"; \
	fi
	@echo ""
	@echo "=== ext2 /bin contents ==="
	@if [ -x "$(DEBUGFS)" ]; then \
		$(DEBUGFS) -R 'ls -l /bin' $(EXT2_IMG); \
	else \
		echo "Could not list ext2 /bin: debugfs not found"; \
	fi
	@echo ""
	@echo "=== FAT32 boot contents ==="
	@mdir -i $(FAT32_IMG) :: 2>/dev/null || echo "Could not list FAT32 /mnt directory"
	@echo ""
	@echo "=== Disk image info ==="
	@file $(FAT32_IMG)
	@file $(EXT2_IMG)
	@file $(DISK_IMG)
	@echo ""
	@echo "=== Disk size ==="
	@ls -lh $(FAT32_IMG) $(EXT2_IMG)
	@ls -lh $(DISK_IMG)

# Extraire le contenu du disque (pour debug)
extract-disk: $(DISK_IMG)
	@echo "Extracting ext2 / and FAT32 boot to disk_contents/..."
	rm -rf disk_contents
	mkdir -p disk_contents/ext2-root disk_contents/fat32-boot
	@if [ -x "$(DEBUGFS)" ]; then \
		$(DEBUGFS) -R 'rdump / disk_contents/ext2-root' $(EXT2_IMG) >/dev/null 2>/dev/null || \
			echo "Could not extract ext2 root"; \
	else \
		echo "Could not extract ext2 root: debugfs not found"; \
	fi
	@mcopy -s -i $(FAT32_IMG) :: disk_contents/fat32-boot/ 2>/dev/null || \
		echo "Could not extract FAT32 boot contents"
	@echo "Extracted to disk_contents/ext2-root and disk_contents/fat32-boot"

# Creer un disque de boot avec le kernel (optionnel)
boot-disk: $(KERNEL_BIN) $(USERFS_DIR)
	@echo "Creating bootable disk with kernel..."
	dd if=/dev/zero of=boot_$(DISK_IMG) bs=1048576 count=$(DISK_SIZE_MB) 2>/dev/null
	mkfs.fat -F 32 -n "ARMBOOT" boot_$(DISK_IMG)
	mcopy -i boot_$(DISK_IMG) $(KERNEL_BIN) ::$(KERNEL_BIN)
	@if [ -d $(USERFS_DIR) ]; then \
		find $(USERFS_DIR) -type f | while read file; do \
			basename_file=$$(basename "$$file"); \
			mcopy -i boot_$(DISK_IMG) "$$file" "::$$basename_file"; \
		done; \
	fi
	@echo "Bootable disk created as boot_$(DISK_IMG)"

clean:
	rm -f $(ALL_OBJS) $(DEPFILES) $(KERNEL_ELF) $(KERNEL_BIN) $(ASM_OFFSETS_S) $(ASM_OFFSETS_H)

clean-all: clean
	rm -f $(DISK_IMG) boot_$(DISK_IMG)
	rm -rf $(USERFS_DIR) disk_contents

# Essayez cette configuration alternative :
run-alt: $(KERNEL_BIN) $(DISK_IMG)
	$(QEMU) -M vexpress-a9 -cpu cortex-a9 \
		-m 1G \
		-kernel $(KERNEL_BIN) \
		-nographic \
		-blockdev driver=file,filename=$(DISK_IMG),node-name=disk0 \
		-device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0

run-trace: $(KERNEL_BIN) $(DISK_IMG)
	$(QEMU) -M vexpress-a9 -cpu cortex-a9 \
		-m 1G \
		-kernel $(KERNEL_BIN) \
		-nographic \
		-drive file=$(DISK_IMG),format=raw,if=none,id=disk0 \
		-device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0 \
		-trace "virtio*" -trace "virtqueue*" -trace "virtio_blk*" \
		-D qemu-virtio.log 

run-mmio: $(KERNEL_BIN) $(DISK_IMG)
	$(QEMU) -M $(QEMU_MACHINE) -cpu $(QEMU_CPU) \
		-m 1G -smp 1 \
		-kernel $(KERNEL_BIN) \
		-nographic \
		$(QEMU_MMIO_DRIVE) \
		$(QEMU_MMIO_DEVICE) \
		-d int,guest_errors,unimp -D qemu.log

run: $(KERNEL_BIN) $(DISK_IMG)
	$(QEMU) -M $(QEMU_RUN_MACHINE) -cpu $(QEMU_CPU) \
		-m 1G -smp 1 \
		-kernel $(KERNEL_BIN) \
		-nographic \
		$(QEMU_SIMPLE_DRIVE)

#-global virtio-mmio.force-legacy=true

debug: $(KERNEL_BIN) $(DISK_IMG)
	$(QEMU) -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -m 128M \
		-kernel $(KERNEL_BIN) -nographic -s -S \
		$(QEMU_MMIO_DRIVE) \
		$(QEMU_DEBUG_DEVICE)
#,bus=virtio-mmio-bus.0

# Commandes d'aide
help:
	@echo "Available targets:"
	@echo "  all          - Build kernel and create disk"
	@echo "  $(KERNEL_BIN)   - Build kernel only"
	@echo "  $(DISK_IMG)       - Create disk with userfs content"
	@echo "  disk-simple  - Create simple disk (files only)"
	@echo "  check-disk   - Display disk contents and info"
	@echo "  extract-disk - Extract disk contents for inspection"
	@echo "  boot-disk    - Create bootable disk with kernel"
	@echo "  run          - Run kernel in QEMU"
	@echo "  debug        - Run kernel in QEMU with debugging"
	@echo "  clean        - Remove object files and binaries"
	@echo "  clean-all    - Remove everything including disk"
	@echo "  help         - Show this help"

# Debug targets
test-kernel: $(KERNEL_BIN)
	@echo "Testing kernel without disk..."
	$(QEMU) \
		-M vexpress-a9 -cpu cortex-a9 \
		-m 1G \
		-kernel $(KERNEL_BIN) \
		-nographic

debug-verbose: $(KERNEL_BIN)
	@echo "Running with verbose debug..."
	$(QEMU) -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -m 128M \
		-kernel $(KERNEL_BIN) -nographic \
		-d cpu,int,guest_errors

debug-monitor: $(KERNEL_BIN)
	@echo "Running with QEMU monitor (type 'info registers' then 'quit')..."
	$(QEMU) -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -m 128M \
		-kernel $(KERNEL_BIN) -nographic \
		-monitor stdio

debug-trace: $(KERNEL_BIN)
	@echo "Running with execution trace..."
	$(QEMU) -machine $(QEMU_MACHINE) -cpu $(QEMU_CPU) -m 128M \
		-kernel $(KERNEL_BIN) -nographic \
		-d exec,cpu -D qemu.log
	@echo "Check qemu.log for execution trace"

info: $(KERNEL_ELF)
	@echo "=== Kernel Info ==="
	file $(KERNEL_ELF) $(KERNEL_BIN)
	@echo ""
	@echo "=== Entry Point ==="
	$(CROSS_COMPILE)readelf -h $(KERNEL_ELF) | grep Entry
	@echo ""
	@echo "=== Sections ==="
	$(CROSS_COMPILE)readelf -S $(KERNEL_ELF) | head -20
	@echo ""
	@echo "=== First bytes of binary ==="
	hexdump -C $(KERNEL_BIN) | head -5

disasm: $(KERNEL_ELF)
	@echo "=== Disassembly of entry point ==="
	$(CROSS_COMPILE)objdump -d $(KERNEL_ELF) | head -50

symbols: $(KERNEL_ELF)
	@echo "=== Important symbols ==="
	$(CROSS_COMPILE)nm $(KERNEL_ELF) | grep -E "(_start|kernel_main|early_init|uart)"

# Test avec machine differente
test-versatile: $(KERNEL_BIN)
	@echo "Testing with versatile machine..."
	$(QEMU) -machine versatilepb -cpu arm1176 -m 256M \
		-kernel $(KERNEL_BIN) -nographic

# Test simple d'affichage
test-simple: 
	@echo "Creating minimal test kernel..."
	@echo '.global _start' > test_simple.s
	@echo '_start:' >> test_simple.s
	@echo '  ldr r0, =0x09000000' >> test_simple.s
	@echo '  mov r1, #72' >> test_simple.s
	@echo '  str r1, [r0]' >> test_simple.s
	@echo '  mov r1, #105' >> test_simple.s
	@echo '  str r1, [r0]' >> test_simple.s
	@echo '  mov r1, #10' >> test_simple.s
	@echo '  str r1, [r0]' >> test_simple.s
	@echo 'loop: b loop' >> test_simple.s
	$(AS) $(ASFLAGS) test_simple.s -o test_simple.o
	$(LD) test_simple.o -Ttext=0x40000000 -o test_simple.elf
	$(OBJCOPY) -O binary test_simple.elf test_simple.bin
	@echo "Running simple test (should print 'Hi')..."
	$(QEMU) -machine virt -cpu cortex-a15 -m 128M \
		-kernel test_simple.bin -nographic
	@rm -f test_simple.s test_simple.o test_simple.elf test_simple.bin

# Mise a jour de la cible .PHONY
.PHONY: all clean clean-all run debug check-disk disk-simple extract-disk boot-disk help test-kernel debug-verbose debug-monitor debug-trace info disasm symbols test-versatile test-simple

-include $(DEPFILES)
