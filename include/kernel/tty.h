/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/tty.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _TTY_H
#define _TTY_H

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>

#define TTY_INPUT_BUF_SIZE  512
#define TTY_OUTPUT_BUF_SIZE 4096
#define TTY_MAX             3
#define TTY_CONSOLE_ID      0
#define TTY_GRAPHICS_ID     1
#define TTY_SERIAL_ID       2
#define DEV_TTY_RDEV        ((4u << 8) | 0u)
#define DEV_TTY1_RDEV       ((4u << 8) | 1u)
#define DEV_TTYS0_RDEV      ((4u << 8) | 64u)
#define DEV_CTTY_RDEV       ((5u << 8) | 0u)
#define DEV_CONSOLE_RDEV    ((5u << 8) | 1u)

#define TTY_STTY_SET_FOREGROUND_PGID 1
#define TTY_GTTY_GET_FOREGROUND_PGID 1
#define TTY_STTY_SET_FOREGROUND_PGID_FD 2
#define TTY_GTTY_GET_FOREGROUND_PGID_FD 2

#define NCCS 32

#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6
#define VSTART    8
#define VSTOP     9
#define VSUSP     10
#define VEOL      11
#define VREPRINT  12
#define VDISCARD  13
#define VWERASE   14
#define VLNEXT    15
#define VEOL2     16

typedef uint32_t tcflag_t;
typedef uint8_t cc_t;
typedef uint32_t speed_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

/*
 * Low-level console backend used by the TTY line discipline.
 *
 * The backend owns the physical transport (currently PL011 UART). The TTY
 * layer owns termios, canonical/raw input, foreground process groups, signal
 * generation, read waiters and output buffering.
 *
 * Rules:
 * - putc/puts may block or poll; they are used only for direct echo/fallback.
 * - try_putc must be non-blocking and return false when TX cannot accept data.
 * - set_tx_irq_enabled controls TX empty interrupts for buffered output.
 * - has_data/getc are polled by tty_read() in addition to UART IRQ delivery.
 */
typedef struct tty_backend_ops {
    void (*putc)(char c);
    bool (*try_putc)(char c);
    void (*puts)(const char *s);
    void (*set_tx_irq_enabled)(bool enabled);
    bool (*has_data)(void);
    int (*getc)(void);
} tty_backend_ops_t;

struct tty_struct {
    int id;
    const tty_backend_ops_t *backend;
    const tty_backend_ops_t *output_mirror;
    bool output_only;
    bool (*virtual_putc)(void *context, char c);
    void *virtual_context;

    /* Buffers circulaires */
    char input_buf[TTY_INPUT_BUF_SIZE];
    uint32_t input_head;
    uint32_t input_tail;

    char output_buf[TTY_OUTPUT_BUF_SIZE];
    uint32_t output_head;
    uint32_t output_tail;
    uint32_t output_enqueued;
    uint32_t output_drained;
    uint32_t output_full_waits;
    uint32_t output_drain_calls;
    
    /* POSIX-ish terminal state. */
    struct termios termios;
    pid_t foreground_pgid;
    uint16_t winsize_rows;
    uint16_t winsize_cols;
    uint16_t winsize_xpixel;
    uint16_t winsize_ypixel;
    
    /* Wait queue pour read bloquant */
    task_t *read_wait;
    bool eof_pending;

    /* Diagnostic counters for long-idle / foreground signal issues. */
    uint32_t input_chars;
    uint32_t char_wakeups;
    uint32_t line_wakeups;
    uint32_t eof_wakeups;
    uint32_t ctrl_c_seen;
    uint32_t ctrl_z_seen;
    uint32_t sigint_delivered;
    uint32_t sigint_missed;
    uint32_t sigtstp_delivered;
    uint32_t sigtstp_missed;
    pid_t last_signal_pgid;
    int last_signal;
    int last_signal_delivered;
    
    spinlock_t lock;
    /*
     * Serializes logical writes. The main lock protects ring-buffer state;
     * this one keeps concurrent writers from interleaving a normal line one
     * byte at a time on SMP.
     */
    spinlock_t write_lock;
};

/* c_iflag */
#define INLCR   0x00000040
#define IGNCR   0x00000080
#define ICRNL   0x00000100
#define IXON    0x00000200
#define IXOFF   0x00000400

/* c_oflag */
#define OPOST   0x00000001
#define ONLCR   0x00000002
#define OCRNL   0x00000004
#define ONOCR   0x00000008
#define ONLRET  0x00000010

/* c_lflag */
#define ECHO    0x00000001
#define ICANON  0x00000002  /* Mode ligne (buffering jusqu'a \n) */
#define ISIG    0x00000004  /* Generer SIGINT/SIGTSTP depuis les caracteres de controle */
#define IEXTEN  0x00000008
#define ECHOE   0x00000010
#define ECHOK   0x00000020
#define ECHOCTL 0x00000040
#define ECHOKE  0x00000080

/* c_cflag */
#define CS8     0x00000300
#define CREAD   0x00000800
#define HUPCL   0x00001000

/* tcflush queue selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

extern struct tty_struct tty0;
extern struct tty_struct tty1;
extern struct tty_struct ttyS0;

void tty_init(void);
int tty_attach_backend(const tty_backend_ops_t *ops);
int tty_attach_backend_to(int tty_id, const tty_backend_ops_t *ops);
int tty_attach_output_backend_to(int tty_id, const tty_backend_ops_t *ops);
int tty_attach_output_mirror_to(int tty_id, const tty_backend_ops_t *ops);
int tty_set_active(int tty_id);
int tty_get_active(void);
void tty_set_kernel_console(int tty_id, bool replay_log);
bool tty_kernel_console_putc(char c);
bool tty_has_backend_for_id(int tty_id);
bool tty_is_output_only_for_id(int tty_id);
int tty_check_open_for_id(int tty_id, int flags);
bool tty_output_pending_for_id(int tty_id);
void tty_input_char(char c);
void tty_input_char_to_id(int tty_id, char c);
bool tty_has_pending_output(void);
void tty_drain_output(void);
void tty_drain_output_for_id(int tty_id);
bool tty_console_output_lock(unsigned long *flags);
void tty_console_output_unlock(unsigned long flags);
ssize_t tty_read(char *buf, size_t count);
ssize_t tty_write(const char *buf, size_t count);
bool tty_read_ready_for_id(int tty_id);
int tty_get_termios(struct termios *tio);
int tty_set_termios(const struct termios *tio, int flush_input);
int tty_flush(int queue_selector);
int tty_get_termios_for_id(int tty_id, struct termios *tio);
int tty_set_termios_for_id(int tty_id, const struct termios *tio, int flush_input);
int tty_flush_for_id(int tty_id, int queue_selector);
void tty_get_winsize(uint16_t *rows, uint16_t *cols,
                     uint16_t *xpixel, uint16_t *ypixel);
int tty_set_winsize(uint16_t rows, uint16_t cols,
                    uint16_t xpixel, uint16_t ypixel);
void tty_get_winsize_for_id(int tty_id, uint16_t *rows, uint16_t *cols,
                            uint16_t *xpixel, uint16_t *ypixel);
int tty_set_winsize_for_id(int tty_id, uint16_t rows, uint16_t cols,
                           uint16_t xpixel, uint16_t ypixel);
int tty_set_foreground_pgid(pid_t pgid);
pid_t tty_get_foreground_pgid(void);
int tty_set_foreground_pgid_for_id(int tty_id, pid_t pgid);
pid_t tty_get_foreground_pgid_for_id(int tty_id);
pid_t tty_get_read_wait_pid(void);
int tty_get_read_wait_state(void);
pid_t tty_get_read_wait_pid_for_id(int tty_id);
int tty_get_read_wait_state_for_id(int tty_id);
void tty_get_tx_stats(uint32_t *enqueued, uint32_t *drained,
                      uint32_t *full_waits, uint32_t *drain_calls);
void tty_get_tx_stats_for_id(int tty_id, uint32_t *enqueued, uint32_t *drained,
                             uint32_t *full_waits, uint32_t *drain_calls);
void tty_get_input_stats(uint32_t *depth, uint32_t *capacity,
                         uint32_t *eof_pending, uint32_t *iflag,
                         uint32_t *oflag, uint32_t *lflag,
                         uint32_t *vmin, uint32_t *vtime,
                         uint32_t *char_wakeups,
                         uint32_t *line_wakeups,
                         uint32_t *eof_wakeups);
void tty_get_input_stats_for_id(int tty_id, uint32_t *depth, uint32_t *capacity,
                                uint32_t *eof_pending, uint32_t *iflag,
                                uint32_t *oflag, uint32_t *lflag,
                                uint32_t *vmin, uint32_t *vtime,
                                uint32_t *char_wakeups,
                                uint32_t *line_wakeups,
                                uint32_t *eof_wakeups);
bool is_tty_device_path(const char* path);
void fill_tty_device_stat(const char* path, struct stat* st);
int tty_id_from_device_path(const char* path);
int tty_id_from_file(file_t* file);
int tty_current_controlling_id(void);
file_t* create_tty_console_file(const char* name, int flags);
int tty_register_virtual(struct tty_struct *tty, int tty_id,
                         bool (*putc)(void *context, char c),
                         void *context);
void tty_unregister_virtual(int tty_id);

#endif
