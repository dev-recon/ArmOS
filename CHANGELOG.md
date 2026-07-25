# Changelog

Release tags use the bare version number starting with `0.7` (`0.7`, `0.7.1`,
`0.7.2`, and so on). Older `v0.x` tags remain historical and are not moved.

## Unreleased

### Native threads

- Added the first scheduler-visible user-thread foundation with shared process
  state, distinct TIDs and stacks, a native clone argument block, and deferred
  SMP-safe thread reclamation.
- Added `threadtest` to validate shared memory, PID/TID identity, stack
  isolation, and clear-on-exit TID publication before the pthread and futex
  layers are introduced.

### Raspberry Pi storage

- Added CMD18/CMD25 multi-block PIO transfers to the BCM2835 SDHOST system-SD
  path used when Raspberry Pi 3 Wi-Fi is enabled, with explicit CMD12
  termination and permanent fallback to CMD17/CMD24 after the first failure.
- Added block-transport identity and physical command counters to
  `/proc/diskstats`, distinguishing block-layer request sizes from commands
  actually issued by drivers that split requests.

### Release images

- Added complete ARM32 and ARM64 QEMU Virt archives under `images/`, including
  TinyCC/Newlib, BSD tools, nano/ncurses and the supported image libraries.
- Added directly usable QEMU kernel binaries and a generated `SHA256SUMS`
  manifest alongside the disk archives.
- Added `images/run-image.sh` to extract and boot either prebuilt target
  without invoking the build system.
- Removed host checkout paths from kernel, newlib, userland and third-party
  development artifacts through reproducible compiler path mappings.
- Added release-image validation for ext2 consistency, required userland
  content, host paths, credentials and macOS metadata.

## ArmOS 0.7.4 - 2026-07-23

ArmOS 0.7.4 connects the common network stack to Raspberry Pi 3 Wi-Fi and
turns networking into a runtime-managed service rather than a fixed image
configuration.

### Highlights

- Made logical console identity independent from hardware transport.
- Routed Raspberry Pi HDMI or ILI9341 output and USB keyboard input through
  the primary `/dev/tty0`, while exposing PL011 separately as `/dev/ttyS0`.
- Kept QEMU serial on `tty0` and optional VirtIO-GPU/input on `tty1`.
- Removed the implicit graphical root login: every automatically spawned shell
  now runs as the ordinary `user`; administrative access remains explicit.
- Added early kernel-log replay when Raspberry Pi hands the boot console from
  UART diagnostics to the framebuffer.
- Added Raspberry Pi 3 CYW43455 bring-up through the common device
  architecture: SDHOST/SDIO enumeration, pinned firmware loading, station
  setup, WPA2 association, and reproducible ARM32/ARM64 Wi-Fi profiles.
- Connected VirtIO-net and CYW43455 BCDC Ethernet transport to a common
  architecture-neutral ARP, IPv4, DHCPv4, and ICMP echo stack.
- Added `/dev/netctl`, generic `/proc/net/dev` reporting, the `netd` kernel
  task, and userland `ifconfig` and numeric-address `ping` commands.
- Added an architecture-neutral UDP, DNS and TCP socket layer shared by
  VirtIO-net and CYW43, including active connect, bounded retransmission,
  peer-window handling, FIN/RST processing, timeouts, passive accept, and
  ordinary descriptor `read`/`write`.
- Added newlib socket transfer and resolver interfaces, including
  `sendto`, `recvfrom`, `shutdown`, and IPv4 `getaddrinfo`.
- Added `httpget`, a compact HTTP/1.1 client used to validate DHCP, DNS and TCP
  end to end on QEMU and Raspberry Pi 3 Wi-Fi.
- Added runtime Wi-Fi scanning and root-only profile management with
  interactive credential confirmation, bounded retries, hot reconnect, and
  persistent per-SSID credentials under `/etc/wifi.d`.
- Made the regulatory country a global `/etc/wifi.conf` policy, prompted on
  first use instead of assuming France, and added boot-time selection of
  visible known networks without making missing configuration fatal.
- Stabilized the CYW43455 SDPCM transport by accepting both receive-data
  indications, draining stale F2 frames after aborted scans, preserving the
  firmware credit window, and splitting CMD53 FIFO transfers into
  controller-safe 512-byte chunks.
- Reduced idle polling by letting `usbd` and `netd` sleep according to their
  next required service interval, and separated user, kernel, IRQ, and idle
  CPU accounting in `top`.
- Extended Raspberry Pi 2 configuration and bootstrap support for the common
  firmware HDMI and DWC2 USB paths.
- Fixed kernel `printf` handling of `long long` arguments, keeping 64-bit
  network counters aligned and correctly formatted on ARM32.
- Made architecture changes safe for the shared `userfs`: `build.sh` now
  detects stale ELF files from the other target and automatically rebuilds the
  complete userland instead of producing a mixed ARM32/ARM64 disk.

## ArmOS 0.7.3 - 2026-07-21

ArmOS 0.7.3 turns the Raspberry Pi 3 into a directly usable workstation target
with firmware HDMI output and USB keyboard/mouse input.

### Highlights

- Added a VideoCore mailbox framebuffer backend for Raspberry Pi HDMI, exposed
  through the common `/dev/fb0` and graphical `tty1` contracts.
- Added a BCM2837 DWC2 host-controller driver with internal-hub enumeration and
  USB boot-protocol keyboard and mouse support.
- Moved USB lifecycle and polling into the architecture-neutral USB core and
  its `usbd` kernel task; the Raspberry Pi platform now only registers the
  controller discovered from the boot DTB.
- Added `/proc/usb` and `lsusb` list, verbose, and topology views.
- Added responsive HID polling and held-key repeat for printable keys,
  Backspace, Tab, and cursor keys.
- Kept HDMI and the ILI9341 backend behind the same framebuffer/display policy,
  with build-time configuration validation preventing conflicting backends.
- Made HDMI plus USB the default Raspberry Pi 3 ARM64 hardware profile while
  preserving UART as the recovery console.

## ArmOS 0.7.2 - 2026-07-18

ArmOS 0.7.2 adds the first framebuffer console on Raspberry Pi hardware and
polishes the fresh-machine build path introduced by the 0.7 series.

### Highlights

- Added a BCM283x GPIO driver, an 8-bit 8080 parallel bus, and an
  ILI9341-compatible backend for the HSD028309 B6 shield on Raspberry Pi 3.
- Kept one framebuffer contract across hardware and emulation: `/dev/fb0`
  exposes ARGB8888 pixels and `tty1` uses either ILI9341 or VirtIO-GPU through
  a common display backend.
- Added runtime portrait/landscape rotation with `fbctl`, framebuffer ioctls,
  tty geometry updates, and `SIGWINCH` notification.
- Added public-domain PNG, JPEG, and minimal uncompressed TIFF samples under
  `/home/user/images` for `fbview` and local XV compatibility testing.
- Corrected the cross-build libtiff `size_t`/`ptrdiff_t` configuration for
  both ARM32 and ARM64.
- Ordered secondary scheduler admission before PID 1 becomes runnable, so SMP
  boot messages cannot overwrite the first framebuffer shell banner.
- Fixed fresh Linux and macOS setup details around TinyCC source staging,
  e2fsprogs discovery, the pinned QEMU GTK display, and Linux Homebrew fallback.
- Fixed `boot-graphics.sh` on the macOS system Bash when networking is
  disabled. Empty network argument arrays are no longer expanded under
  `set -u`.
- Made the pinned QEMU 10.0.2 source build require GTK on Linux and made
  `boot-graphics.sh` reject headless QEMU binaries with an actionable error.

## ArmOS 0.7.1 - 2026-07-16

ArmOS 0.7.1 consolidates the first post-ARM64 release cycle. It extends the
common POSIX surface, improves filesystem and SD throughput, and fixes
cross-CPU address-space and executable-cache coherency bugs found by sustained
Raspberry Pi 3 stress testing.

### Highlights

- Extended the common ARM32/ARM64 POSIX surface with clocks,
  `clock_nanosleep()`, `sched_yield()`, capability queries, positioned I/O,
  directory-relative operations, descriptor metadata operations, filesystem
  capacity queries, timestamp updates, and per-process descriptor limits.
- Added `armos.conf` and `armos.conf.example` as a single configuration layer
  for architecture, platform, SMP, QEMU features, userland bundles, and
  Raspberry Pi image staging. Environment variables remain supported as
  explicit overrides.
- Added `iobench`, `/proc/diskstats`, filesystem statistics, ext2 dirty block
  caching and grouped writeback, FAT32 clustered I/O, and SD/eMMC four-bit
  multi-block transfers using CMD18/CMD25 with a conservative fallback path.
- Hardened ARM64 ASID allocation and COW fault handling under process churn.
- Fixed SMP process reaping and ARM32 ASID reuse after long fork/exec stress.
- Published newly loaded executable code to every participating CPU's
  instruction cache before a task may migrate.
- Reduced Raspberry Pi boot staging to the firmware, DTB and overlays required
  by the selected target instead of copying the complete firmware tree.
- Refreshed the project overview and added Raspberry Pi 3 hardware screenshots.

## ArmOS 0.7 - 2026-07-15

ArmOS 0.7 is the common-kernel ARM64 milestone. ARM32 and ARM64 now enter the
same kernel subsystems for processes, scheduling, VFS, filesystems, syscalls
and device policy; architecture code is limited to CPU, MMU, exception and
context-switch mechanics.

### Highlights

- Added the production ARM64 port for QEMU Virt and Raspberry Pi 3 B+,
  including EL0, ELF64, fork/COW, exec, signals, mmap, timer preemption,
  ASIDs, TLB shootdown and four-CPU SMP scheduling.
- Kept `arm32/qemu-virt` as the fresh-checkout development default while making
  `arm64/qemu-virt` the ARM64 feature reference and Raspberry Pi 3 B+ the
  AArch64 hardware reference.
- Added Raspberry Pi 2 Model B v1.1 support through the ARM32 `raspi2` target,
  with firmware boot, SD/eMMC, ext2 root, FAT32 and UART console support.
- Added Raspberry Pi 3 SD-card tooling, hidden FAT32 firmware partition,
  hardware shutdown and UART development helpers.
- Made newlib the canonical libc and supplied matching ARM32 and AArch64
  userlands containing init, mash, core tools and native TinyCC.
- Added optional ncurses/nano support, syntax highlighting and line numbers.
- Added the BSD userland bundle: `bmake`, `sed`, `awk`, `install`, `mtree`,
  `xargs`, `diff`, `patch`, `pax`, `tar`, `m4`, `ar`, `ranlib`, `nm`, `strip`
  and `size`.
- Added zlib, libjpeg, libpng and libtiff ports, raw framebuffer access,
  framebuffer tests and an image viewer smoke test.
- Enabled the QEMU Virt networking and graphics development paths on ARM64.
- Centralized the release version used by the kernel banner and `uname`.

## ArmOS v0.6 - 2026-07-05

ArmOS v0.6 consolidates the SMP, multi-arch preparation, filesystem hardening,
and terminal-userland work into `main`. The release keeps the public stable
runtime contract on `SMP_CPUS=1`, while making the multi-CPU profile much more
useful for developer stress testing.

### Highlights

- Merged the SMP bring-up line into `main` with the current stability contract:
  `SMP_CPUS=1` is the release-stable profile, `SMP_CPUS>1` is an advanced
  developer stress profile.
- Added multi-arch phase-0 foundations:
  - generated `asm-offsets` for C/ASM context structure offsets;
  - `paddr_t`, `vaddr_t`, and `pfn_t` address type groundwork;
  - a shared in-kernel FDT parser;
  - cleaner split between ARM helpers and portable kernel code.
- Hardened storage and VFS paths:
  - real MBR-partitioned `disk.img`;
  - 512 MB ext2 root filesystem;
  - stronger ext2 block mapping and `sys_write` bounce behavior;
  - improved shutdown/sync diagnostics.
- Added optional ncurses and nano bundles:
  - `tools/build_ncurses.sh`;
  - `tools/build_nano.sh`;
  - custom `TERM=armos` terminfo entry;
  - `cursestest` as a small ncurses validation program;
  - tiny static GNU nano under `/opt/nano/bin` when enabled.
- Extended newlib/TCC compatibility for larger userland ports, including more
  directory, resource, terminal, and program-name glue.
- Kept native TinyCC as the end-user compiler path while preserving GCC/newlib
  as the contributor and release build path.
- Updated boot and `uname` version strings to `ArmOS 0.6 armv7l`.

### Build Flags

Default local builds include the native TinyCC bundle. ncurses and nano are
optional build artifacts:

```sh
./build.sh
BUILD_NCURSES=1 BUILD_NANO=1 ./build.sh
```

The generated `/opt/tcc`, `/opt/ncurses`, and `/opt/nano` contents are staged
into the disk image but are not committed to Git as generated binaries.

### Supported Emulator

ArmOS v0.6 keeps QEMU 10.0.2 as the reference emulator. QEMU 11.0.1 has been
smoke-tested, but its macOS/Cocoa graphical window scaling differs from 10.0.2.

### Known Limitations

- SMP is substantially more capable but still not the public stable contract.
- The graphical console remains optional; UART `tty0` is the recovery path.
- ncurses/nano are early ports and should be validated with `cursestest`,
  `nano`, `kilo`, `top`, and `ttytest` before relying on them for larger work.
- The multi-arch work is groundwork, not an AArch64/Raspberry Pi port yet.

## ArmOS v0.4-v0.5 Internal Milestones

The public tag jumps from v0.3 to v0.6 because several internal stabilization
streams landed together:

### SMP Bring-Up

- Added active SMP hardening work around task ownership, scheduler state,
  TLB shootdown diagnostics, procfs snapshots, and multi-CPU stress tooling.
- Documented the current support contract: the stable public runtime profile is
  still `SMP_CPUS=1`.
- `SMP_CPUS>1` is available as an experimental developer mode for race hunting
  and scheduler/MMU validation, but it is not yet the release-stable
  configuration.

## ArmOS v0.3 - 2026-06-29

ArmOS v0.3 is the first public release that gives ArmOS end users a small
native C programming environment, while keeping the project development and
stabilization workflow on the existing macOS/Linux cross toolchain.

### Highlights

- Native TinyCC bring-up inside ArmOS through `/usr/bin/tcc`, intended for
  end-user experiments and small programs.
- TinyCC runtime bundle staged under `/opt/tcc`, rebuilt by the standard build
  scripts when `BUILD_TCC=1`.
- Expanded TCC/newlib syscall glue for native userland builds.
- `/usr/src/armos` source tree installed directly in the root filesystem.
  ArmOS users can inspect, edit, compile, and run small userland programs from
  inside ArmOS itself.
- Userland source snapshot includes public headers, coreutils, programs,
  `mash`, init, and system tools.
- `mash` now resolves explicit relative command paths such as `./hello` and
  `../tool` before `execve`, matching normal shell expectations.
- Native TCC validation includes:
  - `hello.c` compile/link/run inside ArmOS;
  - argument passing through `argv`;
  - simple newlib `malloc`, `string`, and `stat` paths;
  - direct execution of locally compiled programs with `./program`;
  - non-trivial compile/link of the ArmOS `kilo` source.
- Boot and `uname` version strings updated to `ArmOS 0.3 armv7l`.

### Source-In-Userfs Model

The release intentionally ships userland sources in:

```text
/usr/src/armos/userland
```

This is not just documentation. It is part of the product direction: ArmOS users
should be able to edit code with `kilo`, compile it with native TinyCC, and run
the result directly in ArmOS.

This does not change the main engineering workflow. Kernel development,
stabilization, release builds, and contributor work remain host cross-compiled
from macOS or Linux with `arm-none-eabi-gcc` and the existing build scripts.
The current native scope is small userland programs, not kernel self-hosting.

Example:

```sh
tcc /usr/src/armos/userland/coreutils/src/ls.c -o /tmp/ls-tcc
/tmp/ls-tcc /proc
```

### Supported Emulator

ArmOS v0.3 keeps QEMU 10.0.2 as the reference emulator. QEMU 11.0.1 has been
smoke-tested, but its macOS/Cocoa graphical window scaling differs from 10.0.2
and remains compatible-but-not-reference.

### Known Limitations

- Native TCC support targets small end-user programs first.
- The kernel still builds with `arm-none-eabi-gcc`; TinyCC is not a kernel
  compiler target.
- `/opt/tcc` is generated/staged by the build scripts and remains ignored by
  Git.
- Larger ports may still expose missing POSIX/newlib behavior.

## ArmOS v0.2 - 2026-06-22

ArmOS v0.2 focuses on making the system feel like a small interactive Unix-like
environment rather than only a serial-console kernel lab.

### Highlights

- Userland `init` under `/sbin/init`, with process reaping delegated to
  userland policy.
- Newlib is now the reference userland C library; the older in-tree libc and
  early programs are archived under `userland/legacy/`.
- Root and user sessions, `su`, `/root`, and more Unix-like ownership display.
- Improved `mash` shell behavior, job control, `$?`, `wait`, `fg`, `bg`, and
  POSIX-style `jobs` output.
- Compact POSIX-like `ps`, with the detailed ArmOS diagnostic view moved to
  `lps`.
- More userland utilities, including `top`, `kload`, `ttyinfo`, `keytest`,
  `grep`, `sed`, `sort`, `uniq`, `wc`, `which`, `who`, `whoami`, `uname`,
  `date`, `uptime`, `free`, `df`, `mount`, and `umount`.
- `/dev/null`, `/dev/tty`, `/dev/tty0`, `/dev/tty1`, and `/dev/console`
  support.
- Better `termios` coverage, raw/non-canonical mode, terminal-generated
  signals, foreground process groups, and background TTY read behavior.
- Optional VirtIO-GPU graphical console on `tty1`.
- VirtIO input keyboard support for `tty1`, including a Mac French development
  layout fallback.
- Graphical console bitmap font rendering, ANSI colors, cursor blinking through
  a dedicated `displayd` kernel task, and simple scrollback.
- `kilo` editor usable on both UART and graphical console paths.
- QEMU scripts support selecting an alternate QEMU binary through `QEMU=...`
  while defaulting to the system/Homebrew QEMU.

### Supported Emulator

ArmOS v0.2 is validated against QEMU 10.0.2 on macOS as the reference
emulator. QEMU 11.0.1 has been smoke-tested, including the graphical boot path,
but its macOS/Cocoa window scaling differs from 10.0.2 and should be treated as
compatible-but-not-reference for this release.

### Stability Baseline

The UART console remains the required rescue path. `boot.sh` should stay usable
even if the graphical console regresses. The graphical boot path is additive:

```sh
./boot-graphics.sh
```

Useful validation commands:

```sh
systest
ttytest
ttytest --interactive-canon
ttytest --interactive-raw
keytest
kilo /home/user/hello.c
top
lps
```

Known areas still under active development:

- graphical-console resize and richer scrollback;
- host-style mouse selection/copy-paste in the graphical window;
- full UTF-8/accent rendering;
- more complete login/getty model for multiple consoles;
- broader POSIX/newlib compatibility for larger ports.

## ArmOS v0.1 - 2026-06-18

Initial public release of ArmOS.

ArmOS v0.1 is a small ARMv7-A Unix-like kernel and userland for the QEMU `virt`
machine, targeting Cortex-A15. It is intended for learning, research, and
experimentation around real operating-system mechanisms while staying small
enough to understand.

### Highlights

- ARMv7-A kernel for QEMU `virt` / Cortex-A15
- MMU enabled with split TTBR0/TTBR1 address spaces
- ASID support with rollover handling
- Kernel tasks and user processes
- Timer-driven scheduling
- User/kernel context switching across blocking syscalls
- `fork`, `execve`, `waitpid`, `exit`
- Copy-on-write process memory
- Signals, process groups, and basic job control
- Pipes, file descriptors, `dup`, `dup2`, and file descriptor flags
- Ext2 root filesystem with read/write support
- FAT32 compatibility mount under `/mnt`
- VirtIO block device support
- `/proc` virtual filesystem
- UART-backed console/TTY
- `mash` shell with external commands, scripts, pipes, redirections, background
  jobs, history, and completion
- Newlib-based userland path alongside the older in-tree libc compatibility path
- Core utilities including `cat`, `echo`, `pwd`, `ls`, `cp`, `mv`, `rm`,
  `mkdir`, `rmdir`, `touch`, `sleep`, `kill`, `ps`, `lps`, `stat`, `head`,
  `shutdown`, and stress tools
- Runtime smoke and stress coverage through `systest`

### Stability Baseline

The release has passed repeated parallel `systest` stress runs, including
fork/exec/wait, signals, process groups, pipes, filesystem operations, malloc
stress, ASID rollover, and procfs traversal.

Known areas still under active development:

- long-idle TTY/read wakeup hardening
- terminal/raw mode support
- procfs transient entry presentation under fork/exit storms
- cleanup of old debug code and unused kernel paths
- broader POSIX/newlib compatibility for larger userland ports

### Platform

Supported release target:

- QEMU `virt`
- ARM Cortex-A15
- ARMv7-A, 32-bit
- GICv2
- ARM generic timer
- VirtIO block device
- UART console

### Build And Run

See:

- `README.md`
- `INSTALLATION_macos.md`
- `INSTALLATION_linux.md`

Common commands:

```sh
./run.sh
./boot.sh
```

Exit QEMU with `Ctrl+A`, then `X`.
