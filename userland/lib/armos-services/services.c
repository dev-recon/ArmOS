/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armos-services/services.c
 * Layer: Userland / system-service client library
 *
 * Responsibilities:
 * - Translate current ArmOS system ABIs into stable desktop snapshots.
 * - Centralize framebuffer, process and network discovery.
 * - Keep graphical clients independent from kernel and procfs layouts.
 *
 * Notes:
 * - Read-only queries are local today.
 * - Future privileged mutations can move behind a daemon without changing
 *   the public application-facing structures.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <arm_os_abi.h>
#include <armos/services.h>
#include <sys/fb.h>

static void copy_name(char *destination, size_t capacity,
                      const char *source)
{
    size_t length;

    if (!destination || capacity == 0u)
        return;
    if (!source)
        source = "";
    length = strnlen(source, capacity - 1u);
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void query_display(struct armos_service_snapshot *snapshot)
{
    struct armos_fb_info info;
    int descriptor = open("/dev/fb0", O_RDONLY);

    if (descriptor < 0)
        return;
    memset(&info, 0, sizeof(info));
    if (ioctl(descriptor, ARMOS_FBIOGET_INFO, &info) == 0) {
        snapshot->display_width = info.width;
        snapshot->display_height = info.height;
        snapshot->display_bpp = info.bpp;
    }
    close(descriptor);
}

static void query_network(struct armos_service_snapshot *snapshot)
{
    char buffer[1024];
    ssize_t length;
    int descriptor = open("/proc/net/dev", O_RDONLY);

    if (descriptor < 0)
        return;
    length = read(descriptor, buffer, sizeof(buffer) - 1u);
    close(descriptor);
    if (length <= 0)
        return;
    buffer[length] = '\0';
    if (strstr(buffer, "wlan0") != NULL) {
        snapshot->network_available = 1;
        copy_name(snapshot->network_interface,
                  sizeof(snapshot->network_interface), "wlan0");
    } else if (strstr(buffer, "eth0") != NULL) {
        snapshot->network_available = 1;
        copy_name(snapshot->network_interface,
                  sizeof(snapshot->network_interface), "eth0");
    }
}

int armos_services_snapshot(struct armos_service_snapshot *snapshot)
{
    struct sysinfo_response information;

    if (!snapshot) {
        errno = EINVAL;
        return -1;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    memset(&information, 0, sizeof(information));
    if (getsysinfo(&information) < 0)
        return -1;
    snapshot->memory_total_kb = information.mem_total_kb;
    snapshot->memory_free_kb = information.mem_free_kb;
    snapshot->process_count =
        information.proc_count > 0 ?
        (unsigned int)information.proc_count : 0u;
    for (int index = 0;
         index < information.proc_count &&
         snapshot->visible_processes < ARMOS_SERVICE_MAX_PROCESSES;
         index++) {
        const struct proc_info *source = &information.procs[index];
        struct armos_service_process *destination =
            &snapshot->processes[snapshot->visible_processes++];

        destination->pid = source->pid;
        destination->cpu_x10 = source->cpu_pct_x10;
        destination->rss_kb = source->rss_kb;
        destination->state = source->state;
        copy_name(destination->name, sizeof(destination->name),
                  source->name);
    }
    query_display(snapshot);
    query_network(snapshot);
    return 0;
}
