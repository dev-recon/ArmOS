/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/mmu/virtual.c
 * Layer: ARM64 / user translation backend
 *
 * Responsibilities:
 * - Implement the common vm_space_t contract with AArch64 page tables.
 * - Connect generic VMA, exec, mmap and fault policy to ARM64 mappings.
 * - Provide the direct-map helpers used while loading user images.
 *
 * Notes:
 * - Process and mapping policy remain in common kernel callers.
 * - VMA ownership and fork/COW policy remain in the common VM layer.
 */

#include <asm/mmu.h>
#include <asm/user_vm.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/task.h>

static arm64_user_vm_t *vm_registry;
static DEFINE_SPINLOCK(vm_registry_lock);

static bool register_backend(arm64_user_vm_t *vm)
{
    arm64_user_vm_t *current;
    unsigned long flags;

    spin_lock_irqsave(&vm_registry_lock, &flags);
    for (current = vm_registry; current; current = current->registry_next) {
        if (current == vm) {
            spin_unlock_irqrestore(&vm_registry_lock, flags);
            return false;
        }
    }
    vm->registry_next = vm_registry;
    vm_registry = vm;
    spin_unlock_irqrestore(&vm_registry_lock, flags);
    return true;
}

static void unregister_backend(arm64_user_vm_t *vm)
{
    arm64_user_vm_t **link = &vm_registry;
    unsigned long flags;

    spin_lock_irqsave(&vm_registry_lock, &flags);
    while (*link) {
        if (*link == vm) {
            *link = vm->registry_next;
            vm->registry_next = NULL;
            spin_unlock_irqrestore(&vm_registry_lock, flags);
            return;
        }
        link = &(*link)->registry_next;
    }
    spin_unlock_irqrestore(&vm_registry_lock, flags);
}

static arm64_user_vm_t *backend_for_pgdir(pgdir_t pgdir)
{
    arm64_user_vm_t *vm;
    arm64_user_vm_t *result = NULL;
    unsigned long flags;

    spin_lock_irqsave(&vm_registry_lock, &flags);
    for (vm = vm_registry; vm; vm = vm->registry_next) {
        if (vm->space.pgdir == pgdir) {
            result = vm;
            break;
        }
    }
    spin_unlock_irqrestore(&vm_registry_lock, flags);
    return result;
}

static arm64_user_vm_t *backend(vm_space_t *space)
{
    return space ? (arm64_user_vm_t *)space->arch_private : NULL;
}

vm_space_t *create_vm_space(void)
{
    arm64_user_vm_t *vm = kzalloc(sizeof(*vm));

    if (!vm)
        return NULL;
    if (arm64_user_vm_init(vm) != 0 || !register_backend(vm)) {
        if (vm->magic == ARM64_USER_VM_MAGIC)
            (void)arm64_user_vm_destroy(vm);
        kfree(vm);
        return NULL;
    }
    return &vm->space;
}

void destroy_vm_space(vm_space_t *space)
{
    arm64_user_vm_t *vm = backend(space);

    if (!vm)
        return;
    if (arm64_user_vm_destroy(vm) == 0) {
        vm_release_vmas(space);
        unregister_backend(vm);
        kfree(vm);
    }
}

int map_user_page(pgdir_t pgdir, vaddr_t address, paddr_t physical,
                  uint32_t flags, uint32_t asid)
{
    task_t *task = task_current_local();
    arm64_user_vm_t *vm = NULL;

    if (task && task->process && task->process->vm &&
        task->process->vm->pgdir == pgdir)
        vm = backend(task->process->vm);
    if (!vm)
        vm = backend_for_pgdir(pgdir);
    if (!vm || vm->asid != asid)
        return -1;
    return arm64_user_vm_map_page(vm, address & PAGE_MASK,
                                  physical & PAGE_MASK, flags);
}

int map_user_page_readonly(pgdir_t pgdir, vaddr_t address, paddr_t physical,
                           uint32_t flags, uint32_t asid)
{
    return map_user_page(pgdir, address, physical,
                         (flags | VMA_READ) & ~VMA_WRITE, asid);
}

int remap_user_page(pgdir_t pgdir, vaddr_t address, paddr_t physical,
                    uint32_t flags, uint32_t asid)
{
    (void)unmap_user_page(pgdir, address, asid);
    return map_user_page(pgdir, address, physical, flags, asid);
}

int arm64_user_vm_activate_identity(paddr_t table, uint32_t asid)
{
    arm64_user_vm_t *vm = backend_for_pgdir((pgdir_t)(uintptr_t)table);

    if (!vm || vm->asid != asid)
        return -1;
    return arm64_user_vm_activate(vm);
}

int unmap_user_page(pgdir_t pgdir, vaddr_t address, uint32_t asid)
{
    arm64_user_vm_t *vm = backend_for_pgdir(pgdir);

    if (!vm || vm->asid != asid)
        return -1;
    return arm64_user_vm_unmap_page(vm, address & PAGE_MASK);
}

paddr_t get_physical_address(pgdir_t pgdir, vaddr_t address)
{
    arm64_user_vm_t *vm = backend_for_pgdir(pgdir);
    paddr_t physical;

    if (!vm || arm64_user_vm_lookup(vm, address & PAGE_MASK,
                                    &physical, NULL) != 0)
        return 0;
    return physical ? physical + (address & PAGE_OFFSET_MASK) : 0;
}

int set_user_page_readonly(pgdir_t pgdir, vaddr_t address, uint32_t asid)
{
    arm64_user_vm_t *vm = backend_for_pgdir(pgdir);
    unsigned int flags;

    if (!vm || vm->asid != asid ||
        arm64_user_vm_lookup(vm, address & PAGE_MASK, NULL, &flags) != 0)
        return -1;
    return arm64_user_vm_protect_page(vm, address,
                                      (flags | VMA_READ) & ~VMA_WRITE);
}

int set_user_page_writable(pgdir_t pgdir, vaddr_t address, uint32_t asid)
{
    arm64_user_vm_t *vm = backend_for_pgdir(pgdir);
    unsigned int flags;

    if (!vm || vm->asid != asid ||
        arm64_user_vm_lookup(vm, address & PAGE_MASK, NULL, &flags) != 0)
        return -1;
    return arm64_user_vm_protect_page(vm, address,
                                      flags | VMA_READ | VMA_WRITE);
}

void switch_to_vm_space(vm_space_t *space)
{
    if (space)
        (void)arm64_user_vm_activate_space(space);
}

vaddr_t map_temp_page(paddr_t physical) { return (vaddr_t)physical; }
void unmap_temp_page(void *address) { (void)address; }

uint32_t vm_resident_kb(vm_space_t *space)
{
    arm64_user_vm_t *vm = backend(space);
    uint32_t pages = 0;
    unsigned int index;

    if (!vm)
        return 0;
    for (index = 0; index < vm->mapping_count; index++) {
        if (vm->mappings[index].physical_address != 0)
            pages++;
    }
    return pages * (PAGE_SIZE / 1024u);
}

uint32_t vm_page_table_count(vm_space_t *space)
{
    arm64_user_vm_t *vm = backend(space);
    return vm ? 1u + vm->l2_table_count + vm->l3_table_count : 0;
}

int handle_cow_fault(vaddr_t address)
{
    task_t *task = task_current_local();
    process_t *process = task_get_process(task);
    vm_space_t *space;
    vma_t *vma;
    vaddr_t page = address & PAGE_MASK;
    paddr_t old_page;
    void *new_page;
    uint16_t references;
    int result;

    if (!process || !process->vm)
        return -EINVAL;
    space = process->vm;
    vma = find_vma(space, address);
    if (!vma || !(vma->flags & VMA_WRITE) || (vma->flags & VMA_SHARED))
        return -EACCES;

    old_page = get_physical_address(space->pgdir, page) & PAGE_MASK;
    if (!old_page)
        return -EFAULT;
    references = page_ref_count((void *)(uintptr_t)old_page);
    if (references == 0)
        return -EFAULT;
    if (references == 1) {
        return set_user_page_writable(space->pgdir, page, space->asid);
    }

    new_page = allocate_page();
    if (!new_page)
        return -ENOMEM;
    memcpy((void *)(uintptr_t)arm64_mmu_kernel_address(
               (paddr_t)(uintptr_t)new_page),
           (const void *)(uintptr_t)arm64_mmu_kernel_address(old_page),
           PAGE_SIZE);
    result = remap_user_page(space->pgdir, page,
                             (paddr_t)(uintptr_t)new_page,
                             vma->flags, space->asid);
    if (result < 0) {
        free_page(new_page);
        return result;
    }
    free_page((void *)(uintptr_t)old_page);
    return 0;
}
