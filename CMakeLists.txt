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
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# One Component Manager lock per chip. A single dependencies.lock records the
# target it was solved for, so building an ESP32 profile after an ESP32-S3 one
# would rewrite it. Commit every dependencies.lock.<target> file.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")

project(dfu32)
