# ArmOS Architecture Notes

This document describes the current high-level architecture of ArmOS. It is
intended for contributors who need to understand the kernel before touching MMU,
task switching, process memory, filesystems, drivers, or userland ABI code.

ArmOS targets ARMv7-A and AArch64 on QEMU `virt`, Raspberry Pi 2, and
Raspberry Pi 3. Both architectures enter one common kernel and share process,
VFS, filesystem, syscall, scheduler, and TTY policy. Platform code maps the
available UART, framebuffer, keyboard, block, and network devices onto those
common interfaces. QEMU keeps serial `/dev/tty0` and an optional graphical
`/dev/tty1`; Raspberry Pi 3 uses HDMI or ILI9341 plus USB input as primary
`/dev/tty0` and retains PL011 as recovery `/dev/ttyS0`.

## Common-Kernel Architecture Rule

Every supported architecture attaches to the same kernel core. Process
lifecycle, scheduler policy, syscalls, VFS, filesystems, descriptors, pipes,
TTY policy, IPC, signals and generic virtual-memory policy belong to `kernel/`
and must not be reimplemented under `arch/`.

Architecture code is limited to CPU and MMU mechanisms: boot entry, exception
and interrupt entry/return, register and task-context transfer, page-table and
TLB operations, cache maintenance, atomics and the hardware timer primitives
needed by the common kernel. Board and SoC device discovery belongs to the
platform layer and drivers, not to an architecture-specific kernel runtime.

In particular:

- all architectures enter the common kernel initialization path;
- syscall numbers and implementations are shared across architectures;
- architecture code may copy or validate user ABI state, but does not own
  process, descriptor or filesystem semantics;
- ELF and signal frame layout may have architecture backends while lifecycle
  and policy remain common;
- early bootstrap readers and bounded test models must be retired before a
  userspace milestone is described as VFS or process integration;
- a new architecture must keep existing architecture builds and behavior
  nominal while it is connected to the common core.

## Boot And DTB Detection

QEMU passes a Device Tree Blob (DTB/FDT) pointer to the kernel at boot. ArmOS
saves that pointer very early in `arch/arm32/boot/boot.S`, before clearing
`.bss`, into the global `dtb_address`.

Current boot contract:

```text
QEMU / ARM boot protocol  -> r2 contains ATAGS/DTB pointer
boot.S                    -> stores r2 in dtb_address
kernel C code             -> reads dtb_address
```

Some old comments still mention `R0 = DTB address`; the code path that matters
today stores `r2`. Treat the comments as cleanup candidates unless the boot
protocol is intentionally changed.

### FDT Parser Scope

ArmOS has a small in-kernel FDT parser, not a full libfdt port.

Implemented helpers include:

- FDT magic validation;
- big-endian `fdt32_to_cpu` conversion;
- node scan by prefix/name;
- property lookup inside a node;
- simple device-presence checks.

The parser is intentionally narrow. It is currently used for early platform
detection, not as a complete generic device model.

### Memory Detection From DTB

`arch/arm32/memory/memory_detect.c` first tries to read the `/memory` node and its
`reg` property. On QEMU `virt`, that gives the RAM base and size.

Expected memory shape:

```text
memory.reg -> base 0x40000000, size detected from QEMU
```

If DTB parsing fails, ArmOS falls back to probing/intelligent detection paths.
The DTB path is preferred because it avoids guessing.

### DTB Reservation

The physical allocator reserves the DTB area before handing pages to the buddy
allocator.

Current behavior:

- `reserve_dtb_pages()` uses `dtb_address`;
- reserves a conservative 1 MB DTB window;
- moves `buddy_base` after the reserved DTB area;
- marks those pages used in the physical allocator.

This is important because QEMU places the DTB in RAM. If the buddy allocator
reused those pages, later DTB reads or platform probes could become corrupted.

### VirtIO Detection From DTB

The VirtIO block driver can discover its MMIO base from DTB.

Current behavior:

1. scan FDT nodes matching `virtio_mmio`;
2. read `reg`;
3. convert the physical MMIO base to the private kernel MMIO alias;
4. verify VirtIO magic and block-device ID;
5. derive or read the IRQ;
6. fall back to the old fixed VirtIO address if DTB probing fails.

The driver currently prefers the local QEMU virtio-mmio IRQ numbering derived
from the MMIO slot when available. DTB interrupt cells are parsed, but the GIC
routing still uses ArmOS' local interrupt numbering model.

### Contributor Rules For DTB Work

1. Keep DTB parsing read-only after boot unless a proper FDT ownership model is
   introduced.
2. Reserve DTB memory before the buddy allocator can reuse it.
3. Do not assume every device has hardcoded QEMU `virt` addresses forever.
4. Prefer DTB-derived MMIO base and IRQ values, with explicit fallback paths.
5. Be careful with endianness: FDT cells are big-endian.
6. Keep the parser small unless there is a concrete platform-porting need.

### DTB Cleanup Targets

- Fix stale boot comments that describe the wrong argument register.
- Use the real FDT `totalsize` for reservation instead of a fixed 1 MB window.
- Centralize FDT helpers in a dedicated `kernel/platform/` or `kernel/devicetree/`
  module if more drivers start using DTB.
- Parse `#address-cells`, `#size-cells`, and interrupt-parent data properly
  before relying on non-QEMU platforms.
- Expose useful platform facts through procfs later, for example
  `/proc/device-tree` or a minimal `/proc/platform`.

## Address Space Model

ArmOS uses a split virtual address space:

```text
0x00000000 - 0x3FFFFFFF   TTBR0   per-process user address space
0x40000000 - 0xFFFFFFFF   TTBR1   global kernel address space
```

The effective split boundary is `0x40000000`. The MMU setup writes `TTBCR.N=2`
for this layout. Some older comments and helper names still mention other split
sizes; treat those as historical unless the runtime register setup says
otherwise.

### Kernel RAM Mappings

Kernel RAM is no longer documented as a global `VA == PA` contract. The ARM32
port now has two distinct kernel-side RAM mappings:

```text
boot identity window:
  VA 0x40000000..KERNEL_BOOT_IDENTITY_END -> same physical address

explicit RAM direct-map window:
  VA KERNEL_DIRECT_MAP_BASE..KERNEL_DIRECT_MAP_END
  -> PA VIRT_RAM_START..corresponding RAM end
```

The boot identity window exists so the low-linked kernel image and early boot
metadata keep working while the final split address space is installed. It is a
compatibility window, not a general allocator, DMA, or driver contract.

General RAM access should be expressed through typed physical and virtual
addresses:

```c
paddr_t frame = ...;
void *kva = (void *)phys_to_virt(frame);
```

User page tables receive physical frame addresses. Kernel code that needs to
touch those frames should use the kernel alias returned by `phys_to_virt()`.
Conversely, `virt_to_phys()` is the explicit boundary when a kernel alias has to
be handed to page-table or DMA-facing code.

Some legacy allocator APIs still expose physical-looking values as `void *`
while the type cleanup continues. Treat those values as physical frames unless
the code has explicitly converted them to a kernel virtual address. Do not add
new code that depends on arbitrary RAM being dereferenceable at the same numeric
address.

The explicit direct map is simple and fast on QEMU `virt`, but it is also a
portability boundary. Raspberry Pi, AArch64, high memory, stricter DMA, or
non-identity kernel mappings require keeping `paddr_t`, `vaddr_t`,
`phys_to_virt()`, and `virt_to_phys()` honest.

### User Address Spaces

User processes do not run identity-mapped.

Each process owns a `vm_space_t` with its own `TTBR0` page directory and ASID.
User virtual addresses below `0x40000000` map to physical frames allocated from
kernel RAM.

Typical user layout:

```text
0x00008000 / 0x00010000   ELF text/data area
0x08000000                user heap start
0x30000000 - 0x33FFFFFF   shared memory window
0x3F000000                user stack top
0x3FFFF000                upper user/signal boundary
```

Example mapping:

```text
user VA 0x00008000 -> PA 0x54ABC000
user VA 0x08000000 -> PA 0x54DEF000
```

So from userland, virtual addresses are stable process-local addresses. They are
not physical addresses and must never be trusted by the kernel without
`copy_from_user`, `copy_to_user`, or a controlled temporary mapping path.

### TTBR Responsibilities

`TTBR0`:

- active user process mappings;
- process-specific page tables;
- ASID-tagged translations;
- switched on context switch.

`TTBR1`:

- global kernel mapping;
- low boot identity window for the linked kernel image;
- explicit RAM direct-map aliases for general physical memory;
- kernel text/data/bss/stacks;
- kernel temporary mappings;
- private MMIO aliases;
- not copied into user address spaces.

The task context stores the process `TTBR0` and ASID. The scheduler/task switch
path restores them when switching to another user process. Kernel mappings stay
global through `TTBR1`.

## MMU Runtime Model

ArmOS uses ARMv7-A short-descriptor page tables.

At runtime:

- `TTBR0` points to the active process page directory;
- `TTBR1` points to the global kernel page directory;
- `TTBCR.N=2` selects the `0x40000000` split boundary;
- user translations are ASID-tagged;
- kernel translations are global through `TTBR1`.

The MMU bring-up path creates enough early mappings to keep the kernel running
while switching from physical execution assumptions to the final split model.
After that point, contributors should reason in terms of:

```text
user pointer      -> TTBR0, process-local, must be copied/validated
kernel pointer    -> TTBR1, global, boot identity/direct-map/MMIO/temp alias
physical address  -> frame/device address inserted into descriptors or MMIO
```

Do not treat these three categories as interchangeable. The low boot identity
window can make some early kernel addresses numerically equal to physical
addresses, but that is a compatibility detail, not the general memory model.

### Page Table Shape

The kernel uses:

- L1 section mappings for large kernel/RAM and MMIO regions;
- coarse L1 entries pointing to L2 tables for user pages;
- 4 KB small pages for normal user mappings.

User page table helpers create L2 tables lazily. If a user page is mapped,
unmapped, remapped, made read-only, or made writable, the code must keep the PTE
and TLB state coherent for the target ASID.

## ASID Model

ASIDs let ArmOS keep user TLB entries tagged by process address space instead
of globally flushing on every context switch.

Current expectations:

- each `vm_space_t` owns an ASID;
- the task context stores the active `TTBR0` and ASID;
- context switch restores both the page directory and ASID;
- user TLB invalidations should target the page/ASID when possible;
- ASID rollover must flush stale user translations before reusing identifiers.

The ASID pool is intentionally small on ARMv7. Stress tests that create many
short-lived processes can trigger ASID rollover. That is expected; stale
translations after rollover are not.

Contributor rules:

1. Never reuse an ASID without going through the VM/ASID allocator.
2. Do not cache process address-space assumptions outside `vm_space_t` and task
   context.
3. Any code that changes a user PTE must invalidate the relevant user
   translation.
4. Treat ASID rollover as a normal runtime path, not an exceptional debug-only
   path.

Useful diagnostics:

- `lps` reports ASID rollover counters;
- crash dumps print `TTBR0`, `TTBR1`, `TTBCR`, and page-table walks;
- procfs/task diagnostics should remain ASID-aware.

## SMP Bring-Up Model

Four-CPU scheduling is part of the 0.7 release contract on `arm64/qemu-virt`
and `arm64/raspi3`. The fresh-checkout `arm32/qemu-virt` profile remains a
conservative development default, while `arm32/raspi2` keeps the validated
Raspberry Pi 2 hardware path. `SMP_CPUS` is still configurable so single-CPU
regression runs can isolate scheduler and coherency failures.

The Raspberry Pi 3 profile is AArch64-only. Common scheduler, VM, VFS and
process policy must behave identically across architectures; platform code may
only provide CPU-release, interrupt, timer, MMU and cache mechanisms.

Current SMP contract:

- `smp_processor_id()` reads the ARM MPIDR CPU id;
- `smp_init_boot_cpu()` records the boot CPU during early kernel startup;
- SMP CPU state is explicit: `offline`, `booting`, `parked`, or `online`;
- `/proc/smp` exposes the boot CPU, online count, seen CPU mask, and per-CPU
  state, scheduler participation, timer/IPI counters, TLB counters, and the
  currently running task on each CPU;
- spinlocks use architecture primitives (`LDREX`/`STREX` on ARMv7 and
  acquire/release exclusives on AArch64), not compiler-only test-and-set
  helpers;
- spinlocks record the owning CPU for diagnostics;
- TLB maintenance now has two layers: local ARM helpers in `asm/mmu.h`
  (`tlb_flush_*`) and SMP-aware kernel entry points in `kernel/tlb.h`
  (`tlb_shootdown_*`);
- `IRQ_SGI_TLB_SHOOTDOWN` is the TLB IPI and is visible in `/proc/interrupts`;
  `/proc/smp_ipi` can trigger a full shootdown rendezvous for diagnostics;
- the scheduler has SMP-aware task ownership and runqueue protection;
- process reaping returns task, kernel-stack and physical-page ownership only
  after no CPU can still schedule the dead task;
- executable publication performs the required data and instruction cache
  maintenance on every scheduler CPU before a freshly loaded task can migrate.

ARM32 short-descriptor page tables and ARM64 long-descriptor tables have
different shareability and invalidation instructions, but they implement the
same VM contract. ASID reuse must pass through generation-aware allocation,
and page-table or executable changes must be visible to every CPU that may run
the address space. A QEMU-only success is insufficient for changing these
rules; Raspberry Pi 3 hardware stress remains the coherency reference.

Release validation includes mixed `kload`, `memstress`, `systest`, `vfstest`,
`mmaptest`, `mtop`, TTY, procfs, native compilation and shutdown runs. Live
task, zombie, kernel-stack and physical-page counters must return to baseline
after the workload.

## MMIO Mapping

Drivers should use the private kernel MMIO aliases, not raw low physical device
addresses.

Current aliases:

```text
0xF0000000   GIC
0xF0100000   UART / RTC window
0xF0200000   VirtIO window
```

These aliases live in `TTBR1`, keeping runtime driver access in kernel space.
Some low identity/device mappings may still exist for bootstrapping and
compatibility, but new driver work should prefer the high aliases.

## Page Mapping Rules

Contributors should follow these rules:

1. User virtual addresses must be below the split boundary.
2. Kernel pointers must not be accepted from userland.
3. Allocator pages are physical frames first; convert with `phys_to_virt()`
   before dereferencing them as kernel memory outside the boot identity window.
4. User page table entries should receive physical frame addresses, not kernel
   virtual aliases.
5. On changes to user PTEs, invalidate the relevant TLB entry with the correct
   ASID.
6. Do not map MMIO into user `TTBR0` unless a real device-user ABI is being
   designed.
7. Keep `copy_from_user` / `copy_to_user` boundaries strict, especially around
   `execve`, signals, filesystem syscalls, and pipes.

## Process Memory

Process memory is represented by VMAs plus page-table state.

Important regions include:

- executable mappings loaded by `execve`;
- user heap grown by `brk` / `sbrk`;
- user stack;
- signal frame region;
- shared memory mappings;
- copy-on-write fork mappings.

The fork path creates a child address space with its own `TTBR0`. Copy-on-write
state is tracked through page permissions and page fault handling. When the
kernel needs to copy or inspect a physical frame, it uses the kernel RAM alias
from `phys_to_virt()`. The user-visible mappings remain process-local.

## Context Switching And Syscalls

The kernel supports switching away from a process while it is inside a syscall.
This matters for syscalls such as `waitpid`, `nanosleep`, pipe I/O, TTY reads,
and block I/O waits.

The key rule is:

```text
If a task yields while still executing in kernel/SVC context, it must resume in
kernel/SVC context with its saved kernel stack pointer.
```

The task switch path must not confuse:

- a syscall returning normally to user mode; with
- a blocked syscall resuming its kernel call chain.

ArmOS stores both user context and kernel/SVC context. The syscall entry path
saves the canonical user registers. The task switch path saves the current
kernel stack pointer when switching out. Resuming a blocked syscall must restore
that saved kernel stack, not reset to the top of the SVC stack.

This was a historically important bug class in ArmOS: if a blocked syscall was
restored as if it were ready to return directly to userland, the kernel call
chain was abandoned and the SVC stack appeared corrupted.

## Scheduler

ArmOS uses kernel tasks and user processes represented by `task_t`. User
processes have an attached process structure and VM space; pure kernel threads
do not necessarily have user memory.

Important scheduler concepts:

- each task has a dedicated kernel stack;
- each user task stores user registers and kernel/SVC resume state;
- ready/running/sleeping/stopped/zombie states drive scheduling decisions;
- the scheduler must never run a dead or zombie task;
- the idle task is a kernel task and remains built into the kernel;
- init is currently a userland process and is responsible for userland process
  supervision/reaping policy.

The ready queue is a kernel-owned structure. Code that changes task state must
be careful about interrupt state and queue membership. A task should not be
inserted twice into the ready queue, and a task that is no longer runnable must
not remain schedulable.

### Scheduling Policy

The current policy is exposed as `priority-rr-debt` in `/proc/sched`.

It is intentionally still simpler than Linux CFS, but it borrows the most useful
idea for ArmOS today: runnable tasks should not be ordered only by FIFO position
inside a priority queue. A CPU-bound task accumulates scheduling debt on timer
ticks. A task that waits in the ready queue sees its effective debt decay. The
picker then compares:

1. effective priority, including bounded aging;
2. lower CPU debt;
3. longer ready-queue wait time.

This keeps the existing fixed-priority model and `nice` mapping easy to reason
about, while avoiding the worst behavior of pure priority round-robin: a small
set of CPU-bound tasks can no longer indefinitely dominate equally prioritized
interactive or I/O-heavy tasks.

Important implementation details:

- `task_t.sched_debt` is incremented by the timer while the task is running;
- debt is decayed when the scheduler evaluates a ready task;
- `ready_since_tick` remains the source for bounded aging and debt decay;
- `/proc/sched` exposes `aging_selections` and `debt_selections` so stress tests
  can show whether the non-FIFO parts of the policy are active.

This is a pragmatic "mini-CFS" step, not a full virtual-runtime tree. The
current implementation scans the ready queues and is acceptable for ArmOS'
current `MAX_TASKS` scale. If task counts grow much further, the next scheduler
optimization should be data-structure driven rather than policy driven.

Contributor rules:

1. Keep all fields referenced by assembly layout stable. New `task_t` fields
   should be added away from the low-level context block unless the assembly
   offsets are updated deliberately.
2. Do not mutate visible priority to implement fairness. Use effective priority
   and debt scoring at selection time.
3. Test fairness with concurrent CPU-bound jobs plus sleeping or I/O-heavy jobs,
   not only with a single busy process.
4. Watch `/proc/sched`, `mtop`, and `lps` together: `debt_selections` should rise
   under CPU contention, and sleeping/ready work should still make progress.

### Blocking In Kernel

Many kernel paths may block before their syscall has completed:

- `waitpid`;
- `nanosleep`;
- pipe reads/writes;
- TTY reads;
- VirtIO waits;
- filesystem/block I/O paths.

When this happens, the task is still executing on its kernel stack. The context
switch must save and later restore that kernel stack exactly. Returning directly
to userland is only valid after the syscall handler has completed the syscall
return path.

This is one of the most important invariants in the kernel.

## Timer And Preemption

The ARM generic timer drives periodic scheduling.

The timer interrupt updates kernel time and may request rescheduling. ArmOS now
has two safe-preemption paths:

- normal kernel-safe scheduler points such as syscall return, explicit
  `yield()`, sleep/wait paths, and other scheduler entries;
- an IRQ return-to-user slow path that captures the interrupted user frame,
  switches to a scheduler-safe SVC continuation, runs pending return-to-user
  work such as signal delivery and preemption, then restores the user frame.

The second path is what lets a CPU-bound user process be preempted even when it
does not voluntarily enter the kernel. It is deliberately narrow: it only runs
when returning from IRQ to user mode and when the current CPU is not inside a
kernel critical section.

Practical model:

- the timer IRQ should keep its own work short;
- it may set a reschedule flag;
- syscall return paths, scheduler-safe points, and IRQ return-to-user honor
  that request;
- sleeping tasks are woken when their deadline expires;
- long critical sections should avoid keeping interrupts disabled longer than
  necessary.

This distinction matters during scheduler testing. IRQ return-to-user
preemption must preserve the same invariant as syscall return: user registers,
banked user `SP/LR`, `SPSR`, `TTBR0`, ASID, and the task's SVC stack state must
remain coherent if the task is resumed on a different CPU after `yield()`.

The scheduler quantum is a tuning parameter, but it is also a stress tool. A
short quantum exposes missing critical sections and context-save bugs quickly.
A longer quantum can make the system feel more responsive under slow emulated
I/O because it reduces context-switch churn.

Contributor rules:

1. Do not call heavy filesystem or block code directly from timer IRQ context.
2. Keep IRQ-disabled sections small.
3. Assume a task can be interrupted frequently.
4. Any code that yields in kernel mode must preserve the kernel call chain.
5. Treat `nanosleep`, TTY reads, pipe waits, and VirtIO waits as preemption
   test cases.
6. If `task_context_t` changes, regenerate and review `build/generated/asm-offsets.h`;
   syscall, task-switch, and IRQ return-to-user assembly all depend on those
   generated offsets.

## TTY And Console Model

ArmOS exposes a small Unix-like TTY model with multiple backends.

Current consoles:

```text
QEMU virt:
  tty0   UART serial console
  tty1   optional VirtIO-GPU console with VirtIO input

Raspberry Pi:
  tty0   HDMI or ILI9341 display with USB input
  ttyS0  PL011 recovery transport, without an automatic login shell
```

TTY identity is common-kernel policy; UART, framebuffer, and keyboard drivers
are transports registered by a platform. QEMU keeps serial `tty0` usable when
graphics are absent. Raspberry Pi promotes its display/input pair to `tty0`
and retains PL011 as `/dev/ttyS0`; if display initialization fails, PL011 is
attached to `tty0` as the boot fallback.

### TTY Core

The TTY layer owns:

- canonical and non-canonical input behavior;
- `termios` flags such as `ICANON`, `ECHO`, `ISIG`, `OPOST`, `ONLCR`, `ICRNL`;
- control characters such as `VINTR`, `VSUSP`, `VEOF`, `VERASE`, `VKILL`;
- foreground process group checks;
- terminal-generated signals such as `SIGINT`, `SIGTSTP`, and `SIGTTIN`;
- `/dev/tty`, `/dev/tty0`, `/dev/tty1`, `/dev/ttyS0`, and `/dev/console`
  behavior.

Backends should feed characters into the TTY core and consume output from it.
They should not reimplement line discipline rules independently.

### UART Backend

The UART backend is deliberately conservative. It is the debugging and recovery
transport used by QEMU's terminal.

Contributor rules:

1. Keep the platform's primary `tty0` fully functional after every
   graphical-console change.
2. Preserve serial-console carriage-return behavior. Interactive command
   injection through QEMU/screen typically needs `\r`, not just `\n`.
3. Avoid long IRQ-disabled sections in UART or TTY output paths.
4. Route input to the logical TTY chosen by platform policy, never to a
   transport-specific driver object.

### Graphical Backend

The QEMU graphical console uses VirtIO-GPU as a framebuffer-style text console. It
keeps its own text cell snapshot, renders a bitmap font, handles ANSI color and
cursor movement, and flushes dirty framebuffer regions to the device.

Current graphical-console features:

- framebuffer text output through `tty1`;
- Spleen bitmap font by default;
- ANSI color handling sufficient for `ls`, `mtop`, and `kilo`;
- vertical-bar cursor with blinking driven by a kernel `displayd` task;
- simple scrollback history;
- VirtIO input keyboard routing into `tty1`;
- Mac French keyboard fallback mapping for the development host.

The `displayd` kernel task exists so cursor blinking and deferred graphical
work do not live in the idle task. This also validates pure kernel tasks without
an attached user process.

### Graphical Scrollback

The graphical backend keeps a bounded in-memory scrollback buffer. Keyboard
shortcuts currently enqueue scroll requests:

```text
Shift+Up / Shift+Down     one line
Option+Up / Option+Down   one page
```

VirtIO input runs in interrupt context, so it must not directly render or flush
the framebuffer. It only records a scroll request. `displayd` consumes those
requests in task context and performs the redraw.

Any new graphical operation should follow the same rule:

```text
IRQ context: enqueue minimal state
task context: render, flush, or perform slow work
```

### Current Limits

The graphical console is usable, but it is not a full terminal emulator:

- no host-style mouse selection or copy/paste;
- no dynamic resize propagation yet;
- no virtual-console switching such as Alt-F1 / Alt-F2;
- no full UTF-8/accent rendering;
- scrollback is intentionally simple;
- QEMU graphical boot starts ordinary `user` shells on both available TTYs;
- Raspberry Pi starts one ordinary `user` shell on its display-backed `tty0`.

Future work should move toward a clearer split between TTY core, line
discipline, backend drivers, and session startup (`getty`/login-style policy).

## Interrupts And Exception Context

ARM exception modes have banked registers and stacks. ArmOS relies on separate
mode stacks for exception handling.

Important implications:

- SVC mode owns the syscall/kernel execution stack for a task;
- IRQ mode handles interrupts and must not corrupt SVC task state;
- abort/undefined handlers need valid mode stacks before printing diagnostics;
- user banked `SP/LR` must be restored correctly before returning to user mode;
- exception dumps should capture banked registers early, before helper calls
  clobber useful state.

When debugging crashes, prefer preserving exception diagnostics over making the
handler pretty. The page-table walk information in abort handlers is often the
fastest way to distinguish user pointer bugs, kernel pointer bugs, stale ASIDs,
and missing mappings.

## Block Device, VFS, Ext2 And FAT32

The storage stack is layered:

```text
block transport: VirtIO or Raspberry Pi SD/eMMC
        |
sector read/write API
        |
fixed ArmOS disk partition layout
        |
filesystem drivers: ext2, FAT32
        |
VFS inode/file_operations layer
        |
syscalls: open/read/write/getdents/stat/link/unlink/rename/...
        |
newlib/userland commands
```

### VirtIO Block

The active block driver is `kernel/drivers/virtio_block.c`.

Current model:

- VirtIO MMIO device on QEMU `virt`;
- DTB-based MMIO base discovery when possible;
- fallback fixed MMIO base for compatibility;
- private kernel MMIO alias through `KERNEL_MMIO_VIRTIO_ADDR`;
- one global legacy split virtqueue;
- synchronous read/write request submission;
- interrupt-driven completion when available;
- polling/timeout fallback around used-ring progress;
- one active block operation serialized by `virtio_blk_lock`;
- optional device flush support if `VIRTIO_BLK_F_FLUSH` is negotiated;
- read-only device detection through VirtIO feature bits;
- request bounds checks against detected device capacity.

The driver exposes the generic sector API used by filesystems:

```c
int blk_read_sectors(uint64_t lba, uint32_t count, void *buffer);
int blk_write_sectors(uint64_t lba, uint32_t count, void *buffer);
int blk_read_sector(uint64_t lba, void *buffer);
int blk_write_sector(uint64_t lba, void *buffer);
```

Transport counters are available through `/proc/diskstats`. Storage changes
should be measured with `iobench`; the reproducible procedure and current QEMU
baselines are in [STORAGE_PERFORMANCE.md](STORAGE_PERFORMANCE.md).

Contributor rules:

1. Keep VirtIO request buffers in RAM that has a stable kernel alias via
   `phys_to_virt()` unless DMA translation is introduced explicitly.
2. Do not hold unrelated filesystem locks while waiting for VirtIO completion.
3. Always clear pending wait state after request completion, timeout, or error.
4. Treat timeout as device failure and avoid silently reusing a bad queue.
5. Keep cache/DMA barriers around descriptor, avail ring, used ring, data, and
   status buffers.
6. Keep QEMU `virt` limitations visible: this is not yet a general PCI/VirtIO
   transport layer.

### Disk Partition Layout

ArmOS generates a real MBR partition table for `disk.img`. The build still
keeps `include/kernel/disk_layout.h` as the compiled fallback, but the kernel
reads sector 0 at boot and updates the runtime layout from the MBR before VFS
mounts `/`.

```text
LBA 0       MBR     partition table
virtio0p1   ext2    LBA 2048          512 MB  root filesystem
virtio0p2   FAT32   LBA after p1      64 MB   compatibility mount
```

The root filesystem is ext2. FAT32 is kept for compatibility and cross-checking,
not as the canonical full userland filesystem.

The MBR path is deliberately small: it recognizes the first ext2 partition
(`0x83`) and the first FAT32 partition (`0x0b` or `0x0c`). If the disk has no
valid MBR, ArmOS falls back to the compiled layout.

### VFS

The VFS layer provides common in-memory `inode_t`, `file_t`,
`inode_operations_t`, and `file_operations_t` abstractions.

VFS responsibilities:

- path lookup and symlink resolution;
- mount table dispatch;
- file descriptor allocation;
- permission checks;
- open/read/write/lseek/getdents/stat/fstat/lstat routing;
- common handling for `/`, `/proc`, `/dev`, `/mnt`, and mounted roots.

Mount table entries store:

```text
mount path
source
filesystem type
options
root inode
```

The root ext2 filesystem is considered the base filesystem. Additional mounts
such as procfs and FAT32 are resolved by longest matching mount path.

### Ext2

Ext2 is the primary ArmOS filesystem.

Implemented capabilities include:

- mount from a partition LBA;
- superblock and group descriptor parsing;
- inode load/store;
- directory lookup and readdir;
- regular file read/write;
- file creation/truncation/append;
- mkdir/rmdir;
- unlink;
- rename inside ext2;
- hard links;
- symlinks and readlink;
- chmod/chown metadata updates;
- stat/statfs/fstat support;
- block and inode bitmap updates;
- direct, single-indirect, and double-indirect block paths as implemented by
  the driver.

Ext2 operations are protected by an ext2 operation lock. This keeps the current
implementation simple. Be careful not to hold that lock across unrelated
blocking paths longer than necessary.

The block cache is set-associative and can retain dirty data and allocation
metadata. Sequential writeback groups up to 16 contiguous 4 KiB blocks into a
single 64 KiB block request. The superblock is pinned while mounted, so free
block and inode counters no longer cause a read/write pair for every
allocation. Runtime cache and writeback counters are exposed in
`/proc/fs/ext2/stats`.

Contributor rules:

1. Keep on-disk metadata updates ordered and check every write return.
2. Update inode metadata in memory and on disk consistently.
3. Keep link counts correct for files, directories, hard links, and symlinks.
4. Never leave partially-created directory entries after allocation failure.
5. Test create/write/truncate/append/rename/unlink/rmdir paths through
   `systest`.
6. Run persistence tests with `sync`, `shutdown`, then `boot.sh`.

### FAT32

FAT32 is the compatibility filesystem, usually mounted at `/mnt`.

It exists to keep a simple exchange/test filesystem around, but it is no longer
expected to mirror the full ext2 root filesystem.

Current role:

- list/read/write simple files;
- compatibility mount for `/mnt`;
- exercise VFS cross-filesystem behavior;
- provide an intentionally simpler filesystem for regression comparison.

Sequential I/O coalesces contiguous cluster runs into block requests of up to
128 sectors. FAT synchronization tracks an exact dirty entry range instead of
rewriting the whole table. Runtime counters are exposed in
`/proc/fs/fat32/stats`.

Important differences from ext2:

- FAT32 permissions are synthesized, not native Unix metadata;
- ownership/link semantics are limited;
- long filename support may differ from ext2;
- cross-filesystem rename needs userland fallback (`copy` + `unlink`) rather
  than a pure VFS rename;
- directory and allocation semantics are less Unix-like.

Contributor rules:

1. Do not assume ext2 features exist on FAT32.
2. Keep user-facing tools tolerant of cross-filesystem differences.
3. Test `/mnt` explicitly when changing VFS-generic code.
4. Prefer ext2 for canonical ArmOS behavior and FAT32 for compatibility.

### Sync And Persistence

The persistence path is:

```text
filesystem metadata/data writes
        |
blk_write_sector(s)
        |
selected block transport request
        |
transport flush/stop contract
        |
shutdown / boot.sh verification
```

For changes that touch storage, a useful manual test is:

```sh
echo persistent > /tmp/persist.txt
sync
shutdown
./boot.sh
cat /tmp/persist.txt
```

If QEMU is killed with `Ctrl+A, X`, recent userland state such as shell history
or filesystem writes may not have the same clean shutdown behavior. Prefer the
ArmOS `shutdown` command when validating persistence.

### Shutdown Sequence

The ArmOS shutdown path is designed to be explicit and observable, closer to the
Unix/Linux mental model than a direct emulator exit.

Current order:

```text
userland shutdown command
        |
sys_poweroff
        |
mark shutdown in progress
        |
SIGTERM to non-essential user processes
        |
short grace period
        |
SIGKILL to remaining targets
        |
short grace period
        |
force-terminate any remaining shutdown targets
        |
sync mounted filesystems
        |
unmount non-root filesystems
        |
flush/stop block device
        |
disable interrupts
        |
PSCI SYSTEM_OFF
```

The process that requested shutdown, PID 1, and the login ancestor chain are not
killed early. This avoids userland init seeing the shell disappear and starting a
replacement shell while shutdown is already in progress.

Shutdown is still not a full Linux init runlevel transition. It is a kernel-led
poweroff sequence with Unix-like signal grace. That is sufficient for the
current system and much safer than directly marking every process dead.

Contributor rules:

1. Keep shutdown logs concise but explicit: signal phase, sync phase, unmount
   phase, block-device phase, final poweroff.
2. Do not depend on a specific display backend for shutdown. QEMU retains
   UART `tty0`; Raspberry Pi retains the separate PL011 `/dev/ttyS0` path.
3. If storage code changes, validate with `sync`, `shutdown`, and a fresh
   `boot.sh`, not only with `Ctrl+A, X`.
4. If process lifecycle changes, test shutdown while a long-running background
   process is alive.

## Filesystem Layout

The default runtime layout is:

```text
/                  ext2 root filesystem
/proc              procfs
/mnt               FAT32 compatibility mount when mounted
/dev               device nodes
/bin               core utilities
/sbin              system programs
/usr/bin           ArmOS user programs
/opt/<tool>/bin    imported external tools
/legacy            archived legacy userland
```

The common scheduler publishes its fixed-point 1, 5 and 15 minute
runnable-task averages through `/proc/loadavg`.  The metric is maintained in
architecture- and platform-independent scheduler code; monitoring tools only
consume this shared ABI and do not derive private load values.

Ext2 is the primary filesystem. FAT32 remains useful for compatibility and
testing, but it is not expected to mirror the full root filesystem.

## Userland Direction

Newlib is the supported C library path for current userland development.

The older in-tree libc and older programs are archived for reference and
bring-up archaeology. New commands should use the newlib build path unless a
specific legacy investigation requires otherwise.

## Portability Notes

Version 0.7 established production ARM32 and ARM64 ports joined to one common
kernel. Version 0.7.1 extends the same contract with symmetric POSIX calls,
storage paths, ASID/COW hardening and cross-CPU executable publication. The
portability groundwork includes:

- generated assembly offsets for C structures consumed from ARM assembly;
- `paddr_t`, `vaddr_t`, and `pfn_t` names for address categories;
- a shared FDT parser used by platform/device discovery;
- architecture-specific helpers isolated under `arch/arm32` and `arch/arm64`;
- clearer documentation of boot identity, explicit RAM direct-map, and MMIO
  assumptions.

Platform and architecture backends now provide their own RAM, MMIO, interrupt,
timer and translation-table contracts. Generic kernel code must not assume:

- that RAM starts at one fixed address;
- one page-table format or exception level;
- general RAM is reached through the explicit direct-map window, not by assuming
  arbitrary `VA == PA`;
- that device MMIO addresses match QEMU `virt`;
- that pointers and registers are 32-bit;
- DMA-capable buffers are physical frames whose CPU alias is obtained through
  `phys_to_virt()`.

Continue using explicit physical/virtual conversion helpers wherever a value
crosses between these domains:

```text
physical address  -> paddr_t
kernel pointer    -> vaddr_t / void *
user pointer      -> vaddr_t copied through usercopy helpers
page frame number -> pfn_t
```

New ports must enter through `kernel/main.c` and join the common task, VM,
syscall, VFS and driver contracts. Architecture-local replacements for those
services are not accepted.

## Known Cleanup Targets

- Rename or fix stale TTBR split comments and helper names.
- Audit linker symbols that are wider than 32-bit address space expectations.
- Make physical addresses and kernel virtual addresses distinct in type names.
- Continue reducing old MMU debug paths that no longer represent the runtime
  design.
- Keep crash dumps and page-table walkers; they are still valuable diagnostics.
