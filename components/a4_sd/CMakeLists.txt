idf_component_register(
    SRCS "a4_sd.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES fatfs sdmmc esp_driver_sdspi esp_driver_spi
)
