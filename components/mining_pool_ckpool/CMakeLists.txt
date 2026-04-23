idf_component_register(
    SRCS "src/ckpool.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mining_pool_common data_core tls_gate esp_http_client json prefs
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
