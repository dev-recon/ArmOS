# FreeBSD top port to ArmOS

## Naming contract

- `/bin/mtop` is the historical ArmOS monitor.
- `/usr/bin/top` is reserved for the FreeBSD port.
- The two programs remain independently buildable during bring-up so that
  `mtop` stays available as a diagnostic fallback.

## Architecture

The FreeBSD display, command, sorting and terminal code remains generic
userland code. The ArmOS adaptation is split into two explicit layers:

1. `compat/`: missing libc/BSD compatibility interfaces only;
2. `machine_armos.c`: translation of the stable ArmOS `/proc` ABI into the
   `machine.h` structures expected by FreeBSD top.

No Raspberry Pi, QEMU, ARM32 or ARM64 condition belongs in these layers. If a
metric is unavailable, the backend reports it as unavailable instead of
probing platform drivers.

## Porting lots

1. **Imported frontend**
   Keep `commands.c`, `display.c`, `screen.c`, `top.c`, `username.c` and the
   public headers close to upstream. Link against the existing target
   ncurses/termcap bundle.
2. **ArmOS machine backend — initial implementation complete**
   Process snapshots come from `/proc/tasks`, `/proc/<pid>/status` and
   `/proc/<pid>/cmdline`; CPU accounting comes from `/proc/stat`, memory from
   `/proc/meminfo`, load averages from `/proc/loadavg`, and CPU count from the
   per-CPU lines in `/proc/stat`.
3. **Interactive operations — frontend wired, extended validation pending**
   Process filtering and ordering use the standard FreeBSD frontend. Kill and
   renice go through the existing POSIX interfaces and their normal permission
   checks. Unsupported jail, swap and I/O views remain follow-up work.
4. **Build and regression tests — initial lot complete**
   `BUILD_FREEBSD_TOP=yes` selects the cached cross-build bundle. It installs
   only `/usr/bin/top` and its license. ARM32 and ARM64 compile and link as
   target-native ELF files. A private ARM64/QEMU boot validates interactive and
   batch display, `/proc` parsing, CPU accounting, process totals and newlib's
   command-line option reset contract. Hardware validation remains separate.

## Required kernel/userland contract

The port consumes interfaces that are useful beyond top:

- stable process identity, state, owner, priority, CPU time, RSS and command;
- stable aggregate/per-CPU accounting;
- scheduler-wide 1, 5 and 15 minute runnable-task averages through
  `/proc/loadavg`;
- terminal size through `TIOCGWINSZ` and resize notification through
  `SIGWINCH`;
- `kill(2)`, `getpriority(2)` and `setpriority(2)` semantics.

Missing fields should be added to the common `/proc` or POSIX ABI only when
they are generally useful. The application must not reach into kernel task
lists or platform-specific drivers.
