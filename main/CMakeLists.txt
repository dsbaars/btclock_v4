idf_component_register(
    SRCS "main.cpp"
         "fonts_app.cpp" "boot_ui.cpp" "provisioning_ui.cpp"
         "app/frontlight_fader.cpp" "app/frontlight_controller.cpp"
         "app/led_controller.cpp" "app/led_curves.cpp"
         "app/wifi_guard.cpp"
         "app/time_sync.cpp" "app/screen_manager.cpp"
         "screens/common.cpp" "screens/screen_math.cpp"
         "screens/panel_texts.cpp"
         "screens/block_height.cpp"
         "screens/moscow_time.cpp" "screens/btc_price.cpp"
         "screens/fee_rate.cpp"
         "screens/clock.cpp" "screens/halving.cpp"
         "screens/bitcoin_supply.cpp" "screens/market_cap.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_psram esp_wifi i2c_bus mcp23017 pca9685
             bh1750 epd_ssd1680 fonts wifi prefs btclock_fs webserver qrcode
             data_core btclock_data buttons nostr timezone dnd
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

# BTClock hardware variant. Set via `idf.py build -DPOC_BOARD=REV_A|REV_B|V8`
# (top-level CMakeLists also reads this to pick the right sdkconfig.defaults
# and therefore the right partition table + flash size).
set(POC_BOARD "REV_B" CACHE STRING "BTClock hardware variant: REV_A, REV_B or V8")
if(POC_BOARD STREQUAL "REV_A")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POC_BOARD_REV_A=1)
elseif(POC_BOARD STREQUAL "REV_B")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POC_BOARD_REV_B=1)
elseif(POC_BOARD STREQUAL "V8")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE POC_BOARD_V8=1)
else()
    message(FATAL_ERROR "Unknown POC_BOARD='${POC_BOARD}'. Use REV_A, REV_B or V8.")
endif()
