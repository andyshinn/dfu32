idf_component_register(
    SRCS "src/staging.cpp" "src/staging_io.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dfu_proto dfu32_core esp_partition mbedtls
)
