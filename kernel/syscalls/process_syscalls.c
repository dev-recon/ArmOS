/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/syscalls/process_syscalls.c
 * Layer: Kernel / syscall implementation
 *
 * Responsibilities:
 * - Validate user-facing syscall requests.
 * - Bridge user ABI arguments to kernel subsystems.
 *
 * Notes:
 * - Never trust user pointers; copy through checked helpers.
 */

#include <kernel/syscalls.h>
#include <kernel/arch_cpu.h>
#include <kernel/process.h>
#include <kernel/memory.h>
#include <kernel/address_space.h>
#include <kernel/vfs.h>
#include <kernel/net/armos_socket.h>
#include <kernel/shm.h>
#include <kernel/fd.h>
#include <kernel/string.h>
#include <kernel/task.h>
#include <kernel/smp.h>
#include <kernel/util.h>
#include <kernel/userspace.h>
#include <kernel/kprintf.h>
#include <kernel/file.h>
#include <kernel/timer.h>
#include <kernel/version.h>
#include <kernel/ext2.h>
#include <kernel/fat32.h>
#include <kernel/tty.h>
#include <kernel/null.h>
#include <kernel/keyboard.h>
#include <kernel/display.h>
#include <kernel/virtio_net.h>
#include <kernel/mount.h>
#include <kernel/virtio_block.h>
#include <kernel/input.h>
#include <kernel/pty.h>
#include <kernel/event_timer.h>

#define PIPE_BUF_SIZE 4096

struct tms_kernel {
    int32_t tms_utime;
    int32_t tms_stime;
    int32_t tms_cutime;
    int32_t tms_cstime;
};

struct utimbuf_kernel {
    time_t actime;
    time_t modtime;
};

#define ARMOS_PROT_READ     0x1
#define ARMOS_PROT_WRITE    0x2
#define ARMOS_PROT_EXEC     0x4

#define ARMOS_MAP_SHARED    0x01
#define ARMOS_MAP_PRIVATE   0x02
#define ARMOS_MAP_FIXED     0x10
#define ARMOS_MAP_ANON      0x20

#define ARMOS_POLLIN        0x0001
#define ARMOS_POLLPRI       0x0002
#define ARMOS_POLLOUT       0x0004
#define ARMOS_POLLERR       0x0008
#define ARMOS_POLLHUP       0x0010
#define ARMOS_POLLNVAL      0x0020

#define ARMOS_RUSAGE_SELF   0
#define ARMOS_RUSAGE_CHILDREN (-1)

#define ARMOS_IOV_MAX       64

struct pipe_buffer {
    char data[PIPE_BUF_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t count;           /* Nombre d'octets dans le buffer */
    int readers;            /* Nombre de lecteurs */
    int writers;            /* Nombre d'écrivains */
    int closed_read;        /* Côté lecture fermé */
    int closed_write;       /* Côté écriture fermé */
};

struct pipe_inode_info {
    struct pipe_buffer *buffer;
};

ssize_t pipe_read(file_t* file, void* buf, size_t count);
ssize_t pipe_write(file_t* file, const void* buf, size_t count);
int pipe_close(file_t* file);

static file_operations_t pipe_read_fops = {
    .read = pipe_read,
    .close = pipe_close,
};

static file_operations_t pipe_write_fops = {
    .write = pipe_write,
    .close = pipe_close,
};

extern char* resolve_path(const char* path);
extern inode_operations_t ext2_inode_ops;

static bool vfs_is_special_basename(const char* name)
{
    return !name || name[0] == '\0' ||
           strcmp(name, ".") == 0 ||
           strcmp(name, "..") == 0;
}

static bool vfs_path_is_self_or_descendant(const char* parent, const char* child)
{
    size_t len;

    if (!parent || !child)
        return false;

    if (strcmp(parent, "/") == 0)
        return child[0] == '/';

    len = strlen(parent);
    return strncmp(parent, child, len) == 0 &&
           (child[len] == '\0' || child[len] == '/');
}

static bool vfs_inode_has_external_refs(inode_t* inode)
{
    task_t* task;
    int checked = 0;
    bool found = false;
    unsigned long flags;

    if (!inode)
        return false;

    if (vfs_inode_open_count(inode) > 0)
        return true;

    spin_lock_irqsave(&task_lock, &flags);
    task = task_list_head;
    if (!task) {
        spin_unlock_irqrestore(&task_lock, flags);
        return false;
    }

    do {
        if (task->type == TASK_TYPE_PROCESS && task->process) {
            for (int fd = 0; fd < MAX_FILES; fd++) {
                file_t* file = task->process->files[fd];
                inode_t* open_inode = file ? file->inode : NULL;

                if (open_inode == inode) {
                    found = true;
                    break;
                }
                if (open_inode &&
                    open_inode->i_op == inode->i_op &&
                    open_inode->first_cluster == inode->first_cluster) {
                    found = true;
                    break;
                }
            }
        }

        if (found)
            break;

        task = task->next;
        checked++;
    } while (task && task != task_list_head && checked < MAX_TASKS);
    spin_unlock_irqrestore(&task_lock, flags);

    return found;
}

int sys_sync(void)
{
    return vfs_sync();
}

int sys_mount(const char* source, const char* target, const char* fstype,
              uint32_t flags, const void* data)
{
    char* ksource;
    char* ktarget;
    char* kfstype;
    char* full_target;
    int ret;

    ksource = copy_string_from_user(source);
    if (!ksource)
        return -EFAULT;

    ktarget = copy_string_from_user(target);
    if (!ktarget) {
        kfree(ksource);
        return -EFAULT;
    }

    kfstype = copy_string_from_user(fstype);
    if (!kfstype) {
        kfree(ksource);
        kfree(ktarget);
        return -EFAULT;
    }

    full_target = resolve_path(ktarget);
    if (!full_target) {
        kfree(ksource);
        kfree(ktarget);
        kfree(kfstype);
        return -ENOENT;
    }
    path_canonicalize(full_target);

    ret = vfs_mount_user(ksource, full_target, kfstype, flags, data);

    kfree(ksource);
    kfree(ktarget);
    kfree(kfstype);
    kfree(full_target);
    return ret;
}

int sys_umount(const char* target)
{
    char* ktarget;
    char* full_target;
    int ret;

    ktarget = copy_string_from_user(target);
    if (!ktarget)
        return -EFAULT;

    full_target = resolve_path(ktarget);
    kfree(ktarget);
    if (!full_target)
        return -ENOENT;

    path_canonicalize(full_target);
    ret = vfs_umount(full_target);
    kfree(full_target);
    return ret;
}

int sys_statfs(const char* path, struct statfs* buf)
{
    char* kpath;
    char* full_path;
    struct statfs st;
    int ret;

    if (!buf)
        return -EFAULT;

    kpath = copy_string_from_user(path);
    if (!kpath)
        return -EFAULT;

    full_path = resolve_path(kpath);
    kfree(kpath);
    if (!full_path)
        return -ENOENT;

    path_canonicalize(full_path);
    ret = vfs_statfs(full_path, &st);
    kfree(full_path);
    if (ret < 0)
        return ret;

    if (copy_to_user(buf, &st, sizeof(st)) < 0)
        return -EFAULT;
    return 0;
}

static int statvfs_from_path(const char* path, armos_statvfs_t* out)
{
    struct statfs st;
    int ret;

    if (!path || !out)
        return -EINVAL;

    ret = vfs_statfs(path, &st);
    if (ret < 0)
        return ret;

    memset(out, 0, sizeof(*out));
    out->f_bsize = st.f_bsize;
    out->f_frsize = st.f_frsize ? st.f_frsize : st.f_bsize;
    out->f_blocks = st.f_blocks;
    out->f_bfree = st.f_bfree;
    out->f_bavail = st.f_bavail;
    out->f_files = st.f_files;
    out->f_ffree = st.f_ffree;
    out->f_favail = st.f_ffree;
    out->f_fsid = st.f_type;
    out->f_namemax = st.f_namelen;
    return 0;
}

int sys_statvfs(const char* path, armos_statvfs_t* buf)
{
    armos_statvfs_t st;
    inode_t* inode;
    char* full_path;
    char* kpath;
    int ret;

    if (!buf)
        return -EFAULT;

    kpath = copy_string_from_user(path);
    if (!kpath)
        return -EFAULT;
    full_path = resolve_path(kpath);
    kfree(kpath);
    if (!full_path)
        return -ENOENT;

    path_canonicalize(full_path);
    inode = path_lookup(full_path);
    if (!inode) {
        kfree(full_path);
        return -ENOENT;
    }
    put_inode(inode);

    ret = statvfs_from_path(full_path, &st);
    kfree(full_path);
    if (ret < 0)
        return ret;
    if (copy_to_user(buf, &st, sizeof(st)) < 0)
        return -EFAULT;
    return 0;
}

int sys_fstatvfs(int fd, armos_statvfs_t* buf)
{
    task_t* task = task_current_local();
    armos_statvfs_t st;
    file_t* file;
    int ret;

    if (!buf)
        return -EFAULT;
    if (fd < 0 || fd >= MAX_FILES || !task || !task->process)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;
    if (file->path[0] == '\0')
        return -ENOSYS;

    ret = statvfs_from_path(file->path, &st);
    if (ret < 0)
        return ret;
    if (copy_to_user(buf, &st, sizeof(st)) < 0)
        return -EFAULT;
    return 0;
}

static int pipe_wait_interruptible(void)
{
    return task_poll_wait_once() == 0 ? 0 : -EINTR;
}

bool can_read(file_t* file) {
    int access_mode;

    if (!file) return false;
    access_mode = file->flags & O_ACCMODE;
    return access_mode == O_RDONLY || access_mode == O_RDWR;
}

bool can_write(file_t* file) {
    int access_mode;

    if (!file) return false;
    access_mode = file->flags & O_ACCMODE;
    return access_mode == O_WRONLY || access_mode == O_RDWR;
}

/* Lecture depuis un pipe */
ssize_t pipe_read(file_t* file, void* buf, size_t count) {
    struct pipe_inode_info *pipe = file->private_data;
    struct pipe_buffer *buffer = pipe->buffer;
    char *user_buf = (char*)buf;
    size_t bytes_read = 0;
    uint32_t irq_flags;

    //KDEBUG("ENTERING READING FROM PIPE\n");
    //KDEBUG("file->flags = 0x%08X\n", file->flags);

    /* Vérifier que c'est ouvert en lecture */
    if (!can_read(file)) {
        return -EBADF;
    }

    while (1) {
        irq_flags = disable_interrupts_save();

        /* Si pas de données et plus d'écrivains -> EOF */
        if (buffer->count == 0 && (buffer->writers == 0 || buffer->closed_write)) {
            restore_interrupts(irq_flags);
            return 0;  /* EOF */
        }

        /* Lire les données disponibles */
        while (bytes_read < count && buffer->count > 0) {
            user_buf[bytes_read] = buffer->data[buffer->read_pos];
            buffer->read_pos = (buffer->read_pos + 1) % PIPE_BUF_SIZE;
            buffer->count--;
            bytes_read++;
        }

        restore_interrupts(irq_flags);

        if (bytes_read > 0) {
            task_poll_notify();
            return bytes_read;
        }

        if ((file->flags & O_NONBLOCK) != 0)
            return -EAGAIN;

        if (pipe_wait_interruptible() < 0)
            return -EINTR;
    }
}

/* Écriture vers un pipe */
ssize_t pipe_write(file_t* file, const void* buf, size_t count) {
    struct pipe_inode_info *pipe = file->private_data;
    struct pipe_buffer *buffer = pipe->buffer;
    const char *user_buf = (const char*)buf;
    size_t bytes_written = 0;
    uint32_t irq_flags;
    

    /* Vérifier que c'est ouvert en écriture */
    if (!can_write(file)) {
        return -EBADF;
    }
    
    while (bytes_written < count) {
        irq_flags = disable_interrupts_save();

        /* Si pas de lecteurs -> SIGPIPE */
        if (buffer->readers == 0 || buffer->closed_read) {
            restore_interrupts(irq_flags);
            return bytes_written > 0 ? (ssize_t)bytes_written : -EPIPE;
        }

        //KDEBUG("WRITING TO PIPE\n");

        /* Écrire tant qu'il y a de la place */
        while (bytes_written < count && buffer->count < PIPE_BUF_SIZE) {
            buffer->data[buffer->write_pos] = user_buf[bytes_written];
            buffer->write_pos = (buffer->write_pos + 1) % PIPE_BUF_SIZE;
            buffer->count++;
            bytes_written++;
        }

        restore_interrupts(irq_flags);
        if (bytes_written > 0)
            task_poll_notify();

        if (bytes_written >= count)
            return bytes_written;

        if ((file->flags & O_NONBLOCK) != 0)
            return bytes_written > 0 ? (ssize_t)bytes_written : -EAGAIN;

        if (pipe_wait_interruptible() < 0)
            return bytes_written > 0 ? (ssize_t)bytes_written : -EINTR;
    }

    //KDEBUG("DATA WROTE TO PIPE\n");
    return bytes_written;
}

int pipe_close(file_t* file) {
    struct pipe_inode_info *pipe = file->private_data;
    struct pipe_buffer *buffer = pipe->buffer;
    
    //KDEBUG("CLOSING PIPE\n");

    /* Déterminer si c'était un lecteur ou écrivain */
    int access_mode = file->flags & O_ACCMODE;

    if (access_mode == O_RDONLY || access_mode == O_RDWR) {
        buffer->readers--;
        if (buffer->readers == 0) {
            buffer->closed_read = 1;
        }
    }
    
    if (access_mode == O_WRONLY || access_mode == O_RDWR) {
        buffer->writers--;
        if (buffer->writers == 0) {
            buffer->closed_write = 1;
        }
    }
    
    /* Libérer si plus personne n'utilise le pipe */
    if (buffer->readers == 0 && buffer->writers == 0) {
        //KDEBUG("NO MORE USERS OF THIS PIPE. Freeing it.\n");
        kfree(buffer);
    }
    
    kfree(pipe);
    task_poll_notify();
    //KDEBUG("PIPE CLOSED\n");

    return 0;
}

int sys_pipe(int pipefd[2])
{
    int fd_read = -1;
    int fd_write = -1;
    file_t *read_file = NULL;
    file_t *write_file = NULL;
    struct pipe_buffer *buffer;
    struct pipe_inode_info *read_pipe, *write_pipe;
    inode_t *inode;
    int fds[2];
    task_t *task = task_current_local();
    
    /* Vérifier le pointeur utilisateur */
    if (!pipefd || !task || !task->process) {
        return -EFAULT;
    }

    /* Créer le buffer du pipe */
    buffer = kzalloc(sizeof(struct pipe_buffer));
    if (!buffer) {
        return -ENOMEM;
    }
    
    /* Initialiser le buffer */
    buffer->readers = 1;
    buffer->writers = 1;
    buffer->read_pos = 0;
    buffer->write_pos = 0;
    buffer->count = 0;
    buffer->closed_read = 0;
    buffer->closed_write = 0;
    
    /* Créer l'inode pipe */
    inode = create_inode();
    if (!inode) {
        kfree(buffer);
        return -ENOMEM;
    }
    
    inode->mode = S_IFIFO | 0600;
    
    /* Créer les structures pipe_inode_info */
    read_pipe = kmalloc(sizeof(struct pipe_inode_info));
    write_pipe = kmalloc(sizeof(struct pipe_inode_info));
    if (!read_pipe || !write_pipe) {
        kfree(buffer);
        kfree(read_pipe);
        kfree(write_pipe);
        put_inode(inode);
        return -ENOMEM;
    }
    
    read_pipe->buffer = buffer;
    write_pipe->buffer = buffer;
    
    /* Créer les fichiers */
    read_file = create_file();
    write_file = create_file();
    if (!read_file || !write_file) {
        close_file(read_file);
        close_file(write_file);
        kfree(buffer);
        kfree(read_pipe);
        kfree(write_pipe);
        put_inode(inode);
        return -ENOMEM;
    }
    
    /* Configurer le fichier de lecture */
    strcpy(read_file->name, "pipe:[read]");
    read_file->pos = 0;
    read_file->inode = inode;
    read_file->offset = 0;
    read_file->flags = O_RDONLY;        /* ← Utilise le champ flags */
    read_file->type = FILE_TYPE_PIPE;
    read_file->ref_count = 1;
    read_file->f_op = &pipe_read_fops;
    read_file->private_data = read_pipe;
    
    /* Configurer le fichier d'écriture */
    strcpy(write_file->name, "pipe:[write]");
    write_file->pos = 0;
    write_file->inode = inode;
    inode->ref_count++;
    write_file->offset = 0;
    write_file->flags = O_WRONLY;       /* ← Utilise le champ flags */
    write_file->type = FILE_TYPE_PIPE;
    write_file->ref_count = 1;
    write_file->f_op = &pipe_write_fops;
    write_file->private_data = write_pipe;
    
    /* Installer atomiquement chaque fichier dans la table du processus. */
    fd_read = vfs_install_file(task, read_file, 0u);
    if (fd_read < 0)
        goto cleanup_configured;
    read_file = NULL;
    fd_write = vfs_install_file(task, write_file, 0u);
    if (fd_write < 0) {
        (void)sys_close(fd_read);
        close_file(write_file);
        return fd_write;
    }
    write_file = NULL;
    
    /* Préparer les fd pour l'userspace */
    fds[0] = fd_read;
    fds[1] = fd_write;
    
    /* Copier vers l'userspace */
    if (copy_to_user(pipefd, fds, 2 * sizeof(int)) != 0) {
        sys_close(fd_read);
        sys_close(fd_write);
        return -EFAULT;
    }
    
    //KDEBUG("Created pipe: read_fd=%d, write_fd=%d\n", fd_read, fd_write);
    return 0;
    
cleanup_configured:
    close_file(read_file);
    close_file(write_file);
    return fd_read;
}


static vma_t *find_heap_vma(vm_space_t *vm)
{
    vma_t *vma = vm->vma_list;

    while (vma) {
        if (vma->start == vm->heap_start)
            return vma;
        vma = vma->next;
    }
    return NULL;
}

static void rollback_brk_growth(vm_space_t *vm, vaddr_t start, vaddr_t end)
{
    for (vaddr_t vaddr = start; vaddr < end; vaddr += PAGE_SIZE) {
        paddr_t phys_addr = get_physical_address(vm->pgdir, vaddr);
        if (phys_addr && unmap_user_page(vm->pgdir, vaddr, vm->asid) == 0) {
            free_page((void *)phys_addr);
        }
    }
}

long sys_brk(void* addr)
{
    task_t* task = task_current_local();

    vaddr_t new_brk = (vaddr_t)addr;
    vaddr_t old_brk;
    vaddr_t addr_to_unmap;
    vaddr_t addr_to_map;
    vma_t* heap_vma = NULL;
    
    if (!task ) {
        return -EINVAL;
    }

    process_t* proc = task->process ;
 
    if (!proc || !proc->vm) {
        return -EINVAL;
    }
    
    old_brk = proc->vm->brk;

    /* If addr is NULL, return current brk */
    if (!addr) {
        return (long)old_brk;
    }

    if (new_brk < proc->vm->heap_start || new_brk > proc->vm->heap_end) {
        return -ENOMEM;
    }

    new_brk = ALIGN_UP(new_brk, PAGE_SIZE);
    if (new_brk > proc->vm->heap_end) {
        return -ENOMEM;
    }

    if (new_brk < old_brk) {
        /* Shrinking heap */
        heap_vma = find_heap_vma(proc->vm);
        if (!heap_vma) return -ENOMEM;
        
        /* Unmap pages */
        for (addr_to_unmap = new_brk; addr_to_unmap < old_brk; addr_to_unmap += PAGE_SIZE) {
            paddr_t phys_addr = get_physical_address(proc->vm->pgdir, addr_to_unmap);
            if (phys_addr) {
                if (unmap_user_page(proc->vm->pgdir, addr_to_unmap, proc->vm->asid) < 0) {
                    return -EFAULT;
                }
                free_page((void *)phys_addr);
            }
        }
        if (new_brk == proc->vm->heap_start) {
            remove_vma(proc->vm, heap_vma->start, heap_vma->end);
        } else {
            heap_vma->end = new_brk;
        }
    } else if (new_brk > old_brk) {
        /* Growing heap */
        heap_vma = find_heap_vma(proc->vm);

        for (addr_to_map = old_brk; addr_to_map < new_brk; addr_to_map += PAGE_SIZE) {
            void *new_page = allocate_page();
            if (!new_page) {
                rollback_brk_growth(proc->vm, old_brk, addr_to_map);
                return -ENOMEM;
            }

            if (map_user_page(proc->vm->pgdir, addr_to_map, (paddr_t)new_page,
                              VMA_READ | VMA_WRITE, proc->vm->asid) < 0) {
                free_page(new_page);
                rollback_brk_growth(proc->vm, old_brk, addr_to_map);
                return -ENOMEM;
            }
        }

        if (!heap_vma) {
            heap_vma = create_vma(proc->vm, proc->vm->heap_start,
                                  new_brk - proc->vm->heap_start,
                                  VMA_READ | VMA_WRITE);
            if (!heap_vma) {
                rollback_brk_growth(proc->vm, old_brk, new_brk);
                return -ENOMEM;
            }
        } else {
            heap_vma->end = new_brk;
        }
    }

    //KDEBUG("sys_brk: New BRK is at 0x%08X\n", new_brk);

    proc->vm->brk = new_brk;
    return (long)new_brk;
}


int sys_dup(int oldfd)
{
    task_t *task = task_current_local();
    file_t* file;
    int newfd;

    if (!task || !task->process)
        return -EBADF;
    
    if (oldfd < 0 || oldfd >= MAX_FILES) return -EBADF;
    
    file = task->process->files[oldfd];
    if (!file) return -EBADF;
    
    newfd = allocate_fd(task);
    if (newfd < 0) return -EMFILE;
    
    task->process->files[newfd] = get_file(file);
    if (!task->process->files[newfd])
        return -EBADF;
    task->process->fd_flags[newfd] = 0;
    
    return newfd;
}

int sys_dup2(int oldfd, int newfd)
{
    task_t *task = task_current_local();
    file_t* file;
    file_t* new_file;

    if (!task || !task->process)
        return -EBADF;
    
    if (oldfd < 0 || oldfd >= MAX_FILES) return -EBADF;
    file = task->process->files[oldfd];
    if (!file) return -EBADF;
    if (oldfd == newfd) return newfd;
    if (newfd < 0 || newfd >= (int)vfs_fd_limit(task)) return -EBADF;
    
    /* Close newfd if it's open */
    if (task->process->files[newfd]) {
        file_t* old_newfd = task->process->files[newfd];
        task->process->files[newfd] = NULL;
        task->process->fd_flags[newfd] = 0;
        close_file(old_newfd);
    }
    
    new_file = get_file(file);
    if (!new_file)
        return -EBADF;

    task->process->files[newfd] = new_file;
    task->process->fd_flags[newfd] = 0;

    if (newfd == STDIN_FILENO && file_is_tty(new_file))
        task->process->controlling_tty =
            (int)(uintptr_t)new_file->private_data;
    
    return newfd;
}

static bool fd_read_ready(file_t *file)
{
    if (!file || !can_read(file))
        return false;

    if (file->type == FILE_TYPE_TTY) {
        int tty_id = tty_id_from_file(file);
        return tty_id >= 0 && tty_read_ready_for_id(tty_id);
    }

    if (file->type == FILE_TYPE_PIPE) {
        struct pipe_inode_info *pipe = (struct pipe_inode_info *)file->private_data;
        struct pipe_buffer *buffer = pipe ? pipe->buffer : NULL;
        return buffer && (buffer->count > 0 || buffer->writers == 0 || buffer->closed_write);
    }

    if (file->type == FILE_TYPE_SOCKET)
        return false;

    if (file->type == FILE_TYPE_ARMOS_SOCKET)
        return armos_socket_read_ready(file);

    if (file->type == FILE_TYPE_INPUT)
        return armos_input_read_ready();

    if (file->type == FILE_TYPE_PTY_MASTER)
        return pty_master_read_ready(file);

    if (file->type == FILE_TYPE_EVENTFD)
        return eventfd_read_ready(file);

    if (file->type == FILE_TYPE_TIMERFD)
        return timerfd_read_ready(file);

    return file->f_op && file->f_op->read;
}

static bool fd_write_ready(file_t *file)
{
    if (!file || !can_write(file))
        return false;

    if (file->type == FILE_TYPE_PIPE) {
        struct pipe_inode_info *pipe = (struct pipe_inode_info *)file->private_data;
        struct pipe_buffer *buffer = pipe ? pipe->buffer : NULL;
        return buffer && buffer->readers > 0 && !buffer->closed_read &&
               buffer->count < PIPE_BUF_SIZE;
    }

    if (file->type == FILE_TYPE_ARMOS_SOCKET)
        return armos_socket_write_ready(file);

    if (file->type == FILE_TYPE_PTY_MASTER)
        return pty_master_write_ready(file);

    if (file->type == FILE_TYPE_EVENTFD)
        return eventfd_write_ready(file);

    return file->f_op && file->f_op->write;
}

int sys_fcntl(int fd, int cmd, uintptr_t arg)
{
    task_t *task = task_current_local();
    file_t* file;
    int newfd;
    struct flock_kernel fl;

    if (!task || !task->process)
        return -EINVAL;
    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if ((int)arg < 0 || arg >= vfs_fd_limit(task))
            return -EINVAL;
        for (newfd = (int)arg; newfd < (int)vfs_fd_limit(task); newfd++) {
            if (!task->process->files[newfd]) {
                task->process->files[newfd] = get_file(file);
                if (!task->process->files[newfd])
                    return -EBADF;
                task->process->fd_flags[newfd] =
                    cmd == F_DUPFD_CLOEXEC ? O_CLOEXEC : 0;
                return newfd;
            }
        }
        return -EMFILE;

    case F_GETFD:
        return (task->process->fd_flags[fd] & O_CLOEXEC) ? FD_CLOEXEC : 0;

    case F_SETFD:
        if (arg & FD_CLOEXEC)
            task->process->fd_flags[fd] |= O_CLOEXEC;
        else
            task->process->fd_flags[fd] &= ~O_CLOEXEC;
        return 0;

    case F_GETFL:
        return file->flags;

    case F_SETFL:
        file->flags = (file->flags & O_ACCMODE) |
                      (arg & (O_APPEND | O_NONBLOCK | O_SYNC | O_DSYNC | O_RSYNC));
        return 0;

    case F_GETLK:
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&fl, (void *)arg, sizeof(fl)) < 0)
            return -EFAULT;
        fl.l_type = F_UNLCK;
        fl.l_pid = 0;
        return copy_to_user((void *)arg, &fl, sizeof(fl)) < 0 ? -EFAULT : 0;

    case F_SETLK:
    case F_SETLKW:
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&fl, (void *)arg, sizeof(fl)) < 0)
            return -EFAULT;
        if (fl.l_type != F_RDLCK && fl.l_type != F_WRLCK && fl.l_type != F_UNLCK)
            return -EINVAL;
        return 0;

    default:
        return -EINVAL;
    }
}

int sys_ioctl(int fd, uint32_t request, uintptr_t arg)
{
    task_t *task = task_current_local();
    file_t* file;
    struct termios tio;
    struct winsize wsz;
    struct armos_fb_info fbinfo;
    struct armos_fb_orientation fborientation;
    struct armos_fb_mode fbmode;
    struct armos_fb_blit fbblit;
    struct armos_fb_map fbmap;
    struct armos_fb_present fbpresent;
    int tty_id;
    int fbret;
    uint32_t keyboard_layout;

    if (!task || !task->process)
        return -EINVAL;
    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;

    if (file->type == FILE_TYPE_PTY_MASTER &&
        (request == ARMOS_TIOCGPTN || request == ARMOS_TIOCSPTLCK))
        return pty_master_ioctl(file, request, arg);

    tty_id = file_is_tty(file) ? tty_id_from_file(file) : -ENOTTY;
    if (file_is_tty(file) && tty_id < 0)
        return tty_id;

    switch (request) {
    case ARMOS_TIOCGKEYMAP:
        if (!file_is_tty(file) && file->type != FILE_TYPE_INPUT)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        keyboard_layout = keyboard_layout_get();
        return copy_to_user((void *)arg, &keyboard_layout,
                            sizeof(keyboard_layout)) < 0 ? -EFAULT : 0;

    case ARMOS_TIOCSKEYMAP:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&keyboard_layout, (void *)arg,
                           sizeof(keyboard_layout)) < 0)
            return -EFAULT;
        return keyboard_layout_set(keyboard_layout);

    case ARMOS_FBIOGET_INFO:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        fbret = framebuffer_get_info(file, &fbinfo);
        if (fbret < 0)
            return fbret;
        return copy_to_user((void*)arg, &fbinfo, sizeof(fbinfo)) < 0 ? -EFAULT : 0;

    case ARMOS_FBIOGET_ORIENTATION:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        fbret = framebuffer_get_orientation(file, &fborientation);
        if (fbret < 0)
            return fbret;
        return copy_to_user((void*)arg, &fborientation,
                            sizeof(fborientation)) < 0 ? -EFAULT : 0;

    case ARMOS_FBIOSET_ORIENTATION:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&fborientation, (void*)arg,
                           sizeof(fborientation)) < 0)
            return -EFAULT;
        return framebuffer_set_orientation(file, &fborientation);

    case ARMOS_FBIOSET_MODE:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&fbmode, (void*)arg, sizeof(fbmode)) < 0)
            return -EFAULT;
        return framebuffer_set_mode(file, &fbmode);

    case ARMOS_FBIOACQUIRE:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        return framebuffer_acquire(file);

    case ARMOS_FBIORELEASE:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        return framebuffer_release(file);

    case ARMOS_FBIOBLIT:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&fbblit, (void *)arg, sizeof(fbblit)) < 0)
            return -EFAULT;
        return framebuffer_blit(file, &fbblit);

    case ARMOS_FBIOGET_MAP:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        fbret = framebuffer_get_map(file, &fbmap);
        if (fbret < 0)
            return fbret;
        return copy_to_user((void *)arg, &fbmap,
                            sizeof(fbmap)) < 0 ? -EFAULT : 0;

    case ARMOS_FBIOPRESENT:
        if (file->type != FILE_TYPE_FRAMEBUFFER)
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&fbpresent, (void *)arg,
                           sizeof(fbpresent)) < 0)
            return -EFAULT;
        return framebuffer_present(file, &fbpresent);

    case TIOCGWINSZ:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        tty_get_winsize_for_id(tty_id, &wsz.ws_row, &wsz.ws_col,
                               &wsz.ws_xpixel, &wsz.ws_ypixel);
        return copy_to_user((void*)arg, &wsz, sizeof(wsz)) < 0 ? -EFAULT : 0;

    case TIOCSWINSZ:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&wsz, (void*)arg, sizeof(wsz)) < 0)
            return -EFAULT;
        return tty_set_winsize_for_id(tty_id, wsz.ws_row, wsz.ws_col,
                                      wsz.ws_xpixel, wsz.ws_ypixel);

    case TIOCSCTTY:
        if (!file_is_tty(file))
            return -ENOTTY;
        task->process->controlling_tty = tty_id;
        return 0;

    case TCGETS:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (tty_get_termios_for_id(tty_id, &tio) < 0)
            return -EINVAL;
        return copy_to_user((void*)arg, &tio, sizeof(tio)) < 0 ? -EFAULT : 0;

    case TCSETSW:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&tio, (void*)arg, sizeof(tio)) < 0)
            return -EFAULT;
        while (tty_has_pending_output()) {
            if (has_pending_signals(task))
                return -EINTR;
            tty_drain_output();
            yield();
        }
        return tty_set_termios_for_id(tty_id, &tio, 0);

    case TCSETS:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&tio, (void*)arg, sizeof(tio)) < 0)
            return -EFAULT;
        return tty_set_termios_for_id(tty_id, &tio, 0);

    case TCSETSF:
        if (!file_is_tty(file))
            return -ENOTTY;
        if (!arg)
            return -EFAULT;
        if (copy_from_user(&tio, (void*)arg, sizeof(tio)) < 0)
            return -EFAULT;
        return tty_set_termios_for_id(tty_id, &tio, 1);

    case TCFLSH:
        if (!file_is_tty(file))
            return -ENOTTY;
        return tty_flush_for_id(tty_id, (int)arg);
    default:
        return -ENOTTY;
    }
}

int sys_time(time_t* tloc)
{
    time_t now = get_current_time();

    if (tloc && copy_to_user(tloc, &now, sizeof(now)) < 0)
        return -EFAULT;

    return (int)now;
}

int sys_gettimeofday(struct timeval* tv, struct timezone* tz)
{
    struct timeval ktv;
    struct timezone ktz;
    uint32_t ticks;

    ticks = get_system_ticks();
    ktv.tv_sec = get_current_time();
    ktv.tv_usec = (ticks % TIMER_FREQ) * (1000000u / TIMER_FREQ);

    if (tv && copy_to_user(tv, &ktv, sizeof(ktv)) < 0)
        return -EFAULT;

    if (tz) {
        ktz.tz_minuteswest = 0;
        ktz.tz_dsttime = 0;
        if (copy_to_user(tz, &ktz, sizeof(ktz)) < 0)
            return -EFAULT;
    }

    return 0;
}

int sys_uname(struct utsname_kernel *name)
{
    struct utsname_kernel local;
    const char *machine;

    if (!name)
        return -EFAULT;

    memset(&local, 0, sizeof(local));
    machine = arch_machine_name();
    strcpy(local.sysname, ARMOS_NAME);
    strcpy(local.nodename, "armos");
    strcpy(local.release, ARMOS_VERSION);
    strcpy(local.version, ARMOS_VERSION_BANNER " ");
    strcat(local.version, machine);
    strcpy(local.machine, machine);

    return copy_to_user(name, &local, sizeof(local)) < 0 ? -EFAULT : 0;
}

int sys_times(void* buf)
{
    task_t *task = task_current_local();
    struct tms_kernel ktms;
    uint32_t ticks = get_system_ticks();

    if (buf) {
        memset(&ktms, 0, sizeof(ktms));
        if (task) {
            ktms.tms_utime = (int32_t)task->user_runtime;
            ktms.tms_stime = (int32_t)task->system_runtime;
        }
        if (copy_to_user(buf, &ktms, sizeof(ktms)) < 0)
            return -EFAULT;
    }

    return (int)ticks;
}

int sys_sysconf(int name)
{
    switch (name) {
    case ARMOS_SC_ARG_MAX:
        return 65536;
    case ARMOS_SC_CHILD_MAX:
        return MAX_PROCESSES;
    case ARMOS_SC_CLK_TCK:
        return TIMER_FREQ;
    case ARMOS_SC_OPEN_MAX: {
        task_t* task = task_current_local();
        return task && task->process ? (int)vfs_fd_limit(task) : MAX_FILES;
    }
    case ARMOS_SC_JOB_CONTROL:
        return ARMOS_POSIX_VERSION;
    case ARMOS_SC_SAVED_IDS:
    case ARMOS_SC_VERSION:
    case ARMOS_SC_MAPPED_FILES:
    case ARMOS_SC_MEMORY_PROTECTION:
    case ARMOS_SC_SHARED_MEMORY_OBJECTS:
    case ARMOS_SC_TIMERS:
        return -ENOSYS;
    case ARMOS_SC_PAGESIZE:
        return PAGE_SIZE;
    case ARMOS_SC_NPROCESSORS_CONF:
        return (int)smp_possible_cpu_count();
    case ARMOS_SC_NPROCESSORS_ONLN:
        return (int)smp_online_cpu_count();
    case ARMOS_SC_PHYS_PAGES:
        return (int)get_total_page_count();
    case ARMOS_SC_AVPHYS_PAGES:
        return (int)get_free_page_count();
    case ARMOS_SC_FSYNC:
    case ARMOS_SC_MONOTONIC_CLOCK:
        return ARMOS_POSIX_VERSION;
    case ARMOS_SC_IOV_MAX:
        return ARMOS_IOV_MAX;
    default:
        return -EINVAL;
    }
}

static void fill_rusage_for_task(task_t *task, struct rusage_kernel *usage)
{
    uint32_t user_runtime;
    uint32_t system_runtime;

    memset(usage, 0, sizeof(*usage));
    if (!task)
        return;

    user_runtime = task->user_runtime > 0xffffffffu ?
                   0xffffffffu : (uint32_t)task->user_runtime;
    system_runtime = task->system_runtime > 0xffffffffu ?
                     0xffffffffu : (uint32_t)task->system_runtime;
    usage->ru_utime.tv_sec = (time_t)(user_runtime / TIMER_FREQ);
    usage->ru_utime.tv_usec =
        (uint32_t)((user_runtime % TIMER_FREQ) * (1000000u / TIMER_FREQ));
    usage->ru_stime.tv_sec = (time_t)(system_runtime / TIMER_FREQ);
    usage->ru_stime.tv_usec =
        (uint32_t)((system_runtime % TIMER_FREQ) * (1000000u / TIMER_FREQ));
    usage->ru_minflt = (int32_t)task->page_faults;
    usage->ru_majflt = 0;
    usage->ru_nvcsw = (int32_t)task->switch_count;
    if (task->type == TASK_TYPE_PROCESS && task->process && task->process->vm)
        usage->ru_maxrss = (int32_t)vm_resident_kb(task->process->vm);
}

int sys_getrusage(int who, struct rusage_kernel* usage)
{
    task_t *task = task_current_local();
    struct rusage_kernel local;

    if (!usage)
        return -EFAULT;
    if (who != ARMOS_RUSAGE_SELF && who != ARMOS_RUSAGE_CHILDREN)
        return -EINVAL;

    /*
     * ArmOS currently tracks per-live-task counters. Child aggregate accounting
     * will need process-level lifetime accumulation before destroy_process().
     */
    fill_rusage_for_task(who == ARMOS_RUSAGE_SELF ? task : NULL, &local);
    return copy_to_user(usage, &local, sizeof(local)) < 0 ? -EFAULT : 0;
}

static bool rlimit_resource_known(int resource)
{
    switch (resource) {
    case ARMOS_RLIMIT_CPU:
    case ARMOS_RLIMIT_FSIZE:
    case ARMOS_RLIMIT_DATA:
    case ARMOS_RLIMIT_STACK:
    case ARMOS_RLIMIT_CORE:
    case ARMOS_RLIMIT_RSS:
    case ARMOS_RLIMIT_NOFILE:
    case ARMOS_RLIMIT_AS:
        return true;
    default:
        return false;
    }
}

int sys_getrlimit(int resource, armos_rlimit_t* limit)
{
    task_t* task = task_current_local();
    armos_rlimit_t local;

    if (!limit)
        return -EFAULT;
    if (!task || !task->process)
        return -EINVAL;
    if (!rlimit_resource_known(resource))
        return -EINVAL;

    local.current = ARMOS_RLIM_INFINITY;
    local.maximum = ARMOS_RLIM_INFINITY;
    if (resource == ARMOS_RLIMIT_STACK) {
        local.current = USER_STACK_SIZE;
        local.maximum = USER_STACK_SIZE;
    } else if (resource == ARMOS_RLIMIT_NOFILE) {
        local.current = task->process->rlimit_nofile_cur;
        local.maximum = task->process->rlimit_nofile_max;
    }
    return copy_to_user(limit, &local, sizeof(local)) < 0 ? -EFAULT : 0;
}

int sys_setrlimit(int resource, const armos_rlimit_t* limit)
{
    task_t* task = task_current_local();
    armos_rlimit_t local;

    if (!limit)
        return -EFAULT;
    if (!task || !task->process)
        return -EINVAL;
    if (resource != ARMOS_RLIMIT_NOFILE)
        return -EINVAL;
    if (copy_from_user(&local, limit, sizeof(local)) < 0)
        return -EFAULT;
    if (local.current > local.maximum)
        return -EINVAL;
    if (local.current > MAX_FILES || local.maximum > MAX_FILES)
        return -EPERM;
    if (local.maximum > task->process->rlimit_nofile_max &&
        task->process->uid != 0)
        return -EPERM;

    task->process->rlimit_nofile_cur = (uint32_t)local.current;
    task->process->rlimit_nofile_max = (uint32_t)local.maximum;
    return 0;
}

static int select_words_for_nfds(int nfds)
{
    return (nfds + 31) / 32;
}

static bool fdset_has(uint32_t *set, int fd)
{
    return (set[fd / 32] & (1u << (fd % 32))) != 0;
}

static void fdset_set(uint32_t *set, int fd)
{
    set[fd / 32] |= (1u << (fd % 32));
}

static int select_scan(int nfds, uint32_t *in_read, uint32_t *in_write,
                       uint32_t *out_read, uint32_t *out_write,
                       uint32_t *out_except)
{
    task_t *task = task_current_local();
    int ready = 0;

    if (!task || !task->process)
        return -EINVAL;

    memset(out_read, 0, sizeof(uint32_t) * select_words_for_nfds(nfds));
    memset(out_write, 0, sizeof(uint32_t) * select_words_for_nfds(nfds));
    memset(out_except, 0, sizeof(uint32_t) * select_words_for_nfds(nfds));

    for (int fd = 0; fd < nfds; fd++) {
        file_t *file = NULL;

        if (fd >= MAX_FILES)
            break;

        file = task->process->files[fd];
        if (!file)
            continue;

        if (in_read && fdset_has(in_read, fd) && fd_read_ready(file)) {
            fdset_set(out_read, fd);
            ready++;
        }
        if (in_write && fdset_has(in_write, fd) && fd_write_ready(file)) {
            fdset_set(out_write, fd);
            ready++;
        }
    }

    return ready;
}

int sys_select(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout)
{
    task_t *task = task_current_local();
    struct timeval tv;
    uint32_t ms = 0;
    uint32_t deadline = 0;
    uint32_t read_in[8], write_in[8];
    uint32_t read_out[8], write_out[8], except_out[8];
    uint32_t *read_ptr = NULL, *write_ptr = NULL;
    int words;
    int ready;

    if (nfds < 0 || nfds > MAX_FILES)
        return -EINVAL;
    if (!task || !task->process)
        return -EINVAL;

    words = select_words_for_nfds(nfds);
    memset(read_in, 0, sizeof(read_in));
    memset(write_in, 0, sizeof(write_in));
    if (readfds) {
        if (copy_from_user(read_in, readfds, words * sizeof(uint32_t)) < 0)
            return -EFAULT;
        read_ptr = read_in;
    }
    if (writefds) {
        if (copy_from_user(write_in, writefds, words * sizeof(uint32_t)) < 0)
            return -EFAULT;
        write_ptr = write_in;
    }

    if (!timeout && nfds == 0) {
        return sys_pause();
    }

    if (timeout) {
        if (copy_from_user(&tv, timeout, sizeof(tv)) < 0)
            return -EFAULT;
        if (tv.tv_usec >= 1000000u)
            return -EINVAL;
        ms = (uint32_t)tv.tv_sec * 1000u + (tv.tv_usec + 999u) / 1000u;
        deadline = get_system_ticks() + (ms * TIMER_FREQ + 999u) / 1000u;
    }

    while (1) {
        ready = select_scan(nfds, read_ptr, write_ptr, read_out, write_out, except_out);
        if (ready < 0)
            return ready;
        if (ready > 0 || (timeout && ms == 0))
            break;
        if (timeout && get_system_ticks() >= deadline)
            break;
        if (has_pending_signals(task))
            return -EINTR;
        task_sleep_ms(1);
    }

    if (readfds && copy_to_user(readfds, read_out, words * sizeof(uint32_t)) < 0)
        return -EFAULT;
    if (writefds && copy_to_user(writefds, write_out, words * sizeof(uint32_t)) < 0)
        return -EFAULT;
    if (exceptfds && copy_to_user(exceptfds, except_out, words * sizeof(uint32_t)) < 0)
        return -EFAULT;

    return ready > 0 ? ready : 0;
}

int sys_poll(struct pollfd_kernel* fds, uint32_t nfds, int timeout_ms)
{
    task_t *task = task_current_local();
    struct pollfd_kernel local[64];
    uint32_t deadline = 0;
    int ready;

    if (!task || !task->process)
        return -EINVAL;
    if (!fds && nfds)
        return -EFAULT;
    if (nfds > 64)
        return -EINVAL;
    if (timeout_ms >= 0)
        deadline = get_system_ticks() + ((uint32_t)timeout_ms * TIMER_FREQ + 999u) / 1000u;

    if (nfds && copy_from_user(local, fds, nfds * sizeof(local[0])) < 0)
        return -EFAULT;

    while (1) {
        uint32_t generation = task_poll_generation();
        uint32_t wait_deadline = deadline;

        ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            file_t *file = NULL;

            local[i].revents = 0;
            if (local[i].fd < 0)
                continue;
            if (local[i].fd >= MAX_FILES ||
                !(file = task->process->files[local[i].fd])) {
                local[i].revents = ARMOS_POLLNVAL;
                ready++;
                continue;
            }
            if ((local[i].events & ARMOS_POLLIN) && fd_read_ready(file))
                local[i].revents |= ARMOS_POLLIN;
            if ((local[i].events & ARMOS_POLLOUT) && fd_write_ready(file))
                local[i].revents |= ARMOS_POLLOUT;
            if (file->type == FILE_TYPE_TIMERFD) {
                uint32_t timer_deadline;

                if (timerfd_poll_deadline(file, &timer_deadline) &&
                    (wait_deadline == 0u ||
                     (int32_t)(timer_deadline - wait_deadline) < 0))
                    wait_deadline = timer_deadline;
            }
            if (local[i].revents)
                ready++;
        }

        if (ready > 0 || timeout_ms == 0)
            break;
        if (timeout_ms >= 0 && get_system_ticks() >= deadline)
            break;
        if (has_pending_signals(task))
            return -EINTR;
        if (task_poll_wait(task, generation, wait_deadline) < 0)
            return -EINTR;
    }

    if (nfds && copy_to_user(fds, local, nfds * sizeof(local[0])) < 0)
        return -EFAULT;
    return ready;
}

ssize_t sys_readv(int fd, const struct iovec_kernel* iov, int iovcnt)
{
    struct iovec_kernel local[ARMOS_IOV_MAX];
    ssize_t total = 0;

    if (iovcnt < 0 || iovcnt > ARMOS_IOV_MAX)
        return -EINVAL;
    if (iovcnt == 0)
        return 0;
    if (!iov)
        return -EFAULT;
    if (copy_from_user(local, iov, (size_t)iovcnt * sizeof(local[0])) < 0)
        return -EFAULT;

    for (int i = 0; i < iovcnt; i++) {
        int ret;

        if (local[i].iov_len == 0)
            continue;
        if (!local[i].iov_base)
            return total > 0 ? total : -EFAULT;

        ret = sys_read(fd, local[i].iov_base, local[i].iov_len);
        if (ret < 0)
            return total > 0 ? total : ret;
        total += ret;
        if ((size_t)ret < local[i].iov_len)
            break;
    }

    return total;
}

ssize_t sys_writev(int fd, const struct iovec_kernel* iov, int iovcnt)
{
    struct iovec_kernel local[ARMOS_IOV_MAX];
    ssize_t total = 0;

    if (iovcnt < 0 || iovcnt > ARMOS_IOV_MAX)
        return -EINVAL;
    if (iovcnt == 0)
        return 0;
    if (!iov)
        return -EFAULT;
    if (copy_from_user(local, iov, (size_t)iovcnt * sizeof(local[0])) < 0)
        return -EFAULT;

    for (int i = 0; i < iovcnt; i++) {
        int ret;

        if (local[i].iov_len == 0)
            continue;
        if (!local[i].iov_base)
            return total > 0 ? total : -EFAULT;

        ret = sys_write(fd, local[i].iov_base, local[i].iov_len);
        if (ret < 0)
            return total > 0 ? total : ret;
        total += ret;
        if ((size_t)ret < local[i].iov_len)
            break;
    }

    return total;
}

int sys_alarm(uint32_t seconds)
{
    task_t *task = task_current_local();
    process_t* proc = task ? task->process : NULL;
    uint32_t now = get_system_ticks();
    uint32_t old_remaining = 0;

    if (!proc)
        return -EINVAL;

    if (proc->alarm_active && proc->alarm_expire_tick > now) {
        uint32_t ticks = proc->alarm_expire_tick - now;
        old_remaining = (ticks + TIMER_FREQ - 1) / TIMER_FREQ;
    }

    if (seconds == 0) {
        proc->alarm_active = 0;
        proc->alarm_expire_tick = 0;
    } else {
        proc->alarm_expire_tick = now + seconds * TIMER_FREQ;
        proc->alarm_active = 1;
    }

    return (int)old_remaining;
}

int sys_pause(void)
{
    task_t *task = task_current_local();

    if (!task || task->type != TASK_TYPE_PROCESS || !task->process)
        return -EINVAL;

    while (!has_pending_signals(task)) {
        task_set_interruptible(task);
        schedule();
    }

    return -EINTR;
}

int sys_link(const char* oldpath, const char* newpath)
{
    char *old_kpath, *new_kpath;
    char *old_full, *new_full;
    char *new_parent_path, *new_name;
    inode_t *target = NULL, *new_parent = NULL;
    int result;

    old_kpath = copy_string_from_user(oldpath);
    if (!old_kpath) return -EFAULT;

    new_kpath = copy_string_from_user(newpath);
    if (!new_kpath) { kfree(old_kpath); return -EFAULT; }

    old_full = resolve_path(old_kpath);
    kfree(old_kpath);
    if (!old_full) { kfree(new_kpath); return -ENOENT; }

    new_full = resolve_path(new_kpath);
    kfree(new_kpath);
    if (!new_full) { kfree(old_full); return -ENOENT; }

    result = vfs_check_search_permission(old_full, false);
    if (result < 0) {
        kfree(old_full);
        kfree(new_full);
        return result;
    }

    result = vfs_check_search_permission(new_full, false);
    if (result < 0) {
        kfree(old_full);
        kfree(new_full);
        return result;
    }

    result = split_path(new_full, &new_parent_path, &new_name);
    if (result != 0) {
        kfree(old_full);
        kfree(new_full);
        return result;
    }

    if (vfs_is_special_basename(new_name)) {
        result = -EINVAL;
        goto out_link;
    }

    vfs_begin_mutation();

    target = path_lookup_ex(old_full, false);
    if (!target) {
        result = -ENOENT;
        goto out_link_locked;
    }

    if (S_ISDIR(target->mode)) {
        result = -EPERM;
        goto out_link_locked;
    }

    new_parent = path_lookup(new_parent_path);
    if (!new_parent) {
        result = -ENOENT;
    } else if (!inode_permission(new_parent, MAY_WRITE | MAY_EXEC)) {
        result = -EACCES;
    } else if (new_parent->i_op != &ext2_inode_ops || target->i_op != &ext2_inode_ops) {
        result = -EXDEV;
    } else {
        result = ext2_link_inode(new_parent, new_name, target);
    }

out_link_locked:
    if (new_parent) put_inode(new_parent);
    if (target) put_inode(target);
    vfs_end_mutation();
out_link:
    kfree(old_full);
    kfree(new_full);
    kfree(new_parent_path);
    kfree(new_name);
    return result;
}

int sys_symlink(const char* target, const char* linkpath)
{
    char *target_kpath, *link_kpath;
    char *link_full;
    char *parent_path, *name;
    inode_t *parent;
    int result;

    target_kpath = copy_string_from_user(target);
    if (!target_kpath) return -EFAULT;

    link_kpath = copy_string_from_user(linkpath);
    if (!link_kpath) { kfree(target_kpath); return -EFAULT; }

    link_full = resolve_path(link_kpath);
    kfree(link_kpath);
    if (!link_full) {
        kfree(target_kpath);
        return -ENOENT;
    }

    result = vfs_check_search_permission(link_full, false);
    if (result < 0) {
        kfree(target_kpath);
        kfree(link_full);
        return result;
    }

    result = split_path(link_full, &parent_path, &name);
    if (result != 0) {
        kfree(target_kpath);
        kfree(link_full);
        return result;
    }

    if (vfs_is_special_basename(name)) {
        result = -EINVAL;
        goto out_symlink;
    }

    vfs_begin_mutation();

    parent = path_lookup(parent_path);
    if (!parent) {
        result = -ENOENT;
    } else if (!inode_permission(parent, MAY_WRITE | MAY_EXEC)) {
        result = -EACCES;
    } else if (parent->i_op != &ext2_inode_ops) {
        result = -EROFS;
    } else {
        result = ext2_create_symlink(parent, name, target_kpath);
    }

    if (parent) put_inode(parent);
    vfs_end_mutation();
out_symlink:
    kfree(target_kpath);
    kfree(link_full);
    kfree(parent_path);
    kfree(name);
    return result;
}

int sys_mknod(const char* pathname, mode_t mode, uint32_t dev)
{
    task_t *task = task_current_local();
    char* kernel_path;
    char* abs_path;
    char* parent_path;
    char* name;
    inode_t* parent = NULL;
    inode_t* existing = NULL;
    uint16_t type;
    int result;

    (void)dev; /* Major/minor dispatch is not implemented yet. */

    if (!task || !task->process)
        return -EINVAL;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path) return -EFAULT;

    abs_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!abs_path) return -ENOMEM;

    result = vfs_check_search_permission(abs_path, false);
    if (result < 0) {
        kfree(abs_path);
        return result;
    }

    result = split_path(abs_path, &parent_path, &name);
    if (result != 0) {
        kfree(abs_path);
        return result;
    }

    if (vfs_is_special_basename(name)) {
        result = -EINVAL;
        goto out;
    }

    type = mode & S_IFMT;
    if (type == 0)
        type = S_IFREG;
    if (type != S_IFREG && type != S_IFCHR && type != S_IFBLK && type != S_IFIFO) {
        result = -EINVAL;
        goto out;
    }

    vfs_begin_mutation();

    existing = path_lookup_ex(abs_path, false);
    if (existing) {
        put_inode(existing);
        result = -EEXIST;
        goto out_locked;
    }

    parent = path_lookup(parent_path);
    if (!parent) {
        result = -ENOENT;
    } else if (!S_ISDIR(parent->mode)) {
        result = -ENOTDIR;
    } else if (!inode_permission(parent, MAY_WRITE | MAY_EXEC)) {
        result = -EACCES;
    } else if (parent->i_op != &ext2_inode_ops || !parent->i_op->create) {
        result = -EROFS;
    } else {
        mode = type | ((mode & 07777) & ~task->process->umask);
        result = parent->i_op->create(parent, name, mode);
    }

    if (parent) put_inode(parent);

out_locked:
    vfs_end_mutation();
out:
    kfree(abs_path);
    kfree(parent_path);
    kfree(name);
    return result;
}

int sys_readlink(const char* pathname, char* buf, size_t bufsiz)
{
    char *kernel_path;
    char *full_path;
    inode_t *inode;
    char link_target[MAX_PATH];
    int result;

    if (!buf || bufsiz == 0) return -EINVAL;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path) return -EFAULT;

    full_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!full_path) return -ENOENT;

    result = vfs_check_search_permission(full_path, false);
    if (result < 0) {
        kfree(full_path);
        return result;
    }

    inode = path_lookup_ex(full_path, false);
    kfree(full_path);
    if (!inode) return -ENOENT;

    if (!S_ISLNK(inode->mode) || !inode->i_op || !inode->i_op->readlink) {
        put_inode(inode);
        return -EINVAL;
    }

    result = inode->i_op->readlink(inode, link_target,
                                   bufsiz < sizeof(link_target) ? bufsiz : sizeof(link_target));
    if (result >= 0 && copy_to_user(buf, link_target, (size_t)result) < 0)
        result = -EFAULT;

    put_inode(inode);
    return result;
}

int sys_chdir(const char* path)
{
    char* kernel_path;
    char* abs_path;
    inode_t* inode;
    task_t *task = task_current_local();

    if (!task || !task->process) return -EFAULT;

    kernel_path = copy_string_from_user(path);
    if (!kernel_path) return -EFAULT;

    /* Résoudre en chemin absolu puis canonicaliser (. et .. compris) */
    abs_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!abs_path) return -ENOMEM;

    path_canonicalize(abs_path);

    int search_ret = vfs_check_search_permission(abs_path, true);
    if (search_ret < 0) {
        kfree(abs_path);
        return search_ret;
    }

    inode = path_lookup(abs_path);
    if (!inode) {
        kfree(abs_path);
        return -ENOENT;
    }

    if (!S_ISDIR(inode->mode)) {
        put_inode(inode);
        kfree(abs_path);
        return -ENOTDIR;
    }

    strncpy(task->process->cwd, abs_path, MAX_PATH - 1);
    task->process->cwd[MAX_PATH - 1] = '\0';

    put_inode(inode);
    kfree(abs_path);
    return 0;
}

int sys_getpgrp(void)
{
    task_t *task = task_current_local();

    if (!task || task->type != TASK_TYPE_PROCESS || !task->process)
        return -EINVAL;

    return task->process->pgid;
}

int sys_setsid(void)
{
    task_t *task = task_current_local();
    process_t* proc = task ? task->process : NULL;

    if (!proc)
        return -EINVAL;
    if (proc->pgid == proc->pid)
        return -EPERM;

    proc->sid = proc->pid;
    proc->pgid = proc->pid;
    proc->controlling_tty = -1;
    return proc->sid;
}

int sys_getsid(pid_t pid)
{
    task_t *caller = task_current_local();
    task_t* target;

    if (!caller || caller->type != TASK_TYPE_PROCESS || !caller->process)
        return -EINVAL;

    target = (pid == 0) ? caller : find_process_by_pid(pid);
    if (!target || target->type != TASK_TYPE_PROCESS || !target->process)
        return -ESRCH;

    return target->process->sid;
}

int sys_setpgid(pid_t pid, pid_t pgid)
{
    task_t *caller = task_current_local();
    task_t *target;

    if (!caller || caller->type != TASK_TYPE_PROCESS || !caller->process)
        return -EINVAL;

    if (pid < 0 || pgid < 0)
        return -EINVAL;

    if (pid == 0)
        target = caller;
    else
        target = find_process_by_pid(pid);

    if (!target || target->type != TASK_TYPE_PROCESS || !target->process)
        return -ESRCH;

    if (target != caller && target->process->parent != caller)
        return -EPERM;

    if (target->state == TASK_ZOMBIE || target->state == TASK_TERMINATED)
        return -ESRCH;

    if (pgid == 0)
        pgid = target->process->pid;

    target->process->pgid = pgid;
    return 0;
}

int sys_getcwd(char* buf, size_t size)
{
    //const char* cwd = "/";
    task_t *task = task_current_local();

    if(!task || !task->process){
        return -EFAULT;
    }

    //KDEBUG("sys_getcwd: CWD = %s\n", task->process->cwd);

    size_t len = strlen(task->process->cwd);

    if(len == 0)
        return -EFAULT;
    
    if (size < len + 1) return -ERANGE;
    
    if (copy_to_user(buf, task->process->cwd, len + 1) < 0) {
        return -EFAULT;
    }
    
    return (int)(len + 1);
}

int sys_access(const char* pathname, int mode)
{
    char* kernel_path;
    char* full_path;
    inode_t* inode;
    
    if (mode & ~(MAY_READ | MAY_WRITE | MAY_EXEC)) return -EINVAL;
    
    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path) return -EFAULT;
    
    full_path = resolve_path(kernel_path);
    kfree(kernel_path);

    if (!full_path) return -ENOENT;

    int search_ret = vfs_check_search_permission(full_path, false);
    if (search_ret < 0) {
        kfree(full_path);
        return search_ret;
    }

    if (is_null_device_path(full_path) ||
        is_tty_device_path(full_path) ||
        is_framebuffer_device_path(full_path) ||
        is_input_device_path(full_path) ||
        is_net_echo_device_path(full_path)) {
        kfree(full_path);
        return (mode & MAY_EXEC) ? -EACCES : 0;
    }

    inode = path_lookup(full_path);
    kfree(full_path);
    
    if (!inode) return -ENOENT;

    if (mode != 0 && !inode_permission(inode, mode)) {
        put_inode(inode);
        return -EACCES;
    }
    
    put_inode(inode);
    return 0;
}

int sys_umask(int mask)
{
    task_t *task = task_current_local();
    process_t* proc = task ? task->process : NULL;
    mode_t old_mask;

    if (!proc) return -EINVAL;
    
    old_mask = proc->umask;
    proc->umask = (mode_t)mask & 0777;
    return (int)old_mask;
}

static int chmod_inode(inode_t* inode, mode_t mode)
{
    if (!inode)
        return -EINVAL;

    if (current_uid() != 0 && current_uid() != inode->uid)
        return -EPERM;

    if (inode->i_op != &ext2_inode_ops)
        return -EROFS;

    inode->mode = (inode->mode & S_IFMT) | (mode & 07777);
    inode->ctime = get_current_time();
    return ext2_update_inode_metadata(inode);
}

static int chown_inode(inode_t* inode, uid_t owner, gid_t group)
{
    if (!inode)
        return -EINVAL;

    if (current_uid() != 0)
        return -EPERM;

    if (inode->i_op != &ext2_inode_ops)
        return -EROFS;

    if ((owner != (uid_t)-1 && owner > 0xFFFFu) ||
        (group != (gid_t)-1 && group > 0xFFFFu))
        return -EINVAL;

    if (owner != (uid_t)-1)
        inode->uid = owner;
    if (group != (gid_t)-1)
        inode->gid = group;

    inode->ctime = get_current_time();
    return ext2_update_inode_metadata(inode);
}

int sys_chmod(const char* pathname, mode_t mode)
{
    char* kernel_path;
    char* full_path;
    inode_t* inode;
    int ret = 0;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path) return -EFAULT;

    full_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!full_path) return -ENOENT;

    ret = vfs_check_search_permission(full_path, false);
    if (ret < 0) {
        kfree(full_path);
        return ret;
    }

    if (vfs_is_mountpoint(full_path)) {
        kfree(full_path);
        return -EBUSY;
    }

    vfs_begin_mutation();
    inode = path_lookup(full_path);
    kfree(full_path);
    if (!inode) {
        vfs_end_mutation();
        return -ENOENT;
    }

    ret = chmod_inode(inode, mode);

    put_inode(inode);
    vfs_end_mutation();
    return ret;
}

int sys_fchmod(int fd, mode_t mode)
{
    task_t* task = task_current_local();
    file_t* file;
    int ret;

    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    if (!task || !task->process)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;
    if (!file->inode)
        return -EINVAL;

    vfs_begin_mutation();
    ret = chmod_inode(file->inode, mode);
    vfs_end_mutation();
    return ret;
}

int sys_chown(const char* pathname, uid_t owner, gid_t group)
{
    char* kernel_path;
    char* full_path;
    inode_t* inode;
    int ret = 0;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path) return -EFAULT;

    full_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!full_path) return -ENOENT;

    ret = vfs_check_search_permission(full_path, false);
    if (ret < 0) {
        kfree(full_path);
        return ret;
    }

    if (vfs_is_mountpoint(full_path)) {
        kfree(full_path);
        return -EBUSY;
    }

    vfs_begin_mutation();
    inode = path_lookup(full_path);
    kfree(full_path);
    if (!inode) {
        vfs_end_mutation();
        return -ENOENT;
    }

    ret = chown_inode(inode, owner, group);

    put_inode(inode);
    vfs_end_mutation();
    return ret;
}

int sys_fchown(int fd, uid_t owner, gid_t group)
{
    task_t* task = task_current_local();
    file_t* file;
    int ret;

    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    if (!task || !task->process)
        return -EBADF;

    file = task->process->files[fd];
    if (!file)
        return -EBADF;
    if (!file->inode)
        return -EINVAL;

    vfs_begin_mutation();
    ret = chown_inode(file->inode, owner, group);
    vfs_end_mutation();
    return ret;
}

int sys_utime(const char* pathname, const void* times)
{
    char* kernel_path;
    char* full_path;
    inode_t* inode;
    struct utimbuf_kernel ktimes;
    uint32_t now;
    int ret = 0;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path) return -EFAULT;

    full_path = resolve_path(kernel_path);
    kfree(kernel_path);
    if (!full_path) return -ENOENT;

    ret = vfs_check_search_permission(full_path, false);
    if (ret < 0) {
        kfree(full_path);
        return ret;
    }

    if (times && copy_from_user(&ktimes, times, sizeof(ktimes)) < 0) {
        kfree(full_path);
        return -EFAULT;
    }

    vfs_begin_mutation();
    inode = path_lookup(full_path);
    kfree(full_path);
    if (!inode) {
        vfs_end_mutation();
        return -ENOENT;
    }

    if (current_uid() != 0 && current_uid() != inode->uid &&
        !(!times && inode_permission(inode, MAY_WRITE))) {
        put_inode(inode);
        vfs_end_mutation();
        return -EPERM;
    }

    if (inode->i_op != &ext2_inode_ops) {
        put_inode(inode);
        vfs_end_mutation();
        return -EROFS;
    }

    now = get_current_time();
    inode->atime = times ? (uint32_t)ktimes.actime : now;
    inode->mtime = times ? (uint32_t)ktimes.modtime : now;
    inode->ctime = now;
    ret = ext2_update_inode_metadata(inode);

    put_inode(inode);
    vfs_end_mutation();
    return ret;
}

static int validate_utimens(const armos_timespec_t times[2])
{
    int i;

    if (!times)
        return 0;
    for (i = 0; i < 2; i++) {
        if (times[i].nsec == ARMOS_UTIME_NOW ||
            times[i].nsec == ARMOS_UTIME_OMIT)
            continue;
        if (times[i].nsec < 0 || times[i].nsec >= 1000000000LL)
            return -EINVAL;
        if (times[i].sec < 0 || times[i].sec > 0xffffffffLL)
            return -EOVERFLOW;
    }
    return 0;
}

static int update_inode_times(inode_t* inode,
                              const armos_timespec_t times[2],
                              bool have_times)
{
    bool explicit_time = false;
    bool changed = false;
    uint32_t now;
    int ret;
    int i;

    if (!inode)
        return -EINVAL;
    if (have_times) {
        ret = validate_utimens(times);
        if (ret < 0)
            return ret;
        for (i = 0; i < 2; i++) {
            if (times[i].nsec != ARMOS_UTIME_NOW &&
                times[i].nsec != ARMOS_UTIME_OMIT)
                explicit_time = true;
        }
        if (times[0].nsec == ARMOS_UTIME_OMIT &&
            times[1].nsec == ARMOS_UTIME_OMIT)
            return 0;
    }

    if (current_uid() != 0 && current_uid() != inode->uid &&
        (explicit_time || !inode_permission(inode, MAY_WRITE)))
        return -EPERM;
    if (inode->i_op != &ext2_inode_ops)
        return -EROFS;

    now = get_current_time();
    if (!have_times || times[0].nsec == ARMOS_UTIME_NOW) {
        inode->atime = now;
        changed = true;
    } else if (times[0].nsec != ARMOS_UTIME_OMIT) {
        inode->atime = (uint32_t)times[0].sec;
        changed = true;
    }
    if (!have_times || times[1].nsec == ARMOS_UTIME_NOW) {
        inode->mtime = now;
        changed = true;
    } else if (times[1].nsec != ARMOS_UTIME_OMIT) {
        inode->mtime = (uint32_t)times[1].sec;
        changed = true;
    }
    if (!changed)
        return 0;

    inode->ctime = now;
    return ext2_update_inode_metadata(inode);
}

int sys_utimensat(int dirfd, const char* pathname,
                  const armos_timespec_t* times, int flags)
{
    armos_timespec_t ktimes[2];
    bool follow_final;
    char* kernel_path;
    char* full_path;
    inode_t* inode;
    int ret;

    if (flags & ~ARMOS_AT_SYMLINK_NOFOLLOW)
        return -EINVAL;
    if (times && copy_from_user(ktimes, times, sizeof(ktimes)) < 0)
        return -EFAULT;
    ret = validate_utimens(times ? ktimes : NULL);
    if (ret < 0)
        return ret;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    ret = resolve_path_at(dirfd, kernel_path, &full_path);
    kfree(kernel_path);
    if (ret < 0)
        return ret;
    ret = vfs_check_search_permission(full_path, false);
    if (ret < 0) {
        kfree(full_path);
        return ret;
    }

    follow_final = !(flags & ARMOS_AT_SYMLINK_NOFOLLOW);
    vfs_begin_mutation();
    inode = path_lookup_ex(full_path, follow_final);
    kfree(full_path);
    if (!inode) {
        vfs_end_mutation();
        return -ENOENT;
    }
    ret = update_inode_times(inode, ktimes, times != NULL);
    put_inode(inode);
    vfs_end_mutation();
    return ret;
}

int sys_futimens(int fd, const armos_timespec_t* times)
{
    task_t* task = task_current_local();
    armos_timespec_t ktimes[2];
    file_t* file;
    int ret;

    if (fd < 0 || fd >= MAX_FILES || !task || !task->process)
        return -EBADF;
    file = task->process->files[fd];
    if (!file)
        return -EBADF;
    if (!file->inode)
        return -EINVAL;
    if (times && copy_from_user(ktimes, times, sizeof(ktimes)) < 0)
        return -EFAULT;

    vfs_begin_mutation();
    ret = update_inode_times(file->inode, ktimes, times != NULL);
    vfs_end_mutation();
    return ret;
}

static int unlink_resolved(char *full_path)
{
    char* parent_path;
    char* filename;
    inode_t* parent_inode;
    inode_t* target_inode;
    int result;
    
    result = vfs_check_search_permission(full_path, false);
    if (result < 0) {
        kfree(full_path);
        return result;
    }

    if (vfs_is_mountpoint(full_path)) {
        kfree(full_path);
        return -EBUSY;
    }

    vfs_begin_mutation();

    /* Vérifier que le fichier existe */
    target_inode = path_lookup_ex(full_path, false);
    if (!target_inode) {
        vfs_end_mutation();
        kfree(full_path);
        return -ENOENT;
    }
    
    /* Vérifier que ce n'est pas un répertoire */
    if (S_ISDIR(target_inode->mode)) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(full_path);
        return -EISDIR;
    }

    if (vfs_inode_has_external_refs(target_inode)) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(full_path);
        return -EBUSY;
    }
    
    /* Séparer le chemin parent et le nom du fichier */
    result = split_path(full_path, &parent_path, &filename);
    if (result != 0) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(full_path);
        return result;
    }
    
    if (vfs_is_special_basename(filename)) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(full_path);
        kfree(parent_path);
        kfree(filename);
        return -EINVAL;
    }

    /* Trouver le répertoire parent */
    parent_inode = path_lookup(parent_path);
    if (!parent_inode) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(full_path);
        kfree(parent_path);
        kfree(filename);
        return -ENOENT;
    }
    
    /* Vérifier les permissions d'écriture sur le parent */
    if (!inode_permission(parent_inode, MAY_WRITE | MAY_EXEC)) {
        put_inode(target_inode);
        put_inode(parent_inode);
        vfs_end_mutation();
        kfree(full_path);
        kfree(parent_path);
        kfree(filename);
        return -EACCES;
    }
    
    /* Supprimer le fichier via l'inode operation */
    result = parent_inode->i_op->unlink(parent_inode, filename);
    
    // Decrementer le compteur de liens 
/*     if (result == 0) {
        target_inode->ref_count--;
        if (target_inode->ref_count == 0) {
            // Plus de references, liberer l'inode 
            remove_inode_from_cache(target_inode);
        }
    } */
    
    put_inode(target_inode);
    put_inode(parent_inode);
    vfs_end_mutation();
    kfree(full_path);
    kfree(parent_path);
    kfree(filename);
    
    return result;
}

int sys_unlink(const char* pathname)
{
    char *kernel_path;
    char *full_path;
    int result;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(ARMOS_AT_FDCWD, kernel_path, &full_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    return unlink_resolved(full_path);
}


static int rename_resolved(char *old_full, char *new_full)
{
    char *old_parent_path, *old_name;
    char *new_parent_path, *new_name;
    inode_t *old_parent, *new_parent;
    int result;

    result = vfs_check_search_permission(old_full, false);
    if (result != 0) { kfree(old_full); kfree(new_full); return result; }

    result = vfs_check_search_permission(new_full, false);
    if (result != 0) { kfree(old_full); kfree(new_full); return result; }

    if (vfs_is_mountpoint(old_full) || vfs_is_mountpoint(new_full)) {
        kfree(old_full);
        kfree(new_full);
        return -EBUSY;
    }

    vfs_begin_mutation();

    result = split_path(old_full, &old_parent_path, &old_name);
    if (result != 0) { vfs_end_mutation(); kfree(old_full); kfree(new_full); return result; }

    result = split_path(new_full, &new_parent_path, &new_name);
    if (result != 0) {
        vfs_end_mutation();
        kfree(old_full); kfree(new_full);
        kfree(old_parent_path); kfree(old_name);
        return result;
    }

    if (vfs_is_special_basename(old_name) || vfs_is_special_basename(new_name)) {
        result = -EINVAL;
        goto out;
    }

    old_parent = path_lookup(old_parent_path);
    if (!old_parent) { result = -ENOENT; goto out; }

    new_parent = path_lookup(new_parent_path);
    if (!new_parent) { put_inode(old_parent); result = -ENOENT; goto out; }

    inode_t* rename_target = path_lookup_ex(old_full, false);
    if (!rename_target) {
        result = -ENOENT;
    } else if (vfs_inode_has_external_refs(rename_target)) {
        result = -EBUSY;
    } else if (S_ISDIR(rename_target->mode) &&
               vfs_path_is_self_or_descendant(old_full, new_full)) {
        result = -EINVAL;
    } else if (old_parent->i_op != new_parent->i_op) {
        result = -EXDEV;
    } else if (!old_parent->i_op || !old_parent->i_op->rename) {
        result = -ENOSYS;
    } else if (!inode_permission(old_parent, MAY_WRITE | MAY_EXEC)) {
        result = -EACCES;
    } else if (!inode_permission(new_parent, MAY_WRITE | MAY_EXEC)) {
        result = -EACCES;
    } else {
        result = old_parent->i_op->rename(old_parent, old_name,
                                          new_parent, new_name);
    }
    if (rename_target) put_inode(rename_target);

    put_inode(old_parent);
    put_inode(new_parent);

out:
    vfs_end_mutation();
    kfree(old_full); kfree(new_full);
    kfree(old_parent_path); kfree(old_name);
    kfree(new_parent_path); kfree(new_name);
    return result;
}

int sys_rename(const char* oldpath, const char* newpath)
{
    return sys_renameat(ARMOS_AT_FDCWD, oldpath,
                        ARMOS_AT_FDCWD, newpath);
}

int sys_renameat(int olddirfd, const char* oldpath,
                 int newdirfd, const char* newpath)
{
    char *old_kernel_path;
    char *new_kernel_path;
    char *old_full;
    char *new_full;
    int result;

    old_kernel_path = copy_string_from_user(oldpath);
    if (!old_kernel_path)
        return -EFAULT;
    new_kernel_path = copy_string_from_user(newpath);
    if (!new_kernel_path) {
        kfree(old_kernel_path);
        return -EFAULT;
    }
    result = resolve_path_at(olddirfd, old_kernel_path, &old_full);
    kfree(old_kernel_path);
    if (result < 0) {
        kfree(new_kernel_path);
        return result;
    }
    result = resolve_path_at(newdirfd, new_kernel_path, &new_full);
    kfree(new_kernel_path);
    if (result < 0) {
        kfree(old_full);
        return result;
    }
    return rename_resolved(old_full, new_full);
}


static int rmdir_resolved(char *abs_path)
{
    char* parent_path;
    char* dir_name;
    inode_t* parent_inode;
    inode_t* target_inode;
    int result;

    result = vfs_check_search_permission(abs_path, false);
    if (result < 0) {
        kfree(abs_path);
        return result;
    }

    if (vfs_is_mountpoint(abs_path)) {
        kfree(abs_path);
        return -EBUSY;
    }

    vfs_begin_mutation();

    target_inode = path_lookup_ex(abs_path, false);
    if (!target_inode) {
        vfs_end_mutation();
        kfree(abs_path);
        return -ENOENT;
    }

    if (!S_ISDIR(target_inode->mode)) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(abs_path);
        return -ENOTDIR;
    }

    if (vfs_inode_has_external_refs(target_inode)) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(abs_path);
        return -EBUSY;
    }

    result = split_path(abs_path, &parent_path, &dir_name);
    if (result != 0) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(abs_path);
        return result;
    }

    if (vfs_is_special_basename(dir_name)) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(abs_path);
        kfree(parent_path);
        kfree(dir_name);
        return -EINVAL;
    }

    parent_inode = path_lookup(parent_path);
    if (!parent_inode) {
        put_inode(target_inode);
        vfs_end_mutation();
        kfree(abs_path);
        kfree(parent_path);
        kfree(dir_name);
        return -ENOENT;
    }

    if (!parent_inode->i_op || !parent_inode->i_op->rmdir)
        result = -ENOSYS;
    else if (!inode_permission(parent_inode, MAY_WRITE | MAY_EXEC))
        result = -EACCES;
    else
        result = parent_inode->i_op->rmdir(parent_inode, dir_name);

    put_inode(target_inode);
    put_inode(parent_inode);
    vfs_end_mutation();
    kfree(abs_path);
    kfree(parent_path);
    kfree(dir_name);

    return result;
}

int sys_rmdir(const char* pathname)
{
    char *kernel_path;
    char *abs_path;
    int result;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(ARMOS_AT_FDCWD, kernel_path, &abs_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    return rmdir_resolved(abs_path);
}

int sys_unlinkat(int dirfd, const char* pathname, int flags)
{
    char *kernel_path;
    char *full_path;
    int result;

    if (flags & ~ARMOS_AT_REMOVEDIR)
        return -EINVAL;
    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(dirfd, kernel_path, &full_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    if (flags & ARMOS_AT_REMOVEDIR)
        return rmdir_resolved(full_path);
    return unlink_resolved(full_path);
}


static int mkdir_resolved(char *abs_path, mode_t mode)
{
    char* parent_path;
    char* dir_name;
    inode_t* parent_inode;
    int result;

    result = vfs_check_search_permission(abs_path, false);
    if (result < 0) {
        kfree(abs_path);
        return result;
    }

    vfs_begin_mutation();

    result = split_path(abs_path, &parent_path, &dir_name);
    if (result != 0) {
        vfs_end_mutation();
        kfree(abs_path);
        return result;
    }

    if (vfs_is_special_basename(dir_name)) {
        vfs_end_mutation();
        kfree(abs_path);
        kfree(parent_path);
        kfree(dir_name);
        return -EINVAL;
    }

    parent_inode = path_lookup(parent_path);
    if (!parent_inode) {
        vfs_end_mutation();
        kfree(abs_path);
        kfree(parent_path);
        kfree(dir_name);
        return -ENOENT;
    }

    if (!S_ISDIR(parent_inode->mode)) {
        put_inode(parent_inode);
        vfs_end_mutation();
        kfree(abs_path);
        kfree(parent_path);
        kfree(dir_name);
        return -ENOTDIR;
    }

    if (!inode_permission(parent_inode, MAY_WRITE | MAY_EXEC))
        result = -EACCES;
    else if (!parent_inode->i_op || !parent_inode->i_op->mkdir)
        result = -ENOSYS;
    else
        result = parent_inode->i_op->mkdir(parent_inode, dir_name, mode | S_IFDIR);

    put_inode(parent_inode);
    vfs_end_mutation();
    kfree(abs_path);
    kfree(parent_path);
    kfree(dir_name);

    return result;
}

int sys_mkdir(const char* pathname, mode_t mode)
{
    return sys_mkdirat(ARMOS_AT_FDCWD, pathname, mode);
}

int sys_mkdirat(int dirfd, const char* pathname, mode_t mode)
{
    char *kernel_path;
    char *abs_path;
    int result;

    kernel_path = copy_string_from_user(pathname);
    if (!kernel_path)
        return -EFAULT;
    result = resolve_path_at(dirfd, kernel_path, &abs_path);
    kfree(kernel_path);
    if (result < 0)
        return result;
    return mkdir_resolved(abs_path, mode);
}


static uint64_t divide_u64_u32(uint64_t dividend, uint32_t divisor,
                               uint32_t *remainder)
{
    uint64_t quotient = 0;
    uint64_t rem = 0;
    int bit;

    if (divisor == 0) {
        if (remainder)
            *remainder = 0;
        return 0;
    }

    for (bit = 63; bit >= 0; bit--) {
        rem = (rem << 1) | ((dividend >> bit) & 1u);
        if (rem >= divisor) {
            rem -= divisor;
            quotient |= 1ULL << bit;
        }
    }

    if (remainder)
        *remainder = (uint32_t)rem;
    return quotient;
}

int sys_sched_yield(void)
{
    yield();
    return 0;
}

static int clock_gettime_value(int clock_id, armos_timespec_t *value)
{
    if (clock_id == ARMOS_CLOCK_REALTIME) {
        value->sec = (int64_t)get_current_time();
        value->nsec = 0;
    } else if (clock_id == ARMOS_CLOCK_MONOTONIC) {
        uint32_t frequency = get_timer_frequency();
        uint32_t remainder;
        uint64_t count;

        if (frequency == 0)
            return -ENOSYS;

        count = get_timer_count();
        value->sec = (int64_t)divide_u64_u32(count, frequency, &remainder);
        value->nsec = (int64_t)divide_u64_u32(
            (uint64_t)remainder * 1000000000ULL, frequency, NULL);
    } else {
        return -EINVAL;
    }

    return 0;
}

int sys_clock_gettime(int clock_id, armos_timespec_t *tp)
{
    armos_timespec_t value;
    int ret;

    if (!tp)
        return -EFAULT;

    ret = clock_gettime_value(clock_id, &value);
    if (ret < 0)
        return ret;

    return copy_to_user(tp, &value, sizeof(value)) < 0 ? -EFAULT : 0;
}

int sys_clock_getres(int clock_id, armos_timespec_t *res)
{
    armos_timespec_t value;

    if (clock_id == ARMOS_CLOCK_REALTIME) {
        value.sec = 1;
        value.nsec = 0;
    } else if (clock_id == ARMOS_CLOCK_MONOTONIC) {
        uint32_t frequency = get_timer_frequency();

        if (frequency == 0)
            return -ENOSYS;

        value.sec = 0;
        value.nsec = (int64_t)divide_u64_u32(
            1000000000ULL + frequency - 1u, frequency, NULL);
        if (value.nsec == 0)
            value.nsec = 1;
    } else {
        return -EINVAL;
    }

    if (!res)
        return 0;
    return copy_to_user(res, &value, sizeof(value)) < 0 ? -EFAULT : 0;
}

static int sleep_interruptible_ticks(task_t *task, uint32_t sleep_ticks,
                                     uint32_t *remaining_ticks)
{
    uint32_t start_time, elapsed_time;
    uint32_t now;
    bool interrupted;

    if (sleep_ticks == 0) {
        if (remaining_ticks)
            *remaining_ticks = 0;
        yield();
        return 0;
    }

    start_time = get_system_ticks();
    task_set_interruptible_until(task, start_time + sleep_ticks);
    yield();

    now = get_system_ticks();
    interrupted = has_pending_signals(task) ||
        (task->state == TASK_RUNNING &&
         task->wakeup_time > 0 &&
         now < task->wakeup_time);

    task_set_wakeup_time(task, 0);

    if (interrupted) {
        if (remaining_ticks) {
            elapsed_time = now - start_time;
            *remaining_ticks = elapsed_time >= sleep_ticks ?
                0 : sleep_ticks - elapsed_time;
            if (*remaining_ticks == 0)
                *remaining_ticks = 1;
        }
        return -EINTR;
    }

    if (remaining_ticks)
        *remaining_ticks = 0;
    return 0;
}

static uint32_t duration_to_ticks(int64_t sec, int64_t nsec)
{
    uint32_t nsec_ticks = (uint32_t)(nsec + 999999LL) / 1000000u;
    uint32_t sec_ticks;

    if (sec > (int64_t)(0xFFFFFFFFu / TIMER_FREQ))
        return 0xFFFFFFFFu;

    sec_ticks = (uint32_t)sec * TIMER_FREQ;
    if (sec_ticks > 0xFFFFFFFFu - nsec_ticks)
        return 0xFFFFFFFFu;
    return sec_ticks + nsec_ticks;
}

int sys_nanosleep(const armos_timespec32_t *req,
                  armos_timespec32_t *rem)
{
    task_t *task = task_current_local();
    armos_timespec32_t request;
    armos_timespec32_t remaining;
    uint32_t remaining_ticks = 0;
    int ret;

    if (!task) return -EINVAL;
    if (!req) return -EFAULT;
    if (copy_from_user(&request, req, sizeof(request)) < 0)
        return -EFAULT;
    if (request.nsec >= 1000000000u) return -EINVAL;

    ret = sleep_interruptible_ticks(
        task, duration_to_ticks(request.sec, request.nsec), &remaining_ticks);
    if (ret == -EINTR && rem) {
        remaining.sec = remaining_ticks / TIMER_FREQ;
        remaining.nsec = (remaining_ticks % TIMER_FREQ) *
            (1000000000u / TIMER_FREQ);
        if (copy_to_user(rem, &remaining, sizeof(remaining)) < 0)
            return -EFAULT;
    }
    return ret;
}

int sys_clock_nanosleep(int clock_id, int flags,
                        const armos_timespec_t *req,
                        armos_timespec_t *rem)
{
    task_t *task = task_current_local();
    armos_timespec_t request;
    armos_timespec_t duration;
    armos_timespec_t now;
    armos_timespec_t remaining;
    uint32_t remaining_ticks = 0;
    int ret;

    if (!task)
        return -EINVAL;
    if (!req)
        return -EFAULT;
    if (flags != 0 && flags != ARMOS_TIMER_ABSTIME)
        return -EINVAL;
    if (copy_from_user(&request, req, sizeof(request)) < 0)
        return -EFAULT;
    if (request.sec < 0 || request.nsec < 0 ||
        request.nsec >= 1000000000LL)
        return -EINVAL;

    do {
        ret = clock_gettime_value(clock_id, &now);
        if (ret < 0)
            return ret;

        duration = request;
        if (flags != ARMOS_TIMER_ABSTIME)
            break;
        if (request.sec < now.sec ||
            (request.sec == now.sec && request.nsec <= now.nsec))
            return 0;

        duration.sec = request.sec - now.sec;
        duration.nsec = request.nsec - now.nsec;
        if (duration.nsec < 0) {
            duration.sec--;
            duration.nsec += 1000000000LL;
        }

        ret = sleep_interruptible_ticks(
            task, duration_to_ticks(duration.sec, duration.nsec), NULL);
        if (ret < 0)
            return ret;
    } while (flags == ARMOS_TIMER_ABSTIME);

    ret = sleep_interruptible_ticks(task,
        duration_to_ticks(duration.sec, duration.nsec), &remaining_ticks);
    if (ret != -EINTR || !rem)
        return ret;

    remaining.sec = remaining_ticks / TIMER_FREQ;
    remaining.nsec = (remaining_ticks % TIMER_FREQ) *
        (1000000000u / TIMER_FREQ);
    if (copy_to_user(rem, &remaining, sizeof(remaining)) < 0)
        return -EFAULT;
    return ret;
}

static process_t *task_process_for_sysinfo(task_t *task)
{
    return task_get_process(task);
}

int sys_sysinfo(struct sysinfo_response *resp)
{
    if (!resp) return -EINVAL;

    static const char state_char[8] = {
        'R', /* TASK_READY           */
        'R', /* TASK_RUNNING         */
        'S', /* TASK_BLOCKED         */
        'Z', /* TASK_ZOMBIE          */
        'T', /* TASK_TERMINATED      */
        'S', /* TASK_INTERRUPTIBLE   */
        'D', /* TASK_UNINTERRUPTIBLE */
        't', /* TASK_STOPPED         */
    };

    struct sysinfo_response *local = kmalloc(sizeof(struct sysinfo_response));
    if (!local) return -ENOMEM;
    memset(local, 0, sizeof(struct sysinfo_response));

    /* Mémoire système */
    local->mem_total_kb = (get_total_page_count() * PAGE_SIZE) / 1024;
    local->mem_free_kb  = (get_free_page_count()  * PAGE_SIZE) / 1024;
    local->tasks_created = kernel_lifecycle_stats.tasks_created;
    local->tasks_destroyed = kernel_lifecycle_stats.tasks_destroyed;
    local->zombies_created = kernel_lifecycle_stats.zombies_created;
    local->zombies_reaped = kernel_lifecycle_stats.zombies_reaped;
    local->failed_forks = kernel_lifecycle_stats.failed_forks;
    local->scheduler_refused = kernel_lifecycle_stats.scheduler_refused;
    local->ready_queue_refused = kernel_lifecycle_stats.ready_queue_refused;
    local->stack_pages_allocated = kernel_lifecycle_stats.stack_pages_allocated;
    local->stack_pages_freed = kernel_lifecycle_stats.stack_pages_freed;
    local->phys_pages_allocated = get_allocated_page_count();
    local->phys_pages_freed = get_freed_page_count();
    local->asid_rollovers = kernel_lifecycle_stats.asid_rollovers;
    local->state_sync_repairs = kernel_lifecycle_stats.state_sync_repairs;
    local->blocked_signal_wakeups = kernel_lifecycle_stats.blocked_signal_wakeups;
    local->tty_stale_waiters = kernel_lifecycle_stats.tty_stale_waiters;
    local->fs_wait_timeouts = kernel_lifecycle_stats.fs_wait_timeouts;

    /* Uptime pour %CPU */
    uint32_t uptime = get_system_ticks();
    if (uptime == 0) uptime = 1;

    /* Liste des tâches */
    int count = 0;
    task_t *task = NULL;
    unsigned long task_flags;

    spin_lock_irqsave(&task_lock, &task_flags);
    task = task_list_head;
    if (!task) {
        spin_unlock_irqrestore(&task_lock, task_flags);
        goto out;
    }

    do {
        if (count >= 64) break;
        struct proc_info *p = &local->procs[count];
        process_t *proc = task_process_for_sysinfo(task);

        p->tid      = task->task_id;
        p->pid      = proc ? proc->pid : 0;
        p->ppid     = proc ? proc->ppid : 0;
        p->sid      = proc ? proc->sid : 0;
        p->tty      = proc ? proc->controlling_tty : -1;
        p->uid      = proc ? proc->uid : 0;
        p->gid      = proc ? proc->gid : 0;
        p->priority = task->priority;
        p->switches = task->switch_count;
        p->page_faults = task->page_faults;
        p->cow_faults = task->cow_faults;
        p->stack_faults = task->stack_faults;
        /* Division 64-bit évitée : on scale down en uint32 avant de diviser */
        uint32_t rt32 = (task->total_runtime >= (uint64_t)uptime)
                        ? uptime : (uint32_t)task->total_runtime;
        uint32_t u = uptime, r = rt32;
        while (u > 0x00100000u) { u >>= 1; r >>= 1; }
        p->cpu_pct_x10 = u ? (r * 1000u) / u : 0u;
        p->stack_kb = task->stack_size / 1024;
        p->state    = (task->state < 8) ? state_char[task->state] : '?';

        switch (task->type) {
            case TASK_TYPE_PROCESS: p->type = 'P'; break;
            case TASK_TYPE_THREAD:  p->type = 'T'; break;
            default:                p->type = 'K'; break;
        }

        if (proc && proc->vm) {
            vm_space_t *vm = proc->vm;
            uint32_t used = (vm->brk > vm->heap_start) ? (vm->brk - vm->heap_start) : 0;
            p->heap_kb = used / 1024;
            p->vm_kb = vm_virtual_kb(vm);
            p->rss_kb = vm_resident_kb(vm);
            p->l2_tables = vm_page_table_count(vm);
        }

        int i;
        for (i = 0; i < 31 && task->name[i]; i++)
            p->name[i] = task->name[i];
        p->name[i] = '\0';

        count++;
        task = task->next;
    } while (task && task != task_list_head);
    spin_unlock_irqrestore(&task_lock, task_flags);

out:
    local->proc_count = count;

    int ret = count;
    if (copy_to_user(resp, local, sizeof(struct sysinfo_response)) < 0)
        ret = -EFAULT;

    kfree(local);
    return ret;
}

void* sys_mmap(void* addr, size_t length, int prot, int flags, int fd)
{
    task_t *task = task_current_local();
    vm_space_t *vm;
    size_t size;
    vaddr_t hint = (vaddr_t)addr;
    vaddr_t vaddr;
    uint32_t vma_flags = 0;
    bool is_anon;
    bool lazy_anon;
    vma_t *vma;

    if (!task || !task->process || !task->process->vm)
        return (void *)-EINVAL;

    if (flags & ~(ARMOS_MAP_SHARED | ARMOS_MAP_PRIVATE |
                  ARMOS_MAP_FIXED | ARMOS_MAP_ANON))
        return (void *)-EINVAL;
    if (!!(flags & ARMOS_MAP_SHARED) == !!(flags & ARMOS_MAP_PRIVATE))
        return (void *)-EINVAL;
    if (flags & ARMOS_MAP_FIXED)
        return (void *)-ENOSYS;
    is_anon = (flags & ARMOS_MAP_ANON) != 0;
    if (is_anon && fd != -1)
        return (void *)-EINVAL;
    if (!is_anon && (fd < 0 || fd >= MAX_FILES))
        return (void *)-EBADF;
    if (length == 0)
        return (void *)-EINVAL;
    if (prot & ~(ARMOS_PROT_READ | ARMOS_PROT_WRITE | ARMOS_PROT_EXEC))
        return (void *)-EINVAL;
    if (prot == 0)
        return (void *)-EINVAL;

    size = ALIGN_UP(length, PAGE_SIZE);
    if (size == 0 || size > USER_MMAP_END - USER_MMAP_START)
        return (void *)-ENOMEM;

    if (prot & ARMOS_PROT_READ)
        vma_flags |= VMA_READ;
    if (prot & ARMOS_PROT_WRITE)
        vma_flags |= VMA_WRITE;
    if (prot & ARMOS_PROT_EXEC)
        vma_flags |= VMA_EXEC;

    if (flags & ARMOS_MAP_SHARED) {
        if (is_anon || (prot & ARMOS_PROT_EXEC))
            return (void *)-ENOSYS;
        {
            file_t *file = vfs_get_file_from_fd(task, fd);
            bool is_framebuffer =
                file && file->type == FILE_TYPE_FRAMEBUFFER;

            close_file(file);
            if (is_framebuffer)
                return framebuffer_map_fd(
                    fd, addr, length,
                    vma_flags | VMA_SHARED | VMA_DONTFORK |
                    VMA_EXTERNAL);
        }
        return shm_map_fd(fd, addr, length, vma_flags | VMA_SHARED);
    }

    if (!is_anon) {
        file_t *file = vfs_get_file_from_fd(task, fd);
        bool is_shm = file && file->type == FILE_TYPE_SHM;

        close_file(file);
        if (is_shm)
            return shm_map_fd(fd, addr, length, vma_flags);
    }

    lazy_anon = is_anon && !(prot & ARMOS_PROT_EXEC);
    if (lazy_anon)
        vma_flags |= VMA_LAZY;

    vm = task->process->vm;
    vaddr = vm_find_free_range(vm, hint, size, USER_MMAP_START, USER_MMAP_END);
    if (!vaddr)
        return (void *)-ENOMEM;

    vma = create_vma(vm, vaddr, size, vma_flags);
    if (!vma)
        return (void *)-ENOMEM;

    if (!lazy_anon) {
        for (vaddr_t page = vaddr; page < vaddr + size; page += PAGE_SIZE) {
            void *phys = allocate_page();
            if (!phys) {
                vm_unmap_range(vm, vaddr, size);
                return (void *)-ENOMEM;
            }
            if (map_user_page(vm->pgdir, page, (paddr_t)phys, vma->flags, vm->asid) < 0) {
                free_page(phys);
                vm_unmap_range(vm, vaddr, size);
                return (void *)-ENOMEM;
            }
        }
    }

    if (!is_anon) {
        file_t *file = vfs_get_file_from_fd(task, fd);
        file_t positioned;
        size_t copied = 0;

        if (!file || !can_read(file) || !file->f_op || !file->f_op->read) {
            close_file(file);
            vm_unmap_range(vm, vaddr, size);
            return (void *)-EBADF;
        }

        positioned = *file;
        positioned.offset = 0;
        while (copied < length) {
            vaddr_t page = vaddr + ALIGN_DOWN(copied, PAGE_SIZE);
            paddr_t phys = get_physical_address(vm->pgdir, page);
            size_t page_off = copied % PAGE_SIZE;
            size_t chunk = PAGE_SIZE - page_off;
            int ret;

            if (!phys) {
                close_file(file);
                vm_unmap_range(vm, vaddr, size);
                return (void *)-EFAULT;
            }
            if (chunk > length - copied)
                chunk = length - copied;

            ret = positioned.f_op->read(
                &positioned, (void *)(phys_to_virt(phys) + page_off), chunk);
            if (ret < 0) {
                close_file(file);
                vm_unmap_range(vm, vaddr, size);
                return (void *)(intptr_t)ret;
            }
            if (ret == 0)
                break;
            copied += (size_t)ret;
            if ((size_t)ret < chunk)
                break;
        }
        close_file(file);
    }

    return (void *)vaddr;
}

int sys_munmap(void* addr, size_t length)
{
    task_t *task = task_current_local();
    vaddr_t start = (vaddr_t)addr;
    size_t size;

    if (!task || !task->process || !task->process->vm)
        return -EINVAL;
    if (!addr || !IS_PAGE_ALIGNED(start) || length == 0)
        return -EINVAL;

    size = ALIGN_UP(length, PAGE_SIZE);
    if (size == 0 || start + size <= start || start + size > get_split_boundary())
        return -EINVAL;

    return vm_unmap_range(task->process->vm, start, size);
}

int sys_mprotect(void* addr, size_t length, int prot)
{
    task_t *task = task_current_local();
    vm_space_t *vm;
    vma_t *vma;
    vaddr_t start = (vaddr_t)addr;
    size_t size;
    uint32_t flags = 0;

    if (!task || !task->process || !task->process->vm)
        return -EINVAL;
    if (!addr || !IS_PAGE_ALIGNED(start) || length == 0)
        return -EINVAL;
    if (prot == 0 || (prot & ~(ARMOS_PROT_READ | ARMOS_PROT_WRITE | ARMOS_PROT_EXEC)))
        return -EINVAL;

    size = ALIGN_UP(length, PAGE_SIZE);
    if (size == 0 || start + size <= start || start + size > get_split_boundary())
        return -EINVAL;

    vm = task->process->vm;
    vma = find_vma(vm, start);
    if (!vma || start != vma->start || start + size != vma->end)
        return -ENOSYS;

    if (prot & ARMOS_PROT_READ)
        flags |= VMA_READ;
    if (prot & ARMOS_PROT_WRITE)
        flags |= VMA_WRITE;
    if (prot & ARMOS_PROT_EXEC)
        flags |= VMA_EXEC;
    if ((vma->flags & VMA_LAZY) && !(flags & VMA_EXEC))
        flags |= VMA_LAZY;

    for (vaddr_t page = start; page < start + size; page += PAGE_SIZE) {
        paddr_t phys = get_physical_address(vm->pgdir, page);
        int ret;

        if (!phys)
            continue;
        if (flags & VMA_WRITE)
            ret = set_user_page_writable(vm->pgdir, page, vm->asid);
        else
            ret = set_user_page_readonly(vm->pgdir, page, vm->asid);
        if (ret < 0)
            return ret;
    }

    vma->flags = flags;
    return 0;
}
