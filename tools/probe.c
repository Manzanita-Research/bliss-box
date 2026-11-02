/* SPDX-License-Identifier: MPL-2.0 */
/* probe: 1/3-octave band levels of a 24-bit stereo WAV segment (mono sum, Hann, 65536-point FFT),
 * plus DC, peak and a click detector (big sample-to-sample second differences).
 *   probe file.wav start_s [len_s] */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <complex.h>
#define N 65536
static double complex X[N];
static void fft(double complex *a, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double complex t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double complex w = cexp(-2 * M_PI * I / len);
        for (int i = 0; i < n; i += len) {
            double complex u = 1;
            for (int k = 0; k < len / 2; k++) {
                double complex p = a[i + k], q = a[i + k + len / 2] * u;
                a[i + k] = p + q; a[i + k + len / 2] = p - q; u *= w;
            }
        }
    }
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    double st = atof(argv[2]), dc = 0, pk = 0, prev = 0, prev2 = 0, maxd2 = 0;
    long i, n = N;
    unsigned char b[6];
    if (argc > 3) n = (long)(atof(argv[3]) * 44100);
    if (n > N) n = N;
    fseek(f, 44 + (long)(st * 44100) * 6, SEEK_SET);
    for (i = 0; i < N; i++) X[i] = 0;
    for (i = 0; i < n && fread(b, 1, 6, f) == 6; i++) {
        int l = (b[0] | b[1] << 8 | b[2] << 16) << 8 >> 8, r = (b[3] | b[4] << 8 | b[5] << 16) << 8 >> 8;
        double m = (l + r) / 2.0 / 8388608.0, d2 = fabs(m - 2 * prev + prev2);
        if (i > 2 && d2 > maxd2) maxd2 = d2;
        prev2 = prev; prev = m;
        dc += m;
        if (fabs(m) > pk) pk = fabs(m);
        X[i] = m * (0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)));
    }
    fft(X, N);
    printf("dc %.5f  peak %.1f dB  max 2nd-diff %.4f\n", dc / n, 20 * log10(pk + 1e-12), maxd2);
    for (double lo = 17.8; lo < 18000; lo *= 1.2599) {
        double hi = lo * 1.2599, p = 0;
        int a = (int)(lo * N / 44100.0), z = (int)(hi * N / 44100.0);
        for (int k = a; k < z && k < N / 2; k++) p += creal(X[k]) * creal(X[k]) + cimag(X[k]) * cimag(X[k]);
        p = p * 2 / ((double)n * n * 0.375 / 4) / 4;   /* ~ band power relative to a full-scale sine */
        printf("%7.0f Hz %6.1f dB %s\n", sqrt(lo * hi), 10 * log10(p + 1e-20),
               "##################################################################" + 66 - (int)fmax(0, fmin(66, (10 * log10(p + 1e-20) + 100) / 1.5)));
    }
    return 0;
}
