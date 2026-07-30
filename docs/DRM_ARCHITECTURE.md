# ArmOS DRM Architecture

ArmOS exposes graphics acceleration through one architecture-neutral kernel
device model. Hardware and transport implementations are platform backends;
rendering APIs, window policy and applications remain in userland.

## Layering contract

```text
Raylib / ArmUI / applications
             |
     libarmos-gles + EGL
             |
 /dev/dri/card0 + renderD128
             |
 common GPU objects, ownership, queues and fences
             |
      +------+----------------+
      |                       |
 qemu-virt backend       Raspberry Pi backend
 VirtIO-GPU/VirGL        VC4 display + V3D render
```

The common kernel owns only hardware-independent policy:

- device and context ownership;
- buffer-object handles and mapping permissions;
- command submission lifetime;
- fences, waits and process cleanup;
- capability discovery and validation.

It must not contain VirtIO feature bits, VC4/V3D registers, bus addresses,
packet formats, cache-line assumptions or architecture-specific barriers.
Those details belong to `kernel/platform/qemu_virt/` and
`kernel/platform/raspberrypi/`, with architecture helpers used only by those
backends.

The compositor remains a userland service. Neither Wayland protocol policy nor
Raylib state belongs in the kernel.

## Device nodes

- `/dev/dri/card0` is the privileged modeset/scanout node. It is root-only so
  ordinary clients cannot take over the display.
- `/dev/dri/renderD128` is the unprivileged render node. It never grants
  modeset authority.

Both nodes use the same object ABI. Context handles belong to the open file
description, survive descriptor duplication, and are destroyed by the common
DRM core when the last reference closes. Command submissions are size-bounded
and copied into kernel ownership before reaching a backend. Fence identifiers
are monotonic and scoped to that open file.

## Current milestone

The card and render nodes provide a versioned discovery ABI through
`ARMOS_DRM_IOCTL_GET_INFO`. It reports generic capability bits, resource
limits, scanout geometry, backend class and an opaque command-set identifier.
Applications must select behavior from capabilities, never from the diagnostic
driver name.

The `drm-info` userland diagnostic prints this contract and is the smoke test
for future backends.

The existing qemu-virt VirtIO-GPU 2D implementation is the first registered
backend. It negotiates VirGL when the device offers it and inventories the
standard VirGL/VirGL2 capability sets. The generic `render-3d`, `contexts`,
`command-submit` and `fences` capabilities and opaque command-set name are
published only after both the feature and a supported capability set have been
confirmed. Context creation and bounded submission flow through the common DRM
lifecycle. Per-file buffer objects use page-backed storage, validated formats
and permissions, explicit resource/context attachment and
descriptor-independent mapping references. A BO mapping uses the opaque
page-aligned offset returned by `BO_CREATE` or `BO_MAP`; mappings survive
descriptor closure and release their pages through the generic VMA lifecycle.

VirtIO-GPU translates the generic objects into VirGL resources and keeps all
Gallium/VirtIO constants inside the qemu-virt backend. Submissions are queued
without waiting for the control ring. The device IRQ completes a common fence,
wakes scheduler waiters and releases the queue for the next operation. Fences
remain file-owned until `FENCE_DESTROY` or final descriptor closure, including
after a successful wait. Context and file teardown detach resources before
destroying their backend objects.

`drm-info` validates context creation, submits a valid VirGL NOP through
`SUBMIT_3D`, waits for its asynchronous fence, then exercises the BO
create/map/attach/detach/destroy lifecycle when the backend advertises it.
The expected accelerated smoke line is:

```text
submit-smoke: SUBMIT_3D fence=N signaled
```

The compositor first tries the architecture-neutral card-node contract. It
creates a CPU-mappable scanout BO, composes directly into that mapping and
presents only its damaged rectangle through `BO_PRESENT`. The common DRM core
validates ownership and bounds and performs architecture-neutral cache
maintenance; the qemu-virt backend translates presentation into
`TRANSFER_TO_HOST_2D`, `SET_SCANOUT` and `RESOURCE_FLUSH`. If those
capabilities are unavailable, the compositor retains its framebuffer backend.
This connects display presentation to DRM but does not yet make the compositor
render its UI with VirGL: `SUBMIT_3D` is currently validated by the smoke test,
and the real userspace 3D command encoder remains the next milestone.

Raspberry Pi continues to use the firmware framebuffer and therefore does not
expose DRM nodes until its native backend is present.

QEMU boots remain 2D by default. An explicitly VirGL-enabled QEMU can be used
with:

```sh
QEMU_GPU_ACCEL=virgl ./boot-graphics.sh
```

The launcher rejects this mode when the selected QEMU binary lacks
`virtio-gpu-gl-device`; it never silently claims acceleration while running the
software 2D backend. `QEMU_GPU_HOSTMEM` controls the optional host-visible blob
window and defaults to `512M`.

The qemu-virt scanout geometry is not compiled into the common framebuffer.
The launcher passes `GPU_XRES` and `GPU_YRES` to VirtIO-GPU, then the platform
backend adopts the mode returned by `GET_DISPLAY_INFO` before allocating its
resource and framebuffer. Both variables must be provided together. The
current validated upper bound is 1920x1080:

```sh
GPU_XRES=1920 GPU_YRES=1080 ./boot-graphics.sh
```

Resizing the host QEMU window still scales the negotiated guest scanout. A
live guest mode switch requires the later modeset/resource-replacement
lifecycle and is deliberately not emulated by rewriting framebuffer geometry.

## Next milestones

1. Add a bounded userspace command encoder and validate real VirGL rendering
   through the completed object/attachment/fence contract.
2. Implement Raspberry Pi VC4 scanout management and V3D rendering as a
   platform backend.
3. Build a small EGL/OpenGL ES compatibility layer in userland, then port
   Raylib above it while preserving a software fallback.

Every milestone must retain the same UAPI on ARM32 and ARM64. Unsupported
operations fail through capability checks rather than compile-time
application variants.
