# FreeType for ArmOS

ArmOS cross-builds the official, unmodified FreeType sources as a static
library for the Foot terminal dependency chain. The build is isolated below
`build/<arch>/<platform>/bundles/freetype` and installs to `/opt/freetype`.

The first port disables optional compression, SVG and HarfBuzz integration.
TrueType/OpenType loading, hinting and grayscale glyph rasterization remain
available. The MesloLGS NF font is installed in `/usr/share/fonts/armos` for
the compositor and terminal tests.

`freetype-test` opens that font through the public API and renders Latin and
accented glyphs. This verifies the parser, charmap, scaler and rasterizer on
the target architecture.

The complete public header tree is exported to `/opt/freetype/include` and
`/opt/tcc/include`; `<ft2build.h>` therefore needs no host build path and
third-party files remain outside `/usr/include`.
