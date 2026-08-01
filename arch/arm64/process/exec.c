/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm64/process/exec.c
 * Layer: ARM64 / executable ABI
 *
 * Responsibilities:
 * - Parse AArch64 ELF64 images into the common executable layout.
 * - Synchronize loaded user code with the AArch64 instruction cache.
 *
 * Notes:
 * - VFS reads, VM mappings and process publication remain common code.
 */

#include <asm/mmu.h>
#include <kernel/arch_exec.h>
#include <kernel/elf64.h>
#include <kernel/memory.h>
#include <kernel/string.h>

#define EM_AARCH64 183u

static bool add_overflows_u64(uint64_t left, uint64_t right)
{
    return left + right < left;
}

static const elf64_program_header_t *program_header(
    const uint8_t *image, const elf64_header_t *header, uint32_t index)
{
    return (const elf64_program_header_t *)(const void *)(
        image + header->phoff + (uint64_t)index * header->phentsize);
}

static int validate_image(const void *image, size_t header_size,
                          size_t file_size)
{
    const uint8_t *bytes = image;
    const elf64_header_t *header = image;
    uint32_t index;
    bool loadable = false;
    bool executable_entry = false;
    bool tls_seen = false;

    if (!image || header_size < sizeof(*header))
        return -1;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F' || bytes[4] != 2 || bytes[5] != 1 ||
        bytes[6] != 1 || header->type != ELF64_ET_EXEC ||
        header->machine != EM_AARCH64 || header->version != 1 ||
        header->ehsize != sizeof(*header) ||
        header->phentsize != sizeof(elf64_program_header_t) ||
        header->phnum == 0 || header->phoff < sizeof(*header) ||
        header->entry >= USER_SPACE_END ||
        add_overflows_u64(header->phoff,
                          (uint64_t)header->phnum * header->phentsize) ||
        header->phoff + (uint64_t)header->phnum * header->phentsize >
            header_size)
        return -1;

    for (index = 0; index < header->phnum; index++) {
        const elf64_program_header_t *segment =
            program_header(bytes, header, index);

        if (segment->type == ELF64_PT_DYNAMIC ||
            segment->type == ELF64_PT_INTERP)
            return -1;
        if (segment->type == ELF64_PT_TLS) {
            if (tls_seen || segment->memsz == 0 ||
                segment->filesz > segment->memsz ||
                add_overflows_u64(segment->offset, segment->filesz) ||
                segment->offset + segment->filesz > file_size ||
                add_overflows_u64(segment->vaddr, segment->memsz) ||
                segment->vaddr < USER_SPACE_START ||
                segment->vaddr + segment->memsz > USER_SPACE_END ||
                (segment->align > 1 &&
                 (segment->align & (segment->align - 1)) != 0))
                return -1;
            tls_seen = true;
            continue;
        }
        if (segment->type != ELF64_PT_LOAD)
            continue;
        loadable = true;
        if (segment->memsz < segment->filesz || segment->memsz == 0 ||
            add_overflows_u64(segment->offset, segment->filesz) ||
            segment->offset + segment->filesz > file_size ||
            add_overflows_u64(segment->vaddr, segment->memsz) ||
            segment->vaddr < USER_SPACE_START ||
            segment->vaddr + segment->memsz > USER_SPACE_END ||
            (segment->align > 1 &&
             ((segment->align & (segment->align - 1)) != 0 ||
              (segment->vaddr & (segment->align - 1)) !=
                  (segment->offset & (segment->align - 1)))) ||
            (segment->flags &
             ~(ELF64_PF_R | ELF64_PF_W | ELF64_PF_X)) != 0 ||
            (segment->flags & ELF64_PF_R) == 0 ||
            (segment->flags & (ELF64_PF_W | ELF64_PF_X)) ==
                (ELF64_PF_W | ELF64_PF_X))
            return -1;
        if ((segment->flags & ELF64_PF_X) != 0 &&
            header->entry >= segment->vaddr &&
            header->entry < segment->vaddr + segment->memsz)
            executable_entry = true;
    }
    return loadable && executable_entry ? 0 : -1;
}

int arch_exec_parse_image(const void *image, size_t header_size,
                          size_t file_size,
                          exec_image_layout_t *layout)
{
    const uint8_t *bytes = image;
    const elf64_header_t *header = image;
    uint32_t index;

    if (!layout || validate_image(image, header_size, file_size) != 0)
        return -1;

    memset(layout, 0, sizeof(*layout));
    layout->entry = (vaddr_t)header->entry;
    for (index = 0; index < header->phnum; index++) {
        const elf64_program_header_t *program =
            program_header(bytes, header, index);
        exec_image_segment_t *segment;

        if (program->type == ELF64_PT_TLS) {
            layout->tls_image = (vaddr_t)program->vaddr;
            layout->tls_file_size = program->filesz;
            layout->tls_memory_size = program->memsz;
            layout->tls_alignment = program->align ? program->align : 1u;
            continue;
        }
        if (program->type != ELF64_PT_LOAD)
            continue;
        if (layout->segment_count >= EXEC_IMAGE_MAX_SEGMENTS)
            return -1;
        segment = &layout->segments[layout->segment_count++];
        segment->file_offset = program->offset;
        segment->file_size = program->filesz;
        segment->memory_size = program->memsz;
        segment->virtual_address = (vaddr_t)program->vaddr;
        if (program->flags & ELF64_PF_R)
            segment->flags |= VMA_READ;
        if (program->flags & ELF64_PF_W)
            segment->flags |= VMA_WRITE;
        if (program->flags & ELF64_PF_X)
            segment->flags |= VMA_EXEC;
    }
    return layout->segment_count ? 0 : -1;
}

void arch_sync_loaded_user_page(vaddr_t mapped_vaddr, size_t size,
                                bool executable)
{
    if (executable)
        arm64_mmu_sync_code(mapped_vaddr, size);
}
