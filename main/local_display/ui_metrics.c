#include <math.h>
#include <stdio.h>
#include <sys/param.h>
#include "esp_timer.h"
#include "lvgl.h"

#include "ui_metrics.h"
#include "state.h"

#define UPDATE_MS 500
#define PEAK_WINDOW_S 10 // Must be smaller than the state ring
                        // (PUB_RING_LEN / sample rate = ~10 s at 100 Hz)
#define PEAK_WINDOW_MS  (PEAK_WINDOW_S * 1000)
#define READ_MAX        64

typedef struct {
    lv_obj_t *pmax, *pmin, *db, *temp, *heap, *uptime, *wifi;
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
    lv_obj_set_width(v, 56);     // 7 chars: fits e.g. "-12345"
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

// Max and min pressure (mPa) over the last PEAK_WINDOW_S seconds, scanned
// from the shared sample ring. Returns false if there are no samples in the window.
static bool window_extremes(int32_t *vmax, int32_t *vmin)
{
    pub_sample_t buf[READ_MAX];
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint64_t cursor = state_cursor_latest(PUB_RING_LEN);    // oldest sample available
    int32_t hi = INT32_MIN, lo = INT32_MAX;

    uint32_t n;
    do {
        n = state_read_since(&cursor, buf, READ_MAX, NULL);
        for (uint32_t i = 0; i < n; i++) {
            // signed age: a sample published while we scan can be "newer" than
            // `now` (negative age) and must still count
            int32_t age = (int32_t)(now - buf[i].t_ms);
            if (age > (int32_t)PEAK_WINDOW_MS) continue;
            hi = MAX(hi, buf[i].mpa);
            lo = MIN(lo, buf[i].mpa);
        }
    } while (n == READ_MAX);

    if (hi == INT32_MIN) return false;
    *vmax = hi;
    *vmin = lo;
    return true;
}

// Runs inside lv_timer_handler(), so the LVGL lock is already held
static void metrics_update_cb(lv_timer_t *timer)
{
    metrics_ctx_t *c = lv_timer_get_user_data(timer);

    state_t st;
    if (!state_get(&st, NULL, pdMS_TO_TICKS(5))) {
        return;     // nothing published yet (or lock busy): keep the old text
    }

    int32_t hi, lo;
    if (window_extremes(&hi, &lo)) {
        lv_label_set_text_fmt(c->pmax, "%d", (int)hi);
        lv_label_set_text_fmt(c->pmin, "%d", (int)lo);
    } else {
        lv_label_set_text(c->pmax, "--");
        lv_label_set_text(c->pmin, "--");
    }

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

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "Peaks, %d s", PEAK_WINDOW_S);
    add_header(screen, hdr, 2);
    s_m.pmax = add_row(screen, "Max mPa", 14);
    s_m.pmin = add_row(screen, "Min mPa", 26);

    add_header(screen, "Sensor, 1 s", 42);
    s_m.db   = add_row(screen, "Level dB", 54);
    s_m.temp = add_row(screen, "Temp C",   66);

    add_header(screen, "Device", 82);
    s_m.heap   = add_row(screen, "Heap kB",  94);
    s_m.uptime = add_row(screen, "Uptime s", 106);
    s_m.wifi   = add_row(screen, "WiFi",     118);

    lv_timer_t *timer = lv_timer_create(metrics_update_cb, UPDATE_MS, &s_m);
    lv_timer_ready(timer);   // run once right away
    return screen;
}