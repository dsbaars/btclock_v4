# Rev A's 4MB flash leaves no room for the Merriweather pair under
# IDF v6 — drop them from EMBED_FILES on that board only. Selecting
# `merriweather` in the WebUI on a Rev A silently renders SourceSerif
# (handled in main/fonts_app.cpp); the WebUI bundle itself stays
# identical across boards.
set(_FONT_EMBEDS
    "assets/Antonio.ttf"
    "assets/AntonioSemiBold.ttf" "assets/AntonioBold.ttf"
    "assets/Oswald.ttf"
    "assets/OswaldBold.ttf"
    "assets/Inter.ttf" "assets/InterBold.ttf"
    "assets/SourceSerif.ttf" "assets/SourceSerifBold.ttf"
    "assets/Bitter.ttf" "assets/BitterBold.ttf"
    "assets/Atkinson.ttf" "assets/AtkinsonBold.ttf"
    "assets/SatoshiSymbol.ttf"
    "assets/MaterialDesignIcons.ttf"
)
if(NOT BTCLOCK_BOARD STREQUAL "REV_A")
    list(APPEND _FONT_EMBEDS
        "assets/Merriweather.ttf" "assets/MerriweatherBold.ttf")
endif()

idf_component_register(
    SRCS "font.cpp" "stb_truetype_impl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_psram
    EMBED_FILES ${_FONT_EMBEDS}
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
