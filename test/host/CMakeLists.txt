# Host unit tests for pure-C++ components. Run from the repo root:
#   cmake -S test/host -B build-host -G Ninja && cmake --build build-host && ctest --test-dir build-host --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(dfu32_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

set(DFU32_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_subdirectory(${DFU32_ROOT}/components/dfu_proto ${CMAKE_BINARY_DIR}/dfu_proto)

add_executable(dfu_proto_tests
    test_bootloader_info.cpp
    test_chunk.cpp
    test_crc16.cpp
    test_image.cpp
    test_init_packet.cpp
    test_protocol.cpp
    test_staging_format.cpp
    test_transfer.cpp
)
target_link_libraries(dfu_proto_tests PRIVATE dfu_proto Catch2::Catch2WithMain)
target_compile_options(dfu_proto_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(dfu_proto_tests PRIVATE
    DFU32_FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")

include(CTest)
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(dfu_proto_tests)
