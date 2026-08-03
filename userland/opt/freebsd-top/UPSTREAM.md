# FreeBSD top upstream

- Repository: `https://github.com/freebsd/freebsd-src`
- Commit: `1932bd20ed53f2e695a576cffd183937ed25de3f`
- Imported component: `usr.bin/top`
- License: upstream notices are retained in every source file; the aggregate
  FreeBSD notice is stored in `LICENSES/FREEBSD-COPYRIGHT`.

The imported sources are kept unchanged under `src/usr.bin/top`. ArmOS
adaptation belongs under `compat/` and must not introduce architecture or
platform-specific code into the common kernel.

The FreeBSD `machine.c` file is retained as the upstream reference but will
not be linked on ArmOS. Its `kvm(3)` and FreeBSD `sysctl(3)` implementation is
replaced by an ArmOS machine backend reading the stable `/proc` ABI.
