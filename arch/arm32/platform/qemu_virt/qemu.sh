#!/usr/bin/env bash
# QEMU launch defaults for the ARM32 qemu-virt platform.
#
# This file is sourced by boot scripts. Keep it shell-only; the Makefile uses
# platform.mk for equivalent build-time defaults.

QEMU_MACHINE_DEFAULT="virt"
QEMU_CPU_DEFAULT="cortex-a15"
QEMU_KERNEL_IMAGE_DEFAULT="$ROOT_DIR/build/images/kernel-arm32-qemu-virt.bin"
QEMU_DISK_IMAGE_DEFAULT="$ROOT_DIR/build/images/disk-arm32-qemu-virt.img"
QEMU_BLOCK_DEVICE_DEFAULT="virtio-blk-device,drive=hd0"
QEMU_GPU_DEVICE_DEFAULT="virtio-gpu-device"
QEMU_GPU_XRES_DEFAULT="1024"
QEMU_GPU_YRES_DEFAULT="768"
QEMU_INPUT_DEVICE_DEFAULT="virtio-keyboard-device,event_idx=off,indirect_desc=off"
QEMU_NET_DEVICE_MODEL_DEFAULT="virtio-net-device"
