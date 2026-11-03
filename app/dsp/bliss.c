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
static float now_low, now_motion, now_bloom;
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
static const char *const SEASON_NAMES[BS_NSEASONS] = {"DAWN", "TEMPLE", "REDWOODS", "STARS", "STILL", "RIVER"};
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
    int mode, chord;                 /* a sting's: index in PAL[mode] */
    chord_t cur;                     /* the chord now (its root from the tonic) */
    uint16_t full;                   /* its pitch classes (absolute): what the motion and the melody use */
    uint16_t target;                 /* the ones the pad holds (a five-note chord leaves its fifth to the others) */
    int bass_pc;                     /* the low part's note */
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
static int in_scale(int midi) { return (MODE_SET[H.mode] >> pcm(midi - tonic_pc)) & 1; }
static int in_chord(int midi) { return (H.full >> pcm(midi)) & 1; }
static int in_pad(int midi) { return (H.target >> pcm(midi)) & 1; }

static void scopy(char *d, const char *s, int *k, int max)
{
    while (*s && *k < max - 1)
        d[(*k)++] = *s++;
    d[*k] = 0;
}
static void chord_name_update(void)
{
    const chord_t *c = &H.cur;
    int k = 0;
    scopy(S.chord_name, PC_NAMES[pcm(tonic_pc + c->root)], &k, 16);
    scopy(S.chord_name, c->q, &k, 16);
    if (H.bass_pc != pcm(tonic_pc + c->root)) {
        scopy(S.chord_name, "/", &k, 16);
        scopy(S.chord_name, PC_NAMES[H.bass_pc], &k, 16);
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

/* ------------------------------------------------------------------------------------------ the moods --- */
/* A mood is a key and a scale, two or three written progressions, and the voices of the Grove's four parts: the pad
 * (the chords), the low part (a bass under them), the motion (a ripple, a broken chord, a strum or bell sparks) and
 * the melody. A progression's chords are given from the key (0 the tonic); a step lasts len harmonic beats (the
 * mood's chord_s seconds each), and its bass is the chord's root (-1) or a note from the key (0: over the tonic). */
typedef struct { chord_t c; int8_t bass; uint8_t len; } step_t;
#define Q_ADD9 "add9", {0, 4, 7, 14}, 4
#define Q_MAJ7 "maj7", {0, 4, 7, 11}, 4
#define Q_MAJ9 "maj9", {0, 4, 7, 11, 14}, 5
#define Q_SIX9 "6/9", {0, 4, 9, 14}, 4
#define Q_LYD "maj7#11", {0, 4, 7, 11, 18}, 5
#define Q_MAJ "", {0, 4, 7}, 3
#define Q_SUS2 "sus2", {0, 2, 7, 12}, 4
#define Q_SUS4 "7sus", {0, 5, 7, 10}, 4
#define Q_M7 "m7", {0, 3, 7, 10}, 4
#define Q_M9 "m9", {0, 3, 7, 10, 14}, 5
#define Q_M11 "m11", {0, 3, 7, 10, 17}, 5
#define ST_(root, q, bass, len) {{root, q}, bass, len}

enum { MS_RIPPLE, MS_BROKEN, MS_STRUM, MS_SPARK };   /* how the motion moves */
typedef struct {
    uint8_t room, mode;                  /* its own key (a room) and scale */
    uint8_t pad, pedal, motion, melody;  /* the four parts' voices */
    uint8_t style;                       /* MS_* */
    float chord_s;                       /* a harmonic beat, seconds */
    float ioi;                           /* the motion's pace: seconds a note (a spark's: between them) */
    float vowel, echo_s, tone;           /* the choir's vowel, the echo's time, a tilt of the tone */
    const step_t *prog[3];
    uint8_t plen[3], nprog;
    float g[4];                          /* the four parts level with each other (tools/render, a part alone) */
} mood_t;

/* DAWN, D major: strings, a glass bass, the harp rippling, a flute */
static const step_t DAWN_A[] = {ST_(0, Q_ADD9, -1, 2), ST_(5, Q_MAJ7, -1, 1), ST_(9, Q_M9, -1, 1), ST_(5, Q_SIX9, -1, 1),
                                ST_(7, Q_SUS4, -1, 1), ST_(0, Q_ADD9, -1, 1), ST_(0, Q_MAJ9, -1, 1)};
static const step_t DAWN_B[] = {ST_(0, Q_ADD9, -1, 1), ST_(2, Q_MAJ, 0, 1), ST_(0, Q_MAJ7, -1, 1), ST_(2, Q_MAJ, 0, 1),
                                ST_(5, Q_MAJ9, -1, 1), ST_(4, Q_M7, -1, 1), ST_(2, Q_M9, -1, 1), ST_(7, Q_SUS4, -1, 1)};
static const step_t DAWN_C[] = {ST_(9, Q_M9, -1, 2), ST_(5, Q_MAJ7, -1, 1), ST_(0, Q_ADD9, -1, 1), ST_(7, Q_SUS2, 0, 1),
                                ST_(0, Q_SIX9, -1, 2)};
/* TEMPLE, A dorian: a choir, a string bass, the zither strummed, bells */
static const step_t TEMPLE_A[] = {ST_(0, Q_M9, -1, 2), ST_(10, Q_ADD9, -1, 1), ST_(5, Q_MAJ, -1, 1), ST_(0, Q_M11, -1, 2),
                                  ST_(3, Q_MAJ7, -1, 1), ST_(10, Q_SIX9, -1, 1)};
static const step_t TEMPLE_B[] = {ST_(0, Q_M9, -1, 1), ST_(5, Q_MAJ, 0, 1), ST_(0, Q_M11, -1, 1), ST_(10, Q_ADD9, 0, 1),
                                  ST_(7, Q_M7, -1, 1), ST_(3, Q_MAJ7, -1, 1), ST_(5, Q_SIX9, -1, 2)};
/* REDWOODS, F lydian: strings, a glass bass, the CP-80's broken chords, a flute */
static const step_t REDWOODS_A[] = {ST_(0, Q_LYD, -1, 2), ST_(2, Q_MAJ, 0, 2), ST_(4, Q_M7, -1, 1), ST_(9, Q_M9, -1, 1),
                                    ST_(7, Q_MAJ7, -1, 1), ST_(2, Q_ADD9, -1, 1)};
static const step_t REDWOODS_B[] = {ST_(0, Q_MAJ9, -1, 1), ST_(11, Q_M7, -1, 1), ST_(9, Q_M9, -1, 1), ST_(7, Q_SIX9, -1, 1),
                                    ST_(2, Q_ADD9, 0, 2), ST_(0, Q_LYD, -1, 2)};
/* STARS, E lydian: glass, a string bass, the Rhodes in broken chords, bells */
static const step_t STARS_A[] = {ST_(0, Q_MAJ9, -1, 2), ST_(2, Q_ADD9, -1, 2), ST_(9, Q_M9, -1, 1), ST_(7, Q_MAJ7, -1, 1),
                                 ST_(4, Q_M7, -1, 1), ST_(2, Q_MAJ, 0, 1)};
static const step_t STARS_B[] = {ST_(0, Q_LYD, -1, 2), ST_(11, Q_M7, -1, 1), ST_(9, Q_M11, -1, 1), ST_(2, Q_SIX9, 0, 2),
                                 ST_(7, Q_ADD9, -1, 1), ST_(0, Q_MAJ7, -1, 1)};
/* STILL, G pentatonic: glass, a string bass, a bell now and then, a flute; the slowest */
static const step_t STILL_A[] = {ST_(0, Q_ADD9, -1, 2), ST_(9, Q_M7, -1, 2), ST_(5, Q_MAJ9, -1, 1), ST_(0, Q_SIX9, -1, 2),
                                 ST_(7, Q_SUS2, -1, 1)};
static const step_t STILL_B[] = {ST_(0, Q_SIX9, -1, 2), ST_(2, Q_SUS4, 0, 2), ST_(9, Q_M11, -1, 2), ST_(0, Q_ADD9, -1, 2)};
/* RIVER, C major: a choir, a glass bass, the harp flowing, a flute; the quickest */
static const step_t RIVER_A[] = {ST_(5, Q_MAJ7, -1, 1), ST_(7, Q_ADD9, -1, 1), ST_(4, Q_M7, -1, 1), ST_(9, Q_M9, -1, 1),
                                 ST_(5, Q_MAJ9, -1, 1), ST_(7, Q_SUS4, -1, 1), ST_(0, Q_ADD9, -1, 2)};
static const step_t RIVER_B[] = {ST_(0, Q_MAJ9, -1, 1), ST_(9, Q_M9, -1, 1), ST_(4, Q_M7, -1, 1), ST_(5, Q_LYD, -1, 1),
                                 ST_(2, Q_M9, -1, 1), ST_(7, Q_SUS4, -1, 1), ST_(0, Q_SIX9, -1, 2)};
#define PROGS(a, b) {a, b, 0}, {sizeof a / sizeof a[0], sizeof b / sizeof b[0], 0}, 2
#define PROGS3(a, b, c) {a, b, c}, {sizeof a / sizeof a[0], sizeof b / sizeof b[0], sizeof c / sizeof c[0]}, 3
static const mood_t MOODS[BL_NMOODS] = {
    /* DAWN     */ {1, BM_MAJOR, BV_STRINGS, BV_GLASS, BV_HARP, BV_FLUTE, MS_RIPPLE, 11.0f, 0.34f, 0.4f, 0.62f, 0.0f,
                    PROGS3(DAWN_A, DAWN_B, DAWN_C), {1.43f, 1.48f, 2.9f, 0.9f}},
    /* TEMPLE   */ {5, BM_DORIAN, BV_CHOIR, BV_STRINGS, BV_ZITHER, BV_BELLS, MS_STRUM, 13.0f, 0.06f, 0.2f, 0.74f, -0.1f,
                    PROGS(TEMPLE_A, TEMPLE_B), {0.57f, 2.0f, 1.8f, 3.0f}},
    /* REDWOODS */ {3, BM_LYDIAN, BV_STRINGS, BV_GLASS, BV_CP80, BV_FLUTE, MS_BROKEN, 12.0f, 0.62f, 0.9f, 0.47f, 0.05f,
                    PROGS(REDWOODS_A, REDWOODS_B), {1.45f, 1.55f, 2.3f, 0.88f}},
    /* STARS    */ {2, BM_LYDIAN, BV_GLASS, BV_STRINGS, BV_RHODES, BV_BELLS, MS_BROKEN, 14.0f, 0.55f, 1.2f, 0.82f, 0.12f,
                    PROGS(STARS_A, STARS_B), {1.19f, 2.1f, 0.85f, 3.0f}},
    /* STILL    */ {4, BM_PENTA, BV_GLASS, BV_STRINGS, BV_BELLS, BV_FLUTE, MS_SPARK, 20.0f, 2.6f, 0.6f, 0.7f, -0.15f,
                    PROGS(STILL_A, STILL_B), {1.23f, 1.9f, 1.65f, 1.0f}},
    /* RIVER    */ {0, BM_MAJOR, BV_CHOIR, BV_GLASS, BV_HARP, BV_FLUTE, MS_RIPPLE, 9.0f, 0.26f, 1.6f, 0.55f, 0.0f,
                    PROGS(RIVER_A, RIVER_B), {0.44f, 1.53f, 2.5f, 0.86f}},
};
int bl_mood_room(int m) { return m >= 0 && m < BL_NMOODS ? MOODS[m].room : 3; }
int bl_mood_mode(int m) { return m >= 0 && m < BL_NMOODS ? MOODS[m].mode : BM_LYDIAN; }

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
    int season;                      /* a sting's sky; the Grove's mood */
    int stopping;
    int mood, room;                  /* what it plays now (P's, at the next chord) */
    int prog, prog_next, step;       /* the progression, the one after this pass (-1 the same), the step */
    int sect;                        /* SC_* */
    uint32_t sect_end;
    float inten;                     /* 0..1: how much is going on, after the section */
    uint32_t chord_t0, chord_len;    /* when this chord came, and for how long */
    int steer;                       /* the bottom octave's held key (a pc), -1 none */
    uint32_t m_next;                 /* the motion */
    int m_left, m_len, m_idx, m_dir, m_strum;
    uint32_t mel_next, mel_rest;     /* the melody */
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
    int type = MOODS[G.mood].pad;
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
        if (!in_pad(H.note[k]) && (extra < 0 || rng_chance(&rng, 0.5f)))
            extra = k;
        if (v && v->type != MOODS[G.mood].pad && !ST.len)
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
    int note = 36 + H.bass_pc, type = ST.len ? H.pedal_type : MOODS[G.mood].pedal;
    float att = ST.len || !G.on || clk - G.start < SEC(10.0f) ? 6.0f : 2.2f;
    if (note > 43)
        note -= 12;
    if (v && on && v->gate && v->note == note && v->type == type)
        return;
    if (v)
        voice_release(v, att > 3.0f ? 9.0f : 4.0f);
    H.pedal_id = 0;
    if (on) {
        v = voice_start(type, L_PEDAL, note, bl_note_hz(note), 0.5f, 1.0f, 0.0f, att, 0.0f, -1.0f);
        H.pedal_id = v->id;
        H.pedal_note = note;
    }
}

/* a sting's chord: one of the palette's, over the tonic */
static void chord_set_to(int k)
{
    H.chord = k;
    H.cur = *chord_def(H.mode, k);
    H.target = H.full = chord_set(H.mode, k, tonic_pc);
    H.bass_pc = tonic_pc;
    H.settled = 0;
    chord_name_update();
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
            const chord_t *c = &H.cur;
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

/* a bell melody: three to six notes of the scale, stepping, ending on a note of the chord */
static void phrase_bells(uint32_t t0, int layer, float vel_s)
{
    int Sc[32], n = scale_list(Sc, 72, 91), k, idx, len = 3 + rng_int(&rng, 4), dir = rng_chance(&rng, 0.5f) ? 1 : -1;
    float t = 0.0f;
    if (n < 5)
        return;
    idx = 1 + rng_int(&rng, n - 2);
    for (k = 0; k < len; k++) {
        int note = Sc[idx];
        if (k == len - 1) {
            int C[32], m = chord_list(C, 72, 91), j, bd = 99;
            for (j = 0; j < m; j++) {
                int d = C[j] > note ? C[j] - note : note - C[j];
                if (d < bd) {
                    bd = d;
                    note = C[j];
                }
            }
        }
        sched(t0 + SEC(t), BV_BELLS, layer, note, (0.3f + 0.15f * rng_unit(&rng) + (k == 0 ? 0.08f : 0.0f)) * vel_s,
              rng_range(&rng, -0.5f, 0.5f), 6.0f, 0, 0, 0);
        t += rng_range(&rng, 0.45f, 1.0f) * (k == len - 2 ? 1.5f : 1.0f);
        if (rng_chance(&rng, 0.2f))
            dir = -dir;
        idx += dir * (rng_chance(&rng, 0.75f) ? 1 : 2);
        if (idx < 0 || idx >= n) {
            dir = -dir;
            idx = idx < 0 ? 1 : n - 2;
        }
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
/* The Grove plays the mood's progressions, all four parts at once: the pad's chord arrives together (common tones
 * held, the rest crossfaded), the low part moves to the step's bass, the motion runs through the chord as it is
 * now, the melody answers a chord now and then. Over a piece it has a shape: it arrives (the pad and the bass, then
 * the motion), blooms (everyone, the sounds open), floats (thinner, darker, the chords slower), and blooms again;
 * a timed piece returns home before it ends. Each pass through a progression strays a little (drift): a chord's
 * colour changes, or the next pass is another of the mood's progressions. */
enum { SC_ARRIVE, SC_BLOOM, SC_FLOAT, SC_RETURN };
static const float SECT_INTEN[4] = {0.3f, 1.0f, 0.55f, 0.5f};

static float glen(void) { return (float)P.minutes * 60.0f; }
static const mood_t *mo(void) { return &MOODS[G.mood]; }

static int chord_in_scale(const chord_t *c)
{
    int i;
    for (i = 0; i < c->n; i++)
        if (!((MODE_SET[H.mode] >> pcm(c->root + c->t[i])) & 1))
            return 0;
    return 1;
}

/* a step's chord, sometimes in another colour of the same kind that stays in the scale */
static void step_colour(chord_t *c)
{
    static const chord_t MAJ[] = {{0, Q_ADD9}, {0, Q_MAJ7}, {0, Q_SIX9}, {0, Q_MAJ9}};
    static const chord_t MIN[] = {{0, Q_M7}, {0, Q_M9}, {0, Q_M11}};
    const chord_t *set;
    int n, k;
    if (c->n < 3 || !rng_chance(&rng, 0.1f + 0.4f * P.drift))
        return;
    if (c->t[1] == 4) {
        set = MAJ;
        n = 4;
    } else if (c->t[1] == 3) {
        set = MIN;
        n = 3;
    } else
        return;
    for (k = 0; k < 3; k++) {
        chord_t t = set[rng_int(&rng, n)];
        t.root = c->root;
        if (chord_in_scale(&t)) {
            *c = t;
            return;
        }
    }
}

/* the step's length in samples: the mood's beat, slower when the activity is low and while it floats */
static uint32_t step_len(int len)
{
    float s = mo()->chord_s * (float)len * (1.3f - 0.6f * P.activity) * (G.sect == SC_FLOAT ? 1.25f : 1.0f);
    return SEC(s * rng_range(&rng, 0.9f, 1.12f));
}

/* a new chord, now: the pad's voices all move at once (att: how slowly the new ones come in) */
static void chord_go(const chord_t *c, int bass, uint32_t len, float att)
{
    int k;
    H.cur = *c;
    H.bass_pc = pcm(tonic_pc + (bass < 0 ? c->root : bass));
    H.full = 0;
    for (k = 0; k < c->n; k++)
        H.full |= (uint16_t)(1u << pcm(tonic_pc + c->root + c->t[k]));
    H.target = H.full;
    if (c->n > 4)                                       /* five notes: the pad leaves the fifth to the others */
        H.target &= (uint16_t)~(1u << pcm(tonic_pc + c->root + c->t[2]));
    H.settled = 0;
    chord_name_update();
    for (k = 0; k < 8; k++)
        if (!pad_morph(att * rng_range(&rng, 0.8f, 1.25f), rng_range(&rng, 3.5f, 5.5f)))
            break;
    pedal_set(1);
    G.chord_t0 = clk;
    G.chord_len = len;
    H.next_chord = clk + len;
    H.next_morph = clk + SEC(2.0f);
    if (mo()->style == MS_STRUM && G.sect != SC_ARRIVE) {
        G.m_next = clk + SEC(rng_range(&rng, 0.3f, 1.2f));
        G.m_left = 0;
        G.m_strum = 2;
    }
    if (due(G.mel_rest))                                /* the melody answers a new chord, sometimes */
        G.mel_next = clk + SEC(rng_range(&rng, 1.5f, 4.0f));
}

static void step_go(float att)
{
    const step_t *s = &mo()->prog[G.prog][G.step];
    chord_t c = s->c;
    step_colour(&c);
    chord_go(&c, s->bass, step_len(s->len), att);
}

/* a chord on d (from the tonic) made of the scale: its third, its fifth and its ninth (the octave when the ninth
 * isn't in it); in a pentatonic scale they stack in fourths. Outside the scale a major add9 */
static void chord_build(chord_t *c, int d)
{
    static const chord_t ADD9 = {0, Q_ADD9};
    int sc[12], n = 0, i = -1, k;
    for (k = 0; k < 12; k++)
        if ((MODE_SET[H.mode] >> k) & 1) {
            if (k == d)
                i = n;
            sc[n++] = k;
        }
    *c = ADD9;
    c->root = (int8_t)d;
    if (i < 0)
        return;
    for (k = 0; k < 3; k++) {
        int j = i + 2 * k;
        c->t[k] = (int8_t)(sc[j % n] + 12 * (j / n) - d);
    }
    c->t[3] = (MODE_SET[H.mode] >> pcm(d + 2)) & 1 ? 14 : 12;
    c->q = c->t[1] == 3 ? (c->t[3] == 14 ? "madd9" : "m") : c->t[1] == 4 ? (c->t[3] == 14 ? "add9" : "") : "sus";
}

/* the bottom octave: a chord on pc. The progression's own, if it has one there (and it goes on from it), else the
 * mood's other progression's, else one built in the scale: root, third, fifth and ninth (a major add9 on a note
 * outside it: the borrowed chords, bVII and bVI, that new age loves) */
static void steer_go(int pc)
{
    const mood_t *m = mo();
    int d = pcm(pc - tonic_pc), p, k;
    chord_t c;
    for (p = 0; p < m->nprog; p++) {
        int q = (G.prog + p) % m->nprog;
        for (k = 0; k < m->plen[q]; k++)
            if (pcm(m->prog[q][k].c.root) == d) {
                if (q == G.prog)
                    G.step = k;
                c = m->prog[q][k].c;
                chord_go(&c, -1, step_len(m->prog[q][k].len), 1.2f);
                return;
            }
    }
    chord_build(&c, d);
    chord_go(&c, -1, step_len(1), 1.2f);
}

static void section_next(void)
{
    float el = (float)(clk - G.start) * BL_INV_SR, L = glen();
    int s = G.sect == SC_BLOOM ? SC_FLOAT : SC_BLOOM;
    if (s == SC_BLOOM && mo()->nprog > 1 && rng_chance(&rng, 0.35f + 0.5f * P.drift))
        G.prog_next = (G.prog + 1 + rng_int(&rng, mo()->nprog - 1)) % mo()->nprog;   /* after this pass */
    G.sect = s;
    G.sect_end = clk + SEC(s == SC_BLOOM ? rng_range(&rng, 120.0f, 200.0f) : rng_range(&rng, 80.0f, 140.0f));
    if (L > 240.0f && el + (float)(G.sect_end - clk) * BL_INV_SR > L - 100.0f) {   /* the last: home */
        G.sect = SC_RETURN;
        G.prog_next = 0;
        G.sect_end = clk + SEC(1e5f);
    }
}

static void grove_start(void)
{
    G.on = 1;
    G.start = clk;
    G.stopping = 0;
    G.mood = P.mood;
    G.season = G.mood;
    H.mode = mo()->mode;
    tonic_pc = ROOM_PC[P.room];
    G.room = P.room;
    G.prog = 0;
    G.prog_next = -1;
    G.step = 0;
    G.sect = SC_ARRIVE;
    G.sect_end = clk + SEC(rng_range(&rng, 40.0f, 70.0f));
    G.inten = 0.15f;
    G.m_left = 0;
    G.m_strum = 0;
    G.m_next = clk + SEC(rng_range(&rng, 10.0f, 18.0f));
    G.mel_next = clk + SEC(rng_range(&rng, 25.0f, 40.0f));
    G.mel_rest = clk + SEC(40.0f);
    if (G.steer >= 0)
        steer_go(G.steer);
    else
        step_go(rng_range(&rng, 4.0f, 7.0f));
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

/* the motion: one note when it is due, from the chord as it is now (a chord change never leaves it behind) */
static void motion_step(void)
{
    const mood_t *m = mo();
    float in = G.inten * (0.45f + 0.55f * P.activity) * (P.talk ? 0.7f : 1.0f), ioi = m->ioi;
    int C[32], n, note;
    if (!due(G.m_next))
        return;
    if (G.m_left <= 0) {                                /* a phrase, or a rest */
        float go = G.m_strum == 2 ? 1.0f : G.m_strum ? 0.3f + 0.7f * in : 0.25f + 0.7f * in;
        G.m_strum = 0;
        if (!rng_chance(&rng, go)) {
            G.m_next = clk + SEC(m->style == MS_SPARK ? ioi * rng_range(&rng, 1.0f, 2.0f) : rng_range(&rng, 2.0f, 5.0f));
            return;
        }
        G.m_left = m->style == MS_RIPPLE ? 8 + rng_int(&rng, 4 + (int)(10.0f * in)) : m->style == MS_BROKEN ? 4 + rng_int(&rng, 3)
                   : m->style == MS_STRUM ? 5 + rng_int(&rng, 3) : 1 + rng_chance(&rng, 0.35f);
        G.m_dir = m->style == MS_STRUM || rng_chance(&rng, 0.6f) ? 1 : -1;
        G.m_idx = -1;
        G.m_len = G.m_left;
    }
    switch (m->style) {
    case MS_RIPPLE: n = chord_list(C, 55 + reg_shift(), 88 + reg_shift()); break;
    case MS_BROKEN: n = chord_list(C, 48, 77); break;
    case MS_STRUM: n = chord_list(C, 50, 79); break;
    default: n = chord_list(C, 76, 93); break;
    }
    if (n < 3) {
        G.m_left = 0;
        return;
    }
    if (G.m_idx < 0)                                    /* where a phrase starts: low for a ripple going up */
        G.m_idx = m->style == MS_SPARK ? rng_int(&rng, n) : G.m_dir > 0 ? rng_int(&rng, n / 3 + 1) : n - 1 - rng_int(&rng, n / 3 + 1);
    if (G.m_idx >= n)
        G.m_idx = n - 1;
    note = C[G.m_idx];
    {
        float x = (float)(G.m_len - G.m_left) / (float)(G.m_len > 1 ? G.m_len - 1 : 1);
        float vel = (m->style == MS_SPARK ? 0.28f : 0.24f) + 0.14f * sin1(x * 0.5f) + 0.06f * rng_unit(&rng);
        float hold = m->style == MS_RIPPLE ? 1.6f : m->style == MS_BROKEN ? 1.5f : m->style == MS_STRUM ? 3.0f : 4.0f;
        sched(clk, m->motion, L_HARP, note, vel * (0.8f + 0.3f * in), fm_clampf((float)(note - 70) / 16.0f, -0.7f, 0.7f),
              hold, 0, 0, 0);
    }
    G.m_left--;
    if (m->style == MS_SPARK) {
        G.m_idx = G.m_idx + (rng_chance(&rng, 0.5f) ? 1 : -1);
        G.m_next = clk + SEC(G.m_left ? rng_range(&rng, 0.4f, 0.9f) : ioi * rng_range(&rng, 0.7f, 1.6f));
        return;
    }
    G.m_idx += G.m_dir;
    if (m->style == MS_RIPPLE && (G.m_idx >= n || G.m_idx < 0)) {   /* the top, or the bottom: turn */
        G.m_dir = -G.m_dir;
        G.m_idx += 2 * G.m_dir;
    }
    if (G.m_idx < 0)
        G.m_idx = 0;
    if (G.m_left) {
        G.m_next = clk + SEC(ioi * rng_around(&rng, 1.0f, m->style == MS_BROKEN ? 0.25f : 0.06f));
    } else if (m->style == MS_STRUM) {                  /* the next strum: halfway through the chord */
        uint32_t half = G.chord_t0 + G.chord_len / 2;
        if ((int32_t)(half - clk) > (int32_t)SEC(2.0f)) {
            G.m_next = half;
            G.m_strum = 1;
        } else {                                        /* the next chord strums it (chord_go) */
            G.m_next = H.next_chord + SEC(4.0f);
            G.m_strum = 0;
        }
    } else
        G.m_next = clk + SEC(ioi * rng_range(&rng, 3.0f, 9.0f) / (0.4f + in));
}

static void melody_step(void)
{
    const mood_t *m = mo();
    float in = G.inten * (P.talk ? 0.6f : 1.0f);
    if (!due(G.mel_next) || !due(G.mel_rest))
        return;
    G.mel_next = clk + SEC(rng_range(&rng, 30.0f, 70.0f));      /* (a chord change brings it sooner) */
    if (G.sect == SC_ARRIVE || !rng_chance(&rng, (0.2f + 0.6f * in) * (0.5f + 0.5f * P.activity)))
        return;
    if (m->melody == BV_FLUTE) {                       /* a flute phrase is up to 20 s: then a breath */
        phrase_flute(clk, L_FLUTE, 0.9f);
        G.mel_rest = clk + SEC(rng_range(&rng, 16.0f, 30.0f) * rest_scale());
    } else {
        phrase_bells(clk, L_BELL, 0.9f);
        if (rng_chance(&rng, 0.4f))                    /* and its answer */
            phrase_bells(clk + SEC(rng_range(&rng, 4.5f, 7.0f)), L_BELL, 0.75f);
        G.mel_rest = clk + SEC(rng_range(&rng, 6.0f, 14.0f) * rest_scale());
    }
}

static void grove_step(void)
{
    float el = (float)(clk - G.start) * BL_INV_SR, L = glen();
    S.seconds = (uint32_t)el;
    if (G.on == 3) {                                   /* the tail of a timed piece */
        if (el >= L)
            G.on = 0;
        return;
    }
    {                                                  /* the intensity follows the section (about 15 s) */
        float t = SECT_INTEN[G.sect];
        G.inten += (t - G.inten) * 0.0004f;
    }
    /* the ending of a timed piece: home, everything lets go 25 s before the end */
    if (L > 90.0f && el >= L - 60.0f && G.on == 1) {
        const step_t *s = &mo()->prog[0][0];
        G.on = 2;
        tonic_pc = ROOM_PC[P.room];
        G.prog = 0;
        G.step = 0;
        chord_go(&s->c, s->bass, SEC(60.0f), 3.0f);
        phrase_shower(clk + SEC(14.0f), mo()->motion == BV_ZITHER ? BV_ZITHER : BV_HARP, L_HARP, 0.8f, 10);
        phrase_bell(clk + SEC(26.0f), L_BELL, 0.9f);
    }
    if (G.on == 2 && el >= L - 25.0f) {
        G.on = 3;
        for (int i = 0; i < L_SOLO; i++)
            sched_clear(i);
        release_all(16.0f, 1);
        return;
    }
    if (G.on == 1 && due(G.sect_end)) {
        if (G.sect == SC_ARRIVE)
            G.m_next = clk;
        section_next();
    }
    /* the pad mends itself (a stolen voice, a new mood's colour) one voice at a time */
    if (due(H.next_morph))
        H.next_morph = clk + SEC(pad_morph(rng_range(&rng, 2.0f, 4.0f), rng_range(&rng, 4.0f, 6.0f)) ? 3.0f : 2.0f);
    if (G.on == 1 && due(H.next_chord)) {
        if (G.steer >= 0)                              /* held: the chord stays */
            H.next_chord = clk + SEC(2.0f);
        else {
            int moved = 0;
            if (G.mood != P.mood) {                    /* another mood: its key, its first progression */
                G.mood = P.mood;
                G.season = G.mood;
                H.mode = mo()->mode;
                G.prog = 0;
                G.step = 0;
                G.prog_next = -1;
                moved = 1;
            }
            if (G.room != P.room) {                    /* another key */
                G.room = P.room;
                tonic_pc = ROOM_PC[P.room];
                if (!moved)
                    G.step = 0;
                moved = 1;
            }
            if (!moved && ++G.step >= mo()->plen[G.prog]) {
                G.step = 0;
                if (G.prog_next >= 0) {
                    G.prog = G.prog_next;
                    G.prog_next = -1;
                }
            }
            step_go(moved ? 3.0f : rng_range(&rng, 1.2f, 2.6f));
        }
    }
    if (G.on != 1)
        return;
    if (G.mood != P.mood || G.room != P.room)          /* a new mood or key: at the next chord, soon */
        if ((int32_t)(H.next_chord - clk) > (int32_t)SEC(3.0f))
            H.next_chord = clk + SEC(1.5f);
    if (G.sect != SC_ARRIVE || el > 12.0f)
        motion_step();
    melody_step();
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
    now_low = P.low;
    now_motion = P.motion;
    now_bloom = 0.55f;
    EC.time = fm_clampf(L * 0.12f, 0.25f, 0.62f);
    H.mode = MOODS[P.mood].mode;
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
enum { E_NOTE, E_GROVE, E_STING, E_PANIC, E_STEER };
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
void bl_post_steer(int pc) { post(E_STEER, pc < 0 ? -1 : pcm(pc), 0, 0, 0.0f); }

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
        int best = -1, forced = shed_req != done, pass;
        float lowest = 1e9f;
        for (pass = 0; pass < 2 && best < 0; pass++)   /* released voices first; then, still over, a held one */
        for (i = 0; i < NVOICES; i++) {
            float lv;
            if (!V[i].on || V[i].hold == -2.0f || (V[i].layer == L_SOLO && V[i].gate) || (V[i].gate && !forced && !pass))
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

/* the voices level with each other as solos (loudest 400 ms of tools/render.c's solo phrase within a dB or so:
 * -14 for the keys and plucks, -15.5 for the pads): the bed keeps its own balance */
static const float SOLO_TRIM[BV_NTYPES] = {
    0.89f, /* rhodes */ 1.78f, /* cp80 */ 1.0f, /* flute */ 1.78f, /* harp */ 1.11f, /* zither */
    1.10f, /* bells */ 1.78f, /* strings */ 0.65f, /* choir */ 1.97f /* glass */
};

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
    voice_start(t, L_SOLO, midi, bl_note_hz(midi), vel, SOLO_TRIM[t], fm_clampf((float)(midi - 66) / 20.0f, -0.5f, 0.5f),
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
        case E_STEER:
            G.steer = e.a;
            if (e.a >= 0 && !G.on) {                   /* a chord key starts the Grove, on that chord */
                if (ST.len) {
                    hard_stop();
                    ST.len = 0;
                }
                muted = mute_tgt = 1.0f;
                grove_start();
            } else if (e.a >= 0 && G.on == 1)
                steer_go(e.a);
            else if (e.a < 0 && G.on == 1) {           /* let go: the chord plays out its step, then on */
                uint32_t end = G.chord_t0 + G.chord_len;
                H.next_chord = (int32_t)(end - clk) > (int32_t)SEC(4.0f) ? end : clk + SEC(4.0f);
            }
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
    const mood_t *mood;
    if (n > BL_BLOCK)
        n = BL_BLOCK;
    events_run();
    shed_run();
    mood = &MOODS[ST.len ? G.season : G.mood];
    /* the slow LFOs: 47, 73, 131 and 211 seconds */
    {
        static const float per[4] = {47.0f, 73.0f, 131.0f, 211.0f};
        for (k = 0; k < 4; k++) {
            lfo[k] = wrap1(lfo[k] + (float)n / (per[k] * 44100.0f));
            lfo_v[k] = sin1(lfo[k]);
        }
    }
    /* the bloom: the sounds open as the piece does, and over each chord, and close again */
    {
        float b = 0.55f;
        if (G.on) {
            float ph = G.chord_len ? fm_clampf((float)(clk - G.chord_t0) / (float)G.chord_len, 0.0f, 1.0f) : 0.5f;
            b = P.bloom * (0.15f + 0.55f * G.inten + 0.25f * sin1(ph * 0.5f) + 0.12f * lfo_v[1]) + (1.0f - P.bloom) * 0.55f;
            b = fm_clampf(b * (P.talk ? 0.8f : 1.0f), 0.0f, 1.0f);
        }
        now_bloom = smooth(now_bloom, b, 0.004f);
    }
    now_tone = smooth(now_tone, fm_clampf(P.tone + (G.on ? mood->tone + 0.1f * (now_bloom - 0.5f) : 0.0f) + 0.12f * lfo_v[0], 0.0f, 1.0f), 0.01f);
    now_space = smooth(now_space, P.space, 0.02f);
    now_echo = smooth(now_echo, P.echo, 0.02f);
    now_shimmer = smooth(now_shimmer, fm_clampf(P.shimmer * (0.85f + 0.15f * lfo_v[1]) + (G.on ? 0.25f * (now_bloom - 0.4f) : 0.0f), 0.0f, 1.0f), 0.02f);
    now_level = smooth(now_level, P.level, 0.05f);
    now_vowel = smooth(now_vowel, fm_clampf(mood->vowel + 0.3f * lfo_v[2] + (G.on ? 0.8f * (now_bloom - 0.45f) : 0.0f), 0.0f, 2.0f), 0.002f);
    now_bed = smooth(now_bed, P.bed, 0.03f);
    now_melody = smooth(now_melody, P.melody, 0.03f);
    now_low = smooth(now_low, P.low, 0.03f);
    now_motion = smooth(now_motion, P.motion, 0.03f);
    now_solo = smooth(now_solo, P.solo_level, 0.05f);
    tone = now_tone;

    if (ST.len)
        sting_step();
    else if (G.on)
        grove_step();
    sched_run(clk + (uint32_t)n);
    shed_run();                                        /* a chord's new voices over the budget: now, not a block late */

    for (i = 0; i < n; i++) {
        bus_pad[0][i] = bus_pad[1][i] = bus_keys[0][i] = bus_keys[1][i] = bus_dry[0][i] = bus_dry[1][i] = 0.0f;
        send_rev[i] = send_ech[i] = 0.0f;
    }
    cx.tone = tone;
    cx.vowel = now_vowel;
    cx.bloom = now_bloom;
    for (k = 0; k < NLAYERS; k++)
        cx.lg[k] = 1.0f;
    {
        static const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const float *g = ST.len ? one : mood->g;      /* a mood's parts level with each other */
        float gl = ST.len ? 1.0f : 1.4f;              /* the four parts together as loud as the old Grove */
        cx.lg[L_KEYS] = cx.lg[L_HARP] = cx.lg[L_PLUCK] = now_motion * g[2] * gl * (P.talk ? 0.85f : 1.0f);
        cx.lg[L_BELL] = cx.lg[L_FLUTE] = now_melody * g[3] * gl * (P.talk ? 0.85f : 1.0f);
        cx.lg[L_PEDAL] = 0.55f * (ST.len ? 1.0f : now_low * g[1] * gl);
        cx.lg[L_PAD] = now_bed * g[0] * gl * (G.on ? 0.8f + 0.3f * now_bloom : 1.0f);   /* (the low part has its own) */
    }
    cx.lg[L_SOLO] = now_solo;
    sh = 0.15f + 0.85f * now_shimmer;
    sounding = voices_render(n, &cx, 1.0f, 0.2f + 0.8f * now_echo);

    /* the pads through the ensemble, the keys through a gentler one */
    chorus_run(&CHO[0], bus_pad[0], bus_pad[1], n, sh, 0.6f + 0.4f * now_shimmer);
    chorus_run(&CHO[1], bus_keys[0], bus_keys[1], n, sh, 0.35f + 0.5f * now_shimmer);

    /* echo and hall */
    ech_time = ST.len ? EC.time : mood->echo_s;
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
    S.season = G.on ? G.mood : ST.len ? G.season : P.mood;
    S.section = G.sect;
    S.root_pc = G.on || ST.len ? pcm(tonic_pc + H.cur.root) : -1;
    S.bloom = now_bloom;
    {
        int c = 0;
        for (k = 0; k < NVOICES; k++)
            if (V[k].on)
                c += voice_cost(&V[k]);
        S.cost = (float)c;
    }
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
    P.mood = BS_REDWOODS;
    P.room = MOODS[P.mood].room;                       /* HEART: F, and Lydian is the white keys */
    P.just = 0;
    P.a432 = 0;
    P.activity = 0.5f;
    P.drift = 0.3f;
    P.bloom = 0.7f;
    P.bed = 0.8f;
    P.low = 0.7f;
    P.motion = 0.75f;
    P.melody = 0.7f;
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
    now_low = P.low;
    now_motion = P.motion;
    now_bloom = 0.55f;
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
    G.mood = G.season = P.mood;
    G.room = P.room;
    G.steer = -1;
    ST.len = 0;
    H.mode = MOODS[P.mood].mode;
    tonic_pc = ROOM_PC[P.room];
    chord_set_to(0);
    H.n = 0;
    muted = mute_tgt = 1.0f;
    eq_w = eq_r = 0;
    clk = 0;
}

bl_par_t *bl_par(void) { return &P; }
const bl_status_t *bl_status(void) { return &S; }
