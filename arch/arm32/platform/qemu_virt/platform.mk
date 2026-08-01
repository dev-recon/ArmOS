# ArmOS ARM32 qemu-virt platform fragment.
#
# The top-level Makefile includes one platform fragment after selecting
# TARGET_ARCH. Keep machine-specific CPU flags, platform objects, and QEMU
# defaults here so a second ARM32 board can add its own fragment without
# growing conditionals in the root build.

PLATFORM_CPU_CFLAGS = -mcpu=cortex-a15
PLATFORM_CFLAGS = $(PLATFORM_CPU_CFLAGS) -DARMOS_PLATFORM_QEMU_VIRT

PLATFORM_OBJS = \
	kernel/platform/qemu_virt/devices.o \
	kernel/drivers/keyboard.o \
	kernel/drivers/display.o \
	kernel/platform/qemu_virt/virtio_gpu.o \
	kernel/drivers/virtio_input.o \
	kernel/drivers/virtio_net.o \
	kernel/drivers/virtio_block.o \
	$(ARCH_DIR)/interrupt/gic.o \
	$(ARCH_DIR)/power/psci.o

QEMU_MACHINE ?= virt
QEMU_RUN_MACHINE ?= virt,highmem=off
QEMU_CPU ?= cortex-a15
QEMU_BOOT_DRIVE ?= -drive file=$(PLATFORM_DISK_IMG),if=none,format=raw,id=hd0
QEMU_BOOT_DEVICE ?= -device virtio-blk-device,drive=hd0
QEMU_MMIO_DRIVE ?= -drive file=$(PLATFORM_DISK_IMG),format=raw,if=none,id=disk0
QEMU_MMIO_DEVICE ?= -device virtio-blk-device,drive=disk0,bus=virtio-mmio-bus.0
QEMU_DEBUG_DEVICE ?= -device virtio-blk-device,drive=disk0
QEMU_SIMPLE_DRIVE ?= -drive file=$(PLATFORM_DISK_IMG),if=virtio,format=raw
