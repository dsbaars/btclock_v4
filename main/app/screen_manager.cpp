#include "app/screen_manager.hpp"

#include <cassert>
#include <cmath>
#include <ctime>
#include <utility>

#include "esp_log.h"

#include "screens/common.hpp"
#include "screens/panel_texts.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "screens";
const std::string kEmptyCurrency;

// Anything older than 2020-01-01 00:00:00 UTC is either un-synced
// SNTP or a firmware fallback to build time — either way, not a
// wall-clock we want to paint.
constexpr time_t kMinPlausibleEpoch = 1577836800;

const char* KindName(ScreenType k) {
  switch (k) {
    case ScreenType::kBlockHeight:   return "block";
    case ScreenType::kMoscowTime:    return "moscow";
    case ScreenType::kBtcPrice:      return "price";
    case ScreenType::kBlockFeeRate:  return "fee";
    case ScreenType::kClock:         return "clock";
    case ScreenType::kHalving:       return "halving";
    case ScreenType::kBitcoinSupply: return "supply";
    case ScreenType::kMarketCap:     return "mcap";
  }
  return "?";
}
}  // namespace

ScreenManager::ScreenManager(int64_t now_ms,
                             std::vector<std::string> currencies)
    : currencies_(std::move(currencies)) {
  rot_.last_change_ms = now_ms;
  assert(!currencies_.empty());
}

ScreenType ScreenManager::current_kind() const {
  if (is_fee_rate_slot()) return ScreenType::kBlockFeeRate;
  switch (slot_) {
    case 0: return ScreenType::kBlockHeight;
    case 1: return ScreenType::kClock;
    case 2: return ScreenType::kHalving;
    case 3: return ScreenType::kBitcoinSupply;
    default: break;
  }
  // Per-currency stride: 0=moscow, 1=price, 2=mcap.
  const size_t off = (slot_ - kAgnosticSlots) % kPerCurrencySlots;
  switch (off) {
    case 0: return ScreenType::kMoscowTime;
    case 1: return ScreenType::kBtcPrice;
    case 2: return ScreenType::kMarketCap;
  }
  return ScreenType::kBlockHeight;
}

const std::string& ScreenManager::current_currency() const {
  if (slot_ < kAgnosticSlots || is_fee_rate_slot()) return kEmptyCurrency;
  const size_t ccy_idx = (slot_ - kAgnosticSlots) / kPerCurrencySlots;
  return currencies_[ccy_idx];
}

bool ScreenManager::SetSlot(size_t slot, int64_t now_ms) {
  const size_t n = slot_count();
  if (n == 0) return false;
  slot_ = slot % n;
  dirty_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "set → slot %zu", slot_);
  return true;
}

bool ScreenManager::SetCurrency(const std::string& ccy, int64_t now_ms) {
  // Land on the Moscow slot for the matching currency so auto-rotate
  // continues forward through Price and MarketCap next — same pattern
  // as the production firmware's setCurrentCurrency → setCurrentScreen
  // combo. Moscow is the first of the three per-currency slots.
  for (size_t i = 0; i < currencies_.size(); ++i) {
    if (currencies_[i] == ccy) {
      return SetSlot(kAgnosticSlots + kPerCurrencySlots * i, now_ms);
    }
  }
  return false;
}

bool ScreenManager::NextScreen(int64_t now_ms) {
  slot_ = (slot_ + 1) % slot_count();
  dirty_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "next → slot %zu (%s %s)", slot_,
           KindName(current_kind()), current_currency().c_str());
  return true;
}

bool ScreenManager::PrevScreen(int64_t now_ms) {
  slot_ = (slot_ + slot_count() - 1) % slot_count();
  dirty_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "prev → slot %zu", slot_);
  return true;
}

bool ScreenManager::MaybeAutoRotate(int64_t now_ms, int64_t period_ms) {
  // Pause + deadline decision lives on RotationTimer so it's pinned
  // by host tests. Mirrors the old firmware's /api/action/pause —
  // stops the rotate esp_timer without touching any data source.
  if (!rot_.ShouldAdvance(now_ms, period_ms)) return false;
  slot_ = (slot_ + 1) % slot_count();
  dirty_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "auto-rotate → slot %zu (%s %s)", slot_,
           KindName(current_kind()), current_currency().c_str());
  return true;
}

bool ScreenManager::ShouldRender(const DataSnapshot& snap) const {
  if (dirty_) return true;
  switch (current_kind()) {
    case ScreenType::kBlockHeight:
      return snap.block_height &&
             *snap.block_height != last_rendered_height_;
    case ScreenType::kMoscowTime:
    case ScreenType::kBtcPrice: {
      const auto* p = snap.PriceOf(current_currency());
      return p != nullptr && *p != last_rendered_price_;
    }
    case ScreenType::kBlockFeeRate: {
      // Prefer the precise double when available; fall back to the
      // integer. Compare with a small epsilon so floating-point noise
      // on an otherwise-identical value doesn't force a refresh.
      constexpr double kFeeEpsilon = 1e-3;
      double fee = -1.0;
      if (snap.block_fee_precise) fee = *snap.block_fee_precise;
      else if (snap.block_fee) fee = static_cast<double>(*snap.block_fee);
      if (fee < 0.0) return false;
      return std::fabs(fee - last_rendered_fee_) > kFeeEpsilon;
    }
    case ScreenType::kClock: {
      // Minute-granularity clock — the main loop's poll tick drives
      // re-render decisions: "render when the minute has flipped".
      time_t t;
      std::time(&t);
      if (t < kMinPlausibleEpoch) return !last_rendered_clock_valid_;
      struct tm tm_now {};
      localtime_r(&t, &tm_now);
      return !last_rendered_clock_valid_ ||
             tm_now.tm_min != last_rendered_clock_min_ ||
             tm_now.tm_hour != last_rendered_clock_hour_ ||
             tm_now.tm_mday != last_rendered_clock_mday_ ||
             tm_now.tm_mon != last_rendered_clock_mon_;
    }
    case ScreenType::kHalving:
    case ScreenType::kBitcoinSupply:
      return snap.block_height &&
             *snap.block_height != last_rendered_height_;
    case ScreenType::kMarketCap: {
      const auto* p = snap.PriceOf(current_currency());
      if (!p || !snap.block_height) return false;
      return *p != last_rendered_cap_price_ ||
             *snap.block_height != last_rendered_cap_height_;
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
  const ScreenType kind = current_kind();
  const std::string& ccy = current_currency();

  switch (kind) {
    case ScreenType::kBlockHeight:
      if (snap.block_height) {
        RenderBlockHeightScreen(panels, fb, fonts, *snap.block_height,
                                force_full ? 0 : last_rendered_height_);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kMoscowTime:
      if (const auto* p = snap.PriceOf(ccy)) {
        RenderMoscowTimeScreen(panels, fb, fonts, ccy, *p,
                               force_full ? "" : last_rendered_price_);
        last_rendered_price_ = *p;
      }
      break;
    case ScreenType::kBtcPrice:
      if (const auto* p = snap.PriceOf(ccy)) {
        RenderBtcPriceScreen(panels, fb, fonts, ccy, *p,
                             force_full ? "" : last_rendered_price_,
                             CurrencySymbolUtf8(ccy));
        last_rendered_price_ = *p;
      }
      break;
    case ScreenType::kBlockFeeRate: {
      // Prefer the precise double (blockfee2 / nostr d=medianFee) when
      // present; otherwise fall back to the rounded integer from the
      // `blockfee` subscription. -1 means "no value yet" — passed
      // through to the renderer so it paints blank digit panels instead
      // of lying about the data state.
      double fee;
      if (snap.block_fee_precise) fee = *snap.block_fee_precise;
      else if (snap.block_fee) fee = static_cast<double>(*snap.block_fee);
      else fee = -1.0;
      RenderFeeRateScreen(panels, fb, fonts, fee,
                          force_full ? -1.0 : last_rendered_fee_);
      last_rendered_fee_ = fee;
      break;
    }
    case ScreenType::kClock: {
      time_t t;
      std::time(&t);
      const bool valid = (t >= kMinPlausibleEpoch);
      struct tm tm_now {};
      if (valid) localtime_r(&t, &tm_now);
      RenderClockScreen(
          panels, fb, fonts, valid,
          valid ? tm_now.tm_hour : 0,
          valid ? tm_now.tm_min : 0,
          valid ? tm_now.tm_mday : 0,
          valid ? tm_now.tm_mon + 1 : 0,
          force_full ? false : last_rendered_clock_valid_,
          last_rendered_clock_hour_,
          last_rendered_clock_min_,
          last_rendered_clock_mday_,
          last_rendered_clock_mon_);
      last_rendered_clock_valid_ = valid;
      if (valid) {
        last_rendered_clock_hour_ = tm_now.tm_hour;
        last_rendered_clock_min_  = tm_now.tm_min;
        last_rendered_clock_mday_ = tm_now.tm_mday;
        last_rendered_clock_mon_  = tm_now.tm_mon;
      }
      break;
    }
    case ScreenType::kHalving:
      if (snap.block_height) {
        RenderHalvingScreen(panels, fb, fonts, *snap.block_height,
                            force_full ? 0 : last_rendered_height_);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kBitcoinSupply:
      if (snap.block_height) {
        RenderBitcoinSupplyScreen(panels, fb, fonts, *snap.block_height,
                                  force_full ? 0 : last_rendered_height_);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kMarketCap:
      if (const auto* p = snap.PriceOf(ccy); p && snap.block_height) {
        RenderMarketCapScreen(
            panels, fb, fonts, ccy, *p, *snap.block_height,
            force_full ? "" : last_rendered_cap_price_,
            force_full ? 0 : last_rendered_cap_height_);
        last_rendered_cap_price_ = *p;
        last_rendered_cap_height_ = *snap.block_height;
      }
      break;
  }
  dirty_ = false;
  ESP_LOGI(kTag, "render slot=%zu full=%d", slot_, force_full ? 1 : 0);

  // Refresh the per-panel text mirror for /api/status data[]. Cheap
  // string arithmetic only; safe to do every render. Callers that want
  // the mirror read it via last_panel_texts(). The real wall-clock
  // is re-sampled inside here for the clock screen so the mirror
  // matches what the renderer just painted.
  PanelTextInputs pti;
  pti.kind = kind;
  pti.currency = ccy;
  pti.block_height = snap.block_height;
  if (snap.block_fee_precise) pti.block_fee_sats_vb = *snap.block_fee_precise;
  else if (snap.block_fee) pti.block_fee_sats_vb =
      static_cast<double>(*snap.block_fee);
  if (const auto* p = snap.PriceOf(ccy)) pti.price = *p;
  if (kind == ScreenType::kClock) {
    // Match the renderer's clock-time source: localtime at render.
    // Guards against the unsynced-NTP fallback are done inside
    // BuildPanelTexts → ComputeClockLayout.
    time_t t;
    std::time(&t);
    pti.clock_valid = (t >= kMinPlausibleEpoch);
    if (pti.clock_valid) {
      struct tm tm_now {};
      localtime_r(&t, &tm_now);
      pti.hour = tm_now.tm_hour;
      pti.minute = tm_now.tm_min;
      pti.mday = tm_now.tm_mday;
      pti.month = tm_now.tm_mon + 1;
    }
  }
  last_panel_texts_ = BuildPanelTexts(pti, N);
}

template void ScreenManager::Render<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot&);
template void ScreenManager::Render<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot&);

}  // namespace btclock
