# Get Started - Userland Dev ArmOS

This guide is for contributors who want to write ArmOS userland programs,
commands, shell features, scripts, tests, and newlib-facing code.

ArmOS userland now uses newlib as the supported C library. The older home-grown
libc and old programs are archived under `userland/legacy/` for reference only.
New work should use the newlib path.

ARM32 and ARM64 install the same directory tree and command set. Executables
are rebuilt for the selected ABI; source-level behavior and the common syscall
contracts must remain identical. Select a reproducible target with
`armos.conf` or one of the profiles in `configs/` before rebuilding userland.

Read this first:

- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/GET_STARTED_KERNEL_DEV.md` if your userland change needs a syscall
- `ROADMAP.md`

## Runtime Layout

The installed user filesystem is organized as:

```text
/bin                 core Unix-like commands
/sbin                system programs
/usr/bin             ArmOS-specific user programs and tests
/opt/<program>/bin   imported external tools
/home/user           default user home
/root                root home
/tmp                 temporary files
/etc                 configuration files
/dev                 device nodes
/proc                procfs
/mnt                 optional FAT32 mount
```

Current examples:

- `/sbin/init`
- `/sbin/mash`
- `/sbin/shutdown`
- `/bin/ls`
- `/bin/cat`
- `/bin/grep`
- `/bin/sed`
- `/usr/bin/systest`
- `/usr/bin/ttytest`
- `/usr/bin/kload`
- `/opt/kilo/bin/kilo`

The default interactive shell is `mash`.

## Source Layout

Newlib port:

```text
newlib-port/
  arm64/crt0_newlib.S
  arm64/syscall_raw.S
  crt0_newlib.S
  syscall_raw.S
  syscalls.c
```

Common userland headers:

```text
userland/include/
  arm_os_abi.h
  dirent.h
  termios.h
  sys/ioctl.h
```

Core commands:

```text
userland/coreutils/src/*.c
```

System programs:

```text
userland/system/init/
userland/system/mash/
userland/system/tools/
```

ArmOS test/demo programs:

```text
userland/programs/<name>/*.c
```

Imported external tools:

```text
userland/opt/<name>/src/*.c
```

## Build Model

The top-level userland build is:

```sh
make -C userland
make -C userland install
```

The full project build is normally:

```sh
./run.sh
```

The userland Makefile discovers many programs with wildcards:

- `userland/coreutils/src/foo.c` builds as
  `build/<arch>/<platform>/userland/out/bin/foo` and
  installs as `/bin/foo`;
- `userland/system/tools/foo.c` builds as
  `build/<arch>/<platform>/userland/out/sbin/foo` and
  installs as `/sbin/foo`;
- `userland/programs/foo/*.c` builds as
  `build/<arch>/<platform>/userland/out/usr/bin/foo` and
  installs as `/usr/bin/foo`;
- `userland/opt/foo/src/*.c` builds as
  `build/<arch>/<platform>/userland/out/opt/foo/bin/foo` and installs as
  `/opt/foo/bin/foo`.

There is no `nl-` transition prefix anymore. Newlib is the official ArmOS libc,
so build artifact names match the installed program names.

## Newlib Port

Newlib expects low-level OS hooks such as:

- `_read`
- `_write`
- `_open`
- `_close`
- `_lseek`
- `_sbrk`
- `_fstat`
- `_isatty`
- `_getpid`
- `_kill`
- `_exit`

ArmOS implements these in:

```text
newlib-port/syscalls.c
```

Raw ARM syscall wrappers live in:

```text
newlib-port/syscall_raw.S
```

The raw wrapper convention is:

```asm
RAW_SYSCALL sys_read, 3
```

The raw wrapper returns the kernel value unchanged. `newlib-port/syscalls.c`
then converts negative kernel errors into `errno`:

```c
static int ret_errno(long ret)
{
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}
```

User programs should normally call POSIX/newlib APIs, not raw syscalls.

Good:

```c
fd = open("/tmp/file.txt", O_CREAT | O_WRONLY, 0644);
write(fd, "hello\n", 6);
close(fd);
```

Avoid in normal programs:

```c
sys_open(...);
sys_write(...);
```

Raw syscalls are for libc glue and focused ABI tests.

## Adding A Core Command

Use this for standard Unix-style commands: `head`, `tail`, `grep`, `sed`,
`readlink`, `which`, `mkdir`, `cp`, `mv`, etc.

1. Add:

```text
userland/coreutils/src/name.c
```

2. Implement a normal `main`:

```c
#include <stdio.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}
```

3. Build:

```sh
make -C userland name
```

4. Install through the normal full flow:

```sh
make -C userland install
```

The command installs as:

```text
/bin/name
```

No Makefile edit is usually needed because `coreutils/src/*.c` is discovered by
wildcard.

## Adding A System Tool

Use this for privileged/system-oriented tools:

- `mount`
- `umount`
- `shutdown`
- `fsck-lite`
- `mount-fat32`

Add:

```text
userland/system/tools/name.c
```

It installs as:

```text
/sbin/name
```

System tools may require root behavior or privileged syscalls. Keep permission
rules explicit and test as both normal user and root where relevant.

## Adding A Program Or Test

Use this for ArmOS-specific tests, demos, and stress programs.

Add:

```text
userland/programs/name/main.c
```

or multiple `.c` files under:

```text
userland/programs/name/
```

It installs as:

```text
/usr/bin/name
```

Examples:

- `systest`
- `ttytest`
- `memstress`
- `kload`
- `screen_demo`

## Adding An Imported External Tool

Use `/opt` for imported programs that may eventually carry their own source
tree, build scripts, or patches.

Add:

```text
userland/opt/name/src/*.c
```

It installs as:

```text
/opt/name/bin/name
```

Example:

```text
/opt/kilo/bin/kilo
```

This keeps imported tools separate from native ArmOS core utilities.

Larger imported tools may need a dedicated build script instead of the generic
`userland/opt/<name>/src/*.c` rule. The BSD bundle follows this model:

```text
tools/build_bsdm4.sh
userland/opt/bsdm4/src/
userland/opt/bsdm4/compat/
```

When a port has its own build script, add it to the relevant top-level build
flag in `build.sh` and `run.sh`, then exclude it from the generic external
tool wildcard in `userland/Makefile`. See `docs/BSD_USERLAND.md`.

## Native Programming Inside ArmOS

ArmOS installs a source snapshot into the root filesystem:

```text
/usr/src/armos/userland
```

This lets end users inspect and modify small userland programs from inside
ArmOS itself. The intended in-system workflow is:

```sh
cd /tmp
cp /usr/src/armos/userland/coreutils/src/ls.c ls.c
kilo ls.c
tcc ls.c -o ls-test
./ls-test /proc
```

For contributors, the normal development and stabilization workflow still
happens on the host with the cross toolchain. Native TinyCC is for small
programs and compatibility experiments inside ArmOS, not for replacing the
release build path.

## When A Userland Change Needs A Kernel Change

Start in userland. If a normal POSIX/newlib API already exists, use it.

Only add kernel ABI when there is no existing syscall or procfs/device path.

Prefer this order:

1. existing POSIX/newlib function;
2. existing `/proc` file;
3. existing device node such as `/dev/tty` or `/dev/null`;
4. existing ArmOS ABI in `arm_os_abi.h`;
5. new syscall as last resort.

If a new syscall is required, follow `docs/GET_STARTED_KERNEL_DEV.md`.

## Adding Newlib Glue For A Kernel Syscall

If the kernel syscall exists but newlib cannot call it yet:

1. Add raw wrapper:

```text
newlib-port/syscall_raw.S
```

```asm
RAW_SYSCALL sys_example, 195
```

2. Add extern and wrapper:

```text
newlib-port/syscalls.c
```

```c
extern long sys_example(int arg);

int example(int arg)
{
    return ret_errno(sys_example(arg));
}
```

3. Add or update public user header if needed:

```text
userland/include/
```

4. Add `systest` coverage.

## Shell And Script Compatibility

`mash` supports interactive use, external commands, scripts, PATH lookup,
redirections, pipes, background jobs, and basic job control.

Useful script features to preserve:

- `$VAR`
- `$?`
- `set`
- `export`
- `unset`
- `env`
- globs such as `*.c`, `/bin/*`, `./*.txt`
- redirections: `>`, `>>`, `<`
- pipes
- background jobs with `&`
- command lookup through `PATH`

When changing shell behavior, test:

```sh
echo $HOME
echo $?
which ls
ls *.txt
echo hello | grep hell
sleep 1 &
jobs
```

Also test failure paths:

```sh
not-a-command
cd /does/not/exist
cat /does/not/exist
```

The shell should print clean user-facing errors, not raw exec noise.

## TTY-Aware Programs

Interactive programs must restore terminal state.

Important APIs:

- `tcgetattr`
- `tcsetattr`
- `tcflush`
- `ioctl(TIOCGWINSZ)`
- `ioctl(TIOCSWINSZ)`
- `read`
- `write`

If a program enters raw mode:

1. save old termios;
2. install cleanup on normal exit;
3. handle signal/interruption if possible;
4. restore terminal before returning to `mash`.

Test with:

```sh
ttytest
kilo /home/user/hello.c
```

Use Ctrl-C, Ctrl-Z, arrows, backspace, and tab completion after returning to the
shell.

## Filesystem Behavior

Primary filesystem:

```text
/      ext2, read/write
```

Compatibility filesystem:

```text
/mnt   FAT32, optional mount
```

Useful commands:

```sh
mount
df
ls -la /
mkdir -p /tmp/a/b/c
echo ok > /tmp/a/b/c/file.txt
cp -r /tmp/a /tmp/b
mv /tmp/b /tmp/c
rm -rf /tmp/a /tmp/c
sync
```

When writing tools that manipulate paths:

- support absolute and relative paths;
- handle `.` and `..`;
- return useful error messages;
- keep recursive operations careful;
- do not assume FAT32 and ext2 have identical behavior.

## Testing Procedure

Minimum manual test after userland changes:

```sh
systest
ttytest
ps
lps
ls -la /
ls -la /proc
```

Command-specific tests:

```sh
command --help
command invalid-arg
command missing-file
```

Pipe/redirection tests:

```sh
echo hello | command
command < /etc/passwd
command /etc/passwd > /tmp/out.txt
cat /tmp/out.txt
```

Background/job-control tests:

```sh
sleep 2 &
jobs
sleep 20
```

Use Ctrl-C for foreground process interruption.

Stress tests:

```sh
systest &; systest &
memstress 8192 30
kload -s 30 -m 256 -c 8 -u 25 -p 4 &
top
```

Interactive tests should often be done manually by the developer. Reserve
automated QEMU driving for crash reproduction, regression checks, and hard
debugging.

## Coding Style

Keep userland C simple:

- C99/GNU99 is fine;
- prefer POSIX APIs where available;
- return `0` on success, non-zero on failure;
- print diagnostics to `stderr`;
- use `perror()` when errno is meaningful;
- avoid large stack allocations;
- check all `malloc`, `open`, `read`, `write`, and `fstat` results;
- keep command behavior close to Unix when practical.

For command errors:

```text
name: clear explanation
```

Example:

```c
fprintf(stderr, "cp: cannot open '%s': %s\n", path, strerror(errno));
```

## Memory Notes

User stacks are intentionally small. Avoid large local arrays.

Prefer:

```c
char *buf = malloc(4096);
```

over:

```c
char buf[65536];
```

Always free long-lived allocations in tests and tools. Newlib may keep heap
pages for reuse, so a process heap not shrinking after free is not necessarily
a leak.

## Adding Test Coverage To `systest`

Add focused tests to:

```text
userland/programs/systest/systest.c
```

Good `systest` tests:

- are deterministic;
- create files under `/tmp/systest-<pid>`;
- clean up after themselves;
- print `[OK]` / `[KO]`;
- avoid depending on wall-clock timing unless testing time;
- work when multiple `systest` instances run in parallel.

Avoid hardcoded shared temp paths. Parallel `systest` runs are an important
stress test.

## Common Userland Failure Patterns

Command works alone but fails in scripts:

- missing exit status handling;
- no support for relative paths;
- no support for stdin/stdout pipeline behavior.

Command works on ext2 but fails on FAT32:

- filesystem feature mismatch;
- rename/copy fallback missing;
- long filename or permission assumption.

Shell prompt weird after program exits:

- raw/canonical terminal state leak;
- foreground process group not restored;
- signal interruption path missed cleanup.

Program crashes only after fork/exec:

- invalid envp/argv handling;
- pointer passed from kernel address space;
- user stack layout issue;
- missing newlib glue conversion.

## Definition Of Done

A userland change is not done until:

- it builds with `make -C userland`;
- it installs into the expected filesystem directory;
- QEMU boots;
- the command runs from `mash`;
- focused manual tests pass;
- `systest` still passes when relevant;
- generated binaries are not committed;
- docs/tests are updated if behavior changed.
