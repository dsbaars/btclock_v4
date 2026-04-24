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
//   slot 4         : Bitaxe hashrate       (currency-agnostic)
//   slot 5         : Bitaxe best diff      (currency-agnostic)
//   slot 6+3k      : Moscow time,  currencies[k]
//   slot 7+3k      : BTC price,    currencies[k]
//   slot 8+3k      : Market cap,   currencies[k]
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
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/rotation_timer.hpp"
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

  // Selects one of the 16 Satoshi Symbol glyphs (0..15) used on the
  // Moscow-time screen. Main reads the stored value from NVS at boot
  // and installs it here via ClampSatsVariant(). A future /api PATCH
  // will call this too — toggling on the fly requires MarkDirty() by
  // the caller so the next Render() repaints with the new glyph.
  void SetSatsVariant(uint8_t variant) { sats_variant_ = variant; }
  uint8_t sats_variant() const { return sats_variant_; }

  // Runtime-pushed custom screen — driven by POST /api/show/text and
  // /api/show/custom. `cells` has one string per panel (caller resizes
  // to the device's panel count). Switches the active screen to
  // ScreenType::kCustom so the next Render() paints the custom content
  // instead of the rotation's current slot. Until the user navigates
  // off it (button press, /api/screen/next, auto-rotate tick) the screen
  // stays latched — mirrors the old firmware's SCREEN_CUSTOM latch.
  void SetCustomCells(std::vector<std::string> cells, int64_t now_ms);

  // Push the transient Nostr-zap overlay on top of the current slot.
  // `now_ms` stamps the arrival; the deadline is `now_ms +
  // kZapTimeoutMs` unless scrnRestoreZap=false, in which case the
  // overlay stays latched until the user navigates off (same latch
  // semantics as kCustom). current_kind() returns kNostrZap while the
  // overlay is active (above kCustom, below kDebug in priority).
  // Called from main's on_zap callback after the snapshot's LatestZap
  // has been patched.
  void SetZapNotify(int64_t now_ms, bool auto_restore);

  // Navigation — always returns true today (the slot list always has
  // ≥ 2 entries). Sets the dirty flag so the next Render does a full
  // refresh.
  bool NextScreen(int64_t now_ms);
  bool PrevScreen(int64_t now_ms);

  // If `period_ms` has elapsed since the last navigation/rotation, step
  // to the next slot and return true. Data pushes do not reset timer.
  // No-op while `IsPaused()` — the data pipeline keeps running but the
  // screen holds its current slot, matching the old firmware's
  // pause-timer semantics.
  bool MaybeAutoRotate(int64_t now_ms, int64_t period_ms);

  // Freeze / resume auto-rotation. Button-driven Next/Prev still
  // works while paused so the user can step manually. Pause does not
  // stop data-source updates; the current slot still re-renders when
  // its bound data changes.
  void SetPaused(bool paused) { rot_.paused = paused; }
  bool IsPaused() const { return rot_.paused; }
  // Flip the pause bit and return the new state — convenience wrapper
  // the on-device button-1 handler uses. The HTTP layer can reuse this
  // so /api/action/pause becomes a single-call toggle.
  bool TogglePaused() {
    rot_.paused = !rot_.paused;
    return rot_.paused;
  }

  // --- Off-rotation debug overlay ---
  //
  // The on-device button-4 press enters this mode, displacing whatever
  // data screen was up; a second press returns to the same slot. Auto-
  // rotate is suppressed while IsDebug() is true and ShouldRender()
  // always returns false — the caller paints once via RenderDebug().
  bool IsDebug() const { return debug_mode_; }
  // Enter debug mode. Remembers the current slot so ExitDebug() lands
  // back on it. Idempotent.
  void EnterDebug(int64_t now_ms);
  // Leave debug mode and mark dirty so the main loop repaints the
  // underlying data screen. Restarts the auto-rotate deadline.
  void ExitDebug(int64_t now_ms);
  // One-call combinator for the button-4 press path.
  bool ToggleDebug(int64_t now_ms);

  // Reset the auto-rotate deadline so the next MaybeAutoRotate() call
  // needs a full `period_ms` to elapse before advancing. Used by
  // /api/action/timer_restart to keep the current slot on-screen for
  // another full period from "now". Does not change slot or dirty.
  void RestartTimer(int64_t now_ms) { rot_.Restart(now_ms); }

  // Predicate consulted by NextScreen / PrevScreen / MaybeAutoRotate
  // before committing a new slot. Return true to skip past the candidate
  // ScreenType so the step lands on the next visible slot instead. Used
  // today to hide the mining-pool earnings slot when the active pool is
  // a solo pool (no daily-sats stream). The predicate must terminate —
  // skip advancement stops after slot_count() iterations as a fallback
  // safety. Nullable: default predicate keeps every slot in the cycle.
  void SetSkipPredicate(std::function<bool(ScreenType)> pred) {
    skip_predicate_ = std::move(pred);
  }

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
  // No-op while IsDebug() — callers should use RenderDebug() in that
  // mode, so the debug overlay isn't stomped on a data push.
  template <size_t N>
  void Render(std::array<std::unique_ptr<EpdPanel>, N>& panels,
              uint8_t (&fb)[N][16 * 296],
              const AppFonts& fonts,
              const DataSnapshot& snap);

  // Paint the off-rotation debug screen. Always full-refresh. Sets
  // last_panel_texts_ to the debug mirror so /api/status `data[]`
  // reflects the active screen.
  template <size_t N>
  void RenderDebug(std::array<std::unique_ptr<EpdPanel>, N>& panels,
                   uint8_t (&fb)[N][16 * 296],
                   const AppFonts& fonts,
                   const DebugScreenInfo& info);

  // Per-panel text mirror of the current slot's on-EPD content, kept in
  // sync by Render(). Same shape the old firmware's /api/status `data[]`
  // produced: label in slot 0, digit / separator chars in the tail,
  // unit text in the last slot when applicable. Empty until the first
  // Render() call — /api/status handlers fall back to per-panel empties
  // in that case. See screens/panel_texts.hpp for the layout rules.
  const std::vector<std::string>& last_panel_texts() const {
    return last_panel_texts_;
  }

 public:
  // Notification visible-time window. Matches the old firmware's
  // screenRestoreAfterZap path which reuses `timerSeconds` (default
  // 10 s) — a shorter default here keeps the notification from
  // monopolising the panels while still giving the viewer time to
  // read the amount. Expressed in ms for the monotonic deadline.
  static constexpr int64_t kZapTimeoutMs = 8'000;

 private:
  // Currency-agnostic slots that stack ahead of the per-currency
  // slots. Block=0, Clock=1, Halving=2, BitcoinSupply=3,
  // MiningPoolHashrate=4, MiningPoolEarnings=5,
  // BitaxeHashrate=6, BitaxeBestDiff=7. Keep in lock-step with
  // slot_map::kAgnosticSlots — the ScreenManager's switch in
  // current_kind() is the direct consumer of the layout.
  static constexpr size_t kAgnosticSlots = 8;
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
  // Pause flag + last-advance timestamp live on RotationTimer so the
  // auto-rotate decision is a single pure-logic call (host-testable).
  RotationTimer rot_;
  uint32_t last_seen_height_ = 0;     // snapshot-side tracking (LED flash)
  uint32_t last_rendered_height_ = 0; // screen-side tracking (digit diff)
  std::string last_rendered_price_;   // shared Moscow-time + price diff
  double last_rendered_fee_ = -1.0;   // fee-rate screen diff (<0 = unknown)
  // Market-cap diff: price *and* height both feed into the output,
  // so we remember the last height that drove a cap render (distinct
  // from last_rendered_height_, which is the block-height-screen diff).
  uint32_t last_rendered_cap_height_ = 0;
  std::string last_rendered_cap_price_;
  // Mining-pool screen diff: the screen manager always keeps the
  // last-rendered snapshot around so the renderer can emit partial
  // updates (same pool logo + unit label, only digits change).
  DataSnapshot::PoolStats last_rendered_pool_hashrate_;
  DataSnapshot::PoolStats last_rendered_pool_earnings_;
  // Bitaxe screen diff — keep the last painted values so a repeated
  // poll with identical numbers doesn't repaint. Empty hostname means
  // "never rendered"; the first frame with data forces a full refresh
  // via the dirty flag the manager already tracks.
  std::string last_rendered_bitaxe_hashrate_;
  std::string last_rendered_bitaxe_best_diff_;
  // User-selectable sats-symbol glyph index (0..15). Default matches
  // kSatsVariantDefault so unit tests and code paths that construct a
  // manager without touching NVS render the documented glyph.
  uint8_t sats_variant_ = kSatsVariantDefault;
  // Clock screen diff: valid-bit + HH/MM/dd/mm.
  bool last_rendered_clock_valid_ = false;
  int last_rendered_clock_hour_ = -1;
  int last_rendered_clock_min_ = -1;
  int last_rendered_clock_mday_ = -1;
  int last_rendered_clock_mon_ = -1;
  // Custom-screen latch. `custom_active_` flips Render()'s dispatch to
  // RenderCustomScreen even though the rotation-slot is unchanged. The
  // latch clears on Next/Prev/SetSlot/SetCurrency/auto-rotate so the
  // user leaves it by any nav action. Sized to 8 (the largest N the
  // firmware ships); the renderer only reads the first N entries for
  // the board's actual panel count.
  bool custom_active_ = false;
  std::array<std::string, 8> custom_cells_{};
  std::array<std::string, 8> last_rendered_custom_cells_{};
  // Zap-notification overlay latch. `zap_active_` flips Render() into
  // the kNostrZap dispatch. When `zap_auto_restore_` is true the overlay
  // clears once `now_ms > zap_active_until_`; false keeps it latched
  // like kCustom. Cleared on any nav (Next/Prev/SetSlot/SetCurrency).
  bool zap_active_ = false;
  bool zap_auto_restore_ = true;
  int64_t zap_active_until_ = 0;
  // Text mirror refreshed on every Render(). Populated via
  // screens/panel_texts.hpp, consumed by ControlServer::HandleStatus
  // through the `get_panel_texts` callback wired in main.cpp.
  std::vector<std::string> last_panel_texts_;
  // True while the off-rotation debug overlay is up.
  bool debug_mode_ = false;
  // Runtime skip hook: invoked against the *would-be* next slot's kind
  // during rotation advance. Wired by main.cpp to the mining-pool
  // capability check so users who saved `screens[{id:71,enabled:true}]`
  // under Ocean don't suddenly see "0 SATS" when they switch to a solo
  // pool. Kept as an unconditional std::function (not an override) so a
  // follow-up test case can install a lambda without subclassing.
  std::function<bool(ScreenType)> skip_predicate_;
  // Internal: probe ScreenType for a given slot WITHOUT mutating state.
  // Mirrors current_kind()'s switch but takes the slot argument directly
  // so the auto-rotate / Next / Prev code can look ahead.
  ScreenType KindForSlot(size_t slot) const;
  // Internal: step `slot_` forward/backward past any slots the skip
  // predicate rejects. `direction` is +1 or -1. Bails out after
  // slot_count() steps so a misbehaving predicate can't infinite-loop.
  void AdvancePastSkipped(int direction);
};

}  // namespace btclock
