idf_component_register(
    SRCS "hub.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
