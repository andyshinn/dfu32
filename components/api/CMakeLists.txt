idf_component_register(
    SRCS "src/api.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES config dfu32_core dfu_ble esp_app_format esp_http_server esp_timer fetch job mbedtls net runlog
                  staging
)
