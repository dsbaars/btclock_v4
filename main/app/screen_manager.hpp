// Screen rotation + render dispatch.
//
// Owns the currently-displayed screen, the auto-rotate timer, and the
// "last rendered" bookkeeping needed for the per-digit diff the screen
// renderers use. Main feeds it button events and snapshots; it decides
// when a re-render or LED flash is warranted.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "data_core/snapshot.hpp"
#include "epd_ssd1680.hpp"
#include "fonts_app.hpp"
#include "screens/screens.hpp"

namespace btclock {

class ScreenManager {
 public:
  // Called at boot with the current monotonic-ms time so the auto-
  // rotate cycle starts from a sane baseline.
  explicit ScreenManager(int64_t now_ms);

  ScreenType current() const { return current_; }

  // Navigation — return true if the screen actually changed (always
  // true today since we only have 3 screens; keeps the contract open
  // to future no-op policies like "don't cycle to a screen with no
  // data"). Sets the dirty flag so the next Render does a full refresh.
  bool NextScreen(int64_t now_ms);
  bool PrevScreen(int64_t now_ms);

  // If `period_ms` has elapsed since the last navigation/rotation, step
  // to the next screen and return true. Otherwise returns false. Data
  // pushes do not reset the timer.
  bool MaybeAutoRotate(int64_t now_ms, int64_t period_ms);

  // Decide whether the current screen should be re-rendered against
  // `snap`. Returns true if the screen is dirty (navigation/rotation
  // just happened) or if the snapshot carries new data for the current
  // screen. Idempotent — no state mutation.
  bool ShouldRender(const DataSnapshot& snap) const;

  // Detects a block-height change vs the last snapshot this manager
  // has seen (not vs the last one rendered — two different concerns).
  // Returns true iff the new height differs from the last seen height
  // AND the last seen height was non-zero (skipping the cold-boot
  // first-ever value). Updates the seen-height regardless.
  bool ConsumeNewBlock(const DataSnapshot& snap);

  // Render the current screen. Uses the dirty flag to decide full vs
  // partial refresh; clears it after. Updates last-rendered bookkeeping.
  // The panel-count N is pinned to the board at compile time; explicit
  // instantiations live in screen_manager.cpp.
  template <size_t N>
  void Render(std::array<std::unique_ptr<EpdPanel>, N>& panels,
              uint8_t (&fb)[N][16 * 296],
              const AppFonts& fonts,
              const DataSnapshot& snap);

 private:
  ScreenType current_ = ScreenType::kBlockHeight;
  bool dirty_ = true;                 // first render is always full-refresh
  int64_t last_change_ms_ = 0;
  uint32_t last_seen_height_ = 0;     // snapshot-side tracking (LED flash)
  uint32_t last_rendered_height_ = 0; // screen-side tracking (digit diff)
  std::string last_rendered_price_;   // shared Moscow-time + price diff
};

}  // namespace btclock
