/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/memory.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Declare kernel types, constants, and subsystem contracts.
 * - Define generic VM identity with an opaque architecture-backend link.
 * - Keep cross-module ABI and structure expectations explicit.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_MEMORY_H
#define _KERNEL_MEMORY_H

#include <kernel/arch_mmu.h>
#include <kernel/linker.h>
#include <kernel/types.h>
#include <kernel/user_layout.h>

typedef struct {
    uint32_t* bitmap;
    uint32_t total_pages;
    uint32_t free_pages;
    paddr_t start_addr;
    uint32_t bitmap_pages;  /* Nombre de pages utilisees par le bitmap */
    uint32_t pages_allocated;
    uint32_t pages_freed;
} __attribute__((aligned(4))) physical_allocator_t;

/* Virtual Memory Area */
typedef struct vma {
    vaddr_t start;
    vaddr_t end;
    uint32_t flags;
    uint32_t shm_id;
    struct vma* next;
} __attribute__((aligned(8))) vma_t;

typedef struct vm_space {
    pgdir_t pgdir;       /* User translation-root identity. */
    pgdir_t pgdir_alloc; /* Raw backend allocation identity, when distinct. */
    vma_t* vma_list;
    void* arch_private;  /* Opaque owning architecture backend, if any. */
    vaddr_t heap_start;
    vaddr_t heap_end;
    vaddr_t brk;
    vaddr_t stack_start;
    uint32_t asid;
} __attribute__((aligned(8))) vm_space_t;

/* Initialize architecture-neutral user layout policy for a new VM space. */
void vm_initialize_user_layout(vm_space_t *space);

/*
 * Anonymous mmap area.
 *
 * The historical ArmOS layout leaves 0x34000000..0x37000000 unused between
 * shared-memory mappings and the user stack. Keep mmap there so anonymous
 * mappings cannot collide with brk(), SHM, or stack growth.
 */
#define USER_MMAP_START USER_SHM_END
#define USER_MMAP_END   USER_STACK_BOTTOM

/*
 * Generic virtual-address split helpers.
 *
 * The active MMU backend supplies the boundary while generic VM code relies
 * only on its public meaning: user addresses are below it and kernel addresses
 * are above it, except for explicitly declared low kernel aliases.
 */
vaddr_t get_split_boundary(void);

static inline vaddr_t memory_low_kernel_alias_start(void)
{
    return KERNEL_START & 0xFFF00000u;
}

static inline vaddr_t memory_low_kernel_alias_end(void)
{
    vaddr_t end = (KERNEL_HEAP_END + 0xFFFFFu) & 0xFFF00000u;
    vaddr_t split = get_split_boundary();

    return end > split ? split : end;
}

static inline bool memory_range_overlaps_low_kernel_alias(vaddr_t start,
                                                          vaddr_t end_exclusive)
{
    if (KERNEL_START >= get_split_boundary() || end_exclusive <= start)
        return false;

    return start < memory_low_kernel_alias_end() &&
           end_exclusive > memory_low_kernel_alias_start();
}

static inline bool memory_is_low_kernel_alias(vaddr_t addr)
{
    return memory_range_overlaps_low_kernel_alias(addr, addr + 1);
}

static inline bool memory_is_user_address(vaddr_t addr)
{
    return addr < get_split_boundary() && !memory_is_low_kernel_alias(addr);
}

static inline bool memory_is_kernel_address(vaddr_t addr)
{
    return addr >= get_split_boundary() || memory_is_low_kernel_alias(addr);
}

/* VMA flags */
#define VMA_READ    (1 << 0)
#define VMA_WRITE   (1 << 1)
#define VMA_EXEC    (1 << 2)
#define VMA_KERNEL  (1 << 3)
#define VMA_SHARED  (1 << 4)
#define VMA_LAZY    (1 << 5)
#define VMA_DONTFORK (1 << 6)
#define VMA_EXTERNAL (1 << 7)

/* The active MMU backend owns concrete ASID width and reserved slots. */
#define ASID_KERNEL     ARCH_ASID_KERNEL
#define ASID_MIN_USER   ARCH_ASID_MIN_USER
#define ASID_MAX        ARCH_ASID_MAX

extern uint32_t kernel_memory_size;

#define MAX_ORDER 10  // max block size = 2^10 * PAGE_SIZE = 4 MB
#define BUDDY_BASE 0x54010000  // base de l'arène mémoire du buddy allocator
#define BUDDY_SIZE (1 << MAX_ORDER) * PAGE_SIZE

typedef struct buddy_block {
    struct buddy_block* next;
} buddy_block_t;

struct page_info {
    uint8_t used     : 1;  // page allouée ou libre
    size_t size        ;  // buddy order (si utilisé)
    uint8_t reserved : 1;  // marquée réservée (ex: DTB)
    uint16_t refcount;     // references physiques (COW, mappings partages)
    paddr_t start;
}__attribute__((packed));

/* Memory management */
bool init_memory(void);
void* allocate_page(void);
void free_page(void* page_addr);
void* allocate_pages(uint32_t num_pages);
void free_pages(void* page_addr, uint32_t num_pages);
bool page_is_buddy_page(void* page_addr);
uint16_t page_ref_count(void* page_addr);
int page_ref_inc(void* page_addr);
uint16_t page_ref_dec(void* page_addr);
uint32_t get_kernel_memory_size(void);
uint32_t get_free_page_count(void);
uint32_t get_total_page_count(void);
uint32_t get_allocated_page_count(void);
uint32_t get_freed_page_count(void);


/* Architecture-neutral virtual-memory policy and backend contract. */
vm_space_t* create_vm_space(void);
void destroy_vm_space(vm_space_t* vm);
vm_space_t* fork_vm_space(vm_space_t* parent_vm);
void vm_release_vmas(vm_space_t* vm);
vma_t* create_vma(vm_space_t* vm, vaddr_t start, size_t size, uint32_t flags);
int remove_vma(vm_space_t* vm, vaddr_t start, vaddr_t end);
vaddr_t vm_find_free_range(vm_space_t* vm, vaddr_t hint, size_t size,
                           vaddr_t base, vaddr_t limit);
int vm_unmap_range(vm_space_t* vm, vaddr_t start, size_t size);
vma_t* find_vma(vm_space_t* vm, vaddr_t addr);
bool vm_validate_user_range(vm_space_t* vm, vaddr_t address,
                            size_t length, uint32_t required_flags);
uint32_t vm_virtual_kb(vm_space_t* vm);
uint32_t vm_resident_kb(vm_space_t* vm);
uint32_t vm_page_table_count(vm_space_t* vm);
int handle_cow_fault(vaddr_t fault_addr);
int handle_user_stack_fault(vaddr_t fault_addr);
int handle_lazy_anon_fault(vaddr_t fault_addr, bool is_write);

void switch_to_vm_space(vm_space_t *vm);

/* Architecture MMU operations consumed by generic VM and exec code. */
bool setup_mmu(void);
int map_user_page(pgdir_t pgdir, vaddr_t vaddr, paddr_t phys_addr, uint32_t vma_flags, uint32_t asid);
int map_user_page_readonly(pgdir_t pgdir, vaddr_t vaddr, paddr_t phys_addr, uint32_t vma_flags, uint32_t asid);
int remap_user_page(pgdir_t pgdir, vaddr_t vaddr, paddr_t phys_addr, uint32_t vma_flags, uint32_t asid);
int unmap_user_page(pgdir_t pgdir, vaddr_t vaddr, uint32_t asid);
int set_user_page_readonly(pgdir_t pgdir, vaddr_t vaddr, uint32_t asid);
int set_user_page_writable(pgdir_t pgdir, vaddr_t vaddr, uint32_t asid);
paddr_t get_physical_address(pgdir_t pgdir, vaddr_t vaddr);

/* Temporary mapping helpers */
vaddr_t map_temp_page(paddr_t phys_addr);
void unmap_temp_page(void* temp_vaddr);

/* Kernel malloc */
bool init_kernel_heap(void);
bool init_kernel_heap_region(void* base, size_t size);
void* kmalloc(size_t size);
void kfree(void* ptr);
void* kcalloc(size_t nmemb, size_t size);
void* krealloc(void* ptr, size_t size);
void* kzalloc(size_t size);

/* Memory info */
uint32_t get_free_page_count(void);
uint32_t get_total_page_count(void);
uint32_t get_allocated_page_count(void);
uint32_t get_freed_page_count(void);

/* ELF helper functions */
void zero_fill_bss(vm_space_t* vm, vaddr_t vaddr, uint32_t size);

/* Memory detection */
uint32_t detect_memory(void);

/* Variable globale DTB */
extern uintptr_t dtb_address;

void kheap_stats(void);

void dump_kernel_stack(int depth);
void debug_kernel_stack_integrity(const char* location);
void check_memory_corruption(void);
void setup_svc_stack(void);

void hexdump(const void* addr, size_t n) ;

#endif /* _KERNEL_MEMORY_H */
