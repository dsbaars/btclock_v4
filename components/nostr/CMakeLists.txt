idf_component_register(
    SRCS
        "src/parser.cpp"
        "src/request_frame.cpp"
        "src/relay_client.cpp"
        "src/subscription_manager.cpp"
        "src/zap_listener.cpp"
        "src/event_verify.cpp"
        "src/event_sign.cpp"
        "src/nip4x.cpp"
        "src/nostr_data_source.cpp"
        "src/json_emit.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client esp-tls data_core cjson secp256k1
             tcp_transport proxy_transport prefs settings net_util
    PRIV_REQUIRES mbedtls  # nip4x.cpp routes AES-256-CBC through the HW AES peripheral
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
