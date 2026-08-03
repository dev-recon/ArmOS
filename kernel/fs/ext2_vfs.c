/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/fs/ext2_vfs.c
 * Layer: Kernel / VFS and filesystems
 *
 * Responsibilities:
 * - Provide filesystem-independent VFS operations.
 * - Implement persistent ext2/FAT32/procfs behavior.
 *
 * Notes:
 * - Keep file descriptor and inode ownership rules explicit.
 */

#include <kernel/ext2.h>
#include <kernel/vfs.h>
#include <kernel/memory.h>
#include <kernel/address_space.h>
#include <kernel/string.h>
#include <kernel/kprintf.h>
#include <kernel/file.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <kernel/virtio_block.h>
#include <kernel/stdarg.h>
#include <kernel/arch_barrier.h>

extern int blk_read_sectors(uint64_t lba, uint32_t count, void* buffer);
extern int blk_write_sectors(uint64_t lba, uint32_t count, void* buffer);
extern inode_t* create_inode(void);
extern void put_inode(inode_t* inode);
extern uint32_t get_current_time(void);

static ext2_fs_t ext2_fs;

static int ext2_read_disk_inode(uint32_t ino, ext2_inode_t* out);
static int ext2_write_disk_inode(uint32_t ino, const ext2_inode_t* in);
static int ext2_read_group_desc(uint32_t group, ext2_group_desc_t* out);
static int ext2_flush_pending_inodes(void);

/* Forward declarations */
static inode_t* ext2_inode_lookup(inode_t* dir, const char* name);
static inode_t* ext2_inode_lookup_op(inode_t* dir, const char* name);
static int      ext2_inode_mkdir(inode_t* dir, const char* name, uint16_t mode);
static int      ext2_inode_unlink(inode_t* dir, const char* name);
static int      ext2_inode_rmdir(inode_t* dir, const char* name);
static int      ext2_inode_rename(inode_t* old_dir, const char* old_name,
                                  inode_t* new_dir, const char* new_name);
static int      ext2_inode_readlink(inode_t* inode, char* buf, size_t bufsiz);
static int      ext2_inode_mkdir_op(inode_t* dir, const char* name, uint16_t mode);
static int      ext2_inode_unlink_op(inode_t* dir, const char* name);
static int      ext2_inode_rmdir_op(inode_t* dir, const char* name);
static int      ext2_inode_rename_op(inode_t* old_dir, const char* old_name,
                                     inode_t* new_dir, const char* new_name);
static int      ext2_inode_readlink_op(inode_t* inode, char* buf, size_t bufsiz);
static int      ext2_inode_create_op(inode_t* dir, const char* name, uint16_t mode);
static int      ext2_truncate_inode_data(inode_t* inode, bool allow_dir);
static int      ext2_truncate_inode_unlocked(inode_t* inode);
static int      ext2_remove_dir_entry(inode_t* dir, const char* name);
static int      ext2_adjust_link_count(inode_t* inode, int delta);
static ssize_t  ext2_file_read(file_t* file, void* buffer, size_t count);
static ssize_t  ext2_file_read_unlocked(file_t* file, void* buffer, size_t count);
static ssize_t  ext2_file_write(file_t* file, const void* buffer, size_t count);
static int      ext2_file_open(inode_t* inode, file_t* file);
static int      ext2_file_close(file_t* file);
static off_t    ext2_file_lseek(file_t* file, off_t offset, int whence);
static int      ext2_file_truncate(file_t* file, off_t length);
static int      ext2_dir_readdir(file_t* file, dirent_t* dirent);
static int      ext2_dir_readdir_unlocked(file_t* file, dirent_t* dirent);
static ssize_t  ext2_file_write_op(file_t* file, const void* buffer, size_t count);

file_operations_t ext2_file_ops = {
    .read    = ext2_file_read,
    .write   = ext2_file_write_op,
    .open    = ext2_file_open,
    .close   = ext2_file_close,
    .lseek   = ext2_file_lseek,
    .readdir = NULL,
    .truncate = ext2_file_truncate,
};

file_operations_t ext2_dir_ops = {
    .read    = NULL,
    .write   = NULL,
    .open    = ext2_file_open,
    .close   = ext2_file_close,
    .lseek   = NULL,
    .readdir = ext2_dir_readdir,
};

inode_operations_t ext2_inode_ops = {
    .lookup = ext2_inode_lookup_op,
    .create = ext2_inode_create_op,
    .mkdir  = ext2_inode_mkdir_op,
    .unlink = ext2_inode_unlink_op,
    .rmdir  = ext2_inode_rmdir_op,
    .rename = ext2_inode_rename_op,
    .readlink = ext2_inode_readlink_op,
};

/*
 * Set-associative block cache: 256 sets x 8 ways = 2048 blocks.
 *
 * A set lookup bounds the hot path to eight comparisons regardless of total
 * cache size. Buffers are allocated lazily from the page allocator rather than
 * kmalloc: the kmalloc heap is small and should stay focused on small objects.
 * Fully warmed cost is 2048 pages (8 MiB) on the default 2 GiB QEMU machine.
 */
#define EXT2_BLOCK_CACHE_SETS 256u
#define EXT2_BLOCK_CACHE_WAYS 8u
#define EXT2_BLOCK_CACHE_SIZE (EXT2_BLOCK_CACHE_SETS * EXT2_BLOCK_CACHE_WAYS)

_Static_assert((EXT2_BLOCK_CACHE_SETS & (EXT2_BLOCK_CACHE_SETS - 1u)) == 0,
               "EXT2_BLOCK_CACHE_SETS must be a power of two");

typedef struct ext2_block_cache_entry {
    bool valid;
    bool dirty;
    uint32_t block;
    uint8_t* data;
} ext2_block_cache_entry_t;

static ext2_block_cache_entry_t ext2_block_cache[EXT2_BLOCK_CACHE_SIZE];
static uint32_t ext2_block_cache_set_clock[EXT2_BLOCK_CACHE_SETS];
static spinlock_t ext2_cache_lock = SPINLOCK_INIT("ext2_cache");
static volatile bool ext2_cache_busy;
static task_t* ext2_cache_owner;
static spinlock_t ext2_op_lock = SPINLOCK_INIT("ext2_op");
static volatile bool ext2_op_busy;
static task_t* ext2_op_owner;
static uint32_t ext2_op_depth;
static volatile bool ext2_dirty;
static ext2_stats_t ext2_stats;
static bool ext2_defer_allocation_writes;

#define EXT2_PENDING_INODE_COUNT 128u

typedef struct ext2_pending_inode {
    bool valid;
    uint32_t ino;
    ext2_inode_t inode;
} ext2_pending_inode_t;

static ext2_pending_inode_t ext2_pending_inodes[EXT2_PENDING_INODE_COUNT];
static uint32_t ext2_pending_inode_count;
static uint32_t ext2_alloc_group_hint;
static uint32_t ext2_alloc_bit_hint;

typedef struct ext2_wait_queue {
    spinlock_t lock;
    task_t* waiters[MAX_TASKS];
} ext2_wait_queue_t;

static ext2_wait_queue_t ext2_op_waitq = {
    .lock = SPINLOCK_INIT("ext2_op_waitq"),
};
static ext2_wait_queue_t ext2_cache_waitq = {
    .lock = SPINLOCK_INIT("ext2_cache_waitq"),
};

static void ext2_mark_dirty(void)
{
    ext2_dirty = true;
    ext2_stats.dirty = 1;
}

static bool ext2_wait_prepare(ext2_wait_queue_t* queue, task_t* task)
{
    unsigned long flags;
    bool enqueued = false;

    if (!queue || !task)
        return false;

    task_set_interruptible_until(task, get_system_ticks() + TIMER_FREQ);

    spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (queue->waiters[i] == task) {
            enqueued = true;
            break;
        }
        if (!queue->waiters[i]) {
            queue->waiters[i] = task;
            enqueued = true;
            break;
        }
    }
    spin_unlock_irqrestore(&queue->lock, flags);

    if (!enqueued) {
        task_set_wakeup_time(task, 0);
        task_set_state(task, TASK_RUNNING);
    }

    return enqueued;
}

static void ext2_wait_finish(ext2_wait_queue_t* queue, task_t* task)
{
    unsigned long flags;

    if (!queue || !task)
        return;

    spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (queue->waiters[i] == task) {
            queue->waiters[i] = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&queue->lock, flags);
    task_set_wakeup_time(task, 0);
}

static void ext2_wait_wake_all(ext2_wait_queue_t* queue)
{
    unsigned long flags;
    task_t** wake;
    uint32_t wake_count = 0;

    if (!queue)
        return;
    wake = kmalloc(sizeof(*wake) * MAX_TASKS);
    if (!wake)
        return;

    spin_lock_irqsave(&queue->lock, &flags);
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        task_t* waiter = queue->waiters[i];
        if (!waiter)
            continue;
        queue->waiters[i] = NULL;
        if (waiter->state == TASK_INTERRUPTIBLE ||
            waiter->state == TASK_UNINTERRUPTIBLE) {
            if (wake_count < MAX_TASKS)
                wake[wake_count++] = waiter;
        }
    }
    spin_unlock_irqrestore(&queue->lock, flags);

    for (uint32_t i = 0; i < wake_count; i++) {
        task_wake(wake[i]);
    }
    kfree(wake);
}

static bool ext2_op_try_acquire(void)
{
    task_t* task = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&ext2_op_lock, &flags);
    if (ext2_op_busy && ext2_op_owner && ext2_op_owner == task) {
        ext2_op_depth++;
        spin_unlock_irqrestore(&ext2_op_lock, flags);
        return true;
    }
    if (!ext2_op_busy) {
        ext2_op_busy = true;
        ext2_op_owner = task;
        ext2_op_depth = 1;
        spin_unlock_irqrestore(&ext2_op_lock, flags);
        return true;
    }
    spin_unlock_irqrestore(&ext2_op_lock, flags);
    return false;
}

static void ext2_op_acquire(void)
{
    while (1) {
        task_t* task = task_current_local();

        if (ext2_op_try_acquire())
            return;

        if (!task) {
            arch_cpu_relax();
            continue;
        }

        /*
         * The ext2 backend lock protects on-disk directory and inode updates.
         * Waiting tasks must not stay runnable and spin on yield(): under
         * parallel metadata stress that can starve a lock owner sleeping for
         * VirtIO completion. Register before rechecking the lock; the timeout
         * is only a lost-wakeup safety net.
         */
        ext2_stats.op_waits++;
        if (!ext2_wait_prepare(&ext2_op_waitq, task))
            continue;
        if (ext2_op_try_acquire()) {
            ext2_wait_finish(&ext2_op_waitq, task);
            /*
             * We never actually switched away in this fast recheck path.
             * Restore the visible task state before returning with the ext2
             * lock held, otherwise the caller keeps running kernel code while
             * still reported as TASK_INTERRUPTIBLE.
             */
            task_set_state(task, TASK_RUNNING);
            return;
        }
        schedule();
        ext2_wait_finish(&ext2_op_waitq, task);
    }
}

static void ext2_op_release(void)
{
    task_t* task = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&ext2_op_lock, &flags);
    if (!ext2_op_busy) {
        KERROR("ext2: op release without owner\n");
    } else if (ext2_op_owner && task && ext2_op_owner != task) {
        KERROR("ext2: op release by non-owner\n");
    } else if (ext2_op_depth > 1) {
        ext2_op_depth--;
        spin_unlock_irqrestore(&ext2_op_lock, flags);
        return;
    }

    ext2_op_busy = false;
    ext2_op_owner = NULL;
    ext2_op_depth = 0;
    spin_unlock_irqrestore(&ext2_op_lock, flags);
    ext2_wait_wake_all(&ext2_op_waitq);
}

static bool ext2_cache_try_acquire(void)
{
    task_t* task = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&ext2_cache_lock, &flags);
    if (!ext2_cache_busy) {
        ext2_cache_busy = true;
        ext2_cache_owner = task;
        spin_unlock_irqrestore(&ext2_cache_lock, flags);
        return true;
    }
    spin_unlock_irqrestore(&ext2_cache_lock, flags);
    return false;
}

static void ext2_cache_acquire(void)
{
    while (1) {
        task_t* task = task_current_local();

        if (ext2_cache_try_acquire())
            return;

        if (!task) {
            arch_cpu_relax();
            continue;
        }

        ext2_stats.cache_waits++;
        if (!ext2_wait_prepare(&ext2_cache_waitq, task))
            continue;
        if (ext2_cache_try_acquire()) {
            ext2_wait_finish(&ext2_cache_waitq, task);
            /*
             * Same invariant as ext2_op_acquire(): if the lock became
             * available before schedule(), the task did not sleep and must not
             * leak an interruptible state into the protected section.
             */
            task_set_state(task, TASK_RUNNING);
            return;
        }
        schedule();
        ext2_wait_finish(&ext2_cache_waitq, task);
    }
}

static void ext2_cache_release(void)
{
    task_t* task = task_current_local();
    unsigned long flags;

    spin_lock_irqsave(&ext2_cache_lock, &flags);
    if (!ext2_cache_busy) {
        KERROR("ext2: cache release without owner\n");
    } else if (ext2_cache_owner && task && ext2_cache_owner != task) {
        KERROR("ext2: cache release by non-owner\n");
    }
    ext2_cache_busy = false;
    ext2_cache_owner = NULL;
    spin_unlock_irqrestore(&ext2_cache_lock, flags);
    ext2_wait_wake_all(&ext2_cache_waitq);
}

static bool ext2_valid_disk_inode_ptr(const ext2_inode_t* di)
{
    uintptr_t virtual_start;
    uintptr_t virtual_end;
    paddr_t physical_start;
    paddr_t physical_end;

    if (!di)
        return false;
    virtual_start = (uintptr_t)di;
    virtual_end = virtual_start + sizeof(*di) - 1u;
    if (virtual_end < virtual_start)
        return false;
    physical_start = virt_in_direct_map((vaddr_t)virtual_start) ?
        virt_to_phys((vaddr_t)virtual_start) : (paddr_t)virtual_start;
    physical_end = virt_in_direct_map((vaddr_t)virtual_end) ?
        virt_to_phys((vaddr_t)virtual_end) : (paddr_t)virtual_end;
    return physical_end >= physical_start &&
           IS_VALID_RAM(physical_start) && IS_VALID_RAM(physical_end);
}

/* ---------- block I/O ---------- */

static void ext2_block_cache_reset(void)
{
    for (uint32_t i = 0; i < EXT2_BLOCK_CACHE_SIZE; i++) {
        ext2_block_cache[i].valid = false;
        ext2_block_cache[i].dirty = false;
    }
    for (uint32_t set = 0; set < EXT2_BLOCK_CACHE_SETS; set++)
        ext2_block_cache_set_clock[set] = 0;
}

static uint8_t* ext2_block_cache_data(ext2_block_cache_entry_t* entry)
{
    if (!entry) return NULL;
    if (!entry->data && ext2_fs.block_size) {
        /*
         * One physical page per entry covers all currently supported ext2
         * block sizes (1/2/4 KiB). Entries keep their buffer for the lifetime
         * of the mount; kmalloc is only a fallback for larger future blocks.
         */
        if (ext2_fs.block_size <= PAGE_SIZE) {
            paddr_t phys = (paddr_t)allocate_page();
            entry->data = phys ? (uint8_t*)phys_to_virt(phys) : NULL;
        } else {
            entry->data = kmalloc(ext2_fs.block_size);
        }
    }
    return entry->data;
}

static inline uint32_t ext2_block_cache_set_index(uint32_t block)
{
    /*
     * Fold higher block bits before masking. Plain low-bit indexing is fast,
     * but ext2 metadata and group-aligned data can otherwise collide on
     * power-of-two strides.
     */
    return (block ^ (block >> 8) ^ (block >> 16)) &
           (EXT2_BLOCK_CACHE_SETS - 1u);
}

static inline ext2_block_cache_entry_t* ext2_block_cache_set(uint32_t block)
{
    uint32_t set = ext2_block_cache_set_index(block);

    return &ext2_block_cache[set * EXT2_BLOCK_CACHE_WAYS];
}

static ext2_block_cache_entry_t* ext2_block_cache_find(uint32_t block)
{
    ext2_block_cache_entry_t* set = ext2_block_cache_set(block);

    for (uint32_t way = 0; way < EXT2_BLOCK_CACHE_WAYS; way++) {
        if (set[way].valid && set[way].block == block)
            return &set[way];
    }
    return NULL;
}

static ext2_block_cache_entry_t* ext2_block_cache_pick(uint32_t block)
{
    ext2_block_cache_entry_t* set = ext2_block_cache_set(block);
    uint32_t set_index = ext2_block_cache_set_index(block);
    ext2_block_cache_entry_t* entry;

    for (uint32_t way = 0; way < EXT2_BLOCK_CACHE_WAYS; way++) {
        if (!set[way].valid)
            return &set[way];
    }

    for (uint32_t scanned = 0; scanned < EXT2_BLOCK_CACHE_WAYS; scanned++) {
        entry = &set[ext2_block_cache_set_clock[set_index]++ %
                     EXT2_BLOCK_CACHE_WAYS];
        if (!entry->dirty) {
            entry->valid = false;
            return entry;
        }
    }

    /* Never evict data that has not reached the block device. */
    return NULL;
}

static int ext2_read_block(uint32_t block, void* buf)
{
    uint64_t lba = ext2_fs.lba_start + (uint64_t)block * ext2_fs.sectors_per_block;
    ext2_block_cache_entry_t* entry;
    int ret;

    if (!buf) return -EINVAL;
    if (ext2_fs.block_size == 0)
        return blk_read_sectors(lba, ext2_fs.sectors_per_block, buf);

    ext2_cache_acquire();
    entry = ext2_block_cache_find(block);
    if (entry && entry->data) {
        ext2_stats.cache_hits++;
        memcpy(buf, entry->data, ext2_fs.block_size);
        ext2_cache_release();
        return 0;
    }
    ext2_stats.cache_misses++;
    ext2_cache_release();

    /*
     * Do the potentially sleeping VirtIO I/O outside the cache critical
     * section. Holding ext2_cache while wait_for_used() sleeps can block other
     * tasks behind a cache owner that is no longer runnable.
     */
    ret = blk_read_sectors(lba, ext2_fs.sectors_per_block, buf);
    if (ret < 0)
        return ret;
    ext2_stats.read_blocks++;

    ext2_cache_acquire();
    /*
     * Another task may have inserted this block while the cache lock was
     * dropped for disk I/O. Reuse the existing entry; duplicate entries would
     * leave a stale copy behind after a future write updates only the first
     * matching cache entry.
     */
    entry = ext2_block_cache_find(block);
    if (!entry)
        entry = ext2_block_cache_pick(block);
    if (entry && ext2_block_cache_data(entry)) {
        memcpy(entry->data, buf, ext2_fs.block_size);
        entry->block = block;
        entry->valid = true;
        entry->dirty = false;
    }
    ext2_cache_release();
    return 0;
}

#define EXT2_IO_MAX_BLOCKS 16u

static int ext2_read_contiguous_blocks(uint32_t first_block,
                                       uint32_t block_count,
                                       void* buf)
{
    uint8_t* dst = (uint8_t*)buf;
    bool all_cached = true;
    int ret;

    if (!buf || block_count == 0 || block_count > EXT2_IO_MAX_BLOCKS ||
        ext2_fs.block_size == 0)
        return -EINVAL;

    ext2_cache_acquire();
    for (uint32_t i = 0; i < block_count; i++) {
        ext2_block_cache_entry_t* entry =
            ext2_block_cache_find(first_block + i);

        if (entry && entry->data) {
            ext2_stats.cache_hits++;
            memcpy(dst + i * ext2_fs.block_size, entry->data,
                   ext2_fs.block_size);
        } else {
            ext2_stats.cache_misses++;
            all_cached = false;
        }
    }
    ext2_cache_release();

    if (all_cached)
        return 0;

    ret = blk_read_sectors(ext2_fs.lba_start +
                           (uint64_t)first_block * ext2_fs.sectors_per_block,
                           block_count * ext2_fs.sectors_per_block, buf);
    if (ret < 0)
        return ret;
    ext2_stats.read_blocks += block_count;

    ext2_cache_acquire();
    for (uint32_t i = 0; i < block_count; i++) {
        ext2_block_cache_entry_t* entry =
            ext2_block_cache_find(first_block + i);
        uint8_t* block_buf = dst + i * ext2_fs.block_size;

        /*
         * A dirty cache entry is newer than the disk copy. Preserve it in the
         * caller's buffer instead of exposing stale data from the grouped I/O.
         */
        if (entry && entry->data) {
            if (entry->dirty)
                memcpy(block_buf, entry->data, ext2_fs.block_size);
            continue;
        }

        entry = ext2_block_cache_pick(first_block + i);
        if (entry && ext2_block_cache_data(entry)) {
            memcpy(entry->data, block_buf, ext2_fs.block_size);
            entry->block = first_block + i;
            entry->valid = true;
            entry->dirty = false;
        }
    }
    ext2_cache_release();
    return 0;
}

static int ext2_write_block(uint32_t block, void* buf)
{
    uint64_t lba = ext2_fs.lba_start + (uint64_t)block * ext2_fs.sectors_per_block;
    ext2_block_cache_entry_t* entry;
    int ret;

    if (!buf) return -EINVAL;

    ret = blk_write_sectors(lba, ext2_fs.sectors_per_block, buf);
    if (ret < 0 || ext2_fs.block_size == 0)
        return ret;
    ext2_stats.write_blocks++;
    ext2_mark_dirty();

    ext2_cache_acquire();
    entry = ext2_block_cache_find(block);
    if (!entry)
        entry = ext2_block_cache_pick(block);

    if (entry && ext2_block_cache_data(entry)) {
        if (entry->dirty && ext2_stats.cache_dirty_blocks > 0)
            ext2_stats.cache_dirty_blocks--;
        memcpy(entry->data, buf, ext2_fs.block_size);
        entry->block = block;
        entry->valid = true;
        entry->dirty = false;
        ext2_stats.cache_writes++;
    }

    ext2_cache_release();
    return ret;
}

#define EXT2_WRITEBACK_MAX_BLOCKS 16u

static int ext2_write_data_block(uint32_t block, const void* buf)
{
    ext2_block_cache_entry_t* entry;

    if (!buf || ext2_fs.block_size == 0)
        return -EINVAL;

    ext2_cache_acquire();
    entry = ext2_block_cache_find(block);
    if (!entry)
        entry = ext2_block_cache_pick(block);
    if (!entry || !ext2_block_cache_data(entry)) {
        ext2_cache_release();
        return ext2_write_block(block, (void*)buf);
    }

    if (!entry->dirty)
        ext2_stats.cache_dirty_blocks++;
    memcpy(entry->data, buf, ext2_fs.block_size);
    entry->block = block;
    entry->valid = true;
    entry->dirty = true;
    ext2_stats.cache_writes++;
    ext2_mark_dirty();
    ext2_cache_release();
    return 0;
}

/*
 * Sequential file writes hold ext2_op_lock and may allocate several blocks.
 * Keep their allocation metadata in the same writeback transaction as the
 * file data, then flush all of it before publishing the new inode size.
 */
static int ext2_write_allocation_block(uint32_t block, void* buf)
{
    if (ext2_defer_allocation_writes)
        return ext2_write_data_block(block, buf);
    return ext2_write_block(block, buf);
}

static int ext2_flush_dirty_blocks(void)
{
    uint8_t* batch;
    uint32_t batch_capacity = EXT2_WRITEBACK_MAX_BLOCKS;
    int ret = 0;

    if (ext2_stats.cache_dirty_blocks == 0)
        return 0;

    batch = kmalloc(ext2_fs.block_size * batch_capacity);
    while (!batch && batch_capacity > 1u) {
        batch_capacity >>= 1;
        batch = kmalloc(ext2_fs.block_size * batch_capacity);
    }
    if (!batch)
        return -ENOMEM;

    while (1) {
        uint32_t first = ~0u;
        uint32_t count = 0;

        ext2_cache_acquire();
        for (uint32_t i = 0; i < EXT2_BLOCK_CACHE_SIZE; i++) {
            ext2_block_cache_entry_t* entry = &ext2_block_cache[i];

            if (entry->valid && entry->dirty && entry->block < first)
                first = entry->block;
        }
        if (first == ~0u) {
            ext2_cache_release();
            break;
        }

        while (count < batch_capacity) {
            ext2_block_cache_entry_t* entry =
                ext2_block_cache_find(first + count);

            if (!entry || !entry->dirty || !entry->data)
                break;
            memcpy(batch + count * ext2_fs.block_size,
                   entry->data, ext2_fs.block_size);
            count++;
        }
        ext2_cache_release();

        ret = blk_write_sectors(ext2_fs.lba_start +
                                (uint64_t)first * ext2_fs.sectors_per_block,
                                count * ext2_fs.sectors_per_block, batch);
        if (ret < 0)
            break;

        ext2_stats.write_blocks += count;
        ext2_stats.writeback_batches++;
        ext2_cache_acquire();
        for (uint32_t i = 0; i < count; i++) {
            ext2_block_cache_entry_t* entry =
                ext2_block_cache_find(first + i);

            if (entry && entry->dirty) {
                entry->dirty = false;
                if (ext2_stats.cache_dirty_blocks > 0)
                    ext2_stats.cache_dirty_blocks--;
            }
        }
        ext2_cache_release();
    }

    kfree(batch);
    return ret;
}

/*
 * Pinned superblock copy.
 *
 * Without this cache, every block or inode allocation re-read the superblock
 * from disk (outside the block cache, because it lives at byte offset 1024)
 * and wrote it back synchronously. That is two VirtIO I/O operations per
 * allocation just for free counters. After mount, updates now modify this RAM
 * copy, mark it dirty, and ext2_sync() writes it back before flushing the
 * device. After an unclean shutdown the superblock counters may lag behind the
 * group bitmaps; ext2_check() detects that mismatch but does not repair it yet.
 */
static ext2_superblock_t ext2_sb_cache;
static bool ext2_sb_cached;
static bool ext2_sb_cache_dirty;

static int ext2_read_superblock_disk(ext2_superblock_t* out)
{
    if (!out) return -1;

    uint8_t* buf = kmalloc(1024);
    if (!buf) return -1;

    int ret = blk_read_sectors(ext2_fs.lba_start + 2, 2, buf);
    if (ret >= 0)
        memcpy(out, buf, sizeof(*out));

    kfree(buf);
    return ret;
}

static int ext2_write_superblock_disk(const ext2_superblock_t* in)
{
    if (!in) return -1;

    uint8_t* buf = kmalloc(1024);
    if (!buf) return -1;

    if (blk_read_sectors(ext2_fs.lba_start + 2, 2, buf) < 0) {
        kfree(buf);
        return -1;
    }

    memcpy(buf, in, sizeof(*in));
    int ret = blk_write_sectors(ext2_fs.lba_start + 2, 2, buf);
    if (ret >= 0) {
        ext2_stats.write_blocks++;
        ext2_mark_dirty();
    }
    kfree(buf);
    return ret;
}

static int ext2_read_superblock(ext2_superblock_t* out)
{
    if (!out) return -1;

    if (ext2_sb_cached) {
        *out = ext2_sb_cache;
        return 0;
    }

    return ext2_read_superblock_disk(out);
}

static int ext2_write_superblock(const ext2_superblock_t* in)
{
    if (!in) return -1;

    if (ext2_sb_cached) {
        ext2_sb_cache = *in;
        ext2_sb_cache_dirty = true;
        ext2_mark_dirty();
        return 0;
    }

    return ext2_write_superblock_disk(in);
}

int ext2_sync(void)
{
    int ret = 0;

    if (!ext2_fs.mounted)
        return 0;

    ext2_op_acquire();
    ext2_stats.syncs++;

    if (ext2_flush_dirty_blocks() < 0) {
        ext2_stats.sync_errors++;
        ext2_op_release();
        return -EIO;
    }

    /* Persist allocation counters with the data/allocation transaction. */
    if (ext2_sb_cache_dirty) {
        if (ext2_write_superblock_disk(&ext2_sb_cache) < 0) {
            ext2_stats.sync_errors++;
            ext2_op_release();
            return -EIO;
        }
        ext2_sb_cache_dirty = false;
    }

    /*
     * File-size updates must not reach disk before newly allocated data and
     * allocation metadata. The first flush establishes that ordering; inode
     * table updates are then written as the transaction's commit record.
     */
    if (ext2_pending_inode_count > 0) {
        if (blk_flush() < 0 || ext2_flush_pending_inodes() < 0) {
            ext2_stats.sync_errors++;
            ext2_op_release();
            return -EIO;
        }
    }

    if (ext2_dirty) {
        ret = blk_flush();
        if (ret < 0) {
            ext2_stats.sync_errors++;
            ret = -EIO;
        } else {
            ext2_dirty = false;
            ext2_stats.dirty = 0;
        }
    }
    ext2_op_release();
    return ret;
}

static void ext2_check_append(char* buf, size_t cap, size_t* len,
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

int ext2_check(char* buf, size_t cap, size_t* len)
{
    ext2_superblock_t sb;
    uint32_t free_blocks_sum = 0;
    uint32_t free_inodes_sum = 0;
    uint32_t used_dirs_sum = 0;
    uint32_t errors = 0;
    int ret = 0;

    if (!buf || !len || cap == 0)
        return -EINVAL;

    *len = 0;
    if (!ext2_fs.mounted) {
        ext2_check_append(buf, cap, len, "ext2: not mounted\n");
        return -ENODEV;
    }

    ext2_op_acquire();
    if (ext2_read_superblock(&sb) < 0) {
        ext2_check_append(buf, cap, len, "ext2: superblock read failed\n");
        ext2_stats.check_errors++;
        ext2_op_release();
        return -EIO;
    }

    if (sb.s_magic != EXT2_MAGIC) {
        ext2_check_append(buf, cap, len, "ext2: bad magic 0x%04X\n", sb.s_magic);
        errors++;
    }
    if (ext2_fs.block_size == 0 || ext2_fs.sectors_per_block == 0) {
        ext2_check_append(buf, cap, len, "ext2: invalid block geometry\n");
        errors++;
    }
    if (sb.s_blocks_count != ext2_fs.blocks_count) {
        ext2_check_append(buf, cap, len, "ext2: block count mismatch sb=%u runtime=%u\n",
                          sb.s_blocks_count, ext2_fs.blocks_count);
        errors++;
    }
    if (sb.s_free_blocks_count > sb.s_blocks_count) {
        ext2_check_append(buf, cap, len, "ext2: free blocks exceed total blocks\n");
        errors++;
    }
    if (sb.s_free_inodes_count > sb.s_inodes_count) {
        ext2_check_append(buf, cap, len, "ext2: free inodes exceed total inodes\n");
        errors++;
    }

    for (uint32_t group = 0; group < ext2_fs.groups_count; group++) {
        ext2_group_desc_t gd;
        if (ext2_read_group_desc(group, &gd) < 0) {
            ext2_check_append(buf, cap, len, "ext2: group %u descriptor read failed\n", group);
            errors++;
            continue;
        }
        free_blocks_sum += gd.bg_free_blocks_count;
        free_inodes_sum += gd.bg_free_inodes_count;
        used_dirs_sum += gd.bg_used_dirs_count;
    }

    if (free_blocks_sum != sb.s_free_blocks_count) {
        ext2_check_append(buf, cap, len, "ext2: free block sum mismatch sb=%u groups=%u\n",
                          sb.s_free_blocks_count, free_blocks_sum);
        errors++;
    }
    if (free_inodes_sum != sb.s_free_inodes_count) {
        ext2_check_append(buf, cap, len, "ext2: free inode sum mismatch sb=%u groups=%u\n",
                          sb.s_free_inodes_count, free_inodes_sum);
        errors++;
    }

    ext2_stats.sb_free_blocks = sb.s_free_blocks_count;
    ext2_stats.sb_free_inodes = sb.s_free_inodes_count;
    ext2_stats.gd_free_blocks_sum = free_blocks_sum;
    ext2_stats.gd_free_inodes_sum = free_inodes_sum;
    ext2_stats.gd_used_dirs_sum = used_dirs_sum;
    ext2_stats.check_errors += errors;

    ext2_check_append(buf, cap, len, "ext2: block_size=%u blocks=%u free=%u groups=%u\n",
                      ext2_fs.block_size, sb.s_blocks_count,
                      sb.s_free_blocks_count, ext2_fs.groups_count);
    ext2_check_append(buf, cap, len, "ext2: inodes=%u free=%u used_dirs=%u\n",
                      sb.s_inodes_count, sb.s_free_inodes_count,
                      used_dirs_sum);
    ext2_check_append(buf, cap, len, "ext2: group_sums free_blocks=%u free_inodes=%u\n",
                      free_blocks_sum, free_inodes_sum);
    ext2_check_append(buf, cap, len, "ext2: %s\n",
                      errors == 0 ? "looks consistent" : "inconsistencies found");

    if (errors)
        ret = -EIO;
    ext2_op_release();
    return ret;
}

void ext2_get_stats(ext2_stats_t* out)
{
    ext2_superblock_t sb;
    uint32_t free_blocks_sum = 0;
    uint32_t free_inodes_sum = 0;
    uint32_t used_dirs_sum = 0;

    if (!out)
        return;

    if (ext2_fs.mounted) {
        ext2_op_acquire();
        if (ext2_read_superblock(&sb) == 0) {
            ext2_stats.sb_free_blocks = sb.s_free_blocks_count;
            ext2_stats.sb_free_inodes = sb.s_free_inodes_count;
        }
        for (uint32_t group = 0; group < ext2_fs.groups_count; group++) {
            ext2_group_desc_t gd;
            if (ext2_read_group_desc(group, &gd) == 0) {
                free_blocks_sum += gd.bg_free_blocks_count;
                free_inodes_sum += gd.bg_free_inodes_count;
                used_dirs_sum += gd.bg_used_dirs_count;
            }
        }
        ext2_stats.gd_free_blocks_sum = free_blocks_sum;
        ext2_stats.gd_free_inodes_sum = free_inodes_sum;
        ext2_stats.gd_used_dirs_sum = used_dirs_sum;
        ext2_op_release();
    }

    *out = ext2_stats;
    out->mounted = ext2_fs.mounted ? 1 : 0;
    out->dirty = ext2_dirty ? 1 : 0;
    out->block_size = ext2_fs.block_size;
    out->blocks_count = ext2_fs.blocks_count;
    out->groups_count = ext2_fs.groups_count;
    out->inodes_per_group = ext2_fs.inodes_per_group;
    out->blocks_per_group = ext2_fs.blocks_per_group;
}

int ext2_statfs(struct statfs* st)
{
    ext2_superblock_t sb;
    int ret = 0;

    if (!st)
        return -EINVAL;
    if (!ext2_fs.mounted)
        return -ENODEV;

    ext2_op_acquire();
    if (ext2_read_superblock(&sb) < 0) {
        ret = -EIO;
        goto out;
    }

    memset(st, 0, sizeof(*st));
    st->f_type = EXT2_MAGIC;
    st->f_bsize = ext2_fs.block_size;
    st->f_blocks = sb.s_blocks_count;
    st->f_bfree = sb.s_free_blocks_count;
    st->f_bavail = sb.s_free_blocks_count;
    st->f_files = sb.s_inodes_count;
    st->f_ffree = sb.s_free_inodes_count;
    st->f_namelen = MAX_NAME;
    st->f_frsize = ext2_fs.block_size;

out:
    ext2_op_release();
    return ret;
}

static int ext2_read_group_desc(uint32_t group, ext2_group_desc_t* out)
{
    if (!out || group >= ext2_fs.groups_count) return -1;

    uint32_t descs_per_block = ext2_fs.block_size / sizeof(ext2_group_desc_t);
    uint32_t gdesc_blk = ext2_fs.gdesc_block + group / descs_per_block;
    uint32_t gdesc_idx = group % descs_per_block;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -1;

    if (ext2_read_block(gdesc_blk, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    memcpy(out, (ext2_group_desc_t*)blkbuf + gdesc_idx, sizeof(*out));
    kfree(blkbuf);
    return 0;
}

static int ext2_write_group_desc(uint32_t group, const ext2_group_desc_t* in)
{
    if (!in || group >= ext2_fs.groups_count) return -1;

    uint32_t descs_per_block = ext2_fs.block_size / sizeof(ext2_group_desc_t);
    uint32_t gdesc_blk = ext2_fs.gdesc_block + group / descs_per_block;
    uint32_t gdesc_idx = group % descs_per_block;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -1;

    if (ext2_read_block(gdesc_blk, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    memcpy((ext2_group_desc_t*)blkbuf + gdesc_idx, in, sizeof(*in));

    if (ext2_write_allocation_block(gdesc_blk, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    kfree(blkbuf);
    return 0;
}

static bool ext2_bitmap_test(const uint8_t* bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] & (uint8_t)(1u << (bit % 8))) != 0;
}

static void ext2_bitmap_set(uint8_t* bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void ext2_bitmap_clear(uint8_t* bitmap, uint32_t bit)
{
    bitmap[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

static uint32_t ext2_min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint16_t ext2_dir_rec_len(uint8_t name_len)
{
    return (uint16_t)((sizeof(ext2_dir_entry_t) + name_len + 3) & ~3u);
}

static uint8_t ext2_file_type_from_mode(mode_t mode)
{
    if (S_ISDIR(mode)) return EXT2_FT_DIR;
    if (S_ISLNK(mode)) return EXT2_FT_SYMLINK;
    if (S_ISCHR(mode)) return EXT2_FT_CHRDEV;
    if (S_ISBLK(mode)) return EXT2_FT_BLKDEV;
    if (S_ISFIFO(mode)) return EXT2_FT_FIFO;
    return EXT2_FT_REG_FILE;
}

static uint32_t ext2_ptrs_per_block(void)
{
    return ext2_fs.block_size / sizeof(uint32_t);
}

static uint32_t ext2_max_supported_file_blocks(void)
{
    uint32_t ptrs = ext2_ptrs_per_block();
    return 12 + ptrs + ptrs * ptrs;
}

static uint32_t ext2_truncate_block_capacity(const ext2_inode_t* inode)
{
    uint32_t sectors = ext2_fs.sectors_per_block;
    uint32_t capacity;
    uint32_t maximum;

    if (!inode || sectors == 0)
        return 1;

    /* i_blocks includes data and indirect blocks, in 512-byte sectors. */
    capacity = inode->i_blocks / sectors;
    if (inode->i_blocks % sectors)
        capacity++;

    maximum = ext2_max_supported_file_blocks() + 2 + ext2_ptrs_per_block();
    if (capacity > maximum)
        capacity = maximum;
    return capacity != 0 ? capacity : 1;
}

static int ext2_alloc_block(uint32_t* out_block)
{
    ext2_superblock_t sb;
    uint32_t start_group;

    if (!out_block) return -1;
    if (ext2_read_superblock(&sb) < 0) return -EIO;
    if (sb.s_free_blocks_count == 0) return -ENOSPC;

    start_group = ext2_alloc_group_hint < ext2_fs.groups_count ?
                  ext2_alloc_group_hint : 0;

    for (uint32_t group_offset = 0;
         group_offset < ext2_fs.groups_count;
         group_offset++) {
        uint32_t group = (start_group + group_offset) %
                         ext2_fs.groups_count;
        ext2_group_desc_t gd;

        if (ext2_read_group_desc(group, &gd) < 0)
            return -EIO;
        if (gd.bg_free_blocks_count == 0)
            continue;

        uint32_t group_first = ext2_fs.first_data_block + group * ext2_fs.blocks_per_group;
        if (group_first >= ext2_fs.blocks_count)
            continue;

        uint32_t group_blocks = ext2_min_u32(ext2_fs.blocks_per_group,
                                             ext2_fs.blocks_count - group_first);

        uint8_t* bitmap = kmalloc(ext2_fs.block_size);
        if (!bitmap) return -ENOMEM;

        if (ext2_read_block(gd.bg_block_bitmap, bitmap) < 0) {
            kfree(bitmap);
            return -EIO;
        }

        uint32_t first_bit = group == start_group ?
                             ext2_alloc_bit_hint : 0;
        if (first_bit >= group_blocks)
            first_bit = 0;

        for (uint32_t pass = 0; pass < 2; pass++) {
            uint32_t begin = pass == 0 ? first_bit : 0;
            uint32_t end = pass == 0 ? group_blocks : first_bit;

            if (begin >= end)
                continue;

            for (uint32_t bit = begin; bit < end; bit++) {
                uint32_t block = group_first + bit;
                if (block == 0 || block >= ext2_fs.blocks_count)
                    continue;
                if (ext2_bitmap_test(bitmap, bit))
                    continue;

                uint8_t* zero = kmalloc(ext2_fs.block_size);
                if (!zero) {
                    kfree(bitmap);
                    return -ENOMEM;
                }
                memset(zero, 0, ext2_fs.block_size);
                if (ext2_write_allocation_block(block, zero) < 0) {
                    kfree(zero);
                    kfree(bitmap);
                    return -EIO;
                }
                kfree(zero);

                ext2_bitmap_set(bitmap, bit);
                if (ext2_write_allocation_block(gd.bg_block_bitmap,
                                                bitmap) < 0) {
                    kfree(bitmap);
                    return -EIO;
                }

                if (gd.bg_free_blocks_count > 0)
                    gd.bg_free_blocks_count--;
                if (ext2_write_group_desc(group, &gd) < 0) {
                    kfree(bitmap);
                    return -EIO;
                }

                if (sb.s_free_blocks_count > 0)
                    sb.s_free_blocks_count--;
                sb.s_wtime = get_current_time();
                if (ext2_write_superblock(&sb) < 0) {
                    kfree(bitmap);
                    return -EIO;
                }

                ext2_alloc_group_hint = group;
                ext2_alloc_bit_hint = bit + 1;
                if (ext2_alloc_bit_hint >= group_blocks) {
                    ext2_alloc_group_hint =
                        (group + 1) % ext2_fs.groups_count;
                    ext2_alloc_bit_hint = 0;
                }

                kfree(bitmap);
                *out_block = block;
                return 0;
            }
        }

        kfree(bitmap);
    }

    return -ENOSPC;
}

static int ext2_free_block_list(const uint32_t* blocks, uint32_t count)
{
    ext2_superblock_t sb;

    if (!blocks) return -EINVAL;

    if (ext2_read_superblock(&sb) < 0)
        return -EIO;

    for (uint32_t i = 0; i < count; i++) {
        if (blocks[i] == 0)
            continue;
        if (blocks[i] < ext2_fs.first_data_block || blocks[i] >= ext2_fs.blocks_count)
            return -EINVAL;
    }

    uint32_t total_freed = 0;

    for (uint32_t group = 0; group < ext2_fs.groups_count; group++) {
        ext2_group_desc_t gd;
        uint32_t group_first = ext2_fs.first_data_block + group * ext2_fs.blocks_per_group;
        if (group_first >= ext2_fs.blocks_count)
            break;

        uint32_t group_blocks = ext2_min_u32(ext2_fs.blocks_per_group,
                                             ext2_fs.blocks_count - group_first);
        uint32_t group_end = group_first + group_blocks;
        uint32_t freed = 0;

        uint8_t* bitmap = NULL;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t block = blocks[i];
            if (block < group_first || block >= group_end)
                continue;

            if (!bitmap) {
                if (ext2_read_group_desc(group, &gd) < 0)
                    return -EIO;

                bitmap = kmalloc(ext2_fs.block_size);
                if (!bitmap) return -ENOMEM;

                if (ext2_read_block(gd.bg_block_bitmap, bitmap) < 0) {
                    kfree(bitmap);
                    return -EIO;
                }
            }

            uint32_t bit = block - group_first;
            if (ext2_bitmap_test(bitmap, bit)) {
                ext2_bitmap_clear(bitmap, bit);
                freed++;
            }
        }

        if (!bitmap)
            continue;

        if (freed > 0) {
            if (ext2_write_block(gd.bg_block_bitmap, bitmap) < 0) {
                kfree(bitmap);
                return -EIO;
            }

            gd.bg_free_blocks_count += freed;
            if (ext2_write_group_desc(group, &gd) < 0) {
                kfree(bitmap);
                return -EIO;
            }

            total_freed += freed;
        }

        kfree(bitmap);
    }

    if (total_freed > 0) {
        sb.s_free_blocks_count += total_freed;
        sb.s_wtime = get_current_time();
        if (ext2_write_superblock(&sb) < 0)
            return -EIO;
    }

    return 0;
}

static int ext2_alloc_inode(uint32_t* out_ino)
{
    ext2_superblock_t sb;

    if (!out_ino) return -EINVAL;
    if (ext2_read_superblock(&sb) < 0) return -EIO;
    if (sb.s_free_inodes_count == 0) return -ENOSPC;

    for (uint32_t group = 0; group < ext2_fs.groups_count; group++) {
        ext2_group_desc_t gd;

        if (ext2_read_group_desc(group, &gd) < 0)
            return -EIO;
        if (gd.bg_free_inodes_count == 0)
            continue;

        uint32_t group_first_ino = group * ext2_fs.inodes_per_group + 1;
        if (group_first_ino > sb.s_inodes_count)
            continue;

        uint32_t group_inodes = ext2_min_u32(ext2_fs.inodes_per_group,
                                             sb.s_inodes_count - group_first_ino + 1);

        uint8_t* bitmap = kmalloc(ext2_fs.block_size);
        if (!bitmap) return -ENOMEM;

        if (ext2_read_block(gd.bg_inode_bitmap, bitmap) < 0) {
            kfree(bitmap);
            return -EIO;
        }

        for (uint32_t bit = 0; bit < group_inodes; bit++) {
            if (ext2_bitmap_test(bitmap, bit))
                continue;

            uint32_t ino = group_first_ino + bit;
            if (ino == 0 || ino > sb.s_inodes_count)
                continue;

            ext2_bitmap_set(bitmap, bit);
            if (ext2_write_block(gd.bg_inode_bitmap, bitmap) < 0) {
                kfree(bitmap);
                return -EIO;
            }

            if (gd.bg_free_inodes_count > 0)
                gd.bg_free_inodes_count--;
            if (ext2_write_group_desc(group, &gd) < 0) {
                kfree(bitmap);
                return -EIO;
            }

            if (sb.s_free_inodes_count > 0)
                sb.s_free_inodes_count--;
            sb.s_wtime = get_current_time();
            if (ext2_write_superblock(&sb) < 0) {
                kfree(bitmap);
                return -EIO;
            }

            kfree(bitmap);
            *out_ino = ino;
            return 0;
        }

        kfree(bitmap);
    }

    return -ENOSPC;
}

static int ext2_free_inode(uint32_t ino)
{
    ext2_superblock_t sb;

    if (ino == 0) return -EINVAL;
    if (ext2_read_superblock(&sb) < 0) return -EIO;
    if (ino > sb.s_inodes_count) return -EINVAL;

    uint32_t group = (ino - 1) / ext2_fs.inodes_per_group;
    uint32_t bit = (ino - 1) % ext2_fs.inodes_per_group;
    if (group >= ext2_fs.groups_count)
        return -EINVAL;

    ext2_group_desc_t gd;
    if (ext2_read_group_desc(group, &gd) < 0)
        return -EIO;

    uint8_t* bitmap = kmalloc(ext2_fs.block_size);
    if (!bitmap) return -ENOMEM;

    if (ext2_read_block(gd.bg_inode_bitmap, bitmap) < 0) {
        kfree(bitmap);
        return -EIO;
    }

    if (!ext2_bitmap_test(bitmap, bit)) {
        kfree(bitmap);
        return 0;
    }

    ext2_inode_t zero;
    memset(&zero, 0, sizeof(zero));
    if (ext2_write_disk_inode(ino, &zero) < 0) {
        kfree(bitmap);
        return -EIO;
    }

    ext2_bitmap_clear(bitmap, bit);
    if (ext2_write_block(gd.bg_inode_bitmap, bitmap) < 0) {
        kfree(bitmap);
        return -EIO;
    }
    kfree(bitmap);

    gd.bg_free_inodes_count++;
    if (ext2_write_group_desc(group, &gd) < 0)
        return -EIO;

    sb.s_free_inodes_count++;
    sb.s_wtime = get_current_time();
    if (ext2_write_superblock(&sb) < 0)
        return -EIO;

    return 0;
}

static int ext2_adjust_used_dirs(uint32_t ino, int delta)
{
    ext2_superblock_t sb;
    if (ext2_read_superblock(&sb) < 0) return -EIO;
    if (ino == 0 || ino > sb.s_inodes_count) return -EINVAL;

    uint32_t group = (ino - 1) / ext2_fs.inodes_per_group;
    if (group >= ext2_fs.groups_count) return -EINVAL;

    ext2_group_desc_t gd;
    if (ext2_read_group_desc(group, &gd) < 0) return -EIO;

    if (delta > 0) {
        gd.bg_used_dirs_count++;
    } else if (delta < 0 && gd.bg_used_dirs_count > 0) {
        gd.bg_used_dirs_count--;
    }

    if (ext2_write_group_desc(group, &gd) < 0)
        return -EIO;

    return 0;
}

/* ---------- inode table ---------- */

static ext2_pending_inode_t* ext2_find_pending_inode(uint32_t ino)
{
    for (uint32_t i = 0; i < EXT2_PENDING_INODE_COUNT; i++) {
        if (ext2_pending_inodes[i].valid &&
            ext2_pending_inodes[i].ino == ino)
            return &ext2_pending_inodes[i];
    }

    return NULL;
}

static int ext2_queue_pending_inode(uint32_t ino, const ext2_inode_t* inode)
{
    ext2_pending_inode_t* entry;

    if (ino == 0 || !inode)
        return -EINVAL;

    entry = ext2_find_pending_inode(ino);
    if (!entry) {
        for (uint32_t i = 0; i < EXT2_PENDING_INODE_COUNT; i++) {
            if (!ext2_pending_inodes[i].valid) {
                entry = &ext2_pending_inodes[i];
                entry->valid = true;
                entry->ino = ino;
                ext2_pending_inode_count++;
                break;
            }
        }
    }
    if (!entry)
        return -ENOSPC;

    entry->inode = *inode;
    ext2_mark_dirty();
    return 0;
}

static int ext2_inode_location(uint32_t ino, uint32_t* block, uint32_t* offset)
{
    if (ino == 0) return -1;

    uint32_t group  = (ino - 1) / ext2_fs.inodes_per_group;
    uint32_t local  = (ino - 1) % ext2_fs.inodes_per_group;

    /* Which block inside the group descriptor table holds this group's entry? */
    uint32_t descs_per_block = ext2_fs.block_size / sizeof(ext2_group_desc_t);
    uint32_t gdesc_blk       = ext2_fs.gdesc_block + group / descs_per_block;
    uint32_t gdesc_idx       = group % descs_per_block;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -1;

    if (ext2_read_block(gdesc_blk, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    ext2_group_desc_t* gd  = (ext2_group_desc_t*)blkbuf + gdesc_idx;
    uint32_t itable_block  = gd->bg_inode_table;
    kfree(blkbuf);

    uint32_t inodes_per_block = ext2_fs.block_size / ext2_fs.inode_size;
    *block  = itable_block + local / inodes_per_block;
    *offset = (local % inodes_per_block) * ext2_fs.inode_size;
    return 0;
}

static int ext2_read_disk_inode(uint32_t ino, ext2_inode_t* out)
{
    ext2_pending_inode_t* pending;
    uint32_t inode_block;
    uint32_t inode_off;

    if (!out) return -1;
    pending = ext2_find_pending_inode(ino);
    if (pending) {
        *out = pending->inode;
        return 0;
    }
    if (ext2_inode_location(ino, &inode_block, &inode_off) < 0) return -1;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -1;

    if (ext2_read_block(inode_block, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    memcpy(out, blkbuf + inode_off, sizeof(ext2_inode_t));
    kfree(blkbuf);
    return 0;
}

static int ext2_write_disk_inode(uint32_t ino, const ext2_inode_t* in)
{
    ext2_pending_inode_t* pending;
    uint32_t inode_block;
    uint32_t inode_off;

    if (!in) return -1;
    if (ext2_inode_location(ino, &inode_block, &inode_off) < 0) return -1;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -1;

    if (ext2_read_block(inode_block, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    memcpy(blkbuf + inode_off, in, sizeof(ext2_inode_t));

    if (ext2_write_block(inode_block, blkbuf) < 0) {
        kfree(blkbuf);
        return -1;
    }

    pending = ext2_find_pending_inode(ino);
    if (pending) {
        pending->valid = false;
        if (ext2_pending_inode_count > 0)
            ext2_pending_inode_count--;
    }

    kfree(blkbuf);
    return 0;
}

static int ext2_flush_pending_inodes(void)
{
    for (uint32_t i = 0; i < EXT2_PENDING_INODE_COUNT; i++) {
        if (!ext2_pending_inodes[i].valid)
            continue;
        if (ext2_write_disk_inode(ext2_pending_inodes[i].ino,
                                  &ext2_pending_inodes[i].inode) < 0)
            return -EIO;
    }

    return 0;
}

int ext2_update_inode_metadata(inode_t* inode)
{
    ext2_inode_t disk;
    int ret = 0;

    if (!inode) return -EINVAL;
    if (!ext2_fs.mounted) return -ENODEV;

    ext2_op_acquire();
    if (ext2_read_disk_inode(inode->first_cluster, &disk) < 0) {
        ret = -EIO;
        goto out;
    }

    disk.i_mode = inode->mode;
    disk.i_uid = inode->uid;
    disk.i_gid = inode->gid;
    disk.i_atime = inode->atime;
    disk.i_mtime = inode->mtime;
    disk.i_ctime = inode->ctime ? inode->ctime : get_current_time();

    if (ext2_write_disk_inode(inode->first_cluster, &disk) < 0) {
        ret = -EIO;
        goto out;
    }

    inode->ctime = disk.i_ctime;

out:
    ext2_op_release();
    return ret;
}

/*
 * Logical block mapping cursor.
 *
 * read()/readdir() walk consecutive logical block indexes that often share the
 * same single- or double-indirect table block. The cursor keeps the last table
 * block loaded so the hot loop does not re-read and copy a full metadata block
 * for every data block. Buffers are allocated lazily and must be released with
 * ext2_map_cursor_release().
 */
typedef struct ext2_map_cursor {
    uint32_t table_no;   /* Leaf table block currently loaded in table[]; 0 means none. */
    uint32_t dbl_no;     /* Double-indirect table block loaded in dbl[]; 0 means none. */
    uint32_t* table;
    uint32_t* dbl;
} ext2_map_cursor_t;

static void ext2_map_cursor_init(ext2_map_cursor_t* cur)
{
    cur->table_no = 0;
    cur->dbl_no = 0;
    cur->table = NULL;
    cur->dbl = NULL;
}

static void ext2_map_cursor_release(ext2_map_cursor_t* cur)
{
    if (cur->table) kfree(cur->table);
    if (cur->dbl) kfree(cur->dbl);
    ext2_map_cursor_init(cur);
}

static int ext2_map_cursor_load(uint32_t block_no, uint32_t* loaded_no,
                                uint32_t** buf)
{
    if (*loaded_no == block_no && *buf)
        return 0;

    if (!*buf) {
        *buf = kmalloc(ext2_fs.block_size);
        if (!*buf)
            return -ENOMEM;
    }

    if (ext2_read_block(block_no, *buf) < 0) {
        *loaded_no = 0;
        return -EIO;
    }

    *loaded_no = block_no;
    return 0;
}

/*
 * Resolve a logical block index into an on-disk block number.
 *
 * Returns 0 with *out_blk == 0 for a sparse hole; that is not an error.
 * Returns < 0 when resolution itself fails (memory pressure or I/O). Callers
 * must not treat that case as a hole, otherwise read paths could silently serve
 * zeroes instead of reporting that real data could not be reached.
 */
static int ext2_map_block_cursor(ext2_inode_t* di, uint32_t idx,
                                 uint32_t* out_blk, ext2_map_cursor_t* cur)
{
    uint32_t ptrs_per_block = ext2_ptrs_per_block();
    int ret;

    if (!di || !out_blk || !cur)
        return -EINVAL;

    *out_blk = 0;

    if (idx < 12) {
        *out_blk = di->i_block[idx];
        return 0;
    }

    idx -= 12;
    if (idx < ptrs_per_block) {
        if (di->i_block[12] == 0)
            return 0;

        ret = ext2_map_cursor_load(di->i_block[12], &cur->table_no, &cur->table);
        if (ret < 0)
            return ret;

        *out_blk = cur->table[idx];
        return 0;
    }

    idx -= ptrs_per_block;
    if (idx >= ptrs_per_block * ptrs_per_block)
        return 0;
    if (di->i_block[13] == 0)
        return 0;

    ret = ext2_map_cursor_load(di->i_block[13], &cur->dbl_no, &cur->dbl);
    if (ret < 0)
        return ret;

    uint32_t first = idx / ptrs_per_block;
    uint32_t second = idx % ptrs_per_block;
    if (cur->dbl[first] == 0)
        return 0;

    ret = ext2_map_cursor_load(cur->dbl[first], &cur->table_no, &cur->table);
    if (ret < 0)
        return ret;

    *out_blk = cur->table[second];
    return 0;
}

/* Cursor-less variant for isolated calls outside hot sequential loops. */
static int ext2_map_block(ext2_inode_t* di, uint32_t idx, uint32_t* out_blk)
{
    ext2_map_cursor_t cur;
    int ret;

    ext2_map_cursor_init(&cur);
    ret = ext2_map_block_cursor(di, idx, out_blk, &cur);
    ext2_map_cursor_release(&cur);
    return ret;
}

static uint32_t ext2_get_or_alloc_block_at(ext2_inode_t* di, uint32_t idx, bool* allocated)
{
    uint32_t block = 0;
    uint32_t ptrs_per_block = ext2_ptrs_per_block();

    /*
     * A mapping failure must not be confused with a sparse hole: allocating a
     * replacement over an existing referenced block would orphan data.
     */
    if (ext2_map_block(di, idx, &block) < 0)
        return 0;

    if (block != 0)
        return block;

    if (allocated)
        *allocated = false;

    if (idx < 12) {
        if (ext2_alloc_block(&block) < 0)
            return 0;

        di->i_block[idx] = block;
        di->i_blocks += ext2_fs.sectors_per_block;
        if (allocated)
            *allocated = true;
        return block;
    }

    idx -= 12;
    if (idx < ptrs_per_block) {
        bool new_indirect = false;

        if (di->i_block[12] == 0) {
            uint32_t indirect_block;
            if (ext2_alloc_block(&indirect_block) < 0)
                return 0;

            di->i_block[12] = indirect_block;
            di->i_blocks += ext2_fs.sectors_per_block;
            new_indirect = true;
            if (allocated)
                *allocated = true;
        }

        uint32_t* indirect = kmalloc(ext2_fs.block_size);
        if (!indirect) return 0;

        if (new_indirect) {
            memset(indirect, 0, ext2_fs.block_size);
        } else if (ext2_read_block(di->i_block[12], indirect) < 0) {
            kfree(indirect);
            return 0;
        }

        if (indirect[idx] == 0) {
            uint32_t data_block;
            if (ext2_alloc_block(&data_block) < 0) {
                if (new_indirect)
                    ext2_write_allocation_block(di->i_block[12], indirect);
                kfree(indirect);
                return 0;
            }

            indirect[idx] = data_block;

            if (ext2_write_allocation_block(di->i_block[12], indirect) < 0) {
                kfree(indirect);
                return 0;
            }

            di->i_blocks += ext2_fs.sectors_per_block;
            if (allocated)
                *allocated = true;
        }

        block = indirect[idx];
        kfree(indirect);
        return block;
    }

    idx -= ptrs_per_block;
    if (idx >= ptrs_per_block * ptrs_per_block)
        return 0; /* triple-indirect blocks are not supported */

    uint32_t first = idx / ptrs_per_block;
    uint32_t second = idx % ptrs_per_block;
    bool new_double = false;
    bool new_indirect = false;

    if (di->i_block[13] == 0) {
        uint32_t double_block;
        if (ext2_alloc_block(&double_block) < 0)
            return 0;

        di->i_block[13] = double_block;
        di->i_blocks += ext2_fs.sectors_per_block;
        new_double = true;
        if (allocated)
            *allocated = true;
    }

    uint32_t* dbl = kmalloc(ext2_fs.block_size);
    uint32_t* indirect = kmalloc(ext2_fs.block_size);
    if (!dbl || !indirect) {
        if (dbl) kfree(dbl);
        if (indirect) kfree(indirect);
        return 0;
    }

    if (new_double) {
        memset(dbl, 0, ext2_fs.block_size);
    } else if (ext2_read_block(di->i_block[13], dbl) < 0) {
        kfree(dbl);
        kfree(indirect);
        return 0;
    }

    if (dbl[first] == 0) {
        uint32_t indirect_block;
        if (ext2_alloc_block(&indirect_block) < 0) {
            if (new_double)
                ext2_write_allocation_block(di->i_block[13], dbl);
            kfree(dbl);
            kfree(indirect);
            return 0;
        }

        dbl[first] = indirect_block;
        di->i_blocks += ext2_fs.sectors_per_block;
        new_indirect = true;
        if (allocated)
            *allocated = true;
    }

    if (ext2_write_allocation_block(di->i_block[13], dbl) < 0) {
        kfree(dbl);
        kfree(indirect);
        return 0;
    }

    if (new_indirect) {
        memset(indirect, 0, ext2_fs.block_size);
    } else if (ext2_read_block(dbl[first], indirect) < 0) {
        kfree(dbl);
        kfree(indirect);
        return 0;
    }

    if (indirect[second] == 0) {
        uint32_t data_block;
        if (ext2_alloc_block(&data_block) < 0) {
            if (new_indirect)
                ext2_write_allocation_block(dbl[first], indirect);
            kfree(dbl);
            kfree(indirect);
            return 0;
        }

        indirect[second] = data_block;

        if (ext2_write_allocation_block(dbl[first], indirect) < 0) {
            kfree(dbl);
            kfree(indirect);
            return 0;
        }

        di->i_blocks += ext2_fs.sectors_per_block;
        if (allocated)
            *allocated = true;
    }

    block = indirect[second];
    kfree(dbl);
    kfree(indirect);
    return block;
}

static void ext2_init_dirent(ext2_dir_entry_t* de, uint32_t ino, uint16_t rec_len,
                             const char* name, uint8_t name_len, uint8_t file_type)
{
    de->inode = ino;
    de->rec_len = rec_len;
    de->name_len = name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);
}

static int ext2_add_dir_entry(inode_t* dir, uint32_t ino, const char* name, uint8_t file_type)
{
    if (!dir || !name || !S_ISDIR(dir->mode)) return -EINVAL;

    uint32_t name_len32 = strlen(name);
    if (name_len32 == 0 || name_len32 > 255) return -EINVAL;

    uint8_t name_len = (uint8_t)name_len32;
    uint16_t needed = ext2_dir_rec_len(name_len);

    ext2_inode_t di;
    if (ext2_read_disk_inode(dir->first_cluster, &di) < 0)
        return -EIO;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    uint32_t max_blocks = ext2_max_supported_file_blocks();
    uint32_t used_blocks = (di.i_size + ext2_fs.block_size - 1) / ext2_fs.block_size;

    for (uint32_t idx = 0; idx < max_blocks; idx++) {
        uint32_t blk = 0;

        if (ext2_map_block(&di, idx, &blk) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        if (blk == 0 || idx >= used_blocks) {
            blk = ext2_get_or_alloc_block_at(&di, idx, NULL);
            if (blk == 0) {
                kfree(blkbuf);
                return -ENOSPC;
            }

            memset(blkbuf, 0, ext2_fs.block_size);
            ext2_init_dirent((ext2_dir_entry_t*)blkbuf, ino, ext2_fs.block_size,
                             name, name_len, file_type);

            if (di.i_size < (idx + 1) * ext2_fs.block_size)
                di.i_size = (idx + 1) * ext2_fs.block_size;

            di.i_mtime = get_current_time();
            di.i_ctime = di.i_mtime;

            if (ext2_write_block(blk, blkbuf) < 0 ||
                ext2_write_disk_inode(dir->first_cluster, &di) < 0) {
                kfree(blkbuf);
                return -EIO;
            }

            dir->size = di.i_size;
            dir->blocks = di.i_blocks;
            dir->mtime = di.i_mtime;
            dir->ctime = di.i_ctime;
            kfree(blkbuf);
            return 0;
        }

        if (ext2_read_block(blk, blkbuf) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        uint32_t offset = 0;
        while (offset + sizeof(ext2_dir_entry_t) <= ext2_fs.block_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blkbuf + offset);
            if (de->rec_len == 0)
                break;

            if (de->inode == 0 && de->rec_len >= needed) {
                uint16_t old_len = de->rec_len;
                ext2_init_dirent(de, ino, old_len, name, name_len, file_type);

                di.i_mtime = get_current_time();
                di.i_ctime = di.i_mtime;
                if (ext2_write_block(blk, blkbuf) < 0 ||
                    ext2_write_disk_inode(dir->first_cluster, &di) < 0) {
                    kfree(blkbuf);
                    return -EIO;
                }

                dir->mtime = di.i_mtime;
                dir->ctime = di.i_ctime;
                kfree(blkbuf);
                return 0;
            }

            uint16_t actual_len = ext2_dir_rec_len(de->name_len);
            if (de->inode != 0 && de->rec_len >= actual_len + needed) {
                uint16_t old_len = de->rec_len;
                de->rec_len = actual_len;

                ext2_dir_entry_t* new_de = (ext2_dir_entry_t*)(blkbuf + offset + actual_len);
                ext2_init_dirent(new_de, ino, old_len - actual_len,
                                 name, name_len, file_type);

                di.i_mtime = get_current_time();
                di.i_ctime = di.i_mtime;
                if (ext2_write_block(blk, blkbuf) < 0 ||
                    ext2_write_disk_inode(dir->first_cluster, &di) < 0) {
                    kfree(blkbuf);
                    return -EIO;
                }

                dir->mtime = di.i_mtime;
                dir->ctime = di.i_ctime;
                kfree(blkbuf);
                return 0;
            }

            offset += de->rec_len;
        }
    }

    kfree(blkbuf);
    return -ENOSPC;
}

/* ---------- create VFS inode from ext2 inode number ---------- */

static inode_t* ext2_make_inode(uint32_t ino)
{
    inode_t* inode = vfs_get_backing_inode(&ext2_inode_ops, ino);
    ext2_inode_t disk;

    if (inode)
        return inode;
    if (ext2_read_disk_inode(ino, &disk) < 0) return NULL;

    inode = create_inode();
    if (!inode) return NULL;

    inode->first_cluster = ino;   /* reuse field for ext2 inode number */
    inode->size   = disk.i_size;
    inode->mode   = disk.i_mode;
    inode->uid    = disk.i_uid;
    inode->gid    = disk.i_gid;
    inode->atime  = disk.i_atime;
    inode->mtime  = disk.i_mtime;
    inode->ctime  = disk.i_ctime;
    inode->blocks = disk.i_blocks;
    inode->nlink  = disk.i_links_count;

    if (S_ISDIR(disk.i_mode)) {
        inode->i_op = &ext2_inode_ops;
        inode->f_op = &ext2_dir_ops;
    } else if (S_ISREG(disk.i_mode)) {
        inode->i_op = &ext2_inode_ops;
        inode->f_op = &ext2_file_ops;
    } else {
        inode->i_op = &ext2_inode_ops;
        inode->f_op = NULL;
    }
    return inode;
}

static inode_t* ext2_create_file_unlocked(inode_t* parent, const char* name, mode_t mode)
{
    mode_t type;
    uint8_t dir_type;

    if (!parent || !name) return NULL;
    if (!S_ISDIR(parent->mode)) return NULL;

    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return NULL;

    inode_t* existing = ext2_inode_lookup(parent, name);
    if (existing) {
        put_inode(existing);
        return NULL;
    }

    uint32_t ino;
    if (ext2_alloc_inode(&ino) < 0)
        return NULL;

    type = mode & S_IFMT;
    if (type == 0)
        type = S_IFREG;
    if (type != S_IFREG && type != S_IFIFO &&
        type != S_IFCHR && type != S_IFBLK) {
        ext2_free_inode(ino);
        return NULL;
    }
    dir_type = ext2_file_type_from_mode(type);

    uint32_t now = get_current_time();
    ext2_inode_t di;
    memset(&di, 0, sizeof(di));
    di.i_mode = type | (mode & 07777);
    di.i_uid = current_uid();
    di.i_gid = current_gid();
    di.i_size = 0;
    di.i_atime = now;
    di.i_ctime = now;
    di.i_mtime = now;
    di.i_links_count = 1;
    di.i_blocks = 0;

    if (ext2_write_disk_inode(ino, &di) < 0) {
        ext2_free_inode(ino);
        return NULL;
    }

    if (ext2_add_dir_entry(parent, ino, name, dir_type) < 0) {
        ext2_free_inode(ino);
        return NULL;
    }

    return ext2_make_inode(ino);
}

static int ext2_inode_create_op(inode_t* dir, const char* name, uint16_t mode)
{
    inode_t* inode;

    ext2_op_acquire();
    inode = ext2_create_file_unlocked(dir, name, mode);
    ext2_op_release();

    if (!inode)
        return -EIO;
    put_inode(inode);
    return 0;
}

static int ext2_link_inode_unlocked(inode_t* parent, const char* name, inode_t* target)
{
    int ret;

    if (!parent || !name || !target) return -EINVAL;
    if (!S_ISDIR(parent->mode)) return -ENOTDIR;
    if (S_ISDIR(target->mode)) return -EPERM;
    if (parent->i_op != &ext2_inode_ops || target->i_op != &ext2_inode_ops)
        return -EXDEV;

    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -EINVAL;

    inode_t* existing = ext2_inode_lookup(parent, name);
    if (existing) {
        put_inode(existing);
        return -EEXIST;
    }

    if (target->nlink >= 0xFFFFu)
        return -EMLINK;

    ret = ext2_add_dir_entry(parent, target->first_cluster, name,
                             ext2_file_type_from_mode(target->mode));
    if (ret < 0)
        return ret;

    ret = ext2_adjust_link_count(target, 1);
    if (ret < 0)
        ext2_remove_dir_entry(parent, name);

    return ret;
}

static int ext2_create_symlink_unlocked(inode_t* parent, const char* name, const char* target)
{
    uint32_t ino;
    uint32_t now;
    uint32_t target_len;
    ext2_inode_t di;

    if (!parent || !name || !target) return -EINVAL;
    if (!S_ISDIR(parent->mode)) return -ENOTDIR;

    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -EINVAL;

    target_len = strlen(target);
    if (target_len == 0 || target_len >= MAX_PATH) return -EINVAL;

    inode_t* existing = ext2_inode_lookup(parent, name);
    if (existing) {
        put_inode(existing);
        return -EEXIST;
    }

    if (ext2_alloc_inode(&ino) < 0)
        return -ENOSPC;

    now = get_current_time();
    memset(&di, 0, sizeof(di));
    di.i_mode = EXT2_S_IFLNK | 0777;
    di.i_uid = current_uid();
    di.i_gid = current_gid();
    di.i_size = target_len;
    di.i_atime = now;
    di.i_ctime = now;
    di.i_mtime = now;
    di.i_links_count = 1;

    if (target_len <= sizeof(di.i_block)) {
        memcpy((char*)di.i_block, target, target_len);
        di.i_blocks = 0;
    } else {
        uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
        if (!blkbuf) {
            ext2_free_inode(ino);
            return -ENOMEM;
        }

        uint32_t written = 0;
        while (written < target_len) {
            bool allocated = false;
            uint32_t blk_idx = written / ext2_fs.block_size;
            uint32_t blk_off = written % ext2_fs.block_size;
            uint32_t chunk = ext2_fs.block_size - blk_off;
            uint32_t blk = ext2_get_or_alloc_block_at(&di, blk_idx, &allocated);
            (void)allocated;

            if (blk == 0) {
                kfree(blkbuf);
                ext2_free_inode(ino);
                return -ENOSPC;
            }

            memset(blkbuf, 0, ext2_fs.block_size);
            if (chunk > target_len - written)
                chunk = target_len - written;
            memcpy(blkbuf + blk_off, target + written, chunk);
            if (ext2_write_block(blk, blkbuf) < 0) {
                kfree(blkbuf);
                ext2_free_inode(ino);
                return -EIO;
            }
            written += chunk;
        }
        kfree(blkbuf);
    }

    if (ext2_write_disk_inode(ino, &di) < 0) {
        ext2_free_inode(ino);
        return -EIO;
    }

    int ret = ext2_add_dir_entry(parent, ino, name, EXT2_FT_SYMLINK);
    if (ret < 0) {
        inode_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.first_cluster = ino;
        tmp.mode = di.i_mode;
        ext2_truncate_inode_data(&tmp, false);
        ext2_free_inode(ino);
        return ret;
    }

    return 0;
}

static int ext2_readlink_inode_unlocked(inode_t* inode, char* buf, size_t bufsiz)
{
    ext2_inode_t di;
    size_t copied = 0;

    if (!inode || !buf || bufsiz == 0) return -EINVAL;
    if (ext2_read_disk_inode(inode->first_cluster, &di) < 0) return -EIO;
    if (!S_ISLNK(di.i_mode)) return -EINVAL;

    size_t wanted = di.i_size;
    if (wanted > bufsiz)
        wanted = bufsiz;

    if (di.i_size <= sizeof(di.i_block) && di.i_blocks == 0) {
        memcpy(buf, (char*)di.i_block, wanted);
        inode->atime = get_current_time();
        di.i_atime = inode->atime;
        ext2_write_disk_inode(inode->first_cluster, &di);
        return (int)wanted;
    }

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    while (copied < wanted) {
        uint32_t blk_idx = copied / ext2_fs.block_size;
        uint32_t blk_off = copied % ext2_fs.block_size;
        uint32_t chunk = ext2_fs.block_size - blk_off;
        uint32_t blk = 0;

        if (ext2_map_block(&di, blk_idx, &blk) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        if (chunk > wanted - copied)
            chunk = wanted - copied;

        if (blk == 0) {
            memset(buf + copied, 0, chunk);
        } else {
            if (ext2_read_block(blk, blkbuf) < 0) {
                kfree(blkbuf);
                return -EIO;
            }
            memcpy(buf + copied, blkbuf + blk_off, chunk);
        }

        copied += chunk;
    }

    kfree(blkbuf);
    inode->atime = get_current_time();
    di.i_atime = inode->atime;
    ext2_write_disk_inode(inode->first_cluster, &di);
    return (int)copied;
}

static int ext2_inode_mkdir(inode_t* dir, const char* name, uint16_t mode)
{
    if (!dir || !name) return -EINVAL;
    if (!S_ISDIR(dir->mode)) return -ENOTDIR;

    uint32_t name_len = strlen(name);
    if (name_len == 0 || name_len > 255) return -EINVAL;

    inode_t* existing = ext2_inode_lookup(dir, name);
    if (existing) {
        put_inode(existing);
        return -EEXIST;
    }

    uint32_t ino;
    int ret = ext2_alloc_inode(&ino);
    if (ret < 0)
        return ret;

    uint32_t data_block;
    ret = ext2_alloc_block(&data_block);
    if (ret < 0) {
        ext2_free_inode(ino);
        return ret;
    }

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) {
        ext2_free_block_list(&data_block, 1);
        ext2_free_inode(ino);
        return -ENOMEM;
    }

    memset(blkbuf, 0, ext2_fs.block_size);
    uint16_t dot_len = ext2_dir_rec_len(1);
    ext2_init_dirent((ext2_dir_entry_t*)blkbuf, ino, dot_len,
                     ".", 1, EXT2_FT_DIR);
    ext2_init_dirent((ext2_dir_entry_t*)(blkbuf + dot_len), dir->first_cluster,
                     ext2_fs.block_size - dot_len, "..", 2, EXT2_FT_DIR);

    if (ext2_write_block(data_block, blkbuf) < 0) {
        kfree(blkbuf);
        ext2_free_block_list(&data_block, 1);
        ext2_free_inode(ino);
        return -EIO;
    }
    kfree(blkbuf);

    uint32_t now = get_current_time();
    ext2_inode_t child;
    memset(&child, 0, sizeof(child));
    child.i_mode = EXT2_S_IFDIR | (mode & 0777);
    child.i_uid = current_uid();
    child.i_gid = current_gid();
    child.i_size = ext2_fs.block_size;
    child.i_atime = now;
    child.i_ctime = now;
    child.i_mtime = now;
    child.i_links_count = 2;
    child.i_blocks = ext2_fs.sectors_per_block;
    child.i_block[0] = data_block;

    if (ext2_write_disk_inode(ino, &child) < 0) {
        ext2_free_block_list(&data_block, 1);
        ext2_free_inode(ino);
        return -EIO;
    }

    if (ext2_adjust_used_dirs(ino, 1) < 0) {
        ext2_free_block_list(&data_block, 1);
        ext2_free_inode(ino);
        return -EIO;
    }

    ret = ext2_add_dir_entry(dir, ino, name, EXT2_FT_DIR);
    if (ret < 0) {
        ext2_adjust_used_dirs(ino, -1);
        ext2_free_block_list(&data_block, 1);
        ext2_free_inode(ino);
        return ret;
    }

    ext2_inode_t parent_disk;
    if (ext2_read_disk_inode(dir->first_cluster, &parent_disk) < 0)
        return -EIO;

    parent_disk.i_links_count++;
    parent_disk.i_mtime = now;
    parent_disk.i_ctime = now;
    if (ext2_write_disk_inode(dir->first_cluster, &parent_disk) < 0)
        return -EIO;

    dir->mtime = parent_disk.i_mtime;
    dir->ctime = parent_disk.i_ctime;
    dir->nlink = parent_disk.i_links_count;

    return 0;
}

/* ---------- inode_operations ---------- */

static inode_t* ext2_inode_lookup(inode_t* dir, const char* name)
{
    ext2_inode_t disk;
    if (ext2_read_disk_inode(dir->first_cluster, &disk) < 0) return NULL;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return NULL;

    uint8_t  name_len = (uint8_t)strlen(name);
    uint32_t idx;

    for (idx = 0; idx < ext2_max_supported_file_blocks(); idx++) {
        uint32_t blk = 0;

        /* Mapping errors make lookup fail cleanly instead of faking a hole. */
        if (ext2_map_block(&disk, idx, &blk) < 0 || blk == 0)
            break;

        if (ext2_read_block(blk, blkbuf) < 0) break;

        uint32_t offset = 0;
        while (offset + sizeof(ext2_dir_entry_t) <= ext2_fs.block_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blkbuf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                uint32_t found = de->inode;
                kfree(blkbuf);
                return ext2_make_inode(found);
            }
            offset += de->rec_len;
        }
    }

    kfree(blkbuf);
    return NULL;
}

static inode_t* ext2_inode_lookup_op(inode_t* dir, const char* name)
{
    inode_t* ret;

    /*
     * Path lookup races with directory mutation because ext2 directory entries
     * are rewritten in place. Keep the backend coherent by serializing lookups
     * with create/unlink/rmdir/rename until ArmOS grows ordered inode/dentry
     * locks.
     */
    ext2_op_acquire();
    ret = ext2_inode_lookup(dir, name);
    ext2_op_release();
    return ret;
}

static int ext2_remove_dir_entry(inode_t* dir, const char* name)
{
    ext2_inode_t disk;
    if (!dir || !name) return -EINVAL;
    if (ext2_read_disk_inode(dir->first_cluster, &disk) < 0) return -EIO;

    uint32_t name_len32 = strlen(name);
    if (name_len32 == 0 || name_len32 > 255) return -EINVAL;
    uint8_t name_len = (uint8_t)name_len32;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    for (uint32_t idx = 0; idx < ext2_max_supported_file_blocks(); idx++) {
        uint32_t blk = 0;

        if (ext2_map_block(&disk, idx, &blk) < 0) {
            kfree(blkbuf);
            return -EIO;
        }
        if (blk == 0) break;

        if (ext2_read_block(blk, blkbuf) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        uint32_t offset = 0;
        while (offset + sizeof(ext2_dir_entry_t) <= ext2_fs.block_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blkbuf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                de->inode = 0;

                disk.i_mtime = get_current_time();
                disk.i_ctime = disk.i_mtime;

                if (ext2_write_block(blk, blkbuf) < 0 ||
                    ext2_write_disk_inode(dir->first_cluster, &disk) < 0) {
                    kfree(blkbuf);
                    return -EIO;
                }

                dir->mtime = disk.i_mtime;
                dir->ctime = disk.i_ctime;
                kfree(blkbuf);
                return 0;
            }

            offset += de->rec_len;
        }
    }

    kfree(blkbuf);
    return -ENOENT;
}

static bool ext2_directory_is_empty(inode_t* dir)
{
    ext2_inode_t disk;
    if (!dir || !S_ISDIR(dir->mode)) return false;
    if (ext2_read_disk_inode(dir->first_cluster, &disk) < 0) return false;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return false;

    for (uint32_t idx = 0; idx < ext2_max_supported_file_blocks(); idx++) {
        uint32_t blk = 0;

        /*
         * On mapping errors, report the directory as non-empty so rmdir()
         * refuses it. The opposite would allow deleting a populated directory
         * just because an I/O error hid its entries.
         */
        if (ext2_map_block(&disk, idx, &blk) < 0) {
            kfree(blkbuf);
            return false;
        }
        if (blk == 0) break;

        if (ext2_read_block(blk, blkbuf) < 0) {
            kfree(blkbuf);
            return false;
        }

        uint32_t offset = 0;
        while (offset + sizeof(ext2_dir_entry_t) <= ext2_fs.block_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blkbuf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                bool is_dot = de->name_len == 1 && de->name[0] == '.';
                bool is_dotdot = de->name_len == 2 &&
                                  de->name[0] == '.' && de->name[1] == '.';
                if (!is_dot && !is_dotdot) {
                    kfree(blkbuf);
                    return false;
                }
            }

            offset += de->rec_len;
        }
    }

    kfree(blkbuf);
    return true;
}

static int ext2_update_dotdot(inode_t* dir, uint32_t new_parent_ino)
{
    ext2_inode_t disk;
    if (!dir || !S_ISDIR(dir->mode)) return -EINVAL;
    if (ext2_read_disk_inode(dir->first_cluster, &disk) < 0) return -EIO;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    for (uint32_t idx = 0; idx < ext2_max_supported_file_blocks(); idx++) {
        uint32_t blk = 0;

        if (ext2_map_block(&disk, idx, &blk) < 0) {
            kfree(blkbuf);
            return -EIO;
        }
        if (blk == 0) break;

        if (ext2_read_block(blk, blkbuf) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        uint32_t offset = 0;
        while (offset + sizeof(ext2_dir_entry_t) <= ext2_fs.block_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blkbuf + offset);
            if (de->rec_len == 0) break;

            if (de->inode != 0 &&
                de->name_len == 2 &&
                de->name[0] == '.' &&
                de->name[1] == '.') {
                de->inode = new_parent_ino;
                if (ext2_write_block(blk, blkbuf) < 0) {
                    kfree(blkbuf);
                    return -EIO;
                }

                kfree(blkbuf);
                return 0;
            }

            offset += de->rec_len;
        }
    }

    kfree(blkbuf);
    return -ENOENT;
}

static int ext2_adjust_link_count(inode_t* inode, int delta)
{
    ext2_inode_t disk;
    if (!inode) return -EINVAL;
    if (ext2_read_disk_inode(inode->first_cluster, &disk) < 0) return -EIO;

    if (delta > 0) {
        disk.i_links_count++;
    } else if (delta < 0 && disk.i_links_count > 0) {
        disk.i_links_count--;
    }

    disk.i_ctime = get_current_time();
    if (ext2_write_disk_inode(inode->first_cluster, &disk) < 0)
        return -EIO;

    inode->ctime = disk.i_ctime;
    inode->nlink = disk.i_links_count;
    return 0;
}

static int ext2_inode_unlink(inode_t* dir, const char* name)
{
    if (!dir || !name) return -EINVAL;

    inode_t* target = ext2_inode_lookup(dir, name);
    if (!target) return -ENOENT;

    if (S_ISDIR(target->mode)) {
        put_inode(target);
        return -EISDIR;
    }

    int ret = ext2_remove_dir_entry(dir, name);
    if (ret == 0) {
        ext2_inode_t disk;
        if (ext2_read_disk_inode(target->first_cluster, &disk) < 0) {
            ret = -EIO;
        } else if (disk.i_links_count > 1) {
            disk.i_links_count--;
            disk.i_ctime = get_current_time();
            if (ext2_write_disk_inode(target->first_cluster, &disk) < 0) {
                ret = -EIO;
            } else {
                target->nlink = disk.i_links_count;
                target->ctime = disk.i_ctime;
            }
        } else {
            ret = ext2_truncate_inode_unlocked(target);
            if (ret == 0)
                ret = ext2_free_inode(target->first_cluster);
        }
    }

    put_inode(target);
    return ret;
}

static int ext2_inode_readlink(inode_t* inode, char* buf, size_t bufsiz)
{
    return ext2_readlink_inode_unlocked(inode, buf, bufsiz);
}

static int ext2_inode_rmdir(inode_t* dir, const char* name)
{
    if (!dir || !name) return -EINVAL;

    inode_t* target = ext2_inode_lookup(dir, name);
    if (!target) return -ENOENT;

    if (!S_ISDIR(target->mode)) {
        put_inode(target);
        return -ENOTDIR;
    }

    if (!ext2_directory_is_empty(target)) {
        put_inode(target);
        return -ENOTEMPTY;
    }

    int ret = ext2_remove_dir_entry(dir, name);
    if (ret == 0)
        ret = ext2_truncate_inode_data(target, true);
    if (ret == 0)
        ret = ext2_free_inode(target->first_cluster);
    if (ret == 0)
        ret = ext2_adjust_used_dirs(target->first_cluster, -1);

    if (ret == 0) {
        ext2_inode_t parent_disk;
        if (ext2_read_disk_inode(dir->first_cluster, &parent_disk) < 0) {
            put_inode(target);
            return -EIO;
        }

        if (parent_disk.i_links_count > 0)
            parent_disk.i_links_count--;
        parent_disk.i_mtime = get_current_time();
        parent_disk.i_ctime = parent_disk.i_mtime;
        if (ext2_write_disk_inode(dir->first_cluster, &parent_disk) < 0) {
            put_inode(target);
            return -EIO;
        }

        dir->mtime = parent_disk.i_mtime;
        dir->ctime = parent_disk.i_ctime;
        dir->nlink = parent_disk.i_links_count;
    }

    put_inode(target);
    return ret;
}

static int ext2_inode_rename(inode_t* old_dir, const char* old_name,
                             inode_t* new_dir, const char* new_name)
{
    if (!old_dir || !new_dir || !old_name || !new_name) return -EINVAL;
    if (!S_ISDIR(old_dir->mode) || !S_ISDIR(new_dir->mode)) return -ENOTDIR;

    uint32_t old_len = strlen(old_name);
    uint32_t new_len = strlen(new_name);
    if (old_len == 0 || old_len > 255 || new_len == 0 || new_len > 255)
        return -EINVAL;

    if (old_dir->first_cluster == new_dir->first_cluster &&
        strcmp(old_name, new_name) == 0)
        return 0;

    inode_t* target = ext2_inode_lookup(old_dir, old_name);
    if (!target) return -ENOENT;

    inode_t* existing = ext2_inode_lookup(new_dir, new_name);
    if (existing) {
        put_inode(existing);
        put_inode(target);
        return -EEXIST;
    }

    uint8_t file_type = ext2_file_type_from_mode(target->mode);
    int ret = ext2_add_dir_entry(new_dir, target->first_cluster, new_name, file_type);
    if (ret == 0)
        ret = ext2_remove_dir_entry(old_dir, old_name);

    bool moved_dir = ret == 0 &&
                     S_ISDIR(target->mode) &&
                     old_dir->first_cluster != new_dir->first_cluster;
    if (moved_dir)
        ret = ext2_update_dotdot(target, new_dir->first_cluster);

    if (moved_dir && ret == 0)
        ret = ext2_adjust_link_count(old_dir, -1);
    if (moved_dir && ret == 0)
        ret = ext2_adjust_link_count(new_dir, 1);

    if (ret == 0) {
        ext2_inode_t disk;
        if (ext2_read_disk_inode(target->first_cluster, &disk) == 0) {
            disk.i_ctime = get_current_time();
            ext2_write_disk_inode(target->first_cluster, &disk);
            target->ctime = disk.i_ctime;
        }
    }

    put_inode(target);
    return ret;
}

/* ---------- file_operations — open/close ---------- */

static int ext2_file_open(inode_t* inode, file_t* file)
{
    int ret = 0;

    if (!inode || !file) return -EBADF;
    if (S_ISDIR(inode->mode) && ((file->flags & O_ACCMODE) != O_RDONLY))
        return -EISDIR;

    ext2_op_acquire();

    ext2_inode_t* di = kmalloc(sizeof(ext2_inode_t));
    if (!di) {
        ret = -ENOMEM;
        goto out;
    }

    if (ext2_read_disk_inode(inode->first_cluster, di) < 0) {
        kfree(di);
        ret = -EIO;
        goto out;
    }
    file->private_data = di;

out:
    ext2_op_release();
    return ret;
}

static int ext2_file_close(file_t* file)
{
    int ret = 0;

    if (!file) return -EBADF;

    if ((file->flags & O_ACCMODE) != O_RDONLY && ext2_dirty)
        ret = ext2_sync();

    if (file->private_data && ext2_valid_disk_inode_ptr((ext2_inode_t*)file->private_data)) {
        kfree(file->private_data);
    }
    file->private_data = NULL;
    return ret;
}

static int ext2_truncate_inode_data(inode_t* inode, bool allow_dir)
{
    if (!inode) return -EINVAL;
    if (!allow_dir && S_ISDIR(inode->mode)) return -EISDIR;

    ext2_inode_t di;
    if (ext2_read_disk_inode(inode->first_cluster, &di) < 0)
        return -EIO;

    if (S_ISLNK(di.i_mode) && di.i_blocks == 0) {
        di.i_size = 0;
        memset(di.i_block, 0, sizeof(di.i_block));
        di.i_mtime = get_current_time();
        di.i_ctime = di.i_mtime;

        inode->size = 0;
        inode->blocks = 0;
        inode->mtime = di.i_mtime;
        inode->ctime = di.i_ctime;

        if (ext2_write_disk_inode(inode->first_cluster, &di) < 0)
            return -EIO;
        return 0;
    }

    if (di.i_block[14])
        return -EFBIG; /* triple-indirect truncation is not implemented yet */

    uint32_t ptrs_per_block = ext2_ptrs_per_block();
    uint32_t max_blocks = ext2_truncate_block_capacity(&di);
    uint32_t* blocks = kmalloc(max_blocks * sizeof(uint32_t));
    if (!blocks) return -ENOMEM;

    uint32_t count = 0;

    for (uint32_t i = 0; i < 12; i++) {
        if (di.i_block[i] != 0)
            blocks[count++] = di.i_block[i];
    }

    if (di.i_block[12] != 0) {
        uint32_t* indirect = kmalloc(ext2_fs.block_size);
        if (!indirect) {
            kfree(blocks);
            return -ENOMEM;
        }

        if (ext2_read_block(di.i_block[12], indirect) < 0) {
            kfree(indirect);
            kfree(blocks);
            return -EIO;
        }

        for (uint32_t i = 0; i < ptrs_per_block; i++) {
            if (indirect[i] != 0)
                blocks[count++] = indirect[i];
        }

        kfree(indirect);
        blocks[count++] = di.i_block[12];
    }

    if (di.i_block[13] != 0) {
        uint32_t* dbl = kmalloc(ext2_fs.block_size);
        uint32_t* indirect = kmalloc(ext2_fs.block_size);
        if (!dbl || !indirect) {
            if (dbl) kfree(dbl);
            if (indirect) kfree(indirect);
            kfree(blocks);
            return -ENOMEM;
        }

        if (ext2_read_block(di.i_block[13], dbl) < 0) {
            kfree(dbl);
            kfree(indirect);
            kfree(blocks);
            return -EIO;
        }

        for (uint32_t i = 0; i < ptrs_per_block; i++) {
            if (dbl[i] == 0)
                continue;

            if (ext2_read_block(dbl[i], indirect) < 0) {
                kfree(dbl);
                kfree(indirect);
                kfree(blocks);
                return -EIO;
            }

            for (uint32_t j = 0; j < ptrs_per_block; j++) {
                if (indirect[j] != 0 && count < max_blocks)
                    blocks[count++] = indirect[j];
            }

            if (count < max_blocks)
                blocks[count++] = dbl[i];
        }

        kfree(dbl);
        kfree(indirect);
        if (count < max_blocks)
            blocks[count++] = di.i_block[13];
    }

    int ret = ext2_free_block_list(blocks, count);
    kfree(blocks);
    if (ret < 0)
        return ret;

    di.i_size = 0;
    di.i_blocks = 0;
    memset(di.i_block, 0, sizeof(di.i_block));
    di.i_mtime = get_current_time();
    di.i_ctime = di.i_mtime;

    inode->size = 0;
    inode->blocks = 0;
    inode->mtime = di.i_mtime;
    inode->ctime = di.i_ctime;

    if (ext2_write_disk_inode(inode->first_cluster, &di) < 0)
        return -EIO;

    return 0;
}

static int ext2_truncate_inode_unlocked(inode_t* inode)
{
    return ext2_truncate_inode_data(inode, false);
}

inode_t* ext2_create_file(inode_t* parent, const char* name, mode_t mode)
{
    inode_t* ret;

    ext2_op_acquire();
    ret = ext2_create_file_unlocked(parent, name, mode);
    ext2_op_release();

    return ret;
}

int ext2_link_inode(inode_t* parent, const char* name, inode_t* target)
{
    int ret;

    ext2_op_acquire();
    ret = ext2_link_inode_unlocked(parent, name, target);
    ext2_op_release();

    return ret;
}

int ext2_create_symlink(inode_t* parent, const char* name, const char* target)
{
    int ret;

    ext2_op_acquire();
    ret = ext2_create_symlink_unlocked(parent, name, target);
    ext2_op_release();

    return ret;
}

int ext2_readlink_inode(inode_t* inode, char* buf, size_t bufsiz)
{
    int ret;

    ext2_op_acquire();
    ret = ext2_readlink_inode_unlocked(inode, buf, bufsiz);
    ext2_op_release();

    return ret;
}

int ext2_truncate_inode(inode_t* inode)
{
    int ret;

    ext2_op_acquire();
    ret = ext2_truncate_inode_unlocked(inode);
    ext2_op_release();

    return ret;
}

static bool ext2_block_ptrs_empty(const uint32_t* ptrs, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (ptrs[i] != 0)
            return false;
    }
    return true;
}

static int ext2_queue_block_free(uint32_t* blocks, uint32_t* count,
                                 uint32_t capacity, uint32_t block)
{
    if (block == 0)
        return 0;
    if (*count >= capacity)
        return -EFBIG;

    blocks[(*count)++] = block;
    return 0;
}

static int ext2_file_truncate_unlocked(file_t* file, off_t length)
{
    if (!file || !file->inode) return -EBADF;
    if (length < 0) return -EINVAL;
    if (S_ISDIR(file->inode->mode)) return -EISDIR;

    ext2_inode_t* di = (ext2_inode_t*)file->private_data;
    if (!ext2_valid_disk_inode_ptr(di)) return -EBADF;

    uint32_t new_size = (uint32_t)length;
    uint32_t old_size = di->i_size;

    if (new_size == old_size)
        return 0;
    if (new_size > old_size)
        return -ENOSYS; /* no sparse/zero-extension support yet */

    if (new_size == 0) {
        int ret = ext2_truncate_inode_unlocked(file->inode);
        if (ret < 0)
            return ret;
        if (ext2_read_disk_inode(file->inode->first_cluster, di) < 0)
            return -EIO;
        file->offset = 0;
        return 0;
    }

    if (di->i_block[14])
        return -EFBIG; /* triple-indirect truncation is not implemented yet */

    uint32_t ptrs_per_block = ext2_ptrs_per_block();
    uint32_t keep_blocks = (new_size + ext2_fs.block_size - 1) / ext2_fs.block_size;
    uint32_t free_capacity = ext2_truncate_block_capacity(di);
    uint32_t* blocks = kmalloc(free_capacity * sizeof(uint32_t));
    if (!blocks) return -ENOMEM;

    uint32_t free_count = 0;
    int ret = 0;

    for (uint32_t i = 0; i < 12; i++) {
        if (i >= keep_blocks && di->i_block[i] != 0) {
            ret = ext2_queue_block_free(blocks, &free_count, free_capacity, di->i_block[i]);
            if (ret < 0) goto out;
            di->i_block[i] = 0;
        }
    }

    if (di->i_block[12] != 0) {
        uint32_t* indirect = kmalloc(ext2_fs.block_size);
        if (!indirect) {
            ret = -ENOMEM;
            goto out;
        }

        if (ext2_read_block(di->i_block[12], indirect) < 0) {
            kfree(indirect);
            ret = -EIO;
            goto out;
        }

        bool changed = false;
        for (uint32_t i = 0; i < ptrs_per_block; i++) {
            uint32_t logical = 12 + i;
            if (logical >= keep_blocks && indirect[i] != 0) {
                ret = ext2_queue_block_free(blocks, &free_count, free_capacity, indirect[i]);
                if (ret < 0) {
                    kfree(indirect);
                    goto out;
                }
                indirect[i] = 0;
                changed = true;
            }
        }

        if (ext2_block_ptrs_empty(indirect, ptrs_per_block)) {
            ret = ext2_queue_block_free(blocks, &free_count, free_capacity, di->i_block[12]);
            if (ret < 0) {
                kfree(indirect);
                goto out;
            }
            di->i_block[12] = 0;
        } else if (changed && ext2_write_block(di->i_block[12], indirect) < 0) {
            kfree(indirect);
            ret = -EIO;
            goto out;
        }

        kfree(indirect);
    }

    if (di->i_block[13] != 0) {
        uint32_t* dbl = kmalloc(ext2_fs.block_size);
        uint32_t* indirect = kmalloc(ext2_fs.block_size);
        if (!dbl || !indirect) {
            if (dbl) kfree(dbl);
            if (indirect) kfree(indirect);
            ret = -ENOMEM;
            goto out;
        }

        if (ext2_read_block(di->i_block[13], dbl) < 0) {
            kfree(dbl);
            kfree(indirect);
            ret = -EIO;
            goto out;
        }

        bool dbl_changed = false;
        for (uint32_t first = 0; first < ptrs_per_block; first++) {
            if (dbl[first] == 0)
                continue;

            if (ext2_read_block(dbl[first], indirect) < 0) {
                kfree(dbl);
                kfree(indirect);
                ret = -EIO;
                goto out;
            }

            bool indirect_changed = false;
            for (uint32_t second = 0; second < ptrs_per_block; second++) {
                uint32_t logical = 12 + ptrs_per_block + first * ptrs_per_block + second;
                if (logical >= keep_blocks && indirect[second] != 0) {
                    ret = ext2_queue_block_free(blocks, &free_count, free_capacity, indirect[second]);
                    if (ret < 0) {
                        kfree(dbl);
                        kfree(indirect);
                        goto out;
                    }
                    indirect[second] = 0;
                    indirect_changed = true;
                }
            }

            if (ext2_block_ptrs_empty(indirect, ptrs_per_block)) {
                ret = ext2_queue_block_free(blocks, &free_count, free_capacity, dbl[first]);
                if (ret < 0) {
                    kfree(dbl);
                    kfree(indirect);
                    goto out;
                }
                dbl[first] = 0;
                dbl_changed = true;
            } else if (indirect_changed && ext2_write_block(dbl[first], indirect) < 0) {
                kfree(dbl);
                kfree(indirect);
                ret = -EIO;
                goto out;
            }
        }

        if (ext2_block_ptrs_empty(dbl, ptrs_per_block)) {
            ret = ext2_queue_block_free(blocks, &free_count, free_capacity, di->i_block[13]);
            if (ret < 0) {
                kfree(dbl);
                kfree(indirect);
                goto out;
            }
            di->i_block[13] = 0;
        } else if (dbl_changed && ext2_write_block(di->i_block[13], dbl) < 0) {
            kfree(dbl);
            kfree(indirect);
            ret = -EIO;
            goto out;
        }

        kfree(dbl);
        kfree(indirect);
    }

    uint32_t tail = new_size % ext2_fs.block_size;
    if (tail != 0) {
        uint32_t blk = 0;

        if (ext2_map_block(di, new_size / ext2_fs.block_size, &blk) < 0) {
            ret = -EIO;
            goto out;
        }
        if (blk != 0) {
            uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
            if (!blkbuf) {
                ret = -ENOMEM;
                goto out;
            }
            if (ext2_read_block(blk, blkbuf) < 0) {
                kfree(blkbuf);
                ret = -EIO;
                goto out;
            }
            memset(blkbuf + tail, 0, ext2_fs.block_size - tail);
            if (ext2_write_block(blk, blkbuf) < 0) {
                kfree(blkbuf);
                ret = -EIO;
                goto out;
            }
            kfree(blkbuf);
        }
    }

    if (free_count > 0) {
        ret = ext2_free_block_list(blocks, free_count);
        if (ret < 0)
            goto out;
    }

    di->i_size = new_size;
    if (di->i_blocks >= free_count * ext2_fs.sectors_per_block)
        di->i_blocks -= free_count * ext2_fs.sectors_per_block;
    else
        di->i_blocks = 0;
    di->i_mtime = get_current_time();
    di->i_ctime = di->i_mtime;

    file->inode->size = di->i_size;
    file->inode->blocks = di->i_blocks;
    file->inode->mtime = di->i_mtime;
    file->inode->ctime = di->i_ctime;
    if (file->offset > new_size)
        file->offset = new_size;

    if (ext2_write_disk_inode(file->inode->first_cluster, di) < 0)
        ret = -EIO;

out:
    kfree(blocks);
    return ret;
}

static int ext2_file_truncate(file_t* file, off_t length)
{
    int ret;

    ext2_op_acquire();
    ret = ext2_file_truncate_unlocked(file, length);
    ext2_op_release();

    return ret;
}

/* ---------- file_operations — regular files ---------- */

static ssize_t ext2_file_read(file_t* file, void* buffer, size_t count)
{
    ssize_t ret;

    ext2_op_acquire();
    ret = ext2_file_read_unlocked(file, buffer, count);
    ext2_op_release();

    return ret;
}

static ssize_t ext2_file_read_unlocked(file_t* file, void* buffer, size_t count)
{
    if (!file || !buffer) return -EBADF;

    ext2_inode_t* di = (ext2_inode_t*)file->private_data;
    if (!ext2_valid_disk_inode_ptr(di)) return -EBADF;

    uint32_t size = di->i_size;
    if (file->offset >= size) return 0;
    if (file->offset + (uint32_t)count > size)
        count = size - file->offset;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    ssize_t  total = 0;
    uint8_t* dst   = (uint8_t*)buffer;
    ext2_map_cursor_t cur;

    /*
     * Consecutive data blocks share indirect tables. Keeping the last resolved
     * table in this cursor avoids one block-sized allocation/copy per data
     * block while streaming through a file.
     */
    ext2_map_cursor_init(&cur);

    while (count > 0) {
        uint32_t blk_idx = file->offset / ext2_fs.block_size;
        uint32_t blk_off = file->offset % ext2_fs.block_size;
        uint32_t blk     = 0;

        if (ext2_map_block_cursor(di, blk_idx, &blk, &cur) < 0) {
            /*
             * Memory or I/O errors while resolving metadata are not sparse
             * holes. Returning zero-filled data here would hide corruption or
             * backend failures behind a successful read().
             */
            total = total ? total : -EIO;
            break;
        }

        uint32_t chunk = ext2_fs.block_size - blk_off;
        if (chunk > (uint32_t)count) chunk = (uint32_t)count;

        if (blk == 0) {
            memset(dst, 0, chunk);
        } else {
            if (blk_off == 0 && chunk == ext2_fs.block_size) {
                uint32_t max_run = (uint32_t)(count / ext2_fs.block_size);
                uint32_t run = 1;

                if (max_run > EXT2_IO_MAX_BLOCKS)
                    max_run = EXT2_IO_MAX_BLOCKS;

                while (run < max_run) {
                    uint32_t next = 0;

                    if (ext2_map_block_cursor(di, blk_idx + run, &next,
                                              &cur) < 0) {
                        total = total ? total : -EIO;
                        goto out;
                    }
                    if (next != blk + run)
                        break;
                    run++;
                }

                if (run > 1) {
                    uint32_t bytes = run * ext2_fs.block_size;

                    if (ext2_read_contiguous_blocks(blk, run, dst) < 0) {
                        total = total ? total : -EIO;
                        break;
                    }
                    dst += bytes;
                    file->offset += bytes;
                    total += bytes;
                    count -= bytes;
                    continue;
                }
            }

            if (ext2_read_block(blk, blkbuf) < 0) {
                total = total ? total : -EIO;
                break;
            }
            memcpy(dst, blkbuf + blk_off, chunk);
        }

        dst         += chunk;
        file->offset += chunk;
        total        += chunk;
        count        -= chunk;
    }

out:
    ext2_map_cursor_release(&cur);
    kfree(blkbuf);
    return total;
}

static ssize_t ext2_file_write(file_t* file, const void* buffer, size_t count)
{
    if (!file || !file->inode || !buffer) return -EBADF;
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (count == 0) return 0;

    ext2_inode_t* di = (ext2_inode_t*)file->private_data;
    if (!ext2_valid_disk_inode_ptr(di)) return -EBADF;

    uint32_t old_size = di->i_size;
    if (file->offset > old_size) return -ENOSPC; /* no sparse files yet */

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    ssize_t total = 0;
    const uint8_t* src = (const uint8_t*)buffer;
    bool allocated_block = false;

    ext2_defer_allocation_writes = true;

    while (count > 0) {
        uint32_t blk_idx = file->offset / ext2_fs.block_size;
        uint32_t blk_off = file->offset % ext2_fs.block_size;
        uint32_t blk;

        allocated_block = false;
        blk = ext2_get_or_alloc_block_at(di, blk_idx, &allocated_block);

        if (blk == 0) {
            total = total ? total : -ENOSPC;
            break;
        }

        uint32_t chunk = ext2_fs.block_size - blk_off;
        if (chunk > (uint32_t)count) chunk = (uint32_t)count;

        /*
         * Read-modify-write is only needed for partial writes to an existing
         * block. Fully overwritten blocks do not need a read, and newly
         * allocated blocks are zeroed on disk, so a local memset is equivalent.
         */
        if (chunk == ext2_fs.block_size) {
            /* Full overwrite: blkbuf will be filled by memcpy below. */
        } else if (allocated_block) {
            memset(blkbuf, 0, ext2_fs.block_size);
        } else if (ext2_read_block(blk, blkbuf) < 0) {
            total = total ? total : -EIO;
            break;
        }

        memcpy(blkbuf + blk_off, src, chunk);

        if (ext2_write_data_block(blk, blkbuf) < 0) {
            total = total ? total : -EIO;
            break;
        }

        src += chunk;
        file->offset += chunk;
        total += chunk;
        count -= chunk;
    }

    if (total > 0) {
        if (file->offset > old_size)
            di->i_size = file->offset;
        di->i_mtime = get_current_time();
        di->i_ctime = di->i_mtime;
        if (file->inode) {
            file->inode->size = di->i_size;
            file->inode->mtime = di->i_mtime;
            file->inode->ctime = di->i_ctime;
            file->inode->blocks = di->i_blocks;
        }
        if (ext2_queue_pending_inode(file->inode->first_cluster, di) < 0)
            total = -EIO;
    }

    ext2_defer_allocation_writes = false;
    kfree(blkbuf);
    return total;
}

static off_t ext2_file_lseek(file_t* file, off_t offset, int whence)
{
    if (!file) return -EBADF;

    ext2_inode_t* di = (ext2_inode_t*)file->private_data;
    if (!ext2_valid_disk_inode_ptr(di)) return -EBADF;

    uint32_t size = di->i_size;
    off_t new_pos;

    switch (whence) {
        case SEEK_SET: new_pos = offset;                       break;
        case SEEK_CUR: new_pos = (off_t)file->offset + offset; break;
        case SEEK_END: new_pos = (off_t)size + offset;         break;
        default:       return -EINVAL;
    }

    if (new_pos < 0) return -EINVAL;
    file->offset = (uint32_t)new_pos;
    return new_pos;
}

/* ---------- file_operations — directories ---------- */

static int ext2_dir_readdir(file_t* file, dirent_t* dirent)
{
    int ret;

    ext2_op_acquire();
    ret = ext2_dir_readdir_unlocked(file, dirent);
    ext2_op_release();

    return ret;
}

static int ext2_dir_readdir_unlocked(file_t* file, dirent_t* dirent)
{
    if (!file || !dirent) return -EBADF;

    ext2_inode_t* di = (ext2_inode_t*)file->private_data;
    if (!ext2_valid_disk_inode_ptr(di)) return -EBADF;

    uint32_t dir_size = di->i_size;

    /* file->offset is a byte offset into the directory data */
    if (file->offset >= dir_size) return 0;

    uint8_t* blkbuf = kmalloc(ext2_fs.block_size);
    if (!blkbuf) return -ENOMEM;

    while (file->offset < dir_size) {
        uint32_t blk_idx = file->offset / ext2_fs.block_size;
        uint32_t blk_off = file->offset % ext2_fs.block_size;
        uint32_t blk     = 0;

        if (ext2_map_block(di, blk_idx, &blk) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        if (blk == 0) { file->offset = dir_size; break; }

        if (ext2_read_block(blk, blkbuf) < 0) {
            kfree(blkbuf);
            return -EIO;
        }

        ext2_dir_entry_t* de = (ext2_dir_entry_t*)(blkbuf + blk_off);

        if (de->rec_len == 0) {
            /* Advance to next block */
            file->offset = (blk_idx + 1) * ext2_fs.block_size;
            continue;
        }

        file->offset += de->rec_len;

        if (de->inode == 0) continue; /* deleted entry */

        /* Fill dirent */
        dirent->d_ino = de->inode;
        if (de->file_type == EXT2_FT_FIFO)
            dirent->d_type = DT_FIFO;
        else if (de->file_type == EXT2_FT_CHRDEV)
            dirent->d_type = DT_CHR;
        else if (de->file_type == EXT2_FT_BLKDEV)
            dirent->d_type = DT_BLK;
        else if (de->file_type == EXT2_FT_DIR)
            dirent->d_type = DT_DIR;
        else if (de->file_type == EXT2_FT_SYMLINK)
            dirent->d_type = DT_LNK;
        else
            dirent->d_type = DT_REG;
        uint8_t nlen = de->name_len;
        memcpy(dirent->d_name, de->name, nlen);
        dirent->d_name[nlen] = '\0';
        dirent->d_reclen = sizeof(dirent_t);

        kfree(blkbuf);
        return 1;
    }

    kfree(blkbuf);
    return 0;
}

/* ---------- locked VFS entry points ---------- */

static int ext2_inode_mkdir_op(inode_t* dir, const char* name, uint16_t mode)
{
    int ret;
    ext2_op_acquire();
    ret = ext2_inode_mkdir(dir, name, mode);
    ext2_op_release();
    return ret;
}

static int ext2_inode_unlink_op(inode_t* dir, const char* name)
{
    int ret;
    ext2_op_acquire();
    ret = ext2_inode_unlink(dir, name);
    ext2_op_release();
    return ret;
}

static int ext2_inode_rmdir_op(inode_t* dir, const char* name)
{
    int ret;
    ext2_op_acquire();
    ret = ext2_inode_rmdir(dir, name);
    ext2_op_release();
    return ret;
}

static int ext2_inode_rename_op(inode_t* old_dir, const char* old_name,
                                inode_t* new_dir, const char* new_name)
{
    int ret;
    ext2_op_acquire();
    ret = ext2_inode_rename(old_dir, old_name, new_dir, new_name);
    ext2_op_release();
    return ret;
}

static int ext2_inode_readlink_op(inode_t* inode, char* buf, size_t bufsiz)
{
    int ret;
    ext2_op_acquire();
    ret = ext2_inode_readlink(inode, buf, bufsiz);
    ext2_op_release();
    return ret;
}

static ssize_t ext2_file_write_op(file_t* file, const void* buffer, size_t count)
{
    ssize_t ret;
    bool sync_requested;

    sync_requested = file &&
                     (file->flags & (O_SYNC | O_DSYNC)) != 0;
    ext2_op_acquire();
    ret = ext2_file_write(file, buffer, count);
    ext2_op_release();
    if (ret > 0 && sync_requested && ext2_sync() < 0)
        return -EIO;
    return ret;
}

/* ---------- mount ---------- */

inode_t* ext2_mount(uint64_t lba_start)
{
    /* Superblock is at byte offset 1024 = LBA offset 2 (512-byte sectors) */
    ext2_superblock_t* sb = kmalloc(1024);
    if (!sb) return NULL;

    if (blk_read_sectors(lba_start + 2, 2, sb) < 0) {
        KERROR("[EXT2] Cannot read superblock at LBA %llu\n",
               (unsigned long long)(lba_start + 2));
        kfree(sb);
        return NULL;
    }

    if (sb->s_magic != EXT2_MAGIC) {
        KERROR("[EXT2] Bad magic: 0x%X (expected 0x%X)\n", sb->s_magic, EXT2_MAGIC);
        kfree(sb);
        return NULL;
    }

    ext2_fs.lba_start         = lba_start;
    ext2_fs.block_size        = 1024u << sb->s_log_block_size;
    ext2_fs.sectors_per_block = ext2_fs.block_size / 512;
    ext2_fs.first_data_block  = sb->s_first_data_block;
    ext2_fs.blocks_count      = sb->s_blocks_count;
    ext2_fs.inodes_per_group  = sb->s_inodes_per_group;
    ext2_fs.blocks_per_group  = sb->s_blocks_per_group;
    ext2_fs.groups_count      = (sb->s_blocks_count - sb->s_first_data_block +
                                 sb->s_blocks_per_group - 1) / sb->s_blocks_per_group;
    memset(ext2_pending_inodes, 0, sizeof(ext2_pending_inodes));
    ext2_pending_inode_count  = 0;
    ext2_alloc_group_hint     = 0;
    ext2_alloc_bit_hint       = 0;
    ext2_fs.inode_size        = (sb->s_rev_level >= 1) ? sb->s_inode_size : 128;
    /* Group descriptor table starts right after the superblock's block */
    ext2_fs.gdesc_block       = sb->s_first_data_block + 1;
    ext2_fs.mounted           = true;
    memset(&ext2_stats, 0, sizeof(ext2_stats));
    ext2_dirty = false;
    ext2_stats.mounted = 1;
    ext2_stats.block_size = ext2_fs.block_size;
    ext2_stats.blocks_count = ext2_fs.blocks_count;
    ext2_stats.groups_count = ext2_fs.groups_count;
    ext2_stats.inodes_per_group = ext2_fs.inodes_per_group;
    ext2_stats.blocks_per_group = ext2_fs.blocks_per_group;
    ext2_block_cache_reset();

    /*
     * Pin the validated superblock: after mount, all reads and writes go
     * through this RAM copy, which ext2_sync() writes back to disk.
     */
    ext2_sb_cache = *sb;
    ext2_sb_cached = true;
    ext2_sb_cache_dirty = false;

    KINFO("[EXT2] Mounted: block_size=%u inodes_per_group=%u inode_size=%u gdesc_block=%u\n",
          ext2_fs.block_size, ext2_fs.inodes_per_group,
          ext2_fs.inode_size, ext2_fs.gdesc_block);

    kfree(sb);
    return ext2_make_inode(EXT2_ROOT_INO);
}
