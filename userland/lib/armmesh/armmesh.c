/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/lib/armmesh/armmesh.c
 * Layer: Userland / portable 3D asset loading
 *
 * Responsibilities:
 * - Parse a safe, useful subset of Wavefront OBJ.
 * - Validate indices, line lengths and resource budgets.
 * - Normalize arbitrary model coordinates for demonstration renderers.
 * - Emit triangles without depending on a UI or graphics backend.
 */

#include <armmesh/armmesh.h>

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARMMESH_LINE_CAPACITY       1024u
#define ARMMESH_FACE_VERTICES_MAX     64u
#define ARMMESH_VERTICES_MAX       65536u
#define ARMMESH_NORMALS_MAX        65536u
#define ARMMESH_TRIANGLES_MAX      65536u

struct armmesh_vec3 {
    float x;
    float y;
    float z;
};

struct armmesh_array {
    struct armmesh_vec3 *items;
    size_t count;
    size_t capacity;
    size_t limit;
};

struct armmesh_face_index {
    long vertex;
    long normal;
    int has_normal;
};

static void armmesh_error(char *error, size_t capacity,
                          const char *format, ...)
{
    va_list arguments;

    if (!error || capacity == 0u)
        return;
    va_start(arguments, format);
    (void)vsnprintf(error, capacity, format, arguments);
    va_end(arguments);
}

static int armmesh_array_append(struct armmesh_array *array,
                                struct armmesh_vec3 value)
{
    struct armmesh_vec3 *items;
    size_t capacity;

    if (array->count >= array->limit)
        return -1;
    if (array->count == array->capacity) {
        capacity = array->capacity ? array->capacity * 2u : 256u;
        if (capacity > array->limit)
            capacity = array->limit;
        items = realloc(array->items, capacity * sizeof(*items));
        if (!items)
            return -1;
        array->items = items;
        array->capacity = capacity;
    }
    array->items[array->count++] = value;
    return 0;
}

static struct armmesh_vec3 armmesh_sub(struct armmesh_vec3 left,
                                       struct armmesh_vec3 right)
{
    return (struct armmesh_vec3){
        left.x - right.x, left.y - right.y, left.z - right.z
    };
}

static struct armmesh_vec3 armmesh_cross(struct armmesh_vec3 left,
                                         struct armmesh_vec3 right)
{
    return (struct armmesh_vec3){
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x
    };
}

static struct armmesh_vec3 armmesh_normalize(struct armmesh_vec3 value)
{
    float length = sqrtf(value.x * value.x + value.y * value.y +
                         value.z * value.z);

    if (length < 0.000001f)
        return (struct armmesh_vec3){ 0.0f, 0.0f, 1.0f };
    return (struct armmesh_vec3){
        value.x / length, value.y / length, value.z / length
    };
}

static char *armmesh_skip_space(char *text)
{
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    return text;
}

static int armmesh_parse_vec3(char *text, struct armmesh_vec3 *value)
{
    char *end;
    float components[3];

    for (int axis = 0; axis < 3; axis++) {
        text = armmesh_skip_space(text);
        if (*text == '\0' || *text == '#')
            return -1;
        errno = 0;
        components[axis] = strtof(text, &end);
        if (end == text || errno == ERANGE || !isfinite(components[axis]))
            return -1;
        text = end;
    }
    *value = (struct armmesh_vec3){
        components[0], components[1], components[2]
    };
    return 0;
}

static int armmesh_read_line(FILE *file, char line[ARMMESH_LINE_CAPACITY],
                             size_t number, char *error,
                             size_t error_capacity)
{
    size_t length;

    if (!fgets(line, ARMMESH_LINE_CAPACITY, file))
        return feof(file) ? 0 : -1;
    length = strlen(line);
    if (length != 0u && line[length - 1u] != '\n' && !feof(file)) {
        armmesh_error(error, error_capacity,
                      "line %u exceeds %u bytes", (unsigned int)number,
                      (unsigned int)(ARMMESH_LINE_CAPACITY - 1u));
        return -1;
    }
    return 1;
}

static int armmesh_collect(FILE *file, struct armmesh_array *positions,
                           struct armmesh_array *normals,
                           struct armmesh_vec3 *minimum,
                           struct armmesh_vec3 *maximum,
                           char *error, size_t error_capacity)
{
    char line[ARMMESH_LINE_CAPACITY];
    size_t line_number = 0u;
    int status;

    while ((status = armmesh_read_line(file, line, ++line_number, error,
                                       error_capacity)) > 0) {
        char *text = armmesh_skip_space(line);
        struct armmesh_vec3 value;

        if (text[0] == 'v' && (text[1] == ' ' || text[1] == '\t')) {
            if (armmesh_parse_vec3(text + 1, &value) < 0) {
                armmesh_error(error, error_capacity,
                              "invalid vertex at line %u",
                              (unsigned int)line_number);
                return -1;
            }
            if (armmesh_array_append(positions, value) < 0) {
                armmesh_error(error, error_capacity,
                              "vertex limit or memory exhausted");
                return -1;
            }
            if (positions->count == 1u) {
                *minimum = value;
                *maximum = value;
            } else {
                if (value.x < minimum->x) minimum->x = value.x;
                if (value.y < minimum->y) minimum->y = value.y;
                if (value.z < minimum->z) minimum->z = value.z;
                if (value.x > maximum->x) maximum->x = value.x;
                if (value.y > maximum->y) maximum->y = value.y;
                if (value.z > maximum->z) maximum->z = value.z;
            }
        } else if (text[0] == 'v' && text[1] == 'n' &&
                   (text[2] == ' ' || text[2] == '\t')) {
            if (armmesh_parse_vec3(text + 2, &value) < 0) {
                armmesh_error(error, error_capacity,
                              "invalid normal at line %u",
                              (unsigned int)line_number);
                return -1;
            }
            if (armmesh_array_append(normals, armmesh_normalize(value)) < 0) {
                armmesh_error(error, error_capacity,
                              "normal limit or memory exhausted");
                return -1;
            }
        }
    }
    if (status < 0)
        return -1;
    if (positions->count == 0u) {
        armmesh_error(error, error_capacity, "OBJ contains no vertices");
        return -1;
    }
    return 0;
}

static int armmesh_parse_index(char *token, struct armmesh_face_index *index)
{
    char *slash;
    char *second_slash;
    char *end;

    memset(index, 0, sizeof(*index));
    slash = strchr(token, '/');
    if (slash)
        *slash = '\0';
    errno = 0;
    index->vertex = strtol(token, &end, 10);
    if (end == token || *end != '\0' || errno == ERANGE ||
        index->vertex == 0)
        return -1;
    if (!slash)
        return 0;
    second_slash = strchr(slash + 1, '/');
    if (!second_slash)
        return 0;
    *second_slash++ = '\0';
    if (*second_slash == '\0')
        return 0;
    errno = 0;
    index->normal = strtol(second_slash, &end, 10);
    if (end == second_slash || *end != '\0' || errno == ERANGE ||
        index->normal == 0)
        return -1;
    index->has_normal = 1;
    return 0;
}

static int armmesh_resolve_index(long index, size_t positive_limit,
                                 size_t relative_limit, size_t *resolved)
{
    long value;

    if (index > 0)
        value = index - 1;
    else
        value = (long)relative_limit + index;
    if (value < 0 || (size_t)value >= positive_limit)
        return -1;
    *resolved = (size_t)value;
    return 0;
}

static struct armmesh_vec3 armmesh_scaled_position(
    struct armmesh_vec3 value, struct armmesh_vec3 center, float scale)
{
    return (struct armmesh_vec3){
        (value.x - center.x) * scale,
        (value.y - center.y) * scale,
        (value.z - center.z) * scale
    };
}

static int armmesh_emit_face(
    struct armmesh_face_index indices[ARMMESH_FACE_VERTICES_MAX],
    size_t index_count, const struct armmesh_array *positions,
    const struct armmesh_array *normals, size_t seen_positions,
    size_t seen_normals, struct armmesh_vec3 center, float scale,
    armmesh_triangle_fn emit_triangle, void *data, size_t *triangle_count,
    char *error, size_t error_capacity, size_t line_number)
{
    for (size_t face = 1u; face + 1u < index_count; face++) {
        const size_t corners[3] = { 0u, face, face + 1u };
        struct armmesh_triangle triangle;
        struct armmesh_vec3 points[3];
        int complete_normals = 1;

        if (*triangle_count >= ARMMESH_TRIANGLES_MAX) {
            armmesh_error(error, error_capacity, "triangle limit exceeded");
            return -1;
        }
        for (int corner = 0; corner < 3; corner++) {
            const struct armmesh_face_index *source =
                &indices[corners[corner]];
            size_t vertex_index;

            if (armmesh_resolve_index(source->vertex, positions->count,
                                      seen_positions, &vertex_index) < 0) {
                armmesh_error(error, error_capacity,
                              "vertex index out of range at line %u",
                              (unsigned int)line_number);
                return -1;
            }
            points[corner] = armmesh_scaled_position(
                positions->items[vertex_index], center, scale);
            triangle.vertices[corner].position[0] = points[corner].x;
            triangle.vertices[corner].position[1] = points[corner].y;
            triangle.vertices[corner].position[2] = points[corner].z;
            if (source->has_normal) {
                size_t normal_index;
                struct armmesh_vec3 normal;

                if (armmesh_resolve_index(source->normal, normals->count,
                                          seen_normals,
                                          &normal_index) < 0) {
                    armmesh_error(error, error_capacity,
                                  "normal index out of range at line %u",
                                  (unsigned int)line_number);
                    return -1;
                }
                normal = normals->items[normal_index];
                triangle.vertices[corner].normal[0] = normal.x;
                triangle.vertices[corner].normal[1] = normal.y;
                triangle.vertices[corner].normal[2] = normal.z;
            } else {
                complete_normals = 0;
            }
        }
        if (!complete_normals) {
            struct armmesh_vec3 normal = armmesh_normalize(armmesh_cross(
                armmesh_sub(points[1], points[0]),
                armmesh_sub(points[2], points[0])));

            for (int corner = 0; corner < 3; corner++) {
                triangle.vertices[corner].normal[0] = normal.x;
                triangle.vertices[corner].normal[1] = normal.y;
                triangle.vertices[corner].normal[2] = normal.z;
            }
        }
        if (emit_triangle(&triangle, data) < 0) {
            armmesh_error(error, error_capacity,
                          "renderer rejected triangle %u",
                          (unsigned int)*triangle_count);
            return -1;
        }
        (*triangle_count)++;
    }
    return 0;
}

static int armmesh_parse_faces(
    FILE *file, const struct armmesh_array *positions,
    const struct armmesh_array *normals, struct armmesh_vec3 center,
    float scale, armmesh_triangle_fn emit_triangle, void *data,
    size_t *triangle_count, char *error, size_t error_capacity)
{
    char line[ARMMESH_LINE_CAPACITY];
    size_t line_number = 0u;
    size_t seen_positions = 0u;
    size_t seen_normals = 0u;
    int status;

    while ((status = armmesh_read_line(file, line, ++line_number, error,
                                       error_capacity)) > 0) {
        char *text = armmesh_skip_space(line);

        if (text[0] == 'v' && (text[1] == ' ' || text[1] == '\t')) {
            seen_positions++;
            continue;
        }
        if (text[0] == 'v' && text[1] == 'n' &&
            (text[2] == ' ' || text[2] == '\t')) {
            seen_normals++;
            continue;
        }
        if (text[0] == 'f' && (text[1] == ' ' || text[1] == '\t')) {
            struct armmesh_face_index indices[ARMMESH_FACE_VERTICES_MAX];
            size_t count = 0u;
            char *token;
            char *save = NULL;

            for (token = strtok_r(text + 1, " \t\r\n", &save);
                 token; token = strtok_r(NULL, " \t\r\n", &save)) {
                if (token[0] == '#')
                    break;
                if (count >= ARMMESH_FACE_VERTICES_MAX) {
                    armmesh_error(error, error_capacity,
                                  "face at line %u has too many vertices",
                                  (unsigned int)line_number);
                    return -1;
                }
                if (armmesh_parse_index(token, &indices[count]) < 0) {
                    armmesh_error(error, error_capacity,
                                  "invalid face index at line %u",
                                  (unsigned int)line_number);
                    return -1;
                }
                count++;
            }
            if (count < 3u) {
                armmesh_error(error, error_capacity,
                              "face at line %u has fewer than 3 vertices",
                              (unsigned int)line_number);
                return -1;
            }
            if (armmesh_emit_face(indices, count, positions, normals,
                                  seen_positions, seen_normals, center, scale,
                                  emit_triangle, data, triangle_count,
                                  error, error_capacity, line_number) < 0)
                return -1;
        }
    }
    return status < 0 ? -1 : 0;
}

int armmesh_load_obj(const char *path,
                     armmesh_triangle_fn emit_triangle,
                     void *data,
                     struct armmesh_stats *stats,
                     char *error,
                     size_t error_capacity)
{
    struct armmesh_array positions = { .limit = ARMMESH_VERTICES_MAX };
    struct armmesh_array normals = { .limit = ARMMESH_NORMALS_MAX };
    struct armmesh_vec3 minimum = { 0.0f, 0.0f, 0.0f };
    struct armmesh_vec3 maximum = { 0.0f, 0.0f, 0.0f };
    struct armmesh_vec3 center;
    size_t triangle_count = 0u;
    float extent;
    float scale;
    FILE *file = NULL;
    int result = -1;

    if (error && error_capacity)
        error[0] = '\0';
    if (!path || !emit_triangle) {
        armmesh_error(error, error_capacity, "invalid loader arguments");
        return -1;
    }
    file = fopen(path, "r");
    if (!file) {
        armmesh_error(error, error_capacity, "%s: %s", path,
                      strerror(errno));
        goto out;
    }
    if (armmesh_collect(file, &positions, &normals, &minimum, &maximum,
                        error, error_capacity) < 0)
        goto out;
    center = (struct armmesh_vec3){
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f
    };
    extent = fmaxf(maximum.x - minimum.x,
                   fmaxf(maximum.y - minimum.y,
                         maximum.z - minimum.z));
    if (extent < 0.000001f) {
        armmesh_error(error, error_capacity, "OBJ has zero-sized geometry");
        goto out;
    }
    scale = 2.6f / extent;
    rewind(file);
    if (armmesh_parse_faces(file, &positions, &normals, center, scale,
                            emit_triangle, data, &triangle_count,
                            error, error_capacity) < 0)
        goto out;
    if (triangle_count == 0u) {
        armmesh_error(error, error_capacity, "OBJ contains no faces");
        goto out;
    }
    if (stats) {
        stats->vertices = positions.count;
        stats->normals = normals.count;
        stats->triangles = triangle_count;
    }
    result = 0;

out:
    if (file)
        fclose(file);
    free(positions.items);
    free(normals.items);
    return result;
}
