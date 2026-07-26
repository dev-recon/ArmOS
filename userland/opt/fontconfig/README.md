# Fontconfig for ArmOS

ArmOS cross-builds the official, unmodified Fontconfig library over FreeType
and Expat. It installs a small system policy that maps the generic `monospace`
family to the bundled MesloLGS NF terminal font.

Cache generation and host-side command-line tools are excluded from the first
port. The library can scan application fonts directly and is sufficient for
the fcft/Foot font-selection path. `fontconfig-test` loads the ArmOS policy,
registers Meslo and validates generic-family matching on the target.

Public headers are exported to `/opt/fontconfig/include/fontconfig` and
TinyCC's include tree instead of referring to architecture-specific build
paths or placing third-party files below `/usr/include`.
