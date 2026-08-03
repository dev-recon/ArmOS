/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/include/armmesh/armmesh.h
 * Layer: Userland / public 3D asset API
 *
 * Responsibilities:
 * - Expose a renderer-independent triangle stream for mesh assets.
 * - Describe bounded OBJ loading and its diagnostics.
 * - Keep UI, Wayland and GPU policy outside the asset loader.
 */

#ifndef ARMOS_ARMMESH_H
#define ARMOS_ARMMESH_H

#include <stddef.h>

#define ARMMESH_ERROR_CAPACITY 160u

struct armmesh_vertex {
    float position[3];
    float normal[3];
};

struct armmesh_triangle {
    struct armmesh_vertex vertices[3];
};

struct armmesh_stats {
    size_t vertices;
    size_t normals;
    size_t triangles;
};

typedef int (*armmesh_triangle_fn)(
    const struct armmesh_triangle *triangle,
    void *data);

/*
 * Load the geometry as a centered unit-scale triangle stream. Faces with more
 * than three vertices are triangulated as a fan. Texture coordinates are
 * accepted but deliberately ignored by this geometry-only contract.
 */
int armmesh_load_obj(const char *path,
                     armmesh_triangle_fn emit_triangle,
                     void *data,
                     struct armmesh_stats *stats,
                     char *error,
                     size_t error_capacity);

#endif
