/*
 * The Pluto's receiver (radio.h).
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "iio.h"
#include "radio.h"

int radio_open(struct radio *r, struct iio_context *ctx)
{
    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    r->lo = phy ? iio_device_find_channel(phy, "altvoltage0", true) : 0;
    r->rx = phy ? iio_device_find_channel(phy, "voltage0", false) : 0;
    return r->lo && r->rx ? 0 : -ENODEV;
}

static double read_double(struct iio_channel *ch, const char *attr)
{
    double v;
    if (iio_channel_attr_read_double(ch, attr, &v) < 0)
        return -1;
    return v;
}

/* "[min step max]", as the AD936x driver writes its _available. */
static struct range read_range(struct iio_channel *ch, const char *attr)
{
    struct range r = { 0 };
    char text[96];
    if (iio_channel_attr_read(ch, attr, text, sizeof(text)) <= 0)
        return r;
    r.known = sscanf(text, " [ %lf %lf %lf ]", &r.min, &r.step, &r.max) == 3 && r.min <= r.max;
    return r;
}

void radio_read(const struct radio *r, struct receiver *out)
{
    out->lo = read_double(r->lo, "frequency");
    out->rate = read_double(r->rx, "sampling_frequency");
    out->bandwidth = read_double(r->rx, "rf_bandwidth");
    out->gain = read_double(r->rx, "hardwaregain");
    if (iio_channel_attr_read(r->rx, "gain_control_mode", out->mode, sizeof(out->mode)) < 0)
        strcpy(out->mode, "?");
    out->lo_range = read_range(r->lo, "frequency_available");
    /* The PLL refuses the bottom it reports (Tezuka v0.3.21: 46875001 Hz is
     * "Invalid argument", 47 MHz is taken): the LO starts at the next
     * whole MHz. */
    if (out->lo_range.known)
        out->lo_range.min = ceil(out->lo_range.min / 1e6) * 1e6;
    out->rate_range = read_range(r->rx, "sampling_frequency_available");
    /* Likewise its lowest rate: 2083333 (25 MHz / 12, rounded down) is
     * "Invalid argument", 12 times it being under the ADC's 25 MHz; the
     * rate starts at the next whole Hz up. */
    if (out->rate_range.known)
        out->rate_range.min = floor(out->rate_range.min) + 1;
    out->bandwidth_range = read_range(r->rx, "rf_bandwidth_available");
    out->gain_range = read_range(r->rx, "hardwaregain_available");
}

int radio_gain(const struct radio *r, double *db)
{
    return iio_channel_attr_read_double(r->rx, "hardwaregain", db);
}

double range_clamp(const struct range *r, double v)
{
    if (!r->known)
        return v;
    if (v < r->min)
        v = r->min;
    if (v > r->max)
        v = r->max;
    if (r->step > 0)
        v = r->min + round((v - r->min) / r->step) * r->step;
    return v > r->max ? r->max : v;
}

static int write_number(struct iio_channel *ch, const char *attr, double v)
{
    char text[48];
    snprintf(text, sizeof(text), "%.0f", v);
    ssize_t n = iio_channel_attr_write(ch, attr, text);
    return n < 0 ? (int)n : 0;
}

int radio_set_lo(const struct radio *r, const struct receiver *now, double hz)
{
    return write_number(r->lo, "frequency", range_clamp(&now->lo_range, hz));
}

int radio_set_rate(const struct radio *r, const struct receiver *now, double rate)
{
    return write_number(r->rx, "sampling_frequency", range_clamp(&now->rate_range, rate));
}

int radio_set_bandwidth(const struct radio *r, const struct receiver *now, double hz)
{
    return write_number(r->rx, "rf_bandwidth", range_clamp(&now->bandwidth_range, hz));
}

int radio_set_gain_mode(const struct radio *r, const char *mode)
{
    ssize_t n = iio_channel_attr_write(r->rx, "gain_control_mode", mode);
    return n < 0 ? (int)n : 0;
}

/* hardwaregain is "71.000000 dB" when read; written as a number of dB
 * (only in the manual mode: the AGC sets it otherwise). */
int radio_set_gain(const struct radio *r, const struct receiver *now, double db)
{
    char text[32];
    snprintf(text, sizeof(text), "%.0f", range_clamp(&now->gain_range, db));
    ssize_t n = iio_channel_attr_write(r->rx, "hardwaregain", text);
    return n < 0 ? (int)n : 0;
}
