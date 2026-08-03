# FreeBSD `sh` Porting Dossier

This document records the implemented port of the FreeBSD system shell to
ArmOS. It covers the upstream baseline, portability audit, kernel and libc
work, target source layout, build strategy, validation results, remaining
compatibility gaps, and the runtime contract of ArmOS `/bin/sh`.

The intended result is a real Bourne/POSIX-style system shell suitable for
running build scripts and BSD userland tools. It is not intended to remove
`mash`: `mash` remains the rescue shell and the fallback while the new shell is
being brought up.

## Executive Summary

The port is feasible. A cross-compilation probe showed that the FreeBSD shell
core can compile for both ArmOS ARM32 and AArch64 with a small compatibility
layer. The parser and evaluator are not the main risk. The main risks are
runtime contracts around `execve`, job control, terminal ownership, and the
optional `libedit` dependency.

The recommended delivery sequence is:

1. fix the ArmOS `execve` argument contract;
2. import and build a non-interactive-capable shell without `libedit`;
3. run the upstream non-interactive test corpus;
4. complete `WCONTINUED` and validate interactive job control;
5. port `libedit` and enable line editing, history, and completion;
6. promote the executable to `/bin/sh` only after the acceptance gates pass.

Expected effort for one developer already familiar with ArmOS:

| Milestone | Estimated effort |
| --- | ---: |
| Cross-build and link a minimal shell | 2-4 days |
| Fix `execve` and obtain a useful non-interactive shell | 1-2 weeks total |
| Robust interactive job control | 3-7 additional days |
| Port and integrate `libedit` | 1-3 additional weeks |
| Broad upstream test conformance and `/bin/sh` promotion | 3-6 weeks total |

## Scope

### Goals

- Build the FreeBSD shell for ARM32 and AArch64.
- Provide a dependable `/bin/sh` for BSD makefiles, package build scripts, and
  ordinary interactive use.
- Preserve the imported source as closely as practical.
- Keep ArmOS compatibility code separate from upstream code.
- Reuse existing ArmOS BSD compatibility implementations where possible.
- Adapt the upstream tests so they can run without requiring the complete ATF
  framework.
- Keep `mash` available as `/sbin/mash` and as the boot recovery shell.

### Non-goals for the first milestone

- Full FreeBSD login and account policy.
- FreeBSD-specific verified execution through `O_VERIFY`.
- Immediate replacement of every `mash` invocation.
- `libedit`, command history, completion, `bind`, and `fc` in the first
  executable.
- Perfect compatibility with FreeBSD-specific tests before POSIX shell
  behavior is stable.

## Upstream Baseline

The audit used the official FreeBSD source tree at:

```text
repository: https://github.com/freebsd/freebsd-src.git
commit:     1932bd20ed53f2e695a576cffd183937ed25de3f
source:     bin/sh
```

The imported source must be pinned to an exact commit. A future port directory
must include an `UPSTREAM.md` file containing the repository URL, commit,
import date, imported paths, local patches, and update procedure.

Required upstream paths are:

```text
bin/sh/
bin/kill/kill.c
bin/test/test.c
usr.bin/printf/printf.c
```

The full interactive version additionally needs:

```text
contrib/libedit/
lib/libedit/
```

The audited shell and included builtin sources contain approximately 19,374
lines of C and headers. The FreeBSD `libedit` source adds approximately 22,415
lines. The shell is predominantly covered by the BSD 3-Clause license, which
is compatible with the ArmOS Apache-2.0 project. Imported copyright notices
must remain intact.

## Source and Build Anatomy

The shell core is composed of ordinary C sources plus generated files. The
main source groups are:

```text
alias.c       cd.c          error.c       eval.c
exec.c        expand.c      input.c       jobs.c
mail.c        main.c        memalloc.c    miscbltin.c
mystring.c    options.c     output.c      parser.c
redir.c       show.c        trap.c        var.c
arith_yacc.c  arith_yylex.c
```

The shell also compiles builtin implementations from other FreeBSD source
directories:

```text
echo.c        kill.c        printf.c      test.c
```

Generated target sources and headers are:

```text
builtins.c    builtins.h
nodes.c       nodes.h
syntax.c      syntax.h
token.h
```

Generation uses two host C programs and two shell scripts:

| Output | Host generator |
| --- | --- |
| `nodes.c`, `nodes.h` | `mknodes` |
| `syntax.c`, `syntax.h` | `mksyntax` |
| `builtins.c`, `builtins.h` | `mkbuiltins` |
| `token.h` | `mktokens` |

`mknodes` and `mksyntax` must be compiled for and executed on the development
host. `mkbuiltins` and `mktokens` run with the host shell and standard host
text tools. Generated files are then compiled with the ArmOS cross-compiler.
They must be kept under the target-local build directory and not written into
the vendored source tree.

## Audit Results

### Compilation probe

The upstream generated sources were produced and the shell core was compiled
against the ArmOS headers and newlib runtime. `histedit.c` was excluded and
temporary declarations were used to expose the remaining portability gaps.

| Target | Result | Combined relocatable object |
| --- | --- | ---: |
| ARM32 | Core compiled | 97,760 bytes |
| AArch64 | Core compiled | 109,885 bytes |

These are relocatable-object measurements, not final executable sizes. A
static shell without `libedit` is expected to be roughly 150-250 KiB. The
measurement proves that the compiler and the shell's parser architecture are
not blockers; it does not prove runtime correctness.

### ArmOS facilities already available

ArmOS already provides most of the process and terminal substrate required by
the shell:

- `fork`, copy-on-write address spaces, `execve`, `waitpid`, and `wait4`;
- process groups and sessions through `getpgrp`, `setpgid`, `getsid`, and
  `setsid`;
- `sigaction`, `sigprocmask`, `sigsuspend`, signal delivery, stop, and
  continue;
- pipes, file-descriptor duplication, close-on-exec, and redirections;
- `tcgetpgrp`, `tcsetpgrp`, `/dev/tty`, PTYs, and window-size ioctls;
- `TIOCSCTTY`, `TIOCGWINSZ`, `TIOCSWINSZ`, and `SIGWINCH`;
- `poll`, `ppoll`, and `select`;
- resource limits and `getrusage`;
- password database lookups through `getpwnam` and `getpwuid`;
- multibyte and wide-character support;
- `funopen`, `nl_langinfo`, `strlcpy`, `sig2str`, and `str2sig` in the runtime
  or existing compatibility code.

The current `systest` program already exercises stop/continue, process groups,
PTYs, terminal foreground groups, and window-size behavior. These tests should
be retained and extended rather than replaced by shell-only tests.

## Mandatory Platform Work

### P0: make the `execve` contract truthful and usable

The current implementation has two independent limits that are too small for
a system shell:

- `count_exec_vector()` scans at most 32 entries, allowing no more than 31
  useful entries in each of `argv` and `envp`;
- `setup_user_stack()` stores all argument strings, environment strings,
  vectors, and `argc` in a single 4 KiB page.

At the same time, `sysconf(_SC_ARG_MAX)` reports 65,536 bytes. This is an ABI
contract violation and will break glob expansion, compiler commands, `xargs`,
configure-style scripts, and generated make commands.

Relevant files:

- [`kernel/syscalls/syscalls.c`](../kernel/syscalls/syscalls.c)
- [`kernel/process/exec_stack.c`](../kernel/process/exec_stack.c)
- [`kernel/syscalls/process_syscalls.c`](../kernel/syscalls/process_syscalls.c)

The required fix is:

1. define one shared byte budget for argument and environment data;
2. count pointers and strings with overflow-safe arithmetic;
3. map enough initial stack pages to hold the complete payload;
4. preserve the architecture-specific stack alignment;
5. return `E2BIG` when the documented byte budget is exceeded;
6. make `_SC_ARG_MAX` report the implemented limit;
7. test ARM32 and AArch64 pointer widths separately.

Minimum regression tests must cover:

- more than 31 arguments;
- more than 31 environment entries;
- total data above 4 KiB;
- a payload immediately below `ARG_MAX`;
- a payload immediately above `ARG_MAX` returning `E2BIG`;
- a large glob expanded by the shell.

This work is required before the new shell can be considered useful as
`/bin/sh`.

### P1: add `WCONTINUED`

FreeBSD `jobs.c` waits with `WUNTRACED | WCONTINUED` and distinguishes a
continued process with `WIFCONTINUED(status)`. ArmOS currently accepts only
`WNOHANG | WUNTRACED`; other option bits are rejected with `EINVAL`.

The first bring-up build may compile `WCONTINUED` out, but this is only a
temporary compatibility mode. Correct job-control behavior requires:

1. userland definitions for `WCONTINUED` and `WIFCONTINUED`;
2. a per-child continued event that is observable by the parent;
3. one-shot reporting semantics comparable to stopped-state reporting;
4. support in `waitpid` and `wait4`;
5. tests ensuring that stop, continue, and exit events are not lost or
   reported twice.

Relevant files include:

- [`kernel/syscalls/syscalls.c`](../kernel/syscalls/syscalls.c)
- [`kernel/process/signal.c`](../kernel/process/signal.c)
- [`include/kernel/task.h`](../include/kernel/task.h)
- [`newlib-port/syscalls.c`](../newlib-port/syscalls.c)
- [`userland/programs/systest/systest.c`](../userland/programs/systest/systest.c)

### P1: establish a deterministic login-session contract

The current init path opens a terminal, duplicates it onto standard input,
output, and error, and executes `mash`. It does not explicitly establish a new
session, acquire the controlling terminal, or set the initial foreground
process group.

FreeBSD `sh` performs stricter job-control initialization than `mash`. Before
promoting it to a login shell, ArmOS must validate or explicitly establish:

```c
setsid();
ioctl(tty_fd, TIOCSCTTY, 0);
setpgid(0, 0);
tcsetpgrp(tty_fd, getpgrp());
```

The exact ordering must follow ArmOS session rules and must be tested for both
the serial console and the graphical console. The implementation belongs in
the login/session launcher, not as an ArmOS-specific policy hidden inside the
shell.

Relevant file:

- [`userland/system/init/main.c`](../userland/system/init/main.c)

## Compatibility Layer

The port should provide one forced-inclusion portability header plus focused C
implementations. Avoid adding FreeBSD compatibility names to global ArmOS
headers unless they are generally useful outside this port.

### Required mappings

| FreeBSD interface | ArmOS implementation strategy | Notes |
| --- | --- | --- |
| `_setjmp`, `_longjmp` | patch the shell macros to use `setjmp`, `longjmp` | Never wrap `setjmp` in an ordinary function; the saved frame would be invalid after the wrapper returns. |
| `vfork` | map to `fork` or patch the call site | ArmOS copy-on-write makes `fork` acceptable for the first port. |
| `wait3` | wrapper around `wait4(-1, ...)` | Keep `struct rusage` behavior consistent with the current `wait4`. |
| `tcsetsid` | compatibility function using `TIOCSCTTY` | Validate session-leader and ownership errors. |
| `eaccess` | compatibility implementation | A simple `access` mapping is acceptable only if effective-ID semantics are equivalent; otherwise implement them explicitly. |
| `d_namlen` | use `strlen(dp->d_name)` | Do not alias it to `d_reclen`; that was suitable only for a compile probe. |
| `CLOCK_UPTIME` | map to `CLOCK_MONOTONIC` | Suitable for shell timeout accounting. |
| `sys_signame`, `sys_nsig` | use a local signal-name table or `sig2str`/`str2sig` | Prefer one common ArmOS signal-name implementation. |
| `setmode`, `getmode` | reuse existing BSD port compatibility code | Implementations already exist in the BSD tool ports. |
| `O_VERIFY` | define as zero or remove from the open flags | Verified execution is outside the first port's scope. |
| `getlogin` | derive from `getpwuid(geteuid())` | Fall back to `LOGNAME` only with a defined policy. |
| `err.h` helpers | reuse the BSD userland compatibility implementation | Do not duplicate `err`, `errx`, `warn`, and `warnx` per port. |
| `strchrnul` declaration | add a port-local declaration or general newlib header fix | The runtime symbol already exists. |
| `_PATH_TTY` | `/dev/tty` | Port-local `paths.h` additions are sufficient. |
| `_PATH_CONSOLE` | `/dev/tty0` | Keep the graphical console policy outside the shell. |
| `_PATH_DEFPATH` | `/sbin:/bin:/usr/sbin:/usr/bin` | Must match the staged ArmOS filesystem. |
| `WCOREDUMP` | define according to ArmOS wait status | A constant false result is acceptable until core dumps exist. |

Other small FreeBSD compiler and header macros needed by the source include
`__dead2`, `__unused`, `__nonstring`, `__printflike`, `__DECONST`, `ALIGN`, and
`MAXLOGNAME`. They should live in the port compatibility header.

### Compatibility ownership

Use this rule when deciding where to place a fix:

- a generally useful POSIX function or missing newlib declaration belongs in
  the ArmOS libc/newlib layer;
- a FreeBSD extension used by several BSD ports belongs in a shared BSD
  compatibility library;
- a shell-specific policy or FreeBSD-only constant belongs in the shell port;
- a kernel semantic gap such as `WCONTINUED` must not be hidden by a libc shim.

## `libedit` Strategy

The FreeBSD makefile links the shell against `libedit`. This dependency should
not be allowed to block the first useful `/bin/sh` milestone.

### Stage A: `sh-minimal`

The initial port should build without `histedit.c` and without `libedit`.
Provide an explicit `ARMOS_WITH_LIBEDIT=0` build path and a small upstream
patch or stub module that keeps the non-interactive shell and basic terminal
input operational.

This stage intentionally omits:

- Emacs/vi line-editing modes;
- persistent history;
- completion;
- `bind`;
- the full `fc` behavior tied to history.

The shell must still correctly support scripts, `-c`, pipelines, redirections,
command substitution, variables, traps, and basic interactive line input.

### Stage B: full interactive shell

Port `libedit` separately after the shell core and job control are stable.
ArmOS already has many prerequisites, including ncurses/terminfo, termios,
wide characters, regex, signals, password lookup, and window-size handling.

Expected remaining work includes:

- `vis.h`, `vis`, `strvis`, and `unvis` compatibility;
- `FIONREAD` validation;
- generated `libedit` headers;
- disabling or implementing `issetugid`-dependent policy;
- static linking against the existing ncurses/terminfo build;
- PTY tests for editing, resize, history, and UTF-8 input.

`libedit` should be delivered as a reusable library rather than embedded into
the shell port.

## Proposed Repository Layout

The port should follow the conventions in [`BSD_USERLAND.md`](BSD_USERLAND.md):

```text
userland/opt/freebsd-sh/
  UPSTREAM.md
  LICENSES/
  src/
    bin/sh/
    bin/kill/kill.c
    bin/test/test.c
    usr.bin/printf/printf.c
  compat/
    armos_sh_compat.h
    armos_sh_compat.c
    err.c
    err.h
    paths.h
    histedit_stub.c
  patches/
    README.md
    0001-arm-os-optional-libedit.patch
  tests/
    run-freebsd-sh-tests.sh

tools/build_freebsd_sh.sh
```

Generated files should be placed below:

```text
build/<arch>/<platform>/userland/freebsd-sh/generated/
```

Target objects and the final executable should remain in the corresponding
target-local build tree. No generated target file should be shared between
ARM32 and AArch64.

## Build Integration

### Build modes

The recommended explicit build switch is:

```sh
BUILD_FREEBSD_SH=1 ./build.sh
```

It may be enabled by `BUILD_BSD=1` after the initial port is stable, but an
independent switch is valuable during bring-up because the shell has kernel
and runtime prerequisites beyond those of small BSD tools.

Recommended port-specific settings:

```text
ARMOS_WITH_LIBEDIT=0   # initial default
ARMOS_WITH_LIBEDIT=1   # later full interactive build
```

### Build sequence

`tools/build_freebsd_sh.sh` should:

1. resolve the target through the standard ArmOS configuration helpers;
2. verify the pinned source manifest;
3. build `mknodes` and `mksyntax` with the host compiler;
4. run all four host generators into the target-local generated directory;
5. compile the FreeBSD and compatibility sources with the target compiler;
6. link statically against newlib and the ArmOS runtime;
7. optionally add `libedit`, ncurses, and terminfo;
8. stage the result and its default profile files;
9. record a bundle fingerprint and output manifest using the existing cached
   bundle conventions.

Build dependencies for the minimal shell are:

```text
newlib runtime
ArmOS syscall glue
host C compiler
host sh, awk, sed, and mktemp
target compiler and linker
```

The full shell adds:

```text
libedit
ncurses/terminfo
wide-character runtime support
```

### Runtime staging and recovery

The implemented bundle installs:

```text
/opt/freebsd-sh/bin/sh
/bin/freebsd-sh -> ../opt/freebsd-sh/bin/sh
/bin/sh -> ../opt/freebsd-sh/bin/sh
```

The boot and recovery shell remains independent:

```text
/sbin/mash
```

Init continues to launch `/sbin/mash` on the recovery console. Graphical
terminals receive `SHELL=/bin/sh` and `ENV=/home/user/.shrc`, so normal user
sessions use FreeBSD `sh` without weakening the fallback path.

Shell startup files are installed as:

```text
/etc/profile
/home/user/.profile
/home/user/.shrc
/root/.profile
/root/.shrc
```

Login shells read `/etc/profile` and `$HOME/.profile`. Interactive non-login
shells read the file named by `ENV`; the user and root profiles therefore set
`ENV=$HOME/.shrc`. The per-user files establish `PATH`, UTF-8 locale, prompt,
libedit Emacs mode, and history location. This policy remains in userland and
does not add shell-specific behavior to the common kernel.

## Test Strategy

### Upstream corpus

The audited FreeBSD tree contains 486 executable `.0` shell test scripts and
573 total files under `bin/sh/tests`.

FreeBSD normally wraps these tests in ATF. ArmOS does not need to port all of
ATF initially. A small runner can:

1. discover each `.0` test;
2. derive its expected exit status from the test metadata/name convention;
3. execute it with `/bin/freebsd-sh`;
4. capture stdout and stderr separately;
5. compare optional expected-output files;
6. report pass, fail, skip, timeout, and crash;
7. preserve failing artifacts in a target-local results directory.

Run non-interactive categories first:

```text
parser
parameters
expansion
builtins
execution
errors
set-e
invocation
```

FreeBSD-specific expectations must be classified separately from genuine
shell bugs or ArmOS kernel/libc failures.

### Required ArmOS smoke tests

The following must work before considering the shell useful:

```sh
/bin/freebsd-sh -c 'echo hello'
/bin/freebsd-sh -c 'v=ArmOS; test "$v" = ArmOS'
/bin/freebsd-sh -c 'printf "a\nb\n" | sed -n 2p'
/bin/freebsd-sh -c 'false || echo recovered'
/bin/freebsd-sh -c 'x=$(printf result); test "$x" = result'
/bin/freebsd-sh -c 'for x in 1 2 3; do echo "$x"; done'
```

Add filesystem tests for:

- input, output, append, and descriptor redirections;
- here-documents larger than the pipe buffer;
- glob expansion over more than 31 directory entries;
- scripts executed by pathname and through `PATH`;
- `ENOEXEC` fallback for executable text files;
- descriptors numbered 10 or above;
- close-on-exec behavior;
- signal traps and interrupted waits.

### Interactive PTY tests

Interactive tests must use a PTY so terminal process-group behavior is real.
At minimum, validate:

- `Ctrl-C` terminates the foreground command but not the shell;
- `Ctrl-Z` stops the foreground job;
- `jobs` reports the stopped job;
- `bg` continues it in the background;
- `fg` returns it to the foreground;
- background terminal reads receive `SIGTTIN`;
- terminal ownership returns to the shell after job exit;
- pipelines form one job/process group;
- `SIGCHLD` traps and explicit `wait` do not lose status;
- `SIGWINCH` and terminal resizing do not corrupt input;
- EOF exits a non-login interactive shell cleanly;
- serial and graphical consoles behave consistently.

When `libedit` is enabled, add:

- left/right editing and deletion;
- history navigation;
- UTF-8 insertion and deletion;
- resize while editing;
- persistent history load/save;
- completion without buffer corruption.

### Architecture matrix

Every release candidate must cover:

| Architecture | QEMU non-interactive | QEMU PTY/job control | Hardware smoke |
| --- | --- | --- | --- |
| ARM32 | required | required | Raspberry Pi 2 |
| AArch64 | required | required | Raspberry Pi 3 |

Kernel fixes for stack layout, wait status, and signals must never be validated
on only one pointer width.

## Acceptance Gates

### Gate 1: cross-built executable

- ARM32 and AArch64 builds complete without temporary probe definitions.
- No unresolved shell compatibility symbol remains.
- Generated sources are reproducible and target-local.
- Licensing and upstream metadata are present.

### Gate 2: useful script shell

- The `execve` limit is fixed and `_SC_ARG_MAX` is truthful.
- Core smoke tests pass.
- Large argv, environment, and glob tests pass.
- The majority of applicable non-interactive FreeBSD tests pass.
- Failures are classified and tracked rather than silently skipped.

### Gate 3: interactive job control

- `WCONTINUED` is implemented or an explicitly documented temporary mode is
  still active.
- PTY stop/continue/foreground tests pass repeatedly under SMP load.
- Both ArmOS consoles start the shell with correct session and foreground
  process-group state.
- No shell or child remains unkillable after interrupted pipelines.

### Gate 4: `/bin/sh` promotion

- At least 95% of applicable non-interactive upstream tests pass.
- All ArmOS mandatory smoke and PTY tests pass.
- ARM32 and AArch64 images boot with `mash` still available for recovery.
- `bmake` and the BSD userland smoke tests run under the new shell.
- Native TinyCC build workflows run without shell-specific workarounds.
- Repeated graphical and serial login/logout cycles are stable.

### Gate 5: full interactive feature set

- `libedit` is a separately tested library.
- History, editing, completion, UTF-8, and resize tests pass.
- Enabling `libedit` does not regress the non-interactive test corpus.

## Risks and Mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| `execve` payload remains one page | Common scripts fail unpredictably | Treat the multipage argument stack as a prerequisite, not a shell workaround. |
| Incomplete continued-status reporting | `jobs`, `bg`, and `fg` become stale | Implement `WCONTINUED` in the kernel and add one-shot status tests. |
| Incorrect initial terminal session | Shell stops itself or cannot foreground jobs | Establish and test the contract in init/login code. |
| Hidden `_setjmp` workaround | Non-local error handling corrupts control flow | Patch macros to standard `setjmp`/`longjmp`; never use function wrappers. |
| Treating `d_reclen` as name length | Glob expansion reads invalid lengths | Use `strlen(d_name)`. |
| Importing `libedit` too early | Debugging surface doubles | Deliver and validate `sh-minimal` first. |
| Divergent ARM32/AArch64 generated files | Non-reproducible or ABI-mixed objects | Keep all generated and object files target-local. |
| Replacing `mash` too early | Loss of recovery console | Stage as `/bin/freebsd-sh` and keep `/sbin/mash`. |
| Large upstream drift | Port patches become difficult to rebase | Pin the commit, minimize patches, and record every local delta. |

## Maintenance and Upstream Updates

An upstream update must follow this process:

1. read FreeBSD `bin/sh` and `libedit` changes since the pinned commit;
2. update the source manifest and license inventory;
3. regenerate all derived files from a clean directory;
4. rebuild ARM32 and AArch64;
5. run the complete applicable test corpus;
6. review every ArmOS patch for continued necessity;
7. record the new commit and test results in `UPSTREAM.md`.

Compatibility improvements that are useful to other BSD ports should be
moved into a shared library rather than copied during each update.

## Recommended Work Breakdown

### Phase 0: kernel and ABI preparation

- implement multipage `execve` argument/environment setup;
- align `_SC_ARG_MAX` with the implementation;
- add large-argument tests;
- decide and document the initial-session contract;
- add `WCONTINUED` or explicitly approve its temporary omission.

### Phase 1: source import and generators

- vendor the pinned FreeBSD paths;
- add license and upstream metadata;
- implement host generators in `tools/build_freebsd_sh.sh`;
- produce clean ARM32 and AArch64 objects.

### Phase 2: minimal link and runtime

- implement the compatibility table;
- build without `libedit`;
- stage `/bin/freebsd-sh`;
- run core smoke tests and fix runtime failures.

### Phase 3: upstream non-interactive tests

- import the FreeBSD test files;
- implement the lightweight ArmOS runner;
- classify failures;
- reach the non-interactive promotion threshold.

### Phase 4: job control

- complete continued-status reporting;
- harden session and terminal initialization;
- run PTY tests under ARM32, AArch64, and SMP load.

### Phase 5: `libedit`

- port the reusable library;
- integrate ncurses/terminfo and wide-character support;
- enable history, editing, and completion;
- run the interactive test matrix.

### Phase 6: system integration

- make `/bin/sh` point to FreeBSD `sh`;
- migrate BSD tools and build scripts from `/sbin/mash` where appropriate;
- retain an explicit rescue path to `/sbin/mash`;
- update userland and self-hosting documentation.

## Final Recommendation

Proceed with the port. The compilation audit indicates low source-portability
risk, and ArmOS already provides most of the required Unix process and TTY
foundation. The port should not begin by importing `libedit`, and it should not
hide the current `execve` and `WCONTINUED` limitations behind shell-local
workarounds.

The first meaningful objective is therefore not merely “the shell links.” It
is: an ARM32 and AArch64 `/bin/freebsd-sh` that runs real build scripts with a
truthful `execve` contract, while `mash` remains available as the recovery
shell.

## Implementation status (2026-08-02)

The initial port and the full interactive build are now implemented on the
`codex/freebsd-sh-port` branch:

- FreeBSD `sh` is pinned to commit
  `1932bd20ed53f2e695a576cffd183937ed25de3f` and staged as
  `/opt/freebsd-sh/bin/sh`, with `/bin/freebsd-sh` and `/bin/sh` symlinked to
  that target;
- the separately built FreeBSD `libedit` port supplies history, Emacs/Vi line
  editing and completion, backed by the target ncurses bundle;
- generated shell sources and every target object remain under
  `build/<arch>/<platform>/bundles`, so ARM32 and AArch64 cannot share build
  products;
- ArmOS now enforces a common 64 KiB `ARG_MAX` over argument strings,
  environment strings and their pointer vectors, with a multipage initial
  user stack and its existing guard page;
- `waitpid()` implements one-shot `WCONTINUED` reporting and the libc status
  translation preserves the canonical continued status;
- newlib enables C99 integer formats and `long long` formatted I/O, which are
  required by the unmodified shell arithmetic and job-number paths;
- the common POSIX layer now provides `execlp()` to both the normal newlib
  runtime and the TinyCC runtime;
- the common VFS and syscall layers provide the shell-facing contracts for
  named FIFOs, event-driven pipe polling, `SIGPIPE`, physical path resolution,
  shared path limits, realtime clock interpolation, and regular-file
  validation during `execve`;
- the userland POSIX surface now includes `getconf`, `id`, `mkfifo`, `mktemp`
  and `tr`, plus expanded `date`, `env`, `grep`, `ps`, `rm`, and `wc`
  behavior needed by real shell scripts;
- the ArmOS smoke script crosses the former argument-count and one-page stack
  limits, validates pathname expansion through real `cp`, `mv` and `rm`
  operations, and `systest` checks payloads exactly at and immediately above
  `ARG_MAX`.

Private non-graphical QEMU validation has passed on ARM32 and AArch64 for the
shell smoke suite, 8 KiB arguments, 48 arguments, 48 exported environment
entries, command substitution, pipelines, redirections, traps, arithmetic,
libedit history, `Ctrl-Z`, `bg`, `jobs`, and continued-status consumption.
The latest AArch64 run of the imported non-interactive corpus reports 518
tests: 511 pass and 7 fail, with no silent skips. The remaining failures are
classified as follows:

- `command7.0` and `type2.0` depend on the FreeBSD dynamic-loader fixture
  `/libexec/ld-elf.so.1`, which is not part of the static ArmOS runtime;
- `fc4.0` requires a general-purpose `script` PTY utility not yet ported;
- `locale1.0` and `pathname6.0` require locale message catalogs and collation;
- `read11.0` and `read12.0` expose the remaining POSIX lifetime gap when an
  open named FIFO is unlinked. Namespace removal must succeed while the live
  pipe object remains valid until its last descriptor closes.

These results are a compatibility baseline, not a claim of complete POSIX or
FreeBSD conformance. The FIFO lifetime issue is kernel/VFS work shared by all
architectures; FreeBSD loader and locale fixtures must not be emulated by
shell-local patches.

`mash` remains the boot and recovery shell at `/sbin/mash`; init deliberately
continues to start it so a broken interactive FreeBSD shell cannot remove the
recovery console. `/bin/sh` now names FreeBSD `sh` for scripts and child-shell
execution. Pathname wildcards such as `cp 001*.c ./dev` are expanded by this
shell before `cp` is invoked; the file utilities intentionally do not contain a
second, conflicting glob implementation.

Graphical Foot sessions and the recovery-console environment now export
Mash exports `ENV=/home/user/.shrc` from `.mashrc`, so entering an interactive `/bin/sh` loads the same
configuration on either display path. The user and root startup files provide
deterministic environment, history, libedit editing configuration, and prompts
of the form `user@sh$>` or `root@sh#>`. Recovery mash follows the same identity
policy with `user@mash$>` and `root@mash#>`. UART and boot recovery deliberately
continue to execute `/sbin/mash`.

FreeBSD `sh` also exposes `source` as a compatibility alias for its standard
`.` builtin. Both execute in the current shell, so sourced variables and
functions remain visible. The standalone `/bin/clear` command gives `sh` and
future shells the same terminal-clearing capability that mash already provides
as a builtin.

Foot advertises `TERM=armos`, matching the conservative terminfo fallback
compiled into the target ncurses/libedit libraries. Startup files also
normalize the former `TERM=foot` value for compatibility with older staged
Foot binaries. This avoids silently entering libedit's dumb-terminal mode
without claiming terminal capabilities the ArmOS console does not implement.

### Build and target isolation

`BUILD_LIBEDIT=yes` and `BUILD_FREEBSD_SH=yes` are enabled in every tracked
ARM32 and AArch64 platform profile. Host generators, generated parser files,
target objects, libraries, and staged bundles are isolated below
`build/<arch>/<platform>/bundles`. Normal disk images omit the large upstream
test corpus; private test builds opt in with
`FREEBSD_SH_INSTALL_UPSTREAM_TESTS=1`.
