idf_component_register(
    SRCS "a4_board.c" "a4_battery_smoothing.c" "gsl1680_fw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_i2c esp_adc
    PRIV_REQUIRES esp_timer esp_hw_support esp_system nvs_flash
)
