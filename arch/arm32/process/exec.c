/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: arch/arm32/process/exec.c
 * Layer: ARM32 / executable ABI
 *
 * Responsibilities:
 * - Parse ARM ELF32 images into the common executable layout.
 * - Perform ARMv7 cache maintenance after loading user pages.
 *
 * Notes:
 * - VFS reads, VM mappings and process publication remain common code.
 */

#include <asm/arm.h>
#include <kernel/arch_exec.h>
#include <kernel/elf32.h>
#include <kernel/memory.h>
#include <kernel/string.h>

#define EM_ARM 40u

int arch_exec_parse_image(const void *image, size_t header_size,
                          size_t file_size,
                          exec_image_layout_t *layout)
{
    const uint8_t *bytes = image;
    const elf32_ehdr_t *header = image;
    uint32_t index;
    bool tls_seen = false;

    if (!image || !layout || header_size < sizeof(*header) ||
        header->e_ident[0] != 0x7f || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F' ||
        header->e_ident[4] != 1 || header->e_ident[5] != 1 ||
        header->e_type != 2 || header->e_machine != EM_ARM ||
        header->e_version != 1 ||
        header->e_phentsize != sizeof(elf32_phdr_t) ||
        header->e_phnum == 0 || header->e_phoff > header_size ||
        (size_t)header->e_phnum * sizeof(elf32_phdr_t) >
            header_size - header->e_phoff)
        return -1;

    memset(layout, 0, sizeof(*layout));
    layout->entry = header->e_entry;
    for (index = 0; index < header->e_phnum; index++) {
        const elf32_phdr_t *program =
            (const elf32_phdr_t *)(const void *)(
                bytes + header->e_phoff + index * sizeof(*program));
        exec_image_segment_t *segment;

        if (program->p_type == PT_TLS) {
            if (tls_seen || program->p_memsz == 0 ||
                program->p_filesz > program->p_memsz ||
                program->p_offset > file_size ||
                program->p_filesz > file_size - program->p_offset ||
                program->p_vaddr + program->p_memsz < program->p_vaddr ||
                program->p_vaddr + program->p_memsz > USER_SPACE_END ||
                (program->p_align > 1u &&
                 (program->p_align & (program->p_align - 1u)) != 0))
                return -1;
            tls_seen = true;
            layout->tls_image = program->p_vaddr;
            layout->tls_file_size = program->p_filesz;
            layout->tls_memory_size = program->p_memsz;
            layout->tls_alignment =
                program->p_align ? program->p_align : 1u;
            continue;
        }
        if (program->p_type != PT_LOAD)
            continue;
        if (layout->segment_count >= EXEC_IMAGE_MAX_SEGMENTS ||
            program->p_memsz == 0 || program->p_filesz > program->p_memsz ||
            program->p_offset > file_size ||
            program->p_filesz > file_size - program->p_offset ||
            program->p_vaddr + program->p_memsz < program->p_vaddr ||
            program->p_vaddr + program->p_memsz > USER_SPACE_END)
            return -1;

        segment = &layout->segments[layout->segment_count++];
        segment->file_offset = program->p_offset;
        segment->file_size = program->p_filesz;
        segment->memory_size = program->p_memsz;
        segment->virtual_address = program->p_vaddr;
        if (program->p_flags & PF_R)
            segment->flags |= VMA_READ;
        if (program->p_flags & PF_W)
            segment->flags |= VMA_WRITE;
        if (program->p_flags & PF_X)
            segment->flags |= VMA_EXEC;
    }
    return layout->segment_count ? 0 : -1;
}

void arch_sync_loaded_user_page(vaddr_t mapped_vaddr, size_t size,
                                bool executable)
{
    clean_dcache_by_mva((void *)(uintptr_t)mapped_vaddr, size);
    if (executable)
        sync_icache_for_exec();
}
