/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/mmu/user_vm.c
 * Layer: ARM64 / user address spaces
 *
 * Responsibilities:
 * - Own user tables, mapped pages and ASID allocation.
 * - Grow resident-page metadata and table inventories as mappings evolve.
 * - Provide transactional page mapping and table reclamation.
 * - Clone resident private pages and retain resident shared pages.
 * - Bind the ARM64 backend to the generic vm_space_t identity.
 * - Track mapping generations and resident ASID translations.
 * - Avoid redundant TLBI operations for unchanged address spaces.
 *
 * Notes:
 * - VMA, brk, mmap and page-fault policy belong to the common VM layer.
 */

#include <asm/mmu.h>
#include <asm/user_vm.h>
#include <kernel/string.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/tlb.h>

static int arm64_vm_alloc_pages(unsigned int count, paddr_t *address)
{
    void *pages;

    pages = allocate_pages(count);
    if (!pages)
        return -1;
    *address = (paddr_t)(uintptr_t)pages;
    memset(pages, 0, (size_t)count * PAGE_SIZE);
    return 0;
}

static int arm64_vm_free_pages(paddr_t address, unsigned int count)
{
    free_pages((void *)(uintptr_t)address, count);
    return 0;
}

static void arm64_vm_release_user_page(paddr_t address)
{
    free_page((void *)(uintptr_t)address);
}

#define ARM64_ASID_COUNT       256u
#define ARM64_ASID_BITMAP_SIZE (ARM64_ASID_COUNT / 8u)
#define ARM64_TTBR_TABLE_MASK  0x0000FFFFFFFFF000ULL

static uint8_t arm64_asid_bitmap[ARM64_ASID_BITMAP_SIZE];
static uint32_t arm64_asid_slot_generation[ARM64_ASID_COUNT];
static uint32_t arm64_asid_generation = 1;
static uint32_t next_tlb_generation = 1;
static uint64_t tlb_flush_count;
static uint64_t tlb_preserve_count;
static spinlock_t arm64_asid_lock = SPINLOCK_INIT("arm64_asid_pool");

typedef struct {
    paddr_t table;
    uint32_t generation;
} arm64_asid_residency_t;

static arm64_asid_residency_t
    arm64_asid_residency[ARMOS_MAX_CPUS][ARM64_ASID_COUNT];
static arm64_user_vm_t *arm64_asid_active_vm[ARMOS_MAX_CPUS];

static void arm64_user_vm_publish_targeted_generation(arm64_user_vm_t *vm)
{
    unsigned int hardware_asid =
        vm->asid & (ARM64_ASID_COUNT - 1u);
    uint32_t cpu;

    /* The preceding TLBI ...IS invalidated this mapping on every PE. */
    for (cpu = 0; cpu < ARMOS_MAX_CPUS; cpu++) {
        arm64_asid_residency_t *residency =
            &arm64_asid_residency[cpu][hardware_asid];

        if (residency->table == vm->l1)
            residency->generation = vm->tlb_generation;
    }
}

_Static_assert(ARM64_USER_PAGE_READ == VMA_READ,
               "ARM64 read permission must match generic VMA_READ");
_Static_assert(ARM64_USER_PAGE_WRITE == VMA_WRITE,
               "ARM64 write permission must match generic VMA_WRITE");
_Static_assert(ARM64_USER_PAGE_EXEC == VMA_EXEC,
               "ARM64 execute permission must match generic VMA_EXEC");

static uint32_t allocate_tlb_generation(void)
{
    unsigned long flags;
    uint32_t generation;

    spin_lock_irqsave(&arm64_asid_lock, &flags);
    generation = next_tlb_generation++;

    if (generation == 0) {
        generation = next_tlb_generation++;
        if (generation == 0)
            generation = 1;
    }
    spin_unlock_irqrestore(&arm64_asid_lock, flags);
    return generation;
}

static void clear_bytes(void *address, size_t length)
{
    uint8_t *bytes = address;
    size_t index;

    for (index = 0; index < length; index++)
        bytes[index] = 0;
}

static int asid_is_allocated(unsigned int asid)
{
    return (arm64_asid_bitmap[asid >> 3] & (1u << (asid & 7u))) != 0;
}

static unsigned int arm64_asid_hw(unsigned int asid)
{
    return asid & (ARM64_ASID_COUNT - 1u);
}

static unsigned int arm64_asid_make(unsigned int generation,
                                    unsigned int hardware_asid)
{
    return (generation << 8) | arm64_asid_hw(hardware_asid);
}

static unsigned int allocate_asid_locked(void)
{
    unsigned int asid;
    uint32_t cpu;

    for (asid = 1; asid < ARM64_ASID_COUNT; asid++) {
        if (!asid_is_allocated(asid)) {
            arm64_asid_bitmap[asid >> 3] |= (uint8_t)(1u << (asid & 7u));
            arm64_asid_slot_generation[asid] = arm64_asid_generation;
            return arm64_asid_make(arm64_asid_generation, asid);
        }
    }

    arm64_asid_generation++;
    if (arm64_asid_generation == 0)
        arm64_asid_generation = 1;
    clear_bytes(arm64_asid_bitmap, sizeof(arm64_asid_bitmap));
    clear_bytes(arm64_asid_slot_generation,
                sizeof(arm64_asid_slot_generation));
    clear_bytes(arm64_asid_residency, sizeof(arm64_asid_residency));
    tlb_shootdown_all();
    kernel_lifecycle_stats.asid_rollovers++;

    /*
     * A generation rollover invalidates every cached translation, but the
     * address spaces executing on other CPUs keep using their hardware ASID
     * immediately afterwards.  Reserve those slots in the new generation;
     * inactive spaces will acquire a new slot lazily when next activated.
     */
    for (cpu = 0; cpu < ARMOS_MAX_CPUS; cpu++) {
        arm64_user_vm_t *active = arm64_asid_active_vm[cpu];
        unsigned int hardware_asid;

        if (!active)
            continue;
        hardware_asid = arm64_asid_hw(active->asid);
        if (hardware_asid == 0)
            continue;
        arm64_asid_bitmap[hardware_asid >> 3] |=
            (uint8_t)(1u << (hardware_asid & 7u));
        arm64_asid_slot_generation[hardware_asid] =
            arm64_asid_generation;
        active->asid = arm64_asid_make(arm64_asid_generation,
                                      hardware_asid);
        active->space.asid = active->asid;
    }

    for (asid = 1; asid < ARM64_ASID_COUNT; asid++) {
        if (!asid_is_allocated(asid)) {
            arm64_asid_bitmap[asid >> 3] |=
                (uint8_t)(1u << (asid & 7u));
            arm64_asid_slot_generation[asid] = arm64_asid_generation;
            return arm64_asid_make(arm64_asid_generation, asid);
        }
    }
    return 0;
}

static unsigned int allocate_asid(void)
{
    unsigned long flags;
    unsigned int asid;

    spin_lock_irqsave(&arm64_asid_lock, &flags);
    asid = allocate_asid_locked();
    spin_unlock_irqrestore(&arm64_asid_lock, flags);
    return asid;
}

static void release_asid_locked(unsigned int asid)
{
    unsigned int hardware_asid = arm64_asid_hw(asid);
    unsigned int generation = asid >> 8;
    uint32_t cpu;

    if (hardware_asid > 0 &&
        arm64_asid_slot_generation[hardware_asid] == generation) {
        /*
         * The software generation is not encoded in TTBR0_EL1.  Flush the
         * hardware ASID on every PE before making its slot reusable, or a
         * later process can execute translations left by its predecessor.
         */
        tlb_shootdown_asid(hardware_asid);
        arm64_asid_bitmap[hardware_asid >> 3] &=
            (uint8_t)~(1u << (hardware_asid & 7u));
        arm64_asid_slot_generation[hardware_asid] = 0;
        for (cpu = 0; cpu < ARMOS_MAX_CPUS; cpu++) {
            clear_bytes(&arm64_asid_residency[cpu][hardware_asid],
                        sizeof(arm64_asid_residency[cpu][hardware_asid]));
        }
    }
}

static void release_asid(unsigned int asid)
{
    unsigned long flags;

    spin_lock_irqsave(&arm64_asid_lock, &flags);
    release_asid_locked(asid);
    spin_unlock_irqrestore(&arm64_asid_lock, flags);
}

static arm64_user_vm_l2_table_t *arm64_user_vm_find_l2(
    arm64_user_vm_t *vm,
    vaddr_t virtual_address)
{
    uint32_t l1_index = (uint32_t)((virtual_address >> 30) & 0x1ffu);
    unsigned int index;

    for (index = 0; index < vm->l2_table_count; index++) {
        if (vm->l2_tables[index].l1_index == l1_index)
            return &vm->l2_tables[index];
    }
    return NULL;
}

static arm64_user_vm_l3_table_t *arm64_user_vm_find_l3(
    arm64_user_vm_t *vm,
    vaddr_t virtual_address)
{
    uint32_t l1_index = (uint32_t)((virtual_address >> 30) & 0x1ffu);
    uint32_t l2_index = (uint32_t)((virtual_address >> 21) & 0x1ffu);
    unsigned int index;

    for (index = 0; index < vm->l3_table_count; index++) {
        if (vm->l3_tables[index].l1_index == l1_index &&
            vm->l3_tables[index].l2_index == l2_index)
            return &vm->l3_tables[index];
    }
    return NULL;
}

static unsigned int arm64_user_vm_mapping_index(
    const arm64_user_vm_t *vm,
    vaddr_t virtual_address)
{
    unsigned int index;

    for (index = 0; index < vm->mapping_count; index++) {
        if (vm->mappings[index].virtual_address == virtual_address)
            return index;
    }
    return vm->mapping_count;
}

static int arm64_user_vm_reserve_mappings(arm64_user_vm_t *vm,
                                          unsigned int required)
{
    arm64_user_vm_mapping_t *mappings;
    unsigned int capacity;

    if (required <= vm->mapping_capacity)
        return 0;
    capacity = vm->mapping_capacity ? vm->mapping_capacity : 32u;
    while (capacity < required) {
        if (capacity > (unsigned int)-1 / 2u)
            return -1;
        capacity *= 2u;
    }
    mappings = kmalloc((size_t)capacity * sizeof(*mappings));
    if (!mappings)
        return -1;
    clear_bytes(mappings, (size_t)capacity * sizeof(*mappings));
    if (vm->mapping_count)
        memcpy(mappings, vm->mappings,
               (size_t)vm->mapping_count * sizeof(*mappings));
    kfree(vm->mappings);
    vm->mappings = mappings;
    vm->mapping_capacity = capacity;
    return 0;
}

static void *arm64_user_vm_grow_inventory(const void *old_items,
                                          unsigned int count,
                                          unsigned int old_capacity,
                                          unsigned int required,
                                          size_t item_size,
                                          unsigned int *new_capacity)
{
    unsigned int capacity = old_capacity ? old_capacity : 4u;
    void *new_items;

    while (capacity < required) {
        if (capacity > (unsigned int)-1 / 2u)
            return NULL;
        capacity *= 2u;
    }
    new_items = kmalloc((size_t)capacity * item_size);
    if (!new_items)
        return NULL;
    clear_bytes(new_items, (size_t)capacity * item_size);
    if (count)
        memcpy(new_items, old_items, (size_t)count * item_size);
    *new_capacity = capacity;
    return new_items;
}

static int arm64_user_vm_reserve_l2_tables(arm64_user_vm_t *vm,
                                           unsigned int required)
{
    arm64_user_vm_l2_table_t *tables;
    unsigned int capacity;

    if (required <= vm->l2_table_capacity)
        return 0;
    tables = arm64_user_vm_grow_inventory(
        vm->l2_tables, vm->l2_table_count, vm->l2_table_capacity,
        required, sizeof(*tables), &capacity);
    if (!tables)
        return -1;
    kfree(vm->l2_tables);
    vm->l2_tables = tables;
    vm->l2_table_capacity = capacity;
    return 0;
}

static int arm64_user_vm_reserve_l3_tables(arm64_user_vm_t *vm,
                                           unsigned int required)
{
    arm64_user_vm_l3_table_t *tables;
    unsigned int capacity;

    if (required <= vm->l3_table_capacity)
        return 0;
    tables = arm64_user_vm_grow_inventory(
        vm->l3_tables, vm->l3_table_count, vm->l3_table_capacity,
        required, sizeof(*tables), &capacity);
    if (!tables)
        return -1;
    kfree(vm->l3_tables);
    vm->l3_tables = tables;
    vm->l3_table_capacity = capacity;
    return 0;
}

static int arm64_user_vm_l3_has_mapping(const arm64_user_vm_t *vm,
                                        uint32_t l1_index,
                                        uint32_t l2_index)
{
    unsigned int index;

    for (index = 0; index < vm->mapping_count; index++) {
        vaddr_t address = vm->mappings[index].virtual_address;

        if (vm->mappings[index].physical_address != 0 &&
            ((address >> 30) & 0x1ffu) == l1_index &&
            ((address >> 21) & 0x1ffu) == l2_index)
            return 1;
    }
    return 0;
}

static int arm64_user_vm_l2_has_l3(const arm64_user_vm_t *vm,
                                   uint32_t l1_index)
{
    unsigned int index;

    for (index = 0; index < vm->l3_table_count; index++) {
        if (vm->l3_tables[index].l1_index == l1_index)
            return 1;
    }
    return 0;
}

static int arm64_user_vm_reclaim_empty_tables(
    arm64_user_vm_t *vm,
    vaddr_t virtual_address)
{
    arm64_user_vm_l2_table_t *l2;
    arm64_user_vm_l3_table_t *l3;
    paddr_t table;
    uint32_t l1_index = (uint32_t)((virtual_address >> 30) & 0x1ffu);
    uint32_t l2_index = (uint32_t)((virtual_address >> 21) & 0x1ffu);
    unsigned int index;
    unsigned int last;

    l3 = arm64_user_vm_find_l3(vm, virtual_address);
    if (l3 && !arm64_user_vm_l3_has_mapping(vm, l1_index, l2_index)) {
        l2 = arm64_user_vm_find_l2(vm, virtual_address);
        if (!l2 || arm64_mmu_remove_user_l3(
                l2->table, l3->table, virtual_address,
                arm64_asid_hw(vm->asid)) != 0)
            return -1;
        table = l3->table;
        if (arm64_vm_free_pages(table, 1) != 0)
            return -2;
        for (index = 0; index < vm->l3_table_count; index++) {
            if (vm->l3_tables[index].table == table)
                break;
        }
        if (index == vm->l3_table_count)
            return -3;
        last = vm->l3_table_count - 1;
        if (index != last)
            vm->l3_tables[index] = vm->l3_tables[last];
        clear_bytes(&vm->l3_tables[last], sizeof(vm->l3_tables[last]));
        vm->l3_table_count--;
    }

    l2 = arm64_user_vm_find_l2(vm, virtual_address);
    if (l2 && !arm64_user_vm_l2_has_l3(vm, l1_index)) {
        table = l2->table;
        if (arm64_mmu_remove_user_l2(
                vm->l1, table, virtual_address,
                arm64_asid_hw(vm->asid)) != 0)
            return -4;
        if (arm64_vm_free_pages(table, 1) != 0)
            return -5;
        for (index = 0; index < vm->l2_table_count; index++) {
            if (vm->l2_tables[index].table == table)
                break;
        }
        if (index == vm->l2_table_count)
            return -6;
        last = vm->l2_table_count - 1;
        if (index != last)
            vm->l2_tables[index] = vm->l2_tables[last];
        clear_bytes(&vm->l2_tables[last], sizeof(vm->l2_tables[last]));
        vm->l2_table_count--;
    }
    return 0;
}

static int arm64_user_vm_validate_tables(const arm64_user_vm_t *vm)
{
    unsigned int first;
    unsigned int second;
    int parent_found;

    if (vm->l2_table_count > vm->l2_table_capacity ||
        vm->l3_table_count > vm->l3_table_capacity ||
        (vm->l2_table_capacity == 0) != (vm->l2_tables == NULL) ||
        (vm->l3_table_capacity == 0) != (vm->l3_tables == NULL))
        return -1;
    for (first = 0; first < vm->l2_table_count; first++) {
        if (vm->l2_tables[first].l1_index >= 512u ||
            vm->l2_tables[first].table == 0 ||
            (vm->l2_tables[first].table & PAGE_OFFSET_MASK) != 0 ||
            vm->l2_tables[first].table == vm->l1)
            return -1;
        for (second = first + 1; second < vm->l2_table_count; second++) {
            if (vm->l2_tables[first].l1_index ==
                    vm->l2_tables[second].l1_index ||
                vm->l2_tables[first].table ==
                    vm->l2_tables[second].table)
                return -1;
        }
    }
    for (first = 0; first < vm->l3_table_count; first++) {
        if (vm->l3_tables[first].l1_index >= 512u ||
            vm->l3_tables[first].l2_index >= 512u ||
            vm->l3_tables[first].table == 0 ||
            (vm->l3_tables[first].table & PAGE_OFFSET_MASK) != 0 ||
            vm->l3_tables[first].table == vm->l1)
            return -1;
        parent_found = 0;
        for (second = 0; second < vm->l2_table_count; second++) {
            if (vm->l2_tables[second].l1_index ==
                vm->l3_tables[first].l1_index)
                parent_found = 1;
        }
        if (!parent_found)
            return -1;
        for (second = first + 1; second < vm->l3_table_count; second++) {
            if (vm->l3_tables[first].l1_index ==
                    vm->l3_tables[second].l1_index &&
                vm->l3_tables[first].l2_index ==
                    vm->l3_tables[second].l2_index)
                return -1;
            if (vm->l3_tables[first].table ==
                vm->l3_tables[second].table)
                return -1;
        }
        for (second = 0; second < vm->l2_table_count; second++) {
            if (vm->l3_tables[first].table ==
                vm->l2_tables[second].table)
                return -1;
        }
    }
    return 0;
}

static int arm64_user_vm_flags_valid(unsigned int flags)
{
    if ((flags & VMA_READ) == 0 ||
        (flags & ~(VMA_READ | VMA_WRITE | VMA_EXEC | VMA_SHARED |
                   VMA_LAZY | VMA_DONTFORK | VMA_EXTERNAL)) != 0)
        return 0;
    return (flags & (VMA_WRITE | VMA_EXEC)) !=
           (VMA_WRITE | VMA_EXEC);
}

static int arm64_user_vm_ensure_l3(arm64_user_vm_t *vm,
                                   vaddr_t virtual_address,
                                   paddr_t *l3_address)
{
    arm64_user_vm_l2_table_t *l2 =
        arm64_user_vm_find_l2(vm, virtual_address);
    arm64_user_vm_l3_table_t *l3 =
        arm64_user_vm_find_l3(vm, virtual_address);
    paddr_t table;

    if (l3) {
        *l3_address = l3->table;
        return 0;
    }
    if (!l2) {
        if (arm64_user_vm_reserve_l2_tables(
                vm, vm->l2_table_count + 1u) != 0 ||
            arm64_vm_alloc_pages(1, &table) != 0)
            return -1;
        if (arm64_mmu_install_user_l2(vm->l1, table,
                                      virtual_address) != 0) {
            arm64_vm_free_pages(table, 1);
            return -2;
        }
        l2 = &vm->l2_tables[vm->l2_table_count++];
        l2->l1_index = (uint32_t)((virtual_address >> 30) & 0x1ffu);
        l2->table = table;
    }
    if (arm64_user_vm_reserve_l3_tables(
            vm, vm->l3_table_count + 1u) != 0 ||
        arm64_vm_alloc_pages(1, &table) != 0)
        return -3;
    if (arm64_mmu_install_user_l3(l2->table, table,
                                  virtual_address) != 0) {
        arm64_vm_free_pages(table, 1);
        return -4;
    }
    l3 = &vm->l3_tables[vm->l3_table_count++];
    l3->l1_index = (uint32_t)((virtual_address >> 30) & 0x1ffu);
    l3->l2_index = (uint32_t)((virtual_address >> 21) & 0x1ffu);
    l3->table = table;
    *l3_address = table;
    return 0;
}

static int arm64_user_vm_validate_residents(const arm64_user_vm_t *vm)
{
    unsigned int index;

    if (vm->mapping_count > vm->mapping_capacity ||
        (vm->mapping_capacity == 0 && vm->mappings != NULL) ||
        (vm->mapping_capacity != 0 && vm->mappings == NULL))
        return -1;
    for (index = 0; index < vm->mapping_count; index++) {
        const arm64_user_vm_mapping_t *mapping = &vm->mappings[index];

        if ((mapping->virtual_address & PAGE_OFFSET_MASK) != 0 ||
            mapping->physical_address == 0 ||
            (mapping->physical_address & PAGE_OFFSET_MASK) != 0 ||
            !arm64_user_vm_flags_valid(mapping->flags))
            return -1;
    }
    return 0;
}

static int arm64_user_vm_validate_fields(const arm64_user_vm_t *vm)
{
    if (!vm || vm->magic != ARM64_USER_VM_MAGIC || vm->l1 == 0 ||
        vm->asid == 0 || vm->space.pgdir == NULL ||
        vm->space.pgdir_alloc == NULL || vm->space.asid != vm->asid ||
        (paddr_t)(uintptr_t)vm->space.pgdir != vm->l1 ||
        (paddr_t)(uintptr_t)vm->space.pgdir_alloc != vm->l1 ||
        arm64_user_vm_validate_tables(vm) != 0 ||
        arm64_user_vm_validate_residents(vm) != 0)
        return -1;
    return 0;
}

int arm64_user_vm_validate_identity(const arm64_user_vm_t *vm)
{
    if (arm64_user_vm_validate_fields(vm) != 0 ||
        vm->space.arch_private != vm)
        return -1;
    return 0;
}

int arm64_user_vm_rebind_space(arm64_user_vm_t *vm)
{
    if (!vm || vm->magic != ARM64_USER_VM_MAGIC || vm->l1 == 0 ||
        vm->asid == 0 || vm->mapping_count > vm->mapping_capacity ||
        arm64_user_vm_validate_tables(vm) != 0)
        return -1;
    vm->space.arch_private = vm;
    return arm64_user_vm_validate_identity(vm);
}

const vm_space_t *arm64_user_vm_space(const arm64_user_vm_t *vm)
{
    if (arm64_user_vm_validate_identity(vm) != 0)
        return NULL;
    return &vm->space;
}

const arm64_user_vm_t *arm64_user_vm_from_space(const vm_space_t *space)
{
    const arm64_user_vm_t *vm;

    if (!space || !space->arch_private)
        return NULL;
    vm = (const arm64_user_vm_t *)space->arch_private;
    if (&vm->space != space ||
        arm64_user_vm_validate_identity(vm) != 0)
        return NULL;
    return vm;
}

int arm64_user_vm_init(arm64_user_vm_t *vm)
{
    paddr_t table_pages;
    unsigned int asid;

    if (!vm)
        return -1;

    clear_bytes(vm, sizeof(*vm));
    asid = allocate_asid();
    if (asid == 0)
        return -2;
    if (arm64_vm_alloc_pages(1, &table_pages) != 0) {
        release_asid(asid);
        return -3;
    }
    if (arm64_mmu_prepare_user_ttbr0(table_pages) != 0) {
        arm64_vm_free_pages(table_pages, 1);
        release_asid(asid);
        return -4;
    }

    vm->l1 = table_pages;
    vm->asid = asid;
    vm->tlb_generation = allocate_tlb_generation();
    vm->space.pgdir = (pgdir_t)(uintptr_t)vm->l1;
    vm->space.pgdir_alloc = (pgdir_t)(uintptr_t)vm->l1;
    vm->space.arch_private = vm;
    vm_initialize_user_layout(&vm->space);
    vm->space.asid = asid;
    vm->magic = ARM64_USER_VM_MAGIC;
    return 0;
}

int arm64_user_vm_map_new_page(arm64_user_vm_t *vm,
                               vaddr_t virtual_address,
                               unsigned int flags,
                               paddr_t *physical_address)
{
    paddr_t page;
    paddr_t l3_address;
    unsigned int index;
    int result;

    if (arm64_user_vm_validate_identity(vm) != 0 ||
        !physical_address ||
        (virtual_address & PAGE_OFFSET_MASK) != 0 ||
        virtual_address >= (1ULL << 39) ||
        !arm64_user_vm_flags_valid(flags) || (flags & VMA_LAZY) != 0)
        return -1;

    if (arm64_user_vm_mapping_index(vm, virtual_address) !=
        vm->mapping_count)
        return -2;
    if (arm64_user_vm_reserve_mappings(vm,
                                       vm->mapping_count + 1u) != 0)
        return -3;

    if (arm64_user_vm_ensure_l3(vm, virtual_address, &l3_address) != 0) {
        arm64_user_vm_reclaim_empty_tables(vm, virtual_address);
        return -4;
    }
    if (arm64_vm_alloc_pages(1, &page) != 0) {
        arm64_user_vm_reclaim_empty_tables(vm, virtual_address);
        return -5;
    }

    result = arm64_mmu_map_user_l3_page(
        l3_address, virtual_address, page,
        flags & (VMA_READ | VMA_WRITE | VMA_EXEC),
        arm64_asid_hw(vm->asid));
    if (result != 0) {
        arm64_vm_release_user_page(page);
        arm64_user_vm_reclaim_empty_tables(vm, virtual_address);
        return -6;
    }

    index = vm->mapping_count++;
    vm->mappings[index].virtual_address = virtual_address;
    vm->mappings[index].flags = flags;
    vm->mappings[index].physical_address = page;
    vm->tlb_generation = allocate_tlb_generation();
    arm64_user_vm_publish_targeted_generation(vm);
    *physical_address = page;
    return 0;
}

int arm64_user_vm_map_page(arm64_user_vm_t *vm,
                           vaddr_t virtual_address,
                           paddr_t physical_address,
                           unsigned int flags)
{
    paddr_t l3_address;
    unsigned int index;

    flags &= ~VMA_LAZY;
    if (arm64_user_vm_validate_identity(vm) != 0 ||
        (virtual_address & PAGE_OFFSET_MASK) != 0 ||
        (physical_address & PAGE_OFFSET_MASK) != 0 ||
        !arm64_user_vm_flags_valid(flags))
        return -1;

    index = arm64_user_vm_mapping_index(vm, virtual_address);
    if (index < vm->mapping_count &&
        vm->mappings[index].physical_address != 0)
        return -2;
    if (index == vm->mapping_count &&
        arm64_user_vm_reserve_mappings(
            vm, vm->mapping_count + 1u) != 0)
        return -3;
    if (arm64_user_vm_ensure_l3(vm, virtual_address, &l3_address) != 0) {
        arm64_user_vm_reclaim_empty_tables(vm, virtual_address);
        return -4;
    }
    if (arm64_mmu_map_user_l3_page(l3_address, virtual_address,
                                   physical_address,
                                   flags & (VMA_READ | VMA_WRITE | VMA_EXEC),
                                   arm64_asid_hw(vm->asid)) != 0) {
        arm64_user_vm_reclaim_empty_tables(vm, virtual_address);
        return -5;
    }

    if (index == vm->mapping_count) {
        vm->mapping_count++;
        vm->mappings[index].virtual_address = virtual_address;
    }
    vm->mappings[index].flags = flags;
    vm->mappings[index].physical_address = physical_address;
    vm->tlb_generation = allocate_tlb_generation();
    arm64_user_vm_publish_targeted_generation(vm);
    return 0;
}

int arm64_user_vm_protect_page(arm64_user_vm_t *vm,
                               vaddr_t virtual_address,
                               unsigned int flags)
{
    arm64_user_vm_l3_table_t *l3;
    paddr_t physical;
    arm64_mmu_u64 unmapped;
    unsigned int index;

    flags &= ~VMA_LAZY;
    if (arm64_user_vm_validate_identity(vm) != 0 ||
        !arm64_user_vm_flags_valid(flags))
        return -1;
    virtual_address &= PAGE_MASK;
    index = arm64_user_vm_mapping_index(vm, virtual_address);
    l3 = arm64_user_vm_find_l3(vm, virtual_address);
    if (index == vm->mapping_count || !l3)
        return -2;
    physical = vm->mappings[index].physical_address;
    if (physical == 0 ||
        arm64_mmu_unmap_user_l3_page(l3->table, virtual_address,
                                     arm64_asid_hw(vm->asid),
                                     &unmapped) != 0 ||
        unmapped != physical)
        return -3;
    if (arm64_mmu_map_user_l3_page(
            l3->table, virtual_address, physical,
            flags & (VMA_READ | VMA_WRITE | VMA_EXEC),
            arm64_asid_hw(vm->asid)) != 0)
        return -4;
    vm->mappings[index].flags = flags;
    vm->tlb_generation = allocate_tlb_generation();
    arm64_user_vm_publish_targeted_generation(vm);
    return 0;
}

int arm64_user_vm_unmap_page(arm64_user_vm_t *vm,
                             vaddr_t virtual_address)
{
    arm64_user_vm_l3_table_t *l3;
    paddr_t physical_address;
    arm64_mmu_u64 descriptor_address;
    unsigned int index;
    unsigned int last;

    if (arm64_user_vm_validate_identity(vm) != 0 ||
        (virtual_address & PAGE_OFFSET_MASK) != 0)
        return -1;
    index = arm64_user_vm_mapping_index(vm, virtual_address);
    if (index == vm->mapping_count)
        return -2;
    if (vm->mappings[index].physical_address == 0) {
        last = vm->mapping_count - 1;
        if (index != last)
            vm->mappings[index] = vm->mappings[last];
        clear_bytes(&vm->mappings[last], sizeof(vm->mappings[last]));
        vm->mapping_count--;
        return arm64_user_vm_validate_identity(vm);
    }
    l3 = arm64_user_vm_find_l3(vm, virtual_address);
    if (!l3)
        return -3;
    if (arm64_mmu_unmap_user_l3_page(l3->table, virtual_address,
                                     arm64_asid_hw(vm->asid),
                                     &descriptor_address) != 0)
        return -4;
    physical_address = (paddr_t)descriptor_address;
    if (physical_address != vm->mappings[index].physical_address)
        return -5;
    last = vm->mapping_count - 1;
    if (index != last)
        vm->mappings[index] = vm->mappings[last];
    clear_bytes(&vm->mappings[last], sizeof(vm->mappings[last]));
    vm->mapping_count--;
    vm->tlb_generation = allocate_tlb_generation();
    arm64_user_vm_publish_targeted_generation(vm);
    if (arm64_user_vm_reclaim_empty_tables(vm, virtual_address) != 0)
        return -7;
    return arm64_user_vm_validate_identity(vm);
}

int arm64_user_vm_activate(arm64_user_vm_t *vm)
{
    arm64_asid_residency_t *residency;
    unsigned long flags;
    uint32_t cpu;
    unsigned int generation;
    unsigned int refreshed_asid;
    int result;

    if (arm64_user_vm_validate_identity(vm) != 0)
        return -1;

    cpu = smp_processor_id();
    if (cpu >= ARMOS_MAX_CPUS)
        return -2;

    spin_lock_irqsave(&arm64_asid_lock, &flags);
    generation = vm->asid >> 8;
    if (generation != arm64_asid_generation) {
        refreshed_asid = allocate_asid_locked();
        if (refreshed_asid == 0) {
            spin_unlock_irqrestore(&arm64_asid_lock, flags);
            return -3;
        }
        vm->asid = refreshed_asid;
        vm->space.asid = refreshed_asid;
    }

    residency = &arm64_asid_residency[cpu][arm64_asid_hw(vm->asid)];
    if (residency->table == vm->l1 &&
        residency->generation == vm->tlb_generation) {
        result = arm64_mmu_switch_user_ttbr0_preserve(
            vm->l1, arm64_asid_hw(vm->asid));
        if (result == 0) {
            tlb_preserve_count++;
            arm64_asid_active_vm[cpu] = vm;
        }
        spin_unlock_irqrestore(&arm64_asid_lock, flags);
        return result;
    }

    result = arm64_mmu_switch_user_ttbr0(vm->l1,
                                         arm64_asid_hw(vm->asid));
    if (result == 0) {
        residency->table = vm->l1;
        residency->generation = vm->tlb_generation;
        tlb_flush_count++;
        arm64_asid_active_vm[cpu] = vm;
    }
    spin_unlock_irqrestore(&arm64_asid_lock, flags);
    return result;
}

int arm64_user_vm_activate_space(const vm_space_t *space)
{
    arm64_user_vm_t *vm = (arm64_user_vm_t *)arm64_user_vm_from_space(space);

    if (!vm)
        return -1;
    return arm64_user_vm_activate(vm);
}

void arm64_user_vm_deactivate_cpu(void)
{
    unsigned long flags;
    uint32_t cpu = smp_processor_id();

    if (cpu >= ARMOS_MAX_CPUS)
        return;
    spin_lock_irqsave(&arm64_asid_lock, &flags);
    arm64_asid_active_vm[cpu] = NULL;
    spin_unlock_irqrestore(&arm64_asid_lock, flags);
}

uint64_t arm64_user_vm_tlb_flush_count(void)
{
    return tlb_flush_count;
}

uint64_t arm64_user_vm_tlb_preserve_count(void)
{
    return tlb_preserve_count;
}

int arm64_user_vm_lookup(const arm64_user_vm_t *vm,
                         vaddr_t virtual_address,
                         paddr_t *physical_address,
                         unsigned int *flags)
{
    const arm64_user_vm_mapping_t *mapping;
    unsigned int index;

    if (arm64_user_vm_validate_identity(vm) != 0)
        return -1;
    index = arm64_user_vm_mapping_index(vm,
                                        virtual_address & PAGE_MASK);
    if (index == vm->mapping_count)
        return -2;
    mapping = &vm->mappings[index];
    if (physical_address)
        *physical_address = mapping->physical_address;
    if (flags)
        *flags = mapping->flags;
    return 0;
}

int arm64_user_vm_destroy(arm64_user_vm_t *vm)
{
    unsigned int index;

    if (arm64_user_vm_validate_identity(vm) != 0)
        return -1;
    if ((arm64_mmu_read_ttbr0() & ARM64_TTBR_TABLE_MASK) == vm->l1)
        return -2;

    for (index = 0; index < vm->mapping_count; index++) {
        if (vm->mappings[index].physical_address != 0 &&
            !(vm->mappings[index].flags & VMA_EXTERNAL))
            arm64_vm_release_user_page(
                vm->mappings[index].physical_address);
    }
    kfree(vm->mappings);
    vm->mappings = NULL;
    vm->mapping_count = 0;
    vm->mapping_capacity = 0;
    for (index = 0; index < vm->l3_table_count; index++) {
        if (arm64_vm_free_pages(vm->l3_tables[index].table, 1) != 0)
            return -4;
    }
    for (index = 0; index < vm->l2_table_count; index++) {
        if (arm64_vm_free_pages(vm->l2_tables[index].table, 1) != 0)
            return -5;
    }
    kfree(vm->l3_tables);
    kfree(vm->l2_tables);
    if (arm64_vm_free_pages(vm->l1, 1) != 0)
        return -6;

    release_asid(vm->asid);
    vm->l1 = 0;
    vm->asid = 0;
    vm->space.pgdir = NULL;
    vm->space.pgdir_alloc = NULL;
    vm->space.asid = 0;
    vm->l2_tables = NULL;
    vm->l2_table_count = 0;
    vm->l2_table_capacity = 0;
    vm->l3_tables = NULL;
    vm->l3_table_count = 0;
    vm->l3_table_capacity = 0;
    vm->magic = 0;
    return 0;
}
