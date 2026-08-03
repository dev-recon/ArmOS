/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/process/exec_stack.c
 * Layer: Kernel / process execution
 *
 * Responsibilities:
 * - Release kernel copies of execve path, argument and environment vectors.
 * - Build the native user startup stack consumed by the C runtime.
 * - Keep stack VMA policy identical across supported architectures.
 *
 * Notes:
 * - Pointer width and stack alignment follow the active architecture ABI
 *   through vaddr_t and ARCH_TASK_STACK_ALIGNMENT.
 */

#include <kernel/arch_task.h>
#include <kernel/kprintf.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/userspace.h>
#include <uapi/armos/limits.h>

static int string_vector_count(char **vector)
{
    int count = 0;

    if (vector) {
        while (vector[count])
            count++;
    }
    return count;
}

void cleanup_exec_args(char *filename, char **argv, char **envp)
{
    int index;

    if (filename)
        kfree(filename);
    if (argv) {
        for (index = 0; argv[index]; index++)
            kfree(argv[index]);
        kfree(argv);
    }
    if (envp) {
        for (index = 0; envp[index]; index++)
            kfree(envp[index]);
        kfree(envp);
    }
}

static int copy_stack_strings(char **strings, int count, uint8_t **cursor,
                              uint8_t *buffer_base, vaddr_t user_base,
                              vaddr_t *addresses)
{
    int index;

    for (index = count - 1; index >= 0; index--) {
        size_t length = strlen(strings[index]) + 1u;

        if ((size_t)(*cursor - buffer_base) < length)
            return -1;
        *cursor -= length;
        memcpy(*cursor, strings[index], length);
        addresses[index] = user_base +
            (vaddr_t)(uintptr_t)(*cursor - buffer_base);
    }
    return 0;
}

int setup_user_stack(vm_space_t *vm, char **argv, char **envp)
{
    const size_t word_size = sizeof(vaddr_t);
    const size_t alignment = ARCH_TASK_STACK_ALIGNMENT;
    const vaddr_t stack_bottom =
        USER_STACK_TOP - USER_STACK_SIZE + PAGE_SIZE;
    vaddr_t *argv_addresses = NULL;
    vaddr_t *envp_addresses = NULL;
    uint8_t *stack_image = NULL;
    uint8_t *cursor;
    uint8_t *vector_base;
    vaddr_t final_sp;
    vaddr_t mapped_base;
    int argc = string_vector_count(argv);
    int envc = string_vector_count(envp);
    size_t string_bytes = 0;
    size_t vector_bytes;
    size_t required_bytes;
    size_t mapped_bytes;
    size_t page_offset;
    size_t vector_offset;
    int index;

    if (!vm || create_vma(vm, stack_bottom, USER_STACK_SIZE - PAGE_SIZE,
                          VMA_READ | VMA_WRITE) == NULL)
        return -ENOMEM;

    for (index = 0; index < argc; index++)
        string_bytes += strlen(argv[index]) + 1u;
    for (index = 0; index < envc; index++)
        string_bytes += strlen(envp[index]) + 1u;
    vector_bytes = (size_t)(1 + argc + 1 + envc + 1) * word_size;
    if (string_bytes > ARMOS_ARG_MAX || vector_bytes > ARMOS_ARG_MAX ||
        string_bytes + vector_bytes > ARMOS_ARG_MAX)
        return -E2BIG;

    required_bytes = string_bytes + vector_bytes + alignment - 1u;
    mapped_bytes = (required_bytes + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (mapped_bytes == 0 || mapped_bytes > USER_STACK_SIZE - PAGE_SIZE)
        return -E2BIG;
    mapped_base = USER_STACK_TOP - (vaddr_t)mapped_bytes;
    stack_image = kmalloc(mapped_bytes);
    if (!stack_image)
        return -ENOMEM;
    memset(stack_image, 0, mapped_bytes);
    cursor = stack_image + mapped_bytes;

    if (argc > 0) {
        argv_addresses = kmalloc((size_t)argc * sizeof(vaddr_t));
        if (!argv_addresses)
            goto no_memory;
    }
    if (envc > 0) {
        envp_addresses = kmalloc((size_t)envc * sizeof(vaddr_t));
        if (!envp_addresses)
            goto no_memory;
    }
    if (copy_stack_strings(argv, argc, &cursor, stack_image, mapped_base,
                           argv_addresses) != 0 ||
        copy_stack_strings(envp, envc, &cursor, stack_image, mapped_base,
                           envp_addresses) != 0)
        goto too_large;

    if ((size_t)(cursor - stack_image) < vector_bytes)
        goto too_large;
    vector_offset = (size_t)(cursor - stack_image) - vector_bytes;
    vector_offset &= ~(alignment - 1u);
    cursor = stack_image + vector_offset;
    vector_base = cursor;

    *(vaddr_t *)(void *)cursor = (vaddr_t)argc;
    cursor += word_size;
    for (index = 0; index < argc; index++, cursor += word_size)
        *(vaddr_t *)(void *)cursor = argv_addresses[index];
    *(vaddr_t *)(void *)cursor = 0;
    cursor += word_size;
    for (index = 0; index < envc; index++, cursor += word_size)
        *(vaddr_t *)(void *)cursor = envp_addresses[index];
    *(vaddr_t *)(void *)cursor = 0;

    final_sp = mapped_base + (vaddr_t)(vector_base - stack_image);
    if ((final_sp & (vaddr_t)(alignment - 1u)) != 0)
        goto too_large;
    vm->stack_start = final_sp;

    for (page_offset = 0; page_offset < mapped_bytes;
         page_offset += PAGE_SIZE) {
        void *physical_page = allocate_page();
        vaddr_t temporary;

        if (!physical_page)
            goto no_memory;
        if (map_user_page(vm->pgdir, mapped_base + (vaddr_t)page_offset,
                          (paddr_t)(uintptr_t)physical_page,
                          VMA_READ | VMA_WRITE, vm->asid) != 0) {
            free_page(physical_page);
            goto no_memory;
        }
        temporary = map_temp_page((paddr_t)(uintptr_t)physical_page);
        if (!temporary)
            goto no_memory;
        memcpy((void *)(uintptr_t)temporary, stack_image + page_offset,
               PAGE_SIZE);
        unmap_temp_page((void *)(uintptr_t)temporary);
    }

    kfree(stack_image);
    kfree(argv_addresses);
    kfree(envp_addresses);
    return 0;

too_large:
    kfree(stack_image);
    kfree(argv_addresses);
    kfree(envp_addresses);
    return -E2BIG;

no_memory:
    kfree(stack_image);
    kfree(argv_addresses);
    kfree(envp_addresses);
    return -ENOMEM;
}
