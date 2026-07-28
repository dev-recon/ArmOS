/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/net/armos_socket.h
 * Layer: Kernel / local inter-process communication
 *
 * Responsibilities:
 * - Expose the architecture-neutral local stream socket backend.
 * - Integrate local sockets with common file descriptors and poll.
 *
 * Notes:
 * - POSIX address-family names remain confined to the public socket ABI.
 * - No architecture or platform-specific behavior belongs in this module.
 */

#ifndef _KERNEL_NET_ARMOS_SOCKET_H
#define _KERNEL_NET_ARMOS_SOCKET_H

#include <kernel/task.h>
#include <kernel/types.h>

#define ARMOS_AF_LOCAL 1
#define ARMOS_SOCK_STREAM 1

struct armos_msghdr_kernel;

bool armos_socket_is_fd(int fd);
bool armos_socket_read_ready(file_t *file);
bool armos_socket_write_ready(file_t *file);

int armos_socket_create(int type, int protocol);
int armos_socket_pair(int domain, int type, int protocol, int *user_sockets);
ssize_t armos_socket_send_message(
    int fd, const struct armos_msghdr_kernel *user_message, int flags);
ssize_t armos_socket_receive_message(
    int fd, struct armos_msghdr_kernel *user_message, int flags);
int armos_socket_bind(int fd, const void *address, uint32_t address_length);
int armos_socket_connect(int fd, const void *address, uint32_t address_length);
int armos_socket_listen(int fd, int backlog);
int armos_socket_accept(int fd, void *address, uint32_t *address_length);
ssize_t armos_socket_send(int fd, const void *buffer, size_t length, int flags,
                         const void *address, uint32_t address_length);
ssize_t armos_socket_receive(int fd, void *buffer, size_t length, int flags,
                            void *address, uint32_t *address_length);
int armos_socket_shutdown(int fd, int how);

#endif
