idf_component_register(
    SRCS "btclock_data.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls data_core
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
