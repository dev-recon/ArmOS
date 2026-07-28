/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/mman.h
 * Layer: Userland / C library compatibility
 *
 * Responsibilities:
 * - Declare POSIX memory mapping and named shared-memory interfaces.
 * - Publish the protection and mapping flags supported by ArmOS.
 *
 * Notes:
 * - Named shared-memory objects are resized with ftruncate().
 */

#ifndef _ARMOS_SYS_MMAN_H
#define _ARMOS_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_FAILED     ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);
int shm_open(const char *name, int oflag, mode_t mode);
int shm_unlink(const char *name);

#endif /* _ARMOS_SYS_MMAN_H */
