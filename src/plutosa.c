/*
 * plutosa: a spectrum analyzer for the ADALM-Pluto on RitterOS.
 *
 * Three threads. The acquisition thread connects to the Pluto's iiod
 * (libiio's network backend, ip:192.168.2.1 by default), reads what the
 * receiver is set to (radio.c), then reads blocks of I/Q samples from
 * cf-ad9361-lpc one after another. The link cannot carry every sample at
 * the Pluto's rates, so a block is BLOCK contiguous samples and the samples
 * between blocks are lost: what a spectrum needs. Each block becomes FFT
 * frames (dsp.c), and the averaged spectrum, reduced to the plot's pixel
 * columns, is handed to the drawing thread, which draws at most PICTURES a
 * second (view.c): the text, the spectrum with its marker, the waterfall.
 * The main thread reads the window's events (the keys and the mouse, see
 * KEYS): changes to the receiver become a request that the acquisition
 * thread carries out between blocks. When the link fails, the window says
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
#define BLOCK       16384           /* samples per block (a 16384-point FFT at most) */
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
    "type MHz, Enter: tune  arrows/wheel: tune, level  double-click: center  s/S span  "
    "b/B bandwidth",
    "g gain mode  +/- gain  f FFT size  w window  a averaging  m max hold  r reset",
};

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
static int bandwidth_choice;        /* BANDWIDTHS: = span until chosen */
/* A center frequency being typed (MHz). */
static bool typing;
static char typed[24];
static char typing_line[96];

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

/* The text's two lines from an interval's measurements; printed now and
 * then, with the strongest peaks. 0 dBFS is a full-scale complex tone
 * (I^2 + Q^2 = 1); the peak is the largest single I or Q against full
 * scale. */
static void show_tally(const struct tally *t, const struct spectrum *sp, const struct receiver *r,
                       double at, bool print)
{
    double secs = at - t->start, samples = (double)t->samples, got = samples / secs;
    int n = sp->n;
    double floor_db = power_db(spectrum_floor(sp->average, n));
    pthread_mutex_lock(&lock);
    /* (each about 100 characters at most: the 960-pixel window's width) */
    snprintf(settings_line, sizeof(settings_line),
             "LO %.6f MHz  span %.3f MHz  BW %.3f MHz  RBW %.2f kHz  FFT %d %s  avg %d",
             r->lo / 1e6, r->rate / 1e6, r->bandwidth / 1e6, r->rate * sp->enbw / n / 1e3, n,
             taper_name(sp->taper), sp->averages);
    snprintf(measure_line, sizeof(measure_line),
             "gain %.0f dB %s  power %.1f dBFS  peak %.1f dBFS  floor %.1f dBFS/bin  "
             "%.0f FFT/s  %.2f MS/s",
             r->gain, r->mode, power_db(t->sum_power / samples), 20 * log10(t->peak / FULL_SCALE),
             floor_db, (double)t->frames / secs, got / 1e6);
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
    pthread_mutex_unlock(&lock);
    printf("plutosa: RX LO %.6f MHz, %.6f MSPS, RF bandwidth %.6f MHz, gain %.0f dB (%s)\n",
           r->lo / 1e6, r->rate / 1e6, r->bandwidth / 1e6, r->gain, r->mode);
}

/* A block's samples in full scales, for the FFT. */
static float block_i[BLOCK], block_q[BLOCK];

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
 * the first refusal stops there and is shown. 0, or a link error. */
static int carry_out(const struct radio *radio, struct stream *s, struct receiver *r,
                     const char *receiving)
{
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
    return 0;
}

/* Reads blocks until the link fails: the error. */
static int stream(struct iio_context *ctx, const struct radio *radio, struct receiver *r)
{
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
    unsigned seen = 0;
    struct spectrum *sp = chosen_spectrum(0, &seen);
    if (!sp) {
        iio_buffer_destroy(s.buf);
        return -ENOMEM;
    }
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

    struct tally t = { .start = now_s() };
    double printed = t.start, published = 0;
    int stale = 0;
    for (;;) {
        ssize_t n = iio_buffer_refill(s.buf);
        if (n < 0) {
            err = (int)n;
            break;
        }
        pthread_mutex_lock(&lock);
        bool asking = asked != 0;
        pthread_mutex_unlock(&lock);
        if (asking) {
            err = carry_out(radio, &s, r, receiving);
            if (err < 0)
                break;
            spectrum_reset(sp);
            stale = STALE;
            t = (struct tally){ .start = now_s() };
            continue;
        }
        if (stale > 0) {
            stale--;
            continue;
        }
        /* The samples are le:S12/16: already sign-extended, so a plain
         * 16-bit value. I and Q are interleaved, a step apart each. */
        ptrdiff_t step = iio_buffer_step(s.buf);
        const char *pi = iio_buffer_first(s.buf, s.ich), *pq = iio_buffer_first(s.buf, s.qch);
        const char *end = iio_buffer_end(s.buf);
        int got = 0;
        for (; pi < end && pq < end && got < BLOCK; pi += step, pq += step, got++) {
            int i = *(const int16_t *)pi, q = *(const int16_t *)pq;
            double fi = i / FULL_SCALE, fq = q / FULL_SCALE;
            block_i[got] = (float)fi;
            block_q[got] = (float)fq;
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
        sp = chosen_spectrum(sp, &seen);
        for (int f = 0; f + sp->n <= got; f += sp->n, t.frames++)
            spectrum_add(sp, block_i + f, block_q + f);
        double at = now_s();
        if (at - published >= 1.0 / PICTURES) {
            publish(sp, r);
            published = at;
        }
        if (at - t.start >= SHOW_S && t.samples > 0) {
            /* The gain moves under the AGC: read it again for the text. */
            double gain;
            if (radio_gain(radio, &gain) == 0) {
                r->gain = gain;
                pthread_mutex_lock(&lock);
                now.gain = gain;
                pthread_mutex_unlock(&lock);
            }
            bool print = at - printed >= PRINT_S;
            show_tally(&t, sp, r, at, print);
            if (print)
                printed = at;
            t = (struct tally){ .start = at };
        }
    }
    if (s.buf)
        iio_buffer_destroy(s.buf);
    spectrum_free(sp);
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
            shown.lines[0] = typing ? typing_line : status;
            shown.lines[1] = settings_line;
            shown.lines[2] = measure_line;
            shown.failed = failed && !typing;
            view_draw(view, &shown, fresh);
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
    want.rate = range_clamp(&now.rate_range, SPANS[k]);
    asked |= ASK_RATE;
    if (BANDWIDTHS[bandwidth_choice] == 0) {
        want.bandwidth = want.rate;
        asked |= ASK_BANDWIDTH;
    }
}

static void ask_bandwidth(int dir)
{
    bandwidth_choice = (bandwidth_choice + dir + COUNT(BANDWIDTHS)) % COUNT(BANDWIDTHS);
    double hz = BANDWIDTHS[bandwidth_choice];
    want.bandwidth = hz > 0 ? hz : base_rate();
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

static void ask_gain(int dir)
{
    double from = asked & ASK_GAIN ? want.gain : now.gain;
    want.gain = range_clamp(&now.gain_range, from + dir * GAIN_STEP);
    snprintf(want.mode, sizeof(want.mode), "manual");
    asked |= ASK_MODE | ASK_GAIN;
}

static void set_level(double db)
{
    if (db > 30)
        db = 30;
    if (db < -150)
        db = -150;
    shown.ref_db = db;
}

/* A key typed into the frequency: digits and a point; Enter tunes there,
 * Backspace takes the last one back, Escape gives up. */
static void type_key(int key)
{
    size_t len = strlen(typed);
    if (key == '\n') {
        if (len)
            ask_lo(atof(typed) * 1e6);
        typing = false;
        typed[0] = 0;
    } else if (key == '\b') {
        if (len)
            typed[len - 1] = 0;
        typing = len > 1;
    } else if (key == 27) {
        typing = false;
        typed[0] = 0;
    } else if (len + 1 < sizeof(typed) && (key != '.' || !strchr(typed, '.'))) {
        typed[len] = (char)key;
        typed[len + 1] = 0;
        typing = true;
    }
    snprintf(typing_line, sizeof(typing_line), "center frequency: %s_ MHz   (Enter tunes, Esc gives up)",
             typed);
}

/* The physical keys (WINDOW_KEY_DOWN): the digits, point, Enter,
 * Backspace and Escape of the frequency, from the main keys or the keypad
 * (by place, so a key's escape sequence never types a digit), and the
 * arrows. */
static void key_down(int key, bool shift)
{
    int digit = -1;
    if (key >= WKEY_1 && key <= WKEY_9)
        digit = '1' + key - WKEY_1;
    else if (key == WKEY_0)
        digit = '0';
    else if (key >= 0x59 && key <= 0x61)        /* keypad 1..9 */
        digit = '1' + key - 0x59;
    else if (key == 0x62)                       /* keypad 0 */
        digit = '0';
    if (digit >= 0 && !shift)
        type_key(digit);
    else if ((key == WKEY_PERIOD && !shift) || key == 0x63)  /* (0x63: keypad point) */
        type_key('.');
    else if (typing && (key == WKEY_ENTER || key == 0x58))    /* (0x58: keypad Enter) */
        type_key('\n');
    else if (typing && key == WKEY_BACKSPACE)
        type_key('\b');
    else if (typing && key == WKEY_ESCAPE)
        type_key(27);
    else if (key == WKEY_LEFT || key == WKEY_RIGHT)
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
    if (!view || view_layout(view) < 0) {
        printf("plutosa: out of memory\n");
        return 1;
    }
    shown.keys[0] = KEYS[0];
    shown.keys[1] = KEYS[1];
    redraw = true;
    printf("plutosa: started, connecting to %s\n", uri);

    pthread_t acquirer, drawer;
    if (pthread_create(&acquirer, 0, acquire_thread, 0) != 0 ||
        pthread_create(&drawer, 0, draw_thread, 0) != 0) {
        printf("plutosa: cannot start a thread\n");
        return 1;
    }
    pthread_detach(acquirer);
    pthread_detach(drawer);

    struct window_event ev;
    int px = -1, py = -1;               /* where the pointer is (the wheel's place) */
    bool held[256] = { false };
    while (window_wait(win, &ev) && ev.type != WINDOW_CLOSED) {
        pthread_mutex_lock(&lock);
        double hz = 0;
        if (ev.type == WINDOW_KEY_DOWN || ev.type == WINDOW_KEY_UP) {
            if (ev.key >= 0 && ev.key < 256)
                held[ev.key] = ev.type == WINDOW_KEY_DOWN;
            /* (the receiver's keys wait for a connection: its ranges) */
            if (ev.type == WINDOW_KEY_DOWN && (connected || ev.key == WKEY_UP ||
                                               ev.key == WKEY_DOWN))
                key_down(ev.key, held[WKEY_LSHIFT] || held[WKEY_RSHIFT]);
        } else if (ev.type == WINDOW_KEY) {
            if (connected || !strchr("sSbBg+=-", ev.key))
                key_typed(ev.key);
        } else if (ev.type == WINDOW_POINTER) {
            px = ev.x, py = ev.y;
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
        pthread_mutex_unlock(&lock);
    }
    /* Kept locked: the other threads must not draw into a closed window
     * before the program's end ends them too. */
    pthread_mutex_lock(&lock);
    window_close(win);
    printf("plutosa: closed\n");
    return 0;
}
