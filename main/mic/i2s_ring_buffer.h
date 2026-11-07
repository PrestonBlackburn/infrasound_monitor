#pragma once
// i2s_ring_buffer.h

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// measurement ring buffer - 
// sensor_read_task (i2s_ring_push)
// sensor_process_task (i2s_ring_drain)
// drop oldest on overflow

typedef struct {
    int16_t p_raw;  // raw pressure
    int16_t t_raw;  // raw temp
    uint32_t t_ms;  // timestamp (after i2c read)
} i2s_sample_t;

// setup ring buffers
typedef struct {
    i2s_sample_t *buf;
    uint32_t     mask;  // capacity - 1 (capacity must be power of 2)
    uint32_t     head;
    uint32_t     tail;   
    uint32_t     dropped;  // samples overwritten
    SemaphoreHandle_t lock;
} i2s_ring_buffer_t;

int i2s_ring_init(i2s_ring_buffer_t *r, i2s_sample_t *storage, uint32_t cap_pow2);
void i2s_ring_push(i2s_ring_buffer_t *r, const i2s_sample_t *s);
uint32_t i2s_ring_drain(i2s_ring_buffer_t *r, i2s_sample_t *out, uint32_t max);
uint32_t i2s_ring_count(i2s_ring_buffer_t *r);
uint32_t i2s_ring_dropped(i2s_ring_buffer_t *r);