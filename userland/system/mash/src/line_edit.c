/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/system/mash/src/line_edit.c
 * Layer: Userland / system service
 * Description: System-level userspace component for ArmOS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../include/mash.h"
#include "../include/jobs.h"

#define SHELL_HISTORY_SIZE  128
#define SHELL_COMPLETION_MAX 64
#define SHELL_HISTORY_BASENAME ".mash_history"
#define SHELL_COMMAND_CACHE_MAX 128
#define SHELL_COMMAND_CACHE_NAME_MAX 80

static char shell_history[SHELL_HISTORY_SIZE][SHELL_BUFFER_SIZE];
static int shell_history_count = 0;
static char completion_matches[SHELL_COMPLETION_MAX][80];
static char command_cache[SHELL_COMMAND_CACHE_MAX][SHELL_COMMAND_CACHE_NAME_MAX];
static int command_cache_count = 0;
static int command_cache_valid = 0;
static char command_cache_path[SHELL_BUFFER_SIZE];
static int shell_line_eof = 0;

static void shell_history_path(char* path, size_t path_size) {
    const char* home = getenv("HOME");

    if (!path || path_size == 0)
        return;

    if (!home || !*home)
        home = "/home/user";

    snprintf(path, path_size, "%s/%s", home, SHELL_HISTORY_BASENAME);
}

static int le_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static char* le_trim_spaces(char* s) {
    char* end;

    while (*s == ' ' || *s == '\t')
        s++;

    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }

    return s;
}

static int le_token_has_slash(const char* s) {
    while (*s) {
        if (*s == '/')
            return 1;
        s++;
    }
    return 0;
}

static char* le_strrchr(const char* s, int c) {
    char* last = NULL;

    while (*s) {
        if (*s == (char)c)
            last = (char*)s;
        s++;
    }
    return last;
}

static void le_memmove(char* dst, const char* src, size_t n) {
    size_t i;

    if (dst < src) {
        for (i = 0; i < n; i++)
            dst[i] = src[i];
    } else if (dst > src) {
        while (n-- > 0)
            dst[n] = src[n];
    }
}

static void shell_history_add(const char* line) {
    int i;

    if (!line || !*line)
        return;

    if (shell_history_count > 0 &&
        strcmp(shell_history[shell_history_count - 1], line) == 0) {
        return;
    }

    if (shell_history_count == SHELL_HISTORY_SIZE) {
        for (i = 1; i < SHELL_HISTORY_SIZE; i++)
            strcpy(shell_history[i - 1], shell_history[i]);
        shell_history_count--;
    }

    strncpy(shell_history[shell_history_count], line, SHELL_BUFFER_SIZE - 1);
    shell_history[shell_history_count][SHELL_BUFFER_SIZE - 1] = '\0';
    shell_history_count++;
}

static void shell_history_save(void) {
    char path[160];
    int fd;
    int i;

    shell_history_path(path, sizeof(path));
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        return;

    for (i = 0; i < shell_history_count; i++) {
        write(fd, shell_history[i], strlen(shell_history[i]));
        write(fd, "\n", 1);
    }

    close(fd);
}

static void shell_history_load_line(char* line) {
    char* trimmed = le_trim_spaces(line);

    if (*trimmed)
        shell_history_add(trimmed);
}

static void shell_history_load(void) {
    char path[160];
    char read_buf[256];
    char line[SHELL_BUFFER_SIZE];
    int line_len = 0;
    int fd;
    int n;
    int i;

    shell_history_count = 0;

    shell_history_path(path, sizeof(path));
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    while ((n = read(fd, read_buf, sizeof(read_buf))) > 0) {
        for (i = 0; i < n; i++) {
            char c = read_buf[i];

            if (c == '\n' || c == '\r') {
                line[line_len] = '\0';
                shell_history_load_line(line);
                line_len = 0;
                continue;
            }

            if (line_len < SHELL_BUFFER_SIZE - 1)
                line[line_len++] = c;
        }
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        shell_history_load_line(line);
    }

    close(fd);
}

static void shell_cursor_left(int count) {
    while (count-- > 0) {
        putc_tty('\033');
        putc_tty('[');
        putc_tty('D');
    }
}

static void shell_cursor_right(int count) {
    while (count-- > 0) {
        putc_tty('\033');
        putc_tty('[');
        putc_tty('C');
    }
}

static int shell_utf8_is_continuation(unsigned char c) {
    return (c & 0xc0u) == 0x80u;
}

static int shell_utf8_previous(const char* line, int cursor) {
    if (cursor <= 0)
        return 0;
    cursor--;
    while (cursor > 0 &&
           shell_utf8_is_continuation((unsigned char)line[cursor]))
        cursor--;
    return cursor;
}

static int shell_utf8_next(const char* line, int len, int cursor) {
    if (cursor >= len)
        return len;
    cursor++;
    while (cursor < len &&
           shell_utf8_is_continuation((unsigned char)line[cursor]))
        cursor++;
    return cursor;
}

static int shell_utf8_columns(const char* line, int begin, int end) {
    int columns = 0;
    int offset;

    for (offset = begin; offset < end; offset++) {
        if (!shell_utf8_is_continuation((unsigned char)line[offset]))
            columns++;
    }
    return columns;
}

static void shell_redraw_from(char* line, int len, int cursor) {
    int i;

    for (i = cursor; i < len; i++)
        putc_tty(line[i]);
    putc_tty(' ');
    shell_cursor_left(shell_utf8_columns(line, cursor, len) + 1);
}

static void shell_redraw_line(char* line, int len, int cursor) {
    int i;

    putc_tty('\r');
    putc_tty('\033');
    putc_tty('[');
    putc_tty('K');
    shell_print_prompt();
    for (i = 0; i < len; i++)
        putc_tty(line[i]);
    shell_cursor_left(shell_utf8_columns(line, cursor, len));
}

static void shell_set_line(char* line, int* len, int* cursor, const char* text) {
    strncpy(line, text, SHELL_BUFFER_SIZE - 1);
    line[SHELL_BUFFER_SIZE - 1] = '\0';
    *len = strlen(line);
    *cursor = *len;
    shell_redraw_line(line, *len, *cursor);
}

static void shell_reap_jobs_during_edit(char* line, int len, int cursor) {
    /*
     * Background job notifications can arrive while the user is editing a
     * command. Put the notification on its own clean line, then repaint the
     * prompt and the edit buffer so typed text is not visually lost.
     */
    putc_tty('\r');
    putc_tty('\033');
    putc_tty('[');
    putc_tty('K');

    jobs_reap_background();
    shell_redraw_line(line, len, cursor);
}

static void shell_insert_bytes(char* line, int* len, int* cursor,
                               const char* bytes, int byte_count) {
    int i;

    if (byte_count <= 0 || *len + byte_count >= SHELL_BUFFER_SIZE)
        return;

    for (i = *len; i >= *cursor; i--)
        line[i + byte_count] = line[i];

    for (i = 0; i < byte_count; i++)
        line[*cursor + i] = bytes[i];
    *len += byte_count;
    *cursor += byte_count;

    for (i = *cursor - byte_count; i < *len; i++)
        putc_tty(line[i]);
    shell_cursor_left(shell_utf8_columns(line, *cursor, *len));
}

static void shell_backspace_char(char* line, int* len, int* cursor) {
    int previous;
    int removed;
    int i;

    if (*cursor <= 0)
        return;

    previous = shell_utf8_previous(line, *cursor);
    removed = *cursor - previous;
    for (i = previous; i <= *len - removed; i++)
        line[i] = line[i + removed];

    *cursor = previous;
    *len -= removed;
    shell_cursor_left(1);
    shell_redraw_from(line, *len, *cursor);
}

static void shell_delete_char(char* line, int* len, int cursor) {
    int next;
    int removed;
    int i;

    if (cursor >= *len)
        return;

    next = shell_utf8_next(line, *len, cursor);
    removed = next - cursor;
    for (i = cursor; i <= *len - removed; i++)
        line[i] = line[i + removed];

    *len -= removed;
    shell_redraw_from(line, *len, cursor);
}

static int shell_read_utf8(int first, char bytes[4]) {
    unsigned char lead = (unsigned char)first;
    int expected;
    int index;

    if (lead < 0x80u)
        expected = 1;
    else if (lead >= 0xc2u && lead <= 0xdfu)
        expected = 2;
    else if (lead >= 0xe0u && lead <= 0xefu)
        expected = 3;
    else if (lead >= 0xf0u && lead <= 0xf4u)
        expected = 4;
    else
        return 0;

    bytes[0] = (char)lead;
    for (index = 1; index < expected; index++) {
        int c = getc_tty();

        if (c < 0 ||
            !shell_utf8_is_continuation((unsigned char)c))
            return 0;
        bytes[index] = (char)c;
    }
    return expected;
}

typedef struct completion_result {
    int count;
    int single_is_dir;
    char common[SHELL_BUFFER_SIZE];
} completion_result_t;

static void completion_lcp(char* common, const char* value) {
    int i = 0;

    while (common[i] && value[i] && common[i] == value[i])
        i++;
    common[i] = '\0';
}

static int completion_display_exists(completion_result_t* result,
                                     const char* display) {
    int i;
    int count = result->count;

    if (count > SHELL_COMPLETION_MAX)
        count = SHELL_COMPLETION_MAX;

    for (i = 0; i < count; i++) {
        if (strcmp(completion_matches[i], display) == 0)
            return 1;
    }

    return 0;
}

static void completion_add(completion_result_t* result, const char* replacement,
                           const char* display, int is_dir) {
    if (completion_display_exists(result, display))
        return;

    if (result->count == 0) {
        strncpy(result->common, replacement, sizeof(result->common) - 1);
        result->common[sizeof(result->common) - 1] = '\0';
        result->single_is_dir = is_dir;
    } else {
        completion_lcp(result->common, replacement);
        result->single_is_dir = 0;
    }

    if (result->count < SHELL_COMPLETION_MAX) {
        strncpy(completion_matches[result->count], display,
                sizeof(completion_matches[0]) - 1);
        completion_matches[result->count][sizeof(completion_matches[0]) - 1] = '\0';
    }
    result->count++;
}

static int shell_path_is_dir(const char* path) {
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int command_cache_contains(const char* name) {
    int i;

    for (i = 0; i < command_cache_count; i++) {
        if (strcmp(command_cache[i], name) == 0)
            return 1;
    }

    return 0;
}

static void command_cache_add(const char* name) {
    if (!name || !*name || command_cache_count >= SHELL_COMMAND_CACHE_MAX)
        return;

    if (command_cache_contains(name))
        return;

    strncpy(command_cache[command_cache_count], name,
            sizeof(command_cache[0]) - 1);
    command_cache[command_cache_count][sizeof(command_cache[0]) - 1] = '\0';
    command_cache_count++;
}

static void command_cache_scan_dir(const char* dir) {
    char buf[4096];
    int fd;
    int n;

    fd = open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int pos = 0;

        while (pos < n) {
            struct linux_dirent* entry = (struct linux_dirent*)(buf + pos);
            const char* name = entry->d_name;

            if (entry->d_reclen == 0)
                break;

            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                name[0] != '.') {
                command_cache_add(name);
            }

            pos += entry->d_reclen;
        }
    }

    close(fd);
}

static void command_cache_rebuild_if_needed(void) {
    const char* path = shell_getenv("PATH");
    const char* entry;
    int i;

    if (!path || !*path)
        path = "/bin:/usr/bin:/opt/kilo/bin";

    if (command_cache_valid && strcmp(command_cache_path, path) == 0)
        return;

    command_cache_count = 0;
    strncpy(command_cache_path, path, sizeof(command_cache_path) - 1);
    command_cache_path[sizeof(command_cache_path) - 1] = '\0';

    for (i = 0; i < command_count_registered(); i++) {
        command_cache_add(command_name_at(i));
    }

    entry = path;
    while (*entry) {
        const char* next = strchr(entry, ':');
        int len = next ? (int)(next - entry) : (int)strlen(entry);
        char dir[SHELL_BUFFER_SIZE];

        if (len <= 0) {
            strcpy(dir, ".");
        } else if (len < SHELL_BUFFER_SIZE) {
            strncpy(dir, entry, len);
            dir[len] = '\0';
        } else {
            dir[0] = '\0';
        }

        if (dir[0])
            command_cache_scan_dir(dir);

        if (!next)
            break;
        entry = next + 1;
    }

    command_cache_valid = 1;
}

static int shell_token_start(const char* line, int cursor) {
    int pos = cursor;

    while (pos > 0 &&
           line[pos - 1] != ' ' &&
           line[pos - 1] != '\t' &&
           line[pos - 1] != '<' &&
           line[pos - 1] != '>' &&
           line[pos - 1] != '|' &&
           line[pos - 1] != '&' &&
           line[pos - 1] != ';') {
        pos--;
    }
    return pos;
}

static int shell_is_command_position(const char* line, int start) {
    int i = start - 1;

    while (i >= 0 && (line[i] == ' ' || line[i] == '\t'))
        i--;

    return i < 0 || line[i] == '|' || line[i] == '&' || line[i] == ';';
}

static void shell_split_path_prefix(const char* token, char* dir,
                                    char* typed_prefix, char* partial) {
    const char* slash = le_strrchr(token, '/');
    int prefix_len;

    if (!slash) {
        strcpy(dir, ".");
        typed_prefix[0] = '\0';
        strncpy(partial, token, SHELL_BUFFER_SIZE - 1);
        partial[SHELL_BUFFER_SIZE - 1] = '\0';
        return;
    }

    prefix_len = (int)(slash - token) + 1;
    strncpy(typed_prefix, token, prefix_len);
    typed_prefix[prefix_len] = '\0';
    strncpy(partial, slash + 1, SHELL_BUFFER_SIZE - 1);
    partial[SHELL_BUFFER_SIZE - 1] = '\0';

    if (prefix_len == 1 && token[0] == '/') {
        strcpy(dir, "/");
    } else {
        strncpy(dir, token, prefix_len - 1);
        dir[prefix_len - 1] = '\0';
    }
}

static void shell_scan_dir_matches(const char* dir, const char* partial,
                                   const char* typed_prefix,
                                   completion_result_t* result,
                                   int command_mode) {
    char buf[1024];
    char replacement[SHELL_BUFFER_SIZE];
    char stat_path[SHELL_BUFFER_SIZE];
    int fd;
    int n;

    fd = open(dir, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return;

    while ((n = getdents(fd, buf, sizeof(buf))) > 0) {
        int pos = 0;

        while (pos < n) {
            struct linux_dirent* entry = (struct linux_dirent*)(buf + pos);
            const char* name = entry->d_name;
            int is_dir = 0;

            if (entry->d_reclen == 0)
                break;

            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                le_starts_with(name, partial) &&
                (*partial == '.' || name[0] != '.')) {
                snprintf(replacement, sizeof(replacement), "%s%s", typed_prefix, name);

                if (!command_mode) {
                    if (strcmp(dir, ".") == 0)
                        snprintf(stat_path, sizeof(stat_path), "%s", replacement);
                    else if (strcmp(dir, "/") == 0)
                        snprintf(stat_path, sizeof(stat_path), "/%s", name);
                    else
                        snprintf(stat_path, sizeof(stat_path), "%s/%s", dir, name);
                    is_dir = shell_path_is_dir(stat_path);
                }

                completion_add(result, replacement, name, is_dir);
            }

            pos += entry->d_reclen;
        }
    }

    close(fd);
}

static void shell_scan_path_commands(const char* partial,
                                     completion_result_t* result) {
    int i;

    command_cache_rebuild_if_needed();

    for (i = 0; i < command_cache_count; i++) {
        const char* name = command_cache[i];
        if (name && le_starts_with(name, partial))
            completion_add(result, name, name, 0);
    }
}

static void shell_replace_token_prefix(char* line, int* len, int* cursor,
                                       int start, const char* replacement) {
    int old_len = *cursor - start;
    int repl_len = strlen(replacement);
    int tail_len = *len - *cursor;
    int new_len = *len - old_len + repl_len;

    if (new_len >= SHELL_BUFFER_SIZE)
        return;

    le_memmove(line + start + repl_len, line + *cursor, tail_len + 1);
    memcpy(line + start, replacement, repl_len);
    *len = new_len;
    *cursor = start + repl_len;
    shell_redraw_line(line, *len, *cursor);
}

static void shell_show_completion_matches(char* line, int len, int cursor,
                                          completion_result_t* result) {
    int i;
    int shown = result->count;

    if (shown > SHELL_COMPLETION_MAX)
        shown = SHELL_COMPLETION_MAX;

    printf("\n");
    for (i = 0; i < shown; i++)
        printf("%s  ", completion_matches[i]);
    if (result->count > SHELL_COMPLETION_MAX)
        printf("...");
    printf("\n");
    shell_redraw_line(line, len, cursor);
}

static void shell_complete_line(char* line, int* len, int* cursor) {
    completion_result_t result;
    char token[SHELL_BUFFER_SIZE];
    char dir[SHELL_BUFFER_SIZE];
    char typed_prefix[SHELL_BUFFER_SIZE];
    char partial[SHELL_BUFFER_SIZE];
    char replacement[SHELL_BUFFER_SIZE];
    int start = shell_token_start(line, *cursor);
    int token_len = *cursor - start;
    int command_pos = shell_is_command_position(line, start);

    if (token_len < 0 || token_len >= SHELL_BUFFER_SIZE)
        return;

    memcpy(token, line + start, token_len);
    token[token_len] = '\0';

    memset(&result, 0, sizeof(result));

    if (command_pos && !le_token_has_slash(token)) {
        shell_scan_path_commands(token, &result);
    } else {
        shell_split_path_prefix(token, dir, typed_prefix, partial);
        shell_scan_dir_matches(dir, partial, typed_prefix, &result, 0);
    }

    if (result.count == 0) {
        putc_tty('\a');
        return;
    }

    if (result.count == 1) {
        strncpy(replacement, result.common, sizeof(replacement) - 2);
        replacement[sizeof(replacement) - 2] = '\0';
        if (result.single_is_dir)
            strcat(replacement, "/");
        else
            strcat(replacement, " ");
        shell_replace_token_prefix(line, len, cursor, start, replacement);
        return;
    }

    if ((int)strlen(result.common) > token_len) {
        shell_replace_token_prefix(line, len, cursor, start, result.common);
        return;
    }

    shell_show_completion_matches(line, *len, *cursor, &result);
}

static void shell_handle_escape(char* line, int* len, int* cursor,
                                int* history_index, char* history_draft) {
    int introducer = getc_tty();
    int code;

    if (introducer < 0)
        return;
    if (introducer != '[' && introducer != 'O')
        return;

    code = getc_tty();
    if (code < 0)
        return;
    if (code >= '0' && code <= '9') {
        int suffix = getc_tty();
        if (code == '3' && suffix == '~')
            shell_delete_char(line, len, *cursor);
        return;
    }

    switch (code) {
    case 'D':
        if (*cursor > 0) {
            shell_cursor_left(1);
            *cursor = shell_utf8_previous(line, *cursor);
        }
        break;
    case 'C':
        if (*cursor < *len) {
            shell_cursor_right(1);
            *cursor = shell_utf8_next(line, *len, *cursor);
        }
        break;
    case 'H':
        shell_cursor_left(shell_utf8_columns(line, 0, *cursor));
        *cursor = 0;
        break;
    case 'F':
        shell_cursor_right(shell_utf8_columns(line, *cursor, *len));
        *cursor = *len;
        break;
    case 'A':
        if (shell_history_count > 0 && *history_index > 0) {
            if (*history_index == shell_history_count) {
                strncpy(history_draft, line, SHELL_BUFFER_SIZE - 1);
                history_draft[SHELL_BUFFER_SIZE - 1] = '\0';
            }
            (*history_index)--;
            shell_set_line(line, len, cursor, shell_history[*history_index]);
        }
        break;
    case 'B':
        if (*history_index < shell_history_count) {
            (*history_index)++;
            if (*history_index == shell_history_count)
                shell_set_line(line, len, cursor, history_draft);
            else
                shell_set_line(line, len, cursor, shell_history[*history_index]);
        }
        break;
    default:
        break;
    }
}

char* shell_read_line(void) {
    int len = 0;
    int cursor = 0;
    int history_index = shell_history_count;
    char history_draft[SHELL_BUFFER_SIZE];
    int c;
    char utf8[4];

    input_buffer[0] = '\0';
    history_draft[0] = '\0';
    shell_line_eof = 0;

    while (1) {
        if (shell_termination_requested())
            return NULL;

        c = getc_tty();
        if (c == -2) {
            shell_reap_jobs_during_edit(input_buffer, len, cursor);
            pflush();
            continue;
        }
        if (c < 0) {
            if (shell_termination_requested())
                return NULL;

            if (len > 0)
                printf("\r\n");
            else
                printf("\r\033[K");

            shell_reap_jobs_during_edit(input_buffer, len, cursor);
            pflush();
            continue;
        }
        if (!c)
            continue;

        if (c == '\r' || c == '\n') {
            printf("\n");
            break;
        } else if (c == '\t') {
            shell_complete_line(input_buffer, &len, &cursor);
        } else if (c == 0x04) {
            if (len == 0) {
                printf("\n");
                shell_line_eof = 1;
                return NULL;
            }
            if (cursor < len)
                shell_delete_char(input_buffer, &len, cursor);
        } else if (c == 0x01) {
            shell_cursor_left(shell_utf8_columns(input_buffer, 0, cursor));
            cursor = 0;
        } else if (c == 0x05) {
            shell_cursor_right(shell_utf8_columns(input_buffer, cursor, len));
            cursor = len;
        } else if (c == 0x0B) {
            input_buffer[cursor] = '\0';
            len = cursor;
            shell_redraw_line(input_buffer, len, cursor);
        } else if (c == 0x0C) {
            printf("\033[H\033[2J\033[3J");
            shell_redraw_line(input_buffer, len, cursor);
        } else if (c == 0x15) {
            le_memmove(input_buffer, input_buffer + cursor, len - cursor + 1);
            len -= cursor;
            cursor = 0;
            shell_redraw_line(input_buffer, len, cursor);
        } else if (c == 0x17) {
            while (cursor > 0 && input_buffer[cursor - 1] == ' ')
                shell_backspace_char(input_buffer, &len, &cursor);
            while (cursor > 0 && input_buffer[cursor - 1] != ' ')
                shell_backspace_char(input_buffer, &len, &cursor);
        } else if (c == '\b' || c == 0x7F) {
            shell_backspace_char(input_buffer, &len, &cursor);
        } else if (c == '\033') {
            shell_handle_escape(input_buffer, &len, &cursor,
                                &history_index, history_draft);
        } else if (c >= ' ' && c <= '~') {
            utf8[0] = (char)c;
            shell_insert_bytes(input_buffer, &len, &cursor, utf8, 1);
            if (history_index == shell_history_count) {
                strncpy(history_draft, input_buffer, SHELL_BUFFER_SIZE - 1);
                history_draft[SHELL_BUFFER_SIZE - 1] = '\0';
            }
        } else if (c >= 0x80) {
            int byte_count = shell_read_utf8(c, utf8);

            shell_insert_bytes(input_buffer, &len, &cursor,
                               utf8, byte_count);
            if (history_index == shell_history_count) {
                strncpy(history_draft, input_buffer, SHELL_BUFFER_SIZE - 1);
                history_draft[SHELL_BUFFER_SIZE - 1] = '\0';
            }
        }
        pflush();
    }

    input_buffer[len] = '\0';
    if (len > 0) {
        shell_history_add(input_buffer);
        shell_history_save();
    }
    return input_buffer;
}

int shell_line_was_eof(void) {
    return shell_line_eof;
}

void shell_line_edit_init(void) {
    shell_history_load();
}

void shell_line_edit_shutdown(void) {
    shell_history_save();
}
