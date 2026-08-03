# teapot-demo

`teapot-demo` is the ArmOS CPU-rendering and ArmUI demonstration. It keeps the
built-in Utah teapot and can also browse Wavefront OBJ models installed in
`/home/user/mesh` through a reusable **Fichier > Ouvrir...** dialog. The dialog
starts in `~/mesh`, navigates directories, filters `.obj` files and keeps the
current scene intact until a new file has been accepted and parsed.

The OBJ path is deliberately split into reusable layers:

- `libarmmesh` validates and normalizes the model, then emits triangles through
  a callback without depending on ArmUI, Nuklear, Wayland or a renderer;
- `teapot-demo` converts those triangles to its five educational rendering
  pipelines: mesh, hidden mesh, flat, Gouraud and Phong;
- ArmUI owns the reusable file dialog; the application only receives an
  accepted path and reports loading errors.

Loading is transactional: a model is fully parsed into a temporary mesh before
it replaces the current one. A malformed or oversized file therefore leaves
the displayed model intact.

## Supported OBJ subset

- positions (`v`), normals (`vn`) and polygonal faces (`f`);
- positive and relative negative indices;
- `v`, `v/vt`, `v//vn` and `v/vt/vn` face elements;
- fan triangulation of polygons;
- generated flat normals when a face has no complete normal set.

Texture coordinates and materials are accepted but ignored. Limits are
explicit: 65,536 positions, normals and output triangles, 64 vertices per face,
and 1,023 bytes per input line.

With `BUILD_NUKLEAR=1`, the build installs the basic models plus original,
denser torus, torus-knot, Cornell-style box, UV cube and smooth-sphere scenes.
Their checked-in OBJ files are generated deterministically by
`tools/generate_demo_meshes.c`; generation is not part of the normal build and
therefore adds no host dependency. Suzanne, Stanford Bunny, Stanford Dragon
and Sponza are documented as external reference scenes because their licensing,
attribution or size is not compatible with silently bundling them in the
default Apache-2.0 tree and disk image.
