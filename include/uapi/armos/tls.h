/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/tls.h
 * Layer: UAPI / ELF thread-local storage
 *
 * Responsibilities:
 * - Describe the loaded ELF PT_TLS template without exposing kernel types.
 * - Keep TLS image sizes and alignment identical on every platform of one
 *   architecture.
 *
 * Notes:
 * - Pointer-sized fields follow the native ARM32 or ARM64 userspace ABI.
 * - Userspace owns allocation and initialization of each thread TLS block.
 */

#ifndef _UAPI_ARMOS_TLS_H
#define _UAPI_ARMOS_TLS_H

typedef struct {
    unsigned long image;
    unsigned long file_size;
    unsigned long memory_size;
    unsigned long alignment;
} armos_tls_info_t;

#endif /* _UAPI_ARMOS_TLS_H */
