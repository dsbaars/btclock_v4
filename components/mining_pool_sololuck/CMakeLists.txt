idf_component_register(
    SRCS "src/sololuck.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mining_pool_common data_core tls_gate esp_http_client cjson prefs
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
