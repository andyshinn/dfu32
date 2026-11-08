# DFU32: ESP-IDF project root.
#
# Board profiles live in boards/<name>/sdkconfig.defaults. Each profile sets
# CONFIG_IDF_TARGET, flash size, partition table, PSRAM mode, console and pins,
# so do NOT run `idf.py set-target`; pick the profile's preset instead:
#
#   idf.py --preset feather_esp32s3 build
#
# Each preset in CMakePresets.json sets DFU32_BOARD and its own build directory
# (build/<name>), and the generated sdkconfig lives inside that directory, so
# profiles never clobber each other. tools/check_presets.py keeps the presets
# and boards/ in sync; tools/build_all.py builds every preset.
#
# -DDFU32_DEBUG_ENDPOINTS=ON adds sdkconfig.debug (the fault-injection
# endpoints the bench runner needs for its safety checks). Give it its own
# build directory: defaults only apply when the sdkconfig is first generated.
#
#   idf.py --preset feather_esp32s3 -B build/feather_esp32s3-debug -DDFU32_DEBUG_ENDPOINTS=ON build
cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED CACHE{DFU32_BOARD})
    message(WARNING "DFU32_BOARD not set; defaulting to feather_esp32s3. "
                    "Build with `idf.py --preset <board>` (see CMakePresets.json).")
endif()
set(DFU32_BOARD "feather_esp32s3" CACHE STRING "DFU32 board profile (directory name under boards/)")
set(_dfu32_board_defaults "${CMAKE_CURRENT_LIST_DIR}/boards/${DFU32_BOARD}/sdkconfig.defaults")
if(NOT EXISTS "${_dfu32_board_defaults}")
    message(FATAL_ERROR "Unknown DFU32_BOARD '${DFU32_BOARD}': ${_dfu32_board_defaults} not found")
endif()
# A build directory named after another board (e.g. `idf.py -B build/<board>`
# without --preset, which picks the default preset) would quietly get the
# wrong profile; stop instead.
get_filename_component(_dfu32_build_name "${CMAKE_BINARY_DIR}" NAME)
if(NOT _dfu32_build_name STREQUAL DFU32_BOARD
   AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/boards/${_dfu32_build_name}/sdkconfig.defaults")
    message(FATAL_ERROR "Build directory ${CMAKE_BINARY_DIR} is for board '${_dfu32_build_name}' but "
                        "DFU32_BOARD is '${DFU32_BOARD}'. Use `idf.py --preset ${_dfu32_build_name}`.")
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

# One Component Manager lock per chip. A single dependencies.lock records the
# target it was solved for, so building an ESP32 profile after an ESP32-S3 one
# would rewrite it. Commit every dependencies.lock.<target> file.
idf_build_set_property(DEPENDENCIES_LOCK "${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.${IDF_TARGET}")

project(dfu32)
