# Expat for ArmOS

ArmOS cross-builds the official, unmodified Expat sources as the XML parser
used by Fontconfig. The static library and public headers are installed below
`/opt/expat`.

The target configuration enables namespaces, DTDs and general entities.
Until ArmOS exposes a cryptographic entropy API, Expat's internal hash salt
uses its portable fallback; only trusted system font configuration is parsed.
`expat-test` parses a representative Fontconfig document and validates
element, attribute and character callbacks.

Public headers are exported to `/opt/expat/include` and TinyCC's include tree;
third-party files remain outside `/usr/include`.
