#include "dnd/dnd.hpp"

#include <ctime>

#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace dnd {
namespace {

constexpr const char* kNamespace = "dnd";

}  // namespace

Dnd::Dnd() = default;

void Dnd::Load() {
  Prefs p(kNamespace);
  std::lock_guard<std::mutex> lk(mu_);
  cfg_.enabled      = p.GetBool(prefs::kDndEnabled, cfg_.enabled);
  cfg_.time_enabled = p.GetBool(prefs::kDndTimeEnabled, cfg_.time_enabled);
  cfg_.start_hour   = static_cast<uint8_t>(
      p.GetU32(prefs::kDndStartHour, cfg_.start_hour) & 0xFFu);
  cfg_.start_minute = static_cast<uint8_t>(
      p.GetU32(prefs::kDndStartMin, cfg_.start_minute) & 0xFFu);
  cfg_.end_hour     = static_cast<uint8_t>(
      p.GetU32(prefs::kDndEndHour, cfg_.end_hour) & 0xFFu);
  cfg_.end_minute   = static_cast<uint8_t>(
      p.GetU32(prefs::kDndEndMin, cfg_.end_minute) & 0xFFu);
}

DndConfig Dnd::GetConfig() const {
  std::lock_guard<std::mutex> lk(mu_);
  return cfg_;
}

bool Dnd::IsActive() const {
  DndConfig snapshot;
  {
    std::lock_guard<std::mutex> lk(mu_);
    snapshot = cfg_;
  }
  // Short-circuit the clock read when the manual flag is already on;
  // it also means IsActive() stays cheap before SNTP has synced, which
  // happens during the boot-to-wifi window where time() would return a
  // bogus 1970 epoch value.
  if (snapshot.enabled) return true;
  if (!snapshot.time_enabled) return false;

  std::time_t t;
  std::time(&t);
  std::tm tm_now{};
  localtime_r(&t, &tm_now);
  return ComputeDndActive(
      static_cast<uint8_t>(tm_now.tm_hour),
      static_cast<uint8_t>(tm_now.tm_min),
      snapshot.start_hour, snapshot.start_minute,
      snapshot.end_hour, snapshot.end_minute,
      snapshot.enabled, snapshot.time_enabled);
}

void Dnd::SetEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.enabled = enabled;
  }
  Prefs p(kNamespace);
  p.SetBool(prefs::kDndEnabled, enabled);
  p.Commit();
}

void Dnd::SetTimeEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.time_enabled = enabled;
  }
  Prefs p(kNamespace);
  p.SetBool(prefs::kDndTimeEnabled, enabled);
  p.Commit();
}

void Dnd::SetTimeRange(uint8_t start_hour, uint8_t start_minute,
                       uint8_t end_hour, uint8_t end_minute) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.start_hour = start_hour;
    cfg_.start_minute = start_minute;
    cfg_.end_hour = end_hour;
    cfg_.end_minute = end_minute;
  }
  Prefs p(kNamespace);
  p.SetU32(prefs::kDndStartHour, start_hour);
  p.SetU32(prefs::kDndStartMin, start_minute);
  p.SetU32(prefs::kDndEndHour, end_hour);
  p.SetU32(prefs::kDndEndMin, end_minute);
  p.Commit();
}

Dnd& Instance() {
  // Leaky Meyers singleton — matches the ownership model of LedHandler
  // in the old firmware. No shutdown path; lifetime is the process.
  static Dnd* g = [] {
    auto* d = new Dnd();
    d->Load();
    return d;
  }();
  return *g;
}

}  // namespace dnd
}  // namespace btclock
