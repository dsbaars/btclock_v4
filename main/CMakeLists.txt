idf_component_register(
    SRCS "main.cpp"
         "fonts_app.cpp" "boot_ui.cpp" "boot_spinner.cpp" "provisioning_ui.cpp"
         "app/app_ctx.cpp"
         "app/boot/adapters.cpp"
         "app/boot/factory_reset.cpp"
         "app/boot/helpers.cpp"
         "app/boot/init_boot_leds.cpp"
         "app/boot/init_boot_path.cpp"
         "app/boot/init_cjson_psram.cpp"
         "app/boot/init_control_api.cpp"
         "app/boot/init_mbedtls_psram.cpp"
         "app/boot/init_hardware.cpp"
         "app/boot/init_mdns.cpp"
         "app/boot/init_network.cpp"
         "app/boot/init_panels.cpp"
         "app/boot/init_screen_manager.cpp"
         "app/boot/init_storage.cpp"
         "app/boot/init_wifi_reset_button.cpp"
         "app/boot/init_zap_listener.cpp"
         "app/event_loop.cpp"
         "app/network_coordinator.cpp"
         "app/control_command_drain.cpp"
         "diag/heap_integrity.cpp"
         "diag/udp_log.cpp"
         "sources/sources.cpp"
         "sources/sources_uri.cpp"
         "sources/mempool_kraken_source.cpp"
         "io/frontlight_controller.cpp"
         "io/led_controller.cpp"
         "io/light_sensor.cpp"
         "io/network_led_watchdog.cpp"
         "io/wifi_guard.cpp"
         "io/mining_pool_selector.cpp"
         "app/time_sync.cpp" "app/screen_manager.cpp"
         "screens/common.cpp" "screens/screen_math.cpp"
         "screens/panel_texts.cpp"
         "screens/block_height.cpp"
         "screens/moscow_time.cpp" "screens/btc_price.cpp"
         "screens/fee_rate.cpp"
         "screens/clock.cpp" "screens/halving.cpp"
         "screens/bitcoin_supply.cpp" "screens/market_cap.cpp"
         "screens/mining_pool.cpp" "screens/bitaxe.cpp"
         "screens/assets/pool_logos.cpp"
         "screens/assets/pool_logo_cache.cpp"
         "screens/assets/bitaxe_logo.cpp"
         "screens/show_custom.cpp"
         "screens/nostr_zap.cpp"
         "screens/nwc_balance.cpp"
         "screens/ota_update.cpp"
         "screens/debug.cpp"
         "app/boot/init_nwc.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_ledc esp_timer esp_psram esp_wifi
             i2c_bus mcp23017 pca9685
             bh1750 epd fonts wifi prefs btclock_fs webserver qrcode
             btclock_format frontlight_logic led_strip_logic refresh_policy
             data_core btclock_data buttons nostr nwc timezone dnd settings ota
             mining_pool_common mining_pool_blitzpool mining_pool_braiins
             mining_pool_ckpool
             mining_pool_foundry mining_pool_nerdminer
             mining_pool_noderunners mining_pool_ocean
             mining_pool_public_pool mining_pool_satoshi_radio
             mining_pool_viabtc
             pool_logo_fetcher
             bitaxe mdns net_util esp_websocket_client esp-tls cjson
             espcoredump queue_metrics tcp_transport proxy_transport lwip
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)

# BTClock hardware variant. Set via
#   `idf.py build -DBTCLOCK_BOARD=REV_A|REV_B|V8 [-DBTCLOCK_PANEL=2_13|2_9|7_5]`
# (top-level CMakeLists also reads this to pick the right sdkconfig.defaults
# and therefore the right partition table + flash size). Panel selection
# (BTCLOCK_PANEL_* compile def) is emitted from the top-level
# CMakeLists.txt so every component sees the same flag.
set(BTCLOCK_BOARD "REV_B" CACHE STRING "BTClock hardware variant: REV_A, REV_B or V8")
if(BTCLOCK_BOARD STREQUAL "REV_A")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BTCLOCK_BOARD_REV_A=1)
elseif(BTCLOCK_BOARD STREQUAL "REV_B")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BTCLOCK_BOARD_REV_B=1)
elseif(BTCLOCK_BOARD STREQUAL "V8")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BTCLOCK_BOARD_V8=1)
else()
    message(FATAL_ERROR "Unknown BTCLOCK_BOARD='${BTCLOCK_BOARD}'. Use REV_A, REV_B or V8.")
endif()
