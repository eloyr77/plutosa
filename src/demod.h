/*
 * The narrow FM demodulator's arithmetic: plain C, no RitterOS calls (as
 * dsp.c). From the Pluto's samples at the full rate to 48 kHz audio:
 *
 *   mix      the channel's frequency (offset from the LO) down to 0 Hz
 *   CIC      decimate by D = rate / ~50 kHz (4th order, on integers)
 *   filter   the channel: a low-pass at +-bandwidth / 2 (windowed sinc)
 *   FM       the phase step between samples: the frequency; 5 kHz of
 *            deviation is full scale
 *   audio    DC removed, a 4 kHz low-pass, resampled to 48 kHz, the
 *            volume, 16-bit stereo
 *
 * The channel's samples before its filter come out too (for the FM
 * window's spectrum). One thread owns a demod; its state goes on across
 * calls, so blocks of any size give the same audio as one long one.
 */
#ifndef PLUTOSA_DEMOD_H
#define PLUTOSA_DEMOD_H

#include <stdint.h>

#define DEMOD_AUDIO_RATE  48000     /* audio samples per second */
#define DEMOD_FULL_SCALE  2048.0    /* the input's: the AD936x's 12 bits */
#define DEMOD_DEVIATION   5000.0    /* Hz of deviation that make full-scale audio */
#define DEMOD_TAPS        129       /* the channel filter's */
#define DEMOD_AUDIO_TAPS  63        /* the audio low-pass's */

struct demod;

/* A demodulator for samples at `rate` per second; the channel at `offset`
 * Hz from the LO, `bandwidth` Hz wide. 0 when out of memory or the rate is
 * below DEMOD_AUDIO_RATE. */
struct demod *demod_new(double rate, double offset, double bandwidth);
void demod_free(struct demod *d);

/* Another channel: offset and bandwidth (the filter designed again). */
void demod_tune(struct demod *d, double offset, double bandwidth);

/* The audio's level: 1 makes DEMOD_DEVIATION full scale; 0 silence. */
void demod_volume(struct demod *d, double volume);

/* The resampler's pace: 1 nominal; a little above 1 makes fewer audio
 * samples (for a sound card whose clock runs slow against the Pluto's). */
void demod_pace(struct demod *d, double pace);

/* The channel's samples per second (rate / D) and D. */
double demod_channel_rate(const struct demod *d);
int demod_decimation(const struct demod *d);

/* What one call gave: the arrays are the caller's, *_max their room (the
 * rest is dropped, counted in *_dropped). */
struct demod_out {
    float *ci, *cq;                 /* the channel before its filter, full scale 1 */
    int channel, channel_max, channel_dropped;
    int16_t *audio;                 /* 48 kHz, left and right */
    int frames, frames_max, frames_dropped;
    double power;                   /* the filtered channel's mean power, full scale 1 */
};

/* n samples of I and Q, interleaved as they come (le:S12/16): the
 * channel's samples and the audio frames they make into *out. */
void demod_run(struct demod *d, const int16_t *iq, int n, struct demod_out *out);

/* The samples that come next do not follow the last ones (blocks lost):
 * the discriminator starts again, no click from the jump. */
void demod_gap(struct demod *d);

#endif
