idf_component_register(
    SRCS
        "src/uri.cpp"
        "src/jsonrpc.cpp"
        "src/client.cpp"
        "src/queue.cpp"
        "src/debug.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nostr cjson secp256k1
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
