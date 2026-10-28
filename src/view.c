/*
 * The picture (view.h). Everything is drawn into v->px, the window's size
 * in native pixels, text included (wfont_text draws into a buffer of our
 * own), then shown with one window_blit and window_flush: windowd copies
 * one block instead of hundreds of fills.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "view.h"
#include "wfont.h"

#define TEXT_X   16
#define TEXT_Y   8
#define LINE_H   18
#define LEFT     56         /* room for the dB labels */
#define RIGHT    16
#define AXIS_H   22         /* the frequency labels under the plot */
#define KEYS_H   42         /* two lines of keys */
#define MIN_ROWS 16

struct view {
    struct window *win;
    const struct font *font;
    int w, h;
    uint32_t *px;               /* the window's pixels, w x h */
    int x0, pw;                 /* the plot's and waterfall's columns */
    int py0, ph;                /* the plot's rows */
    int wy0, wh;                /* the waterfall's rows */
    uint32_t *wf;               /* the waterfall, pw x wh, newest row first */
    uint32_t bg, panel, grid, text, dim, trace, fill, hold, marker, bad;
    uint32_t heat[256];         /* the waterfall's colors, weakest first */
};

/* The waterfall's colors from weakest to strongest: night blue through
 * teal and yellow to red and white. */
static const struct { double at; int r, g, b; } HEAT[] = {
    { 0.00, 0x0B, 0x0F, 0x1A }, { 0.30, 0x1D, 0x2F, 0x73 }, { 0.55, 0x19, 0x8B, 0x9C },
    { 0.75, 0xE3, 0xCF, 0x4F }, { 0.90, 0xE3, 0x59, 0x2F }, { 1.00, 0xFF, 0xFF, 0xFF },
};

struct view *view_new(struct window *win)
{
    struct view *v = calloc(1, sizeof(*v));
    if (!v)
        return 0;
    v->win = win;
    v->font = wfont_regular();
    v->bg = window_rgb(win, 0x12, 0x16, 0x1E);
    v->panel = window_rgb(win, 0x17, 0x1C, 0x26);
    v->grid = window_rgb(win, 0x2A, 0x33, 0x42);
    v->text = window_rgb(win, 0xE6, 0xE9, 0xEF);
    v->dim = window_rgb(win, 0x8A, 0x94, 0xA6);
    v->trace = window_rgb(win, 0x8F, 0xBC, 0xBB);
    v->fill = window_rgb(win, 0x22, 0x3D, 0x3D);
    v->hold = window_rgb(win, 0xC9, 0xA3, 0x5A);
    v->marker = window_rgb(win, 0xFF, 0xFF, 0xFF);
    v->bad = window_rgb(win, 0xBF, 0x61, 0x6A);
    int last = (int)(sizeof(HEAT) / sizeof(HEAT[0])) - 1;
    for (int k = 0, s = 0; k < 256; k++) {
        double t = k / 255.0;
        while (s < last - 1 && t > HEAT[s + 1].at)
            s++;
        double f = (t - HEAT[s].at) / (HEAT[s + 1].at - HEAT[s].at);
        if (f < 0)
            f = 0;
        if (f > 1)
            f = 1;
        v->heat[k] = window_rgb(win, (int)(HEAT[s].r + f * (HEAT[s + 1].r - HEAT[s].r)),
                                (int)(HEAT[s].g + f * (HEAT[s + 1].g - HEAT[s].g)),
                                (int)(HEAT[s].b + f * (HEAT[s + 1].b - HEAT[s].b)));
    }
    return v;
}

int view_layout(struct view *v)
{
    int w = window_width(v->win), h = window_height(v->win);
    uint32_t *px = realloc(v->px, (size_t)w * (size_t)h * sizeof(uint32_t));
    if (!px)
        return -1;
    v->px = px;
    v->w = w, v->h = h;
    v->x0 = LEFT;
    v->pw = w - LEFT - RIGHT;
    if (v->pw > VIEW_MAX_COLUMNS)
        v->pw = VIEW_MAX_COLUMNS;
    if (v->pw < MIN_ROWS)
        v->pw = MIN_ROWS;
    /* Under the text: the plot (3/5 of the rest), its frequency labels,
     * the waterfall (2/5), the keys. */
    v->py0 = TEXT_Y + 3 * LINE_H + 8;
    int rest = h - v->py0 - AXIS_H - KEYS_H - 4;
    v->ph = rest * 3 / 5;
    if (v->ph < MIN_ROWS)
        v->ph = MIN_ROWS;
    v->wh = rest - v->ph;
    if (v->wh < MIN_ROWS)
        v->wh = MIN_ROWS;
    v->wy0 = v->py0 + v->ph + AXIS_H;
    uint32_t *wf = realloc(v->wf, (size_t)v->pw * (size_t)v->wh * sizeof(uint32_t));
    if (!wf)
        return -1;
    v->wf = wf;
    for (int k = 0; k < v->pw * v->wh; k++)
        v->wf[k] = v->heat[0];
    return 0;
}

int view_columns(const struct view *v)
{
    return v->pw;
}

/* --- Drawing into px, clipped to the window --- */

static void rect(struct view *v, int x, int y, int w, int h, uint32_t c)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > v->w)
        x1 = v->w;
    if (y1 > v->h)
        y1 = v->h;
    for (int yy = y; yy < y1; yy++) {
        uint32_t *row = v->px + (size_t)yy * (size_t)v->w;
        for (int xx = x; xx < x1; xx++)
            row[xx] = c;
    }
}

/* A column of pixels from y0 to y1 (either order), both included. */
static void vline(struct view *v, int x, int y0, int y1, uint32_t c)
{
    if (y0 > y1) {
        int t = y0;
        y0 = y1, y1 = t;
    }
    rect(v, x, y0, 1, y1 - y0 + 1, c);
}

static int text(struct view *v, int x, int y, const char *s, uint32_t c)
{
    return wfont_text(v->px, (uint32_t)v->w, (uint32_t)v->w, (uint32_t)v->h, v->font, x, y, s, c,
                      WFONT_TRANSPARENT);
}

static int text_width(struct view *v, const char *s)
{
    return wfont_text_width(v->font, s);
}

/* The row of a level in the plot (clamped to it). */
static int row_of(const struct view *v, const struct view_data *d, double db)
{
    double t = (d->ref_db - db) / d->range_db;
    if (t < 0)
        t = 0;
    if (t > 1)
        t = 1;
    return v->py0 + (int)lround(t * (v->ph - 1));
}

static void draw_axes(struct view *v, const struct view_data *d)
{
    int fh = wfont_height(v->font);
    char label[32];
    /* dB: a line every step, about 30 pixels apart at least. */
    double step = tick_step(d->range_db * 30 / v->ph);
    for (double db = d->ref_db; db >= d->ref_db - d->range_db - 1e-9; db -= step) {
        int y = row_of(v, d, db);
        rect(v, v->x0, y, v->pw, 1, v->grid);
        snprintf(label, sizeof(label), "%g", db);
        text(v, v->x0 - 6 - text_width(v, label), y - fh / 2, label, v->dim);
    }
    int ay = v->py0 + v->ph + 4;
    text(v, 8, ay, "dBFS", v->dim);
    /* Frequency: round steps about 100 pixels apart, in MHz; "MHz" at the
     * right end, where no label may reach. */
    if (d->rate <= 0)
        return;
    double left = d->lo - d->rate / 2, fstep = tick_step(d->rate * 100 / v->pw);
    int decimals = fstep >= 1e6 ? 0 : (int)ceil(-log10(fstep / 1e6) - 1e-9);
    if (decimals > 6)
        decimals = 6;
    int unit_x = v->x0 + v->pw - text_width(v, "MHz");
    for (double f = ceil(left / fstep) * fstep; f <= left + d->rate; f += fstep) {
        int x = v->x0 + (int)floor((f - left) / d->rate * v->pw);   /* (as view_at) */
        if (x >= v->x0 + v->pw)
            break;
        rect(v, x, v->py0, 1, v->ph, v->grid);
        snprintf(label, sizeof(label), "%.*f", decimals, f / 1e6);
        int lw = text_width(v, label), lx = x - lw / 2;
        if (lx < v->x0)
            lx = v->x0;
        if (lx + lw < unit_x - 8)
            text(v, lx, ay, label, v->dim);
    }
    text(v, unit_x, ay, "MHz", v->dim);
}

/* A trace: per pixel column the level, joined to the last column's; with
 * a fill below it when `fill` is not 0. */
static void draw_trace(struct view *v, const struct view_data *d, const float *power, uint32_t c,
                       uint32_t fill)
{
    int bottom = v->py0 + v->ph - 1, last = -1;
    for (int x = 0; x < v->pw; x++) {
        int col = (int)((long)x * d->columns / v->pw);
        int y = row_of(v, d, power_db(power[col]));
        if (fill)
            vline(v, v->x0 + x, y + 1, bottom, fill);
        vline(v, v->x0 + x, last < 0 ? y : last, y, c);
        last = y;
    }
}

static void draw_marker(struct view *v, const struct view_data *d)
{
    int mx = v->x0 + (int)((long)d->marker * v->pw / d->columns);
    int my = row_of(v, d, d->marker_db);
    /* A small triangle pointing down at the peak. */
    for (int r = 0; r < 5; r++)
        rect(v, mx - r, my - 3 - r, 2 * r + 1, 1, v->marker);
    char label[96];
    snprintf(label, sizeof(label), "marker %.6f MHz  %.1f dBFS", d->marker_hz / 1e6, d->marker_db);
    text(v, v->x0 + v->pw - 8 - text_width(v, label), v->py0 + 6, label, v->marker);
}

static void draw_waterfall(struct view *v, const struct view_data *d, bool new_row)
{
    if (new_row && d->columns > 0) {
        memmove(v->wf + v->pw, v->wf, (size_t)v->pw * (size_t)(v->wh - 1) * sizeof(uint32_t));
        double bottom = d->ref_db - d->range_db;
        for (int x = 0; x < v->pw; x++) {
            int col = (int)((long)x * d->columns / v->pw);
            double t = (power_db(d->average[col]) - bottom) / d->range_db;
            int k = t <= 0 ? 0 : t >= 1 ? 255 : (int)(t * 255);
            v->wf[x] = v->heat[k];
        }
    }
    for (int y = 0; y < v->wh && v->wy0 + y < v->h; y++)
        memcpy(v->px + (size_t)(v->wy0 + y) * (size_t)v->w + v->x0, v->wf + (size_t)y * v->pw,
               (size_t)v->pw * sizeof(uint32_t));
}

void view_draw(struct view *v, const struct view_data *d, bool new_row)
{
    if (!v->px)
        return;
    rect(v, 0, 0, v->w, v->h, v->bg);
    for (int i = 0; i < 3; i++)
        if (d->lines[i])
            text(v, TEXT_X, TEXT_Y + i * LINE_H, d->lines[i],
                 i == 0 ? (d->failed ? v->bad : v->trace) : v->text);
    rect(v, v->x0, v->py0, v->pw, v->ph, v->panel);
    draw_axes(v, d);
    if (d->columns > 0) {
        if (d->show_hold)
            draw_trace(v, d, d->hold, v->hold, 0);
        draw_trace(v, d, d->average, v->trace, v->fill);
        if (d->marker >= 0)
            draw_marker(v, d);
    }
    draw_waterfall(v, d, new_row);
    for (int i = 0; i < 2; i++)
        if (d->keys[i])
            text(v, TEXT_X, v->h - KEYS_H + 4 + i * LINE_H, d->keys[i], v->dim);
    window_blit(v->win, 0, 0, v->w, v->h, v->px);
    window_flush(v->win);
}

enum view_place view_at(const struct view *v, const struct view_data *d, int x, int y,
                        double *hz)
{
    bool columns = x >= v->x0 && x < v->x0 + v->pw;
    /* Column x covers the bins from x / pw to (x + 1) / pw of the span:
     * its middle. */
    if (columns && d->rate > 0)
        *hz = d->lo - d->rate / 2 + (x - v->x0 + 0.5) / v->pw * d->rate;
    if (columns && y >= v->py0 && y < v->py0 + v->ph)
        return VIEW_PLOT;
    if (columns && y >= v->wy0 && y < v->wy0 + v->wh)
        return VIEW_WATERFALL;
    if (x < v->x0 && y >= v->py0 && y < v->py0 + v->ph)
        return VIEW_LEVELS;
    return VIEW_ELSEWHERE;
}

int view_save(const struct view *v, const char *path)
{
    if (!v->px)
        return errno = EINVAL, -1;
    int rs, gs, bs;
    window_shifts(v->win, &rs, &gs, &bs);
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    unsigned char *row = malloc((size_t)v->w * 3);
    int ok = row && fprintf(f, "P6\n%d %d\n255\n", v->w, v->h) > 0;
    for (int y = 0; ok && y < v->h; y++) {
        const uint32_t *p = v->px + (size_t)y * (size_t)v->w;
        for (int x = 0; x < v->w; x++) {
            row[3 * x] = (unsigned char)(p[x] >> rs);
            row[3 * x + 1] = (unsigned char)(p[x] >> gs);
            row[3 * x + 2] = (unsigned char)(p[x] >> bs);
        }
        ok = fwrite(row, 3, (size_t)v->w, f) == (size_t)v->w;
    }
    free(row);
    if (fclose(f) != 0)
        ok = 0;
    return ok ? 0 : -1;
}
