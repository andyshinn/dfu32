# indicator: status LEDs and the user button (SPEC.md §9.3, §13.4). The
# decision logic is pure and builds as a plain CMake library for host tests;
# the drivers (espressif/led_indicator on led_strip, espressif/button) are added
# only in the firmware build.
if(ESP_PLATFORM)
    idf_component_register(SRCS "src/logic.cpp" "src/indicator.cpp"
                           INCLUDE_DIRS include
                           REQUIRES dfu32_core
                           PRIV_REQUIRES esp_driver_gpio esp_timer job net)
else()
    add_library(indicator_logic STATIC src/logic.cpp)
    target_include_directories(indicator_logic PUBLIC include)
    target_link_libraries(indicator_logic PUBLIC dfu32_core)
    target_compile_features(indicator_logic PUBLIC cxx_std_20)
    target_compile_options(indicator_logic PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()
