idf_component_register(
    SRCS "src/job.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dfu32_core dfu_ble runlog staging
    PRIV_REQUIRES config fetch net esp_timer esp_system
)
