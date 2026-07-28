/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/signal.h
 * Layer: Userland / POSIX signal compatibility
 *
 * Responsibilities:
 * - Publish the upper bound of the native ArmOS signal namespace.
 * - Fill the SIGRTMAX omission in the bare-metal newlib target profile.
 *
 * Notes:
 * - ArmOS currently provides 32 signal slots and no separate RT queue.
 */

#ifndef ARMOS_SIGNAL_COMPAT_H
#define ARMOS_SIGNAL_COMPAT_H

#include <signal.h>

#ifndef SIGRTMAX
#define SIGRTMAX 32
#endif

#endif /* ARMOS_SIGNAL_COMPAT_H */
