/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/fs/vfs.c
 * Layer: Kernel / VFS and filesystems
 *
 * Responsibilities:
 * - Provide filesystem-independent VFS operations.
 * - Implement persistent ext2/FAT32/procfs behavior.
 *
 * Notes:
 * - Keep file descriptor and inode ownership rules explicit.
 */

#include <kernel/vfs.h>
#include <kernel/fat32.h>
#include <kernel/ext2.h>
#include <kernel/procfs.h>
#include <kernel/block_device.h>
#include <kernel/disk_layout.h>
#include <kernel/mount.h>
#include <kernel/memory.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/uart.h>
#include <kernel/kprintf.h>
#include <kernel/task.h>
#include <kernel/file.h>

#define MAX_INODES 1024
#define MAX_SYMLINK_DEPTH 8

static inode_t* inode_table[MAX_INODES] = {0};
static inode_t* root_inode = NULL;
static uint32_t next_inode_number = 1;
static spinlock_t vfs_lock;
static spinlock_t fd_table_lock = SPINLOCK_INIT("fd_table");

/* Mount table */
#define MAX_MOUNTS 8
typedef struct {
    char     path[128];
    char     source[64];
    char     fstype[16];
    char     options[64];
    inode_t* root;
} mount_entry_t;
static mount_entry_t mount_table[MAX_MOUNTS];
static int mount_count = 0;

bool vfs_install_root(inode_t* root)
{
    uint32_t hash;

    if (!root || root_inode)
        return false;
    init_spinlock(&vfs_lock);
    memset(inode_table, 0, sizeof(inode_table));
    memset(mount_table, 0, sizeof(mount_table));
    mount_count = 0;
    next_inode_number = root->ino + 1u;
    root_inode = root;
    if (root_inode->ref_count == 0)
        root_inode->ref_count = 1;
    hash = root_inode->ino % MAX_INODES;
    root_inode->next = inode_table[hash];
    inode_table[hash] = root_inode;
    return true;
}

static const char* vfs_partition_name(disk_partition_id_t id, const char* fallback)
{
    const disk_partition_t* part = disk_partition_get(id);

    return part && part->name ? part->name : fallback;
}

static void vfs_mount_defaults(const char* path, char* source, size_t source_size,
                               char* fstype, size_t fstype_size,
                               char* options, size_t options_size)
{
    if (strcmp(path, "/proc") == 0) {
        strncpy(source, "proc", source_size - 1);
        strncpy(fstype, "proc", fstype_size - 1);
        strncpy(options, "rw,nosuid,nodev,noexec", options_size - 1);
    } else if (strcmp(path, "/mnt") == 0) {
        strncpy(source,
                vfs_partition_name(DISK_PART_FAT32_MNT, "fat32"),
                source_size - 1);
        strncpy(fstype, "fat32", fstype_size - 1);
        strncpy(options, "rw", options_size - 1);
    } else {
        strncpy(source, "none", source_size - 1);
        strncpy(fstype, "unknown", fstype_size - 1);
        strncpy(options, "rw", options_size - 1);
    }

    source[source_size - 1] = '\0';
    fstype[fstype_size - 1] = '\0';
    options[options_size - 1] = '\0';
}

bool vfs_is_mounted(const char* path)
{
    if (!path)
        return false;

    for (int i = 0; i < mount_count; i++) {
        if (strcmp(path, mount_table[i].path) == 0)
            return true;
    }

    return false;
}

bool vfs_is_mountpoint(const char* path)
{
    if (!path)
        return false;

    if (strcmp(path, "/") == 0)
        return true;

    return vfs_is_mounted(path);
}

void vfs_begin_mutation(void)
{
    /*
     * Placeholder for a future dentry/inode lock layer.
     *
     * Do not serialize all VFS mutations here with a global cooperative lock:
     * ext2 already owns a re-entrant operation lock, and path lookup may take
     * that lock before syscall code knows whether it will mutate. A second VFS
     * global lock creates lock-order inversions under parallel mkdir/rename/
     * unlink stress. Backend-local locks are the safe model until ArmOS grows
     * ordered per-inode locks.
     */
}

void vfs_end_mutation(void)
{
}

int vfs_mount_ex(const char* path, inode_t* root, const char* source,
                 const char* fstype, const char* options)
{
    if (!path || !root)
        return -EINVAL;
    if (mount_count >= MAX_MOUNTS)
        return -ENOSPC;
    if (strcmp(path, "/") == 0 || vfs_is_mounted(path))
        return -EBUSY;

    strncpy(mount_table[mount_count].path, path, 127);
    mount_table[mount_count].path[127] = '\0';
    if (source && fstype && options) {
        strncpy(mount_table[mount_count].source, source,
                sizeof(mount_table[mount_count].source) - 1);
        strncpy(mount_table[mount_count].fstype, fstype,
                sizeof(mount_table[mount_count].fstype) - 1);
        strncpy(mount_table[mount_count].options, options,
                sizeof(mount_table[mount_count].options) - 1);
        mount_table[mount_count].source[sizeof(mount_table[mount_count].source) - 1] = '\0';
        mount_table[mount_count].fstype[sizeof(mount_table[mount_count].fstype) - 1] = '\0';
        mount_table[mount_count].options[sizeof(mount_table[mount_count].options) - 1] = '\0';
    } else {
        vfs_mount_defaults(path,
                           mount_table[mount_count].source,
                           sizeof(mount_table[mount_count].source),
                           mount_table[mount_count].fstype,
                           sizeof(mount_table[mount_count].fstype),
                           mount_table[mount_count].options,
                           sizeof(mount_table[mount_count].options));
    }
    mount_table[mount_count].root = root;
    mount_count++;
    KINFO("[VFS] Mounted '%s'\n", path);
    return 0;
}

int vfs_mount(const char* path, inode_t* root)
{
    return vfs_mount_ex(path, root, NULL, NULL, NULL);
}

int vfs_umount(const char* path)
{
    if (!path || strcmp(path, "/") == 0)
        return -EINVAL;

    for (int i = 0; i < mount_count; i++) {
        if (strcmp(path, mount_table[i].path) == 0) {
            inode_t* root = mount_table[i].root;

            for (int j = i + 1; j < mount_count; j++)
                mount_table[j - 1] = mount_table[j];

            mount_count--;
            memset(&mount_table[mount_count], 0, sizeof(mount_table[mount_count]));
            put_inode(root);
            KINFO("[VFS] Unmounted '%s'\n", path);
            return 0;
        }
    }

    return -EINVAL;
}

static void vfs_mounts_append(char* buf, size_t cap, size_t* len,
                              const char* fmt, ...)
{
    va_list args;
    int written;

    if (!buf || !len || *len >= cap)
        return;

    va_start(args, fmt);
    written = vsnprintf(buf + *len, (int)(cap - *len), fmt, args);
    va_end(args);

    if (written < 0)
        return;

    if ((size_t)written >= cap - *len)
        *len = cap - 1;
    else
        *len += (size_t)written;
}

void vfs_format_mounts(char* buf, size_t cap, size_t* len)
{
    vfs_mounts_append(buf, cap, len, "%s / ext2 rw 0 0\n",
                      vfs_partition_name(DISK_PART_EXT2_ROOT, "root"));

    for (int i = 0; i < mount_count; i++) {
        vfs_mounts_append(buf, cap, len, "%s %s %s %s 0 0\n",
                          mount_table[i].source,
                          mount_table[i].path,
                          mount_table[i].fstype,
                          mount_table[i].options);
    }
}

static bool vfs_path_is_mount_child(const char* path, const char* mount_path)
{
    size_t len;

    if (!path || !mount_path)
        return false;
    if (strcmp(mount_path, "/") == 0)
        return true;

    len = strlen(mount_path);
    if (strncmp(path, mount_path, len) != 0)
        return false;
    return path[len] == '\0' || path[len] == '/';
}

int vfs_statfs(const char* path, struct statfs* st)
{
    int best = -1;
    size_t best_len = 0;

    if (!path || !st)
        return -EINVAL;

    for (int i = 0; i < mount_count; i++) {
        size_t len = strlen(mount_table[i].path);
        if (len > best_len && vfs_path_is_mount_child(path, mount_table[i].path)) {
            best = i;
            best_len = len;
        }
    }

    if (best >= 0) {
        if (strcmp(mount_table[best].fstype, "fat32") == 0)
            return fat32_statfs(st);
        if (strcmp(mount_table[best].fstype, "ext2") == 0)
            return ext2_statfs(st);
        return -ENOSYS;
    }

    if (vfs_path_is_mount_child(path, "/"))
        return ext2_statfs(st);

    return -ENOENT;
}

int vfs_sync(void)
{
    int ret = 0;

    if (is_dirty_inodes())
        sync_dirty_inodes();
    if (is_fat_dirty() && sync_fat_to_disk() < 0)
        ret = -EIO;
    if (ext2_sync() < 0)
        ret = -EIO;
    if (blk_flush() < 0)
        ret = -EIO;

    return ret;
}

int vfs_shutdown(void)
{
    int ret;

    kprintf("Shutdown: VFS mounted filesystems\n");
    kprintf("Shutdown:   %s on / type ext2 (rw)\n",
            vfs_partition_name(DISK_PART_EXT2_ROOT, "root"));
    for (int i = 0; i < mount_count; i++) {
        kprintf("Shutdown:   %s on %s type %s (%s)\n",
              mount_table[i].source,
              mount_table[i].path,
              mount_table[i].fstype,
              mount_table[i].options);
    }

    kprintf("Shutdown: VFS sync start\n");
    ret = vfs_sync();
    if (ret < 0)
        KERROR("Shutdown: VFS sync failed (%d)\n", ret);
    else
        kprintf("Shutdown: VFS sync complete\n");

    /*
     * Unmount optional filesystems after sync, in reverse mount order.
     * The root ext2 filesystem is kept mounted; the block device flush below
     * is the final persistence barrier before PSCI SYSTEM_OFF.
     */
    kprintf("Shutdown: VFS unmount non-root filesystems\n");
    while (mount_count > 0) {
        char path[sizeof(mount_table[0].path)];
        char source[sizeof(mount_table[0].source)];
        char fstype[sizeof(mount_table[0].fstype)];
        int idx = mount_count - 1;
        int umount_ret;

        strncpy(path, mount_table[idx].path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        strncpy(source, mount_table[idx].source, sizeof(source) - 1);
        source[sizeof(source) - 1] = '\0';
        strncpy(fstype, mount_table[idx].fstype, sizeof(fstype) - 1);
        fstype[sizeof(fstype) - 1] = '\0';

        kprintf("Shutdown:   unmount %s on %s type %s\n",
              source, path, fstype);
        umount_ret = vfs_umount(path);
        if (umount_ret < 0) {
            KERROR("Shutdown:   unmount %s failed (%d)\n", path, umount_ret);
            ret = ret < 0 ? ret : umount_ret;
            break;
        }
    }

    kprintf("Shutdown:   root / type ext2 remains mounted, synced\n");
    return ret;
}

/* File operations for FAT32 */
extern file_operations_t fat32_file_ops;
extern file_operations_t fat32_dir_ops;
extern inode_operations_t fat32_inode_ops;



/*void test_root_directory(void) {
    if (!root_inode || !root_inode->f_op || !root_inode->f_op->readdir) {
        KERROR("[TEST] Root directory not readable\n");
        return;
    }
    
    // Essayer de lire le contenu du répertoire racine
    struct dirent entries[25];
    int count = root_inode->f_op->readdir(root_inode, entries);
    
    KINFO("[TEST] Root directory contains %d entries:\n", count);
    for (int i = 0; i < count; i++) {
        KINFO("[TEST]   - %s (type: %d)\n", entries[i].d_name, entries[i].d_type);
    }
}*/

uint32_t get_next_inode_number(void){
    return next_inode_number++; 
}

static void vfs_get_inode_ref(inode_t* inode)
{
    unsigned long flags;

    if (!inode) return;

    spin_lock_irqsave(&vfs_lock, &flags);
    inode->ref_count++;
    spin_unlock_irqrestore(&vfs_lock, flags);
}

void vfs_inode_opened(inode_t* inode)
{
    unsigned long flags;

    if (!inode) return;

    spin_lock_irqsave(&vfs_lock, &flags);
    inode->open_count++;
    spin_unlock_irqrestore(&vfs_lock, flags);
}

void vfs_inode_closed(inode_t* inode)
{
    unsigned long flags;

    if (!inode) return;

    spin_lock_irqsave(&vfs_lock, &flags);
    if (inode->open_count > 0)
        inode->open_count--;
    spin_unlock_irqrestore(&vfs_lock, flags);
}

uint32_t vfs_inode_open_count(inode_t* inode)
{
    unsigned long flags;
    uint32_t count;

    if (!inode) return 0;

    spin_lock_irqsave(&vfs_lock, &flags);
    count = inode->open_count;
    spin_unlock_irqrestore(&vfs_lock, flags);
    return count;
}

bool init_vfs(void)
{
    const disk_partition_t* ext2_part;
    inode_t* proc_root;
    int fstab_mounts;

    KINFO("[VFS] Starting VFS initialization...\n");
    
    init_spinlock(&vfs_lock);

    ext2_part = disk_partition_get(DISK_PART_EXT2_ROOT);
    if (!ext2_part) {
        KERROR("[VFS] Disk layout is invalid\n");
        return false;
    }

    /* Mount the ext2 root filesystem from the partition declared in disk_layout.h. */
    KINFO("[VFS] Mounting %s (%s) at LBA %u\n",
          ext2_part->name, ext2_part->mountpoint, (uint32_t)ext2_part->lba_start);
    inode_t* ext2_root = ext2_mount(ext2_part->lba_start);
    if (!ext2_root) {
        KERROR("[VFS] ext2 not found on %s — root unavailable\n", ext2_part->name);
        return false;
    }

    root_inode = ext2_root;

    KINFO("[VFS] Root inode created from ext2:\n");
    KINFO("[VFS]   Inode number: %u\n", root_inode->ino);
    KINFO("[VFS]   Mode: 0x%04X (%s)\n", root_inode->mode,
          S_ISDIR(root_inode->mode) ? "directory" : "other");
    KINFO("[VFS]   First cluster/inode: %u\n", root_inode->first_cluster);
    KINFO("[VFS]   Operations: i_op=%p, f_op=%p\n", root_inode->i_op, root_inode->f_op);

    if (root_inode->f_op && root_inode->f_op->readdir) {
        KINFO("[VFS] OK ext2 root inode has readdir operation\n");
    } else {
        KERROR("[VFS] KO ext2 root inode missing readdir operation\n");
        return false;
    }

    //test_root_directory();

    fstab_mounts = vfs_mount_from_fstab(FSTAB_PATH);
    if (fstab_mounts < 0)
        KINFO("[VFS] No optional fstab mounts loaded (%d)\n", fstab_mounts);

    proc_root = procfs_mount();
    if (!proc_root || vfs_mount("/proc", proc_root) != 0) {
        KERROR("[VFS] procfs mount failed\n");
        if (proc_root) put_inode(proc_root);
        return false;
    }

    KINFO("[VFS] OK VFS initialized successfully\n");
    return true;
}

bool init_vfs2(void)
{
    KDEBUG("Starting init_vfs ....\n");
    init_spinlock(&vfs_lock);
    KDEBUG("init_spinlock Ok ....\n");

    /* Mount FAT32 */
    if (!fat32_mount()) {
        KERROR("VFS: Failed to mount FAT32\n");
        return false;
    }

    KDEBUG("fat32_mount Ok ....\n");

    /* Create root inode */
    root_inode = create_inode();
    if (!root_inode) {
        return false;
    }
    
    KDEBUG("create_inode Ok ....\n");

    root_inode->ino = 1;
    root_inode->mode = S_IFDIR | 0777;
    root_inode->uid = 0;
    root_inode->gid = 0;
    root_inode->size = 0;
    root_inode->nlink = 1;
    root_inode->first_cluster = get_fat32_root_cluster();
    root_inode->i_op = &fat32_inode_ops;
    root_inode->f_op = &fat32_dir_ops;
    
    KDEBUG("VFS: Initialized\n");
    return true;
}

inode_t* create_inode(void)
{
    inode_t* inode;
    uint32_t hash;
    
    inode = (inode_t*)kmalloc(sizeof(inode_t));
    if (!inode) return NULL;
    
    memset(inode, 0, sizeof(inode_t));
    
    unsigned long flags;
    spin_lock_irqsave(&vfs_lock, &flags);
    inode->ino = get_next_inode_number();
    spin_unlock_irqrestore(&vfs_lock, flags);
    
    inode->ref_count = 1;
    inode->nlink = 1;
    
    /* Add to hash table */
    hash = inode->ino % MAX_INODES;
    spin_lock_irqsave(&vfs_lock, &flags);
    inode->next = inode_table[hash];
    inode_table[hash] = inode;
    spin_unlock_irqrestore(&vfs_lock, flags);
    
    return inode;
}

inode_t* get_inode(uint32_t ino)
{
    uint32_t hash;
    inode_t* inode;
    unsigned long flags;
    
    hash = ino % MAX_INODES;
    
    spin_lock_irqsave(&vfs_lock, &flags);
    inode = inode_table[hash];
    
    while (inode) {
        if (inode->ino == ino) {
            inode->ref_count++;
            spin_unlock_irqrestore(&vfs_lock, flags);
            return inode;
        }
        inode = inode->next;
    }
    spin_unlock_irqrestore(&vfs_lock, flags);
    
    return NULL;
}

inode_t* vfs_get_backing_inode(inode_operations_t* operations,
                               uint32_t backing_id)
{
    inode_t* inode;
    unsigned long flags;
    uint32_t hash;

    if (!operations)
        return NULL;

    spin_lock_irqsave(&vfs_lock, &flags);
    for (hash = 0; hash < MAX_INODES; hash++) {
        for (inode = inode_table[hash]; inode; inode = inode->next) {
            if (inode->i_op == operations &&
                inode->first_cluster == backing_id) {
                inode->ref_count++;
                spin_unlock_irqrestore(&vfs_lock, flags);
                return inode;
            }
        }
    }
    spin_unlock_irqrestore(&vfs_lock, flags);
    return NULL;
}

void put_inode(inode_t* inode)
{
    uint32_t hash;
    inode_t** current;
    unsigned long flags;
    
    if (!inode) return;
    
    spin_lock_irqsave(&vfs_lock, &flags);
    inode->ref_count--;
    
    if (inode->ref_count == 0) {
        /* Remove from hash table */
        hash = inode->ino % MAX_INODES;
        current = &inode_table[hash];
        
        while (*current) {
            if (*current == inode) {
                *current = inode->next;
                break;
            }
            current = &(*current)->next;
        }
        
        spin_unlock_irqrestore(&vfs_lock, flags);
        if (inode->release_private)
            inode->release_private(inode);
        kfree(inode);
    } else {
        spin_unlock_irqrestore(&vfs_lock, flags);
    }
}

static bool vfs_append_component(char* path, size_t path_size, const char* component)
{
    size_t len;
    size_t available;
    int written;

    if (!path || !component || path_size == 0) return false;

    len = strlen(path);
    if (len >= path_size) return false;
    available = path_size - len;
    if (len == 0) {
        written = snprintf(path, (int)path_size, "/%s", component);
    } else if (strcmp(path, "/") == 0) {
        written = snprintf(path + 1, (int)path_size - 1, "%s", component);
        available = path_size - 1u;
    } else {
        written = snprintf(path + len, (int)available, "/%s", component);
    }
    return written >= 0 && (size_t)written < available;
}

static void vfs_remove_last_component(char* path)
{
    char* slash;

    if (!path || path[0] == '\0' || strcmp(path, "/") == 0)
        return;
    slash = strrchr(path, '/');
    if (!slash || slash == path) {
        strcpy(path, "/");
        return;
    }
    *slash = '\0';
}

static inode_t* path_lookup_internal(const char* path, bool follow_final_symlink,
                                     int depth, char* resolved,
                                     size_t resolved_size)
{
    inode_t* current;
    char*    path_copy;
    char*    token;
    char*    saveptr;
    char     current_path[MAX_PATH];

    if (!path || path[0] != '/') return NULL;
    if (depth > MAX_SYMLINK_DEPTH) return NULL;

    current = root_inode;
    vfs_get_inode_ref(current);

    if (strcmp(path, "/") == 0) {
        if (resolved && resolved_size > 1u)
            strcpy(resolved, "/");
        return current;
    }

    path_copy = strdup(path);
    if (!path_copy) { put_inode(current); return NULL; }

    current_path[0] = '\0';
    saveptr = NULL;
    token = strtok_r(path_copy + 1, "/", &saveptr);

    while (token && current) {
        char* next_token;
        bool is_final;
        char parent_path[MAX_PATH];

        if (!S_ISDIR(current->mode)) {
            put_inode(current);
            current = NULL;
            break;
        }

        next_token = strtok_r(NULL, "/", &saveptr);
        is_final = (next_token == NULL);

        if (strcmp(token, ".") == 0) {
            token = next_token;
            continue;
        }
        if (strcmp(token, "..") == 0) {
            inode_t* parent;

            vfs_remove_last_component(current_path);
            put_inode(current);
            parent = path_lookup_internal(current_path[0] ? current_path : "/",
                                          true, depth, NULL, 0u);
            if (!parent) {
                current = NULL;
                break;
            }
            current = parent;
            token = next_token;
            continue;
        }

        if (current_path[0] == '\0')
            strcpy(parent_path, "/");
        else
            strncpy(parent_path, current_path, sizeof(parent_path) - 1);
        parent_path[sizeof(parent_path) - 1] = '\0';

        /* Build accumulated path and allow purely virtual mount points. */
        if (!vfs_append_component(current_path, sizeof(current_path), token)) {
            put_inode(current);
            current = NULL;
            break;
        }

        for (int i = 0; i < mount_count; i++) {
            if (strcmp(current_path, mount_table[i].path) == 0) {
                put_inode(current);
                current = mount_table[i].root;
                vfs_get_inode_ref(current);
                goto mounted_component;
            }
        }

        inode_t* next = current->i_op->lookup(current, token);
        put_inode(current);
        current = next;

mounted_component:
        if (current) {
            if (S_ISLNK(current->mode) && (!is_final || follow_final_symlink)) {
                char target[MAX_PATH];
                char remaining[MAX_PATH];
                char new_path[MAX_PATH];
                int ret;
                int written;

                if (!current->i_op || !current->i_op->readlink) {
                    put_inode(current);
                    current = NULL;
                    break;
                }

                ret = current->i_op->readlink(current, target, sizeof(target) - 1);
                if (ret < 0 || ret >= (int)sizeof(target)) {
                    put_inode(current);
                    current = NULL;
                    break;
                }
                target[ret] = '\0';

                remaining[0] = '\0';
                while (next_token) {
                    if (!vfs_append_component(remaining, sizeof(remaining), next_token)) {
                        put_inode(current);
                        kfree(path_copy);
                        return NULL;
                    }
                    next_token = strtok_r(NULL, "/", &saveptr);
                }

                if (target[0] == '/') {
                    written = snprintf(new_path, (int)sizeof(new_path), "%s%s",
                                       target, remaining);
                } else if (strcmp(parent_path, "/") == 0) {
                    written = snprintf(new_path, (int)sizeof(new_path), "/%s%s",
                                       target, remaining);
                } else {
                    written = snprintf(new_path, (int)sizeof(new_path), "%s/%s%s",
                                       parent_path, target, remaining);
                }
                if (written < 0 || written >= (int)sizeof(new_path)) {
                    put_inode(current);
                    kfree(path_copy);
                    return NULL;
                }

                put_inode(current);
                kfree(path_copy);
                return path_lookup_internal(new_path, follow_final_symlink,
                                            depth + 1, resolved, resolved_size);
            }
        }

        token = next_token;
    }

    kfree(path_copy);
    if (current && resolved) {
        const char* final_path = current_path[0] ? current_path : "/";
        if (strlen(final_path) + 1u > resolved_size) {
            put_inode(current);
            return NULL;
        }
        strcpy(resolved, final_path);
    }
    return current;
}

inode_t* path_lookup_ex(const char* path, bool follow_final_symlink)
{
    return path_lookup_internal(path, follow_final_symlink, 0, NULL, 0u);
}

inode_t* path_lookup_resolved(const char* path, bool follow_final_symlink,
                              char* resolved, size_t resolved_size)
{
    if (!resolved || resolved_size == 0u)
        return NULL;
    resolved[0] = '\0';
    return path_lookup_internal(path, follow_final_symlink, 0,
                                resolved, resolved_size);
}

inode_t* path_lookup(const char* path)
{
    return path_lookup_ex(path, true);
}

int vfs_kernel_file_open(const char* path, kernel_file_t* handle)
{
    int ret;

    if (!path || !handle)
        return -EINVAL;
    memset(handle, 0, sizeof(*handle));
    handle->inode = path_lookup(path);
    if (!handle->inode)
        return -ENOENT;
    if (!S_ISREG(handle->inode->mode) || !handle->inode->f_op ||
        !handle->inode->f_op->read) {
        put_inode(handle->inode);
        handle->inode = NULL;
        return -EINVAL;
    }

    handle->file.inode = handle->inode;
    handle->file.flags = O_RDONLY;
    handle->file.ref_count = 1u;
    handle->file.f_op = handle->inode->f_op;
    handle->file.offset = 0u;
    if (handle->file.f_op->open) {
        ret = handle->file.f_op->open(handle->inode, &handle->file);
        if (ret < 0) {
            put_inode(handle->inode);
            handle->inode = NULL;
            return ret;
        }
    }
    handle->opened = true;
    return 0;
}

ssize_t vfs_kernel_file_read(kernel_file_t* handle, void* buffer,
                             size_t count)
{
    if (!handle || !handle->opened || !buffer || !handle->file.f_op ||
        !handle->file.f_op->read)
        return -EINVAL;
    return handle->file.f_op->read(&handle->file, buffer, count);
}

uint32_t vfs_kernel_file_size(const kernel_file_t* handle)
{
    if (!handle || !handle->opened || !handle->inode)
        return 0u;
    return handle->inode->size;
}

void vfs_kernel_file_close(kernel_file_t* handle)
{
    if (!handle || !handle->opened)
        return;
    if (handle->file.f_op && handle->file.f_op->close)
        handle->file.f_op->close(&handle->file);
    put_inode(handle->inode);
    memset(handle, 0, sizeof(*handle));
}

int vfs_kernel_dir_open(const char* path, kernel_file_t* handle)
{
    int ret;

    if (!path || !handle)
        return -EINVAL;
    memset(handle, 0, sizeof(*handle));
    handle->inode = path_lookup(path);
    if (!handle->inode)
        return -ENOENT;
    if (!S_ISDIR(handle->inode->mode) || !handle->inode->f_op ||
        !handle->inode->f_op->readdir) {
        put_inode(handle->inode);
        handle->inode = NULL;
        return -ENOTDIR;
    }

    handle->file.inode = handle->inode;
    handle->file.flags = O_RDONLY;
    handle->file.ref_count = 1u;
    handle->file.f_op = handle->inode->f_op;
    handle->file.offset = 0u;
    if (handle->file.f_op->open) {
        ret = handle->file.f_op->open(handle->inode, &handle->file);
        if (ret < 0) {
            put_inode(handle->inode);
            handle->inode = NULL;
            return ret;
        }
    }
    handle->opened = true;
    return 0;
}

int vfs_kernel_dir_read(kernel_file_t* handle, dirent_t* entry)
{
    if (!handle || !handle->opened || !entry || !handle->file.f_op ||
        !handle->file.f_op->readdir)
        return -EINVAL;
    return handle->file.f_op->readdir(&handle->file, entry);
}

/**
 * Allouer un descripteur de fichier - CORRIGe
 */
int allocate_fd(task_t* process)
{
    int i;
    uint32_t limit;
    
    if (!process || !process->process) {
        KERROR("NULL PROC\n");
        return -EINVAL;
    }
    
    limit = vfs_fd_limit(process);
    for (i = 0; i < (int)limit; i++) {
        if (process->process->files[i] == NULL) {
            return i;
        }
    }
    
    return -EMFILE;
}

int vfs_install_file(task_t *task, file_t *file, uint32_t descriptor_flags)
{
    uint32_t limit;
    unsigned long flags;
    int fd = -EMFILE;

    if (!task || !task->process || !file)
        return -EINVAL;
    limit = vfs_fd_limit(task);
    spin_lock_irqsave(&fd_table_lock, &flags);
    for (uint32_t index = 0u; index < limit; index++) {
        if (!task->process->files[index]) {
            task->process->files[index] = file;
            task->process->fd_flags[index] = descriptor_flags;
            fd = (int)index;
            break;
        }
    }
    spin_unlock_irqrestore(&fd_table_lock, flags);
    return fd;
}

file_t *vfs_get_file_from_fd(task_t *task, int fd)
{
    file_t *file = NULL;
    unsigned long flags;

    if (!task || !task->process || fd < 0 || fd >= MAX_FILES)
        return NULL;
    spin_lock_irqsave(&fd_table_lock, &flags);
    if (task->process->files[fd])
        file = get_file(task->process->files[fd]);
    spin_unlock_irqrestore(&fd_table_lock, flags);
    return file;
}

file_t *vfs_take_file(task_t *task, int fd)
{
    file_t *file;
    unsigned long flags;

    if (!task || !task->process || fd < 0 || fd >= MAX_FILES)
        return NULL;
    spin_lock_irqsave(&fd_table_lock, &flags);
    file = task->process->files[fd];
    task->process->files[fd] = NULL;
    task->process->fd_flags[fd] = 0u;
    spin_unlock_irqrestore(&fd_table_lock, flags);
    return file;
}

uint32_t vfs_fd_limit(task_t* process)
{
    uint32_t limit;

    if (!process || !process->process)
        return 0;
    limit = process->process->rlimit_nofile_cur;
    return limit < MAX_FILES ? limit : MAX_FILES;
}


void free_fd(task_t* proc, int fd)
{
    (void)vfs_take_file(proc, fd);
}

inode_t* get_root_inode(void)
{
    if (root_inode) {
        vfs_get_inode_ref(root_inode);
        return root_inode;
    }
    return NULL;
}

void close_cloexec_files(task_t* proc)
{
    int i;

    if (!proc || !proc->process) {
        KERROR("NULL PROC\n");
        return ;
    }

    
    for (i = 0; i < MAX_FILES; i++) {
        if (proc->process->files[i] && (proc->process->fd_flags[i] & O_CLOEXEC)) {
            close_file(proc->process->files[i]);
            proc->process->files[i] = NULL;
            proc->process->fd_flags[i] = 0;
        }
    }
}
