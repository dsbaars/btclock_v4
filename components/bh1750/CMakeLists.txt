idf_component_register(
    SRCS "bh1750.cpp"
    INCLUDE_DIRS "include"
    REQUIRES i2c_bus
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
