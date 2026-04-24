#include "app/boot/factory_reset.hpp"

#include <string>
#include <vector>

#include "app/boot/helpers.hpp"
#include "app/screen_manager.hpp"
#include "data_core/snapshot.hpp"
#include "settings/factory_reset.hpp"

namespace btclock {

void DoFactoryReset(
    ScreenManager& sm,
    std::array<std::unique_ptr<EpdPanel>, btclock::board::kNumPanels>& panels,
    uint8_t (&fb_storage)[btclock::board::kNumPanels][16 * 296],
    const AppFonts& fonts,
    DataHub* hub) {
  const std::size_t n = static_cast<std::size_t>(btclock::board::kNumPanels);
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
    sm.Render(panels, fb_storage, fonts, DataSnapshot{});
  }
  settings::PerformFactoryReset();
}

}  // namespace btclock
