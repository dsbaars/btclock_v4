// Do-Not-Disturb subsystem — NVS-backed state + runtime "active" query.
//
// The old firmware kept DND fields inside LedHandler; the IDF port
// splits them into a free-standing subsystem because DND also gates
// the frontlight controller (a separate class) and, via /api/status,
// shapes HTTP responses that don't touch LEDs at all. Keeping DND as
// its own component lets both consumers (led_controller,
// frontlight_controller) query a single source of truth without a
// circular include.
//
// NVS layout matches the old firmware key strings so installs can
// migrate without a data conversion step. Namespace is new ("dnd");
// the old firmware used a single global Preferences namespace per
// ArduinoCore.

#pragma once

#include <cstdint>
#include <mutex>

#include "dnd/dnd_window.hpp"

namespace btclock {
namespace dnd {

struct DndConfig {
  bool enabled = false;       // manual override: LEDs off now regardless
  bool time_enabled = false;  // schedule gate
  uint8_t start_hour = 23;    // defaults mirror led_handler.cpp:446-449
  uint8_t start_minute = 0;
  uint8_t end_hour = 7;
  uint8_t end_minute = 0;
};

class Dnd {
 public:
  Dnd();

  // Load persisted state from NVS (namespace "dnd"). Idempotent. No
  // error path: a failed NVS read leaves the in-memory defaults in
  // place, mirroring old-firmware Preferences.get*(..., default) shape.
  void Load();

  // Accessors — all cheap, protected by the internal mutex so HTTP
  // handlers, LED task, and frontlight task can share the instance.
  DndConfig GetConfig() const;
  bool IsActive() const;  // uses wall-clock via localtime_r

  // Mutators — each persists the affected key(s) to NVS before
  // returning. Matches old-firmware writers: fire-and-forget from the
  // caller's perspective.
  void SetEnabled(bool enabled);
  void SetTimeEnabled(bool enabled);
  void SetTimeRange(uint8_t start_hour, uint8_t start_minute, uint8_t end_hour,
                    uint8_t end_minute);

 private:
  mutable std::mutex mu_;
  DndConfig cfg_;
};

// Process-wide singleton accessor. Constructed lazily on first call;
// Load() is called automatically so boot-order doesn't matter for
// consumers that just want "the current DND state".
Dnd& Instance();

}  // namespace dnd
}  // namespace btclock
