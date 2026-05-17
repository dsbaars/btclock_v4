idf_component_register(
    SRCS "littlefs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_partition
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
