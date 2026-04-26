// Do-Not-Disturb time-window algebra.
//
// Ported verbatim from lib/btclock/dnd_window.hpp — kept here as the
// pure-logic core so the host tests can link it without pulling the
// NVS-backed Dnd runtime. The contract below matches the old firmware's
// behaviour bit-for-bit; any change to the algebra must ship with the
// matching test updates.
//
// Contract:
//   * Range is half-open: window includes startHour:startMinute and
//     excludes endHour:endMinute.
//   * If endTime < startTime the window wraps midnight, e.g.
//     22:30 → 07:00 means "22:30 through 06:59".
//   * Start == End is treated as an empty window (never active) — so
//     picking the same start/end can't silently lock DND on forever.
//   * Hour/minute values must be in 0-23 / 0-59; out-of-range values
//     produce bounded but unspecified behaviour (still no crash).

#pragma once

#include <cstdint>

namespace btclock {
namespace dnd {

bool IsTimeInWindow(uint8_t hour, uint8_t minute, uint8_t start_hour,
                    uint8_t start_minute, uint8_t end_hour, uint8_t end_minute);

// Top-level "is DND currently active?" decision that also consults the
// two master flags. `dnd_enabled` forces DND on regardless of the clock
// (manual override). `time_enabled` gates the schedule check. Pure.
bool ComputeDndActive(uint8_t hour_now, uint8_t minute_now, uint8_t start_hour,
                      uint8_t start_minute, uint8_t end_hour,
                      uint8_t end_minute, bool dnd_enabled, bool time_enabled);

}  // namespace dnd
}  // namespace btclock
