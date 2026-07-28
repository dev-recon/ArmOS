ENTRY(_start)

KERNEL_LINK_ADDR = DEFINED(KERNEL_LINK_ADDR) ? KERNEL_LINK_ADDR : 0x40010000;
RAM_BASE_ADDR = DEFINED(RAM_BASE_ADDR) ? RAM_BASE_ADDR : 0x40000000;
RAM_END_ADDR = DEFINED(RAM_END_ADDR) ? RAM_END_ADDR : 0xC0000000;

MEMORY
{
    RAM (rwx) : ORIGIN = KERNEL_LINK_ADDR, LENGTH = RAM_END_ADDR - KERNEL_LINK_ADDR
}

SECTIONS
{
    . = KERNEL_LINK_ADDR;

    /* Vecteurs d'interruptions ARM (alignes 32 bytes) */
    .vectors ALIGN(32) : {
        KEEP(*(.vectors))
    } > RAM
    
    /* Symboles de debut */
    __start = .;
    __kernel_start = .;
    __kernel_phys_start = .;
    
    /* Code */
    .text.boot : {
        *(.text.boot)
    } > RAM
    
    .text : {
        __text_start = .;
        *(.text)
        *(.text.*)
        __text_end = .;
    } > RAM
    
    /* Donnees en lecture seule */
    .rodata : {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        __rodata_end = .;
    } > RAM
    
    /* Donnees initialisees */
    .data : {
        __data_start = .;
        *(.data)
        *(.data.*)
        __data_end = .;
    } > RAM
    
    /* Donnees non initialisees */
    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);  /* Aligner sur page */
        __bss_end = .;
    } > RAM
    
    /* Pile du kernel */
    .stack : {
        . = ALIGN(4096);
        __stack_bottom = .;
        . += 32768;  /* 32 KiB */
        __stack_top = .;
    } > RAM

    /* Pile pour le mode ABT (Abort) */
    .abtpile : {
        . = ALIGN(8);
        __abt_stack_bottom = .;
        . += 4096;            /* 4 KiB */
        __abt_stack_top = .;
    } > RAM

    /* Pile pour le mode IRQ */
    .irqpile : {
        . = ALIGN(8);
        __irq_stack_bottom = .;
        . += 4096;            /* 4 KiB */
        __irq_stack_top = .;
    } > RAM

    /* Pile pour le mode UND (Undefined instruction) */
    .undpile : {
        . = ALIGN(8);
        __und_stack_bottom = .;
        . += 4096;            /* 4 KiB */
        __und_stack_top = .;
    } > RAM

    /* Pile pour le mode SVC */
    .svcpile : {
        . = ALIGN(8);
        __svc_stack_bottom = .;
        . += 8192;            /* 8 KiB for the main SVC mode */
        __svc_stack_top = .;
    } > RAM

    /*
     * Early per-CPU parking stacks for secondary CPUs.
     *
     * They are used before the normal task scheduler exists. CPU0 keeps the
     * historical boot stack; CPU1..CPU3 can safely park here while waiting for
     * the future SMP release path.
     */
    .smpstacks : {
        . = ALIGN(4096);
        __smp_stack_bottom = .;
        . += 4096 * 4;
        __smp_stack_top = .;
    } > RAM

    /*
     * Per-CPU exception stacks for secondary CPUs. They are initialized before
     * a secondary enters the C holding pen, but IRQs stay masked until the real
     * SMP interrupt path is enabled.
     */
    .smpirqstacks : {
        . = ALIGN(4096);
        __smp_irq_stack_bottom = .;
        . += 4096 * 4;
        __smp_irq_stack_top = .;
    } > RAM

    .smpabtstacks : {
        . = ALIGN(1024);
        __smp_abt_stack_bottom = .;
        . += 4096 * 4;
        __smp_abt_stack_top = .;
    } > RAM

    .smpundstacks : {
        . = ALIGN(1024);
        __smp_und_stack_bottom = .;
        . += 4096 * 4;
        __smp_und_stack_top = .;
    } > RAM
    
    /* === HEAP AUTOMATIQUE === */
    . = ALIGN(4096);
    __end = .;
    __kernel_end = .;
    __kernel_phys_end = .;
    
    /* Heap commence apres le kernel */
    __heap_start = .;
    
    /* Reserver de l'espace pour le heap */
    .heap : {
        . = ALIGN(4096);
        __heap_start = .;
        . += 8M;  /* 8MB de heap (augmente pour virt) */
        __heap_end = .;
    } > RAM
    
    /* Zone libre apres le heap */
    __free_memory_start = .;
    
    /* Calculer les tailles */
    __kernel_size = __kernel_end - __kernel_start;
    __heap_size = __heap_end - __heap_start;
    
    /* Limite de la RAM pour machine virt */
    __ram_start = RAM_BASE_ADDR;
    __ram_end = RAM_END_ADDR;
    __ram_size = __ram_end - __ram_start;
    
    /* Sections de debug */
    .debug_info 0 : { *(.debug_info) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_line 0 : { *(.debug_line) }
    .debug_str 0 : { *(.debug_str) }
    
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }
}
