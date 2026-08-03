# Get Started - Kernel Dev ArmOS

This guide is for contributors who want to work on the ArmOS kernel: syscalls,
MMU, scheduler, process lifecycle, TTY, filesystems, VirtIO, and low-level ARM
runtime code.

Read this first:

- `README.md`
- `docs/ARCHITECTURE.md`
- `ROADMAP.md`
- `STABILITY.md`

ArmOS is small enough to understand, but it is now a real Unix-like kernel with
preemption, userspace, newlib, filesystems, signals, procfs, and a shell. Treat
small changes with the same care you would use in a larger kernel.

## Current Platforms

The fresh-checkout development target is `arm32/qemu-virt`. The supported
matrix also includes `arm64/qemu-virt`, Raspberry Pi 2 Model B v1.1 through
`arm32/raspi2`, and Raspberry Pi 3 B+ through `arm64/raspi3`.

All targets enter `kernel/main.c` and share scheduler, process, VM policy, VFS,
syscalls and device policy. Architecture code supplies CPU, MMU, exception,
context-switch, timer and interrupt mechanisms. Do not add an ARM32- or
ARM64-private implementation of a common kernel service.

Use a tracked configuration profile for reproducible work:

```sh
ARMOS_CONFIG=configs/qemu-virt-arm32.conf ./run.sh
ARMOS_CONFIG=configs/qemu-virt-arm64.conf ./run.sh
ARMOS_CONFIG=configs/raspi3-arm64.conf ./build.sh
```

Memory model:

- `TTBR0`: per-process user mappings in the architecture user range
- `TTBR1`: global privileged kernel mappings
- low boot identity window: the linked kernel image and early metadata remain
  reachable at their physical addresses during/after MMU bring-up
- explicit RAM direct-map window: general physical RAM is reached through
  `phys_to_virt()` / `virt_to_phys()`
- userland is not identity-mapped
- ASIDs are used for user TLB entries

Important consequence:

```text
Treat allocator results and page-table inputs as physical frames unless the code
has explicitly converted them to kernel virtual addresses.
```

Do not blur user pointers, kernel pointers, and physical addresses. The boot
identity window can make some low kernel addresses numerically equal to physical
addresses, but that is not the general RAM contract.

## Source Map

Kernel entry and architecture:

- `arch/arm32/boot/boot.S`
- `arch/arm64/boot/boot.S`
- `arch/arm32/interrupt/interrupt.S`
- `arch/arm64/interrupt/vectors.S`
- `arch/arm32/task/task_switch.S`
- `arch/arm64/task/context_switch.S`
- `kernel/main.c`
- `kernel/interrupt/`

Memory:

- `arch/arm32/mmu/mmu.c`
- `arch/arm32/mmu/virtual.c`
- `arch/arm64/mmu/mmu.c`
- `arch/arm64/mmu/user_vm.c`
- `kernel/memory/physical.c`
- `kernel/memory/kmalloc.c`
- `include/kernel/memory.h`
- `arch/arm32/include/asm/mmu.h`

Processes and scheduler:

- `kernel/task/task.c`
- `kernel/process/process.c`
- `kernel/process/fork.c`
- `kernel/process/exec.c`
- `kernel/process/signal.c`

Syscalls:

- `include/kernel/syscalls.h`
- `kernel/syscalls/syscalls.c`
- `kernel/syscalls/file.c`
- `kernel/syscalls/process_syscalls.c`
- `kernel/syscalls/shm.c`

Filesystems and devices:

- `kernel/fs/vfs.c`
- `kernel/fs/ext2_vfs.c`
- `kernel/fs/fat32_vfs.c`
- `kernel/fs/procfs.c`
- `kernel/drivers/virtio_block.c`
- `kernel/drivers/tty.c`
- `kernel/drivers/uart.c`

## Build And Run

Full rebuild and boot:

```sh
./run.sh
```

Boot existing kernel and disk:

```sh
./boot.sh
```

Kernel-only rebuild:

```sh
./build-kernel.sh
```

Exit QEMU:

```text
Ctrl+A, then X
```

Inside `mash`, start with:

```sh
systest
ttytest
ps
lps
ls -la /
ls -la /proc
```

For interactive QEMU automation, send carriage return (`\r`), not newline
(`\n`). The serial console path expects terminal-style Enter.

## Kernel Development Rules

Keep changes small.

Prefer one subsystem per commit:

- syscall ABI change
- scheduler fix
- filesystem fix
- TTY behavior change
- userland test addition

Do not commit generated artifacts:

- `kernel.bin`
- `kernel.elf`
- `kernel.map`
- `disk.img`
- `ext2.img`
- `fat32.img`
- object files
- dependency `.d` files
- stripped userland binaries

Keep crash diagnostics useful. A pretty panic path is less important than
capturing `TTBR0`, `TTBR1`, `TTBCR`, fault address, current task, and page-table
walks.

## Adding A New Syscall

Adding a syscall usually touches both kernel and userland glue.

### 1. Choose A Number

Prefer a Linux ARM32-compatible syscall number when the semantics are close.
Otherwise choose an ArmOS-private number and document it.

Edit:

```text
include/kernel/syscalls.h
```

Add:

```c
#define __NR_example 195
```

Also add the kernel prototype:

```c
int sys_example(int arg1, const char *user_ptr);
```

### 2. Implement The Kernel Function

Put the implementation in the file that owns the subsystem:

- file descriptor / VFS: `kernel/syscalls/file.c`
- process, pipes, chmod, mount, brk: `kernel/syscalls/process_syscalls.c`
- signals: `kernel/process/signal.c`
- shared memory: `kernel/syscalls/shm.c`
- generic dispatch or lifecycle: `kernel/syscalls/syscalls.c`

Return convention:

```text
success: non-negative result
failure: negative errno, for example -EINVAL, -EFAULT, -ENOENT
```

Do not set `errno` in the kernel. Newlib glue converts negative return values
to userland `errno`.

### 3. Validate User Pointers

Never trust a user pointer directly.

Use the existing user-copy helpers and patterns:

- copy strings from user before path lookup;
- copy structs in/out;
- validate buffer length;
- reject kernel-space addresses;
- handle partial failure cleanly.

Bad pattern:

```c
return do_kernel_work((char *)user_path);
```

Expected pattern:

```c
char kernel_path[MAX_PATH];
if (copy_string_from_user(kernel_path, user_path, sizeof(kernel_path)) < 0)
    return -EFAULT;
return do_kernel_work(kernel_path);
```

### 4. Add The Dispatch Table Entry

Edit:

```text
kernel/syscalls/syscalls.c
```

Add the syscall to `syscall_table`:

```c
[__NR_example] = (syscall_func_t)sys_example,
```

The table is intentionally cast through a common function pointer type. Keep the
actual `sys_*` prototype meaningful and documented in `syscalls.h`.

### 5. Think About Blocking

A syscall may yield before completion.

Examples:

- `waitpid`
- `nanosleep`
- TTY read
- pipe read/write
- VirtIO wait
- filesystem operation waiting on block I/O

If your syscall can block:

- set task state deliberately;
- avoid holding locks while yielding;
- preserve ownership of buffers and kernel stack data;
- make sure wakeup paths cannot enqueue the task twice;
- make sure interruption by signals is defined;
- test under short scheduler quantum.

The critical invariant:

```text
A task that yields in SVC/kernel context must resume in SVC/kernel context on
the saved kernel stack, not return directly to user mode.
```

### 6. Add Newlib Raw Glue

Edit:

```text
newlib-port/syscall_raw.S
```

Add:

```asm
RAW_SYSCALL sys_example, 195
```

This creates a raw wrapper that returns the kernel value unchanged.

### 7. Add Newlib POSIX Glue

Edit:

```text
newlib-port/syscalls.c
```

Add an extern:

```c
extern long sys_example(int arg1, const char *arg2);
```

Then add the newlib-facing function:

```c
int example(int arg1, const char *arg2)
{
    return ret_errno(sys_example(arg1, arg2));
}
```

Use `ret_errno()` for integer-returning APIs. For pointer-returning APIs, follow
the existing `_sbrk` and mapping patterns.

### 8. Add Public Headers If Needed

If the syscall exposes an ABI to userland, update:

```text
userland/include/
```

Examples:

- `termios.h`
- `sys/ioctl.h`
- `arm_os_abi.h`
- local structs used by tools/tests

Keep kernel-only structs out of userland unless they are intentionally ABI
stable.

### 9. Add Tests

Prefer adding test coverage to:

```text
userland/programs/systest/systest.c
```

For TTY behavior:

```text
userland/programs/ttytest/ttytest.c
```

Test at least:

- success path;
- invalid user pointer if practical;
- missing file / invalid fd / invalid pid;
- permission failure if relevant;
- signal or interruption behavior if the syscall blocks;
- fork/exec/wait interaction if process state is touched.

### 10. Rebuild And Validate

Minimum validation:

```sh
./run.sh
```

Inside ArmOS:

```sh
systest
ttytest
ps
lps
```

For memory/process changes:

```sh
memstress 8192 30
systest &; systest &
```

For TTY changes:

```sh
ttytest
ttytest --interactive-canon
kilo /home/user/hello.c
```

For filesystem changes:

```sh
touch /tmp/a
echo hello > /tmp/a
cat /tmp/a
rm /tmp/a
mkdir -p /tmp/a/b/c
rm -rf /tmp/a
sync
```

## Scheduler And Preemption Checklist

Before touching scheduler-sensitive code, know these invariants:

- no dead task should be scheduled;
- no zombie should remain runnable;
- no task should be inserted twice in the ready queue;
- state transitions should be done with interrupts/locks handled consciously;
- a task blocked in kernel must resume its kernel call chain;
- signal stop/continue/kill must wake or remove tasks consistently;
- idle is special and must always remain schedulable as fallback.

Stress with:

```sh
systest &; systest &; systest &
kload -s 30 -m 256 -c 8 -u 25 -p 4 &
mtop
```

Watch:

- `sched-refuse`
- `ready-refuse`
- `zombies live`
- `kstack live (+alloc/-free)`
- `phys live (+alloc/-free)`
- `asid-roll`
- `unintr-timeout`

## MMU And ASID Checklist

Before touching mappings:

- confirm whether the address is user VA, kernel VA, or physical;
- confirm whether the page belongs to TTBR0 or TTBR1;
- confirm which ASID owns the user mapping;
- invalidate TLB after PTE updates;
- handle ASID rollover as a normal path;
- never expose MMIO to user mappings accidentally;
- use `paddr_t`, `vaddr_t`, `phys_to_virt()`, and `virt_to_phys()` at every
  physical/virtual boundary instead of relying on numeric address equality.

Crash dumps are your friend. A data abort with page-table walk output usually
tells you whether the failure is a user mapping, kernel mapping, stale ASID, or
bad pointer.

## Filesystem And Block I/O Checklist

Filesystem code can run in syscall context and may hit VirtIO waits.

Rules:

- do not hold broad locks around long I/O waits;
- keep user-copy boundaries outside fragile FS internals when possible;
- return negative errno;
- update `stat`, `getdents`, and path behavior consistently;
- test both ext2 root and FAT32 compatibility when a VFS behavior is generic;
- call `sync` in persistence tests.

Useful commands:

```sh
df
mount
ls -la /
ls -la /mnt
mkdir -p /tmp/fs/a/b
echo ok > /tmp/fs/a/b/file.txt
cp -r /tmp/fs /tmp/fs2
rm -rf /tmp/fs /tmp/fs2
```

## TTY Checklist

TTY changes are deceptively subtle.

Watch:

- canonical vs raw mode;
- `VMIN` / `VTIME`;
- `ECHO`, `ICANON`, `ISIG`, `OPOST`, `ONLCR`, `ICRNL`;
- Ctrl-C, Ctrl-Z, Ctrl-D;
- foreground process group;
- restoring terminal settings after interrupted programs;
- background jobs printing while the shell owns the prompt.

Run:

```sh
ttytest
ttyinfo
kilo /home/user/hello.c
sleep 20
```

Use Ctrl-C and Ctrl-Z interactively. Automated tests do not catch every terminal
state leak.

## Commit Hygiene

Keep generated artifacts out of commits. Good commit shape:

1. kernel implementation;
2. userland/newlib glue if needed;
3. tests;
4. docs if the ABI changed.

Commit messages should be in English.

Example:

```text
Add readlink syscall support
```

## Common Failure Patterns

Bad user pointer:

- kernel page fault on low or strange address;
- copy helper logs `source not in user space`;
- fix by copying/validating user buffers.

Wrong syscall resume path:

- task yields inside syscall;
- later resumes with corrupted SVC stack or skips kernel call chain;
- fix context switching/state handling, not random stack values.

Stale ASID/TLB:

- process sees another process mapping;
- page fault appears impossible from current page tables;
- audit ASID rollover and TLB invalidation.

Double close / reused fd object:

- parent fd points to freed/reused file object;
- output disappears or `f_op` becomes invalid;
- audit fork/exit/wait lifecycle and refcounts.

TTY state leak:

- shell returns but arrows/raw keys behave incorrectly;
- previous foreground app died without restoring termios;
- add restore path or kernel-side cleanup.

## Definition Of Done

A kernel change is not done until:

- it builds cleanly;
- QEMU boots;
- `systest` passes;
- relevant focused tests pass;
- `lps` counters look sane;
- generated artifacts are not staged;
- docs are updated if ABI or architecture changed.
