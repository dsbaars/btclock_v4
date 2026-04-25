idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
             i2c_bus mcp23017 pca9685 epd
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
