/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/socket.h
 * Layer: Userland / C library compatibility
 *
 * Responsibilities:
 * - Provide the small BSD socket surface currently implemented by ArmOS.
 * - Keep source compatibility with simple POSIX-style network tools.
 */

#ifndef _ARMOS_SYS_SOCKET_H
#define _ARMOS_SYS_SOCKET_H

#include <stddef.h>
#include <sys/types.h>

#define AF_UNSPEC    0
#define AF_UNIX      1
#define AF_LOCAL     AF_UNIX
#define AF_INET      2
#define PF_UNIX      AF_UNIX
#define PF_LOCAL     AF_LOCAL
#define PF_INET      AF_INET
#define SOCK_STREAM  1
#define SOCK_DGRAM   2

#define SHUT_RD      0
#define SHUT_WR      1
#define SHUT_RDWR    2

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sockets[2]);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t send(int sockfd, const void *buffer, size_t length, int flags);
ssize_t recv(int sockfd, void *buffer, size_t length, int flags);
ssize_t sendto(int sockfd, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t address_length);
ssize_t recvfrom(int sockfd, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_length);
int shutdown(int sockfd, int how);

#endif /* _ARMOS_SYS_SOCKET_H */
