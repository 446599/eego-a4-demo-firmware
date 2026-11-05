set(DEVDEMO_SRCS
    "devdemo/devdemo_app_loop.c"
    "devdemo/devdemo_app_registry.c"
    "devdemo/demo_fonts.c"
    "devdemo/devdemo_ui.c"
    "devdemo/devdemo_ui_menu.c"
    "devdemo/apps/app_overview.c"
    "devdemo/apps/app_display.c"
    "devdemo/apps/app_touch.c"
    "devdemo/apps/app_keys.c"
    "devdemo/apps/app_frontlight.c"
    "devdemo/apps/app_power.c"
    "devdemo/apps/app_rtc.c"
    "devdemo/apps/app_sd.c"
    "devdemo/apps/app_wifi.c"
    "devdemo/apps/app_ble.c"
)

idf_component_register(
    SRCS "developer_demo.c" ${DEVDEMO_SRCS}
    INCLUDE_DIRS "." "devdemo" "devdemo/apps"
    REQUIRES epd_uc8279c display_manager a4_board a4_sd
    PRIV_REQUIRES esp_app_format esp_driver_gpio esp_driver_spi esp_timer esp_event esp_netif esp_wifi bt esp_pm esp_hw_support nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
