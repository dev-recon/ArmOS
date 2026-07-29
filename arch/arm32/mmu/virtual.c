/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/mmu/virtual.c
 * Layer: ARM32 / user virtual memory and page-table mappings
 *
 * Responsibilities:
 * - Manage physical pages, virtual address spaces, MMU mappings, and ASIDs.
 * - Support user mappings, page faults, and copy-on-write.
 *
 * Notes:
 * - TLB, ASID, and TTBR changes are global stability concerns.
 */

#include <kernel/memory.h>
#include <kernel/address_space.h>
#include <kernel/kprintf.h>
#include <kernel/string.h>
#include <kernel/util.h>
#include <kernel/task.h>
#include <kernel/process.h>
#include <asm/arm.h>
#include <asm/mmu.h>

static pgdir_t allocate_pgdir(void);
static bool vm_l1_entry_is_coarse(uint32_t entry);
static paddr_t vm_l1_coarse_base(uint32_t entry);
static bool vm_phys_page_is_freeable(paddr_t phys_addr, const char* owner);

static inline pgdir_cpu_t vm_pgdir_handle_cpu_view(pgdir_t pgdir)
{
    vaddr_t address = (vaddr_t)(uintptr_t)pgdir;

    if (virt_in_direct_map(address))
        return (pgdir_cpu_t)pgdir;
    if (phys_in_direct_map((paddr_t)address))
        return (pgdir_cpu_t)phys_to_virt((paddr_t)address);
    return (pgdir_cpu_t)pgdir;
}

static inline pgdir_cpu_t vm_pgdir_cpu_view(vm_space_t *vm)
{
    if (!vm || !vm->pgdir)
        return NULL;
    return vm_pgdir_handle_cpu_view(vm->pgdir);
}

static inline void vm_flush_page_table_entry(const void *entry)
{
    dc_clean_mva((void *)entry);
    data_sync_barrier_inner_shareable_write();
}

int unmap_user_page(pgdir_t pgdir, vaddr_t address, uint32_t asid)
{
    uint32_t l1_index;
    uint32_t l2_index;
    l1_entry_t *l1_entry;
    l2_table_t l2_table;
    paddr_t l2_phys;
    uint32_t index;

    if (!pgdir || address >= get_split_boundary() ||
        (address & PAGE_OFFSET_MASK))
        return -EINVAL;

    l1_index = get_L1_index(address);
    l2_index = L2_INDEX(address);
    l1_entry = &vm_pgdir_handle_cpu_view(pgdir)[l1_index];
    if ((*l1_entry & 0x3u) != 0x1u)
        return -EINVAL;

    l2_phys = *l1_entry & 0xFFFFFC00u;
    l2_table = (l2_table_t)phys_to_virt(l2_phys);
    if ((l2_table[l2_index] & 0x3u) == 0)
        return 0;

    l2_table[l2_index] = 0;
    vm_flush_page_table_entry(&l2_table[l2_index]);
    invalidate_tlb_page_asid(address, asid);

    for (index = 0; index < 256u; index++) {
        if ((l2_table[index] & 0x3u) != 0)
            return 0;
    }

    *l1_entry = 0;
    vm_flush_page_table_entry(l1_entry);
    invalidate_tlb_page_asid(address, asid);
    free_page((void *)(uintptr_t)l2_phys);
    return 0;
}

#define PGDIR_SIZE 7

static void install_low_kernel_aliases(vm_space_t *vm)
{
    pgdir_cpu_t pgdir_v = vm_pgdir_cpu_view(vm);
    vaddr_t split = get_split_boundary();
    vaddr_t start = memory_low_kernel_alias_start();
    vaddr_t end = memory_low_kernel_alias_end();

    if (!pgdir_v || KERNEL_START >= split)
        return;

    if (end > split)
        end = split;

    /*
     * Low-linked platforms execute kernel text/data/heap below the TTBR split.
     * While a user process is active, exceptions keep TTBR0 selected, so those
     * sections must be visible in the process page directory as privileged-only
     * aliases. User mode still faults if it tries to touch them.
     */
    for (vaddr_t addr = start; addr < end; addr += 0x100000u) {
        uint32_t index = get_L1_index(addr);
        pgdir_v[index] = addr |
                         0x00000002u |  /* Section descriptor. */
                         0x00000400u |  /* AP[1:0] = 01: privileged RW. */
                         0x00000004u |  /* B: bufferable. */
                         0x00000008u |  /* C: cacheable. */
                         0x00010000u;   /* S: coherent across SMP CPUs. */
    }
}

vm_space_t *create_vm_space(void)
{
    vm_space_t *vm = kmalloc(sizeof(vm_space_t));
    if (!vm)
        return NULL;

    // KDEBUG("create_vm_space: allocating user page directory with ASID and %s\n", is_kernel_space ? "KERNEL" : "USER");

    /* Allouer un ASID unique pour ce processus */
    uint32_t asid = vm_allocate_asid();
    if (asid == 0)
    {
        KERROR("create_vm_space: Failed to allocate ASID\n");
        kfree(vm);
        return NULL;
    }

    /* Allouer le page directory utilisateur et garder sa base brute.
     * TTBR0 exige un alignement 16 KB ; vm->pgdir peut donc pointer plus loin
     * que la base retournee par l'allocateur.
     */
    vm->pgdir_alloc = allocate_pgdir();
    vm->pgdir = vm->pgdir_alloc;

    if (!vm->pgdir)
    {
        vm_free_asid(asid);
        kfree(vm);
        return NULL;
    }

    /* Diagnostic d'alignement pour TTBR0 */
    paddr_t pgdir_addr = (paddr_t)vm->pgdir;
    uint32_t alignment_check = pgdir_addr & 0x3FFF;

    if (alignment_check != 0)
    {
        // KWARN("create_vm_space: user pgdir NOT 16KB aligned!\n");

        /* Forcer l'alignement si nécessaire */
        paddr_t aligned_addr = (pgdir_addr + 0x3FFF) & ~0x3FFF;

        // KDEBUG("  Forced alignment: 0x%08X -> 0x%08X\n", pgdir_addr, aligned_addr);

        /* Vérifier que l'adresse alignée est dans la zone allouée */
        if (aligned_addr >= pgdir_addr && aligned_addr < pgdir_addr + (4 * PAGE_SIZE))
        {
            vm->pgdir = (pgdir_t)aligned_addr;
            // KINFO("create_vm_space: Using aligned address 0x%08X\n", aligned_addr);
        }
        else
        {
            KERROR("create_vm_space: Aligned address out of bounds!\n");
            vm_free_asid(asid);
            free_pages(vm->pgdir_alloc, PGDIR_SIZE);
            kfree(vm);
            return NULL;
        }
    }
    else
    {
        // KINFO("create_vm_space: user pgdir already 16KB aligned ✓\n");
    }

    /*
     * TTBCR.N=2 limite TTBR0 aux indices L1 0..1023, soit une table de 4 Kio.
     * vm->pgdir est aligne vers l'avant dans l'allocation PGDIR_SIZE; effacer
     * les 7 pages depuis cette adresse alignee pourrait depasser l'allocation.
     */
    memset(vm_pgdir_cpu_view(vm), 0, PAGE_SIZE);

    install_low_kernel_aliases(vm);

    vm->arch_private = NULL;
    vm_initialize_user_layout(vm);
    vm->asid = asid; /* Nouveau champ ASID */

    // KDEBUG("create_vm_space: Created VM space with ASID %u\n", asid);
    return vm;
}

static pgdir_t allocate_pgdir(void)
{
    /* Allouer assez de pages pour pouvoir choisir une base TTBR0 alignee 16 KB. */
    pgdir_t pgdir = (pgdir_t)allocate_pages(PGDIR_SIZE);
    if (!pgdir)
    {
        KERROR("allocate_user_pgdir: Failed to allocate pages\n");
        return NULL;
    }

    return pgdir;
}

static bool vm_l1_entry_is_coarse(uint32_t entry)
{
    return (entry & 0x3) == 0x1;
}

static paddr_t vm_l1_coarse_base(uint32_t entry)
{
    return entry & 0xFFFFFC00;
}

static uint32_t vm_user_l1_entries(void)
{
    vaddr_t split = get_split_boundary();
    if (split == 0)
        return 0;
    return get_L1_index(split - 1) + 1;
}

uint32_t vm_resident_kb(vm_space_t *vm)
{
    uint32_t pages = 0;
    pgdir_cpu_t pgdir_v;
    uint32_t l1_entries;

    if (!vm || !vm->pgdir)
        return 0;

    pgdir_v = vm_pgdir_cpu_view(vm);
    if (!pgdir_v)
        return 0;

    l1_entries = vm_user_l1_entries();
    for (uint32_t i = 0; i < l1_entries; i++) {
        l1_entry_t l1_entry = pgdir_v[i];
        if (!vm_l1_entry_is_coarse(l1_entry))
            continue;

        l2_table_t l2_table = (l2_table_t)phys_to_virt(vm_l1_coarse_base(l1_entry));
        for (uint32_t j = 0; j < 256; j++) {
            if ((l2_table[j] & PTE_TYPE_MASK) != PTE_TYPE_FAULT)
                pages++;
        }
    }

    return (pages * PAGE_SIZE) / 1024;
}

uint32_t vm_page_table_count(vm_space_t *vm)
{
    uint32_t count = 0;
    pgdir_cpu_t pgdir_v;
    uint32_t l1_entries;

    if (!vm || !vm->pgdir)
        return 0;

    pgdir_v = vm_pgdir_cpu_view(vm);
    if (!pgdir_v)
        return 0;

    l1_entries = vm_user_l1_entries();
    for (uint32_t i = 0; i < l1_entries; i++) {
        if (vm_l1_entry_is_coarse(pgdir_v[i]))
            count++;
    }

    return count;
}

static bool vm_phys_page_is_freeable(paddr_t phys_addr, const char* owner)
{
    if (!phys_addr) {
        return false;
    }

    if (!IS_PAGE_ALIGNED(phys_addr) || !IS_VALID_RAM(phys_addr)) {
        KERROR("destroy_vm_space: refusing to free invalid %s page 0x%08X\n",
               owner, phys_addr);
        return false;
    }

    return true;
}

void destroy_vm_space(vm_space_t *vm)
{
    vma_t *vma;
    vaddr_t vaddr;
    paddr_t phys_addr;

    if (!vm)
        return;

    //KDEBUG("destroy_vm_space: Destroying VM space 0x%08X , PGDIR = 0x%08X, with ASID %u\n", (uint32_t)vm, (uint32_t)vm->pgdir, vm->asid);

    /* Free all VMAs */
    vma = vm->vma_list;
    while (vma) {
        /* Free pages in this VMA.
         *
         * VMAs may describe byte ranges, but the page tables map whole pages.
         * Always walk the page-aligned backing range; otherwise a non-aligned
         * VMA start would make get_physical_address() return phys+offset and
         * free_page() would correctly reject it as an invalid page address.
         */
        vaddr_t page_start = PAGE_ALIGN_DOWN(vma->start);
        vaddr_t page_end = PAGE_ALIGN_UP(vma->end);
        for (vaddr = page_start; vaddr < page_end; vaddr += PAGE_SIZE)
        {
            phys_addr = get_physical_address(vm->pgdir, vaddr);
            if (!(vma->flags & VMA_EXTERNAL) &&
                vm_phys_page_is_freeable(phys_addr, "user"))
            {
                //KDEBUG("destroy_vm_space: Freeing page 0x%08X\n", phys_addr);
                free_page((void *)phys_addr);
            }
        }

        vma = vma->next;
    }

    /* Les tables L2 sont des pages physiques distinctes des pages utilisateur. */
    pgdir_cpu_t pgdir_v = vm_pgdir_cpu_view(vm);
    for (uint32_t l1_index = 0; l1_index < 1024; l1_index++)
    {
        uint32_t l1_entry = pgdir_v[l1_index];
        if (vm_l1_entry_is_coarse(l1_entry))
        {
            paddr_t l2_phys = vm_l1_coarse_base(l1_entry);
            pgdir_v[l1_index] = 0;
            if (vm_phys_page_is_freeable(l2_phys, "L2 table")) {
                free_page((void *)l2_phys);
            }
        }
    }

    /* Libérer l'ASID avant de détruire le page directory */
    vm_free_asid(vm->asid);
    // KDEBUG("destroy_vm_space: Freed ASID %u\n", vm->asid);

    /* Free user page directory from the original allocation base. */
    free_pages(vm->pgdir_alloc ? vm->pgdir_alloc : vm->pgdir, PGDIR_SIZE);
    vm_release_vmas(vm);
    kfree(vm);
}

int handle_cow_fault(vaddr_t fault_addr)
{
    task_t *task = task_current_local();
    process_t *process = task_get_process(task);

    if (!process || !process->vm) {
        return -EINVAL;
    }

    vm_space_t* vm = process->vm;
    vaddr_t vaddr = fault_addr & ~(PAGE_SIZE - 1);
    vma_t* vma = find_vma(vm, fault_addr);
    if (!vma || !(vma->flags & VMA_WRITE)) {
        return -EACCES;
    }

    pte_ptr_t pte = get_user_pte(vm->pgdir, vaddr);
    if (!pte || ((*pte & PTE_TYPE_MASK) == PTE_TYPE_FAULT)) {
        return -EFAULT;
    }

    if ((*pte & PTE_AP_MASK) != PTE_AP_RW_RO) {
        return -EACCES;
    }

    paddr_t old_phys = *pte & PTE_SMALL_BASE;
    uint16_t refs = page_ref_count((void*)old_phys);
    if (refs == 0) {
        return -EFAULT;
    }

    if (refs == 1) {
        return set_user_page_writable(vm->pgdir, vaddr, vm->asid);
    }

    void* new_page = allocate_page();
    if (!new_page) {
        return -ENOMEM;
    }

    memcpy((void*)phys_to_virt((paddr_t)new_page),
           (void*)phys_to_virt(old_phys),
           PAGE_SIZE);

    int ret = remap_user_page(vm->pgdir, vaddr, (paddr_t)new_page,
                              vma->flags, vm->asid);
    if (ret < 0) {
        free_page(new_page);
        return ret;
    }

    free_page((void*)old_phys);
    return 0;
}

void switch_to_vm_space(vm_space_t *vm)
{
    if (!vm)
    {
        KERROR("switch_to_vm_space: NULL vm space\n");
        return;
    }

    switch_address_space_with_asid(vm->pgdir, vm->asid);
}
