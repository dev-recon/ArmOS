/*
 * FreeBSD top machine backend for ArmOS.
 *
 * The frontend is the pinned FreeBSD implementation.  This backend consumes
 * only the architecture-neutral ArmOS /proc ABI and POSIX/newlib services.
 */
#include "armos_top_compat.h"

#include <stdio.h>

#include "layout.h"
#include "machine.h"
#include "screen.h"
#include "top.h"
#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define ARMOS_TOP_TICK_HZ 1000UL
#define ARMOS_TOP_INITIAL_PROCS 64

enum process_state {
    PS_EMPTY,
    PS_START,
    PS_RUN,
    PS_SLEEP,
    PS_STOP,
    PS_ZOMBIE,
    PS_WAIT,
    PS_LOCK
};

struct armos_process {
    pid_t pid;
    pid_t ppid;
    uid_t uid;
    int tty;
    int priority;
    int cpu;
    int state;
    uint64_t vm_kb;
    uint64_t rss_kb;
    uint64_t runtime_ticks;
    uint64_t context_switches;
    uint64_t page_faults;
    unsigned int cpu_tenths;
    char state_text[8];
    char name[64];
    char command[256];
};

struct previous_runtime {
    pid_t pid;
    uint64_t ticks;
};

struct handle {
    struct armos_process **next;
    int remaining;
};

typedef int compare_fn(const void *, const void *);
struct sort_info {
    const char *si_name;
    compare_fn *si_compare;
};

static const char *const process_state_names[] = {
    "", " starting, ", " running, ", " sleeping, ", " stopped, ",
    " zombie, ", " waiting, ", " locked, ", NULL
};
static const char *const cpu_state_names[] = {
    "user", "nice", "system", "interrupt", "idle", NULL
};
static const char *const memory_names[] = {
    "K Total, ", "K Used, ", "K Free", NULL
};

static int process_states[8];
static int memory_stats[7];
static int aggregate_cpu_states[CPUSTATES];
static int *per_cpu_states;
static long aggregate_current[CPUSTATES];
static long aggregate_previous[CPUSTATES];
static long aggregate_difference[CPUSTATES];
static long *per_cpu_current;
static long *per_cpu_previous;
static long *per_cpu_difference;
static struct armos_process *processes;
static struct armos_process **process_order;
static size_t process_capacity;
static size_t process_count;
static struct previous_runtime *previous;
static size_t previous_count;
static size_t previous_capacity;
static struct handle process_handle;
static uint64_t sample_ticks;
static uint64_t previous_sample_ticks;
static int cpu_count = 1;

enum displaymodes displaymode = DISP_CPU;

static int compare_cpu(const void *, const void *);
static int compare_size(const void *, const void *);
static int compare_res(const void *, const void *);
static int compare_time(const void *, const void *);
static int compare_priority(const void *, const void *);
static int compare_pid(const void *, const void *);

static const struct sort_info sort_data[] = {
    { "cpu", compare_cpu },
    { "size", compare_size },
    { "res", compare_res },
    { "time", compare_time },
    { "pri", compare_priority },
    { "pid", compare_pid },
    { NULL, NULL }
};

static int
ensure_process_capacity(size_t required)
{
    size_t capacity;
    struct armos_process *new_processes;
    struct armos_process **new_order;

    if (required <= process_capacity)
        return 0;
    capacity = process_capacity == 0 ? ARMOS_TOP_INITIAL_PROCS : process_capacity;
    while (capacity < required)
        capacity *= 2;
    new_processes = realloc(processes, capacity * sizeof(*processes));
    if (new_processes == NULL)
        return -1;
    processes = new_processes;
    new_order = realloc(process_order, capacity * sizeof(*process_order));
    if (new_order == NULL)
        return -1;
    process_order = new_order;
    process_capacity = capacity;
    return 0;
}

static uint64_t
previous_ticks(pid_t pid)
{
    size_t index;
    for (index = 0; index < previous_count; index++)
        if (previous[index].pid == pid)
            return previous[index].ticks;
    return 0;
}

static int
save_previous_processes(void)
{
    size_t index;
    if (process_count > previous_capacity) {
        struct previous_runtime *items = realloc(previous,
            process_count * sizeof(*items));
        if (items == NULL)
            return -1;
        previous = items;
        previous_capacity = process_count;
    }
    previous_count = process_count;
    for (index = 0; index < process_count; index++) {
        previous[index].pid = processes[index].pid;
        previous[index].ticks = processes[index].runtime_ticks;
    }
    return 0;
}

static int
read_uptime_ticks(uint64_t *ticks)
{
    FILE *file = fopen("/proc/uptime", "r");
    unsigned long seconds;
    unsigned long hundredths;
    int result;

    if (file == NULL)
        return -1;
    result = fscanf(file, "%lu.%lu", &seconds, &hundredths);
    fclose(file);
    if (result < 1)
        return -1;
    if (result < 2)
        hundredths = 0;
    *ticks = (uint64_t)seconds * ARMOS_TOP_TICK_HZ +
        (uint64_t)(hundredths % 100) * (ARMOS_TOP_TICK_HZ / 100);
    return 0;
}

static int
parse_cpu_line(const char *line, long values[CPUSTATES])
{
    char label[16];
    unsigned long user, nice, system, idle, wait, irq, softirq;

    if (sscanf(line, "%15s %lu %lu %lu %lu %lu %lu %lu",
        label, &user, &nice, &system, &idle, &wait, &irq, &softirq) != 8)
        return -1;
    values[0] = (long)user;
    values[1] = (long)nice;
    values[2] = (long)system;
    values[3] = (long)(irq + softirq);
    values[4] = (long)(idle + wait);
    return 0;
}

static void
read_cpu_stats(void)
{
    char line[256];
    FILE *file = fopen("/proc/stat", "r");
    int cpu = 0;

    if (file == NULL)
        return;
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strncmp(line, "cpu ", 4) == 0) {
            parse_cpu_line(line, aggregate_current);
        } else if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
            if (cpu < cpu_count)
                parse_cpu_line(line, &per_cpu_current[cpu * CPUSTATES]);
            cpu++;
        }
    }
    fclose(file);
    percentages(CPUSTATES, aggregate_cpu_states, aggregate_current,
        aggregate_previous, aggregate_difference);
    for (cpu = 0; cpu < cpu_count; cpu++)
        percentages(CPUSTATES, &per_cpu_states[cpu * CPUSTATES],
            &per_cpu_current[cpu * CPUSTATES],
            &per_cpu_previous[cpu * CPUSTATES],
            &per_cpu_difference[cpu * CPUSTATES]);
}

static void
read_memory_stats(void)
{
    FILE *file = fopen("/proc/meminfo", "r");
    char name[32];
    unsigned long value;
    unsigned long total = 0;
    unsigned long free_kb = 0;

    if (file == NULL)
        return;
    while (fscanf(file, "%31[^:]: %lu kB\n", name, &value) == 2) {
        if (strcmp(name, "MemTotal") == 0)
            total = value;
        else if (strcmp(name, "MemAvailable") == 0)
            free_kb = value;
        else if (strcmp(name, "MemFree") == 0 && free_kb == 0)
            free_kb = value;
    }
    fclose(file);
    memory_stats[0] = (int)total;
    memory_stats[1] = (int)(total >= free_kb ? total - free_kb : 0);
    memory_stats[2] = (int)free_kb;
}

static int
state_index(char state)
{
    switch (state) {
    case 'R': return PS_RUN;
    case 'S': return PS_SLEEP;
    case 'D': return PS_LOCK;
    case 'T': return PS_STOP;
    case 'Z': return PS_ZOMBIE;
    default: return PS_WAIT;
    }
}

static void
parse_status_value(struct armos_process *process, const char *line)
{
    unsigned long long value;
    int number;
    char state;

    if (sscanf(line, "Name:\t%63[^\n]", process->name) == 1)
        return;
    if (sscanf(line, "State:\t%c", &state) == 1) {
        process->state = state_index(state);
        snprintf(process->state_text, sizeof(process->state_text), "%s",
            state == 'R' ? "RUN" : state == 'S' ? "SLEEP" :
            state == 'D' ? "LOCK" : state == 'T' ? "STOP" :
            state == 'Z' ? "ZOMB" : "WAIT");
        return;
    }
    if (sscanf(line, "PPid:\t%d", &number) == 1) process->ppid = number;
    else if (sscanf(line, "Tty:\t%d", &number) == 1) process->tty = number;
    else if (sscanf(line, "Uid:\t%d", &number) == 1) process->uid = (uid_t)number;
    else if (sscanf(line, "Priority:\t%d", &number) == 1) process->priority = number;
    else if (sscanf(line, "CPU:\t%d", &number) == 1) process->cpu = number;
    else if (sscanf(line, "VmSize:\t%llu kB", &value) == 1) process->vm_kb = value;
    else if (sscanf(line, "VmRSS:\t%llu kB", &value) == 1) process->rss_kb = value;
    else if (sscanf(line, "RuntimeTicks:\t%llu", &value) == 1) process->runtime_ticks = value;
    else if (sscanf(line, "CtxSwitches:\t%llu", &value) == 1) process->context_switches = value;
    else if (sscanf(line, "PageFaults:\t%llu", &value) == 1) process->page_faults = value;
}

static int
read_process(pid_t pid, struct armos_process *process)
{
    char path[48];
    char line[320];
    FILE *file;

    memset(process, 0, sizeof(*process));
    process->pid = pid;
    process->tty = -1;
    process->cpu = -1;
    process->state = PS_WAIT;
    strcpy(process->state_text, "WAIT");
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    file = fopen(path, "r");
    if (file == NULL)
        return -1;
    while (fgets(line, sizeof(line), file) != NULL)
        parse_status_value(process, line);
    fclose(file);

    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    file = fopen(path, "r");
    if (file != NULL) {
        size_t length = fread(process->command, 1,
            sizeof(process->command) - 1, file);
        fclose(file);
        process->command[length] = '\0';
        for (size_t index = 0; index < length; index++)
            if (process->command[index] == '\0')
                process->command[index] = ' ';
    }
    if (process->command[0] == '\0')
        snprintf(process->command, sizeof(process->command), "%s", process->name);
    return 0;
}

static int
selected(const struct armos_process *process, const struct process_select *select)
{
    size_t index;
    int uid_match = select->uid[0] == -1;

    if (!select->self && process->pid == mypid)
        return 0;
    if (select->pid != -1 && process->pid != select->pid)
        return 0;
    if (!select->idle && process->cpu_tenths == 0 && process->state != PS_RUN)
        return 0;
    if (select->command != NULL && strstr(process->command, select->command) == NULL)
        return 0;
    for (index = 0; index < TOP_MAX_UIDS; index++)
        if (select->uid[index] == (int)process->uid)
            uid_match = 1;
    return uid_match;
}

static int
load_processes(void)
{
    FILE *file;
    char line[256];
    uint64_t interval = sample_ticks > previous_sample_ticks ?
        sample_ticks - previous_sample_ticks : 1;

    save_previous_processes();
    process_count = 0;
    memset(process_states, 0, sizeof(process_states));
    file = fopen("/proc/tasks", "r");
    if (file == NULL)
        return -1;
    (void)fgets(line, sizeof(line), file);
    while (fgets(line, sizeof(line), file) != NULL) {
        int pid, ppid, priority;
        unsigned int tid, context, faults, cow, stack;
        char state, kind, name[64];
        size_t index;
        int duplicate = 0;

        if (sscanf(line, "%d %u %d %c %c %d %u %u %u %u %63s",
            &pid, &tid, &ppid, &state, &kind, &priority, &context,
            &faults, &cow, &stack, name) != 11 || kind != 'P' || pid <= 0)
            continue;
        for (index = 0; index < process_count; index++)
            if (processes[index].pid == pid)
                duplicate = 1;
        if (duplicate)
            continue;
        if (ensure_process_capacity(process_count + 1) != 0)
            break;
        if (read_process(pid, &processes[process_count]) != 0)
            continue;
        {
            uint64_t old = previous_ticks(pid);
            uint64_t delta = processes[process_count].runtime_ticks >= old ?
                processes[process_count].runtime_ticks - old : 0;
            unsigned long long tenths = delta * 1000ULL / interval;
            if (tenths > 1000)
                tenths = 1000;
            processes[process_count].cpu_tenths = (unsigned int)tenths;
        }
        process_states[processes[process_count].state]++;
        process_count++;
    }
    fclose(file);

    previous_sample_ticks = sample_ticks;
    return 0;
}

static int compare_desc_u64(uint64_t left, uint64_t right)
{
    return left < right ? 1 : left > right ? -1 : 0;
}

static int compare_cpu(const void *left, const void *right)
{
    const struct armos_process *a = *(const struct armos_process *const *)left;
    const struct armos_process *b = *(const struct armos_process *const *)right;
    int result = compare_desc_u64(a->cpu_tenths, b->cpu_tenths);
    return result != 0 ? result : compare_desc_u64(a->runtime_ticks, b->runtime_ticks);
}
static int compare_size(const void *l, const void *r)
{
    return compare_desc_u64((*(const struct armos_process *const *)l)->vm_kb,
        (*(const struct armos_process *const *)r)->vm_kb);
}
static int compare_res(const void *l, const void *r)
{
    return compare_desc_u64((*(const struct armos_process *const *)l)->rss_kb,
        (*(const struct armos_process *const *)r)->rss_kb);
}
static int compare_time(const void *l, const void *r)
{
    return compare_desc_u64((*(const struct armos_process *const *)l)->runtime_ticks,
        (*(const struct armos_process *const *)r)->runtime_ticks);
}
static int compare_priority(const void *l, const void *r)
{
    const struct armos_process *a = *(const struct armos_process *const *)l;
    const struct armos_process *b = *(const struct armos_process *const *)r;
    return a->priority - b->priority;
}
static int compare_pid(const void *l, const void *r)
{
    const struct armos_process *a = *(const struct armos_process *const *)l;
    const struct armos_process *b = *(const struct armos_process *const *)r;
    return a->pid < b->pid ? -1 : a->pid > b->pid ? 1 : 0;
}

static void
update_layout(void)
{
    int extra = pcpu_stats ? cpu_count - 1 : 0;
    y_mem = 3 + extra;
    y_idlecursor = 4 + extra;
    y_message = 4 + extra;
    y_header = 5 + extra;
    y_procs = 6 + extra;
    Header_lines = 6 + extra;
}

int
machine_init(struct statics *statics)
{
    FILE *file;
    char line[256];
    int detected = 0;

    file = fopen("/proc/stat", "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL)
            if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3]))
                detected++;
        fclose(file);
    }
    if (detected > 0)
        cpu_count = detected;
    per_cpu_states = calloc((size_t)cpu_count * CPUSTATES, sizeof(int));
    per_cpu_current = calloc((size_t)cpu_count * CPUSTATES, sizeof(long));
    per_cpu_previous = calloc((size_t)cpu_count * CPUSTATES, sizeof(long));
    per_cpu_difference = calloc((size_t)cpu_count * CPUSTATES, sizeof(long));
    if (per_cpu_states == NULL || per_cpu_current == NULL ||
        per_cpu_previous == NULL || per_cpu_difference == NULL)
        return -1;

    statics->procstate_names = process_state_names;
    statics->cpustate_names = cpu_state_names;
    statics->memory_names = memory_names;
    statics->arc_names = NULL;
    statics->carc_names = NULL;
    statics->swap_names = NULL;
    statics->order_names = NULL;
    statics->nbatteries = 0;
    statics->ncpus = cpu_count;
    update_layout();
    return 0;
}

void
toggle_pcpustats(void)
{
    if (cpu_count > 1)
        update_layout();
}

void
get_system_info(struct system_info *info)
{
    uint64_t uptime = 0;
    FILE *loadavg;

    if (read_uptime_ticks(&uptime) == 0)
        sample_ticks = uptime;
    read_cpu_stats();
    read_memory_stats();
    memset(info, 0, sizeof(*info));
    info->last_pid = -1;
    info->procstates = process_states;
    info->cpustates = pcpu_stats ? per_cpu_states : aggregate_cpu_states;
    info->memory = memory_stats;
    info->ncpus = cpu_count;
    loadavg = fopen("/proc/loadavg", "r");
    if (loadavg != NULL) {
        (void)fscanf(loadavg, "%lf %lf %lf", &info->load_avg[0],
            &info->load_avg[1], &info->load_avg[2]);
        fclose(loadavg);
    }
    gettimeofday(&info->boottime, NULL);
    info->boottime.tv_sec -= (time_t)(uptime / ARMOS_TOP_TICK_HZ);
}

void *
get_process_info(struct system_info *info, struct process_select *select,
    const struct sort_info *sort)
{
    size_t index;
    size_t active = 0;

    load_processes();
    for (index = 0; index < process_count; index++)
        if (selected(&processes[index], select))
            process_order[active++] = &processes[index];
    qsort(process_order, active, sizeof(*process_order), sort->si_compare);
    info->p_total = (int)process_count;
    info->p_pactive = (int)active;
    process_handle.next = process_order;
    process_handle.remaining = (int)active;
    return &process_handle;
}

char *
format_header(const char *user_field)
{
    static char header[160];
    snprintf(header, sizeof(header),
        "  PID %-10.10s PRI    SIZE    RES STATE  C   TIME    CPU COMMAND",
        user_field);
    return header;
}

char *
format_next_process(struct handle *handle, char *(*get_userid)(int), int flags)
{
    static char line[512];
    struct armos_process *process;
    const char *command;

    if (handle == NULL || handle->remaining <= 0)
        return NULL;
    process = *handle->next++;
    handle->remaining--;
    command = (flags & FMT_SHOWARGS) ? process->command : process->name;
    snprintf(line, sizeof(line),
        "%5d %-10.10s %3d %7s %6s %-6.6s %2d %6s %6.2f%% %s",
        (int)process->pid, get_userid((int)process->uid), process->priority,
        format_k((int64_t)process->vm_kb), format_k((int64_t)process->rss_kb),
        process->state_text, process->cpu,
        format_time((long)(process->runtime_ticks / ARMOS_TOP_TICK_HZ)),
        process->cpu_tenths / 10.0, command);
    return line;
}

const struct sort_info *
get_sort_info(const char *name)
{
    const struct sort_info *sort;
    if (name == NULL)
        return &sort_data[0];
    for (sort = sort_data; sort->si_name != NULL; sort++)
        if (strcmp(sort->si_name, name) == 0)
            return sort;
    return NULL;
}

void
dump_sort_names(FILE *file)
{
    const struct sort_info *sort;
    for (sort = sort_data; sort->si_name != NULL; sort++)
        fprintf(file, "%s%s", sort == sort_data ? "" : " ", sort->si_name);
    fputc('\n', file);
}
