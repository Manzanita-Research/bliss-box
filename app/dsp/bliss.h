/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* The Bliss Box sound engine: music without tension, after Steven Halpern, Iasos, Don Slepian and the 1980s
 * new age cassette. Portable C (no double, no libc maths), the same on the FM-1, in the host simulator, in the
 * browser and in tools/render (offline WAVs).
 *
 * Threading: bl_render runs in the audio interrupt. Everything else (bl_post_*, bl_par) is called from the main
 * loop; events go through a single-producer, single-consumer queue, parameters are plain floats read once a
 * block. */
#pragma once
#include <stdint.h>

#define BL_SR 44100
#define BL_BLOCK 256

/* the voices you can play (and the Grove's layers use) */
enum {
    BV_RHODES,   /* tine electric piano, a little bark when struck hard */
    BV_CP80,     /* electric grand: strings and pickups, chorused */
    BV_FLUTE,    /* breathy, swelled like a volume pedal into an Echoplex */
    BV_HARP,     /* plucked string */
    BV_ZITHER,   /* doubled, brighter strings: the autoharp of Day of Radiance */
    BV_BELLS,    /* DX7-ish glass and tubular bells */
    BV_STRINGS,  /* string ensemble pad: detuned saws, ensemble chorus */
    BV_CHOIR,    /* vowel pad: oo, oh, ah */
    BV_GLASS,    /* crystal FM pad */
    BV_NTYPES
};

/* scales (the room's mode) */
enum { BM_PENTA, BM_LYDIAN, BM_MAJOR, BM_DORIAN, BM_NMODES };

/* the moods: each one a key, a scale, its progressions and the voices of its four parts (BS_: they grew from the
 * Grove's old seasons, and the screen still paints one sky for each) */
enum { BS_DAWN, BS_TEMPLE, BS_REDWOODS, BS_STARS, BS_STILL, BS_RIVER, BS_NSEASONS };
#define BL_NMOODS BS_NSEASONS

typedef struct {
    /* harmony */
    int room;          /* 0..6: Halpern's chakra keys C D E F G A B (root .. crown) */
    int just;          /* 0 equal temperament, 1 just intonation on the room's root */
    int a432;          /* 0 A=440, 1 A=432 */
    /* the Grove */
    int mood;          /* BS_*: the key's default (bl_mood_room), the scale, the progressions, the voices */
    float activity;    /* 0..1: how busy the motion and the melody are, and how fast the chords change */
    float drift;       /* 0..1: how far each pass through a progression strays (a chord's colour, another progression) */
    float bloom;       /* 0..1: how much the sounds open and close (filters, FM brightness, the choir's vowel) */
    /* the four parts' levels */
    float bed;         /* 0..1: the pad (the chords) */
    float low;         /* 0..1: the low part (the bass under the chords) */
    float motion;      /* 0..1: the rippling part (harp, zither, Rhodes, CP-80 or bells) */
    float melody;      /* 0..1: the melody (flute or bells) */
    int minutes;       /* 0 endless, else the piece ends itself after this many minutes */
    /* the solo voice (the keys) */
    int solo;          /* BV_* */
    float solo_level;  /* 0..1 */
    /* space */
    float space;       /* 0..1: hall size and mix */
    float echo;        /* 0..1: tape echo amount */
    float shimmer;     /* 0..1: chorus depth on pads and keys */
    float tone;        /* 0..1: dark .. bright */
    /* the master: talk and cassette */
    int talk;          /* 1: a dip at 1-4 kHz and a little less, for speech over the bed */
    int tape;          /* 1: the cassette is in */
    float wear;        /* 0 fresh chrome .. 1 thrift-store */
    float hiss;        /* 0..1, scaled by wear */
    float level;       /* 0..1 output level before MASTER */
} bl_par_t;

/* what the screen shows */
typedef struct {
    int grove;            /* 1 playing, 2 closing (a timed piece's ending), 0 stopped */
    int season;           /* BS_*: the mood playing (resting: the one chosen; a sting: its own) */
    int section;          /* the Grove's shape: 0 arriving, 1 blooming, 2 floating, 3 returning */
    int root_pc;          /* the chord's root, 0..11 (C = 0): the bottom octave lights it; -1 none */
    float bloom;          /* 0..1 now */
    int room_now;         /* where the harmony is now (drift can move it away from home) */
    int chord[8];         /* the pad's notes now (MIDI), chord_n of them */
    int chord_n;
    char chord_name[16];  /* e.g. "Dadd9", "G/D" */
    uint32_t seconds;     /* since the Grove started */
    int sting_len;        /* 0 none playing, else its length in seconds */
    float sting_pos;      /* 0..1 */
    uint32_t sting_seed;
    float meter_l, meter_r;    /* peak, 0..1 */
    float energy;              /* a slow loudness, 0..1: the screen breathes with it */
    /* the last notes (a ring): for sparkles on the screen */
    uint32_t ev_count;
    struct { int8_t note; uint8_t type; uint8_t vel; uint8_t layer; } ev[16];
    float cpu_voices;     /* voices sounding */
    float cost;           /* what they cost, in the units of the voice budget (180) */
} bl_status_t;

void bl_init(uint32_t seed);
void bl_render(float *l, float *r, int n);          /* n <= BL_BLOCK; adds nothing, writes l and r */
bl_par_t *bl_par(void);                             /* the live parameters (main loop writes, audio reads) */
const bl_status_t *bl_status(void);

/* events from the main loop */
void bl_post_note(int midi, float vel);             /* the solo voice: vel 0 = off */
void bl_post_grove(int on);                         /* start (from silence) or stop (a long release) */
void bl_post_sting(int seconds, uint32_t seed);     /* 3, 5, 10 or 30: the same seed is the same sting */
void bl_post_panic(void);
void bl_post_steer(int pc);                         /* the bottom octave: the Grove to a chord on pc (0..11, C = 0)
                                                     * and there while held (and it starts the Grove); -1 lets go */
void bl_shed(void);                                 /* the render ran late: let a voice go (audio context) */

/* names */
const char *bl_voice_name(int v);
const char *bl_mode_name(int m);
const char *bl_room_name(int r);      /* "ROOT" .. "CROWN" */
const char *bl_room_key(int r);       /* "C" .. "B" */
const char *bl_season_name(int s);   /* the mood's name: "DAWN" .. */
int bl_mood_room(int m);              /* the mood's own key (a room, 0..6) */
int bl_mood_mode(int m);              /* and its scale (BM_*) */
float bl_note_hz(int midi);           /* in the current tuning */
