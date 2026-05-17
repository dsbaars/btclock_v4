idf_component_register(
    SRCS "refresh_policy.cpp"
    INCLUDE_DIRS "include"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
