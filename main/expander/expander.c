#include "expander.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_io_expander.h"
#include <stdatomic.h>
#include <stdint.h>
#include "mcp32017_io.h"

// MCP23017 expander
#define EXPANDER_SDA        6    // D5 on board
// #define EXPANDER_SDA        7   // D8 on board (conflict with battery mointor)
#define EXPANDER_SCL        44   // D7 on board
#define EXPANDER_I2C_HZ     100000
#define EXPANDER_ADDR       0x20  // 0x20, A0-A2 tied low

// Port B pins are 8..15 in the component (A0-A7 = 0..7, B0-B7 = 8..15)
#define LED1_PIN   IO_EXPANDER_PIN_NUM_8   // B0
#define LED2_PIN   IO_EXPANDER_PIN_NUM_9   // B1
#define BTN1_PIN   IO_EXPANDER_PIN_NUM_10  // B2
#define BTN2_PIN   IO_EXPANDER_PIN_NUM_11  // B3

#define LED_MASK   (LED1_PIN | LED2_PIN)
#define BTN_MASK   (BTN1_PIN | BTN2_PIN)

static const char *TAG = "expander";

// Start as "all released" so callers see a sane value before the first read or if the bus fails
static _Atomic uint32_t s_btn_levels = BTN_MASK;
static _Atomic uint32_t s_press_count[EXPANDER_BTN_COUNT];

uint32_t expander_button_press_count(expander_btn_t btn)
{
    return atomic_load(&s_press_count[btn]);
}

int expander_button_level(expander_btn_t btn)
{
    uint32_t pin = (btn == EXPANDER_BTN_PAGE) ? BTN1_PIN : BTN2_PIN;
    return (atomic_load(&s_btn_levels) & pin) ? 1 : 0;
}

static esp_err_t configure_pins(esp_io_expander_handle_t io)
{
    esp_err_t err = mcp23017_io_reinit(io);          // restores pull-ups and defaults
    if (err != ESP_OK) return err;
    err = esp_io_expander_set_dir(io, LED_MASK, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) return err;
    err = esp_io_expander_set_dir(io, BTN_MASK, IO_EXPANDER_INPUT);
    if (err != ESP_OK) return err;
    return esp_io_expander_set_level(io, LED_MASK, 0);
}

static void expander_task(void *arg)
{
    esp_io_expander_handle_t io = (esp_io_expander_handle_t)arg;
    uint32_t fails = 0;
    static uint32_t last_levels = BTN_MASK;
    const uint32_t btn_pins[EXPANDER_BTN_COUNT] = { BTN1_PIN, BTN2_PIN };
    int stable[EXPANDER_BTN_COUNT] = { 1, 1 };       // 1 = released
    uint8_t pending[EXPANDER_BTN_COUNT] = { 0, 0 };

    while (1) {
        uint32_t levels = 0;
        esp_err_t r = esp_io_expander_get_level(io, BTN_MASK, &levels);
        if (r == ESP_OK) {
            atomic_store(&s_btn_levels, levels & BTN_MASK);
            // count debounces
            for (int i = 0; i < EXPANDER_BTN_COUNT; i++) {
                int lvl = (levels & btn_pins[i]) ? 1 : 0;
                if (lvl == stable[i]) {
                    pending[i] = 0;
                } else if (++pending[i] >= 2) {              // 2 matching polls = 40 ms debounce
                    stable[i] = lvl;
                    pending[i] = 0;
                    if (lvl == 0) {                          // count presses, not releases
                        atomic_fetch_add(&s_press_count[i], 1);
                    }
                }
            }
            fails = 0;
            if ((levels & BTN_MASK) != (last_levels & BTN_MASK)) {
                esp_io_expander_set_level(io, LED1_PIN, !(levels & BTN1_PIN));
                esp_io_expander_set_level(io, LED2_PIN, !(levels & BTN2_PIN));
                last_levels = levels;
            }
        } else {
            atomic_store(&s_btn_levels, BTN_MASK);   // fail safe: report released
            if (fails++ % 50 == 0) {
                ESP_LOGE(TAG, "read failed: %s (fails=%lu)",
                         esp_err_to_name(r), (unsigned long)fails);
            }
            if (configure_pins(io) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(200));      // back off while unresponsive
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t expander_start(void) {
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = EXPANDER_SDA,
        .scl_io_num = EXPANDER_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  // fine for a short test; add 4.7k external for reliability
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    esp_io_expander_handle_t io = NULL;
    err = mcp23017_io_expander_new(bus, EXPANDER_ADDR, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MCP23017 not found at 0x%02x", EXPANDER_ADDR);
        return err;
    }

    err = esp_io_expander_set_dir(io, LED_MASK, IO_EXPANDER_OUTPUT);
    if (err != ESP_OK) return err;
    err = esp_io_expander_set_dir(io, BTN_MASK, IO_EXPANDER_INPUT);
    if (err != ESP_OK) return err;
    esp_io_expander_set_level(io, LED_MASK, 0);

    BaseType_t ok = xTaskCreate(expander_task, "expander", 4096, io, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;


}