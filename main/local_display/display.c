#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_lcd_st7735.h"

#include "display.h"
#include "ui_chart.h"
#include "ui_metrics.h"
#include "ui_access_point.h"
#include "ui_battery.h"
#include "wifi.h"

static const char *TAG = "display";


#define LCD_HOST                   SPI2_HOST
#define DISPLAY_PIXEL_CLOCK_HZ     (20 * 1000 * 1000)
#define DISPLAY_PIN_SCLK           1
#define DISPLAY_PIN_MOSI           2    // SDA / PICO
#define DISPLAY_PIN_DC             4
#define DISPLAY_PIN_RST            3
#define DISPLAY_PIN_CS             5    // active low
// no backlight pin is wired

#define PAGE_BUTTON_GPIO           6    // pull-up button
#define BUTTON_POLL_MS             20   // two matching samples = 40 ms debounce

#define DISPLAY_H_RES              128
#define DISPLAY_V_RES              128
#define DISPLAY_CMD_BITS           8
#define DISPLAY_PARAM_BITS         8

#define LVGL_DRAW_BUF_LINES        50
#define LVGL_TICK_PERIOD_MS        2
#define LVGL_TASK_MAX_DELAY_MS     500
#define LVGL_TASK_MIN_DELAY_MS     (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_TASK_STACK_SIZE       (16 * 1024)
#define LVGL_TASK_PRIORITY         2


// Handle showing pages
#define PAGE_CHART                 0
#define PAGE_METRICS               1
#define PAGE_AP                    2
#define NUM_PAGES                  3

// decide if AP page should be shown
static bool page_available(int idx) {
    return idx != PAGE_AP || wifi_ap_info(NULL, 0, NULL, 0);
}

// LVGL is not thread-safe. The UI timers run inside lv_timer_handler() with
// this lock already held, so the ui_* modules never need to touch it.
static _lock_t s_lvgl_lock;

static lv_obj_t *s_pages[NUM_PAGES];
static int s_page_idx;

static bool flush_ready_cb(esp_lcd_panel_io_handle_t panel_io,
                           esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(disp);
    int x1 = area->x1, x2 = area->x2, y1 = area->y1, y2 = area->y2;
    // SPI LCD is big-endian: swap RGB565 byte order
    lv_draw_sw_rgb565_swap(px_map, (x2 + 1 - x1) * (y2 + 1 - y1));
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
}

static void tick_cb(void *arg) {
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}


static void lvgl_port_task(void *arg) {
    ESP_LOGI(TAG, "LVGL task started");
    for (;;) {
        _lock_acquire(&s_lvgl_lock);
        uint32_t wait_ms = lv_timer_handler();
        _lock_release(&s_lvgl_lock);
        wait_ms = MAX(wait_ms, LVGL_TASK_MIN_DELAY_MS);   // avoid starving the idle task / WDT
        wait_ms = MIN(wait_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * wait_ms);
    }
}


// Runs inside lv_timer_handler(), so the LVGL lock is already held
static void button_poll_cb(lv_timer_t *timer)
{
    static int stable = 1;          // 1 = released (pull-up), 0 = pressed
    static int changed_count = 0;
    static bool ap_auto_shown;
    if (!ap_auto_shown && wifi_ap_info(NULL, 0, NULL, 0)) {
        ap_auto_shown = true;
        s_page_idx = PAGE_AP;
        lv_screen_load(s_pages[PAGE_AP]);
    }

    int level = gpio_get_level(PAGE_BUTTON_GPIO);
    if (level == stable) {
        changed_count = 0;
        return;
    }
    if (++changed_count < 2) {
        return;                     // wait for a second matching sample (debounce)
    }
    stable = level;
    changed_count = 0;

    if (stable == 0) {              // act on press, not release
        do {
            s_page_idx = (s_page_idx + 1) % NUM_PAGES;
        } while (!page_available(s_page_idx));
        lv_screen_load(s_pages[s_page_idx]);
    }
}



esp_err_t display_start(void)
{
    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = DISPLAY_PIN_DC,
        .cs_gpio_num = DISPLAY_PIN_CS,
        .pclk_hz = DISPLAY_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = DISPLAY_CMD_BITS,
        .lcd_param_bits = DISPLAY_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISPLAY_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7735(io, &panel_cfg, &panel), TAG, "ST7735 driver");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel, false, false), TAG, "panel mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, 2, 1), TAG, "panel gap");   // panel-dependent offset
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "panel on");

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();
    lv_display_t *disp = lv_display_create(DISPLAY_H_RES, DISPLAY_V_RES);

    size_t buf_sz = DISPLAY_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, buf_sz, 0);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "LVGL draw buffers");

    lv_display_set_buffers(disp, buf1, buf2, buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, flush_cb);

    const esp_timer_create_args_t tick_args = { .callback = tick_cb, .name = "lvgl_tick" };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "tick start");

    const esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = flush_ready_cb };
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(io, &cbs, disp), TAG, "io callbacks");

    ESP_RETURN_ON_FALSE(xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL,
                                    LVGL_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "LVGL task");

    gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << PAGE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn_cfg), TAG, "button gpio");

    _lock_acquire(&s_lvgl_lock);
    s_pages[PAGE_CHART] = ui_chart_create();
    s_pages[PAGE_METRICS] = ui_metrics_create();
    s_pages[PAGE_AP] = ui_ap_page_create();
    ui_battery_create();
    lv_screen_load(s_pages[0]);
    lv_timer_create(button_poll_cb, BUTTON_POLL_MS, NULL);
    _lock_release(&s_lvgl_lock);

    ESP_LOGI(TAG, "Display ready");
    return ESP_OK;
}