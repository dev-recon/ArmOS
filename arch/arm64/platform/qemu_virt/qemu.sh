#!/usr/bin/env bash
# QEMU launch defaults for the ARM64 qemu-virt platform.

QEMU_MACHINE_DEFAULT="virt,gic-version=2"
QEMU_CPU_DEFAULT="cortex-a72"
QEMU_MEMORY_DEFAULT="1G"
QEMU_SMP_DEFAULT="4"
QEMU_BLOCK_ENABLED_DEFAULT="1"
QEMU_KERNEL_IMAGE_DEFAULT="$ROOT_DIR/build/images/kernel-arm64-qemu-virt.bin"
QEMU_DISK_IMAGE_DEFAULT="$ROOT_DIR/build/images/disk-arm64-qemu-virt.img"
QEMU_BLOCK_DEVICE_DEFAULT="virtio-blk-device,drive=hd0"
QEMU_GPU_DEVICE_DEFAULT="virtio-gpu-device"
QEMU_GPU_XRES_DEFAULT="1024"
QEMU_GPU_YRES_DEFAULT="768"
QEMU_INPUT_DEVICE_DEFAULT="virtio-keyboard-device,event_idx=off,indirect_desc=off"
QEMU_NET_DEVICE_MODEL_DEFAULT="virtio-net-device"
