# MIPI-DSI panel drivers for the Tab5's ST7123 / ST7121 displays, vendored
# from M5Stack's Tab5 BSP (M5Tab5-UserDemo, Apache-2.0, (c) Espressif/M5Stack).
idf_component_register(SRCS "esp_lcd_st7123.c" "esp_lcd_st7121.c" "st7123_init_data.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd esp_driver_gpio)
