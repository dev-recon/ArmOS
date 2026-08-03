/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/include/asm/platform/qemu_virt.h
 * Layer: Platform / ARM64 QEMU virt
 *
 * Responsibilities:
 * - Describe the RAM and MMIO map exposed by the QEMU virt machine.
 * - Publish the stable platform contract consumed by common kernel code.
 *
 * Notes:
 * - These are hardware facts, not filesystem, process or scheduler policy.
 */

#ifndef ASM_ARM64_PLATFORM_QEMU_VIRT_H
#define ASM_ARM64_PLATFORM_QEMU_VIRT_H

#include <asm/mmu.h>

#define ARMOS_PLATFORM_NAME                     "QEMU virt"
#define ARMOS_PLATFORM_CPU_MODEL                "ARM Cortex-A72 @ QEMU virt"
#define ARMOS_PLATFORM_CPU_FEATURES             "fp asimd evtstrm aes pmull sha1 sha2 crc32"
#define ARMOS_PLATFORM_HARDWARE_NAME            "ArmOS ARM64 QEMU virt"

#define ARMOS_PLATFORM_RAM_START                0x40000000ULL
#define ARMOS_PLATFORM_RAM_FALLBACK_SIZE        0x40000000ULL
#define ARMOS_PLATFORM_RAM_PROBE_MAX_MB         1024u

#define ARMOS_PLATFORM_DEVICE_START             0x08000000ULL
#define ARMOS_PLATFORM_DEVICE_END               0x40000000ULL
#define ARMOS_PLATFORM_UART0_PHYS_BASE          0x09000000ULL
#define ARMOS_PLATFORM_UART0_PHYS_SECTION_BASE  0x09000000ULL
#define ARMOS_PLATFORM_UART0_KERNEL_BASE        \
    (ARM64_KERNEL_VA_BASE + ARMOS_PLATFORM_UART0_PHYS_BASE)
#define ARMOS_PLATFORM_RTC_PHYS_BASE            0x09010000ULL
#define ARMOS_PLATFORM_UART0_CLOCK_HZ           24000000u
#define ARMOS_PLATFORM_UART0_BAUD               115200u
#define ARMOS_PLATFORM_UART_IRQ                 33u

#define ARMOS_PLATFORM_TIMER_FALLBACK_HZ        62500000u
#define ARMOS_PLATFORM_TIMER_IRQ                30u
#define ARMOS_PLATFORM_DEFAULT_CPU_COUNT        4u
#define ARMOS_PLATFORM_HAS_PSCI                 1u
#define ARMOS_PLATFORM_HAS_SMP_IPI              1u
#define ARMOS_PLATFORM_SGI_TLB_SHOOTDOWN_IRQ    0u

#define ARMOS_PLATFORM_KERNEL_MMIO_SECTION_SIZE 0x00100000ULL
#define ARMOS_PLATFORM_KERNEL_MMIO_IRQCTRL_BASE \
    (ARM64_KERNEL_VA_BASE + ARMOS_PLATFORM_IRQCTRL_PHYS_START)
#define ARMOS_PLATFORM_KERNEL_MMIO_UART_BASE    ARMOS_PLATFORM_UART0_KERNEL_BASE
#define ARMOS_PLATFORM_KERNEL_MMIO_RTC_BASE     \
    (ARM64_KERNEL_VA_BASE + ARMOS_PLATFORM_RTC_PHYS_BASE)
#define ARMOS_PLATFORM_IRQCTRL_PHYS_START       0x08000000ULL
#define ARMOS_PLATFORM_IRQCTRL_PHYS_END         0x08050000ULL

#define ARMOS_PLATFORM_VIRTIO_PHYS_START        0x0A000000ULL
#define ARMOS_PLATFORM_VIRTIO_MMIO_SIZE         0x200u
#define ARMOS_PLATFORM_VIRTIO_IRQ_BASE          16u
#define ARMOS_PLATFORM_VIRTIO_IRQ(n)            (16u + (n))
#define ARMOS_PLATFORM_KERNEL_MMIO_VIRTIO_BASE  \
    (ARM64_KERNEL_VA_BASE + ARMOS_PLATFORM_VIRTIO_PHYS_START)
#define ARMOS_PLATFORM_VIRTIO_MMIO_ADDR(paddr)  \
    (ARM64_KERNEL_VA_BASE + (paddr))
#define ARMOS_PLATFORM_VIRTIO_BLOCK_PHYS        0x0A000200ULL
#define ARMOS_PLATFORM_VIRTIO_BLOCK_FALLBACK_PHYS \
    (0x0A000000ULL + 31u * 0x200u)
#define ARMOS_PLATFORM_VIRTIO_BLOCK_IRQ         17u

#endif /* ASM_ARM64_PLATFORM_QEMU_VIRT_H */
