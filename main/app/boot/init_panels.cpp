#include "app/boot/init_panels.hpp"

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "board/board.hpp"
#include "boot_ui.hpp"
#include "epd/factory.hpp"
#include "epd_ssd1680.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "btclock";
}  // namespace

void InitPanelsAndSplash(AppCtx& ctx) {
  using namespace btclock::board;

  // --- SSD1680 bus + panels ---
  auto make_pin = [&](PinSource src, int pin_or_index) -> EpdIoPin {
    // `PS` is an Xtensa register macro — don't use it as an alias.
    using Src = PinSource;
    switch (src) {
      case Src::kNative:
        return EpdIoPin::Native(static_cast<gpio_num_t>(pin_or_index));
      case Src::kMcp1:
        return EpdIoPin::Mcp(&*ctx.mcp, static_cast<uint8_t>(pin_or_index));
      case Src::kMcp2:
        return EpdIoPin::Mcp(ctx.mcp2 ? &*ctx.mcp2 : nullptr,
                             static_cast<uint8_t>(pin_or_index));
    }
    return {};
  };

  ctx.epd_bus.emplace(SPI2_HOST, kEpdSpiSclk, kEpdSpiMosi, kEpdDc,
                      4 * 1000 * 1000, 16 * 296 + 64);
  // Driver dispatch is compile-time inside epd::CreatePanel — the
  // factory keys off BTCLOCK_PANEL_<X> from the top-level CMakeLists.
  // Default is GDEY0213B74 (2.13", 122x250); -DBTCLOCK_PANEL=2_9 swaps
  // to GDEY029T94 (2.9", 128x296), -DBTCLOCK_PANEL=7_5 to GDEY075T7
  // (7.5", 800x480). Pin straps (CS/DC/RST/BUSY) come from BoardConfig
  // and are panel-agnostic, so any board × any panel configures.
  // Multi-panel boards (V8) hold one driver instance per CS line, all
  // bound to the same EpdBus.
  for (int i = 0; i < kNumPanels; ++i) {
    epd::PanelConfig pcfg = {};
    pcfg.bus = &*ctx.epd_bus;
    pcfg.cs = make_pin(kEpdCsSource, kEpdCs[i]);
    pcfg.busy = make_pin(kEpdBusySource, kEpdBusy[i]);
    pcfg.reset = make_pin(kEpdResetSource, kEpdResetMcp[i]);
    ctx.panels[i] = epd::CreatePanel(pcfg);
  }
  for (auto& p : ctx.panels) ESP_ERROR_CHECK(p->Init());

  // Install the EPD polarity flag BEFORE the splash paints. InitStorage
  // does this again later (that's the canonical call site — it's also
  // where TZ / LittleFS come online), but splash runs before storage
  // init so the persisted invertedColor value would otherwise miss the
  // first paint. Prefs::InitOnce is idempotent, and a corrupted NVS
  // falls back to the schema default — same polarity the first data
  // render would pick, so the "brick the screen on NVS corruption"
  // concern that motivated the original ordering doesn't regress.
  (void)Prefs::InitOnce();
  {
    Prefs p(prefs::kSettingsNs);
    EpdSetGlobalInverted(btclock::settings::ReadBool(p, prefs::kInvertedColor));
  }

  // --- Boot splash — letter-per-panel BTCLOCK[!]. ---
  const int64_t t0 = MsNow();
  RenderSplashScreen(ctx.panels, AppCtx::fb_storage(), ctx.fonts);
  ESP_LOGI(kTag, "splash pass %lld ms", MsNow() - t0);
}

}  // namespace btclock
