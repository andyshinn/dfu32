idf_component_register(
    SRCS "src/dfu_ble.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dfu_proto
    PRIV_REQUIRES bt esp_timer
)
