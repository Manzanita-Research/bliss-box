/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* Small DSP pieces shared by the engine: no double, no libm (fm1-base's fastmath.h does the maths). */
#pragma once
#include <stdint.h>
#include "fastmath.h"

#if defined(PLAT_HOST) || defined(BL_OFFLINE)
#define BL_POOL
#else
#define BL_POOL __attribute__((section(".pool")))
#endif

/* the event queue's publish order: one core, the audio in an interrupt, so the compiler's order is enough on the
 * FM-1; a full fence where threads are real (the host simulator, the browser) */
#if defined(PLAT_HOST) || defined(BL_OFFLINE)
#define BL_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#else
#define BL_BARRIER() __asm__ volatile("" ::: "memory")
#endif

/* the block-rate stages stay functions of their own: a call a block costs nothing, and a profile (tools/tprof.py)
 * can tell them apart */
#define BL_NOINLINE __attribute__((noinline))

#define BL_INV_SR (1.0f / 44100.0f)

/* --- random: xorshift32, one per purpose so a sting replays exactly ------------------------------------ */
typedef struct { uint32_t s; } bl_rng_t;
static inline uint32_t rng_u32(bl_rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return r->s = x;
}
static inline void rng_seed(bl_rng_t *r, uint32_t s)
{
    r->s = s * 2654435761u + 0x9E3779B9u;
    if (!r->s)
        r->s = 1;
    rng_u32(r);
    rng_u32(r);
}
static inline float rng_unit(bl_rng_t *r) { return (float)(rng_u32(r) >> 8) * (1.0f / 16777216.0f); }   /* [0,1) */
static inline float rng_bi(bl_rng_t *r) { return rng_unit(r) * 2.0f - 1.0f; }                        /* [-1,1) */
static inline float rng_range(bl_rng_t *r, float a, float b) { return a + (b - a) * rng_unit(r); }
static inline int rng_int(bl_rng_t *r, int n) { return n > 0 ? (int)((rng_u32(r) >> 8) % (uint32_t)n) : 0; }
static inline int rng_chance(bl_rng_t *r, float p) { return rng_unit(r) < p; }
/* a log-normal-ish spread around m: "rubato", never a grid */
static inline float rng_around(bl_rng_t *r, float m, float spread)
{
    return m * fm_exp2f(spread * (rng_unit(r) + rng_unit(r) + rng_unit(r) - 1.5f) * 1.4f);
}

/* --- sine table: 1024 points, linear interpolation (better than -100 dB) -------------------------------- */
#define BL_SIN_N 1024
extern float bl_sin_tab[BL_SIN_N + 1];
void bl_tables_init(void);
static inline float wrap1(float x)
{
    int32_t i = (int32_t)x;
    x -= (float)i;
    return x < 0.0f ? x + 1.0f : x;
}
static inline float sin1(float ph)              /* ph in [0,1) */
{
    float x = ph * (float)BL_SIN_N;
    int32_t i = (int32_t)x;
    float f = x - (float)i;
    const float *t = bl_sin_tab + (i & (BL_SIN_N - 1));
    return t[0] + f * (t[1] - t[0]);
}
static inline float sinw(float ph) { return sin1(wrap1(ph)); }   /* any phase, in turns */
/* Integer phase: a whole turn is 2^32, so a phase wraps by itself as it overflows (no wrap1), and a harmonic is
 * a multiply. uturns: turns (|t| < 1) to that. sinu: the table at an integer phase. */
static inline uint32_t uturns(float t) { return (uint32_t)(int32_t)(t * 2147483648.0f) << 1; }
static inline float sinu(uint32_t ph)
{
    const float *t = bl_sin_tab + (ph >> 22);
    float f = (float)((ph >> 6) & 0xFFFFu) * (1.0f / 65536.0f);
    return t[0] + f * (t[1] - t[0]);
}

/* --- one-pole filters ---------------------------------------------------------------------------------- */
static inline float op_coef(float hz) { return 1.0f - fm_expf(-FM_TWO_PI * hz * BL_INV_SR); }
static inline float tau_coef(float sec) { return sec <= 0.0f ? 1.0f : 1.0f - fm_expf(-1.0f / (sec * 44100.0f)); }
/* the per-sample multiplier that falls 60 dB in t60 seconds */
static inline float t60_mul(float t60) { return fm_exp2f(-9.965784f / (t60 * 44100.0f)); }

/* --- state-variable filter (Zavalishin's TPT form) ---------------------------------------------------- */
typedef struct { float ic1, ic2; } svf_t;
typedef struct { float a1, a2, a3, k; } svf_c;
static inline void svf_set(svf_c *c, float hz, float q)
{
    float g, a1;
    hz = fm_clampf(hz, 10.0f, 19000.0f);
    g = fm_tanf(FM_PI * hz * BL_INV_SR);
    c->k = 1.0f / q;
    a1 = 1.0f / (1.0f + g * (g + c->k));
    c->a1 = a1;
    c->a2 = g * a1;
    c->a3 = g * c->a2;
}
static inline float svf_lp(svf_t *s, const svf_c *c, float x)
{
    float v3 = x - s->ic2, v1 = c->a1 * s->ic1 + c->a2 * v3, v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return v2;
}
static inline float svf_bp(svf_t *s, const svf_c *c, float x)
{
    float v3 = x - s->ic2, v1 = c->a1 * s->ic1 + c->a2 * v3, v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.0f * v1 - s->ic1;
    s->ic2 = 2.0f * v2 - s->ic2;
    return v1;
}

/* --- biquad (RBJ), for the master's EQ ---------------------------------------------------------------- */
typedef struct { float b0, b1, b2, a1, a2; } bq_c;
typedef struct { float z1, z2; } bq_t;
static inline float bq_run(bq_t *s, const bq_c *c, float x)
{
    float y = c->b0 * x + s->z1;
    s->z1 = c->b1 * x - c->a1 * y + s->z2;
    s->z2 = c->b2 * x - c->a2 * y;
    return y;
}
void bq_peak(bq_c *c, float hz, float q, float db);
void bq_lowshelf(bq_c *c, float hz, float db);
void bq_highshelf(bq_c *c, float hz, float db);
void bq_lowpass(bq_c *c, float hz, float q);
void bq_highpass(bq_c *c, float hz, float q);

/* --- soft saturation ----------------------------------------------------------------------------------- */
static inline float soft(float x)              /* a cheap tanh: smooth, 1 at the rails */
{
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

/* --- polyBLEP saw --------------------------------------------------------------------------------------- */
static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}
static inline float saw_blep(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }

/* --- equal-power pan ---------------------------------------------------------------------------------- */
static inline void pan_gains(float p, float *l, float *r)   /* p -1 .. 1 */
{
    float a = (p + 1.0f) * 0.125f;              /* 0 .. 0.25 turns */
    *l = sin1(a + 0.25f);
    *r = sin1(a);
}
