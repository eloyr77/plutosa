/*
 * plutosa: a spectrum analyzer for the ADALM-Pluto on RitterOS.
 *
 * Step 2: samples. A thread connects to the Pluto's iiod (libiio's network
 * backend, ip:192.168.2.1 by default), reads what the receiver is set to,
 * then reads blocks of I/Q samples from cf-ad9361-lpc one after another and
 * measures them: how many blocks arrive, the signal's power and peak, its
 * DC offset. The link cannot carry every sample at the Pluto's rates, so a
 * block is BLOCK contiguous samples and the samples between blocks are
 * lost: what a spectrum needs (one block per FFT). When the link fails, the
 * window says so and the thread connects again. What the window shows is
 * also printed on standard output every few seconds (`make run` brings it
 * back).
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "iio.h"
#include "ritteros.h"
#include "window.h"

PROGRAM_HELP("plutosa [URI]", "spectrum analyzer for the ADALM-Pluto (URI: ip:192.168.2.1)");

#define DEFAULT_URI "ip:192.168.2.1"
#define BLOCK       16384           /* samples per block (a 16384-point FFT at most) */
#define FULL_SCALE  2048.0          /* the AD936x's 12-bit samples: -2048..2047 */
#define SHOW_S      0.25            /* the measurements are redrawn this often */
#define PRINT_S     5.0             /* and printed this often */
#define RETRY_S     2               /* a failed connection is tried again after this */
#define LINES       8

static struct window *win;
static uint32_t bg, fg, dim, accent, bad;

/* What the window shows, written by the acquisition thread. The lock also
 * covers drawing, which both threads do (and window_resize, which replaces
 * the pixels being drawn into). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *uri;
static char status[160] = "connecting...";
static int failed;
static char radio[LINES][96];       /* the receiver's settings */
static int nradio;
static char stats[LINES][96];       /* the samples' measurements */
static int nstats;

static void draw_locked(void)
{
    int w = window_width(win), h = window_height(win);
    char title[160];
    window_fill(win, 0, 0, w, h, bg);
    snprintf(title, sizeof(title), "Pluto at %s", uri);
    window_text(win, 16, 12, title, fg, WINDOW_TRANSPARENT);
    window_text(win, 16, 32, status, failed ? bad : accent, WINDOW_TRANSPARENT);
    int y = 64;
    for (int i = 0; i < nradio; i++, y += 18)
        window_text(win, 16, y, radio[i], fg, WINDOW_TRANSPARENT);
    y += 6;
    window_fill(win, 16, y, w > 32 ? w - 32 : 0, 1, dim);
    y += 10;
    for (int i = 0; i < nstats; i++, y += 18)
        window_text(win, 16, y, stats[i], fg, WINDOW_TRANSPARENT);
    window_flush(win);
}

static void draw(void)
{
    pthread_mutex_lock(&lock);
    draw_locked();
    pthread_mutex_unlock(&lock);
}

/* Sets the status line, prints it and redraws. */
__attribute__((format(printf, 2, 3)))
static void set_status(int is_failure, const char *fmt, ...)
{
    pthread_mutex_lock(&lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    failed = is_failure;
    printf("plutosa: %s\n", status);
    draw_locked();
    pthread_mutex_unlock(&lock);
}

/* Adds a line to one of the window's lists (lock held). */
__attribute__((format(printf, 3, 4)))
static void add_line(char (*list)[96], int *n, const char *fmt, ...)
{
    if (*n == LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(list[*n], sizeof(list[0]), fmt, ap);
    va_end(ap);
    (*n)++;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static const char *errtext(int err)
{
    static char text[96];
    iio_strerror(err, text, sizeof(text));
    return text;
}

/* Reads one channel attribute as a number: -1 when it cannot be read. */
static double read_double(struct iio_channel *ch, const char *attr)
{
    double v;
    if (!ch || iio_channel_attr_read_double(ch, attr, &v) < 0)
        return -1;
    return v;
}

/* The receiver's settings into the window's first list; its sample rate
 * (0 if unknown). They live on the AD936x's control device: the RX LO is
 * output channel altvoltage0, the rest input channel voltage0. */
static double show_radio(struct iio_context *ctx)
{
    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *lo = phy ? iio_device_find_channel(phy, "altvoltage0", true) : 0;
    struct iio_channel *rx = phy ? iio_device_find_channel(phy, "voltage0", false) : 0;
    char mode[32] = "?";
    if (rx)
        iio_channel_attr_read(rx, "gain_control_mode", mode, sizeof(mode));
    double rate = read_double(rx, "sampling_frequency");
    pthread_mutex_lock(&lock);
    nradio = nstats = 0;
    if (!phy) {
        add_line(radio, &nradio, "no ad9361-phy: not a Pluto?");
    } else {
        add_line(radio, &nradio, "RX LO        %.6f MHz", read_double(lo, "frequency") / 1e6);
        add_line(radio, &nradio, "sample rate  %.6f MSPS", rate / 1e6);
        add_line(radio, &nradio, "RF bandwidth %.6f MHz", read_double(rx, "rf_bandwidth") / 1e6);
        add_line(radio, &nradio, "gain         %.0f dB (%s)", read_double(rx, "hardwaregain"), mode);
    }
    for (int i = 0; i < nradio; i++)
        printf("plutosa: %s\n", radio[i]);
    draw_locked();
    pthread_mutex_unlock(&lock);
    return rate > 0 ? rate : 0;
}

/* The measurements of one interval: every sample of the blocks that came. */
struct tally {
    double start;
    long blocks, samples;
    double sum_i, sum_q, sum_power;     /* I, Q and I^2 + Q^2, in full scales */
    int peak;                           /* the largest |I| or |Q| */
};

static double dbfs(double power)
{
    return power > 0 ? 10 * log10(power) : -INFINITY;
}

/* The interval's measurements into the window's second list. 0 dBFS is a
 * full-scale complex tone (I^2 + Q^2 = 1); the peak is the largest single
 * I or Q against full scale. */
static void show_tally(const struct tally *t, double now, double rate, int print)
{
    double secs = now - t->start, n = (double)t->samples;
    double got = n / secs;
    pthread_mutex_lock(&lock);
    nstats = 0;
    add_line(stats, &nstats, "blocks       %.1f/s of %d samples", (double)t->blocks / secs, BLOCK);
    if (rate > 0)
        add_line(stats, &nstats, "received     %.3f MS/s, %.1f%% of the samples", got / 1e6,
                 100 * got / rate);
    else
        add_line(stats, &nstats, "received     %.3f MS/s", got / 1e6);
    add_line(stats, &nstats, "power        %.1f dBFS, peak %.1f dBFS", dbfs(t->sum_power / n),
             20 * log10(t->peak / FULL_SCALE));
    add_line(stats, &nstats, "DC offset    I %+.2f%%  Q %+.2f%% of full scale",
             100 * t->sum_i / n, 100 * t->sum_q / n);
    if (print)
        for (int i = 0; i < nstats; i++)
            printf("plutosa: %s\n", stats[i]);
    draw_locked();
    pthread_mutex_unlock(&lock);
}

/* Reads blocks until the link fails: the error. */
static int stream(struct iio_context *ctx, double rate)
{
    struct iio_device *dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    struct iio_channel *ich = dev ? iio_device_find_channel(dev, "voltage0", false) : 0;
    struct iio_channel *qch = dev ? iio_device_find_channel(dev, "voltage1", false) : 0;
    if (!ich || !qch) {
        set_status(1, "no cf-ad9361-lpc with voltage0 and voltage1: not a Pluto?");
        return -ENODEV;
    }
    iio_channel_enable(ich);
    iio_channel_enable(qch);
    struct iio_buffer *buf = iio_device_create_buffer(dev, BLOCK, false);
    if (!buf)
        return -errno;
    set_status(0, "receiving: %s", iio_context_get_description(ctx));

    struct tally t = { .start = now_s() };
    double printed = t.start;
    for (;;) {
        ssize_t n = iio_buffer_refill(buf);
        if (n < 0) {
            iio_buffer_destroy(buf);
            return (int)n;
        }
        /* The samples are le:S12/16: already sign-extended, so a plain
         * 16-bit value. I and Q are interleaved, a step apart each. */
        ptrdiff_t step = iio_buffer_step(buf);
        const char *pi = iio_buffer_first(buf, ich), *pq = iio_buffer_first(buf, qch);
        const char *end = iio_buffer_end(buf);
        for (; pi < end && pq < end; pi += step, pq += step) {
            int i = *(const int16_t *)pi, q = *(const int16_t *)pq;
            double fi = i / FULL_SCALE, fq = q / FULL_SCALE;
            t.sum_i += fi;
            t.sum_q += fq;
            t.sum_power += fi * fi + fq * fq;
            int a = i < 0 ? -i : i, b = q < 0 ? -q : q;
            if (a > t.peak)
                t.peak = a;
            if (b > t.peak)
                t.peak = b;
            t.samples++;
        }
        t.blocks++;
        double now = now_s();
        if (now - t.start >= SHOW_S) {
            int print = now - printed >= PRINT_S;
            show_tally(&t, now, rate, print);
            if (print)
                printed = now;
            t = (struct tally){ .start = now };
        }
    }
}

static void *acquire_thread(void *arg)
{
    (void)arg;
    for (;;) {
        set_status(0, "connecting...");
        struct iio_context *ctx = iio_create_context_from_uri(uri);
        if (!ctx) {
            set_status(1, "cannot connect: %s (again in %d s)", errtext(errno), RETRY_S);
            sleep(RETRY_S);
            continue;
        }
        double rate = show_radio(ctx);
        int err = stream(ctx, rate);
        set_status(1, "link lost: %s (connecting again in %d s)", errtext(-err), RETRY_S);
        iio_context_destroy(ctx);
        sleep(RETRY_S);
    }
    return 0;
}

int main(int argc, char **argv)
{
    uri = argc > 1 ? argv[1] : DEFAULT_URI;
    win = window_open(560, 300, "plutosa");
    if (!win) {
        printf("plutosa: cannot open a window (is windowd running?)\n");
        return 1;
    }
    bg = window_rgb(win, 0x12, 0x16, 0x1E);
    fg = window_rgb(win, 0xE6, 0xE9, 0xEF);
    dim = window_rgb(win, 0x4C, 0x56, 0x6A);
    accent = window_rgb(win, 0x8F, 0xBC, 0xBB);
    bad = window_rgb(win, 0xBF, 0x61, 0x6A);
    draw();
    printf("plutosa: started, connecting to %s\n", uri);

    pthread_t acquirer;
    if (pthread_create(&acquirer, 0, acquire_thread, 0) != 0) {
        printf("plutosa: cannot start a thread\n");
        return 1;
    }
    pthread_detach(acquirer);

    struct window_event ev;
    while (window_wait(win, &ev) && ev.type != WINDOW_CLOSED) {
        if (ev.type == WINDOW_RESIZE) {
            pthread_mutex_lock(&lock);
            if (window_resize(win, ev.x, ev.y) == 0)    /* the new pixels are blank */
                draw_locked();
            pthread_mutex_unlock(&lock);
        }
    }
    /* Kept locked: the acquisition thread must not draw into a closed
     * window before the program's end ends it too. */
    pthread_mutex_lock(&lock);
    window_close(win);
    printf("plutosa: closed\n");
    return 0;
}
