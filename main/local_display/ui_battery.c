#include "lvgl.h"
#include "ui_battery.h"
#include "battery.h"

#define SEGS 4
static lv_obj_t *s_seg[SEGS];

static void batt_timer_cb(lv_timer_t *t)
{
    static int bars = -1;
    int pct = battery_get_percent();
    if (pct < 0) return;

    int target = (pct + 24) / 25;                 // 1-25 -> 1 ... 76-100 -> 4
    if (target < 1) target = 1;
    if (target > SEGS) target = SEGS;
    // hysteresis: need to be 3% past the boundary before adding a bar
    if (bars > 0 && target > bars && pct < bars * 25 + 3) target = bars;
    if (target == bars) return;
    bars = target;

    lv_color_t c = lv_color_hex(bars <= 1 ? 0xCE6A6A : 0x9ECE6A);
    for (int i = 0; i < SEGS; i++) {
        if (i < bars) {
            lv_obj_set_style_bg_color(s_seg[i], c, 0);
            lv_obj_remove_flag(s_seg[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_seg[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_battery_create(void) {
    lv_obj_t *body = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(body);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(body, 13, 7);
    lv_obj_align(body, LV_ALIGN_TOP_RIGHT, -3, 1);     // leaves room for the cap
    lv_obj_set_style_border_width(body, 1, 0);
    lv_obj_set_style_border_color(body, lv_color_white(), 0);
    lv_obj_set_style_border_opa(body, LV_OPA_COVER, 0);

    lv_obj_t *cap = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 2, 3);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cap, lv_color_white(), 0);
    lv_obj_align_to(cap, body, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    for (int i = 0; i < SEGS; i++) {
        s_seg[i] = lv_obj_create(body);
        lv_obj_remove_style_all(s_seg[i]);
        lv_obj_set_size(s_seg[i], 2, 3);
        lv_obj_set_pos(s_seg[i], i * 3, 1);
        lv_obj_set_style_bg_opa(s_seg[i], LV_OPA_COVER, 0);
    }
    lv_timer_create(batt_timer_cb, 1000, NULL);

}