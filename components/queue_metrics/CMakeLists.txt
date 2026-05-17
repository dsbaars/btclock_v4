idf_component_register(
    SRCS "src/queue_metrics.cpp"
    INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
