#pragma once

// state.h
// shared state between sensor, web, and display

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "state_struct.h"


// Windowed ring buffer for UI consumers
#ifndef PUB_RING_LEN
#define PUB_RING_LEN 1024  // ~10s at 100 Hz, must be power of two
#endif

#ifndef W_MIC_PUB_RING_LEN
#define W_MIC_PUB_RING_LEN 1024  // ~10s at 100 Hz, must be power of two
#endif

typedef struct {
    int32_t mpa;   // pressure, milli-pascals
    uint32_t t_ms; // time since boot (ms)
} pub_sample_t;

// new pub sample and set of functions or use existing?
typedef struct {
    int32_t dbSPL;   // raw i2s readings
    uint32_t t_ms; // time since boot (ms)
} mic_pub_sample_t;

// Differential Pressure Sensor
int state_init(void);
// append a batch of readings (writers)
void state_publish(const pub_sample_t *s, uint32_t n, const state_t *snapshot);
void state_set_wifi(bool up);

// readers
bool state_get(state_t *out, uint32_t *seq, TickType_t timeout);
// cursor + helpers for incremental reads
// start `back` samples ago
uint64_t state_cursor_latest(uint32_t back); 
uint32_t state_read_since(uint64_t *cursor, pub_sample_t *out,
                            uint32_t max, uint32_t *dropped);

// Microphone 
int mic_state_init(void);
void mic_state_publish(const mic_pub_sample_t *s, uint32_t n, const mic_waveform_t *snapshot);
bool mic_state_get(mic_waveform_t *out, uint32_t *seq, TickType_t timeout);
uint64_t mic_state_cursor_latest(uint32_t back);
uint32_t mic_state_read_since(uint64_t *cursor, mic_pub_sample_t *out, uint32_t max, uint32_t *dropped);
