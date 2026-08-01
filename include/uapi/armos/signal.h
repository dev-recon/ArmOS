/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/uapi/armos/signal.h
 * Layer: Kernel / user ABI
 *
 * Responsibilities:
 * - Define the signal-action flags carried across the ArmOS syscall ABI.
 * - Keep the wire representation independent from the public libc values.
 *
 * Notes:
 * - Newlib translates its POSIX-facing SA_* values to these ABI bits.
 * - These values must remain stable for already-built ArmOS executables.
 */

#ifndef UAPI_ARMOS_SIGNAL_H
#define UAPI_ARMOS_SIGNAL_H

#define ARMOS_SA_RESTART    0x01u
#define ARMOS_SA_NODEFER    0x02u
#define ARMOS_SA_RESETHAND  0x04u
#define ARMOS_SA_NOCLDWAIT  0x08u

#endif /* UAPI_ARMOS_SIGNAL_H */
