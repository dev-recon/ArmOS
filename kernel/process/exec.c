/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/process/exec.c
 * Layer: Kernel / process execution
 *
 * Responsibilities:
 * - Read executable images through the shared VFS contract.
 * - Map parsed load segments into a new generic VM space.
 * - Own executable page allocation and initialization.
 *
 * Notes:
 * - ELF class and machine parsing are architecture ABI mechanisms.
 * - Process publication and credential changes remain in sys_execve().
 */

#include <kernel/arch_exec.h>
#include <kernel/exec.h>
#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/vfs.h>

static int exec_open_inode(inode_t *inode, file_t *file)
{
    int result;

    if (!inode || !file || inode->size == 0 ||
        !inode->f_op || !inode->f_op->read)
        return -EINVAL;

    memset(file, 0, sizeof(*file));
    file->inode = inode;
    file->flags = O_RDONLY;
    file->ref_count = 1;
    file->f_op = inode->f_op;
    if (file->f_op->open) {
        result = file->f_op->open(inode, file);
        if (result < 0)
            return result;
    }
    return 0;
}

static int exec_read_exact(file_t *file, uint64_t offset, void *buffer,
                           size_t size)
{
    uint8_t *bytes = buffer;
    size_t total = 0;

    if (!file || !buffer || offset > 0xffffffffULL ||
        (uint64_t)size > 0x100000000ULL - offset)
        return -ENOEXEC;
    file->offset = (uint32_t)offset;
    while (total < size) {
        ssize_t result = file->f_op->read(file, bytes + total, size - total);

        if (result < 0)
            return (int)result;
        if (result == 0)
            return -ENOEXEC;
        total += (size_t)result;
    }
    return 0;
}

static int exec_map_segment(vm_space_t *vm, file_t *file, size_t file_size,
                            const exec_image_segment_t *segment)
{
    vaddr_t segment_end;
    vaddr_t page_start;
    vaddr_t page_end;
    vaddr_t page;

    if (!vm || !file || !segment || segment->memory_size == 0 ||
        segment->file_size > segment->memory_size ||
        segment->file_offset > file_size ||
        segment->file_size > file_size - segment->file_offset ||
        segment->virtual_address + segment->memory_size <
            segment->virtual_address)
        return -ENOEXEC;

    segment_end = segment->virtual_address + segment->memory_size;
    page_start = segment->virtual_address & PAGE_MASK;
    page_end = ALIGN_UP(segment_end, PAGE_SIZE);
    if (!create_vma(vm, page_start, page_end - page_start, segment->flags))
        return -ENOMEM;

    for (page = page_start; page < page_end; page += PAGE_SIZE) {
        vaddr_t data_start = page;
        vaddr_t data_end = page + PAGE_SIZE;
        vaddr_t file_end = segment->virtual_address + segment->file_size;
        void *physical_page;
        vaddr_t temporary;

        physical_page = allocate_page();
        if (!physical_page)
            return -ENOMEM;
        temporary = map_temp_page((paddr_t)(uintptr_t)physical_page);
        if (!temporary) {
            free_page(physical_page);
            return -ENOMEM;
        }

        memset((void *)(uintptr_t)temporary, 0, PAGE_SIZE);
        if (data_start < segment->virtual_address)
            data_start = segment->virtual_address;
        if (data_end > file_end)
            data_end = file_end;
        if (data_start < data_end) {
            size_t destination_offset = (size_t)(data_start - page);
            uint64_t source_offset = segment->file_offset + data_start -
                                     segment->virtual_address;
            size_t length = (size_t)(data_end - data_start);

            int result = exec_read_exact(
                file, source_offset,
                (void *)(uintptr_t)(temporary + destination_offset), length);

            if (result < 0) {
                unmap_temp_page((void *)(uintptr_t)temporary);
                free_page(physical_page);
                return result;
            }
        }

        arch_sync_loaded_user_page(temporary, PAGE_SIZE,
                                   (segment->flags & VMA_EXEC) != 0);
        unmap_temp_page((void *)(uintptr_t)temporary);
        if (map_user_page(vm->pgdir, page,
                          (paddr_t)(uintptr_t)physical_page,
                          segment->flags, vm->asid) < 0) {
            free_page(physical_page);
            return -ENOMEM;
        }
    }
    return 0;
}

static bool exec_tls_layout_valid(const exec_image_layout_t *layout)
{
    uint32_t index;

    if (!layout->tls_memory_size)
        return true;
    for (index = 0; index < layout->segment_count; index++) {
        const exec_image_segment_t *segment = &layout->segments[index];
        uint64_t offset;

        if (layout->tls_image < segment->virtual_address ||
            !(segment->flags & VMA_READ))
            continue;
        offset = (uint64_t)(layout->tls_image - segment->virtual_address);
        if (offset <= segment->memory_size &&
            layout->tls_memory_size <= segment->memory_size - offset &&
            (!layout->tls_file_size ||
             (offset <= segment->file_size &&
              layout->tls_file_size <= segment->file_size - offset)))
            return true;
    }
    return false;
}

int exec_load_image(inode_t *inode, vm_space_t *vm, vaddr_t *entry,
                    exec_image_layout_t *loaded_layout)
{
    exec_image_layout_t layout;
    file_t file;
    void *header;
    size_t header_size;
    uint32_t index;
    int result;

    if (!inode || !vm || !entry)
        return -EINVAL;
    result = exec_open_inode(inode, &file);
    if (result < 0)
        return result;

    header_size = inode->size;
    if (header_size > EXEC_IMAGE_HEADER_LIMIT)
        header_size = EXEC_IMAGE_HEADER_LIMIT;
    header = kmalloc(header_size);
    if (!header) {
        result = -ENOMEM;
        goto close_file;
    }
    result = exec_read_exact(&file, 0, header, header_size);
    if (result < 0)
        goto free_header;

    memset(&layout, 0, sizeof(layout));
    result = arch_exec_parse_image(header, header_size, inode->size, &layout);
    if (result < 0 || layout.segment_count == 0 || !layout.entry ||
        !exec_tls_layout_valid(&layout)) {
        result = -ENOEXEC;
        goto free_header;
    }

    for (index = 0; index < layout.segment_count; index++) {
        result = exec_map_segment(vm, &file, inode->size,
                                  &layout.segments[index]);
        if (result < 0)
            goto free_header;
    }

    *entry = layout.entry;
    if (loaded_layout)
        *loaded_layout = layout;
    result = 0;

free_header:
    kfree(header);
close_file:
    if (file.f_op->close)
        file.f_op->close(&file);
    return result;
}
