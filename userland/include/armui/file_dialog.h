/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armui/file_dialog.h
 * Layer: Userland / public graphical API
 *
 * Responsibilities:
 * - Expose an engine-independent filesystem selection dialog.
 * - Keep directory traversal and selection state outside applications.
 * - Return accepted paths transactionally without exposing Nuklear types.
 */

#ifndef ARMOS_ARMUI_FILE_DIALOG_H
#define ARMOS_ARMUI_FILE_DIALOG_H

#include <stddef.h>

struct armui_context;
struct armui_file_dialog;

enum armui_file_dialog_result {
    ARMUI_FILE_DIALOG_NONE,
    ARMUI_FILE_DIALOG_ACCEPTED,
    ARMUI_FILE_DIALOG_CANCELLED
};

struct armui_file_dialog_config {
    const char *title;
    const char *initial_directory;
    const char *extension;
};

struct armui_file_dialog *armui_file_dialog_create(void);
void armui_file_dialog_destroy(struct armui_file_dialog *dialog);
int armui_file_dialog_open(
    struct armui_file_dialog *dialog,
    const struct armui_file_dialog_config *config);
int armui_file_dialog_is_open(const struct armui_file_dialog *dialog);
enum armui_file_dialog_result armui_file_dialog_draw(
    struct armui_file_dialog *dialog,
    struct armui_context *context,
    int target_width,
    int target_height,
    char *accepted_path,
    size_t accepted_path_capacity);

#endif
