/* mathtest: floating-point math (needs the FPU/SSE support). */

#include <stdio.h>

static double square_root(double x) {   /* Newton's method */
    double g = x > 1 ? x / 2 : 1;
    for (int i = 0; i < 30; i++) {
        g = (g + x / g) / 2;
    }
    return g;
}

static double pi_nilakantha(int terms) {
    double pi = 3, sign = 1;
    for (int i = 0; i < terms; i++) {
        double n = 2.0 * (i + 1);
        pi += sign * 4 / (n * (n + 1) * (n + 2));
        sign = -sign;
    }
    return pi;
}

static double e_series(void) {
    double e = 1, term = 1;
    for (int n = 1; n < 20; n++) {
        term /= n;
        e += term;
    }
    return e;
}

static double sine(double x) {   /* Taylor series */
    double term = x, sum = x;
    for (int n = 1; n < 15; n++) {
        term *= -x * x / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

int main(void) {
    double pi = pi_nilakantha(100000);
    printf("sqrt(2)      = %.12f\n", square_root(2));
    printf("pi           = %.12f  (Nilakantha series, 100000 terms)\n", pi);
    printf("e            = %.12f\n", e_series());
    printf("sin(pi/6)    = %.12f\n", sine(pi / 6));
    printf("1/3          = %f\n", 1.0 / 3);
    printf("-2.5 * 4.25  = %.4f\n", -2.5 * 4.25);
    printf("|%10.3f|%-10.3f|%010.3f|\n", 3.14159, 2.71828, -1.5);
    return 0;
}
