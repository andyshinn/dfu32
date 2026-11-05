idf_component_register(
    SRCS "src/net.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES dfu32_core esp_wifi esp_netif esp_event esp_timer nvs_flash lwip
)
