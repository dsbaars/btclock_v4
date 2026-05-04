idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_https_ota app_update mbedtls esp_partition
             esp_system esp-tls cjson
    PRIV_REQUIRES esp_psram esp_timer
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
