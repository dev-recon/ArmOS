# ArmOS userland build.
#
# Newlib is the supported userspace ABI. Legacy home-grown libc sources are
# kept under userland/legacy for reference only.

TARGET_ARCH ?= arm32
ifeq ($(TARGET_ARCH),arm64)
ARCH ?= aarch64-elf-
else
ARCH ?= arm-none-eabi-
endif
CC := $(ARCH)gcc
AR := $(ARCH)ar
STRIP := $(ARCH)strip
ARMOS_SOURCE_ROOT := $(abspath ..)
ARMOS_REPRODUCIBLE_ROOT ?= /usr/src/armos
REPRO_FLAGS := -ffile-prefix-map=$(ARMOS_SOURCE_ROOT)=$(ARMOS_REPRODUCIBLE_ROOT) \
               -fmacro-prefix-map=$(ARMOS_SOURCE_ROOT)=$(ARMOS_REPRODUCIBLE_ROOT) \
               -fdebug-prefix-map=$(ARMOS_SOURCE_ROOT)=$(ARMOS_REPRODUCIBLE_ROOT)

TARGET_PLATFORM ?= qemu-virt
TARGET_BUILD_ROOT ?= ../build/$(TARGET_ARCH)/$(TARGET_PLATFORM)
BUILD_DIR := $(TARGET_BUILD_ROOT)/userland
OUT_DIR := $(BUILD_DIR)/out
OUT_BIN := $(OUT_DIR)/bin
OUT_SBIN := $(OUT_DIR)/sbin
OUT_USR_BIN := $(OUT_DIR)/usr/bin
OUT_USR_LIB := $(OUT_DIR)/usr/lib
OUT_OPT := $(OUT_DIR)/opt
NEWLIB_PORT := ../newlib-port
USERFS_ROOT ?= $(TARGET_BUILD_ROOT)/userfs
USERFS_BIN := $(USERFS_ROOT)/bin
USERFS_SBIN := $(USERFS_ROOT)/sbin
USERFS_USR_BIN := $(USERFS_ROOT)/usr/bin
USERFS_USR_LIB := $(USERFS_ROOT)/usr/lib
USERFS_OPT := $(USERFS_ROOT)/opt

ifeq ($(TARGET_ARCH),arm64)
ARCH_FLAGS := -mcpu=cortex-a53
USER_TEXT_ADDRESS := 0x100000000
else
ARCH_FLAGS := -mcpu=cortex-a15 -marm -mfpu=neon-vfpv4 -mfloat-abi=soft
USER_TEXT_ADDRESS := 0x8000
endif
NEWLIB_RUNTIME_DIR ?= $(TARGET_BUILD_ROOT)/newlib-port
NEWLIB_SYSROOT ?= $(shell $(CC) -print-sysroot 2>/dev/null)
NEWLIB_LIBC ?= $(NEWLIB_SYSROOT)/lib/libc.a
LIBGCC := $(shell $(CC) $(ARCH_FLAGS) -print-libgcc-file-name 2>/dev/null)

CFLAGS := $(ARCH_FLAGS) -std=gnu99 -Os -ffreestanding -fno-builtin \
          $(REPRO_FLAGS) \
          -ffunction-sections -fdata-sections \
          -fno-stack-protector -DARM_OS_NEWLIB \
          -D__DYNAMIC_REENT__ \
          -DARMOS_USER_TEXT_ADDRESS=$(USER_TEXT_ADDRESS) \
          -Iinclude -I../include -I$(NEWLIB_SYSROOT)/include
LDFLAGS := $(ARCH_FLAGS) -nostdlib -nostartfiles -static \
           -Wl,-Ttext=$(USER_TEXT_ADDRESS) -Wl,-e,_start -Wl,--gc-sections \
           -Wl,--allow-multiple-definition

PORT_OBJECTS := $(NEWLIB_RUNTIME_DIR)/crt0_newlib.o \
                $(NEWLIB_RUNTIME_DIR)/syscall_raw.o \
                $(NEWLIB_RUNTIME_DIR)/syscalls.o \
                $(NEWLIB_RUNTIME_DIR)/stdio_lock.o \
                $(NEWLIB_RUNTIME_DIR)/pthread.o \
                $(NEWLIB_RUNTIME_DIR)/pthread_sync.o

COREUTIL_SRCS := $(wildcard coreutils/src/*.c)
COREUTILS := $(sort $(patsubst coreutils/src/%.c,%,$(COREUTIL_SRCS)))
COREUTIL_TARGETS := $(COREUTILS:%=$(OUT_BIN)/%)

SYSTEM_TOOL_SRCS := $(wildcard system/tools/*.c)
SYSTEM_TOOLS := $(sort $(patsubst system/tools/%.c,%,$(SYSTEM_TOOL_SRCS)))
SYSTEM_TOOL_TARGETS := $(SYSTEM_TOOLS:%=$(OUT_SBIN)/%)

PROGRAM_DIRS := $(sort $(dir $(wildcard programs/*/*.c)))
PROGRAMS := $(patsubst programs/%/,%,$(PROGRAM_DIRS))

# hello64 is a focused ARM64 bootstrap probe, not an installed system command.
# Keep it available as an explicit target without changing the common ARM32 /
# ARM64 userfs path set.
PROGRAMS := $(filter-out hello64,$(PROGRAMS))

# armos-wlcomp is a privileged system service installed in /sbin.
PROGRAMS := $(filter-out armos-wlcomp,$(PROGRAMS))

# Linked explicitly against both native Wayland libraries below.
PROGRAMS := $(filter-out wayland-lib-test,$(PROGRAMS))

EXTERNAL_DIRS := $(sort $(dir $(wildcard opt/*/src/*.c)))
EXTERNALS := $(patsubst opt/%/src/,%,$(EXTERNAL_DIRS))

# These tools have their own configure/build flow and are staged by dedicated
# top-level build switches.  The generic "one directory -> one binary" rule is
# intentionally too small for them.
EXTERNALS := $(filter-out bmake bsdawk bsddiff bsdelftools bsdinstall bsdm4 bsdmtree bsdpax bsdpatch bsdsed bsdxargs tcc zlib libjpeg libpng libtiff,$(EXTERNALS))

ifneq ($(ENABLE_TCC),1)
PROGRAMS := $(filter-out tcc,$(PROGRAMS))
endif

# fbview links against zlib/libjpeg/libpng/libtiff and is built by the
# dedicated top-level BUILD_FBVIEW flow.
PROGRAMS := $(filter-out fbview,$(PROGRAMS))

PROGRAM_TARGETS := $(PROGRAMS:%=$(OUT_USR_BIN)/%)
ARMOS_WLCOMP_TARGET := $(OUT_SBIN)/armos-wlcomp
EXTERNAL_TARGETS := $(foreach name,$(EXTERNALS),$(OUT_OPT)/$(name)/bin/$(name))

SYSTEM_TARGETS := $(OUT_SBIN)/init $(OUT_SBIN)/mash $(SYSTEM_TOOL_TARGETS)
TARGETS := $(SYSTEM_TARGETS) $(ARMOS_WLCOMP_TARGET) $(COREUTIL_TARGETS) \
           $(PROGRAM_TARGETS) $(EXTERNAL_TARGETS)

MASH_SRCS := $(wildcard system/mash/src/*.c)
MASH_OBJECTS := $(patsubst system/mash/src/%.c,$(BUILD_DIR)/system/mash/%.o,$(MASH_SRCS))
INIT_SRCS := $(wildcard system/init/*.c)
INIT_OBJECTS := $(patsubst system/init/%.c,$(BUILD_DIR)/system/init/%.o,$(INIT_SRCS))
HELLO64_SRCS := $(wildcard programs/hello64/*.c)
HELLO64_OBJECTS := $(patsubst programs/hello64/%.c,$(BUILD_DIR)/programs/hello64/%.o,$(HELLO64_SRCS))
ARMOS_WLCOMP_SRCS := $(wildcard programs/armos-wlcomp/*.c)
ARMOS_WLCOMP_OBJECTS := $(patsubst programs/armos-wlcomp/%.c,$(BUILD_DIR)/programs/armos-wlcomp/%.o,$(ARMOS_WLCOMP_SRCS))
WAYLAND_CLIENT_OBJECTS := $(BUILD_DIR)/lib/wayland/client.o
WAYLAND_SERVER_OBJECTS := $(BUILD_DIR)/lib/wayland/server.o
WAYLAND_CLIENT_LIBRARY := $(OUT_USR_LIB)/libwayland-client.a
WAYLAND_SERVER_LIBRARY := $(OUT_USR_LIB)/libwayland-server.a
WAYLAND_LIBRARIES := $(WAYLAND_CLIENT_LIBRARY) $(WAYLAND_SERVER_LIBRARY)
WAYLAND_API_TEST_OBJECT := $(BUILD_DIR)/programs/wayland-lib-test/wayland-lib-test.o
WAYLAND_API_TEST_TARGET := $(OUT_USR_BIN)/wayland-lib-test

TARGETS += $(WAYLAND_LIBRARIES) $(WAYLAND_API_TEST_TARGET)

.PHONY: all clean install check-newlib newlib-port list init mash hello64 armos-wlcomp
.PHONY: $(COREUTILS) $(SYSTEM_TOOLS) $(PROGRAMS) $(EXTERNALS)

all: check-newlib $(TARGETS)

check-newlib:
	@if [ -z "$(NEWLIB_SYSROOT)" ] || [ ! -f "$(NEWLIB_SYSROOT)/include/stdio.h" ] || [ ! -f "$(NEWLIB_LIBC)" ]; then \
		echo "Error: newlib sysroot not found or incomplete."; \
		echo "Set NEWLIB_SYSROOT to a sysroot containing include/stdio.h and libc.a."; \
		echo "Detected NEWLIB_SYSROOT=$(NEWLIB_SYSROOT)"; \
		echo "Detected NEWLIB_LIBC=$(NEWLIB_LIBC)"; \
		exit 1; \
	fi

newlib-port:
	$(MAKE) -C $(NEWLIB_PORT) TARGET_ARCH="$(TARGET_ARCH)" TARGET_PLATFORM="$(TARGET_PLATFORM)" ARCH="$(ARCH)" BUILD_DIR="$(abspath $(NEWLIB_RUNTIME_DIR))" NEWLIB_SYSROOT="$(NEWLIB_SYSROOT)" NEWLIB_LIBC="$(NEWLIB_LIBC)"

list:
	@printf '%s\n' $(TARGETS)

init: $(OUT_SBIN)/init
mash: $(OUT_SBIN)/mash
hello64: $(OUT_USR_BIN)/hello64
armos-wlcomp: $(ARMOS_WLCOMP_TARGET)
$(SYSTEM_TOOLS): %: $(OUT_SBIN)/%
$(COREUTILS): %: $(OUT_BIN)/%
$(PROGRAMS): %: $(OUT_USR_BIN)/%

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/coreutils/%.o: coreutils/src/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -include arm_os_abi.h -c $< -o $@

$(BUILD_DIR)/system/tools/%.o: system/tools/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -include arm_os_abi.h -c $< -o $@

$(BUILD_DIR)/programs/%.o: programs/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/opt/%.o: opt/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lib/wayland/%.o: lib/wayland/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(WAYLAND_CLIENT_LIBRARY): $(WAYLAND_CLIENT_OBJECTS)
	mkdir -p $(@D)
	$(AR) rcs $@ $^

$(WAYLAND_SERVER_LIBRARY): $(WAYLAND_SERVER_OBJECTS)
	mkdir -p $(@D)
	$(AR) rcs $@ $^

$(WAYLAND_API_TEST_TARGET): newlib-port $(WAYLAND_API_TEST_OBJECT) $(WAYLAND_LIBRARIES)
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(WAYLAND_API_TEST_OBJECT) \
		$(WAYLAND_CLIENT_LIBRARY) $(WAYLAND_SERVER_LIBRARY) \
		"$(NEWLIB_LIBC)" "$(LIBGCC)"

$(BUILD_DIR)/system/init/%.o: system/init/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/system/mash/%.o: system/mash/src/%.c | $(BUILD_DIR)
	mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isystem/mash/include -c $< -o $@

define PROGRAM_RULE
$(1)_SRCS := $$(wildcard programs/$(1)/*.c)
$(1)_OBJECTS := $$(patsubst programs/$(1)/%.c,$$(BUILD_DIR)/programs/$(1)/%.o,$$($(1)_SRCS))

$(OUT_USR_BIN)/$(1): newlib-port $$($(1)_OBJECTS)
	mkdir -p $$(@D)
	$$(CC) $$(LDFLAGS) -o $$@ $$(PORT_OBJECTS) $$($(1)_OBJECTS) "$$(NEWLIB_LIBC)" "$$(LIBGCC)"
endef

$(foreach prog,$(PROGRAMS),$(eval $(call PROGRAM_RULE,$(prog))))

$(OUT_USR_BIN)/hello64: newlib-port $(HELLO64_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(HELLO64_OBJECTS) "$(NEWLIB_LIBC)" "$(LIBGCC)"

$(ARMOS_WLCOMP_TARGET): newlib-port $(ARMOS_WLCOMP_OBJECTS) $(WAYLAND_SERVER_LIBRARY)
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(ARMOS_WLCOMP_OBJECTS) \
		$(WAYLAND_SERVER_LIBRARY) "$(NEWLIB_LIBC)" "$(LIBGCC)"

define EXTERNAL_RULE
$(1)_EXTERNAL_SRCS := $$(wildcard opt/$(1)/src/*.c)
$(1)_EXTERNAL_OBJECTS := $$(patsubst opt/%.c,$$(BUILD_DIR)/opt/%.o,$$($(1)_EXTERNAL_SRCS))

$(OUT_OPT)/$(1)/bin/$(1): newlib-port $$($(1)_EXTERNAL_OBJECTS)
	mkdir -p $$(@D)
	$$(CC) $$(LDFLAGS) -o $$@ $$(PORT_OBJECTS) $$($(1)_EXTERNAL_OBJECTS) "$$(NEWLIB_LIBC)" "$$(LIBGCC)"

$(1): $(OUT_OPT)/$(1)/bin/$(1)
endef

$(foreach prog,$(EXTERNALS),$(eval $(call EXTERNAL_RULE,$(prog))))

$(OUT_SBIN)/init: newlib-port $(INIT_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(INIT_OBJECTS) "$(NEWLIB_LIBC)" "$(LIBGCC)"

$(OUT_SBIN)/mash: newlib-port $(MASH_OBJECTS)
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(MASH_OBJECTS) "$(NEWLIB_LIBC)" "$(LIBGCC)"

$(OUT_SBIN)/%: newlib-port $(BUILD_DIR)/system/tools/%.o
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(BUILD_DIR)/system/tools/$*.o "$(NEWLIB_LIBC)" "$(LIBGCC)"

$(OUT_BIN)/%: newlib-port $(BUILD_DIR)/coreutils/%.o
	mkdir -p $(@D)
	$(CC) $(LDFLAGS) -o $@ $(PORT_OBJECTS) $(BUILD_DIR)/coreutils/$*.o "$(NEWLIB_LIBC)" "$(LIBGCC)"

install: $(TARGETS)
	mkdir -p $(USERFS_BIN) $(USERFS_SBIN) $(USERFS_USR_BIN) $(USERFS_USR_LIB) $(USERFS_OPT)
	rm -rf $(USERFS_ROOT)/opt/newlib
	rm -f $(USERFS_USR_BIN)/hello64
	# xvprobe is an optional local-only binary; never retain one from another target.
	rm -f $(USERFS_USR_BIN)/xvprobe
	@for name in init mash; do \
		rm -f $(USERFS_SBIN)/$$name; \
		cp $(OUT_SBIN)/$$name $(USERFS_SBIN)/$$name; \
		rm -f $(USERFS_BIN)/$$name $(USERFS_USR_BIN)/$$name; \
		$(STRIP) --strip-all $(USERFS_SBIN)/$$name; \
	done
	rm -f $(USERFS_SBIN)/windowserverd
	rm -f $(USERFS_SBIN)/armos-wlcomp
	cp $(ARMOS_WLCOMP_TARGET) $(USERFS_SBIN)/armos-wlcomp
	rm -f $(USERFS_BIN)/armos-wlcomp $(USERFS_USR_BIN)/armos-wlcomp
	$(STRIP) --strip-all $(USERFS_SBIN)/armos-wlcomp
	@for name in $(SYSTEM_TOOLS); do \
		rm -f $(USERFS_SBIN)/$$name; \
		cp $(OUT_SBIN)/$$name $(USERFS_SBIN)/$$name; \
		rm -f $(USERFS_BIN)/$$name $(USERFS_USR_BIN)/$$name; \
		rm -f $(USERFS_OPT)/$$name/bin/$$name; \
		$(STRIP) --strip-all $(USERFS_SBIN)/$$name; \
	done
	@for name in $(COREUTILS); do \
		rm -f $(USERFS_BIN)/$$name; \
		cp $(OUT_BIN)/$$name $(USERFS_BIN)/$$name; \
		rm -f $(USERFS_SBIN)/$$name $(USERFS_USR_BIN)/$$name; \
		rm -f $(USERFS_OPT)/$$name/bin/$$name; \
		$(STRIP) --strip-all $(USERFS_BIN)/$$name; \
	done
	@for name in $(PROGRAMS); do \
		rm -f $(USERFS_USR_BIN)/$$name; \
		cp $(OUT_USR_BIN)/$$name $(USERFS_USR_BIN)/$$name; \
		rm -f $(USERFS_BIN)/$$name $(USERFS_SBIN)/$$name; \
		rm -f $(USERFS_OPT)/$$name/bin/$$name; \
		$(STRIP) --strip-all $(USERFS_USR_BIN)/$$name; \
	done
	@for name in $(EXTERNALS); do \
		mkdir -p $(USERFS_OPT)/$$name/bin; \
		rm -f $(USERFS_OPT)/$$name/bin/$$name; \
		cp $(OUT_OPT)/$$name/bin/$$name $(USERFS_OPT)/$$name/bin/$$name; \
		rm -f $(USERFS_BIN)/$$name $(USERFS_SBIN)/$$name $(USERFS_USR_BIN)/$$name; \
		$(STRIP) --strip-all $(USERFS_OPT)/$$name/bin/$$name; \
	done
	cp $(WAYLAND_CLIENT_LIBRARY) $(USERFS_USR_LIB)/libwayland-client.a
	cp $(WAYLAND_SERVER_LIBRARY) $(USERFS_USR_LIB)/libwayland-server.a
	cp $(WAYLAND_API_TEST_TARGET) $(USERFS_USR_BIN)/wayland-lib-test
	$(STRIP) --strip-all $(USERFS_USR_BIN)/wayland-lib-test

clean:
	rm -rf $(BUILD_DIR)
