/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* The Bliss Box engine: one unit (the .inc files are parts of it).
 *
 * The music, after the research (research/01-history-and-technique.md):
 *   - a room: one key (Halpern's chakra keys C..B) and a mode, a tonic pedal under everything
 *   - chords that never resolve because they never leave: add9, 6/9, maj7#11, II/I, IV/I, vi/I over the pedal
 *   - Demby's crossfade: the pad changes chord one voice at a time, a few seconds apart
 *   - phrases with rubato and long rests (Rhodes, flute), arpeggio showers (harp, zither), bells
 *   - seasons every few minutes change who plays and the pad's colour; the harmony barely moves
 *   - all of it into tape echo, a long hall, and a cassette. */
#include "bliss.h"
#include "bl_util.h"

float bl_sin_tab[BL_SIN_N + 1];

void bl_tables_init(void)
{
    int i;
    for (i = 0; i <= BL_SIN_N; i++)
        bl_sin_tab[i] = fm_sin_turns((float)i / (float)BL_SIN_N);
}

/* ------------------------------------------------------------------------------------- biquads (RBJ) --- */
static void bq_set(bq_c *c, float b0, float b1, float b2, float a0, float a1, float a2)
{
    float i = 1.0f / a0;
    c->b0 = b0 * i;
    c->b1 = b1 * i;
    c->b2 = b2 * i;
    c->a1 = a1 * i;
    c->a2 = a2 * i;
}
void bq_peak(bq_c *c, float hz, float q, float db)
{
    float A = fm_exp2f(db * 0.0830482f), w = FM_TWO_PI * hz * BL_INV_SR, cs = fm_cosf(w), al = fm_sinf(w) / (2.0f * q);
    bq_set(c, 1.0f + al * A, -2.0f * cs, 1.0f - al * A, 1.0f + al / A, -2.0f * cs, 1.0f - al / A);
}
void bq_lowshelf(bq_c *c, float hz, float db)
{
    float A = fm_exp2f(db * 0.0830482f), w = FM_TWO_PI * hz * BL_INV_SR, cs = fm_cosf(w);
    float b = 2.0f * fm_sqrtf(A) * fm_sinf(w) * 0.707107f;
    bq_set(c, A * ((A + 1.0f) - (A - 1.0f) * cs + b), 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs),
           A * ((A + 1.0f) - (A - 1.0f) * cs - b), (A + 1.0f) + (A - 1.0f) * cs + b,
           -2.0f * ((A - 1.0f) + (A + 1.0f) * cs), (A + 1.0f) + (A - 1.0f) * cs - b);
}
void bq_highshelf(bq_c *c, float hz, float db)
{
    float A = fm_exp2f(db * 0.0830482f), w = FM_TWO_PI * hz * BL_INV_SR, cs = fm_cosf(w);
    float b = 2.0f * fm_sqrtf(A) * fm_sinf(w) * 0.707107f;
    bq_set(c, A * ((A + 1.0f) + (A - 1.0f) * cs + b), -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs),
           A * ((A + 1.0f) + (A - 1.0f) * cs - b), (A + 1.0f) - (A - 1.0f) * cs + b,
           2.0f * ((A - 1.0f) - (A + 1.0f) * cs), (A + 1.0f) - (A - 1.0f) * cs - b);
}
void bq_lowpass(bq_c *c, float hz, float q)
{
    float w = FM_TWO_PI * fm_minf(hz, 20000.0f) * BL_INV_SR, cs = fm_cosf(w), al = fm_sinf(w) / (2.0f * q);
    bq_set(c, (1.0f - cs) * 0.5f, 1.0f - cs, (1.0f - cs) * 0.5f, 1.0f + al, -2.0f * cs, 1.0f - al);
}
void bq_highpass(bq_c *c, float hz, float q)
{
    float w = FM_TWO_PI * hz * BL_INV_SR, cs = fm_cosf(w), al = fm_sinf(w) / (2.0f * q);
    bq_set(c, (1.0f + cs) * 0.5f, -(1.0f + cs), (1.0f + cs) * 0.5f, 1.0f + al, -2.0f * cs, 1.0f - al);
}

#include "bl_voices.inc"
#include "bl_fx.inc"

/* ------------------------------------------------------------------------------------------- state --- */
static bl_par_t P;
static bl_status_t S;
static uint32_t clk;                 /* samples since bl_init */
static bl_rng_t rng;                 /* the composer's dice */
static float now_tone, now_space, now_echo, now_shimmer, now_level, now_vowel, now_bed, now_melody, now_solo;
static float lfo[4];                 /* the slow LFOs' phases */
static float lfo_v[4];               /* and values, -1..1 */
static float muted = 1.0f, mute_tgt = 1.0f;

#define SEC(s) ((uint32_t)((s) * 44100.0f))
static inline int due(uint32_t t) { return (int32_t)(clk - t) >= 0; }

/* -------------------------------------------------------------------------------------------- names --- */
static const char *const VOICE_NAMES[BV_NTYPES] = {"RHODES", "CP-80", "FLUTE", "HARP", "ZITHER", "BELLS", "STRINGS", "CHOIR", "GLASS"};
static const char *const MODE_NAMES[BM_NMODES] = {"PENTA", "LYDIAN", "MAJOR", "DORIAN"};
static const char *const ROOM_NAMES[7] = {"ROOT", "SACRAL", "SOLAR", "HEART", "THROAT", "BROW", "CROWN"};
static const char *const ROOM_KEYS[7] = {"C", "D", "E", "F", "G", "A", "B"};
static const char *const SEASON_NAMES[BS_NSEASONS] = {"DAWN", "TEMPLE", "GARDEN", "STARS", "STILL", "RIVER"};
static const char *const PC_NAMES[12] = {"C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static const int8_t ROOM_PC[7] = {0, 2, 4, 5, 7, 9, 11};

const char *bl_voice_name(int v) { return v >= 0 && v < BV_NTYPES ? VOICE_NAMES[v] : "?"; }
const char *bl_mode_name(int m) { return m >= 0 && m < BM_NMODES ? MODE_NAMES[m] : "?"; }
const char *bl_room_name(int r) { return r >= 0 && r < 7 ? ROOM_NAMES[r] : "?"; }
const char *bl_room_key(int r) { return r >= 0 && r < 7 ? ROOM_KEYS[r] : "?"; }
const char *bl_season_name(int s) { return s >= 0 && s < BS_NSEASONS ? SEASON_NAMES[s] : "?"; }

/* ------------------------------------------------------------------------------------------- tuning --- */
static int tonic_pc;                 /* where the harmony is now */
static const float JUST[12] = {1.0f, 16.0f / 15.0f, 9.0f / 8.0f, 6.0f / 5.0f, 5.0f / 4.0f, 4.0f / 3.0f,
                               45.0f / 32.0f, 3.0f / 2.0f, 8.0f / 5.0f, 5.0f / 3.0f, 9.0f / 5.0f, 15.0f / 8.0f};

float bl_note_hz(int midi)
{
    float a = P.a432 ? 432.0f : 440.0f;
    if (P.just) {
        int rel = midi - 60 - tonic_pc, oct = 0, pc;
        float root = a * fm_exp2f((float)(60 + tonic_pc - 69) / 12.0f);
        while (rel < 0) {
            rel += 12;
            oct--;
        }
        oct += rel / 12;
        pc = rel % 12;
        return root * JUST[pc] * fm_exp2f((float)oct);
    }
    return a * fm_exp2f((float)(midi - 69) / 12.0f);
}

/* ---------------------------------------------------------------------------------------- the chords --- */
typedef struct { int8_t root; const char *q; int8_t t[5]; int8_t n; } chord_t;
/* the first HOME_N of each palette are the room's own chords: where it rests and where it ends */
static const chord_t PAL_PENTA[] = {
    {0, "add9", {0, 4, 7, 14}, 4},  {0, "6/9", {0, 4, 9, 14}, 4},     {0, "sus2", {0, 2, 7, 12}, 4},
    {9, "m7", {0, 3, 7, 10}, 4},    {7, "sus2", {0, 2, 7, 12}, 4},    {2, "7sus", {0, 5, 10}, 3},
    {4, "m7#5", {0, 3, 8, 10}, 4},
};
static const chord_t PAL_LYDIAN[] = {
    {0, "maj7#11", {0, 4, 7, 11, 18}, 5}, {0, "add9", {0, 4, 7, 14}, 4}, {2, "", {0, 4, 7}, 3},
    {2, "6", {0, 4, 7, 9}, 4},            {7, "maj7", {0, 4, 7, 11}, 4}, {11, "m7", {0, 3, 7, 10}, 4},
    {4, "m7", {0, 3, 7, 10}, 4},          {9, "m9", {0, 3, 7, 10, 14}, 5},
};
static const chord_t PAL_MAJOR[] = {
    {0, "add9", {0, 4, 7, 14}, 4}, {0, "6/9", {0, 4, 9, 14}, 4},  {0, "maj9", {0, 4, 7, 11, 14}, 5},
    {5, "", {0, 4, 7}, 3},         {5, "maj7", {0, 4, 7, 11}, 4}, {9, "m7", {0, 3, 7, 10}, 4},
    {7, "sus4", {0, 5, 7}, 3},     {2, "m9", {0, 3, 7, 10, 14}, 5},
};
static const chord_t PAL_DORIAN[] = {
    {0, "m9", {0, 3, 7, 10, 14}, 5}, {0, "m6", {0, 3, 7, 9}, 4},     {5, "", {0, 4, 7}, 3},
    {10, "add9", {0, 4, 7, 14}, 4},  {3, "maj7", {0, 4, 7, 11}, 4},  {7, "m7", {0, 3, 7, 10}, 4},
    {2, "m7", {0, 3, 7, 10}, 4},     {5, "6/9", {0, 4, 9, 14}, 4},
};
static const struct { const chord_t *c; int n, home; } PAL[BM_NMODES] = {
    {PAL_PENTA, 7, 3}, {PAL_LYDIAN, 8, 2}, {PAL_MAJOR, 8, 3}, {PAL_DORIAN, 8, 2}};
static const uint16_t MODE_SET[BM_NMODES] = {
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 9) | (1 << 11),
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11),
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 10)};

#define PAD_MAX 6
static struct {
    int mode, chord;                 /* index in PAL[mode] */
    uint16_t target;                 /* the chord's pitch classes (absolute) */
    int note[PAD_MAX];
    uint32_t id[PAD_MAX];
    int n;
    uint32_t pedal_id;
    int pedal_note, pedal_type;
    uint32_t next_morph, next_chord;
    int settled;
} H;

static inline int pcm(int x) { return ((x % 12) + 12) % 12; }
static const chord_t *chord_def(int mode, int k) { return &PAL[mode].c[k]; }
static uint16_t chord_set(int mode, int k, int tonic)
{
    const chord_t *c = chord_def(mode, k);
    uint16_t m = 0;
    int i;
    for (i = 0; i < c->n; i++)
        m |= (uint16_t)(1u << pcm(tonic + c->root + c->t[i]));
    return m;
}
static int popcount12(uint16_t m)
{
    int n = 0;
    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}
static int in_scale(int midi) { return (MODE_SET[H.mode] >> pcm(midi - tonic_pc)) & 1; }
static int in_chord(int midi) { return (H.target >> pcm(midi)) & 1; }

static void scopy(char *d, const char *s, int *k, int max)
{
    while (*s && *k < max - 1)
        d[(*k)++] = *s++;
    d[*k] = 0;
}
static void chord_name_update(void)
{
    const chord_t *c = chord_def(H.mode, H.chord);
    int k = 0;
    scopy(S.chord_name, PC_NAMES[pcm(tonic_pc + c->root)], &k, 16);
    scopy(S.chord_name, c->q, &k, 16);
    if (c->root) {
        scopy(S.chord_name, "/", &k, 16);
        scopy(S.chord_name, PC_NAMES[tonic_pc], &k, 16);
    }
}

static voice_t *voice_by_id(uint32_t id)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        if (V[i].on && V[i].id == id)
            return &V[i];
    return 0;
}

/* ------------------------------------------------------------------------------------------ the ring --- */
static void ev_note(int note, int type, float vel, int layer)
{
    int k = (int)(S.ev_count & 15);
    S.ev[k].note = (int8_t)note;
    S.ev[k].type = (uint8_t)type;
    S.ev[k].vel = (uint8_t)(fm_clampf(vel, 0.0f, 1.0f) * 127.0f);
    S.ev[k].layer = (uint8_t)layer;
    S.ev_count++;
}

/* ---------------------------------------------------------------------------------------- the seasons --- */
typedef struct {
    uint8_t pad, keys, harp, pedal;
    float k, h, b, f, pluck;         /* how likely each layer is to speak */
    float vowel, echo_s, tone;
} season_t;
static const season_t SEAS[BS_NSEASONS] = {
    /* DAWN   */ {BV_STRINGS, BV_RHODES, BV_HARP, BV_GLASS, 0.65f, 0.75f, 0.5f, 0.35f, 0.6f, 0.4f, 0.62f, 0.0f},
    /* TEMPLE */ {BV_CHOIR, BV_CP80, BV_ZITHER, BV_STRINGS, 0.45f, 0.35f, 1.0f, 0.25f, 0.3f, 0.2f, 0.74f, -0.1f},
    /* GARDEN */ {BV_STRINGS, BV_RHODES, BV_HARP, BV_GLASS, 0.9f, 0.5f, 0.35f, 0.75f, 0.8f, 0.9f, 0.47f, 0.08f},
    /* STARS  */ {BV_GLASS, BV_RHODES, BV_ZITHER, BV_GLASS, 0.5f, 1.0f, 0.85f, 0.2f, 0.5f, 1.2f, 0.82f, 0.12f},
    /* STILL  */ {BV_GLASS, BV_CP80, BV_HARP, BV_STRINGS, 0.25f, 0.2f, 0.3f, 0.15f, 0.25f, 0.6f, 0.7f, -0.15f},
    /* RIVER  */ {BV_CHOIR, BV_RHODES, BV_HARP, BV_GLASS, 0.7f, 0.65f, 0.4f, 0.9f, 0.7f, 1.6f, 0.55f, 0.0f},
};

/* --------------------------------------------------------------------------------------- the schedule --- */
typedef struct {
    uint32_t at;
    int8_t note;
    uint8_t type, layer, legato, used;
    float vel, pan, hold, att, rel;
} sev_t;
#define NSCHED 64
static sev_t SQ[NSCHED];

static struct {
    int on;                          /* 1 playing, 2 closing, 3 released (the tail of a timed piece) */
    uint32_t start;
    int season;
    uint32_t next_season, next_keys, next_shower, next_pluck, next_bell, next_flute;
    int stopping;
} G;

static struct {
    int len;                         /* 0 none */
    uint32_t start, off, end;
    uint32_t seed;
    float fade;
} ST;

static void sched(uint32_t at, int type, int layer, int note, float vel, float pan, float hold, float att, float rel, int legato)
{
    int i;
    if (ST.len && layer == L_STING) {   /* a sting's notes all end by its "off" */
        float left = (float)(int32_t)(ST.off - at) * BL_INV_SR;
        if (left <= 0.02f)
            return;
        if (hold > left)
            hold = left;
    }
    for (i = 0; i < NSCHED; i++)
        if (!SQ[i].used) {
            sev_t *e = &SQ[i];
            e->at = at;
            e->note = (int8_t)note;
            e->type = (uint8_t)type;
            e->layer = (uint8_t)layer;
            e->legato = (uint8_t)legato;
            e->vel = vel;
            e->pan = pan;
            e->hold = hold;
            e->att = att;
            e->rel = rel;
            e->used = 1;
            return;
        }
}

static void fire(const sev_t *e)
{
    if (e->legato) {
        int i;
        for (i = 0; i < NVOICES; i++) {
            voice_t *v = &V[i];
            if (v->on && v->gate && v->layer == e->layer && v->type == e->type) {
                v->f = bl_note_hz(e->note);
                v->note = e->note;
                v->hold = v->t + e->hold;
                ev_note(e->note, e->type, e->vel, e->layer);
                return;
            }
        }
    }
    voice_start(e->type, e->layer, e->note, bl_note_hz(e->note), e->vel, 1.0f, e->pan, e->att, e->rel, e->hold);
    ev_note(e->note, e->type, e->vel, e->layer);
}

static void sched_run(uint32_t until)
{
    int i;
    for (i = 0; i < NSCHED; i++)
        if (SQ[i].used && (int32_t)(until - SQ[i].at) >= 0) {
            SQ[i].used = 0;
            fire(&SQ[i]);
        }
}

static void sched_clear(int layer)          /* -1: all */
{
    int i;
    for (i = 0; i < NSCHED; i++)
        if (layer < 0 || SQ[i].layer == layer)
            SQ[i].used = 0;
}

/* ------------------------------------------------------------------------------------------- the pad --- */
static int reg_shift(void) { return (int)(lfo_v[3] * 3.5f + 0.5f); }

static void pad_start(int note, float att)
{
    int type = SEAS[G.season].pad;
    voice_t *v;
    float pan = fm_clampf((float)(note - 64) / 18.0f, -0.75f, 0.75f) + rng_range(&rng, -0.2f, 0.2f);
    if (ST.len)
        type = H.pedal_type;
    v = voice_start(type, L_PAD, note, bl_note_hz(note), 0.6f, 1.0f, pan, att * rng_range(&rng, 0.8f, 1.3f), 0.0f, -1.0f);
    H.note[H.n] = note;
    H.id[H.n] = v->id;
    H.n++;
}

static void pad_drop(int k, float rel)
{
    voice_t *v = voice_by_id(H.id[k]);
    if (v)
        voice_release(v, rel);
    H.n--;
    H.note[k] = H.note[H.n];
    H.id[k] = H.id[H.n];
}

/* the nearest note of pitch class pc to near, inside lo..hi */
static int nearest_pc(int pc, int near, int lo, int hi)
{
    int best = -1, d, bd = 99, m;
    for (m = lo; m <= hi; m++)
        if (pcm(m) == pc) {
            d = m > near ? m - near : near - m;
            if (d < bd) {
                bd = d;
                best = m;
            }
        }
    return best;
}

/* where a new pad note should go: away from the others, near the middle */
static int pad_place(int pc)
{
    int lo = 50 + reg_shift(), hi = 76 + reg_shift(), m, best = -1;
    float bs = -1e9f;
    for (m = lo; m <= hi; m++) {
        int k, dmin = 99;
        float sc;
        if (pcm(m) != pc)
            continue;
        for (k = 0; k < H.n; k++) {
            int d = m > H.note[k] ? m - H.note[k] : H.note[k] - m;
            if (d < dmin)
                dmin = d;
        }
        if (dmin < 3)
            continue;                                  /* no seconds inside the pad, no unisons */
        sc = (float)(dmin > 7 ? 7 : dmin) - 0.12f * (float)(m > 63 ? m - 63 : 63 - m);
        if (m < 55 && dmin < 7)
            sc -= 4.0f;                                /* keep the bottom open */
        if (sc > bs) {
            bs = sc;
            best = m;
        }
    }
    return best;
}

/* one step of the crossfade; 0 when the pad is the chord */
static int pad_morph(float att, float rel)
{
    int k, extra = -1, missing = -1, wrong_type = -1, pc;
    uint16_t held = 0;
    for (k = 0; k < H.n; k++) {                        /* a stolen voice leaves the pad */
        voice_t *v = voice_by_id(H.id[k]);
        if (!v || !v->gate) {
            H.n--;
            H.note[k] = H.note[H.n];
            H.id[k] = H.id[H.n];
            k--;
        }
    }
    for (k = 0; k < H.n; k++) {
        voice_t *v = voice_by_id(H.id[k]);
        held |= (uint16_t)(1u << pcm(H.note[k]));
        if (!in_chord(H.note[k]) && (extra < 0 || rng_chance(&rng, 0.5f)))
            extra = k;
        if (v && v->type != SEAS[G.season].pad && !ST.len)
            wrong_type = k;
    }
    for (pc = 0; pc < 12; pc++) {
        int p = pcm(pc + rng_int(&rng, 12));
        if (((H.target >> p) & 1) && !((held >> p) & 1)) {
            missing = p;
            break;
        }
    }
    if (missing >= 0 && extra >= 0) {                  /* move a voice to the nearest missing note */
        int from = H.note[extra], to = nearest_pc(missing, from, 48 + reg_shift(), 79 + reg_shift());
        pad_drop(extra, rel);
        if (to >= 0)
            pad_start(to, att);
        return 1;
    }
    if (missing >= 0 && H.n < PAD_MAX) {
        int to = pad_place(missing);
        if (to < 0)
            to = nearest_pc(missing, 62 + reg_shift(), 50, 80);
        pad_start(to, att);
        return 1;
    }
    if (extra >= 0) {
        pad_drop(extra, rel);
        return 1;
    }
    if (wrong_type >= 0) {                             /* the season's colour, one voice at a time */
        int note = H.note[wrong_type];
        pad_drop(wrong_type, rel);
        pad_start(note, att);
        return 1;
    }
    return 0;
}

static void pedal_set(int on)
{
    voice_t *v = H.pedal_id ? voice_by_id(H.pedal_id) : 0;
    int note = 36 + tonic_pc, type = ST.len ? H.pedal_type : SEAS[G.season].pedal;
    if (note > 43)
        note -= 12;
    if (v && on && v->gate && v->note == note && v->type == type)
        return;
    if (v)
        voice_release(v, 9.0f);
    H.pedal_id = 0;
    if (on) {
        v = voice_start(type, L_PEDAL, note, bl_note_hz(note), 0.5f, 1.0f, 0.0f, 6.0f, 0.0f, -1.0f);
        H.pedal_id = v->id;
        H.pedal_note = note;
    }
}

static void chord_set_to(int k)
{
    H.chord = k;
    H.target = chord_set(H.mode, k, tonic_pc);
    H.settled = 0;
    chord_name_update();
}

/* the next chord: a Markov step that keeps at least two notes, leans home */
static void chord_next(void)
{
    int k, n = PAL[H.mode].n, pick = 0;
    float w[8], tot = 0.0f, u;
    for (k = 0; k < n; k++) {
        uint16_t m = chord_set(H.mode, k, tonic_pc);
        int shared = popcount12((uint16_t)(m & H.target));
        w[k] = k == H.chord ? 0.0f : shared >= 3 ? 3.0f : shared == 2 ? 1.6f : 0.12f;
        if (k < PAL[H.mode].home)
            w[k] *= 1.2f + 1.5f * (1.0f - P.drift);
        tot += w[k];
    }
    u = rng_unit(&rng) * tot;
    for (k = 0; k < n; k++) {
        u -= w[k];
        if (u <= 0.0f) {
            pick = k;
            break;
        }
    }
    chord_set_to(pick);
}

/* drift: a common-tone step to the room a fifth away, and back */
static void maybe_drift(void)
{
    int home = ROOM_PC[P.room], k;
    if (tonic_pc != home) {
        if (rng_chance(&rng, 0.55f)) {
            tonic_pc = home;
            chord_set_to(0);
            pedal_set(1);
        }
        return;
    }
    if (!rng_chance(&rng, P.drift * 0.35f))
        return;
    {
        int cand[2] = {pcm(home + 7), pcm(home + 5)}, c = cand[rng_int(&rng, 2)];
        for (k = 0; k < 7; k++)
            if (ROOM_PC[k] == c) {
                tonic_pc = c;
                chord_set_to(rng_int(&rng, PAL[H.mode].home));
                pedal_set(1);
                return;
            }
    }
}

/* ------------------------------------------------------------------------------------- the melodies --- */
static int scale_list(int *out, int lo, int hi)
{
    int m, n = 0;
    for (m = lo; m <= hi && n < 32; m++)
        if (in_scale(m))
            out[n++] = m;
    return n;
}
static int chord_list(int *out, int lo, int hi)
{
    int m, n = 0;
    for (m = lo; m <= hi && n < 32; m++)
        if (in_chord(m))
            out[n++] = m;
    return n;
}

/* rests get longer when the activity is low and when somebody is talking */
static float rest_scale(void) { return (1.75f - 1.3f * P.activity) * (P.talk ? 1.43f : 1.0f); }

static void phrase_keys(uint32_t t0, int type, int layer, float vel_s)
{
    int S_[32], n = scale_list(S_, 55, 82), idx, k, len, dir;
    float t = 0.0f;
    if (n < 4)
        return;
    if (rng_chance(&rng, 0.25f)) {                      /* a rolled chord */
        int C[32], m = chord_list(C, 55, 74), j, s = m > 4 ? rng_int(&rng, m - 3) : 0;
        for (j = 0; j < 4 && s + j < m; j++) {
            sched(t0 + SEC(t), type, layer, C[s + j], (0.3f + 0.12f * rng_unit(&rng)) * vel_s,
                  rng_range(&rng, -0.35f, 0.35f), rng_range(&rng, 3.0f, 5.0f), 0.0f, 0.0f, 0);
            t += rng_range(&rng, 0.035f, 0.09f);
        }
        return;
    }
    len = 3 + rng_int(&rng, 5);
    idx = 0;
    {
        int target = 64 + reg_shift() + rng_int(&rng, 7) - 2, bd = 99;
        for (k = 0; k < n; k++) {
            int d = S_[k] > target ? S_[k] - target : target - S_[k];
            if (in_chord(S_[k]) && d < bd) {
                bd = d;
                idx = k;
            }
        }
    }
    dir = rng_chance(&rng, 0.55f) ? 1 : -1;
    for (k = 0; k < len; k++) {
        int note = S_[idx];
        float vel = (0.32f + 0.25f * rng_unit(&rng) + (k == 0 ? 0.08f : 0.0f)) * vel_s, hold;
        if (k == len - 1) {                            /* end on the 3rd, the 9th or the 5th */
            const chord_t *c = chord_def(H.mode, H.chord);
            int pick = c->n > 3 && rng_chance(&rng, 0.4f) ? 3 : 1 + rng_int(&rng, 2);
            int pc = pcm(tonic_pc + c->root + c->t[pick < c->n ? pick : 1]);
            int to = nearest_pc(pc, note, 55, 82);
            if (to >= 0)
                note = to;
            hold = rng_range(&rng, 2.5f, 4.5f);
        } else
            hold = rng_range(&rng, 0.7f, 2.2f);
        sched(t0 + SEC(t), type, layer, note, vel, rng_range(&rng, -0.4f, 0.4f), hold, 0.0f, 0.0f, 0);
        t += rng_around(&rng, 0.75f, 0.6f);
        if (rng_chance(&rng, 0.25f))
            dir = -dir;
        idx += dir * (rng_chance(&rng, 0.7f) ? 1 : 2);
        if (idx < 0) {
            idx = 1;
            dir = 1;
        }
        if (idx >= n) {
            idx = n - 2;
            dir = -1;
        }
    }
}

/* a shower: an arpeggio across two octaves, the harp's glissando or the zither's strum */
static void phrase_shower(uint32_t t0, int type, int layer, float vel_s, int len)
{
    int C[32], n = H.mode == BM_PENTA ? scale_list(C, 55, 91) : chord_list(C, 55, 91), s, k, updown;
    float t = 0.0f, ioi = rng_range(&rng, 0.07f, 0.14f);
    if (n < 4)
        return;
    if (len > n)
        len = n;
    s = rng_int(&rng, n - len + 1);
    if (s > 3)
        s = rng_int(&rng, 4);
    updown = rng_chance(&rng, 0.3f);
    for (k = 0; k < len; k++) {
        int j = updown && k > len / 2 ? s + len - k : s + k;
        float x = (float)k / (float)(len - 1), vel = (0.22f + 0.28f * sin1(x * 0.5f)) * vel_s;
        if (j >= n)
            j = n - 1;
        sched(t0 + SEC(t), type, layer, C[j], vel, -0.5f + x, 7.0f, 0.0f, 0.0f, 0);
        t += ioi * rng_around(&rng, 1.0f, 0.15f);
        ioi *= 1.0f + (x > 0.6f ? 0.06f : -0.01f);     /* a breath at the top */
    }
}

static void phrase_bell(uint32_t t0, int layer, float vel_s)
{
    int C[32], n = chord_list(C, 79, 93), a;
    if (n < 1)
        return;
    a = rng_int(&rng, n);
    sched(t0, BV_BELLS, layer, C[a], (0.25f + 0.2f * rng_unit(&rng)) * vel_s, rng_range(&rng, -0.6f, 0.6f), 6.0f, 0, 0, 0);
    if (rng_chance(&rng, 0.4f) && n > 1) {
        int b = a > 0 && rng_chance(&rng, 0.5f) ? a - 1 : (a + 1 < n ? a + 1 : a - 1);
        sched(t0 + SEC(rng_range(&rng, 0.5f, 1.4f)), BV_BELLS, layer, C[b], (0.2f + 0.15f * rng_unit(&rng)) * vel_s,
              rng_range(&rng, -0.6f, 0.6f), 6.0f, 0, 0, 0);
    }
}

static void phrase_flute(uint32_t t0, int layer, float vel_s)
{
    int Sc[32], n = scale_list(Sc, 65, 84), C[32], m = chord_list(C, 67, 81), k, idx, tones = 1 + rng_int(&rng, 3), note;
    float t = 0.0f;
    if (n < 4 || m < 1)
        return;
    note = C[rng_int(&rng, m)];
    for (idx = 0; idx < n && Sc[idx] != note; idx++)
        ;
    for (k = 0; k < tones; k++) {
        float len = rng_range(&rng, 2.5f, 7.0f);
        if (k == 0)
            sched(t0, BV_FLUTE, layer, note, 0.6f * vel_s, rng_range(&rng, -0.25f, 0.25f), len, rng_range(&rng, 0.8f, 2.0f), 1.6f, 0);
        else
            sched(t0 + SEC(t), BV_FLUTE, layer, note, 0.6f * vel_s, 0.0f, len, 0, 0, 1);
        t += len;
        idx += (rng_chance(&rng, 0.5f) ? 1 : -1) * (1 + rng_int(&rng, 2));
        if (idx < 0)
            idx = 1;
        if (idx >= n)
            idx = n - 2;
        note = Sc[idx];
        if (k == tones - 2) {                          /* the last tone is a chord tone */
            int to = nearest_pc(pcm(note), note, 65, 84), j, bd = 99;
            for (j = 0; j < m; j++) {
                int d = C[j] > note ? C[j] - note : note - C[j];
                if (d < bd) {
                    bd = d;
                    to = C[j];
                }
            }
            note = to;
        }
    }
}

/* ------------------------------------------------------------------------------------------ the grove --- */
static float glen(void) { return (float)P.minutes * 60.0f; }

static void grove_start(void)
{
    int k;
    G.on = 1;
    G.start = clk;
    G.stopping = 0;
    H.mode = P.mode;
    tonic_pc = ROOM_PC[P.room];
    G.season = rng_chance(&rng, 0.5f) ? BS_DAWN : (rng_chance(&rng, 0.5f) ? BS_STARS : BS_TEMPLE);
    G.next_season = clk + SEC(rng_range(&rng, 150.0f, 330.0f));
    chord_set_to(rng_int(&rng, PAL[H.mode].home));
    pedal_set(1);
    for (k = 0; k < 4; k++)                            /* the first chord comes in together, staggered */
        pad_morph(rng_range(&rng, 4.0f, 7.0f), 6.0f);
    H.next_morph = clk + SEC(2.0f);
    H.next_chord = clk + SEC(rng_range(&rng, 30.0f, 60.0f));
    G.next_keys = clk + SEC(rng_range(&rng, 9.0f, 16.0f));
    G.next_pluck = clk + SEC(rng_range(&rng, 14.0f, 22.0f));
    G.next_bell = clk + SEC(rng_range(&rng, 20.0f, 32.0f));
    G.next_shower = clk + SEC(rng_range(&rng, 30.0f, 60.0f));
    G.next_flute = clk + SEC(rng_range(&rng, 45.0f, 80.0f));
}

static void release_all(float rel, int keep_solo)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        if (V[i].on && !(keep_solo && V[i].layer == L_SOLO))
            voice_release(&V[i], rel);
    H.n = 0;
    H.pedal_id = 0;
}

static void grove_stop(float rel)
{
    int i;
    G.on = 0;
    for (i = 0; i < L_SOLO; i++)
        sched_clear(i);
    release_all(rel, 1);
}

static void grove_step(void)
{
    float el = (float)(clk - G.start) * BL_INV_SR, L = glen();
    const season_t *se = &SEAS[G.season];
    float rs = rest_scale();
    S.seconds = (uint32_t)el;
    if (G.on == 3) {                                   /* the tail of a timed piece */
        if (el >= L)
            G.on = 0;
        return;
    }
    /* the ending of a timed piece: home, add9, everything lets go 25 s before the end */
    if (L > 90.0f && el >= L - 60.0f && G.on == 1) {
        G.on = 2;
        if (tonic_pc != ROOM_PC[P.room]) {
            tonic_pc = ROOM_PC[P.room];
            pedal_set(1);
        }
        chord_set_to(0);
        H.next_morph = clk;
        phrase_shower(clk + SEC(14.0f), se->harp, L_HARP, 0.8f, 10);
        phrase_bell(clk + SEC(26.0f), L_BELL, 0.9f);
    }
    if (G.on == 2 && el >= L - 25.0f) {
        G.on = 3;
        for (int i = 0; i < L_SOLO; i++)
            sched_clear(i);
        release_all(16.0f, 1);
        return;
    }
    if (G.on == 1 && due(G.next_season)) {
        int s;
        do
            s = rng_int(&rng, BS_NSEASONS);
        while (s == G.season || (s == BS_STILL && rng_chance(&rng, 0.5f)));
        G.season = s;
        se = &SEAS[s];
        G.next_season = clk + SEC(rng_range(&rng, 150.0f, 330.0f));
        H.next_morph = clk + SEC(1.0f);
        H.settled = 0;
        pedal_set(1);
    }
    /* the pad: one voice at a time while it moves, then a dwell */
    if (due(H.next_morph)) {
        int moving = pad_morph(G.on == 2 ? 3.0f : rng_range(&rng, 3.0f, 6.0f), rng_range(&rng, 5.0f, 8.0f));
        H.next_morph = clk + SEC(moving ? rng_range(&rng, 4.0f, 10.0f) : 2.0f);
        if (!moving && !H.settled) {
            H.settled = 1;
            H.next_chord = clk + SEC(rng_range(&rng, 20.0f, 70.0f) * (1.4f - 0.6f * P.activity));
        }
    }
    if (G.on == 1 && H.settled && due(H.next_chord)) {
        if (rng_chance(&rng, 0.3f))
            maybe_drift();
        if (H.settled)
            chord_next();
        H.next_morph = clk;
    }
    if (G.on != 1)
        return;
    /* the melodic layers */
    if (due(G.next_keys)) {
        if (rng_chance(&rng, se->k))
            phrase_keys(clk, se->keys, L_KEYS, 1.0f);
        G.next_keys = clk + SEC(rng_range(&rng, 5.0f, 22.0f) * rs + 3.0f);
    }
    if (due(G.next_shower)) {
        if (rng_chance(&rng, se->h))
            phrase_shower(clk, se->harp, L_HARP, 0.9f, 6 + rng_int(&rng, 9));
        G.next_shower = clk + SEC(rng_range(&rng, 45.0f, 150.0f) * rs);
    }
    if (due(G.next_pluck)) {
        if (rng_chance(&rng, se->pluck)) {
            int C[32], n = chord_list(C, 64, 84);
            if (n)
                sched(clk, se->harp, L_PLUCK, C[rng_int(&rng, n)], 0.3f + 0.2f * rng_unit(&rng),
                      rng_range(&rng, -0.7f, 0.7f), 6.0f, 0, 0, 0);
        }
        G.next_pluck = clk + SEC(rng_range(&rng, 8.0f, 25.0f) * rs);
    }
    if (due(G.next_bell)) {
        if (rng_chance(&rng, se->b))
            phrase_bell(clk, L_BELL, 1.0f);
        G.next_bell = clk + SEC(rng_range(&rng, 14.0f, 45.0f) * rs);
    }
    if (due(G.next_flute)) {
        if (rng_chance(&rng, se->f))
            phrase_flute(clk, L_FLUTE, 1.0f);
        G.next_flute = clk + SEC(rng_range(&rng, 25.0f, 90.0f) * rs);
    }
}

/* ------------------------------------------------------------------------------------------ the sting --- */
enum { GS_SHIMMER, GS_CHIME, GS_BLOOM, GS_BREATH, GS_GROVE };

static void all_reseed(uint32_t seed)
{
    rng_seed(&rng, seed);
    rng_seed(&noise_rng, seed ^ 0xA5A5A5u);
    rng_seed(&TP.rng, seed ^ 0x5A5A5Au);
    lfo[0] = 0.1f;
    lfo[1] = 0.4f;
    lfo[2] = 0.7f;
    lfo[3] = 0.25f;
}

static void hard_stop(void)
{
    int i;
    for (i = 0; i < NVOICES; i++)
        voice_free(&V[i]);
    for (i = 0; i < NSTRINGS; i++)
        KS[i].busy = 0;
    sched_clear(-1);
    H.n = 0;
    H.pedal_id = 0;
    G.on = 0;
}

static void sting_start(int secs, uint32_t seed)
{
    float L = (float)secs, off, t;
    int g, home;
    uint32_t t0;
    hard_stop();
    fx_clear();
    all_reseed(seed);
    ST.len = secs;
    ST.seed = seed;
    ST.start = clk;
    off = L * rng_range(&rng, 0.55f, 0.7f);
    ST.off = clk + SEC(off);
    ST.end = clk + SEC(L);
    ST.fade = fm_clampf(L * 0.3f, 0.8f, 6.0f);
    muted = mute_tgt = 1.0f;
    /* settle the smoothed controls now: the same seed, the same sound */
    now_tone = P.tone;
    now_space = P.space;
    now_echo = P.echo;
    now_shimmer = P.shimmer;
    now_level = P.level;
    now_bed = P.bed;
    now_melody = P.melody;
    EC.time = fm_clampf(L * 0.12f, 0.25f, 0.62f);
    H.mode = P.mode;
    tonic_pc = ROOM_PC[P.room];
    home = rng_int(&rng, PAL[H.mode].home);
    chord_set_to(home);
    t0 = clk;
    if (secs <= 3)
        g = rng_chance(&rng, 0.5f) ? GS_SHIMMER : GS_CHIME;
    else if (secs <= 5)
        g = rng_int(&rng, 3) == 0 ? GS_CHIME : (rng_chance(&rng, 0.5f) ? GS_SHIMMER : GS_BLOOM);
    else if (secs <= 10)
        g = rng_int(&rng, 3) == 0 ? GS_BREATH : (rng_chance(&rng, 0.5f) ? GS_BLOOM : GS_SHIMMER);
    else
        g = rng_chance(&rng, 0.25f) ? GS_BREATH : GS_GROVE;
    G.season = (int)(seed % BS_NSEASONS);
    H.pedal_type = g == GS_BLOOM || g == GS_GROVE ? (rng_chance(&rng, 0.5f) ? BV_STRINGS : BV_CHOIR) : BV_GLASS;
    /* the pad: all at once, an attack that fits */
    {
        float att = fm_clampf(L * 0.16f, 0.25f, 4.0f) * (g == GS_BLOOM ? 1.6f : 1.0f);
        int k;
        for (k = 0; k < 4; k++)
            pad_morph(att, 0.0f);
    }
    if (g == GS_GROVE || g == GS_BREATH)
        pedal_set(1);
    t = 0.05f;
    switch (g) {
    case GS_SHIMMER:
        phrase_shower(t0 + SEC(t), BV_ZITHER, L_STING, 1.0f, (int)fm_clampf(4.0f + L, 6.0f, 13.0f));
        sched(t0 + SEC(fm_minf(off * 0.6f, 1.2f + L * 0.08f)), BV_BELLS, L_STING, 84 + tonic_pc, 0.35f, 0.3f, 6.0f, 0, 0, 0);
        break;
    case GS_CHIME: {
        int notes[3] = {79 + tonic_pc, 86 + tonic_pc, 88 + tonic_pc}, k;
        for (k = 0; k < 3; k++) {
            sched(t0 + SEC(t), BV_BELLS, L_STING, notes[k] - (notes[k] > 93 ? 12 : 0), 0.45f - 0.08f * (float)k,
                  -0.4f + 0.4f * (float)k, 6.0f, 0, 0, 0);
            t += rng_range(&rng, 0.12f, 0.3f) * (L > 3.0f ? 1.4f : 1.0f);
        }
        sched(t0, BV_HARP, L_STING, 48 + tonic_pc, 0.5f, 0.0f, 6.0f, 0, 0, 0);
        break;
    }
    case GS_BLOOM:
        phrase_keys(t0 + SEC(L * 0.18f), rng_chance(&rng, 0.5f) ? BV_RHODES : BV_CP80, L_STING, 0.9f);
        phrase_shower(t0 + SEC(L * 0.28f), BV_HARP, L_STING, 0.8f, 8);
        phrase_bell(t0 + SEC(L * 0.4f), L_STING, 0.9f);
        break;
    case GS_BREATH:
        phrase_flute(t0 + SEC(L * 0.08f), L_STING, 1.0f);
        phrase_bell(t0 + SEC(off * 0.8f), L_STING, 0.8f);
        break;
    case GS_GROVE:
        phrase_keys(t0 + SEC(2.0f), BV_RHODES, L_STING, 1.0f);
        phrase_bell(t0 + SEC(6.0f), L_STING, 1.0f);
        phrase_shower(t0 + SEC(8.5f), BV_HARP, L_STING, 0.85f, 11);
        phrase_keys(t0 + SEC(12.0f), BV_RHODES, L_STING, 0.9f);
        phrase_flute(t0 + SEC(10.0f), L_STING, 0.85f);
        phrase_bell(t0 + SEC(16.5f), L_STING, 0.8f);
        break;
    }
}

static void sting_step(void)
{
    if (!ST.len)
        return;
    if (due(ST.off) && (int32_t)(clk - ST.off) < BL_BLOCK) {
        float tail = (float)(int32_t)(ST.end - ST.off) * BL_INV_SR;
        int i;
        sched_clear(-1);
        for (i = 0; i < NVOICES; i++)
            if (V[i].on && V[i].layer != L_SOLO)
                voice_release(&V[i], tail * 0.55f);
        H.n = 0;
        H.pedal_id = 0;
    }
    if (due(ST.end)) {
        hard_stop();
        ST.len = 0;
        mute_tgt = muted = 0.0f;                      /* silence until the next thing you do */
    }
}

/* --------------------------------------------------------------------------------------- the events --- */
enum { E_NOTE, E_GROVE, E_STING, E_PANIC };
typedef struct { uint8_t type; int8_t a; uint16_t b; uint32_t c; float f; } bev_t;
#define NEV 64
static bev_t EQ[NEV];
static volatile uint32_t eq_w, eq_r;

static void post(int type, int a, int b, uint32_t c, float f)
{
    uint32_t w = eq_w;
    if (w - eq_r >= NEV)
        return;
    EQ[w & (NEV - 1)].type = (uint8_t)type;
    EQ[w & (NEV - 1)].a = (int8_t)a;
    EQ[w & (NEV - 1)].b = (uint16_t)b;
    EQ[w & (NEV - 1)].c = c;
    EQ[w & (NEV - 1)].f = f;
    BL_BARRIER();
    eq_w = w + 1;
}
void bl_post_note(int midi, float vel) { post(E_NOTE, midi, 0, 0, vel); }
void bl_post_grove(int on) { post(E_GROVE, on, 0, 0, 0.0f); }
void bl_post_sting(int seconds, uint32_t seed) { post(E_STING, 0, (uint16_t)seconds, seed, 0.0f); }
void bl_post_panic(void) { post(E_PANIC, 0, 0, 0, 0.0f); }

/* The audio interrupt ran late (the platform's app_overload, from the interrupt itself, after a render): the next
 * render lets go of the voice that costs the least to lose. A word the interrupt writes and the render reads, both on
 * the one core, so no queue. */
static volatile uint32_t shed_req;
void bl_shed(void) { shed_req++; }
static void shed_run(void)
{
    static uint32_t done;
    int i, cost = 0;
    for (i = 0; i < NVOICES; i++)
        if (V[i].on && V[i].hold != -2.0f)       /* voices already shed (hold -2) are on their way out */
            cost += voice_cost(&V[i]);
    /* more voices than the CPU can render: the quietest let go, a quiet one at once, else in 20 ms.
     * And on an overrun (app_overload, bl_shed) one more, held or not, save a played solo key. */
    while (cost > BL_VOICE_BUDGET || shed_req != done) {
        int best = -1, forced = shed_req != done;
        float lowest = 1e9f;
        for (i = 0; i < NVOICES; i++) {
            float lv;
            if (!V[i].on || V[i].hold == -2.0f || (V[i].layer == L_SOLO && V[i].gate) || (V[i].gate && !forced))
                continue;
            lv = V[i].env * V[i].dec * (V[i].gate ? 4.0f : 1.0f) * V[i].gain;
            if (lv < lowest) {
                lowest = lv;
                best = i;
            }
        }
        done = shed_req;
        if (best < 0)
            break;
        cost -= voice_cost(&V[best]);
        voice_release(&V[best], 0.02f);
        V[best].hold = -2.0f;     /* marked: shed */
        if (V[best].env * V[best].dec < 0.003f)
            voice_free(&V[best]);
    }
}

static void solo_note(int midi, float vel)
{
    int i, t = P.solo;
    if (vel <= 0.0f) {
        for (i = 0; i < NVOICES; i++)
            if (V[i].on && V[i].gate && V[i].layer == L_SOLO && V[i].note == midi)
                voice_release(&V[i], is_pad(V[i].type) ? 2.5f : 0.0f);
        return;
    }
    if (ST.len == 0 && muted < 1.0f)
        mute_tgt = 1.0f;
    if (t == BV_FLUTE)                                 /* the flute is one breath: a new key glides */
        for (i = 0; i < NVOICES; i++)
            if (V[i].on && V[i].gate && V[i].layer == L_SOLO && V[i].type == BV_FLUTE) {
                V[i].f = bl_note_hz(midi);
                V[i].note = (int8_t)midi;
                ev_note(midi, t, vel, L_SOLO);
                return;
            }
    voice_start(t, L_SOLO, midi, bl_note_hz(midi), vel, 1.0f, fm_clampf((float)(midi - 66) / 20.0f, -0.5f, 0.5f),
                is_pad(t) ? 0.3f : t == BV_FLUTE ? 0.12f : 0.0f, is_pad(t) ? 2.5f : 0.0f, -1.0f);
    ev_note(midi, t, vel, L_SOLO);
}

static void events_run(void)
{
    while (eq_r != eq_w) {
        bev_t e = EQ[eq_r & (NEV - 1)];
        BL_BARRIER();
        eq_r++;
        switch (e.type) {
        case E_NOTE:
            solo_note(e.a, e.f);
            break;
        case E_GROVE:
            if (e.a && !G.on) {
                if (ST.len) {
                    hard_stop();
                    ST.len = 0;
                }
                muted = 1.0f;
                mute_tgt = 1.0f;
                grove_start();
            } else if (!e.a && G.on)
                grove_stop(9.0f);
            break;
        case E_STING:
            sting_start(e.b < 1 ? 1 : e.b > 60 ? 60 : e.b, e.c);
            break;
        case E_PANIC:
            hard_stop();
            ST.len = 0;
            fx_clear();
            break;
        }
    }
}

/* ------------------------------------------------------------------------------------------ render --- */
static float pad_l[BL_BLOCK], pad_r[BL_BLOCK], ech_l[BL_BLOCK], ech_r[BL_BLOCK], rev_l[BL_BLOCK], rev_r[BL_BLOCK];
static float meter_hold[2];

static inline float smooth(float now, float tgt, float k) { return now + k * (tgt - now); }

void bl_render(float *l, float *r, int n)
{
    int i, k, sounding;
    voice_ctx_t cx;
    float rt, rev_ret, ech_ret, fbk, sh, peak_l = 0.0f, peak_r = 0.0f, e = 0.0f, tone, ech_time;
    const season_t *se;
    if (n > BL_BLOCK)
        n = BL_BLOCK;
    events_run();
    shed_run();
    se = &SEAS[G.season];
    /* the slow LFOs: 47, 73, 131 and 211 seconds */
    {
        static const float per[4] = {47.0f, 73.0f, 131.0f, 211.0f};
        for (k = 0; k < 4; k++) {
            lfo[k] = wrap1(lfo[k] + (float)n / (per[k] * 44100.0f));
            lfo_v[k] = sin1(lfo[k]);
        }
    }
    now_tone = smooth(now_tone, fm_clampf(P.tone + (G.on ? se->tone : 0.0f) + 0.12f * lfo_v[0], 0.0f, 1.0f), 0.01f);
    now_space = smooth(now_space, P.space, 0.02f);
    now_echo = smooth(now_echo, P.echo, 0.02f);
    now_shimmer = smooth(now_shimmer, fm_clampf(P.shimmer * (0.85f + 0.15f * lfo_v[1]), 0.0f, 1.0f), 0.02f);
    now_level = smooth(now_level, P.level, 0.05f);
    now_vowel = smooth(now_vowel, fm_clampf(se->vowel + 0.35f * lfo_v[2], 0.0f, 2.0f), 0.002f);
    now_bed = smooth(now_bed, P.bed, 0.03f);
    now_melody = smooth(now_melody, P.melody, 0.03f);
    now_solo = smooth(now_solo, P.solo_level, 0.05f);
    tone = now_tone;

    if (ST.len)
        sting_step();
    else if (G.on)
        grove_step();
    sched_run(clk + (uint32_t)n);

    for (i = 0; i < n; i++) {
        bus_pad[0][i] = bus_pad[1][i] = bus_keys[0][i] = bus_keys[1][i] = bus_dry[0][i] = bus_dry[1][i] = 0.0f;
        send_rev[i] = send_ech[i] = 0.0f;
    }
    cx.tone = tone;
    cx.vowel = now_vowel;
    for (k = 0; k < NLAYERS; k++)
        cx.lg[k] = 1.0f;
    cx.lg[L_KEYS] = cx.lg[L_HARP] = cx.lg[L_PLUCK] = cx.lg[L_BELL] = cx.lg[L_FLUTE] = now_melody * (P.talk ? 0.85f : 1.0f);
    cx.lg[L_PEDAL] = 0.55f;
    cx.lg[L_SOLO] = now_solo;
    sh = 0.15f + 0.85f * now_shimmer;
    sounding = voices_render(n, &cx, 1.0f, 0.2f + 0.8f * now_echo);

    /* the pads through the ensemble, the keys through a gentler one */
    for (i = 0; i < n; i++) {
        bus_pad[0][i] *= now_bed;
        bus_pad[1][i] *= now_bed;
    }
    chorus_run(&CHO[0], bus_pad[0], bus_pad[1], n, sh, 0.6f + 0.4f * now_shimmer);
    chorus_run(&CHO[1], bus_keys[0], bus_keys[1], n, sh, 0.35f + 0.5f * now_shimmer);

    /* echo and hall */
    ech_time = ST.len ? EC.time : se->echo_s;
    fbk = (0.28f + 0.34f * now_echo) * (ST.len ? 0.7f : 1.0f);
    echo_run(send_ech, ech_l, ech_r, n, ech_time, fbk, tone);
    rt = 2.5f + 9.5f * now_space * now_space;
    if (ST.len) {
        float tail = (float)(int32_t)(ST.end - ST.off) * BL_INV_SR;
        rt = fm_clampf(tail * 0.9f, 1.0f, rt);
    }
    reverb_set(rt, 2600.0f + 5200.0f * tone);
    ech_ret = 0.25f + 0.5f * now_echo;
    for (i = 0; i < n; i++)
        send_rev[i] += (bus_pad[0][i] + bus_pad[1][i]) * 0.3f + (bus_keys[0][i] + bus_keys[1][i]) * 0.17f +
                       (ech_l[i] + ech_r[i]) * 0.25f * ech_ret + 1e-18f;
    reverb_run(send_rev, rev_l, rev_r, n, 0.012f + 0.05f * now_space);
    rev_ret = (0.22f + 0.5f * now_space) * 0.5f;

    for (i = 0; i < n; i++) {
        l[i] = bus_pad[0][i] + bus_keys[0][i] + bus_dry[0][i] + ech_l[i] * ech_ret + rev_l[i] * rev_ret;
        r[i] = bus_pad[1][i] + bus_keys[1][i] + bus_dry[1][i] + ech_r[i] * ech_ret + rev_r[i] * rev_ret;
        l[i] *= now_level * 1.4f;
        r[i] *= now_level * 1.4f;
    }
    (void)pad_l;
    (void)pad_r;
    talk_run(l, r, n, P.talk ? 1.0f : 0.0f);
    tape_run(l, r, n, P.tape, fm_clampf(P.wear, 0.0f, 1.0f), fm_clampf(P.hiss, 0.0f, 1.0f));
    limiter_run(l, r, n);

    /* the sting's last fade, exact to the sample; and the silence after it */
    if (ST.len) {
        int32_t to_end = (int32_t)(ST.end - clk);
        float fl = ST.fade * 44100.0f;
        for (i = 0; i < n; i++) {
            float x = (float)(to_end - i) / fl, g;
            g = x >= 1.0f ? 1.0f : x <= 0.0f ? 0.0f : 0.5f - 0.5f * fm_cosf(FM_PI * x);
            l[i] *= g;
            r[i] *= g;
        }
    } else if (muted != 1.0f || mute_tgt != 1.0f) {
        for (i = 0; i < n; i++) {
            muted += (mute_tgt - muted) * 0.0005f;
            if (fm_fabsf(mute_tgt - muted) < 0.0001f)
                muted = mute_tgt;
            l[i] *= muted;
            r[i] *= muted;
        }
    }

    for (i = 0; i < n; i++) {
        float al = fm_fabsf(l[i]), ar = fm_fabsf(r[i]);
        if (al > peak_l)
            peak_l = al;
        if (ar > peak_r)
            peak_r = ar;
        e += l[i] * l[i] + r[i] * r[i];
    }
    meter_hold[0] = fm_maxf(peak_l, meter_hold[0] * 0.93f);
    meter_hold[1] = fm_maxf(peak_r, meter_hold[1] * 0.93f);
    S.meter_l = meter_hold[0];
    S.meter_r = meter_hold[1];
    S.energy += 0.03f * (fm_minf(1.0f, fm_sqrtf(e / (float)(2 * n)) * 4.0f) - S.energy);
    S.cpu_voices = (float)sounding;
    S.grove = G.on == 0 ? 0 : G.on == 1 ? 1 : 2;
    S.season = G.season;
    {
        int rm;
        S.room_now = P.room;
        for (rm = 0; rm < 7; rm++)
            if (ROOM_PC[rm] == tonic_pc)
                S.room_now = rm;
    }
    S.chord_n = H.n;
    for (k = 0; k < H.n && k < 8; k++)
        S.chord[k] = H.note[k];
    S.sting_len = ST.len;
    S.sting_seed = ST.seed;
    S.sting_pos = ST.len ? fm_clampf((float)(clk - ST.start) / ((float)ST.len * 44100.0f), 0.0f, 1.0f) : 0.0f;
    clk += (uint32_t)n;
}

/* ------------------------------------------------------------------------------------------- setup --- */
void bl_init(uint32_t seed)
{
    int i;
    bl_tables_init();
    for (i = 0; i < NVOICES; i++) {
        uint8_t *p = (uint8_t *)&V[i];
        int b;
        for (b = 0; b < (int)sizeof V[i]; b++)
            p[b] = 0;
        V[i].ks[0] = V[i].ks[1] = -1;
    }
    for (i = 0; i < NSTRINGS; i++) {
        KS[i].busy = 0;
        KS[i].len = 64;
    }
    for (i = 0; i < NSCHED; i++)
        SQ[i].used = 0;
    P.room = 3;                                        /* HEART: F, and Lydian is the white keys */
    P.mode = BM_LYDIAN;
    P.just = 0;
    P.a432 = 0;
    P.activity = 0.5f;
    P.drift = 0.3f;
    P.bed = 0.8f;
    P.melody = 0.75f;
    P.minutes = 0;
    P.solo = BV_RHODES;
    P.solo_level = 0.8f;
    P.space = 0.6f;
    P.echo = 0.45f;
    P.shimmer = 0.6f;
    P.tone = 0.5f;
    P.talk = 0;
    P.tape = 1;
    P.wear = 0.25f;
    P.hiss = 0.3f;
    P.level = 0.8f;
    now_tone = P.tone;
    now_space = P.space;
    now_echo = P.echo;
    now_shimmer = P.shimmer;
    now_level = P.level;
    now_vowel = 0.5f;
    now_bed = P.bed;
    now_melody = P.melody;
    now_solo = P.solo_level;
    TK.amt = 0.0f;
    TK.last = -1.0f;
    TP.amt = 1.0f;
    tape_coefs(P.wear);
    EC.time = 0.62f;
    RV.rt = 0.0f;
    reverb_set(5.0f, 5000.0f);
    fx_clear();
    all_reseed(seed);
    G.on = 0;
    G.season = BS_DAWN;
    ST.len = 0;
    H.mode = P.mode;
    tonic_pc = ROOM_PC[P.room];
    chord_set_to(0);
    H.n = 0;
    muted = mute_tgt = 1.0f;
    eq_w = eq_r = 0;
    clk = 0;
}

bl_par_t *bl_par(void) { return &P; }
const bl_status_t *bl_status(void) { return &S; }
