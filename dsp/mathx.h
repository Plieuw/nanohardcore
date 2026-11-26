/* mathx.h - small freestanding float math (no libm), for RV32 ilp32d targets.
 * Accuracy is ~1e-5 relative; good enough for coefficients and envelopes. */
#ifndef MATHX_H
#define MATHX_H
#include <stdint.h>

#define MX_PI 3.14159265358979f

static inline float mx_bits2f(uint32_t u) { union { uint32_t u; float f; } c; c.u = u; return c.f; }
static inline uint32_t mx_f2bits(float f) { union { uint32_t u; float f; } c; c.f = f; return c.u; }

static inline float mx_clamp(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float mx_abs(float x) { return x < 0.f ? -x : x; }
static inline int mx_floor(float x) { int i = (int)x; return (x < (float)i) ? i - 1 : i; }

/* 2^x, |x| < 126 */
static inline float mx_exp2(float x)
{
    x = mx_clamp(x, -126.f, 126.f);
    int i = mx_floor(x);
    float f = x - (float)i;              /* [0,1) */
    /* minimax-ish polynomial for 2^f on [0,1) */
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f
            + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
    return p * mx_bits2f((uint32_t)(i + 127) << 23);
}

/* log2(x), x > 0 */
static inline float mx_log2(float x)
{
    uint32_t u = mx_f2bits(x);
    int e = (int)((u >> 23) & 255) - 127;
    float m = mx_bits2f((u & 0x007fffff) | 0x3f800000); /* [1,2) */
    /* log2(m) via atanh series: s=(m-1)/(m+1) */
    float s = (m - 1.f) / (m + 1.f), s2 = s * s;
    float l = s * (2.0f + s2 * (0.6666667f + s2 * (0.4f + s2 * (0.2857143f + s2 * 0.2222222f))));
    return (float)e + l * 1.44269504f;
}

static inline float mx_exp(float x) { return mx_exp2(x * 1.44269504f); }
static inline float mx_log(float x) { return mx_log2(x) * 0.69314718f; }
static inline float mx_pow(float a, float b) { return mx_exp2(b * mx_log2(a)); }

/* sin for any x (range reduced), ~1e-6 abs */
static inline float mx_sin(float x)
{
    float k = (float)mx_floor(x * (1.f / (2.f * MX_PI)) + 0.5f);
    x -= k * 2.f * MX_PI;                     /* [-pi, pi] */
    if (x > 0.5f * MX_PI) x = MX_PI - x;
    else if (x < -0.5f * MX_PI) x = -MX_PI - x; /* [-pi/2, pi/2] */
    float x2 = x * x;
    return x * (1.f + x2 * (-0.16666667f + x2 * (0.00833333f + x2 * (-0.00019841f + x2 * 2.7526e-6f))));
}
static inline float mx_cos(float x) { return mx_sin(x + 0.5f * MX_PI); }
static inline float mx_tan(float x) { return mx_sin(x) / mx_cos(x); }

/* one-pole coefficient for time constant tau [s] at sample period dt */
static inline float mx_rc_coef(float tau, float dt) { return mx_exp(-dt / tau); }

/* potentiometer laws: A = audio/log taper, B = linear */
static inline float mx_taper_a(float x) { x = mx_clamp(x, 0.f, 1.f); return x * x * (0.25f + 0.75f * x); } /* 15% @ mid */

/* 32-bit LFSR / xorshift noise in [-1,1) */
static inline float mx_noise(uint32_t *s)
{
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x;
    return (float)(int32_t)x * (1.f / 2147483648.f);
}

#endif
