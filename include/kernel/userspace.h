/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: include/kernel/userspace.h
 * Layer: Kernel / public internal interface
 *
 * Responsibilities:
 * - Expose architecture-neutral user pointer validation and copy helpers.
 * - Keep syscall code from dereferencing user virtual addresses directly.
 *
 * Notes:
 * - Header changes can ripple across kernel and user ABI glue.
 */

#ifndef _KERNEL_USERSPACE_H_
#define _KERNEL_USERSPACE_H_

#include <kernel/types.h>
#include <kernel/memory.h>

bool is_valid_user_range(const void* ptr, size_t size);
bool is_kernel_pointer(const void* ptr);

int copy_to_user(void* to, const void* from, size_t n);

int copy_from_user(void* to, const void* from, size_t n);
int copy_from_user_2d(void *to, size_t to_stride,
                      const void *from, size_t from_stride,
                      size_t row_bytes, size_t rows);

int strncpy_from_user(char* to, const char* from, size_t max_len);
int copy_to_user_safe(void* to, const void* from, size_t n, size_t max_size);

bool is_valid_user_ptr(const void* ptr);

char* copy_string_from_user(const char* user_str);
void cleanup_exec_args(char* filename, char** argv, char** envp);

int strnlen_user(const char* str, int maxlen);
int setup_user_stack(vm_space_t* vm, char** argv, char** envp);

#endif /* _KERNEL_USERSPACE_H_ */
