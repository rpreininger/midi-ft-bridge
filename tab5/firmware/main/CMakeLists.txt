idf_component_register(SRCS "main.c" "board.c" "net.c" "ft_send.c" "config.c" "stream_test.c" "web.c" "midi_host.c" "usb_audio.c" "console.c" "player.c" "display.c" "font.c" "power.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_usb_serial_jtag ftc fatfs sdmmc esp_driver_sdmmc esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_timer nvs_flash esp_netif esp_event esp_wifi json lwip esp_http_server usb esp_lcd esp_driver_ledc tab5_lcd)
