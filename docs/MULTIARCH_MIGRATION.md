# ArmOS Multi-Arch Migration

This is a historical design record for the migration that started from the
`v0.6` tag and reached the production common-kernel contract in ArmOS 0.7.
Current architecture rules live in [ARCHITECTURE.md](ARCHITECTURE.md) and the
ARM64 runtime status lives in [ARM64_PORT.md](ARM64_PORT.md).

The migration restarted the multi-arch work from the v0.6 baseline. The goal
was not to invent a generic HAL up front. It made ARM32 dependencies visible,
contained, and testable, then let the second concrete port define the
interfaces that were actually needed.

## Current Rule

Keep the working ARM32/QEMU `virt` kernel boring while reducing architecture
leaks in small commits.

Good migration steps:

- replace ambiguous address values with `paddr_t`, `vaddr_t`, and `pfn_t`;
- move direct CP15, barrier, cache, timer, and exception details behind
  architecture-local `<asm/...>` helpers;
- keep generated `asm-offsets` as the only source of truth for C/ASM structure
  offsets;
- keep FDT discovery centralized;
- preserve the common logical-console contract while platforms map their UART,
  display, and input transports to `tty0`, `tty1`, or `ttyS0`.

Bad migration steps:

- create a speculative HAL without a second target;
- change syscall ABI conventions in the name of portability;
- hide ARM32 behavior behind vague names that lose ordering or cache semantics;
- move many files at once without a green ARM32 build.

## Phase A - Mechanical Cleanup

Status: mostly complete for direct inline-assembly cleanup.

Done in this branch:

- Added explicit ARM32 helpers for:
  - `cpu_relax()`;
  - inner-shareable data memory barrier;
  - inner-shareable data sync barrier;
  - inner-shareable write data sync barrier.
- Converted selected non-architecture code to use `<asm/arm.h>` instead of
  inline assembly:
  - VirtIO block;
  - VirtIO GPU;
  - VirtIO input;
  - ELF exec cache maintenance.
- Routed MMU, ASID, VBAR, PSCI/HVC, current-task register, IRQ diagnostic
  register, stack-register, and CPU wait operations through ARM helper APIs.
- Removed stale task-switch debug assembly hooks from generic task code.

Still to audit:

- expected architecture-local assembly only:
  - `arch/arm32/asm-offsets.c`, which emits generated C/ASM offsets;
  - the naked user-abort-to-SVC trampoline in the exception path;
- ARM-specific timer helpers in generic-looking headers;
- hardcoded ARM ELF checks before an AArch64 userland exists.

## Phase B - ARM32 Containment

Target shape:

```text
arch/arm32/
  boot/
  include/
  mmu/
  smp/
  syscall/
  task/
  timer/
```

This should be introduced gradually. A file moves only when its dependencies are
clear and the ARM32 build still compiles after the move.

Done so far:

- moved ARM32 CPU identity reads and `/proc/cpuinfo` data to
  `arch/arm32/cpu/cpu.c`, behind `arch_get_cpuinfo()`;
- moved ARM32 local boot CPU controls to `arch/arm32/cpu/cpu.c`, behind
  `arch_cpu` hooks for IRQ enable/disable, WFI, timer frequency, and branch
  predictor/MMU-control state;
- introduced `include/kernel/arch_barrier.h` so VirtIO/IDE drivers use
  architecture-neutral names for CPU relax, cache maintenance, and memory
  ordering primitives instead of including ARM helper headers directly;
- introduced `include/kernel/arch_irq.h` so generic task/process code treats
  saved interrupt state as an opaque architecture token;
- moved ARM32 boot assembly to `arch/arm32/boot/boot.S`;
- moved ARM32 IRQ entry assembly to `arch/arm32/interrupt/interrupt.S`;
- moved ARM32 exception/fault diagnostics to
  `arch/arm32/interrupt/exception.c`;
- moved ARM32 IRQ return-to-user work handling to
  `arch/arm32/interrupt/irq_return.c`;
- moved ARM32 GICv2 controller code to `arch/arm32/interrupt/gic.c`;
- moved ARM32 generic timer code to `arch/arm32/timer/timer.c`;
- moved ARM32 short-descriptor MMU/ASID code to `arch/arm32/mmu/mmu.c`;
- moved ARM32 TLB shootdown code to `arch/arm32/mmu/tlb.c`;
- moved the current ARM32 user-VM/page-table implementation to
  `arch/arm32/mmu/virtual.c`. This is containment, not the final VM
  abstraction boundary.
- moved the current ARM32 user ABI/copy/user-stack helpers to
  `arch/arm32/user/userspace.c`.
- moved ARM32/QEMU-virt memory detection to
  `arch/arm32/memory/memory_detect.c`.
- moved ARM32 syscall entry assembly to `arch/arm32/syscall/syscall.S`;
- moved ARM32 context-switch assembly to `arch/arm32/task/task_switch.S`;
- moved ARM32 PSCI/secondary CPU bring-up to `arch/arm32/smp/smp.c`;
- moved ARM32 PSCI `SYSTEM_OFF` to `arch/arm32/power/psci.c`, behind the
  narrow `arch_system_off()` hook used by generic shutdown;
- moved ARM32 exec machine validation and user-code cache maintenance to
  `arch/arm32/process/exec.c`, behind narrow hooks used by the generic ELF
  loader;
- moved generated assembly-offset source to `arch/arm32/asm-offsets.c`;
- moved ARM32 architecture headers to `arch/arm32/include/asm`;
- moved ARM32 spinlock instruction primitives to
  `arch/arm32/include/asm/spinlock.h`, leaving generic lock policy in
  `kernel/sync/spinlock.c`;
- taught the top-level `Makefile` to build these ARM32 architecture objects
  from `arch/arm32` and to search architecture headers first.
- introduced ARM32 platform fragments under `arch/arm32/platform/`, with
  `TARGET_PLATFORM=qemu-virt` now loading
  `arch/arm32/platform/qemu_virt/platform.mk` instead of hardcoding qemu-virt
  objects and CPU flags in the root Makefile.
- routed the default QEMU machine/CPU settings through the platform fragment
  and matching boot-script environment variables (`QEMU_MACHINE`,
  `QEMU_RUN_MACHINE`, `QEMU_CPU`).
- moved QEMU block/GPU/input/net boot-script defaults into platform-owned
  fragments (`platform.mk` and `qemu.sh`) so boot scripts can fail fast for an
  unsupported platform instead of silently launching a `qemu-virt` shape.
- made interrupt-target routing policy a named platform capability, currently
  `ARMOS_PLATFORM_IRQ_TARGETS_AUTO_MANAGED`, instead of inferring behavior from
  a hardcoded machine name.
- moved IRQ controller identity and line-count reporting behind
  `irq_controller_name()` / `irq_controller_line_count()`, so boot logs no
  longer hardcode `GIC: v2, 288 IRQs`.
- routed generic drivers through `irq_enable()` / `irq_enable_level()` instead
  of calling the ARM32 GIC backend symbols directly. The old `enable_irq*`
  helpers are now backend-private implementation details.
- renamed the generic platform MMIO/physical window contract from GIC-specific
  helpers to IRQ-controller helpers (`IRQCTRL`). QEMU virt still owns GICv2
  details in its platform header and backend, but generic MMU/address-space
  code no longer requires every platform to pretend it has a GIC.
- made VirtIO platform fields optional at the `arch_platform_*` boundary.
  Non-VirtIO boards should not publish fake MMIO values just to compile;
  callers must use `arch_platform_has_virtio_mmio()` before mapping or probing
  that optional window.
- made PL050 keyboard and legacy IDE fields optional too. A platform that does
  not expose those devices can omit their macros, and generic code must check
  `arch_platform_has_pl050_keyboard()` or `arch_platform_has_legacy_ide()`
  before touching the corresponding MMIO registers.
- moved PL011 baud configuration into the platform contract
  (`ARMOS_PLATFORM_UART0_CLOCK_HZ`, `ARMOS_PLATFORM_UART0_BAUD`) while keeping
  the current qemu-virt divisor behavior unchanged.

Kernel code that should remain architecture-neutral:

- scheduler policy;
- VFS and filesystems;
- procfs formatting;
- TTY core and line discipline;
- process lifecycle above register-frame details;
- signal policy above signal-frame layout;
- userland build and root filesystem layout.

This separation is an invariant. An architecture port must join the existing
kernel process, syscall, scheduler and VFS implementations; it must not grow an
architecture-local replacement for them. Temporary bootstrap models may prove
CPU, MMU and exception mechanisms, but they are not substitutes for common
kernel integration and must not become the production userspace path.

## Phase C - Second ARM32 Platform

Status: implemented.

The ARM32 Raspberry Pi family now has two explicit profiles:

- `TARGET_PLATFORM=raspi2` for Raspberry Pi 2 Model B v1.1 and QEMU
  `raspi2b` / Cortex-A7;
- `TARGET_PLATFORM=raspi3` for Raspberry Pi 3 hardware / Cortex-A53 in AArch32.

They select the common BCM283x UART, local interrupt controller, timer,
SD/eMMC, and power backends under `kernel/platform/raspberrypi/`, while
architecture and target-specific build flags and quirks remain separate. This
phase validated the platform interfaces without changing pointer width,
exception level, or page-table format.

Landed milestones:

- PL011 `tty0` recovery console;
- Raspberry Pi interrupt-controller backend implementing the generic IRQ/IPI
  contract;
- SD/eMMC block device, MBR partitions, dedicated FAT32 boot, and ext2 root;
- four-CPU scheduler participation and shutdown parking;
- raspi3-specific CPU identity, timer, firmware handoff, and AArch32 build
  profile;
- no fake VirtIO, PL050, or legacy IDE capabilities on Raspberry Pi targets.

The hardware build and validation contract is documented in
[`docs/RASPBERRY_PI3.md`](RASPBERRY_PI3.md).

## Phase D - AArch64

Status: implemented on `arm64/qemu-virt` and `arm64/raspi3`; both targets now
enter the common kernel and run the shared ELF64 userland.

The production AArch64 path now provides:

- EL2-to-EL1 entry, exception vectors, EL0 transition and complete task
  context switching;
- the common preemptive scheduler on four CPUs;
- dynamic physical memory, generic VMAs, lazy anonymous pages, fork/COW,
  ASIDs and SMP TLB shootdown;
- the common syscall dispatcher, VFS, ext2, FAT32, pipes, TTY and procfs;
- ELF64 `execve`, process lifecycle, signals and core dumps;
- a newlib AArch64 sysroot and the same installed userland paths as ARM32;
- QEMU VirtIO block storage and Raspberry Pi SD/eMMC through common drivers;
- QEMU VirtIO network and keyboard through the same drivers as ARM32, with
  VirtIO-GPU shared by both architectures inside the qemu-virt platform layer;
- common init, mash, kernel tasks, timer accounting and shutdown;
- artifacts isolated by architecture and platform.

Architecture code owns CPU registers, translation tables, TLB operations,
exception frames and context-switch assembly. Process, VM, scheduling, VFS and
device policy remain in the common kernel. The temporary ARM64 bootstrap task,
runtime, VFS and block implementations have been removed.

See [`docs/ARM64_PORT.md`](ARM64_PORT.md) for build and validation commands.

Remaining work is hardening rather than a parallel bring-up runtime: complete
high-half kernel execution, repeated mixed hardware stress, better ASID/TLB
residency tracking, and Raspberry Pi graphics, USB and networking.

## Validation Policy

Common-kernel changes must compile and boot on both QEMU reference ABIs:

```sh
make TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt platform-kernel
make TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt platform-kernel
```

Hardware-facing changes must additionally build `arm64/raspi3` and run the
validation sequence in `docs/RASPBERRY_PI3.md`. QEMU success does not replace
Pi 3 testing for timer, SD/eMMC, cache, TLB or firmware-entry behavior.
