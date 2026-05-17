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
             esp_rom
             queue_metrics
    EMBED_FILES "${_PORTAL_GZ}"
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)

# `fsRev` (the SystemInfo "WebUI commit" field) is sourced at runtime from
# /lfs/www/commit.txt — written by data/gzip_build.py from the WebUI
# submodule's git rev. See ctx.fs_rev in control_server.cpp.

# `gitTag` is non-empty only when the firmware repo's HEAD is exactly on a
# release tag matching the semver pattern (the same `[0-9]*` shape
# release.yaml triggers off). Dev / between-release builds emit an empty
# string and the WebUI hides the row. Note: CMake re-runs configure on
# git index changes, so a `git checkout <tag>` followed by a build will
# pick up the new tag — but a forced rebuild on the *same* worktree after
# tagging may need an explicit `idf.py reconfigure`.
set(_BTCLOCK_GIT_EXACT_TAG "")
if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
                    --match "[0-9]*" HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE _BTCLOCK_GIT_EXACT_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _BTCLOCK_GIT_EXACT_TAG_RC)
        if(NOT _BTCLOCK_GIT_EXACT_TAG_RC EQUAL 0)
            set(_BTCLOCK_GIT_EXACT_TAG "")
        endif()
    endif()
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BTCLOCK_GIT_TAG="${_BTCLOCK_GIT_EXACT_TAG}")

# `lastBuildTime` (the SystemInfo "Build time" row). Stamped here in UTC
# Unix seconds at CMake configure time and baked into control_server.cpp
# as a compile definition. v4-rc.12 onward replaces the older
# `__DATE__` + `__TIME__` parse path, which silently treated the
# compiler's local-wallclock stamp as UTC and so drifted by the build
# host's offset (a CEST build appeared two hours in the future to a
# CEST viewer once the WebUI re-localised it).
#
# Trade-off: `string(TIMESTAMP)` only re-evaluates on CMake re-configure,
# not on every `idf.py build`. CI release builds reconfigure from a
# clean tree so the stamp lands on the actual build moment; for local
# incremental rebuilds the stamp lags behind until the next reconfigure
# (touch CMakeLists.txt or run `idf.py reconfigure` to refresh).
string(TIMESTAMP _BTCLOCK_BUILD_UTC_SECONDS "%s" UTC)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BTCLOCK_BUILD_UTC_SECONDS=${_BTCLOCK_BUILD_UTC_SECONDS}LL)

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
