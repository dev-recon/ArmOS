/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/shm.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_SHM_H
#define _KERNEL_SHM_H

#include <kernel/types.h>
#include <kernel/memory.h>

#define SHM_NAME_MAX          32
#define SHM_MAX_OBJECTS       32
#define SHM_GLOBAL_MAX_BYTES  (256u * 1024u * 1024u)
#define SHM_GLOBAL_MAX_PAGES  (SHM_GLOBAL_MAX_BYTES / PAGE_SIZE)

#define SHM_O_CREAT         0x01
#define SHM_O_EXCL          0x02
#define SHM_RDONLY          0x01
#define SHM_RDWR            0x02

int sys_shm_open(const char *name, size_t size, int flags);
int sys_shm_unlink(const char *name);
void *sys_shm_map(int fd, void *addr, int flags);
int sys_shm_unmap(void *addr, size_t size);
void *shm_map_fd(int fd, void *addr, size_t size, uint32_t vma_flags);

void shm_retain_mapping(uint32_t shm_id);
void shm_release_mapping(uint32_t shm_id);

#endif
