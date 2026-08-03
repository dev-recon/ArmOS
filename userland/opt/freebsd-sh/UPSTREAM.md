# FreeBSD sh upstream

- Repository: FreeBSD src
- Commit: `1932bd20ed53f2e695a576cffd183937ed25de3f`
- Imported components: `bin/sh`, `bin/kill/kill.c`, `bin/test/test.c`,
  `usr.bin/printf/printf.c`, `contrib/libedit`, and `lib/libedit`
- License: BSD licenses retained in each source file; aggregate notices are in
  `LICENSES/FREEBSD-COPYRIGHT`.

ArmOS-specific portability code is isolated under `compat/`. Functional
changes to imported source are guarded by `__ARMOS__` and kept minimal.

The default ArmOS bundle links the imported shell against the separately
cross-built `/opt/libedit/lib/libedit.a`.  `FREEBSD_SH_WITH_LIBEDIT=0` remains
available for bring-up and recovery builds.

The upstream test corpus is kept in the source tree but is not installed in a
normal disk image, whose ext2 inode budget is intentionally small.  Private
test bundles enable it with `FREEBSD_SH_INSTALL_UPSTREAM_TESTS=1`.
