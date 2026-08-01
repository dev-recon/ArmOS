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
  ordinary clients cannot take over the display. Its open file description is
  exclusive: one compositor owns display policy until its last reference is
  closed.
- `/dev/dri/renderD128` is the unprivileged render node. It never grants
  modeset authority.

`ARMOS_DRM_BO_SCANOUT` describes a buffer layout capability, not an authority
to display it.  A render-node client may create such a buffer and export it to
the compositor.  Only a card-node file can issue `BO_PRESENT`, so the
privileged compositor remains the sole owner of scanout policy.

GPU resources that cross a process or display boundary are marked shared when
they are created.  This freezes their backing and layout before the first
render, prevents Mesa/VirGL from substituting a private staging allocation,
and makes later FD export deterministic.  Scanout resources are necessarily
exportable; EGL swapchain images declare the same property explicitly.
Compositor outputs use the canonical BGRA8888 scanout format.  ArmGL validates
the complete render-target, shared and scanout bind set against Gallium before
allocating the resource, rather than deferring an unsupported combination to
the first drawing command.
ArmGL accepts both Gallium surface models: drivers may return an owned view
through `create_surface`, while current VirGL consumes the embedded
`pipe_surface` view directly.  Rendering primitives do not require an optional
driver callback merely to describe a synchronous render target.

Direct-presentation diagnostics must therefore run with the compositor
stopped. Opening `card0` while a compositor owns it fails with `EBUSY`; this
prevents a test or another root process from stealing scanout or restoring a
stale framebuffer when it exits.

Both nodes use the same object ABI. Context handles belong to the open file
description, survive descriptor duplication, and are destroyed by the common
DRM core when the last reference closes. Command submissions are size-bounded
and copied into kernel ownership before reaching a backend. Fence identifiers
are monotonic and scoped to that open file.

Buffer and fence identifiers deliberately remain local to one DRM file. Their
only cross-process representation is a capability descriptor created by
`BO_EXPORT` or `FENCE_EXPORT` and transferred with the normal descriptor
passing contract. `BO_IMPORT` creates a new owner in the receiving DRM file;
it never exposes or guesses another client's local handle. Descriptor creation
flags such as `CLOEXEC` therefore apply to `BO_EXPORT`; `BO_IMPORT` creates no
descriptor and requires its reserved flags to remain zero. Export descriptors,
local owners and active mappings hold independent references. Backend resource
destruction therefore occurs only after the last owner and export descriptor
have gone away, while physical pages additionally remain alive until the last
mapping is released.

An exported fence descriptor is pollable. It becomes readable only after the
backend completes the corresponding submission, and one fixed-size read
returns the completion status. Completion uses the common poll notification
path, so EGL, Wayland and the compositor can block without periodic timer
wakeups. These descriptors provide the outgoing half of the role served by a
Linux sync-file while keeping an independent ArmOS ABI. Gallium's aggregate
native-fence capability remains disabled until the common DRM contract also
supports importing such a descriptor; exporting a completed submission does
not falsely imply support for incoming fences.

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

`drm-info` also verifies the inter-process primitives through two independent
DRM file descriptions. It publishes image metadata, exports the BO, verifies
that this layout is immutable but can be republished idempotently, then imports
and maps it through the second render node and checks both metadata and shared
contents. It separately exports a submitted fence and validates event-driven
`poll()` plus its completion result. The expected lines are:

```text
fence-smoke: sync fd poll/read signaled
buffer-share-smoke: immutable metadata and mmap verified
```

Mesa's next architecture-neutral layer is the ArmGL Gallium frontend. It owns
the `pipe_frontend_screen`, GLES state-tracker contexts and off-screen color,
depth and stencil attachments. Surface allocation is lazy, resize invalidates
attachments atomically, context sharing is explicit and synchronous flushes
wait on Gallium fences. The frontend does not know how the `pipe_screen` was
created and therefore works unchanged with VirGL and the future VC4/V3D
backend. EGL, Wayland buffers, window policy and presentation remain above it.

The frontend compiles as Mesa's `libarmgl.a` with both ArmOS ARM32 and ARM64
ABIs. The ArmOS EGL driver adds an architecture-neutral platform layer above
it: OpenGL ES 2 contexts, surfaceless operation, pbuffer surfaces and native
Wayland window surfaces. EGL display, context and surface references keep
their Gallium screen alive across `eglTerminate`, including resources that
remain current. The ArmOS pipe loader selects the concrete Gallium provider
from the render node's negotiated capabilities and command set, so EGL
contains no VirGL, VirtIO, VC4 or architecture-specific selection logic.
Mesa generates static `libEGL.a` and `libGLESv2.a` targets for this platform.

`libwayland-egl.a` implements the conventional opaque `wl_egl_window`
contract. Configure dimensions are published with a generation-protected
snapshot so a render thread never observes a mixed width/height pair. The EGL
window backend owns a three-image swapchain. Each image is exported as a BO
capability descriptor, imported by the compositor and committed with an
acquire-fence descriptor. An image is not reused until the compositor reports
an immediate release or its release fence signals. Resize allocates images for
the new dimensions only after old images become reusable; it never mutates a
busy image in place.

Image layout metadata is published once, after Gallium has finalized the
stride and before the BO is exported. Width, height, stride and format are
then immutable, so every importer validates and observes the same layout.

The compositor now has two content paths behind the same surface lifecycle.
SHM surfaces are retained in compositor memory and uploaded to cached GPU
images only when their committed generation changes. Imported GPU buffers
still use the validated device-to-CPU transfer path pending direct image
attachment. Both paths preserve the same acquire/release fence and swapchain
ownership contract.

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

The compositor first tries the architecture-neutral GPU provider and card-node
contracts. When both are available, it composes cached surface images,
decorations, resize outlines and the pointer into provider-owned scanout
resources, then presents those resources without a CPU framebuffer copy. If
the provider is absent it falls back to the CPU-mappable DRM path, and then to
the framebuffer backend. The common compositor contains no VirtIO, VirGL,
VC4, V3D or architecture conditionals.

The production GPU-composition boundary is now explicit. The compositor core
knows only `gpu_backend.h`: output creation, image import/allocation, damaged
SHM upload, solid fill, clipped blit and explicit flush fences. The Mesa-side
provider implements this contract over ArmGL and is emitted as
`libarmos-wlcomp-gpu.a`; concrete Gallium provider selection remains behind
the render-node command-set negotiation. A build without that provider keeps
the same compositor sources and selects the software renderer. This separation
also gives the future VC4/V3D provider the same lifecycle without introducing
Raspberry Pi conditionals in common compositor code.

The provider is linked in a second, explicit build stage after Mesa. Its
factory is retained deliberately from the static archive rather than relying
on a weak reference to pull it from the linker. Software-only profiles still
link the base compositor without Mesa. `gpu_present.c` owns the other side of
the boundary: it exports the provider output, ends command generation, exposes
the independently owned render fence to the Wayland event loop and imports and
presents the resulting BO only when that fence becomes readable. It has no
knowledge of the selected command set or platform.

Provider frames follow one strict lifecycle: begin, draw, flush/export, end,
asynchronous fence notification and import/present. With three provider-owned
outputs the compositor permits at most two submitted frames, preserving one
resource which is neither being rendered nor awaiting presentation. Damage is
retained while all submission slots are occupied instead of polling or
blocking client/input dispatch. Several provider outputs may therefore be in
flight, but the event loop arms only the oldest fence and the presenter rejects
out-of-order completion. Scanout order is deterministic even when several
fences become readable during one dispatch turn. The presenter imports each
output lazily and
retains exactly one card-node handle per slot, so rendering never overwrites
the resource currently scanned out. A failed frame still closes every shared
descriptor and ends command generation exactly once; no half-open frame can
poison the next submission.

`begin_frame` reports the selected output slot before rendering. The common
tile-damage bitmap is retained independently for every slot: damage is added
to all outputs and cleared only from the output that was successfully
presented. Dirty 32x32 tiles are merged into horizontal runs. Each run is
cleared and replayed with clipping, while a fully covered opaque run starts at
its topmost covering surface. This preserves triple-buffer contents without
full-screen redraws and keeps the established conservative occlusion rules.

ArmGL output resources may now request scanout at creation. VirGL translates
that generic bind into a scanout-capable 3D resource, while CPU-created legacy
scanouts remain 2D resources. Presentation of a GPU render target flushes and
selects the resource directly; it must not issue the CPU upload command used by
the legacy path. `armgl-import-smoke` covers external import, rectangle blit,
alpha blending, CPU damage upload, solid fill, render-target rebinding and an
exportable scanout resource. Activation in `armos-wlcomp` follows only after
all scene layers use this contract, so z-order, pointer and decorations never
fall into a partial hybrid renderer.

Committed EGL client buffers now remain GPU resources throughout composition.
The compositor exports the client BO through the common DRM sharing contract,
imports it once through `gpu_backend.h`, retains that image for the lifetime of
the Wayland buffer and samples it directly for every damaged tile. It does not
map or read the BO on the CPU when import succeeds. Server-side decorations
remain a separate cached layer, so window policy and client content do not
become coupled to a particular GPU provider.

Import failure has one explicit compatibility path: a BO advertised as
CPU-readable may be mapped and composed by the software renderer. A GPU-only
BO which is not importable is rejected instead of silently presenting stale
content. Acquire fences are consumed before the imported image is sampled.
The compositor waits for its composition fence through the event loop rather
than inside the render call. Every directly sampled client BO records the most
recent composition fence. When a surface replaces that buffer, the compositor
emits `fenced_release` instead of claiming an immediate release; the EGL
swapchain may reuse the image only after the fence signals. A buffer which was
not sampled asynchronously keeps the explicit immediate-release path.

`armos-wlcomp --profile` reports `gpu_imports_total` and
`gpu_direct_blits`, plus `gpu_fenced_releases` and
`gpu_immediate_releases`. Imports are cumulative; the other counters cover one
profiling interval. A running EGL client should produce direct blits and fenced
releases without increasing CPU transfer traffic. Immediate releases remain
valid for buffers which were never sampled by an asynchronous GPU frame.

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
./tools/build_qemu_10_0_2.sh --install-deps
```

The optional installation step provisions the supported Homebrew or apt
packages. Without it, the build remains read-only with respect to the host and
reports every missing tool or pkg-config module. Both macOS and Linux builds
use SDL2, OpenGL, libepoxy and virglrenderer; the build deliberately disables
Nettle so an unrelated or incompatible host Nettle release cannot change the
configuration. See [QEMU 10.0.2 VirGL host](QEMU_VIRGL_HOST.md) for the exact
contract.

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
egl-smoke
armgl-import-smoke
armgl-compositor-smoke
egl-wayland-smoke
```

This milestone makes genuine VirGL contexts, typed resources, explicit
transfers and asynchronous fences available to userland, and validates them
with off-screen draw/readback, cross-context image import and visible Wayland
swapchain tests. A successful external-image test prints:

```text
armgl-import-smoke: import, upload, fill, alpha and render target verified
armgl-compositor-smoke: three GPU buffers exported, fenced and presented
```

The compositor pipeline smoke validates a complete provider frame through
cross-node BO sharing and copy-free presentation. `armos-wlcomp` then validates
production GPU composition: cached SHM layers, decorations, pointer, per-output
damage, clipping and conservative occlusion all pass through the same provider
transaction.

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

1. The minimal Mesa and EGL/Wayland milestone is complete for ARM64
   qemu-virt: Gallium VirGL, surfaceless and Wayland EGL, and OpenGL ES 2 are
   built as static target libraries.
   Mesa's `struct virgl_winsys` is
   implemented by `userland/opt/mesa/armos/virgl_armos_winsys.c` on top of
   `libarmos-virgl-winsys`, with typed resources, mapping, transfers, command
   relocation references and native ArmOS fence lifetimes. The common ArmGL
   frontend supplies GLES state-tracker contexts and pbuffer attachments
   without depending on the selected GPU. The EGL adapter owns Mesa object
   lifetime and exposes GLES2 without introducing a hardware dependency.
   These layers are compiled against Mesa 25.3.6 headers with the
   ARM32 and ARM64 target compilers by `tools/check_mesa_virgl_adapter.sh`.
   `tools/build_mesa.sh` produces the reproducible `/opt/mesa` bundle, the
   architecture-neutral `egl-smoke` off-screen triangle/readback executable
   and the visible, frame-paced `egl-wayland-smoke` swapchain executable.
Mesa's VirGL winsys now maps Gallium FD handles to the common BO
export/import descriptors and maps Gallium fence export to the pollable
common fence descriptor. This provides the backend-neutral transport needed
by an EGL swapchain without making raw DRM handles process-global.
Host VirGL capsets are filtered at this winsys boundary before Gallium sees
them.  The effective capset is the intersection of renderer and ArmOS winsys
features: until ArmOS implements command-buffer encoded transfers, the
encoded and copy-transfer bits are cleared and Mesa uses the supported
`transfer_put`/`transfer_get` path.  Hardware capsets remain unmodified in the
common DRM core and in the VirtIO backend.
   Mesa's generator dependencies are isolated by
   `tools/bootstrap_mesa_host_tools.sh`, which pins M4, Bison, Meson, Ninja and
   the Python modules instead of inheriting incompatible host installations.
   Run `egl-smoke` under VirGL to validate EGL initialization, shader
   compilation, drawing, fence completion and readback. Run
   `egl-wayland-smoke` to validate `wl_egl_window`, BO capability transfer,
   configure-driven resize, acquire/release synchronization, frame callbacks
   and visible presentation. Run `armgl-import-smoke` to validate that an
   exported scanout-capable image can be imported by an independent Gallium
   screen, sampled with a rectangle blit, alpha-composited, rebound as a
   render target and modified without a CPU copy. The diagnostic uses an
   asymmetric image so an
   inverted vertical coordinate contract cannot pass unnoticed. The imported
   layout is checked against immutable common BO metadata before the backend
   attaches the resource. ARM32 source and ABI compilation is validated;
   the complete Mesa bundle and runtime remain required before enabling the
   option in the tracked ARM32 profile.
2. Complete release-fence propagation. Direct client BO import,
   acquire-fence consumption, event-driven compositor render-fence waits and
   per-buffer fenced release are complete. Output BO reuse remains owned by the
   presenter transaction; clients are never given an output/scanout fence for
   an image that is only sampled as composition input.
   Startup profiling attributed the remaining initial desktop delay to
   `fork()` after Mesa had populated the compositor address space, not to
   renderer initialization or Foot. The architecture-neutral `armos_spawnve`
   contract now loads the shell and initial terminal into fresh address spaces
   and publishes each child only after its ELF image, descriptors, credentials,
   working directory, signals and stack are complete. It is common to ARM32 and
   ARM64 and has no dependency on Wayland or the selected GPU backend.
3. Port Raylib on the common EGL/OpenGL ES 2 path. The pinned Raylib 6.0
   bundle, first-party Wayland platform backend and `raylib-smoke` client now
   compile and reach a VirGL-backed GLES context. The backend handles XDG
   configure/close, resize, pointer, XKB keyboard input and EGL presentation
   without hardware-specific APIs. `raypot-demo` now renders the shared Utah
   teapot model through real VBOs, depth testing and custom GLES2 shaders. The
   platform VirtIO-GPU backend canonicalizes untyped PIPE_BUFFER descriptors,
   while the common winsys keeps submitted resources attached until their
   asynchronous fence signals. This validates the complete Raylib frame and
   VBO lifecycle without adding an application-specific GPU path.
4. Implement Raspberry Pi VC4 scanout management and V3D rendering as a
   platform backend behind the same common contracts.

Every milestone must retain the same UAPI on ARM32 and ARM64. Unsupported
operations fail through capability checks rather than compile-time
application variants.
