idf_component_register(
    SRCS "font.cpp" "stb_truetype_impl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_psram
    EMBED_FILES "assets/Antonio.ttf" "assets/Oswald.ttf"
                "assets/OswaldBold.ttf"
                "assets/Inter.ttf" "assets/InterBold.ttf"
                "assets/SourceSerif.ttf" "assets/SourceSerifBold.ttf"
                "assets/Merriweather.ttf" "assets/MerriweatherBold.ttf"
                "assets/Bitter.ttf" "assets/BitterBold.ttf"
                "assets/Atkinson.ttf" "assets/AtkinsonBold.ttf"
                "assets/SatoshiSymbol.ttf"
                "assets/MaterialDesignIcons.ttf"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
