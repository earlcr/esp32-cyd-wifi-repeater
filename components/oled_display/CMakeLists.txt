if(NOT IDF_TARGET STREQUAL "esp32c3" AND NOT IDF_TARGET STREQUAL "esp32s3")
    set(OLED_SRCS "st7789_parallel_display.c")
    set(OLED_REQUIRES esp_lcd driver nvs_flash freertos log lwip esp_netif esp_wifi esp_hw_support)
else()
    set(OLED_SRCS "oled_display.c")
    set(OLED_REQUIRES esp_driver_i2c nvs_flash freertos log lwip esp_netif esp_wifi esp_driver_gpio)
endif()

idf_component_register(
    SRCS ${OLED_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${OLED_REQUIRES}
)
if(OLED_SRCS)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Os)
endif()
