/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/elf64.h
 * Layer: Kernel / executable ABI definitions
 *
 * Responsibilities:
 * - Define architecture-neutral ELF64 file and program-header layouts.
 * - Publish ELF constants shared by executable ABI adapters.
 *
 * Notes:
 * - Machine-specific validation belongs to the active architecture adapter.
 */

#ifndef _KERNEL_ELF64_H
#define _KERNEL_ELF64_H

#include <kernel/types.h>

#define ELF64_IDENT_SIZE 16u
#define ELF64_PT_LOAD    1u
#define ELF64_PT_DYNAMIC 2u
#define ELF64_PT_INTERP  3u
#define ELF64_PT_TLS     7u
#define ELF64_PF_X       1u
#define ELF64_PF_W       2u
#define ELF64_PF_R       4u
#define ELF64_ET_EXEC    2u

typedef struct elf64_header {
    uint8_t ident[ELF64_IDENT_SIZE];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} elf64_header_t;

typedef struct elf64_program_header {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} elf64_program_header_t;

#endif /* _KERNEL_ELF64_H */
