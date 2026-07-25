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

static int exec_read_inode(inode_t *inode, void **image, size_t *image_size)
{
    file_t file;
    void *buffer;
    ssize_t bytes_read;
    int result;

    if (!inode || !image || !image_size || inode->size == 0 ||
        !inode->f_op || !inode->f_op->read)
        return -EINVAL;

    buffer = kmalloc(inode->size);
    if (!buffer)
        return -ENOMEM;

    memset(&file, 0, sizeof(file));
    file.inode = inode;
    file.flags = O_RDONLY;
    file.ref_count = 1;
    file.f_op = inode->f_op;
    if (file.f_op->open) {
        result = file.f_op->open(inode, &file);
        if (result < 0) {
            kfree(buffer);
            return result;
        }
    }

    file.offset = 0;
    bytes_read = file.f_op->read(&file, buffer, inode->size);
    if (file.f_op->close)
        file.f_op->close(&file);
    if (bytes_read != (ssize_t)inode->size) {
        kfree(buffer);
        return -ENOEXEC;
    }

    *image = buffer;
    *image_size = inode->size;
    return 0;
}

static int exec_map_segment(vm_space_t *vm, const void *image,
                            size_t image_size,
                            const exec_image_segment_t *segment)
{
    const uint8_t *bytes = image;
    vaddr_t segment_end;
    vaddr_t page_start;
    vaddr_t page_end;
    vaddr_t page;

    if (!vm || !image || !segment || segment->memory_size == 0 ||
        segment->file_size > segment->memory_size ||
        segment->file_offset > image_size ||
        segment->file_size > image_size - segment->file_offset ||
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
            size_t source_offset =
                (size_t)(segment->file_offset + data_start -
                         segment->virtual_address);
            size_t length = (size_t)(data_end - data_start);

            memcpy((void *)(uintptr_t)(temporary + destination_offset),
                   bytes + source_offset, length);
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
            offset <= segment->file_size &&
            layout->tls_file_size <= segment->file_size - offset)
            return true;
    }
    return false;
}

int exec_load_image(inode_t *inode, vm_space_t *vm, vaddr_t *entry,
                    exec_image_layout_t *loaded_layout)
{
    exec_image_layout_t layout;
    void *image;
    size_t image_size;
    uint32_t index;
    int result;

    if (!inode || !vm || !entry)
        return -EINVAL;
    result = exec_read_inode(inode, &image, &image_size);
    if (result < 0)
        return result;

    memset(&layout, 0, sizeof(layout));
    result = arch_exec_parse_image(image, image_size, &layout);
    if (result < 0 || layout.segment_count == 0 || !layout.entry ||
        !exec_tls_layout_valid(&layout)) {
        kfree(image);
        return -ENOEXEC;
    }

    for (index = 0; index < layout.segment_count; index++) {
        result = exec_map_segment(vm, image, image_size,
                                  &layout.segments[index]);
        if (result < 0) {
            kfree(image);
            return result;
        }
    }

    *entry = layout.entry;
    if (loaded_layout)
        *loaded_layout = layout;
    kfree(image);
    return 0;
}
