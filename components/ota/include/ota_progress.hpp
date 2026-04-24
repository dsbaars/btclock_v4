// Pure-logic OTA progress helpers.
//
// Split out of ota_manager.hpp so host tests can include these without
// dragging in the ESP-IDF headers the manager itself pulls (esp_err,
// esp_http_client, mbedtls). Everything here is header-only and
// free of side effects.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace btclock {

// Emitted by OtaManager to a caller-installed progress callback as the
// push-OTA write progresses. `total` is 0 when Content-Length wasn't
// supplied — the LED helper pulses a single pixel in that case.
struct OtaProgress {
  // Bytes written to flash so far.
  std::size_t written = 0;
  // Expected total bytes (0 if unknown).
  std::size_t total = 0;
  enum class Phase : std::uint8_t {
    kStarting,   // pre-write hook fired, first byte not yet landed
    kWriting,    // in the body loop, one event per ~16 KiB step
    kVerifying,  // SHA-256 compare (brief)
    kRebooting,  // success; esp_restart imminent
    kFailed,     // any error path; the `written` field stops updating
  };
  Phase phase = Phase::kStarting;
};

// Map a [0, 1] progress fraction to a whole number of lit LEDs on the
// 4-pixel strip. The mapping is:
//   [0.00, 0.25) -> 1
//   [0.25, 0.50) -> 2
//   [0.50, 0.75) -> 3
//   [0.75, 1.00] -> 4
// The "always at least 1" rule mirrors the UX intent: as soon as the
// upload has been accepted, the user sees a lit pixel. Out-of-range
// inputs clamp to the nearest endpoint.
constexpr int ProgressFractionToLedCount(float fraction) {
  if (!(fraction > 0.0f)) return 1;  // NaN / negative / exactly 0
  if (fraction >= 1.0f) return 4;
  if (fraction >= 0.75f) return 4;
  if (fraction >= 0.50f) return 3;
  if (fraction >= 0.25f) return 2;
  return 1;
}

// Compute the progress fraction from written/total byte counts. Returns
// 0 when `total == 0` (Content-Length missing) so the LED path can
// fall back to the indeterminate-indicator behaviour. Clamps to [0, 1].
constexpr float ProgressFraction(std::size_t written, std::size_t total) {
  if (total == 0) return 0.0f;
  if (written >= total) return 1.0f;
  return static_cast<float>(written) / static_cast<float>(total);
}

}  // namespace btclock
