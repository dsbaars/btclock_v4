#include "app/screen_manager.hpp"

#include <cassert>
#include <cmath>
#include <ctime>
#include <utility>

#include "app/boot/helpers.hpp"
#include "app/rotation_plan.hpp"
#include "app/screen_slot_map.hpp"
#include "esp_log.h"
#include "prefs.hpp"
#include "screens/common.hpp"
#include "screens/panel_texts.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "screens";
const std::string kEmptyCurrency;

// Anything older than 2020-01-01 00:00:00 UTC is either un-synced
// SNTP or a firmware fallback to build time — either way, not a
// wall-clock we want to paint.
constexpr time_t kMinPlausibleEpoch = 1577836800;

// Bundle of renderer-behaviour flags sourced from the `settings`
// namespace. Read per-render so a live PATCH on any of these takes
// effect on the next paint without a reboot. NVS reads are cheap
// (cached by nvs.c) — a single render call reads ~6 keys, each a
// u8 lookup in the already-opened handle.
struct RenderPrefs {
  bool use_sats_symbol;
  bool use_mscw_time;
  bool use_blk_countdown;
  bool supply_percent;
  bool mcap_big_char;
  bool block_fee_dec;
  bool suffix_price;
  bool mow_mode;
  // suffixShareDot: when the suffix layout fits the K/M/B label form,
  // pack the decimal point into the slot before it so the digits get
  // one more slot of width (v3 parsePriceData shareDot branch). No
  // effect on the overflow path or the plain integer path.
  bool suffix_share_dot;
  // Clock screen: drop the leading zero on single-digit hours.
  bool hide_lead_zero;
  // verticalDesc: rotate label panels 90° CCW so "BLOCK/HEIGHT" etc.
  // read along the panel's long axis. Ports v3's verticalDesc pref
  // (see btclock_v3_fci/src/lib/drivers/epd/epd.cpp splitText).
  bool vertical_desc;
  // Refresh-policy inputs. refr_scrn_change forces full on every nav;
  // full_refresh_min is the ghost-clear cadence in minutes. Defaults
  // match v3 defaults.hpp (`DEFAULT_REFRESH_ON_SCREEN_CHANGE=false`,
  // `DEFAULT_MINUTES_FULL_REFRESH=60`).
  bool refr_scrn_change;
  int full_refresh_min;
};

RenderPrefs ReadRenderPrefs() {
  btclock::Prefs prefs(btclock::prefs::kSettingsNs);
  RenderPrefs out;
  out.use_sats_symbol = prefs.GetBool(btclock::prefs::kUseSatsSymbol, true);
  out.use_mscw_time = prefs.GetBool(btclock::prefs::kUseMscwTime, true);
  out.use_blk_countdown = prefs.GetBool(btclock::prefs::kUseBlkCountdown, true);
  out.supply_percent = prefs.GetBool(btclock::prefs::kSupplyPercent, false);
  out.mcap_big_char = prefs.GetBool(btclock::prefs::kMcapBigChar, true);
  out.block_fee_dec = prefs.GetBool(btclock::prefs::kBlockFeeDec, false);
  out.suffix_price = prefs.GetBool(btclock::prefs::kSuffixPrice, false);
  out.mow_mode = prefs.GetBool(btclock::prefs::kMowMode, false);
  out.suffix_share_dot = prefs.GetBool(btclock::prefs::kSuffixShareDot, false);
  out.hide_lead_zero = prefs.GetBool(btclock::prefs::kHideLeadZero, false);
  out.vertical_desc = prefs.GetBool(btclock::prefs::kVerticalDesc, false);
  out.refr_scrn_change = prefs.GetBool(btclock::prefs::kRefrScrnChange, false);
  out.full_refresh_min =
      static_cast<int>(prefs.GetU32(btclock::prefs::kFullRefreshMin, 60));
  return out;
}

const char* KindName(ScreenType k) {
  switch (k) {
    case ScreenType::kBlockHeight:
      return "block";
    case ScreenType::kMoscowTime:
      return "moscow";
    case ScreenType::kBtcPrice:
      return "price";
    case ScreenType::kBlockFeeRate:
      return "fee";
    case ScreenType::kClock:
      return "clock";
    case ScreenType::kHalving:
      return "halving";
    case ScreenType::kBitcoinSupply:
      return "supply";
    case ScreenType::kMarketCap:
      return "mcap";
    case ScreenType::kMiningPoolHashrate:
      return "poolhash";
    case ScreenType::kMiningPoolEarnings:
      return "poolearn";
    case ScreenType::kBitaxeHashrate:
      return "bxhash";
    case ScreenType::kBitaxeBestDiff:
      return "bxdiff";
    case ScreenType::kCustom:
      return "custom";
    case ScreenType::kDebug:
      return "debug";
    case ScreenType::kNostrZap:
      return "zap";
    case ScreenType::kOtaUpdate:
      return "ota";
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

ScreenType ScreenManager::KindForSlot(size_t slot) const {
  if (slot == slot_count() - 1) return ScreenType::kBlockFeeRate;
  switch (slot) {
    case 0:
      return ScreenType::kBlockHeight;
    case 1:
      return ScreenType::kClock;
    case 2:
      return ScreenType::kHalving;
    case 3:
      return ScreenType::kBitcoinSupply;
    case 4:
      return ScreenType::kMiningPoolHashrate;
    case 5:
      return ScreenType::kMiningPoolEarnings;
    case 6:
      return ScreenType::kBitaxeHashrate;
    case 7:
      return ScreenType::kBitaxeBestDiff;
    default:
      break;
  }
  // Per-currency stride: 0=moscow, 1=price, 2=mcap.
  const size_t off = (slot - kAgnosticSlots) % kPerCurrencySlots;
  switch (off) {
    case 0:
      return ScreenType::kMoscowTime;
    case 1:
      return ScreenType::kBtcPrice;
    case 2:
      return ScreenType::kMarketCap;
  }
  return ScreenType::kBlockHeight;
}

void ScreenManager::AdvancePastSkipped(int direction) {
  if (!skip_predicate_) return;
  const size_t n = slot_count();
  if (n == 0) return;
  // Bound the walk to slot_count() so a predicate that (incorrectly)
  // returns true for every kind leaves slot_ on a deterministic slot
  // instead of spinning.
  for (size_t guard = 0; guard < n; ++guard) {
    if (!skip_predicate_(KindForSlot(slot_))) return;
    if (direction >= 0) {
      slot_ = (slot_ + 1) % n;
    } else {
      slot_ = (slot_ + n - 1) % n;
    }
  }
}

std::size_t ScreenManager::IndexForSlot(std::size_t slot) const {
  for (std::size_t i = 0; i < rotation_sequence_.size(); ++i) {
    if (rotation_sequence_[i] == slot) return i;
  }
  return rotation_sequence_.size();
}

void ScreenManager::AdvanceInSequence(int direction) {
  const std::size_t n = rotation_sequence_.size();
  if (n == 0) return;
  // Starting from the current rotation_idx_, step in `direction` past any
  // slot the skip predicate rejects. Bounded by n so a misbehaving
  // predicate can't spin.
  for (std::size_t guard = 0; guard < n; ++guard) {
    const std::size_t candidate_slot = rotation_sequence_[rotation_idx_];
    const bool skip =
        skip_predicate_ && skip_predicate_(KindForSlot(candidate_slot));
    if (!skip) {
      slot_ = candidate_slot;
      return;
    }
    if (direction >= 0) {
      rotation_idx_ = (rotation_idx_ + 1) % n;
    } else {
      rotation_idx_ = (rotation_idx_ + n - 1) % n;
    }
  }
  // Every entry was skipped — land on the first anyway so the display
  // isn't left stale. Matches the AdvancePastSkipped guard behaviour.
  slot_ = rotation_sequence_[rotation_idx_];
}

void ScreenManager::SetRotationSequence(std::vector<std::size_t> sequence) {
  // Preserve the currently-displayed slot's position in the new sequence
  // when possible, so a live /api/settings PATCH that rebuilds the plan
  // doesn't yank the user off their current screen.
  rotation_sequence_ = std::move(sequence);
  rotation_idx_ = 0;
  if (!rotation_sequence_.empty()) {
    const std::size_t idx = IndexForSlot(slot_);
    rotation_idx_ = (idx < rotation_sequence_.size()) ? idx : 0;
  }
}

void ScreenManager::SetCurrencies(std::vector<std::string> currencies) {
  if (currencies.empty()) return;  // ctor invariant: at least one ccy
  // Remember the (kind, currency-code) the user is currently viewing
  // so the resize doesn't drop them onto a different screen unless the
  // code disappears from the new list. Per-currency slot indices change
  // when the count changes; we map kind+code → new slot below.
  const ScreenType kind_before = KindForSlot(slot_);
  std::string ccy_before;
  if (slot_ >= kAgnosticSlots && !is_fee_rate_slot()) {
    const std::size_t idx = (slot_ - kAgnosticSlots) / kPerCurrencySlots;
    if (idx < currencies_.size()) ccy_before = currencies_[idx];
  }
  currencies_ = std::move(currencies);
  // Re-anchor slot_ on the new layout. Currency-agnostic kinds keep the
  // same slot index. Per-currency kinds resolve via the kind+code pair —
  // if the previously-displayed code is no longer in the active set we
  // fall back to slot 0 (block height) rather than picking an arbitrary
  // currency, matching the boot path's "default to first slot" stance.
  // The fee-rate slot lives at slot_count() - 1, which moves on a resize.
  const std::size_t n = slot_count();
  if (kind_before == ScreenType::kBlockFeeRate) {
    slot_ = n - 1;
  } else if (slot_ >= n) {
    slot_ = 0;
  } else if (!ccy_before.empty()) {
    bool found = false;
    for (std::size_t i = 0; i < currencies_.size(); ++i) {
      if (currencies_[i] == ccy_before) {
        std::size_t off = 0;
        switch (kind_before) {
          case ScreenType::kMoscowTime:
            off = 0;
            break;
          case ScreenType::kBtcPrice:
            off = 1;
            break;
          case ScreenType::kMarketCap:
            off = 2;
            break;
          default:
            break;
        }
        slot_ = kAgnosticSlots + kPerCurrencySlots * i + off;
        found = true;
        break;
      }
    }
    if (!found) slot_ = 0;
  }
}

ScreenType ScreenManager::current_kind() const {
  // OTA push-upload overlay outranks every other kind — the user has
  // committed to replacing the running firmware and the EPD must read
  // "UPDATE!" from any angle until esp_restart fires.
  if (ota_active_) return ScreenType::kOtaUpdate;
  // Debug screen latches above everything else until button 4 clears.
  if (debug_mode_) return ScreenType::kDebug;
  // Zap notification outranks kCustom — a pending zap should interrupt
  // a static custom overlay too. The deadline check lives on the
  // MaybeAutoRotate / Render path rather than here so current_kind()
  // stays monotonic from one call to the next.
  if (zap_active_) return ScreenType::kNostrZap;
  // Custom screen latches over the rotation cycle — the client explicitly
  // pinned it via /api/show/text or /api/show/custom. Any navigation
  // (Next/Prev/SetSlot/SetCurrency/auto-rotate) clears the latch below.
  if (custom_active_) return ScreenType::kCustom;
  return KindForSlot(slot_);
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
  // Re-sync rotation_idx_ so a subsequent auto-rotate / Next picks up
  // from the slot the caller just jumped to. When the target isn't in
  // the rotation sequence (e.g. the user disabled that api_id but still
  // POSTs /api/show/screen?s=<id> to force-display it), leave
  // rotation_idx_ at 0 — the next Next step advances back into the
  // user's configured rotation rather than stranding us outside it.
  if (!rotation_sequence_.empty()) {
    const std::size_t idx = IndexForSlot(slot_);
    rotation_idx_ = (idx < rotation_sequence_.size()) ? idx : 0;
  }
  custom_active_ = false;
  zap_active_ = false;
  screen_change_pending_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "set → slot %zu", slot_);
  return true;
}

bool ScreenManager::SetKind(ScreenType kind, int64_t now_ms) {
  // Scan the slot map for the first slot whose KindForSlot matches.
  // Currency-bearing kinds (Moscow, price, market cap) resolve to the
  // first currency in the active list — stealFocus today only points
  // at kBlockHeight (slot 0), but keeping the scan generic lets a
  // future caller hop to any screen by kind.
  const size_t n = slot_count();
  for (size_t s = 0; s < n; ++s) {
    if (KindForSlot(s) == kind) {
      return SetSlot(s, now_ms);
    }
  }
  return false;
}

bool ScreenManager::SetCurrency(const std::string& ccy, int64_t now_ms) {
  // Preserve the user's per-currency position via slot_map so POST
  // /api/show/currency from Market Cap USD → Market Cap EUR (not
  // Moscow Time EUR). Slot math lives in slot_map so it can be unit
  // tested without pulling in ScreenManager's IDF deps.
  for (size_t i = 0; i < currencies_.size(); ++i) {
    if (currencies_[i] == ccy) {
      const size_t target =
          slot_map::TransposeSlotToCurrency(slot_, i, currencies_.size());
      return SetSlot(target, now_ms);
    }
  }
  return false;
}

void ScreenManager::SetCustomCells(std::vector<std::string> cells,
                                   int64_t now_ms) {
  // Copy up to 8 entries (static upper bound in custom_cells_). Shorter
  // inputs leave trailing cells empty so the renderer blanks those
  // panels rather than leaving them showing stale content from the
  // previously-active rotation screen.
  for (std::size_t i = 0; i < custom_cells_.size(); ++i) {
    custom_cells_[i] = (i < cells.size()) ? std::move(cells[i]) : std::string();
  }
  custom_active_ = true;
  zap_active_ = false;
  screen_change_pending_ = true;
  // Reset the auto-rotate deadline so the user gets a full rotation
  // period to read the custom content before it rolls off.
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "custom → %zu cells latched", custom_cells_.size());
}

void ScreenManager::SetZapNotify(int64_t now_ms, bool auto_restore,
                                 int64_t timeout_ms) {
  zap_active_ = true;
  zap_auto_restore_ = auto_restore;
  // Caller-driven deadline when positive — mirrors the user's
  // `timerSeconds` rotation pref so the overlay dismisses on the same
  // cadence as a rotation step. Guard against 0/negative (e.g. an
  // unset pref) with the documented default so the zap doesn't vanish
  // before the viewer can read it.
  const int64_t t = (timeout_ms > 0) ? timeout_ms : kZapTimeoutMs;
  zap_active_until_ = now_ms + t;
  screen_change_pending_ = true;
  // Hold off rotation advance while the notification is up; the deadline
  // check in MaybeAutoRotate will bounce on zap_active_ so this is
  // belt-and-braces.
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "zap notify → auto_restore=%d timeout=%lld until=%lld",
           auto_restore ? 1 : 0, static_cast<long long>(t),
           static_cast<long long>(zap_active_until_));
}

bool ScreenManager::NextScreen(int64_t now_ms) {
  // When the custom latch is active, Next lands on the *current* rotation
  // slot (unchanged slot_) rather than advancing. Matches the old
  // firmware: leaving SCREEN_CUSTOM via a button press returned the user
  // to the previously-visible rotation screen, not the slot after it.
  // The zap overlay gets the same "first Next dismisses, stays on slot"
  // treatment so a long-latched (scrnRestoreZap=false) notification
  // exits cleanly on a button press.
  const bool exiting_overlay = custom_active_ || zap_active_;
  custom_active_ = false;
  zap_active_ = false;
  if (rotation_sequence_.empty()) {
    if (!exiting_overlay) slot_ = (slot_ + 1) % slot_count();
    AdvancePastSkipped(+1);
  } else {
    if (!exiting_overlay) {
      rotation_idx_ = (rotation_idx_ + 1) % rotation_sequence_.size();
    }
    AdvanceInSequence(+1);
  }
  screen_change_pending_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "next → slot %zu (%s %s)", slot_, KindName(current_kind()),
           current_currency().c_str());
  return true;
}

bool ScreenManager::PrevScreen(int64_t now_ms) {
  const bool exiting_overlay = custom_active_ || zap_active_;
  custom_active_ = false;
  zap_active_ = false;
  if (rotation_sequence_.empty()) {
    if (!exiting_overlay) slot_ = (slot_ + slot_count() - 1) % slot_count();
    AdvancePastSkipped(-1);
  } else {
    const std::size_t n = rotation_sequence_.size();
    if (!exiting_overlay) rotation_idx_ = (rotation_idx_ + n - 1) % n;
    AdvanceInSequence(-1);
  }
  screen_change_pending_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "prev → slot %zu", slot_);
  return true;
}

bool ScreenManager::MaybeAutoRotate(int64_t now_ms, int64_t period_ms) {
  // OTA overlay freezes rotation — a mid-flash auto-rotate tick would
  // try to repaint the EPD from the main task while the HTTP worker
  // still owns it.
  if (ota_active_) return false;
  // Debug overlay suppresses auto-rotate — the user entered it via
  // button 4 and expects it to stay up until they press again.
  if (debug_mode_) return false;
  // Zap notification: auto-exit on deadline when scrnRestoreZap=true
  // (default). Returns true so the caller re-renders the restored
  // slot immediately rather than waiting for a data push. When
  // zap_auto_restore_ is false the overlay stays up until a user
  // nav clears it (same semantics as kCustom at a long enough window).
  if (zap_active_) {
    if (zap_auto_restore_ && now_ms > zap_active_until_) {
      zap_active_ = false;
      screen_change_pending_ = true;
      rot_.Restart(now_ms);
      ESP_LOGI(kTag, "zap restore → slot %zu (%s)", slot_,
               KindName(current_kind()));
      return true;
    }
    return false;
  }
  // Pause + deadline decision lives on RotationTimer so it's pinned
  // by host tests. Mirrors the old firmware's /api/action/pause —
  // stops the rotate esp_timer without touching any data source.
  if (!rot_.ShouldAdvance(now_ms, period_ms)) return false;
  // Auto-rotate also exits the custom-screen latch; the next rotation
  // tick after a /api/show/text call returns the display to the normal
  // cycle. Old firmware keeps SCREEN_CUSTOM latched across rotation
  // ticks — we intentionally diverge here because the production WebUI
  // doesn't expect a runtime-pushed override to stay on-screen forever.
  const bool exiting_custom = custom_active_;
  custom_active_ = false;
  if (rotation_sequence_.empty()) {
    if (!exiting_custom) slot_ = (slot_ + 1) % slot_count();
    AdvancePastSkipped(+1);
  } else {
    if (!exiting_custom) {
      rotation_idx_ = (rotation_idx_ + 1) % rotation_sequence_.size();
    }
    AdvanceInSequence(+1);
  }
  screen_change_pending_ = true;
  rot_.Restart(now_ms);
  ESP_LOGI(kTag, "auto-rotate → slot %zu (%s %s)", slot_,
           KindName(current_kind()), current_currency().c_str());
  return true;
}

bool ScreenManager::ShouldRender(const DataSnapshot& snap) const {
  // OTA overlay: the HTTP worker painted it once and owns the EPD for
  // the duration of the write. The main render loop must stay out so a
  // data push can't stomp the "UPDATE!" screen mid-flash.
  if (ota_active_) return false;
  // Debug overlay is painted via RenderDebug() on entry / value
  // change; the data-driven render path mustn't stomp it.
  if (debug_mode_) return false;
  // Either force-full (MarkDirty / first render) or a pending nav
  // event drives a repaint — the refresh-policy layer decides whether
  // the paint itself is full or partial, but ShouldRender is just
  // "something changed, re-draw".
  if (dirty_ || screen_change_pending_) return true;
  // minSecPriceUpd: gate price-bearing screens on a min-elapsed window
  // since the last EPD price write. Burn-protection — the WS price
  // stream can fire several times a second on volatile candles; without
  // this the EPD would repaint that often. Pref read per-call so a live
  // PATCH applies on the next data push without a reboot. The `dirty_`
  // / `screen_change_pending_` early-out above means nav and force-full
  // bypass the throttle so user input always paints.
  auto price_throttle_blocks = [&]() {
    Prefs throttle_prefs(btclock::prefs::kSettingsNs);
    const uint32_t min_s =
        throttle_prefs.GetU32(btclock::prefs::kMinSecPriceUpd, 30);
    if (min_s == 0) return false;
    const int64_t elapsed_ms = MsNow() - last_price_apply_ms_;
    return elapsed_ms < static_cast<int64_t>(min_s) * 1000;
  };
  switch (current_kind()) {
    case ScreenType::kBlockHeight:
      return snap.block_height && *snap.block_height != last_rendered_height_;
    case ScreenType::kMoscowTime:
    case ScreenType::kBtcPrice: {
      const auto* p = snap.PriceOf(current_currency());
      if (p == nullptr || *p == last_rendered_price_) return false;
      return !price_throttle_blocks();
    }
    case ScreenType::kBlockFeeRate: {
      // Prefer the precise double when available; fall back to the
      // integer. Compare with a small epsilon so floating-point noise
      // on an otherwise-identical value doesn't force a refresh.
      constexpr double kFeeEpsilon = 1e-3;
      double fee = -1.0;
      if (snap.block_fee_precise)
        fee = *snap.block_fee_precise;
      else if (snap.block_fee)
        fee = static_cast<double>(*snap.block_fee);
      if (fee < 0.0) return false;
      return std::fabs(fee - last_rendered_fee_) > kFeeEpsilon;
    }
    case ScreenType::kClock: {
      // Minute-granularity clock — the main loop's poll tick drives
      // re-render decisions: "render when the minute has flipped".
      time_t t;
      std::time(&t);
      if (t < kMinPlausibleEpoch) return !last_rendered_clock_valid_;
      struct tm tm_now{};
      localtime_r(&t, &tm_now);
      return !last_rendered_clock_valid_ ||
             tm_now.tm_min != last_rendered_clock_min_ ||
             tm_now.tm_hour != last_rendered_clock_hour_ ||
             tm_now.tm_mday != last_rendered_clock_mday_ ||
             tm_now.tm_mon != last_rendered_clock_mon_;
    }
    case ScreenType::kHalving:
    case ScreenType::kBitcoinSupply:
      return snap.block_height && *snap.block_height != last_rendered_height_;
    case ScreenType::kMarketCap: {
      const auto* p = snap.PriceOf(current_currency());
      if (!p || !snap.block_height) return false;
      const bool height_changed =
          *snap.block_height != last_rendered_cap_height_;
      const bool price_changed = *p != last_rendered_cap_price_;
      if (!height_changed && !price_changed) return false;
      // Block-height changes always paint (block source is rare and
      // visually informative); only the price-only delta is throttled.
      if (!height_changed && price_changed && price_throttle_blocks()) {
        return false;
      }
      return true;
    }
    case ScreenType::kMiningPoolHashrate:
      return snap.pool.hashrate != last_rendered_pool_hashrate_.hashrate ||
             snap.pool.name != last_rendered_pool_hashrate_.name;
    case ScreenType::kMiningPoolEarnings:
      return snap.pool.daily_sats != last_rendered_pool_earnings_.daily_sats ||
             snap.pool.name != last_rendered_pool_earnings_.name;
    case ScreenType::kBitaxeHashrate: {
      // Re-render when the formatted value string differs from what
      // we last painted — lets the bitaxe poller's every-10-s tick
      // drive refresh even when the underlying double wobbles by a
      // tiny amount that rounds to the same display string.
      const std::string v =
          (snap.bitaxe.hostname.empty() || !snap.bitaxe.hashrate_ghs)
              ? std::string("OFFLINE")
              : FormatBitaxeHashrate(*snap.bitaxe.hashrate_ghs);
      return v != last_rendered_bitaxe_hashrate_;
    }
    case ScreenType::kBitaxeBestDiff: {
      const std::string v =
          (snap.bitaxe.hostname.empty() || !snap.bitaxe.best_diff ||
           snap.bitaxe.best_diff->empty())
              ? std::string("OFFLINE")
              : *snap.bitaxe.best_diff;
      return v != last_rendered_bitaxe_best_diff_;
    }
    case ScreenType::kCustom:
      // The custom screen is push-driven: SetCustomCells always flips
      // screen_change_pending_ so ShouldRender() returns true via the
      // early-out above. If we reach this branch without either flag,
      // there's no data source that would change content → no
      // re-render needed.
      return false;
    case ScreenType::kNostrZap:
      // Same as kCustom — SetZapNotify sets screen_change_pending_ on
      // arrival; further paints are only driven by nav/timeout, not
      // snapshot changes.
      return false;
    case ScreenType::kDebug:
      // Unreachable — the debug_mode_ short-circuit above fires first.
      return false;
    case ScreenType::kOtaUpdate:
      // Unreachable — ota_active_ short-circuits at function entry.
      return false;
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
                           uint8_t (&fb)[N][16 * 296], const AppFonts& fonts,
                           const DataSnapshot& snap) {
  // OTA overlay: the HTTP worker painted via RenderOtaUpdateScreen and
  // owns the panels until esp_restart. Bail so the main loop can't
  // stomp the overlay on a data push that slipped through.
  if (ota_active_) return;
  // Debug overlay renders via RenderDebug(); skip the data path so a
  // stray caller doesn't stomp the diagnostic screen.
  if (debug_mode_) return;
  const ScreenType kind = current_kind();
  const std::string& ccy = current_currency();
  const RenderPrefs rp = ReadRenderPrefs();
  // Route the full-vs-partial decision through the policy so
  // refrScrnChange + fullRefreshMin are honoured. MarkDirty() maps to
  // is_force_full; navigation events (Next/Prev/SetSlot/etc.) map to
  // is_screen_change. Same-screen data pushes fall into neither and
  // therefore render partial — the cheap path.
  //
  // Two independent signals drive the render (see btclock_v4-jo6):
  //   `force_full` → EPD refresh kind (RefreshKind::kFull / kPartial).
  //                  Passed through as each renderer's
  //                  `full_refresh_mode` argument.
  //   `force_repaint` → cell-diff reset. Forces every cell to repaint
  //                     this frame regardless of value-equality.
  //                     Wired via sentinel prev_value arguments.
  // Screen transitions always force repaint (the new screen's state
  // might accidentally equal the last-rendered value of the outgoing
  // screen) but only force a full EPD refresh when the policy says to.
  const int64_t now_ms_policy = MsNow();
  const bool force_full = RefreshPolicy::Decide(
      refresh_state_, now_ms_policy,
      /*is_screen_change=*/screen_change_pending_,
      /*is_force_full=*/dirty_, rp.refr_scrn_change, rp.full_refresh_min);

  // Force cell-diff reset on any transition so the renderer repaints
  // the new screen's content. Without this, POST /api/show/screen?s=0
  // appears to no-op when the block height hasn't changed since the
  // screen was last shown (or since boot — default last_rendered_*
  // zero values happen to match real data). Includes `force_full`
  // because kFull without per-cell repaint would leave update[] masks
  // stale for cells whose value happens to match the previous frame.
  const bool force_repaint = force_full || screen_change_pending_ || dirty_;

  switch (kind) {
    case ScreenType::kBlockHeight:
      if (snap.block_height) {
        RenderBlockHeightScreen(panels, fb, fonts, *snap.block_height,
                                force_repaint ? 0 : last_rendered_height_,
                                force_full, rp.vertical_desc);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kMoscowTime:
      if (const auto* p = snap.PriceOf(ccy)) {
        RenderMoscowTimeScreen(panels, fb, fonts, ccy, *p,
                               force_repaint ? "" : last_rendered_price_,
                               sats_variant_, rp.use_sats_symbol,
                               rp.use_mscw_time, force_full, rp.vertical_desc);
        last_rendered_price_ = *p;
        last_price_apply_ms_ = now_ms_policy;
      }
      break;
    case ScreenType::kBtcPrice:
      if (const auto* p = snap.PriceOf(ccy)) {
        RenderBtcPriceScreen(panels, fb, fonts, ccy, *p,
                             force_repaint ? "" : last_rendered_price_,
                             CurrencySymbolUtf8(ccy), rp.suffix_price,
                             rp.mow_mode, rp.suffix_share_dot, force_full,
                             rp.vertical_desc);
        last_rendered_price_ = *p;
        last_price_apply_ms_ = now_ms_policy;
      }
      break;
    case ScreenType::kBlockFeeRate: {
      // Source selection honours blockFeeDec: the pref decides whether
      // the fee cell shows whole sats/vB (rounded integer from the
      // `blockfee` topic) or two-decimal precision (`blockfee2`). Old
      // firmware's v2_notify.cpp had the same gate. -1 means "no value
      // yet" — passed through so digit panels paint blank rather than
      // misrepresenting the data state.
      double fee;
      if (rp.block_fee_dec && snap.block_fee_precise) {
        fee = *snap.block_fee_precise;
      } else if (snap.block_fee) {
        fee = static_cast<double>(*snap.block_fee);
      } else if (snap.block_fee_precise) {
        // Fall back to the precise field when the integer one hasn't
        // arrived yet — the value rounds to an integer inside
        // LayoutFeeRate so the non-decimal visual is preserved.
        fee = *snap.block_fee_precise;
      } else {
        fee = -1.0;
      }
      RenderFeeRateScreen(panels, fb, fonts, fee,
                          force_repaint ? -1.0 : last_rendered_fee_, force_full,
                          rp.vertical_desc);
      last_rendered_fee_ = fee;
      break;
    }
    case ScreenType::kClock: {
      time_t t;
      std::time(&t);
      const bool valid = (t >= kMinPlausibleEpoch);
      struct tm tm_now{};
      if (valid) localtime_r(&t, &tm_now);
      RenderClockScreen(panels, fb, fonts, valid, valid ? tm_now.tm_hour : 0,
                        valid ? tm_now.tm_min : 0, valid ? tm_now.tm_mday : 0,
                        valid ? tm_now.tm_mon + 1 : 0,
                        force_repaint ? false : last_rendered_clock_valid_,
                        last_rendered_clock_hour_, last_rendered_clock_min_,
                        last_rendered_clock_mday_, last_rendered_clock_mon_,
                        force_full, rp.vertical_desc, rp.hide_lead_zero);
      last_rendered_clock_valid_ = valid;
      if (valid) {
        last_rendered_clock_hour_ = tm_now.tm_hour;
        last_rendered_clock_min_ = tm_now.tm_min;
        last_rendered_clock_mday_ = tm_now.tm_mday;
        last_rendered_clock_mon_ = tm_now.tm_mon;
      }
      break;
    }
    case ScreenType::kHalving:
      if (snap.block_height) {
        RenderHalvingScreen(panels, fb, fonts, *snap.block_height,
                            force_repaint ? 0 : last_rendered_height_,
                            rp.use_blk_countdown, force_full, rp.vertical_desc);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kBitcoinSupply:
      if (snap.block_height) {
        // mcapBigChar drives supply's big-chars form too — matches the
        // v3_fci screen_handler.cpp call site (parseBitcoinSupply gets
        // the mcapBigChar pref as its `bigChars` argument).
        RenderBitcoinSupplyScreen(panels, fb, fonts, *snap.block_height,
                                  force_repaint ? 0 : last_rendered_height_,
                                  rp.mcap_big_char, rp.supply_percent,
                                  force_full, rp.vertical_desc);
        last_rendered_height_ = *snap.block_height;
      }
      break;
    case ScreenType::kMarketCap:
      if (const auto* p = snap.PriceOf(ccy); p && snap.block_height) {
        RenderMarketCapScreen(panels, fb, fonts, ccy, *p, *snap.block_height,
                              force_repaint ? "" : last_rendered_cap_price_,
                              force_repaint ? 0 : last_rendered_cap_height_,
                              rp.mcap_big_char, rp.suffix_share_dot, force_full,
                              rp.vertical_desc);
        last_rendered_cap_price_ = *p;
        last_rendered_cap_height_ = *snap.block_height;
        last_price_apply_ms_ = now_ms_policy;
      }
      break;
    case ScreenType::kMiningPoolHashrate: {
      // Always render — even with an empty pool snapshot the screen
      // shows a blank pool-label area + "0 H/S" placeholder rather
      // than whatever stale content was on the panels. Keeps the slot
      // navigable even when the user toggles miningPoolStats off at
      // runtime.
      const DataSnapshot::PoolStats& prev = force_repaint
                                                ? DataSnapshot::PoolStats{}
                                                : last_rendered_pool_hashrate_;
      RenderMiningPoolHashrateScreen(panels, fb, fonts, snap.pool, prev,
                                     force_full, rp.vertical_desc);
      last_rendered_pool_hashrate_ = snap.pool;
      break;
    }
    case ScreenType::kMiningPoolEarnings: {
      const DataSnapshot::PoolStats& prev = force_repaint
                                                ? DataSnapshot::PoolStats{}
                                                : last_rendered_pool_earnings_;
      RenderMiningPoolEarningsScreen(panels, fb, fonts, snap.pool, prev,
                                     force_full, rp.vertical_desc);
      last_rendered_pool_earnings_ = snap.pool;
      break;
    }
    case ScreenType::kBitaxeHashrate: {
      RenderBitaxeHashrateScreen(
          panels, fb, fonts, snap.bitaxe.hostname, snap.bitaxe.hashrate_ghs,
          force_full, force_repaint ? "" : last_rendered_bitaxe_hashrate_,
          rp.vertical_desc);
      last_rendered_bitaxe_hashrate_ =
          (snap.bitaxe.hostname.empty() || !snap.bitaxe.hashrate_ghs)
              ? std::string("OFFLINE")
              : FormatBitaxeHashrate(*snap.bitaxe.hashrate_ghs);
      break;
    }
    case ScreenType::kBitaxeBestDiff: {
      RenderBitaxeBestDiffScreen(
          panels, fb, fonts, snap.bitaxe.hostname, snap.bitaxe.best_diff,
          force_full, force_repaint ? "" : last_rendered_bitaxe_best_diff_,
          rp.vertical_desc);
      last_rendered_bitaxe_best_diff_ =
          (snap.bitaxe.hostname.empty() || !snap.bitaxe.best_diff ||
           snap.bitaxe.best_diff->empty())
              ? std::string("OFFLINE")
              : *snap.bitaxe.best_diff;
      break;
    }
    case ScreenType::kCustom: {
      // Copy the first N persistent cells into an N-sized array so the
      // templated renderer (which takes std::array<string, N>) can bind.
      // Using a local keeps the "last rendered" bookkeeping identically
      // sized to the live state — ensures the diff sees stale cells as
      // dirty on re-entry.
      std::array<std::string, N> live{};
      std::array<std::string, N> prev{};
      for (std::size_t i = 0; i < N; ++i) {
        live[i] = custom_cells_[i];
        prev[i] = last_rendered_custom_cells_[i];
      }
      RenderCustomScreen(panels, fb, fonts, live, prev, force_repaint,
                         force_full);
      for (std::size_t i = 0; i < N; ++i) {
        last_rendered_custom_cells_[i] = custom_cells_[i];
      }
      break;
    }
    case ScreenType::kNostrZap:
      RenderNostrZapScreen(panels, fb, fonts, snap.latest_zap,
                           rp.use_sats_symbol, sats_variant_, force_full,
                           rp.vertical_desc);
      break;
    case ScreenType::kDebug:
      // Unreachable — the debug_mode_ short-circuit at function entry
      // returns before this switch executes.
      break;
    case ScreenType::kOtaUpdate:
      // Unreachable — the ota_active_ short-circuit at function entry
      // returns before this switch executes.
      break;
  }
  dirty_ = false;
  screen_change_pending_ = false;
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
  // Mirror the fee-source selection the EPD renderer just used so the
  // /api/status data[] integer cells line up with what the panels paint.
  if (rp.block_fee_dec && snap.block_fee_precise) {
    pti.block_fee_sats_vb = *snap.block_fee_precise;
  } else if (snap.block_fee) {
    pti.block_fee_sats_vb = static_cast<double>(*snap.block_fee);
  } else if (snap.block_fee_precise) {
    pti.block_fee_sats_vb = *snap.block_fee_precise;
  }
  if (const auto* p = snap.PriceOf(ccy)) pti.price = *p;
  // Mirror the active mining-pool snapshot so /api/status data[] matches
  // what the renderer just painted on the mining-pool slots. Fields are
  // no-ops for the non-pool screen kinds.
  pti.pool.name = snap.pool.name;
  pti.pool.hashrate = snap.pool.hashrate;
  pti.pool.daily_sats = snap.pool.daily_sats;
  // Decoration flags: read per-render (cheap) so a PATCH lands live.
  pti.halving_as_blocks = rp.use_blk_countdown;
  pti.supply_big_chars = rp.mcap_big_char;
  pti.supply_percent = rp.supply_percent;
  pti.mcap_big_chars = rp.mcap_big_char;
  pti.use_sats_symbol = rp.use_sats_symbol;
  pti.use_mscw_time = rp.use_mscw_time;
  pti.suffix_price = rp.suffix_price;
  pti.mow_mode = rp.mow_mode;
  pti.share_dot = rp.suffix_share_dot;
  pti.hide_lead_zero = rp.hide_lead_zero;
  // Bitaxe mirror fields. pti copies the raw snapshot values so the
  // panel_texts builder formats identically to what the EPD renderer
  // just painted; both share FormatBitaxeHashrate.
  pti.bitaxe_hostname = snap.bitaxe.hostname;
  pti.bitaxe_hashrate_ghs = snap.bitaxe.hashrate_ghs;
  pti.bitaxe_best_diff = snap.bitaxe.best_diff;
  if (kind == ScreenType::kClock) {
    // Match the renderer's clock-time source: localtime at render.
    // Guards against the unsynced-NTP fallback are done inside
    // BuildPanelTexts → ComputeClockLayout.
    time_t t;
    std::time(&t);
    pti.clock_valid = (t >= kMinPlausibleEpoch);
    if (pti.clock_valid) {
      struct tm tm_now{};
      localtime_r(&t, &tm_now);
      pti.hour = tm_now.tm_hour;
      pti.minute = tm_now.tm_min;
      pti.mday = tm_now.tm_mday;
      pti.month = tm_now.tm_mon + 1;
    }
  }
  if (kind == ScreenType::kCustom) {
    // The custom screen's panels show caller-supplied strings verbatim;
    // /api/status `data[]` echoes exactly that rather than a
    // synthesised label+digit layout. Short-circuits the BuildPanelTexts
    // switch which has no synthetic rule for runtime-pushed content.
    last_panel_texts_.clear();
    last_panel_texts_.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
      last_panel_texts_.push_back(custom_cells_[i]);
    }
  } else if (kind == ScreenType::kNostrZap) {
    // Zap mirror: feed the snapshot's LatestZap into BuildNostrZap
    // so /api/status sees the same data the panels are painting.
    pti.zap_amount_sats = snap.latest_zap.amount_sats;
    pti.zap_message = snap.latest_zap.message;
    last_panel_texts_ = BuildPanelTexts(pti, N);
  } else {
    last_panel_texts_ = BuildPanelTexts(pti, N);
  }
}

template void ScreenManager::Render<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DataSnapshot&);
template void ScreenManager::Render<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DataSnapshot&);

void ScreenManager::EnterDebug(int64_t /*now_ms*/) {
  // Remember nothing explicit — slot_ is not touched on entry, so the
  // exit path simply turns debug_mode_ off and re-renders the
  // underlying slot. Matches the brief's "press 4 from data → debug,
  // press 4 from debug → back to rotation".
  debug_mode_ = true;
}

void ScreenManager::ExitDebug(int64_t now_ms) {
  debug_mode_ = false;
  dirty_ = true;
  rot_.Restart(now_ms);
}

bool ScreenManager::ToggleDebug(int64_t now_ms) {
  if (debug_mode_) {
    ExitDebug(now_ms);
  } else {
    EnterDebug(now_ms);
  }
  return debug_mode_;
}

template <size_t N>
void ScreenManager::RenderDebug(
    std::array<std::unique_ptr<EpdPanel>, N>& panels,
    uint8_t (&fb)[N][16 * 296], const AppFonts& fonts,
    const DebugScreenInfo& info, int64_t now_ms, bool force_full) {
  // Consult the shared RefreshPolicy so the overlay's auto-refresh
  // honours the user's fullRefreshMin pref: a full clear lands at most
  // once per fullRefreshMin minutes, with the 10 s ticks in between
  // running as silent partial refreshes.
  const RenderPrefs rp = ReadRenderPrefs();
  const bool full_refresh = RefreshPolicy::Decide(
      refresh_state_, now_ms,
      /*is_screen_change=*/force_full,
      /*is_force_full=*/force_full, rp.refr_scrn_change, rp.full_refresh_min);
  RenderDebugScreen(panels, fb, fonts, info, full_refresh);
  // Invalidate per-screen diff state so the slot we return to does a
  // full refresh rather than trying to paint a partial against the
  // debug layout.
  dirty_ = true;
  last_rendered_height_ = 0;
  last_rendered_price_.clear();
  last_rendered_fee_ = -1.0;
  last_rendered_cap_height_ = 0;
  last_rendered_cap_price_.clear();
  last_rendered_pool_hashrate_ = DataSnapshot::PoolStats{};
  last_rendered_pool_earnings_ = DataSnapshot::PoolStats{};
  last_rendered_clock_valid_ = false;
  last_rendered_bitaxe_hashrate_.clear();
  last_rendered_bitaxe_best_diff_.clear();
  ESP_LOGI(kTag, "render debug");

  // Panel-text mirror: just the label + blanks. The debug screen
  // doesn't map to the data[] slot layout the WebUI expects.
  PanelTextInputs pti;
  pti.kind = ScreenType::kDebug;
  last_panel_texts_ = BuildPanelTexts(pti, N);
}

template void ScreenManager::RenderDebug<7>(
    std::array<std::unique_ptr<EpdPanel>, 7>&, uint8_t (&)[7][16 * 296],
    const AppFonts&, const DebugScreenInfo&, int64_t, bool);
template void ScreenManager::RenderDebug<8>(
    std::array<std::unique_ptr<EpdPanel>, 8>&, uint8_t (&)[8][16 * 296],
    const AppFonts&, const DebugScreenInfo&, int64_t, bool);

}  // namespace btclock
