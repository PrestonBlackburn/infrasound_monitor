#pragma once
// i2s_mic.h

#include <stdbool.h>
#include "esp_err.h"

// initializes the SDP801 and starts the reat and process tasks
esp_err_t i2s_start(void);

// enable/disable 1 Hz logging
void i2s_set_verbose(bool on);