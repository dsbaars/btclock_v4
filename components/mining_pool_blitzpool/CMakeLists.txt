idf_component_register(
    SRCS "src/blitzpool.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mining_pool_common mining_pool_public_pool data_core tls_gate
             esp_http_client cjson prefs
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
