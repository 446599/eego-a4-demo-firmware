idf_component_register(
    SRCS "epd_uc8279c.c" "uc8279c_lut.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi
    PRIV_REQUIRES esp_driver_gpio esp_timer
)
