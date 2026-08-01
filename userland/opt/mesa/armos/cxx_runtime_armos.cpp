/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/opt/mesa/armos/cxx_runtime_armos.cpp
 * Layer: Mesa-private C++ compatibility
 *
 * This translation unit supplies only the allocation operators required by
 * Mesa's GLSL compiler.  Mesa is built without exceptions or RTTI, and its
 * externally visible EGL/GLES interfaces remain C APIs.
 */

#include <new>
#include <stdlib.h>

extern "C" {
void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;
}

static void *armos_mesa_allocate(size_t size)
{
    void *pointer = malloc(size != 0 ? size : 1);

    if (pointer == NULL)
        abort();
    return pointer;
}

void *operator new(size_t size)
{
    return armos_mesa_allocate(size);
}

void *operator new[](size_t size)
{
    return armos_mesa_allocate(size);
}

void operator delete(void *pointer) noexcept
{
    free(pointer);
}

void operator delete[](void *pointer) noexcept
{
    free(pointer);
}

void operator delete(void *pointer, size_t) noexcept
{
    free(pointer);
}

void operator delete[](void *pointer, size_t) noexcept
{
    free(pointer);
}
