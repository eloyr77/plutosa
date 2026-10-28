/*
 * radiotest: src/radio.c on a real Pluto. It reads the receiver and the
 * ranges it takes, changes each setting the analyzer controls (the RX LO,
 * the sample rate, the RF bandwidth, the gain mode and a manual gain),
 * reads each back, tries values outside the ranges (they must be clamped
 * to them), then puts everything back as it was and checks that too.
 * Receive side only: nothing of the transmitter is touched. Built and run
 * on the RitterOS machine (make check-radio from the Mac).
 *
 *   radiotest [URI]       (ip:192.168.2.1 by default)
 *
 * Prints PASS/FAIL lines; exits 1 if anything failed.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "iio.h"
#include "radio.h"

static int failures;

static void check(int ok, const char *what, double got, double want)
{
    printf("%s  %s: %.6f (want %.6f)\n", ok ? "PASS" : "FAIL", what, got, want);
    if (!ok)
        failures++;
}

/* Within a relative tolerance (the AD936x's clocks round some values). */
static void near(const char *what, double got, double want, double relative)
{
    check(fabs(got - want) <= fabs(want) * relative, what, got, want);
}

static void show(const char *when, const struct receiver *r)
{
    printf("%s: LO %.0f Hz [%.0f %.0f %.0f], rate %.0f [%.0f %.0f %.0f], bandwidth %.0f "
           "[%.0f %.0f %.0f], gain %.1f dB [%.0f %.0f %.0f] %s\n", when,
           r->lo, r->lo_range.min, r->lo_range.step, r->lo_range.max,
           r->rate, r->rate_range.min, r->rate_range.step, r->rate_range.max,
           r->bandwidth, r->bandwidth_range.min, r->bandwidth_range.step, r->bandwidth_range.max,
           r->gain, r->gain_range.min, r->gain_range.step, r->gain_range.max, r->mode);
}

int main(int argc, char **argv)
{
    const char *uri = argc > 1 ? argv[1] : "ip:192.168.2.1";
    struct iio_context *ctx = iio_create_context_from_uri(uri);
    if (!ctx) {
        printf("radiotest: cannot connect to %s (errno %d)\n", uri, errno);
        return 1;
    }
    struct radio radio;
    if (radio_open(&radio, ctx) < 0) {
        printf("radiotest: %s is not a Pluto\n", uri);
        return 1;
    }
    struct receiver before, r;
    radio_read(&radio, &before);
    show("before", &before);
    check(before.lo_range.known && before.rate_range.known && before.bandwidth_range.known &&
          before.gain_range.known, "the four ranges read", 1, 1);

    /* Each setting changed and read back. */
    check(radio_set_lo(&radio, &before, before.lo + 1e6) == 0, "set the LO 1 MHz up", 0, 0);
    radio_read(&radio, &r);
    near("the LO read back", r.lo, before.lo + 1e6, 1e-9);
    check(radio_set_rate(&radio, &before, 5e6) == 0, "set the rate to 5 MSPS", 0, 0);
    radio_read(&radio, &r);
    near("the rate read back", r.rate, 5e6, 1e-3);
    check(radio_set_bandwidth(&radio, &before, 5e6) == 0, "set the RF bandwidth to 5 MHz", 0, 0);
    radio_read(&radio, &r);
    near("the RF bandwidth read back", r.bandwidth, 5e6, 0.02);
    check(radio_set_gain_mode(&radio, "manual") == 0 && radio_set_gain(&radio, &before, 30) == 0,
          "manual gain, 30 dB", 0, 0);
    radio_read(&radio, &r);
    check(!strcmp(r.mode, "manual"), "the mode read back is manual", 0, 0);
    near("the gain read back", r.gain, 30, 1e-9);
    double gain;
    check(radio_gain(&radio, &gain) == 0 && gain == 30, "radio_gain", gain, 30);

    /* Outside the ranges: clamped to their ends. */
    check(radio_set_gain(&radio, &before, 500) == 0, "a gain of 500 dB asked", 0, 0);
    radio_read(&radio, &r);
    near("clamped to the range's top", r.gain, before.gain_range.max, 1e-9);
    check(radio_set_lo(&radio, &before, 1) == 0, "an LO of 1 Hz asked", 0, 0);
    radio_read(&radio, &r);
    near("clamped to the range's bottom (its next whole MHz)", r.lo, before.lo_range.min, 1e-6);

    /* Everything back: the rate, the bandwidth, the LO, then the gain mode
     * (and the gain, if it was manual). */
    radio_set_rate(&radio, &before, before.rate);
    radio_set_bandwidth(&radio, &before, before.bandwidth);
    radio_set_lo(&radio, &before, before.lo);
    radio_set_gain_mode(&radio, before.mode);
    if (!strcmp(before.mode, "manual"))
        radio_set_gain(&radio, &before, before.gain);
    radio_read(&radio, &r);
    show("after", &r);
    near("the LO back", r.lo, before.lo, 1e-9);
    near("the rate back", r.rate, before.rate, 1e-6);
    near("the RF bandwidth back", r.bandwidth, before.bandwidth, 1e-6);
    check(!strcmp(r.mode, before.mode), "the gain mode back", 0, 0);

    iio_context_destroy(ctx);
    if (failures) {
        printf("%d check(s) failed.\n", failures);
        return 1;
    }
    printf("All checks passed.\n");
    return 0;
}
