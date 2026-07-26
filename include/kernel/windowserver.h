/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/windowserver.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare the bootstrap hook for the userland window server.
 * - Keep graphical service policy outside architecture and platform code.
 *
 * Notes:
 * - A kernel supervisor thread creates the root userland compositor process.
 * - Wayland protocol and compositor logic remain entirely in userland.
 */

#ifndef _KERNEL_WINDOWSERVER_H
#define _KERNEL_WINDOWSERVER_H

int windowserverd_start(void);

#endif /* _KERNEL_WINDOWSERVER_H */
