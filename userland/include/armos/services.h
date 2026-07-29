/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armos/services.h
 * Layer: Userland / public system-service API
 *
 * Responsibilities:
 * - Expose stable, bounded system snapshots to desktop applications.
 * - Keep procfs, device descriptors and kernel ABI details out of clients.
 * - Provide the future client boundary for privileged service daemons.
 */

#ifndef ARMOS_SERVICES_H
#define ARMOS_SERVICES_H

#include <stddef.h>
#include <stdint.h>

#define ARMOS_SERVICE_MAX_PROCESSES 16u

struct armos_service_process {
    int pid;
    unsigned int cpu_x10;
    unsigned int rss_kb;
    char state;
    char name[32];
};

struct armos_service_snapshot {
    uint32_t display_width;
    uint32_t display_height;
    uint32_t display_bpp;
    unsigned int memory_total_kb;
    unsigned int memory_free_kb;
    unsigned int process_count;
    int network_available;
    char network_interface[16];
    struct armos_service_process processes[ARMOS_SERVICE_MAX_PROCESSES];
    size_t visible_processes;
};

int armos_services_snapshot(struct armos_service_snapshot *snapshot);

#endif /* ARMOS_SERVICES_H */
