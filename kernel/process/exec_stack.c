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
                              uint8_t *page_base, vaddr_t user_page,
                              vaddr_t *addresses)
{
    int index;

    for (index = count - 1; index >= 0; index--) {
        size_t length = strlen(strings[index]) + 1u;

        if ((size_t)(*cursor - page_base) < length)
            return -1;
        *cursor -= length;
        memcpy(*cursor, strings[index], length);
        addresses[index] = user_page +
            (vaddr_t)(uintptr_t)(*cursor - page_base);
    }
    return 0;
}

int setup_user_stack(vm_space_t *vm, char **argv, char **envp)
{
    const size_t word_size = sizeof(vaddr_t);
    const size_t alignment = ARCH_TASK_STACK_ALIGNMENT;
    const vaddr_t stack_bottom =
        USER_STACK_TOP - USER_STACK_SIZE + PAGE_SIZE;
    const vaddr_t stack_page = USER_STACK_TOP - PAGE_SIZE;
    vaddr_t *argv_addresses = NULL;
    vaddr_t *envp_addresses = NULL;
    vaddr_t temporary;
    uint8_t *page_base;
    uint8_t *cursor;
    uint8_t *vector_base;
    vaddr_t final_sp;
    void *physical_page;
    int argc = string_vector_count(argv);
    int envc = string_vector_count(envp);
    size_t vector_bytes;
    int index;

    if (!vm || create_vma(vm, stack_bottom, USER_STACK_SIZE - PAGE_SIZE,
                          VMA_READ | VMA_WRITE) == NULL)
        return -1;

    physical_page = allocate_page();
    if (!physical_page)
        return -1;
    if (map_user_page(vm->pgdir, stack_page,
                      (paddr_t)(uintptr_t)physical_page,
                      VMA_READ | VMA_WRITE, vm->asid) != 0) {
        free_page(physical_page);
        return -1;
    }

    temporary = map_temp_page((paddr_t)(uintptr_t)physical_page);
    if (!temporary)
        return -1;
    page_base = (uint8_t *)(uintptr_t)temporary;
    cursor = page_base + PAGE_SIZE;

    if (argc > 0) {
        argv_addresses = kmalloc((size_t)argc * sizeof(vaddr_t));
        if (!argv_addresses)
            goto failed;
    }
    if (envc > 0) {
        envp_addresses = kmalloc((size_t)envc * sizeof(vaddr_t));
        if (!envp_addresses)
            goto failed;
    }
    if (copy_stack_strings(argv, argc, &cursor, page_base, stack_page,
                           argv_addresses) != 0 ||
        copy_stack_strings(envp, envc, &cursor, page_base, stack_page,
                           envp_addresses) != 0)
        goto failed;

    vector_bytes = (size_t)(1 + argc + 1 + envc + 1) * word_size;
    if ((size_t)(cursor - page_base) < vector_bytes)
        goto failed;
    cursor = (uint8_t *)((uintptr_t)(cursor - vector_bytes) &
                         ~(uintptr_t)(alignment - 1u));
    if (cursor < page_base)
        goto failed;
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

    final_sp = stack_page + (vaddr_t)(vector_base - page_base);
    vm->stack_start = final_sp;

    unmap_temp_page((void *)(uintptr_t)temporary);
    kfree(argv_addresses);
    kfree(envp_addresses);
    return 0;

failed:
    unmap_temp_page((void *)(uintptr_t)temporary);
    kfree(argv_addresses);
    kfree(envp_addresses);
    KERROR("setup_user_stack: arguments exceed the initial stack page\n");
    return -1;
}
