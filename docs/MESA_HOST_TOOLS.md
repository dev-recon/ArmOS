# Meson host tools

Mesa 25.3.6 requires a newer Bison than the 2.3 release provided by macOS.
ArmOS therefore does not use arbitrary generators found in the host `PATH`.
Foot also reuses this pinned Meson/Ninja environment to build its native
Wayland protocol scanner.

Run:

```sh
./tools/bootstrap_mesa_host_tools.sh
```

The bootstrap builds the pinned GNU M4 1.4.19 and GNU Bison 3.8.2 releases and
creates an isolated Python environment containing pinned Meson, Ninja, Mako,
MarkupSafe, PyYAML and packaging releases in
`build/host-tools/<host>/install`. Downloaded archives are cached under
`build/downloads/host-tools` and verified with committed SHA-256 values before
extraction. These host programs are never installed in an architecture target
tree or copied into ArmOS userfs.

The bootstrap resolves the base host interpreter before creating the virtual
environment and clears macOS' `__PYVENV_LAUNCHER__` state for every Python
invocation. It is therefore safe to call from an already activated virtual
environment or from a build whose `PATH` contains the previous ArmOS tools.
M4, Bison and the Python environment are validated independently: an
interrupted Python installation does not rebuild the two GNU tools on the next
run.

Mesa and Foot build scripts obtain the isolated prefix with:

```sh
MESA_HOST_PREFIX="$(./tools/bootstrap_mesa_host_tools.sh --print-prefix)"
PATH="$MESA_HOST_PREFIX/bin:$MESA_HOST_PREFIX/python/bin:$PATH"
export PATH
```

Set `ARMOS_FORCE_HOST_TOOLS_REBUILD=1` to rebuild the tools. `WORK_ROOT`,
`DOWNLOAD_DIR` and `PREFIX` may be overridden for isolated build or CI tests.

This bootstrap is also the contract for the future containerized ArmOS
toolchain: the container will provide the cross compilers and these pinned
host generators, while target objects remain separated under
`build/<arch>/<platform>`. Graphical QEMU execution stays a host-side test
step and is not coupled to the build container.

`BUILD_MESA=yes` and `BUILD_FOOT=yes` invoke this bootstrap automatically. A
normal cached build reuses both the host prefix and the target-local bundle; use
`ARMOS_FORCE_HOST_TOOLS_REBUILD=1` only for the host tools and
`ARMOS_FORCE_USERLAND_REBUILD=1` for a complete target bundle rebuild.
