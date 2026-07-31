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

When a backend publishes a command set, `ARMOS_DRM_IOCTL_GET_COMMAND_CAPS`
copies its versioned capability blob through a size-bounded common UAPI. The
common core neither parses nor names that blob. The platform backend chooses
the native capability-set identifier and translates the request to its
transport protocol. The maximum exposed blob is 16 KiB, and all copying passes
through kernel-owned storage.

The `drm-info` userland diagnostic prints the generic contract. The
`libarmos-virgl-winsys` adapter consumes it from `/dev/dri/renderD128` and owns
the first VirGL-specific userland boundary. It provides context, buffer,
mapping, attachment, explicit CPU/device transfer, submission and fence
operations without exposing VirtIO transport details. This is deliberately a
small dependency-free layer suitable for a later Mesa winsys; it is not a
partial Gallium state tracker.

Mesa's next architecture-neutral layer is the ArmGL Gallium frontend. It owns
the `pipe_frontend_screen`, GLES state-tracker contexts and off-screen color,
depth and stencil attachments. Surface allocation is lazy, resize invalidates
attachments atomically, context sharing is explicit and synchronous flushes
wait on Gallium fences. The frontend does not know how the `pipe_screen` was
created and therefore works unchanged with VirGL and the future VC4/V3D
backend. EGL, Wayland buffers, window policy and presentation remain above it.

The frontend compiles as Mesa's `libarmgl.a` with both ArmOS ARM32 and ARM64
ABIs. This milestone validates the common state-tracker lifecycle; it does not
yet install `libEGL.a` or `libGLESv2.a`. The following lot adds a surfaceless
EGL driver and pbuffer triangle/readback test before any native Wayland window
surface is introduced.

Command sets may require resource metadata that does not belong in the common
BO model. `BO_CREATE` therefore accepts a size-bounded opaque resource
descriptor after command-set negotiation. The common core copies and owns the
descriptor, preserves it for the complete BO lifetime and passes it back only
to the selected backend. The optional `drm_virgl.h` contract defines the
VirGL descriptor consumed by the Mesa adapter and qemu-virt backend; neither
the common DRM core nor other platform backends include that header. Legacy
generic BO creation remains source- and ABI-compatible because the descriptor
fields replace reserved space without changing the ioctl structure size.

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
Gallium/VirtIO constants inside the qemu-virt backend. `BO_CREATE` returns both
the common DRM handle and an opaque command-stream handle; clients never derive
a transport resource identifier from a DRM handle. Generic BO usage flags
describe render targets, textures, vertex, index, constant and shader-storage
resources without naming Gallium. `BO_TRANSFER` synchronizes explicit
CPU-to-device uploads and device-to-CPU readbacks, including common cache
maintenance. For command-described resources, the common core validates the
backing-memory range while qemu-virt validates mip level and texture/array box
geometry from the VirGL descriptor before emitting a transport command.
Submissions are queued without waiting for the control ring. The
device IRQ completes a common fence, wakes scheduler waiters and releases the
queue for the next operation. Fences remain file-owned until `FENCE_DESTROY` or
final descriptor closure, including after a successful wait. Context and file
teardown detach resources before destroying their backend objects.

`drm-info` validates context creation, submits a valid VirGL NOP through
`SUBMIT_3D`, waits for its asynchronous fence, then exercises the BO
create/map/attach/detach/destroy lifecycle when the backend advertises it.
The expected accelerated smoke line is:

```text
submit-smoke: SUBMIT_3D fence=N signaled
```

`virgl-smoke` additionally retrieves the selected VirGL capability blob,
uploads a vertex buffer, renders a red triangle into an off-screen BGRA render
target, waits for the asynchronous fence, reads the target back and verifies
both background and covered pixels. A successful accelerated run prints:

```text
virgl-smoke: command-set=virgl2 caps-version=N caps-size=N
virgl-smoke: corner=ff000000 center=ffff0000 changed=N
virgl-smoke: off-screen triangle verified
```

The exact command-set suffix, version and size are device properties and must
not be hard-coded by clients. Failure to negotiate a usable capset leaves the
3D capabilities unpublished, so applications can select their software path
without platform checks.

The compositor first tries the architecture-neutral card-node contract. It
creates a CPU-mappable scanout BO, composes directly into that mapping and
presents only its damaged rectangle through `BO_PRESENT`. The common DRM core
validates ownership and bounds and performs architecture-neutral cache
maintenance; the qemu-virt backend translates presentation into
`TRANSFER_TO_HOST_2D`, `SET_SCANOUT` and `RESOURCE_FLUSH`. If those
capabilities are unavailable, the compositor retains its framebuffer backend.
This connects display presentation to DRM but does not yet make the compositor
render its UI with VirGL. The raw protocol smoke test validates the kernel and
winsys lifecycle; Mesa/Gallium remains responsible for the production command
encoder and graphics API.

Raspberry Pi continues to use the firmware framebuffer and therefore does not
expose DRM nodes until its native backend is present.

DRM BO mapping offsets are opaque, non-negative values returned by
`ARMOS_DRM_IOCTL_BO_CREATE`; they are not filesystem byte offsets. The ArmOS
`mmap()` adapters must forward them unchanged to the common kernel mapping
path. Rejecting every non-zero offset makes the compositor destroy its BO and
silently fall back to `/dev/fb0`, so `virgl-smoke` and compositor validation
must both exercise a non-zero BO mapping.

The tracked qemu-virt ARM32 and ARM64 profiles select VirGL. A local profile
may retain the compatibility path with `QEMU_GPU_ACCEL=2d`. VirGL can also be
selected explicitly without changing a profile:

```sh
ARMOS_CONFIG=configs/qemu-virt-arm64.conf ./boot-graphics.sh
# or
QEMU_GPU_ACCEL=virgl TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt \
    ./boot-graphics.sh
```

The launcher rejects this mode when the selected QEMU binary lacks
`virtio-gpu-gl-device`; it never silently claims acceleration while running the
software 2D backend. `QEMU_GPU_HOSTMEM` controls the optional host-visible blob
window and defaults to `512M`. On macOS, VirGL requires the SDL OpenGL backend;
the Cocoa backend does not expose OpenGL to QEMU. The repository-local QEMU can
be built reproducibly with:

```sh
./tools/build_qemu_10_0_2.sh
```

The build helper validates SDL, OpenGL, VirGL and the QEMU firmware data
directory before installing the runtime under `build/qemu-10.0.2/install`.
The launcher prefers that runtime and passes its matching firmware directory
explicitly. No absolute developer-machine QEMU path is embedded in ArmOS.
The tracked macOS QEMU patch also keeps pixel conversion at the host display
boundary. VirGL scanout bypasses QEMU's normal `DisplaySurface` upload and
hands SDL a host texture directly, so the macOS SDL renderer samples that
texture through a dedicated red/blue conversion shader. Guest applications,
the common DRM core and the VirtIO-GPU backend retain the standard BGRA
contract and contain no macOS-specific conversion.

A second host-only QEMU patch defers SDL mouse-grab, cursor and window-title
updates from the VirtIO input notifier to the SDL event thread. Cocoa requires
all `NSWindow` changes to run on that thread; applying them from a vCPU thread
causes an `NSInternalInconsistencyException` as soon as the guest keyboard or
mouse becomes active. The notifier publishes only the latest desired mouse
mode, which the SDL polling loop then consumes. This synchronization remains
entirely outside ArmOS and does not alter the common DRM or input contracts.

At boot, the qemu-virt backend reports one unambiguous line:

```text
VirtIO GPU acceleration: VirGL2 capset=N version=N size=N
```

`VirtIO GPU acceleration: 2D` means the 3D feature/capset contract was not
negotiated. After boot, validate both layers:

```sh
drm-info
virgl-smoke
```

This milestone makes genuine VirGL contexts, typed resources, explicit
transfers and asynchronous fences available to userland, and validates them
with an off-screen draw/readback test. The desktop remains software-composited
until Mesa/Gallium is connected to
`libarmos-virgl-winsys`; running through `virtio-gpu-gl-device` alone must not
be described as GPU-accelerated window composition.

The qemu-virt scanout geometry is not compiled into the common framebuffer.
The launcher passes `GPU_XRES` and `GPU_YRES` to VirtIO-GPU, then the platform
backend adopts the mode returned by `GET_DISPLAY_INFO` before allocating its
resource and framebuffer. Both variables must be provided together. The
current validated upper bound is 1920x1080:

```sh
QEMU_GPU_ACCEL=virgl GPU_XRES=1920 GPU_YRES=1080 \
    ./boot-graphics.sh
```

Resizing the host QEMU window still scales the negotiated guest scanout. A
live guest mode switch requires the later modeset/resource-replacement
lifecycle and is deliberately not emulated by rewriting framebuffer geometry.
VirtIO configuration events preserve the resource that currently owns the
scanout; they never restore the boot framebuffer while a DRM client is active.
When that client releases its scanout buffer, the backend restores and fully
publishes the boot framebuffer before destroying the resource, providing a
deterministic console fallback.

## Next milestones

1. Cross-build a minimal Mesa containing Gallium VirGL, EGL and OpenGL ES 2
   only. The first slice is now present: Mesa's `struct virgl_winsys` is
   implemented by `userland/opt/mesa/armos/virgl_armos_winsys.c` on top of
   `libarmos-virgl-winsys`, with typed resources, mapping, transfers, command
   relocation references and native ArmOS fence lifetimes. The common ArmGL
   frontend now supplies GLES state-tracker contexts and pbuffer attachments
   without depending on the selected GPU. Both layers are compiled against
   Mesa 25.3.6 headers with the ARM32 and ARM64 target compilers by
   `tools/check_mesa_virgl_adapter.sh`. The remaining part of this milestone
   is the reproducible Mesa static build and the ArmOS EGL platform glue.
2. Extend the Wayland contract with explicit GPU buffer exchange and
   acquire/release fences, then use it for EGL window surfaces.
3. Import client GPU buffers into the compositor and compose textured damage
   rectangles before copy-free DRM presentation.
4. Port Raylib on the common EGL/OpenGL ES 2 path.
5. Implement Raspberry Pi VC4 scanout management and V3D rendering as a
   platform backend behind the same common contracts.

Every milestone must retain the same UAPI on ARM32 and ARM64. Unsupported
operations fail through capability checks rather than compile-time
application variants.
