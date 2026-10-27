/*
 * plutosa: a spectrum analyzer for the ADALM-Pluto on RitterOS.
 *
 * Step 1: the window and the connection. A thread connects to the Pluto's
 * iiod (libiio's network backend, ip:192.168.2.1 by default) and reads what
 * the receiver is set to; the window shows it and stays responsive meanwhile
 * (a connection to an absent Pluto takes seconds to fail). Everything shown
 * is also printed on standard output, which `make run` brings back.
 */
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "iio.h"
#include "ritteros.h"
#include "window.h"

PROGRAM_HELP("plutosa [URI]", "spectrum analyzer for the ADALM-Pluto (URI: ip:192.168.2.1)");

#define DEFAULT_URI "ip:192.168.2.1"
#define LINES 24

static struct window *win;
static uint32_t bg, fg, dim, accent, bad;

/* What the window shows, written by the connecting thread. The lock also
 * covers drawing, which both threads do (and window_resize, which replaces
 * the pixels being drawn into). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static const char *uri;
static char status[128] = "connecting...";
static int failed;
static char lines[LINES][96];
static int nlines;

static void draw_locked(void)
{
    int w = window_width(win), h = window_height(win);
    char title[160];
    window_fill(win, 0, 0, w, h, bg);
    snprintf(title, sizeof(title), "Pluto at %s", uri);
    window_text(win, 16, 12, title, fg, WINDOW_TRANSPARENT);
    window_text(win, 16, 32, status, failed ? bad : accent, WINDOW_TRANSPARENT);
    window_fill(win, 16, 54, w > 32 ? w - 32 : 0, 1, dim);
    for (int i = 0; i < nlines; i++)
        window_text(win, 16, 64 + 18 * i, lines[i], fg, WINDOW_TRANSPARENT);
    window_flush(win);
}

static void draw(void)
{
    pthread_mutex_lock(&lock);
    draw_locked();
    pthread_mutex_unlock(&lock);
}

/* Adds a line to the window's list and prints it (lock held). */
__attribute__((format(printf, 1, 2)))
static void add_line(const char *fmt, ...)
{
    if (nlines == LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lines[nlines], sizeof(lines[0]), fmt, ap);
    va_end(ap);
    printf("plutosa: %s\n", lines[nlines++]);
}

/* Reads one channel attribute as a number: -1 when it cannot be read. */
static double read_double(struct iio_channel *ch, const char *attr)
{
    double v;
    if (!ch || iio_channel_attr_read_double(ch, attr, &v) < 0)
        return -1;
    return v;
}

static void *connect_thread(void *arg)
{
    (void)arg;
    struct iio_context *ctx = iio_create_context_from_uri(uri);
    pthread_mutex_lock(&lock);
    if (!ctx) {
        char why[96];
        iio_strerror(errno, why, sizeof(why));
        snprintf(status, sizeof(status), "cannot connect: %s", why);
        failed = 1;
        printf("plutosa: %s\n", status);
        draw_locked();
        pthread_mutex_unlock(&lock);
        return 0;
    }
    snprintf(status, sizeof(status), "connected: %s", iio_context_get_description(ctx));
    printf("plutosa: %s\n", status);

    unsigned n = iio_context_get_devices_count(ctx);
    for (unsigned i = 0; i < n && nlines < LINES / 2; i++) {
        struct iio_device *dev = iio_context_get_device(ctx, i);
        const char *name = iio_device_get_name(dev);
        add_line("device %s", name ? name : iio_device_get_id(dev));
    }

    /* The receiver's settings live on the AD936x's control device: the RX
     * LO is output channel altvoltage0, the rest input channel voltage0. */
    struct iio_device *phy = iio_context_find_device(ctx, "ad9361-phy");
    if (!phy) {
        add_line("no ad9361-phy: not a Pluto?");
    } else {
        struct iio_channel *lo = iio_device_find_channel(phy, "altvoltage0", true);
        struct iio_channel *rx = iio_device_find_channel(phy, "voltage0", false);
        char mode[32] = "?";
        if (rx)
            iio_channel_attr_read(rx, "gain_control_mode", mode, sizeof(mode));
        add_line("RX LO        %.6f MHz", read_double(lo, "frequency") / 1e6);
        add_line("sample rate  %.6f MSPS", read_double(rx, "sampling_frequency") / 1e6);
        add_line("RF bandwidth %.6f MHz", read_double(rx, "rf_bandwidth") / 1e6);
        add_line("gain         %.0f dB", read_double(rx, "hardwaregain"));
        add_line("gain mode    %s", mode);
    }
    draw_locked();
    pthread_mutex_unlock(&lock);
    /* Kept open: the next steps read samples through it. */
    return ctx;
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

    pthread_t connector;
    if (pthread_create(&connector, 0, connect_thread, 0) != 0) {
        printf("plutosa: cannot start a thread\n");
        return 1;
    }
    pthread_detach(connector);

    struct window_event ev;
    while (window_wait(win, &ev) && ev.type != WINDOW_CLOSED) {
        if (ev.type == WINDOW_RESIZE) {
            pthread_mutex_lock(&lock);
            if (window_resize(win, ev.x, ev.y) == 0)    /* the new pixels are blank */
                draw_locked();
            pthread_mutex_unlock(&lock);
        }
    }
    window_close(win);
    printf("plutosa: closed\n");
    return 0;
}
