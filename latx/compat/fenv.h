#ifndef LATX_BARE_FENV_H
#define LATX_BARE_FENV_H
/* No host FP environment in the bare build: rounding is handled by the
 * softfloat paths, so save/restore is a no-op. */
#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3
typedef int fenv_t;
static inline int fegetenv(fenv_t *e) { if (e) { *e = 0; } return 0; }
static inline int fesetenv(const fenv_t *e) { (void)e; return 0; }
static inline int fesetround(int m) { (void)m; return 0; }
static inline int fegetround(void) { return FE_TONEAREST; }
#endif
