#include <stdio.h>
#include "lvgl.h"
#include "ui_access_point.h"
#include "wifi.h"

#define UPDATE_MS 500

#include "ui_access_point.h"

static lv_obj_t *s_name, *s_pass;
static bool s_filled;

static void add_header(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, text);
    lv_obj_set_style_text_color(h, lv_color_hex(0x9ece6a), 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 2, y);
}

// title on one line, value underneath; returns the value label
static lv_obj_t *add_field(lv_obj_t *parent, const char *title, const char *initial, int y)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 2, y);

    lv_obj_t *v = lv_label_create(parent);
    lv_obj_set_width(v, 124);                 // labels wrap by default, so long passwords wrap
    lv_label_set_text(v, initial);
    lv_obj_align(v, LV_ALIGN_TOP_LEFT, 2, y + 11);
    return v;
}

static void update_cb(lv_timer_t *timer)
{
    if (s_filled) {
        return;
    }
    char ssid[33], pass[65];
    if (!wifi_ap_info(ssid, sizeof ssid, pass, sizeof pass)) {
        return;
    }
    lv_label_set_text(s_name, ssid);
    lv_label_set_text(s_pass, pass);
    s_filled = true;
}

lv_obj_t *ui_ap_page_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(screen, &lv_font_unscii_8, 0);
    lv_obj_set_scrollable(screen, false);

    add_header(screen, "Access Point", 2);
    s_name = add_field(screen, "Network",       "--",          16);
    s_pass = add_field(screen, "Password",      "--",          44);
    add_field(screen,          "Then browse to", "192.168.4.1", 84);

    lv_timer_create(update_cb, UPDATE_MS, NULL);   // runs under the LVGL lock, like ui_metrics
    return screen;
}