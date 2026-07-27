/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/memory/usercopy.c
 * Layer: Kernel / virtual memory policy
 *
 * Responsibilities:
 * - Validate syscall buffers against the current process VMA list.
 * - Materialize lazy and stack pages before kernel-side user-memory access.
 * - Resolve copy-on-write before writing and copy through the kernel direct map.
 *
 * Notes:
 * - Architecture backends provide page-table lookup, fault and COW primitives.
 * - User virtual addresses are never dereferenced directly by this module.
 */

#include <kernel/address_space.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/userspace.h>

static vm_space_t *current_user_vm(void)
{
    task_t *task = task_current_local();
    process_t *process = task_get_process(task);

    if (!process || !process->vm)
        return NULL;
    return process->vm;
}

static void note_resolved_fault(bool lazy)
{
    task_t *task = task_current_local();

    if (!task)
        return;
    task->page_faults++;
    if (lazy)
        task->lazy_faults++;
    else
        task->stack_faults++;
}

static int prepare_user_page(vm_space_t *space, vaddr_t page, bool write)
{
    paddr_t physical;
    vma_t *vma;

    if (!space)
        return -1;
    physical = get_physical_address(space->pgdir, page) & PAGE_MASK;
    vma = find_vma(space, page);
    if (!vma)
        return -1;

    if (!physical) {
        if (handle_lazy_anon_fault(page, write) == 0) {
            note_resolved_fault(true);
            physical = get_physical_address(space->pgdir, page) & PAGE_MASK;
        } else if (handle_user_stack_fault(page) == 0) {
            note_resolved_fault(false);
            physical = get_physical_address(space->pgdir, page) & PAGE_MASK;
        }
    }
    if (!physical)
        return -1;

    if (write && !(vma->flags & VMA_SHARED) &&
        page_ref_count((void *)(uintptr_t)physical) > 1) {
        task_t *task;

        if (handle_cow_fault(page) != 0)
            return -1;
        task = task_current_local();
        if (task)
            task->cow_faults++;
    }
    return 0;
}

static int copy_user_pages(void *kernel_buffer, vaddr_t user_address,
                           size_t length, bool to_user)
{
    vm_space_t *vm = current_user_vm();
    uint8_t *kernel_bytes = kernel_buffer;
    size_t copied = 0;

    if (!vm || !kernel_buffer || length == 0)
        return -1;
    if (!vm_validate_user_range(vm, user_address, length,
                                to_user ? VMA_WRITE : VMA_READ))
        return -1;

    while (copied < length) {
        vaddr_t current = user_address + copied;
        vaddr_t page = current & PAGE_MASK;
        size_t offset = (size_t)(current & PAGE_OFFSET_MASK);
        size_t chunk = PAGE_SIZE - offset;
        paddr_t physical;
        uint8_t *mapped;

        if (chunk > length - copied)
            chunk = length - copied;
        if (prepare_user_page(vm, page, to_user) != 0)
            return -1;
        physical = get_physical_address(vm->pgdir, page);
        if (!physical)
            return -1;
        mapped = (uint8_t *)(uintptr_t)phys_to_virt(physical + offset);
        if (to_user)
            memcpy(mapped, kernel_bytes + copied, chunk);
        else
            memcpy(kernel_bytes + copied, mapped, chunk);
        copied += chunk;
    }
    return 0;
}

bool is_kernel_pointer(const void *ptr)
{
    return ptr && memory_is_kernel_address((vaddr_t)(uintptr_t)ptr);
}

bool is_valid_user_ptr(const void *ptr)
{
    vm_space_t *vm = current_user_vm();

    return ptr && vm_validate_user_range(
        vm, (vaddr_t)(uintptr_t)ptr, 1, VMA_READ);
}

bool is_valid_user_range(const void *ptr, size_t size)
{
    vm_space_t *vm = current_user_vm();

    return ptr && vm_validate_user_range(
        vm, (vaddr_t)(uintptr_t)ptr, size, VMA_READ);
}

int copy_to_user(void *to, const void *from, size_t n)
{
    return copy_user_pages((void *)from, (vaddr_t)(uintptr_t)to, n, true);
}

int copy_from_user(void *to, const void *from, size_t n)
{
    return copy_user_pages(to, (vaddr_t)(uintptr_t)from, n, false);
}

int copy_from_user_2d(void *to, size_t to_stride,
                      const void *from, size_t from_stride,
                      size_t row_bytes, size_t rows)
{
    vm_space_t *vm = current_user_vm();
    uint8_t *destination = to;
    vaddr_t source = (vaddr_t)(uintptr_t)from;
    size_t span;

    if (!vm || !to || !from || !row_bytes || !rows ||
        to_stride < row_bytes || from_stride < row_bytes)
        return -1;
    if (rows - 1u > ((size_t)-1 - row_bytes) / from_stride)
        return -1;
    span = (rows - 1u) * from_stride + row_bytes;
    if (!vm_validate_user_range(vm, source, span, VMA_READ))
        return -1;

    for (size_t row = 0u; row < rows; row++) {
        size_t copied = 0u;

        while (copied < row_bytes) {
            vaddr_t current = source + row * from_stride + copied;
            vaddr_t page = current & PAGE_MASK;
            size_t offset = (size_t)(current & PAGE_OFFSET_MASK);
            size_t chunk = PAGE_SIZE - offset;
            paddr_t physical;
            uint8_t *mapped;

            if (chunk > row_bytes - copied)
                chunk = row_bytes - copied;
            if (prepare_user_page(vm, page, false) != 0)
                return -1;
            physical = get_physical_address(vm->pgdir, page);
            if (!physical)
                return -1;
            mapped = (uint8_t *)(uintptr_t)phys_to_virt(physical + offset);
            memcpy(destination + row * to_stride + copied, mapped, chunk);
            copied += chunk;
        }
    }
    return 0;
}

int strncpy_from_user(char *to, const char *from, size_t max_len)
{
    size_t index;

    if (!to || !from || max_len == 0)
        return -1;
    for (index = 0; index < max_len; index++) {
        if (copy_from_user(&to[index], &from[index], 1) != 0)
            return -1;
        if (to[index] == '\0')
            return (int)index;
    }
    to[max_len - 1] = '\0';
    return -1;
}

int strnlen_user(const char *str, int maxlen)
{
    int index;
    char value;

    if (!str || maxlen <= 0)
        return -1;
    for (index = 0; index < maxlen; index++) {
        if (copy_from_user(&value, str + index, 1) != 0)
            return -1;
        if (value == '\0')
            return index;
    }
    return -1;
}

int copy_to_user_safe(void *to, const void *from, size_t n,
                      size_t max_size)
{
    return n <= max_size ? copy_to_user(to, from, n) : -1;
}

char *copy_string_from_user(const char *user_str)
{
    int length = strnlen_user(user_str, MAX_PATH);
    char *kernel_string;

    if (length < 0)
        return NULL;
    kernel_string = kmalloc((size_t)length + 1u);
    if (!kernel_string)
        return NULL;
    if (copy_from_user(kernel_string, user_str,
                       (size_t)length + 1u) != 0) {
        kfree(kernel_string);
        return NULL;
    }
    return kernel_string;
}
