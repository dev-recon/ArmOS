# BSD bmake for ArmOS

This directory vendors the upstream `bmake` sources used as the first BSD
tooling milestone for ArmOS self-hosting.

The current ArmOS build is intentionally conservative:

- statically linked against the ArmOS newlib port;
- installed under `/opt/bmake`;
- exposed as `/usr/bin/bmake` in `userfs` for convenience;
- configured with `/sbin/mash` as the recipe shell;
- built without filemon/meta mode;
- built with POSIX regex support, required by bmake's `:C` variable modifier.

The imported upstream tree under `src/` is kept unchanged. ArmOS-specific
`share/mk` defaults live under `overlays/mk/` and are copied over the upstream
`mk` directory by `tools/build_bmake.sh` when building the bundle.

ArmOS enables `newlib/libc/posix` for the `arm-none-eabi` target so that
`regcomp`, `regexec`, `regerror`, and `regfree` are provided by the canonical
newlib `libc.a`.  `tools/build_bmake.sh` checks for these symbols before
building because bmake will fail very early without them.

Build from the repository root:

```sh
./tools/build_bmake.sh
```

Stage the generated bundle into the target filesystem:

```sh
rsync -a build/arm32/qemu-virt/bundles/bmake/bundle/ \
  build/arm32/qemu-virt/userfs/
cp build/arm32/qemu-virt/bundles/bmake/bundle/opt/bmake/bin/bmake \
  build/arm32/qemu-virt/userfs/usr/bin/bmake
```

`mash -c "command"` is required for bmake recipes and is implemented as a
non-interactive shell path: it does not load `.mashrc`, print the banner, or
start the line editor.
