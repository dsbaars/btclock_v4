idf_component_register(
    SRCS "ota_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_https_ota app_update mbedtls esp_partition
             esp_system esp-tls json tls_gate
    PRIV_REQUIRES esp_psram
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
