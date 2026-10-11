/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

// This demo UI is adapted from LVGL official example: https://docs.lvgl.io/master/examples.html#loader-with-arc

#include "lvgl.h"
#include <math.h>
#include <stdlib.h>
#include <sys/param.h>
#include "esp_system.h"   // esp_get_free_heap_size()
#include "esp_timer.h"    // esp_timer_get_time()

// chart info
#define CHART_Y_MIN  0
#define CHART_Y_MAX  100
#define SCALE_W  34   // 3 chars of unscii_8 (24px) + 4px tick + a little padding
#define CHART_W  90   // 2 + 34 + 90 + 2 = 128
#define PLOT_H   88   // chart and scale must be the same height
#define MAX_WINDOW_SAMPLES  60 

// Summary page info 
#define STAT_WINDOW  20   // samples used for max/avg (20 x 500 ms = 10 s)



typedef struct {
    lv_obj_t *chart;
    lv_chart_series_t *series;
    lv_obj_t *label;
    uint32_t tick;
} chart_ctx_t;

static chart_ctx_t s_chart_ctx;

typedef struct {
    lv_obj_t *p_max, *p_avg, *t_avg, *heap, *uptime;
    int32_t  p_buf[STAT_WINDOW];   // pressure samples, in tenths
    int32_t  t_buf[STAT_WINDOW];   // temperature samples, in tenths
    uint32_t count;                // valid samples (<= STAT_WINDOW)
    uint32_t head;                 // next write slot
    uint32_t tick;
} metrics_ctx_t;

static metrics_ctx_t s_metrics;

// Runs inside lv_timer_handler(), so the LVGL lock is already held
static void chart_update_cb(lv_timer_t *timer)
{
    chart_ctx_t *ctx = lv_timer_get_user_data(timer);

    // Fake sensor: slow sine wave plus a little noise
    float phase = ctx->tick++ * 0.15f;
    int32_t value = 50 + (int32_t)(35.0f * sinf(phase)) + (rand() % 7 - 3);

    lv_chart_set_next_value(ctx->chart, ctx->series, value);

    // Max over the last MAX_WINDOW_SAMPLES samples
    const int32_t *y = lv_chart_get_y_array(ctx->chart, ctx->series);
    uint32_t count   = lv_chart_get_point_count(ctx->chart);
    uint32_t window  = MIN(MAX_WINDOW_SAMPLES, count);

    int32_t max_val = LV_CHART_POINT_NONE;
    for (uint32_t i = count - window; i < count; i++) {
        if (y[i] == LV_CHART_POINT_NONE) continue;   // slots not filled yet
        if (max_val == LV_CHART_POINT_NONE || y[i] > max_val) max_val = y[i];
    }

    if (max_val == LV_CHART_POINT_NONE) {
        lv_label_set_text(ctx->label, "Max: --");
    } else {
        lv_label_set_text_fmt(ctx->label, "Max: %d", (int)max_val);
    }
}
void example_lvgl_hello_ui(lv_display_t *disp)
{

    static lv_style_t style_dark;
    static bool inited = false;

    if (!inited) {
    lv_style_init(&style_dark);
    lv_style_set_bg_color(&style_dark, lv_color_hex(0x000000));
    lv_style_set_text_color(&style_dark, lv_color_hex(0xffffff));

    inited = true;
    }
    lv_obj_t * screen = lv_screen_active();
    lv_obj_t * container = lv_obj_create(screen);
    lv_obj_set_size(container, 128, 128);
    lv_obj_set_align(container, LV_ALIGN_CENTER);
    lv_obj_add_style(container, &style_dark, 0);

    lv_obj_t *label = lv_label_create(container);
    lv_label_set_text(label, "Hello World");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void example_static_chart(lv_display_t *disp) {
    static lv_style_t style_dark;
    static bool inited = false;

    if (!inited) {
        lv_style_init(&style_dark);
        lv_style_set_bg_color(&style_dark, lv_color_hex(0x000000));
        lv_style_set_text_color(&style_dark, lv_color_hex(0xffffff));
        inited = true;
    }
    lv_obj_t * screen = lv_screen_active();
    lv_obj_t * container = lv_obj_create(screen);
    lv_obj_set_size(container, 128, 128);
    lv_obj_set_align(container, LV_ALIGN_CENTER);
    lv_obj_add_style(container, &style_dark, 0);

    lv_obj_t * line_chart = lv_chart_create(container);
    lv_obj_set_size(line_chart, lv_pct(90), lv_pct(90));
    lv_chart_set_type(line_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(line_chart, 12);
    lv_chart_series_t * lv_chart_series_1 = lv_chart_add_series(line_chart, lv_color_hex(0xef4444),
                                                                (lv_chart_axis_t)LV_CHART_AXIS_PRIMARY_Y);
    static const int32_t line_chart_values_1[] = {8, 10, 40, 20, 80, 50, 20, 30, 60, 30, 40, 20};
    lv_chart_set_series_values(line_chart, lv_chart_series_1, line_chart_values_1, 12);
    lv_chart_set_axis_min_value(line_chart, LV_CHART_AXIS_PRIMARY_Y, 0);
    lv_chart_set_axis_max_value(line_chart, LV_CHART_AXIS_PRIMARY_Y, 150);

}

lv_obj_t *example_dynamic_chart(lv_display_t *disp) {
    (void)disp;

    // will be its own page essentially
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_scrollable(screen, false);

    // Chart: top of the screen, leaving room for a readout underneath
    lv_obj_t *chart = lv_chart_create(screen);
    lv_obj_set_size(chart, CHART_W, PLOT_H);
    lv_obj_align(chart, LV_ALIGN_TOP_RIGHT, -2, 4);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT); // new samples push the plot left
    lv_chart_set_point_count(chart, 60);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, CHART_Y_MIN, CHART_Y_MAX);
    lv_chart_set_div_line_count(chart, 5, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);

    // Chart appearance
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x000000), 0);
    // lv_obj_set_style_border_color(chart, lv_color_hex(0x404040), 0);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);        // hide the per-point dots
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);          // data line thickness
    lv_obj_set_style_line_color(chart, lv_color_hex(0x303030), LV_PART_MAIN); // grid lines
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 0, 0);

    // Y Axis Labels
    lv_obj_t *scale = lv_scale_create(screen);
    lv_scale_set_mode(scale, LV_SCALE_MODE_VERTICAL_LEFT);
    lv_obj_set_size(scale, SCALE_W, PLOT_H);
    lv_obj_align_to(scale, chart, LV_ALIGN_OUT_LEFT_MID, 0, 0);
    lv_scale_set_range(scale, CHART_Y_MIN, CHART_Y_MAX);
    lv_scale_set_total_tick_count(scale, 5);     // 0, 25, 50, 75, 100
    lv_scale_set_major_tick_every(scale, 1);     // label every tick
    lv_scale_set_label_show(scale, true);
    lv_obj_set_style_text_font(scale, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(scale, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x606060), LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x606060), LV_PART_MAIN);


    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(0x9ece6a),
                                                    LV_CHART_AXIS_PRIMARY_Y);

    // Numeric readout
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "Value: --");
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_chart_ctx = (chart_ctx_t){ .chart = chart, .series = series, .label = label, .tick = 0 };

    // New sample every 200 ms
    lv_timer_create(chart_update_cb, 200, &s_chart_ctx);

    return screen;
}


// Summary Page Helpers + Logic
static void add_header(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(0x9ece6a), 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 2, y);
}

// Creates "name ........ value" and returns the value label so it can be updated
static lv_obj_t *add_row(lv_obj_t *parent, const char *name, int y)
{
    lv_obj_t *n = lv_label_create(parent);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_color(n, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 2, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_label_set_text(v, "--");
    lv_obj_set_width(v, 44);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -2, y);
    return v;
}

// Shows a value stored in tenths as "12.3" (integer math, no float printf needed)
static void set_tenths(lv_obj_t *label, int32_t x10)
{
    const char *sign = x10 < 0 ? "-" : "";
    int32_t a = x10 < 0 ? -x10 : x10;
    lv_label_set_text_fmt(label, "%s%d.%d", sign, (int)(a / 10), (int)(a % 10));
}

// Runs inside lv_timer_handler(), so the LVGL lock is already held
static void metrics_update_cb(lv_timer_t *timer)
{
    metrics_ctx_t *c = lv_timer_get_user_data(timer);

    // Fake sensors: replace with real readings (in tenths of a unit)
    int32_t p = 25 + rand() % 10;                                    // about 2.5
    if (c->tick % 37 == 36) p = 860;                                 // occasional spike
    int32_t t = 261 + (int32_t)(5.0f * sinf(c->tick * 0.1f));        // about 26.1
    c->tick++;

    // Ring buffer of recent samples
    c->p_buf[c->head] = p;
    c->t_buf[c->head] = t;
    c->head = (c->head + 1) % STAT_WINDOW;
    if (c->count < STAT_WINDOW) c->count++;

    int32_t p_max = c->p_buf[0], p_sum = 0, t_sum = 0;
    for (uint32_t i = 0; i < c->count; i++) {
        p_max = LV_MAX(p_max, c->p_buf[i]);
        p_sum += c->p_buf[i];
        t_sum += c->t_buf[i];
    }

    lv_label_set_text_fmt(c->p_max, "%d", (int)(p_max / 10));
    set_tenths(c->p_avg, p_sum / (int32_t)c->count);
    set_tenths(c->t_avg, t_sum / (int32_t)c->count);

    // Real device metrics
    lv_label_set_text_fmt(c->heap, "%u", (unsigned)(esp_get_free_heap_size() / 1024));
    lv_label_set_text_fmt(c->uptime, "%u", (unsigned)(esp_timer_get_time() / 1000000));
}


lv_obj_t *example_metrics_page(lv_display_t *disp)
{
    (void)disp;

    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(screen, &lv_font_unscii_8, 0);   // inherited by all labels
    lv_obj_set_scrollable(screen, false);

    add_header(screen, "Sensor Metrics", 4);
    s_metrics.p_max = add_row(screen, "P max mPa",  18);
    s_metrics.p_avg = add_row(screen, "P avg mPa",  32);
    s_metrics.t_avg = add_row(screen, "Temp avg C", 46);

    add_header(screen, "Device Metrics", 66);
    s_metrics.heap   = add_row(screen, "Heap kB",  80);
    s_metrics.uptime = add_row(screen, "Uptime s", 94);

    lv_timer_t *timer = lv_timer_create(metrics_update_cb, 500, &s_metrics);
    lv_timer_ready(timer);   // run once right away instead of waiting 500 ms

    return screen;
}