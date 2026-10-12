#include <math.h>
#include "lvgl.h"

#include "ui_metrics.h"
#include "state.h"

#define UPDATE_MS 500

typedef struct {
    lv_obj_t *mpa, *db, *temp, *heap, *uptime, *wifi;
} metrics_ctx_t;

static metrics_ctx_t s_m;

static void add_header(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(0x9ece6a), 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 2, y);
}

// "name ........ value"; returns the value label so it can be updated
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

// Value stored in tenths, shown as "12.3" (integer math, no float printf)
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

    state_t st;
    if (!state_get(&st, NULL, pdMS_TO_TICKS(5))) {
        return;     // nothing published yet (or lock busy): keep the old text
    }

    lv_label_set_text_fmt(c->mpa, "%d", (int)lroundf(st.raw_pressure_mpa));
    set_tenths(c->db,   (int32_t)lroundf(st.raw_db * 10.0f));
    set_tenths(c->temp, (int32_t)lroundf(st.temperature_c * 10.0f));
    lv_label_set_text_fmt(c->heap,   "%u", (unsigned)(st.heap_free / 1024));
    lv_label_set_text_fmt(c->uptime, "%u", (unsigned)st.uptime_s);
    lv_label_set_text(c->wifi, st.wifi_up ? "up" : "down");
}

lv_obj_t *ui_metrics_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(screen, &lv_font_unscii_8, 0);   // inherited by all labels
    lv_obj_set_scrollable(screen, false);

    add_header(screen, "Sensor", 4);
    s_m.mpa  = add_row(screen, "Press mPa", 18);
    s_m.db   = add_row(screen, "Level dB",  32);
    s_m.temp = add_row(screen, "Temp C",    46);

    add_header(screen, "Device", 66);
    s_m.heap   = add_row(screen, "Heap kB",  80);
    s_m.uptime = add_row(screen, "Uptime s", 94);
    s_m.wifi   = add_row(screen, "WiFi",    108);

    lv_timer_t *timer = lv_timer_create(metrics_update_cb, UPDATE_MS, &s_m);
    lv_timer_ready(timer);   // run once right away
    return screen;
}