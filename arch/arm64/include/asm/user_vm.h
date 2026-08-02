/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/include/asm/user_vm.h
 * Layer: ARM64 / user address spaces
 *
 * Responsibilities:
 * - Own user translation tables, resident-page metadata and ASID identity.
 * - Grow translation-table inventories according to mapped address ranges.
 * - Expose that identity through the generic vm_space_t contract.
 * - Track mapping generations for safe resident-ASID activation.
 *
 * Notes:
 * - ASID residency is tracked independently for each participating CPU.
 * - High-half entry must rebind the opaque backend pointer before low-map
 *   retirement because the object is created through its identity alias.
 */

#ifndef ASM_ARM64_USER_VM_H
#define ASM_ARM64_USER_VM_H

#include <asm/mmu.h>
#include <kernel/memory.h>
#include <kernel/types.h>

#define ARM64_USER_VM_MAGIC 0x41564D36u

typedef struct {
    vaddr_t virtual_address;
    paddr_t physical_address;
    uint32_t flags;
} arm64_user_vm_mapping_t;

typedef struct {
    uint32_t l1_index;
    paddr_t table;
} arm64_user_vm_l2_table_t;

typedef struct {
    uint32_t l1_index;
    uint32_t l2_index;
    paddr_t table;
} arm64_user_vm_l3_table_t;

typedef struct arm64_user_vm {
    vm_space_t space;
    uint32_t magic;
    paddr_t l1;
    unsigned int asid;
    unsigned int mapping_count;
    unsigned int mapping_capacity;
    unsigned int l2_table_count;
    unsigned int l2_table_capacity;
    unsigned int l3_table_count;
    unsigned int l3_table_capacity;
    uint32_t tlb_generation;
    arm64_user_vm_l2_table_t *l2_tables;
    arm64_user_vm_l3_table_t *l3_tables;
    arm64_user_vm_mapping_t *mappings;
    struct arm64_user_vm *registry_next;
} arm64_user_vm_t;

const vm_space_t *arm64_user_vm_space(const arm64_user_vm_t *vm);
const arm64_user_vm_t *arm64_user_vm_from_space(const vm_space_t *space);
int arm64_user_vm_validate_identity(const arm64_user_vm_t *vm);
int arm64_user_vm_rebind_space(arm64_user_vm_t *vm);
int arm64_user_vm_activate_space(const vm_space_t *space);
int arm64_user_vm_activate_identity(paddr_t table, uint32_t asid,
                                    uint32_t *active_asid);
void arm64_user_vm_deactivate_cpu(void);

int arm64_user_vm_init(arm64_user_vm_t *vm);
int arm64_user_vm_map_new_page(arm64_user_vm_t *vm,
                               vaddr_t virtual_address,
                               unsigned int flags,
                               paddr_t *physical_address);
int arm64_user_vm_map_page(arm64_user_vm_t *vm,
                           vaddr_t virtual_address,
                           paddr_t physical_address,
                           unsigned int flags);
int arm64_user_vm_protect_page(arm64_user_vm_t *vm,
                               vaddr_t virtual_address,
                               unsigned int flags);
int arm64_user_vm_unmap_page(arm64_user_vm_t *vm,
                             vaddr_t virtual_address);
int arm64_user_vm_activate(arm64_user_vm_t *vm);
int arm64_user_vm_lookup(const arm64_user_vm_t *vm,
                         vaddr_t virtual_address,
                         paddr_t *physical_address,
                         unsigned int *flags);
int arm64_user_vm_destroy(arm64_user_vm_t *vm);
uint64_t arm64_user_vm_tlb_flush_count(void);
uint64_t arm64_user_vm_tlb_preserve_count(void);

#endif
