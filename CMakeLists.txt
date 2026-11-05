cmake_minimum_required(VERSION 3.16)

# Standalone developer-demo firmware version.
set(PROJECT_VER "V1.1.0-DEMO" CACHE STRING
    "Firmware version embedded for display and OTA comparison")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eegoread_firmware)
