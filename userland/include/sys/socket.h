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
#include <sys/uio.h>

#define AF_UNSPEC    0
#define AF_UNIX      1
#define AF_LOCAL     AF_UNIX
#define AF_INET      2
#define PF_UNIX      AF_UNIX
#define PF_LOCAL     AF_LOCAL
#define PF_INET      AF_INET
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_NONBLOCK 0x4000
#define SOCK_CLOEXEC  0x40000

#define SHUT_RD      0
#define SHUT_WR      1
#define SHUT_RDWR    2

#define SOL_SOCKET   1
#define SCM_RIGHTS   1

#define MSG_CTRUNC          0x0008
#define MSG_CMSG_CLOEXEC    0x40000000

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct msghdr {
    void *msg_name;
    socklen_t msg_namelen;
    struct iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

#define CMSG_ALIGN(length) \
    (((length) + sizeof(size_t) - 1u) & ~(sizeof(size_t) - 1u))
#define CMSG_SPACE(length) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(length))
#define CMSG_LEN(length) \
    (CMSG_ALIGN(sizeof(struct cmsghdr)) + (length))
#define CMSG_DATA(header) \
    ((unsigned char *)(header) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(message) \
    ((message) && (message)->msg_control && \
     (message)->msg_controllen >= sizeof(struct cmsghdr) ? \
     (struct cmsghdr *)(message)->msg_control : NULL)

static inline struct cmsghdr *
__armos_cmsg_nxthdr(const struct msghdr *message,
                    const struct cmsghdr *header)
{
    const unsigned char *base;
    const unsigned char *end;
    const unsigned char *next;

    if (!message || !message->msg_control || !header ||
        header->cmsg_len < sizeof(*header))
        return NULL;
    base = (const unsigned char *)message->msg_control;
    end = base + message->msg_controllen;
    next = (const unsigned char *)header + CMSG_ALIGN(header->cmsg_len);
    if (next < base || next > end ||
        (size_t)(end - next) < sizeof(struct cmsghdr))
        return NULL;
    return (struct cmsghdr *)(void *)next;
}

#define CMSG_NXTHDR(message, header) \
    __armos_cmsg_nxthdr((message), (header))

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int sockets[2]);
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags);
ssize_t send(int sockfd, const void *buffer, size_t length, int flags);
ssize_t recv(int sockfd, void *buffer, size_t length, int flags);
ssize_t sendto(int sockfd, const void *buffer, size_t length, int flags,
               const struct sockaddr *address, socklen_t address_length);
ssize_t recvfrom(int sockfd, void *buffer, size_t length, int flags,
                 struct sockaddr *address, socklen_t *address_length);
ssize_t sendmsg(int sockfd, const struct msghdr *message, int flags);
ssize_t recvmsg(int sockfd, struct msghdr *message, int flags);
int shutdown(int sockfd, int how);

#endif /* _ARMOS_SYS_SOCKET_H */
