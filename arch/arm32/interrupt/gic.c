/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/interrupt/gic.c
 * Layer: ARM32 / GICv2 interrupt controller
 *
 * Responsibilities:
 * - Handle IRQs, timer ticks, aborts, and crash diagnostics.
 * - Keep exception reports actionable during early kernel debugging.
 *
 * Notes:
 * - Handlers run in privileged exception modes with banked registers.
 */

#include <kernel/interrupt.h>
#include <kernel/timer.h>
#include <kernel/keyboard.h>
#include <kernel/virtio_block.h>
#include <kernel/virtio_input.h>
#include <kernel/virtio_net.h>
#include <kernel/uart.h>
#include <kernel/kprintf.h>
#include <kernel/platform_devices.h>
#include <kernel/smp.h>
#include <kernel/tlb.h>
#include <kernel/arch_platform.h>
#include <asm/arm.h>

/* Compteur global pour verifier que les IRQ arrivent */
static volatile uint32_t irq_count = 0;
static volatile uint32_t last_irq_id = 0;
static uint32_t gic_irq_line_count = 0;
#define GIC_IRQ_COUNTERS 1024
static volatile uint32_t irq_counts[GIC_IRQ_COUNTERS];

/* Acces runtime via l'alias MMIO prive TTBR1. */
#define LOCAL_GICD_BASE     QEMU_VIRT_KERNEL_MMIO_GIC_DIST_BASE
#define LOCAL_GICC_BASE     QEMU_VIRT_KERNEL_MMIO_GIC_CPU_BASE
#define LOCAL_VIRTIO_BASE   ARMOS_PLATFORM_KERNEL_MMIO_VIRTIO_BASE

static uint8_t gic_boot_cpu_mask(void)
{
    uint32_t cpu = smp_boot_cpu_id();

    if (cpu >= 8)
        cpu = 0;
    return (uint8_t)(1U << cpu);
}

static uint8_t gic_route_spi_to_boot_cpu(uint32_t irq)
{
    volatile uint8_t* itargetsr = (volatile uint8_t*)(LOCAL_GICD_BASE + 0x800);
    uint8_t mask;

    /*
     * GICv2 ITARGETSR is meaningful for SPIs only. SGIs/PPIs are private to a
     * CPU interface and must not be programmed here. Platforms with multiple
     * CPUs may otherwise route UART/VirtIO interrupts to a parked CPU.
     */
    if (irq < 32)
        return itargetsr[irq];

    mask = gic_boot_cpu_mask();
    itargetsr[irq] = mask;
    return itargetsr[irq];
}

void init_gic(void)
{
    KDEBUG("[GIC] Starting GIC initialization (%s)...\n", arch_platform_name());
    
    volatile uint32_t* gicd = (volatile uint32_t*)LOCAL_GICD_BASE;
    volatile uint32_t* gicc = (volatile uint32_t*)LOCAL_GICC_BASE;
    
    /* === PHASE 1: IDENTIFICATION === */
    
    uint32_t gicd_iidr = gicd[0x008/4];  /* GICD_IIDR */
    uint32_t gicc_iidr = gicc[0x00C/4];  /* GICC_IIDR */
    uint32_t gicd_typer = gicd[0x004/4]; /* GICD_TYPER */
    
    KDEBUG("[GIC] GICD_IIDR: 0x%08X\n", gicd_iidr);
    KDEBUG("[GIC] GICC_IIDR: 0x%08X\n", gicc_iidr);
    KDEBUG("[GIC] GICD_TYPER: 0x%08X\n", gicd_typer);
    
    uint32_t num_irqs = ((gicd_typer & 0x1F) + 1) * 32;
    gic_irq_line_count = num_irqs;
    KDEBUG("[GIC] Number of IRQs: %u\n", num_irqs);
    
    bool targets_auto_managed = arch_platform_irq_targets_auto_managed();
    KDEBUG("[GIC] ITARGETSR mode: %s\n",
           targets_auto_managed ? "platform auto-managed" : "kernel-programmed");
    
    /* === PHASE 2: CONFIGURATION === */
    
    KDEBUG("[GIC] Disabling GIC temporarily...\n");
    gicd[0x000/4] = 0;
    gicc[0x000/4] = 0;
    
    /* Configuration des priorites */
    KDEBUG("[GIC] Setting interrupt priorities...\n");
    for (uint32_t i = 0; i < num_irqs; i += 4) {
        gicd[0x400/4 + i/4] = 0xA0A0A0A0;
    }
    
    /* Configuration CPU interface */
    KDEBUG("[GIC] Configuring CPU interface...\n");
    gicc[0x004/4] = 0xF0;  /* GICC_PMR: Priorite mask tres permissive */
    gicc[0x008/4] = 0x03;  /* GICC_BPR: Binary point */
    
    /* === PHASE 3: TEST ITARGETSR (INFORMATIF SEULEMENT) === */
    
    volatile uint8_t* itargetsr = (volatile uint8_t*)(LOCAL_GICD_BASE + 0x800);
    
    if (targets_auto_managed) {
        KDEBUG("[GIC] Platform mode: ITARGETSR is read-only/auto-managed\n");
        KDEBUG("[GIC] This is expected for this platform\n");
        
        /* Verifier les valeurs auto-configurees */
        for (uint32_t irq = 16; irq < 48; irq += 4) {
            uint8_t t0 = itargetsr[irq];
            uint8_t t1 = itargetsr[irq+1];
            uint8_t t2 = itargetsr[irq+2];
            uint8_t t3 = itargetsr[irq+3];
            
            if (t0 | t1 | t2 | t3) {  /* Si au moins une est non-zero */
                KDEBUG("[GIC] IRQ %u-%u targets: 0x%02X 0x%02X 0x%02X 0x%02X\n",
                       irq, irq+3, t0, t1, t2, t3);
            }
        }
    }
    
    /* === PHASE 4: ACTIVATION === */
    
    KDEBUG("[GIC] Enabling GIC...\n");
    gicd[0x000/4] = 0x01;  /* Enable distributor */
    gicc[0x000/4] = 0x01;  /* Enable CPU interface */
    
    /* === PHASE 5: CONFIGURATION IRQ IMPORTANTES === */
    
    //KDEBUG("[GIC] Enabling important IRQs...\n");
    /* IRQs coeur. Les drivers MMIO activent ensuite leurs IRQ propres. */
#if 1
    uint32_t important_irqs[] = {IRQ_UART, IRQ_KEYBOARD, IRQ_TIMER};
    
    for (int i = 0; i < (int)(sizeof(important_irqs) / sizeof(important_irqs[0])); i++) {
        uint32_t irq = important_irqs[i];
        uint8_t target;
        
        /*
         * Core platform interrupts used by ArmOS are level-triggered:
         * - ARM generic timer PPI
         * - PL011 UART SPI (GIC id 33)
         *
         * Treating PL011 as edge-triggered can lose RX interrupts when the
         * line is asserted while masked or already being serviced. That maps
         * exactly to the historical "long idle then no keyboard" failure mode.
         */
        if (irq >= 16) {
            volatile uint32_t* icfgr = (volatile uint32_t*)(LOCAL_GICD_BASE + 0xC00);
            uint32_t cfg = icfgr[irq / 16];
            uint32_t shift = (irq % 16) * 2;
            cfg &= ~(0b11 << shift);
            icfgr[irq / 16] = cfg;
        }
        
        /* 2. Route SPI interrupts to the boot CPU before enabling them. */
        if (irq >= 32)
            target = gic_route_spi_to_boot_cpu(irq);

        /* 3. Activer l'IRQ */
        uint32_t reg = irq / 32;
        uint32_t bit = irq % 32;
        gicd[0x100/4 + reg] |= (1 << bit);
        
        /* 4. Verifier l'etat */
        uint32_t enabled = gicd[0x100/4 + reg] & (1 << bit);
        target = itargetsr[irq];
        
        if (targets_auto_managed && target == 0) {
            KDEBUG("[GIC] IRQ %u: %s, target=AUTO OK\n", 
                   irq, enabled ? "ENABLED" : "disabled");
        } else {
            KDEBUG("[GIC] IRQ %u: %s, target=0x%02X %s\n", 
                   irq, enabled ? "ENABLED" : "disabled", target,
                   (target != 0) ? "OK" : "KO");
        }
    }
#endif
    
    /* === PHASE 6: VeRIFICATION FINALE === */
    
    uint32_t final_gicd = gicd[0x000/4];
    uint32_t final_gicc = gicc[0x000/4];
    
    KDEBUG("[GIC] Final status:\n");
    KDEBUG("[GIC]   GICD_CTLR: 0x%08X %s\n", final_gicd,
           (final_gicd & 1) ? "OK ACTIVE" : "KO INACTIVE");
    KDEBUG("[GIC]   GICC_CTLR: 0x%08X %s\n", final_gicc,
           (final_gicc & 1) ? "OK ACTIVE" : "KO INACTIVE");
    
    if ((final_gicd & 1) && (final_gicc & 1)) {
        KDEBUG("[GIC] - %s GIC ready! (%s mode)\n",
               arch_platform_name(),
               targets_auto_managed ? "auto-targeting" : "kernel-targeting");
    } else {
        KDEBUG("[GIC] KO GIC initialization failed\n");
    }
}

void gic_init_secondary_cpu_interface(void)
{
    volatile uint32_t* gicd = (volatile uint32_t*)LOCAL_GICD_BASE;
    volatile uint32_t* gicc = (volatile uint32_t*)LOCAL_GICC_BASE;
    volatile uint8_t* ipriority = (volatile uint8_t*)(LOCAL_GICD_BASE + 0x400);
    uint32_t sgi_reg = IRQ_SGI_TLB_SHOOTDOWN / 32;
    uint32_t sgi_bit = IRQ_SGI_TLB_SHOOTDOWN % 32;
    uint32_t timer_reg = IRQ_TIMER / 32;
    uint32_t timer_bit = IRQ_TIMER % 32;

    /*
     * Secondary CPUs enter here with TTBR1 enabled but outside the scheduler.
     * Configure only their private GICC state and the diagnostic SGI/PPI bit.
     * Device SPIs stay routed to the boot CPU until the drivers are SMP-safe.
     */
    gicc[0x000 / 4] = 0;
    gicc[0x004 / 4] = 0xF0;  /* GICC_PMR */
    gicc[0x008 / 4] = 0x03;  /* GICC_BPR */
    ipriority[IRQ_SGI_TLB_SHOOTDOWN] = 0xA0;
    ipriority[IRQ_TIMER] = 0xA0;
    gicd[0x100 / 4 + sgi_reg] |= (1u << sgi_bit);
    gicd[0x100 / 4 + timer_reg] |= (1u << timer_bit);
    gicc[0x000 / 4] = 0x01;

    data_sync_barrier();
    instruction_sync_barrier();
}

void fiq_c_handler(void)
{
    irq_c_handler();
}

void irq_c_handler(void)
{
    volatile uint32_t* gicc = (volatile uint32_t*)LOCAL_GICC_BASE;
    
    /* Lire l'IRQ ID */
    uint32_t irq_id = gicc[0x00C/4];  /* GICC_IAR */
    uint32_t int_id = irq_id & 0x3FF;
    uint32_t cpu_id = smp_processor_id();

    /*
     * GIC spurious interrupt: no active interrupt was acknowledged, so there
     * is nothing to service and no EOIR to write. Keep it silent; printing from
     * IRQ context can inject text into the interactive TTY input line.
     */
    if (int_id == 1023) {
        last_irq_id = int_id;
        return;
    }
    
    /* Compteur global */
    irq_count++;
    last_irq_id = int_id;
    if (int_id < GIC_IRQ_COUNTERS)
        irq_counts[int_id]++;
    smp_note_irq(cpu_id);
    
    /* Debug : afficher l'IRQ recue */
    //kprintf("[IRQ] DONE IRQ %u received! (count=%u)\n", int_id, irq_count);

    if (int_id == IRQ_SGI_TLB_SHOOTDOWN) {
        /*
         * Reserved SMP IPI. Parked secondaries can acknowledge TLB maintenance
         * without joining the scheduler or taking device interrupts.
         */
        smp_note_ipi(cpu_id);
        tlb_handle_remote_ipi(cpu_id);
        gicc[0x010/4] = irq_id;  /* GICC_EOIR */
        return;
    }

    if (platform_device_irq_dispatch(int_id)) {
        gicc[0x010/4] = irq_id;  /* GICC_EOIR */
        return;
    }

    if (int_id == virtio_blk_get_irq()) {
        virtio_block_irq_handler();
        gicc[0x010/4] = irq_id;  /* GICC_EOIR */
        return;
    }

    if (int_id == virtio_input_get_irq() ||
        virtio_input_handles_irq(int_id)) {
        virtio_input_irq_handler(int_id);
        gicc[0x010/4] = irq_id;  /* GICC_EOIR */
        return;
    }

    if (int_id == virtio_net_get_irq()) {
        virtio_net_irq_handler();
        gicc[0x010/4] = irq_id;  /* GICC_EOIR */
        return;
    }
    
    /* Route to the platform/core IRQ handlers. */
    switch (int_id) {
        case IRQ_TIMER:  /* Timer generique ARM */
            //kprintf("[IRQ] Generic Timer IRQ %u Received\n", int_id);
            timer_irq_handler();
            break;
            
        case IRQ_UART:  /* Console UART */
            //kprintf("[IRQ] UART IRQ %u Received\n", int_id);
            uart_irq_handler();
            break;

        case IRQ_KEYBOARD:
            /*
             * Legacy keyboard aliases are still routed through the console UART
             * until a platform provides a separate keyboard interrupt backend.
             */
            uart_irq_handler();
            break;
            
        /* VirtIO IRQs not used yet. Keep them visible and ignored here. */
        case IRQ_VIRTIO_CONSOLE:
        case IRQ_VIRTIO_RNG:
        case 79:
        case 48:
            //kprintf("[IRQ] Ignoring unused VirtIO IRQ %u\n", int_id);
            break;
            
        default:
            kprintf("[IRQ] Unknown IRQ %u\n", int_id);
            break;
    }
    
    /* CRITIQUE : Acquitter l'IRQ */
    gicc[0x010/4] = irq_id;  /* GICC_EOIR */
}

uint32_t gic_get_irq_count(uint32_t irq)
{
    if (irq >= GIC_IRQ_COUNTERS)
        return 0;
    return irq_counts[irq];
}

uint32_t gic_get_total_irq_count(void)
{
    return irq_count;
}

uint32_t gic_get_last_irq_id(void)
{
    return last_irq_id;
}

void gic_send_sgi(uint32_t target_cpu_mask, uint32_t sgi_id)
{
    if (sgi_id >= 16 || target_cpu_mask == 0)
        return;

    volatile uint32_t* gicd = (volatile uint32_t*)LOCAL_GICD_BASE;
    uint32_t value = ((target_cpu_mask & 0xFFu) << 16) | (sgi_id & 0xFu);

    /*
     * GICD_SGIR TargetListFilter=0 sends the SGI to the explicit CPU target
     * list in bits [23:16]. The caller passes a GIC CPU-interface target mask;
     * platform code is responsible for keeping that mask consistent.
     */
    gicd[0xF00 / 4] = value;
}

void gic_send_sgi_others(uint32_t sgi_id)
{
    if (sgi_id >= 16)
        return;

    volatile uint32_t* gicd = (volatile uint32_t*)LOCAL_GICD_BASE;

    /*
     * TargetListFilter=1 means "all CPU interfaces except the requester".
     * This is a useful SMP bring-up diagnostic because it avoids assuming that
     * MPIDR affinity bits and GIC target-list bits are identical.
     */
    gicd[0xF00 / 4] = (1u << 24) | (sgi_id & 0xFu);
}

static void enable_irq_with_type(uint32_t irq, bool edge_triggered)
{
    bool targets_auto_managed = arch_platform_irq_targets_auto_managed();

    KDEBUG("[IRQ] Enabling IRQ %u (%s, %s)...\n",
           irq,
           targets_auto_managed ? "auto-targeting" : "kernel-targeting",
           edge_triggered ? "edge" : "level");
    
    volatile uint32_t* gicd = (volatile uint32_t*)LOCAL_GICD_BASE;
    
    /* 1. Route SPIs to the boot CPU before enabling them. */
    uint8_t target = gic_route_spi_to_boot_cpu(irq);
    if (target == 0) {
        KDEBUG("[IRQ]   Target: platform auto-managed\n");
    } else {
        KDEBUG("[IRQ]   Target: 0x%02X \n", target);
    }
    
    /* 2. Configurer le type */
    if (irq >= 16) {
        volatile uint32_t* icfgr = (volatile uint32_t*)(LOCAL_GICD_BASE + 0xC00);
        uint32_t cfg = icfgr[irq / 16];
        uint32_t shift = (irq % 16) * 2;
        
        cfg &= ~(0b11 << shift);
        if (edge_triggered)
            cfg |=  (0b10 << shift);
        icfgr[irq / 16] = cfg;
        
        KDEBUG("[IRQ]   Type: %s OK\n",
               edge_triggered ? "Edge-triggered" : "Level-triggered");
    } else {
        KDEBUG("[IRQ]   Type: Level-triggered (SGI/PPI) OK\n");
    }
    
    /* 3. Configurer la priorite */
    volatile uint8_t* ipriority = (volatile uint8_t*)(LOCAL_GICD_BASE + 0x400);
    ipriority[irq] = 0xA0;
    KDEBUG("[IRQ]   Priority: 0xA0 OK\n");
    
    /* 4. Activer l'IRQ */
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    gicd[0x100/4 + reg] |= (1 << bit);
    
    /* Verifier l'activation */
    uint32_t enabled = gicd[0x100/4 + reg] & (1 << bit);
    KDEBUG("[IRQ]   Status: %s\n", enabled ? "ENABLED OK" : "FAILED KO");
    
    if (enabled) {
        KDEBUG("[IRQ] OK IRQ %u ready! (%s)\n",
               irq, targets_auto_managed ? "auto-route" : "explicit-route");
    } else {
        KDEBUG("[IRQ] KO IRQ %u activation failed\n", irq);
    }
}

static void enable_irq(uint32_t irq)
{
    /*
     * Default to level-triggered interrupts. ArmOS' current platform devices
     * (console UART, VirtIO MMIO and ARM generic timer) all use level
     * semantics; edge-triggering them risks losing interrupts that remain
     * asserted while masked.
     */
    enable_irq_with_type(irq, false);
}

static void enable_irq_level(uint32_t irq)
{
    enable_irq_with_type(irq, false);
}

static void disable_irq(uint32_t irq)
{
    volatile uint32_t* gicd = (volatile uint32_t*)LOCAL_GICD_BASE;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    
    gicd[0x180/4 + reg] |= (1 << bit);
}

/* Fonctions de test simplifiees sans redefinitions */
void test_timer_irq_virt(void)
{
    kprintf("[TEST] === TESTING ARM GENERIC TIMER ===\n");
    
    /* Utiliser ARM Generic Timer au lieu de SP804 */
    uint32_t freq = get_cntfrq();
    
    if (freq > 0) {
        kprintf("[TEST] Generic Timer frequency: %u Hz\n", freq);
        
        /* Configurer timer pour 1 seconde */
        uint32_t tval = freq;  /* 1 seconde */
        set_cntp_tval(tval);
        
        /* Activer timer */
        uint32_t ctl = 1;  /* Enable */
        set_cntp_ctl(ctl);
        
        /* Activer IRQ */
        enable_irq(IRQ_TIMER);
        
        kprintf("[TEST] Generic Timer configured, waiting for IRQ...\n");
    } else {
        kprintf("[TEST] Generic Timer not available\n");
    }
}

void comprehensive_irq_test_virt(void)
{
    kprintf("[TEST] === %s IRQ TEST ===\n", arch_platform_name());
    
    uint32_t initial_count = irq_count;
    
    kprintf("[TEST] Initial IRQ count: %u\n", initial_count);
    
    /* Test ARM Generic Timer */
    test_timer_irq_virt();
    
    /* Attendre */
    for (volatile int i = 0; i < 10000000; i++);
    
    uint32_t final_count = irq_count;
    uint32_t received = final_count - initial_count;
    
    kprintf("[TEST] === TEST SUMMARY ===\n");
    kprintf("[TEST] IRQs received during test: %u\n", received);
    kprintf("[TEST] Total IRQ count: %u\n", final_count);
    kprintf("[TEST] Last IRQ ID: %u\n", last_irq_id);
    
    if (received > 0) {
        kprintf("[TEST] DONE SUCCESS: platform interrupt system is WORKING!\n");
    } else {
        kprintf("[TEST] WARNING  No IRQs received - may need timer events\n");
    }
}

/* Simple VirtIO IRQ smoke test. */
void test_virtio_irq_virt(void)
{
    kprintf("[TEST] === TESTING VIRTIO IRQ (%s) ===\n", arch_platform_name());
    
    volatile uint32_t* virtio = (volatile uint32_t*)LOCAL_VIRTIO_BASE;
    
    /* Verifier la presence VirtIO */
    uint32_t magic = virtio[0x000/4];  /* VIRTIO_MAGIC */
    
    kprintf("[TEST] VirtIO Magic: 0x%08X %s\n", magic, 
            (magic == 0x74726976) ? "OK VALID" : "KO INVALID");
    
    if (magic == 0x74726976) {
        uint32_t version = virtio[0x004/4];   /* VIRTIO_VERSION */
        uint32_t device_id = virtio[0x008/4]; /* VIRTIO_DEVICE_ID */
        
        kprintf("[TEST]   Version: %u\n", version);
        kprintf("[TEST]   Device ID: %u\n", device_id);
        
        /* Activer les IRQ VirtIO */
        enable_irq(IRQ_VIRTIO_NET);
        enable_irq(IRQ_VIRTIO_BLOCK);
        
        /* Reset et configure device */
        virtio[0x070/4] = 0;      /* Reset */
        virtio[0x070/4] = 1;      /* Acknowledge */
        
        kprintf("[TEST] VirtIO configured for %s\n", arch_platform_name());
    } else {
        kprintf("[TEST] No VirtIO device found at 0x%08X\n", LOCAL_VIRTIO_BASE);
    }
}

/* Fonction de test principale a appeler depuis kernel_main */
void complete_gic_debug_virt(void)
{
    kprintf("\n");
    kprintf("- === %s GIC DEBUG ===\n", arch_platform_name());
    kprintf("Testing platform interrupt system...\n");
    kprintf("\n");
    
    /* Test 1: Timer generique ARM */
    test_timer_irq_virt();
    kprintf("\n");
    
    /* Test 2: VirtIO */
    test_virtio_irq_virt();
    kprintf("\n");
    
    /* Test 3: Test complet */
    comprehensive_irq_test_virt();
    
    kprintf("=== PLATFORM IRQ TESTING COMPLETE ===\n");
    kprintf("DONE Your platform interrupt system is ready!\n");
}

/* Fonctions aliases pour compatibilite */
void set_irq_edge_triggered(uint32_t irq)
{
    volatile uint32_t* icfgr = (volatile uint32_t*)(LOCAL_GICD_BASE + 0xC00);
    if (irq >= 16) {
        uint32_t cfg = icfgr[irq / 16];
        uint32_t shift = (irq % 16) * 2;
        cfg &= ~(0b11 << shift);
        cfg |=  (0b10 << shift);
        icfgr[irq / 16] = cfg;
        KDEBUG("[IRQ] IRQ %u configured as edge-triggered\n", irq);
    }
}

void arch_irq_controller_init(void)
{
    init_gic();
}

void arch_irq_init_local_cpu_interface(void)
{
    gic_init_secondary_cpu_interface();
}

const char* arch_irq_controller_name(void)
{
    return "GIC: v2";
}

uint32_t arch_irq_controller_line_count(void)
{
    return gic_irq_line_count;
}

void arch_irq_enable(uint32_t irq)
{
    enable_irq(irq);
}

void arch_irq_enable_level(uint32_t irq)
{
    enable_irq_level(irq);
}

void arch_irq_disable(uint32_t irq)
{
    disable_irq(irq);
}

void arch_irq_ack(uint32_t irq)
{
    volatile uint32_t* gicc = (volatile uint32_t*)LOCAL_GICC_BASE;
    gicc[0x010/4] = irq;
}

uint32_t arch_irq_get_count(uint32_t irq)
{
    return gic_get_irq_count(irq);
}

uint32_t arch_irq_get_total_count(void)
{
    return gic_get_total_irq_count();
}

uint32_t arch_irq_get_last_id(void)
{
    return gic_get_last_irq_id();
}

void arch_irq_send_ipi(uint32_t target_cpu_mask, uint32_t irq)
{
    gic_send_sgi(target_cpu_mask, irq);
}

void arch_irq_send_ipi_others(uint32_t irq)
{
    gic_send_sgi_others(irq);
}
