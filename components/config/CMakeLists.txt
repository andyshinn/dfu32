idf_component_register(
    SRCS "src/config.cpp"
    INCLUDE_DIRS "include"
    REQUIRES dfu32_core
    PRIV_REQUIRES nvs_flash
)
