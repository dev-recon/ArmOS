/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/foot/armos-foot-config.h
 * Layer: Userland / Foot port configuration
 *
 * Responsibilities:
 * - Define the terminal identity selected for the initial ArmOS Foot port.
 * - Keep string-valued build definitions independent from shell quoting.
 */

#ifndef ARMOS_FOOT_CONFIG_H
#define ARMOS_FOOT_CONFIG_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define FOOT_DEFAULT_TERM "armos"

/*
 * Foot normally prefers memfd_create() and otherwise falls back to a
 * growable regular temporary file. ArmOS SHM objects already provide the
 * required anonymous, growable descriptor semantics, while ext2 sparse file
 * growth is deliberately not implemented yet.
 */
#define MEMFD_CREATE 1
#define MFD_CLOEXEC 0x0001u
#define MFD_ALLOW_SEALING 0x0002u
#define F_ADD_SEALS 1033
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004

static inline int armos_foot_memfd_create(const char *label,
                                          unsigned int flags)
{
    static unsigned int sequence;
    char name[64];
    int fd;

    (void)label;
    (void)flags;
    snprintf(name, sizeof(name), "/foot-buffer-%d-%u", getpid(),
             __sync_fetch_and_add(&sequence, 1u));
    shm_unlink(name);
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (shm_unlink(name) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

#define memfd_create(label, flags) armos_foot_memfd_create((label), (flags))

#endif /* ARMOS_FOOT_CONFIG_H */
