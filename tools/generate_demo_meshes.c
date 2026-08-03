/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: tools/generate_demo_meshes.c
 * Layer: Host tools / generated demonstration assets
 *
 * Responsibilities:
 * - Generate original deterministic OBJ scenes used by teapot-demo.
 * - Keep large mechanical mesh data out of handwritten source files.
 * - Avoid network and host-specific model conversion during normal builds.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct vector {
    double x;
    double y;
    double z;
};

static FILE *open_asset(const char *directory, const char *name)
{
    char path[1024];

    if (snprintf(path, sizeof(path), "%s/%s", directory, name) < 0)
        return NULL;
    return fopen(path, "w");
}

static struct vector cross(struct vector a, struct vector b)
{
    return (struct vector){
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static struct vector normalize(struct vector value)
{
    double length = sqrt(value.x * value.x + value.y * value.y +
                         value.z * value.z);

    if (length < 1e-9)
        return (struct vector){1.0, 0.0, 0.0};
    return (struct vector){value.x / length, value.y / length,
                           value.z / length};
}

static int generate_torus(const char *directory)
{
    enum { RINGS = 32, SIDES = 16 };
    FILE *file = open_asset(directory, "torus.obj");

    if (!file)
        return -1;
    fputs("# Original ArmOS parametric torus\no Torus\n", file);
    for (int ring = 0; ring < RINGS; ring++) {
        double u = 2.0 * M_PI * ring / RINGS;

        for (int side = 0; side < SIDES; side++) {
            double v = 2.0 * M_PI * side / SIDES;
            double radius = 1.55 + 0.48 * cos(v);

            fprintf(file, "v %.9f %.9f %.9f\n",
                    radius * cos(u), radius * sin(u), 0.48 * sin(v));
            fprintf(file, "vn %.9f %.9f %.9f\n",
                    cos(u) * cos(v), sin(u) * cos(v), sin(v));
        }
    }
    for (int ring = 0; ring < RINGS; ring++) {
        for (int side = 0; side < SIDES; side++) {
            int a = ring * SIDES + side + 1;
            int b = ((ring + 1) % RINGS) * SIDES + side + 1;
            int c = ((ring + 1) % RINGS) * SIDES + (side + 1) % SIDES + 1;
            int d = ring * SIDES + (side + 1) % SIDES + 1;

            fprintf(file, "f %d//%d %d//%d %d//%d %d//%d\n",
                    a, a, b, b, c, c, d, d);
        }
    }
    return fclose(file);
}

static struct vector knot_center(double t)
{
    double radius = 1.55 + 0.52 * cos(3.0 * t);

    return (struct vector){radius * cos(2.0 * t),
                           radius * sin(2.0 * t),
                           0.52 * sin(3.0 * t)};
}

static int generate_torus_knot(const char *directory)
{
    enum { RINGS = 72, SIDES = 10 };
    FILE *file = open_asset(directory, "torus-knot.obj");

    if (!file)
        return -1;
    fputs("# Original ArmOS (2,3) torus knot\no TorusKnot\n", file);
    for (int ring = 0; ring < RINGS; ring++) {
        double t = 2.0 * M_PI * ring / RINGS;
        struct vector center = knot_center(t);
        struct vector before = knot_center(t - 0.001);
        struct vector after = knot_center(t + 0.001);
        struct vector tangent = normalize((struct vector){
            after.x - before.x, after.y - before.y, after.z - before.z});
        struct vector normal = normalize(cross(tangent,
            fabs(tangent.z) < 0.85 ? (struct vector){0.0, 0.0, 1.0} :
                                     (struct vector){0.0, 1.0, 0.0}));
        struct vector binormal = normalize(cross(tangent, normal));

        for (int side = 0; side < SIDES; side++) {
            double v = 2.0 * M_PI * side / SIDES;
            struct vector radial = {
                normal.x * cos(v) + binormal.x * sin(v),
                normal.y * cos(v) + binormal.y * sin(v),
                normal.z * cos(v) + binormal.z * sin(v)
            };

            fprintf(file, "v %.9f %.9f %.9f\n",
                    center.x + radial.x * 0.21,
                    center.y + radial.y * 0.21,
                    center.z + radial.z * 0.21);
            fprintf(file, "vn %.9f %.9f %.9f\n",
                    radial.x, radial.y, radial.z);
        }
    }
    for (int ring = 0; ring < RINGS; ring++) {
        for (int side = 0; side < SIDES; side++) {
            int a = ring * SIDES + side + 1;
            int b = ((ring + 1) % RINGS) * SIDES + side + 1;
            int c = ((ring + 1) % RINGS) * SIDES + (side + 1) % SIDES + 1;
            int d = ring * SIDES + (side + 1) % SIDES + 1;

            fprintf(file, "f %d//%d %d//%d %d//%d %d//%d\n",
                    a, a, b, b, c, c, d, d);
        }
    }
    return fclose(file);
}

static int generate_sphere(const char *directory)
{
    enum { RINGS = 16, SEGMENTS = 32 };
    FILE *file = open_asset(directory, "metallic-sphere.obj");

    if (!file)
        return -1;
    fputs("# Original ArmOS UV sphere\nmtllib metallic-sphere.mtl\n"
          "o MetallicSphere\nusemtl Metal\n", file);
    for (int ring = 0; ring <= RINGS; ring++) {
        double latitude = M_PI * ring / RINGS;
        double y = cos(latitude);
        double radius = sin(latitude);

        for (int segment = 0; segment < SEGMENTS; segment++) {
            double longitude = 2.0 * M_PI * segment / SEGMENTS;
            double x = radius * cos(longitude);
            double z = radius * sin(longitude);

            fprintf(file, "v %.9f %.9f %.9f\n", x, y, z);
            fprintf(file, "vn %.9f %.9f %.9f\n", x, y, z);
            fprintf(file, "vt %.9f %.9f\n",
                    (double)segment / SEGMENTS, 1.0 - (double)ring / RINGS);
        }
    }
    for (int ring = 0; ring < RINGS; ring++) {
        for (int segment = 0; segment < SEGMENTS; segment++) {
            int a = ring * SEGMENTS + segment + 1;
            int b = (ring + 1) * SEGMENTS + segment + 1;
            int c = (ring + 1) * SEGMENTS + (segment + 1) % SEGMENTS + 1;
            int d = ring * SEGMENTS + (segment + 1) % SEGMENTS + 1;

            fprintf(file, "f %d/%d/%d %d/%d/%d %d/%d/%d %d/%d/%d\n",
                    a, a, a, b, b, b, c, c, c, d, d, d);
        }
    }
    fclose(file);
    file = open_asset(directory, "metallic-sphere.mtl");
    if (!file)
        return -1;
    fputs("# ArmOS demonstration material\nnewmtl Metal\n"
          "Ka 0.08 0.08 0.10\nKd 0.22 0.34 0.40\n"
          "Ks 0.95 0.95 1.00\nNs 180.0\n", file);
    return fclose(file);
}

static int generate_textured_cube(const char *directory)
{
    FILE *file = open_asset(directory, "textured-cube.obj");

    if (!file)
        return -1;
    fputs("# Original ArmOS UV cube\nmtllib textured-cube.mtl\n"
          "o TexturedCube\nusemtl Checker\n"
          "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\n"
          "v -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
          "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
          "f 1/1 4/2 3/3 2/4\nf 5/1 6/2 7/3 8/4\n"
          "f 1/1 2/2 6/3 5/4\nf 4/1 8/2 7/3 3/4\n"
          "f 1/1 5/2 8/3 4/4\nf 2/1 3/2 7/3 6/4\n", file);
    fclose(file);
    file = open_asset(directory, "textured-cube.mtl");
    if (!file)
        return -1;
    fputs("# Placeholder material for the future texture pipeline\n"
          "newmtl Checker\nKa 0.05 0.05 0.05\n"
          "Kd 0.82 0.55 0.18\nKs 0.25 0.25 0.25\nNs 32\n", file);
    return fclose(file);
}

static int generate_cornell_box(const char *directory)
{
    FILE *file = open_asset(directory, "cornell-box.obj");

    if (!file)
        return -1;
    fputs("# Original simplified ArmOS Cornell-style scene\n"
          "mtllib cornell-box.mtl\no CornellBox\n"
          "v -2 -2 -2\nv 2 -2 -2\nv 2 2 -2\nv -2 2 -2\n"
          "v -2 -2 2\nv 2 -2 2\nv 2 2 2\nv -2 2 2\n"
          "usemtl White\nf 1 2 3 4\nf 1 5 6 2\nf 4 3 7 8\n"
          "usemtl Red\nf 1 4 8 5\nusemtl Green\nf 2 6 7 3\n"
          "o ShortBox\nusemtl White\n"
          "v -1.25 -2 -0.8\nv 0.1 -2 -1.1\nv 0.25 -2 0.2\nv -1.1 -2 0.5\n"
          "v -1.25 -0.65 -0.8\nv 0.1 -0.65 -1.1\nv 0.25 -0.65 0.2\nv -1.1 -0.65 0.5\n"
          "f 9 10 11 12\nf 13 16 15 14\nf 9 13 14 10\n"
          "f 10 14 15 11\nf 11 15 16 12\nf 12 16 13 9\n"
          "o TallBox\n"
          "v 0.45 -2 -0.25\nv 1.5 -2 0.1\nv 1.15 -2 1.25\nv 0.1 -2 0.9\n"
          "v 0.45 0.65 -0.25\nv 1.5 0.65 0.1\nv 1.15 0.65 1.25\nv 0.1 0.65 0.9\n"
          "f 17 18 19 20\nf 21 24 23 22\nf 17 21 22 18\n"
          "f 18 22 23 19\nf 19 23 24 20\nf 20 24 21 17\n", file);
    fclose(file);
    file = open_asset(directory, "cornell-box.mtl");
    if (!file)
        return -1;
    fputs("newmtl White\nKd 0.72 0.72 0.68\n"
          "newmtl Red\nKd 0.70 0.10 0.08\n"
          "newmtl Green\nKd 0.08 0.55 0.16\n", file);
    return fclose(file);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s output-directory\n", argv[0]);
        return 2;
    }
    if (generate_torus(argv[1]) < 0 ||
        generate_torus_knot(argv[1]) < 0 ||
        generate_sphere(argv[1]) < 0 ||
        generate_textured_cube(argv[1]) < 0 ||
        generate_cornell_box(argv[1]) < 0) {
        perror("generate_demo_meshes");
        return 1;
    }
    return 0;
}
