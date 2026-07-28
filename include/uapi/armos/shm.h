/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/shm.h
 * Layer: UAPI / shared memory
 *
 * Responsibilities:
 * - Define the stable flags exchanged by shm_open wrappers and the kernel.
 * - Keep ArmOS extensions separate from the public POSIX flag namespace.
 *
 * Notes:
 * - Public libc wrappers translate O_* flags into these syscall flags.
 * - Values are architecture independent.
 */

#ifndef ARMOS_UAPI_SHM_H
#define ARMOS_UAPI_SHM_H

#define ARMOS_SHM_OPEN_CREATE    0x01
#define ARMOS_SHM_OPEN_EXCLUSIVE 0x02
#define ARMOS_SHM_OPEN_READONLY  0x04
#define ARMOS_SHM_OPEN_CLOEXEC   0x08
#define ARMOS_SHM_OPEN_TRUNCATE  0x10

#define ARMOS_SHM_MAP_READONLY   0x01
#define ARMOS_SHM_MAP_READWRITE  0x02

#endif /* ARMOS_UAPI_SHM_H */
