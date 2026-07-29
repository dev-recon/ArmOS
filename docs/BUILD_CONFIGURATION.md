# ArmOS Build Configuration

ArmOS can select an architecture, a platform, QEMU features, and optional
userland packages from one local `armos.conf` file. Existing environment and
`make` command-line workflows remain supported.

## Quick Start

Create a local configuration from the example:

```sh
cp armos.conf.example armos.conf
```

The local `armos.conf` is ignored by Git. Inspect the resolved values with:

```sh
./tools/armos_config.sh --show
make config
```

Then use the normal entry points:

```sh
./run.sh
./build.sh
./boot.sh
./boot-graphics.sh
./boot-net.sh
```

Without a local file or environment overrides, the reference target remains
`arm32/qemu-virt`.

## Incremental Userland Builds

`build.sh` preserves userland objects below `build/<arch>/<platform>/`.
Compiler dependency files rebuild a source object when its C file or one of
its included headers changes. Unchanged objects and executables are retained.

Configured bundles cache a fingerprint containing the target, toolchain,
flags, source version, dependencies and configure arguments. A matching
generated configuration skips `configure`; a changed fingerprint invalidates
the bundle objects before configuring again.

Every complete third-party bundle also records its source and direct bundle
dependency contracts plus a manifest of produced files. An unchanged bundle
is not entered at all during a complete userland build. A changed public ABI
or direct dependency invalidates only the affected bundle and its consumers.

Use these explicit overrides when required:

```sh
./build.sh --reconfigure  # rerun configure, preserve common userland objects
./build.sh --rebuild      # clean userland objects and rerun every configure
ARMOS_FORCE_RECONFIGURE=1 ./tools/build_freetype.sh
```

Kernel output remains independent from these userland caches and is rebuilt
from a target-local clean tree.

## Profiles

Tracked profiles provide reproducible starting points without changing the
local file:

```sh
ARMOS_CONFIG=configs/qemu-virt-arm32.conf ./run.sh
ARMOS_CONFIG=configs/qemu-virt-arm64.conf ./run.sh
ARMOS_CONFIG=configs/raspi2-arm32.conf tools/build_pi2_sd.sh --mode none
ARMOS_CONFIG=configs/raspi3-arm64.conf tools/build_pi3_sd.sh --mode none
ARMOS_CONFIG=configs/raspi3-arm64-wifi.conf tools/build_pi3_sd.sh --mode none
```

`ARMOS_CONFIG` paths are resolved from the repository root.

## Precedence

Values are resolved in this order, from highest to lowest priority:

1. script command-line options, when the script provides them
2. exported environment variables
3. the selected `ARMOS_CONFIG` file, or local `armos.conf`
4. entry-point defaults

For GNU make, command-line variable assignments remain highest priority:

```sh
make TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt platform-kernel
```

An environment override also wins over a conflicting file value:

```sh
TARGET_ARCH=arm64 ARMOS_CONFIG=configs/qemu-virt-arm32.conf make config
```

## Main Options

The configuration format is strict `KEY=VALUE`. Blank lines and lines starting
with `#` are ignored. Unknown keys are rejected so spelling mistakes fail
before a long build begins.

Target and launch options:

```text
TARGET_ARCH=arm32
TARGET_PLATFORM=qemu-virt
SMP_CPUS=1
KEYBOARD_LAYOUT=us
ENABLE_NET=no
ENABLE_WIFI=no
ENABLE_GPU=no
ENABLE_HDMI=no
ENABLE_ILI9341=no
ENABLE_USB=no
HDMI_WIDTH=1280
HDMI_HEIGHT=720
```

`KEYBOARD_LAYOUT` selects the initial seat-wide keyboard layout. The supported
values are `us`, `us-mac`, `fr` (French Standard AZERTY / KBDFRNA),
`fr-legacy` (traditional French AZERTY / KBDFR), and `fr-mac`. Tracked QEMU
profiles use `fr-mac`; Raspberry Pi profiles use `fr-legacy` for the current
USB keyboard. The selected profile must provide the build-time definition
used by both the kernel and userland.

The layout can be inspected or changed without rebooting:

```sh
keymap
keymap us
keymap us-mac
keymap fr
keymap fr-mac
keymap fr-legacy
```

The change applies to the common input seat. The kernel immediately switches
the console translation and notifies `armos-wlcomp`, which republishes the
Wayland keymap to connected clients.

`ENABLE_NET=yes` adds VirtIO-net and the configured host forwarding when QEMU
starts. `ENABLE_GPU=yes` opens the VirtIO-GPU display and adds the keyboard
device. Both can be enabled together. These two launch options require the
`qemu-virt` platform.

`ENABLE_WIFI=yes` selects the Raspberry Pi 3 CYW43455 SDIO bring-up path. It
requires `TARGET_PLATFORM=raspi3`, firmware installed by
`tools/fetch_raspi3_wifi_firmware.sh --accept-license`. Credentials do not
need to be embedded at build time: root can use `wifi scan` and
`wifi connect SSID` on the running system. The command stores the global
regulatory country in `/etc/wifi.conf` and mode-`0600` network profiles under
`/etc/wifi.d`. Boot scans for visible known profiles and otherwise leaves the
radio idle without failing. The current milestone loads firmware, associates
with a WPA2 access point, exchanges BCDC Ethernet frames, and configures
`wlan0` through DHCP. The tracked `raspi3-arm64.conf` hardware reference
enables the driver; the dedicated `raspi3-arm32-wifi.conf` and
`raspi3-arm64-wifi.conf` profiles remain useful for focused Wi-Fi builds.

`boot-graphics.sh` always enables the graphical device for that invocation.
It does not require networking: with `ENABLE_NET=no`, no network arguments are
passed to QEMU. The QEMU serial console remains `/dev/tty0`; the framebuffer
console is `/dev/tty1`. When both are present, `init` starts an ordinary
`user` login shell on each console. Neither console receives an implicit root
shell. `boot-net.sh` similarly provides the explicit network-oriented entry
point.

`ENABLE_ILI9341` controls the HSD028309 B6 / ILI9341 GPIO display backend on
Raspberry Pi 3. The tracked `raspi3-arm64` profile enables it alongside HDMI
as the independent `/dev/fb1` and output-only `/dev/tty1` auxiliary display.
It is not a QEMU launch option and has no effect on `qemu-virt`.

`ENABLE_HDMI=yes` selects the Raspberry Pi firmware framebuffer on Raspberry
Pi 2 or Raspberry Pi 3 and exposes it through `/dev/fb0` and the Raspberry Pi
primary `/dev/tty0`. USB keyboard
input is routed to that console. PL011 remains separately accessible as
`/dev/ttyS0`, without an automatic login shell. HDMI requires
`TARGET_PLATFORM=raspi2` or `TARGET_PLATFORM=raspi3`; `HDMI_WIDTH` and
`HDMI_HEIGHT` request an exact mode.
The tracked Raspberry Pi 3 profile requests `1280x720`. If either dimension
is omitted, the platform defaults remain `1280x720`.
HDMI remains the primary `/dev/fb0`; an enabled ILI9341 does not mirror it.
These values select the boot mode. On a running Raspberry Pi HDMI system,
`fbctl mode WIDTHxHEIGHT` requests another exact firmware mode through
`/dev/fb0`.

`ENABLE_USB=yes` enables the Raspberry Pi DWC2 host, external hub support, and
boot-protocol keyboard/mouse input on Raspberry Pi 2 or Raspberry Pi 3. This
milestone enumerates devices present during startup.

Optional userland components:

```text
BUILD_NEWLIB=yes
BUILD_ALL_USERLAND=no
BUILD_TCC=yes
BUILD_BSD=no
BUILD_NCURSES=no
BUILD_NANO=no
BUILD_EPOLL_SHIM=no
BUILD_UTF8PROC=no
BUILD_FREETYPE=no
BUILD_EXPAT=no
BUILD_FONTCONFIG=no
BUILD_HARFBUZZ=no
BUILD_FCFT=no
BUILD_FOOT=no
BUILD_NUKLEAR=no
BUILD_XV_DEPS=no
BUILD_FBVIEW=no
```

`BUILD_NANO=yes` requires either `BUILD_NCURSES=yes` or an ncurses bundle
already installed in `userfs`. The bundle remains under `/opt/nano`, with
`/usr/bin/nano` created as a relative symlink during userfs staging.
`BUILD_EPOLL_SHIM=yes` installs the common
poll-backed epoll compatibility library and its regression test for ports such
as Foot. `BUILD_UTF8PROC=yes` installs the Unicode normalization and grapheme
library used by Foot and its font stack. `BUILD_FREETYPE=yes` installs the
FreeType rasterizer, the Meslo terminal font and its target-side regression
test. `BUILD_EXPAT=yes` installs the XML parser needed by Fontconfig.
`BUILD_FONTCONFIG=yes` installs font discovery and the generic monospace
policy used by Foot. `BUILD_HARFBUZZ=yes` cross-compiles the HarfBuzz
amalgamation with the host C++ compiler, without exceptions, RTTI, GLib, ICU,
Graphite or subsetting. Only the C headers and static `libharfbuzz.a` are
installed in ArmOS; TCC can consume the library but does not build it.
`BUILD_FCFT=yes` installs fcft 2.5.1 with HarfBuzz and utf8proc shaping for
Foot. `BUILD_FOOT=yes` cross-builds Foot 1.9.2 with the maintained ArmOS
portability patch and generates its Wayland protocol bindings below the
target-specific build tree. `BUILD_NUKLEAR=yes` builds the pinned Nuklear
library under `/opt/nuklear`, installs its public header for TCC, and includes
the native `armui-demo` Wayland client. It also installs the first-party,
engine-independent `/usr/lib/libarmui.a` and `<armui/armui.h>` interface;
Nuklear types remain private to its implementation. `BUILD_ALL_USERLAND=yes` enables the
complete third-party toolchain and graphics dependency set already handled by
`build.sh`.

The repository intentionally shares one `userfs` source tree between ARM32
and ARM64 builds. Before rebuilding, `build.sh` checks every installed ELF.
When it detects files left by the other architecture, it automatically enables
the complete userland build so optional tools and libraries are replaced
together. Switching architecture therefore requires no manual `userfs`
cleanup and cannot silently mix ARM32 and ARM64 programs.

Useful optional overrides include:

```text
QEMU_MEMORY=2G
NET_HOST_ADDR=127.0.0.1
NET_HOST_PORT=2323
NET_GUEST_PORT=2323
GPU_XRES=1024
GPU_YRES=768
SD_VOLUME=/Volumes/RASPI3
RASPI_FIRMWARE_DIR=../PI2/firmware/boot
```

Boolean values accept `yes/no`, `true/false`, `on/off`, or `1/0`.

## Compatibility

The previous commands continue to work unchanged:

```sh
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./run.sh
BUILD_BSD=1 BUILD_NCURSES=1 BUILD_NANO=1 ./build.sh
tools/build_raspberry_sd.sh --arch arm64 --platform raspi3 --mode raw \
  --raw-device /dev/rdisk4 --yes
```

This compatibility is intentional: `armos.conf` is a convenient default layer,
not a second build system.
