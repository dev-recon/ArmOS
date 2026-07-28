# Pixman for ArmOS

ArmOS builds the unmodified official Pixman 0.46.4 release as a target-local
static userland bundle. The build script downloads the upstream release archive
and verifies its pinned SHA-512 digest before extraction.

The initial port uses Pixman's portable C renderer on both ARM32 and ARM64.
Architecture-specific SIMD can be enabled later after the common renderer and
its correctness tests are stable on every platform.

Build artifacts are isolated below:

```text
build/<arch>/<platform>/bundles/pixman/
```

The bundle installs:

```text
/opt/pixman/include/pixman-1/pixman.h
/opt/pixman/include/pixman-1/pixman-version.h
/opt/pixman/lib/libpixman-1.a
/opt/pixman/lib/pkgconfig/pixman-1.pc
/usr/bin/pixman-test
```

Pixman retains its upstream MIT license, installed as
`/opt/pixman/COPYING`.
