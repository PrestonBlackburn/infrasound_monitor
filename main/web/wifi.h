#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

/* Initializes WiFi in station mode and starts connecting.
 * Non-blocking: listen for IP_EVENT_STA_GOT_IP to know when you're online.
 * Requires nvs_flash_init(), esp_netif_init() and
 * esp_event_loop_create_default() to have been called first. */
esp_err_t wifi_init_sta(void);
esp_err_t wifi_save_creds(const char *ssid, const char* pass);
esp_err_t wifi_forget_creds(void);

// check if ap point is up (for ui)
bool wifi_ap_info(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);