/*
 * The narrow FM demodulator (demod.h).
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include "demod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHANNEL_RATE 50000.0        /* the channel's samples per second, about */
#define CIC_ORDER    4
#define CIC_SCALE    16.0           /* the mixed samples' fraction kept in the integers */
#define AUDIO_CUT    4000.0         /* Hz: the audio low-pass */
#define DC_CUT       50.0           /* Hz: what the DC removal takes away below */
#define RENORMALIZE  1024           /* the oscillator kept on the unit circle this often */

struct demod {
    double rate, channel_rate;
    int decimation;
    double volume, pace;
    /* The oscillator: p turns by w each sample (e^-j*2*pi*offset/rate). */
    double pr, pi, wr, wi;
    int since_renormalized;
    /* The CIC: integrators and combs on unsigned integers (they wrap, as a
     * CIC's must), I and Q. */
    uint64_t integrator[2][CIC_ORDER], comb[2][CIC_ORDER];
    int count;
    double cic_gain;                /* D^N * CIC_SCALE * DEMOD_FULL_SCALE: to full scale 1 */
    /* The channel filter: taps, and each input twice (a window without a wrap). */
    double bandwidth;
    float taps[DEMOD_TAPS];
    float hr[2 * DEMOD_TAPS], hi[2 * DEMOD_TAPS];
    int pos;
    /* The discriminator: the last filtered sample. */
    float yr, yi;
    bool have_last;
    /* The audio: DC removal, low-pass, resampler. */
    double dc_in, dc_out, dc_a;
    float audio_taps[DEMOD_AUDIO_TAPS];
    float ha[2 * DEMOD_AUDIO_TAPS];
    int audio_pos;
    double a0, t;                   /* the last audio sample; the next output's place after it */
};

/* A low-pass of `count` taps (odd) cutting at `cut` of the sample rate:
 * a sinc under a Blackman window, its sum 1 (no gain at DC). */
static void low_pass(float *taps, int count, double cut)
{
    int m = (count - 1) / 2;
    double sum = 0;
    double h[DEMOD_TAPS > DEMOD_AUDIO_TAPS ? DEMOD_TAPS : DEMOD_AUDIO_TAPS];
    for (int k = 0; k < count; k++) {
        double x = k - m;
        double sinc = x == 0 ? 2 * cut : sin(2 * M_PI * cut * x) / (M_PI * x);
        double w = 0.42 - 0.5 * cos(2 * M_PI * k / (count - 1)) +
                   0.08 * cos(4 * M_PI * k / (count - 1));
        h[k] = sinc * w;
        sum += h[k];
    }
    for (int k = 0; k < count; k++)
        taps[k] = (float)(h[k] / sum);
}

struct demod *demod_new(double rate, double offset, double bandwidth)
{
    if (!(rate >= DEMOD_AUDIO_RATE))
        return 0;
    struct demod *d = calloc(1, sizeof(*d));
    if (!d)
        return 0;
    d->rate = rate;
    d->decimation = (int)lround(rate / CHANNEL_RATE);
    if (d->decimation < 1)
        d->decimation = 1;
    d->channel_rate = rate / d->decimation;
    d->cic_gain = pow(d->decimation, CIC_ORDER) * CIC_SCALE * DEMOD_FULL_SCALE;
    d->volume = 1;
    d->pace = 1;
    d->pr = 1;
    d->dc_a = 1 - 2 * M_PI * DC_CUT / d->channel_rate;
    low_pass(d->audio_taps, DEMOD_AUDIO_TAPS, AUDIO_CUT / d->channel_rate);
    demod_tune(d, offset, bandwidth);
    return d;
}

void demod_free(struct demod *d)
{
    free(d);
}

void demod_tune(struct demod *d, double offset, double bandwidth)
{
    d->wr = cos(-2 * M_PI * offset / d->rate);
    d->wi = sin(-2 * M_PI * offset / d->rate);
    double widest = 0.9 * d->channel_rate;
    if (bandwidth > widest)
        bandwidth = widest;
    if (bandwidth < 500)
        bandwidth = 500;
    if (bandwidth != d->bandwidth) {
        d->bandwidth = bandwidth;
        low_pass(d->taps, DEMOD_TAPS, bandwidth / 2 / d->channel_rate);
    }
}

void demod_volume(struct demod *d, double volume)
{
    d->volume = volume < 0 ? 0 : volume;
}

void demod_pace(struct demod *d, double pace)
{
    d->pace = pace;
}

double demod_channel_rate(const struct demod *d)
{
    return d->channel_rate;
}

int demod_decimation(const struct demod *d)
{
    return d->decimation;
}

void demod_gap(struct demod *d)
{
    d->have_last = false;
}

/* A FIR over a doubled history: x in, the filtered value out. */
static float fir(const float *taps, int count, float *h, int *pos, float x)
{
    *pos = *pos + 1 == count ? 0 : *pos + 1;
    h[*pos] = h[*pos + count] = x;
    const float *newest = h + *pos + count;
    float y = 0;
    for (int k = 0; k < count; k++)
        y += taps[k] * newest[-k];
    return y;
}

/* One audio sample at the channel's rate: resampled to 48 kHz (linearly:
 * the audio is low-passed far below either rate) into the output. */
static void audio_out(struct demod *d, double a1, struct demod_out *out)
{
    double step = d->channel_rate / DEMOD_AUDIO_RATE * d->pace;
    while (d->t < 1) {
        double a = (d->a0 + (a1 - d->a0) * d->t) * d->volume * 32767;
        int16_t s = a >= 32767 ? 32767 : a <= -32768 ? -32768 : (int16_t)lround(a);
        if (out->frames < out->frames_max) {
            out->audio[2 * out->frames] = s;
            out->audio[2 * out->frames + 1] = s;
            out->frames++;
        } else {
            out->frames_dropped++;
        }
        d->t += step;
    }
    d->t -= 1;
    d->a0 = a1;
}

/* One channel sample (from the CIC, full scale 1): filtered, demodulated,
 * on to the audio. */
static void channel_sample(struct demod *d, float cr, float ci, struct demod_out *out,
                           double *power, int *filtered)
{
    if (out->channel < out->channel_max) {
        out->ci[out->channel] = cr;
        out->cq[out->channel] = ci;
        out->channel++;
    } else {
        out->channel_dropped++;
    }
    int pos = d->pos;
    float yr = fir(d->taps, DEMOD_TAPS, d->hr, &pos, cr);
    pos = d->pos;
    float yi = fir(d->taps, DEMOD_TAPS, d->hi, &pos, ci);
    d->pos = pos;
    *power += (double)yr * yr + (double)yi * yi;
    (*filtered)++;
    /* The phase from the last sample to this one: y times the last's
     * conjugate. */
    double hz = 0;
    if (d->have_last) {
        double re = (double)yr * d->yr + (double)yi * d->yi;
        double im = (double)yi * d->yr - (double)yr * d->yi;
        hz = atan2(im, re) * d->channel_rate / (2 * M_PI);
    }
    d->yr = yr, d->yi = yi;
    d->have_last = true;
    double x = hz / DEMOD_DEVIATION;
    d->dc_out = x - d->dc_in + d->dc_a * d->dc_out;
    d->dc_in = x;
    float a = fir(d->audio_taps, DEMOD_AUDIO_TAPS, d->ha, &d->audio_pos, (float)d->dc_out);
    audio_out(d, a, out);
}

/* The nearest integer of v (the integers' rounding, without the library's). */
static int64_t nearest(double v)
{
    return (int64_t)(v >= 0 ? v + 0.5 : v - 0.5);
}

void demod_run(struct demod *d, const int16_t *iq, int n, struct demod_out *out)
{
    out->channel = out->channel_dropped = 0;
    out->frames = out->frames_dropped = 0;
    double power = 0;
    int filtered = 0;
    for (int k = 0; k < n; k++) {
        /* Mixed: (i + jq)(pr + j pi). */
        double i = iq[2 * k], q = iq[2 * k + 1];
        double mr = i * d->pr - q * d->pi, mi = i * d->pi + q * d->pr;
        double pr = d->pr * d->wr - d->pi * d->wi;
        d->pi = d->pr * d->wi + d->pi * d->wr;
        d->pr = pr;
        if (++d->since_renormalized == RENORMALIZE) {
            double g = 1.5 - 0.5 * (d->pr * d->pr + d->pi * d->pi);
            d->pr *= g, d->pi *= g;
            d->since_renormalized = 0;
        }
        /* The CIC's integrators, at the full rate. */
        uint64_t vr = (uint64_t)nearest(mr * CIC_SCALE), vi = (uint64_t)nearest(mi * CIC_SCALE);
        for (int s = 0; s < CIC_ORDER; s++) {
            vr = d->integrator[0][s] += vr;
            vi = d->integrator[1][s] += vi;
        }
        if (++d->count < d->decimation)
            continue;
        d->count = 0;
        /* Its combs, at the channel's rate. */
        for (int s = 0; s < CIC_ORDER; s++) {
            uint64_t r = vr - d->comb[0][s], m = vi - d->comb[1][s];
            d->comb[0][s] = vr, d->comb[1][s] = vi;
            vr = r, vi = m;
        }
        channel_sample(d, (float)((double)(int64_t)vr / d->cic_gain),
                       (float)((double)(int64_t)vi / d->cic_gain), out, &power, &filtered);
    }
    out->power = filtered ? power / filtered : 0;
}
