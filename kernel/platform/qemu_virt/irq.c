/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/platform/qemu_virt/irq.c
 * Layer: Kernel / QEMU virt GICv2 controller
 *
 * Responsibilities:
 * - Configure the QEMU virt GICv2 distributor and CPU interface.
 * - Route acknowledged IRQs to common kernel device handlers.
 * - Implement the architecture interrupt-controller contract.
 *
 * Notes:
 * - Device and scheduling policy remain in common kernel subsystems.
 * - EOIR ownership stays here so nested driver acknowledgements are harmless.
 */

#include <kernel/arch_platform.h>
#include <kernel/interrupt.h>
#include <kernel/kprintf.h>
#include <kernel/smp.h>
#include <kernel/timer.h>
#include <kernel/tlb.h>
#include <kernel/uart.h>
#include <kernel/virtio_block.h>
#include <kernel/virtio_input.h>
#include <kernel/virtio_net.h>

#define GICD_BASE ARMOS_PLATFORM_KERNEL_MMIO_IRQCTRL_BASE
#define GICC_BASE \
    (GICD_BASE + (0x08010000ULL - ARMOS_PLATFORM_IRQCTRL_PHYS_START))
#define GIC_SPURIOUS_IRQ 1023u
#define GIC_IRQ_COUNTERS 1024u

static volatile uint32_t irq_counts[GIC_IRQ_COUNTERS];
static volatile uint32_t total_count;
static volatile uint32_t last_id;
static uint32_t line_count;

static volatile uint32_t *mmio32(uint64_t address)
{
    return (volatile uint32_t *)(uintptr_t)address;
}

static volatile uint8_t *mmio8(uint64_t address)
{
    return (volatile uint8_t *)(uintptr_t)address;
}

void arch_irq_controller_init(void)
{
    volatile uint32_t *gicd = mmio32(GICD_BASE);
    volatile uint32_t *gicc = mmio32(GICC_BASE);
    uint32_t irq;

    gicc[0x000 / 4] = 0;
    gicd[0x000 / 4] = 0;
    line_count = ((gicd[0x004 / 4] & 0x1fu) + 1u) * 32u;
    if (line_count > GIC_IRQ_COUNTERS)
        line_count = GIC_IRQ_COUNTERS;

    for (irq = 32; irq < line_count; irq += 32)
        gicd[0x180 / 4 + irq / 32] = 0xffffffffu;
    for (irq = 0; irq < line_count; irq += 4)
        gicd[0x400 / 4 + irq / 4] = 0xa0a0a0a0u;

    gicc[0x004 / 4] = 0xf0u;
    gicc[0x008 / 4] = 0x03u;
    gicd[0x000 / 4] = 1u;
    gicc[0x000 / 4] = 1u;
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

void arch_irq_init_local_cpu_interface(void)
{
    volatile uint32_t *gicc = mmio32(GICC_BASE);

    gicc[0x004 / 4] = 0xf0u;
    gicc[0x008 / 4] = 0x03u;
    gicc[0x000 / 4] = 1u;
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

const char *arch_irq_controller_name(void) { return "GIC: v2"; }
uint32_t arch_irq_controller_line_count(void) { return line_count; }

static void enable_irq(uint32_t irq, bool edge)
{
    volatile uint32_t *gicd = mmio32(GICD_BASE);
    volatile uint8_t *priority = mmio8(GICD_BASE + 0x400u);

    if (irq >= line_count)
        return;
    if (irq >= 32)
        mmio8(GICD_BASE + 0x800u)[irq] = 1u;
    if (irq >= 16) {
        uint32_t value = gicd[0xc00 / 4 + irq / 16];
        uint32_t shift = (irq % 16u) * 2u;

        value &= ~(3u << shift);
        if (edge)
            value |= 2u << shift;
        gicd[0xc00 / 4 + irq / 16] = value;
    }
    priority[irq] = 0xa0u;
    gicd[0x100 / 4 + irq / 32] = 1u << (irq % 32u);
    __asm__ volatile("dsb sy" ::: "memory");
}

void arch_irq_enable(uint32_t irq) { enable_irq(irq, true); }
void arch_irq_enable_level(uint32_t irq) { enable_irq(irq, false); }

void arch_irq_disable(uint32_t irq)
{
    if (irq < line_count)
        mmio32(GICD_BASE)[0x180 / 4 + irq / 32] = 1u << (irq % 32u);
}

void arch_irq_ack(uint32_t irq) { (void)irq; }
uint32_t arch_irq_get_count(uint32_t irq)
{
    return irq < GIC_IRQ_COUNTERS ? irq_counts[irq] : 0;
}
uint32_t arch_irq_get_total_count(void) { return total_count; }
uint32_t arch_irq_get_last_id(void) { return last_id; }

void arch_irq_send_ipi(uint32_t target_cpu_mask, uint32_t irq)
{
    if (irq < 16u && target_cpu_mask != 0)
        mmio32(GICD_BASE)[0xf00 / 4] =
            ((target_cpu_mask & 0xffu) << 16) | irq;
}

void arch_irq_send_ipi_others(uint32_t irq)
{
    if (irq < 16u)
        mmio32(GICD_BASE)[0xf00 / 4] = (1u << 24) | irq;
}

void irq_c_handler(void)
{
    volatile uint32_t *gicc = mmio32(GICC_BASE);
    uint32_t iar = gicc[0x00c / 4];
    uint32_t irq = iar & 0x3ffu;
    uint32_t cpu = smp_processor_id();

    if (irq == GIC_SPURIOUS_IRQ) {
        last_id = irq;
        return;
    }

    last_id = irq;
    total_count++;
    if (irq < GIC_IRQ_COUNTERS)
        irq_counts[irq]++;
    smp_note_irq(cpu);

    if (irq == IRQ_SGI_TLB_SHOOTDOWN) {
        smp_note_ipi(cpu);
        tlb_handle_remote_ipi(cpu);
    } else if (irq == virtio_blk_get_irq()) {
        virtio_block_irq_handler();
    } else if (irq == virtio_input_get_irq() ||
               virtio_input_handles_irq(irq)) {
        virtio_input_irq_handler(irq);
    } else if (irq == virtio_net_get_irq()) {
        virtio_net_irq_handler();
    } else if (irq == IRQ_TIMER) {
        timer_irq_handler();
    } else if (irq == IRQ_UART || irq == IRQ_KEYBOARD) {
        uart_irq_handler();
    } else {
        KDEBUG("Unhandled IRQ %u\n", irq);
    }

    gicc[0x010 / 4] = iar;
}

void fiq_c_handler(void)
{
    irq_c_handler();
}
