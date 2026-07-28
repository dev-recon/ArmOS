/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/types.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_TYPES_H
#define _KERNEL_TYPES_H

#include <kernel/arch_cache.h>
#include <kernel/arch_page.h>

/* === TYPES DE BASE === */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

/* === TYPES SYSTeME === */

#ifndef ARMOS_ARCH_BITS
#error "ARMOS_ARCH_BITS must be defined by the active architecture build"
#endif

#if ARMOS_ARCH_BITS == 32
typedef __SIZE_TYPE__      size_t;
typedef __PTRDIFF_TYPE__   ssize_t;
typedef int32_t            off_t;
#elif ARMOS_ARCH_BITS == 64
typedef __SIZE_TYPE__      size_t;
typedef __PTRDIFF_TYPE__   ssize_t;
typedef int64_t            off_t;
#else
#error "Unsupported ARMOS_ARCH_BITS value"
#endif
typedef uint32_t           mode_t;
typedef int32_t            pid_t;
typedef uint32_t           uid_t;
typedef uint32_t           gid_t;
typedef uint32_t           dev_t;
typedef uint32_t           ino_t;
typedef uint32_t           nlink_t;
typedef uint32_t           blksize_t;
typedef uint32_t           blkcnt_t;
typedef uint32_t           time_t;

/* === TYPES POUR ADRESSES === */

/* Pour les calculs d'adresses et pointeurs */
#if ARMOS_ARCH_BITS == 32
typedef uint32_t           uintptr_t;
typedef int32_t            intptr_t;
typedef uint32_t           paddr_t;
typedef uint32_t           vaddr_t;
typedef uint32_t           pfn_t;
#elif ARMOS_ARCH_BITS == 64
typedef uint64_t           uintptr_t;
typedef int64_t            intptr_t;
typedef uint64_t           paddr_t;
typedef uint64_t           vaddr_t;
typedef uint64_t           pfn_t;
#else
#error "Unsupported ARMOS_ARCH_BITS value"
#endif

typedef void*              pgdir_t;      /* Opaque architecture page-directory handle */
typedef paddr_t            phys_addr_t;  /* Legacy alias; prefer paddr_t */
typedef vaddr_t            virt_addr_t;  /* Legacy alias; prefer vaddr_t */

/* === TYPES BOOLeENS === */

typedef enum {
    false = 0,
    true = 1
} bool;

/* === CONSTANTES === */

#define NULL ((void*)0)

/* Page geometry supplied by the active architecture backend. */
#define PAGE_SIZE        ARCH_PAGE_SIZE
#define PAGE_SHIFT       ARCH_PAGE_SHIFT
#define PAGE_OFFSET_MASK ARCH_PAGE_OFFSET_MASK
#define PAGE_MASK        ARCH_PAGE_MASK

/* Limites systeme */
#define MAX_FILES       256
#define MAX_ARGS        32
#define MAX_PATH        256
#define MAX_NAME        255

/* === CODES D'ERREUR === */

#define EPERM           1       /* Operation not permitted */
#define ENOENT          2       /* No such file or directory */
#define ESRCH           3       /* No such process */
#define EINTR           4       /* Interrupted system call */
#define EIO             5       /* I/O error */
#define ENXIO           6       /* No such device or address */
#define E2BIG           7       /* Argument list too long */
#define ENOEXEC         8       /* Exec format error */
#define EBADF           9       /* Bad file number */
#define ECHILD         10       /* No child processes */
#define EAGAIN         11       /* Try again */
#define ENOMEM         12       /* Out of memory */
#define EACCES         13       /* Permission denied */
#define EFAULT         14       /* Bad address */
#define ENOTBLK        15       /* Block device required */
#define EBUSY          16       /* Device or resource busy */
#define EEXIST         17       /* File exists */
#define EXDEV          18       /* Cross-device link */
#define ENODEV         19       /* No such device */
#define ENOTDIR        20       /* Not a directory */
#define EISDIR         21       /* Is a directory */
#define EINVAL         22       /* Invalid argument */
#define ENFILE         23       /* File table overflow */
#define EMFILE         24       /* Too many open files */
#define ENOTTY         25       /* Not a typewriter */
#define ETXTBSY        26       /* Text file busy */
#define EFBIG          27       /* File too large */
#define ENOSPC         28       /* No space left on device */
#define ESPIPE         29       /* Illegal seek */
#define EROFS          30       /* Read-only file system */
#define EMLINK         31       /* Too many links */
#define EPIPE          32       /* Broken pipe */
#define EDOM           33       /* Math argument out of domain */
#define ERANGE         34       /* Math result not representable */
#define ENAMETOOLONG   36       /* File name too long */
#define ENOSYS         38       /* Function not implemented */
#define ENOTEMPTY      39       /* Directory not empty */
#define ELOOP          40       /* Too many symbolic links */
#define EOVERFLOW      75       /* Value too large for defined data type */
#define ENOTSOCK      108       /* Socket operation on non-socket */
#define EDESTADDRREQ  121       /* Destination address required */
#define EMSGSIZE      122       /* Message too long */
#define EPROTONOSUPPORT 123     /* Protocol not supported */
#define EAFNOSUPPORT  106       /* Address family not supported */
#define EADDRINUSE    112       /* Address already in use */
#define EADDRNOTAVAIL 125       /* Cannot assign requested address */
#define ENETDOWN      115       /* Network is down */
#define ENETUNREACH   114       /* Network is unreachable */
#define ECONNABORTED  113       /* Software caused connection abort */
#define ECONNRESET    104       /* Connection reset by peer */
#define EISCONN       127       /* Transport endpoint is already connected */
#define ENOTCONN      128       /* Transport endpoint is not connected */
#define ETIMEDOUT     116       /* Connection timed out */
#define ECONNREFUSED  111       /* Connection refused */
#define EINPROGRESS   119       /* Operation now in progress */
#define ENOTSUP       134       /* Operation not supported */

/* === TYPES DE SYNCHRONISATION === */

/* Legacy cache-line-sized lock storage. */
typedef struct {
    volatile uint32_t locked;            /* 4 bytes - etat du lock */
    uint32_t owner;                      /* 4 bytes - CPU proprietaire */
    uint32_t count;                      /* 4 bytes - compteur recursif */
    uint32_t padding[5];                 /* 20 bytes - alignement cache line */
} __attribute__((aligned(ARCH_CACHE_LINE_SIZE))) spinlock_tt;  /* Aligne sur cache line arch */

/* Mutex simple */
typedef struct {
    volatile uint32_t locked;            /* 4 bytes */
    pid_t owner;                         /* 4 bytes */
    uint32_t count;                      /* 4 bytes - pour mutex recursif */
    uint32_t padding[5];                 /* 20 bytes */
} __attribute__((aligned(32))) mutex_t;

/* Semaphore */
typedef struct {
    volatile int32_t count;              /* 4 bytes */
    volatile uint32_t waiting;           /* 4 bytes */
    uint32_t padding[6];                 /* 24 bytes */
} __attribute__((aligned(32))) semaphore_t;

/* === TYPES POUR PROCESSUS === */

/* etat d'un processus */
typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE,
    PROCESS_TERMINATED
} process_state_t;

/* Priorite des processus */
typedef uint32_t priority_t;

/* === TYPES POUR MeMOIRE === */

/* Flags pour les pages memoire */
typedef uint32_t page_flags_t;

#define PAGE_FLAG_PRESENT   (1 << 0)
#define PAGE_FLAG_WRITABLE  (1 << 1)
#define PAGE_FLAG_USER      (1 << 2)
#define PAGE_FLAG_CACHED    (1 << 3)
#define PAGE_FLAG_BUFFERED  (1 << 4)
#define PAGE_FLAG_DIRTY     (1 << 5)
#define PAGE_FLAG_ACCESSED  (1 << 6)

/* === TYPES POUR FICHIERS === */

/* Mode d'ouverture de fichier */
//#define O_RDONLY    0x0000
//#define O_WRONLY    0x0001
//#define O_RDWR      0x0002
//#define O_CREAT     0x0040
//#define O_EXCL      0x0080
//#define O_TRUNC     0x0200
//#define O_APPEND    0x0400
//#define O_NONBLOCK  0x0800

/* Type de fichier */
#define S_IFMT      0170000   /* Type mask */
#define S_IFREG     0100000   /* Regular file */
#define S_IFDIR     0040000   /* Directory */
#define S_IFCHR     0020000   /* Character device */
#define S_IFBLK     0060000   /* Block device */
#define S_IFIFO     0010000   /* FIFO */
#define S_IFLNK     0120000   /* Symbolic link */
#define S_IFSOCK    0140000   /* Socket */

/* Permissions */
#define S_ISUID     0004000   /* Set user ID */
#define S_ISGID     0002000   /* Set group ID */
#define S_ISVTX     0001000   /* Sticky bit */
#define S_IRWXU     0000700   /* User permissions */
#define S_IRUSR     0000400   /* User read */
#define S_IWUSR     0000200   /* User write */
#define S_IXUSR     0000100   /* User execute */
#define S_IRWXG     0000070   /* Group permissions */
#define S_IRGRP     0000040   /* Group read */
#define S_IWGRP     0000020   /* Group write */
#define S_IXGRP     0000010   /* Group execute */
#define S_IRWXO     0000007   /* Other permissions */
#define S_IROTH     0000004   /* Other read */
#define S_IWOTH     0000002   /* Other write */
#define S_IXOTH     0000001   /* Other execute */

/* === TYPES POUR SIGNAUX === */

typedef uint32_t sigset_t;

/* Signaux standard */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1    10
#define SIGSEGV    11
#define SIGUSR2    12
#define SIGPIPE    13
#define SIGALRM    14
#define SIGTERM    15
#define SIGCHLD    17
#define SIGCONT    18
#define SIGSTOP    19
#define SIGTSTP    20
#define SIGTTIN    21
#define SIGTTOU    22

/* === TYPES POUR TIMER === */

/* Structure pour les timers */
typedef struct {
    uint32_t sec;                        /* Secondes */
    uint32_t nsec;                       /* Nanosecondes */
} timespec_t;

typedef struct {
    uint32_t sec;                        /* Secondes */
    uint32_t usec;                       /* Microsecondes */
} timeval_t;

/* === TYPES POUR ReSEAU === */

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

/* === MACROS UTILITAIRES === */

/* Alignement */
#define ALIGN_UP(x, align)      (((x) + (align) - 1) & ~((align) - 1))
#define ALIGN_DOWN(x, align)    ((x) & ~((align) - 1))
#define IS_ALIGNED(x, align)    (((x) & ((align) - 1)) == 0)

/* Taille d'un tableau */
#define ARRAY_SIZE(arr)         (sizeof(arr) / sizeof((arr)[0]))

/* Min/Max */
#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define MAX(a, b)               ((a) > (b) ? (a) : (b))

/* Conversion octets */
#define KB(x)                   ((x) * 1024)
#define MB(x)                   ((x) * 1024 * 1024)
#define GB(x)                   ((x) * 1024 * 1024 * 1024)

/* Public cache-line aliases used by generic alignment macros. */
#define L1_CACHE_LINE_SIZE      ARCH_L1_CACHE_LINE_SIZE
#define L2_CACHE_LINE_SIZE      ARCH_L2_CACHE_LINE_SIZE
#define CACHE_LINE_SIZE         ARCH_CACHE_LINE_SIZE

/* === ATTRIBUTS COMPILATEUR === */

/* Attributs pour l'optimisation */
#define PACKED                  __attribute__((packed))
#define ALIGNED(x)              __attribute__((aligned(x)))
#define CACHE_ALIGNED           __attribute__((aligned(CACHE_LINE_SIZE)))
#define UNUSED                  __attribute__((unused))
#define LIKELY(x)               __builtin_expect(!!(x), 1)
#define UNLIKELY(x)             __builtin_expect(!!(x), 0)

/* Verifications des tailles */
typedef char size_check_uint8_t[sizeof(uint8_t) == 1 ? 1 : -1];
typedef char size_check_uint16_t[sizeof(uint16_t) == 2 ? 1 : -1];
typedef char size_check_uint32_t[sizeof(uint32_t) == 4 ? 1 : -1];
typedef char size_check_uint64_t[sizeof(uint64_t) == 8 ? 1 : -1];
typedef char size_check_uintptr_t[sizeof(uintptr_t) == (ARMOS_ARCH_BITS / 8) ? 1 : -1];

#endif /* _KERNEL_TYPES_H */
