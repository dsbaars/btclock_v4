idf_component_register(
    SRCS "epd_ssd1680.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver mcp23017
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
