idf_component_register(
    SRCS
        "src/parser.cpp"
        "src/request_frame.cpp"
        "src/relay_client.cpp"
        "src/subscription_manager.cpp"
        "src/zap_listener.cpp"
        "src/nostr_data_source.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls data_core
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
