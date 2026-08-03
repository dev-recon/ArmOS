# ArmOS Linux Installation

This guide sets up ArmOS on Linux. Debian and Ubuntu are the primary reference
distributions for now.

ArmOS supports ARM32 and ARM64. `arm32/qemu-virt` is the fresh-checkout default,
and `arm64/qemu-virt` is the 64-bit feature reference. Supported hardware is
Raspberry Pi 3 Model B+ in AArch64 mode and Raspberry Pi 2 Model B v1.1 in
ARMv7-A mode; see [docs/RASPBERRY_PI3.md](docs/RASPBERRY_PI3.md) and
[docs/RASPBERRY_PI2.md](docs/RASPBERRY_PI2.md).

The reproducible 0.7.5 emulator baseline is QEMU 10.0.2. Newer releases can be
used for compatibility testing, but should not silently replace the baseline.

## Disk Layout

The build creates intermediate `ext2.img` and `fat32.img` files, then publishes
platform-specific artifacts under `build/images/`:

- `kernel-arm32-qemu-virt.bin` and `disk-arm32-qemu-virt.img`: default QEMU
  development images
- `kernel-arm64-qemu-virt.bin` and `disk-arm64-qemu-virt.img`: AArch64 QEMU
  development images; QEMU Virt disks use ext2 partition 1 and FAT32 partition 2
- `kernel-arm64-raspi3.bin` and `disk-arm64-raspi3.img`: Raspberry Pi 3
  hardware images; standard FAT32 boot is partition 1 and ext2 root is
  partition 2
- `kernel-arm32-raspi2.bin` and `disk-arm32-raspi2.img`: Raspberry Pi 2
  hardware images with the same boot/root partition order

Generated images are build artifacts and should not be committed.

## 1. Install Required Packages

On Debian/Ubuntu:

```sh
sudo apt update
sudo apt install -y \
  build-essential \
  make \
  git \
  file \
  procps \
  curl \
  xz-utils \
  bison \
  flex \
  gperf \
  ncurses-bin \
  libc++-dev \
  gcc-arm-none-eabi \
  binutils-arm-none-eabi \
  qemu-system-arm \
  qemu-utils \
  mtools \
  dosfstools \
  e2fsprogs \
  ninja-build \
  pkg-config \
  python3-venv \
  libglib2.0-dev \
  libpixman-1-dev \
  libsdl2-dev \
  libepoxy-dev \
  libvirglrenderer-dev \
  libslirp-dev \
  zlib1g-dev
```

Tool purpose:

- `build-essential`, `make`, `file`, `procps`: host build and Homebrew bootstrap
  tools
- `gcc-arm-none-eabi`, `binutils-arm-none-eabi`: ARM bare-metal toolchain
- `qemu-system-arm`, `qemu-utils`: emulator and disk tooling
- `mtools`: FAT image manipulation (`mcopy`, `mmd`, `mdir`)
- `dosfstools`: FAT image creation (`mkfs.fat`)
- `e2fsprogs`: ext2 tools (`mke2fs`, `debugfs`, `e2fsck`)
- `curl`, `xz-utils`: optional source package download/extraction helpers
- `bison`, `flex`, `gperf`: host-side parser, lexer and perfect-hash generators
  required by the complete BSD userland, Fontconfig and other imported bundles
- `ncurses-bin`: supplies the host `tic` compiler used by the ncurses bundle
- `libc++-dev`: build-only C++ headers used to compile the HarfBuzz amalgamation;
  ArmOS still exposes HarfBuzz through its C API and installs no C++ runtime
- `ninja-build`, `pkg-config`, `python3-venv`, `libglib2.0-dev`,
  `libpixman-1-dev`, `libslirp-dev`, `zlib1g-dev`: core host dependencies for
  the exact QEMU 10.0.2 build
- `libsdl2-dev`, `libepoxy-dev`, `libvirglrenderer-dev`: SDL/OpenGL/VirGL
  graphics path used by accelerated `qemu-virt` sessions

The package list above is sufficient for the default ARM32 route. ARM64 builds
also require a bare-metal toolchain exposing the `aarch64-elf-` prefix,
including `gcc`, `ld`, `objcopy`, `objdump`, `readelf` and `nm`. Do not silently
substitute an `aarch64-linux-gnu-` compiler: the ArmOS userland targets newlib,
not the Linux ABI.

## 2. Optional Homebrew Fallback

Use the distribution packages when they are available. If a required package
is missing, too old, or does not expose the expected tools, Homebrew is a
supported fallback on Linux. Install it with the official installer:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

The default Linux prefix is `/home/linuxbrew/.linuxbrew`. Activate it in the
current shell and make it persistent for Bash:

```sh
eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
echo 'eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"' >> ~/.bashrc
```

For Zsh, write the same line to `~/.zshrc` instead. See the official
[Homebrew on Linux documentation](https://docs.brew.sh/Homebrew-on-Linux) for
other distributions and non-default shells.

For example, install `e2fsprogs` when `mke2fs` or `debugfs` is unavailable from
the distribution:

```sh
brew install e2fsprogs
export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"
echo 'export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"' >> ~/.bashrc
```

Use `~/.zshrc` instead of `~/.bashrc` for Zsh. `build.sh` and the root Makefile
also discover the Homebrew `e2fsprogs` prefix automatically, but adding its
`sbin` directory to `PATH` makes the verification commands below work directly.

Where the distribution does not package `aarch64-elf-gcc`, install the
[Homebrew formula](https://formulae.brew.sh/formula/aarch64-elf-gcc):

```sh
brew install aarch64-elf-gcc bison flex gperf
brew install llvm
```

When Bison comes from Homebrew, ensure its executable directory precedes an
older distribution copy:

```sh
export PATH="$(brew --prefix bison)/bin:$PATH"
```

## 3. Clone The Repository

The pinned QEMU builder and the ArmOS build scripts live in the repository, so
clone it before running any command under `tools/`:

```sh
git clone https://github.com/dev-recon/ArmOS.git arm-os
cd arm-os
chmod +x run.sh boot.sh boot-graphics.sh tools/build_newlib.sh \
  tools/build_qemu_10_0_2.sh
```

All remaining commands in this guide are run from this `arm-os` directory.

## 4. Install Exactly QEMU 10.0.2

The distro package installs the version carried by the configured Debian or
Ubuntu release. The reliable cross-distribution method is an isolated source
build:

```sh
./tools/build_qemu_10_0_2.sh
```

Missing supported apt prerequisites are installed automatically. Pass
`--no-install-deps` to fail without changing host packages.

The script downloads the [official QEMU 10.0.2 source archive](https://download.qemu.org/qemu-10.0.2.tar.xz),
checks its pinned SHA-256, builds `arm-softmmu` and `aarch64-softmmu`, and
installs the binaries under:

```text
build/qemu-10.0.2/install/bin/qemu-system-arm
build/qemu-10.0.2/install/bin/qemu-system-aarch64
```

On Linux the script explicitly enables SDL, OpenGL and virglrenderer. It
refuses to install a headless or 2D-only reference build. Verify the available
display backend and accelerated VirtIO-GPU device with:

```sh
build/qemu-10.0.2/install/bin/qemu-system-arm -display help
build/qemu-10.0.2/install/bin/qemu-system-aarch64 -device help | \
  grep virtio-gpu-gl-device
```

The first output must contain `sdl`; the second command must report
`virtio-gpu-gl-device`. If an older repository-local QEMU exists without that
device, rerun the build command above after installing the VirGL packages.
Rebuilding ArmOS itself is not necessary.

ArmOS boot scripts automatically prefer that binary. APT can install an exact
package only if that package version is present in the configured repositories:

```sh
apt-cache madison qemu-system-arm
sudo apt install qemu-system-arm='EXACT_APT_VERSION'
sudo apt-mark hold qemu-system-arm
```

The APT version includes distro epoch/revision fields and usually does not map
to a portable `10.0.2` command, so the source build is the documented baseline.

## 5. Verify The Toolchain

```sh
make --version
arm-none-eabi-gcc --version
arm-none-eabi-ld --version
arm-none-eabi-objcopy --version
aarch64-elf-gcc --version        # required for ARM64 targets
aarch64-elf-objcopy --version
bison --version
flex --version
gperf --version
tic -V
test -f /usr/include/c++/v1/__config || \
  test -f "$(brew --prefix llvm)/include/c++/v1/__config"
qemu-system-arm --version
qemu-system-aarch64 --version
mkfs.fat -V
mcopy -V
mmd -V
mke2fs -V
debugfs -V
e2fsck -V
```

For a strict baseline run, make a version mismatch fatal:

```sh
QEMU_REQUIRED_VERSION=10.0.2 ./boot.sh
QEMU_REQUIRED_VERSION=10.0.2 ./boot-graphics.sh
```

### VirGL graphics session

Boot the ARM64 graphical reference target with:

```sh
SMP_CPUS=4 TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt \
GPU_XRES=1280 GPU_YRES=720 QEMU_GPU_ACCEL=virgl \
./boot-graphics.sh
```

### Mouse in a Linux virtual machine

When Ubuntu itself runs inside Parallels, a nested QEMU window may receive
button clicks but no pointer movement. This is a host-input issue: Parallels'
absolute pointer integration does not provide the relative deltas expected by
the nested SDL window.

Set **Parallels → Configuration → Hardware → Mouse & Keyboard → Optimize for
games** to **Always**, then force the tested X11/SDL relative-pointer path:

```sh
SDL_VIDEODRIVER=x11 \
QEMU_DISPLAY=sdl,show-cursor=off \
QEMU_POINTER_DEVICE=virtio-mouse-device,event_idx=off,indirect_desc=off \
SMP_CPUS=4 TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt \
GPU_XRES=1280 GPU_YRES=720 QEMU_GPU_ACCEL=virgl \
./boot-graphics.sh
```

This override is intended for nested Linux virtual machines. Native Linux can
keep the default `virtio-tablet-device` absolute-pointer path.

## 6. Build And Run

```sh
./run.sh
```

`run.sh` performs a full local rebuild:

1. rebuilds libc/userland pieces
2. rebuilds `build/images/kernel-arm32-qemu-virt.bin`
3. recreates `build/images/disk-arm32-qemu-virt.img`
4. boots QEMU

With no environment overrides, `run.sh` deliberately selects the stable
`TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt` route. Select ARM64 explicitly:

```sh
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./run.sh
```

For a persistent local target, create the ignored `armos.conf` file:

```sh
cp armos.conf.example armos.conf
./tools/armos_config.sh --show
./run.sh
```

Tracked profiles can be selected with `ARMOS_CONFIG`, for example
`ARMOS_CONFIG=configs/qemu-virt-arm64.conf ./run.sh`. See
[`docs/BUILD_CONFIGURATION.md`](docs/BUILD_CONFIGURATION.md) for precedence,
QEMU network/graphics options, and userland package switches.

At the `mash$>` prompt, a quick smoke test is:

```sh
systest
ps
ls -la /
ls -la /proc
hello
```

Exit QEMU with:

```text
Ctrl+A, then X
```

## 7. Useful Commands

Build kernel only:

```sh
TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt make platform-kernel
```

Rebuild disk images only:

```sh
rm -f disk.img ext2.img fat32.img build/images/disk-arm32-qemu-virt.img
TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt make platform-disk
```

Inspect generated filesystems:

```sh
make check-disk
```

Verify ext2 without modifying it:

```sh
e2fsck -fn ext2.img
```

Boot without rebuilding:

```sh
./boot.sh
```

## 8. Optional Newlib Build

Build the optional repo-local newlib sysroot and include newlib-linked test
programs:

```sh
./tools/build_newlib.sh
BUILD_NEWLIB=1 ./run.sh
```

The script uses the local `newlib-4.4.0.20231231.tar.gz` archive when present.
If the archive is missing, it downloads it with `curl` from Sourceware.

The installed sysroot lives in:

```text
build/newlib-sysroot/arm-none-eabi
build/newlib-sysroot/aarch64-elf
```

Select the AArch64 sysroot explicitly when rebuilding it alone:

```sh
TARGET=aarch64-elf ./tools/build_newlib.sh
```

Those directories are generated and should not be committed.

You can also point the build at another sysroot:

```sh
BUILD_NEWLIB=1 NEWLIB_SYSROOT=/path/to/arm-none-eabi ./run.sh
```

## 9. Native TinyCC And Shipped Sources

ArmOS 0.7.1 can build small C programs from inside ARM32 and ARM64 systems.
This is for end-user programming and experiments inside ArmOS; the project
itself still uses the macOS/Linux cross toolchain for kernel work,
stabilization, and release builds.

The standard build scripts stage TinyCC under:

```text
/opt/tcc
```

and install the user-facing wrapper:

```text
/usr/bin/tcc
```

The root filesystem also contains a userland source snapshot:

```text
/usr/src/armos/userland
```

Inside `mash`, try:

```sh
tcc /usr/src/armos/userland/coreutils/src/ls.c -o /tmp/ls-tcc
/tmp/ls-tcc /proc
```

Set `BUILD_TCC=0` when running `build.sh` or `run.sh` if you want to skip the
native TinyCC bundle during local development.

## 10. Optional ncurses And nano

ArmOS 0.7.1 can also stage static ncurses and nano bundles:

```sh
BUILD_NCURSES=1 BUILD_NANO=1 ./build.sh
```

This installs generated bundles under `/opt/ncurses` and `/opt/nano`, plus
`/usr/bin/cursestest` for a small runtime smoke test. These directories are
generated artifacts and are not committed to Git.

Build every supported optional userland component for the configured ABI with:

```sh
BUILD_ALL_USERLAND=1 ./build.sh
```

This includes TinyCC, ncurses, nano, the BSD tools and supported graphics
libraries. Kernel-only iterations should reuse these generated bundles.

## 11. Raspberry Pi Images

The generic SD-image builder accepts the same tracked profiles as QEMU builds:

```sh
ARMOS_CONFIG=configs/raspi2-arm32.conf \
  tools/build_raspberry_sd.sh --mode image
ARMOS_CONFIG=configs/raspi3-arm64.conf \
  tools/build_raspberry_sd.sh --mode image
```

Writing a complete image to a removable device is destructive. Confirm the
whole block device carefully, then follow
[the Raspberry Pi 2 guide](docs/RASPBERRY_PI2.md) or
[the Raspberry Pi 3 guide](docs/RASPBERRY_PI3.md). Use boot-only mode for
normal kernel updates after the first complete card initialization.

## 12. Common Problems

### `arm-none-eabi-gcc: command not found`

Install the cross compiler:

```sh
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi
```

Then verify:

```sh
which arm-none-eabi-gcc
```

### `bison`, `flex`, `gperf`, or `tic` not found

Complete userland profiles generate parsers and lexers for imported tools on
the host. Install both generators before restarting the incremental build:

```sh
sudo apt install bison flex gperf ncurses-bin
```

Already completed bundles remain cached; neither `make clean` nor
`./build.sh --rebuild` is required.

### LLVM libc++ headers not found

HarfBuzz is compiled from its upstream C++ amalgamation but exports only its C
API to ArmOS. Install the build-only libc++ headers, then resume the same build:

```sh
sudo apt install libc++-dev
```

With Homebrew, use `brew install llvm`. If LLVM uses a non-standard prefix,
pass it explicitly:

```sh
LIBCXX_INCLUDE="$(brew --prefix llvm)/include/c++/v1" \
  ARMOS_CONFIG=configs/qemu-virt-arm64.conf ./build.sh
```

### FAT tools not found

If `mkfs.fat`, `mcopy`, `mmd`, or `mdir` are missing:

```sh
sudo apt install dosfstools mtools
```

### Ext2 tools not found

If `mke2fs`, `debugfs`, or `e2fsck` are missing:

```sh
sudo apt install e2fsprogs
```

If the distribution package is unavailable or incomplete, follow
[the Homebrew fallback](#2-optional-homebrew-fallback). `run.sh` detects the
Homebrew formula automatically.

### QEMU starts but the terminal looks stuck

ArmOS runs QEMU with `-nographic`, so QEMU owns the terminal while it is
running.

Exit with:

```text
Ctrl+A, then X
```

### The graphical console does not open

Check whether the selected QEMU binary includes a window backend:

```sh
build/qemu-10.0.2/install/bin/qemu-system-arm -display help
```

If `gtk` is absent, install its development package and rebuild the pinned
emulator. The ArmOS build script now enables and verifies GTK explicitly:

```sh
sudo apt install libgtk-3-dev
./tools/build_qemu_10_0_2.sh
./boot-graphics.sh
```

`boot-graphics.sh` reports this condition directly instead of silently
selecting a headless display backend.

### Permission problems on scripts

```sh
chmod +x run.sh boot.sh tools/build_newlib.sh tools/build_qemu_10_0_2.sh
```

### Fresh disk images

To force fresh disk images:

```sh
rm -f disk.img ext2.img fat32.img build/images/disk-arm32-qemu-virt.img
TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt make platform-disk
```

`./run.sh` already removes and recreates these images before booting.
