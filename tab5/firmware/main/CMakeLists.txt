idf_component_register(SRCS "main.c" "board.c"
                       INCLUDE_DIRS "."
                       REQUIRES ftc fatfs sdmmc esp_driver_sdmmc esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_timer)
