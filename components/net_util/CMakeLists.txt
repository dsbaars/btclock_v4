# Header-only network-identity helpers shared between mDNS init and the
# /api/settings emitter. Lives in components/ (not main/) so the webserver
# component can include it without a circular dep on main.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format
)
target_compile_features(${COMPONENT_LIB} INTERFACE cxx_std_23)
