#pragma once

#include "esp_err.h"

/* Initializes WiFi in station mode and starts connecting.
 * Non-blocking: listen for IP_EVENT_STA_GOT_IP to know when you're online.
 * Requires nvs_flash_init(), esp_netif_init() and
 * esp_event_loop_create_default() to have been called first. */
esp_err_t wifi_init_sta(void);
