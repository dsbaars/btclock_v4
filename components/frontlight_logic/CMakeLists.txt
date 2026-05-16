idf_component_register(
    SRCS "frontlight_fader.cpp"
         "frontlight_ambient_policy.cpp"
         "frontlight_stagger.cpp"
    INCLUDE_DIRS "include"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
