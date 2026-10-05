idf_component_register(
    SRCS "app_main.cc" "kws_features.c" "kws_audio.c" "kws_detector.c" "kws_mic.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_timer)
