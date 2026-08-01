/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/arch_exec.h
 * Layer: Kernel / architecture boundary
 *
 * Responsibilities:
 * - Parse the active CPU executable ABI into the common image layout.
 * - Declare cache maintenance needed after loading executable pages.
 *
 * Notes:
 * - The generic loader owns VFS reads, VM policy and physical pages.
 */

#ifndef _KERNEL_ARCH_EXEC_H
#define _KERNEL_ARCH_EXEC_H

#include <kernel/exec.h>
#include <kernel/types.h>

int arch_exec_parse_image(const void *image, size_t header_size,
                          size_t file_size,
                          exec_image_layout_t *layout);
/*
 * The page has just been filled through a kernel temporary mapping.  The arch
 * hook owns the cache maintenance needed before the same physical page is
 * installed in the user address space.
 */
void arch_sync_loaded_user_page(vaddr_t mapped_vaddr, size_t size, bool executable);

#endif /* _KERNEL_ARCH_EXEC_H */
