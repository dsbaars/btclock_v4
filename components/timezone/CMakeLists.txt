# Timezone: IANA name -> POSIX TZ string lookup, plus a small wrapper
# around setenv("TZ", ...)/tzset() that persists the current zone in
# NVS. The lookup table is generated from the production firmware's
# include/timezone_data.hpp by tools/timezone/generate_tz_table.py; we
# commit the generated header so the IDF build has no Python step.
idf_component_register(
    SRCS "timezone.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES prefs settings
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
