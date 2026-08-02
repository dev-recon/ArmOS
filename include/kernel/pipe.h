/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/pipe.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare the filesystem-independent pipe and FIFO open contract.
 * - Keep named FIFO semantics shared by every architecture and filesystem.
 *
 * Notes:
 * - Filesystems persist FIFO inodes; this layer owns their live byte streams.
 */

#ifndef _KERNEL_PIPE_H
#define _KERNEL_PIPE_H

#include <kernel/task.h>

int pipe_open_named(inode_t* inode, file_t* file, int flags);

#endif /* _KERNEL_PIPE_H */
