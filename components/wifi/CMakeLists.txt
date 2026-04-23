idf_component_register(
    SRCS "wifi.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash
    PRIV_REQUIRES lwip
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
