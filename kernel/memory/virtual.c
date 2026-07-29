/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/memory/virtual.c
 * Layer: Kernel / virtual memory policy
 *
 * Responsibilities:
 * - Own the architecture-neutral VMA list and free-range policy.
 * - Split and remove anonymous mappings while preserving page ownership.
 * - Resolve lazy anonymous and user-stack faults through MMU primitives.
 *
 * Notes:
 * - Architecture backends own page tables, ASIDs and TLB invalidation.
 * - Shared mappings may only be unmapped as complete VMAs until SHM
 *   reference accounting becomes split-aware.
 */

#include <kernel/arch_barrier.h>
#include <kernel/memory.h>
#include <kernel/shm.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/tlb.h>
#include <kernel/util.h>

/*
 * Page-table creation is not reentrant within one address space. Serialize
 * demand faults so simultaneous pthread stack growth cannot race while
 * installing intermediate translation tables.
 */
static spinlock_t vm_fault_lock = SPINLOCK_INIT("vm_fault");

static void vm_fault_lock_acquire(void)
{
    /*
     * Fault handlers can run with local IRQs masked. Service pending TLB
     * rendezvous while contending, otherwise the lock owner may wait forever
     * for this CPU's shootdown acknowledgement.
     */
    while (!spin_trylock(&vm_fault_lock)) {
        tlb_handle_remote_ipi(smp_processor_id());
        arch_cpu_relax();
    }
}

static bool vm_range_overlaps(const vma_t *vma, vaddr_t start, vaddr_t end)
{
    return vma && start < vma->end && end > vma->start;
}

void vm_initialize_user_layout(vm_space_t *space)
{
    if (!space)
        return;
    space->vma_list = NULL;
    space->heap_start = USER_HEAP_START;
    space->heap_end = USER_HEAP_END;
    space->brk = USER_HEAP_START;
    space->stack_start = USER_STACK_TOP;
}

void vm_release_vmas(vm_space_t *space)
{
    vma_t *vma;

    if (!space)
        return;
    vma = space->vma_list;
    space->vma_list = NULL;
    while (vma) {
        vma_t *next = vma->next;

        if ((vma->flags & VMA_SHARED) &&
            !(vma->flags & VMA_EXTERNAL))
            shm_release_mapping(vma->shm_id);
        kfree(vma);
        vma = next;
    }
}

vma_t *find_vma(vm_space_t *space, vaddr_t address)
{
    vma_t *vma;

    for (vma = space ? space->vma_list : NULL; vma; vma = vma->next) {
        if (address >= vma->start && address < vma->end)
            return vma;
    }
    return NULL;
}

bool vm_validate_user_range(vm_space_t *space, vaddr_t address,
                            size_t length, uint32_t required_flags)
{
    vaddr_t cursor = address;
    vaddr_t end;

    if (!space || length == 0 || required_flags == 0)
        return false;
    end = address + (vaddr_t)length;
    if (end <= address || end > get_split_boundary() ||
        memory_range_overlaps_low_kernel_alias(address, end))
        return false;

    while (cursor < end) {
        vma_t *vma = find_vma(space, cursor);

        if (!vma || (vma->flags & required_flags) != required_flags ||
            vma->end <= cursor)
            return false;
        cursor = vma->end < end ? vma->end : end;
    }
    return true;
}

vma_t *create_vma(vm_space_t *space, vaddr_t start, size_t size,
                  uint32_t flags)
{
    vaddr_t end;
    vma_t **link;
    vma_t *vma;

    if (!space || size == 0)
        return NULL;
    end = start + (vaddr_t)size;
    if (end <= start || end > get_split_boundary())
        return NULL;

    link = &space->vma_list;
    while (*link && (*link)->end <= start)
        link = &(*link)->next;
    if (*link && (*link)->start < end)
        return NULL;

    vma = kzalloc(sizeof(*vma));
    if (!vma)
        return NULL;
    vma->start = start;
    vma->end = end;
    vma->flags = flags;
    vma->next = *link;
    *link = vma;
    return vma;
}

static int vm_clone_resident_page(vm_space_t *parent, vm_space_t *child,
                                  const vma_t *vma, vaddr_t page)
{
    paddr_t physical = get_physical_address(parent->pgdir, page);
    int result;

    if (!physical)
        return 0;
    physical &= PAGE_MASK;
    if (page_ref_inc((void *)(uintptr_t)physical) < 0)
        return -ENOMEM;

    if ((vma->flags & VMA_WRITE) && !(vma->flags & VMA_SHARED)) {
        if (set_user_page_readonly(parent->pgdir, page,
                                   parent->asid) < 0) {
            free_page((void *)(uintptr_t)physical);
            return -EFAULT;
        }
    }

    if (vma->flags & VMA_SHARED)
        result = map_user_page(child->pgdir, page, physical,
                               vma->flags, child->asid);
    else
        result = map_user_page_readonly(child->pgdir, page, physical,
                                        vma->flags, child->asid);
    if (result < 0) {
        free_page((void *)(uintptr_t)physical);
        if ((vma->flags & VMA_WRITE) &&
            page_ref_count((void *)(uintptr_t)physical) == 1)
            (void)set_user_page_writable(parent->pgdir, page,
                                         parent->asid);
        return result;
    }
    return 0;
}

vm_space_t *fork_vm_space(vm_space_t *parent)
{
    vm_space_t *child;
    vma_t *source;

    if (!parent)
        return NULL;
    child = create_vm_space();
    if (!child)
        return NULL;

    for (source = parent->vma_list; source; source = source->next) {
        vaddr_t page;
        vma_t *copy;

        if (source->end <= source->start ||
            (source->flags & VMA_DONTFORK))
            continue;
        copy = create_vma(child, source->start,
                          source->end - source->start,
                          source->flags);
        if (!copy)
            goto failed;
        copy->shm_id = source->shm_id;
        if (source->flags & VMA_SHARED)
            shm_retain_mapping(source->shm_id);

        for (page = PAGE_ALIGN_DOWN(source->start);
             page < PAGE_ALIGN_UP(source->end); page += PAGE_SIZE) {
            if (vm_clone_resident_page(parent, child, source, page) < 0)
                goto failed;
        }
    }

    child->heap_start = parent->heap_start;
    child->heap_end = parent->heap_end;
    child->brk = parent->brk;
    child->stack_start = parent->stack_start;
    return child;

failed:
    destroy_vm_space(child);
    return NULL;
}

int remove_vma(vm_space_t *space, vaddr_t start, vaddr_t end)
{
    vma_t **link;
    vma_t *vma;

    if (!space || end <= start)
        return -EINVAL;
    link = &space->vma_list;
    while (*link) {
        vma = *link;
        if (vma->start == start && vma->end == end) {
            *link = vma->next;
            kfree(vma);
            return 0;
        }
        link = &vma->next;
    }
    return -ENOENT;
}

vaddr_t vm_find_free_range(vm_space_t *space, vaddr_t hint, size_t size,
                           vaddr_t base, vaddr_t limit)
{
    vaddr_t address;
    vaddr_t end;
    vma_t *vma;

    if (!space || size == 0 || !IS_PAGE_ALIGNED(size) ||
        !IS_PAGE_ALIGNED(base) || !IS_PAGE_ALIGNED(limit) ||
        base >= limit || size > limit - base)
        return 0;

    if (hint >= base && hint < limit && IS_PAGE_ALIGNED(hint) &&
        size <= limit - hint) {
        end = hint + size;
        for (vma = space->vma_list; vma; vma = vma->next) {
            if (vm_range_overlaps(vma, hint, end))
                break;
        }
        if (!vma)
            return hint;
    }

    address = base;
    for (vma = space->vma_list; vma; vma = vma->next) {
        if (vma->end <= address || vma->end <= base)
            continue;
        if (vma->start >= limit)
            break;
        if (size <= vma->start - address)
            return address;
        if (vma->end > address)
            address = ALIGN_UP(vma->end, PAGE_SIZE);
        if (address < base)
            address = base;
        if (address >= limit || size > limit - address)
            return 0;
    }
    return address < limit && size <= limit - address ? address : 0;
}

int vm_unmap_range(vm_space_t *space, vaddr_t start, size_t size)
{
    vaddr_t end;
    vma_t *vma;
    vma_t *previous;

    if (!space || size == 0 || !IS_PAGE_ALIGNED(start))
        return -EINVAL;
    size = ALIGN_UP(size, PAGE_SIZE);
    end = start + size;
    if (end <= start || end > get_split_boundary())
        return -EINVAL;

    for (vma = space->vma_list; vma; vma = vma->next) {
        vaddr_t cut_start;
        vaddr_t cut_end;

        if (!vm_range_overlaps(vma, start, end) ||
            !(vma->flags & VMA_SHARED))
            continue;
        cut_start = start > vma->start ? start : vma->start;
        cut_end = end < vma->end ? end : vma->end;
        if (cut_start != vma->start || cut_end != vma->end)
            return -EINVAL;
    }

    previous = NULL;
    vma = space->vma_list;
    while (vma) {
        vma_t *next = vma->next;
        vaddr_t cut_start;
        vaddr_t cut_end;
        vaddr_t page;

        if (vma->end <= start) {
            previous = vma;
            vma = next;
            continue;
        }
        if (vma->start >= end)
            break;

        cut_start = start > vma->start ? start : vma->start;
        cut_end = end < vma->end ? end : vma->end;
        if (cut_start >= cut_end) {
            previous = vma;
            vma = next;
            continue;
        }

        if (cut_start > vma->start && cut_end < vma->end) {
            vma_t *right = kmalloc(sizeof(*right));

            if (!right)
                return -ENOMEM;
            *right = *vma;
            right->start = cut_end;
            right->next = vma->next;
            for (page = cut_start; page < cut_end; page += PAGE_SIZE) {
                paddr_t physical =
                    get_physical_address(space->pgdir, page);

                if (physical != 0 &&
                    unmap_user_page(space->pgdir, page, space->asid) == 0 &&
                    !(vma->flags & VMA_EXTERNAL))
                    free_page((void *)(uintptr_t)physical);
            }
            vma->end = cut_start;
            vma->next = right;
            previous = right;
            vma = right->next;
            continue;
        }

        for (page = cut_start; page < cut_end; page += PAGE_SIZE) {
            paddr_t physical = get_physical_address(space->pgdir, page);

            if (physical != 0 &&
                unmap_user_page(space->pgdir, page, space->asid) == 0 &&
                !(vma->flags & VMA_EXTERNAL))
                free_page((void *)(uintptr_t)physical);
        }

        if (cut_start == vma->start && cut_end == vma->end) {
            if (previous)
                previous->next = next;
            else
                space->vma_list = next;
            if ((vma->flags & VMA_SHARED) &&
                !(vma->flags & VMA_EXTERNAL))
                shm_release_mapping(vma->shm_id);
            kfree(vma);
            vma = next;
        } else if (cut_start == vma->start) {
            vma->start = cut_end;
            previous = vma;
            vma = next;
        } else {
            vma->end = cut_start;
            previous = vma;
            vma = next;
        }
    }
    return 0;
}

uint32_t vm_virtual_kb(vm_space_t *space)
{
    uint64_t bytes = 0;
    vma_t *vma;

    for (vma = space ? space->vma_list : NULL; vma; vma = vma->next)
        bytes += vma->end - vma->start;
    return bytes / 1024u > 0xffffffffu ? 0xffffffffu :
           (uint32_t)(bytes / 1024u);
}

static vm_space_t *current_user_vm(void)
{
    task_t *task = task_current_local();
    process_t *process = task_get_process(task);

    return process ? process->vm : NULL;
}

int handle_lazy_anon_fault(vaddr_t address, bool write)
{
    vm_space_t *space = current_user_vm();
    vma_t *vma;
    vaddr_t page_address = address & PAGE_MASK;
    void *page;
    int result = 0;

    if (!space)
        return -EINVAL;
    vm_fault_lock_acquire();
    vma = find_vma(space, address);
    if (!vma || !(vma->flags & VMA_LAZY)) {
        result = -EINVAL;
        goto out;
    }
    if ((write && !(vma->flags & VMA_WRITE)) ||
        (!write && !(vma->flags & VMA_READ))) {
        result = -EACCES;
        goto out;
    }
    if (get_physical_address(space->pgdir, page_address) != 0)
        goto out;

    page = allocate_page();
    if (!page) {
        result = -ENOMEM;
        goto out;
    }
    if (map_user_page(space->pgdir, page_address,
                      (paddr_t)(uintptr_t)page, vma->flags,
                      space->asid) < 0) {
        free_page(page);
        result = -ENOMEM;
    }
out:
    spin_unlock(&vm_fault_lock);
    return result;
}

int handle_user_stack_fault(vaddr_t address)
{
    vm_space_t *space = current_user_vm();
    vma_t *vma;
    vaddr_t page_address;
    void *page;
    int result = 0;

    if (!space || address < USER_STACK_BOTTOM + PAGE_SIZE ||
        address >= USER_STACK_TOP)
        return -EINVAL;
    vm_fault_lock_acquire();
    vma = find_vma(space, address);
    if (!vma || !(vma->flags & VMA_WRITE)) {
        result = -EACCES;
        goto out;
    }
    page_address = address & PAGE_MASK;
    if (get_physical_address(space->pgdir, page_address) != 0)
        goto out;

    page = allocate_page();
    if (!page) {
        result = -ENOMEM;
        goto out;
    }
    if (map_user_page(space->pgdir, page_address,
                      (paddr_t)(uintptr_t)page, vma->flags,
                      space->asid) < 0) {
        free_page(page);
        result = -ENOMEM;
    }
out:
    spin_unlock(&vm_fault_lock);
    return result;
}
