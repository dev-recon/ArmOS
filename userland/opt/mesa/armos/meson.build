# ArmOS integration for Mesa 25.3.x.
#
# This file is copied into src/gallium/winsys/virgl/armos by the ArmOS Mesa
# bundle builder.  The platform-neutral adapter links against the public
# ArmOS VirGL userspace ABI; the qemu-virt transport remains in the kernel
# platform backend.

armos_virgl_winsys_dir = get_option('armos-virgl-winsys-dir')
if armos_virgl_winsys_dir == ''
  error('-Darmos-virgl-winsys-dir must name the target ArmOS library directory')
endif

dep_armos_virgl_winsys = declare_dependency(
  link_args : ['-larmos-virgl-winsys'],
)

libvirglarmos = static_library(
  'virglarmos',
  'virgl_armos_winsys.c',
  include_directories : [
    inc_include,
    inc_src,
    inc_gallium,
    inc_gallium_aux,
    inc_gallium_drivers,
    inc_virtio,
  ],
  dependencies : [
    dep_armos_virgl_winsys,
    dep_libvirglcommon,
    idep_mesautil,
    idep_nir_headers,
  ],
  gnu_symbol_visibility : 'hidden',
)
