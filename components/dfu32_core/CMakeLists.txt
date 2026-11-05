# dfu32_core: pure C++ job logic (JSON, URLs, manifests, staging writer). No
# ESP-IDF dependencies, so it builds both as an IDF component and as a plain
# CMake library for host unit tests (test/host).
set(DFU32_CORE_SRCS
    src/errors.cpp
    src/json.cpp
    src/manifest.cpp
    src/stage_writer.cpp
    src/target_config.cpp
    src/url.cpp
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${DFU32_CORE_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES dfu_proto)
else()
    add_library(dfu32_core STATIC ${DFU32_CORE_SRCS})
    target_include_directories(dfu32_core PUBLIC include)
    target_link_libraries(dfu32_core PUBLIC dfu_proto)
    target_compile_features(dfu32_core PUBLIC cxx_std_20)
    target_compile_options(dfu32_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
