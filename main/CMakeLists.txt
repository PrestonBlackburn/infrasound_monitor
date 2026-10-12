set(requires esp-tls nvs_flash esp_netif esp_http_server)
idf_build_get_property(target IDF_TARGET)

# setup for sensor only
idf_component_register(
    SRCS 
        "infrasound_monitor.c" 
        "sensor/pressure_sensor.c" 
        "sensor/raw_ring_buffer.c" 
        "state/state.c" 
        "local_display/display.c"
        "local_display/ui_chart.c"
        "local_display/ui_metrics.c"
    PRIV_REQUIRES 
        esp_driver_i2c 
        esp_timer 
        esp_lcd 
        esp_driver_spi 
        esp_driver_gpio
        esp_lcd_st7735
    INCLUDE_DIRS 
        "." 
        "sensor" 
        "state" 
        "local_display"
)
