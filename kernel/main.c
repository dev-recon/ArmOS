/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/main.c
 * Layer: Kernel / bootstrap orchestration
 *
 * Responsibilities:
 * - Bring up core kernel subsystems in boot order.
 * - Report high-level boot status and enter the scheduler.
 *
 * Notes:
 * - Platforms register devices while common code owns logical console policy.
 */

#include <kernel/arch_platform.h>
#include <kernel/config.h>
#include <kernel/linker.h>
#include <kernel/memory.h>
#include <kernel/process.h>
#include <kernel/interrupt.h>
#include <kernel/timer.h>
#include <kernel/version.h>
#include <kernel/display.h>
#include <kernel/platform_devices.h>
#include <kernel/vfs.h>
#include <kernel/smp.h>
#include <kernel/tlb.h>
#include <kernel/arch_cpu.h>
#include <kernel/uart.h>
#include <kernel/panic.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/debug_print.h>
#include <kernel/stdarg.h>
#include <kernel/disk_layout.h>
#include <kernel/device_service.h>
#include <kernel/windowserver.h>

#include <kernel/task.h>
#include <kernel/kernel_tasks.h>
#include <kernel/usb.h>
#include <kernel/net/stack.h>

#include <kernel/tty.h>
#include <kernel/exceptions.h>

/* Static stack-protector canary used by freestanding kernel builds. */
uintptr_t __stack_chk_guard = 0xDEADBEEF;

void __attribute__((noreturn)) __stack_chk_fail(void) {
    panic("Stack smashing detected");
    while (1) {}
}


void init_early_uart(void)
{
    volatile uint32_t* uart =
        (volatile uint32_t*)(uintptr_t)arch_platform_uart0_phys_base();
    
    /* Disable UART */
    uart[0x30/4] = 0;
    
    /* Configure baud rate (38400) */
    uart[0x24/4] = 1;   /* UARTIBRD */
    uart[0x28/4] = 40;  /* UARTFBRD */
    
    /* Configure format: 8N1 */
    uart[0x2C/4] = (3 << 5);
    
    /* Enable UART, TX, RX */
    uart[0x30/4] = (1 << 0) | (1 << 8) | (1 << 9);
}

void panic(const char* message)
{
    arch_disable_interrupts();
    uart_puts("KERNEL PANIC: ");
    uart_puts(message);
    uart_puts("\n");
    
    while (1) {
        arch_wait_for_interrupt();
    }
}


/* Forward declarations */
void early_init(void); 
void kernel_main(void);

static uint32_t boot_timer_frequency(void)
{
    return arch_timer_frequency();
}

static uint32_t boot_bogomips_x100(uint32_t timer_freq)
{
    /*
     * Early Linux printed a delay-loop calibration as BogoMIPS. In ArmOS this
     * is a stable, readable estimate tied to the active platform timer.
     */
    return timer_freq / 5000;
}

void early_init(void)
{
    /* Phase 1: Detection memoire pour MMU */
    kernel_memory_size = detect_memory();
    KBOOT_OKF("Memory: detected %uMB", kernel_memory_size / (1024 * 1024));
    
    if (kernel_memory_size > 0) {
        /* Phase 2: Setup MMU - AVANT tout allocateur */
        if (setup_mmu()) {
            KBOOT_OK("MMU: setup");
        } else {
            panic("MMU setup FAILED - cannot continue");
        }
    } else {
        panic("Memory detection returned suspicious value");
    } 
}


/* Ajoutez cette fonction dans votre kernel */
void test_heap_health(const char* phase_name)
{
    kprintf("=== HEAP TEST: %s ===\n", phase_name);
    
    void* test = kmalloc(64);
    if (test) {
        kprintf("OK %s: Heap OK\n", phase_name);
        kfree(test);
    } else {
        kprintf("KO %s: HEAP CORRUPTED!\n", phase_name);
        
        /* Debug detaille */
        extern void* free_list;  /* Ajustez selon votre implementation */
        kprintf("  free_list: %p\n", free_list);
        
        if (free_list) {
            /* Essayez de lire la premiere structure (prudemment) */
            volatile uint32_t* ptr = (volatile uint32_t*)free_list;
            kprintf("  first_block[0]: 0x%08X\n", ptr[0]);
            kprintf("  first_block[1]: 0x%08X\n", ptr[1]);
        }
    }
}

/*
 * Point d'entree principal du kernel.
 */
void kernel_main(void)
{
    arch_cpuinfo_t boot_cpuinfo;
    uint32_t timer_freq;
    uint32_t bogo_x100;
    uint32_t total_mb;
    uint32_t available_mb;
    platform_devices_state_t platform_devices;
    bool block_ready;
    bool rootfs_ready = false;

    tty_init();
    platform_console_early_init();

    /* Phase 0: etats du processeur */
    arch_enable_async_abort();

    arch_enable_smp_coherency();
    smp_init_boot_cpu();

    timer_freq = boot_timer_frequency();
    bogo_x100 = boot_bogomips_x100(timer_freq);
    arch_get_cpuinfo(&boot_cpuinfo);

    KBOOT("\n");
    KBOOT(KBOOT_COLOR_INFO ARMOS_VERSION_BANNER " %s" KBOOT_COLOR_RESET "\n",
          arch_machine_name());
    KBOOT_OKF("CPU: %s", boot_cpuinfo.model_name ? boot_cpuinfo.model_name : "unknown");
    KBOOT_OKF("Calibrating delay loop... %u.%02u BogoMIPS",
                bogo_x100 / 100, bogo_x100 % 100);
    
    if (!init_memory()) {  // <- Allocateur physique EN PREMIER
        KBOOT_FAIL("Memory: physical allocator");
        panic("init_memory failed");
    }
    KBOOT_OK("Memory: physical allocator");

    /* Phase 1: Initialisation critique (MMU, memoire de base) */
    early_init();  // MMU + detection memoire
    KBOOT_OK("Memory: early init");

    total_mb = kernel_memory_size / (1024 * 1024);
    available_mb = (get_free_page_count() * PAGE_SIZE) / (1024 * 1024);
    KBOOT_OKF("Memory: %uMB total, %uMB available", total_mb, available_mb);
    KBOOT_OKF("Kernel: %p-%p", (void *)(uintptr_t)KERNEL_START,
              (void *)(uintptr_t)KERNEL_END);
    KBOOT_OKF("MMU: split TTBR enabled, ASID pool %u", ASID_MAX);
   
    setup_svc_stack();

    /* Phase 3: Controleurs materiels de base */
    irq_init_controller();
    platform_console_enable_rx();
    KBOOT_OKF("%s, %u IRQs", irq_controller_name(), irq_controller_line_count());

    //init_timer_software();
    init_timer();
    KBOOT_OKF("Timer: ARM generic timer @ %u Hz, tick %u us",
                timer_freq, 1000000 / TIMER_FREQ);

    smp_start_secondary_cpus();
    KBOOT_OKF("SMP: %u CPU(s) configured, %u online, seen=0x%X",
              smp_possible_cpu_count(), smp_online_cpu_count(),
              smp_seen_cpu_mask());

    /* Phase 4: Peripheriques d'entree/sortie */
    platform_devices = platform_devices_init();

    /* Phase 6: Activation des interruptions */
    arch_enable_interrupts();

    /* Phase 7: Systemes de fichiers (OPTIONNEL) */
    block_ready = platform_block_init();

    if (!block_ready) {
        KBOOT_WARN("VFS: skipped, no block device");
    } else if (!init_vfs()) {
        KBOOT_WARN("VFS: mount failed");
    } else {
        const disk_partition_t* ext2_part = disk_partition_get(DISK_PART_EXT2_ROOT);
        const disk_partition_t* fat32_part = disk_partition_get(DISK_PART_FAT32_MNT);
        KBOOT_OKF("Partition: %s ext2 %uMB",
                  ext2_part ? ext2_part->name : "disk0p1",
                  ext2_part ? (uint32_t)(ext2_part->sector_count / 2048ULL) : 0);
        KBOOT_OKF("Partition: %s fat32 %uMB",
                  fat32_part ? fat32_part->name : "disk0p2",
                  fat32_part ? (uint32_t)(fat32_part->sector_count / 2048ULL) : 0);
        KBOOT_OKF("VFS: mounted ext2 on /");
        KBOOT_OKF("VFS: mounted proc on /proc");
        KBOOT_OKF("VFS: fat32 available on /mnt (manual)");
        rootfs_ready = true;
    }    

    /* Phase 5: Gestion des processus (APReS allocateurs) */
    if (rootfs_ready) {
        init_process_system();
    } else {
        init_kernel_task_system();
        KBOOT_WARN("Init: skipped, no root filesystem");
    }
    timer_enable_scheduling();

    if (rootfs_ready)
        device_services_start();

    if (rootfs_ready) {
        if (coredumpd_start() == 0)
            KBOOT_OK("Core: coredump daemon");
        else
            KBOOT_WARN("Core: coredump daemon unavailable");
    }

    if (platform_devices.display_ready) {
        if (display_start_daemon() == 0)
            KBOOT_OK("Display: cursor daemon");
        else
            KBOOT_WARN("Display: cursor daemon unavailable");
    }

    {
        int usb_daemon = usb_start_daemon();

        if (usb_daemon > 0)
            KBOOT_OK("USB: service daemon");
        else if (usb_daemon < 0)
            KBOOT_WARN("USB: service daemon unavailable");
    }

    {
        int net_daemon = net_start_daemon();

        if (net_daemon > 0)
            KBOOT_OK("Net: service daemon");
        else if (net_daemon < 0)
            KBOOT_WARN("Net: service daemon unavailable");
    }

    KBOOT_OK("Process: scheduler ready");

    if (smp_possible_cpu_count() > 1) {
        KBOOT_OK("SMP: TLB/IPI preflight");
        tlb_shootdown_all();
        /*
         * Admit secondary schedulers while PID 1 is still outside the
         * runqueue. This keeps late SMP boot messages from interleaving with
         * init or the first shell banner on a framebuffer console.
         */
        scheduler_prepare_secondary_cpus();
    }

    if (rootfs_ready)
        process_release_init();

    if (rootfs_ready && platform_devices.display_ready) {
        if (windowserverd_start() == 0)
            KBOOT_OK("WindowServer: userland compositor");
        else
            KBOOT_WARN("WindowServer: userland compositor unavailable");
    }

    /* Main scheduler loop */
    sched_start();

    panic("Returned from unified scheduler!");

}
