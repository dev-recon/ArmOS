/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/pty.h
 * Layer: Kernel / common terminal services
 *
 * Responsibilities:
 * - Define the architecture-neutral POSIX pseudo-terminal interface.
 * - Connect PTY masters to the common TTY line discipline.
 * - Expose readiness and control operations to descriptor syscalls.
 */

#ifndef KERNEL_PTY_H
#define KERNEL_PTY_H

#include <kernel/task.h>

#define ARMOS_TIOCGPTN   0x80045430u
#define ARMOS_TIOCSPTLCK 0x40045431u

bool pty_is_master_path(const char *path);
bool pty_is_slave_path(const char *path);
int pty_slave_tty_id(const char *path);
file_t *pty_create_master_file(int flags);
bool pty_master_read_ready(file_t *file);
bool pty_master_write_ready(file_t *file);
int pty_master_tty_id(file_t *file);
int pty_master_ioctl(file_t *file, uint32_t request, uintptr_t arg);

#endif
