# tllist for ArmOS

ArmOS packages the unmodified official tllist 1.1.0 header for Foot. The build
script downloads the upstream release archive and verifies its pinned SHA-512
digest.

The target-local bundle is stored below:

```text
build/<arch>/<platform>/bundles/tllist/
```

It installs the header and pkg-config metadata below `/opt/tllist`, the
upstream MIT license, and the ArmOS-native `/usr/bin/tllist-test`.
