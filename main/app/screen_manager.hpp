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
//   slot 0          : Block height           (currency-agnostic)
//   slot 1          : Wall clock (HH:MM)     (currency-agnostic)
//   slot 2          : Halving countdown      (currency-agnostic)
//   slot 3          : Bitcoin supply         (currency-agnostic)
//   slot 4          : Mining pool hashrate   (currency-agnostic)
//   slot 5          : Mining pool earnings   (currency-agnostic)
//   slot 6          : Bitaxe hashrate        (currency-agnostic)
//   slot 7          : Bitaxe best diff       (currency-agnostic)
//   slot 8          : NWC wallet balance     (currency-agnostic)
//   slot 9+3k       : Moscow time,  currencies[k]
//   slot 10+3k      : BTC price,    currencies[k]
//   slot 11+3k      : Market cap,   currencies[k]
//   slot last       : Block fee rate (sats/vB integer, currency-agnostic)
//
// kAgnosticSlots MUST match slot_map::kAgnosticSlots — the rotation
// builder (rotation_plan::BuildRotationSequence) emits slot indices
// using the slot_map constant, and ScreenManager consumes them via
// KindForSlot below. A drift between the two (e.g. forgetting to
// bump ScreenManager when slot_map grows) shifts every per-currency
// slot by the delta, so the user sees BTC ticker where the Moscow
// time screen should be (bd btclock_v4-oni).
//
// Auto-rotate steps one slot per period; buttons cycle forward/back.
// The fee-rate slot sits at the end of the cycle so it naturally
// follows the last per-currency group — mirrors the old firmware's
// `DEFAULT_SCREEN_ORDER` where FEE_RATE comes after the currency
// screens.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/refresh_policy.hpp"
#include "app/rotation_timer.hpp"
#include "app/screen_slot_map.hpp"
#include "data_core/snapshot.hpp"
#include "epd/panel.hpp"
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
  // body-first `/api/show/screen` (`{"s":<idx>}`) endpoint; button-driven
  // navigation goes
  // through NextScreen/PrevScreen so it keeps its auto-rotate baseline
  // reset alongside the slot change.
  bool SetSlot(size_t slot, int64_t now_ms);

  // Jump to the first slot matching `kind`. Used by the stealFocus
  // branch of the new-block event in event_loop.cpp so a mined block
  // pops the block-height screen to the front. Returns true if a slot
  // with that kind was found and selected; false otherwise (caller
  // falls back to leaving the current slot up). Marks dirty so the
  // next Render() paints the target with a full refresh rather than a
  // digit-diff against a different screen.
  bool SetKind(ScreenType kind, int64_t now_ms);

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
  // `now_ms` stamps the arrival; the deadline is `now_ms + timeout_ms`
  // when `auto_restore=true` (scrnRestoreZap pref on), otherwise the
  // overlay stays latched until the user navigates off (same latch
  // semantics as kCustom). `timeout_ms <= 0` uses kZapTimeoutMs as a
  // guard against a 0-second rotation timer, which would auto-dismiss
  // the zap before the user sees it. current_kind() returns kNostrZap
  // while the overlay is active (above kCustom, below kDebug in
  // priority). Called from main's on_zap callback after the snapshot's
  // LatestZap has been patched.
  void SetZapNotify(int64_t now_ms, bool auto_restore, int64_t timeout_ms);

  // Push the transient NWC payment-notification overlay. Same latch
  // semantics as SetZapNotify; `timeout_ms <= 0` uses kZapTimeoutMs
  // (the two overlay kinds share the budget). current_kind() returns
  // kNwcPaymentNotify while the overlay is active.
  void SetNwcPaymentNotify(int64_t now_ms, bool auto_restore,
                           int64_t timeout_ms);

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

  // --- Firmware OTA overlay ---
  //
  // Latched by the /upload/firmware handler on the httpd worker before
  // esp_ota_begin erases the partition. While active, current_kind()
  // returns ScreenType::kOtaUpdate (outranks kDebug + every other
  // override), ShouldRender() returns false so a data push can't paint
  // over the overlay, and MaybeAutoRotate() is a no-op. The HTTP
  // worker is responsible for painting the overlay once via
  // RenderOtaUpdateScreen — the main loop stays out of the renderer
  // entirely while this is active, which avoids needing a separate EPD
  // mutex for the brief window.
  bool IsOtaActive() const { return ota_active_.load(); }
  void SetOtaOverlay(bool active) { ota_active_.store(active); }

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

  // Install the user's auto-rotate traversal sequence. Each entry is a
  // slot index; rotation (Next/Prev/MaybeAutoRotate) walks this list
  // rather than the dense slot_map order.
  //
  // Sourced from rotation_plan::BuildRotationSequence which combines
  // `screenOrder` NVS + `screen<id>Visible` toggles + the active currency
  // list. Pass an empty vector to fall back to "walk every slot in index
  // order" — the screen manager treats that as equivalent to a freshly-
  // flashed device with no screenOrder yet.
  //
  // Safe to call at runtime: rebuilds internal state, preserves the
  // currently-displayed slot if it still appears in the new sequence.
  void SetRotationSequence(std::vector<std::size_t> sequence);

  // Read-only accessor for the rotation plan. Used by the boot-time
  // resume path so it can validate a persisted lastSlot against the
  // freshly-built sequence before restoring the cursor.
  const std::vector<std::size_t>& rotation_sequence() const {
    return rotation_sequence_;
  }

  // Replace the active currency list at runtime. Used by the
  // on_screens_changed hook when PATCH /api/settings updates
  // `actCurrencies` so the per-currency slot expansion follows the new
  // set without a reboot. Caller must call SetRotationSequence with a
  // freshly built plan immediately afterwards — slot_count() depends on
  // currencies_.size() and a stale rotation_sequence_ would point at
  // out-of-range slots after a shrink. The currently-displayed slot is
  // preserved when its kind+currency-index still maps inside the new
  // layout; otherwise it clamps to slot 0 (block-height) so the next
  // render lands on a valid slot. Empty input is rejected (matches the
  // constructor invariant) — callers fall back to ["USD"] in that case.
  void SetCurrencies(std::vector<std::string> currencies);

  // Decide whether the current slot should be re-rendered against
  // `snap`. True if dirty (navigation just happened) or if the snapshot
  // carries new data for the current slot. Idempotent.
  bool ShouldRender(const DataSnapshot& snap) const;

  // Detects a block-height change vs the last snapshot this manager
  // has seen (not vs the last one rendered). True iff the new height
  // differs AND the last seen height was non-zero. Updates seen-height.
  // When `out_prev_height` is non-null, fills it with the height the
  // manager was tracking before this call — paired with the snapshot's
  // new height the caller can detect catch-up jumps via
  // BlockEventPolicy::IsCatchUpJump (suppresses the LED flash, the
  // frontlight pulse, and the stealFocus yank when the device is just
  // catching up to chain tip after being offline).
  bool ConsumeNewBlock(const DataSnapshot& snap,
                       uint32_t* out_prev_height = nullptr);

  // Seed `last_seen_height_` from persisted runtime state so the very
  // first WS frame after a reboot can detect "this is a new block I
  // missed while offline" instead of silently swallowing it (the
  // ConsumeNewBlock debounce gates on `prev != 0`). Called once at
  // boot from init_screen_manager after the runtime-state NVS read.
  // No effect when `height == 0` (cold device, never persisted).
  void SeedLastSeenHeight(uint32_t height) { last_seen_height_ = height; }
  uint32_t last_seen_height() const { return last_seen_height_; }

  // Render the current slot. Uses dirty to decide full vs partial
  // refresh; clears dirty after. Updates last-rendered bookkeeping.
  // No-op while IsDebug() — callers should use RenderDebug() in that
  // mode, so the debug overlay isn't stomped on a data push.
  template <size_t N>
  void Render(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
              uint8_t (&fb)[N][16 * 296], const AppFonts& fonts,
              const DataSnapshot& snap);

  // Paint the off-rotation debug screen. `force_full=true` (debug entry
  // / first paint) pins a full refresh; `force_full=false` (the 10 s
  // auto-tick) consults RefreshPolicy so partial refreshes ride between
  // mandatory fullRefreshMin-driven full clears. Sets last_panel_texts_
  // to the debug mirror so /api/status `data[]` reflects the overlay.
  template <size_t N>
  void RenderDebug(std::array<std::unique_ptr<epd::IEpdPanel>, N>& panels,
                   uint8_t (&fb)[N][16 * 296], const AppFonts& fonts,
                   const DebugScreenInfo& info, int64_t now_ms,
                   bool force_full);

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
  // BitaxeHashrate=6, BitaxeBestDiff=7, NwcBalance=8. Sourced
  // from slot_map::kAgnosticSlots so the rotation builder (which
  // emits indices using the slot_map constant) and the ScreenManager
  // switch in KindForSlot() can never drift apart — a stale-by-one
  // copy here would shift every per-currency slot's interpretation
  // by the delta. bd btclock_v4-oni (slot 9..N rendered as the
  // wrong kind after NWC balance was added).
  static constexpr size_t kAgnosticSlots = slot_map::kAgnosticSlots;
  // Per-currency cycle: Moscow, Price, MarketCap.
  static constexpr size_t kPerCurrencySlots = slot_map::kPerCurrencySlots;
  // Fee-rate is a singleton trailing slot at index slot_count() - 1.
  size_t slot_count() const {
    return kAgnosticSlots + kPerCurrencySlots * currencies_.size() + 1;
  }
  bool is_fee_rate_slot() const { return slot_ == slot_count() - 1; }

  std::vector<std::string> currencies_;
  size_t slot_ = 0;
  // `dirty_` = caller explicitly wants the next paint to be a full
  // refresh, regardless of the refrScrnChange / fullRefreshMin policy.
  // Set by MarkDirty() (/api/full_refresh + invertedColor PATCH),
  // debug-overlay exit, first construction. Cleared after each paint.
  bool dirty_ = true;
  // `screen_change_pending_` = the next paint follows a nav event
  // (Next/Prev/SetSlot/SetCurrency/MaybeAutoRotate/SetCustomCells/
  // SetZapNotify). Feeds the RefreshPolicy screen-change branch so the
  // user's refrScrnChange=false + fullRefreshMin window is honoured
  // without stomping on MarkDirty()-driven force-fulls. Also forces
  // ShouldRender() to return true once so a nav with no new data still
  // repaints.
  bool screen_change_pending_ = false;
  // Per-manager refresh-policy state: monotonic-ms of the last full
  // refresh. Decide() stamps it on every full paint so screed change +
  // fullRefreshMin resets alongside MarkDirty()-driven fulls.
  RefreshPolicyState refresh_state_;
  // Pause flag + last-advance timestamp live on RotationTimer so the
  // auto-rotate decision is a single pure-logic call (host-testable).
  RotationTimer rot_;
  uint32_t last_seen_height_ = 0;      // snapshot-side tracking (LED flash)
  uint32_t last_rendered_height_ = 0;  // screen-side tracking (digit diff)
  std::string last_rendered_price_;    // shared Moscow-time + price diff
  // minSecPriceUpd (seconds): monotonic-ms timestamp of the last price-
  // bearing screen paint (kMoscowTime / kBtcPrice / kMarketCap). Read
  // by ShouldRender to throttle EPD writes when the WS pushes prices
  // faster than `minSecPriceUpd` — protects the e-paper from rapid
  // burn-in. Mutable so the const-qualified ShouldRender can stamp it,
  // but the actual stamp lives in Render() after the paint completes.
  mutable int64_t last_price_apply_ms_ = 0;
  double last_rendered_fee_ = -1.0;  // fee-rate screen diff (<0 = unknown)
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
  DataSnapshot::PoolStats last_rendered_pool_estimated_earnings_;
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
  // NWC payment-notification overlay latch. Mirrors the zap overlay
  // shape: a kNwcPaymentNotify dispatch flips Render() into the payment
  // screen; auto-restore lifts the latch after the timeout. Two
  // separate latches (rather than a generic "overlay" state) so the
  // two notification kinds keep distinct timeouts and so a zap during
  // an NWC notification (or vice versa) replaces cleanly without
  // interleaving the two payloads in current_kind().
  bool nwc_notify_active_ = false;
  bool nwc_notify_auto_restore_ = true;
  int64_t nwc_notify_active_until_ = 0;
  // NWC balance screen diff — feeds the per-cell partial-refresh path
  // on RenderNwcBalanceScreen so a refreshed get_balance response only
  // repaints the cells whose digit content actually changed.
  std::optional<int64_t> last_rendered_nwc_balance_;
  // Text mirror refreshed on every Render(). Populated via
  // screens/panel_texts.hpp, consumed by ControlServer::HandleStatus
  // through the `get_panel_texts` callback wired in main.cpp.
  std::vector<std::string> last_panel_texts_;
  // True while the off-rotation debug overlay is up.
  bool debug_mode_ = false;
  // True while a firmware OTA push upload is streaming bytes to flash.
  // The OTA worker (auto-update task / push-OTA httpd worker) sets
  // this through SetOtaOverlay before the EPD render handoff; the
  // main task reads it via ShouldRender / MaybeAutoRotate / current_kind
  // to short-circuit the data-driven render path. Made std::atomic so
  // the cross-task write is well-defined; non-atomic int writes on
  // Xtensa LX7 are atomic in practice but formally UB, and the
  // sanitizer build correctly flagged it as a data race.
  std::atomic<bool> ota_active_{false};
  // Runtime skip hook: invoked against the *would-be* next slot's kind
  // during rotation advance. Wired by main.cpp to the mining-pool
  // capability check so users who saved `screens[{id:71,enabled:true}]`
  // under Ocean don't suddenly see "0 SATS" when they switch to a solo
  // pool. Kept as an unconditional std::function (not an override) so a
  // follow-up test case can install a lambda without subclassing.
  std::function<bool(ScreenType)> skip_predicate_;
  // User-editable traversal sequence. Empty → "walk every slot in index
  // order" (pre-screenOrder fallback). Non-empty → rotate only through
  // these slot indices, in this order.
  std::vector<std::size_t> rotation_sequence_;
  // Index into rotation_sequence_ for the currently-displayed slot. Kept
  // in sync whenever `slot_` changes (SetSlot, NextScreen, etc.) so auto-
  // rotate resumes from "next in sequence" even after a direct HTTP jump
  // to a slot outside the sequence.
  std::size_t rotation_idx_ = 0;
  // Internal: resolve the sequence index for the given slot. Returns
  // rotation_sequence_.size() (end) when the slot isn't in the sequence
  // — the caller treats that as "rotation resumes from index 0".
  std::size_t IndexForSlot(std::size_t slot) const;
  // Internal: advance the rotation index by +1 / -1 (wrapping) past any
  // slots the skip predicate rejects. Updates slot_ to the resolved
  // target. Bails out after rotation_sequence_.size() steps so a
  // misbehaving predicate can't infinite-loop.
  void AdvanceInSequence(int direction);
  // Internal: probe ScreenType for a given slot WITHOUT mutating state.
  // Mirrors current_kind()'s switch but takes the slot argument directly
  // so the auto-rotate / Next / Prev code can look ahead.
  ScreenType KindForSlot(size_t slot) const;
  // Internal: step `slot_` forward/backward past any slots the skip
  // predicate rejects. `direction` is +1 or -1. Bails out after
  // slot_count() steps so a misbehaving predicate can't infinite-loop.
  // Used only on the legacy "no rotation sequence" path.
  void AdvancePastSkipped(int direction);
};

}  // namespace btclock
