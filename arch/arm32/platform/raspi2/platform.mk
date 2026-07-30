# ArmOS ARM32 Raspberry Pi 2 platform fragment.
#
# Shared BCM2836 platform with optional firmware HDMI and DWC2 USB host.

PLATFORM_CPU_CFLAGS = -mcpu=cortex-a7
PLATFORM_CFLAGS = $(PLATFORM_CPU_CFLAGS) -DARMOS_PLATFORM_RASPI2 -DARMOS_PLATFORM_RASPBERRYPI
PLATFORM_ASFLAGS = -march=armv7ve --defsym ARMOS_PLATFORM_RASPI2=1 --defsym ARMOS_PLATFORM_RASPBERRYPI=1
PLATFORM_LDFLAGS = \
	--defsym=KERNEL_LINK_ADDR=0x02010000 \
	--defsym=RAM_BASE_ADDR=0x00000000 \
	--defsym=RAM_END_ADDR=0x3F000000

# QEMU raspi2b models the SD card strictly and rejects raw images whose size is
# not a power of two.  The MBR partition layout remains 577 MiB; the platform
# image is padded to a 1 GiB card.
PLATFORM_DISK_SIZE_MB ?= 1024
PLATFORM_DISK_LAYOUT ?= fat32-first
# Keep the Raspberry Pi boot partition as standard FAT32 LBA by default.
# Some firmware revisions may ignore hidden FAT32 partition types (0x1B/0x1C).
PLATFORM_DISK_HIDDEN_BOOT ?= 0

RASPBERRYPI_PLATFORM_DIR = kernel/platform/raspberrypi
ENABLE_HDMI ?= 0
ENABLE_USB ?= 0
HDMI_WIDTH ?= 1280
HDMI_HEIGHT ?= 720
PLATFORM_OBJS = \
	$(RASPBERRYPI_PLATFORM_DIR)/devices.o \
	$(RASPBERRYPI_PLATFORM_DIR)/irq.o \
	$(RASPBERRYPI_PLATFORM_DIR)/mailbox.o \
	$(RASPBERRYPI_PLATFORM_DIR)/power.o \
	kernel/drivers/mmc/bcm2835_emmc.o \
	kernel/drivers/keyboard.o \
	kernel/drivers/display.o \
	kernel/drivers/virtio_input.o \
	kernel/drivers/virtio_net.o \
	kernel/drivers/virtio_block.o

ifneq ($(filter 1 yes true on,$(ENABLE_HDMI)),)
PLATFORM_CFLAGS += -DARMOS_ENABLE_HDMI \
	-DARMOS_HDMI_WIDTH=$(HDMI_WIDTH) -DARMOS_HDMI_HEIGHT=$(HDMI_HEIGHT)
PLATFORM_OBJS += kernel/drivers/video/raspberrypi_hdmi.o
endif

ifneq ($(filter 1 yes true on,$(ENABLE_USB)),)
PLATFORM_CFLAGS += -DARMOS_ENABLE_USB
PLATFORM_OBJS += kernel/drivers/usb/dwc2.o
endif

QEMU_MACHINE ?= raspi2b
QEMU_RUN_MACHINE ?= raspi2b
QEMU_CPU ?= cortex-a7
QEMU_BOOT_DRIVE ?= -drive file=$(PLATFORM_DISK_IMG),if=sd,format=raw
QEMU_BOOT_DEVICE ?=
QEMU_MMIO_DRIVE ?=
QEMU_MMIO_DEVICE ?=
QEMU_DEBUG_DEVICE ?=
QEMU_SIMPLE_DRIVE ?= -drive file=$(PLATFORM_DISK_IMG),if=sd,format=raw
