// Screen rotation + render dispatch.
//
// Owns the currently-displayed slot (screen type × currency), the auto-
// rotate timer, and the "last rendered" bookkeeping that feeds the
// per-digit diff in the screen renderers. Main feeds it button events
// and snapshots; it decides when a re-render or LED flash is warranted.
//
// Slot model
// ==========
// Currency-agnostic slots come first, then the currency-specific ones
// fan out per active currency, with a trailing currency-agnostic fee-
// rate slot. With C currencies there are
// kAgnosticSlots + kPerCurrencySlots·C + 1 slots total:
//
//   slot 0         : Block height          (currency-agnostic)
//   slot 1         : Wall clock (HH:MM)    (currency-agnostic)
//   slot 2         : Halving countdown     (currency-agnostic)
//   slot 3         : Bitcoin supply        (currency-agnostic)
//   slot 4+3k      : Moscow time,  currencies[k]
//   slot 5+3k      : BTC price,    currencies[k]
//   slot 6+3k      : Market cap,   currencies[k]
//   slot last      : Block fee rate (sats/vB integer, currency-agnostic)
//
// Auto-rotate steps one slot per period; buttons cycle forward/back.
// The fee-rate slot sits at the end of the cycle so it naturally
// follows the last per-currency group — mirrors the old firmware's
// `DEFAULT_SCREEN_ORDER` where FEE_RATE comes after the currency
// screens.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data_core/snapshot.hpp"
#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"
#include "screens/screens.hpp"

namespace btclock {

class ScreenManager {
 public:
  // `currencies` must be non-empty (caller validates — typically passed
  // straight from the user's active-currency pref). `now_ms` seeds the
  // auto-rotate timer baseline.
  ScreenManager(int64_t now_ms, std::vector<std::string> currencies);

  ScreenType current_kind() const;
  const std::string& current_currency() const;  // "" for block screen

  // Current slot index (0 … slot_count()-1). Exposed for the HTTP
  // /api/status response which needs to echo back `currentScreen`.
  size_t current_slot() const { return slot_; }
  size_t slot_count_public() const { return slot_count(); }
  const std::vector<std::string>& currencies() const { return currencies_; }

  // Jump to a specific slot (wraps on overflow). Used by the HTTP
  // `/api/show/screen?s=<idx>` endpoint; button-driven navigation goes
  // through NextScreen/PrevScreen so it keeps its auto-rotate baseline
  // reset alongside the slot change.
  bool SetSlot(size_t slot, int64_t now_ms);

  // Jump to the first Moscow/price pair for a given currency code.
  // Returns false if the currency is not in the active set.
  bool SetCurrency(const std::string& ccy, int64_t now_ms);

  // Force the next Render() to do a full EPD refresh rather than the
  // digit-diff partial. Used by `POST /api/full_refresh`.
  void MarkDirty() { dirty_ = true; }

  // Navigation — always returns true today (the slot list always has
  // ≥ 2 entries). Sets the dirty flag so the next Render does a full
  // refresh.
  bool NextScreen(int64_t now_ms);
  bool PrevScreen(int64_t now_ms);

  // If `period_ms` has elapsed since the last navigation/rotation, step
  // to the next slot and return true. Data pushes do not reset timer.
  bool MaybeAutoRotate(int64_t now_ms, int64_t period_ms);

  // Decide whether the current slot should be re-rendered against
  // `snap`. True if dirty (navigation just happened) or if the snapshot
  // carries new data for the current slot. Idempotent.
  bool ShouldRender(const DataSnapshot& snap) const;

  // Detects a block-height change vs the last snapshot this manager
  // has seen (not vs the last one rendered). True iff the new height
  // differs AND the last seen height was non-zero. Updates seen-height.
  bool ConsumeNewBlock(const DataSnapshot& snap);

  // Render the current slot. Uses dirty to decide full vs partial
  // refresh; clears dirty after. Updates last-rendered bookkeeping.
  template <size_t N>
  void Render(std::array<std::unique_ptr<EpdPanel>, N>& panels,
              uint8_t (&fb)[N][16 * 296],
              const AppFonts& fonts,
              const DataSnapshot& snap);

 private:
  // Currency-agnostic slots that stack ahead of the per-currency
  // slots. Block=0, Clock=1, Halving=2, BitcoinSupply=3.
  static constexpr size_t kAgnosticSlots = 4;
  // Per-currency cycle: Moscow, Price, MarketCap.
  static constexpr size_t kPerCurrencySlots = 3;
  // Fee-rate is a singleton trailing slot at index slot_count() - 1.
  size_t slot_count() const {
    return kAgnosticSlots + kPerCurrencySlots * currencies_.size() + 1;
  }
  bool is_fee_rate_slot() const { return slot_ == slot_count() - 1; }

  std::vector<std::string> currencies_;
  size_t slot_ = 0;
  bool dirty_ = true;                 // first render is always full-refresh
  int64_t last_change_ms_ = 0;
  uint32_t last_seen_height_ = 0;     // snapshot-side tracking (LED flash)
  uint32_t last_rendered_height_ = 0; // screen-side tracking (digit diff)
  std::string last_rendered_price_;   // shared Moscow-time + price diff
  int32_t last_rendered_fee_ = -1;    // fee-rate screen diff (-1 = unknown)
  // Market-cap diff: price *and* height both feed into the output,
  // so we remember the last height that drove a cap render (distinct
  // from last_rendered_height_, which is the block-height-screen diff).
  uint32_t last_rendered_cap_height_ = 0;
  std::string last_rendered_cap_price_;
  // Clock screen diff: valid-bit + HH/MM/dd/mm.
  bool last_rendered_clock_valid_ = false;
  int last_rendered_clock_hour_ = -1;
  int last_rendered_clock_min_ = -1;
  int last_rendered_clock_mday_ = -1;
  int last_rendered_clock_mon_ = -1;
};

}  // namespace btclock
