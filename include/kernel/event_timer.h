/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/event_timer.h
 * Layer: Kernel / common descriptor services
 *
 * Responsibilities:
 * - Declare readiness helpers for event and timer descriptors.
 * - Keep descriptor implementations architecture and platform independent.
 * - Expose only the contracts needed by poll and select.
 */

#ifndef KERNEL_EVENT_TIMER_H
#define KERNEL_EVENT_TIMER_H

#include <kernel/task.h>

bool eventfd_read_ready(file_t *file);
bool eventfd_write_ready(file_t *file);
bool timerfd_read_ready(file_t *file);
bool timerfd_poll_deadline(file_t *file, uint32_t *deadline);

#endif /* KERNEL_EVENT_TIMER_H */
