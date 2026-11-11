/*
 * plutosa: a spectrum analyzer for the ADALM-Pluto on RitterOS.
 *
 * The threads. The acquisition thread connects to the Pluto's iiod
 * (libiio's network backend, ip:192.168.2.1 by default), reads what the
 * receiver is set to (radio.c), then reads blocks of I/Q samples from
 * cf-ad9361-lpc one after another and does nothing else, so that as few
 * samples as the link allows are lost between blocks (it cannot carry
 * every one at the Pluto's rates: the loss is measured and shown). The
 * blocks go into a small ring; the spectrum thread takes the newest each
 * time (skipping what it had no time for: a spectrum needs no more) and
 * makes it FFT frames (dsp.c), and the averaged spectrum, reduced to the
 * plot's pixel columns, is handed to the drawing thread, which draws at
 * most PICTURES a second (view.c): the text, the spectrum with its marker,
 * the waterfall. While the FM window is open (key d), the FM thread
 * takes every block in order through the demodulator (demod.c) and the
 * sound thread plays its audio through /dev/pcm0; the FM window has an
 * events thread of its own.
 * The main thread reads the window's events (the keys and the mouse, see
 * KEYS; a click on a value in the text opens the SDK's text field over it
 * to type a new one): changes to the receiver become a request that the
 * acquisition thread carries out between blocks. When the link fails, the window says
 * so and the acquisition thread connects again. The text is also printed
 * on standard output every few seconds (`make run` brings it back), and
 * with -p FILE the picture is written to FILE (a PPM image) as often: to
 * look at it from elsewhere.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "demod.h"
#include "dsp.h"
#include "iio.h"
#include "radio.h"
#include "ritteros.h"
#include "view.h"
#include "window.h"

PROGRAM_HELP("plutosa [URI] [-p FILE] [-f MHZ]",
             "spectrum analyzer for the ADALM-Pluto (URI: ip:192.168.2.1; -p: the picture to FILE "
             "(and FILE-fm); -f: the FM window open at MHZ)");

#define DEFAULT_URI "ip:192.168.2.1"
#define BLOCK       65536           /* samples per block (FFT frames of up to 16384) */
#define SLOTS       4               /* blocks in the ring */
#define LOSS_S      2.0             /* the loss is measured over this long */
#define GAIN_S      0.5             /* the gain (the AGC's) is read again this often */
#define FULL_SCALE  2048.0          /* the AD936x's 12-bit samples: -2048..2047 */
#define PICTURES    25              /* pictures a second at most */
#define SHOW_S      0.25            /* the text's measurements are made this often */
#define PRINT_S     5.0             /* and printed this often */
#define RETRY_S     2               /* a failed connection is tried again after this */
#define PEAKS       5               /* printed */
#define STALE       4               /* blocks thrown away after a change: iiod's kernel
                                     * buffers (4) may hold samples from before it */
#define GAIN_STEP   3               /* dB, + and - */
#define LEVEL_STEP  10              /* dB, the reference level's */
/* The FM window. */
#define FM_W        760
#define FM_H        520
#define FM_FFT      2048            /* points of the channel's spectrum (24 Hz at 50 kHz) */
#define FM_AVERAGES 4
#define FM_HALF     10000.0         /* Hz either side of the channel shown */
#define FM_STEP     500.0           /* Hz: the arrows and the wheel tune by this */
#define FM_MAX_BANDWIDTH 20000.0
#define FM_MIN_BANDWIDTH 1000.0
#define FM_MAX_VOLUME 4.0           /* 400% */
#define VOLUME_STEP 0.1
#define SQUELCH_STEP 3.0            /* dB, s and S */
#define FM_MIN_SQUELCH -130.0       /* dBFS */
#define FM_MAX_SQUELCH 0.0
#define CHANNEL_MAX 8192            /* channel samples a block makes, at most */

/* What the keys choose from. */
static const int FFT_SIZES[] = { 1024, 4096, 16384 };
static const int AVERAGES[] = { 1, 4, 16, 64 };
static const double SPANS[] = { 2.5e6, 5e6, 10e6, 20e6, 30.72e6, 40e6, 56e6 };
static const double BANDWIDTHS[] = { 0, 1e6, 2e6, 5e6, 10e6, 20e6, 40e6, 56e6 };   /* 0: = span */
static const char *const GAIN_MODES[] = { "slow_attack", "fast_attack", "hybrid", "manual" };
static const double FM_BANDWIDTHS[] = { 3000, 5000, 6250, 8330, 10000, 12500, 16000, 20000 };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char *const KEYS[2] = {
    "click a value or type MHz: set it  arrows/wheel: tune, level  double-click: center  "
    "s/S span",
    "b/B bandwidth  g gain mode  +/- gain  f FFT size  w window  a averaging  m max hold  r reset"
    "  d FM",
};
static const char *const FM_KEYS[2] = {     /* (80 characters at most: the window's width) */
    "click a value or type MHz  arrows/wheel: tune 0.5 kHz, level  double-click: tune",
    "+/- volume  b/B bandwidth  s/S squelch  m max hold  r reset  main: click tunes",
};

/* The values a click on the text edits, typed in their unit: the main
 * window's (its view_data's values 0-3) and the FM window's (its 0-3). */
enum {
    VALUE_LO, VALUE_SPAN, VALUE_BANDWIDTH, VALUE_GAIN,
    VALUE_FM_FREQUENCY, VALUE_FM_BANDWIDTH, VALUE_FM_VOLUME, VALUE_FM_SQUELCH, VALUES_COUNT
};
static const struct { const char *name, *unit; double scale; } VALUES[VALUES_COUNT] = {
    [VALUE_LO] = { "LO", "MHz", 1e6 },
    [VALUE_SPAN] = { "span", "MHz", 1e6 },
    [VALUE_BANDWIDTH] = { "RF bandwidth", "MHz", 1e6 },
    [VALUE_GAIN] = { "gain", "dB", 1 },
    [VALUE_FM_FREQUENCY] = { "FM frequency", "MHz", 1e6 },
    [VALUE_FM_BANDWIDTH] = { "FM bandwidth", "kHz", 1e3 },
    [VALUE_FM_VOLUME] = { "volume", "%", 0.01 },
    [VALUE_FM_SQUELCH] = { "squelch", "dBFS", 1 },
};
#define EDIT_W      120             /* the edit box's width at least, pixels */

static struct window *win;
static struct view *view;
static const char *uri;
static const char *picture_file;    /* -p FILE: the picture written there every PRINT_S */

/* Shared by the threads, under the lock, which also covers drawing and
 * window_resize (it replaces the pixels drawn into). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char status[200] = "connecting...";
static bool failed;
static char settings_line[200], measure_line[200];
static float shown_average[VIEW_MAX_COLUMNS], shown_hold[VIEW_MAX_COLUMNS];
static struct view_data shown = {
    .average = shown_average, .hold = shown_hold, .marker = -1,
    .ref_db = 0, .range_db = 100, .keys = { 0, 0 }, .button = "FM demodulator", .button_line = 0,
};
static bool fresh;                  /* a new spectrum since the last picture */
static bool redraw;                 /* something else changed */
/* The spectrum's choices: the acquisition thread makes a new spectrum when
 * `changed` moves on. */
static int fft_choice = 1, average_choice = 2;
static enum taper taper = TAPER_BLACKMAN_HARRIS;
static unsigned changed;
/* The receiver: what it is set to (the acquisition thread's last read,
 * while `connected`), and what the keys ask of it: the fields `asked`
 * names, carried out between blocks. */
static bool connected;
static struct receiver now;
static struct receiver want;
enum { ASK_LO = 1, ASK_RATE = 2, ASK_BANDWIDTH = 4, ASK_MODE = 8, ASK_GAIN = 16 };
static unsigned asked;
/* Something asked (set with __atomic_*, outside the lock): the acquisition
 * thread looks at it after every block without waiting for the lock, which
 * the drawing thread holds while it draws. */
static bool pending;
static int bandwidth_choice;        /* BANDWIDTHS: = span until chosen */
/* A value being typed in a window: the SDK's text field over it (the
 * window's events thread gives it the events, the drawing thread draws it
 * over each picture). */
struct editor {
    struct window_field *field;
    int editing;                    /* VALUES, -1: none */
    int box_x, box_y, box_w, box_h;
    char hint[200];                 /* shown instead of the status meanwhile */
    bool hint_bad;                  /* the last value typed was refused */
    int sequence;                   /* in_sequence's state for this window's keys */
};
static struct editor main_edit = { .editing = -1 };

/* The FM window (open while fm_win is not 0) and what it asks of the
 * demodulator: the channel's frequency (kept inside the span by the
 * demodulating thread), bandwidth and volume. */
static struct window *fm_win;
static struct view *fm_view;
static struct editor fm_edit = { .editing = -1 };
static double fm_hz, fm_bandwidth = 12500, fm_volume = 0.3;   /* (FM's hiss without a signal is loud) */
static bool fm_squelch_on;          /* the squelch, at fm_squelch dBFS (the channel's level) */
static double fm_squelch, fm_level = -60;          /* dBFS; the level the last measured */
static char fm_status[200], fm_values_line[200], fm_measure_line[200];
static bool fm_failed, fm_fresh, fm_redraw;
static float fm_average[VIEW_MAX_COLUMNS], fm_hold[VIEW_MAX_COLUMNS];
static struct view_data fm_shown = {
    .average = fm_average, .hold = fm_hold, .marker = -1, .ref_db = 0, .range_db = 100,
    .offsets = true, .button = "reset", .button_line = 1,
};
static bool fm_reset;               /* the FM window's measurements to start again */

/* Sets the status line and prints it. */
__attribute__((format(printf, 2, 3)))
static void set_status(bool is_failure, const char *fmt, ...)
{
    pthread_mutex_lock(&lock);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof(status), fmt, ap);
    va_end(ap);
    failed = is_failure;
    printf("plutosa: %s\n", status);
    redraw = true;
    pthread_mutex_unlock(&lock);
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

/* The measurements of one interval: every sample of the blocks that came. */
struct tally {
    double start;
    long blocks, samples, frames;
    double sum_i, sum_q, sum_power;     /* I, Q and I^2 + Q^2, in full scales */
    int peak;                           /* the largest |I| or |Q| */
};

/* Adds to text line `line` (1 or 2, d->lines' index); with value >= 0 (an
 * index in d->values), what it adds is that value, for a click to edit
 * (under the lock). */
__attribute__((format(printf, 6, 7)))
static void append(struct view_data *d, char *text, size_t size, int line, int value,
                   const char *fmt, ...)
{
    size_t start = strlen(text);
    if (start + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + start, size - start, fmt, ap);
    va_end(ap);
    if (value >= 0)
        d->values[value] = (struct view_value){ line, (int)start, (int)strlen(text) };
}

/* The text's two lines from an interval's measurements; printed now and
 * then, with the strongest peaks. 0 dBFS is a full-scale complex tone
 * (I^2 + Q^2 = 1); the peak is the largest single I or Q against full
 * scale. */
static void show_tally(const struct tally *t, const struct spectrum *sp, const struct receiver *r,
                       double received, double lost, double at, bool print)
{
    double secs = at - t->start, samples = (double)t->samples;
    int n = sp->n;
    double floor_db = power_db(spectrum_floor(sp->average, n));
    pthread_mutex_lock(&lock);
    /* (each about 100 characters at most: the 960-pixel window's width) */
    settings_line[0] = measure_line[0] = 0;
    append(&shown, settings_line, sizeof(settings_line), 1, -1, "LO ");
    append(&shown, settings_line, sizeof(settings_line), 1, VALUE_LO, "%.6f MHz", r->lo / 1e6);
    append(&shown, settings_line, sizeof(settings_line), 1, -1, "  span ");
    append(&shown, settings_line, sizeof(settings_line), 1, VALUE_SPAN, "%.3f MHz", r->rate / 1e6);
    append(&shown, settings_line, sizeof(settings_line), 1, -1, "  BW ");
    append(&shown, settings_line, sizeof(settings_line), 1, VALUE_BANDWIDTH, "%.3f MHz",
           r->bandwidth / 1e6);
    append(&shown, settings_line, sizeof(settings_line), 1, -1, "  RBW %.2f kHz  FFT %d %s  avg %d",
           r->rate * sp->enbw / n / 1e3, n, taper_name(sp->taper), sp->averages);
    append(&shown, measure_line, sizeof(measure_line), 2, -1, "gain ");
    append(&shown, measure_line, sizeof(measure_line), 2, VALUE_GAIN, "%.0f dB", r->gain);
    append(&shown, measure_line, sizeof(measure_line), 2, -1,
           " %s  dBFS: power %.1f peak %.1f floor %.1f/bin  %.0f FFT/s",
           r->mode, power_db(t->sum_power / samples), 20 * log10(t->peak / FULL_SCALE), floor_db,
           (double)t->frames / secs);
    if (lost >= 0)                          /* (measured over LOSS_S: not at once) */
        append(&shown, measure_line, sizeof(measure_line), 2, -1, "  %.2f MS/s %.0f%% lost",
               received / 1e6, 100 * lost);
    redraw = true;
    if (print) {
        printf("plutosa: %s\nplutosa: %s\n", settings_line, measure_line);
        int bins[PEAKS];
        int count = spectrum_peaks(sp->average, n, (int)ceil(2 * sp->enbw), bins, PEAKS);
        for (int k = 0; k < count; k++)
            printf("plutosa: peak %.6f MHz %.1f dBFS (max hold %.1f)\n",
                   (r->lo + (double)(bins[k] - n / 2) * r->rate / n) / 1e6,
                   power_db(sp->average[bins[k]]), power_db(sp->hold[bins[k]]));
    }
    pthread_mutex_unlock(&lock);
}

/* Hands the spectrum to the drawing thread: reduced to the plot's columns
 * (outside the lock), with its strongest peak for the marker (at least two
 * noise bandwidths from any stronger one). */
static void publish(const struct spectrum *sp, const struct receiver *r)
{
    static float average[VIEW_MAX_COLUMNS], hold[VIEW_MAX_COLUMNS];
    pthread_mutex_lock(&lock);
    int columns = view_columns(view);
    pthread_mutex_unlock(&lock);
    int n = sp->n, bin;
    spectrum_columns(sp->average, n, average, columns);
    spectrum_columns(sp->hold, n, hold, columns);
    bool marked = spectrum_peaks(sp->average, n, (int)ceil(2 * sp->enbw), &bin, 1) == 1;
    pthread_mutex_lock(&lock);
    if (columns == view_columns(view)) {    /* (else resized meanwhile: the next one) */
        memcpy(shown_average, average, (size_t)columns * sizeof(float));
        memcpy(shown_hold, hold, (size_t)columns * sizeof(float));
        shown.columns = columns;
        shown.lo = r->lo > 0 ? r->lo : 0;
        shown.rate = r->rate > 0 ? r->rate : 0;
        shown.marker = marked ? (int)((long)bin * columns / n) : -1;
        if (marked) {
            shown.marker_hz = shown.lo + (double)(bin - n / 2) * shown.rate / n;
            shown.marker_db = power_db(sp->average[bin]);
        }
        fresh = true;
    }
    pthread_mutex_unlock(&lock);
}

/* The spectrum the keys chose: a new one when they changed (out of
 * memory: the old one stays, 0 if there is none). */
static struct spectrum *chosen_spectrum(struct spectrum *old, unsigned *seen)
{
    pthread_mutex_lock(&lock);
    int n = FFT_SIZES[fft_choice], averages = AVERAGES[average_choice];
    enum taper t = taper;
    unsigned at = changed;
    pthread_mutex_unlock(&lock);
    if (old && at == *seen)
        return old;
    struct spectrum *s = spectrum_new(n, t, averages);
    if (!s)
        return old;
    spectrum_free(old);
    *seen = at;
    return s;
}

/* What the receiver is set to, read again and shared. */
static void reread(const struct radio *radio, struct receiver *r)
{
    radio_read(radio, r);
    pthread_mutex_lock(&lock);
    now = *r;
    redraw = true;
    printf("plutosa: RX LO %.6f MHz, %.6f MSPS, RF bandwidth %.6f MHz, gain %.0f dB (%s)\n",
           r->lo / 1e6, r->rate / 1e6, r->bandwidth / 1e6, r->gain, r->mode);
    pthread_mutex_unlock(&lock);
}

/* --- The blocks: the acquisition thread writes them into a ring, the
 * spectrum thread copies the newest out (both under blocks_lock, held only
 * for the copy) --- */

struct block {
    unsigned long seq;              /* the how-manyeth block (1, 2, ...) */
    unsigned stream;                /* a new number after each change of the receiver */
    double lo, rate;                /* what it was taken at */
    int count;
    int16_t iq[2 * BLOCK];          /* I, Q, I, Q, ... as they came (le:S12/16) */
};

static pthread_mutex_t blocks_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blocks_came = PTHREAD_COND_INITIALIZER;
static struct block ring[SLOTS];
static unsigned long written;       /* the newest block's seq (0: none yet) */
/* The link, measured by the acquisition thread: samples per second that
 * came, the part of the Pluto's lost between blocks (-1: not yet known),
 * and the gain (the AGC moves it; -1000: not read). */
static double link_received, link_lost = -1, link_gain = -1000;

struct stream {
    struct iio_device *dev;
    struct iio_channel *ich, *qch;
    struct iio_buffer *buf;
};

static int stream_open(struct stream *s)
{
    s->buf = iio_device_create_buffer(s->dev, BLOCK, false);
    return s->buf ? 0 : -errno;
}

/* Carries out what the keys asked (between blocks): the sample rate with
 * the stream closed (the AD936x's clocks change under it), then the rest;
 * the first refusal stops there and is shown. 1 when something was done,
 * 0 when nothing was asked, or a link error. */
static int carry_out(const struct radio *radio, struct stream *s, struct receiver *r,
                     const char *receiving)
{
    __atomic_store_n(&pending, false, __ATOMIC_RELEASE);
    pthread_mutex_lock(&lock);
    unsigned what = asked;
    struct receiver w = want;
    asked = 0;
    pthread_mutex_unlock(&lock);
    if (!what)
        return 0;
    int err = 0;
    const char *which = "";
    if (what & ASK_RATE) {
        iio_buffer_destroy(s->buf);
        s->buf = 0;
        which = "the sample rate", err = radio_set_rate(radio, r, w.rate);
        int opened = stream_open(s);
        if (opened < 0)
            return opened;
    }
    if (!err && (what & ASK_BANDWIDTH))
        which = "the RF bandwidth", err = radio_set_bandwidth(radio, r, w.bandwidth);
    if (!err && (what & ASK_LO))
        which = "the LO frequency", err = radio_set_lo(radio, r, w.lo);
    if (!err && (what & ASK_MODE))
        which = "the gain mode", err = radio_set_gain_mode(radio, w.mode);
    if (!err && (what & ASK_GAIN))
        which = "the gain", err = radio_set_gain(radio, r, w.gain);
    reread(radio, r);
    if (err == -EPIPE || err == -ETIMEDOUT)
        return err;                         /* (the link, not the Pluto) */
    if (err)
        set_status(true, "the Pluto refused %s: %s", which, errtext(-err));
    else
        set_status(false, "%s", receiving);
    return 1;
}

/* A block into the ring: its I and Q (a step apart each in the buffer). */
static void put_block(const struct stream *s, unsigned stream, const struct receiver *r)
{
    ptrdiff_t step = iio_buffer_step(s->buf);
    const char *pi = iio_buffer_first(s->buf, s->ich), *pq = iio_buffer_first(s->buf, s->qch);
    const char *end = iio_buffer_end(s->buf);
    pthread_mutex_lock(&blocks_lock);
    struct block *b = &ring[(written + 1) % SLOTS];
    int got = 0;
    for (; pi < end && pq < end && got < BLOCK; pi += step, pq += step, got++) {
        b->iq[2 * got] = *(const int16_t *)pi;
        b->iq[2 * got + 1] = *(const int16_t *)pq;
    }
    b->seq = ++written;
    b->stream = stream;
    b->lo = r->lo;
    b->rate = r->rate;
    b->count = got;
    pthread_cond_broadcast(&blocks_came);
    pthread_mutex_unlock(&blocks_lock);
}

/* Reads blocks until the link fails: the error. Nothing else is done here
 * but what the keys asked and, now and then, reading the gain. */
static int stream(struct iio_context *ctx, const struct radio *radio, struct receiver *r)
{
    static unsigned streams;
    struct stream s = { .dev = iio_context_find_device(ctx, "cf-ad9361-lpc") };
    s.ich = s.dev ? iio_device_find_channel(s.dev, "voltage0", false) : 0;
    s.qch = s.dev ? iio_device_find_channel(s.dev, "voltage1", false) : 0;
    if (!s.ich || !s.qch) {
        set_status(true, "no cf-ad9361-lpc with voltage0 and voltage1: not a Pluto?");
        return -ENODEV;
    }
    iio_channel_enable(s.ich);
    iio_channel_enable(s.qch);
    int err = stream_open(&s);
    if (err < 0)
        return err;
    /* The board and its firmware, from the context's attributes (Tezuka
     * and ADI's firmware give both); the kernel's description otherwise. */
    char receiving[200];
    const char *model = iio_context_get_attr_value(ctx, "hw_model");
    const char *firmware = iio_context_get_attr_value(ctx, "fw_version");
    if (model)
        snprintf(receiving, sizeof(receiving), "receiving from %s: %s, %s", uri, model,
                 firmware ? firmware : "");
    else
        snprintf(receiving, sizeof(receiving), "receiving from %s: %s", uri,
                 iio_context_get_description(ctx));
    set_status(false, "%s", receiving);

    unsigned stream = ++streams;
    int stale = STALE;
    double since = 0, samples = 0, gain_read = 0;   /* (since: 0 until the first block) */
    for (;;) {
        ssize_t n = iio_buffer_refill(s.buf);
        if (n < 0) {
            err = (int)n;
            break;
        }
        if (__atomic_load_n(&pending, __ATOMIC_ACQUIRE)) {
            err = carry_out(radio, &s, r, receiving);
            if (err < 0)
                break;
            if (err > 0) {
                stream = ++streams;
                stale = STALE;
                since = 0;
                pthread_mutex_lock(&blocks_lock);
                link_lost = -1;             /* (the new rate's is not known yet) */
                pthread_mutex_unlock(&blocks_lock);
            }
            continue;
        }
        if (stale > 0) {
            stale--;
            continue;
        }
        put_block(&s, stream, r);
        /* The loss: the samples that came against the Pluto's, from the
         * first block's arrival (its own samples not counted). */
        double at = now_s();
        if (since == 0) {
            since = at, samples = 0;
        } else {
            samples += (double)(n / (ssize_t)iio_buffer_step(s.buf));
            if (at - since >= LOSS_S) {
                double expected = r->rate * (at - since);
                pthread_mutex_lock(&blocks_lock);
                link_received = samples / (at - since);
                link_lost = samples < expected ? 1 - samples / expected : 0;
                pthread_mutex_unlock(&blocks_lock);
                since = at, samples = 0;
            }
        }
        if (at - gain_read >= GAIN_S) {
            double gain;
            if (radio_gain(radio, &gain) == 0) {
                r->gain = gain;
                pthread_mutex_lock(&blocks_lock);
                link_gain = gain;
                pthread_mutex_unlock(&blocks_lock);
            }
            gain_read = at;
        }
    }
    if (s.buf)
        iio_buffer_destroy(s.buf);
    pthread_mutex_lock(&blocks_lock);
    link_lost = -1;
    pthread_mutex_unlock(&blocks_lock);
    return err;
}

static void *acquire_thread(void *arg)
{
    (void)arg;
    for (;;) {
        set_status(false, "connecting to %s...", uri);
        struct iio_context *ctx = iio_create_context_from_uri(uri);
        if (!ctx) {
            set_status(true, "cannot connect to %s: %s (again in %d s)", uri, errtext(errno),
                       RETRY_S);
            sleep(RETRY_S);
            continue;
        }
        struct radio radio;
        struct receiver r;
        int err = radio_open(&radio, ctx);
        if (err == 0) {
            reread(&radio, &r);
            pthread_mutex_lock(&lock);
            want = r;                       /* (nothing asked yet: the keys start here) */
            asked = 0;
            connected = true;
            pthread_mutex_unlock(&lock);
            err = stream(ctx, &radio, &r);
        }
        pthread_mutex_lock(&lock);
        connected = false;
        pthread_mutex_unlock(&lock);
        if (err == -ENODEV)
            set_status(true, "%s is not a Pluto (no ad9361-phy receiver); again in %d s", uri,
                       RETRY_S);
        else
            set_status(true, "link lost: %s (connecting again in %d s)", errtext(-err), RETRY_S);
        iio_context_destroy(ctx);
        sleep(RETRY_S);
    }
    return 0;
}

/* The spectrum: the newest block each time, made FFT frames; a new
 * spectrum (and measurement) when the receiver changed. */
static void *spectrum_thread(void *arg)
{
    (void)arg;
    static struct block b;
    static float fi[BLOCK], fq[BLOCK];
    struct spectrum *sp = 0;
    unsigned seen = 0, stream = 0;
    struct tally t = { 0 };
    double printed = 0, published = 0;
    for (;;) {
        pthread_mutex_lock(&blocks_lock);
        while (written == 0 || ring[written % SLOTS].seq == b.seq)
            pthread_cond_wait(&blocks_came, &blocks_lock);
        b = ring[written % SLOTS];
        double received = link_received, lost = link_lost, gain = link_gain;
        pthread_mutex_unlock(&blocks_lock);
        struct spectrum *chosen = chosen_spectrum(sp, &seen);
        if (!chosen)
            continue;                       /* (out of memory: the next block) */
        double at = now_s();
        if (chosen != sp || b.stream != stream) {
            sp = chosen;
            spectrum_reset(sp);
            stream = b.stream;
            t = (struct tally){ .start = at };
        }
        for (int k = 0; k < b.count; k++) {
            int i = b.iq[2 * k], q = b.iq[2 * k + 1];
            double di = i / FULL_SCALE, dq = q / FULL_SCALE;
            fi[k] = (float)di;
            fq[k] = (float)dq;
            t.sum_i += di;
            t.sum_q += dq;
            t.sum_power += di * di + dq * dq;
            int a = i < 0 ? -i : i, c = q < 0 ? -q : q;
            if (a > t.peak)
                t.peak = a;
            if (c > t.peak)
                t.peak = c;
        }
        t.samples += b.count;
        t.blocks++;
        for (int f = 0; f + sp->n <= b.count; f += sp->n, t.frames++)
            spectrum_add(sp, fi + f, fq + f);
        pthread_mutex_lock(&lock);
        struct receiver r = now;
        if (gain > -1000)
            r.gain = now.gain = gain;
        pthread_mutex_unlock(&lock);
        r.lo = b.lo;
        r.rate = b.rate;
        at = now_s();
        if (at - published >= 1.0 / PICTURES) {
            publish(sp, &r);
            published = at;
        }
        if (at - t.start >= SHOW_S && t.samples > 0) {
            bool print = at - printed >= PRINT_S;
            show_tally(&t, sp, &r, received, lost, at, print);
            if (print)
                printed = at;
            t = (struct tally){ .start = at };
        }
    }
    return 0;
}

/* --- The sound: the FM audio to /dev/pcm0 --- */

/* The FM thread puts its audio into a buffer of our own and never waits
 * for the sound card; the sound thread writes it to /dev/pcm0 in pieces
 * (a write waits while the card's ring is full: the card's clock sets the
 * pace). The Pluto's clock and the card's differ a little, so the buffer
 * would fill up or run dry: the demodulator's resampler is paced to keep
 * it at SOUND_TARGET. Run dry (blocks lost), silence is written instead
 * (the card's ring never empties) until the buffer is at SOUND_TARGET
 * again; overflowing, the newest audio is dropped. Its own lock:
 * sound_lock is taken inside `lock`, never the other way round. */
#define SOUND_FRAMES 24000          /* the buffer's room: 0.5 s at 48 kHz */
#define SOUND_TARGET 4800           /* frames kept in it: 100 ms */
#define SOUND_PIECE  480            /* frames a write: 10 ms */
#define SOUND_PACE_S 1.0            /* the pace is set again this often */
#define SOUND_GAIN   0.002          /* the pace's change for the buffer a whole SOUND_TARGET off */
#define SOUND_PACE   0.005          /* the pace's furthest from 1 */

static pthread_mutex_t sound_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sound_wake = PTHREAD_COND_INITIALIZER;
static bool sound_wanted;           /* the FM window is open */
static int16_t sound_buffer[2 * SOUND_FRAMES];
static int sound_head, sound_fill;  /* frames: the oldest's place, how many */
static bool sound_playing;          /* (else silence until sound_fill reaches SOUND_TARGET) */
static bool sound_played;           /* since the window opened (silence before is no gap) */
static double sound_pace = 1, sound_level = -1;   /* the level: ms in the buffer, -1 unknown */
static long sound_silent, sound_dropped;          /* frames since the FM window's reset */
static char sound_error[120];       /* /dev/pcm0 refused (empty: none) */

/* The FM window opened (true) or closed. */
static void sound_want(bool on)
{
    pthread_mutex_lock(&sound_lock);
    sound_wanted = on;
    pthread_cond_broadcast(&sound_wake);
    pthread_mutex_unlock(&sound_lock);
}

/* The FM thread's audio into the buffer (the newest dropped when full). */
static void sound_put(const int16_t *audio, int frames)
{
    pthread_mutex_lock(&sound_lock);
    int room = SOUND_FRAMES - sound_fill;
    if (frames > room) {
        sound_dropped += frames - room;
        frames = room;
    }
    for (int k = 0; k < frames; k++) {
        int at = (sound_head + sound_fill + k) % SOUND_FRAMES;
        sound_buffer[2 * at] = audio[2 * k];
        sound_buffer[2 * at + 1] = audio[2 * k + 1];
    }
    sound_fill += frames;
    pthread_mutex_unlock(&sound_lock);
}

static int write_all(int fd, const void *p, size_t n)
{
    const char *b = p;
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w <= 0)
            return -1;
        b += w, n -= (size_t)w;
    }
    return 0;
}

/* While the FM window is open: /dev/pcm0 open, a piece written after
 * another, the pace set from the buffer's mean level. */
static void *sound_thread(void *arg)
{
    (void)arg;
    static int16_t piece[2 * SOUND_PIECE];
    for (;;) {
        pthread_mutex_lock(&sound_lock);
        while (!sound_wanted)
            pthread_cond_wait(&sound_wake, &sound_lock);
        sound_head = sound_fill = 0;
        sound_playing = sound_played = false;
        sound_pace = 1, sound_level = -1;
        pthread_mutex_unlock(&sound_lock);
        int fd = open("/dev/pcm0", O_WRONLY);
        if (fd < 0) {
            pthread_mutex_lock(&sound_lock);
            snprintf(sound_error, sizeof(sound_error), "/dev/pcm0: %s", strerror(errno));
            while (sound_wanted)                /* (tried again when the window opens again) */
                pthread_cond_wait(&sound_wake, &sound_lock);
            pthread_mutex_unlock(&sound_lock);
            continue;
        }
        double level = 0, since = now_s();
        int levels = 0;
        for (;;) {
            pthread_mutex_lock(&sound_lock);
            if (!sound_wanted) {
                pthread_mutex_unlock(&sound_lock);
                break;
            }
            sound_error[0] = 0;
            if (sound_playing && sound_fill < SOUND_PIECE)
                sound_playing = false;          /* (run dry: silence until it is full again) */
            else if (!sound_playing && sound_fill >= SOUND_TARGET)
                sound_playing = sound_played = true;
            if (sound_playing) {
                for (int k = 0; k < SOUND_PIECE; k++) {
                    piece[2 * k] = sound_buffer[2 * sound_head];
                    piece[2 * k + 1] = sound_buffer[2 * sound_head + 1];
                    sound_head = (sound_head + 1) % SOUND_FRAMES;
                }
                sound_fill -= SOUND_PIECE;
                level += sound_fill;
                levels++;
            } else {
                memset(piece, 0, sizeof(piece));
                if (sound_played)
                    sound_silent += SOUND_PIECE;
            }
            double at = now_s();
            if (at - since >= SOUND_PACE_S) {
                if (levels > 0) {
                    double mean = level / levels;
                    double pace = 1 + SOUND_GAIN * (mean - SOUND_TARGET) / SOUND_TARGET;
                    sound_pace = pace > 1 + SOUND_PACE ? 1 + SOUND_PACE
                               : pace < 1 - SOUND_PACE ? 1 - SOUND_PACE : pace;
                    sound_level = mean * 1000 / DEMOD_AUDIO_RATE;
                }
                level = 0, levels = 0;
                since = at;
            }
            pthread_mutex_unlock(&sound_lock);
            if (write_all(fd, piece, sizeof(piece)) < 0) {
                pthread_mutex_lock(&sound_lock);
                snprintf(sound_error, sizeof(sound_error), "/dev/pcm0: %s", strerror(errno));
                while (sound_wanted)
                    pthread_cond_wait(&sound_wake, &sound_lock);
                pthread_mutex_unlock(&sound_lock);
                break;
            }
        }
        close(fd);                              /* (plays out the rest) */
    }
    return 0;
}

/* --- The FM demodulator's thread: every block in order --- */

static void fm_values(void);
static double fm_inside(double hz, double lo, double rate);

/* The channel's spectrum to the FM window: its middle +-FM_HALF reduced
 * to the plot's columns, with its strongest peak for the marker. */
static void fm_publish(const struct spectrum *sp, double channel_rate, double hz)
{
    static float average[VIEW_MAX_COLUMNS], hold[VIEW_MAX_COLUMNS];
    int n = sp->n;
    double bin = channel_rate / n;
    int half = (int)lround(FM_HALF / bin);
    if (half > n / 2)
        half = n / 2;
    int start = n / 2 - half, count = 2 * half, peak;
    pthread_mutex_lock(&lock);
    int columns = fm_view ? view_columns(fm_view) : 0;
    pthread_mutex_unlock(&lock);
    if (columns <= 0)
        return;
    spectrum_columns(sp->average + start, count, average, columns);
    spectrum_columns(sp->hold + start, count, hold, columns);
    bool marked = spectrum_peaks(sp->average + start, count, (int)ceil(2 * sp->enbw), &peak, 1) == 1;
    pthread_mutex_lock(&lock);
    if (fm_view && columns == view_columns(fm_view)) {
        memcpy(fm_average, average, (size_t)columns * sizeof(float));
        memcpy(fm_hold, hold, (size_t)columns * sizeof(float));
        fm_shown.columns = columns;
        fm_shown.lo = hz;
        fm_shown.rate = count * bin;
        fm_shown.marker = marked ? (int)((long)peak * columns / count) : -1;
        if (marked) {
            fm_shown.marker_hz = hz + (peak - half) * bin;
            fm_shown.marker_db = power_db(sp->average[start + peak]);
        }
        fm_fresh = true;
    }
    pthread_mutex_unlock(&lock);
}

/* While the FM window is open: every block through the demodulator, in
 * order (one missed, overwritten in the ring before it came to it, or a
 * change of the receiver, starts its discriminator again); the channel's
 * samples become the window's spectrum, the audio is measured and goes
 * to the sound thread. The channel stays inside the span. */
static void *fm_thread(void *arg)
{
    (void)arg;
    static struct block b;
    static float ci[CHANNEL_MAX], cq[CHANNEL_MAX], frame_i[FM_FFT], frame_q[FM_FFT];
    static int16_t sound[2 * CHANNEL_MAX];
    struct spectrum *sp = spectrum_new(FM_FFT, TAPER_BLACKMAN_HARRIS, FM_AVERAGES);
    struct demod *d = 0;
    double d_rate = 0;
    unsigned long last = 0, missed = 0;
    unsigned stream = 0;
    bool was_open = false;
    int filled = 0, loudest = 0;
    double power = 0, start = now_s(), published = 0, printed = 0;
    long channel = 0;
    if (!sp)
        return 0;
    for (;;) {
        pthread_mutex_lock(&blocks_lock);
        while (written == last)
            pthread_cond_wait(&blocks_came, &blocks_lock);
        unsigned long next = last == 0 ? written : last + 1;
        unsigned long oldest = written >= SLOTS ? written - SLOTS + 1 : 1;
        bool gap = false;
        if (next < oldest) {
            missed += oldest - next;
            next = oldest;
            gap = true;
        }
        b = ring[next % SLOTS];
        last = next;
        double lost = link_lost;
        pthread_mutex_unlock(&blocks_lock);

        pthread_mutex_lock(&lock);
        bool open = fm_win != 0;
        double hz = fm_hz, bandwidth = fm_bandwidth, volume = fm_volume;
        double squelch = fm_squelch_on ? pow(10, fm_squelch / 10) : 0;
        if (open) {
            double inside = fm_inside(hz, b.lo, b.rate);
            if (inside != hz) {
                fm_hz = hz = inside;
                fm_values();
            }
        }
        pthread_mutex_unlock(&lock);
        if (!open) {
            was_open = false;
            continue;
        }
        if (!was_open) {                        /* (opened: everything anew) */
            was_open = true;
            gap = true;
            missed = 0;
            spectrum_reset(sp);
            filled = 0;
        }
        pthread_mutex_lock(&lock);
        bool reset = fm_reset;
        fm_reset = false;
        pthread_mutex_unlock(&lock);
        if (reset) {                            /* (the window's reset: the measurements anew) */
            missed = 0;
            spectrum_reset(sp);
            power = 0, channel = 0, loudest = 0;
            start = now_s();
            pthread_mutex_lock(&sound_lock);
            sound_silent = sound_dropped = 0;
            pthread_mutex_unlock(&sound_lock);
        }
        if (!d || d_rate != b.rate) {
            demod_free(d);
            d = demod_new(b.rate, hz - b.lo, bandwidth);
            d_rate = b.rate;
            spectrum_reset(sp);
            filled = 0;
            if (!d)
                continue;
        } else if (gap || b.stream != stream) {
            demod_gap(d);
        }
        stream = b.stream;
        demod_tune(d, hz - b.lo, bandwidth);
        demod_volume(d, volume);
        demod_squelch(d, squelch);
        pthread_mutex_lock(&sound_lock);
        demod_pace(d, sound_pace);
        pthread_mutex_unlock(&sound_lock);
        struct demod_out out = { .ci = ci, .cq = cq, .channel_max = CHANNEL_MAX,
                                 .audio = sound, .frames_max = CHANNEL_MAX };
        demod_run(d, b.iq, b.count, &out);
        sound_put(sound, out.frames);
        power += out.power * out.channel;
        channel += out.channel;
        for (int j = 0; j < out.frames; j++) {
            int a = sound[2 * j] < 0 ? -sound[2 * j] : sound[2 * j];
            if (a > loudest)
                loudest = a;
        }
        for (int j = 0; j < out.channel; j++) {
            frame_i[filled] = ci[j];
            frame_q[filled] = cq[j];
            if (++filled == FM_FFT) {
                spectrum_add(sp, frame_i, frame_q);
                filled = 0;
            }
        }
        double at = now_s();
        if (at - published >= 1.0 / PICTURES && sp->frames > 0) {
            fm_publish(sp, demod_channel_rate(d), hz);
            published = at;
        }
        if (at - start >= SHOW_S && channel > 0) {
            pthread_mutex_lock(&sound_lock);
            char sound_text[160];
            if (sound_error[0])
                snprintf(sound_text, sizeof(sound_text), "sound: %s", sound_error);
            else if (sound_level < 0)
                snprintf(sound_text, sizeof(sound_text), "sound starting");
            else
                snprintf(sound_text, sizeof(sound_text), "sound %.0f ms %+.3f%%", sound_level,
                         (sound_pace - 1) * 100);
            bool sound_failed = sound_error[0] != 0;
            long silent = sound_silent, dropped = sound_dropped;
            pthread_mutex_unlock(&sound_lock);
            pthread_mutex_lock(&lock);
            snprintf(fm_status, sizeof(fm_status),
                     "narrow FM %+.3f kHz from the LO  channel %.0f/s (/%d)  %s",
                     (hz - b.lo) / 1e3, demod_channel_rate(d), demod_decimation(d), sound_text);
            fm_failed = lost > 0.01 || missed > 0 || sound_failed;
            fm_level = power_db(power / (double)channel);
            int n = demod_squelch_open(d)
                  ? snprintf(fm_measure_line, sizeof(fm_measure_line),
                             "level %.1f dBFS  audio peak %.1f dBFS", fm_level,
                             loudest > 0 ? 20 * log10(loudest / 32767.0) : -999.0)
                  : snprintf(fm_measure_line, sizeof(fm_measure_line),
                             "level %.1f dBFS  squelch closed", fm_level);
            if (lost >= 0 && n > 0 && n < (int)sizeof(fm_measure_line))
                n += snprintf(fm_measure_line + n, sizeof(fm_measure_line) - (size_t)n,
                              "  %.0f%% lost", 100 * lost);
            if (missed > 0 && n > 0 && n < (int)sizeof(fm_measure_line))
                n += snprintf(fm_measure_line + n, sizeof(fm_measure_line) - (size_t)n,
                              "  %lu blocks missed", missed);
            if (silent > 0 && n > 0 && n < (int)sizeof(fm_measure_line))
                n += snprintf(fm_measure_line + n, sizeof(fm_measure_line) - (size_t)n,
                              "  %.1f s silent", (double)silent / DEMOD_AUDIO_RATE);
            if (dropped > 0 && n > 0 && n < (int)sizeof(fm_measure_line))
                snprintf(fm_measure_line + n, sizeof(fm_measure_line) - (size_t)n,
                         "  %.1f s dropped", (double)dropped / DEMOD_AUDIO_RATE);
            if (at - printed >= PRINT_S) {
                printf("%s\n%s\n", fm_status, fm_measure_line);
                printed = at;
            }
            fm_redraw = true;
            pthread_mutex_unlock(&lock);
            power = 0, channel = 0, loudest = 0;
            start = at;
        }
    }
    return 0;
}

/* Draws when there is something new, at most PICTURES a second; each new
 * spectrum is a waterfall row. */
static void *draw_thread(void *arg)
{
    (void)arg;
    double saved = now_s();
    for (;;) {
        usleep(1000000 / PICTURES);
        pthread_mutex_lock(&lock);
        if (fresh || redraw) {
            shown.lines[0] = main_edit.editing >= 0 ? main_edit.hint : status;
            shown.lines[1] = settings_line;
            shown.lines[2] = measure_line;
            shown.failed = main_edit.editing >= 0 ? main_edit.hint_bad : failed;
            shown.band_lo = fm_win ? fm_hz - fm_bandwidth / 2 : 0;
            shown.band_hi = fm_win ? fm_hz + fm_bandwidth / 2 : 0;
            view_draw(view, &shown, fresh);
            if (main_edit.editing >= 0)
                window_field_draw(main_edit.field);
            window_flush(win);
            fresh = redraw = false;
        }
        if (fm_win && (fm_fresh || fm_redraw)) {
            fm_shown.lines[0] = fm_edit.editing >= 0 ? fm_edit.hint : fm_status;
            fm_shown.lines[1] = fm_values_line;
            fm_shown.lines[2] = fm_measure_line;
            fm_shown.failed = fm_edit.editing >= 0 ? fm_edit.hint_bad : fm_failed;
            fm_shown.band_lo = fm_hz - fm_bandwidth / 2;
            fm_shown.band_hi = fm_hz + fm_bandwidth / 2;
            view_draw(fm_view, &fm_shown, fm_fresh);
            if (fm_edit.editing >= 0)
                window_field_draw(fm_edit.field);
            window_flush(fm_win);
            fm_fresh = fm_redraw = false;
        }
        if (picture_file && now_s() - saved >= PRINT_S) {
            if (view_save(view, picture_file) < 0)
                printf("plutosa: cannot write %s: %s\n", picture_file, strerror(errno));
            /* The FM window's beside it: FILE-fm (before a .ppm). */
            char fm_file[512];
            size_t len = strlen(picture_file);
            bool ppm = len > 4 && !strcmp(picture_file + len - 4, ".ppm");
            snprintf(fm_file, sizeof(fm_file), "%.*s-fm%s", (int)(ppm ? len - 4 : len),
                     picture_file, ppm ? ".ppm" : "");
            if (fm_win && view_save(fm_view, fm_file) < 0)
                printf("plutosa: cannot write %s: %s\n", fm_file, strerror(errno));
            saved = now_s();
        }
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

/* --- The keys and the mouse (the main thread; all under the lock) --- */

/* The receiver's value a new request starts from: what is already asked
 * and not yet carried out, else what it is set to. */
static double base_lo(void) { return asked & ASK_LO ? want.lo : now.lo; }
static double base_rate(void) { return asked & ASK_RATE ? want.rate : now.rate; }

static void ask_lo(double hz)
{
    want.lo = range_clamp(&now.lo_range, hz);
    asked |= ASK_LO;
}

/* The span (the sample rate); the RF bandwidth follows it until chosen. */
static void ask_rate(double rate)
{
    want.rate = range_clamp(&now.rate_range, rate);
    asked |= ASK_RATE;
    if (BANDWIDTHS[bandwidth_choice] == 0) {
        want.bandwidth = want.rate;
        asked |= ASK_BANDWIDTH;
    }
}

/* The next (dir 1) or previous (-1) span from the list, from the present. */
static void ask_span(int dir)
{
    double rate = base_rate();
    int k;
    if (dir > 0) {
        for (k = 0; k < COUNT(SPANS) - 1 && SPANS[k] <= rate + 1; k++)
            ;
    } else {
        for (k = COUNT(SPANS) - 1; k > 0 && SPANS[k] >= rate - 1; k--)
            ;
    }
    ask_rate(SPANS[k]);
}

static void ask_bandwidth(int dir)
{
    bandwidth_choice = (bandwidth_choice + dir + COUNT(BANDWIDTHS)) % COUNT(BANDWIDTHS);
    double hz = BANDWIDTHS[bandwidth_choice];
    want.bandwidth = range_clamp(&now.bandwidth_range, hz > 0 ? hz : base_rate());
    asked |= ASK_BANDWIDTH;
}

/* A bandwidth typed: chosen, no longer the span's; b and B go on from the
 * list's next one up. */
static void ask_bandwidth_hz(double hz)
{
    bandwidth_choice = COUNT(BANDWIDTHS) - 1;
    for (int k = 1; k < COUNT(BANDWIDTHS); k++)
        if (BANDWIDTHS[k] >= hz - 1) {
            bandwidth_choice = k;
            break;
        }
    want.bandwidth = range_clamp(&now.bandwidth_range, hz);
    asked |= ASK_BANDWIDTH;
}

/* The next gain mode after the present one; into manual at the gain the
 * AGC had, so the level does not jump. */
static void ask_gain_mode(void)
{
    const char *mode = asked & ASK_MODE ? want.mode : now.mode;
    int k = 0;
    while (k < COUNT(GAIN_MODES) && strcmp(GAIN_MODES[k], mode) != 0)
        k++;
    k = (k + 1) % COUNT(GAIN_MODES);
    snprintf(want.mode, sizeof(want.mode), "%s", GAIN_MODES[k]);
    asked |= ASK_MODE;
    if (!strcmp(GAIN_MODES[k], "manual") && now.gain >= now.gain_range.min) {
        want.gain = now.gain;
        asked |= ASK_GAIN;
    }
}

/* A gain is the manual mode's. */
static void ask_gain_db(double db)
{
    want.gain = range_clamp(&now.gain_range, db);
    snprintf(want.mode, sizeof(want.mode), "manual");
    asked |= ASK_MODE | ASK_GAIN;
}

static void ask_gain(int dir)
{
    ask_gain_db((asked & ASK_GAIN ? want.gain : now.gain) + dir * GAIN_STEP);
}

static void set_level(struct view_data *d, double db)
{
    if (db > 30)
        db = 30;
    if (db < -150)
        db = -150;
    d->ref_db = db;
}

/* --- A value typed in an edit box --- */

/* The range value k may be typed in (in Hz, dB or a fraction). */
static struct range value_range(int k)
{
    switch (k) {
    case VALUE_LO:
        return now.lo_range;
    case VALUE_SPAN:
        return now.rate_range;
    case VALUE_BANDWIDTH:
        return now.bandwidth_range;
    case VALUE_GAIN:
        return now.gain_range;
    case VALUE_FM_FREQUENCY: {
        /* Inside the span, the channel's band too. */
        double half = base_rate() / 2 - fm_bandwidth / 2;
        return (struct range){ base_rate() > 0, base_lo() - half, 0, base_lo() + half };
    }
    case VALUE_FM_BANDWIDTH:
        return (struct range){ true, FM_MIN_BANDWIDTH, 0, FM_MAX_BANDWIDTH };
    case VALUE_FM_VOLUME:
        return (struct range){ true, 0, 0, FM_MAX_VOLUME };
    default:
        return (struct range){ true, FM_MIN_SQUELCH, 0, FM_MAX_SQUELCH };
    }
}

/* What value k is, or is asked to be. */
static double value_now(int k)
{
    switch (k) {
    case VALUE_LO:
        return base_lo();
    case VALUE_SPAN:
        return base_rate();
    case VALUE_BANDWIDTH:
        return asked & ASK_BANDWIDTH ? want.bandwidth : now.bandwidth;
    case VALUE_GAIN:
        return asked & ASK_GAIN ? want.gain : now.gain;
    case VALUE_FM_FREQUENCY:
        return fm_hz;
    case VALUE_FM_BANDWIDTH:
        return fm_bandwidth;
    case VALUE_FM_VOLUME:
        return fm_volume;
    default:
        return fm_squelch;
    }
}

static void redraw_window_of(const struct editor *e)
{
    if (e == &fm_edit)
        fm_redraw = true;
    else
        redraw = true;
}

/* The line shown instead of the status while value k is typed: its unit
 * and range; `bad` after a refusal (in red). */
static void describe(struct editor *e, int k, bool bad)
{
    struct range r = value_range(k);
    double scale = VALUES[k].scale;
    int n = snprintf(e->hint, sizeof(e->hint), "%s%s in %s", bad ? "not a number in range: " : "",
                     VALUES[k].name, VALUES[k].unit);
    if (r.known && n > 0 && n < (int)sizeof(e->hint))
        n += snprintf(e->hint + n, sizeof(e->hint) - (size_t)n, ", %.7g to %.7g", r.min / scale,
                      r.max / scale);
    if (k == VALUE_FM_SQUELCH && n > 0 && n < (int)sizeof(e->hint))
        n += snprintf(e->hint + n, sizeof(e->hint) - (size_t)n, " (empty or off: off)");
    if (n > 0 && n < (int)sizeof(e->hint))
        snprintf(e->hint + n, sizeof(e->hint) - (size_t)n, "   (Enter sets it, Esc gives up)");
    e->hint_bad = bad;
    redraw_window_of(e);
}

/* Opens the edit box over value k, with its present value selected (typing
 * replaces it), or empty. */
static void edit_value(struct editor *e, int k, bool empty)
{
    char text[32] = "";
    if (k == VALUE_FM_SQUELCH && !fm_squelch_on) {
        snprintf(text, sizeof(text), "off");
    } else if (!empty) {
        snprintf(text, sizeof(text), "%.6f", value_now(k) / VALUES[k].scale);
        char *end = text + strlen(text);        /* 2400.000000 -> 2400, 455.500000 -> 455.5 */
        while (end > text && end[-1] == '0')
            *--end = 0;
        if (end > text && end[-1] == '.')
            end[-1] = 0;
    }
    e->box_h = window_field_height(e->field);
    if (e == &fm_edit)
        view_value_box(fm_view, &fm_shown, k - VALUE_FM_FREQUENCY, e->box_h, EDIT_W, &e->box_x,
                       &e->box_y, &e->box_w);
    else
        view_value_box(view, &shown, k, e->box_h, EDIT_W, &e->box_x, &e->box_y, &e->box_w);
    window_field_place(e->field, e->box_x, e->box_y, e->box_w);
    window_field_set_text(e->field, text);
    window_field_focus(e->field, 1);
    e->editing = k;
    describe(e, k, false);
}

static void stop_editing(struct editor *e)
{
    window_field_focus(e->field, 0);
    e->editing = -1;
    redraw_window_of(e);
}

/* The FM channel's bandwidth: the next (dir 1) or previous (-1) from the
 * list, from the present. */
static void fm_step_bandwidth(int dir)
{
    int k;
    if (dir > 0) {
        for (k = 0; k < COUNT(FM_BANDWIDTHS) - 1 && FM_BANDWIDTHS[k] <= fm_bandwidth + 1; k++)
            ;
    } else {
        for (k = COUNT(FM_BANDWIDTHS) - 1; k > 0 && FM_BANDWIDTHS[k] >= fm_bandwidth - 1; k--)
            ;
    }
    fm_bandwidth = FM_BANDWIDTHS[k];
}

static void fm_set_volume(double volume)
{
    fm_volume = volume < 0 ? 0 : volume > FM_MAX_VOLUME ? FM_MAX_VOLUME : volume;
}

/* The squelch on at db dBFS (clamped to its range). */
static void fm_set_squelch(double db)
{
    fm_squelch = db < FM_MIN_SQUELCH ? FM_MIN_SQUELCH : db > FM_MAX_SQUELCH ? FM_MAX_SQUELCH : db;
    fm_squelch_on = true;
}

/* s (dir 1) and S (-1): the squelch SQUELCH_STEP up or down; s turns it
 * on SQUELCH_STEP over the channel's level (just closed on its hiss). */
static void fm_step_squelch(int dir)
{
    if (!fm_squelch_on && dir > 0)
        fm_set_squelch(round(fm_level) + SQUELCH_STEP);
    else if (fm_squelch_on)
        fm_set_squelch(fm_squelch + dir * SQUELCH_STEP);
}

/* The edit box's text as value k: asked of the receiver (or set for the
 * demodulator), or false (not a number, or outside the range: nothing
 * done). */
static bool ask_value(int k, const char *text)
{
    if (k == VALUE_FM_SQUELCH) {                /* (empty or "off": off) */
        const char *t = text;
        while (*t == ' ')
            t++;
        if (!*t || !strncmp(t, "off", 3)) {
            for (t += *t ? 3 : 0; *t == ' '; t++)
                ;
            if (!*t) {
                fm_squelch_on = false;
                return true;
            }
        }
    }
    char *end;
    double typed = strtod(text, &end), v = typed * VALUES[k].scale;
    while (*end == ' ')
        end++;
    struct range r = value_range(k);
    double slack = VALUES[k].scale * 1e-6;      /* (the range as shown: 7 digits) */
    if (end == text || *end || !isfinite(typed) ||
        (r.known && (v < r.min - slack || v > r.max + slack)))
        return false;
    switch (k) {
    case VALUE_LO:
        ask_lo(v);
        break;
    case VALUE_SPAN:
        ask_rate(v);
        break;
    case VALUE_BANDWIDTH:
        ask_bandwidth_hz(v);
        break;
    case VALUE_GAIN:
        ask_gain_db(v);
        break;
    case VALUE_FM_FREQUENCY:
        fm_hz = v;
        break;
    case VALUE_FM_BANDWIDTH:
        fm_bandwidth = v;
        break;
    case VALUE_FM_VOLUME:
        fm_set_volume(v);
        break;
    default:
        fm_set_squelch(v);
    }
    return true;
}

/* While a window's edit box is open, it has the events first: true when
 * one was its (every key is, while it is open). Escape or a click outside
 * it closes it; Enter sets the value, or says why not and leaves it open. */
static bool field_event(struct editor *e, const struct window_event *ev, bool pressed)
{
    if (e->editing < 0)
        return false;
    bool key = ev->type == WINDOW_KEY || ev->type == WINDOW_KEY_DOWN || ev->type == WINDOW_KEY_UP;
    if (ev->type == WINDOW_KEY_DOWN && ev->key == WKEY_ESCAPE) {
        stop_editing(e);
        return true;
    }
    if (pressed && !(ev->x >= e->box_x && ev->x < e->box_x + e->box_w && ev->y >= e->box_y &&
                     ev->y < e->box_y + e->box_h)) {
        stop_editing(e);
        return false;                           /* (the click is the window's) */
    }
    int taken = window_field_event(e->field, ev);
    if (taken == 1) {
        if (!connected)
            stop_editing(e);
        else if (ask_value(e->editing, window_field_text(e->field)))
            stop_editing(e);
        else
            describe(e, e->editing, true);
    }
    return taken >= 0 || key;
}

/* Whether a key's byte is part of an escape sequence (the bytes of an
 * arrow or another key without a character, ESC [ ... or ESC O ..., which
 * come as WINDOW_KEY_DOWN too): the B of the down arrow's must not step
 * the bandwidth. `state` is the window's (1: after ESC, 2: in one). */
static bool in_sequence(int *state, int byte)
{
    if (byte == 27) {
        *state = 1;
        return true;
    }
    if (*state == 1) {
        *state = byte == '[' || byte == 'O' ? 2 : 0;
        return *state == 2;
    }
    if (*state == 2) {
        if (byte >= 0x40 && byte <= 0x7E)       /* (the final byte) */
            *state = 0;
        return true;
    }
    return false;
}

/* --- The FM window (its events thread; all under the lock) --- */

/* The channel inside the span (lo +- rate / 2), its band too. */
static double fm_inside(double hz, double lo, double rate)
{
    double half = rate / 2 - fm_bandwidth / 2;
    if (rate <= 0 || half <= 0)
        return hz;
    return hz > lo + half ? lo + half : hz < lo - half ? lo - half : hz;
}

/* The FM window's values line (its values 0-3: frequency, bandwidth,
 * volume, squelch) from what is asked now. */
static void fm_values(void)
{
    fm_values_line[0] = 0;
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, -1, "frequency ");
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, 0, "%.6f MHz", fm_hz / 1e6);
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, -1, "  bandwidth ");
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, 1, "%.2f kHz", fm_bandwidth / 1e3);
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, -1, "  volume ");
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, 2, "%.0f%%", fm_volume * 100);
    append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, -1, "  squelch ");
    if (fm_squelch_on)
        append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, 3, "%.0f dBFS", fm_squelch);
    else
        append(&fm_shown, fm_values_line, sizeof(fm_values_line), 1, 3, "off");
    fm_redraw = true;
}

static void *fm_events(void *arg);

/* Opens the FM window at hz (0: where it was, else the main window's
 * marker, else a quarter span above the LO). */
static void fm_open(double hz)
{
    if (fm_win)
        return;
    if (hz > 0)
        fm_hz = hz;
    else if (fm_hz <= 0 || (connected && fm_inside(fm_hz, base_lo(), base_rate()) != fm_hz))
        fm_hz = shown.marker >= 0 ? shown.marker_hz : base_lo() + base_rate() / 4;
    if (connected)
        fm_hz = fm_inside(fm_hz, base_lo(), base_rate());
    fm_win = window_open(FM_W, FM_H, "plutosa FM");
    if (!fm_win) {
        snprintf(status, sizeof(status), "cannot open the FM window");
        failed = true;
        redraw = true;
        return;
    }
    fm_view = view_new(fm_win);
    fm_edit.field = window_field_new(fm_win, 0, 0, EDIT_W);
    fm_edit.editing = -1;
    fm_edit.sequence = 0;
    pthread_t events;
    if (!fm_view || !fm_edit.field || view_layout(fm_view) < 0 ||
        pthread_create(&events, 0, fm_events, 0) != 0) {
        if (fm_edit.field)
            window_field_free(fm_edit.field);
        fm_edit.field = 0;
        view_free(fm_view);
        fm_view = 0;
        window_close(fm_win);
        fm_win = 0;
        snprintf(status, sizeof(status), "cannot open the FM window: out of memory");
        failed = true;
        redraw = true;
        return;
    }
    pthread_detach(events);
    sound_want(true);
    fm_shown.keys[0] = FM_KEYS[0];
    fm_shown.keys[1] = FM_KEYS[1];
    fm_shown.columns = 0;
    fm_shown.marker = -1;
    snprintf(fm_status, sizeof(fm_status), "narrow FM: waiting for samples");
    fm_failed = false;
    snprintf(fm_measure_line, sizeof(fm_measure_line), " ");
    fm_values();
    redraw = true;                              /* (the channel's band in the main window) */
}

/* The FM window's events, until it is closed (then it is closed here). */
static void *fm_events(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    struct window *w = fm_win;
    pthread_mutex_unlock(&lock);
    struct window_event ev;
    int px = -1, py = -1;
    unsigned buttons = 0;
    while (window_wait(w, &ev) && ev.type != WINDOW_CLOSED) {
        pthread_mutex_lock(&lock);
        double hz = 0;
        bool pressed = false;
        if (ev.type == WINDOW_POINTER) {
            pressed = (ev.buttons & 1) && !(buttons & 1);
            buttons = ev.buttons;
            px = ev.x, py = ev.y;
        }
        bool sequence = ev.type == WINDOW_KEY && in_sequence(&fm_edit.sequence, ev.key);
        if (field_event(&fm_edit, &ev, pressed)) {
            /* (the edit box's) */
        } else if (ev.type == WINDOW_KEY_DOWN) {
            if (ev.key == WKEY_LEFT || ev.key == WKEY_RIGHT)
                fm_hz += (ev.key == WKEY_RIGHT ? 1 : -1) * FM_STEP;
            else if (ev.key == WKEY_UP || ev.key == WKEY_DOWN)
                set_level(&fm_shown, fm_shown.ref_db + (ev.key == WKEY_UP ? LEVEL_STEP : -LEVEL_STEP));
        } else if (ev.type == WINDOW_KEY && !sequence) {
            if (connected && ((ev.key >= '0' && ev.key <= '9') || ev.key == '.')) {
                edit_value(&fm_edit, VALUE_FM_FREQUENCY, true);
                window_field_event(fm_edit.field, &ev);
            } else if (ev.key == '+' || ev.key == '=') {
                fm_set_volume(fm_volume + VOLUME_STEP);
            } else if (ev.key == '-') {
                fm_set_volume(fm_volume - VOLUME_STEP);
            } else if (ev.key == 'b' || ev.key == 'B') {
                fm_step_bandwidth(ev.key == 'b' ? 1 : -1);
            } else if (ev.key == 's' || ev.key == 'S') {
                fm_step_squelch(ev.key == 's' ? 1 : -1);
            } else if (ev.key == 'm') {
                fm_shown.show_hold = !fm_shown.show_hold;
            } else if (ev.key == 'r') {
                fm_reset = true;
            }
        } else if (ev.type == WINDOW_POINTER) {
            int k = pressed ? view_value_at(fm_view, &fm_shown, ev.x, ev.y) : -1;
            if (pressed && view_button_at(fm_view, &fm_shown, ev.x, ev.y))
                fm_reset = true;
            else if (k >= 0 && connected)
                edit_value(&fm_edit, VALUE_FM_FREQUENCY + k, false);
        } else if (ev.type == WINDOW_DOUBLE_CLICK) {
            enum view_place at = view_at(fm_view, &fm_shown, ev.x, ev.y, &hz);
            if (fm_shown.rate > 0 && (at == VIEW_PLOT || at == VIEW_WATERFALL))
                fm_hz = hz;
        } else if (ev.type == WINDOW_SCROLL) {
            enum view_place at = view_at(fm_view, &fm_shown, px, py, &hz);
            if (at == VIEW_LEVELS)
                set_level(&fm_shown, fm_shown.ref_db - ev.y * LEVEL_STEP);
            else if (at == VIEW_PLOT || at == VIEW_WATERFALL)
                fm_hz -= ev.y * FM_STEP;
        } else if (ev.type == WINDOW_RESIZE) {
            if (window_resize(w, ev.x, ev.y) == 0 && view_layout(fm_view) < 0)
                printf("plutosa: out of memory for the FM window's new size\n");
            fm_shown.columns = 0;
        }
        if (connected)
            fm_hz = fm_inside(fm_hz, base_lo(), base_rate());
        fm_values();
        redraw = true;                          /* (the band in the main window) */
        pthread_mutex_unlock(&lock);
    }
    sound_want(false);
    pthread_mutex_lock(&lock);
    window_field_free(fm_edit.field);
    fm_edit.field = 0;
    fm_edit.editing = -1;
    view_free(fm_view);
    fm_view = 0;
    window_close(w);
    fm_win = 0;
    redraw = true;
    pthread_mutex_unlock(&lock);
    return 0;
}

/* The physical keys (WINDOW_KEY_DOWN): the arrows. */
static void key_down(int key)
{
    if (key == WKEY_LEFT || key == WKEY_RIGHT)
        ask_lo(base_lo() + (key == WKEY_RIGHT ? 1 : -1) * base_rate() / 10);
    else if (key == WKEY_UP || key == WKEY_DOWN)
        set_level(&shown, shown.ref_db + (key == WKEY_UP ? LEVEL_STEP : -LEVEL_STEP));
}

/* The keys' bytes (WINDOW_KEY): the letters, + and -. */
static void key_typed(int key)
{
    switch (key) {
    case 'f':
        fft_choice = (fft_choice + 1) % COUNT(FFT_SIZES);
        changed++;
        break;
    case 'w':
        taper = (taper + 1) % TAPERS;
        changed++;
        break;
    case 'a':
        average_choice = (average_choice + 1) % COUNT(AVERAGES);
        changed++;
        break;
    case 'r':
        changed++;
        break;
    case 'm':
        shown.show_hold = !shown.show_hold;
        break;
    case 's':
    case 'S':
        ask_span(key == 's' ? 1 : -1);
        break;
    case 'b':
    case 'B':
        ask_bandwidth(key == 'b' ? 1 : -1);
        break;
    case 'g':
        ask_gain_mode();
        break;
    case '+':
    case '=':                                   /* (+ without Shift) */
        ask_gain(1);
        break;
    case '-':
        ask_gain(-1);
        break;
    case 'd':
        fm_open(0);
        break;
    }
}

int main(int argc, char **argv)
{
    uri = DEFAULT_URI;
    double fm_start = 0;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "-p") && k + 1 < argc) {
            picture_file = argv[++k];
        } else if (!strcmp(argv[k], "-f") && k + 1 < argc) {
            fm_start = atof(argv[++k]) * 1e6;
        } else if (argv[k][0] != '-') {
            uri = argv[k];
        } else {
            printf("usage: plutosa [URI] [-p FILE] [-f MHZ]\n");
            return 2;
        }
    }
    win = window_open(960, 640, "plutosa");
    if (!win) {
        printf("plutosa: cannot open a window (is windowd running?)\n");
        return 1;
    }
    view = view_new(win);
    main_edit.field = window_field_new(win, 0, 0, EDIT_W);
    if (!view || !main_edit.field || view_layout(view) < 0) {
        printf("plutosa: out of memory\n");
        return 1;
    }
    shown.keys[0] = KEYS[0];
    shown.keys[1] = KEYS[1];
    redraw = true;
    printf("plutosa: started, connecting to %s (the FFT in %s)\n", uri,
           spectrum_avx2(-1) ? "AVX2" : "plain C");

    pthread_t acquirer, analyser, demodulator, player, drawer;
    if (pthread_create(&acquirer, 0, acquire_thread, 0) != 0 ||
        pthread_create(&analyser, 0, spectrum_thread, 0) != 0 ||
        pthread_create(&demodulator, 0, fm_thread, 0) != 0 ||
        pthread_create(&player, 0, sound_thread, 0) != 0 ||
        pthread_create(&drawer, 0, draw_thread, 0) != 0) {
        printf("plutosa: cannot start a thread\n");
        return 1;
    }
    pthread_detach(acquirer);
    pthread_detach(analyser);
    pthread_detach(demodulator);
    pthread_detach(player);
    pthread_detach(drawer);
    if (fm_start > 0) {
        pthread_mutex_lock(&lock);
        fm_open(fm_start);
        pthread_mutex_unlock(&lock);
    }

    struct window_event ev;
    int px = -1, py = -1;               /* where the pointer is (the wheel's place) */
    unsigned buttons = 0;               /* held at the last pointer event */
    while (window_wait(win, &ev) && ev.type != WINDOW_CLOSED) {
        pthread_mutex_lock(&lock);
        double hz = 0;
        bool pressed = false;           /* the left button, just now */
        if (ev.type == WINDOW_POINTER) {
            pressed = (ev.buttons & 1) && !(buttons & 1);
            buttons = ev.buttons;
            px = ev.x, py = ev.y;
        }
        bool sequence = ev.type == WINDOW_KEY && in_sequence(&main_edit.sequence, ev.key);
        if (field_event(&main_edit, &ev, pressed)) {
            /* (the edit box's) */
        } else if (ev.type == WINDOW_KEY_DOWN) {
            /* (the receiver's keys wait for a connection: its ranges) */
            if (connected || ev.key == WKEY_UP || ev.key == WKEY_DOWN)
                key_down(ev.key);
        } else if (ev.type == WINDOW_KEY && !sequence) {
            if (connected && ((ev.key >= '0' && ev.key <= '9') || ev.key == '.')) {
                edit_value(&main_edit, VALUE_LO, true);   /* a frequency typed: the LO's box */
                window_field_event(main_edit.field, &ev);
            } else if (connected || !strchr("sSbBg+=-", ev.key)) {
                key_typed(ev.key);
            }
        } else if (ev.type == WINDOW_POINTER) {
            int k = pressed ? view_value_at(view, &shown, ev.x, ev.y) : -1;
            enum view_place at = pressed ? view_at(view, &shown, ev.x, ev.y, &hz) : VIEW_ELSEWHERE;
            if (pressed && view_button_at(view, &shown, ev.x, ev.y)) {
                fm_open(0);
            } else if (k >= 0 && connected) {
                edit_value(&main_edit, k, false);
            } else if (fm_win && connected && shown.rate > 0 &&
                       (at == VIEW_PLOT || at == VIEW_WATERFALL)) {
                fm_hz = fm_inside(hz, base_lo(), base_rate());     /* (the FM channel there) */
                fm_values();
            }
        } else if (ev.type == WINDOW_DOUBLE_CLICK) {
            enum view_place at = view_at(view, &shown, ev.x, ev.y, &hz);
            if (connected && shown.rate > 0 && (at == VIEW_PLOT || at == VIEW_WATERFALL))
                ask_lo(hz);
        } else if (ev.type == WINDOW_SCROLL) {
            /* Up (negative steps): higher in frequency, or the level. */
            enum view_place at = view_at(view, &shown, px, py, &hz);
            if (at == VIEW_LEVELS)
                set_level(&shown, shown.ref_db - ev.y * LEVEL_STEP);
            else if (connected && (at == VIEW_PLOT || at == VIEW_WATERFALL))
                ask_lo(base_lo() - ev.y * base_rate() / 10);
        } else if (ev.type == WINDOW_RESIZE) {
            if (window_resize(win, ev.x, ev.y) == 0 && view_layout(view) < 0)
                printf("plutosa: out of memory for the new size\n");
            shown.columns = 0;              /* (the next spectrum fits the new width) */
        }
        redraw = true;
        if (asked)
            __atomic_store_n(&pending, true, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&lock);
    }
    /* Kept locked: the other threads must not draw into a closed window
     * before the program's end ends them too. */
    pthread_mutex_lock(&lock);
    window_close(win);
    printf("plutosa: closed\n");
    return 0;
}
