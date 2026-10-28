/*
 * The spectrum's arithmetic (dsp.h). The FFT is an iterative radix-2
 * decimation in time on separate real and imaginary arrays: RitterOS's cc
 * makes C's complex multiplication a library call (Annex G), too slow for
 * a hundred thousand butterflies a frame.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846     /* (RitterOS's math.h has it; strict C11 elsewhere not) */
#endif

/* The windows as cosine sums, w[k] = a0 - a1 cos(x) + a2 cos(2x) - ...,
 * x = 2 pi k / n (periodic: the DFT's own period, as spectrum analysis
 * wants). Blackman-Harris is the 4-term, -92 dB one; the flat top is the
 * common 5-term one (MATLAB's flattopwin), whose scalloping loss is under
 * 0.01 dB: the one to measure a tone's level with. */
static const double COEFFS[TAPERS][5] = {
    [TAPER_HANN] = { 0.5, 0.5 },
    [TAPER_BLACKMAN_HARRIS] = { 0.35875, 0.48829, 0.14128, 0.01168 },
    [TAPER_FLAT_TOP] = { 0.21557895, 0.41663158, 0.277263158, 0.083578947, 0.006947368 },
};

const char *taper_name(enum taper t)
{
    static const char *const names[TAPERS] = {
        [TAPER_HANN] = "Hann",
        [TAPER_BLACKMAN_HARRIS] = "Blackman-Harris",
        [TAPER_FLAT_TOP] = "flat top",
    };
    return t >= 0 && t < TAPERS ? names[t] : "?";
}

struct spectrum *spectrum_new(int n, enum taper taper, int averages)
{
    int bits = 0;
    while ((1 << bits) < n)
        bits++;
    if (n < 8 || n > 65536 || (1 << bits) != n || taper < 0 || taper >= TAPERS)
        return 0;
    struct spectrum *s = calloc(1, sizeof(*s));
    if (!s)
        return 0;
    s->n = n;
    s->taper = taper;
    s->averages = averages < 1 ? 1 : averages;
    size_t f = (size_t)n * sizeof(float);
    s->frame = malloc(f), s->average = malloc(f), s->hold = malloc(f);
    s->w = malloc(f), s->re = malloc(f), s->im = malloc(f);
    s->cos_t = malloc(f / 2), s->sin_t = malloc(f / 2);
    s->rev = malloc((size_t)n * sizeof(unsigned));
    if (!s->frame || !s->average || !s->hold || !s->w || !s->re || !s->im || !s->cos_t ||
        !s->sin_t || !s->rev) {
        spectrum_free(s);
        return 0;
    }
    double sum = 0, sum2 = 0;
    for (int k = 0; k < n; k++) {
        double x = 2 * M_PI * k / n, v = 0;
        for (int m = 0; m < 5; m++)
            v += (m & 1 ? -1 : 1) * COEFFS[taper][m] * cos(m * x);
        s->w[k] = (float)v;
        sum += v;
        sum2 += v * v;
    }
    s->scale = 1 / (sum * sum);
    s->enbw = n * sum2 / (sum * sum);
    for (int k = 0; k < n / 2; k++) {
        s->cos_t[k] = (float)cos(2 * M_PI * k / n);
        s->sin_t[k] = (float)sin(2 * M_PI * k / n);
    }
    for (int k = 0; k < n; k++) {
        unsigned r = 0;
        for (int b = 0; b < bits; b++)
            r |= ((unsigned)k >> b & 1) << (bits - 1 - b);
        s->rev[k] = r;
    }
    spectrum_reset(s);
    return s;
}

void spectrum_free(struct spectrum *s)
{
    if (!s)
        return;
    free(s->frame), free(s->average), free(s->hold);
    free(s->w), free(s->re), free(s->im), free(s->cos_t), free(s->sin_t), free(s->rev);
    free(s);
}

void spectrum_reset(struct spectrum *s)
{
    s->frames = 0;
    memset(s->frame, 0, (size_t)s->n * sizeof(float));
    memset(s->average, 0, (size_t)s->n * sizeof(float));
    memset(s->hold, 0, (size_t)s->n * sizeof(float));
}

void spectrum_fft(const struct spectrum *s, float *re, float *im)
{
    int n = s->n;
    for (int k = 0; k < n; k++) {
        int r = (int)s->rev[k];
        if (r > k) {
            float t = re[k];
            re[k] = re[r], re[r] = t;
            t = im[k];
            im[k] = im[r], im[r] = t;
        }
    }
    /* Stages of butterflies, len points each: the twiddle e^(-2 pi i j / len)
     * is entry j * n / len of the n-point tables. */
    for (int len = 2; len <= n; len <<= 1) {
        int half = len >> 1, stride = n / len;
        for (int start = 0; start < n; start += len) {
            for (int j = 0; j < half; j++) {
                float wr = s->cos_t[j * stride], wi = -s->sin_t[j * stride];
                int a = start + j, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

void spectrum_add(struct spectrum *s, const float *i, const float *q)
{
    int n = s->n, half = n / 2;
    for (int k = 0; k < n; k++) {
        s->re[k] = i[k] * s->w[k];
        s->im[k] = q[k] * s->w[k];
    }
    spectrum_fft(s, s->re, s->im);
    /* A tone of amplitude A on bin k gives |X_k| = A * sum(w): scaled by
     * 1 / sum(w)^2 its power is A^2, 1.0 for full scale. */
    s->frames++;
    long k_avg = s->frames < s->averages ? s->frames : s->averages;
    float a = 1.0f / (float)k_avg;
    for (int k = 0; k < n; k++) {
        int j = (k + half) & (n - 1);        /* lowest frequency first */
        float p = (float)((s->re[k] * s->re[k] + s->im[k] * s->im[k]) * s->scale);
        s->frame[j] = p;
        s->average[j] += (p - s->average[j]) * a;
        if (p > s->hold[j])
            s->hold[j] = p;
    }
}

int spectrum_peaks(const float *power, int n, int guard, int *bins, int max)
{
    if (guard < 1)
        guard = 1;
    int count = 0;
    for (int j = 0; j < n; j++) {
        float p = power[j];
        if (p <= 0)
            continue;
        /* A local maximum: nothing higher within the guard (the first of
         * equal ones). */
        int lo = j - guard < 0 ? 0 : j - guard, hi = j + guard >= n ? n - 1 : j + guard;
        int is_max = 1;
        for (int m = lo; m <= hi && is_max; m++)
            if (power[m] > p || (power[m] == p && m < j))
                is_max = 0;
        if (!is_max)
            continue;
        /* Kept in order, strongest first; the weakest falls off the end. */
        int at = count;
        while (at > 0 && power[bins[at - 1]] < p)
            at--;
        if (at >= max)
            continue;
        if (count < max)
            count++;
        for (int m = count - 1; m > at; m--)
            bins[m] = bins[m - 1];
        bins[at] = j;
    }
    return count;
}

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

double spectrum_floor(const float *power, int n)
{
    float *copy = malloc((size_t)n * sizeof(float));
    if (!copy || n <= 0) {
        free(copy);
        return 0;
    }
    memcpy(copy, power, (size_t)n * sizeof(float));
    qsort(copy, (size_t)n, sizeof(float), compare_float);
    double median = n & 1 ? copy[n / 2] : (copy[n / 2 - 1] + copy[n / 2]) / 2.0;
    free(copy);
    return median;
}

void spectrum_columns(const float *power, int n, float *out, int columns)
{
    for (int x = 0; x < columns; x++) {
        int lo = (int)((long)x * n / columns), hi = (int)((long)(x + 1) * n / columns);
        if (hi <= lo)
            hi = lo + 1;
        float m = power[lo];
        for (int j = lo + 1; j < hi; j++)
            if (power[j] > m)
                m = power[j];
        out[x] = m;
    }
}

double tick_step(double wanted)
{
    if (!(wanted > 0))
        return 1;
    double decade = pow(10, floor(log10(wanted)));
    static const double STEPS[] = { 1, 2, 5, 10 };
    for (int k = 0; k < 4; k++)
        if (STEPS[k] * decade >= wanted * (1 - 1e-9))
            return STEPS[k] * decade;
    return 10 * decade;
}

double power_db(double power)
{
    return power > 0 ? 10 * log10(power) : -999;
}
