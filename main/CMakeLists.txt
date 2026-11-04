idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        app_update
        dfu_proto
        esp_app_format
        esp_partition
        esp_psram
        nvs_flash
)
