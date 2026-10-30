/*
 * plutosa: a spectrum analyzer for the ADALM-Pluto on RitterOS.
 *
 * Four threads. The acquisition thread connects to the Pluto's iiod
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
 * the waterfall.
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
#include "dsp.h"
#include "iio.h"
#include "radio.h"
#include "ritteros.h"
#include "view.h"
#include "window.h"

PROGRAM_HELP("plutosa [URI] [-p FILE]",
             "spectrum analyzer for the ADALM-Pluto (URI: ip:192.168.2.1; -p: the picture to FILE)");

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

/* What the keys choose from. */
static const int FFT_SIZES[] = { 1024, 4096, 16384 };
static const int AVERAGES[] = { 1, 4, 16, 64 };
static const double SPANS[] = { 2.5e6, 5e6, 10e6, 20e6, 30.72e6, 40e6, 56e6 };
static const double BANDWIDTHS[] = { 0, 1e6, 2e6, 5e6, 10e6, 20e6, 40e6, 56e6 };   /* 0: = span */
static const char *const GAIN_MODES[] = { "slow_attack", "fast_attack", "hybrid", "manual" };
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char *const KEYS[2] = {
    "click a value or type MHz: set it  arrows/wheel: tune, level  double-click: center  "
    "s/S span",
    "b/B bandwidth  g gain mode  +/- gain  f FFT size  w window  a averaging  m max hold  r reset",
};

/* The values a click on the text edits (view_data's values), typed in
 * their unit. */
enum { VALUE_LO, VALUE_SPAN, VALUE_BANDWIDTH, VALUE_GAIN };
static const struct { const char *name, *unit; double scale; } VALUES[VIEW_VALUES] = {
    [VALUE_LO] = { "LO", "MHz", 1e6 },
    [VALUE_SPAN] = { "span", "MHz", 1e6 },
    [VALUE_BANDWIDTH] = { "RF bandwidth", "MHz", 1e6 },
    [VALUE_GAIN] = { "gain", "dB", 1 },
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
    .ref_db = 0, .range_db = 100, .keys = { 0, 0 },
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
/* A value being typed: the SDK's text field over it (the main thread
 * gives it the events, the drawing thread draws it over each picture). */
static struct window_field *field;
static int editing = -1;            /* VALUES, -1: none */
static int box_x, box_y, box_w, box_h;
static char hint[200];              /* shown instead of the status meanwhile */
static bool hint_bad;               /* the last value typed was refused */

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

/* Adds to text line `line` (1 or 2, shown.lines' index); with value >= 0,
 * what it adds is that value, for a click to edit (under the lock). */
__attribute__((format(printf, 5, 6)))
static void append(char *text, size_t size, int line, int value, const char *fmt, ...)
{
    size_t start = strlen(text);
    if (start + 1 >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text + start, size - start, fmt, ap);
    va_end(ap);
    if (value >= 0)
        shown.values[value] = (struct view_value){ line, (int)start, (int)strlen(text) };
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
    append(settings_line, sizeof(settings_line), 1, -1, "LO ");
    append(settings_line, sizeof(settings_line), 1, VALUE_LO, "%.6f MHz", r->lo / 1e6);
    append(settings_line, sizeof(settings_line), 1, -1, "  span ");
    append(settings_line, sizeof(settings_line), 1, VALUE_SPAN, "%.3f MHz", r->rate / 1e6);
    append(settings_line, sizeof(settings_line), 1, -1, "  BW ");
    append(settings_line, sizeof(settings_line), 1, VALUE_BANDWIDTH, "%.3f MHz",
           r->bandwidth / 1e6);
    append(settings_line, sizeof(settings_line), 1, -1, "  RBW %.2f kHz  FFT %d %s  avg %d",
           r->rate * sp->enbw / n / 1e3, n, taper_name(sp->taper), sp->averages);
    append(measure_line, sizeof(measure_line), 2, -1, "gain ");
    append(measure_line, sizeof(measure_line), 2, VALUE_GAIN, "%.0f dB", r->gain);
    append(measure_line, sizeof(measure_line), 2, -1,
           " %s  power %.1f dBFS  peak %.1f  floor %.1f dBFS/bin  %.0f FFT/s",
           r->mode, power_db(t->sum_power / samples), 20 * log10(t->peak / FULL_SCALE), floor_db,
           (double)t->frames / secs);
    if (lost >= 0)                          /* (measured over LOSS_S: not at once) */
        append(measure_line, sizeof(measure_line), 2, -1, "  %.2f MS/s (%.0f%% lost)",
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
            shown.lines[0] = editing >= 0 ? hint : status;
            shown.lines[1] = settings_line;
            shown.lines[2] = measure_line;
            shown.failed = editing >= 0 ? hint_bad : failed;
            view_draw(view, &shown, fresh);
            if (editing >= 0)
                window_field_draw(field);
            window_flush(win);
            fresh = redraw = false;
        }
        if (picture_file && now_s() - saved >= PRINT_S) {
            if (view_save(view, picture_file) < 0)
                printf("plutosa: cannot write %s: %s\n", picture_file, strerror(errno));
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

static void set_level(double db)
{
    if (db > 30)
        db = 30;
    if (db < -150)
        db = -150;
    shown.ref_db = db;
}

/* --- A value typed in the edit box --- */

static const struct range *value_range(int k)
{
    return k == VALUE_LO ? &now.lo_range : k == VALUE_SPAN ? &now.rate_range
         : k == VALUE_BANDWIDTH ? &now.bandwidth_range : &now.gain_range;
}

/* What value k is, or is asked to be (in Hz or dB). */
static double value_now(int k)
{
    switch (k) {
    case VALUE_LO:
        return base_lo();
    case VALUE_SPAN:
        return base_rate();
    case VALUE_BANDWIDTH:
        return asked & ASK_BANDWIDTH ? want.bandwidth : now.bandwidth;
    default:
        return asked & ASK_GAIN ? want.gain : now.gain;
    }
}

/* The line shown instead of the status while value k is typed: its unit
 * and range; `bad` after a refusal (in red). */
static void describe(int k, bool bad)
{
    const struct range *r = value_range(k);
    double scale = VALUES[k].scale;
    int n = snprintf(hint, sizeof(hint), "%s%s in %s", bad ? "not a number in range: " : "",
                     VALUES[k].name, VALUES[k].unit);
    if (r->known && n > 0 && n < (int)sizeof(hint))
        n += snprintf(hint + n, sizeof(hint) - (size_t)n, ", %.7g to %.7g", r->min / scale,
                      r->max / scale);
    if (n > 0 && n < (int)sizeof(hint))
        snprintf(hint + n, sizeof(hint) - (size_t)n, "   (Enter sets it, Esc gives up)");
    hint_bad = bad;
    redraw = true;
}

/* Opens the edit box over value k, with its present value selected (typing
 * replaces it), or empty. */
static void edit_value(int k, bool empty)
{
    char text[32] = "";
    if (!empty) {
        snprintf(text, sizeof(text), "%.6f", value_now(k) / VALUES[k].scale);
        char *end = text + strlen(text);        /* 2400.000000 -> 2400, 455.500000 -> 455.5 */
        while (end > text && end[-1] == '0')
            *--end = 0;
        if (end > text && end[-1] == '.')
            end[-1] = 0;
    }
    box_h = window_field_height(field);
    view_value_box(view, &shown, k, box_h, EDIT_W, &box_x, &box_y, &box_w);
    window_field_place(field, box_x, box_y, box_w);
    window_field_set_text(field, text);
    window_field_focus(field, 1);
    editing = k;
    describe(k, false);
}

static void stop_editing(void)
{
    window_field_focus(field, 0);
    editing = -1;
    redraw = true;
}

/* The edit box's text as value k: asked of the receiver, or false (not a
 * number, or outside the Pluto's range: nothing asked). */
static bool ask_value(int k, const char *text)
{
    char *end;
    double typed = strtod(text, &end), v = typed * VALUES[k].scale;
    while (*end == ' ')
        end++;
    const struct range *r = value_range(k);
    double slack = VALUES[k].scale * 1e-6;      /* (the range as shown: 7 digits) */
    if (end == text || *end || !isfinite(typed) ||
        (r->known && (v < r->min - slack || v > r->max + slack)))
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
    default:
        ask_gain_db(v);
    }
    return true;
}

/* While the edit box is open, it has the events first: true when one was
 * its (every key is, while it is open). Escape or a click outside it closes
 * it; Enter sets the value, or says why not and leaves it open. */
static bool field_event(const struct window_event *ev, bool pressed)
{
    if (editing < 0)
        return false;
    bool key = ev->type == WINDOW_KEY || ev->type == WINDOW_KEY_DOWN || ev->type == WINDOW_KEY_UP;
    if (ev->type == WINDOW_KEY_DOWN && ev->key == WKEY_ESCAPE) {
        stop_editing();
        return true;
    }
    if (pressed && !(ev->x >= box_x && ev->x < box_x + box_w && ev->y >= box_y &&
                     ev->y < box_y + box_h)) {
        stop_editing();
        return false;                           /* (the click is the window's) */
    }
    int taken = window_field_event(field, ev);
    if (taken == 1) {
        if (!connected)
            stop_editing();
        else if (ask_value(editing, window_field_text(field)))
            stop_editing();
        else
            describe(editing, true);
    }
    return taken >= 0 || key;
}

/* Whether a key's byte is part of an escape sequence (the bytes of an
 * arrow or another key without a character, ESC [ ... or ESC O ..., which
 * come as WINDOW_KEY_DOWN too): the B of the down arrow's must not step
 * the bandwidth. */
static bool in_sequence(int byte)
{
    static int state;                           /* 1: after ESC, 2: in the sequence */
    if (byte == 27) {
        state = 1;
        return true;
    }
    if (state == 1) {
        state = byte == '[' || byte == 'O' ? 2 : 0;
        return state == 2;
    }
    if (state == 2) {
        if (byte >= 0x40 && byte <= 0x7E)       /* (the final byte) */
            state = 0;
        return true;
    }
    return false;
}

/* The physical keys (WINDOW_KEY_DOWN): the arrows. */
static void key_down(int key)
{
    if (key == WKEY_LEFT || key == WKEY_RIGHT)
        ask_lo(base_lo() + (key == WKEY_RIGHT ? 1 : -1) * base_rate() / 10);
    else if (key == WKEY_UP || key == WKEY_DOWN)
        set_level(shown.ref_db + (key == WKEY_UP ? LEVEL_STEP : -LEVEL_STEP));
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
    }
}

int main(int argc, char **argv)
{
    uri = DEFAULT_URI;
    for (int k = 1; k < argc; k++) {
        if (!strcmp(argv[k], "-p") && k + 1 < argc) {
            picture_file = argv[++k];
        } else if (argv[k][0] != '-') {
            uri = argv[k];
        } else {
            printf("usage: plutosa [URI] [-p FILE]\n");
            return 2;
        }
    }
    win = window_open(960, 640, "plutosa");
    if (!win) {
        printf("plutosa: cannot open a window (is windowd running?)\n");
        return 1;
    }
    view = view_new(win);
    field = window_field_new(win, 0, 0, EDIT_W);
    if (!view || !field || view_layout(view) < 0) {
        printf("plutosa: out of memory\n");
        return 1;
    }
    shown.keys[0] = KEYS[0];
    shown.keys[1] = KEYS[1];
    redraw = true;
    printf("plutosa: started, connecting to %s\n", uri);

    pthread_t acquirer, analyser, drawer;
    if (pthread_create(&acquirer, 0, acquire_thread, 0) != 0 ||
        pthread_create(&analyser, 0, spectrum_thread, 0) != 0 ||
        pthread_create(&drawer, 0, draw_thread, 0) != 0) {
        printf("plutosa: cannot start a thread\n");
        return 1;
    }
    pthread_detach(acquirer);
    pthread_detach(analyser);
    pthread_detach(drawer);

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
        bool sequence = ev.type == WINDOW_KEY && in_sequence(ev.key);
        if (field_event(&ev, pressed)) {
            /* (the edit box's) */
        } else if (ev.type == WINDOW_KEY_DOWN) {
            /* (the receiver's keys wait for a connection: its ranges) */
            if (connected || ev.key == WKEY_UP || ev.key == WKEY_DOWN)
                key_down(ev.key);
        } else if (ev.type == WINDOW_KEY && !sequence) {
            if (connected && ((ev.key >= '0' && ev.key <= '9') || ev.key == '.')) {
                edit_value(VALUE_LO, true);     /* a frequency typed: the LO's box */
                window_field_event(field, &ev);
            } else if (connected || !strchr("sSbBg+=-", ev.key)) {
                key_typed(ev.key);
            }
        } else if (ev.type == WINDOW_POINTER) {
            int k = pressed ? view_value_at(view, &shown, ev.x, ev.y) : -1;
            if (k >= 0 && connected)
                edit_value(k, false);
        } else if (ev.type == WINDOW_DOUBLE_CLICK) {
            enum view_place at = view_at(view, &shown, ev.x, ev.y, &hz);
            if (connected && shown.rate > 0 && (at == VIEW_PLOT || at == VIEW_WATERFALL))
                ask_lo(hz);
        } else if (ev.type == WINDOW_SCROLL) {
            /* Up (negative steps): higher in frequency, or the level. */
            enum view_place at = view_at(view, &shown, px, py, &hz);
            if (at == VIEW_LEVELS)
                set_level(shown.ref_db - ev.y * LEVEL_STEP);
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
