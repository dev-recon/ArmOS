/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/pty.c
 * Layer: Kernel / common terminal services
 *
 * Responsibilities:
 * - Provide bounded PTY master/slave pairs for terminal emulators.
 * - Route master input through the existing TTY line discipline.
 * - Buffer slave output for blocking or non-blocking master reads.
 *
 * Notes:
 * - This code is architecture and platform independent.
 * - Slave termios, signals and window size are owned by the common TTY layer.
 */

#include <kernel/pty.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/string.h>

#define PTY_COUNT 8u
#define PTY_TTY_BASE 16
#define PTY_OUTPUT_SIZE 4096u

struct pty_pair {
    bool used;
    bool unlocked;
    bool master_open;
    uint32_t index;
    struct tty_struct slave;
    char output[PTY_OUTPUT_SIZE];
    uint32_t head;
    uint32_t tail;
    spinlock_t lock;
};

static struct pty_pair pty_pairs[PTY_COUNT];
static spinlock_t pty_table_lock = SPINLOCK_INIT("pty_table");

static uint32_t pty_next(uint32_t position)
{
    return (position + 1u) % PTY_OUTPUT_SIZE;
}

static bool pty_slave_putc(void *context, char character)
{
    struct pty_pair *pair = context;
    uint32_t next;

    if (!pair || !pair->used || !pair->master_open)
        return false;
    spin_lock(&pair->lock);
    next = pty_next(pair->head);
    if (next == pair->tail) {
        spin_unlock(&pair->lock);
        return false;
    }
    pair->output[pair->head] = character;
    pair->head = next;
    spin_unlock(&pair->lock);
    task_poll_notify_key(pair);
    return true;
}

static ssize_t pty_master_read(file_t *file, void *buffer, size_t count)
{
    struct pty_pair *pair = file ? file->private_data : NULL;
    char *bytes = buffer;
    size_t copied = 0;

    if (!pair || !buffer)
        return -EINVAL;
    while (1) {
        const void *wait_key = pair;
        uint32_t generation = task_poll_generation();

        if (pty_master_read_ready(file))
            break;
        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;
        if (task_poll_wait(task_current_local(), generation, 0u,
                           &wait_key, 1u) != 0)
            return -EINTR;
    }
    spin_lock(&pair->lock);
    while (copied < count && pair->tail != pair->head) {
        bytes[copied++] = pair->output[pair->tail];
        pair->tail = pty_next(pair->tail);
    }
    spin_unlock(&pair->lock);
    if (copied != 0u)
        task_poll_notify_key(pair);
    tty_drain_output_for_id(PTY_TTY_BASE + (int)pair->index);
    return (ssize_t)copied;
}

static ssize_t pty_master_write(file_t *file, const void *buffer, size_t count)
{
    struct pty_pair *pair = file ? file->private_data : NULL;
    const char *bytes = buffer;

    if (!pair || !buffer || !pair->used)
        return -EINVAL;
    for (size_t index = 0; index < count; index++)
        tty_input_char_to_id(PTY_TTY_BASE + (int)pair->index, bytes[index]);
    return (ssize_t)count;
}

static int pty_master_close(file_t *file)
{
    struct pty_pair *pair = file ? file->private_data : NULL;

    if (!pair)
        return 0;
    spin_lock(&pty_table_lock);
    pair->master_open = false;
    pair->used = false;
    tty_unregister_virtual(PTY_TTY_BASE + (int)pair->index);
    spin_unlock(&pty_table_lock);
    task_poll_notify_key(pair);
    return 0;
}

static file_operations_t pty_master_ops = {
    .read = pty_master_read,
    .write = pty_master_write,
    .open = NULL,
    .close = pty_master_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = NULL,
};

bool pty_is_master_path(const char *path)
{
    return path && strcmp(path, "/dev/ptmx") == 0;
}

bool pty_is_slave_path(const char *path)
{
    return path && strncmp(path, "/dev/pts/", 9u) == 0 &&
           path[9] >= '0' && path[9] < (char)('0' + PTY_COUNT) &&
           path[10] == '\0';
}

int pty_slave_tty_id(const char *path)
{
    uint32_t index;

    if (!pty_is_slave_path(path))
        return -ENODEV;
    index = (uint32_t)(path[9] - '0');
    return pty_pairs[index].used && pty_pairs[index].unlocked ?
        PTY_TTY_BASE + (int)index : -EACCES;
}

file_t *pty_create_master_file(int flags)
{
    struct pty_pair *pair = NULL;
    file_t *file;
    inode_t *inode;

    spin_lock(&pty_table_lock);
    for (uint32_t index = 0; index < PTY_COUNT; index++) {
        if (!pty_pairs[index].used) {
            pair = &pty_pairs[index];
            memset(pair, 0, sizeof(*pair));
            pair->used = true;
            pair->master_open = true;
            pair->index = index;
            init_spinlock(&pair->lock);
            if (tty_register_virtual(&pair->slave,
                                     PTY_TTY_BASE + (int)index,
                                     pty_slave_putc, pair) < 0) {
                pair->used = false;
                pair = NULL;
            }
            break;
        }
    }
    spin_unlock(&pty_table_lock);
    if (!pair)
        return NULL;

    file = create_file();
    inode = create_inode();
    if (!file || !inode) {
        if (file)
            kfree(file);
        if (inode)
            kfree(inode);
        tty_unregister_virtual(PTY_TTY_BASE + (int)pair->index);
        pair->used = false;
        return NULL;
    }
    inode->mode = S_IFCHR | 0666;
    inode->f_op = &pty_master_ops;
    file->inode = inode;
    file->f_op = &pty_master_ops;
    file->flags = flags;
    file->type = FILE_TYPE_PTY_MASTER;
    file->private_data = pair;
    strncpy(file->name, "ptmx", sizeof(file->name) - 1u);
    return file;
}

bool pty_master_read_ready(file_t *file)
{
    struct pty_pair *pair = file ? file->private_data : NULL;
    bool ready;

    if (!pair)
        return false;
    spin_lock(&pair->lock);
    ready = pair->head != pair->tail;
    spin_unlock(&pair->lock);
    return ready;
}

bool pty_master_write_ready(file_t *file)
{
    struct pty_pair *pair = file ? file->private_data : NULL;

    return pair && pair->used;
}

int pty_master_tty_id(file_t *file)
{
    struct pty_pair *pair = file ? file->private_data : NULL;

    if (!pair || !pair->used)
        return -ENOTTY;
    return PTY_TTY_BASE + (int)pair->index;
}

int pty_master_ioctl(file_t *file, uint32_t request, uintptr_t arg)
{
    struct pty_pair *pair = file ? file->private_data : NULL;
    int value;

    if (!pair)
        return -ENOTTY;
    if (request == ARMOS_TIOCGPTN) {
        value = (int)pair->index;
        return copy_to_user((void *)arg, &value, sizeof(value)) < 0 ?
            -EFAULT : 0;
    }
    if (request == ARMOS_TIOCSPTLCK) {
        if (copy_from_user(&value, (void *)arg, sizeof(value)) < 0)
            return -EFAULT;
        pair->unlocked = value == 0;
        return 0;
    }
    return -ENOTTY;
}
