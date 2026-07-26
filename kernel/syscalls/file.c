/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/syscalls/file.c
 * Layer: Kernel / syscall implementation
 *
 * Responsibilities:
 * - Validate user-facing syscall requests.
 * - Bridge user ABI arguments to kernel subsystems.
 *
 * Notes:
 * - Never trust user pointers; copy through checked helpers.
 */

#include <kernel/task.h>
#include <kernel/syscalls.h>
#include <kernel/vfs.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/string.h>
#include <kernel/userspace.h>
#include <kernel/kprintf.h>
#include <kernel/fat32.h>
#include <kernel/ext2.h>
#include <kernel/timer.h>
#include <kernel/dirent.h>
#include <kernel/tty.h>
#include <kernel/null.h>
#include <kernel/display.h>
#include <kernel/virtio_net.h>
#include <kernel/net/control.h>
#include <kernel/spinlock.h>
#include <kernel/input.h>

#define SYSCALL_IO_BOUNCE_SIZE     (64u * 1024u)
#define SYSCALL_IO_BOUNCE_MIN_SIZE (4u * 1024u)


/* Forward declarations de toutes les fonctions statiques */
static bool check_file_permission(inode_t* inode, int flags);
extern int fat32_file_exists_in_dir(inode_t* dir_inode, const char* filename);
extern inode_t* fat32_create_file(const char* parent_path, const char* filename, mode_t mode);
extern void fat32_free_cluster_chain(uint32_t start_cluster);
extern int fat32_update_file_by_name(const char* filename, uint32_t parent_cluster, uint32_t new_cluster);
extern int fat32_update_file_size_in_dir(const char* filename, uint32_t parent_cluster, uint32_t new_size);
extern file_operations_t fat32_file_ops;
extern inode_operations_t fat32_inode_ops;
extern file_operations_t ext2_file_ops;
extern inode_operations_t ext2_inode_ops;

static spinlock_t file_ref_lock = SPINLOCK_INIT("file_ref");

bool inode_permission(inode_t* inode, int mask) {
    /* Root peut tout faire */
    if (current_uid() == 0) return true;
    
    /* Propriétaire du fichier */
    if (current_uid() == inode->uid) {
        if ((mask & MAY_READ) && !(inode->mode & 0400)) return false;
        if ((mask & MAY_WRITE) && !(inode->mode & 0200)) return false;
        if ((mask & MAY_EXEC) && !(inode->mode & 0100)) return false;
        return true;
    }
    
    /* Groupe du fichier */
    if (current_gid() == inode->gid) {
        if ((mask & MAY_READ) && !(inode->mode & 0040)) return false;
        if ((mask & MAY_WRITE) && !(inode->mode & 0020)) return false;
        if ((mask & MAY_EXEC) && !(inode->mode & 0010)) return false;
        return true;
    }
    
    /* Autres utilisateurs */
    if ((mask & MAY_READ) && !(inode->mode & 0004)) return false;
    if ((mask & MAY_WRITE) && !(inode->mode & 0002)) return false;
    if ((mask & MAY_EXEC) && !(inode->mode & 0001)) return false;
    
    return true;
}

static int file_read_buffer(file_t* file, void* buf, size_t count)
{
    ssize_t result;
    void *kbuf = NULL;
    size_t read_count;

    if (count == 0) return 0;
    if (!buf) return -EFAULT;
    if (!file) return -EBADF;
    if (!can_read(file)) return -EBADF;
    if (!file->f_op || !file->f_op->read) return -ENOSYS;

    /*
     * VFS read methods operate on kernel buffers.  User reads are bounced
     * through kernel memory and copied out with page-aware validation; this
     * prevents a partially unmapped user buffer from aborting the kernel
     * inside a filesystem or driver memcpy().
     */
    if (is_kernel_pointer(buf)) {
        result = file->f_op->read(file, buf, count);
        return (int)result;
    }

    read_count = count;
    if (read_count > SYSCALL_IO_BOUNCE_SIZE)
        read_count = SYSCALL_IO_BOUNCE_SIZE;

    kbuf = kmalloc(read_count);
    while (!kbuf && read_count > SYSCALL_IO_BOUNCE_MIN_SIZE) {
        read_count /= 2u;
        if (read_count < SYSCALL_IO_BOUNCE_MIN_SIZE)
            read_count = SYSCALL_IO_BOUNCE_MIN_SIZE;
        kbuf = kmalloc(read_count);
    }
    if (!kbuf) return -ENOMEM;

    result = file->f_op->read(file, kbuf, read_count);
    if (result > 0) {
        if (copy_to_user(buf, kbuf, (size_t)result) < 0)
            result = -EFAULT;
    }

    kfree(kbuf);
    return (int)result;
}

static int file_write_buffer(file_t* file, const void* buf, size_t count)
{
    ssize_t result = 0;
    void *kbuf = NULL;
    size_t chunk_cap;
    size_t done = 0;

    if (count == 0) return 0;
    if (!buf) return -EFAULT;
    if (!file) return -EBADF;
    if (!can_write(file)) return -EBADF;
    if (!file->f_op || !file->f_op->write) return -ENOSYS;

    if (is_kernel_pointer(buf)) {
        result = file->f_op->write(file, buf, count);
        return (int)result;
    }

    /*
     * Bound the user bounce buffer like sys_read(). A raw kmalloc(count) lets
     * one user write consume an arbitrary fraction of kernel heap; chunking
     * keeps memory pressure predictable while preserving partial-write
     * semantics.
     */
    chunk_cap = count < SYSCALL_IO_BOUNCE_SIZE ? count : SYSCALL_IO_BOUNCE_SIZE;
    kbuf = kmalloc(chunk_cap);
    while (!kbuf && chunk_cap > SYSCALL_IO_BOUNCE_MIN_SIZE) {
        chunk_cap /= 2u;
        if (chunk_cap < SYSCALL_IO_BOUNCE_MIN_SIZE)
            chunk_cap = SYSCALL_IO_BOUNCE_MIN_SIZE;
        kbuf = kmalloc(chunk_cap);
    }
    if (!kbuf) return -ENOMEM;

    /*
     * Filesystem writes may block on backend locks or VirtIO completion.
     * Keep IRQs enabled here; each filesystem is responsible for protecting
     * its own metadata and data paths. Holding the global critical-section
     * flag across a write makes a legitimate ext2 wait look like a scheduler
     * violation.
     */
    while (done < count) {
        size_t chunk = count - done;

        if (chunk > chunk_cap)
            chunk = chunk_cap;

        if (copy_from_user(kbuf, (const uint8_t*)buf + done, chunk) < 0) {
            if (done == 0) {
                kfree(kbuf);
                return -EFAULT;
            }
            break;
        }

        result = file->f_op->write(file, kbuf, chunk);
        if (result < 0) {
            if (done == 0) {
                kfree(kbuf);
                return (int)result;
            }
            break;
        }

        done += (size_t)result;
        if ((size_t)result < chunk)
            break;              /* Short write: return the partial count. */
    }

    kfree(kbuf);
    return (int)done;
}

int sys_read(int fd, void* buf, size_t count)
{
    task_t* task = task_current_local();

    if (count == 0) return 0;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;
    return file_read_buffer(task->process->files[fd], buf, count);
}

int sys_write(int fd, const void* buf, size_t count)
{
    task_t* task = task_current_local();

    if (count == 0) return 0;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;
    return file_write_buffer(task->process->files[fd], buf, count);
}

int kernel_write(int fd, const void* kernel_buf, size_t count)
{
    task_t* task = task_current_local();

    if (count == 0) return 0;
    if (!kernel_buf || !is_kernel_pointer(kernel_buf)) return -EFAULT;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;
    return file_write_buffer(task->process->files[fd], kernel_buf, count);
}

static int positioned_file(int fd, const armos_offset_t* user_offset,
                           file_t* positioned)
{
    task_t* task = task_current_local();
    armos_offset_t offset;
    file_t* file;

    if (!user_offset || !positioned) return -EFAULT;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;

    file = task->process->files[fd];
    if (!file) return -EBADF;
    if (!file->f_op || !file->f_op->lseek) return -ESPIPE;
    if (copy_from_user(&offset, user_offset, sizeof(offset)) < 0)
        return -EFAULT;
    if (offset.value < 0 ||
        (unsigned long long)offset.value > ARMOS_FILE_OFFSET_MAX)
        return -EINVAL;

    *positioned = *file;
    positioned->offset = (uint32_t)offset.value;
    return 0;
}

int sys_pread(int fd, void* buf, size_t count,
              const armos_offset_t* offset)
{
    file_t positioned;
    int ret;

    ret = positioned_file(fd, offset, &positioned);
    if (ret < 0) return ret;
    return file_read_buffer(&positioned, buf, count);
}

int sys_pwrite(int fd, const void* buf, size_t count,
               const armos_offset_t* offset)
{
    file_t positioned;
    int ret;

    ret = positioned_file(fd, offset, &positioned);
    if (ret < 0) return ret;

    /* POSIX pwrite() uses its explicit offset even on an O_APPEND handle. */
    positioned.flags &= ~O_APPEND;
    return file_write_buffer(&positioned, buf, count);
}

static int truncate_file_inode(inode_t *inode, const char *name)
{
    if (!inode || !name) return -EINVAL;
    if (S_ISDIR(inode->mode)) return -EISDIR;

    if (inode->first_cluster != 0) {
        fat32_free_cluster_chain(inode->first_cluster);
        inode->first_cluster = 0;
        inode->blocks = 0;
        fat32_update_file_by_name(name, inode->parent_cluster, 0);
    }

    inode->size = 0;
    inode->mtime = get_current_time();
    return fat32_update_file_size_in_dir(name, inode->parent_cluster, 0);
}

int sys_close(int fd)
{
    task_t* task = task_current_local();
    file_t* file;

    //KDEBUG("[CLOSE] fd=%d\n", fd);

    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;

    file = vfs_take_file(task, fd);
    if (!file) return -EBADF;

    close_file(file);
    return 0;
}

int sys_ftruncate(int fd, off_t length)
{
    task_t* task = task_current_local();
    file_t* file;
    int result;

    if (length < 0) return -EINVAL;
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;

    file = task->process->files[fd];
    if (!file) return -EBADF;
    if (!can_write(file)) return -EBADF;
    if (!file->inode) return -EINVAL;
    if (S_ISDIR(file->inode->mode)) return -EISDIR;
    if (!file->f_op || !file->f_op->truncate) return -ENOSYS;

    /*
     * Truncate may update filesystem metadata and wait for disk completion.
     * The filesystem backend owns its locking; the syscall layer must not
     * wrap it in a global non-schedulable section.
     */
    result = file->f_op->truncate(file, length);

    return result;
}

int sys_truncate(const char* pathname, off_t length)
{
    int fd;
    int ret;

    if (length < 0)
        return -EINVAL;

    fd = sys_open(pathname, O_WRONLY, 0);
    if (fd < 0)
        return fd;

    ret = sys_ftruncate(fd, length);
    sys_close(fd);
    return ret;
}

static int sync_file_descriptor(int fd)
{
    task_t* task = task_current_local();
    file_t* file;

    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    if (!task || !task->process)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;

    /*
     * ArmOS does not yet expose per-file sync callbacks. A global VFS sync is
     * conservative and correct for ext2/fat32 persistence, just less granular
     * than Linux fsync(2).
     */
    return vfs_sync();
}

int sys_fsync(int fd)
{
    return sync_file_descriptor(fd);
}

int sys_fdatasync(int fd)
{
    /* The current VFS sync is conservative and may flush extra metadata. */
    return sync_file_descriptor(fd);
}

/**
 * Séparer un chemin en répertoire parent et nom de fichier
 */
int split_path(const char* full_path, char** parent_path, char** filename) {
    int len = strlen(full_path);
    int i;
    
    /* Trouver le dernier '/' */
    for (i = len - 1; i >= 0; i--) {
        if (full_path[i] == '/') break;
    }
    
    if (i < 0) {
        /* Pas de '/', fichier dans le répertoire courant */
        *parent_path = strdup(".");
        *filename = strdup(full_path);
    } else if (i == 0) {
        /* Fichier dans la racine */
        *parent_path = strdup("/");
        *filename = strdup(full_path + 1);
    } else {
        /* Fichier dans un sous-répertoire */
        *parent_path = kmalloc(i + 1);
        if (!*parent_path) return -ENOMEM;
        
        strncpy(*parent_path, full_path, i);
        (*parent_path)[i] = '\0';
        
        *filename = strdup(full_path + i + 1);
    }
    
    if (!*filename) {
        kfree(*parent_path);
        return -ENOMEM;
    }
    
    return 0;
}

static int install_open_file(task_t *task, inode_t *inode, int flags,
                             const char *opened_name, const char *opened_path)
{
    file_t *file;
    int fd;

    fd = allocate_fd(task);
    if (fd < 0) {
        put_inode(inode);
        return -EMFILE;
    }
    file = create_file();
    if (!file) {
        put_inode(inode);
        return -ENOMEM;
    }
    file->inode = inode;
    file->flags = flags & ~O_CLOEXEC;
    file->offset = flags & O_APPEND ? inode->size : 0;
    file->f_op = inode->f_op;
    vfs_inode_opened(inode);
    strncpy(file->name, opened_name, sizeof(file->name) - 1u);
    file->name[sizeof(file->name) - 1u] = '\0';
    strncpy(file->path, opened_path, sizeof(file->path) - 1u);
    file->path[sizeof(file->path) - 1u] = '\0';

    if (file->f_op && file->f_op->open) {
        int result = file->f_op->open(inode, file);

        if (result < 0) {
            close_file(file);
            return result;
        }
    }
    task->process->files[fd] = file;
    task->process->fd_flags[fd] = flags & O_CLOEXEC;
    return fd;
}

int kernel_open_existing(char* kernel_path, int flags)
{
    task_t *task = task_current_local();
    inode_t *inode;
    char opened_name[256];
    char *slash;
    const char *base;

    if (!task || !task->process || !kernel_path)
        return -EINVAL;
    slash = strrchr(kernel_path, '/');
    base = slash ? slash + 1 : kernel_path;
    strncpy(opened_name, base, sizeof(opened_name) - 1u);
    opened_name[sizeof(opened_name) - 1u] = '\0';

    inode = flags & O_NOFOLLOW ? path_lookup_ex(kernel_path, false) :
                                path_lookup(kernel_path);
    if (!inode) {
        kfree(kernel_path);
        return -ENOENT;
    }
    if ((flags & O_NOFOLLOW) && S_ISLNK(inode->mode)) {
        put_inode(inode);
        kfree(kernel_path);
        return -ELOOP;
    }
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
        put_inode(inode);
        kfree(kernel_path);
        return -EEXIST;
    }
    if (!check_file_permission(inode, flags)) {
        put_inode(inode);
        kfree(kernel_path);
        return -EACCES;
    }
    if ((flags & O_DIRECTORY) && !S_ISDIR(inode->mode)) {
        put_inode(inode);
        kfree(kernel_path);
        return -ENOTDIR;
    }
    if (flags & O_TRUNC) {
        put_inode(inode);
        kfree(kernel_path);
        return -EROFS;
    }
    {
        int result = install_open_file(task, inode, flags, opened_name,
                                       kernel_path);
        kfree(kernel_path);
        return result;
    }
}

int kernel_open(char* kernel_path, int flags, mode_t mode)
{
    task_t* task = task_current_local();
    inode_t* inode;
    file_t* file;
    int fd;
    char* filename = NULL;
    char opened_name[256];
    char opened_path[MAX_PATH];
    bool opened_existing;

    opened_name[0] = '\0';
    strncpy(opened_path, kernel_path, sizeof(opened_path) - 1u);
    opened_path[sizeof(opened_path) - 1u] = '\0';
    
    /* Suppression du warning unused parameter */
    (void)mode;

    if (!(flags & (O_CREAT | O_TRUNC)))
        return kernel_open_existing(kernel_path, flags);
    
    /* Find inode */
    inode = (flags & O_NOFOLLOW) ? path_lookup_ex(kernel_path, false)
                                 : path_lookup(kernel_path);
    opened_existing = inode != NULL;
    //kfree(kernel_path);
    
/*     if (!inode) {
        if (flags & O_CREAT) {
            // TODO: Create file 
            return -ENOSYS;
        }
        return -ENOENT;
    } */

    {
        char* slash = strrchr(kernel_path, '/');
        const char* base = slash ? slash + 1 : kernel_path;
        strncpy(opened_name, base, sizeof(opened_name) - 1);
        opened_name[sizeof(opened_name) - 1] = '\0';
    }

    if (inode && (flags & O_NOFOLLOW) && S_ISLNK(inode->mode)) {
        put_inode(inode);
        kfree(kernel_path);
        return -ELOOP;
    }

    if (!inode) {
        //KDEBUG("kernel_open: INODE IS NULL for %s\n", kernel_path);
        if (flags & O_CREAT) {
            char* parent_path;

            if (flags & O_DIRECTORY) {
                kfree(kernel_path);
                return -EINVAL;
            }
            
            /* Séparer le chemin */
            if (split_path(kernel_path, &parent_path, &filename) != 0) {
                kfree(kernel_path);
                return -ENOMEM;
            }
            
            /* Vérifier que le fichier n'existe pas déjà */
            inode_t* parent = path_lookup(parent_path);
            if (!parent) {
                kfree(parent_path);
                kfree(filename);
                kfree(kernel_path);
                return -ENOENT;
            }

            if (!S_ISDIR(parent->mode)) {
                put_inode(parent);
                kfree(parent_path);
                kfree(filename);
                kfree(kernel_path);
                return -ENOTDIR;
            }

            if (!inode_permission(parent, MAY_WRITE | MAY_EXEC)) {
                put_inode(parent);
                kfree(parent_path);
                kfree(filename);
                kfree(kernel_path);
                return -EACCES;
            }

            if (!filename[0] || strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0) {
                put_inode(parent);
                kfree(parent_path);
                kfree(filename);
                kfree(kernel_path);
                return -EINVAL;
            }

            if (parent->i_op != &fat32_inode_ops && parent->i_op != &ext2_inode_ops) {
                put_inode(parent);
                kfree(parent_path);
                kfree(filename);
                kfree(kernel_path);
                return -EROFS;
            }

            if (parent->i_op == &fat32_inode_ops && fat32_file_exists_in_dir(parent, filename)) {
                if (flags & O_EXCL) {
                    /* O_CREAT | O_EXCL = échec si existe */
                    put_inode(parent);
                    kfree(parent_path);
                    kfree(filename);
                    kfree(kernel_path);
                    return -EEXIST;
                }
                
                /* Fichier existe, l'ouvrir normalement */
                char* full_path_again = kernel_path; 
                inode = path_lookup(full_path_again);
                put_inode(parent);
                kfree(parent_path);
                kfree(filename);
                filename = NULL;
            } else {
                /* Créer le nouveau fichier */
                if (task && task->process)
                    mode &= ~task->process->umask;

                if (parent->i_op == &fat32_inode_ops) {
                    inode = fat32_create_file(parent_path, filename, mode);
                } else {
                    inode = ext2_create_file(parent, filename, mode);
                }

                if (parent) put_inode(parent);
                kfree(parent_path);
                
                if (!inode) {
                    kfree(kernel_path);
                    return -EIO;  /* Échec création */
                }
            }
        } else {
            kfree(kernel_path);
            return -ENOENT;
        }
    }
    else{
        // File exists
        // If O_CREAT && 0_EXCL then return error
        if((flags & O_CREAT) && (flags & O_EXCL)) {
            kfree(kernel_path);
            return -EEXIST;
        }

    }
    
    kfree(kernel_path);
    
    /* Check permissions */
    if (!check_file_permission(inode, flags)) {
        put_inode(inode);
        return -EACCES;
    }

    if ((flags & O_DIRECTORY) && !S_ISDIR(inode->mode)) {
        put_inode(inode);
        return -ENOTDIR;
    }

    if (opened_existing &&
        (flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY)) {
        int truncate_result;

        if (inode->f_op == &fat32_file_ops) {
            truncate_result = truncate_file_inode(inode, opened_name);
        } else if (inode->f_op == &ext2_file_ops) {
            truncate_result = ext2_truncate_inode(inode);
        } else if (inode->f_op && inode->f_op->write) {
            /*
             * Pseudo files such as /proc control endpoints and character
             * devices do not have persistent contents to truncate. Shell
             * redirection still uses O_TRUNC, so treat it as a no-op when the
             * target is writable but not backed by a disk filesystem.
             */
            truncate_result = 0;
        } else {
            truncate_result = -EROFS;
        }

        if (truncate_result < 0) {
            put_inode(inode);
            return truncate_result;
        }
    }
    
    /* Allocate file descriptor */
    fd = allocate_fd(task);
    if (fd < 0) {
        put_inode(inode);
        return -EMFILE;
    }
    
    /* Create file structure */
    file = create_file();
    if (!file) {
        put_inode(inode);
        return -ENOMEM;
    }
    
    file->inode = inode;
    file->flags = flags & ~O_CLOEXEC;
    vfs_inode_opened(inode);

    if (flags & O_APPEND) {
        //KDEBUG("APPEND FLAG DETECTED offset = %d...\n", inode->size);
        file->offset = inode->size;
    }
    else {
        file->offset = 0;
    }

    file->f_op = inode->f_op;
    strncpy(file->path, opened_path, sizeof(file->path) - 1u);
    file->path[sizeof(file->path) - 1u] = '\0';

    if (filename) {
        strcpy(file->name, filename);
        kfree(filename);
    } else {
        strncpy(file->name, opened_name, sizeof(file->name) - 1);
        file->name[sizeof(file->name) - 1] = '\0';
    }
    
    /* Open file */
    if (file->f_op && file->f_op->open) {
        int result = file->f_op->open(inode, file);
        //KDEBUG("kernel_open: File opnened %s with result = %d\n", file->name, result);

        if (result < 0) {
            close_file(file);
            return result;
        }
    }
    
    task->process->files[fd] = file;
    task->process->fd_flags[fd] = flags & O_CLOEXEC;
    return fd;
}

char *get_current_working_directory(void){

    task_t *task = task_current_local();
    
    if(!task || !task->process) return NULL;

    return strdup(task->process->cwd);
}


/* Normalise un chemin absolu en place : résout . et .. composant par composant. */
void path_canonicalize(char *path) {
    const char *segs[MAX_PATH / 2 + 1];
    int depth = 0;
    const char *p = path + 1;

    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);

        if (len == 0 || (len == 1 && seg[0] == '.')) {
            /* ignore . et slashs multiples */
        } else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (depth > 0) depth--;   /* remonte d'un niveau (plancher = racine) */
        } else {
            segs[depth++] = seg;
        }
        if (*p == '/') p++;
    }

    /* Reconstruction en place (toujours plus court ou égal à l'original) */
    char *out = path + 1;
    for (int i = 0; i < depth; i++) {
        if (i > 0) *out++ = '/';
        const char *s = segs[i];
        while (*s && *s != '/') *out++ = *s++;
    }
    *out = '\0';
}

int resolve_path_at(int dirfd, const char* path, char** resolved)
{
    task_t *task = task_current_local();
    const char *base;
    file_t *dir_file = NULL;
    char *full_path;
    size_t base_len;
    size_t path_len;
    bool add_slash;

    if (!path || !resolved)
        return -EINVAL;
    *resolved = NULL;
    if (path[0] == '\0')
        return -ENOENT;

    /* POSIX ignores dirfd when pathname is already absolute. */
    if (path[0] == '/') {
        if (strlen(path) >= MAX_PATH)
            return -ENAMETOOLONG;
        full_path = strdup(path);
        if (!full_path)
            return -ENOMEM;
        path_canonicalize(full_path);
        *resolved = full_path;
        return 0;
    }

    if (!task || !task->process)
        return -EINVAL;
    if (dirfd == ARMOS_AT_FDCWD) {
        base = task->process->cwd;
    } else {
        if (dirfd < 0 || dirfd >= MAX_FILES)
            return -EBADF;
        dir_file = task->process->files[dirfd];
        if (!dir_file)
            return -EBADF;
        if (!dir_file->inode || !S_ISDIR(dir_file->inode->mode))
            return -ENOTDIR;
        if (dir_file->path[0] == '\0')
            return -EBADF;
        base = dir_file->path;
    }

    base_len = strlen(base);
    path_len = strlen(path);
    add_slash = base_len == 0 || base[base_len - 1u] != '/';
    if (base_len + (add_slash ? 1u : 0u) + path_len + 1u > MAX_PATH)
        return -ENAMETOOLONG;

    full_path = kmalloc(base_len + (add_slash ? 1u : 0u) + path_len + 1u);
    if (!full_path)
        return -ENOMEM;
    strcpy(full_path, base);
    if (add_slash)
        strcat(full_path, "/");
    strcat(full_path, path);
    path_canonicalize(full_path);
    *resolved = full_path;
    return 0;
}

char* resolve_path(const char* path)
{
    char *resolved;

    if (resolve_path_at(ARMOS_AT_FDCWD, path, &resolved) < 0)
        return NULL;
    return resolved;
}

int vfs_check_search_permission(const char* path, bool include_final)
{
    char current_path[MAX_PATH];
    char* path_copy;
    char* token;
    char* saveptr;

    if (!path || path[0] != '/')
        return -EINVAL;

    if (current_uid() == 0)
        return 0;

    inode_t* root = path_lookup("/");
    if (!root)
        return -ENOENT;
    int root_ret = inode_permission(root, MAY_EXEC) ? 0 : -EACCES;
    put_inode(root);
    if (root_ret < 0 || strcmp(path, "/") == 0)
        return include_final ? root_ret : 0;

    path_copy = strdup(path);
    if (!path_copy)
        return -ENOMEM;

    current_path[0] = '\0';
    saveptr = NULL;
    token = strtok_r(path_copy + 1, "/", &saveptr);

    while (token) {
        char* next = strtok_r(NULL, "/", &saveptr);
        bool is_final = (next == NULL);

        if (is_final && !include_final)
            break;

        if (current_path[0] == '\0') {
            snprintf(current_path, sizeof(current_path), "/%s", token);
        } else {
            size_t len = strlen(current_path);
            if (len + 1 + strlen(token) >= sizeof(current_path)) {
                kfree(path_copy);
                return -EINVAL;
            }
            strcat(current_path, "/");
            strcat(current_path, token);
        }

        inode_t* inode = path_lookup(current_path);
        if (!inode) {
            kfree(path_copy);
            return -ENOENT;
        }

        if (!S_ISDIR(inode->mode)) {
            put_inode(inode);
            kfree(path_copy);
            return -ENOTDIR;
        }

        if (!inode_permission(inode, MAY_EXEC)) {
            put_inode(inode);
            kfree(path_copy);
            return -EACCES;
        }

        put_inode(inode);
        token = next;
    }

    kfree(path_copy);
    return 0;
}

int sys_open_vfs(const char* pathname, int flags, mode_t mode)
{
    char *kernel_path;
    char *full_path;
    int search_result;

    (void)mode;
    if (!task_current_local() || !task_current_local()->process)
        return -EINVAL;
    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    full_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!full_path)
        return -ENOENT;
    search_result = vfs_check_search_permission(full_path, false);
    if (search_result < 0) {
        kfree(full_path);
        return search_result;
    }
    if (flags & (O_CREAT | O_TRUNC)) {
        kfree(full_path);
        return -EROFS;
    }
    return kernel_open_existing(full_path, flags);
}

static int sys_open_resolved(task_t *task, char *full_path,
                             int flags, mode_t mode)
{
    file_t* tty_file;
    file_t* null_file;
    file_t* fb_file;
    file_t* net_echo_file;
    file_t* net_control_file;

    int fd;

    /* Suppression du warning unused parameter */
    (void)mode;

    if (!task || !task->process || !full_path) {
        kfree(full_path);
        return -EINVAL;
    }

    int search_ret = vfs_check_search_permission(full_path, false);
    if (search_ret < 0) {
        kfree(full_path);
        return search_ret;
    }

    if (is_tty_device_path(full_path)) {
        int tty_id = tty_id_from_device_path(full_path);

        if (tty_id < 0) {
            int ret = tty_id;
            kfree(full_path);
            return ret;
        }

        int tty_open_ret = tty_check_open_for_id(tty_id, flags);

        if (tty_open_ret < 0) {
            kfree(full_path);
            return tty_open_ret;
        }

        fd = allocate_fd(task);
        if (fd < 0) {
            kfree(full_path);
            return fd;
        }

        tty_file = create_tty_console_file(
            strcmp(full_path, "/dev/tty") == 0 ? "tty" :
            strcmp(full_path, "/dev/console") == 0 ? "console" :
            strcmp(full_path, "/dev/tty1") == 0 ? "tty1" :
            strcmp(full_path, "/dev/ttyS0") == 0 ? "ttyS0" : "tty0",
            flags & ~O_CLOEXEC);
        if (!tty_file) {
            free_fd(task, fd);
            kfree(full_path);
            return -ENOMEM;
        }

        task->process->files[fd] = tty_file;
        task->process->fd_flags[fd] = flags & O_CLOEXEC;
        kfree(full_path);
        return fd;
    }

    if (is_null_device_path(full_path)) {
        if (flags & O_DIRECTORY) {
            kfree(full_path);
            return -ENOTDIR;
        }

        fd = allocate_fd(task);
        if (fd < 0) {
            kfree(full_path);
            return fd;
        }

        null_file = create_null_device_file("null", flags & ~O_CLOEXEC);
        if (!null_file) {
            free_fd(task, fd);
            kfree(full_path);
            return -ENOMEM;
        }

        task->process->files[fd] = null_file;
        task->process->fd_flags[fd] = flags & O_CLOEXEC;
        kfree(full_path);
        return fd;
    }

    if (is_framebuffer_device_path(full_path)) {
        if (flags & O_DIRECTORY) {
            kfree(full_path);
            return -ENOTDIR;
        }

        fd = allocate_fd(task);
        if (fd < 0) {
            kfree(full_path);
            return fd;
        }

        fb_file = create_framebuffer_device_file(
            strcmp(full_path, "/dev/fb1") == 0 ? "fb1" : "fb0",
            flags & ~O_CLOEXEC);
        if (!fb_file) {
            free_fd(task, fd);
            kfree(full_path);
            return -ENODEV;
        }

        task->process->files[fd] = fb_file;
        task->process->fd_flags[fd] = flags & O_CLOEXEC;
        kfree(full_path);
        return fd;
    }

    if (is_input_device_path(full_path)) {
        file_t *input_file;

        if (flags & O_DIRECTORY) {
            kfree(full_path);
            return -ENOTDIR;
        }
        if (current_uid() != 0) {
            kfree(full_path);
            return -EACCES;
        }
        fd = allocate_fd(task);
        if (fd < 0) {
            kfree(full_path);
            return fd;
        }
        input_file = create_input_device_file("input0",
                                               flags & ~O_CLOEXEC);
        if (!input_file) {
            free_fd(task, fd);
            kfree(full_path);
            return -ENOMEM;
        }
        task->process->files[fd] = input_file;
        task->process->fd_flags[fd] = flags & O_CLOEXEC;
        kfree(full_path);
        return fd;
    }

    if (is_net_echo_device_path(full_path)) {
        if (flags & O_DIRECTORY) {
            kfree(full_path);
            return -ENOTDIR;
        }

        fd = allocate_fd(task);
        if (fd < 0) {
            kfree(full_path);
            return fd;
        }

        net_echo_file = create_net_echo_device_file("netecho", flags & ~O_CLOEXEC);
        if (!net_echo_file) {
            free_fd(task, fd);
            kfree(full_path);
            return -ENODEV;
        }

        task->process->files[fd] = net_echo_file;
        task->process->fd_flags[fd] = flags & O_CLOEXEC;
        kfree(full_path);
        return fd;
    }

    if (is_net_control_device_path(full_path)) {
        if (flags & O_DIRECTORY) {
            kfree(full_path);
            return -ENOTDIR;
        }

        fd = allocate_fd(task);
        if (fd < 0) {
            kfree(full_path);
            return fd;
        }

        net_control_file = create_net_control_device_file(
            "netctl", flags & ~O_CLOEXEC);
        if (!net_control_file) {
            free_fd(task, fd);
            kfree(full_path);
            return -ENODEV;
        }

        task->process->files[fd] = net_control_file;
        task->process->fd_flags[fd] = flags & O_CLOEXEC;
        kfree(full_path);
        return fd;
    }

    if (flags & O_CREAT)
        vfs_begin_mutation();
    fd = kernel_open(full_path, flags, mode);
    if (flags & O_CREAT)
        vfs_end_mutation();

    return fd;
}

int sys_open(const char* pathname, int flags, mode_t mode)
{
    task_t *task = task_current_local();
    char *kernel_path;
    char *full_path;
    int result;

    if (!task || !task->process)
        return -EINVAL;
    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(ARMOS_AT_FDCWD, kernel_path, &full_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    return sys_open_resolved(task, full_path, flags, mode);
}

int sys_openat(int dirfd, const char* pathname, int flags, mode_t mode)
{
    task_t *task = task_current_local();
    char *kernel_path;
    char *full_path;
    int result;

    if (!task || !task->process)
        return -EINVAL;
    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(dirfd, kernel_path, &full_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    return sys_open_resolved(task, full_path, flags, mode);
}

int sys_creat(const char* pathname, mode_t mode)
{
    return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

off_t sys_lseek(int fd, off_t offset, int whence)
{
    task_t* task = task_current_local();
    file_t* file;
    off_t new_offset;
    
    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;
    
    file = task->process->files[fd];
    if (!file) return -EBADF;
    
    if (file->f_op && file->f_op->lseek) {
        return file->f_op->lseek(file, offset, whence);
    }
    
    /* Default lseek implementation */
    switch (whence) {
        case SEEK_SET:
            new_offset = offset;
            break;
        case SEEK_CUR:
            new_offset = file->offset + offset;
            break;
        case SEEK_END:
            new_offset = (off_t)file->inode->size + offset;
            break;
        default:
            return -EINVAL;
    }
    
    if (new_offset < 0) {
        return -EINVAL;
    }
    
    file->offset = new_offset;
    return new_offset;
}

static void fill_stat_from_inode(struct stat* kstat, inode_t* inode)
{
    memset(kstat, 0, sizeof(*kstat));
    kstat->st_dev = 0;      /* TODO: expose VFS mount/device id */
    kstat->st_ino = inode->first_cluster ? inode->first_cluster : inode->ino;
    kstat->st_mode = inode->mode;
    kstat->st_nlink = inode->nlink ? inode->nlink : 1;
    kstat->st_uid = inode->uid;
    kstat->st_gid = inode->gid;
    kstat->st_rdev = (S_ISCHR(inode->mode) || S_ISBLK(inode->mode)) ? inode->parent_cluster : 0;
    kstat->st_size = inode->size;
    kstat->st_blksize = 1024;
    kstat->st_blocks = (S_ISCHR(inode->mode) || S_ISBLK(inode->mode)) ? 0 :
        (inode->blocks ? inode->blocks : (inode->size + 511) / 512);
    kstat->st_atime = inode->atime;
    kstat->st_mtime = inode->mtime;
    kstat->st_ctime = inode->ctime;
}

static int sys_stat_vfs_impl(const char *pathname, struct stat *statbuf,
                             bool follow_final_symlink)
{
    char *kernel_path;
    char *full_path;
    inode_t *inode;
    struct stat kernel_stat;
    int search_result;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    full_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!full_path)
        return -ENOENT;
    search_result = vfs_check_search_permission(full_path, false);
    if (search_result < 0) {
        kfree(full_path);
        return search_result;
    }
    inode = path_lookup_ex(full_path, follow_final_symlink);
    kfree(full_path);
    if (!inode)
        return -ENOENT;
    fill_stat_from_inode(&kernel_stat, inode);
    if (copy_to_user(statbuf, &kernel_stat, sizeof(kernel_stat)) < 0) {
        put_inode(inode);
        return -EFAULT;
    }
    put_inode(inode);
    return 0;
}

int sys_stat_vfs(const char *pathname, struct stat *statbuf)
{
    return sys_stat_vfs_impl(pathname, statbuf, true);
}

int sys_lstat_vfs(const char *pathname, struct stat *statbuf)
{
    return sys_stat_vfs_impl(pathname, statbuf, false);
}

static int stat_resolved_path(char *full_path, struct stat *kstat,
                              bool follow_final_symlink)
{
    inode_t *inode;
    int search_ret;

    search_ret = vfs_check_search_permission(full_path, false);
    if (search_ret < 0) {
        kfree(full_path);
        return search_ret;
    }

    if (is_null_device_path(full_path)) {
        fill_null_device_stat(kstat);
        kfree(full_path);
        return 0;
    }

    if (is_tty_device_path(full_path)) {
        fill_tty_device_stat(full_path, kstat);
        kfree(full_path);
        return 0;
    }

    if (is_framebuffer_device_path(full_path)) {
        fill_framebuffer_device_stat(full_path, kstat);
        kfree(full_path);
        return 0;
    }

    if (is_input_device_path(full_path)) {
        fill_input_device_stat(kstat);
        kfree(full_path);
        return 0;
    }

    if (is_net_echo_device_path(full_path)) {
        fill_net_echo_device_stat(kstat);
        kfree(full_path);
        return 0;
    }

    if (is_net_control_device_path(full_path)) {
        fill_net_control_device_stat(kstat);
        kfree(full_path);
        return 0;
    }

    inode = path_lookup_ex(full_path, follow_final_symlink);
    kfree(full_path);
    if (!inode)
        return -ENOENT;
    fill_stat_from_inode(kstat, inode);
    put_inode(inode);
    return 0;
}

static int stat_user_path_at(int dirfd, const char *pathname,
                             struct stat *statbuf,
                             bool follow_final_symlink)
{
    char *kernel_path;
    char *full_path;
    struct stat kstat;
    int result;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(dirfd, kernel_path, &full_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    result = stat_resolved_path(full_path, &kstat, follow_final_symlink);
    if (result < 0)
        return result;
    if (copy_to_user(statbuf, &kstat, sizeof(kstat)) < 0)
        return -EFAULT;
    return 0;
}

int sys_stat(const char* pathname, struct stat* statbuf)
{
    return stat_user_path_at(ARMOS_AT_FDCWD, pathname, statbuf, true);
}

int sys_lstat(const char* pathname, struct stat* statbuf)
{
    return stat_user_path_at(ARMOS_AT_FDCWD, pathname, statbuf, false);
}

int sys_fstatat(int dirfd, const char* pathname, struct stat* statbuf,
                int flags)
{
    if (flags & ~ARMOS_AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    return stat_user_path_at(dirfd, pathname, statbuf,
                             !(flags & ARMOS_AT_SYMLINK_NOFOLLOW));
}

int sys_fstat(int fd, struct stat* statbuf)
{

    task_t* task = task_current_local();
    inode_t* inode;
    struct stat kstat;
    file_t* file;

    if (fd < 0 || fd >= MAX_FILES) return -EBADF;
    if (!task || !task->process) return -EBADF;
    
    file = task->process->files[fd];
    if (!file) return -EBADF;
    
    inode = file->inode;
    if (!inode) return -ENOENT;
    
    fill_stat_from_inode(&kstat, inode);
    
    /* Copy to user space */
    if (copy_to_user(statbuf, &kstat, sizeof(struct stat)) < 0) {
        return -EFAULT;
    }
    
    return 0;
}

/* Helper functions */
file_t* create_file(void)
{
    file_t* file = kmalloc(sizeof(file_t));
    if (file) {
        memset(file, 0, sizeof(file_t));
        file->type = FILE_TYPE_REGULAR;
        file->ref_count = 1;
    }
    return file;
}

bool file_is_tty(file_t* file)
{
    return file && file->type == FILE_TYPE_TTY;
}

file_t* get_file(file_t* file)
{
    unsigned long flags;

    if (!file) return NULL;

    spin_lock_irqsave(&file_ref_lock, &flags);
    if (file->ref_count == 0) {
        spin_unlock_irqrestore(&file_ref_lock, flags);
        return NULL;
    }
    file->ref_count++;
    spin_unlock_irqrestore(&file_ref_lock, flags);

    return file;
}

void close_file(file_t* file)
{
    bool do_close = false;
    unsigned long flags;

    if (!file) return;

    spin_lock_irqsave(&file_ref_lock, &flags);
    if (file->ref_count == 0) {
        spin_unlock_irqrestore(&file_ref_lock, flags);
        KERROR("close_file: invalid zero refcount for file %p\n", file);
        return;
    }
    
    file->ref_count--;
    do_close = (file->ref_count == 0);
    spin_unlock_irqrestore(&file_ref_lock, flags);

    if (do_close) {
        if (file->f_op && file->f_op->close)
            file->f_op->close(file);
        if (file->inode) {
            vfs_inode_closed(file->inode);
            put_inode(file->inode);
        }
        kfree(file);
    }
}

static bool check_file_permission(inode_t* inode, int flags)
{
    int mask = 0;

    if (!inode)
        return false;

    /*
     * POSIX open() access bits are not permission bits. Translate the requested
     * file access into the inode permission model used by access() and execve().
     * O_TRUNC also needs write permission, even if a buggy caller forgot to use
     * O_WRONLY/O_RDWR.
     */
    switch (flags & O_ACCMODE) {
        case O_RDONLY:
            mask |= MAY_READ;
            break;
        case O_WRONLY:
            mask |= MAY_WRITE;
            break;
        case O_RDWR:
            mask |= MAY_READ | MAY_WRITE;
            break;
        default:
            return false;
    }

    if (flags & O_TRUNC)
        mask |= MAY_WRITE;

    return inode_permission(inode, mask);
}


int sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count) {
    task_t *task = task_current_local();
    file_t *file;
    char *kbuf = NULL;
    char *buf_ptr;
    size_t bytes_written = 0;
    unsigned int kernel_count = count;

    //KDEBUG("[GETDENTS] entry fd=%u dirp=%p count=%u\n", fd, dirp, count);

    /* Vérifier le fd */
    if (!task || !task->process || fd >= MAX_FILES || !task->process->files[fd]) {
        KERROR("[GETDENTS] EBADF fd=%u files[fd]=%p\n", fd,
               task && task->process && fd < MAX_FILES ? (void*)task->process->files[fd] : NULL);
        return -EBADF;
    }
    
    file = task->process->files[fd];
    
    /* Vérifier que c'est un répertoire */
    if (!file->inode || !S_ISDIR(file->inode->mode)) {
        return -ENOTDIR;
    }
    
    /* Vérifier que le buffer est valide */
    if (!dirp || count < sizeof(struct linux_dirent)) {
        return -EINVAL;
    }

    if (kernel_count > SYSCALL_IO_BOUNCE_SIZE)
        kernel_count = SYSCALL_IO_BOUNCE_SIZE;
    
    //KDEBUG("Reading entries from %s fd=%d\n", file->name, fd);

    /* Vérifier que f_op et readdir sont valides */
    if (!file->f_op || !file->f_op->readdir) {
        KERROR("[GETDENTS] fd=%u: f_op=%p readdir=%p\n",
               fd, file->f_op,
               file->f_op ? (void*)file->f_op->readdir : NULL);
        return -ENOSYS;
    }

    kbuf = kmalloc(kernel_count);
    if (!kbuf)
        return -ENOMEM;
    buf_ptr = kbuf;

    //KDEBUG("[GETDENTS] fd=%u file=%p inode=%p f_op=%p readdir=%p\n",
    //       fd, file, file->inode, file->f_op, file->f_op->readdir);

    /* Lire les entrées du répertoire */
    while (bytes_written < kernel_count) {
        struct dirent entry;
        struct linux_dirent *dirent;
        size_t name_len;
        size_t rec_len;
        uint32_t saved_offset;

        /* Lire une entrée via le VFS */
        saved_offset = file->offset;
        ssize_t ret = file->f_op->readdir(file, &entry);
        
        if (ret <= 0) {
            /* Fin du répertoire ou erreur */
            break;
        }

        /* Calculer la taille de l'entrée */
        name_len = strlen(entry.d_name);
        rec_len = offsetof(struct linux_dirent, d_name) + name_len + 1 ;
        rec_len = (rec_len + 7) & ~7;  /* Aligner sur 4 bytes */

        /* Vérifier qu'il reste assez de place */
        if (bytes_written + rec_len > kernel_count) {
            /* Reculer la position de lecture pour cette entrée */
            file->offset = saved_offset;
            break;
        }
        
        /* Construire l'entrée linux_dirent */
        dirent = (struct linux_dirent *)buf_ptr;
        
        dirent->d_ino = entry.d_ino;
        dirent->d_off = file->offset;
        dirent->d_reclen = rec_len;
        dirent->d_type = entry.d_type;
        
        /* Copier le nom */
        memcpy(dirent->d_name, entry.d_name, name_len + 1);
        
        buf_ptr += rec_len;
        bytes_written += rec_len;
    }

    if (kbuf) {
        int ret = 0;
        if (bytes_written > 0 && copy_to_user(dirp, kbuf, bytes_written) < 0)
            ret = -EFAULT;
        kfree(kbuf);
        if (ret < 0)
            return ret;
    }
    
    return bytes_written;
}
