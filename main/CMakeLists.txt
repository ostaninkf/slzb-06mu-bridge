idf_component_register(SRCS "main.c" "bridge.c" "net.c" "web.c" "settings.c"
                            "leds.c" "mqttpub.c" "syslogc.c" "ncpflash.c" "usbmode.c" "safety.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_eth esp_netif esp_wifi esp_http_server app_update
                                nvs_flash json mqtt esp_driver_uart esp_driver_gpio
                                esp_driver_spi esp_driver_tsens esp_driver_usb_serial_jtag
                                lwip esp_timer)
