set(requires esp-tls nvs_flash esp_netif esp_http_server web_assets)
idf_build_get_property(target IDF_TARGET)

# setup for sensor only
idf_component_register(SRCS "infrasound_monitor.c" "sensor/pressure_sensor.c" "sensor/raw_ring_buffer.c" "state/state.c"
                    PRIV_REQUIRES esp_driver_i2c esp_timer
                    INCLUDE_DIRS "." "sensor" "state")
