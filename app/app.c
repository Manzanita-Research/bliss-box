/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* Bliss Box on the FM-1: the panel, the screen and the settings around the sound engine (dsp/bliss.c).
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in scope.
 *
 * The panel
 *   PLAY        the Grove: start, or let it close (a long release)
 *   REC         a new sting (a new seed) at the sting length; SEQ plays the last one again; ARP the length
 *   HOME LFO FX ENV EDIT   the pages K1..K4 turn: HOME, GROVE, SPACE, TAPE, TUNING
 *   GLO         TALK: the bed steps back for a voice
 *   SEL         the cassette in or out
 *   SAVE        keeps the settings
 *   OCT- OCT+   the keys' octave
 *   ALGO        the room (the key); PRESET the voice the keys play; SELECT the piece's length
 * The keys play the solo voice with velocity, and so does MIDI in (any channel). */
#include "fastmath.h"
#include "bliss.h"

/* ---------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x424C5331u            /* "BLS1" */
static const uint8_t LENGTHS[] = {0, 5, 10, 15, 20, 30, 45, 60};   /* the piece, minutes (0: endless) */
static const uint8_t STING_S[] = {3, 5, 10, 30};
#define NLENGTHS (sizeof LENGTHS)
#define NSTINGS (sizeof STING_S)

typedef struct {
    uint32_t magic;
    bl_par_t par;
    int8_t octave;                       /* -2 .. +2 */
    uint8_t length;                      /* LENGTHS index */
    uint8_t sting;                       /* STING_S index */
    uint8_t rsv;
    uint32_t seed;                       /* the last sting's */
} settings_t;
static settings_t set;
static uint8_t set_dirty;

static void settings_defaults(void)
{
    set.magic = SET_MAGIC;
    set.par = *bl_par();                 /* the engine's own defaults (bl_init) */
    set.octave = 0;
    set.length = 4;                      /* 20 minutes: a video's length */
    set.sting = 1;                       /* 5 s */
    set.seed = 1;
}

static int f01(float v) { return v >= 0.0f && v <= 1.0f; }   /* (false for a NaN) */
static int settings_ok(const settings_t *s)
{
    const bl_par_t *p = &s->par;
    return s->magic == SET_MAGIC && p->room >= 0 && p->room < 7 && p->mode >= 0 && p->mode < BM_NMODES &&
           (p->just | p->a432 | p->talk | p->tape) <= 1 && p->just >= 0 && p->a432 >= 0 && p->talk >= 0 &&
           p->tape >= 0 && p->solo >= 0 && p->solo < BV_NTYPES && f01(p->activity) && f01(p->drift) &&
           f01(p->bed) && f01(p->melody) && f01(p->solo_level) && f01(p->space) && f01(p->echo) &&
           f01(p->shimmer) && f01(p->tone) && f01(p->wear) && f01(p->hiss) && f01(p->level) &&
           s->octave >= -2 && s->octave <= 2 && s->length < NLENGTHS && s->sting < NSTINGS;
}

/* ------------------------------------------------------------------- audio --- */
static float gain_now;
static volatile uint8_t panicked;
static volatile uint32_t load_pk;                /* the longest render since HOME was pressed, in % of a block */

void app_overload(void) { bl_shed(); }

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float l[BL_BLOCK], r[BL_BLOCK];
    float gain = (float)master_q12 * (8300000.0f / 4096.0f), g = gain_now, dg;
    uint32_t i;
    if (n > BL_BLOCK)
        n = BL_BLOCK;
    if (panicked) {
        for (i = 0; i < 2u * n; i++)
            lr[i] = 0;
        return;
    }
    {
        uint32_t t0 = plat_ticks24(), pct;
        bl_render(l, r, (int)n);
        pct = (plat_ticks24() - t0) * 100u / (n * (PLAT_TICKS_PER_US * 1000000u / 44100u));
        if (pct > load_pk)
            load_pk = pct;
    }
    dg = (gain - g) / (float)n * 0.25f;  /* MASTER glides over a few blocks: no step */
    for (i = 0; i < n; i++) {
        g += dg;
        lr[2 * i] = (int32_t)(l[i] * g);
        lr[2 * i + 1] = (int32_t)(r[i] * g);
    }
    gain_now = g;
}

void app_panic(void)
{
    panicked = 1;
}

void app_init(void)
{
    bl_init(plat_ticks24() | 1u);
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok(&set))
        settings_defaults();
    set.par.talk = 0;                    /* TALK and the solo's held notes start off at power-on */
    *bl_par() = set.par;
    bl_par()->minutes = LENGTHS[set.length];
}

/* ------------------------------------------------------------------- pages --- */
enum { K_F, K_ROOM, K_MODE, K_BOOL };
typedef struct {
    const char *name;
    uint8_t kind;
    uint8_t off;                         /* offsetof bl_par_t */
} knob_t;
#define KF(n, f) {n, K_F, (uint8_t)__builtin_offsetof(bl_par_t, f)}
#define KI(n, k, f) {n, k, (uint8_t)__builtin_offsetof(bl_par_t, f)}
enum { PG_HOME, PG_GROVE, PG_SPACE, PG_TAPE, PG_TUNING, NPAGES };
static const char *const PAGE_NAME[NPAGES] = {"HOME", "GROVE", "SPACE", "TAPE", "TUNING"};
static const uint8_t PAGE_BTN[NPAGES] = {B_HOME, B_LFO, B_FX, B_ENV, B_EDIT};
static const knob_t PAGES[NPAGES][4] = {
    {KF("BED", bed), KF("SOLO", solo_level), KF("SPACE", space), KF("WEAR", wear)},
    {KF("ACTIVITY", activity), KF("DRIFT", drift), KF("BED", bed), KF("MELODY", melody)},
    {KF("SPACE", space), KF("ECHO", echo), KF("SHIMMER", shimmer), KF("TONE", tone)},
    {KF("WEAR", wear), KF("HISS", hiss), KI("TAPE", K_BOOL, tape), KF("LEVEL", level)},
    {KI("ROOM", K_ROOM, room), KI("MODE", K_MODE, mode), KI("JUST", K_BOOL, just), KI("A432", K_BOOL, a432)},
};

static float *kf(const knob_t *k) { return (float *)((char *)bl_par() + k->off); }
static int *ki(const knob_t *k) { return (int *)((char *)bl_par() + k->off); }

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void knob_value(const knob_t *k, char *t)
{
    switch (k->kind) {
    case K_F:
        int_text((int32_t)(*kf(k) * 100.0f + 0.5f), t);
        break;
    case K_ROOM:
        t[0] = 0;
        text_cat(t, bl_room_key(*ki(k)));
        text_cat(t, " ");
        text_cat(t, bl_room_name(*ki(k)));
        break;
    case K_MODE:
        t[0] = 0;
        text_cat(t, bl_mode_name(*ki(k)));
        break;
    default:
        t[0] = 0;
        text_cat(t, *ki(k) ? "ON" : "OFF");
    }
}
static float knob_frac(const knob_t *k)
{
    switch (k->kind) {
    case K_F: return *kf(k);
    case K_ROOM: return (float)*ki(k) / 6.0f;
    case K_MODE: return (float)*ki(k) / (float)(BM_NMODES - 1);
    default: return (float)*ki(k);
    }
}

/* ---------------------------------------------------------------------- UI --- */
/* the screen's looks (the sky's area): HOME held and SELECT turned */
enum { LOOK_REDWOOD, LOOK_SPECTRUM, LOOK_JCARD, NLOOKS };
static const char *const LOOK_NAME[NLOOKS] = {"REDWOOD", "SPECTRUM", "J-CARD"};

static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    int32_t key_note[NKEYS];             /* the note each held key started (its off finds it after an octave change) */
    uint8_t page;
    uint8_t redraw;                      /* header and footer */
    uint32_t sky_at;                     /* the sky's last frame */
    uint32_t last_sec, last_ev;
    int turned;                          /* the knob last turned (lit a moment), -1 none */
    uint32_t turned_at;
    uint8_t look;                        /* LOOK_*: HOME held and SELECT turned */
} ui;

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1400u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void dirty(void)
{
    set_dirty = 1;
    ui.redraw = 1;
}

static void knob_turn(int i, int32_t e)
{
    const knob_t *k = &PAGES[ui.page][i];
    char t[24];
    switch (k->kind) {
    case K_F:
        *kf(k) = fm_clampf(*kf(k) + 0.02f * (float)e, 0.0f, 1.0f);
        break;
    case K_ROOM:
        *ki(k) = (int)clamp(*ki(k) + e, 0, 6);
        break;
    case K_MODE:
        *ki(k) = (int)clamp(*ki(k) + e, 0, BM_NMODES - 1);
        break;
    default:
        *ki(k) = e > 0;
    }
    ui.turned = i;
    ui.turned_at = plat_ms();
    knob_value(k, t);
    app_say(k->name, t);
    dirty();
}

static void solo_all_off(void)
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++)
        if (ui.keys >> k & 1u)
            bl_post_note(ui.key_note[k], 0.0f);
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave)
        return;
    set.octave = (int8_t)o;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
    dirty();
}

static void sting_go(uint32_t seed)
{
    char t[24];
    set.seed = seed;
    bl_post_sting(STING_S[set.sting], seed);
    int_text((int32_t)STING_S[set.sting], t);
    text_cat(t, "S  #");
    {
        char s[12];
        int_text((int32_t)(seed % 10000u), s);
        text_cat(t, s);
    }
    app_say("STING", t);
    dirty();
}

static void button(uint32_t k)
{
    const bl_status_t *st = bl_status();
    uint32_t p;
    for (p = 0; p < NPAGES; p++)
        if (PAGE_BTN[p] == k) {
            ui.page = (uint8_t)p;
            app_say(PAGE_NAME[p], "");
            return;
        }
    switch (k) {
    case B_PLAY:
        if (st->grove == 1) {
            bl_post_grove(0);
            app_say("GROVE", "CLOSING");
        } else {
            bl_post_grove(1);
            app_say("GROVE", LENGTHS[set.length] ? "BEGINS" : "ENDLESS");
        }
        break;
    case B_REC: {
        uint32_t s = plat_ticks24() ^ (plat_ms() * 2654435761u);
        sting_go((s >> 7) % 9999u + 1u);
        break;
    }
    case B_SEQ:
        sting_go(set.seed);
        break;
    case B_ARP: {
        char t[8];
        set.sting = (uint8_t)((set.sting + 1u) % NSTINGS);
        int_text((int32_t)STING_S[set.sting], t);
        text_cat(t, " S");
        app_say("STING LENGTH", t);
        dirty();
        break;
    }
    case B_GLO:
        bl_par()->talk ^= 1;
        app_say("TALK", bl_par()->talk ? "ON" : "OFF");
        ui.redraw = 1;
        break;
    case B_SEL:
        bl_par()->tape ^= 1;
        app_say("TAPE", bl_par()->tape ? "IN" : "OUT");
        dirty();
        break;
    case B_SAVE:
        set.par = *bl_par();
        if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
            set_dirty = 0;
            app_say("SAVED", "");
        } else {
            app_say("SAVE", "FAILED");
        }
        break;
    case B_OCTDN:
        octave_step(-1);
        break;
    case B_OCTUP:
        octave_step(1);
        break;
    default:
        break;
    }
}

static void midi_in(void)                /* the main loop: the engine's events have one producer */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if ((st & 0xF0u) == 0x90u && d2)
            bl_post_note((int)d1, (float)d2 / 127.0f);
        else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u)
            bl_post_note((int)d1, 0.0f);
        else if ((st & 0xF0u) == 0xB0u && (d1 == 123u || d1 == 120u))
            bl_post_panic();
    }
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k, lit = 0;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {        /* key 0 is F3 (MIDI 53) */
        uint32_t bit = 1u << k;
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            int32_t note = 53 + (int32_t)k + 12 * set.octave;
            ui.key_note[k] = note;
            bl_post_note(note, (float)(v ? v : 1u) / 127.0f);
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16 | v << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit)) {
            bl_post_note(ui.key_note[k], 0.0f);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)ui.key_note[k] << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++)
        if ((btn >> k & 1u) && !(ui.btn >> k & 1u))
            button(k);
    for (k = 0; k < 4u; k++)
        if ((e = plat_enc((int)(EN_K1 + k))) != 0)
            knob_turn((int)k, e);
    if ((e = plat_enc(EN_ALGO)) != 0) {
        char t[24];
        bl_par()->room = (int)clamp(bl_par()->room + e, 0, 6);
        t[0] = 0;
        text_cat(t, bl_room_key(bl_par()->room));
        text_cat(t, " ");
        text_cat(t, bl_room_name(bl_par()->room));
        app_say("ROOM", t);
        dirty();
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        solo_all_off();                  /* (a held note's off goes to the voice that played it) */
        bl_par()->solo = (int)((bl_par()->solo + e + 9 * BV_NTYPES) % BV_NTYPES);
        app_say("VOICE", bl_voice_name(bl_par()->solo));
        dirty();
    }
    if ((e = plat_enc(EN_SELECT)) != 0 && (btn >> B_HOME & 1u)) {
        ui.look = (uint8_t)((ui.look + e + 3 * NLOOKS) % NLOOKS);
        app_say("LOOK", LOOK_NAME[ui.look]);
    } else if (e != 0) {
        char t[16];
        set.length = (uint8_t)clamp((int32_t)set.length + e, 0, (int32_t)NLENGTHS - 1);
        bl_par()->minutes = LENGTHS[set.length];
        if (LENGTHS[set.length]) {
            int_text(LENGTHS[set.length], t);
            text_cat(t, " MIN");
        } else {
            t[0] = 0;
            text_cat(t, "ENDLESS");
        }
        app_say("PIECE", t);
        dirty();
    }
    midi_in();
    if ((btn & ~ui.btn) >> B_HOME & 1u)
        load_pk = 0;                     /* HOME pressed: the peak afresh */
    ui.keys = keys;
    ui.btn = btn;
    lit = 1u << PAGE_BTN[ui.page];
    if (bl_par()->talk)
        lit |= 1u << B_GLO;
    if (bl_par()->tape)
        lit |= 1u << B_SEL;
    if (set_dirty)
        lit |= 1u << B_SAVE;
    if (bl_status()->grove)
        lit |= 1u << B_PLAY;
    if (bl_status()->sting_len)
        lit |= 1u << B_REC;
    {                                    /* the lights only when they change: the main loop spins fast */
        static uint32_t was_b = ~0u, was_k = ~0u;
        static int was_red = -1;
        int red = bl_status()->grove == 2;
        if ((btn | lit) != was_b || keys != was_k || red != was_red) {
            plat_play_red(red);
            plat_glow(1);
            plat_leds(btn | lit, keys);
            was_b = btn | lit;
            was_k = keys;
            was_red = red;
        }
    }
}

void app_input_only(void) { input(); }

/* ------------------------------------------------------------------ screen --- */
/* the sky: a gradient by season, a sun that breathes with the music, redwoods, and a spark for each note */
typedef struct { uint8_t top[3], mid[3], low[3], sun[3]; } sky_t;
static const sky_t SKY[BS_NSEASONS + 1] = {
    {{38, 30, 92}, {196, 120, 168}, {252, 176, 128}, {255, 226, 170}},   /* DAWN */
    {{24, 18, 64}, {120, 64, 140}, {236, 132, 150}, {255, 200, 190}},    /* TEMPLE */
    {{48, 104, 176}, {150, 170, 210}, {246, 212, 168}, {255, 246, 210}}, /* GARDEN */
    {{4, 4, 22}, {22, 18, 70}, {70, 44, 110}, {220, 220, 255}},          /* STARS */
    {{30, 40, 78}, {98, 112, 160}, {170, 170, 200}, {240, 236, 255}},    /* STILL */
    {{16, 56, 92}, {60, 140, 160}, {150, 214, 200}, {230, 255, 240}},    /* RIVER */
    {{6, 6, 26}, {30, 20, 66}, {84, 46, 96}, {200, 150, 190}},           /* (resting) */
};
static float sky_now[4][3];
static float sun_r = 16.0f;

/* the redwoods on the horizon: x, height, half-width at the foot */
static const uint8_t TREES[][3] = {
    {8, 70, 9}, {26, 96, 11}, {40, 58, 8}, {58, 84, 10}, {176, 64, 8}, {192, 104, 12}, {210, 76, 9},
    {226, 92, 10}, {238, 52, 7}, {120, 30, 6}, {98, 40, 6}, {146, 36, 6},
};
#define NTREES (sizeof TREES / sizeof TREES[0])

#define NSPARK 24
static struct { int16_t x, y; uint32_t t; uint8_t layer; } spark[NSPARK];
static uint32_t spark_i;

static uint16_t mix3(const float *a, const float *b, float f)
{
    return RGB((uint32_t)(a[0] + (b[0] - a[0]) * f), (uint32_t)(a[1] + (b[1] - a[1]) * f),
               (uint32_t)(a[2] + (b[2] - a[2]) * f));
}

static void sky_row_rgb(int32_t y, float *c)    /* y 0..143 */
{
    float f;
    int i;
    if (y < 96) {
        f = (float)y / 96.0f;
        for (i = 0; i < 3; i++)
            c[i] = sky_now[0][i] + (sky_now[1][i] - sky_now[0][i]) * f;
    } else {
        f = (float)(y - 96) / 48.0f;
        for (i = 0; i < 3; i++)
            c[i] = sky_now[1][i] + (sky_now[2][i] - sky_now[1][i]) * f;
    }
}

/* the notes since the last frame: a spark each, and a ring of the Spectrum look lit by its pitch */
static float ring_glow[7];
static void take_events(uint32_t now)
{
    const bl_status_t *st = bl_status();
    static const uint8_t PC_RING[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
    while (ui.last_ev != st->ev_count) {
        uint32_t e = ui.last_ev++ % 16u;
        int note = st->ev[e].note;
        spark[spark_i % NSPARK].x = (int16_t)clamp((int32_t)((note - 36) * 240 / 60), 4, 235);
        spark[spark_i % NSPARK].y = (int16_t)(20 + (int32_t)((st->ev[e].layer * 37u + (uint32_t)note * 11u) % 70u));
        spark[spark_i % NSPARK].t = now;
        spark[spark_i % NSPARK].layer = st->ev[e].layer;
        spark_i++;
        ring_glow[PC_RING[((note % 12) + 12) % 12]] = 1.0f;
    }
}

static void sky_turn(int s)              /* the sky turns slowly to the season's colours (about 6 s) */
{
    const sky_t *k = &SKY[s];
    const uint8_t *tgt[4] = {k->top, k->mid, k->low, k->sun};
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 3; j++)
            sky_now[i][j] += ((float)tgt[i][j] - sky_now[i][j]) * 0.012f;
}

static void draw_redwood(uint32_t now)
{
    const bl_status_t *st = bl_status();
    int s = st->grove || st->sting_len ? st->season : BS_NSEASONS, i, j;
    int32_t y, cy = 118, cx = 120;
    float rr, glow = 0.0f;
    (void)j;
    sky_turn(s);
    rr = 14.0f + 16.0f * st->energy;     /* the sun breathes with the music */
    sun_r += (rr - sun_r) * 0.08f;
    glow = sun_r * 1.9f;
    cv_begin(240, 144, C_BLACK);
    for (y = 0; y < 144; y++) {
        float c[3], dy = (float)(y - cy), g2 = glow * glow - dy * dy, s2 = sun_r * sun_r - dy * dy;
        sky_row_rgb(y, c);
        cv_rect(0, y, 240, 1, RGB((uint32_t)c[0], (uint32_t)c[1], (uint32_t)c[2]));
        if (g2 > 0.0f) {                 /* the halo, then the disc: spans of each row */
            int32_t w = (int32_t)fm_sqrtf(g2);
            cv_rect(cx - w, y, 2 * w, 1, mix3(c, sky_now[3], 0.22f));
            w = (int32_t)(fm_sqrtf(g2) * 0.72f);
            cv_rect(cx - w, y, 2 * w, 1, mix3(c, sky_now[3], 0.42f));
        }
        if (s2 > 0.0f) {
            int32_t w = (int32_t)fm_sqrtf(s2);
            cv_rect(cx - w, y, 2 * w, 1, mix3(c, sky_now[3], 0.94f));
        }
    }
    if (s == BS_STARS || s == BS_NSEASONS) {   /* stars: fixed, twinkling */
        uint32_t h = 0x9E3779B9u;
        for (i = 0; i < 40; i++) {
            int32_t x, yy;
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            x = (int32_t)(h % 240u);
            yy = (int32_t)((h >> 8) % 90u);
            if (((h >> 20) + now / 400u) % 7u)
                cv_pset(x, yy, (h >> 3) & 1u ? C_WHITE : RGB(170, 170, 220));
        }
    }
    take_events(now);
    for (i = 0; i < NSPARK; i++) {
        uint32_t age = now - spark[i].t;
        int32_t x = spark[i].x, yy, r;
        uint16_t c;
        if (!spark[i].t || age > 3000u)
            continue;
        yy = spark[i].y - (int32_t)(age / 120u);
        r = age < 600u ? 3 : age < 1600u ? 2 : 1;
        c = age < 1200u ? C_WHITE : RGB(255, 230, 200);
        cv_rect(x - r, yy, 2 * r + 1, 1, c);
        cv_rect(x, yy - r, 1, 2 * r + 1, c);
    }
    /* the redwoods, against the light */
    for (i = 0; i < (int)NTREES; i++) {
        int32_t x = TREES[i][0], h = TREES[i][1], hw = TREES[i][2], top = 144 - h;
        for (y = top; y < 144; y++) {
            int32_t w = 1 + (hw * (y - top)) / h;
            cv_rect(x - w, y, 2 * w + 1, 1, RGB(14, 12, 26));
        }
    }
    cv_rect(0, 138, 240, 6, RGB(14, 12, 26));
    cv_blit(0, 32);
}

/* --- SPECTRUM: seven rings, root to crown from the outside in, each lit by the notes that fall on it; the room's
 * own ring a little brighter; the whole breathing with the music */
static const uint8_t CHAKRA[7][3] = {
    {226, 74, 84}, {244, 142, 74}, {244, 206, 96}, {104, 200, 132}, {86, 166, 232}, {112, 100, 214}, {196, 128, 232},
};
static void disc_rows(int32_t cx, int32_t cy, float r, uint16_t c)
{
    int32_t y, y0 = cy - (int32_t)r, y1 = cy + (int32_t)r;
    for (y = y0 < 0 ? 0 : y0; y <= y1 && y < 144; y++) {
        float dy = (float)(y - cy), w2 = r * r - dy * dy;
        if (w2 > 0.0f) {
            int32_t w = (int32_t)(fm_sqrtf(w2) + 0.5f);
            cv_rect(cx - w, y, 2 * w + 1, 1, c);
        }
    }
}

static void draw_spectrum(uint32_t now)
{
    const bl_status_t *st = bl_status();
    int playing = st->grove || st->sting_len, k, room = bl_par()->room;
    float breath = 0.94f + 0.08f * st->energy + 0.012f * fm_sinf((float)(now % 60000u) * 0.0011f);
    static const float BG[3] = {10, 8, 24};
    take_events(now);
    cv_begin(240, 144, RGB(10, 8, 24));
    {                                                            /* a few stars, twinkling */
        uint32_t h = 0x2545F491u;
        for (k = 0; k < 36; k++) {
            h ^= h << 13; h ^= h >> 17; h ^= h << 5;
            if (((h >> 20) + now / 500u) % 6u)
                cv_pset((int32_t)(h % 240u), (int32_t)((h >> 8) % 144u), (h >> 3) & 1u ? RGB(120, 110, 170) : RGB(200, 196, 240));
        }
    }
    for (k = 0; k < 7; k++) {
        float c[3], lit = playing ? 0.52f + 0.48f * ring_glow[k] : 0.3f, r = (70.0f - 9.4f * (float)k) * breath;
        int j;
        if (k == room)
            lit = fm_maxf(lit, playing ? 0.8f : 0.5f);
        for (j = 0; j < 3; j++)
            c[j] = (float)CHAKRA[k][j];
        disc_rows(120, 72, r, mix3(BG, c, lit * 0.25f));          /* a dark gap, then the band */
        disc_rows(120, 72, r - 1.5f, mix3(BG, c, lit));
        ring_glow[k] *= 0.93f;
    }
    disc_rows(120, 72, 8.0f * breath, RGB(255, 244, 230));        /* the bindu */
    cv_blit(0, 32);
}

/* --- J-CARD: the cassette itself, on an airbrushed sky over a grid that runs to the horizon; the reels turn
 * while it plays and the tape winds from left to right through a timed piece */
static void reel(int32_t cx, int32_t cy, float pack, float ang)
{
    int k;
    disc_rows(cx, cy, pack, RGB(74, 48, 44));                    /* the tape wound on it */
    disc_rows(cx, cy, 7.0f, RGB(236, 230, 220));                 /* the hub */
    disc_rows(cx, cy, 3.0f, RGB(30, 24, 40));
    for (k = 0; k < 6; k++) {                                    /* its teeth */
        float a = ang + (float)k * 1.0472f;
        cv_rect(cx + (int32_t)(5.0f * fm_cosf(a)), cy + (int32_t)(5.0f * fm_sinf(a)), 1, 1, RGB(30, 24, 40));
    }
}

static void draw_jcard(uint32_t now)
{
    const bl_status_t *st = bl_status();
    int s = st->grove || st->sting_len ? st->season : BS_NSEASONS, playing = st->grove || st->sting_len;
    int32_t y, i;
    float prog, ang = playing ? (float)(now % 100000u) * 0.0021f : 0.0f;
    static const float GRID[3] = {246, 150, 210};
    take_events(now);
    sky_turn(s);
    cv_begin(240, 144, C_BLACK);
    for (y = 0; y < 90; y++) {                                   /* the sky, to the horizon */
        float c[3];
        sky_row_rgb(y * 144 / 90, c);
        cv_rect(0, y, 240, 1, RGB((uint32_t)c[0], (uint32_t)c[1], (uint32_t)c[2]));
    }
    {                                                            /* the floor: lines that close in on the horizon */
        static const float FLOOR[3] = {28, 14, 52};
        float sh = (float)(now % 4000u) / 4000.0f;
        cv_rect(0, 90, 240, 54, RGB(28, 14, 52));
        for (i = 0; i < 9; i++) {
            float z = ((float)i + 1.0f - (playing ? sh : 0.0f)) / 9.0f;
            int32_t yy = 90 + (int32_t)(54.0f * z * z);
            cv_rect(0, yy, 240, 1, mix3(FLOOR, GRID, 0.25f + 0.6f * z));
        }
        for (i = -7; i <= 7; i++)
            cv_line(120 + i * 6, 90, 120 + i * 40, 143, mix3(FLOOR, GRID, 0.55f));
        cv_rect(0, 90, 240, 1, mix3(FLOOR, GRID, 0.9f));
    }
    /* the shell, the label and its window */
    cv_rect(36, 18, 168, 92, RGB(26, 22, 34));
    cv_rect(38, 20, 164, 88, RGB(40, 34, 52));
    cv_rect(46, 26, 148, 58, RGB(240, 232, 214));
    for (i = 0; i < 7; i++)                                      /* a band of the room's colours across the label */
        cv_rect(46, 28 + i * 3, 148, 3, RGB(CHAKRA[i][0], CHAKRA[i][1], CHAKRA[i][2]));
    cv_text(52, 51, &FONT_XS, "BLISS BOX", RGB(60, 44, 80));
    cv_text(150, 51, &FONT_XS, playing ? "SIDE A" : "SIDE B", RGB(150, 120, 150));
    cv_rect(72, 64, 96, 32, RGB(22, 18, 28));
    prog = st->grove && bl_par()->minutes ? fm_clampf((float)st->seconds / (60.0f * (float)bl_par()->minutes), 0, 1)
           : st->sting_len ? st->sting_pos : 0.35f;
    reel(96, 80, 8.0f + 7.0f * (1.0f - prog), ang);
    reel(144, 80, 8.0f + 7.0f * prog, ang * 1.3f);
    cv_blit(0, 32);
}

static void draw_sky(uint32_t now)
{
    if (ui.look == LOOK_SPECTRUM)
        draw_spectrum(now);
    else if (ui.look == LOOK_JCARD)
        draw_jcard(now);
    else
        draw_redwood(now);
}

static void time_text(uint32_t s, char *t)
{
    char n[12];
    t[0] = 0;
    int_text((int32_t)(s / 60u), n);
    text_cat(t, n);
    text_cat(t, s % 60u < 10u ? ":0" : ":");
    int_text((int32_t)(s % 60u), n);
    text_cat(t, n);
}

static void draw_header(uint32_t now)
{
    const bl_status_t *st = bl_status();
    char t[32];
    cv_begin(240, 32, C_BLACK);
    if (ui.btn >> B_HOME & 1u) {         /* HOME held: the audio's load and its xruns since boot */
        char n[12];
        t[0] = 0;
        text_cat(t, "DSP ");
        int_text((int32_t)plat_cpu_pct(), n);
        text_cat(t, n);
        text_cat(t, "% PK ");
        int_text((int32_t)load_pk, n);
        text_cat(t, n);
        text_cat(t, "% X ");
        int_text((int32_t)plat_xruns(), n);
        text_cat(t, n);
        text_cat(t, "  V ");
        int_text((int32_t)st->cpu_voices, n);
        text_cat(t, n);
        cv_text(8, 6, &FONT_B, t, C_WHITE);
    } else if (now < ui.msg_until) {
        int32_t x = cv_text(8, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], RGB(246, 170, 140));
    } else {
        int32_t x = cv_text(8, 6, &FONT_B, "BLISS BOX", RGB(246, 196, 170));
        if (bl_par()->talk)
            x = cv_text(x + 8, 10, &FONT_XS, "TALK", RGB(150, 210, 255));
        if (!bl_par()->tape)
            cv_text(x + 8, 10, &FONT_XS, "NO TAPE", C_DIM);
        if (st->sting_len) {
            t[0] = 0;
            text_cat(t, "STING ");
            int_text(st->sting_len, t + 6);
            text_cat(t, "S");
            cv_text(232 - text_w(&FONT_S, t), 8, &FONT_S, t, C_WHITE);
            cv_rect(150, 28, (int32_t)(82.0f * st->sting_pos), 2, RGB(246, 170, 140));
        } else if (st->grove) {
            time_text(st->seconds, t);
            if (LENGTHS[set.length]) {
                char m[8];
                int_text(LENGTHS[set.length], m);
                text_cat(t, " / ");
                text_cat(t, m);
                text_cat(t, ":00");
            }
            cv_text(232 - text_w(&FONT_S, t), 8, &FONT_S, t, st->grove == 2 ? RGB(246, 170, 140) : C_WHITE);
        } else {
            int_text((int32_t)plat_cpu_pct(), t);
            text_cat(t, "%");
            cv_text(232 - text_w(&FONT_XS, t), 10, &FONT_XS, t, C_DIM);
        }
    }
    cv_blit(0, 0);
}

static void draw_footer(uint32_t now)
{
    const bl_status_t *st = bl_status();
    char t[32];
    int32_t x, i;
    cv_begin(240, 64, C_BLACK);
    /* the voice and octave on the right; on the left the room's key, then what fits of the chord, the season,
     * the room's name and the mode (in that order of need), drawn in reading order */
    t[0] = 0;
    text_cat(t, bl_voice_name(bl_par()->solo));
    text_cat(t, set.octave < 0 ? " -" : set.octave > 0 ? " +" : "");
    if (set.octave) {
        char o[4] = {(char)('0' + (set.octave < 0 ? -set.octave : set.octave)), 0};
        text_cat(t, o);
    }
    i = 232 - text_w(&FONT_XS, t);
    cv_text(i, 6, &FONT_XS, t, RGB(246, 170, 140));
    {
        int playing = st->grove || st->sting_len;
        const char *name[4] = {playing ? st->chord_name : 0, playing ? bl_season_name(st->season) : 0,
                               bl_room_name(bl_par()->room), bl_mode_name(bl_par()->mode)};
        int show[4] = {0, 0, 0, 0}, k;
        int32_t room = i - 8;                         /* the left group ends 8 px before the voice */
        x = 8 + text_w(&FONT_B, bl_room_key(bl_par()->room));
        for (k = 0; k < 4; k++)
            if (name[k] && x + 8 + text_w(&FONT_XS, name[k]) <= room) {
                show[k] = 1;
                x += 8 + text_w(&FONT_XS, name[k]);
            }
        x = cv_text(8, 2, &FONT_B, bl_room_key(bl_par()->room), RGB(246, 196, 170));
        if (show[2])
            x = cv_text(x + 6, 6, &FONT_XS, name[2], C_GRAY);
        if (show[3])
            x = cv_text(x + 6, 6, &FONT_XS, name[3], C_DIM);
        if (show[0])
            x = cv_text(x + 8, 6, &FONT_XS, name[0], C_WHITE);
        if (show[1])
            cv_text(x + 8, 6, &FONT_XS, name[1], RGB(150, 210, 255));
    }
    /* the page's four knobs */
    cv_rect(0, 24, 240, 1, C_LINE);
    for (i = 0; i < 4; i++) {
        const knob_t *k = &PAGES[ui.page][i];
        int32_t cx = 4 + i * 59;
        int lit = ui.turned == i && now - ui.turned_at < 600u;
        knob_value(k, t);
        cv_text(cx, 30, &FONT_XS, k->name, lit ? C_WHITE : C_GRAY);
        cv_rect(cx, 44, 54, 3, C_LINE);
        cv_rect(cx, 44, (int32_t)(54.0f * knob_frac(k)), 3, lit ? C_WHITE : RGB(246, 170, 140));
        cv_text(cx, 50, &FONT_XS, t, lit ? C_WHITE : C_DIM);
    }
    cv_blit(0, 176);
}

static void speak(void)
{
    char t[96];
    if (ann_on() && ann_where()) {
        const bl_status_t *st = bl_status();
        t[0] = 0;
        text_cat(t, "BLISS BOX. ROOM ");
        text_cat(t, bl_room_key(bl_par()->room));
        text_cat(t, ". VOICE ");
        text_cat(t, bl_voice_name(bl_par()->solo));
        text_cat(t, st->grove ? ". GROVE PLAYING" : ". GROVE RESTING");
        text_cat(t, ". PAGE ");
        text_cat(t, PAGE_NAME[ui.page]);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

void app_ui_init(void)
{
    int i, j;
    touch_on(1);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 3; j++)
            sky_now[i][j] = (float)(i == 0 ? SKY[BS_NSEASONS].top[j] : i == 1 ? SKY[BS_NSEASONS].mid[j]
                                    : i == 2 ? SKY[BS_NSEASONS].low[j] : SKY[BS_NSEASONS].sun[j]);
    ui.turned = -1;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
}

void app_frame(void)
{
    uint32_t now = plat_ms();
    const bl_status_t *st = bl_status();
    input();
    speak();
    if (st->seconds != ui.last_sec || (ui.msg_until && now >= ui.msg_until) ||
        (ui.turned >= 0 && now - ui.turned_at > 600u && now - ui.turned_at < 700u) || st->sting_len ||
        (ui.btn >> B_HOME & 1u)) {
        ui.last_sec = st->seconds;
        if (ui.msg_until && now >= ui.msg_until)
            ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (now - ui.sky_at >= 50u) {        /* the sky at 20 frames a second */
        ui.sky_at = now;
        draw_sky(now);
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw_header(now);
        draw_footer(now);
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    const bl_status_t *st = bl_status();
    if (!strcmp(what, "grove"))
        *got = st->grove;
    else if (!strcmp(what, "sting"))
        *got = st->sting_len;
    else if (!strcmp(what, "room"))
        *got = bl_par()->room;
    else if (!strcmp(what, "solo"))
        *got = bl_par()->solo;
    else if (!strcmp(what, "talk"))
        *got = bl_par()->talk;
    else if (!strcmp(what, "tape"))
        *got = bl_par()->tape;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "voices"))
        *got = (int)st->cpu_voices;
    else
        return 0;
    return 1;
}
#endif
