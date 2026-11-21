arm_ss.add(files(
  'lata.c',
  'mem.c',
))
arm_ss.add(when: 'CONFIG_LATA', if_true: files('pvir.c'))
arm_ss.add(when: 'CONFIG_LATA', if_true: files('../../../latx/runtime/cpu_features.c'))
subdir('ir1')
subdir('ir2')
subdir('optimization')
subdir('translator')
