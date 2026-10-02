idf_component_register(SRCS "main.c" "mic_protocol.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2s esp_driver_usb_serial_jtag esp_timer)
