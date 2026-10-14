#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Values this large are already integers (and wouldn't fit an int64). */
#define EXACT_LIMIT 4503599627370496.0   /* 2^52 */

double floor(double x) {
    if (x >= EXACT_LIMIT || x <= -EXACT_LIMIT || x != x) {
        return x;
    }
    double t = (double)(int64_t)x;   /* truncates toward zero */
    return t > x ? t - 1 : t;
}

double ceil(double x) {
    if (x >= EXACT_LIMIT || x <= -EXACT_LIMIT || x != x) {
        return x;
    }
    double t = (double)(int64_t)x;
    return t < x ? t + 1 : t;
}

float floorf(float x) {
    return (float)floor(x);
}

float ceilf(float x) {
    return (float)ceil(x);
}

double round(double x) {
    return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5);
}

float roundf(float x) {
    return (float)round(x);
}

/* sin on [-pi/4, pi/4] by its Taylor series, and the rest by symmetry. */
static double sin_core(double x) {
    double x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 * (1 - x2 / 110)))));
}

static double cos_core(double x) {
    double x2 = x * x;
    return 1 - x2 / 2 * (1 - x2 / 12 * (1 - x2 / 30 * (1 - x2 / 56 * (1 - x2 / 90))));
}

double sin(double x) {
    /* Reduce to x = k * pi/2 + r, |r| <= pi/4. */
    double k = round(x / (M_PI / 2));
    double r = x - k * (M_PI / 2);
    switch ((int64_t)k & 3) {
    case 0:  return sin_core(r);
    case 1:  return cos_core(r);
    case 2:  return -sin_core(r);
    default: return -cos_core(r);
    }
}

double cos(double x) {
    return sin(x + M_PI / 2);
}

/*
 * exp/log/pow/ldexp on the x87 unit, which has the building blocks:
 * fyl2x (y * log2 x), f2xm1 (2^x - 1 for |x| <= 1) and fscale (* 2^n).
 * Its 64-bit mantissa leaves plenty of precision for a double result.
 */

static long double x87_log2(long double x) {
    long double r;
    __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x));
    return r;
}

/* 2^x: split into integer and fraction, 2^fraction by f2xm1, then scale. */
static long double x87_exp2(long double x) {
    long double n = __builtin_floorl(x + 0.5L), f = x - n, r;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1;
    __asm__("fscale" : "=t"(r) : "0"(r), "u"(n));
    return r;
}

double log(double x) {
    if (x < 0 || x != x) {
        return __builtin_nan("");
    }
    if (x == 0) {
        return -__builtin_inf();
    }
    return (double)(x87_log2(x) * 0.693147180559945309417232121458176568L);
}

double log2(double x) {
    if (x < 0 || x != x) {
        return __builtin_nan("");
    }
    return x == 0 ? -__builtin_inf() : (double)x87_log2(x);
}

double log10(double x) {
    return log(x) / 2.302585092994045684017991454684364208;
}

double exp(double x) {
    if (x != x) {
        return x;
    }
    if (x > 709.8) {
        return __builtin_inf();
    }
    if (x < -745.2) {
        return 0;
    }
    return (double)x87_exp2(x * 1.442695040888963407359924681001892137L);
}

double pow(double x, double y) {
    if (y == 0) {
        return 1;
    }
    if (x == 0) {
        return y > 0 ? 0 : __builtin_inf();
    }
    if (x != x || y != y) {
        return __builtin_nan("");
    }
    bool negative = false;
    if (x < 0) {
        /* Only whole powers of negative numbers are real. */
        if (y != floor(y)) {
            return __builtin_nan("");
        }
        negative = (int64_t)fmod(y, 2.0) != 0;
        x = -x;
    }
    long double e = (long double)y * x87_log2(x);
    if (e > 16383) {
        return negative ? -__builtin_inf() : __builtin_inf();
    }
    if (e < -16383) {
        return 0;
    }
    double r = (double)x87_exp2(e);
    return negative ? -r : r;
}

double ldexp(double x, int n) {
    long double r, s = n;
    __asm__("fscale" : "=t"(r) : "0"((long double)x), "u"(s));
    return (double)r;
}

double fmod(double x, double y) {
    long double r;
    __asm__("1: fprem; fnstsw %%ax; testw $0x400, %%ax; jnz 1b" : "=t"(r) : "0"((long double)x), "u"((long double)y) : "ax");
    return (double)r;
}

float expf(float x) { return (float)exp(x); }
float logf(float x) { return (float)log(x); }
float powf(float x, float y) { return (float)pow(x, y); }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float ldexpf(float x, int n) { return (float)ldexp(x, n); }
