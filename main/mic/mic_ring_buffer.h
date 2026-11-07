#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// measurement ring buffer - 
// sensor_read_task (raw_ring_push)
// sensor_process_task (raw_ring_drain)
// drop oldest on overflow

typedef struct {
    int16_t p_raw;  // raw pressure
    int16_t t_raw;  // raw temp
    uint32_t t_ms;  // timestamp (after i2c read)
} raw_sample_t;

// setup ring buffers
typedef struct {
    raw_sample_t *buf;
    uint32_t     mask;  // capacity - 1 (capacity must be power of 2)
    uint32_t     head;
    uint32_t     tail;   
    uint32_t     dropped;  // samples overwritten
    SemaphoreHandle_t lock;
} measurement_ring_buffer_t;

int measurement_ring_init(measurement_ring_buffer_t *r, raw_sample_t *storage, uint32_t cap_pow2);
void measurement_ring_push(measurement_ring_buffer_t *r, const raw_sample_t *s);
uint32_t measurement_ring_drain(measurement_ring_buffer_t *r, raw_sample_t *out, uint32_t max);
uint32_t measurement_ring_count(measurement_ring_buffer_t *r);
uint32_t measurement_ring_dropped(measurement_ring_buffer_t *r);