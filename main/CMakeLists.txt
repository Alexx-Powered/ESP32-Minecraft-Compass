idf_component_register(
    SRCS
        "main.c"
        "calibration.c"
        "config_portal.c"
        "geo_config.c"
        "gps_service.c"
        "ui_service.c"
        "oled_display.c"
        "oled_font.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES
        "config_page.html"
    PRIV_REQUIRES
        json
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        esp_hw_support
        esp_lcd
        esp_timer
        nvs_flash
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_rmt
        esp_driver_uart
        hmc5883l
        esp-idf-lib__i2cdev
        espressif__led_strip
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall
    -Wextra
    -Werror=return-type
)
