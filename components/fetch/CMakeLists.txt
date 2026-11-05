idf_component_register(
    SRCS "src/fetch.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dfu32_core dfu_proto
    PRIV_REQUIRES esp_http_client esp_app_format esp_timer staging
)
