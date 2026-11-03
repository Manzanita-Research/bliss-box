/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* Offline renders of the engine to 24-bit WAV, for listening and for checks.
 *   cc -O2 -DBL_OFFLINE -I base/include -I app/dsp tools/render.c app/dsp/bliss.c -o build/render -lm
 *   build/render grove 600 out.wav [key=value ...]
 *   build/render sting 10 out.wav seed=1234
 *   build/render solo rhodes out.wav
 * Keys: mood room just a432 activity drift bloom bed low motion melody minutes space echo shimmer tone talk tape wear
 * hiss level seed steer=PC@SECONDS (the bottom octave held from then, -1 lets go; up to 16)
 * mood= sets the room to the mood's own key; give room= after it to move it.
 * It prints the peak, the RMS, the clipped-sample count and the voices' cost (the FM-1's budget is 180). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "bliss.h"

static FILE *wav;
static uint32_t frames;
static double sumsq, cost_sum;
static float cost_peak;
static long cost_n, cost_over;
static int logc;
static char last_chord[16];
static int last_sect = -1;
static float peak;
static long clipped;

static void w32(uint32_t v) { fputc(v & 255, wav); fputc((v >> 8) & 255, wav); fputc((v >> 16) & 255, wav); fputc(v >> 24, wav); }
static void w16(uint16_t v) { fputc(v & 255, wav); fputc(v >> 8, wav); }

static void wav_open(const char *path)
{
    wav = fopen(path, "wb");
    if (!wav) {
        perror(path);
        exit(1);
    }
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, wav);
    w32(16); w16(1); w16(2); w32(44100); w32(44100 * 6); w16(6); w16(24);
    fwrite("data\0\0\0\0", 1, 8, wav);
}
static void wav_close(void)
{
    uint32_t bytes = frames * 6;
    fseek(wav, 4, SEEK_SET);
    w32(36 + bytes);
    fseek(wav, 40, SEEK_SET);
    w32(bytes);
    fclose(wav);
}
static void put(float x)
{
    int32_t v;
    float a = fabsf(x);
    if (a > peak)
        peak = a;
    if (a >= 0.999f)
        clipped++;
    sumsq += (double)x * x;
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    v = (int32_t)lrintf(x * 8388607.0f);
    fputc(v & 255, wav); fputc((v >> 8) & 255, wav); fputc((v >> 16) & 255, wav);
}

static void run(double seconds)
{
    float l[BL_BLOCK], r[BL_BLOCK];
    uint32_t n = (uint32_t)(seconds * 44100.0), done = 0;
    while (done < n) {
        int k = n - done < BL_BLOCK ? (int)(n - done) : BL_BLOCK, i;
        bl_render(l, r, k);
        cost_sum += bl_status()->cost;
        cost_n++;
        cost_over += bl_status()->cost > 180.0f;
        if (logc && (strcmp(last_chord, bl_status()->chord_name) || last_sect != bl_status()->section)) {
            static const char *SN[4] = {"arrive", "bloom", "float", "return"};
            const bl_status_t *st = bl_status();
            printf("  %6.1f  %-8s %-7s %-10s bloom %.2f\n", frames / 44100.0, bl_season_name(st->season),
                   st->grove ? SN[st->section & 3] : "-", st->chord_name, st->bloom);
            strcpy(last_chord, st->chord_name);
            last_sect = st->section;
        }
        if (bl_status()->cost > cost_peak)
            cost_peak = bl_status()->cost;
        for (i = 0; i < k; i++) {
            put(l[i]);
            put(r[i]);
        }
        done += k;
        frames += k;
    }
}

static int voice_by_name(const char *s)
{
    int v;
    for (v = 0; v < BV_NTYPES; v++)
        if (!strcasecmp(s, bl_voice_name(v)) || (v == BV_CP80 && !strcasecmp(s, "cp80")))
            return v;
    return atoi(s);
}

static uint32_t seed = 1;
static int nsteer, steer_pc[16];
static double steer_at[16];
static void set(const char *kv)
{
    bl_par_t *p = bl_par();
    char k[32];
    const char *eq = strchr(kv, '=');
    float v;
    if (!eq || eq - kv > 30)
        return;
    memcpy(k, kv, eq - kv);
    k[eq - kv] = 0;
    v = (float)atof(eq + 1);
#define F(name) else if (!strcmp(k, #name)) p->name = v
#define I(name) else if (!strcmp(k, #name)) p->name = (int)v
    if (!strcmp(k, "log")) logc = (int)v;
    else if (!strcmp(k, "seed")) seed = (uint32_t)strtoul(eq + 1, 0, 10);
    else if (!strcmp(k, "steer")) {
        if (nsteer < 16) {
            const char *at = strchr(eq + 1, '@');
            steer_pc[nsteer] = atoi(eq + 1);
            steer_at[nsteer++] = at ? atof(at + 1) : 0.0;
        }
    }
    else if (!strcmp(k, "mood")) {
        p->mood = (int)v;
        p->room = bl_mood_room(p->mood);
    }
    else if (!strcmp(k, "solo")) p->solo = voice_by_name(eq + 1);
    I(room); I(just); I(a432); F(activity); F(drift); F(bloom); F(bed); F(low); F(motion); F(melody); I(minutes);
    F(space); F(echo); F(shimmer); F(tone); I(talk); I(tape); F(wear); F(hiss); F(level); F(solo_level);
    else fprintf(stderr, "unknown key %s\n", k);
}

/* a little tune to hear a voice: a chord, a line, a long note */
static void solo_demo(void)
{
    static const struct { float t; int note; float vel; float len; } N[] = {
        {0.2f, 53, 0.55f, 3.0f}, {0.25f, 60, 0.5f, 3.0f}, {0.3f, 64, 0.5f, 3.0f}, {0.35f, 67, 0.55f, 3.0f},
        {4.0f, 72, 0.6f, 0.6f}, {4.6f, 74, 0.45f, 0.6f}, {5.2f, 76, 0.5f, 0.6f}, {5.8f, 79, 0.55f, 0.9f},
        {6.9f, 76, 0.35f, 0.5f}, {7.5f, 74, 0.4f, 0.5f}, {8.1f, 72, 0.6f, 3.2f},
        {12.0f, 48, 0.9f, 2.0f}, {12.0f, 55, 0.9f, 2.0f}, {12.0f, 64, 0.9f, 2.0f}, {12.0f, 71, 0.9f, 2.0f},
        {15.5f, 84, 0.3f, 1.5f}, {16.0f, 81, 0.3f, 1.5f}, {16.5f, 79, 0.3f, 3.0f},
    };
    int n = (int)(sizeof N / sizeof N[0]), i;
    double t = 0.0;
    struct { double at; int note; float vel; } ev[64];
    int ne = 0;
    for (i = 0; i < n; i++) {
        ev[ne].at = N[i].t; ev[ne].note = N[i].note; ev[ne].vel = N[i].vel; ne++;
        ev[ne].at = N[i].t + N[i].len; ev[ne].note = N[i].note; ev[ne].vel = 0.0f; ne++;
    }
    /* sort by time */
    for (i = 1; i < ne; i++) {
        int j = i;
        while (j > 0 && ev[j - 1].at > ev[j].at) {
            __typeof__(ev[0]) x = ev[j]; ev[j] = ev[j - 1]; ev[j - 1] = x; j--;
        }
    }
    for (i = 0; i < ne; i++) {
        if (ev[i].at > t) {
            run(ev[i].at - t);
            t = ev[i].at;
        }
        bl_post_note(ev[i].note, ev[i].vel);
    }
    run(26.0 - t);
}

int main(int argc, char **argv)
{
    int i;
    clock_t c0;
    double secs;
    if (argc < 4) {
        fprintf(stderr, "usage: render grove SECONDS out.wav [k=v..] | sting SECONDS out.wav [k=v..] | solo VOICE out.wav [k=v..]\n");
        return 2;
    }
    for (i = 4; i < argc; i++)
        if (!strncmp(argv[i], "seed=", 5))
            seed = (uint32_t)strtoul(argv[i] + 5, 0, 10);
    bl_init(seed);
    for (i = 4; i < argc; i++)
        set(argv[i]);
    wav_open(argv[3]);
    c0 = clock();
    if (!strcmp(argv[1], "grove")) {
        secs = atof(argv[2]);
        double t = 0.0;
        int j;
        bl_post_grove(1);
        for (j = 0; j < nsteer; j++)
            if (steer_at[j] < secs) {
                if (steer_at[j] > t) {
                    run(steer_at[j] - t);
                    t = steer_at[j];
                }
                bl_post_steer(steer_pc[j]);
            }
        run(secs - t);
    } else if (!strcmp(argv[1], "sting")) {
        int s = atoi(argv[2]);
        run(0.3);
        bl_post_sting(s, seed);
        run(s + 0.5);
        secs = s + 0.8;
    } else if (!strcmp(argv[1], "solo")) {
        bl_par()->solo = voice_by_name(argv[2]);
        solo_demo();
        secs = 26.0;
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    wav_close();
    {
        double el = (double)(clock() - c0) / CLOCKS_PER_SEC;
        double rms = sqrt(sumsq / (2.0 * frames));
        printf("%s: %.1fs  peak %.2f dBFS  rms %.1f dBFS  clipped %ld  (%.0fx realtime on this machine)\n", argv[3],
               frames / 44100.0, 20 * log10(peak + 1e-12), 20 * log10(rms + 1e-12), clipped, (frames / 44100.0) / (el + 1e-9));
        printf("  voice cost: average %.0f, peak %.0f, over the budget (180) %.2f%% of blocks\n", cost_sum / (cost_n ? cost_n : 1),
               cost_peak, 100.0 * cost_over / (cost_n ? cost_n : 1));
    }
    return 0;
}
