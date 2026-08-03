# Containerized ArmOS Build

The ArmOS build container makes the Linux build contract reproducible on
Windows 11, Linux, and macOS. It contains the complete host dependency set and
pinned ARM GNU Toolchain 15.2.Rel1 compilers for both ARM32 and ARM64.

The image supports native `linux/amd64` and `linux/arm64` hosts. Windows 11
ARM64 therefore runs the AArch64 image directly through Docker Desktop and
WSL2 rather than emulating an x86 compiler.

## Windows 11 ARM64

Install Docker Desktop, enable its WSL2 backend, clone ArmOS, and open a
PowerShell terminal in the repository. Build ArmOS directly:

```powershell
.\tools\container\build.ps1
```

The first invocation creates the toolchain image automatically. Later builds
reuse it; `-BuildImage` explicitly refreshes it.

Select another target explicitly:

```powershell
.\tools\container\build.ps1 `
  -Config configs/raspi3-arm64.conf
```

Use `-Reconfigure` after changing a third-party configuration contract, or
`-Rebuild` to force a complete userland rebuild. Generated files remain in the
normal repository paths:

```text
build/<arch>/<platform>/
build/targets/<arch>-<platform>/
build/images/kernel-<arch>-<platform>.bin
build/images/disk-<arch>-<platform>.img
```

Keep the checkout in the WSL2 Linux filesystem for the best Docker Desktop
performance. A checkout under `C:\` also works, but large bundle builds incur
additional file-sharing overhead.

## Linux and macOS

The shell launcher follows the same contract:

```sh
./tools/container/build.sh --config configs/qemu-virt-arm64.conf
```

The image is created automatically when absent. `--build-image` forces a
refresh. On native Linux the launcher maps the host UID/GID into the container
so generated files do not become root-owned.

## QEMU

The same container can build the pinned QEMU 10.0.2 with SDL, OpenGL and
VirGL. The operation is explicit and cached, so ordinary ArmOS builds never
recompile QEMU:

```powershell
.\tools\container\build.ps1 -Qemu
```

```sh
./tools/container/build.sh --qemu
```

The result is isolated by container architecture:

```text
build/host-tools/qemu/linux-amd64/qemu-10.0.2/install/
build/host-tools/qemu/linux-arm64/qemu-10.0.2/install/
```

This prevents a Linux executable produced by Docker from replacing the native
QEMU under `build/qemu-10.0.2/install`. Repeating the command validates and
reuses the existing emulator unless its build script, patches or VirGL
dependency contract changed.

The produced emulator is a Linux program. It is directly usable for automated
tests inside the build container and from a matching Linux/WSL environment.
Native graphical windows still require a native host QEMU because Docker
Desktop does not provide a portable SDL/VirGL window transport on Windows and
macOS. Build that native copy with `tools/build_qemu_10_0_2.sh`; both copies can
coexist in the same checkout.

## Raspberry Pi SD cards

Windows does not need an ext2 filesystem driver. Raspberry Pi Imager or Etcher
writes the complete `disk-arm64-raspi3.img` or `disk-arm32-raspi2.img` file as
raw sectors, including its FAT32 and ext2 partitions. Windows may expose only
the FAT32 partition after flashing; this is expected.

Direct USB/SD passthrough is intentionally not enabled in the build container.
Docker Desktop requires USB/IP for such devices and a mistaken raw target can
destroy the host disk. Build in Docker, then flash the resulting image through
a host application that identifies removable devices and asks for explicit
confirmation.

## Reproducibility and caches

The container never keeps a private source copy. It mounts the checkout at
`/workspace`, and ArmOS continues to isolate target objects under
`build/<arch>/<platform>`. Existing compiler identity and bundle contracts
invalidate incompatible cached objects when a checkout moves between a host
toolchain and the container toolchain.

Local and container builds may therefore alternate in the same checkout.
When the host path or toolchain changes, ArmOS recreates only the affected
third-party configure directory; source archives, extracted sources, unrelated
bundles, kernels, and other target directories are preserved. No manual global
clean is required.

The first build after a configure-cache format update may recreate these
bundle build directories once. Later builds reuse them normally.

The Docker build verifies the official SHA-256 checksum of each downloaded
Arm GNU archive. Third-party ArmOS bundles retain their existing pinned source
versions and checksum validation.
