# Each TTF is gzipped at configure time and embedded as `<name>.ttf.gz`,
# saving ~30% on the app partition (~46 KB across the full bundle). The
# Font ctor sniffs the gzip magic and decompresses into PSRAM at boot
# via miniz from ROM (no flash cost for the codec). Generation deps are
# tracked via CMAKE_CONFIGURE_DEPENDS so re-running cmake refreshes the
# .gz blobs whenever an upstream .ttf changes.
#
# Rev A's 4MB flash leaves no room for the Merriweather pair under
# IDF v6 — drop them on that board only. Selecting `merriweather` in
# the WebUI on a Rev A silently renders SourceSerif (handled in
# main/fonts_app.cpp); the WebUI bundle itself stays identical across
# boards.
set(_FONT_SOURCES
    Antonio.ttf
    AntonioSemiBold.ttf AntonioBold.ttf
    Oswald.ttf
    OswaldBold.ttf
    Inter.ttf InterBold.ttf
    OpenRunde.ttf OpenRundeBold.ttf
    SourceSerif.ttf SourceSerifBold.ttf
    Bitter.ttf BitterBold.ttf
    Atkinson.ttf AtkinsonBold.ttf
    SatoshiSymbol.ttf
    MaterialDesignIcons.ttf
)
if(NOT BTCLOCK_BOARD STREQUAL "REV_A")
    list(APPEND _FONT_SOURCES
        Merriweather.ttf MerriweatherBold.ttf)
endif()

# Write generated .gz blobs under the component dir (gitignored)
# rather than CMAKE_CURRENT_BINARY_DIR — IDF processes each component's
# CMakeLists multiple times during the requirements scan, and the
# binary dir is phase-dependent. CMAKE_CURRENT_LIST_DIR is the only
# path that stays stable across both phases.
set(_FONT_GZ_DIR "${CMAKE_CURRENT_LIST_DIR}/.gz_cache")
file(MAKE_DIRECTORY "${_FONT_GZ_DIR}")

# Compress at configure time. `gzip -n -9` strips name+timestamp so the
# gzip header is a fixed 10 bytes (FLG=0), which lets the runtime parser
# in font.cpp skip header parsing and feed raw deflate straight to
# tinfl_decompress_mem_to_mem.
set(_FONT_EMBEDS "")
foreach(_font IN LISTS _FONT_SOURCES)
    set(_in  "${CMAKE_CURRENT_LIST_DIR}/assets/${_font}")
    set(_out "${_FONT_GZ_DIR}/${_font}.gz")
    if(NOT EXISTS "${_out}" OR "${_in}" IS_NEWER_THAN "${_out}")
        execute_process(
            COMMAND gzip -n -9 -c "${_in}"
            OUTPUT_FILE "${_out}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_in}")
    list(APPEND _FONT_EMBEDS "${_out}")
endforeach()

idf_component_register(
    SRCS "font.cpp" "stb_truetype_impl.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES esp_psram esp_rom
    EMBED_FILES ${_FONT_EMBEDS}
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
