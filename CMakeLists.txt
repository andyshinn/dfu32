# DFU32: ESP-IDF project root.
#
# Board profiles live in boards/<name>/sdkconfig.defaults. Each profile sets
# CONFIG_IDF_TARGET, flash size, partition table, PSRAM mode, console and pins,
# so do NOT run `idf.py set-target`; select a profile instead and give each
# profile its own build directory:
#
#   idf.py -B build/feather_esp32s3 -DDFU32_BOARD=feather_esp32s3 build
#
# The generated sdkconfig lives inside the build directory, so profiles never
# clobber each other.
#
# -DDFU32_DEBUG_ENDPOINTS=ON adds sdkconfig.debug (the fault-injection
# endpoints the bench runner needs for its safety checks). Give it its own
# build directory: defaults only apply when the sdkconfig is first generated.
#
#   idf.py -B build/feather_esp32s3-debug -DDFU32_BOARD=feather_esp32s3 -DDFU32_DEBUG_ENDPOINTS=ON build
cmake_minimum_required(VERSION 3.22)

set(DFU32_BOARD "feather_esp32s3" CACHE STRING "DFU32 board profile (directory name under boards/)")
set(_dfu32_board_defaults "${CMAKE_CURRENT_LIST_DIR}/boards/${DFU32_BOARD}/sdkconfig.defaults")
if(NOT EXISTS "${_dfu32_board_defaults}")
    message(FATAL_ERROR "Unknown DFU32_BOARD '${DFU32_BOARD}': ${_dfu32_board_defaults} not found")
endif()

set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults"
    "${_dfu32_board_defaults}"
)
option(DFU32_DEBUG_ENDPOINTS "Bench-only build with the fault-injection endpoints (never for release)" OFF)
if(DFU32_DEBUG_ENDPOINTS)
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.debug")
endif()
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dfu32)
