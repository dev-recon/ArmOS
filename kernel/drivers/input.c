/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/input.c
 * Layer: Kernel / common device interface
 *
 * Responsibilities:
 * - Merge input events produced by platform drivers into one bounded queue.
 * - Provide blocking and non-blocking reads through /dev/input0.
 * - Report readiness to the common poll/select implementation.
 *
 * Notes:
 * - Producers may run in interrupt context; the queue never allocates there.
 * - A slow consumer loses the oldest event rather than blocking a driver.
 */

#include <kernel/input.h>
#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscalls.h>
#include <kernel/timer.h>

#define ARMOS_INPUT_QUEUE_LENGTH 512u
#define DEV_INPUT0_RDEV          0x130u

static armos_input_event_t input_queue[ARMOS_INPUT_QUEUE_LENGTH];
static uint32_t input_head;
static uint32_t input_tail;
static spinlock_t input_lock = SPINLOCK_INIT("input_queue");
static file_t *input_owner;

static bool input_event_is_priority(uint16_t type)
{
    return type == ARMOS_INPUT_EVENT_KEY ||
           type == ARMOS_INPUT_EVENT_CONFIG;
}

static bool input_drop_nonpriority_locked(void)
{
    uint32_t index = input_tail;

    while (index != input_head) {
        uint32_t next;

        if (input_event_is_priority(input_queue[index].type)) {
            index = (index + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
            continue;
        }
        next = (index + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
        while (next != input_head) {
            input_queue[index] = input_queue[next];
            index = next;
            next = (next + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
        }
        input_head = (input_head + ARMOS_INPUT_QUEUE_LENGTH - 1u) %
            ARMOS_INPUT_QUEUE_LENGTH;
        return true;
    }
    return false;
}

void armos_input_emit(uint16_t type, uint16_t code, int32_t value)
{
    armos_input_event_t *event;
    uint32_t previous;
    uint32_t next;

    spin_lock(&input_lock);
    /*
     * HID transports may delimit every poll with SYN_REPORT, including polls
     * which contain no state change.  Keeping more than one consecutive sync
     * has no observable meaning and can evict a key release while a graphical
     * consumer is delayed by rendering.
     */
    if (type == ARMOS_INPUT_EVENT_SYNC && input_head != input_tail) {
        previous = (input_head + ARMOS_INPUT_QUEUE_LENGTH - 1u) %
            ARMOS_INPUT_QUEUE_LENGTH;
        if (input_queue[previous].type == ARMOS_INPUT_EVENT_SYNC) {
            spin_unlock(&input_lock);
            return;
        }
    }
    next = (input_head + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
    if (next == input_tail) {
        if (!input_event_is_priority(type)) {
            spin_unlock(&input_lock);
            return;
        }
        if (!input_drop_nonpriority_locked())
            input_tail = (input_tail + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
        next = (input_head + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
    }
    event = &input_queue[input_head];
    event->timestamp_ms = get_system_ticks() * 1000u / TIMER_FREQ;
    event->type = type;
    event->code = code;
    event->value = value;
    input_head = next;
    spin_unlock(&input_lock);
    task_poll_notify();
}

bool armos_input_tty_routing_enabled(void)
{
    bool enabled;

    spin_lock(&input_lock);
    enabled = input_owner == NULL;
    spin_unlock(&input_lock);
    return enabled;
}

bool armos_input_read_ready(void)
{
    bool ready;

    spin_lock(&input_lock);
    ready = input_head != input_tail;
    spin_unlock(&input_lock);
    return ready;
}

static ssize_t input_read(file_t *file, void *buffer, size_t count)
{
    armos_input_event_t *events = (armos_input_event_t *)buffer;
    size_t capacity = count / sizeof(*events);
    size_t copied = 0;

    if (!file || !buffer || capacity == 0u)
        return -EINVAL;
    spin_lock(&input_lock);
    if (input_owner != file) {
        spin_unlock(&input_lock);
        return -EACCES;
    }
    spin_unlock(&input_lock);
    while (!armos_input_read_ready()) {
        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;
        if (task_poll_wait_once() != 0)
            return -EINTR;
    }

    spin_lock(&input_lock);
    while (copied < capacity && input_tail != input_head) {
        events[copied++] = input_queue[input_tail];
        input_tail = (input_tail + 1u) % ARMOS_INPUT_QUEUE_LENGTH;
    }
    spin_unlock(&input_lock);
    return (ssize_t)(copied * sizeof(*events));
}

static int input_close(file_t *file)
{
    spin_lock(&input_lock);
    if (input_owner == file) {
        input_owner = NULL;
        input_tail = input_head;
    }
    spin_unlock(&input_lock);
    return 0;
}

static file_operations_t input_file_ops = {
    .read = input_read,
    .write = NULL,
    .open = NULL,
    .close = input_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

bool is_input_device_path(const char *path)
{
    return path && strcmp(path, "/dev/input0") == 0;
}

void fill_input_device_stat(struct stat *st)
{
    uint32_t now;

    if (!st)
        return;
    now = get_current_time();
    memset(st, 0, sizeof(*st));
    st->st_ino = DEV_INPUT0_RDEV;
    st->st_mode = S_IFCHR | 0600;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = DEV_INPUT0_RDEV;
    st->st_blksize = sizeof(armos_input_event_t);
    st->st_atime = now;
    st->st_mtime = now;
    st->st_ctime = now;
}

file_t *create_input_device_file(const char *name, int flags, int *error)
{
    file_t *file = create_file();
    inode_t *inode;
    struct stat st;

    if (error)
        *error = -ENOMEM;
    if (!file)
        return NULL;
    inode = create_inode();
    if (!inode) {
        kfree(file);
        return NULL;
    }
    fill_input_device_stat(&st);
    inode->mode = st.st_mode;
    inode->uid = st.st_uid;
    inode->gid = st.st_gid;
    inode->nlink = st.st_nlink;
    inode->parent_cluster = st.st_rdev;
    inode->f_op = &input_file_ops;
    file->f_op = &input_file_ops;
    file->flags = flags;
    file->type = FILE_TYPE_INPUT;
    file->inode = inode;
    if (name) {
        strncpy(file->name, name, sizeof(file->name) - 1u);
        file->name[sizeof(file->name) - 1u] = '\0';
    }
    spin_lock(&input_lock);
    if (input_owner) {
        spin_unlock(&input_lock);
        kfree(inode);
        kfree(file);
        if (error)
            *error = -EBUSY;
        return NULL;
    }
    input_owner = file;
    input_tail = input_head;
    spin_unlock(&input_lock);
    if (error)
        *error = 0;
    return file;
}
