/*
 * ArmOS
 * Copyright (c) 2026 Mohamed Ennassiri
 *
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE for details.
 *
 * File: userland/programs/drm-info/drm-info.c
 * Layer: Userland / ArmOS DRM diagnostic
 *
 * Responsibilities:
 * - Validate the architecture-neutral ArmOS DRM discovery ABI.
 * - Report generic capabilities without depending on a concrete GPU backend.
 *
 * Notes:
 * - The diagnostic driver name is informational, never a feature selector.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <uapi/armos/drm.h>

typedef struct capability_name {
    unsigned long long bit;
    const char *name;
} capability_name_t;

static const capability_name_t capability_names[] = {
    { ARMOS_DRM_CAP_SCANOUT, "scanout" },
    { ARMOS_DRM_CAP_TRANSFER_2D, "transfer-2d" },
    { ARMOS_DRM_CAP_BUFFER_OBJECTS, "buffer-objects" },
    { ARMOS_DRM_CAP_CONTEXTS, "contexts" },
    { ARMOS_DRM_CAP_COMMAND_SUBMIT, "command-submit" },
    { ARMOS_DRM_CAP_FENCES, "fences" },
    { ARMOS_DRM_CAP_CPU_MAPPABLE, "cpu-mappable" },
    { ARMOS_DRM_CAP_RENDER_3D, "render-3d" },
    { ARMOS_DRM_CAP_RESOURCE_TRANSFER, "resource-transfer" },
};

static const char *backend_class_name(unsigned int backend_class)
{
    switch (backend_class) {
    case ARMOS_DRM_BACKEND_SOFTWARE:
        return "software";
    case ARMOS_DRM_BACKEND_PARAVIRTUAL:
        return "paravirtual";
    case ARMOS_DRM_BACKEND_NATIVE:
        return "native";
    default:
        return "unknown";
    }
}

int main(void)
{
    armos_drm_info_t info;
    armos_drm_context_create_t create;
    armos_drm_context_destroy_t destroy;
    armos_drm_bo_create_t bo_create;
    armos_drm_bo_destroy_t bo_destroy;
    armos_drm_resource_attachment_t attachment;
    armos_drm_submit_t submit;
    armos_drm_fence_wait_t wait;
    armos_drm_fence_destroy_t fence_destroy;
    unsigned int virgl_noop = 0u;
    void *mapping;
    unsigned int index;
    int fd;

    fd = open("/dev/dri/renderD128", O_RDWR, 0);
    if (fd < 0) {
        fprintf(stderr, "drm-info: /dev/dri/renderD128: %s\n",
                strerror(errno));
        return 1;
    }
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, ARMOS_DRM_IOCTL_GET_INFO, &info) < 0) {
        fprintf(stderr, "drm-info: query: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (info.abi_version != ARMOS_DRM_ABI_VERSION ||
        info.struct_size < sizeof(info)) {
        fprintf(stderr, "drm-info: unsupported ABI %u size %u\n",
                info.abi_version, info.struct_size);
        close(fd);
        return 1;
    }

    printf("driver: %s\n", info.driver_name[0] ? info.driver_name : "unnamed");
    printf("backend: %s\n", backend_class_name(info.backend_class));
    printf("scanouts: %u\n", info.scanout_count);
    if (info.scanout_count) {
        printf("scanout-0: %ux%u\n",
               info.scanout_width, info.scanout_height);
    }
    printf("max-resource: %ux%u\n",
           info.max_resource_width, info.max_resource_height);
    printf("command-set: %s\n",
           info.command_set[0] ? (const char *)info.command_set : "none");
    printf("capabilities:");
    for (index = 0;
         index < sizeof(capability_names) / sizeof(capability_names[0]);
         index++) {
        if (info.capabilities & capability_names[index].bit)
            printf(" %s", capability_names[index].name);
    }
    printf("\n");
    if (info.capabilities & ARMOS_DRM_CAP_CONTEXTS) {
        memset(&create, 0, sizeof(create));
        create.abi_version = ARMOS_DRM_ABI_VERSION;
        if (ioctl(fd, ARMOS_DRM_IOCTL_CONTEXT_CREATE, &create) < 0) {
            fprintf(stderr, "drm-info: context create: %s\n",
                    strerror(errno));
            close(fd);
            return 1;
        }
        printf("context-smoke: id=%u command-set=%s\n",
               create.context_id,
               create.command_set[0] ?
                   (const char *)create.command_set : "none");
        if ((info.capabilities &
             (ARMOS_DRM_CAP_COMMAND_SUBMIT |
              ARMOS_DRM_CAP_FENCES)) ==
            (ARMOS_DRM_CAP_COMMAND_SUBMIT |
             ARMOS_DRM_CAP_FENCES) &&
            strncmp((const char *)create.command_set,
                    "virgl", sizeof("virgl") - 1u) == 0) {
            /*
             * VIRGL_CMD0(VIRGL_CCMD_NOP, VIRGL_OBJECT_NULL, 0).
             * A one-dword no-op validates SUBMIT_3D and asynchronous fence
             * completion without depending on a rendering state tracker.
             */
            memset(&submit, 0, sizeof(submit));
            submit.context_id = create.context_id;
            submit.command_address =
                (unsigned long long)(uintptr_t)&virgl_noop;
            submit.command_size = sizeof(virgl_noop);
            if (ioctl(fd, ARMOS_DRM_IOCTL_SUBMIT, &submit) < 0) {
                fprintf(stderr, "drm-info: SUBMIT_3D: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            memset(&wait, 0, sizeof(wait));
            wait.fence_id = submit.fence_id;
            wait.timeout_ns = 2000000000LL;
            if (ioctl(fd, ARMOS_DRM_IOCTL_FENCE_WAIT, &wait) < 0) {
                fprintf(stderr, "drm-info: fence wait: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            memset(&fence_destroy, 0, sizeof(fence_destroy));
            fence_destroy.fence_id = submit.fence_id;
            if (ioctl(fd, ARMOS_DRM_IOCTL_FENCE_DESTROY,
                      &fence_destroy) < 0) {
                fprintf(stderr, "drm-info: fence destroy: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            printf("submit-smoke: SUBMIT_3D fence=%llu signaled\n",
                   submit.fence_id);
        }
        if ((info.capabilities &
             (ARMOS_DRM_CAP_BUFFER_OBJECTS |
              ARMOS_DRM_CAP_CPU_MAPPABLE)) ==
            (ARMOS_DRM_CAP_BUFFER_OBJECTS |
             ARMOS_DRM_CAP_CPU_MAPPABLE)) {
            memset(&bo_create, 0, sizeof(bo_create));
            bo_create.abi_version = ARMOS_DRM_ABI_VERSION;
            bo_create.flags = ARMOS_DRM_BO_CPU_READ |
                              ARMOS_DRM_BO_CPU_WRITE |
                              ARMOS_DRM_BO_COMMAND;
            bo_create.size = 4096;
            if (ioctl(fd, ARMOS_DRM_IOCTL_BO_CREATE, &bo_create) < 0) {
                fprintf(stderr, "drm-info: BO create: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            mapping = mmap(NULL, (size_t)bo_create.size,
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                           (off_t)bo_create.map_offset);
            if (mapping == MAP_FAILED) {
                fprintf(stderr, "drm-info: BO mmap: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            memset(mapping, 0xa5, (size_t)bo_create.size);
            if (munmap(mapping, (size_t)bo_create.size) < 0) {
                fprintf(stderr, "drm-info: BO munmap: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            memset(&attachment, 0, sizeof(attachment));
            attachment.context_id = create.context_id;
            attachment.handle = bo_create.handle;
            if (ioctl(fd, ARMOS_DRM_IOCTL_RESOURCE_ATTACH,
                      &attachment) < 0 ||
                ioctl(fd, ARMOS_DRM_IOCTL_RESOURCE_DETACH,
                      &attachment) < 0) {
                fprintf(stderr, "drm-info: BO attachment: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            memset(&bo_destroy, 0, sizeof(bo_destroy));
            bo_destroy.handle = bo_create.handle;
            if (ioctl(fd, ARMOS_DRM_IOCTL_BO_DESTROY, &bo_destroy) < 0) {
                fprintf(stderr, "drm-info: BO destroy: %s\n",
                        strerror(errno));
                close(fd);
                return 1;
            }
            printf("buffer-smoke: handle=%u mmap attach detach destroy\n",
                   bo_create.handle);
        }
        memset(&destroy, 0, sizeof(destroy));
        destroy.context_id = create.context_id;
        if (ioctl(fd, ARMOS_DRM_IOCTL_CONTEXT_DESTROY, &destroy) < 0) {
            fprintf(stderr, "drm-info: context destroy: %s\n",
                    strerror(errno));
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}
