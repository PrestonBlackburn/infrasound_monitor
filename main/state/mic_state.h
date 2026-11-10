#pragma once
// mic_state.h

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "state_struct.h"


#ifndef W_MIC_PUB_RING_LEN
#define W_MIC_PUB_RING_LEN 512  // ~10s at 100 Hz, must be power of two
#endif

#ifndef SPEC_RING_LEN
#define SPEC_RING_LEN 256        // Waterfall history buffer length
#endif

typedef struct {
    int16_t min;
    int16_t max;
} wave_bin_t;

typedef struct {
    uint32_t   t_ms;
    wave_bin_t bins[50]; // 50 UI bins
} mic_waveform_sample_t;

int mic_state_init(void);
void mic_waveform_publish(const mic_waveform_sample_t *s);
uint32_t mic_waveform_read_since(uint64_t *cursor, mic_waveform_sample_t *out, uint32_t max, uint32_t *dropped);

typedef struct {
    uint32_t t_ms;
    float    mel_bins[64]; // Your 64 Mel-bins
} mic_spec_sample_t;

void mic_spec_publish(const mic_spec_sample_t *s);
uint32_t mic_spec_read_since(uint64_t *cursor, mic_spec_sample_t *out, uint32_t max, uint32_t *dropped);