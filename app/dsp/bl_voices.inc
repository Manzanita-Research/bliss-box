/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* The voices. Part of bliss.c (one unit). Every voice is synthesized: the FM-1 has room for code, not samples. */

#define NVOICES 24
#define NSTRINGS 10
#define KS_LEN 1024                          /* a string down to 43 Hz */

enum { L_PAD, L_PEDAL, L_KEYS, L_HARP, L_PLUCK, L_BELL, L_FLUTE, L_SOLO, L_STING, NLAYERS };
enum { BUS_PAD, BUS_KEYS, BUS_DRY };

typedef struct {
    uint8_t on, type, layer, gate, perc, bus;
    int8_t note;
    int8_t ks[2];                            /* string indices, -1 none */
    uint32_t id;
    float f, vel, gain, pl, pr, rev, ech;
    float env, env2, env_tgt, env_c, rel_c, dec, dec_m;
    float t, hold;                           /* seconds alive; auto note-off after hold (< 0: by the gate) */
    float peak;                              /* a slow level, to free a string voice when it has died away */
    union { float ph[10]; uint32_t uph[10]; };     /* float turns, or integer (uturns) for the FM voices */
    union { float dph[10]; uint32_t udph[10]; };
    float amp[10], amul[10];
    float x[8];
    svf_t s[3];
    svf_c c[3];
} voice_t;

typedef struct {
    float buf[KS_LEN];
    int32_t len, w;
    float apc, ap_x1, ap_y1, lpc, lp, g;
    uint8_t busy;
    uint32_t owner;
} ks_t;

static voice_t V[NVOICES];
static BL_POOL ks_t KS[NSTRINGS];
static uint32_t voice_ids;
static bl_rng_t noise_rng;                   /* the breath and the hammers */

static inline float noise1(void) { return rng_bi(&noise_rng); }

static const float TYPE_GAIN[BV_NTYPES] = {
    0.30f, /* rhodes */ 0.20f, /* cp80 */ 0.30f, /* flute */ 0.55f, /* harp */ 0.42f, /* zither */
    0.17f, /* bells */ 0.085f, /* strings */ 0.16f, /* choir */ 0.12f /* glass */
};
/* sends: reverb, echo */
static const float TYPE_REV[BV_NTYPES] = {0.32f, 0.30f, 0.45f, 0.42f, 0.45f, 0.62f, 0.55f, 0.60f, 0.60f};
static const float TYPE_ECH[BV_NTYPES] = {0.22f, 0.18f, 0.65f, 0.30f, 0.35f, 0.42f, 0.04f, 0.06f, 0.08f};
/* what a voice costs a sample, roughly, in the units of BL_VOICE_BUDGET (the CP-80's is per resonator pair) */
static const uint8_t TYPE_COST[BV_NTYPES] = {14, 4, 14, 10, 10, 20, 16, 10, 16};
#define BL_VOICE_BUDGET 180
static int voice_cost(const voice_t *v) { return TYPE_COST[v->type] + (v->type == BV_CP80 ? 4 * (int)v->x[4] / 2 : 0); }

static int is_pad(int t) { return t == BV_STRINGS || t == BV_CHOIR || t == BV_GLASS; }

/* --------------------------------------------------------------------------------------- strings (KS) --- */
static int ks_alloc(uint32_t owner)
{
    int i, best = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (i = 0; i < NSTRINGS; i++)
        if (!KS[i].busy) {
            KS[i].busy = 1;
            KS[i].owner = owner;
            return i;
        }
    for (i = 0; i < NSTRINGS; i++)          /* all busy: the oldest owner gives its string up */
        if (KS[i].owner < oldest) {
            oldest = KS[i].owner;
            best = i;
        }
    if (best >= 0) {
        int v;
        for (v = 0; v < NVOICES; v++)
            if (V[v].on && V[v].id == oldest) {
                if (V[v].ks[0] == best)
                    V[v].ks[0] = -1;
                if (V[v].ks[1] == best)
                    V[v].ks[1] = -1;
                if (V[v].ks[0] < 0 && V[v].ks[1] < 0)
                    V[v].on = 0;
            }
        KS[best].owner = owner;
    }
    return best;
}

/* pluck: fill the loop with a burst of noise shaped by how hard and where it was plucked */
static void ks_pluck(ks_t *k, float f, float vel, float bright, float t60, float pos)
{
    float d, lpd, lp = 0.0f, prev = 0.0f, c;
    int32_t i, pd;
    k->lpc = bright;                          /* loop low-pass: one-pole coefficient */
    lpd = (1.0f - bright) / bright;           /* its delay at low frequencies, in samples */
    d = 44100.0f / f - lpd - 0.5f;
    if (d < 4.0f)
        d = 4.0f;
    if (d > (float)(KS_LEN - 2))
        d = (float)(KS_LEN - 2);
    k->len = (int32_t)d;
    d -= (float)k->len;
    if (d < 0.15f) {                          /* keep the allpass's delay in its well-behaved range */
        k->len--;
        d += 1.0f;
    }
    k->apc = (1.0f - d) / (1.0f + d);
    k->ap_x1 = k->ap_y1 = 0.0f;
    k->lp = 0.0f;
    k->g = fm_exp2f(-9.965784f / (t60 * f));  /* -60 dB in t60 */
    c = 0.25f + 0.7f * vel;                   /* the burst: brighter when harder */
    pd = (int32_t)(pos * (float)k->len);
    if (pd < 1)
        pd = 1;
    for (i = 0; i < KS_LEN; i++)
        k->buf[i] = 0.0f;
    for (i = 0; i < k->len; i++) {
        lp += c * (noise1() - lp);
        k->buf[i] = lp;
    }
    for (i = k->len - 1; i >= pd; i--)        /* pluck position: a comb */
        k->buf[i] -= k->buf[i - pd];
    for (i = 0; i < k->len; i++) {            /* and no DC */
        prev += (k->buf[i] - prev) * 0.05f;
        k->buf[i] -= prev;
    }
    k->w = k->len;
    (void)prev;
}

static inline float ks_tick(ks_t *k)
{
    int32_t r = (k->w - k->len) & (KS_LEN - 1);
    float x = k->buf[r], y;
    y = k->apc * (x - k->ap_y1) + k->ap_x1;   /* first-order allpass: the fraction of a sample */
    k->ap_x1 = x;
    k->ap_y1 = y;
    k->lp += k->lpc * (y - k->lp);
    y = k->lp * k->g;
    k->buf[k->w & (KS_LEN - 1)] = y;
    k->w = (k->w + 1) & (KS_LEN - 1);
    return y;
}

/* ------------------------------------------------------------------------------------------ allocation --- */
static voice_t *voice_alloc(int layer)
{
    int i, best = -1;
    float lowest = 1e9f;
    uint32_t oldest = 0xFFFFFFFFu;
    for (i = 0; i < NVOICES; i++)
        if (!V[i].on)
            return &V[i];
    for (i = 0; i < NVOICES; i++)           /* a released voice, the quietest */
        if (!V[i].gate && V[i].env * V[i].dec < lowest) {
            lowest = V[i].env * V[i].dec;
            best = i;
        }
    if (best < 0)
        for (i = 0; i < NVOICES; i++)       /* else the oldest of the same layer, else the oldest */
            if (V[i].id < oldest && (V[i].layer == layer || best < 0)) {
                oldest = V[i].id;
                best = i;
            }
    if (best < 0)
        best = 0;
    for (i = 0; i < 2; i++)
        if (V[best].ks[i] >= 0)
            KS[V[best].ks[i]].busy = 0;
    return &V[best];
}

static void voice_free(voice_t *v)
{
    int i;
    for (i = 0; i < 2; i++)
        if (v->ks[i] >= 0) {
            if (KS[v->ks[i]].owner == v->id)
                KS[v->ks[i]].busy = 0;
            v->ks[i] = -1;
        }
    v->on = 0;
}

static void voice_release(voice_t *v, float rel_s)
{
    if (!v->on)
        return;
    v->gate = 0;
    v->env_tgt = 0.0f;
    if (rel_s > 0.0f)
        v->rel_c = tau_coef(rel_s * 0.25f);   /* a time constant a quarter of the release: ~ -35 dB at rel_s */
    v->env_c = v->rel_c;
}

/* ---------------------------------------------------------------------------------------------- start --- */
/* attack and release in seconds (0 = the type's own) */
static voice_t *voice_start(int type, int layer, int note, float f, float vel, float gain, float pan,
                            float att, float rel, float hold)
{
    voice_t *v = voice_alloc(layer);
    int i;
    float pitch = (float)(note - 60) / 12.0f;   /* octaves from middle C */
    for (i = 0; i < 2; i++)
        if (v->ks[i] >= 0 && v->on) {
            KS[v->ks[i]].busy = 0;
        }
    for (i = 0; i < (int)sizeof *v; i++)
        ((uint8_t *)v)[i] = 0;
    v->ks[0] = v->ks[1] = -1;
    v->on = 1;
    v->gate = 1;
    v->type = (uint8_t)type;
    v->layer = (uint8_t)layer;
    v->note = (int8_t)note;
    v->id = ++voice_ids;
    v->f = f;
    v->vel = vel;
    v->gain = gain * TYPE_GAIN[type];
    pan_gains(fm_clampf(pan, -1.0f, 1.0f), &v->pl, &v->pr);
    v->rev = TYPE_REV[type];
    v->ech = TYPE_ECH[type];
    v->hold = hold;
    v->dec = 1.0f;
    v->dec_m = 1.0f;
    v->env_tgt = 1.0f;
    v->bus = BUS_DRY;
    switch (type) {
    case BV_RHODES: {
        float t60 = fm_clampf(9.0f * fm_exp2f(-pitch * 0.65f), 2.2f, 12.0f);
        v->perc = 1;
        v->bus = BUS_KEYS;
        v->env_c = tau_coef(0.0009f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 0.35f) * 0.25f);
        v->dec_m = t60_mul(t60);
        v->udph[0] = uturns(f * BL_INV_SR);
        v->udph[1] = uturns(f * 1.0007f * BL_INV_SR);
        v->udph[2] = uturns(f * (f * 14.0f < 15000.0f ? 14.0f : 7.0f) * BL_INV_SR);
        v->amp[0] = (0.35f + 1.9f * vel * vel) * 0.159155f;     /* body index, in turns */
        v->amul[0] = t60_mul(1.4f);
        v->amp[1] = (0.25f + 1.0f * vel) * 0.159155f;           /* tine index */
        v->amul[1] = t60_mul(0.11f * fm_exp2f(-pitch * 0.5f));
        v->x[0] = 0.25f * 0.159155f;                            /* the body index never quite goes */
        v->x[1] = 0.12f + 0.45f * vel;                          /* pickup bark */
        break;
    }
    case BV_CP80: {
        float t1 = fm_clampf(7.0f * fm_exp2f(-pitch * 0.8f), 1.6f, 9.0f), tilt = 1.55f - 0.8f * vel;
        int n;
        v->perc = 1;
        v->bus = BUS_KEYS;
        v->env_c = tau_coef(0.0006f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 0.25f) * 0.25f);
        /* each partial a damped two-pole resonator, y = c*y1 - r2*y2: a decaying sine for two multiplies.
         * ph = y1, dph = y2, amp = c = 2 r cos w, amul = r2 = r*r. Partials above 9 kHz are left out (x[4]). */
        v->x[4] = 0.0f;
        for (n = 0; n < 5; n++) {
            float k = (float)(n + 1), fn = f * k * fm_sqrtf(1.0f + 0.00028f * k * k);
            float a = fm_powf(k, -tilt) * 0.42f;
            float r = t60_mul(t1 / (1.0f + 0.5f * (float)n * (1.0f + f / 900.0f)));
            int s;
            if (fn >= 9000.0f)
                break;
            for (s = 0; s < 2; s++) {                           /* string A a little flat, B a little sharp */
                float w = fn * (s ? 1.0019f : 1.0f - 0.0019f) * BL_INV_SR, p0 = s ? 0.13f * k : 0.0f;
                int j = 2 * n + s;
                v->amp[j] = 2.0f * r * sin1(wrap1(w + 0.25f));
                v->amul[j] = r * r;
                v->ph[j] = a * sin1(wrap1(p0));
                v->dph[j] = a * sin1(wrap1(p0 - w)) / r;
            }
            v->x[4] += 2.0f;
        }
        v->dec_m = t60_mul(t1);                                 /* the fundamental: the voice ends with it */
        v->x[0] = 1.0f;                                         /* hammer noise */
        v->x[1] = t60_mul(0.025f);
        svf_set(&v->c[0], f * 3.1f, 1.4f);
        v->x[2] = 0.1f + 0.3f * vel;                            /* pickup */
        break;
    }
    case BV_FLUTE:
        v->env_c = tau_coef((att > 0.0f ? att : 0.18f) * 0.5f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 0.7f) * 0.25f);
        v->uph[1] = 0;                                          /* vibrato phase, and its rate */
        v->udph[1] = uturns(rng_range(&noise_rng, 4.6f, 5.4f) * BL_INV_SR);
        v->x[2] = 0.0f;                                         /* vibrato depth now */
        v->x[3] = f;                                            /* the glide's frequency now */
        v->x[4] = 1.0f;                                         /* chiff */
        v->x[5] = t60_mul(0.25f);
        v->x[6] = att > 0.0f ? att : 0.18f;
        svf_set(&v->c[1], 2600.0f, 0.7f);
        break;
    case BV_HARP:
    case BV_ZITHER: {
        int zither = type == BV_ZITHER, s;
        float t60 = fm_clampf(5.5f * fm_exp2f(-pitch * 0.55f), 1.2f, 8.0f) * (zither ? 1.5f : 1.0f);
        float bright = zither ? 0.72f + 0.2f * vel : 0.42f + 0.33f * vel;
        v->perc = 1;
        v->env_c = 1.0f;
        v->env = v->env2 = 1.0f;
        v->rel_c = tau_coef((rel > 0.0f ? rel : 2.5f) * 0.25f);
        for (s = 0; s < (zither ? 2 : 1); s++) {
            int k = ks_alloc(v->id);
            v->ks[s] = (int8_t)k;
            if (k >= 0)
                ks_pluck(&KS[k], f * (zither ? (s ? 1.0024f : 0.9976f) : 1.0f), vel, bright, t60,
                         zither ? 0.08f + 0.04f * (float)s : 0.13f);
        }
        v->x[0] = 0.0f;                                         /* DC */
        break;
    }
    case BV_BELLS: {
        float t60 = fm_clampf(10.0f * fm_exp2f(-pitch * 0.5f), 3.0f, 14.0f);
        v->perc = 1;
        v->env_c = tau_coef(0.0005f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 3.0f) * 0.25f);
        v->dec_m = t60_mul(t60);
        v->udph[0] = uturns(f * BL_INV_SR);
        v->udph[1] = uturns(f * 3.5f * BL_INV_SR);
        v->udph[2] = uturns((f * 2.0f + 0.45f) * BL_INV_SR);
        v->udph[3] = uturns((f * 2.0f + 0.45f) * 1.4142f * BL_INV_SR);
        v->amp[0] = (0.6f + 1.8f * vel) * 0.159155f;
        v->amul[0] = t60_mul(1.8f);
        v->amp[1] = (0.4f + 0.9f * vel) * 0.159155f;
        v->amul[1] = t60_mul(0.5f);
        v->amp[2] = 0.33f;                                      /* the strike partial's level */
        v->amul[2] = t60_mul(t60 * 0.35f);
        v->x[0] = 0.18f * 0.159155f;
        break;
    }
    case BV_STRINGS:
        v->bus = BUS_PAD;
        v->env_c = tau_coef((att > 0.0f ? att : 3.0f) * 0.42f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 6.0f) * 0.25f);
        v->amp[0] = f * (1.0f - 0.0034f) * BL_INV_SR;           /* the three saws' rates in turns (drift scales them) */
        v->amp[1] = f * (1.0f + 0.0037f) * BL_INV_SR;
        v->amp[2] = f * BL_INV_SR;
        v->uph[0] = uturns(rng_unit(&noise_rng) - 0.5f);
        v->uph[1] = uturns(rng_unit(&noise_rng) - 0.5f);
        v->uph[2] = uturns(rng_unit(&noise_rng) - 0.5f);
        v->x[0] = rng_unit(&noise_rng);                         /* drift LFO */
        v->x[1] = rng_range(&noise_rng, 0.05f, 0.16f) * BL_INV_SR;
        v->x[2] = rng_unit(&noise_rng);                         /* filter LFO */
        v->x[3] = rng_range(&noise_rng, 0.03f, 0.09f) * BL_INV_SR;
        break;
    case BV_CHOIR:
        v->bus = BUS_PAD;
        v->env_c = tau_coef((att > 0.0f ? att : 3.5f) * 0.42f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 6.0f) * 0.25f);
        v->amp[0] = f * BL_INV_SR;                              /* the two saws' rates in turns */
        v->amp[1] = f * 1.0041f * BL_INV_SR;
        v->uph[0] = uturns(rng_unit(&noise_rng) - 0.5f);
        v->uph[1] = uturns(rng_unit(&noise_rng) - 0.5f);
        v->x[0] = rng_unit(&noise_rng);                         /* vibrato */
        v->x[1] = rng_range(&noise_rng, 4.9f, 5.6f) * BL_INV_SR;
        v->x[2] = rng_range(&noise_rng, -0.35f, 0.35f);         /* this voice's own vowel */
        break;
    case BV_GLASS:
        v->bus = BUS_PAD;
        v->env_c = tau_coef((att > 0.0f ? att : 2.2f) * 0.42f);
        v->rel_c = tau_coef((rel > 0.0f ? rel : 5.0f) * 0.25f);
        v->udph[0] = uturns(f * BL_INV_SR);
        v->udph[1] = uturns(f * 1.0004f * BL_INV_SR);
        v->udph[2] = uturns((f * 2.0f + 0.31f) * BL_INV_SR);
        v->udph[3] = uturns(f * 3.0f * BL_INV_SR);
        v->udph[4] = uturns((f * 4.0f + 0.9f) * BL_INV_SR);
        v->x[0] = rng_unit(&noise_rng);                         /* index LFO */
        v->x[1] = rng_range(&noise_rng, 0.07f, 0.2f) * BL_INV_SR;
        v->x[2] = rng_unit(&noise_rng);                         /* tremolo */
        v->x[3] = rng_range(&noise_rng, 0.3f, 0.7f) * BL_INV_SR;
        break;
    }
    return v;
}

/* ----------------------------------------------------------------------------------------------- render --- */
static float bus_pad[2][BL_BLOCK], bus_keys[2][BL_BLOCK], bus_dry[2][BL_BLOCK];
static float send_rev[BL_BLOCK], send_ech[BL_BLOCK];

typedef struct { float tone, vowel, lg[NLAYERS]; } voice_ctx_t;

/* one voice's block into vbuf; returns 0 when the voice has finished. (restrict: the voice's state stays in
 * registers through the loop instead of being read back after every store to vbuf) */
/* one function per type, so a profile shows each */
static BL_NOINLINE int vb_rhodes(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    uint32_t p0 = v->uph[0], p1 = v->uph[1], p2 = v->uph[2], d0 = v->udph[0], d1 = v->udph[1], d2 = v->udph[2];
    float g = v->gain, env = v->env, ec = v->env_c, et = v->env_tgt, dec = v->dec, dm = v->dec_m;
    float a0 = v->amp[0], a1 = v->amp[1], m0 = v->amul[0], m1 = v->amul[1], floor0 = v->x[0], bark = v->x[1];
    float dc = v->x[2];
    (void)cx;
    for (i = 0; i < n; i++) {
        float s = sinu(p0 + uturns((a0 + floor0) * sinu(p1) + a1 * sinu(p2)));
        env += ec * (et - env);
        s = s + bark * s * s;
        dc += (s - dc) * 0.002f;                                /* DC out */
        vbuf[i] = (s - dc) * env * dec * g;
        dec *= dm;
        a0 *= m0;
        a1 *= m1;
        p0 += d0;
        p1 += d1;
        p2 += d2;
    }
    v->uph[0] = p0;
    v->uph[1] = p1;
    v->uph[2] = p2;
    v->env = env;
    v->dec = dec;
    v->amp[0] = a0;
    v->amp[1] = a1;
    v->x[2] = dc;
    return 1;
}

static BL_NOINLINE int vb_cp80(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i, k, np = (int)v->x[4];
    float g = v->gain, dc = v->x[3], hm = v->x[0], hd = v->x[1], hv = 0.4f + v->vel, pu = v->x[2];
    float env = v->env, ec = v->env_c, et = v->env_tgt, dec = v->dec, dm = v->dec_m;
    (void)cx;
    for (i = 0; i < n; i++)
        vbuf[i] = 0.0f;
    for (k = 0; k + 1 < np; k += 2) {                       /* the two strings of a partial together: two chains
                                                             * the FPU can overlap, their state in registers */
        float a1 = v->ph[k], a2 = v->dph[k], ac = v->amp[k], ar = v->amul[k];
        float b1 = v->ph[k + 1], b2 = v->dph[k + 1], bc = v->amp[k + 1], br = v->amul[k + 1];
        for (i = 0; i < n; i++) {
            float a0 = ac * a1 - ar * a2, b0 = bc * b1 - br * b2;
            vbuf[i] += a0 + b0;
            a2 = a1;
            a1 = a0;
            b2 = b1;
            b1 = b0;
        }
        v->ph[k] = a1;
        v->dph[k] = a2;
        v->ph[k + 1] = b1;
        v->dph[k + 1] = b2;
    }
    if (hm > 1e-5f) {                                       /* the hammer, the first tenth of a second */
        for (i = 0; i < n; i++) {
            vbuf[i] += svf_bp(&v->s[0], &v->c[0], noise1()) * hm * hv;
            hm *= hd;
        }
    } else {
        hm = 0.0f;
    }
    for (i = 0; i < n; i++) {
        float s = vbuf[i];
        s = s + pu * s * s;
        dc += (s - dc) * 0.002f;
        env += ec * (et - env);
        vbuf[i] = (s - dc) * env * g;
        dec *= dm;
    }
    v->env = env;
    v->dec = dec;
    v->x[0] = hm;
    v->x[3] = dc;
    /* the top partial, the quickest to fade, is dropped once it is under -100 dB: a resonator's squared
     * amplitude is (y1^2 - c y1 y2 + r2 y2^2) / (1 - c^2 / 4 r2) */
    if (np >= 2) {
        float y1 = v->ph[np - 2], y2 = v->dph[np - 2], c = v->amp[np - 2], r2 = v->amul[np - 2];
        float q = (y1 * y1 - c * y1 * y2 + r2 * y2 * y2) / (1.0f - c * c / (4.0f * r2) + 1e-9f);
        float lv = env * g * (1.0f + pu);
        if (q * lv * lv < 1e-10f)
            v->x[4] = (float)(np - 2);
    }
    return 1;
}

static BL_NOINLINE int vb_flute(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    uint32_t ph = v->uph[0], vp = v->uph[1], vd = v->udph[1];
    const uint32_t third = 429496730u;                         /* a tenth of a turn: the third harmonic's offset */
    float g = v->gain, env = v->env, env2 = v->env2, ec = v->env_c, et = v->env_tgt;
    float glide = tau_coef(0.07f), fnow = v->x[3], ftgt = v->f, depth = v->x[2], chiff = v->x[4], cm = v->x[5];
    float dstep = v->t > 0.35f ? 0.0042f / 30000.0f : 0.0f;   /* the vibrato comes in after a moment */
    (void)cx;
    svf_set(&v->c[0], fnow, 6.0f);
    for (i = 0; i < n; i++) {
        float vib, s, a, br;
        env += ec * (et - env);
        env2 += ec * (env - env2);                          /* two poles: the swell's S */
        a = env2;
        depth = fm_minf(depth + dstep, 0.0042f);
        vib = sinu(vp) * depth;
        vp += vd;
        fnow += glide * (ftgt - fnow);
        ph += uturns(fnow * (1.0f + vib) * BL_INV_SR);
        s = sinu(ph) + (0.05f + 0.16f * a) * sinu(ph * 2u) + (0.015f + 0.05f * a) * sinu(ph * 3u + third);
        br = svf_bp(&v->s[0], &v->c[0], noise1()) * 0.55f + svf_bp(&v->s[1], &v->c[1], noise1()) * 0.22f;
        br *= 0.09f + 0.55f * chiff + 0.08f * (1.0f - a);
        chiff *= cm;
        vbuf[i] = (s * (1.0f + vib * 6.0f) + br) * a * g;
    }
    v->uph[0] = ph;
    v->uph[1] = vp;
    v->env = env;
    v->env2 = env2;
    v->x[2] = depth;
    v->x[3] = fnow;
    v->x[4] = chiff;
    return 1;
}

static BL_NOINLINE int vb_ks(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    float g = v->gain;
    int z = v->type == BV_ZITHER;
    float pk = 0.0f;
    for (i = 0; i < n; i++) {
        float s = 0.0f;
        if (v->ks[0] >= 0)
            s += ks_tick(&KS[(int)v->ks[0]]);
        if (z && v->ks[1] >= 0)
            s += ks_tick(&KS[(int)v->ks[1]]);
        v->env += v->env_c * (v->env_tgt - v->env);
        s *= v->env * g;
        vbuf[i] = s;
        pk = fm_maxf(pk, fm_fabsf(s));
    }
    v->peak = v->peak * 0.9f + pk;
    if (v->t > 0.3f && v->peak < 0.00004f)
        return 0;
    if (v->ks[0] < 0 && v->ks[1] < 0)
        return 0;
    (void)cx;
    (void)g;
    return 1;
}

static BL_NOINLINE int vb_bells(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    uint32_t p0 = v->uph[0], p1 = v->uph[1], p2 = v->uph[2], p3 = v->uph[3];
    uint32_t d0 = v->udph[0], d1 = v->udph[1], d2 = v->udph[2], d3 = v->udph[3];
    float g = v->gain, env = v->env, ec = v->env_c, et = v->env_tgt, dec = v->dec, dm = v->dec_m;
    float a0 = v->amp[0], a1 = v->amp[1], a2 = v->amp[2], m0 = v->amul[0], m1 = v->amul[1], m2 = v->amul[2];
    float floor0 = v->x[0];
    (void)cx;
    for (i = 0; i < n; i++) {
        float s;
        env += ec * (et - env);
        s = sinu(p0 + uturns((a0 + floor0) * sinu(p1)));
        s += a2 * sinu(p2 + uturns(a1 * sinu(p3)));
        vbuf[i] = s * env * dec * g;
        dec *= dm;
        a0 *= m0;
        a1 *= m1;
        a2 *= m2;
        p0 += d0;
        p1 += d1;
        p2 += d2;
        p3 += d3;
    }
    v->uph[0] = p0;
    v->uph[1] = p1;
    v->uph[2] = p2;
    v->uph[3] = p3;
    v->env = env;
    v->dec = dec;
    v->amp[0] = a0;
    v->amp[1] = a1;
    v->amp[2] = a2;
    return 1;
}

static BL_NOINLINE int vb_strings(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    float g = v->gain, env = v->env, env2 = v->env2, ec = v->env_c, et = v->env_tgt, drift, cut;
    float i0, i1, i2;
    uint32_t p0 = v->uph[0], p1 = v->uph[1], p2 = v->uph[2], d0, d1, d2;
    svf_t st = v->s[0];
    svf_c c;
    v->x[0] = wrap1(v->x[0] + v->x[1] * (float)n);
    v->x[2] = wrap1(v->x[2] + v->x[3] * (float)n);
    drift = 1.0f + 0.0011f * sin1(v->x[0]);
    cut = (380.0f + 3600.0f * cx->tone * cx->tone) * fm_sqrtf(v->f / 262.0f) * (1.0f + 0.22f * sin1(v->x[2]))
          * (0.55f + 0.45f * env2);
    svf_set(&c, cut, 0.75f);
    d0 = uturns(v->amp[0] * drift);
    d1 = uturns(v->amp[1] * drift);
    d2 = uturns(v->amp[2] * drift);
    i0 = 1.0f / (float)d0;
    i1 = 1.0f / (float)d1;
    i2 = 1.0f / (float)d2;
    for (i = 0; i < n; i++) {
        float s;
        env += ec * (et - env);
        env2 += ec * (env - env2);
        s = saw_u(p0, d0, i0) + saw_u(p1, d1, i1) + 0.6f * saw_u(p2, d2, i2);
        p0 += d0;
        p1 += d1;
        p2 += d2;
        vbuf[i] = svf_lp(&st, &c, s) * env2 * g;
    }
    v->uph[0] = p0;
    v->uph[1] = p1;
    v->uph[2] = p2;
    v->s[0] = st;
    v->env = env;
    v->env2 = env2;
    return 1;
}

/* The choir: each voice is only its source, two saws a little apart and some breath. Its vowel is one formant
 * bank per side on the choir's sum (choir_run): three filters for the whole choir, not three for every singer,
 * and the two sides' vowels a little apart, which spreads the choir across the stereo field. */
static float choir_src[2][BL_BLOCK];
static svf_t CHS[2][3];
static uint8_t choir_live, choir_tail;         /* a choir voice sounded this block; blocks left to ring out */

static BL_NOINLINE int vb_choir(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    float g = v->gain, env = v->env, env2 = v->env2, ec = v->env_c, et = v->env_tgt, vib0, vib1, i0, i1;
    uint32_t p0 = v->uph[0], p1 = v->uph[1], d0, d1;
    int32_t dd0, dd1;
    (void)cx;
    /* the vibrato at the block's two ends, the pitch ramped between them */
    vib0 = 1.0f + 0.0028f * sin1(v->x[0]);
    v->x[0] = wrap1(v->x[0] + v->x[1] * (float)n);
    vib1 = 1.0f + 0.0028f * sin1(v->x[0]);
    d0 = uturns(v->amp[0] * vib0);
    d1 = uturns(v->amp[1] * vib0);
    dd0 = ((int32_t)uturns(v->amp[0] * vib1) - (int32_t)d0) / n;
    dd1 = ((int32_t)uturns(v->amp[1] * vib1) - (int32_t)d1) / n;
    i0 = 1.0f / (float)d0;
    i1 = 1.0f / (float)d1;
    for (i = 0; i < n; i++) {
        float s;
        env += ec * (et - env);
        env2 += ec * (env - env2);
        s = saw_u(p0, d0, i0) + saw_u(p1, d1, i1) + noise1() * 0.12f;
        p0 += d0;
        p1 += d1;
        d0 += (uint32_t)dd0;
        d1 += (uint32_t)dd1;
        vbuf[i] = s * env2 * g;
    }
    v->env = env;
    v->env2 = env2;
    v->uph[0] = p0;
    v->uph[1] = p1;
    return 1;
}

/* the choir's two formant banks onto the pad bus, and its echo send. vowel 0 oo .. 1 oh .. 2 ah */
static BL_NOINLINE void choir_run(int n, float vowel, float tone, float ech)
{
    static const float F[3][3] = {{300.0f, 870.0f, 2240.0f}, {460.0f, 820.0f, 2800.0f}, {730.0f, 1090.0f, 2440.0f}};
    static const float A[3][3] = {{1.0f, 0.30f, 0.10f}, {1.0f, 0.45f, 0.14f}, {1.0f, 0.55f, 0.20f}};
    int side, i;
    if (choir_live)
        choir_tail = 8;
    else if (choir_tail)
        choir_tail--;
    else {
        for (side = 0; side < 2; side++)
            for (i = 0; i < 3; i++)
                CHS[side][i].ic1 = CHS[side][i].ic2 = 0.0f;
        return;
    }
    for (side = 0; side < 2; side++) {
        float vw = fm_clampf(vowel + (side ? 0.16f : -0.16f), 0.0f, 1.999f), fr, f1, f2, f3, a2, a3;
        float *src = choir_src[side], *out = bus_pad[side];
        svf_c c1, c2, c3;
        svf_t s1 = CHS[side][0], s2 = CHS[side][1], s3 = CHS[side][2];
        int k = (int)vw;
        fr = vw - (float)k;
        f1 = F[k][0] + fr * (F[k + 1][0] - F[k][0]);
        f2 = F[k][1] + fr * (F[k + 1][1] - F[k][1]);
        f3 = F[k][2] + fr * (F[k + 1][2] - F[k][2]);
        a2 = A[k][1] + fr * (A[k + 1][1] - A[k][1]);
        a3 = (A[k][2] + fr * (A[k + 1][2] - A[k][2])) * (0.6f + 0.8f * tone);
        svf_set(&c1, f1, f1 / 80.0f);
        svf_set(&c2, f2, f2 / 100.0f);
        svf_set(&c3, f3, f3 / 140.0f);
        for (i = 0; i < n; i++) {
            float x = src[i], y = svf_bp(&s1, &c1, x) + a2 * svf_bp(&s2, &c2, x) + a3 * svf_bp(&s3, &c3, x);
            out[i] += y;
            send_ech[i] += y * ech;
            src[i] = 0.0f;
        }
        CHS[side][0] = s1;
        CHS[side][1] = s2;
        CHS[side][2] = s3;
    }
    choir_live = 0;
}

static BL_NOINLINE int vb_glass(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int i;
    uint32_t p0 = v->uph[0], p1 = v->uph[1], p2 = v->uph[2], p3 = v->uph[3], p4 = v->uph[4];
    uint32_t d0 = v->udph[0], d1 = v->udph[1], d2 = v->udph[2], d3 = v->udph[3], d4 = v->udph[4];
    float g = v->gain, env = v->env, env2 = v->env2, ec = v->env_c, et = v->env_tgt, i1, trem;
    v->x[0] = wrap1(v->x[0] + v->x[1] * (float)n);
    v->x[2] = wrap1(v->x[2] + v->x[3] * (float)n);
    i1 = (0.55f + 0.45f * sin1(v->x[0])) * (0.5f + 0.9f * cx->tone) * 0.159155f;
    trem = 0.07f * (0.5f + 0.5f * sin1(v->x[2]));
    for (i = 0; i < n; i++) {
        float s;
        env += ec * (et - env);
        env2 += ec * (env - env2);
        s = sinu(p0 + uturns(i1 * sinu(p1)));
        s += 0.3f * sinu(p2 + uturns(0.06f * sinu(p3)));
        s += trem * sinu(p4);
        vbuf[i] = s * env2 * g;
        p0 += d0;
        p1 += d1;
        p2 += d2;
        p3 += d3;
        p4 += d4;
    }
    v->uph[0] = p0;
    v->uph[1] = p1;
    v->uph[2] = p2;
    v->uph[3] = p3;
    v->uph[4] = p4;
    v->env = env;
    v->env2 = env2;
    return 1;
}

static BL_NOINLINE int voice_block(voice_t *restrict v, int n, const voice_ctx_t *restrict cx, float *restrict vbuf)
{
    int alive = 1;
    switch (v->type) {
    case BV_RHODES:
        alive = vb_rhodes(v, n, cx, vbuf);
        break;
    case BV_CP80:
        alive = vb_cp80(v, n, cx, vbuf);
        break;
    case BV_FLUTE:
        alive = vb_flute(v, n, cx, vbuf);
        break;
    case BV_HARP:
    case BV_ZITHER:
        alive = vb_ks(v, n, cx, vbuf);
        break;
    case BV_BELLS:
        alive = vb_bells(v, n, cx, vbuf);
        break;
    case BV_STRINGS:
        alive = vb_strings(v, n, cx, vbuf);
        break;
    case BV_CHOIR:
        alive = vb_choir(v, n, cx, vbuf);
        break;
    case BV_GLASS:
        alive = vb_glass(v, n, cx, vbuf);
        break;
    }
    if (!alive)
        return 0;
    (void)cx;
    v->t += (float)n * BL_INV_SR;
    if (v->hold >= 0.0f && v->gate && v->t >= v->hold)
        voice_release(v, 0.0f);
    if (!v->gate && v->env < 0.00015f)
        return 0;
    if (v->perc && v->dec < 0.0002f)
        return 0;
    return 1;
}

static BL_NOINLINE int voices_render(int n, const voice_ctx_t *cx, float sendscale_rev, float sendscale_ech)
{
    int k, i, sounding = 0;
    float vbuf[BL_BLOCK];
    for (k = 0; k < NVOICES; k++) {
        voice_t *v = &V[k];
        float *bl, *br, rs, es;
        if (!v->on)
            continue;
        sounding++;
        if (!voice_block(v, n, cx, vbuf)) {
            voice_free(v);
            continue;
        }
        {
            float lg = cx->lg[v->layer], pl = v->pl * lg, pr = v->pr * lg;
            if (v->type == BV_CHOIR) {           /* into the choir's formant banks (choir_run) */
                float *cl = choir_src[0], *cr = choir_src[1];
                for (i = 0; i < n; i++) {
                    cl[i] += vbuf[i] * pl;
                    cr[i] += vbuf[i] * pr;
                }
                choir_live = 1;
                continue;
            }
            bl = v->bus == BUS_PAD ? bus_pad[0] : v->bus == BUS_KEYS ? bus_keys[0] : bus_dry[0];
            br = v->bus == BUS_PAD ? bus_pad[1] : v->bus == BUS_KEYS ? bus_keys[1] : bus_dry[1];
            rs = v->bus == BUS_DRY ? v->rev * sendscale_rev * lg : 0.0f;   /* pads and keys send after their chorus */
            es = v->ech * sendscale_ech * lg;
            if (rs != 0.0f && es != 0.0f)        /* one pass: the pan and both sends */
                for (i = 0; i < n; i++) {
                    float x = vbuf[i];
                    bl[i] += x * pl;
                    br[i] += x * pr;
                    send_rev[i] += x * rs;
                    send_ech[i] += x * es;
                }
            else if (es != 0.0f)
                for (i = 0; i < n; i++) {
                    float x = vbuf[i];
                    bl[i] += x * pl;
                    br[i] += x * pr;
                    send_ech[i] += x * es;
                }
            else {
                for (i = 0; i < n; i++) {
                    float x = vbuf[i];
                    bl[i] += x * pl;
                    br[i] += x * pr;
                }
                if (rs != 0.0f)
                    for (i = 0; i < n; i++)
                        send_rev[i] += vbuf[i] * rs;
            }
        }
    }
    choir_run(n, cx->vowel, cx->tone, TYPE_ECH[BV_CHOIR] * sendscale_ech);
    return sounding;
}
