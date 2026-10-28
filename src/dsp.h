/*
 * The spectrum's arithmetic, apart from the radio and the window: a
 * windowed FFT of complex samples, its power per bin scaled so that a
 * full-scale complex tone reads 0 dBFS whatever the window, averaging, max
 * hold, peaks and the noise floor. Plain C and math.h only, so
 * tests/dsptest.c checks it on the Mac too.
 *
 * Samples come in full scales (the AD936x's 2048 is 1.0). Bins are kept
 * lowest frequency first: bin j is (j - n/2) * rate / n from the LO, so DC
 * is bin n/2.
 */
#ifndef PLUTOSA_DSP_H
#define PLUTOSA_DSP_H

/* The window over a frame ("taper": window.h has the other windows). */
enum taper { TAPER_HANN, TAPER_BLACKMAN_HARRIS, TAPER_FLAT_TOP, TAPERS };
const char *taper_name(enum taper t);

struct spectrum {
    int n;                      /* points, a power of two */
    enum taper taper;
    double enbw;                /* the window's equivalent noise bandwidth, in bins */
    int averages;               /* the average covers this many frames */
    long frames;                /* frames since the last reset */
    float *frame;               /* the last frame's power per bin (1.0 = 0 dBFS) */
    float *average;             /* averaged power per bin */
    float *hold;                /* the highest power each bin had since the reset */
    /* the FFT's own */
    float *w, *cos_t, *sin_t, *re, *im;
    unsigned *rev;
    double scale;               /* 1 / (sum of w)^2 */
};

/* 0 if n is not a power of two from 8 to 65536, or memory ran out. */
struct spectrum *spectrum_new(int n, enum taper taper, int averages);
void spectrum_free(struct spectrum *s);
void spectrum_reset(struct spectrum *s);

/* One frame of n samples: the window, the FFT, the power, then the
 * average (a mean of the frames so far until there are `averages`, then
 * exponential with weight 1/averages) and max hold. */
void spectrum_add(struct spectrum *s, const float *i, const float *q);

/* The FFT alone, in place (forward, unscaled, bins in FFT order). */
void spectrum_fft(const struct spectrum *s, float *re, float *im);

/* The strongest local maxima of power (n bins), at least `guard` bins
 * apart, strongest first: their bins into bins[], at most max; the count. */
int spectrum_peaks(const float *power, int n, int guard, int *bins, int max);

/* The median of power (n bins): the noise floor of a spectrum that is
 * mostly noise. */
double spectrum_floor(const float *power, int n);

/* 10 log10(power), -999 for nothing at all. */
double power_db(double power);

#endif
