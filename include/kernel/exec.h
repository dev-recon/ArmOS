/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/exec.h
 * Layer: Kernel / process execution
 *
 * Responsibilities:
 * - Describe an architecture-neutral executable image layout.
 * - Expose the shared VFS-to-VM executable loading operation.
 *
 * Notes:
 * - Architecture backends parse their ELF ABI into this layout.
 * - VFS acquisition, physical-page ownership and VM mappings stay common.
 */

#ifndef _KERNEL_EXEC_H
#define _KERNEL_EXEC_H

#include <kernel/types.h>

#define EXEC_IMAGE_MAX_SEGMENTS 16u

struct inode;
struct vm_space;

typedef struct exec_image_segment {
    uint64_t file_offset;
    uint64_t file_size;
    uint64_t memory_size;
    vaddr_t virtual_address;
    uint32_t flags;
} exec_image_segment_t;

typedef struct exec_image_layout {
    vaddr_t entry;
    uint32_t segment_count;
    vaddr_t tls_image;
    uint64_t tls_file_size;
    uint64_t tls_memory_size;
    uint64_t tls_alignment;
    exec_image_segment_t segments[EXEC_IMAGE_MAX_SEGMENTS];
} exec_image_layout_t;

int exec_load_image(struct inode *inode, struct vm_space *vm,
                    vaddr_t *entry, exec_image_layout_t *loaded_layout);

#endif /* _KERNEL_EXEC_H */
