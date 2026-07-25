Newlib patches for arm-os
=========================

`tools/build_newlib.sh` extracts the vanilla newlib archive, then applies the
patches listed in `series` from this directory.

The series is intentionally small. The arm-os integration keeps the OS ABI in
tracked repo code:

- `newlib-port/` provides crt0, raw syscall wrappers, errno/status translation,
  signal translation, and newlib syscall hooks.
- `userland/include/` provides the small compatibility headers
  that arm-os exposes but vanilla newlib does not ship for this target.

If we later need to modify upstream newlib sources, add a patch file here and
list it in `series`. A fresh install will then reproduce the exact same sysroot
from the upstream archive.

Current patch:

- `0001-enable-posix-for-arm-none-eabi.patch`: packages the POSIX regex sources
  from `newlib/libc/posix` into `libc.a` for the `arm-none-eabi` target. ArmOS
  needs `regcomp`, `regexec`, `regerror`, and `regfree` because BSD bmake uses
  the `:C` variable modifier from its own `share/mk` files. The patch does not
  enable all POSIX directory routines, which would currently pull unsupported
  `dirent.h` internals into the libc build.
- `0002-expose-armos-pthread-features.patch`: publishes the pthread,
  synchronization, timeout, and robust-mutex declarations implemented by the
  ArmOS userspace runtime. Process-shared synchronization remains advertised
  as unsupported.
