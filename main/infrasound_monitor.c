// infrasound_monitor.c

#include <stdio.h>
#include "esp_netif.h"
#include "wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "pressure_sensor.h"
#include "display.h"
#include "state.h"
#include "webserver.h"

void app_main(void)
{
    printf("Booting Up Monitor!\n");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // init shared mutex and state_t
    state_init();

    // setup the I2C comms and tasks for the SDP801 Sensor
    // Create the read (from sensor) and process (shared state) tasks
    ESP_ERROR_CHECK(sensor_start());
    ESP_ERROR_CHECK(display_start());
    ESP_ERROR_CHECK(webserver_start());
    ESP_ERROR_CHECK(wifi_init_sta());

    sensor_set_verbose(true);  //override logging setting
}
