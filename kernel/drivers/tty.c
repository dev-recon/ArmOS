/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/drivers/tty.c
 * Layer: Kernel / terminal and character devices
 *
 * Responsibilities:
 * - Drive UART/framebuffer console backends and TTY line discipline.
 * - Preserve canonical/raw terminal semantics and job-control signals.
 *
 * Notes:
 * - Logical TTY identities are independent from their hardware backends.
 */

#include <kernel/tty.h>
#include <kernel/task.h>
#include <kernel/string.h>
#include <kernel/process.h>
#include <kernel/vfs.h>
#include <kernel/signal.h>
#include <kernel/timer.h>
#include <kernel/memory.h>
#include <kernel/smp.h>
#include <kernel/kprintf.h>
#include <kernel/file.h>
#include <kernel/pty.h>

struct tty_struct tty0;
struct tty_struct tty1;
struct tty_struct ttyS0;

#define TTY_TX_DRAIN_BUDGET 64
#define TTY_VIRTUAL_BASE 16
#define TTY_VIRTUAL_COUNT 8

static int active_tty_id = TTY_CONSOLE_ID;
static int kernel_console_tty_id = -1;
static bool tty_initialized = false;
static struct tty_struct *virtual_ttys[TTY_VIRTUAL_COUNT];

static bool tty_output_empty_locked(struct tty_struct *tty);
static void tty_wake_reader(task_t *reader);
static void tty_init_one(struct tty_struct *tty, int id);

static struct tty_struct *tty_by_id(int tty_id)
{
    if (tty_id >= TTY_VIRTUAL_BASE &&
        tty_id < TTY_VIRTUAL_BASE + TTY_VIRTUAL_COUNT)
        return virtual_ttys[tty_id - TTY_VIRTUAL_BASE];
    switch (tty_id) {
    case TTY_CONSOLE_ID:
        return &tty0;
    case TTY_GRAPHICS_ID:
        return &tty1;
    case TTY_SERIAL_ID:
        return &ttyS0;
    default:
        return NULL;
    }
}

int tty_register_virtual(struct tty_struct *tty, int tty_id,
                         bool (*putc)(void *context, char c),
                         void *context)
{
    size_t index;

    if (!tty || !putc || tty_id < TTY_VIRTUAL_BASE ||
        tty_id >= TTY_VIRTUAL_BASE + TTY_VIRTUAL_COUNT)
        return -EINVAL;
    index = (size_t)(tty_id - TTY_VIRTUAL_BASE);
    if (virtual_ttys[index])
        return -EBUSY;
    tty_init_one(tty, tty_id);
    tty->virtual_putc = putc;
    tty->virtual_context = context;
    virtual_ttys[index] = tty;
    return 0;
}

void tty_unregister_virtual(int tty_id)
{
    if (tty_id >= TTY_VIRTUAL_BASE &&
        tty_id < TTY_VIRTUAL_BASE + TTY_VIRTUAL_COUNT)
        virtual_ttys[tty_id - TTY_VIRTUAL_BASE] = NULL;
}

static int tty_attach_backend_mode_to(int tty_id,
                                      const tty_backend_ops_t *ops,
                                      bool output_only)
{
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return -ENODEV;
    if (!ops || !ops->putc || !ops->try_putc || !ops->puts ||
        !ops->set_tx_irq_enabled || !ops->has_data || !ops->getc)
        return -EINVAL;

    tty->backend = ops;
    tty->output_only = output_only;
    return 0;
}

int tty_attach_backend_to(int tty_id, const tty_backend_ops_t *ops)
{
    return tty_attach_backend_mode_to(tty_id, ops, false);
}

int tty_attach_output_backend_to(int tty_id, const tty_backend_ops_t *ops)
{
    return tty_attach_backend_mode_to(tty_id, ops, true);
}

int tty_attach_output_mirror_to(int tty_id, const tty_backend_ops_t *ops)
{
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return -ENODEV;
    if (!ops || !ops->putc)
        return -EINVAL;

    tty->output_mirror = ops;
    return 0;
}

int tty_attach_backend(const tty_backend_ops_t *ops)
{
    return tty_attach_backend_to(TTY_CONSOLE_ID, ops);
}

int tty_set_active(int tty_id)
{
    if (!tty_by_id(tty_id))
        return -ENODEV;
    active_tty_id = tty_id;
    return 0;
}

int tty_get_active(void)
{
    return active_tty_id;
}

bool tty_has_backend_for_id(int tty_id)
{
    struct tty_struct *tty = tty_by_id(tty_id);

    return tty && (tty->backend != NULL || tty->virtual_putc != NULL);
}

bool tty_is_output_only_for_id(int tty_id)
{
    struct tty_struct *tty = tty_by_id(tty_id);

    return tty && tty->backend && tty->output_only;
}

int tty_check_open_for_id(int tty_id, int flags)
{
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty || (!tty->backend && !tty->virtual_putc))
        return -ENODEV;
    if (tty->output_only && (flags & O_ACCMODE) != O_WRONLY)
        return -EACCES;
    return 0;
}

bool tty_output_pending_for_id(int tty_id)
{
    unsigned long flags;
    bool pending;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return false;

    spin_lock_irqsave(&tty->lock, &flags);
    pending = !tty_output_empty_locked(tty);
    spin_unlock_irqrestore(&tty->lock, flags);
    return pending;
}

static const tty_backend_ops_t *tty_backend_for(const struct tty_struct *tty)
{
    return tty ? tty->backend : NULL;
}

static void tty_backend_putc_to(struct tty_struct *tty, char c)
{
    const tty_backend_ops_t *backend = tty_backend_for(tty);

    if (backend)
        backend->putc(c);
    if (tty && tty->output_mirror)
        tty->output_mirror->putc(c);
}

static bool tty_backend_try_putc_to(struct tty_struct *tty, char c)
{
    const tty_backend_ops_t *backend = tty_backend_for(tty);

    if (tty && tty->virtual_putc)
        return tty->virtual_putc(tty->virtual_context, c);
    if (!backend || !backend->try_putc(c))
        return false;

    /* The primary backend owns queue progress; mirror the accepted byte once. */
    if (tty->output_mirror)
        tty->output_mirror->putc(c);
    return true;
}

static void tty_backend_puts_to(struct tty_struct *tty, const char *s)
{
    const tty_backend_ops_t *backend = tty_backend_for(tty);
    const char *p;

    if (backend)
        backend->puts(s);
    if (tty && tty->output_mirror) {
        for (p = s; p && *p; p++)
            tty->output_mirror->putc(*p);
    }
}

static void tty_backend_set_tx_irq_enabled_to(struct tty_struct *tty, bool enabled)
{
    const tty_backend_ops_t *backend = tty_backend_for(tty);

    if (backend)
        backend->set_tx_irq_enabled(enabled);
}

static uint32_t tty_output_next(uint32_t pos)
{
    return (pos + 1) % TTY_OUTPUT_BUF_SIZE;
}

static uint32_t tty_input_prev(uint32_t pos)
{
    return pos == 0 ? TTY_INPUT_BUF_SIZE - 1 : pos - 1;
}

static bool tty_output_empty_locked(struct tty_struct *tty)
{
    return tty->output_head == tty->output_tail;
}

static bool tty_output_full_locked(struct tty_struct *tty)
{
    return tty_output_next(tty->output_head) == tty->output_tail;
}

static void tty_init_termios(struct termios *tio)
{
    memset(tio, 0, sizeof(*tio));
    tio->c_iflag = ICRNL | IXON;
    tio->c_oflag = OPOST | ONLCR;
    tio->c_cflag = CS8 | CREAD | HUPCL;
    /* mash owns line echo/editing today; keep ECHO disabled by default. */
    tio->c_lflag = ICANON | ISIG | ECHOE | ECHOK | ECHOCTL | ECHOKE;
    tio->c_cc[VINTR] = 0x03;   /* Ctrl-C */
    tio->c_cc[VQUIT] = 0x1C;   /* Ctrl-\ */
    tio->c_cc[VERASE] = 0x7F;  /* DEL */
    tio->c_cc[VKILL] = 0x15;   /* Ctrl-U */
    tio->c_cc[VEOF] = 0x04;    /* Ctrl-D */
    tio->c_cc[VTIME] = 0;
    tio->c_cc[VMIN] = 1;
    tio->c_cc[VSTART] = 0x11;  /* Ctrl-Q */
    tio->c_cc[VSTOP] = 0x13;   /* Ctrl-S */
    tio->c_cc[VSUSP] = 0x1A;   /* Ctrl-Z */
    tio->c_cc[VWERASE] = 0x17; /* Ctrl-W */
    tio->c_cc[VEOL] = 0;
    tio->c_cc[VEOL2] = 0;
    tio->c_ispeed = 115200;
    tio->c_ospeed = 115200;
}

static void tty_init_one(struct tty_struct *tty, int id)
{
    memset(tty, 0, sizeof(*tty));
    
    tty->id = id;
    tty_init_termios(&tty->termios);
    tty->foreground_pgid = 0;
    tty->winsize_rows = 24;
    tty->winsize_cols = 80;
    tty->winsize_xpixel = 0;
    tty->winsize_ypixel = 0;
    
    init_spinlock(&tty->lock);
    init_spinlock(&tty->write_lock);
}

void tty_init(void) {
    tty_initialized = false;
    kernel_console_tty_id = -1;
    tty_init_one(&tty0, TTY_CONSOLE_ID);
    tty_init_one(&tty1, TTY_GRAPHICS_ID);
    tty_init_one(&ttyS0, TTY_SERIAL_ID);
    memset(virtual_ttys, 0, sizeof(virtual_ttys));
    tty_initialized = true;
}

static void tty_kernel_putc_to(struct tty_struct *tty, char c)
{
    if (!tty || !tty->backend)
        return;

    if (c == '\n')
        tty_backend_putc_to(tty, '\r');
    tty_backend_putc_to(tty, c);
}

bool tty_kernel_console_putc(char c)
{
    struct tty_struct *tty;

    if (!tty_initialized || kernel_console_tty_id < 0)
        return false;

    tty = tty_by_id(kernel_console_tty_id);
    if (!tty || !tty->backend)
        return false;

    tty_kernel_putc_to(tty, c);
    return true;
}

void tty_set_kernel_console(int tty_id, bool replay_log)
{
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty || !tty->backend)
        return;

    kernel_console_tty_id = tty_id;
    if (replay_log) {
        char *log = kmalloc(8192);
        size_t length;

        if (!log)
            return;
        length = kmsg_read(log, 8192);
        for (size_t i = 0; i < length; i++)
            tty_kernel_putc_to(tty, log[i]);
        kfree(log);
    }
}

bool tty_console_output_lock(unsigned long *flags)
{
    if (!tty_initialized || !flags)
        return false;

    /*
     * kprintf normally serializes with user writes through tty0.write_lock.
     * If a diagnostic is emitted while the current CPU already owns that lock,
     * skip the extra lock rather than deadlocking the rescue console path.
     */
    if (tty0.write_lock.locked && tty0.write_lock.owner == smp_processor_id())
        return false;

    spin_lock_irqsave(&tty0.write_lock, flags);
    return true;
}

void tty_console_output_unlock(unsigned long flags)
{
    spin_unlock_irqrestore(&tty0.write_lock, flags);
}

static int tty_signal_process_group(pid_t pgid, int sig)
{
    task_t* task;
    task_t** targets;
    uint32_t target_count = 0;
    uint32_t walked = 0;
    unsigned long flags;
    int delivered = 0;
    uint32_t i;

    if (pgid <= 0)
        return 0;
    targets = kmalloc(sizeof(*targets) * MAX_TASKS);
    if (!targets)
        return -ENOMEM;

    spin_lock_irqsave(&task_lock, &flags);
    task = task_list_head;
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        kfree(targets);
        return 0;
    }

    do {
        if (task->type == TASK_TYPE_PROCESS && task->process &&
            task->process->pgid == pgid &&
            task->state != TASK_ZOMBIE &&
            task->state != TASK_TERMINATED) {
            if (target_count < MAX_TASKS)
                targets[target_count++] = task;
        }

        task = task->next;
        walked++;
    } while (task && task != task_list_head && walked < MAX_TASKS);

    spin_unlock_irqrestore(&task_lock, flags);

    for (i = 0; i < target_count; i++) {
        if (send_signal(targets[i], sig) == 0)
            delivered++;
    }

    kfree(targets);
    return delivered;
}

int tty_set_foreground_pgid_for_id(int tty_id, pid_t pgid)
{
    unsigned long flags;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return -ENODEV;

    if (pgid < 0)
        return -EINVAL;

    spin_lock_irqsave(&tty->lock, &flags);
    tty->foreground_pgid = pgid;
    spin_unlock_irqrestore(&tty->lock, flags);
    return 0;
}

int tty_set_foreground_pgid(pid_t pgid)
{
    return tty_set_foreground_pgid_for_id(TTY_CONSOLE_ID, pgid);
}

pid_t tty_get_foreground_pgid_for_id(int tty_id)
{
    unsigned long flags;
    pid_t pgid;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return -ENODEV;

    spin_lock_irqsave(&tty->lock, &flags);
    pgid = tty->foreground_pgid;
    spin_unlock_irqrestore(&tty->lock, flags);
    return pgid;
}

pid_t tty_get_foreground_pgid(void)
{
    return tty_get_foreground_pgid_for_id(TTY_CONSOLE_ID);
}

static bool tty_current_task_is_background(struct tty_struct *tty,
                                           task_t **leader_out,
                                           pid_t *pgid_out)
{
    task_t *task = task_current_local();
    task_t *leader;
    process_t *process;
    unsigned long flags;
    pid_t fg_pgid;
    pid_t pgid;

    process = task_get_process(task);
    leader = task_get_process_leader(task);
    if (!process || !leader)
        return false;

    pgid = process->pgid;
    spin_lock_irqsave(&tty->lock, &flags);
    fg_pgid = tty->foreground_pgid;
    spin_unlock_irqrestore(&tty->lock, flags);

    if (leader_out)
        *leader_out = leader;
    if (pgid_out)
        *pgid_out = pgid;

    return fg_pgid > 0 && pgid > 0 && pgid != fg_pgid;
}

int tty_job_control_check_for_id(int tty_id, int signal_number)
{
    struct tty_struct *tty = tty_by_id(tty_id);
    task_t *leader = NULL;
    process_t *process;
    pid_t pgid = 0;

    if (!tty)
        return -ENODEV;
    if (!tty_current_task_is_background(tty, &leader, &pgid))
        return 0;

    process = task_get_process(leader);
    if (!process || signal_number <= 0 || signal_number >= MAX_SIGNALS)
        return -EINVAL;

    /*
     * POSIX permits a background group to perform the operation when the
     * corresponding job-control signal is blocked or explicitly ignored.
     * Otherwise the whole process group is stopped (default action) or its
     * installed handler is invoked, and the interrupted operation reports
     * EINTR.
     */
    if ((process->signals.blocked & (1u << signal_number)) != 0u ||
        process->signals.actions[signal_number].sa_handler == SIG_IGN)
        return 0;

    tty_signal_process_group(pgid, signal_number);
    (void)check_pending_signals();
    return -EINTR;
}

int tty_get_termios_for_id(int tty_id, struct termios *tio)
{
    unsigned long flags;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tio)
        return -EINVAL;
    if (!tty)
        return -ENODEV;

    memset(tio, 0, sizeof(*tio));

    spin_lock_irqsave(&tty->lock, &flags);
    *tio = tty->termios;
    spin_unlock_irqrestore(&tty->lock, flags);

    return 0;
}

int tty_get_termios(struct termios *tio)
{
    return tty_get_termios_for_id(TTY_CONSOLE_ID, tio);
}

int tty_set_termios_for_id(int tty_id, const struct termios *tio, int flush_input)
{
    unsigned long flags;
    struct termios next;
    task_t *reader = NULL;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tio)
        return -EINVAL;
    if (!tty)
        return -ENODEV;

    next = *tio;
    next.c_iflag &= (INLCR | IGNCR | ICRNL | IXON | IXOFF);
    next.c_oflag &= (OPOST | ONLCR | OCRNL | ONOCR | ONLRET);
    next.c_lflag &= (ECHO | ICANON | ISIG | IEXTEN |
                     ECHOE | ECHOK | ECHOCTL | ECHOKE);
    next.c_cflag &= (CS8 | CREAD | HUPCL);

    spin_lock_irqsave(&tty->lock, &flags);
    tty->termios = next;
    if (flush_input) {
        tty->input_head = 0;
        tty->input_tail = 0;
        tty->eof_pending = false;
        if (tty->read_wait && tty->read_wait->state == TASK_INTERRUPTIBLE) {
            reader = tty->read_wait;
            tty->read_wait = NULL;
        }
    }
    spin_unlock_irqrestore(&tty->lock, flags);

    tty_wake_reader(reader);

    return 0;
}

int tty_set_termios(const struct termios *tio, int flush_input)
{
    return tty_set_termios_for_id(TTY_CONSOLE_ID, tio, flush_input);
}

int tty_flush_for_id(int tty_id, int queue_selector)
{
    unsigned long flags;
    task_t *reader = NULL;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return -ENODEV;

    if (queue_selector != TCIFLUSH &&
        queue_selector != TCOFLUSH &&
        queue_selector != TCIOFLUSH)
        return -EINVAL;

    if (queue_selector == TCIFLUSH || queue_selector == TCIOFLUSH) {
        spin_lock_irqsave(&tty->lock, &flags);
        tty->input_head = 0;
        tty->input_tail = 0;
        tty->eof_pending = false;
        if (tty->read_wait && tty->read_wait->state == TASK_INTERRUPTIBLE)
            reader = tty->read_wait;
        tty->read_wait = NULL;
        spin_unlock_irqrestore(&tty->lock, flags);

        tty_wake_reader(reader);
    }

    if (queue_selector == TCOFLUSH || queue_selector == TCIOFLUSH) {
        spin_lock_irqsave(&tty->lock, &flags);
        tty->output_head = 0;
        tty->output_tail = 0;
        spin_unlock_irqrestore(&tty->lock, flags);
        tty_backend_set_tx_irq_enabled_to(tty, false);
    }

    return 0;
}

int tty_flush(int queue_selector)
{
    return tty_flush_for_id(TTY_CONSOLE_ID, queue_selector);
}

void tty_get_winsize_for_id(int tty_id, uint16_t *rows, uint16_t *cols,
                            uint16_t *xpixel, uint16_t *ypixel)
{
    unsigned long flags;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return;

    spin_lock_irqsave(&tty->lock, &flags);
    if (rows)
        *rows = tty->winsize_rows;
    if (cols)
        *cols = tty->winsize_cols;
    if (xpixel)
        *xpixel = tty->winsize_xpixel;
    if (ypixel)
        *ypixel = tty->winsize_ypixel;
    spin_unlock_irqrestore(&tty->lock, flags);
}

void tty_get_winsize(uint16_t *rows, uint16_t *cols,
                     uint16_t *xpixel, uint16_t *ypixel)
{
    tty_get_winsize_for_id(TTY_CONSOLE_ID, rows, cols, xpixel, ypixel);
}

int tty_set_winsize_for_id(int tty_id, uint16_t rows, uint16_t cols,
                           uint16_t xpixel, uint16_t ypixel)
{
    unsigned long flags;
    pid_t pgid = 0;
    bool changed;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (rows == 0 || cols == 0)
        return -EINVAL;
    if (!tty)
        return -ENODEV;

    spin_lock_irqsave(&tty->lock, &flags);
    changed = tty->winsize_rows != rows ||
              tty->winsize_cols != cols ||
              tty->winsize_xpixel != xpixel ||
              tty->winsize_ypixel != ypixel;
    tty->winsize_rows = rows;
    tty->winsize_cols = cols;
    tty->winsize_xpixel = xpixel;
    tty->winsize_ypixel = ypixel;
    if (changed)
        pgid = tty->foreground_pgid;
    spin_unlock_irqrestore(&tty->lock, flags);

    if (changed && pgid > 0)
        tty_signal_process_group(pgid, SIGWINCH);

    return 0;
}

int tty_set_winsize(uint16_t rows, uint16_t cols,
                    uint16_t xpixel, uint16_t ypixel)
{
    return tty_set_winsize_for_id(TTY_CONSOLE_ID, rows, cols, xpixel, ypixel);
}

pid_t tty_get_read_wait_pid_for_id(int tty_id)
{
    unsigned long flags;
    task_t* waiter;
    pid_t pid = 0;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return 0;

    spin_lock_irqsave(&tty->lock, &flags);
    waiter = tty->read_wait;
    if (waiter && waiter->process)
        pid = waiter->process->pid;
    spin_unlock_irqrestore(&tty->lock, flags);
    return pid;
}

pid_t tty_get_read_wait_pid(void)
{
    return tty_get_read_wait_pid_for_id(TTY_CONSOLE_ID);
}

int tty_get_read_wait_state_for_id(int tty_id)
{
    unsigned long flags;
    task_t* waiter;
    int state = -1;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty)
        return -1;

    spin_lock_irqsave(&tty->lock, &flags);
    waiter = tty->read_wait;
    if (waiter)
        state = (int)waiter->state;
    spin_unlock_irqrestore(&tty->lock, flags);
    return state;
}

int tty_get_read_wait_state(void)
{
    return tty_get_read_wait_state_for_id(TTY_CONSOLE_ID);
}

void tty_get_tx_stats_for_id(int tty_id, uint32_t *enqueued, uint32_t *drained,
                             uint32_t *full_waits, uint32_t *drain_calls)
{
    unsigned long flags;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty) {
        if (enqueued) *enqueued = 0;
        if (drained) *drained = 0;
        if (full_waits) *full_waits = 0;
        if (drain_calls) *drain_calls = 0;
        return;
    }

    spin_lock_irqsave(&tty->lock, &flags);
    if (enqueued)
        *enqueued = tty->output_enqueued;
    if (drained)
        *drained = tty->output_drained;
    if (full_waits)
        *full_waits = tty->output_full_waits;
    if (drain_calls)
        *drain_calls = tty->output_drain_calls;
    spin_unlock_irqrestore(&tty->lock, flags);
}

void tty_get_tx_stats(uint32_t *enqueued, uint32_t *drained,
                      uint32_t *full_waits, uint32_t *drain_calls)
{
    tty_get_tx_stats_for_id(TTY_CONSOLE_ID, enqueued, drained,
                            full_waits, drain_calls);
}

void tty_get_input_stats_for_id(int tty_id, uint32_t *depth, uint32_t *capacity,
                                uint32_t *eof_pending, uint32_t *iflag,
                                uint32_t *oflag, uint32_t *lflag,
                                uint32_t *vmin, uint32_t *vtime,
                                uint32_t *char_wakeups,
                                uint32_t *line_wakeups,
                                uint32_t *eof_wakeups)
{
    unsigned long flags;
    uint32_t head;
    uint32_t tail;
    struct termios tio;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (!tty) {
        if (depth) *depth = 0;
        if (capacity) *capacity = 0;
        if (eof_pending) *eof_pending = 0;
        if (iflag) *iflag = 0;
        if (oflag) *oflag = 0;
        if (lflag) *lflag = 0;
        if (vmin) *vmin = 0;
        if (vtime) *vtime = 0;
        if (char_wakeups) *char_wakeups = 0;
        if (line_wakeups) *line_wakeups = 0;
        if (eof_wakeups) *eof_wakeups = 0;
        return;
    }

    spin_lock_irqsave(&tty->lock, &flags);
    head = tty->input_head;
    tail = tty->input_tail;
    tio = tty->termios;

    if (depth)
        *depth = head >= tail ? head - tail : TTY_INPUT_BUF_SIZE - tail + head;
    if (capacity)
        *capacity = TTY_INPUT_BUF_SIZE - 1;
    if (eof_pending)
        *eof_pending = tty->eof_pending ? 1 : 0;
    if (iflag)
        *iflag = tio.c_iflag;
    if (oflag)
        *oflag = tio.c_oflag;
    if (lflag)
        *lflag = tio.c_lflag;
    if (vmin)
        *vmin = tio.c_cc[VMIN];
    if (vtime)
        *vtime = tio.c_cc[VTIME];
    if (char_wakeups)
        *char_wakeups = tty->char_wakeups;
    if (line_wakeups)
        *line_wakeups = tty->line_wakeups;
    if (eof_wakeups)
        *eof_wakeups = tty->eof_wakeups;
    spin_unlock_irqrestore(&tty->lock, flags);
}

void tty_get_input_stats(uint32_t *depth, uint32_t *capacity,
                         uint32_t *eof_pending, uint32_t *iflag,
                         uint32_t *oflag, uint32_t *lflag,
                         uint32_t *vmin, uint32_t *vtime,
                         uint32_t *char_wakeups,
                         uint32_t *line_wakeups,
                         uint32_t *eof_wakeups)
{
    tty_get_input_stats_for_id(TTY_CONSOLE_ID, depth, capacity,
                               eof_pending, iflag, oflag, lflag,
                               vmin, vtime, char_wakeups,
                               line_wakeups, eof_wakeups);
}

bool tty_has_pending_output(void)
{
    unsigned long flags;
    bool pending = false;

    for (int tty_id = 0; tty_id < TTY_MAX; tty_id++) {
        struct tty_struct *tty = tty_by_id(tty_id);

        spin_lock_irqsave(&tty->lock, &flags);
        pending = !tty_output_empty_locked(tty);
        spin_unlock_irqrestore(&tty->lock, flags);
        if (pending)
            return true;
    }

    return pending;
}

static void tty_wake_reader(task_t *reader)
{
    if (!reader)
        return;

    task_wake(reader);
}

static bool tty_normalize_input_char(const struct termios *tio, char *c)
{
    if (*c == '\r') {
        if (tio->c_iflag & IGNCR)
            return false;
        if (tio->c_iflag & ICRNL)
            *c = '\n';
    } else if (*c == '\n' && (tio->c_iflag & INLCR)) {
        *c = '\r';
    }

    return true;
}

static bool tty_handle_signal_char_locked(struct tty_struct *tty,
                                          const struct termios *tio, char c,
                                          unsigned long *flags)
{
    int sig;
    int delivered;
    pid_t pgid;
    const char *echo;

    if (!(tio->c_lflag & ISIG))
        return false;

    if (c == tio->c_cc[VINTR]) {
        sig = SIGINT;
        echo = "^C\n";
        tty->ctrl_c_seen++;
    } else if (c == tio->c_cc[VSUSP]) {
        sig = SIGTSTP;
        echo = "^Z\n";
        tty->ctrl_z_seen++;
    } else {
        return false;
    }

    pgid = tty->foreground_pgid;
    tty->last_signal_pgid = pgid;
    tty->last_signal = sig;

    spin_unlock_irqrestore(&tty->lock, *flags);
    tty_backend_puts_to(tty, echo);
    delivered = tty_signal_process_group(pgid, sig);
    spin_lock_irqsave(&tty->lock, flags);

    tty->last_signal_delivered = delivered;
    if (sig == SIGINT) {
        if (delivered > 0)
            tty->sigint_delivered += delivered;
        else
            tty->sigint_missed++;
    } else {
        if (delivered > 0)
            tty->sigtstp_delivered += delivered;
        else
            tty->sigtstp_missed++;
    }

    return true;
}

static task_t *tty_take_interruptible_reader_locked(struct tty_struct *tty)
{
    task_t *reader = NULL;

    if (tty->read_wait && tty->read_wait->state == TASK_INTERRUPTIBLE) {
        reader = tty->read_wait;
        tty->read_wait = NULL;
    }

    return reader;
}

static bool tty_handle_eof_locked(struct tty_struct *tty,
                                  const struct termios *tio, char c,
                                  task_t **reader)
{
    if (!((tio->c_lflag & ICANON) && (tio->c_lflag & ECHO)) ||
        c != tio->c_cc[VEOF])
        return false;

    tty->eof_pending = true;
    *reader = tty_take_interruptible_reader_locked(tty);
    if (*reader)
        tty->eof_wakeups++;

    return true;
}

static bool tty_handle_canonical_edit_locked(struct tty_struct *tty,
                                             const struct termios *tio, char c)
{
    bool canonical_echo = (tio->c_lflag & ICANON) && (tio->c_lflag & ECHO);

    if (!canonical_echo)
        return false;

    /* In canonical echo mode, the kernel owns basic line editing. mash keeps
     * ECHO disabled and still receives raw editing keys for its own editor. */
    if (c == '\b' || c == tio->c_cc[VERASE]) {
        if (tty->input_head != tty->input_tail) {
            uint32_t prev = tty_input_prev(tty->input_head);
            if (tty->input_buf[prev] != '\n' && tty->input_buf[prev] != '\r') {
                tty->input_head = prev;
                tty_backend_puts_to(tty, "\b \b");
            }
        }
        return true;
    }

    if (c == tio->c_cc[VKILL]) {
        while (tty->input_head != tty->input_tail) {
            uint32_t prev = tty_input_prev(tty->input_head);
            if (tty->input_buf[prev] == '\n' || tty->input_buf[prev] == '\r')
                break;
            tty->input_head = prev;
            tty_backend_puts_to(tty, "\b \b");
        }
        return true;
    }

    if (c == tio->c_cc[VWERASE]) {
        while (tty->input_head != tty->input_tail) {
            uint32_t prev = tty_input_prev(tty->input_head);
            char pc = tty->input_buf[prev];
            if (pc == '\n' || pc == '\r' || pc != ' ')
                break;
            tty->input_head = prev;
            tty_backend_puts_to(tty, "\b \b");
        }
        while (tty->input_head != tty->input_tail) {
            uint32_t prev = tty_input_prev(tty->input_head);
            char pc = tty->input_buf[prev];
            if (pc == '\n' || pc == '\r' || pc == ' ')
                break;
            tty->input_head = prev;
            tty_backend_puts_to(tty, "\b \b");
        }
        return true;
    }

    return false;
}

static task_t *tty_enqueue_input_char_locked(struct tty_struct *tty,
                                             const struct termios *tio, char c)
{
    uint32_t next_head;
    bool canonical_echo = (tio->c_lflag & ICANON) && (tio->c_lflag & ECHO);
    bool should_wake = true;
    task_t *reader = NULL;

    if (tio->c_lflag & ECHO)
        tty_backend_putc_to(tty, c);

    next_head = (tty->input_head + 1) % TTY_INPUT_BUF_SIZE;
    if (next_head == tty->input_tail) {
        tty->input_dropped++;
        return NULL;
    }

    tty->input_buf[tty->input_head] = c;
    tty->input_head = next_head;
    tty->input_chars++;
    if (canonical_echo)
        should_wake = (c == '\n' || c == '\r');

    /* read(0, &c, 1) doit être réveillé dès qu'un caractère arrive.
     * Ne pas conserver un waiter mort ou qui n'attend plus le TTY: après
     * un signal, il doit revenir en userland via -EINTR plutôt que voler
     * le prochain caractère du shell. */
    if (should_wake && tty->read_wait) {
        if (tty->read_wait->state == TASK_INTERRUPTIBLE) {
            reader = tty->read_wait;
            tty->read_wait = NULL;
            if (canonical_echo)
                tty->line_wakeups++;
            else
                tty->char_wakeups++;
        } else if (tty->read_wait->state == TASK_ZOMBIE ||
                   tty->read_wait->state == TASK_TERMINATED ||
                   tty->read_wait->state == TASK_READY ||
                   tty->read_wait->state == TASK_RUNNING) {
            kernel_lifecycle_stats.tty_stale_waiters++;
            tty->read_wait = NULL;
        }
    }

    return reader;
}

static void tty_input_char_to(struct tty_struct *tty, char c)
{
    task_t* reader = NULL;
    struct termios tio;
    unsigned long flags;

    if (!tty)
        return;

    spin_lock_irqsave(&tty->lock, &flags);
    tio = tty->termios;

    if (!tty_normalize_input_char(&tio, &c)) {
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    if (tty_handle_signal_char_locked(tty, &tio, c, &flags)) {
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    if (tty_handle_eof_locked(tty, &tio, c, &reader)) {
        spin_unlock_irqrestore(&tty->lock, flags);
        tty_wake_reader(reader);
        task_poll_notify();
        return;
    }

    if (tty_handle_canonical_edit_locked(tty, &tio, c)) {
        spin_unlock_irqrestore(&tty->lock, flags);
        return;
    }

    reader = tty_enqueue_input_char_locked(tty, &tio, c);
    spin_unlock_irqrestore(&tty->lock, flags);
    tty_wake_reader(reader);
    task_poll_notify();
}

void tty_input_char_to_id(int tty_id, char c)
{
    tty_input_char_to(tty_by_id(tty_id), c);
}

/* Compatibility entry point for input drivers that target the primary TTY. */
void tty_input_char(char c) {
    tty_input_char_to_id(TTY_CONSOLE_ID, c);
}

bool tty_read_ready_for_id(int tty_id)
{
    struct tty_struct *tty = tty_by_id(tty_id);
    unsigned long flags;
    struct termios tio;
    uint32_t pos;
    bool ready = false;

    if (!tty)
        return false;

    if (tty->backend) {
        while (tty->backend->has_data()) {
            int c = tty->backend->getc();
            if (c < 0)
                break;
            tty_input_char_to(tty, (char)c);
        }
    }

    spin_lock_irqsave(&tty->lock, &flags);
    tio = tty->termios;

    if (tty->eof_pending) {
        ready = true;
    } else if (tty->input_head != tty->input_tail) {
        if (!(tio.c_lflag & ICANON)) {
            ready = true;
        } else {
            pos = tty->input_tail;
            while (pos != tty->input_head) {
                char c = tty->input_buf[pos];
                if (c == '\n' || c == '\r') {
                    ready = true;
                    break;
                }
                pos = (pos + 1) % TTY_INPUT_BUF_SIZE;
            }
        }
    }

    spin_unlock_irqrestore(&tty->lock, flags);
    return ready;
}

static ssize_t tty_read_to(struct tty_struct *tty, char *buf, size_t count)
{
    size_t read = 0;
    bool interbyte_timer_active = false;
    uint32_t interbyte_deadline = 0;
    unsigned long flags;

    if (count == 0)
        return 0;

    if (!tty)
        return -ENODEV;

    if (tty_job_control_check_for_id(tty->id, SIGTTIN) < 0)
        return -EINTR;
    
    while (read < count) {
        while (tty->backend && tty->backend->has_data()) {
            int c = tty->backend->getc();
            if (c < 0) break;
            tty_input_char_to(tty, (char)c);
        }

        spin_lock_irqsave(&tty->lock, &flags);
        
        /* Buffer vide ? */
        if (tty->input_head == tty->input_tail) {
            struct termios tio = tty->termios;
            bool canon = (tio.c_lflag & ICANON) != 0;
            uint32_t vmin = tio.c_cc[VMIN];
            uint32_t timeout_ticks = (uint32_t)tio.c_cc[VTIME] * (TIMER_FREQ / 10);
            uint32_t deadline = 0;

            if (tty->eof_pending) {
                tty->eof_pending = false;
                spin_unlock_irqrestore(&tty->lock, flags);
                break;
            }

            if (read > 0) {
                if (canon || vmin == 0 || read >= vmin ||
                    (interbyte_timer_active && get_system_ticks() >= interbyte_deadline)) {
                    spin_unlock_irqrestore(&tty->lock, flags);
                    break;
                }
            }

            task_t *task = task_current_local();

            if (!task) {
                spin_unlock_irqrestore(&tty->lock, flags);
                break;
            }

            if (!canon && vmin == 0 && timeout_ticks == 0) {
                spin_unlock_irqrestore(&tty->lock, flags);
                break;
            }

            if (tty->read_wait && tty->read_wait != task) {
                if (tty->read_wait->state != TASK_INTERRUPTIBLE) {
                    tty->read_wait = NULL;
                    kernel_lifecycle_stats.tty_stale_waiters++;
                } else {
                    spin_unlock_irqrestore(&tty->lock, flags);
                    yield();
                    continue;
                }
            }

            tty->read_wait = task;
            if (!canon && timeout_ticks > 0) {
                if (vmin == 0 && read == 0) {
                    deadline = get_system_ticks() + timeout_ticks;
                } else if (read > 0) {
                    if (!interbyte_timer_active) {
                        interbyte_timer_active = true;
                        interbyte_deadline = get_system_ticks() + timeout_ticks;
                    }
                    deadline = interbyte_deadline;
                }
            }
            spin_unlock_irqrestore(&tty->lock, flags);

            task_set_interruptible_until(task, deadline);

            /*
             * A character may arrive between publishing read_wait and marking
             * the task interruptible. The input path then sees a RUNNING waiter,
             * treats it as stale, clears read_wait, and leaves the byte queued.
             * Recheck both conditions before sleeping; otherwise the reader can
             * go to sleep with data already buffered and no registered waiter.
             */
            spin_lock_irqsave(&tty->lock, &flags);
            if (tty->input_head != tty->input_tail ||
                tty->eof_pending ||
                tty->read_wait != task) {
                if (tty->read_wait == task)
                    tty->read_wait = NULL;
                spin_unlock_irqrestore(&tty->lock, flags);
                task_set_wakeup_time(task, 0);
                task_set_state(task, TASK_RUNNING);
                continue;
            }
            spin_unlock_irqrestore(&tty->lock, flags);

            schedule();
            spin_lock_irqsave(&tty->lock, &flags);
            if (tty->read_wait == task)
                tty->read_wait = NULL;
            spin_unlock_irqrestore(&tty->lock, flags);

            if (has_pending_signals(task))
                return read > 0 ? (ssize_t)read : (ssize_t)-EINTR;
            if (!canon && timeout_ticks > 0 && deadline && get_system_ticks() >= deadline) {
                task_set_wakeup_time(task, 0);
                break;
            }
            task_set_wakeup_time(task, 0);
            continue;
        }
        
        /* Lire un caractère */
        char c = tty->input_buf[tty->input_tail];
        struct termios tio = tty->termios;
        tty->input_tail = (tty->input_tail + 1) % TTY_INPUT_BUF_SIZE;
        
        spin_unlock_irqrestore(&tty->lock, flags);
        
        buf[read++] = c;

        if (!(tio.c_lflag & ICANON)) {
            uint32_t vmin = tio.c_cc[VMIN];
            uint32_t timeout_ticks = (uint32_t)tio.c_cc[VTIME] * (TIMER_FREQ / 10);

            if (vmin > 0 && read >= vmin)
                break;

            if (timeout_ticks > 0) {
                interbyte_timer_active = true;
                interbyte_deadline = get_system_ticks() + timeout_ticks;
            }

            continue;
        }
        
        /* En mode ligne, s'arrêter au '\n' */
        if (c == '\n' || c == '\r') {
            break;
        }
    }
    
    return read;
}

ssize_t tty_read(char *buf, size_t count) {
    return tty_read_to(&tty0, buf, count);
}

static void tty_drain_output_limited_to(struct tty_struct *tty, uint32_t budget);

static ssize_t tty_write_to(struct tty_struct *tty, const char *buf, size_t count) {
    uint32_t oflag;
    unsigned long flags;
    unsigned long write_flags;
    size_t written = 0;

    if (!tty)
        return -ENODEV;

    spin_lock_irqsave(&tty->lock, &flags);
    oflag = tty->termios.c_oflag;
    spin_unlock_irqrestore(&tty->lock, flags);

    spin_lock_irqsave(&tty->write_lock, &write_flags);

    for (size_t i = 0; i < count; i++) {
        if ((oflag & OPOST) && (oflag & ONLCR) && buf[i] == '\n') {
            while (1) {
                spin_lock_irqsave(&tty->lock, &flags);
                if (!tty_output_full_locked(tty)) {
                    tty->output_buf[tty->output_head] = '\r';
                    tty->output_head = tty_output_next(tty->output_head);
                    tty->output_enqueued++;
                    spin_unlock_irqrestore(&tty->lock, flags);
                    tty_backend_set_tx_irq_enabled_to(tty, true);
                    break;
                }
                tty->output_full_waits++;
                spin_unlock_irqrestore(&tty->lock, flags);
                tty_drain_output_limited_to(tty, TTY_TX_DRAIN_BUDGET);
                task_t *task = task_current_local();
                if (task && has_pending_signals(task)) {
                    spin_unlock_irqrestore(&tty->write_lock, write_flags);
                    return written > 0 ? (ssize_t)written : (ssize_t)-EINTR;
                }
                if (task) {
                    spin_unlock_irqrestore(&tty->write_lock, write_flags);
                    yield();
                    spin_lock_irqsave(&tty->write_lock, &write_flags);
                } else {
                    tty_backend_putc_to(tty, '\r');
                    break;
                }
            }
        }

        while (1) {
            spin_lock_irqsave(&tty->lock, &flags);
            if (!tty_output_full_locked(tty)) {
                tty->output_buf[tty->output_head] = buf[i];
                tty->output_head = tty_output_next(tty->output_head);
                tty->output_enqueued++;
                spin_unlock_irqrestore(&tty->lock, flags);
                tty_backend_set_tx_irq_enabled_to(tty, true);
                break;
            }
            tty->output_full_waits++;
            spin_unlock_irqrestore(&tty->lock, flags);
            tty_drain_output_limited_to(tty, TTY_TX_DRAIN_BUDGET);
            task_t *task = task_current_local();
            if (task && has_pending_signals(task)) {
                spin_unlock_irqrestore(&tty->write_lock, write_flags);
                return written > 0 ? (ssize_t)written : (ssize_t)-EINTR;
            }
            if (task) {
                spin_unlock_irqrestore(&tty->write_lock, write_flags);
                yield();
                spin_lock_irqsave(&tty->write_lock, &write_flags);
            } else {
                tty_backend_putc_to(tty, buf[i]);
                break;
            }
        }
        written++;
    }
    tty_drain_output_limited_to(tty, TTY_OUTPUT_BUF_SIZE);
    spin_unlock_irqrestore(&tty->write_lock, write_flags);
    return (ssize_t)written;
}

ssize_t tty_write(const char *buf, size_t count) {
    return tty_write_to(&tty0, buf, count);
}

static void tty_drain_output_limited_to(struct tty_struct *tty, uint32_t budget)
{
    unsigned long flags;
    uint32_t drained = 0;

    if (!tty)
        return;

    spin_lock_irqsave(&tty->lock, &flags);
    tty->output_drain_calls++;
    spin_unlock_irqrestore(&tty->lock, flags);

    while (drained < budget) {
        char c;
        bool sent;

        spin_lock_irqsave(&tty->lock, &flags);
        if (tty_output_empty_locked(tty)) {
            spin_unlock_irqrestore(&tty->lock, flags);
            tty_backend_set_tx_irq_enabled_to(tty, false);
            return;
        }

        c = tty->output_buf[tty->output_tail];
        sent = tty_backend_try_putc_to(tty, c);
        if (sent) {
            tty->output_tail = tty_output_next(tty->output_tail);
            tty->output_drained++;
            drained++;
        }
        spin_unlock_irqrestore(&tty->lock, flags);

        if (!sent) {
            tty_backend_set_tx_irq_enabled_to(tty, true);
            return;
        }
    }

    spin_lock_irqsave(&tty->lock, &flags);
    if (tty_output_empty_locked(tty)) {
        spin_unlock_irqrestore(&tty->lock, flags);
        tty_backend_set_tx_irq_enabled_to(tty, false);
    } else {
        spin_unlock_irqrestore(&tty->lock, flags);
        tty_backend_set_tx_irq_enabled_to(tty, true);
    }
}

static void tty_drain_output_limited(uint32_t budget)
{
    tty_drain_output_limited_to(&tty0, budget);
    tty_drain_output_limited_to(&tty1, budget);
    tty_drain_output_limited_to(&ttyS0, budget);
}

void tty_drain_output(void)
{
    tty_drain_output_limited(TTY_TX_DRAIN_BUDGET);
}

void tty_drain_output_for_id(int tty_id)
{
    tty_drain_output_limited_to(tty_by_id(tty_id), TTY_TX_DRAIN_BUDGET);
}

static ssize_t tty_file_read(file_t* file, void* buf, size_t count) {
    int tty_id = file ? (int)(uintptr_t)file->private_data : TTY_CONSOLE_ID;
    struct tty_struct *tty = tty_by_id(tty_id);

    if (tty && tty->output_only)
        return -EACCES;
    return tty_read_to(tty, (char*)buf, count);
}

static ssize_t tty_file_write(file_t* file, const void* buf, size_t count) {
    int tty_id = file ? (int)(uintptr_t)file->private_data : TTY_CONSOLE_ID;
    struct tty_struct *tty = tty_by_id(tty_id);

    return tty_write_to(tty, (const char*)buf, count);
}

static file_operations_t tty_file_ops = {
    .read = tty_file_read,
    .write = tty_file_write,
    .open = NULL,
    .close = NULL,
    .lseek = NULL,
    .readdir = NULL
};

bool is_tty_device_path(const char* path)
{
    return path && (strcmp(path, "/dev/tty") == 0 ||
                    strcmp(path, "/dev/tty0") == 0 ||
                    strcmp(path, "/dev/tty1") == 0 ||
                    strcmp(path, "/dev/ttyS0") == 0 ||
                    strcmp(path, "/dev/console") == 0 ||
                    pty_is_slave_path(path));
}

int tty_current_controlling_id(void)
{
    task_t *task = task_current_local();
    int tty_id;

    if (!task || !task->process)
        return -ENXIO;

    tty_id = task->process->controlling_tty;
    if (!tty_by_id(tty_id))
        return -ENXIO;

    return tty_id;
}

int tty_id_from_device_path(const char* path)
{
    if (!path)
        return -ENODEV;
    if (strcmp(path, "/dev/tty") == 0)
        return tty_current_controlling_id();
    if (strcmp(path, "/dev/tty1") == 0)
        return TTY_GRAPHICS_ID;
    if (strcmp(path, "/dev/ttyS0") == 0)
        return TTY_SERIAL_ID;
    if (strcmp(path, "/dev/tty0") == 0 ||
        strcmp(path, "/dev/console") == 0)
        return TTY_CONSOLE_ID;
    if (pty_is_slave_path(path))
        return pty_slave_tty_id(path);
    return -ENODEV;
}

int tty_id_from_file(file_t* file)
{
    int tty_id;

    if (!file)
        return -ENOTTY;
    if (file->type == FILE_TYPE_PTY_MASTER)
        return pty_master_tty_id(file);
    if (file->type != FILE_TYPE_TTY)
        return -ENOTTY;

    tty_id = (int)(uintptr_t)file->private_data;
    return tty_by_id(tty_id) ? tty_id : -ENODEV;
}

static uint32_t tty_rdev_from_path(const char* path)
{
    if (path && strcmp(path, "/dev/tty") == 0)
        return DEV_CTTY_RDEV;
    if (path && strcmp(path, "/dev/console") == 0)
        return DEV_CONSOLE_RDEV;
    if (path && strcmp(path, "/dev/tty1") == 0)
        return DEV_TTY1_RDEV;
    if (path && strcmp(path, "/dev/ttyS0") == 0)
        return DEV_TTYS0_RDEV;
    return DEV_TTY_RDEV;
}

static int tty_id_from_name(const char* name)
{
    if (!name)
        return TTY_CONSOLE_ID;
    if (strcmp(name, "tty") == 0)
        return tty_current_controlling_id();
    if (strcmp(name, "tty1") == 0)
        return TTY_GRAPHICS_ID;
    if (strcmp(name, "ttyS0") == 0)
        return TTY_SERIAL_ID;
    if (strncmp(name, "pts/", 4u) == 0) {
        char path[16] = "/dev/";

        strncpy(path + 5u, name, sizeof(path) - 6u);
        path[sizeof(path) - 1u] = '\0';
        return pty_slave_tty_id(path);
    }
    return TTY_CONSOLE_ID;
}

void fill_tty_device_stat(const char* path, struct stat* st)
{
    uint32_t now;
    uint32_t rdev;

    if (!st) return;

    rdev = tty_rdev_from_path(path);
    now = get_current_time();

    memset(st, 0, sizeof(*st));
    st->st_dev = 0;
    st->st_ino = rdev;
    st->st_mode = S_IFCHR | 0666;
    st->st_nlink = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = rdev;
    st->st_size = 0;
    st->st_blksize = 1024;
    st->st_blocks = 0;
    st->st_atime = now;
    st->st_mtime = now;
    st->st_ctime = now;
}

file_t* create_tty_console_file(const char* name, int flags) {
    file_t* file = create_file();
    inode_t* inode;
    struct stat st;
    int tty_id;
    if (!file) return NULL;

    inode = create_inode();
    if (!inode) {
        kfree(file);
        return NULL;
    }

    tty_id = tty_id_from_name(name);
    if (tty_check_open_for_id(tty_id, flags) < 0) {
        kfree(inode);
        kfree(file);
        return NULL;
    }

    if (name && strcmp(name, "tty") == 0)
        fill_tty_device_stat("/dev/tty", &st);
    else if (name && strcmp(name, "console") == 0)
        fill_tty_device_stat("/dev/console", &st);
    else if (tty_id == TTY_GRAPHICS_ID)
        fill_tty_device_stat("/dev/tty1", &st);
    else if (tty_id == TTY_SERIAL_ID)
        fill_tty_device_stat("/dev/ttyS0", &st);
    else
        fill_tty_device_stat("/dev/tty0", &st);
    inode->mode = st.st_mode;
    inode->uid = st.st_uid;
    inode->gid = st.st_gid;
    inode->size = st.st_size;
    inode->blocks = st.st_blocks;
    inode->nlink = st.st_nlink;
    inode->first_cluster = 0;
    inode->parent_cluster = st.st_rdev;
    inode->atime = st.st_atime;
    inode->mtime = st.st_mtime;
    inode->ctime = st.st_ctime;
    inode->i_op = NULL;
    inode->f_op = &tty_file_ops;

    file->f_op = &tty_file_ops;
    file->flags = flags;
    file->type = FILE_TYPE_TTY;
    file->pos = 0;
    file->inode = inode;
    file->private_data = (void*)(uintptr_t)tty_id;

    if (name) {
        strncpy(file->name, name, sizeof(file->name) - 1);
        file->name[sizeof(file->name) - 1] = '\0';
    }

    return file;
}
