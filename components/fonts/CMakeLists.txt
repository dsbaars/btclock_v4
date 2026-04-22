idf_component_register(
    SRCS "font.cpp" "stb_truetype_impl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_psram
    EMBED_FILES "assets/Antonio.ttf" "assets/Oswald.ttf"
                "assets/OswaldBold.ttf"
                "assets/DejaVu.ttf" "assets/DejaVuBold.ttf"
                "assets/SatoshiSymbol.ttf"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
