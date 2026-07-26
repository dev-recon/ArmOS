# TinyCC Bring-up Notes

ArmOS can now build and run a small C program from inside `mash` using a native
TinyCC binary staged under `/opt/tcc`. This document records the exact working
shape so the bring-up does not get lost again.

The native path is validated for both ARM32 and AArch64 userlands. Each TinyCC
build targets the ABI of the running ArmOS image; it is not a multilib compiler
and does not generate the other architecture's executable format.

## Current Status

Validated inside ArmOS:

```sh
which tcc
tcc -c /home/user/tcc-src/hello.c -o /tmp/tcc_hello.o
tcc -o /tmp/tcc_hello /tmp/tcc_hello.o
/tmp/tcc_hello alpha beta
echo $?
```

Expected output:

```text
/usr/bin/tcc
hello from native TinyCC on ArmOS
argc=3
argv[0]=/tmp/tcc_hello
argv[1]=alpha
argv[2]=beta
42
```

The one-shot compile/link path is also validated:

```sh
tcc /home/user/tcc-src/hello.c -o /tmp/tcc_hello2
/tmp/tcc_hello2 one-shot
```

Kilo is also validated as the first non-trivial native TCC program:

```sh
tcc -c /home/user/tcc-src/kilo.c -o /tmp/kilo.o
tcc -o /tmp/kilo-tcc /tmp/kilo.o
/tmp/kilo-tcc /home/user/tcc-src/hello.c
```

The direct one-command path works too:

```sh
tcc /home/user/tcc-src/kilo.c -o /tmp/kilo-direct
```

On AArch64, an additional fork/wait smoke test has been compiled and executed
on Raspberry Pi 3 hardware. This validates native ELF64 output, process startup
and exit-status propagation rather than only successful compilation.

## Important Architecture Decision

ArmOS uses a split compiler policy:

```text
kernel and release builds -> host arm-none-eabi-gcc on macOS/Linux
end-user programs        -> native TinyCC ARM/EABI inside ArmOS, progressively
```

GCC remains the kernel compiler and the contributor/release compiler because
the kernel and stable userland build depend on the strongest available ARMv7-A
code generation, inline assembly handling, linker-script behavior, diagnostics,
and ABI conservatism. TinyCC is intentionally scoped to ArmOS end users first:
fast native compilation, small command-line tools, and short edit-build-run
loops inside the running system.

The intended migration path is:

1. Keep GCC as the official kernel compiler.
2. Keep GCC/newlib as the host fallback for complex userland programs.
3. Make `/usr/bin/tcc` the native ArmOS compiler for user-authored programs.
4. Gradually validate simple coreutils and programs with TCC as compatibility
   tests, without replacing the host cross build as the release path.
5. Use kilo as the first serious interactive userland validation target.

`/usr/bin/tcc` is not the real compiler. It is an ArmOS wrapper around:

```text
/opt/tcc/bin/tcc
```

Keep `/opt/tcc/bin` out of `PATH`. Users should invoke `/usr/bin/tcc`, because
the wrapper injects the ArmOS runtime objects, include paths, linker address,
entry point, newlib archive, libm archive, and libgcc archive.

The real compiler installed at `/opt/tcc/bin/tcc` must be TinyCC's
`arm-eabi-tcc` target, not the generic `arm` target. The generic ARM target can
compile simple objects, but the linker path hits relocation failures such as:

```text
tcc: error: can't relocate value ...,28
```

The working native bundle is produced by `tools/build_tcc_native.sh`, which
builds `arm-eabi-tcc` and copies it as `/opt/tcc/bin/tcc`.

## Source Layout

Relevant files:

```text
userland/opt/tcc/src/              Vendored upstream TinyCC source snapshot
userland/programs/tcc/tcc.c        ArmOS /usr/bin/tcc wrapper
newlib-port/tcc/syscalls_min.c     Minimal syscall glue for TCC-linked binaries
tools/build_tcc_host.sh            Host-side TinyCC ARM/EABI validation helper
tools/test_tcc_armos_hello.sh      Host-side linker/runtime smoke test
tools/build_tcc_native.sh          Native ArmOS TinyCC bundle builder
userfs/home/user/tcc-src/hello.c   In-ArmOS smoke test source
userfs/home/user/tcc-src/kilo.c    In-ArmOS non-trivial native TCC test source
userfs/usr/src/armos/              Userland source snapshot installed in /
```

Generated bundle output:

```text
build/<arch>/<platform>/bundles/tcc-native/bundle/opt/tcc/
```

The target filesystem staging directory is
`build/<arch>/<platform>/userfs/`. Generated binaries from different targets
are never staged together.

## Sources Installed In ArmOS

ArmOS deliberately installs userland sources into the generated root
filesystem:

```text
/usr/src/armos/userland
```

This is part of the native end-user programming story. A user booted into ArmOS
should be able to inspect a command source, edit a small program with `kilo`,
compile it with `/usr/bin/tcc`, and execute it without returning to the host
machine. Contributors still build and validate the operating system from the
host cross toolchain.

The installed snapshot currently contains:

```text
/usr/src/armos/userland/include
/usr/src/armos/userland/coreutils/src
/usr/src/armos/userland/programs
/usr/src/armos/userland/system
```

Example inside `mash`:

```sh
tcc /usr/src/armos/userland/coreutils/src/ls.c -o /tmp/ls-tcc
/tmp/ls-tcc /proc
```

The installed sources are a snapshot, not a substitute for Git history. When a
source file changes in the repository, keep the `/usr/src/armos` copy in
`userfs` synchronized before building a release image.

`tools/build_tcc_native.sh` copies newlib headers first, then overlays
`userland/include`. This order is deliberate: ArmOS owns public ABI headers such
as `termios.h` and `sys/ioctl.h`, and those must override newlib's generic or
incomplete versions for TTY-aware programs like kilo.

## Build Recipe

Prerequisites:

```sh
./tools/build_newlib.sh
./build.sh
```

Build the host-side TinyCC probes:

```sh
./tools/build_tcc_host.sh
./tools/test_tcc_armos_hello.sh
```

Build the native ArmOS bundle:

```sh
./tools/build_tcc_native.sh
```

Stage it into the generated filesystem manually for now:

```sh
rsync -a build/arm32/qemu-virt/bundles/tcc-native/bundle/opt/tcc/ \
  build/arm32/qemu-virt/userfs/opt/tcc/
make -C userland tcc ENABLE_TCC=1 TARGET_ARCH=arm32 TARGET_PLATFORM=qemu-virt
cp build/arm32/qemu-virt/userland/out/usr/bin/tcc \
  build/arm32/qemu-virt/userfs/usr/bin/tcc
```

Rebuild the disk:

```sh
rm -f disk.img ext2.img fat32.img
make disk.img ARCH=arm-none-eabi- CROSS_COMPILE=arm-none-eabi-
```

Boot and test:

```sh
./boot.sh
```

Inside `mash`:

```sh
which tcc
tcc -c /home/user/tcc-src/hello.c -o /tmp/tcc_hello.o
tcc -o /tmp/tcc_hello /tmp/tcc_hello.o
/tmp/tcc_hello alpha beta
echo $?
```

Kilo native compile smoke test:

```sh
tcc -c /home/user/tcc-src/kilo.c -o /tmp/kilo.o
tcc -o /tmp/kilo-tcc /tmp/kilo.o
/tmp/kilo-tcc /home/user/tcc-src/hello.c
```

## Why syscalls_min.c Exists

Normal ArmOS userland links against `newlib-port/syscalls.c`. TinyCC's linker is
stricter around duplicate symbols, especially with high-level POSIX wrappers
that newlib already provides. `newlib-port/tcc/syscalls_min.c` intentionally
keeps only the low-level syscall glue needed by simple TCC-generated programs.

This keeps the native compiler milestone small:

- compile and link simple C programs;
- use newlib for `stdio`, `malloc`, and normal C library code;
- avoid dragging the full stable userland glue into TinyCC's linker path.

Current extra wrappers carried by the TCC profile include `ioctl`,
`tcgetattr`, `tcsetattr`, and `ftruncate`, because kilo needs TTY raw mode and
file truncation when saving.

## libgcc Trap

The host-side probes showed that TinyCC can link a full newlib-backed ArmOS
binary when given the ARM/EABI root `libgcc.a`. The problematic path was the
Thumb multilib selected by the normal GCC userland flags:

```text
thumb/v7-a/nofp/libgcc.a
```

For TinyCC-linked output, use the root ARM/EABI `libgcc.a` exposed by:

```sh
arm-none-eabi-gcc -print-libgcc-file-name
```

The native compiler binary itself is still linked with the regular ArmOS/newlib
toolchain flags; this distinction matters.

## Current Limitations

This is not a full native development environment yet.

Known limitations:

- no automatic integration into the default `build.sh` flow yet;
- generated `/opt/tcc` bundle is staged manually;
- no broad coreutils self-host compile test yet;
- no TCC test-suite pass inside ArmOS yet;
- `tcc -run` is intentionally not a milestone;
- larger programs may expose missing libc/syscall behavior.

## Next Useful Milestones

1. Add an optional `ENABLE_TCC=1` build/install path that stages `/opt/tcc`.
2. Add a `tcctest` userland smoke command that compiles and runs `hello.c`.
3. Compile two or three small ArmOS coreutils sources with native TCC.
4. Expand `syscalls_min.c` only when a real TCC-generated program needs it.
5. Decide whether `/opt/tcc` should become part of release images.
