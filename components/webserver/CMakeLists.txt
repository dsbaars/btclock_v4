# The real custom command to gzip portal.html has to live outside the
# early-expansion pass (add_custom_command isn't scriptable). The path
# below is referenced unconditionally so EMBED_FILES stays stable in
# both passes; the command that produces it only runs during the build.
set(_PORTAL_SRC "${CMAKE_CURRENT_LIST_DIR}/assets/portal.html")
set(_PORTAL_GZ "${CMAKE_CURRENT_BINARY_DIR}/portal.html.gz")

idf_component_register(
    SRCS "provisioning_server.cpp" "dns_hijack.cpp" "control_server.cpp"
         "mime.cpp" "sse_server.cpp" "sse_frame.cpp" "show_text_parse.cpp"
         "auth_gate.cpp" "auth_gate_logic.cpp" "url_decode.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server wifi prefs esp_wifi lwip esp_app_format
             data_core esp_psram cjson btclock_fs vfs settings ota
             app_update net_util epd pool_logo_fetcher
    EMBED_FILES "${_PORTAL_GZ}"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

# Bake the WebUI submodule's `git describe` into a compile definition so
# /api/settings can return `fsRev` alongside the firmware's `gitRev`.
# Mirrors how ESP-IDF stamps PROJECT_VER from `git describe`. Falls back
# to an empty string when `data/` isn't a git tree (CI tarball builds,
# fresh sparse checkouts), in which case the field is suppressed by the
# emit-only-if-non-empty rule in settings_api.cpp.
set(_WEBUI_REV "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/data/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/data"
        OUTPUT_VARIABLE _WEBUI_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WEBUI_FS_REV="${_WEBUI_REV}")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Serve portal with Content-Encoding: gzip — ~65 % flash saving vs raw.
    add_custom_command(
        OUTPUT "${_PORTAL_GZ}"
        COMMAND gzip -n -9 -c "${_PORTAL_SRC}" > "${_PORTAL_GZ}"
        DEPENDS "${_PORTAL_SRC}"
        COMMENT "Gzipping portal.html"
        VERBATIM
    )
    add_custom_target(portal_gz DEPENDS "${_PORTAL_GZ}")
    add_dependencies(${COMPONENT_LIB} portal_gz)
endif()
