/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/file.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_FILE_H
#define _KERNEL_FILE_H

#include <kernel/task.h>

/* Accès (mutuellement exclusifs) */
#ifndef O_RDONLY
#define O_RDONLY    0x0000      /* lecture seule */
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x0001      /* écriture seule */
#endif
#ifndef O_RDWR
#define O_RDWR      0x0002      /* lecture + écriture */
#endif
#ifndef O_ACCMODE
#define O_ACCMODE   0x0003      /* masque pour extraire l'accès */
#endif

/* Création / comportement */
#ifndef O_CREAT
#define O_CREAT     0x0200      /* créer si n'existe pas */
#endif
#ifndef O_EXCL
#define O_EXCL      0x0800      /* avec O_CREAT : échouer si existe */
#endif
#ifndef O_NOCTTY
#define O_NOCTTY    0x8000      /* ne pas devenir controlling-tty */
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x0400      /* tronquer à 0 si écriture */
#endif
#ifndef O_APPEND
#define O_APPEND    0x0008      /* writes ajoutent à la fin */
#endif

/* Non-bloquant / async */
#ifndef O_NONBLOCK
#define O_NONBLOCK  0x4000      /* non-bloquant */
#endif
#ifndef O_DSYNC
#define O_DSYNC     0x1000      /* écriture synchronisée des données */
#endif
#ifndef O_RSYNC
#define O_RSYNC     0x2000      /* lectures synchronisées */
#endif
#ifndef O_SYNC
#define O_SYNC      0x2000      /* écriture synchronisée */
#endif

/* Options diverses / sécurités */
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x200000     /* doit être répertoire */
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW  0x100000     /* ne pas suivre symlink */
#endif
#ifndef O_DIRECT
#define O_DIRECT    0x80000     /* bypass caches */
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC   0x40000     /* FD fermé au exec */
#endif

#ifndef FD_CLOEXEC
#define FD_CLOEXEC  1
#endif

#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_GETLK     7
#define F_SETLK     8
#define F_SETLKW    9
#define F_DUPFD_CLOEXEC 14

#define F_RDLCK     1
#define F_WRLCK     2
#define F_UNLCK     3

struct flock_kernel {
    int16_t l_type;
    int16_t l_whence;
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
};

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCFLSH      0x540B
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCSCTTY   0x540E
#include <uapi/armos/input.h>

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

/* Modes (mode_t) — permissions POSIX */
#ifndef S_IRUSR
#define S_IRUSR     0400        /* owner read */
#endif
#ifndef S_IWUSR
#define S_IWUSR     0200        /* owner write */
#endif
#ifndef S_IXUSR
#define S_IXUSR     0100        /* owner exec */
#endif

#ifndef S_IRGRP
#define S_IRGRP     0040        /* group read */
#endif
#ifndef S_IWGRP
#define S_IWGRP     0020        /* group write */
#endif
#ifndef S_IXGRP
#define S_IXGRP     0010        /* group exec */
#endif

#ifndef S_IROTH
#define S_IROTH     0004        /* others read */
#endif
#ifndef S_IWOTH
#define S_IWOTH     0002        /* others write */
#endif
#ifndef S_IXOTH
#define S_IXOTH     0001        /* others exec */
#endif

/* Utilitaires */
#ifndef S_IRWXU
#define S_IRWXU     (S_IRUSR|S_IWUSR|S_IXUSR)  /* owner rwx */
#endif
#ifndef S_IRWXG
#define S_IRWXG     (S_IRGRP|S_IWGRP|S_IXGRP)  /* group rwx */
#endif
#ifndef S_IRWXO
#define S_IRWXO     (S_IROTH|S_IWOTH|S_IXOTH)  /* others rwx */
#endif


/* Permissions pour inode_permission */
#define MAY_EXEC    1
#define MAY_WRITE   2
#define MAY_READ    4

int split_path(const char* full_path, char** parent_path, char** filename);
bool inode_permission(inode_t* inode, int mask);
bool can_read(file_t* file);
bool can_write(file_t* file);
bool file_is_tty(file_t* file);

char* resolve_path(const char* path);
int   resolve_path_at(int dirfd, const char* path, char** resolved);
int   resolve_path_at_raw(int dirfd, const char* path, char** resolved);
int   vfs_check_search_permission(const char* path, bool include_final);
void  path_canonicalize(char* path);
char* get_current_working_directory(void);

#endif /* _KERNEL_FILE_H */
