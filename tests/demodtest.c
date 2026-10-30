/*
 * demodtest: src/demod.c against signals made here with known numbers. A
 * tone at the channel's offset plus 1 kHz must come out of the mixer and
 * CIC at +1 kHz with the CIC's known gain, at several rates and offsets
 * either side of the LO; the channel filter must pass a tone inside its
 * bandwidth and stop one outside; an FM signal (2.5 kHz deviation, a 1 kHz
 * tone) must give that tone at half of full scale, in phase with the
 * modulation (after the chain's known delays), with little distortion and
 * no DC even when the carrier is off by 300 Hz; blocks of any size the same
 * audio as one; 48000 audio frames a second (fewer with a pace above 1);
 * the volume, and clipping without wrapping. Built and run on the Mac
 * (make check) and on the RitterOS machine (make check-machine). Prints
 * PASS/FAIL lines; exits 1 if anything failed.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "demod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CHUNK      65536
#define AUDIO_MAX  (DEMOD_AUDIO_RATE)               /* 1 s of frames */
#define CHANNEL_MAX 60000                           /* 1 s of channel samples, about */
#define CIC_ORDER  4                                /* (demod.c's) */

static int failures;

static void check(int ok, const char *what, double got, double want)
{
    printf("%s  %s: %.4f (want %.4f)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok)
        failures++;
}

static void near(const char *what, double got, double want, double tolerance)
{
    check(fabs(got - want) <= tolerance, what, got, want);
}

/* A carrier at `offset` Hz from the LO, amplitude `amp` (of 2048),
 * frequency-modulated by a tone of `fm` Hz with `deviation` Hz (none if
 * fm is 0). */
struct signal {
    double rate, offset, amp, deviation, fm;
};

static void make(const struct signal *s, long k0, int n, int16_t *iq)
{
    for (int k = 0; k < n; k++) {
        double t = (double)(k0 + k) / s->rate;
        double phase = 2 * M_PI * s->offset * t;
        if (s->fm > 0)
            phase += s->deviation / s->fm * sin(2 * M_PI * s->fm * t);
        iq[2 * k] = (int16_t)lround(s->amp * cos(phase));
        iq[2 * k + 1] = (int16_t)lround(s->amp * sin(phase));
    }
}

/* What a run gave, all of it. */
static int16_t audio[2 * AUDIO_MAX];
static float channel_i[CHANNEL_MAX], channel_q[CHANNEL_MAX];
static int frames, channels;
static double last_power;

/* `seconds` of the signal through d, in blocks cycling through `sizes`. */
static void run(struct demod *d, const struct signal *s, double seconds, const int *sizes,
                int count)
{
    static int16_t iq[2 * CHUNK];
    long total = lround(seconds * s->rate);
    frames = channels = 0;
    for (long k0 = 0, c = 0; k0 < total; c++) {
        int n = sizes[c % count];
        if (n > total - k0)
            n = (int)(total - k0);
        make(s, k0, n, iq);
        struct demod_out out = {
            .ci = channel_i + channels, .cq = channel_q + channels,
            .channel_max = CHANNEL_MAX - channels,
            .audio = audio + 2 * frames, .frames_max = AUDIO_MAX - frames,
        };
        demod_run(d, iq, n, &out);
        channels += out.channel;
        frames += out.frames;
        if (out.channel)
            last_power = out.power;
        k0 += n;
    }
}

static const int ONE_SIZE[] = { CHUNK };

/* The CIC's gain at f Hz (1 at DC). */
static double cic_gain(double f, double rate, int decimation)
{
    if (f == 0)
        return 1;
    double x = M_PI * f / rate;
    return pow(fabs(sin(x * decimation) / (decimation * sin(x))), CIC_ORDER);
}

static void test_rates(void)
{
    static const struct { double rate; int d; } RATES[] = {
        { 2.1e6, 42 }, { 2083334, 42 }, { 5e6, 100 }, { 30.72e6, 614 }, { 61.44e6, 1229 },
    };
    for (int k = 0; k < (int)(sizeof(RATES) / sizeof(RATES[0])); k++) {
        struct demod *d = demod_new(RATES[k].rate, 0, 12500);
        char what[80];
        snprintf(what, sizeof(what), "decimation at %.0f samples/s", RATES[k].rate);
        check(d && demod_decimation(d) == RATES[k].d, what, d ? demod_decimation(d) : -1,
              RATES[k].d);
        snprintf(what, sizeof(what), "channel rate at %.0f samples/s", RATES[k].rate);
        near(what, d ? demod_channel_rate(d) : 0, RATES[k].rate / RATES[k].d, 1e-6);
        demod_free(d);
    }
    check(demod_new(1000, 0, 12500) == 0, "a rate below the audio's refused", 0, 0);
}

/* A tone 1 kHz above the channel's offset: at +1 kHz in the channel's
 * samples, its amplitude times the CIC's gain there. */
static void test_mixing(void)
{
    static const struct { double rate, offset; } CASES[] = {
        { 2.1e6, 700e3 }, { 2.1e6, -950e3 }, { 5e6, -2e6 }, { 30.72e6, 12.3e6 }, { 2083334, 0 },
    };
    for (int k = 0; k < (int)(sizeof(CASES) / sizeof(CASES[0])); k++) {
        struct signal s = { CASES[k].rate, CASES[k].offset + 1000, 1000, 0, 0 };
        struct demod *d = demod_new(s.rate, CASES[k].offset, 12500);
        run(d, &s, 0.05, ONE_SIZE, 1);
        double fc = demod_channel_rate(d), steps = 0, amplitude = 0;
        int from = 50, n = 0;
        for (int c = from + 1; c < channels; c++, n++) {
            /* The phase step: this sample times the last's conjugate. */
            double re = channel_i[c] * channel_i[c - 1] + channel_q[c] * channel_q[c - 1];
            double im = channel_q[c] * channel_i[c - 1] - channel_i[c] * channel_q[c - 1];
            steps += atan2(im, re);
            amplitude += hypot(channel_i[c], channel_q[c]);
        }
        char what[96];
        snprintf(what, sizeof(what), "%.0f samples/s, offset %+.0f: the tone's Hz in the channel",
                 s.rate, CASES[k].offset);
        near(what, steps / n * fc / (2 * M_PI), 1000, 0.5);
        double want = 1000 / DEMOD_FULL_SCALE * cic_gain(1000, s.rate, demod_decimation(d));
        snprintf(what, sizeof(what), "%.0f samples/s, offset %+.0f: its amplitude", s.rate,
                 CASES[k].offset);
        near(what, amplitude / n, want, want * 2e-3);
        demod_free(d);
    }
}

/* The channel filter: a tone inside the bandwidth passes (at the CIC's
 * gain), one outside is stopped. */
static void test_filter(void)
{
    double rate = 2.1e6, offset = 300e3, a = 1000 / DEMOD_FULL_SCALE;
    struct demod *d = demod_new(rate, offset, 12500);
    int D = demod_decimation(d);
    struct signal s = { rate, offset + 3000, 1000, 0, 0 };
    run(d, &s, 0.05, ONE_SIZE, 1);
    double g = cic_gain(3000, rate, D);
    near("12.5 kHz wide: a tone 3 kHz off passes (dB against its level)",
         10 * log10(last_power / (a * a * g * g)), 0, 0.1);
    s.offset = offset + 12000;
    run(d, &s, 0.05, ONE_SIZE, 1);
    double stopped = 10 * log10(last_power / (a * a));
    check(stopped < -60, "12.5 kHz wide: a tone 12 kHz off is stopped (dB)", stopped, -60);
    demod_tune(d, offset, 25000);
    s.offset = offset + 10000;
    run(d, &s, 0.05, ONE_SIZE, 1);
    g = cic_gain(10000, rate, D);
    near("25 kHz wide: a tone 10 kHz off passes (dB against its level)",
         10 * log10(last_power / (a * a * g * g)), 0, 0.3);
    demod_free(d);
}

/* The audio of an FM signal from frame `from` on: the 1 kHz tone's
 * amplitude and phase against the modulation as it should come out after
 * the chain's delays, the rest (distortion and noise) against it, and the
 * mean. */
struct tone_fit {
    double amplitude, phase, rest, mean;
};

static struct tone_fit fit(const struct demod *d, double rate, double fm, int from)
{
    int D = demod_decimation(d);
    double fc = demod_channel_rate(d), step = fc / DEMOD_AUDIO_RATE;
    /* Frame j is at channel sample j * step - 1 (the resampler starts at
     * a sample before the first), channel sample c at input sample
     * c * D + D - 1; the delays: the CIC's N (D - 1) / 2 input samples, the
     * filters' (taps - 1) / 2 channel samples each, the discriminator's
     * half a sample. */
    double delay = CIC_ORDER * (D - 1) / 2.0 / rate +
                   ((DEMOD_TAPS - 1) / 2.0 + (DEMOD_AUDIO_TAPS - 1) / 2.0 + 0.5) / fc;
    double a = 0, b = 0, mean = 0;
    int n = frames - from;
    for (int j = from; j < frames; j++) {
        double t = ((j * step - 1) * D + D - 1) / rate - delay;
        double x = audio[2 * j];
        a += x * cos(2 * M_PI * fm * t);
        b += x * sin(2 * M_PI * fm * t);
        mean += x;
    }
    a *= 2.0 / n, b *= 2.0 / n;
    struct tone_fit f = { hypot(a, b), atan2(-b, a), 0, mean / n };
    double rest = 0;
    for (int j = from; j < frames; j++) {
        double t = ((j * step - 1) * D + D - 1) / rate - delay;
        double e = audio[2 * j] - (a * cos(2 * M_PI * fm * t) + b * sin(2 * M_PI * fm * t));
        rest += e * e;
    }
    f.rest = sqrt(rest / n) / (f.amplitude / sqrt(2));
    return f;
}

static void test_fm(void)
{
    static const struct { double rate, offset, seconds; } CASES[] = {
        { 2.1e6, -400e3, 0.5 }, { 5e6, 1.7e6, 0.3 }, { 30.72e6, -9e6, 0.2 },
    };
    for (int k = 0; k < (int)(sizeof(CASES) / sizeof(CASES[0])); k++) {
        /* The carrier 300 Hz off the channel's middle: no DC in the audio. */
        struct signal s = { CASES[k].rate, CASES[k].offset + 300, 1500, 2500, 1000 };
        struct demod *d = demod_new(s.rate, CASES[k].offset, 12500);
        run(d, &s, CASES[k].seconds, ONE_SIZE, 1);
        char what[96];
        snprintf(what, sizeof(what), "%.0f samples/s: audio frames for %.1f s", s.rate,
                 CASES[k].seconds);
        near(what, frames, CASES[k].seconds * DEMOD_AUDIO_RATE, 2);
        int same = 1;
        for (int j = 0; j < frames; j++)
            same &= audio[2 * j] == audio[2 * j + 1];
        check(same, "left and right the same", same, 1);
        struct tone_fit f = fit(d, s.rate, s.fm, DEMOD_AUDIO_RATE / 10);
        /* 2.5 kHz of 5 kHz: half of full scale; the DC removal's gain at
         * 1 kHz and its lead (50 Hz / 1 kHz rad) are in the want. */
        snprintf(what, sizeof(what), "%.0f samples/s: the 1 kHz tone's amplitude", s.rate);
        near(what, f.amplitude, 32767 * 0.5 * cos(atan(50.0 / 1000)), 32767 * 0.5 * 0.01);
        snprintf(what, sizeof(what), "%.0f samples/s: its phase against the modulation (rad)",
                 s.rate);
        near(what, f.phase, atan(50.0 / 1000), 0.1);
        snprintf(what, sizeof(what), "%.0f samples/s: the rest against it", s.rate);
        check(f.rest < 0.01, what, f.rest, 0.01);
        snprintf(what, sizeof(what), "%.0f samples/s: the audio's mean (DC)", s.rate);
        near(what, f.mean, 0, 32767 * 0.005);
        demod_free(d);
    }
}

/* Blocks of odd sizes give the same audio, frame for frame. */
static void test_blocks(void)
{
    static int16_t whole[2 * AUDIO_MAX];
    static const int SIZES[] = { 1, 7, 1000, 4093, 65536, 333, 2 };
    struct signal s = { 2.1e6, 250e3, 1500, 2500, 1000 };
    struct demod *d = demod_new(s.rate, s.offset, 12500);
    run(d, &s, 0.2, ONE_SIZE, 1);
    int whole_frames = frames;
    memcpy(whole, audio, sizeof(int16_t) * 2 * (size_t)frames);
    demod_free(d);
    d = demod_new(s.rate, s.offset, 12500);
    run(d, &s, 0.2, SIZES, (int)(sizeof(SIZES) / sizeof(SIZES[0])));
    check(frames == whole_frames, "blocks of odd sizes: as many frames", frames, whole_frames);
    int same = frames == whole_frames &&
               memcmp(whole, audio, sizeof(int16_t) * 2 * (size_t)frames) == 0;
    check(same, "blocks of odd sizes: the same frames", same, 1);
    demod_free(d);
}

static void test_pace_volume(void)
{
    struct signal s = { 2.1e6, 0, 1500, 2500, 1000 };
    struct demod *d = demod_new(s.rate, 0, 12500);
    demod_pace(d, 1.01);
    run(d, &s, 0.5, ONE_SIZE, 1);
    near("pace 1.01: audio frames for 0.5 s", frames, 0.5 * DEMOD_AUDIO_RATE / 1.01, 2);
    demod_free(d);

    d = demod_new(s.rate, 0, 12500);
    demod_volume(d, 0);
    run(d, &s, 0.1, ONE_SIZE, 1);
    int loudest = 0;
    for (int j = 0; j < 2 * frames; j++)
        loudest = audio[j] > loudest ? audio[j] : -audio[j] > loudest ? -audio[j] : loudest;
    check(loudest == 0, "volume 0: silence (the loudest sample)", loudest, 0);
    demod_free(d);

    /* Twice full scale: clipped at the ends, never wrapped to the other
     * side (a wrapped sample would be a jump of nearly 65536). */
    d = demod_new(s.rate, 0, 12500);
    demod_volume(d, 4);
    run(d, &s, 0.2, ONE_SIZE, 1);
    int top = 0, bottom = 0, jump = 0;
    for (int j = DEMOD_AUDIO_RATE / 20; j < frames; j++) {
        top = audio[2 * j] > top ? audio[2 * j] : top;
        bottom = audio[2 * j] < bottom ? audio[2 * j] : bottom;
        int step = audio[2 * j] - audio[2 * j - 2];
        jump = step > jump ? step : -step > jump ? -step : jump;
    }
    check(top == 32767 && bottom == -32768, "volume 4: clipped at both ends", top, 32767);
    check(jump < 20000, "volume 4: no wrapped sample (the largest step)", jump, 20000);
    demod_free(d);
}

/* Not a check: how much faster than the Pluto the demodulator runs (one
 * core; the signal made beforehand, only demod_run timed). */
static void speed(double rate)
{
    static int16_t iq[2 * CHUNK];
    struct signal s = { rate, 100e3, 1500, 2500, 1000 };
    make(&s, 0, CHUNK, iq);
    struct demod *d = demod_new(rate, s.offset, 12500);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int blocks = (int)(rate / CHUNK);               /* about 1 s of samples */
    for (int k = 0; k < blocks; k++) {
        struct demod_out out = { .ci = channel_i, .cq = channel_q, .channel_max = CHANNEL_MAX,
                                 .audio = audio, .frames_max = AUDIO_MAX };
        demod_run(d, iq, CHUNK, &out);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("INFO  %.2f MS/s: %.1f ms for %.3f s of samples (%.0f times the Pluto's pace)\n",
           rate / 1e6, 1e3 * secs, (double)blocks * CHUNK / rate, (double)blocks * CHUNK / rate / secs);
    demod_free(d);
}

int main(void)
{
    test_rates();
    test_mixing();
    test_filter();
    test_fm();
    test_blocks();
    test_pace_volume();
    speed(2.1e6);
    speed(5e6);
    if (failures) {
        printf("%d check(s) failed.\n", failures);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
