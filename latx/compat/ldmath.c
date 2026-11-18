/*
 * Extended-precision transcendentals for the x87 helpers.
 *
 * The portable build has no 80/128-bit libm yet, so these wrappers evaluate in
 * double precision.  That is a documented accuracy limit for x87 fsin/fcos/
 * fptan on inputs that need the extended mantissa; replacing them with the
 * musl ld128 implementations is tracked as follow-up work.
 */
#include "math.h"

long double sinl(long double x)  { return (long double)sin((double)x); }
long double cosl(long double x)  { return (long double)cos((double)x); }
long double tanl(long double x)  { return (long double)(sin((double)x) / cos((double)x)); }
long double atanl(long double x) { return (long double)atan((double)x); }
