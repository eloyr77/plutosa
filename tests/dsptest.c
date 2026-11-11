/*
 * dsptest: src/dsp.c against what is known apart from it. The FFT against
 * a plain DFT in double precision; each window's equivalent noise
 * bandwidth and scalloping loss against their published values; a tone's
 * level and bin; white noise's floor against sigma^2 * ENBW / n; the
 * average, max hold and peaks. Where the processor has AVX2 and FMA (the
 * laptop, not the Mac), the FFT's and the frame's checks run again with
 * them, and both ways must agree on the same frames (with a speed line).
 * Built and run on the Mac (make check) and the machine (make
 * check-machine). Prints PASS/FAIL lines; exits 1 if anything failed.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;
static const char *way = "";        /* "AVX2: " while the checks run with it */

static void check(int ok, const char *what, double got, double want)
{
    printf("%s  %s%s: %.4f (want %.4f)\n", ok ? "PASS" : "FAIL", way, what, got, want);
    if (!ok)
        failures++;
}

static void near(const char *what, double got, double want, double tolerance)
{
    check(fabs(got - want) <= tolerance, what, got, want);
}

/* A fixed sequence (the test repeats exactly): uniform in [0, 1). */
static uint64_t seed = 0x9E3779B97F4A7C15u;
static double uniform(void)
{
    seed = seed * 6364136223846793005u + 1442695040888963407u;
    return (double)(seed >> 11) / 9007199254740992.0;
}

/* Gaussian with variance 1 (Box-Muller). */
static double gaussian(void)
{
    double u = uniform(), v = uniform();
    if (u < 1e-300)
        u = 1e-300;
    return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* A complex tone of amplitude a at `bin` bins (FFT bins: negative below 0). */
static void tone(float *i, float *q, int n, double bin, double a)
{
    for (int k = 0; k < n; k++) {
        i[k] = (float)(a * cos(2 * M_PI * bin * k / n));
        q[k] = (float)(a * sin(2 * M_PI * bin * k / n));
    }
}

static int bin_of(int n, int fft_bin)
{
    return (fft_bin + n / 2) & (n - 1);
}

static void test_fft(void)
{
    static const int sizes[] = { 8, 16, 64, 1024, 16384 };
    for (unsigned t = 0; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
        int n = sizes[t];
        struct spectrum *s = spectrum_new(n, TAPER_HANN, 1);
        float *re = malloc((size_t)n * sizeof(float)), *im = malloc((size_t)n * sizeof(float));
        double *xr = malloc((size_t)n * sizeof(double)), *xi = malloc((size_t)n * sizeof(double));
        for (int k = 0; k < n; k++) {
            re[k] = (float)(xr[k] = 2 * uniform() - 1);
            im[k] = (float)(xi[k] = 2 * uniform() - 1);
        }
        spectrum_fft(s, re, im);
        /* The DFT of a few bins (all of them for the small sizes). */
        double worst = 0, largest = 0;
        int step = n <= 1024 ? 1 : 97;
        for (int m = 0; m < n; m += step) {
            double sr = 0, si = 0;
            for (int k = 0; k < n; k++) {
                double x = -2 * M_PI * (double)((long)m * k % n) / n;
                sr += xr[k] * cos(x) - xi[k] * sin(x);
                si += xr[k] * sin(x) + xi[k] * cos(x);
            }
            double e = hypot(re[m] - sr, im[m] - si), size = hypot(sr, si);
            if (e > worst)
                worst = e;
            if (size > largest)
                largest = size;
        }
        char what[64];
        snprintf(what, sizeof(what), "FFT %d points = DFT (largest error / largest bin)", n);
        check(worst / largest < 2e-5, what, worst / largest, 0);
        free(re), free(im), free(xr), free(xi);
        spectrum_free(s);
    }
}

/* Published values (Harris 1978; Heinzel, Rudiger, Schilling 2002):
 * equivalent noise bandwidth in bins and the loss of a tone halfway
 * between two bins. */
static const struct {
    enum taper taper;
    double enbw, scallop_db, scallop_tolerance;
} WINDOWS[] = {
    { TAPER_HANN, 1.5000, -1.4236, 0.005 },
    { TAPER_BLACKMAN_HARRIS, 2.0044, -0.8256, 0.005 },
    { TAPER_FLAT_TOP, 3.7702, 0.0, 0.03 },
};

static void test_windows(void)
{
    int n = 4096;
    float *i = malloc((size_t)n * sizeof(float)), *q = malloc((size_t)n * sizeof(float));
    for (unsigned t = 0; t < sizeof(WINDOWS) / sizeof(WINDOWS[0]); t++) {
        char what[96];
        const char *name = taper_name(WINDOWS[t].taper);
        struct spectrum *s = spectrum_new(n, WINDOWS[t].taper, 1);
        snprintf(what, sizeof(what), "%s: ENBW in bins", name);
        near(what, s->enbw, WINDOWS[t].enbw, 0.001);

        /* A full-scale tone on a bin reads 0 dBFS there, above and below DC. */
        tone(i, q, n, 1000, 1.0);
        spectrum_add(s, i, q);
        int peak;
        spectrum_peaks(s->frame, n, 4, &peak, 1);
        snprintf(what, sizeof(what), "%s: full-scale tone on bin +1000, dBFS", name);
        near(what, power_db(s->frame[bin_of(n, 1000)]), 0.0, 0.001);
        snprintf(what, sizeof(what), "%s: and it is the peak (bin)", name);
        check(peak == bin_of(n, 1000), what, peak, bin_of(n, 1000));
        tone(i, q, n, -500, 0.5);
        spectrum_reset(s);
        spectrum_add(s, i, q);
        snprintf(what, sizeof(what), "%s: half-scale tone on bin -500, dBFS", name);
        near(what, power_db(s->frame[bin_of(n, -500)]), 20 * log10(0.5), 0.001);

        /* Halfway between bins: the larger of the two neighbours. */
        tone(i, q, n, 1000.5, 1.0);
        spectrum_reset(s);
        spectrum_add(s, i, q);
        double a = s->frame[bin_of(n, 1000)], b = s->frame[bin_of(n, 1001)];
        snprintf(what, sizeof(what), "%s: scalloping loss halfway between bins, dB", name);
        near(what, power_db(a > b ? a : b), WINDOWS[t].scallop_db, WINDOWS[t].scallop_tolerance);
        spectrum_free(s);
    }
    free(i), free(q);
}

/* White noise of power sigma^2 (I and Q sigma^2 / 2 each): every bin
 * averages sigma^2 * ENBW / n. Over 64 frames the median of the averages is
 * within a few hundredths of a dB of their mean. */
static void test_noise(void)
{
    int n = 4096, frames = 64;
    double sigma2 = 0.01;           /* -20 dBFS in all */
    float *i = malloc((size_t)n * sizeof(float)), *q = malloc((size_t)n * sizeof(float));
    for (unsigned t = 0; t < sizeof(WINDOWS) / sizeof(WINDOWS[0]); t++) {
        struct spectrum *s = spectrum_new(n, WINDOWS[t].taper, frames);
        for (int f = 0; f < frames; f++) {
            for (int k = 0; k < n; k++) {
                i[k] = (float)(gaussian() * sqrt(sigma2 / 2));
                q[k] = (float)(gaussian() * sqrt(sigma2 / 2));
            }
            spectrum_add(s, i, q);
        }
        char what[96];
        snprintf(what, sizeof(what), "%s: noise floor of -20 dBFS white noise, dBFS per bin",
                 taper_name(WINDOWS[t].taper));
        near(what, power_db(spectrum_floor(s->average, n)), power_db(sigma2 * s->enbw / n), 0.15);
        spectrum_free(s);
    }
    free(i), free(q);
}

static void test_average_hold_peaks(void)
{
    int n = 64;
    float i[64], q[64];
    struct spectrum *s = spectrum_new(n, TAPER_HANN, 4);
    /* Power 1, 0, 0 on a bin: the mean of three frames is 1/3, max hold 1. */
    tone(i, q, n, 5, 1.0);
    spectrum_add(s, i, q);
    tone(i, q, n, 5, 0.0);
    spectrum_add(s, i, q);
    spectrum_add(s, i, q);
    near("average of 3 frames (of 4) = their mean", s->average[bin_of(n, 5)], 1.0 / 3, 1e-6);
    near("max hold keeps the first frame's", s->hold[bin_of(n, 5)], 1.0, 1e-6);
    /* From then on exponential, weight 1/4: two more empty frames. */
    spectrum_add(s, i, q);
    spectrum_add(s, i, q);
    near("then exponential (weight 1/4)", s->average[bin_of(n, 5)], 1.0 / 4 * 3 / 4, 1e-6);
    spectrum_reset(s);
    near("reset empties the average", s->average[bin_of(n, 5)], 0, 0);
    near("and max hold", s->hold[bin_of(n, 5)], 0, 0);
    spectrum_free(s);

    /* Peaks: strongest first, a guard apart, at most max. */
    float p[100] = { 0 };
    for (int k = 0; k < 100; k++)
        p[k] = 1e-10f;
    p[10] = 1e-2f, p[11] = 5e-3f;   /* a peak and its shoulder */
    p[50] = 1e-1f;
    p[80] = 1e-4f;
    /* (the flat floor has local maxima too, the first of each run of equal
     * bins, so a fourth one follows: a floor bin, never the shoulder) */
    int bins[4];
    int got = spectrum_peaks(p, 100, 3, bins, 4);
    check(got == 4 && bins[0] == 50 && bins[1] == 10 && bins[2] == 80 && bins[3] != 11 &&
          p[bins[3]] == 1e-10f, "peaks 50, 10, 80, then a floor bin, not the shoulder: count",
          got, 4);
    got = spectrum_peaks(p, 100, 3, bins, 2);
    check(got == 2 && bins[0] == 50 && bins[1] == 10, "the two strongest: count", got, 2);

    check(!spectrum_new(1000, TAPER_HANN, 1) && !spectrum_new(4, TAPER_HANN, 1),
          "sizes other than powers of two from 8 refused", 0, 0);
}

static void test_columns_ticks(void)
{
    /* 8 bins into 4 columns: the higher of each pair; into 16: each bin twice. */
    float p[8] = { 1, 5, 2, 2, 9, 0, 3, 4 }, out[16];
    spectrum_columns(p, 8, out, 4);
    check(out[0] == 5 && out[1] == 2 && out[2] == 9 && out[3] == 4,
          "8 bins in 4 columns keep each column's highest (column 2)", out[2], 9);
    spectrum_columns(p, 8, out, 16);
    check(out[0] == 1 && out[1] == 1 && out[2] == 5 && out[15] == 4,
          "8 bins in 16 columns: each bin twice (column 2)", out[2], 5);
    /* A one-bin peak among 16384 bins still shows in 900 columns. */
    static float wide[16384];
    static float cols[900];
    wide[12345] = 1;
    spectrum_columns(wide, 16384, cols, 900);
    int x = 12345 * 900 / 16384;
    check(cols[x] == 1, "a one-bin peak of 16384 bins shows in 900 columns (column)", x, x);

    near("tick step for 0.37", tick_step(0.37), 0.5, 1e-12);
    near("tick step for 3.07e6 (Hz)", tick_step(3.07e6), 5e6, 1e-3);
    near("tick step for 10", tick_step(10), 10, 1e-12);
    near("tick step for 1.2e-3", tick_step(1.2e-3), 2e-3, 1e-15);
}

/* The same frames through both ways: every bin of the FFT, and of the
 * frame, average and max hold of spectrum_add, within float rounding. */
static void test_same(void)
{
    static const int sizes[] = { 8, 16, 32, 4096, 65536 };
    for (unsigned t = 0; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
        int n = sizes[t];
        size_t bytes = (size_t)n * sizeof(float);
        float *i = malloc(bytes), *q = malloc(bytes), *re[2], *im[2], *got[2][3];
        struct spectrum *s[2];
        for (int way2 = 0; way2 < 2; way2++) {
            re[way2] = malloc(bytes), im[way2] = malloc(bytes);
            s[way2] = spectrum_new(n, TAPER_BLACKMAN_HARRIS, 4);
        }
        double fft_worst = 0, fft_largest = 0, worst[3] = { 0 }, largest[3] = { 0 };
        for (int f = 0; f < 6; f++) {
            for (int k = 0; k < n; k++) {
                i[k] = (float)(2 * uniform() - 1);
                q[k] = (float)(2 * uniform() - 1);
            }
            for (int way2 = 0; way2 < 2; way2++) {
                spectrum_avx2(way2);
                memcpy(re[way2], i, bytes);
                memcpy(im[way2], q, bytes);
                spectrum_fft(s[way2], re[way2], im[way2]);
                spectrum_add(s[way2], i, q);
                got[way2][0] = s[way2]->frame, got[way2][1] = s[way2]->average;
                got[way2][2] = s[way2]->hold;
            }
            for (int k = 0; k < n; k++) {
                fft_worst = fmax(fft_worst, hypot(re[1][k] - re[0][k], im[1][k] - im[0][k]));
                fft_largest = fmax(fft_largest, hypot(re[0][k], im[0][k]));
                for (int m = 0; m < 3; m++) {
                    worst[m] = fmax(worst[m], fabs(got[1][m][k] - got[0][m][k]));
                    largest[m] = fmax(largest[m], got[0][m][k]);
                }
            }
        }
        char what[96];
        snprintf(what, sizeof(what), "%d points: the FFTs' largest difference / largest bin", n);
        check(fft_worst / fft_largest < 2e-6, what, fft_worst / fft_largest, 0);
        static const char *const NAMES[3] = { "frame", "average", "max hold" };
        for (int m = 0; m < 3; m++) {
            snprintf(what, sizeof(what), "%d points: the %s's largest difference / largest bin",
                     n, NAMES[m]);
            check(worst[m] / largest[m] < 5e-6, what, worst[m] / largest[m], 0);
        }
        for (int way2 = 0; way2 < 2; way2++) {
            free(re[way2]), free(im[way2]);
            spectrum_free(s[way2]);
        }
        free(i), free(q);
    }
}

/* Not a check: spectrum_add's time for a frame of 4096, each way. */
static void speed(void)
{
    int n = 4096, frames = 1000;
    float *i = malloc((size_t)n * sizeof(float)), *q = malloc((size_t)n * sizeof(float));
    for (int k = 0; k < n; k++)
        i[k] = (float)(2 * uniform() - 1), q[k] = (float)(2 * uniform() - 1);
    struct spectrum *s = spectrum_new(n, TAPER_BLACKMAN_HARRIS, 16);
    for (int way2 = 0; way2 < 2; way2++) {
        if (spectrum_avx2(way2) != way2)
            continue;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int f = 0; f < frames; f++)
            spectrum_add(s, i, q);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("INFO  %s: %.1f us a frame of %d (%.0f frames a second)\n",
               way2 ? "AVX2" : "plain C", secs * 1e6 / frames, n, frames / secs);
    }
    spectrum_free(s);
    free(i), free(q);
}

int main(void)
{
    spectrum_avx2(0);
    test_fft();
    test_windows();
    test_noise();
    test_average_hold_peaks();
    test_columns_ticks();
    if (spectrum_avx2(1)) {
        way = "AVX2: ";
        test_fft();
        test_windows();
        test_noise();
        test_average_hold_peaks();
        way = "";
        test_same();
    } else {
        printf("INFO  no AVX2 and FMA here: the plain C only\n");
    }
    speed();
    if (failures) {
        printf("%d check(s) failed.\n", failures);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
