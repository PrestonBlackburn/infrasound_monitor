// lorawan.h
#pragma once

#include "esp_err.h"

// lorawan.cpp from radiolib compat
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lorawan_radio_init(void);
esp_err_t start_lorawan_task(void);
 
#ifdef __cplusplus
}
#endif