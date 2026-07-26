# xv Bring-Up Notes

The goal is to make `xv` build and run on ArmOS without turning the work into a
full Xorg port by default.

ArmOS 0.7.2 builds the framebuffer dependency set and `fbview` for the
selected ARM32 or ARM64 userland. This does not change the xv redistribution
boundary below and does not place xv sources in the worktree.

## Source And Redistribution Boundary

`xv` is an external test target for ArmOS graphics bring-up, not vendored
ArmOS source.

- Do not commit `xv` source files, source-derived compatibility headers, or
  patches to this repository.
- Use a local source checkout, such as `../xv-3.10a`, only as an input for
  local experiments.
- A locally built ArmOS `xv` binary may be staged into a generated disk image
  for testing, but it must remain a local/generated artifact unless
  redistribution rights are clarified separately.
- ArmOS documentation may explain that `xv` was used as a compatibility target
  and that users can download/build `xv` in their own environment.

## Strategy

1. Expose a minimal framebuffer ABI first.
   - `/dev/fb0` is a character device.
   - `ARMOS_FBIOGET_INFO` reports width, height, pitch, bpp, size, and format.
   - The first supported pixel format is ARGB8888.
2. Keep every step testable from the shell.
   - `fbtest` opens `/dev/fb0`, paints a deterministic pattern, reads back the
     first pixel, and prints `FBTEST_OK`.
3. Port image/display dependencies only after the raw display path is stable.
   - First dependency: `zlib` as `/opt/zlib/lib/libz.a`.
   - Second dependency: `libjpeg` as `/opt/libjpeg/lib/libjpeg.a`.
   - Third dependency: `libpng` as `/opt/libpng/lib/libpng.a`.
   - Fourth dependency: `libtiff` as `/opt/libtiff/lib/libtiff.a`.
   - Next step: start an `xv` build probe against those installed libraries.
   - Avoid committing to a full Xorg server until `xv` has a proven build path.

## Current Smoke Test

```sh
fbtest
```

Expected output:

```text
fbtest: /dev/fb0 1024x768 pitch=4096 bpp=32 size=3145728 stat_size=3145728
FBTEST_OK
```

## zlib Smoke Test

The whole current dependency set can be built and staged with:

```sh
BUILD_XV_DEPS=1 ./build.sh
```

This enables `zlib`, `libjpeg`, `libpng`, and `libtiff`, while skipping the
native TCC bundle unless `BUILD_TCC=1` is passed explicitly.  It also builds
`fbview`, a small ArmOS-owned framebuffer image viewer used to validate the
decode-to-display path without depending on xv redistribution rights.

Build and stage only zlib with:

```sh
BUILD_ZLIB=1 BUILD_TCC=0 ./build.sh
```

Then run:

```sh
ztest
```

Expected output ends with:

```text
ZTEST_OK
```

## libjpeg Smoke Test

Build and stage libjpeg with:

```sh
BUILD_LIBJPEG=1 BUILD_TCC=0 ./build.sh
```

Then run:

```sh
jpgtest
```

Expected output ends with:

```text
JPGTEST_OK
```

## libpng Smoke Test

Build and stage libpng with:

```sh
BUILD_LIBPNG=1 BUILD_TCC=0 ./build.sh
```

Then run:

```sh
pngtest
```

Expected output ends with:

```text
PNGTEST_OK
```

## libtiff Smoke Test

Build and stage libtiff with:

```sh
BUILD_LIBTIFF=1 BUILD_TCC=0 ./build.sh
```

Then run:

```sh
tifftest
```

Expected output ends with:

```text
TIFFTEST_OK
```

The current `tifftest` covers uncompressed grayscale scanlines.  The port builds
Deflate/ZIP support through zlib, but compressed TIFF runtime coverage should be
added separately before treating that path as proven.

The ArmOS libtiff configuration derives `size_t` from the active compiler and
uses `ptrdiff_t` for `tmsize_t`.  Do not replace these with fixed 32-bit values:
the same source configuration is used for ARM32 and ARM64 builds.  The bundled
`/home/user/images/test-pattern-320x240.tiff` is intentionally a minimal,
little-endian, uncompressed RGB file without EXIF or ICC metadata.

## fbview Smoke Test

Build and stage `fbview` plus its image dependencies with:

```sh
BUILD_FBVIEW=1 BUILD_TCC=0 ./build.sh
```

Then run:

```sh
fbview image.jpg
echo status=$?
```

Expected output includes:

```text
FBVIEW_OK
status=0
```

`fbview` is ArmOS-owned test code.  It supports JPEG, PNG, and TIFF input,
scales large images down to fit `/dev/fb0`, and paints ARGB8888 pixels through
the public framebuffer ABI.

The generated user filesystem includes public-domain samples for the three
supported formats under `/home/user/images`:

```sh
fbview /home/user/images/test-pattern.png
fbview /home/user/images/jpeg-landscape.jpg
fbview /home/user/images/test-pattern-320x240.tiff
```

The directory's `README.txt` records the source page and license of each
downloaded image. The TIFF is a local 320x240 derivative of the PNG test
pattern and is convenient for the Raspberry Pi 3 ILI9341 landscape mode.

## External xv Loader Probe

A local-only probe built outside the repository from `../xv-3.10a` has been
used to validate the original xv BMP, JPEG, and TIFF loader paths against the
ArmOS userland libraries. The probe binary may be copied into
`build/<arch>/<platform>/userfs/usr/bin` for a generated-disk smoke test, but
the probe sources, xv sources, source patches, and source-derived compatibility
headers must stay outside this repository.

Runtime test:

```sh
xvprobe
echo status=$?
```

Expected output includes:

```text
XVBMP_OK type=PIC24 size=2x2
XVJPEG_OK type=PIC24 size=2x2
XVTIFF_OK type=PIC8 size=2x2
XVPROBE_OK
status=0
```

Current local findings:

- The framebuffer path and dependency chain are strong enough to run the xv
  BMP/JPEG/TIFF loader code in ArmOS.
- Any local xv compatibility shim must let `jpeglib.h` define `boolean`,
  `TRUE`, and `FALSE` before defining xv-style boolean macros; otherwise the
  application and libjpeg disagree on `struct jpeg_compress_struct` and
  `struct jpeg_decompress_struct` sizes.
- The xv TIFF loader needs local scratch adaptation for modern libtiff symbol
  names before it can be used with libtiff 4.x.  Keep that adaptation outside
  the repo until redistribution rights are settled.
