/*
 * streamtest: how many of the Pluto's samples the link carries, with
 * nothing else done (no FFT, no window): for each block size, refill after
 * refill for a few seconds, counting the samples that came against the
 * sample rate's. 100% means no gaps between the blocks (audio needs that;
 * a spectrum does not). Sets the sample rate (and the RF bandwidth with
 * it, as plutosa does) for the measurement and puts both back. Then the
 * rate the samples really come at, as the spectrum sees it: the LO moved by a tenth of the
 * span, the signals in the spectrum move by span / 10 / rate of its width
 * (the LO put back after). Built and run on the RitterOS machine (make
 * measure from the Mac).
 *
 *   streamtest [URI] [MSPS] [SECONDS] [KERNEL_BUFFERS] [SAMPLES...]
 *   (ip:192.168.2.1, 2.1, 5, 4, 16384 65536 262144 1048576)
 */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsp.h"
#include "iio.h"
#include "radio.h"

#define SCALE_N     4096            /* the FFT's points for the rate's measurement */
#define SCALE_PEAKS 24

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* An averaged spectrum of a few blocks (after those the kernel kept from
 * before the last change); its strongest peaks into bins: how many, or a
 * link error. */
static int peaks(struct iio_device *dev, struct iio_channel *i, struct iio_channel *q,
                 int bins[SCALE_PEAKS])
{
    static float fi[SCALE_N], fq[SCALE_N];
    struct iio_buffer *buf = iio_device_create_buffer(dev, SCALE_N, false);
    if (!buf)
        return -errno;
    struct spectrum *sp = spectrum_new(SCALE_N, TAPER_BLACKMAN_HARRIS, 64);
    int ret = sp ? 0 : -ENOMEM;
    for (int k = 0; k < 40 && ret == 0; k++) {
        ssize_t n = iio_buffer_refill(buf);
        if (n < 0) {
            ret = (int)n;
            break;
        }
        if (k < 8)
            continue;
        ptrdiff_t step = iio_buffer_step(buf);
        const char *pi = iio_buffer_first(buf, i), *pq = iio_buffer_first(buf, q);
        for (int j = 0; j < SCALE_N; j++, pi += step, pq += step) {
            fi[j] = *(const int16_t *)pi / 2048.0f;
            fq[j] = *(const int16_t *)pq / 2048.0f;
        }
        spectrum_add(sp, fi, fq);
    }
    if (ret == 0)
        ret = spectrum_peaks(sp->average, SCALE_N, 8, bins, SCALE_PEAKS);
    spectrum_free(sp);
    iio_buffer_destroy(buf);
    return ret;
}

/* The rate the samples come at, from how far the peaks move when the LO
 * moves by span / 10: every pair of a peak before and one after votes for
 * its distance; the most voted (the signals', not the fixed DC and spurs'
 * near 0) gives it. */
static void measure_rate(struct iio_device *dev, struct iio_channel *i, struct iio_channel *q,
                         struct radio *radio, const struct receiver *r)
{
    int before[SCALE_PEAKS], after[SCALE_PEAKS];
    int nb = peaks(dev, i, q, before);
    double move = r->rate / 10;
    radio_set_lo(radio, r, r->lo + move);
    int na = nb > 0 ? peaks(dev, i, q, after) : 0;
    radio_set_lo(radio, r, r->lo);
    if (nb <= 0 || na <= 0) {
        printf("streamtest: no peaks to measure the rate with (%d, %d)\n", nb, na);
        return;
    }
    static int votes[SCALE_N];
    memset(votes, 0, sizeof(votes));
    for (int a = 0; a < nb; a++)
        for (int b = 0; b < na; b++) {
            int d = before[a] - after[b];       /* (the LO up: the signals down) */
            if (d > 3)
                votes[d]++;
        }
    int best = 0;
    for (int d = 4; d < SCALE_N; d++)
        if (votes[d] + votes[d - 1] > votes[best] + (best ? votes[best - 1] : 0))
            best = d;
    int agreeing = best ? votes[best] + votes[best - 1] : 0;
    if (agreeing < 6) {
        printf("streamtest: too few signals at this LO to measure the rate with (%d of %d "
               "peak pairs agree)\n", agreeing, nb * na);
        return;
    }
    /* The distance, between the two most voted neighbours. */
    double d = (best * votes[best] + (best - 1) * votes[best - 1]) /
               (double)(votes[best] + votes[best - 1]);
    double rate = move * SCALE_N / d;
    printf("streamtest: the LO moved %.0f Hz, the peaks %.1f bins of %d (%d of %d pairs): the "
           "samples come at %.0f per second, %.4f of the rate set\n",
           move, d, SCALE_N, agreeing, nb * na, rate, rate / r->rate);
}

/* One block size: refills for `seconds`; 0, or a link error. */
static int measure(struct iio_device *dev, size_t samples, double rate, double seconds,
                   unsigned kernel_buffers)
{
    iio_device_set_kernel_buffers_count(dev, kernel_buffers);
    struct iio_buffer *buf = iio_device_create_buffer(dev, samples, false);
    if (!buf) {
        printf("%8zu samples: no buffer (errno %d)\n", samples, errno);
        return -errno;
    }
    /* The first refills empty what the kernel kept from before. */
    for (unsigned k = 0; k < kernel_buffers; k++)
        if (iio_buffer_refill(buf) < 0)
            break;
    double start = now_s(), longest = 0, last = start;
    long blocks = 0;
    double got = 0;
    ssize_t n = 0;
    while (last - start < seconds) {
        n = iio_buffer_refill(buf);
        if (n < 0)
            break;
        double at = now_s();
        if (at - last > longest)
            longest = at - last;
        last = at;
        blocks++;
        got += (double)n / 4;                       /* (I and Q, 2 bytes each) */
    }
    iio_buffer_destroy(buf);
    if (n < 0) {
        printf("%8zu samples: refill failed (%zd)\n", samples, n);
        return (int)n;
    }
    double secs = last - start, block_s = (double)samples / rate;
    printf("%8zu samples: %5.2f MS/s of %.2f (%5.1f%%), %ld blocks, refill %.1f ms on average "
           "(%.1f ms longest) for %.1f ms of samples\n",
           samples, got / secs / 1e6, rate / 1e6, 100 * got / secs / rate, blocks,
           1e3 * secs / (double)blocks, 1e3 * longest, 1e3 * block_s);
    return 0;
}

int main(int argc, char **argv)
{
    const char *uri = argc > 1 ? argv[1] : "ip:192.168.2.1";
    double rate = (argc > 2 ? atof(argv[2]) : 2.1) * 1e6;
    double seconds = argc > 3 ? atof(argv[3]) : 5;
    unsigned kernel_buffers = argc > 4 ? (unsigned)atoi(argv[4]) : 4;
    size_t sizes[16] = { 16384, 65536, 262144, 1048576 };
    int count = 4;
    if (argc > 5) {
        count = 0;
        for (int k = 5; k < argc && count < 16; k++)
            sizes[count++] = (size_t)atol(argv[k]);
    }
    struct iio_context *ctx = iio_create_context_from_uri(uri);
    if (!ctx) {
        printf("streamtest: cannot connect to %s (errno %d)\n", uri, errno);
        return 1;
    }
    struct radio radio;
    struct iio_device *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    struct iio_channel *i = dev ? iio_device_find_channel(dev, "voltage0", false) : 0;
    struct iio_channel *q = dev ? iio_device_find_channel(dev, "voltage1", false) : 0;
    if (radio_open(&radio, ctx) < 0 || !i || !q) {
        printf("streamtest: %s is not a Pluto\n", uri);
        return 1;
    }
    iio_channel_enable(i);
    iio_channel_enable(q);
    struct receiver before, r;
    radio_read(&radio, &before);
    radio_set_rate(&radio, &before, rate);
    radio_set_bandwidth(&radio, &before, rate);
    radio_read(&radio, &r);
    printf("streamtest: %.6f MSPS (asked %.6f), %u kernel buffers, %.0f s a size\n", r.rate / 1e6,
           rate / 1e6, kernel_buffers, seconds);
    int err = 0;
    for (int k = 0; k < count && err == 0; k++)
        err = measure(dev, sizes[k], r.rate, seconds, kernel_buffers);
    if (err == 0)
        measure_rate(dev, i, q, &radio, &r);
    radio_set_rate(&radio, &before, before.rate);
    radio_set_bandwidth(&radio, &before, before.bandwidth);
    radio_read(&radio, &r);
    printf("streamtest: put back: %.6f MSPS, RF bandwidth %.6f MHz\n", r.rate / 1e6,
           r.bandwidth / 1e6);
    iio_context_destroy(ctx);
    return err ? 1 : 0;
}
