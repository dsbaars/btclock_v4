idf_component_register(
    SRCS "led_curves.cpp"
         "led_prefs.cpp"
    INCLUDE_DIRS "include"
    REQUIRES settings
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
