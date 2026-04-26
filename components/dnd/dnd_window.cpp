#include "dnd/dnd_window.hpp"

namespace btclock {
namespace dnd {

bool IsTimeInWindow(uint8_t hour, uint8_t minute, uint8_t start_hour,
                    uint8_t start_minute, uint8_t end_hour,
                    uint8_t end_minute) {
  // Everything collapses to minutes-past-midnight so wrap stays
  // arithmetic rather than a special case.
  const uint16_t now =
      static_cast<uint16_t>(hour) * 60u + static_cast<uint16_t>(minute);
  const uint16_t start = static_cast<uint16_t>(start_hour) * 60u +
                         static_cast<uint16_t>(start_minute);
  const uint16_t end =
      static_cast<uint16_t>(end_hour) * 60u + static_cast<uint16_t>(end_minute);

  // Degenerate equal-start-end window is treated as "never active" to
  // prevent users locking DND on by picking the same time twice.
  if (start == end) return false;
  if (start < end) return now >= start && now < end;
  return now >= start || now < end;
}

bool ComputeDndActive(uint8_t hour_now, uint8_t minute_now, uint8_t start_hour,
                      uint8_t start_minute, uint8_t end_hour,
                      uint8_t end_minute, bool dnd_enabled, bool time_enabled) {
  // Precedence mirrors the old firmware's LedHandler::isDNDActive:
  // the manual flag wins outright, the time-based window is only
  // consulted when the manual flag is off. Stacking them this way
  // lets the user flip DND on instantly without touching the schedule.
  if (dnd_enabled) return true;
  if (!time_enabled) return false;
  return IsTimeInWindow(hour_now, minute_now, start_hour, start_minute,
                        end_hour, end_minute);
}

}  // namespace dnd
}  // namespace btclock
