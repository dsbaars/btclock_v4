// Minimal emscripten-side stand-in for btclock::epd::IEpdPanel.
//
// The real driver under components/epd/ pulls ESP-IDF GPIO/SPI + the
// mcp23017 headers — none of which compile with em++. The screen
// renderers (main/screens/*.cpp) only touch a panel through
//   - epd::IEpdPanel::kStride (constexpr)
//   - epd::IEpdPanel::Width() / Height()
//   - epd::IEpdPanel::DrawFramebufferStart(fb, kind)
//   - epd::IEpdPanel::WaitForRefresh()
// so this shim provides exactly that surface, backed by plain memory.
// Class lives in btclock::epd:: with the same name as the device-side
// interface so the renderers reference one type-name in both worlds.
//
// DrawFramebufferStart / WaitForRefresh are no-ops here — the renderer
// already wrote to the caller-provided `fb_storage[N][16*296]`, and the
// WASM binding pulls those bytes out after the renderer returns.

#pragma once

#include <cstdint>

#include "font.hpp"  // for Rotation (re-used by PrepFb)

namespace btclock {

enum class RefreshKind : uint8_t {
  kFull,
  kPartial,
};

namespace epd {

// Mirror the subset of the device enums the renderers reference.
enum class PanelKind : uint8_t {
  k2_13,  // 122 x 250 — only variant we target for WASM preview today.
  k2_9,   // 128 x 296 — follow-up.
};

class IEpdPanel {
 public:
  explicit IEpdPanel(PanelKind kind = PanelKind::k2_13) : kind_(kind) {}

  // Match the device class' no-copy contract so code patterns port over.
  IEpdPanel(const IEpdPanel&) = delete;
  IEpdPanel& operator=(const IEpdPanel&) = delete;

  int Width() const { return kind_ == PanelKind::k2_13 ? 122 : 128; }
  int Height() const { return kind_ == PanelKind::k2_13 ? 250 : 296; }
  static constexpr int kStride = 16;
  int FrameBytes() const { return kStride * Height(); }

  // The renderers treat these as fire-and-forget; the frame bytes are
  // already in the caller's fb_storage. Return 0 ("ESP_OK") equivalent.
  // `last_refresh_kind_` captures the most recent RefreshKind passed in
  // — host-testable via `last_refresh_kind()`, used by tools/wasm/
  // smoke_test.mjs to verify the full/partial decoupling (btclock_v4-
  // jo6).
  int DrawFramebufferStart(const uint8_t* /*fb*/,
                           RefreshKind kind = RefreshKind::kFull) {
    last_refresh_kind_ = kind;
    ++refresh_count_;
    return 0;
  }
  int WaitForRefresh(uint32_t /*timeout_ms*/ = 10'000) { return 0; }

  // Host-only accessors for the refresh-kind sidechannel. Unused on
  // device (the real epd::IEpdPanel doesn't expose these).
  RefreshKind last_refresh_kind() const { return last_refresh_kind_; }
  int refresh_count() const { return refresh_count_; }
  void reset_refresh_state() {
    last_refresh_kind_ = RefreshKind::kFull;
    refresh_count_ = 0;
  }

 private:
  PanelKind kind_;
  // Default to kFull so a test that forgets to paint any panel still
  // sees the "pre-initialized" sentinel and doesn't accidentally read
  // stale state from a previous test.
  RefreshKind last_refresh_kind_ = RefreshKind::kFull;
  int refresh_count_ = 0;
};

}  // namespace epd
}  // namespace btclock
