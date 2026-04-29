idf_component_register(
    SRCS "btclock_data.cpp" "btclock_lastblock_uri.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls esp_http_client data_core
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
