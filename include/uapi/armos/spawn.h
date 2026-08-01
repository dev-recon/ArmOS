/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/spawn.h
 * Layer: UAPI / process creation ABI
 *
 * Responsibilities:
 * - Describe the architecture-neutral direct process-spawn contract.
 * - Keep process attributes identical for ARM32 and ARM64 callers.
 *
 * Notes:
 * - The structure contains no native pointers and is ABI-stable.
 * - An unpublished child is either made runnable in full or discarded.
 */

#ifndef _UAPI_ARMOS_SPAWN_H
#define _UAPI_ARMOS_SPAWN_H

#define ARMOS_SPAWN_ABI_VERSION 1u

#define ARMOS_SPAWN_SET_UID (1u << 0)
#define ARMOS_SPAWN_SET_GID (1u << 1)
#define ARMOS_SPAWN_SET_CWD (1u << 2)
#define ARMOS_SPAWN_VALID_FLAGS \
    (ARMOS_SPAWN_SET_UID | ARMOS_SPAWN_SET_GID | ARMOS_SPAWN_SET_CWD)

#define ARMOS_SPAWN_CWD_MAX 256u

typedef struct armos_spawn_attributes {
    unsigned int abi_version;
    unsigned int flags;
    unsigned int uid;
    unsigned int gid;
    char cwd[ARMOS_SPAWN_CWD_MAX];
} armos_spawn_attributes_t;

#endif
