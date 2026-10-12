#include <math.h>
#include <sys/param.h>
#include "lvgl.h"

#include "ui_chart.h"
#include "state.h"

#define TICK_MS        50      // chart update period (ms), 20FPS max for display
#define POINTS_PER_TICK 5      // 5 points x 10 ms = 50 ms window
#define CHART_POINTS   90     // x TICK_MS = 10 s on screen
#define SCALE_W        34      // 4 chars of unscii_8 (32 px) + tick
#define CHART_W        90      // 2 + 34 + 90 + 2 = 128
#define PLOT_H         88      // chart and scale must be the same height
#define READ_MAX       64      // samples read per tick (~10 expected)

// Symmetric Y ranges in +/- mPa. All give integer labels with 5 ticks.
// Larger signals clip at +/-500; the readout still shows the true peak.
static const int32_t k_ranges[] = {10, 20, 50, 100, 200, 500};
#define NUM_RANGES (sizeof(k_ranges) / sizeof(k_ranges[0]))

typedef struct {
    lv_obj_t          *chart;
    lv_obj_t          *scale;
    lv_obj_t          *label;
    lv_chart_series_t *series;     // per-tick envelope: upper
    uint64_t           cursor;    // our own position in the state ring
    int32_t            range;     // current +/- range, mPa
    int32_t            peak;      // abs peak over the visible window, mPa
} chart_ctx_t;

static chart_ctx_t s_ctx;

static void set_range(chart_ctx_t *c, int32_t r)
{
    c->range = r;
    lv_chart_set_range(c->chart, LV_CHART_AXIS_PRIMARY_Y, -r, r);
    lv_scale_set_range(c->scale, -r, r);
}

// Pick the smallest range that fits the visible window with ~10% headroom
static void autoscale(chart_ctx_t *c)
{
    const int32_t *y = lv_chart_get_y_array(c->chart, c->series);

    int32_t peak = 0;
    for (uint32_t i = 0; i < CHART_POINTS; i++) {
        if (y[i] != LV_CHART_POINT_NONE) peak = MAX(peak, abs(y[i]));
    }
    c->peak = peak;

    int32_t want = k_ranges[NUM_RANGES - 1];
    for (size_t i = 0; i < NUM_RANGES; i++) {
        if (k_ranges[i] * 10 >= peak * 11) {
            want = k_ranges[i];
            break;
        }
    }
    if (want != c->range) {
        set_range(c, want);
    }
}

// Runs inside lv_timer_handler(), so the LVGL lock is already held
static void chart_update_cb(lv_timer_t *timer)
{
    chart_ctx_t *c = lv_timer_get_user_data(timer);

    // New samples since our last tick, using our own cursor
    pub_sample_t buf[READ_MAX];
    uint32_t n = state_read_since(&c->cursor, buf, READ_MAX, NULL);
    if (n > 0) {
        // Distribute incoming samples across 5 time buckets (10 ms resolution)
        for (int b = 0; b < POINTS_PER_TICK; b++) {
            uint32_t start_idx = (b * n) / POINTS_PER_TICK;
            uint32_t end_idx   = ((b + 1) * n) / POINTS_PER_TICK;

            if (start_idx >= end_idx) {
                if (start_idx < n) {
                    lv_chart_set_next_value(c->chart, c->series, buf[start_idx].mpa);
                }
                continue;
            }

            // Pick peak amplitude inside this 10 ms bucket to preserve spikes
            int32_t pt = buf[start_idx].mpa;
            for (uint32_t i = start_idx + 1; i < end_idx; i++) {
                if (abs(buf[i].mpa) > abs(pt)) {
                    pt = buf[i].mpa;
                }
            }

            // Push each 10 ms point into the chart
            lv_chart_set_next_value(c->chart, c->series, pt);
        }

        autoscale(c);
    }

    state_t st;
    if (state_get(&st, NULL, pdMS_TO_TICKS(5))) {
        lv_label_set_text_fmt(c->label, "%dmPa %ddB",
                              (int)c->peak, (int)lroundf(st.raw_db));
    }
}

lv_obj_t *ui_chart_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_scrollable(screen, false);

    lv_obj_t *chart = lv_chart_create(screen);
    lv_obj_set_size(chart, CHART_W, PLOT_H);
    lv_obj_align(chart, LV_ALIGN_TOP_RIGHT, -2, 4);
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

    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, &lv_font_unscii_8, 0);
    lv_label_set_text(label, "pk -- mPa");
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_ctx = (chart_ctx_t){
        .chart = chart, .scale = scale, .label = label,
        .series = series,
        .cursor = state_cursor_latest(0),
    };
    set_range(&s_ctx, k_ranges[0]);

    lv_timer_create(chart_update_cb, TICK_MS, &s_ctx);
    return screen;
}