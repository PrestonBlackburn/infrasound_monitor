#pragma once
// i2s_mic.h

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MIC_SAMPLE_RATE  16000

// Latest summary levels, updated every 50 ms.
typedef struct {
    uint32_t t_ms;
    float    db_spl;   // unweighted, time-domain RMS
    float    db_a;     // A-weighted (from FFT)
    float    db_c;     // C-weighted (from FFT)
} mic_levels_t;
 

// Starts I2S capture and the read + DSP + log tasks.
esp_err_t i2s_start(void);

// enable/disable 1 Hz logging
void i2s_set_verbose(bool on);

bool mic_get_levels(mic_levels_t *out);
