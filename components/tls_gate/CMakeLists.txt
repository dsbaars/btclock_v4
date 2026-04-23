# Header-only TLS handshake mutex. See include/tls_gate/tls_gate.hpp
# for the rationale (heap fragmentation under concurrent handshakes).
idf_component_register(
    INCLUDE_DIRS "include"
)
target_compile_features(${COMPONENT_LIB} INTERFACE cxx_std_17)
