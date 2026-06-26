#include "io/network_led_watchdog.hpp"

#include "esp_log.h"
#include "io/led_controller.hpp"
#include "wifi.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "net-led";
}  // namespace

NetworkLedWatchdog::Tier NetworkLedWatchdog::ClassifyFault() const {
  // STA disconnect dominates — without WiFi the data probes can't be
  // trusted (the WS callbacks may report "connected" right up until
  // the next failed I/O), and a red wifi indicator is the only useful
  // signal anyway. (The connecting / waiting-for-data boot window never
  // reaches here — the event loop only ticks this watchdog after the boot
  // tail has finished; see the class comment.)
  if (wifi_ && wifi_->state() != Wifi::State::kConnected) return Tier::kWifi;

  // A missing probe is treated as "ok" — the fallback behaviour avoids
  // false positives during the boot-to-data-source-wired window and
  // when running on dataSource=0 (single v2 WS) where per-channel
  // probes don't exist yet (see init_control_api.cpp's connection-
  // status wiring + bd btclock_v4-1xc).
  const bool price_ok = !price_connected_ || price_connected_();
  const bool blocks_ok = !blocks_connected_ || blocks_connected_();
  if (!price_ok && !blocks_ok) return Tier::kMulti;
  if (!blocks_ok) return Tier::kBlock;
  if (!price_ok) return Tier::kPrice;
  return Tier::kNone;
}

uint32_t NetworkLedWatchdog::CadenceMs(Tier t) {
  switch (t) {
    case Tier::kWifi:
    case Tier::kMulti:
      // 2 s breath + 3 s gap reads as a steady "alive but unhappy"
      // rhythm without being too noisy.
      return 5'000;
    case Tier::kBlock:
    case Tier::kPrice:
      // ~600 ms blink pair every 10 s — enough that a user notices
      // within a screen-rotation cycle but not so frequent it strobes.
      return 10'000;
    case Tier::kNone:
      return 0;
  }
  return 0;
}

void NetworkLedWatchdog::PostEffectFor(Tier t) {
  switch (t) {
    case Tier::kWifi:
    case Tier::kMulti:
      PostLedEffect(LedEffect::kDataError);
      return;
    case Tier::kBlock:
      PostLedEffect(LedEffect::kDataBlockError);
      return;
    case Tier::kPrice:
      PostLedEffect(LedEffect::kDataPriceError);
      return;
    case Tier::kNone:
      // No-op on recovery: the previous one-shot's PaintResting tail
      // already restored the user's resting mirror, so the strip is
      // back where it should be.
      return;
  }
}

void NetworkLedWatchdog::Tick(uint32_t now_ms) {
  const Tier t = ClassifyFault();
  const bool was_red = IsRedBreath(current_tier_);
  const bool now_red = IsRedBreath(t);

  // Seed the grace clock on the leading edge of the red-breath class. A
  // kWifi<->kMulti hand-off stays "red" throughout, so it does NOT reseed
  // — the window measures continuous red-fault presence, not the tier.
  if (now_red && !was_red) red_fault_since_ms_ = now_ms;

  const bool tier_changed = (t != current_tier_);
  if (tier_changed) {
    if (t == Tier::kNone) {
      ESP_LOGI(kTag, "all clear");
    } else {
      ESP_LOGW(kTag, "fault tier=%u", static_cast<unsigned>(t));
    }
    current_tier_ = t;
  }

  if (t == Tier::kNone) return;

  if (ShouldPostIndicator(now_red, tier_changed, now_ms, red_fault_since_ms_,
                          last_post_ms_, kRedBreathGraceMs, CadenceMs(t))) {
    last_post_ms_ = now_ms;
    PostEffectFor(t);
  }
}

}  // namespace btclock
