/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Manzanita Research */
/* Offline renders of the engine to 24-bit WAV, for listening and for checks.
 *   cc -O2 -DBL_OFFLINE -I base/include -I app/dsp tools/render.c app/dsp/bliss.c -o build/render -lm
 *   build/render grove 600 out.wav [key=value ...]
 *   build/render sting 10 out.wav seed=1234
 *   build/render solo rhodes out.wav
 * Keys: room mode just a432 activity drift bed melody minutes space echo shimmer tone talk tape wear hiss level seed
 * It prints the peak, the RMS, and the clipped-sample count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "bliss.h"

static FILE *wav;
static uint32_t frames;
static double sumsq;
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
    if (!strcmp(k, "seed")) seed = (uint32_t)strtoul(eq + 1, 0, 10);
    else if (!strcmp(k, "solo")) p->solo = voice_by_name(eq + 1);
    I(room); I(mode); I(just); I(a432); F(activity); F(drift); F(bed); F(melody); I(minutes);
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
        bl_post_grove(1);
        run(secs);
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
    }
    return 0;
}
