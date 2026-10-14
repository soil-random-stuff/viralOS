#pragma once

/* A small math library. sqrt/fabs compile to single SSE instructions. */

#define M_PI 3.14159265358979323846

static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline float sqrtf(float x) { return __builtin_sqrtf(x); }
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline float fminf(float a, float b) { return a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a > b ? a : b; }

double floor(double x);
double ceil(double x);
float floorf(float x);
float ceilf(float x);
double round(double x);
float roundf(float x);
double sin(double x);
double cos(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double ldexp(double x, int n);
double fmod(double x, double y);
float expf(float x);
float logf(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float ldexpf(float x, int n);
