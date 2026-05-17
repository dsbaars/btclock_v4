idf_component_register(
    SRCS "prefs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
