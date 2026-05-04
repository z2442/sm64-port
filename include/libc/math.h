#ifndef MATH_H
#define MATH_H

#define M_PI 3.14159265358979323846

float sinf(float);
double sin(double);
float cosf(float);
double cos(double);

float sqrtf(float);

static inline float rsqrtf(float x) {
#ifdef TARGET_PSP
    float out;
    __asm__ volatile (
        "mtv    %1, S000\n"
        "vrsq.s S000, S000\n"
        "mfv    %0, S000\n"
        : "=r"(out)
        : "r"(x)
    );
    return out;
#else
    return 1.0f / sqrtf(x);
#endif
}

#endif
