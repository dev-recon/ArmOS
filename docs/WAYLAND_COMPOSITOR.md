# Wayland Compositor

ArmOS provides a small native Wayland stack intended to remain independent of
the CPU architecture and display controller. The compositor is a normal
userland process; display, input, shared memory, descriptor passing and process
isolation remain common kernel services.

## Architecture

The graphical session is split into three layers:

```text
common kernel services
  AF_ARMOS, SCM_RIGHTS, poll, shm/mmap, input, framebuffer
                         |
                         v
kernel windowserverd launcher
                         |
                         v
/sbin/armos-wlcomp
  Wayland protocol, window policy, software renderer
                         |
                         v
VirtIO-GPU framebuffer or Raspberry Pi firmware HDMI framebuffer
```

`windowserverd` is a small kernel task responsible only for starting and
reaping the privileged compositor. It executes `/sbin/armos-wlcomp` in a
regular user address space. The compositor starts `/usr/bin/foot` as the
initial unprivileged graphical terminal. If the graphical session terminates,
the kernel launcher restores console ownership but does not automatically
restart the compositor.

Platform and architecture code provide framebuffer and input backends only.
Wayland protocol handling, focus, stacking, decorations, clipping and layout
policy stay in userland.

## Implemented Protocol Surface

The current server publishes:

- `wl_compositor`, `wl_surface`, regions and frame callbacks;
- `wl_shm`, growable pools and subregion-backed buffers;
- `wl_seat` with pointer and keyboard input;
- `wl_output` and `zxdg_output_manager_v1`;
- `xdg_wm_base`, `xdg_surface` and `xdg_toplevel`;
- `zxdg_decoration_manager_v1` for server-side decorations;
- `wl_subcompositor` and subsurfaces;
- `wl_data_device_manager` for the current selection path.

ArmOS also supplies native `libwayland-client`, `libwayland-server` and
`libwayland-cursor` compatibility libraries. They are sufficient for the
native demos and the current Foot 1.9.2 port.

## Window System

`armos-wlcomp` currently implements:

- movable, clipped and stacked top-level windows;
- click-to-focus with an active blue title bar;
- server-side close controls and client lifecycle isolation;
- software cursors, keyboard focus and runtime keyboard-layout updates;
- a 32x32 dirty-tile bitmap spanning client damage through presentation;
- tile-local composition, with lower layers skipped only when an opaque
  surface completely covers the tile;
- coalesced rectangular tile presentation to limit framebuffer operations;
- a software renderer shared by QEMU and Raspberry Pi framebuffer targets.

The `teapot-demo` client exercises continuous SHM updates and keyboard input.
Foot provides a real terminal workload and can run GNU nano and ordinary ArmOS
programs inside a Wayland window.

## Build And Runtime

The complete graphical bundle is selected through the target configuration.
Foot requires its font and rendering dependency chain:

```text
BUILD_EPOLL_SHIM=yes
BUILD_UTF8PROC=yes
BUILD_FREETYPE=yes
BUILD_EXPAT=yes
BUILD_FONTCONFIG=yes
BUILD_HARFBUZZ=yes
BUILD_FCFT=yes
BUILD_FOOT=yes
BUILD_NCURSES=yes
BUILD_NANO=yes
```

Nano is installed below `/opt/nano` and exposed as `/usr/bin/nano`. Foot is
installed as `/usr/bin/foot`; the compositor itself is installed as
`/sbin/armos-wlcomp`.

For a manual compositor test:

```sh
su
armos-wlcomp
```

The normal graphical boot path starts the compositor and Foot automatically
when the platform reports a usable display and permits the compositor.

## Platform Status

- `arm64/qemu-virt` is the primary implementation and regression target.
- `arm32/qemu-virt` exercises the same protocol and renderer code.
- Raspberry Pi 3 AArch64 uses the same compositor through the firmware HDMI
  framebuffer and DWC2 USB input.
- Raspberry Pi console and UART recovery remain available independently of
  Wayland. The `fr-legacy` USB layout is not affected by the QEMU `fr-mac`
  Option-key policy.

The Raspberry Pi path is functional but remains limited by software
composition and framebuffer bandwidth. Current optimizations use surface
damage, tile-local composition, opaque-span copies, partial presentation and
batched cache maintenance.

Framebuffer presentation now uses one common userspace contract:

- the compositor acquires `/dev/fb0`, queries its scanout buffers and maps
  them once;
- composition writes directly into a mapped buffer, so presentation no longer
  copies pixels through `copy_from_user_2d`;
- the Raspberry Pi HDMI backend exposes the firmware's two virtual pages and
  presents a completed back buffer with a virtual-offset page flip;
- the QEMU VirtIO-GPU path exposes one mapped resource and retains its
  device-specific transfer/flush operation behind the same present request;
- per-buffer dirty-tile history ensures that a reused back buffer receives all
  damage accumulated since it was last displayed.

The mapping lifecycle and ownership checks live in the common framebuffer and
virtual-memory code. Firmware offsets and cache maintenance remain confined to
the Raspberry Pi HDMI backend; VirtIO commands remain confined to the QEMU
backend.

## Next Milestones

The software renderer isolates solid fill, opaque copy, alpha blend and
framebuffer presentation behind small common primitives. This keeps scene and
Wayland policy readable while allowing measured targets to substitute SIMD,
DMA or device-specific presentation later. Client ARGB surfaces detect fully
opaque spans and tiles: opaque spans are copied directly, and adjacent tiles
with the same visibility are composed as one horizontal run instead of
invoking the complete renderer for every 32x32 area.

The next performance milestone is to measure the remaining time separately in
surface upload, tile composition and backend presentation after mapped
presentation. Large Raspberry Pi operations may then use DMA above a measured
size threshold, with cache maintenance and a CPU fallback kept inside the
platform backend. QEMU should instead batch VirtIO-GPU transfer and flush
rectangles.

Native VC4/V3D composition is a later backend milestone. It must not move
Wayland policy into Raspberry-specific code: the common compositor will retain
window management and protocol ownership while a renderer/backend interface
selects software, VirtIO-GPU or Raspberry Pi GPU presentation.

Further protocol work includes stronger clipboard interoperability, richer
shell policy, resize interactions, additional input devices and broader
application compatibility beyond Foot.
