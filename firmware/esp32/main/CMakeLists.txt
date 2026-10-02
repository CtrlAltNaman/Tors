idf_component_register(SRCS "main.c" "audio_conversion.c" "backend_client.c" "kws_detector.cc" "stream_policy.c"
                           "diagnostic_store.c" "device_diagnostics.c" "kws_features.c" "kws_frontend.c"
                           "device_state.c" "device_leds.c" "recording_format.c" "local_recordings.c" "recordings_http.c" "playback_buffer.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio
                             esp_driver_i2s
                             esp_driver_usb_serial_jtag
                             esp-tflite-micro
                             esp_websocket_client
                             esp_wifi
                             esp_netif
                             esp_http_server
                             esp_partition
                             esp_psram
                             esp_timer
                             nvs_flash)

# Preserve the package's multiply/add rounding (no fused contractions/fast-math).
set_source_files_properties(kws_features.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
