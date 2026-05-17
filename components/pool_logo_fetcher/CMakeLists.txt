# Background fetcher that downloads mining-pool logos from the
# upstream `mining-pool-logos` repo into LittleFS on demand. Replaces
# the v4 "everything baked into .text" approach (bd btclock_v4-5yi).
#
# Depends on `prefs` for the `poolLogosUrl` setting, `tls_gate` to
# serialise the mbedtls handshake with the other HTTPS pollers, and
# `esp_http_client` + `esp-tls` for the actual transport. The cache
# resolver + LookupMeta symbols live in main/ — the linker resolves
# them at the final exec link step (declared via forward decls in
# the .cpp).
idf_component_register(
    SRCS "src/pool_logo_fetcher.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls tls_gate
    PRIV_REQUIRES prefs settings esp_psram proxy_transport
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
