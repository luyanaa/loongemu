/* Accuracy parity check: the in-tree musl math replacements must agree with the
 * host libm within a few ulp.  Numerical differences here mean the bare-metal
 * x87 transcendental helpers would diverge, so this is the gate for the
 * libm replacement. */
#include <math.h>
#include <stdio.h>

double latx_sin(double);
double latx_cos(double);
void   latx_sincos(double, double *, double *);
double latx_atan(double);
double latx_atan2(double, double);
double latx_pow(double, double);
double latx_log2(double);
double latx_logb(double);
double latx_log1p(double);
double latx_fabs(double);
double latx_floor(double);

static int failures;
static int checks;

static void close_enough(double got, double want, double rel, const char *what, double x)
{
    double scale = fabs(want) > 1.0 ? fabs(want) : 1.0;
    double err = fabs(got - want) / scale;

    checks++;
    if (got == want || (isnan(got) && isnan(want))) {
        return;
    }
    if (!(err <= rel)) {
        failures++;
        printf("FAIL %s(x=%.17g): got %.17g want %.17g relerr %.3e\n",
               what, x, got, want, err);
    }
}

int main(void)
{
    static const double xs[] = {
        0.0, 1e-9, 0.25, 1.0, 3.141592653589793, 1.5707963267948966,
        2.0, 10.0, -1.0, -3.5, 1e6, 1e-300, 123.456,
    };
    unsigned i;

    for (i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        double x = xs[i];
        close_enough(latx_sin(x), sin(x), 4e-16, "sin", x);
        close_enough(latx_cos(x), cos(x), 4e-16, "cos", x);
        close_enough(latx_atan(x), atan(x), 4e-16, "atan", x);
        close_enough(latx_fabs(x), fabs(x), 0.0, "fabs", x);
        close_enough(latx_floor(x), floor(x), 0.0, "floor", x);
        {
            double s1, c1, s2, c2;
            latx_sincos(x, &s1, &c1);
            s2 = sin(x);
            c2 = cos(x);
            close_enough(s1, s2, 4e-16, "sincos.sin", x);
            close_enough(c1, c2, 4e-16, "sincos.cos", x);
        }
        if (x > 0.0) {
            close_enough(latx_log2(x), log2(x), 4e-16, "log2", x);
            close_enough(latx_logb(x), logb(x), 0.0, "logb", x);
            close_enough(latx_log1p(x - 1.0), log1p(x - 1.0), 4e-16, "log1p", x - 1.0);
            close_enough(latx_pow(x, 2.5), pow(x, 2.5), 8e-16, "pow", x);
        }
        if (x != 0.0) {
            close_enough(latx_atan2(x, 3.0), atan2(x, 3.0), 4e-16, "atan2", x);
            close_enough(latx_atan2(-x, x), atan2(-x, x), 4e-16, "atan2b", x);
        }
    }
    close_enough(latx_pow(2.0, 0.5), pow(2.0, 0.5), 8e-16, "pow", 2.0);
    close_enough(latx_pow(0.5, -3.0), pow(0.5, -3.0), 8e-16, "powneg", 0.5);

    printf("libm parity: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
