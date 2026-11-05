# ESP-IDF 6.0.1 ships ChaN FatFs with the exFAT implementation present but
# hard-disabled in src/ffconf.h. This component intentionally has the same
# name as the SDK component, so it overrides that component for this project
# without changing the shared ESP-IDF checkout.
if(CMAKE_SCRIPT_MODE_FILE)
    file(TO_CMAKE_PATH "$ENV{IDF_PATH}/components/fatfs/Kconfig"
         FATFS_IDF_KCONFIG)
    idf_component_register(
        INCLUDE_DIRS "include"
        REQUIRES wear_levelling sdmmc esp_driver_sdmmc esp_driver_sdspi
        PRIV_REQUIRES vfs esp_driver_gpio esptool_py
        KCONFIG "${FATFS_IDF_KCONFIG}"
    )
    return()
endif()

idf_build_get_property(FATFS_IDF_PATH IDF_PATH)
set(FATFS_IDF_COMPONENT "${FATFS_IDF_PATH}/components/fatfs")
set(FATFS_CORE_DIR "${CMAKE_CURRENT_BINARY_DIR}/core")

if(NOT EXISTS "${FATFS_IDF_COMPONENT}/src/ff.c" OR
   NOT EXISTS "${FATFS_IDF_COMPONENT}/src/ffconf.h")
    message(FATAL_ERROR "local fatfs override needs ESP-IDF FatFs sources")
endif()

file(MAKE_DIRECTORY "${FATFS_CORE_DIR}")
foreach(FATFS_CORE_FILE ff.c ff.h ffunicode.c diskio.h)
    configure_file(
        "${FATFS_IDF_COMPONENT}/src/${FATFS_CORE_FILE}"
        "${FATFS_CORE_DIR}/${FATFS_CORE_FILE}"
        COPYONLY
    )
endforeach()

file(READ "${FATFS_IDF_COMPONENT}/src/ffconf.h" FATFS_FFCONF)
string(REGEX REPLACE
       "#define[ \t]+FF_FS_EXFAT[ \t]+0"
       "#define FF_FS_EXFAT 1"
       FATFS_FFCONF "${FATFS_FFCONF}")
string(REPLACE
       "#include \"sdkconfig.h\""
       "#include \"sdkconfig.h\"\n\n/* exFAT directory scanning evaluates this in a C expression. */\n#ifndef CONFIG_FATFS_USE_LABEL\n#define CONFIG_FATFS_USE_LABEL 0\n#endif"
       FATFS_FFCONF "${FATFS_FFCONF}")
if(NOT FATFS_FFCONF MATCHES
       "#define[ \t]+FF_FS_EXFAT[ \t]+1")
    message(FATAL_ERROR "Unable to enable FF_FS_EXFAT in ESP-IDF FatFs")
endif()
if(NOT FATFS_FFCONF MATCHES
       "#define CONFIG_FATFS_USE_LABEL 0")
    message(FATAL_ERROR "Unable to define the disabled volume-label setting")
endif()
file(WRITE "${FATFS_CORE_DIR}/ffconf.h" "${FATFS_FFCONF}")

set(FATFS_SRCS
    "${FATFS_CORE_DIR}/ff.c"
    "${FATFS_CORE_DIR}/ffunicode.c"
    "${FATFS_IDF_COMPONENT}/diskio/diskio.c"
    "${FATFS_IDF_COMPONENT}/diskio/diskio_rawflash.c"
    "${FATFS_IDF_COMPONENT}/diskio/diskio_wl.c"
    "${FATFS_IDF_COMPONENT}/diskio/diskio_sdmmc.c"
    "${FATFS_IDF_COMPONENT}/port/freertos/ffsystem.c"
    "${FATFS_IDF_COMPONENT}/vfs/vfs_fat.c"
    "${FATFS_IDF_COMPONENT}/vfs/vfs_fat_sdmmc.c"
    "${FATFS_IDF_COMPONENT}/vfs/vfs_fat_spiflash.c"
    "fatfs_status.c"
)

idf_component_register(
    SRCS ${FATFS_SRCS}
    INCLUDE_DIRS "include" "${FATFS_CORE_DIR}"
                 "${FATFS_IDF_COMPONENT}/diskio"
                 "${FATFS_IDF_COMPONENT}/vfs"
    REQUIRES wear_levelling sdmmc esp_driver_sdmmc esp_driver_sdspi
    PRIV_REQUIRES vfs esp_driver_gpio esptool_py
    KCONFIG "${FATFS_IDF_COMPONENT}/Kconfig"
)
