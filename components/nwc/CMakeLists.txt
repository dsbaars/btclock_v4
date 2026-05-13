idf_component_register(
    SRCS
        "src/uri.cpp"
        "src/jsonrpc.cpp"
        "src/client.cpp"
        "src/queue.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nostr secp256k1
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
