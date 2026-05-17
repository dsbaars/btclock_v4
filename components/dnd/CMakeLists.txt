idf_component_register(
    SRCS "dnd_window.cpp" "dnd.cpp"
    INCLUDE_DIRS "include"
    REQUIRES prefs settings
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
