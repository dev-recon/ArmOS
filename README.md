# ArmOS

<p align="center">
  <img src="docs/images/armos-project-overview.png" alt="ArmOS project overview" width="100%">
</p>

<p align="center">
  A compact Unix-like operating system written from scratch for ARM.<br>
  Small enough to study, complete enough to run real processes, filesystems,
  SMP workloads, and a native C toolchain.
</p>

<p align="center">
  <a href="https://github.com/dev-recon/ArmOS/releases/tag/0.7.4"><img src="https://img.shields.io/badge/release-0.7.4-2878d7" alt="ArmOS 0.7.4"></a>
  <img src="https://img.shields.io/badge/architectures-ARMv7%20%7C%20AArch64-22a06b" alt="ARMv7 and AArch64">
  <img src="https://img.shields.io/badge/SMP-4%20cores-7557d3" alt="SMP on four cores">
  <img src="https://img.shields.io/badge/QEMU-10.0.2-e65d19" alt="QEMU 10.0.2">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-3d8fc5" alt="Apache License 2.0"></a>
</p>

ArmOS is an educational and research operating system, not a Linux
distribution. Version 0.7 brought ARM64 onto the same common kernel used by
ARM32. Version 0.7.4 adds a common IPv4, TCP, UDP and DNS stack, plus
CYW43455 Wi-Fi association and DHCP on Raspberry Pi 3 B+. The
architecture layer handles CPU and MMU details while processes, syscalls,
scheduling, VFS, filesystems, drivers, and userland contracts remain shared.

## See It Run

### Raspberry Pi 3 B+

<table>
  <tr>
    <td width="50%"><img src="docs/images/armos-raspi3-boot.png" alt="ArmOS AArch64 booting on a Raspberry Pi 3 B+ over UART"></td>
    <td width="50%"><img src="docs/images/armos-raspi3-top-smp.png" alt="ArmOS top showing four active Raspberry Pi 3 CPU cores"></td>
  </tr>
  <tr>
    <td align="center">AArch64 boot, MMU, SD/eMMC, ext2, FAT32 and SMP bring-up</td>
    <td align="center"><code>top</code> under <code>fork</code>, memory and CPU load on four cores</td>
  </tr>
</table>

These are direct UART captures from the current `arm64/raspi3` image running
on a Raspberry Pi 3 B+. They show the same common scheduler, VM, VFS and
userland used by the QEMU targets.

### Native development

<table>
  <tr>
    <td width="50%"><img src="docs/images/armos-raspi3-nano.png" alt="GNU nano with C syntax highlighting running inside ArmOS"></td>
    <td width="50%"><img src="docs/images/armos-raspi3-native-c.png" alt="A C fork and wait program compiled and executed natively inside ArmOS"></td>
  </tr>
  <tr>
    <td align="center">GNU nano 8.7 with syntax highlighting</td>
    <td align="center">TinyCC compiling and running a <code>fork</code>/<code>waitpid</code> program</td>
  </tr>
</table>

### Graphical console

<p align="center">
  <img src="docs/images/armos-wayland-foot-nano-teapot.png" alt="ArmOS Wayland compositor running Foot, GNU nano and the interactive Utah teapot demo" width="100%">
</p>

<p align="center">
  Native ArmOS Wayland compositor with server-side decorations, Foot,
  GNU nano 8.7 and the software-rendered interactive Utah teapot demo.
</p>

The current architecture, protocol coverage, platform status and remaining
renderer work are described in the
[Wayland compositor documentation](docs/WAYLAND_COMPOSITOR.md).

<table>
  <tr>
    <td width="50%"><img src="docs/images/armos-qemu-top.png" alt="ArmOS top running on the graphical tty1 console"></td>
    <td width="50%"><img src="docs/images/armos-qemu-framebuffer.png" alt="ArmOS framebuffer test pattern"></td>
  </tr>
  <tr>
    <td align="center"><code>top</code> on the VirtIO-GPU <code>tty1</code> console</td>
    <td align="center"><code>fbtest</code> writing directly to <code>/dev/fb0</code></td>
  </tr>
</table>

The graphical captures come from `arm64/qemu-virt`. The Raspberry Pi 3 target
can drive either an HSD028309 B6 / ILI9341 panel through GPIO or an HDMI
monitor through the firmware framebuffer, using the same `/dev/fb0` and
common display interfaces. On Raspberry Pi, the display and USB keyboard form
the primary `tty0`; the PL011 recovery transport remains available separately
as `/dev/ttyS0`. Its USB host supports boot keyboards and mice through the
Pi 3 internal hub.

## Quick Start

Install the host dependencies for [macOS](INSTALLATION_macos.md) or
[Linux](INSTALLATION_linux.md), then:

```sh
git clone https://github.com/dev-recon/ArmOS.git
cd ArmOS
./run.sh
```

The default route builds and boots the conservative `arm32/qemu-virt` target.
Try the ARM64 reference kernel explicitly with:

```sh
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./run.sh
```

Open the graphical console with:

```sh
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./boot-graphics.sh
```

ArmOS also supports a local [`armos.conf`](docs/BUILD_CONFIGURATION.md) and
tracked configuration profiles, so architecture, platform, networking, GPU,
memory, and optional userland bundles do not have to be repeated on every
command line.

Prebuilt QEMU images are attached to each release. See
[`images/README.md`](images/README.md) to boot ARM32 or ARM64 without compiling
the kernel or userland.

For a build-free launch from a checkout containing the release images:

```sh
./images/run-image.sh
./images/run-image.sh arm64
```

## What Is Inside

| Area | Current implementation |
| --- | --- |
| Processes | Preemptive scheduling, SMP, `fork`, ELF `execve`, `waitpid`, signals, job control |
| Virtual memory | Split user/kernel address spaces, MMU, ASIDs, copy-on-write, anonymous `mmap` |
| Files and IPC | VFS, descriptors, pipes, `dup`, ext2 root, FAT32, `/proc` |
| Devices | PL011 UART, TTYs, VirtIO block/net/GPU/input, Raspberry Pi SD/eMMC, HDMI, ILI9341, USB boot HID and CYW43455 Wi-Fi |
| Networking | Common ARP/IPv4, DHCP, ICMP, UDP, DNS and TCP stack with POSIX sockets |
| Userland | newlib, `mash`, core Unix tools, editors, tests, optional BSD utilities |
| Native development | TinyCC can compile and run small C programs from inside ArmOS |

The normal contributor workflow still uses the host cross toolchain. Native
TinyCC is the next step toward self-hosting, not yet a replacement for the
release build system.

## Supported Targets

| Target | Role | CPU mode | Main devices |
| --- | --- | --- | --- |
| `arm32/qemu-virt` | Fresh-checkout default | ARMv7-A | GICv2, PL011, VirtIO |
| `arm64/qemu-virt` | Kernel feature reference | AArch64 | GICv2, PL011, VirtIO, 4-core SMP |
| `arm64/raspi3` | Hardware reference | AArch64 | Raspberry Pi 3 B+, PL011, SD/eMMC, HDMI/ILI9341, USB HID, CYW43455 Wi-Fi, DHCP, DNS, ICMP and TCP, 4-core SMP |
| `arm32/raspi2` | Supported hardware | ARMv7-A | Raspberry Pi 2 B v1.1, PL011, SD/eMMC |

QEMU 10.0.2 is the reference emulator for the 0.7 line. Hardware images use a
dedicated FAT32 firmware partition and an ext2 root filesystem.
The repository can install and validate the host SDL/OpenGL/VirGL
prerequisites before building its private QEMU; see
[QEMU 10.0.2 VirGL host build](docs/QEMU_VIRGL_HOST.md).

## Try ArmOS

Once `mash` starts, a useful first tour is:

```sh
uname -a
ps
ls -la /
ls -la /proc
ifconfig
ping -c 4 10.0.2.2
httpget http://example.com/
systest
```

A small native compilation looks like this:

```sh
printf '#include <stdio.h>\nint main(void){puts("hello from ArmOS");}\n' > hello.c
tcc hello.c -o hello
./hello
```

Optional packages include ncurses, GNU nano, BSD `bmake`, `sed`, `awk`,
`install`, `mtree`, `xargs`, `diff`, `patch`, `pax`, `tar`, `m4`, and ELF
inspection tools. They can be included through `armos.conf` or the existing
build flags.

## Documentation

- [Architecture](docs/ARCHITECTURE.md) - kernel structure, MMU, scheduler, VFS, TTYs, and drivers
- [Build configuration](docs/BUILD_CONFIGURATION.md) - targets, profiles, and `armos.conf`
- [Kernel development](docs/GET_STARTED_KERNEL_DEV.md) - source map and contributor workflow
- [Userland development](docs/GET_STARTED_USERLAND_DEV.md) - newlib programs and native experiments
- [ARM64 port](docs/ARM64_PORT.md) - AArch64 contracts and current implementation
- [POSIX compatibility](docs/POSIX_COMPATIBILITY.md) - implemented surface and priorities
- [Storage performance](docs/STORAGE_PERFORMANCE.md) - ext2, FAT32, and SD/eMMC measurements
- [Raspberry Pi 3](docs/RASPBERRY_PI3.md) and [Raspberry Pi 2](docs/RASPBERRY_PI2.md) - hardware guides
- [Roadmap](ROADMAP.md) and [stability notes](STABILITY.md)

## Project Status

ArmOS is experimental software. It is built to expose real operating-system
mechanisms clearly and to support serious testing, but it is not production
ready and does not promise a stable ABI yet. Current work focuses on POSIX
surface growth, storage performance, networking, graphical I/O, hardware
drivers, and the road to a self-hosted toolchain.

Contributions, experiments, bug reports, and documentation improvements are
welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md).

## License

ArmOS is licensed under the Apache License, Version 2.0. See
[LICENSE](LICENSE) and [NOTICE](NOTICE).
