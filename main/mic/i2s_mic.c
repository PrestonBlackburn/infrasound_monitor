// i2s_mic.c 

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "i2s_mic.h"
#include "i2s_ring_buffer.h"
#include "state.h"

static const char *TAG = "i2s_mic";

// i2s gpio
#define GPIO_BCLK   13
#define GPIO_DIN    12   /* DOUT on the breakout → DIN on the S3 */
#define GPIO_WS     11   /* LRCL on the breakout */


// read properties
#define ...


#define PROCESS_PERIOD_MS    100    // publish frequency
#define STATS_WINDOW_MS      1000   // window for raw_db / temperature
#define RAW_RING_LEN         512    // power of 2, about 100 samples/sec (5 sec window)
#define BATCH_MAX            64     // samples drained per pass (~10 expected)

// enable/disable 1 Hz logging
static volatile bool s_verbose  = 
#if CONFIG_I2S_VERBOSE
    true;
#else
    false;
#endif

static volatile uint32_t s_reads_ok, s_i2s_errs, s_crc_errs;

// init buffer
static i2s_ring_buffer_t s_ring;
static i2s_sample_t s_ring_storage[RAW_RING_LEN];


void i2s_set_verbose(bool on) {
    s_verbose = on;
    ESP_LOGI(TAG, "verbose logging %s", on ? "on" : "off");
}


static esp_err_t i2s_init(void) {
    i2s_chan_handle_t rx_chan;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_chan));
 
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_MONO,
            .slot_mask      = I2S_STD_SLOT_LEFT,  /* SEL tied to GND */
            .ws_width       = 32,
            .ws_pol         = false,
            .bit_shift      = true,                /* Philips format  */
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_BCLK,
            .ws   = GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = GPIO_DIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
}

static void i2s_read_task(void *arg)
{
    // read 
    i2s_chan_handle_t rx_chan = (i2s_chan_handle_t)arg;
 
    static int32_t  i2s_buf[I2S_READ_FRAMES];
    static float    f32_buf[I2S_READ_FRAMES];
    size_t bytes_read = 0;
 
    while (1) {
        esp_err_t err = i2s_channel_read(rx_chan, i2s_buf,
                                          sizeof(i2s_buf),
                                          &bytes_read,
                                          pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(err));
            continue;
        }
 
        int n_frames = (int)(bytes_read / sizeof(int32_t));
 
        /* ICS-43432: 24-bit audio in top 3 bytes of each 32-bit word.
         * Shift right by 8, then normalise to [-1, 1] using the
         * 24-bit full-scale value (8 388 608 = 2^23).               */
        for (int i = 0; i < n_frames; i++) {
            int32_t s24 = i2s_buf[i] >> 8;   /* sign-extended 24-bit */
            f32_buf[i]  = (float)s24 / 8388608.0f;
        }
 
        /* Write to ring buffer.  Use 0-tick timeout so we never block
         * the I2S reader; if the DSP task falls behind, samples are
         * dropped rather than stalling capture.                       */
        size_t bytes_to_send = n_frames * sizeof(float);
        if (xRingbufferSend(s_ring_buf, f32_buf, bytes_to_send, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Ring buffer full — dropped %d samples", n_frames);
        }
    }
}


void i2s_process_task(void *pvParameters) {

}

static void i2s_log_task(void *arg) {

}


// public entry point
esp_err_t i2s_start(void) {
    ESP_RETURN_ON_FALSE(i2s_ring_init(&s_ring, s_ring_storage, I2S_RING_LEN) == 0,
        ESP_ERR_NO_MEM, TAG, "i2s raw ring init");
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "i2s init failed");
    // Reader above the processor so a busy processor can't delay sampling.
    // Stack depth is in BYTES on ESP-IDF.
    ESP_RETURN_ON_FALSE(xTaskCreate(i2s_read_task, "I2S_READ", 4096, NULL, 6, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "read i2s task");
    ESP_RETURN_ON_FALSE(xTaskCreate(i2s_process_task, "I2S_PROC", 4096, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "process i2s task");
    ESP_RETURN_ON_FALSE(xTaskCreate(sensor_log_task, "I2S_LOG", 4096, NULL, 1, NULL) == pdPASS,
                    ESP_ERR_NO_MEM, TAG, "log i2s task");
    return ESP_OK;
}