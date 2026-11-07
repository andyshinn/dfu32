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

# Workaround: h2zero/esp-nimble-cpp (2.6.0 and upstream master) declares
# cmake_minimum_required(VERSION 3.5), which the CMake 4.x shipped with IDF v6.1
# flags as deprecated on every configure. Raising the policy floor to 3.10 makes
# any component that asks for less get the 3.6-3.10 policies as NEW
# (CMP0066-CMP0071: try_compile flags, macOS RPATH, IPO, file(GENERATE) paths,
# AUTOMOC). Today esp-nimble-cpp is the only such component, and it uses none of
# them. IDF adds managed components from its own build function, so this cannot
# be scoped to one component. The bootloader sub-build never includes it.
# Older CMake ignores this variable; a value from -D or the environment wins.
# Remove once esp-nimble-cpp requires CMake >= 3.10.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM AND NOT DEFINED ENV{CMAKE_POLICY_VERSION_MINIMUM})
    set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dfu32)
