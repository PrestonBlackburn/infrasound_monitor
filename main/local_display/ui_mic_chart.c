// ui_waveform_chart.c
//
// Sound-level trace (dB SPL vs time) built from the microphone waveform ring.
//
// Data source: mic_waveform_read_since() -> one mic_waveform_sample_t every
// 50 ms, each holding 50 min/max bins (1 ms per bin, int16, full-scale = +/-32767).
// Each chart column is the peak magnitude over BINS_PER_POINT bins, converted to
// dB SPL. The value is the SPL of a sine with that peak amplitude, so a pure
// tone reads the same as the overall dB SPL number. Noise peaks read higher
// than its RMS level by the crest factor (typically ~10 dB).
//
// The y window slides in 10 dB steps and uses a 40, 80 or 120 dB span so the
// 5 tick labels are always round numbers.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/param.h>
#include "lvgl.h"

#include "ui_mic_chart.h"
#include "mic_state.h"

#define TICK_MS          25     // poll period; the chart only redraws when new data arrived
#define BINS_PER_POINT   10     // 10 bins = 10 ms per column. 25 -> 25 ms, 50 -> 50 ms (longer window)
#define CHART_POINTS     90     // columns on screen: 90 x 10 ms = 0.9 s at BINS_PER_POINT = 10
#define SCALE_W          34     // 4 chars of unscii_8 (32 px) + tick
#define CHART_W          90     // 2 + 34 + 90 + 2 = 128
#define PLOT_H           88     // chart and scale must be the same height
#define READ_MAX         4      // samples per read call (~1 expected per 50 ms)
#define TITLE_TEXT       "Sound Level dB"
#define TITLE_TOP        13     // title y offset from the top of the screen
#define CHART_TOP        32     // chart y offset; leaves a gap below the title

// dB SPL of a full-scale sine (peak = 1.0): 123.01 (MIC_SPL_OFFSET_DB) - 3.01.
// Keep in sync with i2s_mic.c.
#define MIC_FS_SINE_SPL_DB  120.0f

// Y window behaviour
#define DB_MAX              130     // top of the highest window
#define LO_MARGIN_DB        5       // keep the visible minimum this far above the bottom
#define HEADROOM_DB         3       // keep the visible maximum this far below the top
#define SHRINK_HOLD_TICKS   60      // ~3 s of updates before the window is allowed to shrink

#define WAVE_BINS          ((int)(sizeof(((mic_waveform_sample_t *)0)->bins) / sizeof(wave_bin_t)))
#define POINTS_PER_SAMPLE  (WAVE_BINS / BINS_PER_POINT)

_Static_assert(WAVE_BINS % BINS_PER_POINT == 0, "BINS_PER_POINT must divide the waveform bin count");

// Window spans in dB. 4 tick intervals -> 10 / 20 / 30 dB per tick.
static const int32_t k_spans[] = {40, 80, 120};
#define NUM_SPANS ((uint32_t)(sizeof(k_spans) / sizeof(k_spans[0])))

typedef struct {
    lv_obj_t          *chart;
    lv_obj_t          *scale;
    lv_obj_t          *label;
    lv_chart_series_t *series;
    uint64_t           cursor;    // our own position in the waveform ring
    int32_t            bottom;    // current y window, dB SPL
    int32_t            top;
    uint32_t           hold;      // consecutive updates that wanted a smaller window
} chart_ctx_t;

static chart_ctx_t s_ctx;

// ---------------------------------------------------------------------------

// int16 peak magnitude -> dB SPL (sine-equivalent). 1 count is ~30 dB, which
// is the floor of this trace.
static inline int32_t counts_to_db(int32_t a)
{
    if (a < 1) a = 1;
    return (int32_t)lroundf(20.0f * log10f((float)a / 32767.0f) + MIC_FS_SINE_SPL_DB);
}

static void set_window(chart_ctx_t *c, int32_t bottom, int32_t top)
{
    c->bottom = bottom;
    c->top    = top;
    c->hold   = 0;
    lv_chart_set_range(c->chart, LV_CHART_AXIS_PRIMARY_Y, bottom, top);
    lv_scale_set_range(c->scale, bottom, top);
}

// Fit the window to the visible data. Grows immediately when data doesn't fit,
// shrinks only after the smaller window has been wanted for a while, so the
// axis doesn't jump every time a transient scrolls off the 0.9 s window.
static void autoscale(chart_ctx_t *c)
{
    const int32_t *y = lv_chart_get_y_array(c->chart, c->series);

    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (uint32_t i = 0; i < CHART_POINTS; i++) {
        if (y[i] == LV_CHART_POINT_NONE) continue;
        lo = MIN(lo, y[i]);
        hi = MAX(hi, y[i]);
    }
    if (hi == INT32_MIN) return;       // nothing plotted yet

    int32_t bottom = MAX(lo - LO_MARGIN_DB, 0) / 10 * 10;
    int32_t span = 0, top = 0;
    for (uint32_t i = 0; i < NUM_SPANS; i++) {
        span = k_spans[i];
        top  = bottom + span;
        if (top >= hi + HEADROOM_DB) break;     // smallest span that fits
    }
    if (top > DB_MAX) {
        top    = DB_MAX;
        bottom = top - span;
    }

    if (bottom == c->bottom && top == c->top) {
        c->hold = 0;
        return;
    }

    const bool fits_current = (hi <= c->top) && (lo >= c->bottom);
    if (!fits_current || ++c->hold >= SHRINK_HOLD_TICKS) {
        set_window(c, bottom, top);
    }
}

// Runs inside lv_timer_handler(), so the LVGL lock is already held
static void chart_update_cb(lv_timer_t *timer)
{
    chart_ctx_t *c = lv_timer_get_user_data(timer);

    // Timer callbacks are serialized by LVGL, so a static buffer is safe and
    // keeps ~800 B off the LVGL task stack.
    static mic_waveform_sample_t buf[READ_MAX];

    bool got_data = false;
    uint32_t n;
    do {
        n = mic_waveform_read_since(&c->cursor, buf, READ_MAX, NULL);

        for (uint32_t s = 0; s < n; s++) {
            for (int p = 0; p < POINTS_PER_SAMPLE; p++) {
                const wave_bin_t *b = &buf[s].bins[p * BINS_PER_POINT];
                int32_t peak = 0;
                for (int j = 0; j < BINS_PER_POINT; j++) {
                    peak = MAX(peak, MAX(abs(b[j].max), abs(b[j].min)));
                }
                lv_chart_set_next_value(c->chart, c->series, counts_to_db(peak));
            }
        }
        got_data |= (n > 0);
    } while (n == READ_MAX);      // drain any backlog (e.g. first tick after boot)

    if (!got_data) return;

    autoscale(c);
}

lv_obj_t *ui_waveform_chart_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x9ece6a), 0);   // same green as the trace
    lv_label_set_text(title, TITLE_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TITLE_TOP);

    lv_obj_t *chart = lv_chart_create(screen);
    lv_obj_set_size(chart, CHART_W, PLOT_H);
    lv_obj_align(chart, LV_ALIGN_TOP_RIGHT, -2, CHART_TOP);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);   // new points push the plot left
    lv_chart_set_point_count(chart, CHART_POINTS);
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x000000), 0);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);          // hide per-point dots
    lv_obj_set_style_line_width(chart, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x303030), LV_PART_MAIN);   // grid
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 0, 0);

    lv_obj_t *scale = lv_scale_create(screen);
    lv_scale_set_mode(scale, LV_SCALE_MODE_VERTICAL_LEFT);
    lv_obj_set_size(scale, SCALE_W, PLOT_H);
    lv_obj_align_to(scale, chart, LV_ALIGN_OUT_LEFT_MID, 0, 0);
    lv_scale_set_total_tick_count(scale, 5);
    lv_scale_set_major_tick_every(scale, 1);
    lv_scale_set_label_show(scale, true);
    lv_obj_set_style_text_font(scale, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(scale, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x606060), LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x606060), LV_PART_MAIN);

    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(0x9ece6a), LV_CHART_AXIS_PRIMARY_Y);

    // cursor = 0: the first read is clamped to the oldest sample still in the
    // ring, so the chart pre-fills with recent history instead of starting blank.
    s_ctx = (chart_ctx_t){
        .chart = chart, 
        .scale = scale,
        .series = series,
        .cursor = 0,
    };
    set_window(&s_ctx, 20, 60);     // initial window; autoscale takes over on the first data

    lv_timer_create(chart_update_cb, TICK_MS, &s_ctx);
    return screen;
}