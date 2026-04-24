// BTClock ESP-IDF C++ PoC — orchestrator.
//
// app_main is wire-up only: bring up hardware, load fonts, pick a boot
// path (provisioning vs STA), start the long-running services, and run
// the event loop. Each subsystem has its own file:
//
//   app/led_controller  — NeoPixel task + event queue
//   app/wifi_guard      — block until STA has an IP
//   app/time_sync       — SNTP init
//   app/screen_manager  — screen rotation + render dispatch
//   screens/            — per-screen renderers
//   board/              — per-variant pin map, selected by -DPOC_BOARD
//
// Board variant comes from -DPOC_BOARD=REV_A|REV_B|V8 at build time.

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/catalogs.hpp"
#include "app/frontlight_controller.hpp"
#include "app/led_controller.hpp"
#include "app/light_sensor.hpp"
#include "app/mining_pool_selector.hpp"
#include "bitaxe/bitaxe_source.hpp"
#include "app/screen_manager.hpp"
#include "app/screen_slot_map.hpp"
#include "app/time_sync.hpp"
#include "app/wifi_guard.hpp"
#include "bh1750.hpp"
#include "board/board.hpp"
#include "boot_ui.hpp"
#include "btclock_data.hpp"
#include "buttons.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
#include "sse_server.hpp"
#include "dnd/dnd.hpp"
#include "epd_ssd1680.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fonts_app.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "i2c_bus.hpp"
#include "littlefs.hpp"
#include "lwip/inet.h"
#include "mcp23017.hpp"
#include "net_util.hpp"
#include "nostr/nostr_data_source.hpp"
#include "ota_manager.hpp"
#include "settings/factory_reset.hpp"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"
#include "nostr/relay_client.hpp"
#include "nostr/subscription_manager.hpp"
#include "nostr/zap_listener.hpp"
#include "pca9685.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "provisioning_server.hpp"
#include "provisioning_ui.hpp"
#include "sdkconfig.h"
#include "timezone/timezone.hpp"
#include "wifi.hpp"

namespace {
constexpr const char* kTag = "poc";

int64_t MsNow() { return esp_timer_get_time() / 1000; }

// AP-mode credential helpers. Pure logic is in net_util.hpp; these wrap
// the ESP-IDF bits (MAC read, RNG, NVS).
std::string MakeApSsid() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  return btclock::FormatApSsid(mac);
}

std::string MakeOrLoadApPassword(btclock::Prefs& prefs) {
  std::string pw = prefs.GetString("app", "");
  if (pw.size() >= 8) return pw;
  pw = btclock::GenerateApPassword([] { return esp_random(); });
  prefs.SetString("app", pw.c_str());
  prefs.Commit();
  return pw;
}

#ifdef CONFIG_BTCLOCK_LITTLEFS_SELFTEST
// Smoke test the mounted LittleFS partition: write a known blob, read
// it back, assert equality, then delete the file. Logs and swallows
// errors so a degraded FS never bricks boot. Guarded by
// CONFIG_BTCLOCK_LITTLEFS_SELFTEST (Kconfig, default off).
void RunLittleFsSelfTest(const char* base_path) {
  const std::string path = std::string(base_path) + "/_bq0_selftest.txt";
  constexpr const char kPayload[] = "btclock-lfs-selftest-v1";

  FILE* wf = std::fopen(path.c_str(), "w");
  if (!wf) {
    ESP_LOGE(kTag, "selftest: fopen(w) '%s' failed", path.c_str());
    return;
  }
  const size_t wn = std::fwrite(kPayload, 1, sizeof(kPayload) - 1, wf);
  std::fclose(wf);
  if (wn != sizeof(kPayload) - 1) {
    ESP_LOGE(kTag, "selftest: short write %u/%u", static_cast<unsigned>(wn),
             static_cast<unsigned>(sizeof(kPayload) - 1));
    std::remove(path.c_str());
    return;
  }

  char buf[sizeof(kPayload)] = {};
  FILE* rf = std::fopen(path.c_str(), "r");
  if (!rf) {
    ESP_LOGE(kTag, "selftest: fopen(r) '%s' failed", path.c_str());
    std::remove(path.c_str());
    return;
  }
  const size_t rn = std::fread(buf, 1, sizeof(buf) - 1, rf);
  std::fclose(rf);

  const bool ok = (rn == sizeof(kPayload) - 1) &&
                  (std::memcmp(buf, kPayload, rn) == 0);
  if (ok) {
    ESP_LOGI(kTag, "selftest: OK (%u bytes round-tripped)",
             static_cast<unsigned>(rn));
  } else {
    ESP_LOGE(kTag, "selftest: MISMATCH rn=%u buf='%.*s'",
             static_cast<unsigned>(rn), static_cast<int>(rn), buf);
  }

  if (std::remove(path.c_str()) != 0) {
    ESP_LOGW(kTag, "selftest: remove '%s' failed", path.c_str());
  }
}
#endif  // CONFIG_BTCLOCK_LITTLEFS_SELFTEST

}  // namespace

extern "C" void app_main() {
  using namespace btclock::board;

  ESP_LOGI(kTag, "BTClock IDF C++ PoC — boot");
  // Label "heap" as the internal free bytes specifically — the default
  // allocator can pull from PSRAM on S3, which made the old print
  // report free > size when compared against the internal-only total
  // the status endpoints publish.
  ESP_LOGI(kTag, "psram=%uB heap_internal_free=%uB",
           static_cast<unsigned>(esp_psram_get_size()),
           static_cast<unsigned>(
               heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

  btclock::InitLeds(kNeopixel, kNeopixelCount);
  // DND predicate goes in before the first effect post so the boot
  // rainbow is gated too if the user has DND currently armed.
  btclock::SetLedActiveSuppressor(
      [] { return btclock::dnd::Instance().IsActive(); });
  btclock::PostLedEvent(btclock::LedEvent::kSetBoot);

  // --- I2C bus + peripherals ---
  btclock::I2cBus i2c(I2C_NUM_0, kI2cSda, kI2cScl);

  // V8: drive the 6 MCP address-strap GPIOs BEFORE releasing RESET,
  // otherwise both MCP23017s latch to the default 000 → 0x20 and
  // collide on the I2C bus (symptom: bus scan finds one device only).
  // Matches the old Arduino firmware's setupMcp().
  if constexpr (kHasMcpAddressGpios) {
    uint64_t mask = 0;
    for (gpio_num_t pin : kMcpAddressGpios) {
      mask |= 1ULL << static_cast<int>(pin);
    }
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    for (size_t i = 0; i < kMcpAddressGpios.size(); ++i) {
      gpio_set_level(kMcpAddressGpios[i], kMcpAddressLevels[i] ? 1 : 0);
    }
  }

  // V8: pulse the shared MCP RESET line once before any I2C transaction.
  // >= 5 µs low is the datasheet minimum; 5 ms is conservative. Address
  // straps set just above must be stable before this pulse.
  if constexpr (kHasMcpResetGpio) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << static_cast<int>(kMcpResetGpio);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(kMcpResetGpio, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(kMcpResetGpio, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  btclock::Mcp23017 mcp(i2c, kMcp1Addr);
  ESP_ERROR_CHECK(mcp.SetDirectionPort(0xFFFF));
  // Buttons on pins 0..3 for every variant so far.
  for (uint8_t p = 0; p < 4; ++p) {
    ESP_ERROR_CHECK(
        mcp.SetDirection(p, btclock::Mcp23017::PinMode::kInputPullup));
  }
  // Rev A/B: RESET on pins 8..14, preload high for a known-good state.
  // V8 has RESET on MCP2, so skip.
  if constexpr (!kHasSecondMcp) {
    ESP_ERROR_CHECK(mcp.WritePort(0xFF00));
  }

  std::optional<btclock::Mcp23017> mcp2;
  if constexpr (kHasSecondMcp) {
    mcp2.emplace(i2c, kMcp2Addr);
    ESP_ERROR_CHECK(mcp2->SetDirectionPort(0x0000));
    ESP_ERROR_CHECK(mcp2->WritePort(0xFFFF));
  }

  std::optional<btclock::Pca9685> pca;
  std::unique_ptr<btclock::FrontlightController> frontlight;
  if constexpr (kHasFrontlight) {
    pca.emplace(i2c, kPcaAddr, kPcaOe);
    ESP_ERROR_CHECK(pca->Begin(1000));
    pca->SetOutputEnable(true);
    frontlight = std::make_unique<btclock::FrontlightController>(
        *pca, kFrontlightChannelFirst, kFrontlightChannelCount);
    // Install DND gate before Start() so the boot fade-in is
    // suppressed when a schedule is already active at power-up.
    frontlight->SetActiveSuppressor(
        [] { return btclock::dnd::Instance().IsActive(); });
    frontlight->Start();
    // Fade up to the configured brightness at boot. Matches old
    // firmware, which powered the frontlight on once the panels were
    // initialised.
    frontlight->On();
  }

  std::optional<btclock::Bh1750> bh;
  std::unique_ptr<btclock::LightSensor> light_sensor;
  if constexpr (kHasAmbientLight) {
    bh.emplace(i2c, kBhAddr);
    // Init() probes the bus, so a depopulated/absent sensor reports
    // ESP_ERR_NOT_FOUND rather than aborting the boot — the downstream
    // LightSensor manager then stays in IsAvailable()==false mode and
    // /api/settings suppresses the lightLevel field.
    const esp_err_t ierr = bh->Init();
    if (ierr != ESP_OK) {
      ESP_LOGW(kTag, "BH1750 init failed: %s — continuing without ambient",
               esp_err_to_name(ierr));
    }
    light_sensor = std::make_unique<btclock::LightSensor>(*bh);
    light_sensor->Start();
  }

  // --- SSD1680 bus + panels ---
  auto make_pin = [&](btclock::board::PinSource src,
                      int pin_or_index) -> btclock::EpdIoPin {
    // `PS` is an Xtensa register macro — don't use it as an alias.
    using Src = btclock::board::PinSource;
    switch (src) {
      case Src::kNative:
        return btclock::EpdIoPin::Native(
            static_cast<gpio_num_t>(pin_or_index));
      case Src::kMcp1:
        return btclock::EpdIoPin::Mcp(&mcp,
                                       static_cast<uint8_t>(pin_or_index));
      case Src::kMcp2:
        return btclock::EpdIoPin::Mcp(mcp2 ? &*mcp2 : nullptr,
                                       static_cast<uint8_t>(pin_or_index));
    }
    return {};
  };

  btclock::EpdBus epd_bus(SPI2_HOST, kEpdSpiSclk, kEpdSpiMosi, kEpdDc,
                          4 * 1000 * 1000, 16 * 296 + 64);
  std::array<std::unique_ptr<btclock::EpdPanel>, kNumPanels> panels;
  for (int i = 0; i < kNumPanels; ++i) {
    btclock::EpdPanel::Config cfg = {};
    cfg.bus = &epd_bus;
    cfg.cs = make_pin(kEpdCsSource, kEpdCs[i]);
    cfg.busy = make_pin(kEpdBusySource, kEpdBusy[i]);
    cfg.reset = make_pin(kEpdResetSource, kEpdResetMcp[i]);
    cfg.kind = btclock::PanelKind::k2_13;
    panels[i] = std::make_unique<btclock::EpdPanel>(cfg);
  }
  for (auto& p : panels) ESP_ERROR_CHECK(p->Init());

  btclock::AppFonts fonts;
  static uint8_t fb_storage[kNumPanels][16 * 296];

  // --- Boot splash — letter-per-panel BTCLOCK[!]. ---
  {
    const int64_t t0 = MsNow();
    btclock::RenderSplashScreen(panels, fb_storage, fonts);
    ESP_LOGI(kTag, "splash pass %lld ms", MsNow() - t0);
  }

  // --- WiFi + NVS + optional provisioning portal ---
  ESP_ERROR_CHECK(btclock::Prefs::InitOnce());

  // Install the EPD polarity flag from NVS before the first data-driven
  // render. Boot splash above already painted (non-inverted) — that's
  // intentional: a corrupted NVS shouldn't leave a user staring at a
  // black screen. The first post-provisioning paint will honour the
  // stored value. Default `true` matches BuildGetResponse's shipping
  // default (white-on-black for the black-hardware majority).
  {
    btclock::Prefs p(btclock::prefs::kSettingsNs);
    btclock::EpdSetGlobalInverted(
        p.GetBool(btclock::prefs::kInvertedColor, true));
  }

  // Set the process-wide TZ from NVS (namespace "time", key "tz")
  // before anything that calls localtime_r. The clock screen, log
  // timestamps, and any future scheduling code all rely on it being
  // set; if the stored value is missing or unknown we fall back to
  // UTC and log the reason. setenv/tzset don't need the network.
  //
  // TODO(beads): wire /api/settings write-back so the WebUI can
  // change the zone at runtime — that lives in the jwz epic, not
  // here. This call only restores whatever's already in NVS.
  btclock::timezone::InitFromNvs();

  // LittleFS is used for the future static-WebUI bundle and OTA-webui
  // uploads. We format-on-failure so a blank partition (fresh flash,
  // first boot after partition-table change) self-heals. A mount error
  // after that fallback is logged but non-fatal — the firmware should
  // continue to boot without a filesystem rather than brick.
  {
    const esp_err_t lfs_err =
        btclock::MountLittleFs(btclock::kLittleFsDefaultBasePath);
    if (lfs_err != ESP_OK) {
      ESP_LOGE(kTag, "LittleFS mount failed (%s); continuing without FS",
               esp_err_to_name(lfs_err));
    }
#ifdef CONFIG_BTCLOCK_LITTLEFS_SELFTEST
    if (lfs_err == ESP_OK) {
      RunLittleFsSelfTest(btclock::kLittleFsDefaultBasePath);
    }
#endif
  }

  btclock::Prefs net_prefs("net");
  const std::string ssid = net_prefs.GetString("ssid", CONFIG_POC_WIFI_SSID);
  const std::string pw = net_prefs.GetString("pw", CONFIG_POC_WIFI_PASSWORD);

  btclock::Wifi wifi;
  std::unique_ptr<btclock::ProvisioningServer> portal;
  std::unique_ptr<btclock::DnsHijack> dns;
  std::string ap_ssid;
  std::string ap_pw;

  // Soft watchdog for multi-minute STA outages. Loaded here so the
  // setting applies even before the main loop starts pumping ticks.
  // Default 10 min matches Arduino main.cpp::checkWiFiConnection();
  // range clamp mirrors the schema (0 disables, ≤ 120).
  uint32_t wifi_reboot_minutes = 10;
  {
    btclock::Prefs settings_for_wifi(btclock::prefs::kSettingsNs);
    wifi_reboot_minutes =
        settings_for_wifi.GetU32(btclock::prefs::kWifiRebootMin, 10);
    if (wifi_reboot_minutes > 120) wifi_reboot_minutes = 120;
  }
  btclock::OutageWatchdog outage_watchdog(wifi_reboot_minutes);

  if (!ssid.empty()) {
    ESP_ERROR_CHECK(wifi.Start());
    ESP_ERROR_CHECK(wifi.Connect(ssid.c_str(), pw.c_str()));
    btclock::WaitForConnected(wifi, net_prefs);
    ESP_ERROR_CHECK(btclock::StartSntpSync());
  } else {
    ap_ssid = MakeApSsid();
    ap_pw = MakeOrLoadApPassword(net_prefs);
    ESP_LOGW(kTag, "provisioning mode: SoftAP '%s' pw='%s'", ap_ssid.c_str(),
             ap_pw.c_str());
    ESP_ERROR_CHECK(wifi.StartSoftAp(ap_ssid.c_str(), ap_pw.c_str()));
    // Warm the SSID scan cache in the background so /api/scan can return
    // instantly once the portal page loads.
    wifi.StartBackgroundScan();

    portal = std::make_unique<btclock::ProvisioningServer>(
        wifi, btclock::board::kHardwareName,
        [&net_prefs](const std::string& new_ssid, const std::string& new_pw) {
          net_prefs.SetString("ssid", new_ssid.c_str());
          net_prefs.SetString("pw", new_pw.c_str());
          net_prefs.Commit();
          ESP_LOGI("poc", "creds saved; rebooting");
          vTaskDelay(pdMS_TO_TICKS(1000));
          esp_restart();
        });
    ESP_ERROR_CHECK(portal->Start());

    ip4_addr_t ap_ip4 = {};
    inet_pton(AF_INET, wifi.ap_ip().c_str(), &ap_ip4);
    dns = std::make_unique<btclock::DnsHijack>(ap_ip4.addr);
    ESP_ERROR_CHECK(dns->Start());
  }

  // --- Data hub + initial render ---
  std::unique_ptr<btclock::DataHub> hub;
  QueueHandle_t button_q = xQueueCreate(8, sizeof(btclock::ButtonInput));
  assert(button_q != nullptr);
  std::unique_ptr<btclock::ButtonReader> buttons;
  TaskHandle_t main_task = xTaskGetCurrentTaskHandle();

  // Zap listener stack — kept separate from the Nostr DataSource so the
  // user can run one without the other and so the zap relay (often
  // relay.primal.net) can differ from the data relay. Lifetime must
  // outlive the STA boot branch below because the WSS callback is
  // asynchronous and we never return from app_main.
  std::unique_ptr<btclock::nostr::RelayClient> zap_relay;
  std::unique_ptr<btclock::nostr::SubscriptionManager> zap_subs;
  std::unique_ptr<btclock::nostr::ZapListener> zap_listener;
  // Atomic so a future /api settings PATCH handler can toggle the flash
  // without tearing down the listener. Default matches the NVS default
  // applied below; the captured reference inside the SetOnZap lambda
  // reads this at each receipt.
  static std::atomic<bool> flash_on_zap_enabled{true};
  static std::atomic<bool> flash_frontlight_on_zap_enabled{false};
  // Zap notification-screen preferences (nostrZapNotify + scrnRestoreZap).
  // Cached atomics so the worker-thread callback can read them without
  // hitting NVS on every receipt. Defaults mirror the old firmware
  // (notify=true, auto-restore=true).
  static std::atomic<bool> zap_notify_screen_enabled{true};
  static std::atomic<bool> zap_screen_auto_restore{true};
  // Flag raised by the zap callback; the main render loop clears it.
  // atomic<bool> is lock-free on ESP32 so the callback and the main
  // task can touch it without a mutex.
  static std::atomic<bool> zap_notify_pending{false};

  // Active currency set. For now hardcoded; beads lx0.11+ tracks the
  // NVS-backed config. Antonio's subset covers $/£/¥/€ symbols.
  const std::vector<std::string> kCurrencies = {"USD", "EUR", "GBP", "JPY"};
  btclock::ScreenManager sm(MsNow(), kCurrencies);
  // Sats-symbol glyph index (0..15) lives in the "ui" namespace so it
  // can sit alongside future user-facing display prefs (color, layout)
  // rather than cohabiting with network / Nostr creds. ClampSatsVariant
  // defends against an out-of-range stored value — see fonts_app.hpp.
  {
    btclock::Prefs ui_prefs("ui");
    sm.SetSatsVariant(btclock::ClampSatsVariant(
        ui_prefs.GetU32("sats_variant", btclock::kSatsVariantDefault)));
  }
  // Rotation skip hook: honours the pool capability flag so solo pools
  // don't cycle onto the earnings slot even if the user's persisted
  // `screen71Visible` is still true. Evaluated per-tick so a PATCH that
  // flips miningPoolName picks up on the next rotation step without a
  // reboot. NVS GetString is cached after the first open — the hit is
  // effectively free.
  sm.SetSkipPredicate([](btclock::ScreenType kind) -> bool {
    if (kind != btclock::ScreenType::kMiningPoolEarnings) return false;
    btclock::Prefs prefs(btclock::prefs::kSettingsNs);
    const std::string name =
        prefs.GetString(btclock::prefs::kMiningPoolName, "");
    return !btclock::mining_pools::PoolSupportsDailyEarnings(name);
  });

  if (wifi.is_ap_mode()) {
    const int64_t t0 = MsNow();
    btclock::RenderProvisioningScreen(panels, fb_storage, fonts, ap_ssid,
                                       ap_pw);
    ESP_LOGI(kTag, "provisioning pass %lld ms", MsNow() - t0);
  } else {
    hub = std::make_unique<btclock::DataHub>();
    hub->AddSource(std::make_unique<btclock::BtclockDataSource>(
        "wss://ws.btclock.dev/api/v2/ws", kCurrencies));
    // Optional Nostr DataSource. Opt-in via NVS (namespace "nostr"):
    //   key "enable" (bool, default false)  — master switch
    //   key "relay"  (string)                — wss:// URL
    //   key "pub"    (hex string, 64 chars)  — publisher pubkey
    // Missing or empty strings → skip cleanly rather than failing boot.
    // A future follow-up will expose these via the control-server /api
    // and the provisioning portal; for now set them with `nvs_tool` or a
    // one-shot boot-time Prefs::SetString().
    {
      btclock::Prefs nostr_prefs("nostr");
      const bool enable = nostr_prefs.GetBool("enable", false);
      const std::string relay = nostr_prefs.GetString("relay", "");
      const std::string pub = nostr_prefs.GetString("pub", "");
      if (enable && !relay.empty() && !pub.empty()) {
        btclock::nostr::NostrDataSource::Config ncfg;
        ncfg.relay_url = relay;
        ncfg.author_pubkey_hex = pub;
        // Leave d_tags empty → subscribe to all slots the publisher
        // emits (price:*, blockheight, medianFee). Narrowing is a
        // future optimisation if the pubkey publishes more than we need.
        hub->AddSource(
            std::make_unique<btclock::nostr::NostrDataSource>(std::move(ncfg)));
        ESP_LOGI(kTag, "nostr enabled: relay=%s pub=%s…", relay.c_str(),
                 pub.substr(0, 8).c_str());
      } else {
        ESP_LOGI(kTag, "nostr disabled (enable=%d relay=%s pub=%s)",
                 enable ? 1 : 0, relay.empty() ? "<empty>" : "set",
                 pub.empty() ? "<empty>" : "set");
      }
    }

    // Optional zap listener — separate WSS connection from the Nostr
    // DataSource above so enabling/disabling one doesn't tear down the
    // other and the zap relay URL can differ from the data relay.
    // NVS (namespace "nostr"):
    //   zapEnable  (bool, default true)   — master switch for zap receipts
    //   zapRelay   (string)               — wss:// URL
    //   zapPubkey  (string)               — recipient pubkey (hex, 64 chars)
    //   flashOnZap (bool, default true)   — gate for LED pulse on receipt
    // NVS key names kept under 15 chars to fit the nvs_set_* limit.
    // Defaults are the current test setup; provisioning portal /
    // /api settings PATCH will overwrite them.
    {
      btclock::Prefs zap_prefs("nostr");
      const bool zap_enable = zap_prefs.GetBool("zapEnable", true);
      const std::string zap_relay_url =
          zap_prefs.GetString("zapRelay", "wss://relay.primal.net");
      const std::string zap_pub = zap_prefs.GetString(
          "zapPubkey",
          "b5127a08cf33616274800a4387881a9f98e04b9c37116e92de5250498635c422");
      flash_on_zap_enabled.store(zap_prefs.GetBool("flashOnZap", true));
      // Frontlight flash-on-zap lives in the frontlight pref namespace to
      // match the old firmware (src/lib/system/pref_keys.hpp::FlFlashOnZap),
      // so re-using it keeps settings migration from Arduino straightforward.
      {
        btclock::Prefs fl_prefs("frontlight");
        flash_frontlight_on_zap_enabled.store(
            fl_prefs.GetBool("flFlashOnZap", false));
      }
      // Notification-screen gates live in the "settings" namespace
      // alongside the other renderer-behaviour flags so a WebUI PATCH
      // round-trips without poking two namespaces.
      {
        btclock::Prefs settings_prefs(btclock::prefs::kSettingsNs);
        zap_notify_screen_enabled.store(
            settings_prefs.GetBool(btclock::prefs::kNostrZapNotify, true));
        zap_screen_auto_restore.store(
            settings_prefs.GetBool(btclock::prefs::kScrnRestoreZap, true));
      }
      if (zap_enable && !zap_relay_url.empty() && zap_pub.size() == 64) {
        zap_relay =
            std::make_unique<btclock::nostr::RelayClient>(zap_relay_url);
        zap_subs = std::make_unique<btclock::nostr::SubscriptionManager>(
            *zap_relay);
        zap_listener = std::make_unique<btclock::nostr::ZapListener>(
            *zap_subs, std::string("zap"), zap_pub);
        btclock::FrontlightController* fl_ptr = frontlight.get();
        btclock::DataHub* hub_ptr = hub.get();
        zap_listener->SetOnZap(
            [fl_ptr, hub_ptr, main_task](
                const btclock::nostr::ZapListener::ZapInfo& z) {
              const uint64_t sats = z.amount_msat / 1000ULL;
              const std::string eid =
                  z.raw ? z.raw->id.substr(0, 8) : std::string("?");
              ESP_LOGI(kTag, "zap: %llu sats id=%s…",
                       static_cast<unsigned long long>(sats), eid.c_str());
              ESP_LOGD(kTag, "zap bolt11: %s", z.bolt11.c_str());
              // Always update the snapshot so /api/status can echo the
              // latest receipt regardless of whether we pop the screen —
              // matches the spec choice of "nostrZapNotify gates the
              // override + LED flash only, not the data side".
              if (hub_ptr) {
                btclock::DataSnapshot patch;
                patch.latest_zap.amount_sats =
                    static_cast<int64_t>(sats);
                patch.latest_zap.message = z.content;
                patch.latest_zap.received_ms = MsNow();
                hub_ptr->Report(patch);
              }
              // Notify + LED flash only fire when the user hasn't
              // disabled them. Keep both gated by the same pref so a
              // "quiet" user experience is opt-in via a single toggle.
              if (zap_notify_screen_enabled.load()) {
                if (flash_on_zap_enabled.load()) {
                  btclock::PostLedEffect(btclock::LedEffect::kZap);
                }
                if (fl_ptr && flash_frontlight_on_zap_enabled.load()) {
                  fl_ptr->ZapFlash();
                }
                // Signal the main loop to flip ScreenManager into the
                // zap overlay. The hub Report above also wakes main_task
                // via the on-update callback; the pending flag picks
                // that wake up and dispatches SetZapNotify.
                zap_notify_pending.store(true);
                xTaskNotifyGive(main_task);
              }
            });
        ESP_ERROR_CHECK(zap_relay->Start());
        zap_listener->Start();
        ESP_LOGI(kTag,
                 "zap listener enabled: relay=%s pub=%s… flashLed=%d flashFl=%d",
                 zap_relay_url.c_str(), zap_pub.substr(0, 8).c_str(),
                 flash_on_zap_enabled.load() ? 1 : 0,
                 flash_frontlight_on_zap_enabled.load() ? 1 : 0);
      } else {
        ESP_LOGI(kTag,
                 "zap listener disabled (enable=%d relay=%s pub=%s)",
                 zap_enable ? 1 : 0,
                 zap_relay_url.empty() ? "<empty>" : "set",
                 zap_pub.size() == 64 ? "set" : "<invalid>");
      }
    }
    // Optional mining-pool HTTPS poller. Only the selected pool polls
    // (old-firmware behaviour — a single DataSnapshot::pool producer
    // avoids last-writer-wins races). Gated on settings/miningPoolStats;
    // disabled-by-default keeps Wi-Fi-bandwidth impact zero for users
    // who don't mine. See app/mining_pool_selector.{hpp,cpp}.
    if (auto pool_src = btclock::mining_pools::MakeActivePoolSource()) {
      hub->AddSource(std::move(pool_src));
    }

    // Optional Bitaxe LAN poller. Gated on settings/bitaxeEnabled +
    // a non-empty settings/bitaxeHostname. When both screens (hashrate
    // + best-diff) are in the rotation they paint "OFFLINE" until the
    // first poll lands.
    if (auto bitaxe_src = btclock::bitaxe::MakeBitaxeSource()) {
      hub->AddSource(std::move(bitaxe_src));
    }

    hub->SetOnUpdate([main_task](const btclock::DataSnapshot&) {
      xTaskNotifyGive(main_task);
    });
    ESP_ERROR_CHECK(hub->StartAll());

    // Block until the first blockheight arrives (or 30 s passes and we
    // paint whatever — the event loop will catch up when data lands).
    ESP_LOGI(kTag, "waiting for first blockheight push …");
    const int64_t deadline = MsNow() + 30'000;
    while (!hub->GetSnapshot().block_height) {
      const int64_t remain = deadline - MsNow();
      if (remain <= 0) break;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(remain));
    }
    sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());

    // Buttons come up after first paint so early clicks don't race a
    // blank display.
    buttons = std::make_unique<btclock::ButtonReader>(mcp, button_q);
    ESP_ERROR_CHECK(buttons->Start());

    btclock::PostLedEvent(btclock::LedEvent::kSetIdle);
  }

  // --- HTTP control API (STA mode only) ---
  // The control surface mirrors the production firmware's /api/* routes
  // so the existing WebUI in data/ can drive this PoC. Functional
  // endpoints post back into the event loop via the command queue so we
  // never touch ScreenManager / panels from the httpd worker task.
  // Stubbed endpoints return 501 with a tracking token; see
  // components/webserver/control_server.cpp for the list.
  // Adapter exposing FrontlightController to the webserver component
  // without pulling a main-owned header into the component tree. The
  // adapter lives as long as the controller does; its lifetime is
  // bounded by the enclosing unique_ptr below.
  struct FrontlightAdapter : btclock::FrontlightIface {
    explicit FrontlightAdapter(btclock::FrontlightController* fl) : fl_(fl) {}
    void On() override { fl_->On(); }
    void Off() override { fl_->Off(); }
    void Flash() override { fl_->Flash(); }
    void SetBrightness(uint16_t duty) override { fl_->SetBrightness(duty); }
    Status GetStatus() const override {
      const auto s = fl_->GetStatus();
      return Status{s.enabled, s.current_duty, s.target_duty,
                    s.configured_brightness, s.lux_threshold,
                    s.ambient_auto_off};
    }
    btclock::FrontlightController* fl_;
  };
  std::unique_ptr<FrontlightAdapter> fl_adapter;
  if (frontlight) {
    fl_adapter = std::make_unique<FrontlightAdapter>(frontlight.get());
  }

  // NeoPixel adapter — same shape as FrontlightAdapter, forwards to
  // the led_controller namespace-level API. Shared LED state lives in
  // that TU so there's no per-controller handle; the adapter is
  // effectively stateless.
  struct LedsAdapter : btclock::LedsIface {
    Status GetStatus() const override {
      const auto s = btclock::GetLedState();
      Status out{};
      out.brightness = s.brightness;
      out.block_flash_color = s.block_flash_color;
      out.disabled = s.disabled;
      out.flash_on_update = s.flash_on_update;
      out.pixel_count = s.pixel_count;
      const uint32_t n = s.pixel_count <
                         (sizeof(out.pixels) / sizeof(out.pixels[0]))
                             ? s.pixel_count
                             : (sizeof(out.pixels) / sizeof(out.pixels[0]));
      for (uint32_t i = 0; i < n; ++i) out.pixels[i] = s.pixels[i];
      return out;
    }
    void SetSolidColor(uint32_t rgb) override {
      btclock::SetLedSolidColor(rgb);
    }
    void SetPixels(const uint32_t* rgb_array, uint32_t count) override {
      btclock::SetLedPixels(rgb_array, count);
    }
    void SetDisabled(bool disabled) override {
      btclock::SetLedDisabled(disabled);
    }
    void SetBrightness(uint8_t value) override {
      btclock::SetLedBrightness(value);
    }
    void SetBlockFlashColor(uint32_t rgb) override {
      btclock::SetBlockFlashColor(rgb);
    }
    void TriggerIdentify() override {
      btclock::PostLedEffect(btclock::LedEffect::kIdentify);
    }
  };
  auto leds_adapter = std::make_unique<LedsAdapter>();

  // DND adapter — forwards the webserver's DndIface calls to the
  // process-wide Dnd singleton. Keeps the webserver component free of
  // a dependency on the `dnd` component while the main module, which
  // already pulls both in, bridges them.
  struct DndAdapter : btclock::DndIface {
    Status GetStatus() const override {
      auto& d = btclock::dnd::Instance();
      const auto cfg = d.GetConfig();
      Status s{};
      s.enabled = cfg.enabled;
      s.time_enabled = cfg.time_enabled;
      s.start_hour = cfg.start_hour;
      s.start_minute = cfg.start_minute;
      s.end_hour = cfg.end_hour;
      s.end_minute = cfg.end_minute;
      s.active = d.IsActive();
      return s;
    }
    void SetEnabled(bool enabled) override {
      btclock::dnd::Instance().SetEnabled(enabled);
    }
  };
  auto dnd_adapter = std::make_unique<DndAdapter>();

  // LightSensor adapter — thin forwarder so the webserver component
  // doesn't have to pull in bh1750.hpp or FreeRTOS. Stateless apart
  // from the pointer; nullable LightSensor means the adapter is only
  // constructed when the board actually has a BH1750.
  struct LightSensorAdapter : btclock::LightSensorIface {
    explicit LightSensorAdapter(btclock::LightSensor* ls) : ls_(ls) {}
    bool IsAvailable() const override {
      return ls_ && ls_->IsAvailable();
    }
    float GetLux() const override { return ls_ ? ls_->GetLux() : -1.0f; }
    btclock::LightSensor* ls_;
  };
  std::unique_ptr<LightSensorAdapter> light_sensor_adapter;
  if (light_sensor) {
    light_sensor_adapter =
        std::make_unique<LightSensorAdapter>(light_sensor.get());
  }

  // Timer adapter — pause / restart the screen-rotation deadline from
  // the HTTP task. ScreenManager is owned on the main task; the
  // adapter's writes are plain scalar updates so a cross-task poke is
  // safe without an explicit command-queue hop.
  struct TimerAdapter : btclock::TimerIface {
    TimerAdapter(btclock::ScreenManager& sm_ref, int64_t (*now_fn)())
        : sm(sm_ref), now(now_fn) {}
    bool IsPaused() const override { return sm.IsPaused(); }
    void SetPaused(bool paused) override { sm.SetPaused(paused); }
    void Restart() override { sm.RestartTimer(now()); }
    btclock::ScreenManager& sm;
    int64_t (*now)();
  };
  auto timer_adapter = std::make_unique<TimerAdapter>(sm, MsNow);

  // Factory-reset trigger shared by the HTTP endpoint and the
  // all-buttons-held hardware combo. Renders a "RESETTING" splash onto
  // the EPD chain (so the user gets visual confirmation that the wipe
  // is happening) and then calls PerformFactoryReset(), which erases
  // NVS and reboots. The helper is [[noreturn]] — nothing after it
  // runs. Invoked from the httpd worker task (endpoint path) or the
  // button poll task (combo path); in both cases the main task may be
  // mid-frame on the panels, but the imminent reboot makes any torn
  // frame moot, and the helper's 2 s vTaskDelay gives the final SPI
  // transfers time to drain.
  auto factory_reset_trigger = [&]() {
    const std::size_t n = static_cast<std::size_t>(kNumPanels);
    std::vector<std::string> cells(n, std::string(" "));
    // "ERASING" fits 7-panel boards exactly and centers with one right
    // pad on 8-panel V8. "RESETTING" would truncate to "RESETTI" on
    // 7-panel which reads as a different word — pick a shorter message
    // that renders intact on every variant.
    static constexpr char kMsg[] = "ERASING";
    const std::size_t msg_len = sizeof(kMsg) - 1;
    const std::size_t take = msg_len < n ? msg_len : n;
    const std::size_t pad = (n - take) / 2;
    for (std::size_t i = 0; i < take; ++i) {
      cells[pad + i] = std::string(1, kMsg[i]);
    }
    sm.SetCustomCells(std::move(cells), MsNow());
    if (hub) {
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
    } else {
      // AP mode still needs a visible splash. Use an empty snapshot —
      // the custom-cells path doesn't read data from it anyway.
      sm.Render(panels, fb_storage, fonts, btclock::DataSnapshot{});
    }
    btclock::settings::PerformFactoryReset();
  };

  if (buttons) {
    buttons->SetOnAllButtonsLongPress(factory_reset_trigger);
  }

  std::unique_ptr<btclock::ControlServer> ctrl;
  std::unique_ptr<btclock::SseServer> sse;
  if (!wifi.is_ap_mode()) {
    btclock::ControlServer::Config ccfg;
    ccfg.wifi = &wifi;
    ccfg.hub = hub.get();
    ccfg.currencies = kCurrencies;
    ccfg.num_screens = kNumPanels;
    ccfg.hw_name = btclock::board::kHardwareName;
    // Populates /api/settings `availablePools` so the WebUI dropdown
    // matches the compiled-in pool list. Kept in sync with the factory
    // in mining_pool_selector.cpp.
    ccfg.available_pools = btclock::mining_pools::AvailablePoolNames();
    ccfg.frontlight = fl_adapter.get();
    ccfg.leds = leds_adapter.get();
    ccfg.dnd = dnd_adapter.get();
    ccfg.timer = timer_adapter.get();
    ccfg.light_sensor = light_sensor_adapter.get();
    // Mirror freshly-PATCHed DND fields into the singleton so the LED
    // and frontlight suppressor predicates (`dnd::Instance().IsActive()`,
    // evaluated on every frame) switch over to the new window without a
    // reboot. Settings persist to the `settings` namespace via the PATCH
    // handler; the singleton reads/writes its own `dnd` namespace, so
    // this hook is also what keeps the two namespaces in sync at
    // runtime.
    ccfg.on_factory_reset = factory_reset_trigger;
    // tzString PATCH -> setenv("TZ", ...) + tzset() so the clock screen
    // follows the new zone without reboot. Old firmware applied the zone
    // inline inside the PATCH handler; we keep the split so the webserver
    // component stays independent of the timezone component.
    ccfg.on_tz_changed = [](const std::string& zone) {
      if (zone.empty()) return;
      (void)btclock::timezone::SetTimezoneByName(zone.c_str());
    };
    // invertedColor PATCH hook — flip the EPD polarity flag + mark the
    // screen dirty so the next frame paints a full-refresh with the new
    // polarity. The data-driven render path runs on the main task's
    // event loop; the dirty flag is plain scalar state so poking it
    // from the httpd worker is safe without a command-queue hop.
    {
      btclock::ScreenManager* sm_ptr = &sm;
      ccfg.on_inverted_color_changed = [sm_ptr](bool v) {
        btclock::EpdSetGlobalInverted(v);
        sm_ptr->MarkDirty();
      };
    }
    ccfg.on_dnd_changed = [] {
      btclock::Prefs settings_ns(btclock::prefs::kSettingsNs);
      auto& d = btclock::dnd::Instance();
      d.SetTimeEnabled(
          settings_ns.GetBool(btclock::prefs::kDndTimeEnabled, false));
      d.SetTimeRange(
          static_cast<uint8_t>(
              settings_ns.GetU32(btclock::prefs::kDndStartHour, 22) & 0xFFu),
          static_cast<uint8_t>(
              settings_ns.GetU32(btclock::prefs::kDndStartMin, 0) & 0xFFu),
          static_cast<uint8_t>(
              settings_ns.GetU32(btclock::prefs::kDndEndHour, 7) & 0xFFu),
          static_cast<uint8_t>(
              settings_ns.GetU32(btclock::prefs::kDndEndMin, 0) & 0xFFu));
    };
    // Runtime catalogues for GET /api/settings drop-downs. Copies of the
    // constexpr arrays in app/catalogs.hpp; the settings handler holds
    // std::vector<std::string> slots so we materialise the views here
    // rather than refactoring the settings API type.
    for (const auto& f : btclock::catalogs::kAvailableFonts) {
      ccfg.available_fonts.emplace_back(f);
    }
    for (const auto& c : btclock::catalogs::kAvailableCurrencies) {
      ccfg.available_currencies.emplace_back(c);
    }
    for (const auto& s : btclock::catalogs::kScreenKinds) {
      ccfg.screens_catalog.push_back({s.api_id, std::string(s.display_label)});
    }
    // Nostr zap-relay liveness — read on every /api/status so the WebUI's
    // connection badge tracks reality instead of the hardcoded-false we
    // used before the ZapListener was wired.
    {
      btclock::nostr::RelayClient* zap_ptr = zap_relay.get();
      if (zap_ptr != nullptr) {
        ccfg.nostr_connected = [zap_ptr]() { return zap_ptr->connected(); };
      }
    }
    // Capability gate — lets /api/settings and /api/show/screen know that
    // the mining-pool earnings slot is useless on a solo pool (no per-user
    // payout to render; the screen would forever show "0 SATS"). Evaluated
    // per-request so switching pool via PATCH /api/settings takes effect on
    // the next GET. Read is a cheap cached NVS lookup.
    ccfg.screen_is_hidden = [](int api_id) -> bool {
      if (api_id != btclock::slot_map::kApiIdMiningPoolEarnings) return false;
      btclock::Prefs prefs(btclock::prefs::kSettingsNs);
      const std::string name =
          prefs.GetString(btclock::prefs::kMiningPoolName, "");
      return !btclock::mining_pools::PoolSupportsDailyEarnings(name);
    };

    // POST /api/show/screen?s=<api_id> and the `currentScreen` field in
    // /api/status both speak the settings-catalog id the WebUI persists —
    // not ScreenManager's dense slot index. Bridge the two with pure-logic
    // helpers (app/screen_slot_map.hpp) so the HTTP handler stays free of
    // ScreenManager internals. api_id_to_slot lands per-currency screens
    // on whichever currency is currently displayed, matching the old
    // firmware's "stay on my current currency when I pick a new screen"
    // UX.
    {
      btclock::ScreenManager* sm_ptr = &sm;
      ccfg.api_id_to_slot = [sm_ptr](int api_id) -> int {
        const auto& ccs = sm_ptr->currencies();
        std::size_t pref = 0;
        const std::string& cur = sm_ptr->current_currency();
        for (std::size_t i = 0; i < ccs.size(); ++i) {
          if (ccs[i] == cur) { pref = i; break; }
        }
        return btclock::slot_map::SlotForApiId(api_id, ccs.size(), pref);
      };
      ccfg.slot_to_api_id = [sm_ptr](std::size_t slot) -> int {
        return btclock::slot_map::ApiIdForSlot(
            slot, sm_ptr->currencies().size());
      };
    }
    ctrl = std::make_unique<btclock::ControlServer>(std::move(ccfg));
    // SSE lifecycle: construct before Start() so RegisterRoute fires
    // in the same handler-registration pass as /api/*. The SseServer
    // does not own the httpd; ControlServer does, and stops it in its
    // destructor — which outlives `sse` via the destruction order of
    // these unique_ptrs.
    sse = std::make_unique<btclock::SseServer>(btclock::SseServer::Config{});
    ctrl->AttachSse(sse.get());
    if (ctrl->Start() != ESP_OK) {
      ESP_LOGE(kTag, "control server failed to start; control API disabled");
      ctrl.reset();
      sse.reset();
    }
  }

  // OTA manager — stores release URL + per-variant asset name so the
  // /api/firmware/auto_update handler can kick off a pull update. The
  // release_url is read from NVS so a user-set override (GET/PATCH
  // /api/settings `gitReleaseUrl`) is honoured without a rebuild.
  {
    btclock::settings::NvsPrefs ota_prefs(btclock::prefs::kSettingsNs);
    btclock::OtaManager::Config ocfg;
    ocfg.release_url = ota_prefs.GetString(btclock::prefs::kGitReleaseUrl, "");
    // Asset naming mirrors the Arduino release workflow:
    //   btclock_<variant>_ota.bin  / btclock_<variant>_webui.bin
    // The variant slug matches the old `BOARD` define — lowercased
    // with underscores. Rev A / Rev B / V8 are the only shipping
    // variants today. Kept behind POC_BOARD_* macros because the
    // human-readable kHardwareName ("Rev B") contains a space.
#if defined(POC_BOARD_REV_A)
    ocfg.firmware_asset = "btclock_rev_a_ota.bin";
    ocfg.webui_asset = "btclock_rev_a_webui.bin";
#elif defined(POC_BOARD_REV_B)
    ocfg.firmware_asset = "btclock_rev_b_ota.bin";
    ocfg.webui_asset = "btclock_rev_b_webui.bin";
#elif defined(POC_BOARD_V8)
    ocfg.firmware_asset = "btclock_v8_ota.bin";
    ocfg.webui_asset = "btclock_v8_webui.bin";
#else
    // TODO: fill in exact filename once a release ships for this variant.
    ocfg.firmware_asset = "btclock_unknown_ota.bin";
    ocfg.webui_asset = "btclock_unknown_webui.bin";
#endif
    btclock::GetOtaManager().Init(ocfg);
  }

  // Re-hook DataHub on-update so fresh snapshots also fan out to SSE
  // subscribers. Capturing `ctrl`'s raw pointer is safe — the hub's
  // callback lifetime is bounded by this scope (no dangling after
  // app_main returns; app_main never returns in this firmware). We
  // keep the main-task notify so the render loop still wakes.
  if (hub) {
    btclock::ControlServer* ctrl_ptr = ctrl.get();
    hub->SetOnUpdate([main_task, ctrl_ptr](const btclock::DataSnapshot&) {
      xTaskNotifyGive(main_task);
      if (ctrl_ptr) ctrl_ptr->BroadcastStatus();
    });
  }

  // Pull timer state live from the screen manager so /api/status
  // doesn't drift when the user toggles /api/action/pause from the
  // WebUI. The webserver's TimerIface also reads this directly — this
  // lambda just keeps LiveStatus in sync for readers that cache it.
  auto publish_status = [&]() {
    if (!ctrl) return;
    btclock::ControlServer::LiveStatus ls;
    ls.current_slot = static_cast<int32_t>(sm.current_slot());
    ls.slot_count = static_cast<int32_t>(sm.slot_count_public());
    ls.timer_running = !sm.IsPaused();
    ls.currency = sm.current_currency();
    ls.panel_texts = sm.last_panel_texts();
    ctrl->PublishStatus(ls);
    // Fan out to SSE subscribers so screen rotations / button presses
    // surface in the WebUI without waiting for the next poll.
    ctrl->BroadcastStatus();
  };
  publish_status();

  // --- Event loop ---
  // Three wake sources: data-push notify, button event, 1 s heartbeat
  // tick. We drain the button queue first so a queued click is honoured
  // before we sleep on the task-notify.
  constexpr int64_t kAutoRotateMs = 30'000;
  int64_t last_heartbeat_ms = 0;

  while (true) {
    // Drain control-API commands first. These ride in on the httpd
    // worker task via the ControlServer's queue, not the button queue,
    // so they need their own drain step. A single iteration handles
    // one command to keep the event loop's "one action per pass"
    // contract intact (rotations, rendering, etc. in the same pass).
    btclock::ControlCommand ccmd{};
    if (ctrl && ctrl->TryPopCommand(&ccmd)) {
      using Kind = btclock::ControlCommand::Kind;
      bool re_render = false;
      switch (ccmd.kind) {
        case Kind::kFullRefresh:
          sm.MarkDirty();
          re_render = true;
          break;
        case Kind::kIdentify:
          // Triple rapid multi-colour flash — matches the old firmware's
          // LED_FLASH_IDENTIFY (red↔cyan then green↔blue).
          btclock::PostLedEffect(btclock::LedEffect::kIdentify);
          break;
        case Kind::kRestart:
          ESP_LOGW(kTag, "restart requested via /api/restart");
          vTaskDelay(pdMS_TO_TICKS(500));
          esp_restart();
          break;
        case Kind::kShowScreen:
          sm.SetSlot(static_cast<size_t>(ccmd.arg_i), MsNow());
          re_render = true;
          break;
        case Kind::kShowCurrency:
          sm.SetCurrency(ccmd.arg_s, MsNow());
          re_render = true;
          break;
        case Kind::kNextScreen:
          sm.NextScreen(MsNow());
          re_render = true;
          break;
        case Kind::kPrevScreen:
          sm.PrevScreen(MsNow());
          re_render = true;
          break;
        case Kind::kStopDataSources:
          if (hub) hub->StopAll();
          break;
        case Kind::kRestartDataSources:
          // StartAll() on an already-running source is a no-op for the
          // btclock WS source today; a clean stop+start is the closer
          // match to the old firmware. Cheap — sources only number 1.
          if (hub) {
            hub->StopAll();
            hub->StartAll();
          }
          break;
        case Kind::kShowCustom: {
          // Payload landed on ControlServer::pending_custom_ alongside
          // the command; pull it here before touching ScreenManager so
          // a concurrent second request can't slip its payload between
          // the pull and the apply.
          std::vector<std::string> cells;
          if (ctrl->TakePendingCustomCells(&cells)) {
            sm.SetCustomCells(std::move(cells), MsNow());
            re_render = true;
          }
          break;
        }
      }
      if (re_render && hub) sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    btclock::ButtonInput ev{};
    if (xQueueReceive(button_q, &ev, 0) == pdTRUE) {
      // Only the falling-edge kClick drives actions today; kLongPress
      // events are intentionally dropped until someone finds them a
      // semantic. Mirrors the brief: "ignore long-press for now".
      if (ev.event != btclock::ButtonEvent::kClick) continue;
      bool re_render = false;
      bool show_debug = false;
      switch (ev.id) {
        case btclock::ButtonId::k0:
          // Pause / resume auto-rotate. The current slot stays up.
          sm.TogglePaused();
          ESP_LOGI(kTag, "button: pause=%d", sm.IsPaused() ? 1 : 0);
          // No re-render — the screen content is unchanged; publish
          // status so the WebUI timer flag updates.
          break;
        case btclock::ButtonId::k1:
          // Next screen — same as auto-rotate step, Restart() resets
          // the rotation deadline so we don't immediately advance again.
          if (!sm.IsDebug()) {
            sm.NextScreen(MsNow());
            re_render = true;
          }
          break;
        case btclock::ButtonId::k2:
          if (!sm.IsDebug()) {
            sm.PrevScreen(MsNow());
            re_render = true;
          }
          break;
        case btclock::ButtonId::k3: {
          // Toggle debug overlay. Entry renders via RenderDebug (full-
          // refresh, no data snapshot); exit marks dirty so the normal
          // render path repaints the underlying data slot.
          const bool now_debug = sm.ToggleDebug(MsNow());
          show_debug = now_debug;
          re_render = !now_debug;
          break;
        }
      }
      if (show_debug) {
        btclock::DebugScreenInfo info;
        info.ip = wifi.ip();
        info.ssid = ssid;
        info.free_heap = static_cast<uint32_t>(
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        info.free_psram = static_cast<uint32_t>(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        info.hw_name = btclock::board::kHardwareName;
        info.built = __DATE__;
        info.uptime_s = static_cast<uint32_t>(MsNow() / 1000);
        sm.RenderDebug(panels, fb_storage, fonts, info);
      } else if (re_render && hub) {
        sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      }
      publish_status();
      continue;
    }

    const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const int64_t now_ms = MsNow();

    // Soft watchdog pump — skipped in AP/provisioning mode, where the
    // whole point is that there's no STA connection to watch.
    if (!wifi.is_ap_mode()) {
      outage_watchdog.Tick(wifi, static_cast<uint32_t>(now_ms));
    }

    if (now_ms - last_heartbeat_ms >= 10'000) {
      uint16_t port = 0;
      mcp.ReadPort(&port);
      // Prefer the cached reading from the poll task so we don't race
      // on the I2C bus with it. Falls back to -1.0 when no sensor.
      const float lux = light_sensor ? light_sensor->GetLux() : -1.0f;
      ESP_LOGI(kTag, "t=%llds buttons=0x%X lux=%.1f heap=%u psram=%u",
               static_cast<long long>(now_ms / 1000),
               static_cast<unsigned>(port & 0xF), lux,
               static_cast<unsigned>(
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(
                   heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      // Auto-off: feed each fresh lux reading to the frontlight
      // controller. Below threshold -> on, above -> off. No-op if the
      // board has no frontlight or no ambient sensor.
      if (frontlight) frontlight->OnAmbientLux(lux);
      last_heartbeat_ms = now_ms;
    }

    // Zap notification: push the overlay onto ScreenManager from the
    // main task. The atomic flag was raised by the zap listener's worker-
    // thread callback, so doing the mutation here keeps ScreenManager
    // single-threaded. Expected bool for exchange — clear the flag
    // before dispatching.
    bool pending_zap = true;
    if (zap_notify_pending.compare_exchange_strong(pending_zap, false) &&
        hub) {
      sm.SetZapNotify(now_ms, zap_screen_auto_restore.load());
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    if (sm.MaybeAutoRotate(now_ms, kAutoRotateMs) && hub) {
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status();
      continue;
    }

    if (!hub) continue;
    const auto snap = hub->GetSnapshot();

    if (sm.ConsumeNewBlock(snap)) {
      btclock::PostLedEvent(btclock::LedEvent::kBlockFlash);
      if (frontlight) frontlight->Flash();
    }

    if (got != 0 && sm.ShouldRender(snap)) {
      sm.Render(panels, fb_storage, fonts, snap);
    }
  }
}
