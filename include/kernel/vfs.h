/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/vfs.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

#include <kernel/types.h>
#include <kernel/task.h>

/* Forward declarations */
struct process;

/* File types */
#define S_IFMT      0170000
#define S_IFREG     0100000
#define S_IFDIR     0040000
#define S_IFCHR     0020000
#define S_IFBLK     0060000
#define S_IFIFO     0010000
#define S_IFLNK     0120000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)

/* Permissions */
#define S_IRWXU     0000700
#define S_IRUSR     0000400
#define S_IWUSR     0000200
#define S_IXUSR     0000100
#define S_IRWXG     0000070
#define S_IRGRP     0000040
#define S_IWGRP     0000020
#define S_IXGRP     0000010
#define S_IRWXO     0000007
#define S_IROTH     0000004
#define S_IWOTH     0000002
#define S_IXOTH     0000001


/* Seek whence */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* Directory entry types */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

struct statfs {
    uint32_t f_type;
    uint32_t f_bsize;
    uint32_t f_blocks;
    uint32_t f_bfree;
    uint32_t f_bavail;
    uint32_t f_files;
    uint32_t f_ffree;
    uint32_t f_namelen;
    uint32_t f_frsize;
};

typedef struct kernel_file {
    file_t file;
    inode_t* inode;
    bool opened;
} kernel_file_t;



/* VFS functions */
bool init_vfs(void);
bool vfs_install_root(inode_t* root);
int  vfs_mount(const char* path, inode_t* root);
int  vfs_mount_ex(const char* path, inode_t* root, const char* source,
                  const char* fstype, const char* options);
int  vfs_umount(const char* path);
bool vfs_is_mounted(const char* path);
bool vfs_is_mountpoint(const char* path);
void vfs_begin_mutation(void);
void vfs_end_mutation(void);
void vfs_inode_opened(inode_t* inode);
void vfs_inode_closed(inode_t* inode);
uint32_t vfs_inode_open_count(inode_t* inode);
void vfs_format_mounts(char* buf, size_t cap, size_t* len);
int  vfs_statfs(const char* path, struct statfs* st);
int  vfs_sync(void);
int  vfs_shutdown(void);
inode_t* create_inode(void);
inode_t* get_inode(uint32_t ino);
inode_t* vfs_get_backing_inode(inode_operations_t* operations,
                               uint32_t backing_id);
inode_t* get_root_inode(void);
void put_inode(inode_t* inode);
inode_t* path_lookup(const char* path);
inode_t* path_lookup_ex(const char* path, bool follow_final_symlink);

/* Kernel-internal streaming file access without a process descriptor table. */
int vfs_kernel_file_open(const char* path, kernel_file_t* handle);
ssize_t vfs_kernel_file_read(kernel_file_t* handle, void* buffer,
                             size_t count);
uint32_t vfs_kernel_file_size(const kernel_file_t* handle);
void vfs_kernel_file_close(kernel_file_t* handle);
int vfs_kernel_dir_open(const char* path, kernel_file_t* handle);
int vfs_kernel_dir_read(kernel_file_t* handle, dirent_t* entry);

/* File descriptor management */
void free_fd(task_t* proc, int fd);
int allocate_fd(task_t* process);
uint32_t vfs_fd_limit(task_t* process);
int vfs_install_file(task_t* task, file_t* file, uint32_t descriptor_flags);
file_t* vfs_get_file_from_fd(task_t* task, int fd);
file_t* vfs_take_file(task_t* task, int fd);

/* File operations */
file_t* get_file(file_t* file);
void close_file(file_t* file);

/* User space memory functions */
int copy_to_user(void* to, const void* from, size_t n);
int copy_from_user(void* to, const void* from, size_t n);
bool is_valid_user_ptr(const void* ptr);

/* File creation */
file_t* create_file(void);
void close_file(file_t* file);

uint32_t get_next_inode_number(void);

/* Helper functions */


/* Syscall entry points live in syscall modules, not in VFS. */

#endif
