# EPD HAL component. Single component, three+ drivers; the factory
# (src/factory.cpp) does compile-time dispatch via BTCLOCK_PANEL_<X>=1
# emitted by the top-level CMakeLists.txt.
#
# Every driver source is listed unconditionally so static analysis +
# regression sweeps see all three even when the build only links one;
# the factory is the only place that actually instantiates a concrete
# driver.

idf_component_register(
    SRCS "src/bus.cpp"
         "src/global.cpp"
         "src/factory.cpp"
         "src/drivers/ssd1680_base.cpp"
         "src/drivers/gdey0213b74.cpp"
         "src/drivers/gdey029t94.cpp"
         "src/drivers/uc8179_base.cpp"
         "src/drivers/gdey075t7.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver mcp23017
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
