# BSD awk for ArmOS

This directory vendors NetBSD's historical `nawk` and builds it as the ArmOS
BSD awk bundle.

The ArmOS integration keeps upstream sources under `src/` and local portability
glue under `compat/`. The build script generates the yacc parser and procedure
table in the selected target hierarchy, then links a static ArmOS binary at:

```sh
build/<arch>/<platform>/bundles/bsdawk/bundle/opt/bsdawk/bin/awk
```

When `BUILD_BSD=1` is used, the top-level build stages the bundle into
`build/<arch>/<platform>/userfs/opt/bsdawk` and creates:

```sh
/bin/awk -> ../opt/bsdawk/bin/awk
```

Build just this bundle with:

```sh
./tools/build_bsdawk.sh
```
