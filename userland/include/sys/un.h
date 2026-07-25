/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/sys/un.h
 * Layer: Userland / POSIX socket compatibility
 *
 * Responsibilities:
 * - Declare the standardized local socket address structure.
 * - Preserve source compatibility with applications using sys/un.h.
 *
 * Notes:
 * - The public AF_UNIX name is retained because it is defined by POSIX.
 * - The implementation itself uses ArmOS-specific internal names.
 */

#ifndef _ARMOS_SYS_UN_H
#define _ARMOS_SYS_UN_H

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};

#endif
