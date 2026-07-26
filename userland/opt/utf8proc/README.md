# ArmOS utf8proc bundle

This bundle packages the unmodified official utf8proc 2.11.3 source for the
Unicode processing required by Foot and its font stack.

The ArmOS build produces a static `libutf8proc.a`, installs the public header
and `libutf8proc.pc`, and includes `utf8proc-test` for native validation of:

- UTF-8 decoding and invalid-sequence rejection;
- Unicode character widths;
- grapheme cluster boundaries;
- NFC composition.

All objects remain below `build/<arch>/<platform>/bundles/utf8proc`. The
installed bundle is rooted at `/opt/utf8proc` in the target user filesystem.
Its public header is exported to `/opt/utf8proc/include` and
`/opt/tcc/include`, so target programs never depend on a host build path and
third-party files remain outside `/usr/include`.
