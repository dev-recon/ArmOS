# ArmOS macOS Installation

This guide sets up ArmOS on macOS. The project is primarily developed on Apple
Silicon, but Intel Homebrew paths are noted where they differ.

ArmOS supports ARM32 and ARM64. `arm32/qemu-virt` is the fresh-checkout default,
and `arm64/qemu-virt` is the 64-bit feature reference. Supported hardware is
Raspberry Pi 3 Model B+ in AArch64 mode and Raspberry Pi 2 Model B v1.1 in
ARMv7-A mode; see [docs/RASPBERRY_PI3.md](docs/RASPBERRY_PI3.md) and
[docs/RASPBERRY_PI2.md](docs/RASPBERRY_PI2.md).

The reproducible 0.7.1 emulator baseline is QEMU 10.0.2. Newer releases can be
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

## 1. Install Homebrew

If Homebrew is not installed yet:

```sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

Make sure Homebrew is available in your shell:

```sh
eval "$(/opt/homebrew/bin/brew shellenv)"
```

On Intel Macs, Homebrew usually lives under `/usr/local` instead of
`/opt/homebrew`.

## 2. Install Required Tools

```sh
brew install make
brew install git
brew install python
brew install arm-none-eabi-gcc
brew install aarch64-elf-gcc
brew install qemu
brew install mtools
brew install dosfstools
brew install e2fsprogs
brew install curl
brew install xz
brew install bison flex
brew install glib pixman ninja pkg-config
```

Tool purpose:

- `make`: GNU make used by the root, libc, kernel, and userland Makefiles
- `git`: source checkout and QEMU fallback subproject retrieval
- `python`: host Python used by ArmOS image tools and QEMU's build system
- `arm-none-eabi-gcc`: ARM bare-metal compiler, assembler, linker, and binutils
- `aarch64-elf-gcc`: AArch64 bare-metal compiler and binutils used by ARM64
  kernels, newlib and userland
- `qemu`: `qemu-system-arm`
- `mtools`: FAT image manipulation (`mcopy`, `mmd`, `mdir`)
- `dosfstools`: FAT image creation (`mkfs.fat`)
- `e2fsprogs`: ext2 tools (`mke2fs`, `debugfs`, `e2fsck`)
- `curl`: optional newlib source download when the local archive is absent
- `xz`: archive support for some upstream source packages
- `bison`, `flex`: host-side parser and lexer generators required by the
  complete BSD userland and other imported source bundles
- `glib`, `pixman`, `ninja`, `pkg-config`: host dependencies for the exact
  repo-local QEMU 10.0.2 build

`brew install qemu` installs Homebrew's current QEMU release, not necessarily
10.0.2. Keep it as a convenient fallback, or use the pinned build below.

## 3. Configure PATH

`run.sh` prepends common Homebrew locations, including the `e2fsprogs` sbin
directory. For an interactive shell, this is still useful:

```sh
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:$PATH"
export PATH="/opt/homebrew/opt/e2fsprogs/sbin:$PATH"
export PATH="/opt/homebrew/opt/bison/bin:$PATH"
```

For Intel Homebrew:

```sh
export PATH="/usr/local/bin:/usr/local/sbin:$PATH"
export PATH="/usr/local/opt/e2fsprogs/sbin:$PATH"
export PATH="/usr/local/opt/bison/bin:$PATH"
```

To make it permanent, add the relevant lines to `~/.zshrc`.

## 4. Clone The Repository

```sh
git clone https://github.com/dev-recon/ArmOS.git arm-os
cd arm-os
chmod +x run.sh boot.sh boot-graphics.sh tools/build_newlib.sh \
  tools/build_qemu_10_0_2.sh
```

The remaining commands in this guide are run from the repository root.

## 5. Install Exactly QEMU 10.0.2

The reliable method is to build the official source release in an isolated,
repo-local prefix:

```sh
./tools/build_qemu_10_0_2.sh
```

The script downloads the [official QEMU 10.0.2 source archive](https://download.qemu.org/qemu-10.0.2.tar.xz),
checks its pinned SHA-256, builds `arm-softmmu` and `aarch64-softmmu`, and
installs the binaries under:

```text
build/qemu-10.0.2/install/bin/qemu-system-arm
build/qemu-10.0.2/install/bin/qemu-system-aarch64
```

ArmOS boot scripts automatically prefer that binary. Homebrew's `brew pin`
only prevents upgrades after a version has been installed; it cannot install
10.0.2 when the formula history no longer exposes it. `brew extract` can create
a historical formula in a personal tap, but that formula is user-maintained and
less reproducible than the source build. See the
[Homebrew versioning documentation](https://docs.brew.sh/Versions).

## 6. Verify The Toolchain

From the repository root:

```sh
make --version
arm-none-eabi-gcc --version
arm-none-eabi-ld --version
arm-none-eabi-objcopy --version
aarch64-elf-gcc --version
aarch64-elf-objcopy --version
bison --version
flex --version
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

The scripts also accept an explicit QEMU binary through `QEMU=/path/to/qemu-system-arm`
or as their first argument. Explicit selection takes precedence over the
repo-local pinned build.

If `mke2fs`, `debugfs`, or `e2fsck` are not found, your shell is missing the
`e2fsprogs` sbin path.

## 7. Build And Run

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

## 8. Useful Commands

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
$(brew --prefix e2fsprogs)/sbin/e2fsck -fn ext2.img
```

Boot without rebuilding:

```sh
./boot.sh
```

## 9. Optional Newlib Build

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

## 10. Adding Userland Programs

User programs live under `userland/` and are installed into the generated
`userfs` tree.

For newlib-linked tools, the current convention is:

- shell/init sources under `userland/system/`
- system tool sources under `userland/system/tools/`
- core utility sources under `userland/coreutils/src/`
- ArmOS program sources under `userland/programs/<name>/`
- imported external tool sources under `userland/opt/<name>/src/`
- system programs under `userfs/sbin`
- core utilities under `userfs/bin`
- ArmOS user programs under `userfs/usr/bin`
- imported external tools under `userfs/opt/<name>/bin`

The root filesystem is ext2, so long filenames are supported there. FAT32 is
still available under `/mnt`, but it is intentionally a smaller compatibility
filesystem.

## 11. Native TinyCC And Shipped Sources

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

## 12. Optional ncurses And nano

ArmOS 0.7.1 can also stage static ncurses and nano bundles:

```sh
BUILD_NCURSES=1 BUILD_NANO=1 ./build.sh
```

This downloads/builds ncurses and nano into generated bundle directories and
stages them into:

```text
/opt/ncurses
/opt/nano
/usr/bin/cursestest
```

These bundles are optional generated artifacts and are intentionally ignored by
Git. Use them for terminal UI experiments, not as a replacement for the normal
host cross-build workflow.

Build the complete optional userland for the configured ABI with:

```sh
BUILD_ALL_USERLAND=1 ./build.sh
```

This includes TinyCC, ncurses, nano, the BSD tools and the supported graphics
libraries. Rebuilding it is unnecessary for a kernel-only iteration.

## 13. Raspberry Pi Images

Use the tracked hardware profiles and the generic SD builder:

```sh
ARMOS_CONFIG=configs/raspi2-arm32.conf \
  tools/build_raspberry_sd.sh --mode image
ARMOS_CONFIG=configs/raspi3-arm64.conf \
  tools/build_raspberry_sd.sh --mode image
```

For raw-device writes, identify the whole removable disk with `diskutil list`
and follow [the Raspberry Pi 2 guide](docs/RASPBERRY_PI2.md) or
[the Raspberry Pi 3 guide](docs/RASPBERRY_PI3.md). Raw mode is destructive;
normal kernel iteration should use boot-only mode once the complete card has
been initialized.

## 14. Common Problems

### `arm-none-eabi-gcc: command not found`

Install the cross compiler and reopen your shell:

```sh
brew install arm-none-eabi-gcc
which arm-none-eabi-gcc
```

### `bison` or `flex` not found

Install both host-side generators, then restart the same incremental build:

```sh
brew install bison flex
export PATH="$(brew --prefix bison)/bin:$PATH"
```

No clean rebuild is required.

### `mke2fs` or `debugfs` not found

Install `e2fsprogs` and add its sbin directory to `PATH`:

```sh
brew install e2fsprogs
export PATH="/opt/homebrew/opt/e2fsprogs/sbin:$PATH"
```

On Intel Homebrew:

```sh
export PATH="/usr/local/opt/e2fsprogs/sbin:$PATH"
```

### FAT tools not found

If `mkfs.fat`, `mcopy`, `mmd`, or `mdir` are missing:

```sh
brew install dosfstools mtools
```

### QEMU starts but the terminal looks stuck

ArmOS runs QEMU with `-nographic`, so QEMU owns the terminal while it is
running.

Exit with:

```text
Ctrl+A, then X
```

### `make clean` does not remove disk images

`make clean` removes kernel objects and binaries. To force fresh disk images:

```sh
rm -f disk.img ext2.img fat32.img build/images/disk-arm32-qemu-virt.img
TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt make platform-disk
```

`./run.sh` already removes and recreates these images before booting.
