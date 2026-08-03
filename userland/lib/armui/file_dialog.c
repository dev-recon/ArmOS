/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armui/file_dialog.c
 * Layer: Userland / graphical libraries
 *
 * Responsibilities:
 * - Navigate the filesystem through a reusable ArmUI file dialog.
 * - Filter regular files while keeping directory traversal available.
 * - Preserve selection state until explicit acceptance or cancellation.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <armui/armui.h>
#include <armui/file_dialog.h>

#define ARMUI_FILE_DIALOG_PATH_MAX 512
#define ARMUI_FILE_DIALOG_NAME_MAX 128
#define ARMUI_FILE_DIALOG_ENTRIES_MAX 128

struct armui_file_dialog_entry {
    char name[ARMUI_FILE_DIALOG_NAME_MAX];
    int directory;
};

struct armui_file_dialog {
    char title[64];
    char directory[ARMUI_FILE_DIALOG_PATH_MAX];
    char extension[16];
    char selected[ARMUI_FILE_DIALOG_NAME_MAX];
    char status[160];
    struct armui_file_dialog_entry entries[ARMUI_FILE_DIALOG_ENTRIES_MAX];
    size_t entry_count;
    int open;
};

static int copy_string(char *target, size_t capacity, const char *source)
{
    size_t length = source ? strlen(source) : 0u;

    if (!target || capacity == 0u || length >= capacity)
        return -1;
    memcpy(target, source, length + 1u);
    return 0;
}

static int join_path(char *target, size_t capacity,
                     const char *directory, const char *name)
{
    int length;

    if (strcmp(directory, "/") == 0)
        length = snprintf(target, capacity, "/%s", name);
    else
        length = snprintf(target, capacity, "%s/%s", directory, name);

    return length >= 0 && (size_t)length < capacity ? 0 : -1;
}

static int has_extension(const char *name, const char *extension)
{
    size_t name_length;
    size_t extension_length;

    if (!extension || extension[0] == '\0')
        return 1;
    name_length = strlen(name);
    extension_length = strlen(extension);
    return name_length >= extension_length &&
        strcmp(name + name_length - extension_length, extension) == 0;
}

static void sort_entries(struct armui_file_dialog *dialog)
{
    for (size_t index = 1u; index < dialog->entry_count; index++) {
        struct armui_file_dialog_entry entry = dialog->entries[index];
        size_t position = index;

        while (position > 0u) {
            const struct armui_file_dialog_entry *previous =
                &dialog->entries[position - 1u];
            int after = previous->directory < entry.directory ||
                (previous->directory == entry.directory &&
                 strcmp(previous->name, entry.name) > 0);

            if (!after)
                break;
            dialog->entries[position] = *previous;
            position--;
        }
        dialog->entries[position] = entry;
    }
}

static int refresh_directory(struct armui_file_dialog *dialog)
{
    DIR *directory = opendir(dialog->directory);
    struct dirent *entry;

    dialog->entry_count = 0u;
    if (!directory) {
        (void)snprintf(dialog->status, sizeof(dialog->status),
                       "Ouverture impossible: %s", strerror(errno));
        return -1;
    }
    while ((entry = readdir(directory)) != NULL &&
           dialog->entry_count < ARMUI_FILE_DIALOG_ENTRIES_MAX) {
        struct armui_file_dialog_entry *destination;
        struct stat information;
        char path[ARMUI_FILE_DIALOG_PATH_MAX];
        size_t length;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        length = strlen(entry->d_name);
        if (length >= ARMUI_FILE_DIALOG_NAME_MAX ||
            join_path(path, sizeof(path), dialog->directory,
                      entry->d_name) < 0 || stat(path, &information) < 0)
            continue;
        if (!S_ISDIR(information.st_mode) &&
            (!S_ISREG(information.st_mode) ||
             !has_extension(entry->d_name, dialog->extension)))
            continue;
        destination = &dialog->entries[dialog->entry_count++];
        memcpy(destination->name, entry->d_name, length + 1u);
        destination->directory = S_ISDIR(information.st_mode);
    }
    closedir(directory);
    sort_entries(dialog);
    (void)snprintf(dialog->status, sizeof(dialog->status),
                   "%u entree(s)", (unsigned int)dialog->entry_count);
    return 0;
}

static void parent_directory(struct armui_file_dialog *dialog)
{
    char *separator;

    if (strcmp(dialog->directory, "/") == 0)
        return;
    separator = strrchr(dialog->directory, '/');
    if (!separator)
        return;
    if (separator == dialog->directory)
        separator[1] = '\0';
    else
        *separator = '\0';
    dialog->selected[0] = '\0';
    (void)refresh_directory(dialog);
}

static int enter_directory(struct armui_file_dialog *dialog,
                           const char *name)
{
    char path[ARMUI_FILE_DIALOG_PATH_MAX];

    if (join_path(path, sizeof(path), dialog->directory, name) < 0 ||
        copy_string(dialog->directory, sizeof(dialog->directory), path) < 0)
        return -1;
    dialog->selected[0] = '\0';
    return refresh_directory(dialog);
}

struct armui_file_dialog *armui_file_dialog_create(void)
{
    return calloc(1u, sizeof(struct armui_file_dialog));
}

void armui_file_dialog_destroy(struct armui_file_dialog *dialog)
{
    free(dialog);
}

int armui_file_dialog_open(
    struct armui_file_dialog *dialog,
    const struct armui_file_dialog_config *config)
{
    const char *initial;
    const char *home;
    char expanded[ARMUI_FILE_DIALOG_PATH_MAX];

    if (!dialog || !config)
        return -1;
    initial = config->initial_directory ? config->initial_directory : "/";
    home = getenv("HOME");
    if (initial[0] == '~' && initial[1] == '/' && home) {
        int length = snprintf(expanded, sizeof(expanded), "%s/%s",
                              home, initial + 2);

        if (length < 0 || (size_t)length >= sizeof(expanded))
            return -1;
        initial = expanded;
    }
    if (copy_string(dialog->title, sizeof(dialog->title),
                    config->title ? config->title : "Ouvrir") < 0 ||
        copy_string(dialog->directory, sizeof(dialog->directory), initial) < 0 ||
        copy_string(dialog->extension, sizeof(dialog->extension),
                    config->extension ? config->extension : "") < 0)
        return -1;
    dialog->selected[0] = '\0';
    dialog->open = 1;
    if (refresh_directory(dialog) < 0 && home && strcmp(initial, home) != 0) {
        if (copy_string(dialog->directory, sizeof(dialog->directory), home) < 0)
            return -1;
        (void)refresh_directory(dialog);
    }
    return 0;
}

int armui_file_dialog_is_open(const struct armui_file_dialog *dialog)
{
    return dialog && dialog->open;
}

enum armui_file_dialog_result armui_file_dialog_draw(
    struct armui_file_dialog *dialog,
    struct armui_context *context,
    int target_width,
    int target_height,
    char *accepted_path,
    size_t accepted_path_capacity)
{
    float width;
    float height;
    int opened;
    int navigation = 0;

    if (!dialog || !dialog->open || !context)
        return ARMUI_FILE_DIALOG_NONE;
    width = target_width > 720 ? 680.0f : (float)(target_width - 24);
    height = target_height > 560 ? 520.0f : (float)(target_height - 24);
    if (width < 280.0f)
        width = 280.0f;
    if (height < 220.0f)
        height = 220.0f;
    opened = armui_window_begin(context, dialog->title,
        ((float)target_width - width) * 0.5f,
        ((float)target_height - height) * 0.5f,
        width, height, 1);
    if (opened) {
        armui_row(context, 24.0f, 1);
        armui_label(context, dialog->directory, ARMUI_ALIGN_LEFT);
        armui_row(context, 28.0f, 3);
        if (armui_button_label(context, "Dossier parent")) {
            parent_directory(dialog);
            navigation = 1;
        }
        if (armui_button_label(context, "Actualiser")) {
            (void)refresh_directory(dialog);
            navigation = 1;
        }
        armui_label(context, dialog->status, ARMUI_ALIGN_RIGHT);
        if (!navigation) {
            for (size_t index = 0u; index < dialog->entry_count; index++) {
                char label[ARMUI_FILE_DIALOG_NAME_MAX + 8u];
                const struct armui_file_dialog_entry *entry =
                    &dialog->entries[index];

                (void)snprintf(label, sizeof(label), "%s%s",
                               entry->directory ? "[D] " : "", entry->name);
                armui_row(context, 26.0f, 1);
                if (!armui_button_label(context, label))
                    continue;
                if (entry->directory) {
                    (void)enter_directory(dialog, entry->name);
                    navigation = 1;
                    break;
                }
                (void)copy_string(dialog->selected,
                                  sizeof(dialog->selected), entry->name);
            }
        }
        armui_row(context, 24.0f, 1);
        armui_label(context, dialog->selected[0] ? dialog->selected :
                    "Selectionnez un fichier", ARMUI_ALIGN_LEFT);
        armui_row(context, 30.0f, 2);
        armui_set_enabled(context, dialog->selected[0] != '\0');
        if (armui_button_label(context, "Ouvrir")) {
            if (join_path(accepted_path, accepted_path_capacity,
                          dialog->directory, dialog->selected) == 0) {
                dialog->open = 0;
                armui_set_enabled(context, 1);
                armui_window_end(context);
                return ARMUI_FILE_DIALOG_ACCEPTED;
            }
        }
        armui_set_enabled(context, 1);
        if (armui_button_label(context, "Annuler")) {
            dialog->open = 0;
            armui_window_end(context);
            return ARMUI_FILE_DIALOG_CANCELLED;
        }
    }
    armui_window_end(context);
    return ARMUI_FILE_DIALOG_NONE;
}
