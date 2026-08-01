# ArmOS ARM64 QEMU virt platform.

PLATFORM_CFLAGS = -mcpu=cortex-a72 -DARMOS_PLATFORM_QEMU_VIRT
PLATFORM_ASFLAGS = -mcpu=cortex-a72
PLATFORM_LDFLAGS = \
	--defsym=KERNEL_LINK_ADDR=0x40080000 \
	--defsym=RAM_BASE_ADDR=0x40000000 \
	--defsym=RAM_END_ADDR=0x80000000
STACK_PROTECTOR_FLAG = -fno-stack-protector
LINKER_SCRIPT = $(ARCH_DIR)/linker.ld
PLATFORM_OBJS = \
	kernel/platform/qemu_virt/devices.o \
	kernel/platform/qemu_virt/irq.o \
	kernel/drivers/keyboard.o \
	kernel/drivers/display.o \
	kernel/platform/qemu_virt/virtio_gpu.o \
	kernel/drivers/virtio_input.o \
	kernel/drivers/virtio_net.o \
	kernel/drivers/virtio_block.o \
	$(ARCH_DIR)/power/psci.o

QEMU_MACHINE ?= virt,gic-version=2
QEMU_CPU ?= cortex-a72
