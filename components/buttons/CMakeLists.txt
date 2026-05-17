idf_component_register(
    SRCS "buttons.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mcp23017 queue_metrics
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
