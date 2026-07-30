# ARM64 Port

ArmOS supports AArch64 on QEMU `virt` and Raspberry Pi 3 B+ alongside the
ARM32 QEMU Virt and Raspberry Pi 2 targets. Every target enters the same kernel
at `kernel/main.c` and uses the same process, scheduler, syscall, VFS, device,
timer and shutdown subsystems. ArmOS 0.7 established this common-kernel
contract; 0.7.1 hardens it under sustained SMP process and storage stress.

The ARM64 port is an architecture backend, not a second kernel.

## Architecture Boundary

Code under `arch/arm64` is limited to mechanisms that are inherently tied to
ARMv8-A:

- EL2-to-EL1 entry, exception vectors and EL0 transitions;
- CPU identity and local interrupt state;
- translation-table operations, ASIDs and TLB maintenance;
- saved register context and stack switching;
- local interrupt-controller access, architectural timer access and platform
  power-control entry points.

Policy and resource ownership stay in the common kernel:

- process creation, execution, waiting, signals and core dumps;
- task states, scheduling policy, sleep deadlines and kernel tasks;
- physical-memory policy, VMAs, anonymous mappings and page faults;
- user-copy validation, lazy materialization, COW and signal-stack policy;
- VFS, ext2, FAT32, file descriptors, pipes, TTY and `/proc`;
- block and character devices, console behavior and system shutdown.

Platform files describe QEMU `virt` or Raspberry Pi 3 addresses and select
common drivers. There is no private ARM64 shell, VFS, block driver, task model
or bootstrap runtime.

In particular, `kernel/memory/virtual.c` owns the VMA model and fork/COW
policy, while `kernel/memory/usercopy.c` owns all syscall buffer access. An
architecture backend supplies only page-table lookup, mapping, permission and
TLB primitives; it must not walk the common VMA list or assign meaning to VMA
flags.

## Boot Path

The AArch64 boot sequence is:

1. `arch/arm64/boot/boot.S` selects the boot CPU, establishes an EL1 stack,
   clears BSS and installs the architectural entry conditions.
2. The minimal ARM64 MMU setup maps the low-linked kernel for EL1 and publishes
   high TTBR1 aliases for kernel MMIO and RAM.
3. Control enters the common `kernel_main()` in `kernel/main.c`.
4. Common initialization discovers memory, starts the allocator, interrupts,
   timer, devices, VFS, kernel tasks and process scheduler.
5. Common process execution loads ELF64 `/sbin/init` from ext2; init starts
   the AArch64 build of `/sbin/mash` in `/home/user`.

Each user address space owns a TTBR0 hierarchy and ASID. The current low-linked
kernel image requires its RAM region to remain mapped as EL1-only in every
TTBR0. EL0 cannot access that mapping. Moving the kernel image completely to
its TTBR1 high alias is a later hardening step.

Resident-page metadata and the L2/L3 table inventories grow dynamically. They
have no bring-up quotas: mapping succeeds until the physical allocator, kernel
heap or configured user virtual-address space is genuinely exhausted. The
remaining numeric bounds in the backend describe AArch64 hardware or the active
translation regime, such as 512 entries per table, the 39-bit TTBR0 range and
the implemented ASID width.

Executable publication is also an SMP contract. After `execve()` installs new
EL0 text, the common kernel cleans data-cache lines to the point of unification
and requests instruction-cache invalidation on every scheduler CPU before the
task may migrate. A local-only cache flush is insufficient on Cortex-A53 even
when the same workload appears correct under QEMU.

## Userland And Disk

ARM32 and ARM64 disk images use the same MBR, ext2 root, FAT32 boot partition,
directory tree, configuration and installed command set. Executables are built
for the selected ABI, but their paths and runtime behavior are the same.

Build the AArch64 userland and disk with:

```sh
./tools/build_arm64_userland.sh
./tools/build_arm64_disk.sh --skip-userland
```

Or use the integrated target:

```sh
make TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt platform-disk
```

Generated files are isolated under:

```text
build/images/kernel-arm64-qemu-virt.bin
build/images/kernel-arm64-qemu-virt.elf
build/images/kernel-arm64-qemu-virt.map
build/images/kernel-arm64-qemu-virt.dis
build/images/disk-arm64-qemu-virt.img
build/images/rootfs-arm64-qemu-virt.ext2
```

The Raspberry Pi 3 hardware image uses the same AArch64 userland and common
kernel services:

```sh
make TARGET_ARCH=arm64 TARGET_PLATFORM=raspi3 platform-disk
tools/build_raspberry_sd.sh --arch arm64 --platform raspi3 --mode image
```

See [Raspberry Pi 3](RASPBERRY_PI3.md) for the firmware handoff, SD-card,
counter-frequency, SMP, and hardware validation contracts.

## Validation

Build and boot QEMU 10.0.2 with:

```sh
make TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt -j4 platform-kernel
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./boot.sh
```

At the mash prompt, the basic parity sequence is:

```sh
pwd
ls
ps
sleep 1
uname -a
/sbin/shutdown
```

The common-kernel regression suites must also pass on both ABIs:

```sh
mmaptest
vfstest
systest
kload -s 5 -m 2048 -c 4 -u 25 -p 8 -f 1
iobench -f /tmp/iobench.dat -m 8 -b 64
nano /tmp/arm64-smoke.c
```

QEMU VirtIO network uses the same driver as ARM32. Graphics uses one
architecture-independent VirtIO-GPU backend owned by the qemu-virt platform. The
ARM64 platform supplies only its MMIO aliases, DTB discovery inputs and GIC
interrupt routing. Validate each optional profile independently:

```sh
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./boot-net.sh
TARGET_ARCH=arm64 TARGET_PLATFORM=qemu-virt ./boot-graphics.sh
```

The network boot must report a VirtIO MAC and IRQ. Running `netecho` in the
guest and connecting to the displayed forwarded host port validates RX, TX and
interrupt delivery. The graphics boot must report a 32-bit VirtIO framebuffer,
attach `tty1`, detect the VirtIO keyboard and start the cursor daemon while
keeping UART `tty0` usable. Both automatic QEMU login shells run as the
ordinary `user`.

Expected behavior matches ARM32: the initial directory is `/home/user`, disk
commands use the common VFS, sleeps use the common timer/scheduler path, and
shutdown exits QEMU through the platform power backend.

On Pi 3, AArch64 consumes the 19.2 MHz architectural counter rate reported by
`CNTFRQ_EL0`. The 1 MHz effective-counter quirk remains specific to the
AArch32 Pi 3 target. Sharing that quirk with AArch64 causes excessive timer
preemption and shortens both process sleeps and EMMC timeouts by a factor of
19.2.

ARM32 remains a required regression target after every common-kernel change:

```sh
make TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt -j4 platform-kernel
TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt ./boot.sh
```

## Next Work

1. High-link the complete ARM64 kernel and retire the temporary privileged
   TTBR0 RAM mapping.
2. Broaden repeated mixed CPU, VM, VFS, and SD stress on Pi 3 hardware.
3. Reduce conservative TLB maintenance only after per-CPU ASID residency,
   generation tracking and executable publication remain hardware-validated
   through repeated fork/exec/COW stress.
4. Add Raspberry Pi graphics, USB input, and networking through common device
   contracts.
