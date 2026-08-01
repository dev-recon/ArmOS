/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/task.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_TASK_H
#define _KERNEL_TASK_H

#include <kernel/types.h>
#include <kernel/memory.h>
#include <kernel/spinlock.h>
#include <kernel/stddef.h>
#include <kernel/arch_irq.h>
#include <kernel/arch_task.h>

struct signal_state_t;
struct file;

/* Configuration */
#define KERNEL_TASK_STACK_SIZE        (32 * 1024) /* 32 KiB usable per task */
#define KERNEL_TASK_STACK_GUARD_SIZE  (4 * 1024)  /* Canary guard below stack */
#define MAX_TASKS               1024             /* Maximum de taches vivantes */
#define TASK_NAME_MAX           32              /* Longueur max du nom */
#define MAX_SIGNALS             32
#define PROC_CMDLINE_MAX        512
#define PROC_ENVIRON_MAX        512
#define TASK_PRIORITY_LEVELS    256
#define TASK_DEFAULT_PRIORITY   10
#define TASK_IDLE_PRIORITY      (TASK_PRIORITY_LEVELS - 1)
#define TASK_NICE_MIN           (-20)
#define TASK_NICE_MAX           19
#define TASK_CPU_NONE           0xffffffffu
#define SCHED_POLICY_NAME       "priority-rr-debt"
#define SCHED_AGING_STEP_TICKS  20
#define SCHED_AGING_MAX_BONUS   8
#define SCHED_DEBT_DECAY_TICKS  4

#define SCHED_PICK_NONE         0
#define SCHED_PICK_PRIORITY     1
#define SCHED_PICK_AGING        2
#define SCHED_PICK_DEBT         3
#define SCHED_PICK_WAIT         4

#define QUANTUM_TICKS           10

typedef struct kernel_lifecycle_stats {
    uint32_t tasks_created;
    uint32_t tasks_destroyed;
    uint32_t zombies_created;
    uint32_t zombies_reaped;
    uint32_t failed_forks;
    uint32_t scheduler_refused;
    uint32_t scheduler_critical_repaired;
    uint32_t ready_queue_refused;
    uint32_t stack_pages_allocated;
    uint32_t stack_pages_freed;
    uint32_t asid_rollovers;
    uint32_t state_sync_repairs;
    uint32_t blocked_signal_wakeups;
    uint32_t tty_stale_waiters;
    uint32_t fs_wait_timeouts;
    uint32_t sleep_deadline_wakeups;
    uint32_t sleep_overshoots;
} kernel_lifecycle_stats_t;

extern volatile kernel_lifecycle_stats_t kernel_lifecycle_stats;

#define SCHED_TRACE_SIZE 64
#define TASK_MAGIC_ALIVE 0x5441534bu  /* "TASK" */
#define TASK_MAGIC_DEAD  0xdead7a5bu

typedef enum sched_trace_event_type {
    SCHED_TRACE_FS_WAIT_TIMEOUT = 1,
    SCHED_TRACE_REFUSE_CRITICAL,
    SCHED_TRACE_REFUSE_BROKEN_LIST,
    SCHED_TRACE_REFUSE_NULL_NEXT,
    SCHED_TRACE_REFUSE_INVALID_TASK,
    SCHED_TRACE_REFUSE_LOOP,
    SCHED_TRACE_READY_REFUSE_DEAD,
    SCHED_TRACE_READY_REFUSE_CORRUPT,
    SCHED_TRACE_READY_REFUSE_REMOTE_RUNNING,
    SCHED_TRACE_SLEEP_OVERSHOOT,
} sched_trace_event_type_t;

typedef struct sched_trace_event {
    uint32_t seq;
    uint32_t tick;
    uint32_t event;
    uint32_t syscall;
    uint32_t pid;
    uint32_t tid;
    uint32_t state;
    uint32_t wakeup_time;
    uint32_t current_pid;
    uint32_t current_tid;
    uint32_t current_syscall;
    uintptr_t task_ptr;
    uintptr_t next_ptr;
    uintptr_t prev_ptr;
    char name[TASK_NAME_MAX];
    char current_name[TASK_NAME_MAX];
} sched_trace_event_t;

typedef struct task task_t;

typedef int (*task_poll_wait_handler_t)(void *owner);

int task_register_poll_wait_handler(task_poll_wait_handler_t handler,
                                    void *owner);
int task_poll_wait_once(void);

void sched_trace_record(sched_trace_event_type_t event, task_t* task);
void sched_trace_snapshot(sched_trace_event_t* out, uint32_t max,
                          uint32_t* total, uint32_t* written);
/*
 * Internal scheduler primitive: caller must hold task_lock.  Use this when a
 * higher-level subsystem already owns the scheduler critical section and needs
 * to make a task runnable without recursively taking task_lock.
 */
void task_make_ready_under_lock(task_t* task);
void task_set_wakeup_time(task_t* task, uint32_t wakeup_time);
void task_wake(task_t* task);

typedef struct scheduler_stats {
    uint32_t nr_running;
    uint32_t nonempty_queues;
    uint32_t policy_levels;
    uint32_t default_priority;
    uint32_t idle_priority;
    int32_t nice_min;
    int32_t nice_max;
    uint32_t quantum_ticks;
    uint32_t aging_step_ticks;
    uint32_t aging_max_bonus;
    uint32_t aging_selections;
    uint32_t debt_decay_ticks;
    uint32_t debt_selections;
    uint32_t last_pick_reason;
    uint32_t last_pick_tid;
    uint32_t last_pick_pid;
    uint32_t last_pick_priority;
    uint32_t last_pick_effective_priority;
    uint32_t last_pick_debt;
    uint32_t last_pick_waited_ticks;
    uint32_t last_scan_tasks;
    uint32_t max_ready_debt;
    uint32_t avg_ready_debt;
    uint32_t highest_ready_priority;
    uint32_t lowest_ready_priority;
    uint32_t current_tid;
    uint32_t current_pid;
    uint32_t current_priority;
    char current_name[TASK_NAME_MAX];
    uint32_t priority_counts[TASK_PRIORITY_LEVELS];
} scheduler_stats_t;

void scheduler_get_stats(scheduler_stats_t* stats);

/* Forward declarations for structures */
typedef struct inode inode_t;
typedef struct file file_t;
typedef struct file_operations file_operations_t;

typedef enum file_type {
    FILE_TYPE_REGULAR = 0,
    FILE_TYPE_TTY,
    FILE_TYPE_NULL,
    FILE_TYPE_PIPE,
    FILE_TYPE_NETECHO,
    FILE_TYPE_SOCKET,
    FILE_TYPE_ARMOS_SOCKET,
    FILE_TYPE_SHM,
    FILE_TYPE_FRAMEBUFFER,
    FILE_TYPE_NETCTL,
    FILE_TYPE_INPUT,
    FILE_TYPE_PTY_MASTER,
    FILE_TYPE_EVENTFD,
    FILE_TYPE_TIMERFD,
    FILE_TYPE_DRM,
    FILE_TYPE_DRM_BO,
    FILE_TYPE_DRM_FENCE,
} file_type_t;
typedef struct inode_operations inode_operations_t;

/* Inode structure */
struct inode {
    uint32_t ino;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint32_t size;
    uint32_t atime;
    uint32_t mtime;
    uint32_t ctime;
    uint32_t blocks;
    nlink_t nlink;
    uint32_t ref_count;
    uint32_t open_count;
    uint32_t flags;
    
    /* Filesystem specific */
    uint32_t first_cluster;
    uint32_t parent_cluster;
    
    /* Operations */
    inode_operations_t* i_op;
    file_operations_t* f_op;
    void* private_data;
    void (*release_private)(struct inode* inode);
    
    struct inode* next;
};

/* File structure */
struct file {
    char name[256];               /* Nom du fichier */
    char path[MAX_PATH];          /* Chemin absolu stable pour les appels *at */
    uint32_t pos;
    inode_t* inode;
    uint32_t offset;
    uint32_t flags;
    file_type_t type;
    uint32_t ref_count;
    file_operations_t* f_op;
    void* private_data;
};

/* Directory entry */
typedef struct dirent {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
} dirent_t;

/* File operations */
struct file_operations {
    ssize_t (*read)(file_t* file, void* buffer, size_t count);
    ssize_t (*write)(file_t* file, const void* buffer, size_t count);
    int (*open)(inode_t* inode, file_t* file);
    int (*close)(file_t* file);
    off_t (*lseek)(file_t* file, off_t offset, int whence);
    int (*readdir)(file_t* file, dirent_t* dirent);
    int (*truncate)(file_t* file, off_t length);
};

/* Inode operations */
struct inode_operations {
    inode_t* (*lookup)(inode_t* dir, const char* name);
    int (*create)(inode_t* dir, const char* name, uint16_t mode);
    int (*mkdir)(inode_t* dir, const char* name, uint16_t mode);
    int (*unlink)(inode_t* dir, const char* name);
    int (*rmdir)(inode_t* dir, const char* name);
    int (*rename)(inode_t* old_dir, const char* old_name,
                  inode_t* new_dir, const char* new_name);
    int (*readlink)(inode_t* inode, char* buf, size_t bufsiz);
};

/*
 * Stable ArmOS stat syscall payload.
 *
 * Newlib translates this fixed-width kernel ABI into its native struct stat.
 * Do not use architecture-sized kernel aliases here: changing off_t or time_t
 * width must not move fields in an existing user ABI.
 */
struct stat {
    uint32_t st_dev;
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_rdev;
    int32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};

/* etats des taches */
typedef enum {
    TASK_READY = 0,         /* Prete a s'executer */
    TASK_RUNNING,           /* En cours d'execution */
    TASK_BLOCKED,           /* Bloquee (I/O, sleep, etc.) */
    TASK_ZOMBIE,            /* Terminee, en attente de nettoyage */
    TASK_TERMINATED,        /* Completement terminee */
    TASK_INTERRUPTIBLE,     /* Bloquee mais peut etre reveillee par un signal */
    TASK_UNINTERRUPTIBLE,   /* Bloquee, ne peut pas etre reveillee */
    TASK_STOPPED            /* Stoppee par SIGSTOP/SIGTSTP */
} task_state_t;

typedef enum {
    PROC_READY ,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE,
    PROC_DEAD,
    PROC_INTERRUPTIBLE,
    PROC_UNINTERRUPTIBLE,
    PROC_STOPPED
} proc_state_t;

typedef enum {
    TASK_TYPE_PROCESS = 1,  /* Processus principal */
    TASK_TYPE_THREAD = 2,   /* Thread dans un processus */
    TASK_TYPE_KERNEL = 3    /* Tache kernel pure */
} task_type_t;

/* Signal actions */
typedef enum {
    SIG_ACT_TERM,
    SIG_ACT_IGN,
    SIG_ACT_CORE,
    SIG_ACT_STOP,
    SIG_ACT_CONT
} sig_default_action_t;

/* Signal handler */
typedef void (*sig_handler_t)(int);
#define SIG_DFL  ((sig_handler_t)0)
#define SIG_IGN  ((sig_handler_t)1)

/* Sigaction structure */
typedef struct {
    sig_handler_t sa_handler;
    uint32_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
} sigaction_t;

/* Signal state */
typedef struct {
    sigaction_t actions[MAX_SIGNALS];
    uint32_t pending;
    uint32_t blocked;
    uint32_t in_handler;
    uint32_t return_override;
    
    task_context_t saved_context;
} signal_state_t;

typedef struct {
    pid_t pid;              /* PID POSIX */
    pid_t ppid;             /* Parent PID */
    pid_t pgid;             /* Process group ID */
    pid_t sid;              /* Session ID */
    int controlling_tty;    /* Controlling TTY id, -1 if none */
    struct task* parent;    /* Processus parent */
    struct task* children;  /* Enfants */
    struct task* sibling_next;
    vm_space_t* vm;         /* Espace memoire */
    file_t* files[MAX_FILES];  /* Descripteurs */
    uint32_t fd_flags[MAX_FILES]; /* Flags propres au descripteur (FD_CLOEXEC, etc.) */
    int exit_code;
    int term_signal;
    int stop_signal;
    int stop_reported;
    uid_t uid, gid;
    mode_t umask;
    uint32_t rlimit_nofile_cur;
    uint32_t rlimit_nofile_max;
    uint32_t alarm_expire_tick;
    uint32_t alarm_active;
    struct task* leader;
    uint32_t thread_count;
    vaddr_t tls_image;
    size_t tls_file_size;
    size_t tls_memory_size;
    size_t tls_alignment;

    proc_state_t state;

        /* Signals */
    signal_state_t signals;
    
    /* Signal stack */
    vaddr_t signal_stack_base;
    size_t signal_stack_size;

    /* === AJOUT : Contexte waitpid === */
    /* Ces champs permettent de sauvegarder le contexte lors d'un blocage dans waitpid */
    pid_t waitpid_pid;          /* PID attendu dans waitpid */
    int* waitpid_status;        /* Pointeur status dans waitpid */
    int waitpid_options;        /* Options waitpid */
    int waitpid_iteration;      /* Numero d'iteration dans waitpid */
    uint32_t waitpid_caller_lr; /* LR pour retourner apres waitpid */

    char cwd[MAX_PATH];   /* Current Working Directory */
    char exe_path[MAX_PATH];
    char cmdline[PROC_CMDLINE_MAX];
    size_t cmdline_len;
    char environ[PROC_ENVIRON_MAX];
    size_t environ_len;

} __attribute__((aligned(8))) process_t;


/* Structure d'une tache */
typedef struct task {
    /* Identification */
    uint32_t task_id;                       /* ID unique */
    char name[TASK_NAME_MAX];               /* Nom de la tache */
    task_state_t state;                     /* etat actuel */
    uint32_t priority;                      /* Priorite (0 = plus haute) */
    
    /* Contexte et stack */
    task_context_t context;                 /* Contexte sauvegarde */
    void* stack_base;                       /* Base de la stack */
    void* stack_top;                        /* Sommet de la stack */
    uint32_t stack_size;                    /* Taille de la stack */
    void* stack_phys_base;                  /* Physical base used to free stack pages */
    
    /* Fonction d'entree */
    void (*entry_point)(void* arg);         /* Point d'entree */
    void* entry_arg;                        /* Argument */
    
    /* Global task list: all live tasks, including sleeping/zombie tasks. */
    struct task* next;
    struct task* prev;

    /* Scheduler runqueue: only TASK_READY tasks waiting for CPU time. */
    struct task* rq_next;
    struct task* rq_prev;
    uint32_t rq_priority;
    
    /* Statistiques */
    uint64_t created_time;                  /* Timestamp de creation */
    uint64_t total_runtime;                 /* Temps total d'execution */
    uint32_t switch_count;                  /* Nombre de commutations */
    uint32_t page_faults;                   /* Fautes user resolues */
    uint32_t cow_faults;                    /* Fautes COW resolues */
    uint32_t stack_faults;                  /* Croissances de pile user */
    uint32_t lazy_faults;                   /* Anonymous mmap pages faulted in lazily */
    uint32_t wakeup_time;                   /* Temps de reveil (ms) */
    
    /* === EXTENSIONS PROCESSUS === */
    task_type_t type;               /* Type de tache */
    uint32_t quantum_left;
    
    /* Donnees processus (seulement si type == PROCESS) */
    union {
        process_t* process;
        struct {
            /*
             * Keep the shared process pointer first: task->process is then a
             * valid common view for both leaders and member threads without
             * changing the established union size or assembly-visible offsets.
             */
            process_t* process;
            struct task* leader;
        } thread;
    };

    /*
     * Scheduler diagnostics only. Keep these fields at the end so adding
     * tracing does not move legacy task_t offsets used by ARM assembly or
     * low-level debug code.
     */
    uint32_t current_syscall;               /* Syscall currently executing, or 0 */
    uint32_t last_syscall;                  /* Last syscall entered by this task */
    uint32_t ready_since_tick;              /* Scheduler aging: tick when queued READY */
    uint32_t sched_debt;                    /* CPU fairness debt, in timer ticks */
    uint32_t running_cpu;                   /* CPU currently owning TASK_RUNNING */
    uint32_t last_cpu;                      /* Last CPU that executed this task */
    uint32_t magic;                         /* Object lifetime guard for SMP diagnostics */
    uint64_t user_runtime;                  /* EL0 execution time in timer ticks */
    uint64_t system_runtime;                /* Kernel time charged to this task */
    vaddr_t clear_child_tid;                 /* User TID word cleared on thread exit */
    vaddr_t futex_wait_address;              /* Active userspace wait key */
    uint32_t futex_wait_active;
    uint32_t poll_wait_active;               /* Waiting for descriptor state change */

} __attribute__((aligned(8))) task_t;

/* Interface publique */
uid_t current_uid(void);
uid_t current_gid(void);

/* Initialisation du systeme */
void init_task_system(void);
void cleanup_task_system(void);

/* Creation et destruction */
task_t* task_create(const char* name, void (*entry)(void* arg), void* arg, uint32_t priority);
task_t* task_create_process(const char* name, void (*entry)(void* arg), void* arg, uint32_t priority, task_type_t type);
task_t* task_create_user_thread(task_t* creator, vaddr_t entry, vaddr_t argument,
                                vaddr_t user_stack, vaddr_t tls_base,
                                vaddr_t clear_child_tid);
void task_publish_user_thread(task_t* thread);
void task_abort_user_thread(task_t* thread);
void task_exit_current_thread(int status) __attribute__((noreturn));
int task_futex_wait(task_t* task, vaddr_t address, uint32_t expected,
                    uint32_t deadline);
uint32_t task_futex_wake(process_t* process, vaddr_t address,
                         uint32_t max_count);
uint32_t task_poll_generation(void);
int task_poll_wait(task_t* task, uint32_t generation, uint32_t deadline);
void task_poll_notify(void);
void task_destroy(task_t* task);
void task_free_kernel_stack(task_t* task);

/* Gestion du scheduler */
void yield(void);                           /* Ceder le CPU volontairement */
void schedule(void);                        /* Forcer une commutation */
void sched_start(void);                     /* Demarrer le scheduler */
void task_sleep_ms(uint32_t ms);
int task_sleep_interruptible_ms(uint32_t ms);
void schedule_to(task_t *next_task);
uint64_t task_runtime_ticks(task_t* task);  /* Temps CPU cumule en ticks */

/* Acces aux taches */
task_t* task_find_by_id(uint32_t task_id);  /* Trouver par ID */
task_t* task_find_by_name(const char* name); /* Trouver par nom */
void setup_task_context(task_t* task);
void add_to_ready_queue(task_t* task);
void remove_from_ready_queue(task_t* task);
void destroy_zombie_process(task_t* zombie);
bool is_in_ready_queue(task_t* task);
void remove_task_from_list(task_t* task);

/* Gestion des priorites */
void task_set_priority(task_t* task, uint32_t priority);
uint32_t task_get_priority(task_t* task);

/* etats des taches */
void task_set_state(task_t* task, task_state_t state);
void task_set_blocked_under_lock(task_t* task);
void task_set_stopped_under_lock(task_t* task);
void task_set_ready(task_t* task);
void task_set_blocked(task_t* task);
void task_set_interruptible(task_t* task);
void task_set_interruptible_until(task_t* task, uint32_t wakeup_time);
void task_set_uninterruptible(task_t* task);
void task_set_uninterruptible_until(task_t* task, uint32_t wakeup_time);
void task_set_stopped(task_t* task);
void task_set_zombie(task_t* task);
void task_set_terminated(task_t* task);
task_state_t task_get_state(task_t* task);
const char* task_state_string(task_state_t state);
const char* proc_state_string(proc_state_t state);

/* Utilitaires de debug */
void task_dump_info(task_t* task);          /* Infos d'une tache */
void task_list_all(void);                   /* Liste toutes les taches */
void task_dump_stacks(void);                /* Analyse des stacks */
void task_check_stack_integrity(void);      /* Verification integrite */
void debug_current_task_detailed(const char* location);
void task_dump_stacks_detailed(void);                /* Analyse des stacks */
void debug_all_task_stacks(void);
void debug_print_ctx(task_context_t *context, const char* caller);


/* Statistiques */
uint32_t task_get_count(void);              /* Nombre de taches */
void task_print_stats(void);                /* Statistiques globales */

/* Fonctions internes de commutation (assembleur) */
void __task_first_switch_v2(task_context_t* new_ctx);
void __task_switch_asm(task_context_t* old_ctx, task_context_t* new_ctx);
void __task_switch(task_context_t* old_ctx, task_context_t* new_ctx);
void task_context_save_complete(task_context_t* ctx);
void switch_to_idle(void);

extern task_t* current_task;
extern task_t* current_tasks[];
extern task_t* task_list_head;
extern uint32_t task_count;
extern spinlock_t task_lock;
extern task_t* idle_task;
extern task_t* idle_tasks[];
extern task_t* init_process;

task_t* task_current_on_cpu(uint32_t cpu_id);
task_t* task_current_local(void);
process_t* task_get_process(task_t* task);
task_t* task_get_process_leader(task_t* task);
bool task_is_user_context(task_t* task);
int task_current_publish(uint32_t cpu_id, task_t* task);
void task_current_clear_all(void);
task_t* task_idle_on_cpu(uint32_t cpu_id);
bool task_is_valid(task_t* task);
bool task_is_idle_task(task_t* task);
void task_register_idle_cpu(uint32_t cpu_id, task_t* task);
void task_start_secondary_scheduler(uint32_t cpu_id) __attribute__((noreturn));
void scheduler_prepare_secondary_cpus(void);
void scheduler_request_resched_current_cpu(void);
bool scheduler_take_resched_current_cpu(void);
bool scheduler_resched_pending_on_cpu(uint32_t cpu_id);
uint32_t scheduler_idle_work_seen_count(uint32_t cpu_id);
uint32_t scheduler_idle_schedule_count(uint32_t cpu_id);
uint32_t scheduler_idle_fallback_count(uint32_t cpu_id);


void add_task_to_list(task_t* task);

extern void get_and_save_usr_context(task_t* t);
/* ↑ Petite routine appelée à l’entrée SVC pour remplir :
     t->context.usr_pc   = LR_svc (PC de retour user)
     t->context.usr_cpsr = SPSR_svc (donc 0x10 typiquement)
     t->context.usr_sp   = SP_usr   (via cps #SYS puis mov)
     t->context.usr_r[i] = r0..r12 user (cf. wrapper SVC)
*/

#define TASK_CONTEXT_OFF offsetof(task_t, context)

static inline uint32_t disable_interrupts_save(void)
{
    return arch_irq_fiq_save();
}

static inline void restore_interrupts(uint32_t cpsr)
{
    arch_irq_fiq_restore(cpsr);
}

#endif /* _KERNEL_TASK_H */
