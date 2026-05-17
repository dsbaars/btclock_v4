# Shared base class + pure-logic helpers for mining-pool DataSources.
idf_component_register(
    SRCS "src/pool_base.cpp"
    INCLUDE_DIRS "include"
    REQUIRES data_core tls_gate esp_http_client esp-tls cjson
    PRIV_REQUIRES prefs settings proxy_transport
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
