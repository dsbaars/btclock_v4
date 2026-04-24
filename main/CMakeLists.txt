idf_component_register(
    SRCS "main.cpp"
         "fonts_app.cpp" "boot_ui.cpp" "provisioning_ui.cpp"
         "app/app_ctx.cpp"
         "app/boot/adapters.cpp"
         "app/boot/factory_reset.cpp"
         "app/boot/helpers.cpp"
         "app/boot/init_boot_leds.cpp"
         "app/boot/init_boot_path.cpp"
         "app/boot/init_control_api.cpp"
         "app/boot/init_hardware.cpp"
         "app/boot/init_mdns.cpp"
         "app/boot/init_network.cpp"
         "app/boot/init_panels.cpp"
         "app/boot/init_screen_manager.cpp"
         "app/boot/init_storage.cpp"
         "app/boot/init_zap_listener.cpp"
         "app/event_loop.cpp"
         "sources/sources.cpp"
         "io/frontlight_fader.cpp" "io/frontlight_controller.cpp"
         "io/frontlight_ambient_policy.cpp"
         "io/led_controller.cpp" "io/led_curves.cpp"
         "io/light_sensor.cpp"
         "io/wifi_guard.cpp"
         "io/mining_pool_selector.cpp"
         "app/time_sync.cpp" "app/screen_manager.cpp"
         "app/refresh_policy.cpp"
         "screens/common.cpp" "screens/screen_math.cpp"
         "screens/panel_texts.cpp"
         "screens/block_height.cpp"
         "screens/moscow_time.cpp" "screens/btc_price.cpp"
         "screens/fee_rate.cpp"
         "screens/clock.cpp" "screens/halving.cpp"
         "screens/bitcoin_supply.cpp" "screens/market_cap.cpp"
         "screens/mining_pool.cpp" "screens/bitaxe.cpp"
         "screens/assets/pool_logos.cpp"
         "screens/assets/bitaxe_logo.cpp"
         "screens/show_custom.cpp"
         "screens/nostr_zap.cpp"
         "screens/ota_update.cpp"
         "screens/debug.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_psram esp_wifi i2c_bus mcp23017 pca9685
             bh1750 epd_ssd1680 fonts wifi prefs btclock_fs webserver qrcode
             data_core btclock_data buttons nostr timezone dnd settings ota
             mining_pool_common mining_pool_braiins mining_pool_ckpool
             mining_pool_gobrrr mining_pool_noderunners mining_pool_ocean
             mining_pool_public_pool mining_pool_satoshi_radio
             bitaxe mdns net_util
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
