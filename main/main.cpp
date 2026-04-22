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
#include <memory>
#include <optional>
#include <string>

#include "app/led_controller.hpp"
#include "app/screen_manager.hpp"
#include "app/time_sync.hpp"
#include "app/wifi_guard.hpp"
#include "bh1750.hpp"
#include "board/board.hpp"
#include "boot_ui.hpp"
#include "btclock_data.hpp"
#include "buttons.hpp"
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
  if constexpr (kHasFrontlight) {
    pca.emplace(i2c, kPcaAddr, kPcaOe);
    ESP_ERROR_CHECK(pca->Begin(1000));
    pca->SetOutputEnable(true);
    for (uint8_t i = 0; i < kFrontlightChannelCount; ++i) {
      pca->SetDuty(kFrontlightChannelFirst + i, 1024);
    }
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

  btclock::ScreenManager sm(MsNow());

  if (wifi.is_ap_mode()) {
    const int64_t t0 = MsNow();
    btclock::RenderProvisioningScreen(panels, fb_storage, fonts, ap_ssid,
                                       ap_pw);
    ESP_LOGI(kTag, "provisioning pass %lld ms", MsNow() - t0);
  } else {
    hub = std::make_unique<btclock::DataHub>();
    hub->AddSource(std::make_unique<btclock::BtclockDataSource>(
        "wss://ws.btclock.dev/api/v2/ws"));
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

  // --- Event loop ---
  // Three wake sources: data-push notify, button event, 1 s heartbeat
  // tick. We drain the button queue first so a queued click is honoured
  // before we sleep on the task-notify.
  constexpr int64_t kAutoRotateMs = 30'000;
  int64_t last_heartbeat_ms = 0;

  while (true) {
    btclock::ButtonInput ev{};
    if (xQueueReceive(button_q, &ev, 0) == pdTRUE) {
      bool rotated = false;
      if (ev.event == btclock::ButtonEvent::kClick) {
        rotated = sm.NextScreen(MsNow());
      } else if (ev.event == btclock::ButtonEvent::kLongPress) {
        rotated = sm.PrevScreen(MsNow());
      }
      if (rotated && hub) sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
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
      last_heartbeat_ms = now_ms;
    }

    if (sm.MaybeAutoRotate(now_ms, kAutoRotateMs) && hub) {
      sm.Render(panels, fb_storage, fonts, hub->GetSnapshot());
      continue;
    }

    if (!hub) continue;
    const auto snap = hub->GetSnapshot();

    if (sm.ConsumeNewBlock(snap)) {
      btclock::PostLedEvent(btclock::LedEvent::kBlockFlash);
    }

    if (got != 0 && sm.ShouldRender(snap)) {
      sm.Render(panels, fb_storage, fonts, snap);
    }
  }
}
