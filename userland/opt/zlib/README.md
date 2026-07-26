# ArmOS zlib Port

This directory contains the zlib source used to build a static `libz.a` for
ArmOS userland.

Build and stage it with:

```sh
./tools/build_zlib.sh
rsync -a build/arm32/qemu-virt/bundles/zlib/bundle/ \
  build/arm32/qemu-virt/userfs/
```

Or include it in a full image build:

```sh
BUILD_ZLIB=1 BUILD_NEWLIB=0 BUILD_TCC=0 ./build.sh
```

The smoke test installed by the bundle is:

```sh
ztest
```
