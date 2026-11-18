#include <math.h>

/* musl reports an invalid operation through errno/fenv; the bare build has
 * neither, so the NaN result is all that is observable. */
float __math_invalidf(float x)
{
    (void)x;
    return __builtin_nanf("");
}
