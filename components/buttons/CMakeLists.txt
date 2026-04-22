idf_component_register(
    SRCS "buttons.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mcp23017 driver
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
