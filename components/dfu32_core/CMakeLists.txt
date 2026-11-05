# dfu32_core: pure C++ job logic (JSON, URLs, manifests, staging writer). No
# ESP-IDF dependencies, so it builds both as an IDF component and as a plain
# CMake library for host unit tests (test/host). JSON parsing uses cJSON:
# espressif/cjson in the firmware (idf_component.yml), the same upstream
# release fetched here for host builds.
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
    include(FetchContent)
    FetchContent_Declare(
        cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG v1.7.19
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR do-not-add  # only fetch; cJSON.c is built below
    )
    FetchContent_MakeAvailable(cjson)
    add_library(dfu32_cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
    target_include_directories(dfu32_cjson PUBLIC ${cjson_SOURCE_DIR})
    # Must match json::kNestingLimit (firmware: CONFIG_CJSON_NESTING_LIMIT).
    target_compile_definitions(dfu32_cjson PUBLIC CJSON_NESTING_LIMIT=16)
    set_target_properties(dfu32_cjson PROPERTIES C_STANDARD 99)

    add_library(dfu32_core STATIC ${DFU32_CORE_SRCS})
    target_include_directories(dfu32_core PUBLIC include)
    target_link_libraries(dfu32_core PUBLIC dfu_proto PRIVATE dfu32_cjson)
    target_compile_features(dfu32_core PUBLIC cxx_std_20)
    target_compile_options(dfu32_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
