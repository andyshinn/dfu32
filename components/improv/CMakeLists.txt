# improv: Improv Wi-Fi provisioning (SPEC.md §9.2). The core (packet handling,
# state machine) and the vendored improv-wifi/sdk-cpp are pure C++ and build as
# a plain CMake library for host tests; the transports (Improv BLE over a
# NimBLE GATT server, Improv Serial on the console port) are firmware-only.
set(IMPROV_PURE_SRCS
    src/core.cpp
    src/serial_frame.cpp
    third_party/sdk-cpp/improv.cpp
)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${IMPROV_PURE_SRCS} "src/attempt.cpp" "src/improv_ble.cpp" "src/improv_serial.cpp"
                           INCLUDE_DIRS include
                           PRIV_INCLUDE_DIRS third_party/sdk-cpp
                           PRIV_REQUIRES bt esp_app_format esp_timer job net)
    # Upstream code, kept unmodified: silence what -Werror would reject there.
    set_source_files_properties(third_party/sdk-cpp/improv.cpp PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare")
else()
    add_library(improv_core STATIC ${IMPROV_PURE_SRCS})
    target_include_directories(improv_core PUBLIC include PRIVATE third_party/sdk-cpp)
    target_compile_features(improv_core PUBLIC cxx_std_20)
    target_compile_options(improv_core PRIVATE -Wall -Wextra -Werror)
    set_source_files_properties(third_party/sdk-cpp/improv.cpp PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare")
endif()
