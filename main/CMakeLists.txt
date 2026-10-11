set(requires esp-tls nvs_flash esp_netif esp_http_server web_assets)
idf_build_get_property(target IDF_TARGET)

if(${target} STREQUAL "linux")
    list(APPEND requires esp_stubs protocol_examples_common)
else()
    list(APPEND requires esp_wifi esp_eth)
endif()

idf_component_register(SRCS "infrasound_monitor.c" "pressure_sensor.c" "wifi.c" "webserver.c" spi_lcd_touch_example_main.c" "lvgl_demo_ui.c"
                    PRIV_REQUIRES esp_driver_i2c spi_flash esp_lcd_st7735 esp_timer ${requires}
                    INCLUDE_DIRS ".")
