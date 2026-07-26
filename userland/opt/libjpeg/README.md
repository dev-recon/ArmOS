# ArmOS libjpeg Port

This directory contains the Independent JPEG Group source used to build a
static `libjpeg.a` for ArmOS userland.

Build and stage it with:

```sh
./tools/build_libjpeg.sh
rsync -a build/arm32/qemu-virt/bundles/libjpeg/bundle/ \
  build/arm32/qemu-virt/userfs/
```

Or include it in a full image build:

```sh
BUILD_LIBJPEG=1 BUILD_NEWLIB=0 BUILD_TCC=0 ./build.sh
```

The smoke test installed by the bundle is:

```sh
jpgtest
```
