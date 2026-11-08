#pragma once
// i2s_mic.h

#include <stdbool.h>
#include "esp_err.h"

#define MIC_SAMPLE_RATE  16000

// Starts I2S capture and the read + DSP + log tasks.
esp_err_t i2s_start(void);

// enable/disable 1 Hz logging
void i2s_set_verbose(bool on);