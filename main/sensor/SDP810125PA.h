#pragma once
// pressure_sensor.h

#include <stdbool.h>
#include "esp_err.h"

// initializes the SDP801 and starts the reat and process tasks
esp_err_t sensor_start(void);

// enable/disable 1 Hz logging (CONFIG_INFRASOUND_SENSOR_VERBOSE)
void sensor_set_verbose(bool on);