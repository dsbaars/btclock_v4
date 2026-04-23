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
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/frontlight_controller.hpp"
#include "app/led_controller.hpp"
#include "app/screen_manager.hpp"
#include "app/time_sync.hpp"
#include "app/wifi_guard.hpp"
#include "bh1750.hpp"
#include "board/board.hpp"
#include "boot_ui.hpp"
#include "btclock_data.hpp"
#include "buttons.hpp"
#include "control_server.hpp"
#include "data_core/hub.hpp"
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
#include "pca9685.hpp"
#include "prefs.hpp"
#include "provisioning_server.hpp"
#include "provisioning_ui.hpp"
#include "sdkconfig.h"
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
  ESP_LOGI(kTag, "psram=%uB heap=%uB",
           static_cast<unsigned>(esp_psram_get_size()),
           static_cast<unsigned>(esp_get_free_heap_size()));

  btclock::InitLeds(kNeopixel, kNeopixelCount);
  btclock::PostLedEvent(btclock::LedEvent::kSetBoot);

  // --- I2C bus + peripherals ---
  btclock::I2cBus i2c(I2C_NUM_0, kI2cSda, kI2cScl);

  // V8: pulse the shared MCP RESET line once before any I2C transaction.
  // >= 5 µs low is the datasheet minimum; 5 ms is conservative.
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
    frontlight->Start();
    // Fade up to the configured brightness at boot. Matches old
    // firmware, which powered the frontlight on once the panels were
    // initialised.
    frontlight->On();
  }

  std::optional<btclock::Bh1750> bh;
  if constexpr (kHasAmbientLight) {
    bh.emplace(i2c, kBhAddr);
    ESP_ERROR_CHECK(bh->Begin());
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

  if (!ssid.empty()) {
    ESP_ERROR_CHECK(wifi.Start());
    ESP_ERROR_CHECK(wifi.Connect(ssid.c_str(), pw.c_str()));
    btclock::WaitForConnected(wifi);
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

  // Active currency set. For now hardcoded; beads lx0.11+ tracks the
  // NVS-backed config. Antonio's subset covers $/£/¥/€ symbols.
  const std::vector<std::string> kCurrencies = {"USD", "EUR", "GBP", "JPY"};
  btclock::ScreenManager sm(MsNow(), kCurrencies);

  if (wifi.is_ap_mode()) {
    const int64_t t0 = MsNow();
    btclock::RenderProvisioningScreen(panels, fb_storage, fonts, ap_ssid,
                                       ap_pw);
    ESP_LOGI(kTag, "provisioning pass %lld ms", MsNow() - t0);
  } else {
    hub = std::make_unique<btclock::DataHub>();
    hub->AddSource(std::make_unique<btclock::BtclockDataSource>(
        "wss://ws.btclock.dev/api/v2/ws", kCurrencies));
    // TODO(btclock_v3_fci-0wm): wire a nostr::NostrDataSource here once
    // the relay URL + publisher pubkey are user-configurable. Suggested
    // NVS keys (namespace "nostr"): "relay" (wss://...), "pub" (hex
    // pubkey), "zap_pub" (hex pubkey for zap listener), "enable" (bool).
    // See components/nostr/include/nostr/nostr_data_source.hpp.
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

  std::unique_ptr<btclock::ControlServer> ctrl;
  if (!wifi.is_ap_mode()) {
    btclock::ControlServer::Config ccfg;
    ccfg.wifi = &wifi;
    ccfg.hub = hub.get();
    ccfg.currencies = kCurrencies;
    ccfg.num_screens = kNumPanels;
    ccfg.hw_name = btclock::board::kHardwareName;
    ccfg.frontlight = fl_adapter.get();
    ctrl = std::make_unique<btclock::ControlServer>(std::move(ccfg));
    if (ctrl->Start() != ESP_OK) {
      ESP_LOGE(kTag, "control server failed to start; control API disabled");
      ctrl.reset();
    }
  }

  auto publish_status = [&](bool timer_running) {
    if (!ctrl) return;
    btclock::ControlServer::LiveStatus ls;
    ls.current_slot = static_cast<int32_t>(sm.current_slot());
    ls.slot_count = static_cast<int32_t>(sm.slot_count_public());
    ls.timer_running = timer_running;
    ls.currency = sm.current_currency();
    ctrl->PublishStatus(ls);
  };
  publish_status(true);

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
          btclock::PostLedEvent(btclock::LedEvent::kBlockFlash);
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
      }
      if (re_render && hub) sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status(true);
      continue;
    }

    btclock::ButtonInput ev{};
    if (xQueueReceive(button_q, &ev, 0) == pdTRUE) {
      bool rotated = false;
      if (ev.event == btclock::ButtonEvent::kClick) {
        rotated = sm.NextScreen(MsNow());
      } else if (ev.event == btclock::ButtonEvent::kLongPress) {
        rotated = sm.PrevScreen(MsNow());
      }
      if (rotated && hub) sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      if (rotated) publish_status(true);
      continue;
    }

    const uint32_t got = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    const int64_t now_ms = MsNow();

    if (now_ms - last_heartbeat_ms >= 10'000) {
      uint16_t port = 0;
      mcp.ReadPort(&port);
      const float lux = bh.has_value() ? bh->ReadLux() : -1.0f;
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

    if (sm.MaybeAutoRotate(now_ms, kAutoRotateMs) && hub) {
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      publish_status(true);
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
