#pragma once
#include "esp_err.h"

typedef enum {
    EXPANDER_BTN_PAGE = 0,   // B2
    EXPANDER_BTN_2,          // B3
    EXPANDER_BTN_COUNT
} expander_btn_t;

esp_err_t expander_start(void);

// Last sampled level: 1 = released, 0 = pressed. No I2C traffic, safe from any task.
int expander_button_level(expander_btn_t btn);

// Number of debounced presses since boot. Compare with your last-seen value to detect new presses.
uint32_t expander_button_press_count(expander_btn_t btn);