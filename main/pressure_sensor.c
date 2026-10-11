#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"


static const char *TAG = "diff_pressure";

#define I2C_MASTER_SCL_IO           9/*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           8 /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              I2C_NUM_0 /*!< I2C port number for master dev */
#define I2C_MASTER_FREQ_HZ          100000 /*!< I2C master clock frequency 400kHz-1000kHz */
// #define I2C_MASTER_TIMEOUT_MS       1000
#define SENSOR_ADDR         0x25        /*!< Address of the Sensiron sensor */

// Sensor properties
#define READING_MAX_LEN 512
#define PRESSURE_SCALE_FACTOR 240 /* Pascals */
#define TEMP_SCALE_FACTOR 200 /* C */

// Calculation properties
#define BATCH_MAX 512 // about 100 samples/sec (5 sec window)
static int16_t raw_pressure_arr[BATCH_MAX];
static int16_t raw_temp_arr[BATCH_MAX];
static SemaphoreHandle_t rb_lock; // for keeping press + temp readings in sync

typedef struct {
    int n;
    int64_t t0_us; // batch timestamp
    int16_t pressure_scale; 
    int16_t temperature_scale;
    int16_t pressure_raw[BATCH_MAX];
    int16_t temperature_raw[BATCH_MAX];
} sse_live_msg_t;

typedef struct {
    int n;                            // samples in summary
    float avg_p, min_p, max_p, std_p; // Pa
    float avg_t;                      // C
    // additional metadata
    int heap_free;
    int uptime;
} sse_summary_msg_t;

static QueueHandle_t sse_live_queue;
static QueueHandle_t sse_summary_queue;

TaskHandle_t SensorReadHandle = NULL;
TaskHandle_t SesnorProcessHandle = NULL;
TaskHandle_t QueueTestReadHandle = NULL;


// setup ring buffers
typedef struct {
    int16_t * const buffer;
    int head;
    int tail;
    const int maxlen;
} measurement_ring_buffer_t;

int16_t pressure_reading_space[READING_MAX_LEN];
int16_t temperature_reading_space[READING_MAX_LEN];

measurement_ring_buffer_t pressure_reading_ring_buffer = {
    .buffer = pressure_reading_space,
    .head = 0,
    .tail = 0,
    .maxlen = READING_MAX_LEN
};

measurement_ring_buffer_t temperature_reading_ring_buffer = {
    .buffer = temperature_reading_space,
    .head = 0,
    .tail = 0,
    .maxlen = READING_MAX_LEN
};

int circ_bbuf_push(measurement_ring_buffer_t *c, int16_t data) {
    int next;
    
    next = c->head+1; // head after write
    if (next >= c->maxlen) {
        next = 0;
    }
    // if head + 1 == tail, buffer full
    // on full, discard oldest to make room
    if (next == c->tail) { 
        c->tail++;
        if (c->tail >= c->maxlen) {
            c->tail = 0;
        }
    }
    
    c->buffer[c->head] = data; // load data then move
    c->head = next;   // head to next data offset
    return 0; // return success
}

int circ_bbuf_pop(measurement_ring_buffer_t *c, int16_t *data) {
    int next;
    
    if (c->head == c->tail) { // no data in this case
        return -1;
    }
    next = c->tail + 1; // tail after read
    if (next >= c->maxlen) {
        next = 0;
    }
    *data = c->buffer[c->tail]; // read data
    c->tail = next; // tail to next offset
    
    return 0;
}

int circ_bbuf_peek_latest(
    const measurement_ring_buffer_t *c, 
    int16_t *out, 
    int n_samples
) {
    int count = c->head - c->tail;
    if (count < 0) { // get the whole ring if head less than tail
        count += c->maxlen; 
    }
    if (n_samples > count) { // set n_samples to count if fewer samples
        n_samples = count;
    }

    int start = c->head - n_samples;
    if (start < 0) {
        start += c->maxlen;
    }
    for (int i = 0; i<n_samples; i++) {
        out[i] = c->buffer[(start+i) % c->maxlen];
    }
    return n_samples; // number copied
}


void sensor_read_task(void *pvParameters) {
    (void)pvParameters;

    ESP_LOGI(TAG, "Initializing I2C Master Bus...");
    /* Print chip information */
   
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_config, &bus_handle));
    
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SENSOR_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev_handle;
    
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle));

    ESP_LOGI(TAG, "I2C Initialized successfully");

    esp_err_t p = i2c_master_probe(bus_handle, SENSOR_ADDR, 100);
    ESP_LOGI(TAG, "Probe 0x%02X: %s", SENSOR_ADDR, esp_err_to_name(p));

    // Reset any existing continuous measurement /soft reset
    uint8_t stop_cmd[] = {0x3F, 0xF9};
    esp_err_t stop_ret = i2c_master_transmit(dev_handle, stop_cmd, sizeof(stop_cmd), 100);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (stop_ret == ESP_OK) {
        ESP_LOGI(TAG, "Stopped any existing measurements");
    } else {
        ESP_LOGE(TAG, "Stop Config Failed: %s", esp_err_to_name(stop_ret));
        vTaskDelete(NULL);
    }

    // Set to Continuous Measurement Mode 
    // 0x361E: Diff Pressure, no averaging 0.5ms update
    uint8_t config_cmd[] = {0x36, 0x1E};    

    esp_err_t ret = i2c_master_transmit(
        dev_handle,
        config_cmd,
        sizeof(config_cmd),
        1000
    );

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Config success");
    } else {
        ESP_LOGE(TAG, "Config Failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Continuous Measurement Mode Started");
    // First measurement after 8ms
    vTaskDelay(pdMS_TO_TICKS(15));

    // read i2c data, apply scaling factor, write to ring buffer
    // 3 bytes Diff pressure (MSB, LSB, CRC), 3 bytes Temp (MSB, LSB, CRC)
    // limit logging here to debug and errors
    uint8_t raw_data[9] = {0};

    for (;;) {
        ret = i2c_master_receive(
            dev_handle, 
            raw_data, 
            sizeof(raw_data), 
            1000
        );

        int16_t raw_dp = 0;
        int16_t raw_temp = 0;
        // int16_t scale_factor = 0;
        if (ret == ESP_OK) {
            raw_dp = (int16_t)((raw_data[0] << 8) | raw_data[1]);
            raw_temp = (int16_t)((raw_data[3] << 8) | raw_data[4]);
            //scale_factor = (int16_t)((raw_data[6] << 8) | raw_data[7]);

            // ESP_LOGI(TAG, "Raw DP: %d | Raw Temp: %d", raw_dp, raw_temp);
        } else {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(ret));
        }

        if (ret == ESP_OK) {
            xSemaphoreTake(rb_lock, portMAX_DELAY);
            circ_bbuf_push(&pressure_reading_ring_buffer, raw_dp);
            circ_bbuf_push(&temperature_reading_ring_buffer, raw_temp);
            xSemaphoreGive(rb_lock);
            // log only on debug
            // ESP_LOGI(TAG, "Pressure Raw: %d Pa | Temp Raw: %d C",
            //     pressure_reading_ring_buffer.buffer[pressure_reading_ring_buffer.head-1], 
            //     temperature_reading_ring_buffer.buffer[temperature_reading_ring_buffer.head-1]);
        } else {
            ESP_LOGE(TAG, "Calc failed: %s", esp_err_to_name(ret));
        }
        // slight subsampling
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    
    vTaskDelete(NULL); 

}

void sensor_process_task(void *pvParameters) {
    (void)pvParameters;

    static sse_live_msg_t live_msg;
    static sse_summary_msg_t summary_msg;

    // tracking for summary stats
    int64_t p_sum = 0;
    int64_t p_sumsq = 0;
    int64_t t_sum = 0;
    int16_t p_min = INT16_MAX;
    int16_t p_max = INT16_MIN;
    int32_t acc_n = 0;   // summary frequency is different than live
    int64_t window_start = esp_timer_get_time();

    live_msg.pressure_scale = PRESSURE_SCALE_FACTOR;
    live_msg.temperature_scale = TEMP_SCALE_FACTOR;

    TickType_t last_wake = xTaskGetTickCount();


    for (;;) {
        int n = 0;
        xSemaphoreTake(rb_lock, portMAX_DELAY);
        while (n < BATCH_MAX && circ_bbuf_pop(
                &pressure_reading_ring_buffer,
                &raw_pressure_arr[n]) == 0) {
                    circ_bbuf_pop(
                        &temperature_reading_ring_buffer,
                        &raw_temp_arr[n]);
                    n++;

                    }
        xSemaphoreGive(rb_lock);

        // collection path
        if (n > 0) {
            // live data collection
            live_msg.n = n;
            live_msg.t0_us = esp_timer_get_time();
            memcpy(live_msg.pressure_raw, raw_pressure_arr, n * sizeof(int16_t));
            memcpy(live_msg.temperature_raw, raw_temp_arr, n * sizeof(int16_t));
            if (xQueueSend(sse_live_queue, &live_msg, 0) != pdTRUE) {
                // sse side is behind, drop old values
            }

            // summary collector
            for (int i = 0; i < n; i++) {
                int16_t p = raw_pressure_arr[i];
                p_sum += p;
                p_sumsq += (int32_t)p * p;
                t_sum += raw_temp_arr[i];
                if (p < p_min) p_min = p;
                if (p > p_max) p_max = p;
            }
            acc_n += n;
        }

        // reset summary counter
        int64_t now = esp_timer_get_time();
        if (now - window_start >= 1000000 && acc_n > 0) {
            float mean_raw = (float)p_sum / acc_n;
            float var_raw = (float)p_sumsq / acc_n - mean_raw * mean_raw;
            if (var_raw < 0) {
                var_raw = 0;
            }

            summary_msg.n = acc_n;
            summary_msg.avg_p = mean_raw / PRESSURE_SCALE_FACTOR;
            summary_msg.min_p = (float)p_min / PRESSURE_SCALE_FACTOR;
            summary_msg.max_p = (float)p_max / PRESSURE_SCALE_FACTOR;
            summary_msg.std_p = sqrtf(var_raw) / PRESSURE_SCALE_FACTOR;
            summary_msg.avg_t = ((float)t_sum / acc_n) / TEMP_SCALE_FACTOR;
            summary_msg.heap_free = esp_get_free_heap_size();
            summary_msg.uptime = (uint32_t)(now / 1000000);
            xQueueSend(sse_summary_queue, &summary_msg, 0);

            // reset
            p_sum = 0;
            p_sumsq = 0;
            t_sum = 0;
            p_min = INT16_MAX;
            p_max = INT16_MIN;
            acc_n = 0;
            window_start = now;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
    }
}

void test_queue_consume(void *pvParameters) {
    (void)pvParameters;

    static sse_live_msg_t live_msg_recv;
    // static sse_summary_msg_t summary_msg_recv;

    for(;;) {
        if (xQueueReceive(
            sse_live_queue,
            &live_msg_recv,
            portMAX_DELAY) == pdPASS) {
                printf("Queued Samples: %d\n", 
                    live_msg_recv.n
                    );
            }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


void app_main(void)
{
    rb_lock = xSemaphoreCreateMutex();

    sse_live_queue = xQueueCreate(4, sizeof(sse_live_msg_t));
    sse_summary_queue = xQueueCreate(2, sizeof(sse_summary_msg_t));

    if (rb_lock == NULL || !sse_live_queue || !sse_summary_queue) {
        ESP_LOGE(TAG, "Failed to create ring buffer mutex or queues");
        return;
    }

    BaseType_t xReturned = xTaskCreate(
        sensor_read_task,   /* Function that implements the task. */
        "SENSOR_READ",      /* Text name for the task. */
        4096,               /* Stack size in words, not bytes. */
        NULL,               /* Parameter passed into the task. */
        1,                  /* Priority at which the task is created. */
        &SensorReadHandle 
    );


    if (xReturned == pdPASS) {
        // Task was created successfully
    }

    BaseType_t xProcessReturned = xTaskCreate(
        sensor_process_task,
        "SENSOR_PROCESS",
        4096,
        NULL,
        1,
        &SesnorProcessHandle
    );

    if (xProcessReturned == pdPASS) {
        // Task was created successfully
    }


    BaseType_t xTestQueueRead = xTaskCreate(
        test_queue_consume,
        "QUEUE_TEST",
        4096,
        NULL,
        1,
        &QueueTestReadHandle
    );

    if (xTestQueueRead == pdPASS) {
        // Task was created successfully
    }

}
