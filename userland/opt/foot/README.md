# Foot on ArmOS

ArmOS cross-builds the unmodified Foot 1.9.2 sources as a static executable.
The build uses the native Wayland client library together with fcft, HarfBuzz,
Fontconfig, FreeType, utf8proc and Pixman.

The initial port deliberately disables input-method support until
`armos-wlcomp` exposes the corresponding protocol. The executable and its
default configuration are installed as:

```text
/usr/bin/foot
/opt/foot/share/foot.ini
```

The compositor launches Foot with this file explicitly. ArmOS uses a
16-pixel Meslo profile, an opaque high-contrast palette and strongly hinted
grayscale rendering so the result remains readable on both progressive and
interlaced displays.

The build is isolated below `build/<arch>/<platform>/bundles/foot`; ARM32 and
ARM64 objects and executables are never shared.
