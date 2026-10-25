#pragma once
#include "esp_err.h"

esp_err_t battery_start(void);
int battery_get_percent(void);
int battery_get_mv(void); // battery voltage directly