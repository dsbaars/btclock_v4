idf_component_register(
    SRCS "mcp23017.cpp"
    INCLUDE_DIRS "include"
    REQUIRES i2c_bus esp_driver_i2c
    PRIV_REQUIRES freertos
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
