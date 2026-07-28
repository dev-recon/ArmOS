# HarfBuzz for ArmOS

ArmOS cross-compiles the upstream `harfbuzz.cc` amalgamation into a static
library. The target interface remains entirely C and is available to both
cross-compiled programs and TCC.

The bundle intentionally excludes GLib, ICU, Graphite, platform backends,
tools and subsetting. Exceptions, RTTI, thread-safe C++ statics and the C++
runtime are also disabled. FreeType and pthread integration remain enabled.
