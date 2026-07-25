ARCH ?= arm-none-eabi-
TARGET_ARCH ?= arm32
CC := $(ARCH)gcc
AS := $(ARCH)as
ARMOS_SOURCE_ROOT := $(abspath ..)
ARMOS_REPRODUCIBLE_ROOT ?= /usr/src/armos
REPRO_FLAGS := -ffile-prefix-map=$(ARMOS_SOURCE_ROOT)=$(ARMOS_REPRODUCIBLE_ROOT) \
               -fmacro-prefix-map=$(ARMOS_SOURCE_ROOT)=$(ARMOS_REPRODUCIBLE_ROOT) \
               -fdebug-prefix-map=$(ARMOS_SOURCE_ROOT)=$(ARMOS_REPRODUCIBLE_ROOT)

ifeq ($(TARGET_ARCH),arm64)
ARCH_FLAGS := -mcpu=cortex-a53
ASFLAGS := -mcpu=cortex-a53
CRT0_SOURCE := arm64/crt0_newlib.S
SYSCALL_SOURCE := arm64/syscall_raw.S
BUILD_DIR := build/arm64
else
ARCH_FLAGS := -mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft
ASFLAGS := -mcpu=cortex-a15
CRT0_SOURCE := crt0_newlib.S
SYSCALL_SOURCE := syscall_raw.S
BUILD_DIR := build
endif

NEWLIB_SYSROOT ?= $(shell $(CC) -print-sysroot 2>/dev/null)
NEWLIB_INCLUDE ?= $(NEWLIB_SYSROOT)/include
NEWLIB_LIBC ?= $(shell $(CC) $(ARCH_FLAGS) --sysroot=$(NEWLIB_SYSROOT) -print-file-name=libc.a 2>/dev/null)

CFLAGS := $(ARCH_FLAGS) -std=gnu99 -ffreestanding -nostdlib -fno-builtin \
          $(REPRO_FLAGS) \
          -D__DYNAMIC_REENT__ \
          -fno-stack-protector -ffunction-sections -fdata-sections \
          -Os -I../userland/include -I../include -I$(NEWLIB_INCLUDE)
OBJECTS := $(BUILD_DIR)/crt0_newlib.o \
           $(BUILD_DIR)/syscall_raw.o \
           $(BUILD_DIR)/syscalls.o \
           $(BUILD_DIR)/stdio_lock.o \
           $(BUILD_DIR)/pthread.o \
           $(BUILD_DIR)/pthread_sync.o

.PHONY: all clean check-newlib

all: check-newlib $(OBJECTS)

check-newlib:
	@if [ -z "$(NEWLIB_SYSROOT)" ] || [ ! -f "$(NEWLIB_INCLUDE)/stdio.h" ] || [ ! -f "$(NEWLIB_LIBC)" ]; then \
		echo "Error: newlib sysroot not found or incomplete."; \
		echo "Set NEWLIB_SYSROOT to a sysroot containing include/stdio.h and libc.a."; \
		echo "Example: make -C newlib-port NEWLIB_SYSROOT=/path/to/arm-os-sysroot"; \
		echo "Detected NEWLIB_SYSROOT=$(NEWLIB_SYSROOT)"; \
		echo "Detected NEWLIB_LIBC=$(NEWLIB_LIBC)"; \
		exit 1; \
	fi

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/crt0_newlib.o: $(CRT0_SOURCE) | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

$(BUILD_DIR)/syscall_raw.o: $(SYSCALL_SOURCE) | $(BUILD_DIR)
	$(AS) $(ASFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)
