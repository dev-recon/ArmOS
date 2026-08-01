# Raylib upstream contract

- Project: Raylib
- Version: 6.0
- Release archive: `https://github.com/raysan5/raylib/archive/refs/tags/6.0.tar.gz`
- SHA-256: `2b3ee1e2120c7a0796b33062c7e9a694dd8a8caa56a96319ac8c8ecf54a90d0b`
- License: zlib/libpng

ArmOS does not vendor the upstream source tree. The target-specific build
downloads and verifies the pinned archive, applies the tracked integration
patch, then adds the first-party `rcore_armos.c` backend. Raylib only consumes
the common Wayland, EGL and OpenGL ES 2 ABIs; it contains no QEMU, VirtIO,
VirGL, VC4, V3D or Raspberry Pi code.
