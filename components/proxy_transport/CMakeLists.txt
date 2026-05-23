# Proxy transport — wraps a TCP socket with SOCKS4/4a/5 or HTTP CONNECT
# handshake plus optional TLS. Pure framing in proxy_framing.cpp /
# proxy_bypass.cpp is host-testable; the rest pulls in lwIP + esp-tls
# and is firmware-only.
idf_component_register(
    SRCS
        "src/proxy_framing.cpp"
        "src/proxy_bypass.cpp"
        "src/proxy_socket_io.cpp"
        "src/proxy_handshake.cpp"
        "src/proxy_transport.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES tcp_transport esp-tls settings
    PRIV_REQUIRES esp_timer log lwip mbedtls  # proxy_framing.cpp routes Base64Encode through mbedtls_base64_encode
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
