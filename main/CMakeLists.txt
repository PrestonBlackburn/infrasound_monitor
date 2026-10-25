set(requires esp-tls nvs_flash esp_netif esp_http_server web)
idf_build_get_property(target IDF_TARGET)

# web assets: render templates, gzip, embed
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(WEB_TEMPLATES  ${PROJECT_DIR}/templates)
    set(WEB_STATIC     ${PROJECT_DIR}/static)
    set(WEB_TOOLS      ${PROJECT_DIR}/tools/web)
    set(WEB_OUT        ${CMAKE_CURRENT_BINARY_DIR}/web_out)

    # files that should trigger a re-render
    file(GLOB_RECURSE WEB_INPUTS CONFIGURE_DEPENDS
        ${WEB_TEMPLATES}/* ${WEB_STATIC}/* ${WEB_TOOLS}/*.py
        ${PROJECT_DIR}/main/state/state_struct.h)

    # Outputs
    set(WEB_GZ ${WEB_OUT}/dashboard.html.gz)
    file(GLOB_RECURSE STATIC_FILES CONFIGURE_DEPENDS ${WEB_STATIC}/*)
    foreach(f ${STATIC_FILES})
        get_filename_component(name ${f} NAME)
        list(APPEND WEB_GZ ${WEB_OUT}/${name}.gz)
    endforeach()

    set(WEB_PYTHON ${PROJECT_DIR}/.venv/bin/python)
    if(NOT EXISTS ${WEB_PYTHON})
        set(WEB_PYTHON ${PYTHON})   # fall back to IDF's Python
    endif()

    add_custom_command(
        OUTPUT ${WEB_GZ}
        COMMAND ${WEB_PYTHON} ${WEB_TOOLS}/build_web.py ${WEB_TEMPLATES} ${WEB_STATIC} ${WEB_OUT}
        DEPENDS ${WEB_INPUTS}
        COMMENT "Rendering web assets"
        VERBATIM
    )
endif()


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
        "local_display/ui_access_point.c"
        "local_display/ui_battery.c"
        "web/wifi.c"
        "web/webserver.c"
        "web/web_frame.c"
        "web/web_assets.c"
        "battery/battery.c"
    PRIV_REQUIRES 
        esp_driver_i2c 
        esp_timer 
        esp_lcd 
        esp_event
        esp_driver_spi 
        esp_driver_gpio
        esp_lcd_st7735
        esp_wifi
        esp_netif
        esp_http_server
        esp_eth
        nvs_flash
        esp_adc
        EMBED_FILES ${WEB_GZ}
    INCLUDE_DIRS 
        "." 
        "sensor" 
        "state" 
        "local_display"
        "web"
        "battery"
)
