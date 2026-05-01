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
             app_update net_util epd pool_logo_fetcher espcoredump
             queue_metrics
    EMBED_FILES "${_PORTAL_GZ}"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

# Stamp the firmware's PROJECT_VER as `fsRev` so the WebUI bundle built
# alongside this firmware reports a matching value to `gitRev`. The
# previous source (`git describe` of the WebUI submodule) paced
# independently of the firmware repo, so the two values almost never
# lined up — every release built in the same CI workflow tripped the
# SystemInfo mismatch banner. PROJECT_VER is set in the project root
# CMakeLists from `git describe --tags --always --dirty --match
# "[0-9]*"` of the firmware repo; sourcing fsRev from the same value
# means a CI build of firmware + WebUI from one tree always reports
# fsRev == gitRev. Empty when PROJECT_VER itself is empty (sparse
# checkout / source tarball), in which case settings_api.cpp's
# emit-only-if-non-empty rule suppresses the field.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WEBUI_FS_REV="${PROJECT_VER}")

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
