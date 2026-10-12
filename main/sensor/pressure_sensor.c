#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "pressure_sensor.h"
#include "raw_ring_buffer.h"
#include "state.h"

static const char *TAG = "pressure_sensor";

// Sensor hardware
#define I2C_MASTER_SCL_IO           9/*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           8 /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              I2C_NUM_0 /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ          100000 /*!< I2C master clock frequency 400kHz-1000kHz */
#define I2C_READ_TIMEOUT_MS         20  
#define SENSOR_ADDR                 0x25 /*!< Address of the Sensiron sensor */

// Sensor properties
#define PRESSURE_SCALE_FACTOR 240 /* Pascals */
#define TEMP_SCALE_FACTOR 200 /* C */
// dB SPL = 20 log10 (p/p0),
#define REF_PRESSURE_PA  20e-6 // p0, dB SPL reference (20 uPa)

// timing/sizing
#define SAMPLE_PERIOD_MS 10    // 100 Hz
#define SAMPLE_PERIOD_TICKS pdMS_TO_TICKS(SAMPLE_PERIOD_MS)
_Static_assert(SAMPLE_PERIOD_TICKS >= 1, "SAMPLE_PERIOD_MS is shorter than one FreeRTOS tick; raise CONFIG_FREERTOS_HZ");

#define PROCESS_PERIOD_MS    100    // publish frequency
#define STATS_WINDOW_MS      1000   // window for raw_db / temperature
#define RAW_RING_LEN         512    // power of 2, about 100 samples/sec (5 sec window)
#define BATCH_MAX            64     // samples drained per pass (~10 expected)


static i2c_master_dev_handle_t s_dev;
static measurement_ring_buffer_t s_ring;
static raw_sample_t s_ring_storage[RAW_RING_LEN];


static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static inline int32_t counts_to_mpa(int16_t counts)
{
    return (int32_t)lroundf((float)counts * (1000.0f / PRESSURE_SCALE_FACTOR));
}

// Sensirion Checksum Validation
static uint8_t sensirion_crc8(const uint8_t *d, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}


// Hardware init
static esp_err_t sensor_hw_init(void) {

    ESP_LOGI(TAG, "Initializing I2C Master Bus...");

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus init");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SENSOR_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "I2C add device");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, SENSOR_ADDR, 100), TAG, "probe 0x%02X", SENSOR_ADDR);
    
    ESP_LOGI(TAG, "I2C Initialized successfully");

    // Stop any measurement already running
    const uint8_t stop_cmd[] = {0x3F, 0xF9};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, stop_cmd, sizeof(stop_cmd), 100), TAG, "stop cmd");
    vTaskDelay(pdMS_TO_TICKS(20));

    // Continuous mode, differential pressure, no averaging (0.5 ms update)
    const uint8_t start_cmd[] = {0x36, 0x1E};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_dev, start_cmd, sizeof(start_cmd), 1000), TAG, "start cmd");
    vTaskDelay(pdMS_TO_TICKS(15));   // first result is ready after ~8 ms

    ESP_LOGI(TAG, "SDP801 continuous measurement started");
    return ESP_OK;
}


// read sensor task, i2c -> measurement ring buffer
void sensor_read_task(void *arg) {
    (void)arg;
    // read i2c data, apply scaling factor, write to ring buffer
    // 3 bytes Diff pressure (MSB, LSB, CRC), 3 bytes Temp (MSB, LSB, CRC)
    // limit logging here to debug and errors
    uint8_t buf[9];  // dp(2)+crc, temp(2)+crc, scale(2)+crc
    uint32_t i2c_errs = 0, crc_errs = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, SAMPLE_PERIOD_TICKS);   // fixed period, no drift
        esp_err_t ret = i2c_master_receive(
            s_dev, 
            buf, 
            sizeof(buf), 
            I2C_READ_TIMEOUT_MS
        );
        uint32_t t_ms = now_ms();         // stamp immediately after the transfer

        if (ret != ESP_OK) {
            if ((i2c_errs++ & 0x3F) == 0) {
                ESP_LOGE(TAG, "read failed: %s (%" PRIu32 " total)", esp_err_to_name(ret), i2c_errs);
            }
            continue;
        }
        if (sensirion_crc8(&buf[0], 2) != buf[2] || sensirion_crc8(&buf[3], 2) != buf[5]) {
            if ((crc_errs++ & 0x3F) == 0) {
                ESP_LOGE(TAG, "CRC mismatch (%" PRIu32 " total)", crc_errs);
            }
            continue;
        }
        
        raw_sample_t s = {
            .p_raw    = (int16_t)(uint16_t)((buf[0] << 8) | buf[1]),
            .t_raw    = (int16_t)(uint16_t)((buf[3] << 8) | buf[4]),
            .t_ms = t_ms,
        };
        measurement_ring_push(&s_ring, &s);
    }
}

// process task, measurement ring buffer -> state
typedef struct {
    // current stats window
    int64_t p_sum, p_sumsq, t_sum;
    int32_t n, start_ms;
    // result between window close
    bool have_window;
    float db, temp_c;
    float latest_mpa;
} proc_t;

static void window_close(proc_t *c) {
    double n = (double)c->n;
    double mean = (double)c->p_sum / n;
    double var = (double)c->p_sumsq / n - mean * mean;
    if (var < 0) {
        var = 0;
    }

    // handle pressure offest
    double rms_pa = sqrt(var) / PRESSURE_SCALE_FACTOR;
    c->db = rms_pa > REF_PRESSURE_PA ? (float)(20.0 * log10(rms_pa / REF_PRESSURE_PA)) : 0.0f;
    c->temp_c = (float)((double)c->t_sum / n / TEMP_SCALE_FACTOR);
    c->have_window = true;

    c->p_sum = 0;
    c->p_sumsq = 0;
    c->t_sum = 0;
    c->n = 0;
}

// convert + accumulate one drained batch then publish
static void handle_batch(proc_t *c, const raw_sample_t *batch, uint32_t n) {
    static pub_sample_t pub[BATCH_MAX];

    for (uint32_t i = 0; i < n; i++) {
        const raw_sample_t *r = &batch[i];

        int32_t mpa = counts_to_mpa(r->p);
        pub[i].mpa = mpa;
        pub[i].t_ms = r->t_ms;

        c->latest_mpa = (float)mpa;

        if (c->n == 0) {
            c->start_ms = r->t_ms;
        }
        c->p_sum   += r->p;
        c->p_sumsq += (int32_t)r->p * r->p;
        c->t_sum   += r->t;
        c-> n++;
        if(!c->have_window) {
            // show live temp if no window
            c->temp_c = (float)r->t / TEMP_SCALE_FACTOR;
        }
        if ((uint32_t)(r->t_ms - c->start_ms) >= STATS_WINDOW_MS) {
            window_close(c);
        }

    }

    state_t snap = {
        .uptime_s         = (uint32_t)(esp_timer_get_time() / 1000000),
        .heap_free        = esp_get_free_heap_size(),
        .raw_pressure_mpa = c->latest_mpa,
        .raw_db           = c->db,
        .temperature_c    = c->temp_c,
    };

    // publish batch to state ring  + updates overall summary stats snapshot
    state_publish(pub, n, &snap);
}


void sensor_process_task(void *pvParameters) {
    (void)pvParameters;
    static raw_sample_t batch[BATCH_MAX];
    static proc_t ctx;
    uint32_t drpped_seen = 0;
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PROCESS_PERIOD_MS));

        uint32_t n;
        // loop if behind
        do {
            n = measurement_ring_drain(&s_ring, batch, BATCH_MAX);
            handle_batch(&ctx, batch, n);
        } while (n == BATCH_MAX);

        uint32_t dropped = measurement_ring_dropped(&s_ring);
        if (dropped != drpped_seen) {
            ESP_LOGW(TAG, "raw ring overflow: %" PRIu32 " samples lost", dropped - dropped_seen);
            dropped_seen = dropped;
        }
    }
}

// public entry point
esp_err_t sensor_start(void) {
    ESP_RETURN_ON_FALSE(measurement_ring_init(&s_ring, s_ring_storage, RAW_RING_LEN) == 0,
        ESP_ERR_NO_MEM, TAG, "raw ring init");
    ESP_RETURN_ON_ERROR(sensor_hw_init(), TAG, "sensor init failed");
    // Reader above the processor so a busy processor can't delay sampling.
    // Stack depth is in BYTES on ESP-IDF.
    ESP_RETURN_ON_FALSE(xTaskCreate(sensor_read_task, "SENSOR_READ", 4096, NULL, 6, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "read task");
    ESP_RETURN_ON_FALSE(xTaskCreate(sensor_process_task, "SENSOR_PROC", 4096, NULL, 4, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "process task");
    return ESP_OK;
}