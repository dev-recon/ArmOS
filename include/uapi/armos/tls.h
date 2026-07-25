/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/tls.h
 * Layer: UAPI / ELF thread-local storage
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
