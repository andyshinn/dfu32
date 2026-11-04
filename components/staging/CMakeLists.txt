idf_component_register(
    SRCS "src/staging.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dfu_proto esp_partition
    PRIV_REQUIRES mbedtls
)
