/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: kernel/syscalls/shm.c
 * Layer: Kernel / syscall implementation
 *
 * Responsibilities:
 * - Validate user-facing syscall requests.
 * - Manage named, growable shared-memory objects and their descriptors.
 * - Enforce one global physical-page budget across all shared-memory objects.
 * - Map complete objects or page-aligned prefixes into process VM spaces.
 *
 * Notes:
 * - Never trust user pointers; copy through checked helpers.
 * - Page vectors are dynamically sized and impose no per-object page ceiling.
 * - The implementation is common to every architecture and platform.
 */

#include <kernel/shm.h>
#include <kernel/file.h>
#include <kernel/memory.h>
#include <kernel/task.h>
#include <kernel/userspace.h>
#include <kernel/string.h>
#include <kernel/kprintf.h>
#include <kernel/spinlock.h>
#include <kernel/user_layout.h>
#include <kernel/vfs.h>

typedef struct shm_object {
    bool used;
    bool unlinked;
    uint32_t id;
    char name[SHM_NAME_MAX];
    size_t size;
    uint32_t pages;
    uint32_t page_capacity;
    paddr_t *phys;
    uint32_t mappings;
    uint32_t handles;
} shm_object_t;

static shm_object_t shm_objects[SHM_MAX_OBJECTS];
static uint32_t shm_next_id = 1;
static uint32_t shm_global_pages;
static spinlock_t shm_lock = SPINLOCK_INIT("shm");

static shm_object_t *shm_find_by_name(const char *name)
{
    for (uint32_t i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (shm_objects[i].used && !shm_objects[i].unlinked &&
            strcmp(shm_objects[i].name, name) == 0) {
            return &shm_objects[i];
        }
    }
    return NULL;
}

static shm_object_t *shm_find_by_id(uint32_t id)
{
    for (uint32_t i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (shm_objects[i].used && shm_objects[i].id == id)
            return &shm_objects[i];
    }
    return NULL;
}

static shm_object_t *shm_alloc_slot(void)
{
    for (uint32_t i = 0; i < SHM_MAX_OBJECTS; i++) {
        if (!shm_objects[i].used)
            return &shm_objects[i];
    }
    return NULL;
}

static void shm_free_object_pages(shm_object_t *obj)
{
    for (uint32_t i = 0; i < obj->pages; i++) {
        if (obj->phys[i]) {
            free_page((void *)obj->phys[i]);
            obj->phys[i] = 0;
        }
    }
    if (obj->pages <= shm_global_pages)
        shm_global_pages -= obj->pages;
    else
        shm_global_pages = 0;
    kfree(obj->phys);
    obj->phys = NULL;
    obj->pages = 0;
    obj->page_capacity = 0;
}

static int shm_resize_locked(shm_object_t *obj, size_t requested_size)
{
    paddr_t *new_phys;
    uint32_t old_pages;
    uint32_t new_pages;
    uint32_t allocated;

    if (!obj || !obj->used)
        return -EINVAL;
    if (requested_size > SHM_GLOBAL_MAX_BYTES)
        return -EFBIG;

    requested_size = ALIGN_UP(requested_size, PAGE_SIZE);
    new_pages = (uint32_t)(requested_size / PAGE_SIZE);
    old_pages = obj->pages;
    if (new_pages == old_pages) {
        obj->size = requested_size;
        return 0;
    }

    if (new_pages < old_pages) {
        if (obj->mappings != 0)
            return -EBUSY;
        for (uint32_t i = new_pages; i < old_pages; i++) {
            free_page((void *)obj->phys[i]);
            obj->phys[i] = 0;
        }
        shm_global_pages -= old_pages - new_pages;
        obj->pages = new_pages;
        obj->size = requested_size;
        return 0;
    }

    if (new_pages - old_pages >
        SHM_GLOBAL_MAX_PAGES - shm_global_pages)
        return -ENOSPC;

    if (new_pages > obj->page_capacity) {
        new_phys = kcalloc(new_pages, sizeof(*new_phys));
        if (!new_phys)
            return -ENOMEM;
        if (old_pages != 0)
            memcpy(new_phys, obj->phys, old_pages * sizeof(*new_phys));
    } else {
        new_phys = obj->phys;
    }

    for (allocated = old_pages; allocated < new_pages; allocated++) {
        void *page = allocate_page();

        if (!page)
            break;
        new_phys[allocated] = (paddr_t)page;
    }
    if (allocated != new_pages) {
        while (allocated > old_pages)
            free_page((void *)new_phys[--allocated]);
        if (new_phys != obj->phys)
            kfree(new_phys);
        return -ENOMEM;
    }

    if (new_phys != obj->phys) {
        kfree(obj->phys);
        obj->phys = new_phys;
        obj->page_capacity = new_pages;
    }
    shm_global_pages += new_pages - old_pages;
    obj->pages = new_pages;
    obj->size = requested_size;
    return 0;
}

static void shm_try_destroy_locked(shm_object_t *obj)
{
    if (!obj || !obj->used || !obj->unlinked ||
        obj->mappings != 0 || obj->handles != 0)
        return;

    shm_free_object_pages(obj);
    memset(obj, 0, sizeof(*obj));
}

static int shm_file_close(file_t *file)
{
    shm_object_t *obj = file ? file->private_data : NULL;

    if (!obj)
        return 0;
    spin_lock(&shm_lock);
    if (obj->used && obj->handles > 0)
        obj->handles--;
    shm_try_destroy_locked(obj);
    spin_unlock(&shm_lock);
    file->private_data = NULL;
    return 0;
}

static int shm_file_truncate(file_t *file, off_t length)
{
    shm_object_t *obj = file ? file->private_data : NULL;
    int result;

    if (!obj || length < 0)
        return -EINVAL;

    spin_lock(&shm_lock);
    result = shm_resize_locked(obj, (size_t)length);
    if (result == 0 && file->inode)
        file->inode->size = obj->size;
    spin_unlock(&shm_lock);
    return result;
}

static file_operations_t shm_file_operations = {
    .read = NULL,
    .write = NULL,
    .open = NULL,
    .close = shm_file_close,
    .lseek = NULL,
    .readdir = NULL,
    .truncate = shm_file_truncate,
};

static int shm_install_handle(shm_object_t *obj)
{
    task_t *task = task_current_local();
    file_t *file;
    inode_t *inode;
    int fd;

    if (!task || !task->process || !obj)
        return -EINVAL;
    file = create_file();
    inode = file ? create_inode() : NULL;
    if (!file || !inode) {
        if (file)
            close_file(file);
        if (inode)
            put_inode(inode);
        spin_lock(&shm_lock);
        if (obj->used && obj->handles > 0)
            obj->handles--;
        shm_try_destroy_locked(obj);
        spin_unlock(&shm_lock);
        return -ENOMEM;
    }
    strcpy(file->name, "shm");
    file->type = FILE_TYPE_SHM;
    file->flags = O_RDWR;
    file->f_op = &shm_file_operations;
    file->private_data = obj;
    inode->mode = S_IFREG | 0600;
    inode->size = obj->size;
    inode->f_op = &shm_file_operations;
    file->inode = inode;
    fd = vfs_install_file(task, file, 0u);
    if (fd < 0)
        close_file(file);
    return fd;
}

static vaddr_t shm_find_free_vaddr(vm_space_t *vm, size_t size)
{
    size_t aligned_size = ALIGN_UP(size, PAGE_SIZE);

    if (aligned_size == 0 ||
        aligned_size > USER_SHM_END - USER_SHM_START)
        return 0;
    for (vaddr_t addr = USER_SHM_START;
         addr <= USER_SHM_END - aligned_size; addr += PAGE_SIZE) {
        bool overlaps = false;
        for (vma_t *vma = vm->vma_list; vma; vma = vma->next) {
            if (addr < vma->end && addr + aligned_size > vma->start) {
                overlaps = true;
                addr = ALIGN_UP(vma->end, PAGE_SIZE) - PAGE_SIZE;
                break;
            }
        }
        if (!overlaps)
            return addr;
    }

    return 0;
}

int sys_shm_open(const char *user_name, size_t size, int flags)
{
    char name[SHM_NAME_MAX];
    shm_object_t *obj;
    int result;

    if (!user_name)
        return -EINVAL;
    if (strncpy_from_user(name, user_name, sizeof(name)) < 0)
        return -EFAULT;
    if (name[0] == '\0')
        return -EINVAL;
    if (flags & ~(SHM_O_CREAT | SHM_O_EXCL))
        return -EINVAL;

    if (size > SHM_GLOBAL_MAX_BYTES)
        return -EFBIG;
    size = ALIGN_UP(size, PAGE_SIZE);

    spin_lock(&shm_lock);

    obj = shm_find_by_name(name);
    if (obj) {
        if ((flags & SHM_O_CREAT) && (flags & SHM_O_EXCL)) {
            spin_unlock(&shm_lock);
            return -EEXIST;
        }
        if (size > obj->size) {
            spin_unlock(&shm_lock);
            return -EINVAL;
        }
        obj->handles++;
        spin_unlock(&shm_lock);
        return shm_install_handle(obj);
    }

    if (!(flags & SHM_O_CREAT)) {
        spin_unlock(&shm_lock);
        return -ENOENT;
    }

    obj = shm_alloc_slot();
    if (!obj) {
        spin_unlock(&shm_lock);
        return -ENFILE;
    }

    memset(obj, 0, sizeof(*obj));
    obj->used = true;
    obj->id = shm_next_id++;
    if (shm_next_id == 0)
        shm_next_id = 1;
    strncpy(obj->name, name, sizeof(obj->name) - 1);

    result = shm_resize_locked(obj, size);
    if (result < 0) {
        memset(obj, 0, sizeof(*obj));
        spin_unlock(&shm_lock);
        return result;
    }

    obj->handles = 1u;
    spin_unlock(&shm_lock);
    return shm_install_handle(obj);
}

int sys_shm_unlink(const char *user_name)
{
    char name[SHM_NAME_MAX];
    shm_object_t *obj;

    if (!user_name)
        return -EINVAL;
    if (strncpy_from_user(name, user_name, sizeof(name)) < 0)
        return -EFAULT;

    spin_lock(&shm_lock);
    obj = shm_find_by_name(name);
    if (!obj) {
        spin_unlock(&shm_lock);
        return -ENOENT;
    }

    obj->unlinked = true;
    obj->name[0] = '\0';
    shm_try_destroy_locked(obj);
    spin_unlock(&shm_lock);
    return 0;
}

void *shm_map_fd(int fd, void *addr, size_t requested_size,
                 uint32_t vma_flags)
{
    task_t *task = task_current_local();
    file_t *file;
    shm_object_t *obj;
    vm_space_t *vm;
    vaddr_t vaddr;
    vma_t *vma;
    size_t mapping_size;
    uint32_t mapping_pages;
    void *result = NULL;

    if (!task || !task->process || !task->process->vm)
        return (void *)-EINVAL;
    if (!(vma_flags & VMA_SHARED) ||
        !(vma_flags & (VMA_READ | VMA_WRITE)) ||
        (vma_flags & ~(VMA_READ | VMA_WRITE | VMA_SHARED)))
        return (void *)-EINVAL;
    vm = task->process->vm;

    file = vfs_get_file_from_fd(task, fd);
    if (!file || file->type != FILE_TYPE_SHM || !file->private_data) {
        close_file(file);
        return (void *)-EBADF;
    }
    obj = file->private_data;
    spin_lock(&shm_lock);
    if (!obj->used) {
        result = (void *)-ENOENT;
        goto out;
    }
    if (requested_size > obj->size) {
        result = (void *)-EINVAL;
        goto out;
    }
    mapping_size = requested_size != 0u ?
        ALIGN_UP(requested_size, PAGE_SIZE) : obj->size;
    if (mapping_size == 0 || mapping_size > obj->size) {
        result = (void *)-EINVAL;
        goto out;
    }
    mapping_pages = (uint32_t)(mapping_size / PAGE_SIZE);

    if (addr) {
        vaddr = (vaddr_t)addr;
        if ((vaddr & (PAGE_SIZE - 1)) || vaddr < USER_SHM_START ||
            mapping_size > USER_SHM_END - USER_SHM_START ||
            vaddr > USER_SHM_END - mapping_size) {
            result = (void *)-EINVAL;
            goto out;
        }
    } else {
        vaddr = shm_find_free_vaddr(vm, mapping_size);
        if (!vaddr) {
            result = (void *)-ENOMEM;
            goto out;
        }
    }

    vma = create_vma(vm, vaddr, mapping_size, vma_flags);
    if (!vma) {
        result = (void *)-ENOMEM;
        goto out;
    }
    vma->shm_id = obj->id;

    for (uint32_t i = 0; i < mapping_pages; i++) {
        if (page_ref_inc((void *)obj->phys[i]) < 0) {
            for (uint32_t j = 0; j < i; j++) {
                unmap_user_page(vm->pgdir, vaddr + j * PAGE_SIZE, vm->asid);
                free_page((void *)obj->phys[j]);
            }
            remove_vma(vm, vaddr, vaddr + mapping_size);
            result = (void *)-ENOMEM;
            goto out;
        }

        if (map_user_page(vm->pgdir, vaddr + i * PAGE_SIZE, obj->phys[i],
                          vma_flags, vm->asid) < 0) {
            free_page((void *)obj->phys[i]);
            for (uint32_t j = 0; j < i; j++) {
                unmap_user_page(vm->pgdir, vaddr + j * PAGE_SIZE, vm->asid);
                free_page((void *)obj->phys[j]);
            }
            remove_vma(vm, vaddr, vaddr + mapping_size);
            result = (void *)-ENOMEM;
            goto out;
        }
    }

    obj->mappings++;
    result = (void *)vaddr;

out:
    spin_unlock(&shm_lock);
    close_file(file);
    return result;
}

void *sys_shm_map(int fd, void *addr, int flags)
{
    uint32_t vma_flags = VMA_READ | VMA_SHARED;

    if (flags != SHM_RDONLY && flags != SHM_RDWR)
        return (void *)-EINVAL;
    if (flags == SHM_RDWR)
        vma_flags |= VMA_WRITE;
    return shm_map_fd(fd, addr, 0u, vma_flags);
}

int sys_shm_unmap(void *addr, size_t size)
{
    task_t *task = task_current_local();
    vm_space_t *vm;
    vma_t *vma;
    vaddr_t start = (vaddr_t)addr;
    vaddr_t end;

    if (!task || !task->process || !task->process->vm)
        return -EINVAL;
    if (!addr || (start & (PAGE_SIZE - 1)))
        return -EINVAL;

    vm = task->process->vm;
    size = ALIGN_UP(size, PAGE_SIZE);
    if (size == 0)
        return -EINVAL;
    if (start < USER_SHM_START || size > USER_SHM_END - USER_SHM_START ||
        start > USER_SHM_END - size)
        return -EINVAL;
    end = start + size;

    vma = find_vma(vm, start);
    if (!vma || !(vma->flags & VMA_SHARED) || vma->start != start || vma->end != end)
        return -EINVAL;

    for (vaddr_t vaddr = start; vaddr < end; vaddr += PAGE_SIZE) {
        paddr_t phys = get_physical_address(vm->pgdir, vaddr);
        if (phys && unmap_user_page(vm->pgdir, vaddr, vm->asid) == 0)
            free_page((void *)phys);
    }

    shm_release_mapping(vma->shm_id);
    return remove_vma(vm, start, end);
}

void shm_retain_mapping(uint32_t shm_id)
{
    spin_lock(&shm_lock);
    shm_object_t *obj = shm_find_by_id(shm_id);
    if (obj)
        obj->mappings++;
    spin_unlock(&shm_lock);
}

void shm_release_mapping(uint32_t shm_id)
{
    spin_lock(&shm_lock);
    shm_object_t *obj = shm_find_by_id(shm_id);
    if (obj) {
        if (obj->mappings > 0)
            obj->mappings--;
        shm_try_destroy_locked(obj);
    }
    spin_unlock(&shm_lock);
}
