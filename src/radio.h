/*
 * The Pluto's receiver through libiio: what it is set to, the ranges it
 * takes, and changing the settings the analyzer controls (the RX LO, the
 * sample rate, the RF bandwidth, the gain mode and the gain). They live on
 * the AD936x's control device ad9361-phy: the RX LO is output channel
 * altvoltage0, the rest input channel voltage0. Nothing else is written.
 */
#ifndef PLUTOSA_RADIO_H
#define PLUTOSA_RADIO_H

#include <stdbool.h>

struct iio_context;
struct iio_channel;

/* An attribute's "[min step max]" (its _available). */
struct range {
    bool known;
    double min, step, max;
};

/* What the receiver is set to (-1 where it could not be read). */
struct receiver {
    double lo, rate, bandwidth, gain;   /* Hz, samples per second, Hz, dB */
    char mode[32];                      /* gain_control_mode */
    struct range lo_range, rate_range, bandwidth_range, gain_range;
};

struct radio {
    struct iio_channel *lo, *rx;
};

/* Finds the receiver's channels: 0, or -ENODEV (not a Pluto). */
int radio_open(struct radio *r, struct iio_context *ctx);

/* Every setting and range. */
void radio_read(const struct radio *r, struct receiver *out);

/* The gain now (it moves under the AGC) into *db: 0, or a negative error
 * (-1 dB is a gain like any other). */
int radio_gain(const struct radio *r, double *db);

/* Each a value clamped to its range (if known) and written: 0 or a
 * negative error. */
int radio_set_lo(const struct radio *r, const struct receiver *now, double hz);
int radio_set_rate(const struct radio *r, const struct receiver *now, double rate);
int radio_set_bandwidth(const struct radio *r, const struct receiver *now, double hz);
int radio_set_gain_mode(const struct radio *r, const char *mode);
int radio_set_gain(const struct radio *r, const struct receiver *now, double db);

/* v within the range (a known one), on its step. */
double range_clamp(const struct range *r, double v);

#endif
