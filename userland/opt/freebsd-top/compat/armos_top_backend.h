/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/freebsd-top/compat/armos_top_backend.h
 * Layer: Userland / FreeBSD top compatibility
 * Description: Stable ArmOS process-monitoring backend contract.
 */

#ifndef ARMOS_TOP_BACKEND_H
#define ARMOS_TOP_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARMOS_TOP_MAX_CPUS 8
#define ARMOS_TOP_COMMAND_MAX 64

struct armos_top_cpu_sample {
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t irq_ticks;
    uint64_t idle_ticks;
};

struct armos_top_memory_sample {
    uint64_t total_kb;
    uint64_t free_kb;
};

struct armos_top_process_sample {
    pid_t pid;
    pid_t ppid;
    uid_t uid;
    int tty;
    int last_cpu;
    int priority;
    char state;
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t context_switches;
    uint64_t page_faults;
    uint64_t virtual_kb;
    uint64_t resident_kb;
    char command[ARMOS_TOP_COMMAND_MAX];
};

struct armos_top_snapshot {
    unsigned cpu_count;
    struct armos_top_cpu_sample cpus[ARMOS_TOP_MAX_CPUS];
    struct armos_top_memory_sample memory;
    struct armos_top_process_sample *processes;
    size_t process_count;
};

int armos_top_snapshot_read(struct armos_top_snapshot *snapshot);
void armos_top_snapshot_destroy(struct armos_top_snapshot *snapshot);

#endif
