idf_component_register(
    SRCS "src/net.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash lwip
)
