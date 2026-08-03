# ArmOS POSIX Compatibility Roadmap

This document tracks source-level POSIX compatibility work for ArmOS. It is
not a certification claim and it is not a request to mirror another kernel's
syscall ABI. POSIX specifies application-visible behavior; ArmOS remains free
to implement that behavior with a smaller architecture-neutral kernel ABI and
newlib wrappers.

The current common dispatcher exposes 125 syscall entries. It already covers
the central Unix process, VFS, descriptor, signal, virtual-memory, TTY, polling
and identity contracts. The next stage is therefore semantic completion and a
small number of missing primitives, not one syscall for every function listed
in the POSIX System Interfaces volume.

ArmOS 0.7.1 is the first release carrying the complete implemented baseline
listed below on both ARM32 and ARM64. Later work must preserve that ABI parity:
an interface is not considered delivered when only one architecture can use
the common kernel implementation.

## Design Rules

- All architectures use the same syscall numbers, kernel implementations and
  POSIX behavior. Architecture code only marshals registers and handles
  CPU/MMU-specific state.
- Add a syscall only when the behavior cannot be implemented correctly in
  newlib from existing primitives.
- A symbol that always succeeds without enforcing its contract is not treated
  as implemented.
- Optional POSIX facilities must be reported honestly through `<unistd.h>`,
  `sysconf()`, `pathconf()` and `fpathconf()`.
- Kernel signal numbers remain an internal ArmOS UAPI. The newlib boundary
  translates signal arguments, masks, wait status and the number delivered to
  application handlers in both directions. A handler must always observe the
  constants from the libc against which it was compiled.
- Every new contract must be tested on the reference `arm32/qemu-virt` target
  and then on `arm64/qemu-virt` and `arm64/raspi3` where applicable.

## Current Baseline

The following families are already connected to the common kernel:

| Area | Existing foundation |
| --- | --- |
| Processes | `fork`, `execve`, `_exit`, `waitpid`, `wait4`, process groups and sessions |
| Descriptors | `open`, `openat`, `close`, `read`, `write`, `pread`, `pwrite`, `readv`, `writev`, `lseek`, `dup`, `dup2`, partial `fcntl` |
| VFS | `stat`, `lstat`, `fstat`, `fstatat`, `getdents`, links, directories, `mkdirat`, `unlinkat`, `renameat`, ownership, permissions and truncation |
| Signals | `kill`, `sigaction`, `sigprocmask`, `sigpending`, `sigsuspend`, signal return and ArmOS/newlib number translation |
| Memory | `brk`, private `mmap`, `munmap`, partial `mprotect` and ArmOS shared-memory calls |
| Waiting | `select`, `poll`, `nanosleep`, pipes and blocking TTY I/O |
| Time and accounting | `time`, `gettimeofday`, `clock_gettime`, `clock_getres`, `times`, `getrusage`, priorities and capability-aware `sysconf` |
| Networking | Common IPv4, ICMP, DHCP, UDP, DNS and TCP sockets with active and passive connections |

### FreeBSD `/bin/sh` integration increment

The FreeBSD `sh` port extends the common POSIX surface rather than hiding
missing behavior in shell-local compatibility code. The shared ARM32/AArch64
path now includes a truthful 64 KiB `ARG_MAX`, multipage executable argument
stacks, `WCONTINUED`, `SIGPIPE`, named FIFOs, event-driven pipe polling,
`pathconf()`/`fpathconf()`, and realtime clock interpolation. Userland adds
`execlp()`, `getconf`, `id`, `mkfifo`, `mktemp`, `tr`, and more complete file
and text utilities.

`/bin/sh` points to the pinned FreeBSD shell with reusable `libedit` support.
Graphical user terminals select it through `SHELL=/bin/sh`. Mash loads
`$HOME/.mashrc`; that file exports `ENV=$HOME/.shrc`, allowing an interactive
`sh` entered from mash to load the same configuration. Prompts identify both
the session user and active shell. `source` is available as an alias of `.`,
and `/bin/clear` supplies shell-independent terminal clearing. `/sbin/mash`
remains the boot and recovery shell. The current imported corpus baseline is
511 passes out of 518 tests. The open-FIFO
unlink lifetime issue remains a common VFS gap and is not considered delivered
until namespace removal and final-object reclamation are both correct.

## Priority Axes

### P0 - ABI Truth And Core Primitives

This axis removes misleading or incomplete contracts and supplies primitives
needed by a broad range of otherwise simple Unix programs.

| Interface or family | Current state | Required work |
| --- | --- | --- |
| `sched_yield` | Implemented | Common scheduler handler and symmetric ARM32/ARM64 wrappers |
| `clock_gettime`, `clock_getres` | Realtime and monotonic clocks implemented | Preserve the common UAPI and improve realtime resolution when hardware RTC support grows |
| `clock_nanosleep` | Relative and absolute realtime/monotonic sleep implemented | Preserve direct POSIX error returns and extend clock support only with real clock sources |
| `pread`, `pwrite` | Implemented for seekable files | Preserve the shared open-file offset and reject non-seekable descriptors with `ESPIPE` |
| `openat` and `*at` path operations | Implemented with dirfds stable across `chdir` | Replace the stored canonical base path with dentry-relative lookup so an open directory also survives namespace renames |
| `fchmod`, `fchown` | Implemented for ext2 descriptors | Extend descriptor metadata updates when writable filesystems gain equivalent inode operations |
| `fdatasync` | Implemented with conservative VFS sync | Add per-file data-only flush callbacks to avoid syncing unrelated metadata |
| `sysconf` | Limits, memory, CPUs, I/O vectors and selected capabilities implemented | Keep partial facilities explicitly unsupported and add selectors only with their complete contracts |

Acceptance criteria:

- the interfaces have identical semantics on ARM32 and ARM64;
- invalid descriptors, pointers, clocks and flags return the documented error;
- `pread` and `pwrite` do not move the descriptor offset;
- dirfd-relative operations remain correct if the process working directory
  changes concurrently;
- `systest` contains ABI and behavioral tests for every added primitive.

### Delivered P0 Foundation

The first P0 increment establishes three contracts shared by every
architecture:

- `sched_yield()` enters the common scheduler and never carries
  architecture-specific scheduling policy;
- `clock_gettime()` and `clock_getres()` expose `CLOCK_MONOTONIC` from the ARM
  generic counter and `CLOCK_REALTIME` from the current ArmOS wall-clock
  source;
- a fixed UAPI time structure isolates the kernel from newlib's different
  ARM32 and ARM64 `struct timespec` layouts. The released 32-bit `nanosleep`
  wire layout remains stable and newlib translates it explicitly.

Realtime resolution is currently one second because ArmOS only has a
second-resolution wall-clock source. The monotonic clock uses the effective
platform counter frequency and reports its corresponding nanosecond
resolution. Neither result is derived from a CPU delay-loop calibration.

`sysconf()` now distinguishes three outcomes: a numeric limit, a supported
optional capability, and a known but unsupported capability. The latter
returns `-1` without changing `errno`, as POSIX requires. In particular,
ArmOS does not yet claim `_SC_VERSION`, `_SC_TIMERS`, `_SC_MAPPED_FILES`,
`_SC_MEMORY_PROTECTION`, `_SC_SHARED_MEMORY_OBJECTS`, or saved-ID support.

Positioned I/O uses a fixed signed 64-bit offset in the ArmOS UAPI. The common
kernel creates a private view of the open file for each `pread()` or `pwrite()`
operation, so backend reads and writes can advance that private cursor without
changing the offset shared by `dup()` or `fork()`. `pwrite()` also honors its
explicit offset on an `O_APPEND` descriptor. Current ext2 and FAT32 inode sizes
remain 32-bit; newlib reports `EOVERFLOW` before entering the kernel when an
application requests an offset outside that filesystem range.

Directory-relative I/O now exposes `openat()`, `fstatat()`, `mkdirat()`,
`unlinkat()` and `renameat()` through one architecture-neutral resolver. Open
directory descriptors retain their canonical absolute path, so relative
operations remain anchored to the descriptor after `chdir()`. Absolute paths
ignore `dirfd`, `AT_SYMLINK_NOFOLLOW` controls the final `fstatat()` lookup,
and `AT_REMOVEDIR` selects directory removal for `unlinkat()`. ARM32 and ARM64
newlib use the same constants and kernel syscall numbers; `systest` combines
all five interfaces and verifies descriptor, path and flag errors.

Descriptor metadata operations now expose `fchmod()` and `fchown()` through
the same permission and ext2 inode-update paths as their pathname variants.
`fdatasync()` validates the open descriptor and currently uses the common VFS
flush, which is persistence-safe but may write unrelated metadata. `systest`
checks successful descriptor updates, ownership rules and invalid-descriptor
errors on both ABIs.

`clock_nanosleep()` now uses the same interruptible scheduler deadline path as
`nanosleep()` while exposing a separate signed 64-bit time ABI. Relative and
`TIMER_ABSTIME` sleeps accept both realtime and monotonic clocks, past absolute
deadlines return immediately, and absolute waits recheck their clock after
every scheduler wakeup so they never return before the requested deadline. The
newlib wrapper preserves the POSIX rule that this interface returns an error
number directly instead of setting `errno`.

### P1 - Complete Existing Contracts

This axis turns currently partial implementations into dependable POSIX
building blocks.

| Interface or family | Current limitation | Required work |
| --- | --- | --- |
| `mmap` | No non-zero file offset, `MAP_SHARED`, `MAP_FIXED` or `PROT_NONE` | Pass an offset through the ABI, support shared mappings and define safe fixed-map replacement rules |
| `mprotect` | Only accepts an exact complete VMA | Split and merge VMAs for arbitrary page-aligned subranges |
| `msync` | Missing | Flush dirty shared file-backed pages through VFS |
| `fcntl` locks | Lock requests are accepted but not enforced | Add open-file-description lock state, conflict detection and interruptible `F_SETLKW` waiting |
| `pselect`, `ppoll` | Mask change and wait are separate operations | Make signal-mask replacement and blocking atomic in the kernel |
| `getrlimit`, `setrlimit` | `RLIMIT_NOFILE` stored, inherited and enforced; other known limits report infinity | Enforce address-space, data, stack, core, file-size and CPU limits in their owning subsystems |
| `statvfs`, `fstatvfs` | Implemented for ext2 and FAT32 paths and descriptors | Extend mount flags and statistics when new filesystem backends are added |
| `futimens`, `utimensat` | Implemented with ext2 second-resolution persistence | Preserve nanoseconds when filesystem inode formats grow higher-resolution fields |
| Process identity | Basic UID/GID only | Add effective-ID changes, supplementary groups and complete permission checks |
| `waitid` | Missing | Expose non-destructive status queries and the required selection modes |

Acceptance criteria:

- partial mappings can be protected and unmapped without damaging adjacent
  regions;
- shared mappings are visible between processes and persist through `msync`;
- descriptor locks block and wake without spinning;
- signal delivery cannot be lost between mask replacement and `pselect` or
  `ppoll` sleep;
- resource limits are enforced by the subsystem that consumes the resource.

`statvfs()` and `fstatvfs()` expose block, inode, filesystem identity and name
length information through an architecture-neutral 64-bit UAPI structure.
Newlib translates that structure into its native POSIX types, preserving the
same source interface and counter widths on ARM32 and ARM64. Path queries
validate that the named object exists, while descriptor queries retain their
filesystem association after the file has been opened.

`utimensat()` and `futimens()` share the existing VFS permission and inode
metadata path. They support explicit timestamps, `UTIME_NOW`, `UTIME_OMIT`,
directory-relative lookup and `AT_SYMLINK_NOFOLLOW`. The UAPI transports signed
64-bit seconds and nanoseconds consistently across both architectures; ext2
currently persists whole seconds because its supported inode format has no
high-resolution timestamp fields. Cached VFS objects are canonicalized by ext2
inode number so path and descriptor metadata remain coherent. The same metadata
repair also makes legacy `utime()` persist access and modification times
correctly.

`RLIMIT_NOFILE` is now process state rather than a newlib constant. The soft
and hard limits cross a fixed 64-bit UAPI, survive `fork()`, constrain common
descriptor allocation and duplication, and drive `_SC_OPEN_MAX`. Known limits
whose consumers do not yet enforce them remain readable as infinity but cannot
be changed, so applications do not receive a misleading successful update.

### P2 - Threads And POSIX Synchronization

ArmOS exposes a userspace POSIX thread runtime over shared process state,
scheduler-visible tasks, TLS, and futexes instead of a syscall per
`pthread_*` function.

Implemented kernel primitives:

- create and exit a thread sharing its process VM and descriptor table;
- wait for thread termination through clear-child-TID plus futex wake;
- set and restore architecture-specific thread-local storage;
- wait on and wake a user-memory word, with timeout.

Implemented userspace surface:

- create, join, detach, attributes, once control, cleanup, TSD, and deferred
  cancellation;
- mutexes, condition variables, read/write locks, barriers, spin locks,
  unnamed semaphores, timed waits, and robust owner-death recovery;
- compiler TLS through ELF `PT_TLS` and `__thread` on ARM32 and ARM64.

Remaining semantics include per-thread signal masks and pending signals,
multithreaded `fork`/`exec`/exit rules, process-shared synchronization,
priority protocols, asynchronous cancellation, and named semaphores.

Acceptance criteria:

- thread creation does not duplicate the process VM;
- blocking synchronization consumes no CPU while waiting;
- process exit terminates and reaps all threads safely;
- SMP stress covers contention, timeout, signal interruption and owner death.

### P3 - Complete Socket And Asynchronous Interfaces

The architecture-neutral network core now serves both VirtIO-net and CYW43.
It provides UDP, DNS A-record resolution, passive and active TCP sockets,
bounded retransmission, peer-window handling, FIN/RST processing, socket
timeouts, `shutdown()`, and ordinary `read()`/`write()` on connected sockets.
Newlib exposes `getaddrinfo()`, `send()`, `recv()`, `sendto()` and
`recvfrom()` over the common kernel ABI.

| Family | Current state | Remaining work |
| --- | --- | --- |
| Connected TCP | Implemented for IPv4 clients and servers | Add congestion control, selective acknowledgements and larger adaptive queues |
| Data transfer | `read`, `write`, `send`, `recv`, `sendto` and `recvfrom` implemented | Add `sendmsg` and `recvmsg` |
| Socket control | `shutdown` implemented | Add `getsockopt`, `setsockopt`, `getsockname` and `getpeername` |
| Name resolution | DNS A records through `getaddrinfo` | Add search domains, multiple answers, IPv6 and `/etc/hosts` |
| Local IPC | Pipes are available | Add `socketpair`, preferably over a generic local socket backend |
| POSIX timers | Not implemented | Add `timer_create`, `timer_settime`, `timer_gettime`, overrun accounting and deletion |
| Asynchronous I/O | Not implemented | Begin with a newlib worker-thread implementation; add kernel support only if measurements justify it |

`httpget` is the end-to-end acceptance tool: the same HTTP/1.1 client binary
must resolve a host and retrieve a response over VirtIO-net on QEMU and CYW43
Wi-Fi on Raspberry Pi 3.

## Interfaces That Should Stay In Newlib

These functions can be composed from the kernel primitives and should not
receive dedicated syscalls:

- `system`, `popen`, `posix_spawn` and the `exec*` family;
- `sleep`, `usleep`, directory streams and pathname helpers;
- `realpath`, `dirname`, `basename` and passwd-file lookup;
- static portions of `pathconf`, `fpathconf` and `confstr`;
- most pthread synchronization operations once wait/wake and TLS exist;
- an initial asynchronous-I/O implementation based on worker threads.

## ABI Cleanup

Two current ArmOS names conflict with established POSIX contracts:

- ArmOS `shm_open(name, size, flags)` is not POSIX `shm_open(name, oflag,
  mode)`. Keep the existing mechanism under an ArmOS-specific name while a
  descriptor-based POSIX shared-memory interface is introduced.
- `ARMOS_NR_SHUTDOWN` powers off the machine, while POSIX `shutdown()` operates
  on a socket. Rename the internal operation to `poweroff` before adding socket
  shutdown, while preserving the old syscall number as a compatibility alias.

The public syscall-number header should eventually describe the complete
shared ABI. Kernel-only duplicate number definitions should be reduced once
ARM32 and ARM64 wrappers consume that header from generated assembly constants.

## Delivery Order

1. Wire `sched_yield`; add monotonic/realtime clock queries and honest
   capability reporting.
2. Add position-based I/O and dirfd-aware path resolution.
3. Complete file metadata, synchronization and atomic polling contracts.
4. Complete `mmap`/`mprotect`, then provide POSIX shared memory and `msync`.
5. Add the minimal thread wait/wake and TLS substrate, then implement pthreads
   primarily in newlib.
6. Generalize the socket layer and only then broaden network protocols.
7. Maintain a generated conformance matrix mapping every targeted POSIX
   interface to `kernel`, `newlib`, `optional`, `partial` or `unsupported`.
