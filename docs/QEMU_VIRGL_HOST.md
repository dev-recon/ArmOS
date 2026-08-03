# QEMU 10.0.2 VirGL host build

ArmOS uses a repository-local QEMU 10.0.2 for reproducible ARM32 and ARM64
graphics validation. The supported build enables the SDL2 display backend,
OpenGL and `virtio-gpu-gl-device` through virglrenderer. It does not reuse an
arbitrary system QEMU.

## One-command setup

On macOS with Homebrew, or on an apt-based Linux host, build the reference
emulator with:

```sh
./tools/build_qemu_10_0_2.sh
```

Missing SDL/OpenGL/VirGL prerequisites are installed automatically. Use
`--no-install-deps` when the host must remain unchanged and missing packages
should instead produce a diagnostic.

For a host-independent Linux build through the ArmOS container, use:

```sh
./tools/container/build.sh --qemu
```

PowerShell uses `tools\container\build.ps1 -Qemu`. Container QEMU artifacts
live under `build/host-tools/qemu/linux-<architecture>/`; they never replace
the native host prefix described below.

The resulting binaries and matching firmware resources are installed under:

```text
build/qemu-10.0.2/install/bin/
build/qemu-10.0.2/install/share/qemu/
```

The boot scripts prefer this prefix automatically. Host packages never enter
`build/<arch>/<platform>`, a target sysroot or an ArmOS disk image.

## Dependency contract

The bootstrap installs or validates:

- common build tools: C compiler, Make, Ninja, Python 3, pkg-config, Git,
  Patch, Curl, Tar and XZ;
- QEMU base libraries: GLib, Pixman and libslirp;
- accelerated display libraries: SDL2, libepoxy and virglrenderer.

Homebrew owns these packages on macOS. On Debian and Ubuntu the bootstrap uses
apt and the corresponding development packages. Other Linux distributions can
run the check, then provide the reported modules through their package manager:

```sh
./tools/bootstrap_qemu_10_0_2_host_deps.sh --check
```

macOS additionally requires the repository patch
`tools/patches/virglrenderer-1.3.0/0001-macos-core-profile-blitter-glsl.patch`
in the source used to build virglrenderer 1.3.0. Apple provides OpenGL 3.2+
only as a core profile, while the upstream internal blitter embeds GLSL 1.30.
The patch selects GLSL 1.50 core for all desktop blitter variants without
changing GLES or non-Apple builds. The prerequisite check distinguishes the
single adaptive guest-shader GLSL 1.30 string retained by upstream from the
additional incompatible `#version 130` strings embedded by the internal
blitter, and rejects a library that still contains the latter.
Rebuilding QEMU alone is insufficient because QEMU links virglrenderer as a
host dynamic library.

The QEMU configuration disables Nettle, SPICE, GTK and Cocoa intentionally.
ArmOS VirGL uses one tested SDL/OpenGL path on every supported host; disabling
Nettle also prevents incompatible host versions from being selected merely
because they happen to be installed.

## Validation and execution

The build fails unless both installed system emulators report the SDL backend
and `virtio-gpu-gl-device`, and unless the matching `efi-virtio.rom` resource
was installed. Boot the accelerated target with:

```sh
QEMU_GPU_ACCEL=virgl TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt \
    ./boot-graphics.sh
```

Inside ArmOS, `drm-info` must report `command-set: virgl2`; `virgl-smoke` then
validates the capset, a 3D submission, its asynchronous fence and pixel
readback. A build or boot failure must not silently fall back while claiming
VirGL acceleration.

## Nested Linux pointer input

When the Linux host is itself a Parallels guest, the nested SDL window can
receive clicks without receiving relative trackpad movement. Configure
Parallels to **Optimize for games: Always**, then start ArmOS through X11 with
the relative VirtIO mouse:

```sh
SDL_VIDEODRIVER=x11 \
QEMU_DISPLAY=sdl,show-cursor=off \
QEMU_POINTER_DEVICE=virtio-mouse-device,event_idx=off,indirect_desc=off \
QEMU_GPU_ACCEL=virgl TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt \
./boot-graphics.sh
```

This changes only host event transport. ArmOS continues to consume the common
VirtIO input event interface, with no platform-specific policy in the kernel.
