# Shared LoongArch IR2 backend, formerly target/arm/lata/ir2/.
# The QEMU-integrated build pulls these from target/arm/lata/ir2/meson.build;
# the freestanding lata/latr/latm Makefiles compile this directory directly.
arm_ss.add(when: 'CONFIG_LATA', if_true: files(
  'ir2.c',
  'ir2-peephole.c',
  'ir2-assemble.c',
  'la-append.c',
  '../../perf/src/perf.c',
))
