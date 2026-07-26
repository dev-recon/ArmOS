/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/event_timer.c
 * Layer: Kernel / common descriptor services
 *
 * Responsibilities:
 * - Provide counter-backed event descriptors for userland wakeups.
 * - Provide monotonic timer descriptors readable through the file API.
 * - Integrate both descriptor classes with poll and blocking I/O.
 *
 * Notes:
 * - This implementation is architecture and platform independent.
 * - Timer deadlines use the common scheduler tick clock.
 * - Descriptor state is protected for concurrent SMP readers and writers.
 */

#include <kernel/event_timer.h>
#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

#define ARMOS_EFD_SEMAPHORE 1
#define ARMOS_EFD_NONBLOCK  O_NONBLOCK
#define ARMOS_EFD_CLOEXEC   O_CLOEXEC

#define ARMOS_TFD_NONBLOCK      O_NONBLOCK
#define ARMOS_TFD_CLOEXEC       O_CLOEXEC
#define ARMOS_TFD_TIMER_ABSTIME 1

#define EVENT_TIMER_U64_MAX (~(uint64_t)0)
#define EVENT_TIMER_U32_MAX (~(uint32_t)0)
#define EVENTFD_COUNTER_MAX (EVENT_TIMER_U64_MAX - 1u)

struct eventfd_state {
    uint64_t counter;
    bool semaphore;
    spinlock_t lock;
};

struct timerfd_state {
    uint32_t deadline;
    uint32_t interval;
    uint64_t expirations;
    bool armed;
    spinlock_t lock;
};

static int event_timer_close(file_t *file)
{
    if (file && file->private_data) {
        kfree(file->private_data);
        file->private_data = NULL;
    }
    return 0;
}

static ssize_t eventfd_read(file_t *file, void *buffer, size_t count)
{
    struct eventfd_state *state = file ? file->private_data : NULL;
    uint64_t value;

    if (!state || !buffer)
        return -EINVAL;
    if (count != sizeof(value))
        return -EINVAL;

    while (1) {
        spin_lock(&state->lock);
        if (state->counter != 0) {
            if (state->semaphore) {
                value = 1;
                state->counter--;
            } else {
                value = state->counter;
                state->counter = 0;
            }
            spin_unlock(&state->lock);
            memcpy(buffer, &value, sizeof(value));
            return (ssize_t)sizeof(value);
        }
        spin_unlock(&state->lock);

        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;
        if (task_poll_wait_once() != 0)
            return -EINTR;
    }
}

static ssize_t eventfd_write(file_t *file, const void *buffer, size_t count)
{
    struct eventfd_state *state = file ? file->private_data : NULL;
    uint64_t value;

    if (!state || !buffer)
        return -EINVAL;
    if (count != sizeof(value))
        return -EINVAL;
    memcpy(&value, buffer, sizeof(value));
    if (value == EVENT_TIMER_U64_MAX)
        return -EINVAL;

    while (1) {
        spin_lock(&state->lock);
        if (value <= EVENTFD_COUNTER_MAX - state->counter) {
            state->counter += value;
            spin_unlock(&state->lock);
            return (ssize_t)sizeof(value);
        }
        spin_unlock(&state->lock);

        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;
        if (task_poll_wait_once() != 0)
            return -EINTR;
    }
}

static file_operations_t eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .open = NULL,
    .close = event_timer_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

bool eventfd_read_ready(file_t *file)
{
    struct eventfd_state *state = file ? file->private_data : NULL;
    bool ready;

    if (!state)
        return false;
    spin_lock(&state->lock);
    ready = state->counter != 0;
    spin_unlock(&state->lock);
    return ready;
}

bool eventfd_write_ready(file_t *file)
{
    struct eventfd_state *state = file ? file->private_data : NULL;
    bool ready;

    if (!state)
        return false;
    spin_lock(&state->lock);
    ready = state->counter < EVENTFD_COUNTER_MAX;
    spin_unlock(&state->lock);
    return ready;
}

static void timerfd_update_locked(struct timerfd_state *state, uint32_t now)
{
    uint32_t elapsed;
    uint64_t count;

    if (!state->armed || (int32_t)(now - state->deadline) < 0)
        return;

    if (state->interval == 0) {
        state->armed = false;
        count = 1;
    } else {
        elapsed = now - state->deadline;
        count = 1u + elapsed / state->interval;
        state->deadline += (uint32_t)(count * state->interval);
    }

    if (EVENT_TIMER_U64_MAX - state->expirations < count)
        state->expirations = EVENT_TIMER_U64_MAX;
    else
        state->expirations += count;
}

static ssize_t timerfd_read(file_t *file, void *buffer, size_t count)
{
    struct timerfd_state *state = file ? file->private_data : NULL;
    uint64_t expirations;

    if (!state || !buffer)
        return -EINVAL;
    if (count != sizeof(expirations))
        return -EINVAL;

    while (1) {
        spin_lock(&state->lock);
        timerfd_update_locked(state, get_system_ticks());
        if (state->expirations != 0) {
            expirations = state->expirations;
            state->expirations = 0;
            spin_unlock(&state->lock);
            memcpy(buffer, &expirations, sizeof(expirations));
            return (ssize_t)sizeof(expirations);
        }
        spin_unlock(&state->lock);

        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;
        if (task_poll_wait_once() != 0)
            return -EINTR;
    }
}

static file_operations_t timerfd_ops = {
    .read = timerfd_read,
    .write = NULL,
    .open = NULL,
    .close = event_timer_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

bool timerfd_read_ready(file_t *file)
{
    struct timerfd_state *state = file ? file->private_data : NULL;
    bool ready;

    if (!state)
        return false;
    spin_lock(&state->lock);
    timerfd_update_locked(state, get_system_ticks());
    ready = state->expirations != 0;
    spin_unlock(&state->lock);
    return ready;
}

static file_t *event_timer_create_file(file_operations_t *operations,
                                       file_type_t type, int flags,
                                       void *state, const char *name)
{
    file_t *file = create_file();
    inode_t *inode = create_inode();

    if (!file || !inode) {
        if (file)
            kfree(file);
        if (inode)
            kfree(inode);
        kfree(state);
        return NULL;
    }

    inode->mode = S_IFCHR | 0600;
    inode->f_op = operations;
    file->inode = inode;
    file->f_op = operations;
    file->flags = flags & ~O_CLOEXEC;
    file->type = type;
    file->private_data = state;
    strncpy(file->name, name, sizeof(file->name) - 1u);
    return file;
}

int sys_eventfd2(uint32_t initial_value, int flags)
{
    struct eventfd_state *state;
    file_t *file;
    task_t *task = task_current_local();
    int fd;

    if (!task || !task->process)
        return -EINVAL;
    if ((flags & ~(ARMOS_EFD_SEMAPHORE | ARMOS_EFD_NONBLOCK |
                   ARMOS_EFD_CLOEXEC)) != 0)
        return -EINVAL;

    state = kmalloc(sizeof(*state));
    if (!state)
        return -ENOMEM;
    memset(state, 0, sizeof(*state));
    state->counter = initial_value;
    state->semaphore = (flags & ARMOS_EFD_SEMAPHORE) != 0;
    init_spinlock(&state->lock);

    file = event_timer_create_file(&eventfd_ops, FILE_TYPE_EVENTFD,
                                   O_RDWR | (flags & O_NONBLOCK), state,
                                   "eventfd");
    if (!file)
        return -ENOMEM;
    fd = vfs_install_file(task, file,
                          (flags & O_CLOEXEC) != 0 ? O_CLOEXEC : 0u);
    if (fd < 0)
        close_file(file);
    return fd;
}

static bool timerfd_timespec_valid(const armos_timespec_t *value)
{
    return value && value->sec >= 0 && value->nsec >= 0 &&
           value->nsec < 1000000000LL;
}

static int timerfd_timespec_to_ticks(const armos_timespec_t *value,
                                     uint32_t *ticks)
{
    uint64_t total;
    uint32_t subsecond;
    uint32_t nanoseconds_per_tick = 1000000000u / TIMER_FREQ;

    if (!timerfd_timespec_valid(value) || !ticks)
        return -EINVAL;
    if ((uint64_t)value->sec > EVENT_TIMER_U32_MAX / TIMER_FREQ)
        return -EINVAL;

    total = (uint64_t)value->sec * TIMER_FREQ;
    subsecond = ((uint32_t)value->nsec + nanoseconds_per_tick - 1u) /
        nanoseconds_per_tick;
    total += subsecond;
    if (total > EVENT_TIMER_U32_MAX)
        return -EINVAL;
    *ticks = (uint32_t)total;
    return 0;
}

static void timerfd_ticks_to_timespec(uint32_t ticks,
                                      armos_timespec_t *value)
{
    value->sec = ticks / TIMER_FREQ;
    value->nsec = (int64_t)(ticks % TIMER_FREQ) *
        (1000000000u / TIMER_FREQ);
}

static void timerfd_snapshot_locked(struct timerfd_state *state,
                                    struct armos_itimerspec *value)
{
    uint32_t now = get_system_ticks();
    uint32_t remaining = 0;

    timerfd_update_locked(state, now);
    if (state->armed && (int32_t)(state->deadline - now) > 0)
        remaining = state->deadline - now;
    timerfd_ticks_to_timespec(state->interval, &value->interval);
    timerfd_ticks_to_timespec(remaining, &value->value);
}

int sys_timerfd_create(int clock_id, int flags)
{
    struct timerfd_state *state;
    file_t *file;
    task_t *task = task_current_local();
    int fd;

    if (!task || !task->process)
        return -EINVAL;
    if (clock_id != ARMOS_CLOCK_MONOTONIC)
        return -EINVAL;
    if ((flags & ~(ARMOS_TFD_NONBLOCK | ARMOS_TFD_CLOEXEC)) != 0)
        return -EINVAL;

    state = kmalloc(sizeof(*state));
    if (!state)
        return -ENOMEM;
    memset(state, 0, sizeof(*state));
    init_spinlock(&state->lock);

    file = event_timer_create_file(&timerfd_ops, FILE_TYPE_TIMERFD,
                                   O_RDONLY | (flags & O_NONBLOCK), state,
                                   "timerfd");
    if (!file)
        return -ENOMEM;
    fd = vfs_install_file(task, file,
                          (flags & O_CLOEXEC) != 0 ? O_CLOEXEC : 0u);
    if (fd < 0)
        close_file(file);
    return fd;
}

int sys_timerfd_settime(int fd, int flags,
                        const struct armos_itimerspec *new_value,
                        struct armos_itimerspec *old_value)
{
    struct armos_itimerspec requested;
    struct armos_itimerspec previous;
    struct timerfd_state *state;
    task_t *task = task_current_local();
    file_t *file;
    uint32_t first;
    uint32_t interval;
    int ret;

    if (!task || !task->process)
        return -EINVAL;
    if (fd < 0 || fd >= MAX_FILES ||
        !(file = task->process->files[fd]))
        return -EBADF;
    if (file->type != FILE_TYPE_TIMERFD || !file->private_data)
        return -EINVAL;
    if ((flags & ~ARMOS_TFD_TIMER_ABSTIME) != 0)
        return -EINVAL;
    if (!new_value)
        return -EFAULT;
    if (copy_from_user(&requested, new_value, sizeof(requested)) < 0)
        return -EFAULT;
    ret = timerfd_timespec_to_ticks(&requested.value, &first);
    if (ret < 0)
        return ret;
    ret = timerfd_timespec_to_ticks(&requested.interval, &interval);
    if (ret < 0)
        return ret;

    state = file->private_data;
    spin_lock(&state->lock);
    if (old_value)
        timerfd_snapshot_locked(state, &previous);
    state->interval = interval;
    state->expirations = 0;
    state->armed = first != 0;
    if (state->armed)
        state->deadline = (flags & ARMOS_TFD_TIMER_ABSTIME) != 0 ?
            first : get_system_ticks() + first;
    spin_unlock(&state->lock);

    if (old_value &&
        copy_to_user(old_value, &previous, sizeof(previous)) < 0)
        return -EFAULT;
    return 0;
}

int sys_timerfd_gettime(int fd, struct armos_itimerspec *current_value)
{
    struct armos_itimerspec value;
    struct timerfd_state *state;
    task_t *task = task_current_local();
    file_t *file;

    if (!task || !task->process)
        return -EINVAL;
    if (fd < 0 || fd >= MAX_FILES ||
        !(file = task->process->files[fd]))
        return -EBADF;
    if (file->type != FILE_TYPE_TIMERFD || !file->private_data)
        return -EINVAL;
    if (!current_value)
        return -EFAULT;

    state = file->private_data;
    spin_lock(&state->lock);
    timerfd_snapshot_locked(state, &value);
    spin_unlock(&state->lock);
    return copy_to_user(current_value, &value, sizeof(value)) < 0 ?
        -EFAULT : 0;
}
