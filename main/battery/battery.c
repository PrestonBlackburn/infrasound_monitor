#include "battery.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "battery";

// ADC1/A8, battery positive and negative connected via 200k ohm resistors on each
#define BATT_GPIO   7    // D8 on the board
#define BATT_DIVIDER        2
#define BATT_SAMPLES        32
#define BATT_PERIOD_MS      5000

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static SemaphoreHandle_t s_lock;
static int s_mv;
static int s_pct = -1;

static esp_err_t read_battery_mv(int *out_mv) {
    int sum = 0;
    int n = 0;
    int raw;

    adc_oneshot_read(s_adc, s_chan, &raw);
    for (int i = 0; i < BATT_SAMPLES; i++) {
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK) {
            sum += raw;
            n++;
        }
    }

    ESP_RETURN_ON_FALSE(n > 0, ESP_FAIL, TAG, "no valid ADC samples");
    int raw_avg = sum / n;
    int pin_mv;
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali, sum / n, &pin_mv), TAG, "cali");
    *out_mv = pin_mv * BATT_DIVIDER;
    ESP_LOGD(TAG, "samples=%d/%d raw=%d pin=%dmV batt=%dmV",
        n, BATT_SAMPLES, raw_avg, pin_mv, *out_mv);
    return ESP_OK;

}


static int mv_to_percent(int mv) {
    float p = 110.2f * (mv / 1000.0f) - 344.5f; // based on panasonic discharge curve at 2A
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return (int)(p + 0.5f);
}

static void battery_task(void *arg) {
    float filt = 0;
    bool first = true;
    for (;;) {
        int mv;
        esp_err_t err = read_battery_mv(&mv);
        if (err == ESP_OK) {
            filt = first ? mv : 0.8f * filt + 0.2f * mv; // EMA smoothing?
            first = false;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_mv = (int)filt;
            s_pct = mv_to_percent(s_mv);
            xSemaphoreGive(s_lock);
            
            ESP_LOGI(TAG, "batt=%dmV filtered=%dmV pct=%d%%", mv, (int)filt, mv_to_percent((int)filt));
            if ((int)filt < 3200) {
                ESP_LOGW(TAG, "battery low: %dmV", (int)filt);
            }

        } else {
            ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(BATT_PERIOD_MS));
    }
}

esp_err_t battery_start(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    adc_unit_t unit;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BATT_GPIO, &unit, &s_chan), TAG, "gpio not ADC");
    ESP_RETURN_ON_FALSE(unit == ADC_UNIT_1, ESP_ERR_INVALID_ARG, TAG, "expected ADC1");

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc unit");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, s_chan, &chan_cfg), TAG, "adc chan");

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,

    };
    ESP_RETURN_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali), TAG, "adc_cali");

    ESP_RETURN_ON_FALSE(xTaskCreate(battery_task, "battery", 3072, NULL, 1, NULL) == pdPASS,
        ESP_ERR_NO_MEM, TAG, "task");

    return ESP_OK;
}

int battery_get_percent(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int p = s_pct;
    xSemaphoreGive(s_lock);
    return p;
}

int battery_get_mv(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = s_mv;
    xSemaphoreGive(s_lock);
    return v;
}
