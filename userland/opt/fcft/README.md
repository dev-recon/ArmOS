# fcft for ArmOS

ArmOS cross-builds the official, unmodified fcft 2.5.1 sources. This is the
last fcft 2.x release accepted by Foot 1.9.2.

The ArmOS-facing fcft library remains C-only. HarfBuzz itself is cross-built
from its C++ amalgamation without exceptions, RTTI or a C++ runtime, then
consumed exclusively through its C API. Font discovery, FreeType glyph
rasterization, caching, kerning, Unicode precomposition and HarfBuzz shaping
are therefore enabled without adding C++ to the ArmOS toolchain.

The library and headers are installed below `/opt/fcft`; public headers are
also copied to `/opt/tcc/include/fcft` for native TinyCC builds.
`fcft-test` loads the configured monospace font and rasterizes Latin and
accented glyphs on the target.
