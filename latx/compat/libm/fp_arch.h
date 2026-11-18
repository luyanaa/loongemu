#ifndef LATX_BARE_FP_ARCH_H
#define LATX_BARE_FP_ARCH_H
/* Arch overrides for musl libm internals.  The portable build does not hide
 * symbols and has no weak-alias machinery beyond ELF attributes. */
#ifndef hidden
#define hidden __attribute__((__visibility__("hidden")))
#endif
#ifndef weak_alias
#define weak_alias(old, new) \
    extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#endif
#endif
