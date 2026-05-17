idf_component_register(
    SRCS "pca9685.cpp"
    INCLUDE_DIRS "include"
    REQUIRES i2c_bus esp_driver_gpio esp_driver_i2c
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
