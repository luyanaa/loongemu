#ifndef LATX_QEMU_CPU_FEATURES_H
#define LATX_QEMU_CPU_FEATURES_H

int latx_cpu_has_lsx(void);
int latx_cpu_has_lasx(void);
/* CPUCFG.2[22] = LBT (binary translation): gr2scr/jiscr0/armmtflag. */
int latx_cpu_has_lbt(void);
void latx_cpu_set_vector_features(int lsx, int lasx);
/* Test/emulator hook: force the LBT verdict (-1 = probe the host). */
void latx_cpu_set_lbt_feature(int lbt);
/* 3 = x86-64-v3, 2 = x86-64-v2, 0 = host cannot run x86-64 at all. */
int latx_cpu_x86_level(void);
/* Called when the host cannot honour the advertised ABI; does not return. */
void latx_platform_refuse(const char *why);

#endif
