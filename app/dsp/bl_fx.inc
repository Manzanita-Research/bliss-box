/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* The space and the tape. Part of bliss.c (one unit).
 *   ensemble chorus (pads, keys) -> tape echo -> hall (FDN) -> TALK -> cassette -> limiter */

/* ------------------------------------------------------------------------------------- ensemble chorus --- */
#define CH_LEN 1024
typedef struct {
    float *bl, *br;
    int32_t w;
    float ph1, ph2, r1, r2, base, d1, d2;
    float m[3];                          /* the taps' swing now */
} chorus_t;
static BL_POOL float ch_mem[2][2][CH_LEN];
static chorus_t CHO[2];

static inline float tapf(const float *b, int32_t w, float d, int32_t mask)
{
    float p = (float)(w + mask + 1) - d, f;
    int32_t i = (int32_t)p;
    f = p - (float)i;
    return b[i & mask] + f * (b[(i + 1) & mask] - b[i & mask]);
}

static void chorus_init(chorus_t *c, float *bl, float *br, float r1_hz, float r2_hz, float base, float d1, float d2)
{
    c->bl = bl;
    c->br = br;
    c->w = 0;
    c->ph1 = 0.0f;
    c->ph2 = 0.37f;
    c->r1 = r1_hz * BL_INV_SR;
    c->r2 = r2_hz * BL_INV_SR;
    c->base = base;
    c->d1 = d1;
    c->d2 = d2;
    c->m[0] = c->m[1] = c->m[2] = 0.0f;
}

/* three taps a third of a cycle apart, the right channel's swing inverted: the string machine's ensemble. The swing
 * is worked out every CH_STEP samples and ramped between (its fastest LFO is 6.6 Hz: nothing lost) */
#define CH_STEP 16
static BL_NOINLINE void chorus_run(chorus_t *c, float *l, float *r, int n, float depth, float mix)
{
    int i0, i, k;
    float dry = 1.0f - 0.45f * mix, wet = 0.42f * mix;
    const float *bl = c->bl, *br = c->br;
    for (i0 = 0; i0 < n; i0 += CH_STEP) {
        int m = n - i0 < CH_STEP ? n - i0 : CH_STEP;
        float d[3], dd[3], inv = 1.0f / (float)m;
        int32_t w = c->w;
        c->ph1 = wrap1(c->ph1 + c->r1 * (float)m);
        c->ph2 = wrap1(c->ph2 + c->r2 * (float)m);
        for (k = 0; k < 3; k++) {
            float o = (float)k * 0.333333f;
            float t = (c->d1 * sin1(wrap1(c->ph1 + o)) + c->d2 * sin1(wrap1(c->ph2 + o))) * depth;
            dd[k] = (t - c->m[k]) * inv;
            d[k] = c->m[k];
            c->m[k] = t;
        }
        for (i = i0; i < i0 + m; i++) {
            float ol = 0.0f, orr = 0.0f;
            c->bl[w] = l[i];
            c->br[w] = r[i];
            for (k = 0; k < 3; k++) {
                d[k] += dd[k];
                ol += tapf(bl, w, c->base + d[k], CH_LEN - 1);
                orr += tapf(br, w, c->base - d[k], CH_LEN - 1);
            }
            l[i] = l[i] * dry + ol * wet;
            r[i] = r[i] * dry + orr * wet;
            w = (w + 1) & (CH_LEN - 1);
        }
        c->w = w;
    }
}

/* ------------------------------------------------------------------------------------------ tape echo --- */
/* one mono loop of tape at half the sample rate (int16), two playback heads (D, 1.5 D) for left and right;
 * the repeats lose top and bottom and saturate a little each time round, like an Echoplex */
#define EC_LEN 32768
#define EC_SCALE 8192.0f
static BL_POOL int16_t ec_buf[EC_LEN];
static struct {
    int32_t w, half;
    float acc, lp_in, fb_lp, fb_hp, wph, time, fbk;
    float y_a;
} EC;

static inline float tap16(float pos)
{
    int32_t i = (int32_t)pos;
    float f = pos - (float)i;
    float a = (float)ec_buf[i & (EC_LEN - 1)], b = (float)ec_buf[(i + 1) & (EC_LEN - 1)];
    return (a + f * (b - a)) * (1.0f / EC_SCALE);
}

static BL_NOINLINE void echo_run(const float *in, float *ol, float *or_, int n, float time_s, float fbk, float tone)
{
    int i;
    float tc = tau_coef(2.5f), lpc = op_coef(2800.0f + 2200.0f * tone), hpc = op_coef(120.0f), inc = op_coef(6000.0f);
    float wr = 0.33f * BL_INV_SR;
    for (i = 0; i < n; i++) {
        float d, now, ya, yb, fb, x;
        EC.time += tc * (time_s - EC.time);          /* time changes glide, with the pitch bend of a real tape */
        EC.wph = wrap1(EC.wph + wr);
        d = (EC.time * 44100.0f + 22.0f * sin1(EC.wph)) * 0.5f;
        now = (float)(EC.w + EC_LEN) - 1.0f + (EC.half ? 0.5f : 0.0f);
        ya = tap16(now - d);
        yb = tap16(now - d * 1.5f);
        fb = ya;
        EC.fb_lp += lpc * (fb - EC.fb_lp);
        EC.fb_hp += hpc * (EC.fb_lp - EC.fb_hp);
        fb = soft((EC.fb_lp - EC.fb_hp) * 1.3f) * 0.77f;
        EC.lp_in += inc * (in[i] - EC.lp_in);
        x = EC.lp_in + fb * fbk;
        EC.acc += x;
        if (EC.half) {
            float s = fm_clampf(EC.acc * 0.5f * EC_SCALE, -32767.0f, 32767.0f);
            ec_buf[EC.w] = (int16_t)s;
            EC.w = (EC.w + 1) & (EC_LEN - 1);
            EC.acc = 0.0f;
        }
        EC.half ^= 1;
        ol[i] = ya;
        or_[i] = yb;
    }
}

/* ------------------------------------------------------------------------------------------------ hall --- */
/* eight delay lines, a Hadamard matrix, damping in the loop, four lines slowly modulated: a long, soft,
 * never metallic tail. Mono in, stereo out. */
#define RV_N 8
#define RV_PAD 16
/* the hall runs at half the rate (22.05 kHz): its tail is damped below 8 kHz anyway, and it costs half */
static const int16_t RV_LEN[RV_N] = {1103, 1187, 1265, 1343, 1417, 1505, 1583, 1671};
#define RV_TOTAL (1103 + 1187 + 1265 + 1343 + 1417 + 1505 + 1583 + 1671 + RV_N * RV_PAD)
static BL_POOL float rv_mem[RV_TOTAL];
#define PD_LEN 4096                      /* the predelay, at half rate: up to 0.18 s */
static BL_POOL int16_t pd_buf[PD_LEN];
static const int16_t AP_LEN[4] = {71, 53, 189, 139};
static float ap_mem[71 + 53 + 189 + 139];
#define RV_MOD 4.5f                      /* the modulated lines' swing, half-rate samples */
static struct {
    float *b[RV_N];
    int32_t size[RV_N], w[RV_N];
    float g[RV_N], lp[RV_N];
    float mph[4], md[4], rt, damp;
    float *ap[4];
    int32_t api[4], pdw;
    float in_hp, in_lp;
    int half;                            /* 1: the first of a pair is in acc */
    float acc, yl, yr, pl, pr;           /* the input's first half; the last two outputs (the upsampling) */
} RV;

static void reverb_layout(void)
{
    int k;
    float *p = rv_mem, *q = ap_mem;
    for (k = 0; k < RV_N; k++) {
        RV.b[k] = p;
        RV.size[k] = RV_LEN[k] + RV_PAD;
        p += RV.size[k];
    }
    for (k = 0; k < 4; k++) {
        RV.ap[k] = q;
        q += AP_LEN[k];
    }
    for (k = 0; k < 4; k++) {
        RV.mph[k] = 0.25f * (float)k;
        RV.md[k] = RV_MOD * sin1(RV.mph[k]);
    }
}

static void reverb_set(float rt60, float damp_hz)
{
    int k;
    if (fm_fabsf(rt60 - RV.rt) < 0.01f && fm_fabsf(damp_hz - RV.damp) < 5.0f)
        return;
    RV.rt = rt60;
    RV.damp = damp_hz;
    for (k = 0; k < RV_N; k++)
        RV.g[k] = fm_exp2f(-9.965784f * (float)RV_LEN[k] / (rt60 * 22050.0f));
}

/* one half-rate step of the hall: x in, the left and right taps out */
static inline void reverb_step(float x, float dc, float lpc, float hpc, int32_t pd, const float *md, float *yl, float *yr)
{
    int k;
    float o[RV_N], a, b, c, d, e, f, g, h;
    pd_buf[RV.pdw] = (int16_t)fm_clampf(x * 8192.0f, -32767.0f, 32767.0f);
    x = (float)pd_buf[(RV.pdw - pd) & (PD_LEN - 1)] * (1.0f / 8192.0f);
    RV.pdw = (RV.pdw + 1) & (PD_LEN - 1);
    RV.in_lp += lpc * (x - RV.in_lp);
    RV.in_hp += hpc * (RV.in_lp - RV.in_hp);
    x = RV.in_lp - RV.in_hp;
    for (k = 0; k < 4; k++) {            /* diffusion */
        float *ab = RV.ap[k];
        int32_t j = RV.api[k];
        float dl = ab[j], y = dl - 0.62f * x;
        ab[j] = x + 0.62f * y;
        RV.api[k] = j + 1 < AP_LEN[k] ? j + 1 : 0;
        x = y;
    }
    for (k = 0; k < RV_N; k++) {         /* the lines: four of them swung a little */
        const float *bk = RV.b[k];
        int32_t sz = RV.size[k], w = RV.w[k];
        float sv;
        if (k < 4) {
            float p = (float)(w + sz) - ((float)RV_LEN[k] + md[k]), fr;
            int32_t j = (int32_t)p, j1;
            fr = p - (float)j;
            if (j >= sz)
                j -= sz;
            j1 = j + 1 < sz ? j + 1 : 0;
            sv = bk[j] + fr * (bk[j1] - bk[j]);
        } else {
            int32_t j = w - RV_LEN[k];
            sv = bk[j < 0 ? j + sz : j];
        }
        RV.lp[k] += dc * (sv - RV.lp[k]);
        o[k] = RV.lp[k] * RV.g[k];
    }
    *yl = o[0] - o[2] + o[4] - o[6];
    *yr = o[1] - o[3] + o[5] - o[7];
    a = o[0] + o[1]; b = o[0] - o[1]; c = o[2] + o[3]; d = o[2] - o[3];      /* Hadamard 8 */
    e = o[4] + o[5]; f = o[4] - o[5]; g = o[6] + o[7]; h = o[6] - o[7];
    o[0] = a + c; o[2] = a - c; o[1] = b + d; o[3] = b - d;
    o[4] = e + g; o[6] = e - g; o[5] = f + h; o[7] = f - h;
    a = o[0] + o[4]; e = o[0] - o[4]; b = o[1] + o[5]; f = o[1] - o[5];
    c = o[2] + o[6]; g = o[2] - o[6]; d = o[3] + o[7]; h = o[3] - o[7];
    o[0] = a; o[1] = b; o[2] = c; o[3] = d; o[4] = e; o[5] = f; o[6] = g; o[7] = h;
    for (k = 0; k < RV_N; k++) {
        int32_t w = RV.w[k];
        RV.b[k][w] = fm_flush(o[k] * 0.353553f + ((k & 1) ? -x : x) * 0.5f);
        RV.w[k] = w + 1 < RV.size[k] ? w + 1 : 0;
    }
}

static BL_NOINLINE void reverb_run(const float *in, float *ol, float *or_, int n, float predelay_s)
{
    int i, k;
    int32_t pd = (int32_t)(predelay_s * 22050.0f);
    /* (the one-poles' coefficients at the half rate: op_coef of twice the frequency) */
    float dc = op_coef(2.0f * RV.damp), hpc = op_coef(300.0f), lpc = op_coef(18000.0f);
    float md[4], dmd[4], steps = (float)(n / 2 + 1);
    static const float MR[4] = {0.31f, 0.43f, 0.57f, 0.71f};
    if (pd < 1)
        pd = 1;
    if (pd > PD_LEN - 1)
        pd = PD_LEN - 1;
    for (k = 0; k < 4; k++) {            /* the lines' swing: a ramp across the block */
        RV.mph[k] = wrap1(RV.mph[k] + MR[k] * (float)n * BL_INV_SR);
        md[k] = RV.md[k];
        RV.md[k] = RV_MOD * sin1(RV.mph[k]);
        dmd[k] = (RV.md[k] - md[k]) / steps;
    }
    for (i = 0; i < n; i++) {
        if (!RV.half) {                  /* the first of a pair: keep it; out, the hall's latest */
            RV.acc = in[i];
            RV.half = 1;
            ol[i] = RV.yl;
            or_[i] = RV.yr;
        } else {                         /* the second: a step of the hall; out, halfway to its new output */
            RV.half = 0;
            for (k = 0; k < 4; k++)
                md[k] += dmd[k];
            RV.pl = RV.yl;
            RV.pr = RV.yr;
            reverb_step(0.5f * (RV.acc + in[i]), dc, lpc, hpc, pd, md, &RV.yl, &RV.yr);
            ol[i] = 0.5f * (RV.pl + RV.yl);
            or_[i] = 0.5f * (RV.pr + RV.yr);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ talk --- */
/* room for a voice: a wide dip where speech lives, a little less low end and level */
static struct { bq_c pk, ls; bq_t s[2][2]; float amt, last; } TK;

static BL_NOINLINE void talk_run(float *l, float *r, int n, float target)
{
    int i;
    float a0 = TK.amt, a1, g0, g1;
    a1 = TK.amt + (target - TK.amt) * 0.05f;                /* about half a second to come in */
    if (fm_fabsf(target - a1) < 0.001f)
        a1 = target;
    TK.amt = a1;
    if (a0 < 0.0005f && a1 < 0.0005f)
        return;
    if (fm_fabsf(a1 - TK.last) > 0.002f || (a1 == target && TK.last != target)) {
        bq_peak(&TK.pk, 2000.0f, 0.8f, -6.0f * a1);
        bq_lowshelf(&TK.ls, 120.0f, -2.0f * a1);
        TK.last = a1;
    }
    g0 = fm_db2lin(-3.0f * a0);
    g1 = fm_db2lin(-3.0f * a1);
    for (i = 0; i < n; i++) {
        float g = g0 + (g1 - g0) * (float)i * (1.0f / (float)BL_BLOCK);
        l[i] = bq_run(&TK.s[0][1], &TK.ls, bq_run(&TK.s[0][0], &TK.pk, l[i])) * g;
        r[i] = bq_run(&TK.s[1][1], &TK.ls, bq_run(&TK.s[1][0], &TK.pk, r[i])) * g;
    }
}

/* -------------------------------------------------------------------------------------------- cassette --- */
/* drive into the tape with pre-emphasis, saturate, de-emphasize; the head bump; the top end the tape and the
 * deck can still hold; wow, flutter and a slow drift on a modulated delay; dropouts on a tired tape; hiss
 * shaped like hiss; a little crosstalk. WEAR goes from fresh chrome to the thrift-store bin. */
#define TP_LEN 512
#define TP_STEP 16
static float tp_buf[2][TP_LEN];
static struct {
    bq_c pre, de, bump, lp;
    bq_t s[2][5];
    float wear_c, amt;
    int32_t w;
    float wow, flut, drift, drift_t, dph;
    float drop, drop_t;
    uint32_t drop_left;
    float hs[2][2];
    float d, hp[2], hpc;                 /* the delay now; the 30 Hz high-pass */
    bl_rng_t rng;
} TP;

static void tape_coefs(float wear)
{
    bq_highshelf(&TP.pre, 2800.0f, 6.0f);
    bq_highshelf(&TP.de, 2800.0f, -6.0f);
    bq_peak(&TP.bump, 90.0f, 1.0f, 1.5f + 1.0f * wear);
    TP.hpc = op_coef(30.0f);
    bq_lowpass(&TP.lp, 15000.0f * fm_exp2f(-1.1f * wear), 0.6f);
    TP.wear_c = wear;
}

static BL_NOINLINE void tape_run(float *l, float *r, int n, int on, float wear, float hiss)
{
    int i, c;
    float a0 = TP.amt, a1, drive, comp, wow_d, flut_d, hiss_g, x;
    float wr = 0.7f * BL_INV_SR, fr = 8.1f * BL_INV_SR;
    a1 = on ? fm_minf(1.0f, TP.amt + (float)n / 4410.0f) : fm_maxf(0.0f, TP.amt - (float)n / 4410.0f);   /* 0.1 s */
    TP.amt = a1;
    if (a0 == 0.0f && a1 == 0.0f)
        return;
    if (fm_fabsf(wear - TP.wear_c) > 0.01f)
        tape_coefs(wear);
    drive = 1.1f + 1.6f * wear;
    comp = 1.0f / soft(drive * 0.5f) * 0.5f;                 /* the level stays put as the drive goes up */
    /* speed variation as a fraction: 0.08% fresh .. 0.6% worn; delay depth = frac * SR / (2 pi f) */
    wow_d = (0.0006f + 0.0045f * wear * wear + 0.0012f * wear) * 44100.0f / (FM_TWO_PI * 0.7f);
    flut_d = (0.0002f + 0.0007f * wear) * 44100.0f / (FM_TWO_PI * 8.1f);
    hiss_g = fm_db2lin(-82.0f + 24.0f * hiss + 10.0f * wear);
    /* slow random drift: a new target every second or two */
    TP.drift_t -= (float)n * BL_INV_SR;
    if (TP.drift_t <= 0.0f) {
        TP.drift_t = rng_range(&TP.rng, 0.8f, 2.4f);
        TP.dph = rng_bi(&TP.rng) * (2.0f + 30.0f * wear * wear);
    }
    /* dropouts */
    if (wear > 0.5f && !TP.drop_left && rng_chance(&TP.rng, (wear - 0.5f) * 0.6f * (float)n * BL_INV_SR)) {
        TP.drop_left = (uint32_t)rng_range(&TP.rng, 1300.0f, 7000.0f);
        TP.drop_t = rng_range(&TP.rng, 0.25f, 0.7f);
    }
    {
        /* stage by stage, the state of each in locals: first the delay and the dropout gain a sample, then each
         * channel through the record chain into the loop and back out through the moving head */
        float dbuf[BL_BLOCK], gbuf[BL_BLOCK], obuf[2][BL_BLOCK];
        float d0 = TP.d, dd = 0.0f, drop = TP.drop, az = 0.35f * wear;
        uint32_t left = TP.drop_left;
        int32_t w0 = TP.w;
        int j = 0;
        for (i = 0; i < n; i++) {
            if (j-- == 0) {                                  /* the speed's swing: every TP_STEP samples, ramped */
                float d1;
                j = TP_STEP - 1;
                TP.wow = wrap1(TP.wow + wr * TP_STEP);
                TP.flut = wrap1(TP.flut + fr * TP_STEP);
                TP.drift += 0.00003f * TP_STEP * (TP.dph - TP.drift);
                d1 = 100.0f + wow_d * sin1(TP.wow) + flut_d * sin1(TP.flut) + TP.drift;   /* 4 .. 196 */
                dd = (d1 - d0) * (1.0f / TP_STEP);
            }
            d0 += dd;
            dbuf[i] = d0;
            if (left) {
                left--;
                drop += 0.004f * (TP.drop_t - drop);
            } else
                drop += 0.002f * (1.0f - drop);
            gbuf[i] = drop;
        }
        TP.d = d0;
        TP.drop = drop;
        TP.drop_left = left;
        for (c = 0; c < 2; c++) {
            const float *in = c ? r : l;
            float *buf = tp_buf[c], *o = obuf[c];
            bq_t s0 = TP.s[c][0], s1 = TP.s[c][1], s2 = TP.s[c][2], s4 = TP.s[c][4];
            const bq_c pre = TP.pre, de = TP.de, bump = TP.bump, lp = TP.lp;
            float hp = TP.hp[c], hpc = TP.hpc, hs = TP.hs[c][0], off = c ? az : 0.0f;
            bl_rng_t rng = TP.rng;
            int32_t w = w0;
            for (i = 0; i < n; i++) {                        /* record: the chain into the loop */
                x = bq_run(&s0, &pre, in[i]);
                x = soft(x * drive) * comp;
                x = bq_run(&s1, &de, x);
                x = bq_run(&s2, &bump, x);
                hp += hpc * (x - hp);                        /* 30 Hz: a one-pole is plenty */
                buf[w] = bq_run(&s4, &lp, x - hp);
                w = (w + 1) & (TP_LEN - 1);
            }
            w = w0;
            for (i = 0; i < n; i++) {                        /* play: the head (at least 4 samples behind), hiss */
                float h = rng_bi(&rng);
                hs += 0.35f * (h - hs);                      /* white, a little more on top */
                o[i] = tapf(buf, w, dbuf[i] + off, TP_LEN - 1) * gbuf[i] + (h - 0.6f * hs) * hiss_g;
                w = (w + 1) & (TP_LEN - 1);
            }
            TP.s[c][0] = s0;
            TP.s[c][1] = s1;
            TP.s[c][2] = s2;
            TP.s[c][4] = s4;
            TP.hp[c] = hp;
            TP.hs[c][0] = hs;
            TP.rng = rng;
        }
        TP.w = (w0 + n) & (TP_LEN - 1);
        if (a0 == 1.0f && a1 == 1.0f)
            for (i = 0; i < n; i++) {                        /* a little crosstalk */
                l[i] = obuf[0][i] + 0.03f * obuf[1][i];
                r[i] = obuf[1][i] + 0.03f * obuf[0][i];
            }
        else
            for (i = 0; i < n; i++) {
                /* the bypass: a crossfade (the dry is ahead of the tape by its delay; over 0.1 s it is a blur) */
                float a = a0 + (a1 - a0) * (float)i / (float)n;
                l[i] += a * (obuf[0][i] + 0.03f * obuf[1][i] - l[i]);
                r[i] += a * (obuf[1][i] + 0.03f * obuf[0][i] - r[i]);
            }
    }
}

/* --------------------------------------------------------------------------------------------- limiter --- */
static struct { float env, gain; } LM;

static BL_NOINLINE void limiter_run(float *l, float *r, int n)
{
    int i;
    const float thr = 0.84f, rel = t60_mul(0.9f);
    for (i = 0; i < n; i++) {
        float p = fm_maxf(fm_fabsf(l[i]), fm_fabsf(r[i])), g;
        LM.env = p > LM.env ? p : LM.env * rel;
        g = LM.env > thr ? thr / LM.env : 1.0f;
        LM.gain += (g < LM.gain ? 0.25f : 0.0008f) * (g - LM.gain);
        l[i] = soft(l[i] * LM.gain * 1.08f) * 0.93f;
        r[i] = soft(r[i] * LM.gain * 1.08f) * 0.93f;
    }
}

static void fx_clear(void)
{
    int i, k;
    for (k = 0; k < 2; k++)
        for (i = 0; i < CH_LEN; i++)
            ch_mem[k][0][i] = ch_mem[k][1][i] = 0.0f;
    for (i = 0; i < EC_LEN; i++)
        ec_buf[i] = 0;
    for (i = 0; i < RV_TOTAL; i++)
        rv_mem[i] = 0.0f;
    for (i = 0; i < PD_LEN; i++)
        pd_buf[i] = 0;
    for (i = 0; i < (int)(sizeof ap_mem / sizeof ap_mem[0]); i++)
        ap_mem[i] = 0.0f;
    for (i = 0; i < TP_LEN; i++)
        tp_buf[0][i] = tp_buf[1][i] = 0.0f;
    {
        float t = EC.time, f = EC.fbk;
        uint8_t *p = (uint8_t *)&EC;
        for (i = 0; i < (int)sizeof EC; i++)
            p[i] = 0;
        EC.time = t;
        EC.fbk = f;
    }
    for (k = 0; k < RV_N; k++) {
        RV.w[k] = 0;
        RV.lp[k] = 0.0f;
    }
    for (k = 0; k < 4; k++)
        RV.api[k] = 0;
    RV.pdw = 0;
    RV.in_hp = RV.in_lp = 0.0f;
    RV.half = 0;
    RV.acc = RV.yl = RV.yr = RV.pl = RV.pr = 0.0f;
    reverb_layout();
    chorus_init(&CHO[0], ch_mem[0][0], ch_mem[0][1], 0.6f, 6.6f, 330.0f, 95.0f, 7.0f);
    chorus_init(&CHO[1], ch_mem[1][0], ch_mem[1][1], 0.9f, 5.1f, 260.0f, 42.0f, 3.0f);
    for (k = 0; k < 2; k++)
        for (i = 0; i < 5; i++)
            TP.s[k][i].z1 = TP.s[k][i].z2 = 0.0f;
    TP.w = 0;
    TP.wow = TP.flut = TP.drift = TP.dph = TP.drift_t = 0.0f;
    TP.d = 100.0f;
    TP.hp[0] = TP.hp[1] = 0.0f;
    TP.drop = 1.0f;
    TP.drop_left = 0;
    TP.hs[0][0] = TP.hs[1][0] = 0.0f;
    for (k = 0; k < 2; k++)
        for (i = 0; i < 2; i++)
            TK.s[k][i].z1 = TK.s[k][i].z2 = 0.0f;
    LM.env = 0.0f;
    LM.gain = 1.0f;
}
