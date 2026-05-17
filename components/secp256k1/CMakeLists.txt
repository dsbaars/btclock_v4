# Vendored libsecp256k1 (bitcoin-core/secp256k1 v0.7.0).
#
# Built as an ESP-IDF component. We only need schnorr signature
# verification for Nostr (NIP-01 BIP-340) — signing, ECDH, recovery,
# musig and ellswift are excluded to keep flash + RAM down on the
# 4 MiB Rev A. ECMULT_WINDOW_SIZE is set to 4 (vs. upstream default
# 15) because verification frequency is < 1 Hz and the default 1 MiB
# precomputed table would not fit Rev A's app partition.
#
# Local patches:
#   - src/hash.h + src/hash_impl.h: secp256k1_sha256_* routed through
#     mbedtls so the ESP32-S3 hardware SHA peripheral does the work
#     (CONFIG_MBEDTLS_HARDWARE_SHA=y). Saves ~9 KiB code on every
#     variant. PRIV_REQUIRES mbedtls below makes the include path
#     visible to src/secp256k1.c.

idf_component_register(
    SRCS
        "src/secp256k1.c"
        "src/precomputed_ecmult.c"
        "src/precomputed_ecmult_gen.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES mbedtls
)

# Compile-time configuration. ENABLE_MODULE_EXTRAKEYS is implied by
# ENABLE_MODULE_SCHNORRSIG but we set it explicitly so the dependency
# is visible in the build log.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ENABLE_MODULE_EXTRAKEYS=1
    ENABLE_MODULE_SCHNORRSIG=1
    ECMULT_WINDOW_SIZE=4
)

# Upstream is intentionally warning-clean under its own flag set. The
# IDF flag set adds -Wshadow / -Wsign-compare / -Wconversion that the
# constant-time crypto code triggers extensively; suppress at the
# component level rather than the file level so future upstream pulls
# don't need patching.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -w
)
