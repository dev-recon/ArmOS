# Nuklear for ArmOS

Nuklear is built as `/opt/nuklear/lib/libnuklear.a`, with its public header
installed as `/opt/nuklear/include/nuklear.h`.

The library contains only the portable immediate-mode UI engine. It does not
depend on Wayland, a framebuffer driver, QEMU or Raspberry Pi hardware.
Applications provide their own platform backend; `armui-demo` is the reference
ArmOS Wayland/SHM backend.
