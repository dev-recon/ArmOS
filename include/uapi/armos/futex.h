/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/futex.h
 * Layer: UAPI / userspace synchronization
 */

#ifndef _UAPI_ARMOS_FUTEX_H
#define _UAPI_ARMOS_FUTEX_H

#define ARMOS_FUTEX_WAIT 0
#define ARMOS_FUTEX_WAKE 1

/* Raw kernel ABI result translated to the libc-specific ETIMEDOUT value. */
#define ARMOS_FUTEX_RESULT_TIMEDOUT (-110)

#endif /* _UAPI_ARMOS_FUTEX_H */
