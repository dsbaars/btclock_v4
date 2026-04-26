idf_component_register(
    SRCS "src/bitaxe_source.cpp" "src/bitaxe_parser.cpp"
    INCLUDE_DIRS "include"
    REQUIRES data_core tls_gate esp_http_client cjson prefs settings
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
