# dfu_proto: pure C++ legacy-DFU protocol logic. No ESP-IDF or NimBLE
# dependencies, so it builds both as an IDF component and as a plain CMake
# library for host unit tests (test/host).
set(DFU_PROTO_SRCS
    src/crc16.cpp
    src/image.cpp
    src/init_packet.cpp
    src/protocol.cpp
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${DFU_PROTO_SRCS}
                           INCLUDE_DIRS include)
else()
    add_library(dfu_proto STATIC ${DFU_PROTO_SRCS})
    target_include_directories(dfu_proto PUBLIC include)
    target_compile_features(dfu_proto PUBLIC cxx_std_20)
    target_compile_options(dfu_proto PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
