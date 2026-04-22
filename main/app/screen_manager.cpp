#include "app/screen_manager.hpp"

#include "esp_log.h"

namespace btclock {
namespace {
constexpr const char* kTag = "screens";

ScreenType Next(ScreenType s) {
  switch (s) {
    case ScreenType::kBlockHeight: return ScreenType::kMoscowTime;
    case ScreenType::kMoscowTime:  return ScreenType::kBtcPrice;
    case ScreenType::kBtcPrice:    return ScreenType::kBlockHeight;
  }
  return ScreenType::kBlockHeight;
}
ScreenType Prev(ScreenType s) {
  switch (s) {
    case ScreenType::kBlockHeight: return ScreenType::kBtcPrice;
    case ScreenType::kMoscowTime:  return ScreenType::kBlockHeight;
    case ScreenType::kBtcPrice:    return ScreenType::kMoscowTime;
  }
  return ScreenType::kBlockHeight;
}
}  // namespace

ScreenManager::ScreenManager(int64_t now_ms) : last_change_ms_(now_ms) {}

bool ScreenManager::NextScreen(int64_t now_ms) {
  current_ = Next(current_);
  dirty_ = true;
  last_change_ms_ = now_ms;
  ESP_LOGI(kTag, "next → screen %d", static_cast<int>(current_));
  return true;
}

bool ScreenManager::PrevScreen(int64_t now_ms) {
  current_ = Prev(current_);
  dirty_ = true;
  last_change_ms_ = now_ms;
  ESP_LOGI(kTag, "prev → screen %d", static_cast<int>(current_));
  return true;
}

bool ScreenManager::MaybeAutoRotate(int64_t now_ms, int64_t period_ms) {
  if (now_ms - last_change_ms_ < period_ms) return false;
  current_ = Next(current_);
  dirty_ = true;
  last_change_ms_ = now_ms;
  ESP_LOGI(kTag, "auto-rotate → screen %d", static_cast<int>(current_));
  return true;
}

bool ScreenManager::ShouldRender(const DataSnapshot& snap) const {
  if (dirty_) return true;
  switch (current_) {
    case ScreenType::kBlockHeight:
      return snap.block_height &&
             *snap.block_height != last_rendered_height_;
    case ScreenType::kMoscowTime:
    case ScreenType::kBtcPrice: {
      const auto* usd = snap.PriceOf("USD");
      return usd != nullptr && *usd != last_rendered_price_;
    }
  }
  return false;
}

bool ScreenManager::ConsumeNewBlock(const DataSnapshot& snap) {
  if (!snap.block_height) return false;
  const uint32_t h = *snap.block_height;
  const bool is_new = last_seen_height_ != 0 && h != last_seen_height_;
  last_seen_height_ = h;
  return is_new;
}

template <size_t N>
void ScreenManager::Render(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                           uint8_t (&fb)[N][16 * 296],
                           const AppFonts& fonts,
                           const DataSnapshot& snap) {
  const bool force_full = dirty_;
  switch (current_) {
    case ScreenType::kBlockHeight:
      if (snap.block_height) {
        RenderBlockHeightScreen(panels, fb, fonts, *snap.block_height,
                                force_full ? 0 : last_rendered_height_);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kMoscowTime:
      if (const auto* usd = snap.PriceOf("USD")) {
        RenderMoscowTimeScreen(panels, fb, fonts, "USD", *usd,
                               force_full ? "" : last_rendered_price_);
        last_rendered_price_ = *usd;
      }
      break;
    case ScreenType::kBtcPrice:
      if (const auto* usd = snap.PriceOf("USD")) {
        RenderBtcPriceScreen(panels, fb, fonts, "USD", *usd,
                             force_full ? "" : last_rendered_price_);
        last_rendered_price_ = *usd;
      }
      break;
  }
  dirty_ = false;
  ESP_LOGI(kTag, "render screen=%d full=%d", static_cast<int>(current_),
           force_full ? 1 : 0);
}

template void ScreenManager::Render<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot&);
template void ScreenManager::Render<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot&);

}  // namespace btclock
