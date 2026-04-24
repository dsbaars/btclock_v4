#include "app/boot/init_panels.hpp"

#include "app/app_ctx.hpp"
#include "app/boot/helpers.hpp"
#include "board/board.hpp"
#include "boot_ui.hpp"
#include "esp_err.h"
#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "poc";
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
        return EpdIoPin::Mcp(&*ctx.mcp,
                             static_cast<uint8_t>(pin_or_index));
      case Src::kMcp2:
        return EpdIoPin::Mcp(ctx.mcp2 ? &*ctx.mcp2 : nullptr,
                             static_cast<uint8_t>(pin_or_index));
    }
    return {};
  };

  ctx.epd_bus.emplace(SPI2_HOST, kEpdSpiSclk, kEpdSpiMosi, kEpdDc,
                      4 * 1000 * 1000, 16 * 296 + 64);
  for (int i = 0; i < kNumPanels; ++i) {
    EpdPanel::Config cfg = {};
    cfg.bus = &*ctx.epd_bus;
    cfg.cs = make_pin(kEpdCsSource, kEpdCs[i]);
    cfg.busy = make_pin(kEpdBusySource, kEpdBusy[i]);
    cfg.reset = make_pin(kEpdResetSource, kEpdResetMcp[i]);
    cfg.kind = PanelKind::k2_13;
    ctx.panels[i] = std::make_unique<EpdPanel>(cfg);
  }
  for (auto& p : ctx.panels) ESP_ERROR_CHECK(p->Init());

  // --- Boot splash — letter-per-panel BTCLOCK[!]. ---
  const int64_t t0 = MsNow();
  RenderSplashScreen(ctx.panels, AppCtx::fb_storage(), ctx.fonts);
  ESP_LOGI(kTag, "splash pass %lld ms", MsNow() - t0);
}

}  // namespace btclock
