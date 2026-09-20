idf_component_register(SRCS "main.c" "ble_link.c" "update_page.c" "ota.c" "imu.c" "led.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_lcd esp_timer esp_driver_gpio nvs_flash bt app_update esp_app_format esp_driver_i2c esp_driver_rmt epet_core)
