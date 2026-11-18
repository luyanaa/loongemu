#ifndef LATX_BARE_MATH_H
#define LATX_BARE_MATH_H
typedef float float_t;
typedef double double_t;
#define FLT_MIN 1.17549435e-38F
#define FLT_MAX 3.40282347e+38F
#define DBL_MIN 2.2250738585072014e-308
#define DBL_MAX 1.7976931348623157e+308
#define DBL_EPSILON 2.220446049250313e-16
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, \
                                           FP_SUBNORMAL, FP_ZERO, (x))
#define isgreater(x, y)      __builtin_isgreater((x), (y))
#define isgreaterequal(x, y) __builtin_isgreaterequal((x), (y))
#define isless(x, y)         __builtin_isless((x), (y))
#define islessequal(x, y)    __builtin_islessequal((x), (y))
#define islessgreater(x, y)  __builtin_islessgreater((x), (y))
#define isunordered(x, y)    __builtin_isunordered((x), (y))
#define isnan(x)   __builtin_isnan(x)
#define isinf(x)   __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)
double fabs(double x);
float  fabsf(float x);
float  fmaf(float x, float y, float z);
double fma(double x, double y, double z);
double floor(double x);
double ceil(double x);
double sqrt(double x);
float  sqrtf(float x);
double fmin(double a, double b);
double fmax(double a, double b);
double atan(double x);
double atan2(double y, double x);
double cos(double x);
double sin(double x);
double pow(double x, double y);
double log(double x);
double log2(double x);
double logb(double x);
void   sincos(double x, double *s, double *c);
double copysign(double x, double y);
double frexp(double x, int *e);
double ldexp(double x, int e);
double scalbn(double x, int n);
int    ilogb(double x);
#define FP_ILOGB0   (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)
double log1p(double x);
double expm1(double x);
double rint(double x);
#endif

long double sinl(long double x);
long double cosl(long double x);
long double tanl(long double x);
long double atanl(long double x);
