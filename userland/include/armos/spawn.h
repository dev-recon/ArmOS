/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/spawn.h
 * Layer: Userland / process API
 *
 * Responsibilities:
 * - Expose the ArmOS direct process-spawn service to applications.
 * - Preserve errno-style userland return conventions.
 */

#ifndef _ARMOS_SPAWN_H
#define _ARMOS_SPAWN_H

#include <sys/types.h>
#include <uapi/armos/spawn.h>

#ifdef __cplusplus
extern "C" {
#endif

pid_t armos_spawnve(const char *path, char *const argv[],
                    char *const envp[],
                    const armos_spawn_attributes_t *attributes);

#ifdef __cplusplus
}
#endif

#endif
