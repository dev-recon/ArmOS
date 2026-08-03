/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/coreutils/src/mtop.c
 * Layer: Userland / core utility
 * Description: POSIX-like command-line utility for ArmOS.
 */

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define TOP_PROC_BUF 4096
#define TOP_FRAME_BUF 32768
#define TOP_MAX_TASKS 128
#define TOP_HZ 1000
#define TOP_MAX_CPUS 8
#define TOP_DEFAULT_ROWS 24
#define TOP_DEFAULT_COLS 80
#define TOP_HEADER_ROWS 7
#define TOP_FULL_WIDTH 128

#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"
#define C_DIM     "\033[2m"
#define C_GREEN   "\033[32m"
#define C_YELLOW  "\033[33m"
#define C_BLUE    "\033[34m"
#define C_MAGENTA "\033[35m"
#define C_CYAN    "\033[36m"
#define C_RED     "\033[31m"

typedef struct top_task {
    int pid;
    int tty;
    unsigned runtime_ticks;
    unsigned user_ticks;
    unsigned system_ticks;
    unsigned cpu_pct_x10;
    unsigned user_pct_x10;
    unsigned system_pct_x10;
    unsigned priority;
    unsigned debt_score;
    unsigned ctx;
    unsigned pf;
    unsigned rss_kb;
    int cpu;
    char state;
    char state_detail[16];
    char name[64];
} top_task_t;

typedef struct top_mem {
    unsigned total_kb;
    unsigned free_kb;
} top_mem_t;

typedef struct top_sample {
    int pid;
    unsigned runtime_ticks;
    unsigned user_ticks;
    unsigned system_ticks;
} top_sample_t;

typedef struct top_cpu_sample {
    unsigned user_ticks;
    unsigned system_ticks;
    unsigned irq_ticks;
    unsigned idle_ticks;
} top_cpu_sample_t;

typedef struct top_buf {
    char *data;
    int len;
    int cap;
} top_buf_t;

static volatile sig_atomic_t top_running = 1;
static top_task_t top_tasks[TOP_MAX_TASKS];
static top_sample_t top_prev_samples[TOP_MAX_TASKS];
static int top_prev_count = 0;
static int top_have_prev = 0;
static struct termios top_saved_termios;
static int top_termios_saved = 0;
static unsigned top_cpu_load_x10[TOP_MAX_CPUS];
static unsigned top_cpu_user_x10[TOP_MAX_CPUS];
static unsigned top_cpu_system_x10[TOP_MAX_CPUS];
static unsigned top_cpu_irq_x10[TOP_MAX_CPUS];
static unsigned top_cpu_idle_x10[TOP_MAX_CPUS];
static top_cpu_sample_t top_prev_cpu[TOP_MAX_CPUS];
static int top_have_cpu_prev = 0;
static unsigned top_cpu_elapsed_ticks = TOP_HZ;
static unsigned top_cpu_count = 1;
static char top_proc_dirbuf[TOP_PROC_BUF];
static char top_frame_data[TOP_FRAME_BUF];

static int top_write_all(int fd, const char *buf, size_t count)
{
    size_t written = 0;

    while (written < count) {
        ssize_t result = write(fd, buf + written, count - written);

        if (result <= 0)
            return -1;
        written += (size_t)result;
    }
    return 0;
}

static void top_buf_free(top_buf_t *buf)
{
    buf->len = 0;
}

static int top_buf_append(top_buf_t *buf, const char *s, int len)
{
    if (!s || len <= 0)
        return 0;

    if (len > buf->cap - buf->len)
        return -1;

    memcpy(buf->data + buf->len, s, (size_t)len);
    buf->len += len;
    return 0;
}

static int top_buf_printf(top_buf_t *buf, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (len < 0)
        return -1;

    if (len < (int)sizeof(tmp))
        return top_buf_append(buf, tmp, len);

    {
        char *dyn = malloc((size_t)len + 1);
        int rc;

        if (!dyn)
            return -1;

        va_start(ap, fmt);
        vsnprintf(dyn, (size_t)len + 1, fmt, ap);
        va_end(ap);
        rc = top_buf_append(buf, dyn, len);
        free(dyn);
        return rc;
    }
}

static void top_enter_screen(void)
{
    static const char seq[] = "\033[?1049h\033[?25l\033[H\033[2J";
    top_write_all(STDOUT_FILENO, seq, sizeof(seq) - 1);
}

static void top_leave_screen(void)
{
    static const char seq[] = "\033[?2026l" C_RESET "\033[?25h\033[?1049l";
    top_write_all(STDOUT_FILENO, seq, sizeof(seq) - 1);
}

static void top_enable_interactive(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &top_saved_termios) < 0)
        return;

    raw = top_saved_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
        top_termios_saved = 1;
}

static void top_set_raw_poll(void)
{
    struct termios raw;

    if (!top_termios_saved)
        return;
    if (tcgetattr(STDIN_FILENO, &raw) < 0)
        return;

    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void top_restore_terminal(void)
{
    if (top_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &top_saved_termios);
        top_termios_saved = 0;
    }
}

static void on_signal(int sig)
{
    (void)sig;
    top_running = 0;
}

static void handle_key(char c, unsigned *delay_sec)
{
    switch (c) {
    case 'q':
    case 'Q':
    case 3:     /* Ctrl-C when ISIG is not active for any reason. */
    case 4:     /* Ctrl-D */
        top_running = 0;
        break;
    case '+':
    case '=':
        if (*delay_sec < 60)
            (*delay_sec)++;
        break;
    case '-':
    case '_':
        if (*delay_sec > 1)
            (*delay_sec)--;
        break;
    case 'r':
    case 'R':
        top_have_prev = 0;
        top_prev_count = 0;
        top_have_cpu_prev = 0;
        break;
    default:
        break;
    }
}

static void poll_input(unsigned *delay_sec)
{
    char c;

    while (read(STDIN_FILENO, &c, 1) == 1)
        handle_key(c, delay_sec);
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static const char *parse_uint(const char *p, unsigned *out)
{
    unsigned value = 0;

    p = skip_ws(p);
    if (!is_digit(*p))
        return NULL;

    while (is_digit(*p)) {
        value = value * 10u + (unsigned)(*p - '0');
        p++;
    }

    *out = value;
    return p;
}

static const char *parse_int(const char *p, int *out)
{
    int sign = 1;
    unsigned value = 0;

    p = skip_ws(p);
    if (*p == '-') {
        sign = -1;
        p++;
    }

    p = parse_uint(p, &value);
    if (!p)
        return NULL;

    *out = (int)value * sign;
    return p;
}

static int read_file(const char *path, char *buf, int size)
{
    int fd;
    int n;

    if (!buf || size <= 1)
        return -1;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return -1;

    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;

    buf[n] = '\0';
    return n;
}

static int starts_with_key(const char *p, const char *key)
{
    while (*key) {
        if (*p++ != *key++)
            return 0;
    }
    return 1;
}

static const char *line_after_key(const char *buf, const char *key)
{
    const char *p = buf;

    while (*p) {
        if (starts_with_key(p, key))
            return p + strlen(key);
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    return NULL;
}

static void read_mem(top_mem_t *mem)
{
    char buf[1024];
    const char *p;

    memset(mem, 0, sizeof(*mem));
    if (read_file("/proc/meminfo", buf, sizeof(buf)) < 0)
        return;

    p = line_after_key(buf, "MemTotal:");
    if (p)
        parse_uint(p, &mem->total_kb);
    p = line_after_key(buf, "MemFree:");
    if (p)
        parse_uint(p, &mem->free_kb);
}

static unsigned read_cpu_count(void)
{
    char buf[1024];
    const char *p;
    unsigned cpus = 1;

    if (read_file("/proc/smp", buf, sizeof(buf)) < 0)
        return 1;

    p = line_after_key(buf, "possible:");
    if (p)
        parse_uint(p, &cpus);

    if (cpus == 0)
        cpus = 1;
    if (cpus > TOP_MAX_CPUS)
        cpus = TOP_MAX_CPUS;
    return cpus;
}

static int parse_cpu_sample(const char *p, top_cpu_sample_t *sample)
{
    unsigned ignored;

    memset(sample, 0, sizeof(*sample));
    p = parse_uint(p, &sample->user_ticks);
    if (!p) return -1;
    p = parse_uint(p, &ignored);               /* nice */
    if (!p) return -1;
    p = parse_uint(p, &sample->system_ticks);
    if (!p) return -1;
    p = parse_uint(p, &sample->idle_ticks);
    if (!p) return -1;
    p = parse_uint(p, &ignored);               /* iowait */
    if (!p) return -1;
    p = parse_uint(p, &sample->irq_ticks);
    return p ? 0 : -1;
}

static void update_cpu_accounting(void)
{
    top_cpu_sample_t current[TOP_MAX_CPUS];
    char buf[1024];
    const char *p;
    unsigned max_elapsed = 0;

    memset(current, 0, sizeof(current));
    memset(top_cpu_load_x10, 0, sizeof(top_cpu_load_x10));
    memset(top_cpu_user_x10, 0, sizeof(top_cpu_user_x10));
    memset(top_cpu_system_x10, 0, sizeof(top_cpu_system_x10));
    memset(top_cpu_irq_x10, 0, sizeof(top_cpu_irq_x10));
    memset(top_cpu_idle_x10, 0, sizeof(top_cpu_idle_x10));

    if (read_file("/proc/stat", buf, sizeof(buf)) < 0)
        return;

    p = buf;
    while (*p) {
        if (p[0] == 'c' && p[1] == 'p' && p[2] == 'u' && is_digit(p[3])) {
            unsigned cpu;
            const char *values = parse_uint(p + 3, &cpu);

            if (values && cpu < top_cpu_count && cpu < TOP_MAX_CPUS)
                parse_cpu_sample(values, &current[cpu]);
        }
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    if (top_have_cpu_prev) {
        for (unsigned cpu = 0; cpu < top_cpu_count; cpu++) {
            unsigned user = current[cpu].user_ticks -
                            top_prev_cpu[cpu].user_ticks;
            unsigned system = current[cpu].system_ticks -
                              top_prev_cpu[cpu].system_ticks;
            unsigned irq = current[cpu].irq_ticks -
                           top_prev_cpu[cpu].irq_ticks;
            unsigned idle = current[cpu].idle_ticks -
                            top_prev_cpu[cpu].idle_ticks;
            unsigned total = user + system + irq + idle;

            if (total == 0)
                continue;
            if (total > max_elapsed)
                max_elapsed = total;

            top_cpu_user_x10[cpu] = (user * 1000u) / total;
            top_cpu_system_x10[cpu] = (system * 1000u) / total;
            top_cpu_irq_x10[cpu] = (irq * 1000u) / total;
            top_cpu_idle_x10[cpu] = (idle * 1000u) / total;
            top_cpu_load_x10[cpu] =
                top_cpu_user_x10[cpu] +
                top_cpu_system_x10[cpu] +
                top_cpu_irq_x10[cpu];
            if (top_cpu_load_x10[cpu] > 1000u)
                top_cpu_load_x10[cpu] = 1000u;
        }
    }

    memcpy(top_prev_cpu, current, sizeof(top_prev_cpu));
    top_have_cpu_prev = 1;
    if (max_elapsed != 0)
        top_cpu_elapsed_ticks = max_elapsed;
}

static int parse_proc_stat(const char *buf, top_task_t *task)
{
    const char *p;
    const char *start;
    const char *end;
    int dummy;
    unsigned udummy;
    int len;

    p = parse_int(buf, &task->pid);
    if (!p)
        return -1;

    p = skip_ws(p);
    if (*p != '(')
        return -1;

    start = ++p;
    end = start;
    while (*end && *end != ')')
        end++;
    if (*end != ')')
        return -1;

    len = (int)(end - start);
    if (len >= (int)sizeof(task->name))
        len = (int)sizeof(task->name) - 1;
    memcpy(task->name, start, (size_t)len);
    task->name[len] = '\0';

    p = skip_ws(end + 1);
    task->state = *p ? *p++ : 'S';

    p = parse_int(p, &dummy);       /* ppid */
    if (!p) return -1;
    p = parse_int(p, &dummy);       /* pgid */
    if (!p) return -1;
    p = parse_int(p, &dummy);       /* sid */
    if (!p) return -1;
    p = parse_int(p, &task->tty);
    if (!p) return -1;
    p = parse_uint(p, &task->pf);
    if (!p) return 0;
    p = parse_uint(p, &udummy);     /* cow faults */
    if (!p) return 0;
    p = parse_uint(p, &udummy);     /* stack faults */
    if (!p) return 0;
    p = parse_uint(p, &task->ctx);
    if (!p) return 0;
    p = parse_uint(p, &task->runtime_ticks);
    if (!p) return 0;
    p = parse_uint(p, &task->user_ticks);
    if (!p) return 0;
    parse_uint(p, &task->system_ticks);

    return 0;
}

static void enrich_from_status(top_task_t *task)
{
    char path[64];
    char buf[1024];
    const char *p;

    if (task->pid <= 0)
        return;

    sprintf(path, "/proc/%d/status", task->pid);
    if (read_file(path, buf, sizeof(buf)) < 0)
        return;

    p = line_after_key(buf, "VmRSS:");
    if (p)
        parse_uint(p, &task->rss_kb);

    p = line_after_key(buf, "Priority:");
    if (p)
        parse_uint(p, &task->priority);

    p = line_after_key(buf, "DebtScore:");
    if (p)
        parse_uint(p, &task->debt_score);

    p = line_after_key(buf, "CPU:");
    if (p)
        parse_int(p, &task->cpu);

    p = line_after_key(buf, "State:");
    if (p) {
        const char *open = strchr(p, '(');
        const char *close = open ? strchr(open + 1, ')') : NULL;
        int len;

        if (open && close && close > open + 1) {
            len = (int)(close - open - 1);
            if (len >= (int)sizeof(task->state_detail))
                len = (int)sizeof(task->state_detail) - 1;
            memcpy(task->state_detail, open + 1, (size_t)len);
            task->state_detail[len] = '\0';
        }
    }
}

static int load_tasks(top_task_t *tasks, int max_tasks)
{
    char statbuf[512];
    int fd;
    int n;
    int count = 0;

    fd = open("/proc", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return 0;

    while ((n = getdents(fd, top_proc_dirbuf, TOP_PROC_BUF)) > 0 && count < max_tasks) {
        char *ptr = top_proc_dirbuf;

        while (ptr < top_proc_dirbuf + n && count < max_tasks) {
            struct linux_dirent *e = (struct linux_dirent *)ptr;
            char path[64];

            if (e->d_reclen == 0)
                break;

            if (e->d_ino != 0 && is_digit(e->d_name[0])) {
                memset(&tasks[count], 0, sizeof(tasks[count]));
                tasks[count].tty = -1;
                tasks[count].cpu = -1;
                tasks[count].priority = 10;
                sprintf(path, "/proc/%s/stat", e->d_name);
                if (read_file(path, statbuf, sizeof(statbuf)) >= 0 &&
                    parse_proc_stat(statbuf, &tasks[count]) == 0) {
                    enrich_from_status(&tasks[count]);
                    count++;
                }
            }

            ptr += e->d_reclen;
        }
    }

    close(fd);
    return count;
}

static int find_prev_runtime(int pid, top_sample_t *sample)
{
    for (int i = 0; i < top_prev_count; i++) {
        if (top_prev_samples[i].pid == pid) {
            *sample = top_prev_samples[i];
            return 1;
        }
    }

    return 0;
}

static void update_cpu_percent(top_task_t *tasks, int count, unsigned delay_sec)
{
    unsigned elapsed_ticks = top_cpu_elapsed_ticks;

    if (elapsed_ticks == 0)
        elapsed_ticks = delay_sec ? delay_sec * TOP_HZ : TOP_HZ;

    for (int i = 0; i < count; i++) {
        top_sample_t previous = {0};

        tasks[i].cpu_pct_x10 = 0;
        tasks[i].user_pct_x10 = 0;
        tasks[i].system_pct_x10 = 0;
        if (top_have_prev && find_prev_runtime(tasks[i].pid, &previous)) {
            unsigned runtime_delta = tasks[i].runtime_ticks -
                                     previous.runtime_ticks;
            unsigned user_delta = tasks[i].user_ticks - previous.user_ticks;
            unsigned system_delta = tasks[i].system_ticks -
                                    previous.system_ticks;

            tasks[i].cpu_pct_x10 = (runtime_delta * 1000u) / elapsed_ticks;
            tasks[i].user_pct_x10 = (user_delta * 1000u) / elapsed_ticks;
            tasks[i].system_pct_x10 = (system_delta * 1000u) / elapsed_ticks;
        }
    }

    top_prev_count = count > TOP_MAX_TASKS ? TOP_MAX_TASKS : count;
    for (int i = 0; i < top_prev_count; i++) {
        top_prev_samples[i].pid = tasks[i].pid;
        top_prev_samples[i].runtime_ticks = tasks[i].runtime_ticks;
        top_prev_samples[i].user_ticks = tasks[i].user_ticks;
        top_prev_samples[i].system_ticks = tasks[i].system_ticks;
    }
    top_have_prev = 1;
}

static int compare_tasks(const void *a, const void *b)
{
    const top_task_t *ta = (const top_task_t *)a;
    const top_task_t *tb = (const top_task_t *)b;

    if (tb->cpu_pct_x10 != ta->cpu_pct_x10)
        return tb->cpu_pct_x10 > ta->cpu_pct_x10 ? 1 : -1;
    if (tb->runtime_ticks != ta->runtime_ticks)
        return tb->runtime_ticks > ta->runtime_ticks ? 1 : -1;
    if (tb->ctx != ta->ctx)
        return tb->ctx > ta->ctx ? 1 : -1;
    return ta->pid - tb->pid;
}

static void format_runtime(unsigned ticks, char *buf, int size)
{
    unsigned centis = (ticks * 100u) / TOP_HZ;
    unsigned minutes = centis / 6000u;
    unsigned seconds = (centis / 100u) % 60u;
    unsigned hundredths = centis % 100u;

    snprintf(buf, (size_t)size, "%u:%02u.%02u", minutes, seconds, hundredths);
}

static void format_count(unsigned value, char *buf, int size)
{
    if (value >= 1000000u) {
        unsigned whole = value / 1000000u;
        unsigned dec = (value % 1000000u) / 100000u;
        if (dec)
            snprintf(buf, (size_t)size, "%u.%uM", whole, dec);
        else
            snprintf(buf, (size_t)size, "%uM", whole);
    } else if (value >= 1000u) {
        unsigned whole = value / 1000u;
        unsigned dec = (value % 1000u) / 100u;
        if (dec)
            snprintf(buf, (size_t)size, "%u.%uK", whole, dec);
        else
            snprintf(buf, (size_t)size, "%uK", whole);
    } else {
        snprintf(buf, (size_t)size, "%u", value);
    }
}

static const char *state_name(const top_task_t *task)
{
    switch (task->state) {
    case 'R':
        if (strcmp(task->state_detail, "ready") == 0)
            return "ready";
        return "run";
    case 'Z': return "zombie";
    case 'T': return "term";
    case 't': return "stop";
    case 'D': return "wait";
    default:  return "sleep";
    }
}

static const char *state_color(const top_task_t *task)
{
    switch (task->state) {
    case 'R':
        if (strcmp(task->state_detail, "ready") == 0)
            return C_YELLOW;
        return C_GREEN;
    case 'Z': return C_RED;
    case 'T':
    case 't': return C_YELLOW;
    case 'D': return C_MAGENTA;
    default:  return C_CYAN;
    }
}

static void append_tty(top_buf_t *buf, int tty)
{
    if (tty >= 0)
        top_buf_printf(buf, "tty%d", tty);
    else
        top_buf_append(buf, "?", 1);
}

static void append_cpu(top_buf_t *buf, int cpu)
{
    if (cpu >= 0)
        top_buf_printf(buf, "%3d", cpu);
    else
        top_buf_append(buf, "  -", 3);
}

static void read_terminal_size(unsigned *rows, unsigned *cols)
{
    struct winsize size;

    *rows = TOP_DEFAULT_ROWS;
    *cols = TOP_DEFAULT_COLS;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_row > 0)
            *rows = size.ws_row;
        if (size.ws_col > 0)
            *cols = size.ws_col;
    }
}

static void append_rule(top_buf_t *buf, unsigned cols)
{
    static const char rule[] =
        "--------------------------------------------------------------------------------"
        "------------------------------------------------";
    unsigned width = cols > 2u ? cols - 2u : cols;

    if (width > sizeof(rule) - 1u)
        width = sizeof(rule) - 1u;
    top_buf_append(buf, C_DIM, sizeof(C_DIM) - 1);
    top_buf_append(buf, rule, (int)width);
    top_buf_append(buf, C_RESET "\033[0K\r\n",
                   sizeof(C_RESET "\033[0K\r\n") - 1);
}

static void append_task_compact(top_buf_t *buf, const top_task_t *task,
                                unsigned cols)
{
    char timebuf[16];
    char ctxbuf[16];
    const char *color = state_color(task);
    int name_width = cols > 53u ? (int)(cols - 53u) : 1;

    format_runtime(task->runtime_ticks, timebuf, sizeof(timebuf));
    format_count(task->ctx, ctxbuf, sizeof(ctxbuf));
    top_buf_printf(buf, C_CYAN "%5d" C_RESET " ", task->pid);
    if (task->tty >= 0)
        top_buf_printf(buf, "tty%-8d", task->tty);
    else
        top_buf_append(buf, "?   ", 4);
    top_buf_printf(buf, " %s%-5s" C_RESET " ", color, state_name(task));
    append_cpu(buf, task->cpu);
    top_buf_printf(buf, " %3u %3u.%u %7s %6s %5uK %.*s\033[0K\r\n",
                   task->priority,
                   task->cpu_pct_x10 / 10u,
                   task->cpu_pct_x10 % 10u,
                   timebuf,
                   ctxbuf,
                   task->rss_kb,
                   name_width,
                   task->name);
}

static void render_top(unsigned delay_sec, int iteration)
{
    top_buf_t frame = {
        .data = top_frame_data,
        .len = 0,
        .cap = sizeof(top_frame_data),
    };
    top_mem_t mem;
    unsigned used_kb;
    unsigned pct_x10;
    unsigned cpu_total_x10 = 0;
    unsigned cpu_average_x100;
    unsigned cpu_user_average_x10 = 0;
    unsigned cpu_system_average_x10 = 0;
    unsigned cpu_irq_average_x10 = 0;
    unsigned cpu_idle_average_x10 = 0;
    unsigned terminal_rows;
    unsigned terminal_cols;
    int visible_count;
    int full_width;
    int count;

    read_mem(&mem);
    top_cpu_count = read_cpu_count();
    update_cpu_accounting();
    count = load_tasks(top_tasks, TOP_MAX_TASKS);
    update_cpu_percent(top_tasks, count, delay_sec);
    for (unsigned cpu = 0; cpu < top_cpu_count; cpu++) {
        cpu_total_x10 += top_cpu_load_x10[cpu];
        cpu_user_average_x10 += top_cpu_user_x10[cpu];
        cpu_system_average_x10 += top_cpu_system_x10[cpu];
        cpu_irq_average_x10 += top_cpu_irq_x10[cpu];
        cpu_idle_average_x10 += top_cpu_idle_x10[cpu];
    }
    cpu_user_average_x10 /= top_cpu_count;
    cpu_system_average_x10 /= top_cpu_count;
    cpu_irq_average_x10 /= top_cpu_count;
    cpu_idle_average_x10 /= top_cpu_count;
    cpu_average_x100 = (cpu_total_x10 * 10u + top_cpu_count / 2u) /
                       top_cpu_count;
    qsort(top_tasks, (size_t)count, sizeof(top_tasks[0]), compare_tasks);
    read_terminal_size(&terminal_rows, &terminal_cols);
    visible_count = terminal_rows > TOP_HEADER_ROWS ?
                    (int)(terminal_rows - TOP_HEADER_ROWS) : 1;
    if (visible_count > count)
        visible_count = count;
    full_width = terminal_cols >= TOP_FULL_WIDTH;

    used_kb = mem.total_kb >= mem.free_kb ? mem.total_kb - mem.free_kb : 0;
    pct_x10 = mem.total_kb ? (used_kb * 1000u / mem.total_kb) : 0;

    top_buf_append(&frame, "\033[?2026h\033[?25l\033[H",
                   sizeof("\033[?2026h\033[?25l\033[H") - 1);
    top_buf_printf(&frame, C_BOLD C_CYAN "ArmOS mtop" C_RESET " - refresh %us", delay_sec);
    if (iteration >= 0)
        top_buf_printf(&frame, " - iteration %d", iteration + 1);
    top_buf_printf(&frame,
                   " - CPU " C_GREEN "%u.%02u%%" C_RESET " - " C_DIM "q quit, +/- delay" C_RESET "\033[0K\r\n",
                   cpu_average_x100 / 100u,
                   cpu_average_x100 % 100u);
    top_buf_printf(&frame, C_BOLD "CPU:" C_RESET " total " C_GREEN "%u.%u%%" C_RESET,
                   cpu_total_x10 / 10u,
                   cpu_total_x10 % 10u);
    for (unsigned cpu = 0; cpu < top_cpu_count; cpu++) {
        top_buf_printf(&frame, " cpu%u " C_GREEN "%u.%u%%" C_RESET,
                       cpu,
                       top_cpu_load_x10[cpu] / 10u,
                       top_cpu_load_x10[cpu] % 10u);
    }
    top_buf_append(&frame, "\033[0K\r\n", 6);
    top_buf_printf(&frame,
                   C_BOLD "Mode:" C_RESET
                   " user " C_GREEN "%u.%u%%" C_RESET
                   " system " C_YELLOW "%u.%u%%" C_RESET
                   " irq " C_MAGENTA "%u.%u%%" C_RESET
                   " idle %u.%u%%\033[0K\r\n",
                   cpu_user_average_x10 / 10u,
                   cpu_user_average_x10 % 10u,
                   cpu_system_average_x10 / 10u,
                   cpu_system_average_x10 % 10u,
                   cpu_irq_average_x10 / 10u,
                   cpu_irq_average_x10 % 10u,
                   cpu_idle_average_x10 / 10u,
                   cpu_idle_average_x10 % 10u);
    top_buf_printf(&frame,
                   C_BOLD "Mem:" C_RESET " %uM total, " C_GREEN "%uM free" C_RESET
                   ", " C_YELLOW "%u.%u%% used" C_RESET ", tasks: %d\033[0K\r\n\033[0K\r\n",
                   mem.total_kb / 1024u,
                   mem.free_kb / 1024u,
                   pct_x10 / 10u,
                   pct_x10 % 10u,
                   count);

    if (full_width) {
        top_buf_printf(&frame, C_BOLD "%5s %-8s %-8s %4s %3s %5s %5s %5s %5s %8s %8s %6s %6s %s" C_RESET "\033[0K\r\n",
                       "PID", "TTY", "STATE", "LAST", "PRI", "DEBT",
                       "%CPU", "%USR", "%SYS", "TIME", "CTX", "PF", "RSS", "CMD");
    } else {
        top_buf_printf(&frame, C_BOLD "%5s %-8s %-8s %5s %3s %5s %8s %6s %6s %s" C_RESET "\033[0K\r\n",
                       "PID", "TTY", "STATE", "CPU", "PRI", "%CPU", "TIME", "CTX", "RSS", "CMD");
    }
    append_rule(&frame, terminal_cols);

    for (int i = 0; i < visible_count; i++) {
        char timebuf[16];
        char ctxbuf[16];
        const char *color = state_color(&top_tasks[i]);

        if (!full_width) {
            append_task_compact(&frame, &top_tasks[i], terminal_cols);
            continue;
        }

        format_runtime(top_tasks[i].runtime_ticks, timebuf, sizeof(timebuf));
        format_count(top_tasks[i].ctx, ctxbuf, sizeof(ctxbuf));
        top_buf_printf(&frame, C_CYAN "%5d" C_RESET " ", top_tasks[i].pid);
        append_tty(&frame, top_tasks[i].tty);
        top_buf_printf(&frame, "%*s %s%-8s" C_RESET " ",
                       top_tasks[i].tty >= 0 ? 8 : 8,
                       "",
                       color,
                       state_name(&top_tasks[i]));
        append_cpu(&frame, top_tasks[i].cpu);
        top_buf_printf(&frame,
                       " %5u %8u %3u.%u %3u.%u %3u.%u %8s %8s %6u %5uK %s\033[0K\r\n",
                       top_tasks[i].priority,
                       top_tasks[i].debt_score,
                       top_tasks[i].cpu_pct_x10 / 10u,
                       top_tasks[i].cpu_pct_x10 % 10u,
                       top_tasks[i].user_pct_x10 / 10u,
                       top_tasks[i].user_pct_x10 % 10u,
                       top_tasks[i].system_pct_x10 / 10u,
                       top_tasks[i].system_pct_x10 % 10u,
                       timebuf,
                       ctxbuf,
                       top_tasks[i].pf,
                       top_tasks[i].rss_kb,
                       top_tasks[i].name);
    }

    top_buf_append(&frame, "\033[J\033[?2026l",
                   sizeof("\033[J\033[?2026l") - 1);
    if (frame.data && frame.len > 0)
        top_write_all(STDOUT_FILENO, frame.data, (size_t)frame.len);
    top_buf_free(&frame);
}

static void top_delay(unsigned *delay_sec)
{
    if (*delay_sec == 0)
        return;

    sleep(*delay_sec);

    if (top_termios_saved)
        top_set_raw_poll();
}

static int parse_args(int argc, char **argv, unsigned *delay_sec, int *count)
{
    *delay_sec = 2;
    *count = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            int value = atoi(argv[++i]);
            if (value <= 0)
                return -1;
            *delay_sec = (unsigned)value;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            int value = atoi(argv[++i]);
            if (value <= 0)
                return -1;
            *count = value;
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            return 1;
        } else {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    unsigned delay_sec;
    int max_count;
    int parsed;
    int iteration = 0;

    parsed = parse_args(argc, argv, &delay_sec, &max_count);
    if (parsed != 0) {
        printf("usage: mtop [-s seconds] [-n count]\n");
        return parsed < 0 ? 1 : 0;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    top_enter_screen();
    top_enable_interactive();

    while (top_running && (max_count < 0 || iteration < max_count)) {
        poll_input(&delay_sec);
        render_top(delay_sec, max_count >= 0 ? iteration : -1);
        iteration++;
        if (!top_running || (max_count >= 0 && iteration >= max_count))
            break;
        top_delay(&delay_sec);
    }

    top_restore_terminal();
    top_leave_screen();
    return 0;
}
