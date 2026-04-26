idf_component_register(
    SRCS "src/viabtc.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mining_pool_common data_core tls_gate esp_http_client cjson prefs settings
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
