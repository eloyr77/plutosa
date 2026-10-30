/*
 * The picture: three lines of text, the spectrum with its dB and
 * frequency axes and the peak marker, and the waterfall below, drawn into
 * a pixel buffer of our own and shown with one window_blit. Not
 * thread-safe: the caller holds its lock around every call.
 */
#ifndef PLUTOSA_VIEW_H
#define PLUTOSA_VIEW_H

#include <stdbool.h>
#include "window.h"

#define VIEW_MAX_COLUMNS 4096       /* the widest plot, in pixels */
#define VIEW_VALUES      4          /* the values in the text a click edits */

/* A value a click edits: bytes start..end of lines[line] (end 0: none),
 * drawn in its own color. */
struct view_value {
    int line, start, end;
};

/* What one picture shows. */
struct view_data {
    int columns;                    /* in the arrays (0: nothing yet) */
    const float *average, *hold;    /* power per column (1.0 = 0 dBFS) */
    bool show_hold;
    double lo, rate;                /* Hz: the plot spans lo - rate/2 .. lo + rate/2 */
    int marker;                     /* the strongest peak's column, -1 for none */
    double marker_hz, marker_db;
    const char *lines[3];           /* the text above the plot (0: none) */
    struct view_value values[VIEW_VALUES];
    bool failed;                    /* line 0 tells of a failure: in red */
    double ref_db, range_db;        /* the dB axis: ref_db at the top, range_db down */
    const char *keys[2];            /* the lines at the bottom */
};

struct view;
struct view *view_new(struct window *win);

/* Fits the view to the window's size (at the start and after
 * window_resize; the waterfall starts again): 0, or -1 out of memory. */
int view_layout(struct view *v);

/* The plot's width in pixels: how many columns a picture wants. */
int view_columns(const struct view *v);

/* Draws a picture into the window (window_blit); new_row first adds a
 * waterfall row from d->average. The caller draws anything else over it,
 * then shows it all with window_flush. */
void view_draw(struct view *v, const struct view_data *d, bool new_row);

/* The value at (x, y) in the window: its index in d->values, or -1. */
int view_value_at(const struct view *v, const struct view_data *d, int x, int y);

/* Where an edit box `height` pixels tall goes over value k: at (*x, *y),
 * *w wide (the value's width, at least min_w). */
void view_value_box(const struct view *v, const struct view_data *d, int k, int height, int min_w,
                    int *x, int *y, int *w);

/* Where (x, y) in the window is, and the frequency there (in the plot's
 * or the waterfall's columns; *hz untouched elsewhere). */
enum view_place { VIEW_ELSEWHERE, VIEW_PLOT, VIEW_LEVELS, VIEW_WATERFALL };
enum view_place view_at(const struct view *v, const struct view_data *d, int x, int y,
                        double *hz);

/* The last picture as a PPM file (P6, 8 bits a channel): 0, or -1 with
 * errno. */
int view_save(const struct view *v, const char *path);

#endif
