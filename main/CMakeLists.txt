idf_component_register(
    SRCS
        "console.cpp"
        "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        app_update
        console
        dfu_ble
        dfu_proto
        esp_app_format
        esp_partition
        esp_psram
        mbedtls
        nvs_flash
        staging
)
