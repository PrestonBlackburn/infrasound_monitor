// infrasound_monitor.c

#include <stdio.h>
#include "esp_netif.h"
#include "wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "pressure_sensor.h"
#include "display.h"
#include "state.h"
#include "webserver.h"
#include "battery.h"
#include "lorawan.h"
#include "i2s_mic.h"

static void log_heap(const char *where) {
    ESP_LOGI("heap", "%s: internal free=%u, largest block=%u, min ever=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

void app_main(void)
{
    printf("Booting Up Monitor!\n");
    log_heap("boot");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // init shared mutex and state_t
    state_init();
    log_heap("after init");

    // setup the I2C comms and tasks for the SDP801 Sensor
    // Create the read (from sensor) and process (shared state) tasks
    ESP_ERROR_CHECK(battery_start());
    ESP_ERROR_CHECK(sensor_start());
    ESP_ERROR_CHECK(i2s_start());
    log_heap("after mic");

    // display inits SPI bus, 
    // so it must be before the lorawan radio init
    // which shares the SPI bus
    ESP_ERROR_CHECK(display_start());
    log_heap("after display");

    ESP_ERROR_CHECK(lorawan_radio_init());   // shared SPI + SX1262 up, idle
    ESP_ERROR_CHECK(start_lorawan_task());
    log_heap("after lorawan");

    ESP_ERROR_CHECK(webserver_start());

    log_heap("before wifi");
    ESP_ERROR_CHECK(wifi_init_sta());
    log_heap("after wifi");

    sensor_set_verbose(true);  //override logging setting
    
}
