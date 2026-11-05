# runlog: run records + captured log (SPEC.md §13). The pure parts build as a
# plain CMake library for host tests; the ESP-IDF glue (partition storage, log
# hook, job marker in NVS) is added only in the firmware build.
set(RUNLOG_PURE_SRCS
    src/log_ring.cpp
    src/run_log.cpp
    src/run_record.cpp
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${RUNLOG_PURE_SRCS} "src/runlog_idf.cpp"
                           INCLUDE_DIRS include
                           REQUIRES dfu_proto
                           PRIV_REQUIRES esp_partition nvs_flash esp_timer log heap)
else()
    add_library(runlog STATIC ${RUNLOG_PURE_SRCS})
    target_include_directories(runlog PUBLIC include)
    target_link_libraries(runlog PUBLIC dfu_proto)
    target_compile_features(runlog PUBLIC cxx_std_20)
    target_compile_options(runlog PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
