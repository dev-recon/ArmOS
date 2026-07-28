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
- damage tracking for surfaces, window movement and framebuffer presentation;
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
damage, partial presentation, word-at-a-time memory operations and batched
cache maintenance.

## Next Milestones

The next performance milestone is to verify that damage remains bounded from
client commit through final framebuffer presentation, then add architecture
optimized copy and fill primitives where measurements justify them. This work
is useful for both the software renderer and a future GPU backend.

Native VC4/V3D composition is a later backend milestone. It must not move
Wayland policy into Raspberry-specific code: the common compositor will retain
window management and protocol ownership while a renderer/backend interface
selects software, VirtIO-GPU or Raspberry Pi GPU presentation.

Further protocol work includes stronger clipboard interoperability, richer
shell policy, resize interactions, additional input devices and broader
application compatibility beyond Foot.
