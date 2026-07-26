/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/time.h
 * Layer: UAPI / time ABI
 *
 * Responsibilities:
 * - Define architecture-independent time structures used by ArmOS syscalls.
 * - Keep ARM32 and ARM64 newlib layouts out of the kernel ABI.
 *
 * Notes:
 * - The legacy nanosleep structure preserves the released 32-bit ABI.
 * - New clock interfaces use signed 64-bit seconds and nanoseconds.
 */

#ifndef _UAPI_ARMOS_TIME_H
#define _UAPI_ARMOS_TIME_H

typedef struct {
    unsigned int sec;
    unsigned int nsec;
} armos_timespec32_t;

typedef struct {
    signed long long sec;
    signed long long nsec;
} armos_timespec_t;

struct armos_itimerspec {
    armos_timespec_t interval;
    armos_timespec_t value;
};

#define ARMOS_CLOCK_REALTIME  0
#define ARMOS_CLOCK_MONOTONIC 1

#define ARMOS_TIMER_ABSTIME 1

#define ARMOS_UTIME_NOW  (-2LL)
#define ARMOS_UTIME_OMIT (-1LL)

#endif
