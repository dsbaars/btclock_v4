# Settings: JSON schema + validator for /api/settings. Pure-logic core
# (settings_api.cpp) talks only to cJSON + the PrefsReader/PrefsWriter
# abstractions, so it drops straight into host tests without an IDF
# dependency. The NVS-backed adapter (include/settings/nvs_store.hpp)
# wraps btclock::Prefs for the on-device build.
idf_component_register(
    SRCS "settings_api.cpp" "factory_reset.cpp" "build_time.cpp"
         "nostr_config.cpp"
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES prefs nvs_flash
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
