# ArmOS Roadmap

This roadmap tracks the Linux-1.0-inspired workstreams currently active in
ArmOS. The goal is not to clone Linux line by line, but to converge toward the
same useful Unix contracts while keeping the kernel understandable and
debuggable.

## 0.7.5 Current Baseline

The 0.7.5 release keeps the 0.7 common-kernel architecture and adds:

- one POSIX implementation path for ARM32 and ARM64, including clocks,
  positioned I/O, directory-relative operations, filesystem capacity,
  timestamp updates, and descriptor limits;
- one `armos.conf` configuration layer for builds, QEMU launch features,
  userland bundles, and Raspberry Pi image staging;
- measured ext2 and FAT32 improvements, filesystem and block statistics, and
  the `iobench` regression tool;
- four-bit, multi-block Raspberry Pi SD/eMMC transfers with automatic fallback
  to the validated single-block path;
- stronger ASID, COW, process-reaping, and cross-CPU executable-cache
  coherency under sustained SMP process churn;
- target-specific Raspberry Pi firmware staging;
- a common display-backend contract shared by VirtIO-GPU and the Raspberry
  Pi 3 HDMI and ILI9341 GPIO backends, including `/dev/fb0`, graphical `tty1`,
  runtime orientation changes, and public-domain image fixtures;
- a common USB core and `usbd` task, with the Raspberry Pi DWC2 controller,
  internal hub enumeration, HID keyboard/mouse input, `/proc/usb`, and
  `lsusb`;
- a Raspberry Pi 3 CYW43455 path that initializes SDHOST/SDIO, loads pinned
  firmware, completes WPA2 association, acquires a DHCP lease, and exchanges
  ARP and ICMP traffic on hardware;
- one common ARP, IPv4, DHCP, ICMP, UDP, DNS and TCP implementation shared by
  VirtIO-net and CYW43455, with POSIX socket I/O and an HTTP/1.1 validation
  client;
- runtime Wi-Fi scans, regulatory-country policy, root-managed per-SSID
  profiles, known-network selection at boot, and nonfatal startup without
  credentials;
- separate user, kernel, IRQ and idle accounting in `mtop`, plus adaptive idle
  polling for the `usbd` and `netd` kernel services;
- deterministic boot ordering that admits secondary schedulers before PID 1
  can print the first framebuffer shell banner.
- a pinned FreeBSD `/bin/sh` with target-isolated `libedit`, user/root startup
  profiles, wildcard expansion, interactive job control, and a 511/518
  imported-test baseline, while `/sbin/mash` remains the recovery shell.
- the historical process monitor renamed to `mtop`, with the pinned FreeBSD
  `top` frontend imported separately and an ArmOS `/proc` backend contract
  prepared under `userland/opt/freebsd-top`.

The next release should build on this baseline instead of introducing an
architecture-private implementation of a common kernel policy.

## HDMI And USB Follow-up

The 0.7.5 hardware baseline includes two Raspberry Pi 3 backends while
preserving UART as the recovery path:

- a VideoCore mailbox framebuffer exposed through the common `/dev/fb0` and
  graphical `tty1` interfaces;
- a BCM2837 DWC2 host path that enumerates the internal hub and routes USB boot
  keyboard/mouse input through the common TTY and display input contracts.

Follow-up work includes USB hotplug, interrupt-driven transfers, more HID
layouts, USB storage, dynamic class-driver binding, and eventually native VC4
display management.

## Wayland Compositor And Graphical Userland

Status: usable software-rendered baseline.

The current implementation is documented in
[`WAYLAND_COMPOSITOR.md`](WAYLAND_COMPOSITOR.md). The common kernel provides
IPC, SHM, input and framebuffer contracts; `/sbin/armos-wlcomp` owns Wayland
protocol and window policy in userland. Foot, GNU nano and the interactive
teapot demo now run as real Wayland clients on the shared ARM32/ARM64 stack.

Immediate goals:

- enforce the common application and lifecycle contract documented in
  `GRAPHICAL_APPLICATION_CONTRACT.md`;
- replace global `poll` wakeups with wait registrations on individual
  sockets, PTYs, timers and input objects;
- move Wayland connection, SHM generations, configure handling, frame pacing,
  input and clipboard into one reusable ArmUI backend;
- complete transactional xdg-shell and subsurface state in the compositor;
- preserve bounded damage from client commits through framebuffer
  presentation;
- profile and optimize software copy/fill paths on Raspberry Pi 3;
- keep QEMU VirtIO-GPU and Raspberry Pi HDMI behind the same renderer/backend
  contract;
- complete clipboard, resizing and shell interactions needed by a small
  desktop;
- retain UART/console recovery while Raspberry Pi graphics stabilizes.

Later milestone:

- add a VC4/V3D renderer backend without moving compositor policy into
  Raspberry-specific kernel or platform code.

The GPU and Raylib workstream is specified in
[`DRM_ARCHITECTURE.md`](DRM_ARCHITECTURE.md). Its first milestone provides
versioned `/dev/dri/card0` and `/dev/dri/renderD128` discovery nodes plus a
qemu-virt VirtIO-GPU backend.
Buffer objects, CPU mappings, typed resource usage, explicit
context/resource attachments and interrupt-completed asynchronous fences now
share the same common contract on ARM32 and ARM64. The common UAPI also exposes
opaque command handles, bounded command-capability blobs and explicit
CPU/device transfers. The qemu-virt backend translates these operations to the
selected VirGL capset without leaking VirtIO details into common code.
`libarmos-virgl-winsys` validates the complete lifecycle with a real off-screen
triangle, fence wait, readback and pixel verification. The reproducible
Mesa/Gallium VirGL port and explicit EGL/Wayland buffer exchange are now
implemented. The accelerated-compositor provider composes cached SHM layers,
decorations, pointer and resize feedback into three provider-owned outputs.
Per-output tile damage, clipping and conservative opaque occlusion preserve
triple-buffer contents without full-screen redraws. Composition fences are
dispatched by the Wayland event loop; up to two exported output transactions
remain pending while command generation continues, and intervening damage is
retained when all submission slots are occupied. Fence notification is armed
strictly in submission order so multiple readable fences cannot reorder
scanout.
Direct import of client GPU buffers, acquire fences and per-buffer compositor
release fences are implemented. The common Raylib 6.0 Wayland/EGL/GLES2 port
now builds and presents through VirGL. `raypot-demo` validates a complete
accelerated frame with VBOs, depth testing and GLES2 shaders using the same
Utah model as the software demo. VC4/V3D remains a platform implementation
behind the same object and synchronization model.

Startup profiling separates renderer readiness, first presentation and client
launch. On the current static Mesa image the renderer and first frame complete
in about half a second. The common `armos_spawnve` process contract now starts
the desktop shell, Foot and shell-launched applications in fresh address spaces
without cloning the Mesa-populated compositor. The contract is independent of
the graphics stack and shared by ARM32 and ARM64.

### ArmUI And Desktop Applications

The graphical application toolkit will use Nuklear as an internal immediate
mode engine while keeping the public ArmOS API independent from that
implementation.

Roadmap:

1. **Implemented** — port Nuklear and provide a native `armui-demo` Wayland
   client using SHM,
   mouse, keyboard, UTF-8, resizing and double buffering.
2. **Implemented** — build `libarmui` above the backend. Its small C API does
   not expose Nuklear structures, so themes, widgets and the rendering engine
   remain replaceable.
3. **Implemented and validated** — `armos-control-center` provides display,
   network, process and appearance panels. It consumes bounded snapshots
   through `libarmos-services`; applications do not parse procfs or access
   display devices directly. Future mutations must cross the same explicit
   service boundary.
4. **Implemented and validated** — `xdg_positioner` and `xdg_popup` provide
   parent-relative transient surfaces, configure/ack transactions, clipping,
   stacking and input hit testing. The protocol test covers popup creation and
   geometry.
5. **Implemented and validated** — `/sbin/armos-shell` is an ArmUI system-bar
   client. A narrow authenticated `armos_shell_v1` protocol assigns its panel
   role and reserves the desktop work area. Programs launched from the bar
   drop to the ordinary user identity.

Nuklear applications must remain event driven: static windows sleep until
input, state changes or an explicit animation deadline requires another frame.
Client damage, compositor dirty tiles and framebuffer presentation must remain
bounded throughout this stack.

Current status:

- Nuklear 4.13.3 is pinned as an unmodified third-party header and built as
  `/opt/nuklear/lib/libnuklear.a`;
- `libarmui.a` owns the Nuklear context, font atlas, portable software
  renderer, Wayland/xdg lifecycle, input routing, frame pacing and
  generation-safe SHM buffer recreation behind the engine-independent
  `<armui/armui.h>` API;
- pointer input is consumed as a Wayland transaction ending at
  `wl_pointer.frame`; a bounded stabilization frame makes immediate-mode
  state and theme changes visible on button release without application
  polling;
- `armui-demo` now contains application state and widgets only. It exercises
  the common backend rather than carrying a private Wayland implementation;
- `teapot-demo` uses the same application lifecycle for its continuously
  rendered canvas, resize, keyboard input, SHM generations and frame pacing;
- `armos-control-center` uses only `libarmos-services`, keeping kernel and
  device details outside the desktop application;
- `/sbin/armos-shell` uses the dedicated ArmUI system-bar role. The
  compositor creates a per-session capability token and never grants panel
  ownership based on a process name or window title;
- the common compositor implements border resizing, maximize/restore,
  fullscreen, reversible compact minimization, transient popup surfaces and a
  panel-aware work area.

The five milestones are complete and the architecture has been validated
interactively. ARM32 and ARM64 private cross-build validation remains
mandatory after each contract change, followed by interactive validation on
each enabled target before release.

## 0.7 ARM64 Baseline

The 0.7 baseline is:

- fresh-checkout default: `arm32/qemu-virt`;
- ARM64 feature reference: `arm64/qemu-virt`, four CPUs;
- supported hardware: Raspberry Pi 3 Model B+ in AArch64 mode and Raspberry
  Pi 2 Model B v1.1 in ARMv7-A mode;
- UART `tty0` is the required recovery console;
- optional graphical `tty1` remains additive;
- ext2 root is 512 MB inside a real MBR-partitioned `disk.img`;
- newlib is the supported libc;
- TinyCC is the native end-user compiler path;
- ncurses and nano are optional generated bundles;
- ARM64 bring-up history includes an ARM64/QEMU-virt EL1 serial bootstrap, exception
  vectors, a minimal long-descriptor identity MMU, GICv2/generic-timer IRQ
  delivery, a shared early page allocator, and FDT-driven RAM/reservation
  discovery, allocated TTBR0 L1/L2/L3 tables, and a permission-checked TTBR1
  kernel alias now used for the live PC, stack, and vectors after retiring the
  complete low kernel/MMIO map; isolated user-only TTBR0 tables and ASID
  switching are validated; a first EL0t smoke payload runs with separate
  RX and RW/NX mappings, returns through SVC, and preserves timer IRQ delivery,
  and a bootstrap user-VM object now owns its tables, pages, mappings, and ASID,
  while a bounded `svc #0` dispatcher validates the AArch64 register ABI,
  user-buffer faults, unknown calls, and `exit`; EL0 entry and lower-EL
  exception capture now share an explicit register image with generated
  C/assembly offsets and preservation checks; a bootstrap task context also
  performs a validated two-stack cooperative switch of `x19-x30`, SP and its
  resume PC, and the switch boundary now activates and verifies each context's
  TTBR0/ASID identity; mapping generations preserve unchanged resident ASIDs
  without per-switch TLBI on the single bootstrap CPU; the bootstrap task now
  allocates, clears, and releases its owned high-half kernel stack with exact
  page-accounting checks, and that probe now uses the generic `task_t` identity,
  state, stack metadata, and lifetime guards; task-level switching now moves
  `RUNNING/BLOCKED` state and CPU ownership between the borrowed bootstrap stack
  and owned probe stack; a bounded single-CPU generic FIFO now publishes the
  blocked probe as ready, rejects duplicate publication, selects it, and drives
  two validated cooperative switch cycles; two simultaneously ready tasks now
  rotate deterministically in `A, B, A, B` order with independent owned stacks
  and balanced page recovery; a reusable dispatcher now owns current-task,
  yield, block, requeue, rollback, and safe-point preemption policy; a
  generic EL0 task now yields through SVC, resumes after the trap, and exits by
  blocking through that dispatcher; physical timer events now coalesce into
  `need_resched`, defer while preemption is disabled, and switch a kernel task
  only after IRQ acknowledgement at a complete exception-return frame; a real
  lower-EL timer IRQ also suspends an EL0 task on its owned kernel stack, runs a
  kernel peer, and resumes the interrupted user computation before normal
  exit; a bounded two-tick schedule now preempts both EL0 and the kernel peer,
  then unwinds both owned IRQ frames in deterministic FIFO order; dispatcher
  quantum accounting also proves that four physical ticks with a two-tick
  slice create exactly two scheduling requests; scheduler code can now own and
  cancel a continuous timer lifetime after the same sequence; architecture
  callbacks now mask and restore local IRQ/FIQ state around normal generic
  dispatcher mutations, while timer accounting runs under IRQ-entry masking;
  this includes critical sections suspended across task switches; ARM64 user
  address spaces now publish a generic `vm_space_t` identity used by tasks,
  TTBR0/ASID activation, and SVC buffer validation; their bounded bootstrap
  mappings now publish sorted generic VMAs used for lookup and permission
  checks; L2/L3 user tables now grow on demand across multiple virtual regions
  after low-map retirement, while page unmap uses targeted ASID/VA invalidation
  and returns physical ownership with balanced hierarchy destruction; eager
  anonymous ranges prevalidate overlap, roll back partial allocation, cross L3
  boundaries, and reclaim empty tables; ARM64 now finishes bring-up by retiring
  the borrowed bootstrap task into a persistent timer-driven runtime where an
  owned-stack `kinit` task blocks on tick deadlines and an owned-stack `idle0`
  task waits in `WFI`, with wakeup and repeated task switching validated at IRQ
  return; a native-width generic syscall table now receives AArch64 SVC calls;
  generic process state covers fork/exec/wait/signals; an AArch64 ELF64 loader
  validates and populates `PT_LOAD` segments; and lazy `brk`/anonymous `mmap`
  reservations are populated by lower-EL translation faults and released by
  `munmap`; the completed production path now runs VFS-backed `execve`, fork,
  `/sbin/init`, mash and the common ELF64 userland.

Version 0.7 hardware milestone:

- Raspberry Pi 3 B+ boots as the AArch64 hardware reference;
- four Cortex-A53 CPUs participate in scheduling and pass sustained `kload`;
- SD/eMMC, ext2 root, UART console, procfs diagnostics, and shutdown are usable;
- Raspberry Pi 2 Model B v1.1 is supported by the ARM32 `raspi2` target;
- framebuffer/input/network remain future Raspberry Pi milestones;
- ASID-aware context-switch optimization remains blocked on a hardware-correct
  residency and invalidation design.

## 1. Unix Permissions

Status: started.

Immediate goals:
- Enforce read/write/execute permissions consistently at `open`, `access`, and
  `execve` boundaries.
- Keep root (`uid 0`) semantics explicit.
- Tighten ownership rules for administrative calls such as `chown`.
- Add regression coverage for user/root access decisions.

First milestone: implemented.
- `open()` maps its access flags to inode permission checks.
- `O_TRUNC` happens only after write permission is granted.
- `systest` covers read/write permission denial for non-root users.

## 2. `/dev` And TTY

Status: active and usable.

Immediate goals:
- Keep logical TTY policy independent from UART, display, and input drivers.
- Keep QEMU `tty1`/VirtIO-GPU optional and isolated from serial `tty0`.
- Keep Raspberry Pi display/input on `tty0` and PL011 recovery on `ttyS0`.
- Continue improving termios, canonical/raw behavior, job control, and device
  aliases such as `/dev/tty`, `/dev/tty0`, `/dev/tty1`, `/dev/ttyS0`,
  `/dev/console`, and `/dev/null`.
- Prepare the line discipline so future framebuffer/keyboard backends do not
  leak backend details into userland.

First milestone:
- `/dev/tty` resolves through the process controlling terminal.

## 3. `/proc`

Status: active.

Immediate goals:
- Keep Linux-like low-risk files (`meminfo`, `uptime`, `stat`, `mounts`,
  `interrupts`, `tty`, `net/dev`) readable from userland.
- Expand per-process entries (`status`, `stat`, `maps`, `fd`, `cwd`, `exe`,
  `root`) without holding scheduler locks while formatting large outputs.
- Add simple network visibility under `/proc/net`.

First milestone: implemented.
- Add `/proc/net/tcp` as a minimal TCP visibility endpoint.

## 4. Syscalls And Toolchain Support

Status: active.

The prioritized syscall and libc compatibility plan is maintained in
[`POSIX_COMPATIBILITY.md`](POSIX_COMPATIBILITY.md). It distinguishes missing
kernel primitives from interfaces that belong in newlib and records acceptance
criteria for each priority axis.

Immediate goals:
- Keep newlib as the reference libc.
- Keep `arm-none-eabi-gcc` as the kernel compiler and move userland toward
  native TinyCC incrementally.
- Fill the syscalls needed by small Unix tools and TCC before attempting larger
  packages.
- Keep syscall ABI glue documented and tested in `systest`.
- Avoid kernel changes driven only by TCC quirks unless they match a real Unix
  contract.

First milestone:
- Maintain `getcwd`, `fcntl`, `ioctl`, `stat`, `lstat`, `fstat`, `statfs`,
  `statvfs`, `fstatvfs`, `utimensat`, `futimens`, `getrlimit`, `setrlimit`, and
  process-control syscalls as stable contracts.
- Keep experimental TCC sources out of the default userland build unless
  `ENABLE_TCC=1` is set.
- Native TCC can compile and run `hello.c`, and can compile/link the ArmOS kilo
  source as a first non-trivial interactive program.
- Optional ncurses/nano build scripts exist and should become part of a regular
  terminal UI validation profile.

## 5. VFS And ext2 Hardening

Status: active.

Immediate goals:
- Keep ext2 read/write paths robust under parallel userland tests.
- Improve cross-filesystem behavior for userland tools (`cp`, `mv`, `rm`).
- Add lightweight consistency checks for ext2 metadata and block allocation.
- Keep FAT32 as a compatibility filesystem mounted manually on `/mnt`.

First milestone:
- Keep permission checks and mount metadata coherent across VFS filesystems.

## 6. Minimal Networking

Status: active.

Immediate goals:
- Keep virtio-net DTB probing automatic.
- Maintain `/proc/net/dev` counters and IRQ visibility.
- Preserve the common UDP, DNS and TCP transport layer across VirtIO-net and
  CYW43 without driver-specific socket policy.
- Extend the current TCP client with congestion control, adaptive buffering
  and broader socket options.
- Add an SMSC95xx USB Ethernet driver for the Raspberry Pi 3 wired port.
- Extend configuration mutation beyond the current read-only `ifconfig`
  surface once privilege and routing semantics are defined.

First milestone: complete.
- Expose TCP diagnostic state in `/proc/net/tcp`.
- Add a small `netstat` command based on `/proc/net/dev` and `/proc/net/tcp`.
- Initialize CYW43455 firmware, complete WPA2 association and DHCP on Raspberry
  Pi 3 B+, and validate BCDC Ethernet RX/TX with ARP and ICMP.
- Provide `ifconfig`, numeric-address `ping`, and generic interface counters
  through `/dev/netctl` and `/proc/net/dev`.
- Provide common active/passive TCP, UDP, DNS A resolution, POSIX socket data
  transfer, and an HTTP/1.1 `httpget` validation client.

## 7. Userland Init

Status: active.

Immediate goals:
- Keep `/sbin/init` in userland as PID 1.
- Keep zombie reaping reliable.
- Restart tty shells cleanly without coupling tty0 and tty1 lifetimes.
- Move toward a small declarative init configuration once the current behavior
  remains stable.

First milestone:
- Start ordinary `user` shells on available primary TTYs and require explicit
  `su` for administrative access.

## 8. Terminal UI Stack

Status: early but usable.

Immediate goals:
- Keep `TERM=armos` honest: terminfo should declare only ANSI capabilities that
  the console backend really implements.
- Use `cursestest`, `nano`, `kilo`, `mtop`, and `ttytest --interactive-*` as the
  terminal UI regression set.
- Keep ncurses/nano optional until build time and runtime behavior are boring.
- Preserve the platform console mapping before every graphical or
  curses-related change.

First milestone: implemented.
- Static ncurses cross-build with compiled fallback terminfo.
- Tiny GNU nano cross-build staged under `/opt/nano/bin`.

## 9. Multi-Arch Runtime

Status: ARM32 and ARM64 use the common production kernel.

Immediate goals:
- Keep architecture code limited to CPU, MMU, exception and context-switch
  mechanics.
- Continue replacing ambiguous address values with `paddr_t`, `vaddr_t`, and
  `pfn_t` at hardware boundaries.
- Keep generated `asm-offsets` as the only source of truth for C/ASM structure
  offsets.
- Keep FDT parsing centralized instead of adding more hardcoded QEMU addresses.
- Keep process, VFS, scheduler, syscall and device policy in the common kernel.
- Track the active migration sequence in
  [`docs/MULTIARCH_MIGRATION.md`](MULTIARCH_MIGRATION.md).

First milestone: implemented.
- Generated assembly offsets.
- Shared FDT parser.
- Address-type groundwork.
