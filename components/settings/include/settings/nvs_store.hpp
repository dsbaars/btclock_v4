// NVS-backed PrefsReader/PrefsWriter adapter. Wraps a single
// btclock::Prefs handle. Exposed as its own header (rather than
// anonymous in settings_nvs.cpp) so the control server can construct
// one on the stack inside its request handler and dispose of it before
// the response completes.
//
// The schema knows nothing about signed-int or u8 storage — Prefs only
// ships GetU32/SetU32. Int and UChar slots are therefore round-tripped
// through u32 with a sign/range-preserving cast. That matches how the
// old Arduino Preferences library behaves under the hood (every slot
// is a 4-byte payload with a type tag).

#pragma once

#include "prefs.hpp"
#include "settings/api.hpp"

namespace btclock {
namespace settings {

class NvsPrefs final : public PrefsReader, public PrefsWriter {
 public:
  explicit NvsPrefs(const char* ns_name) : prefs_(ns_name) {}

  // PrefsReader
  std::string GetString(const char* key,
                        const char* default_value) const override {
    return prefs_.GetString(key, default_value);
  }
  uint32_t GetU32(const char* key, uint32_t default_value) const override {
    return prefs_.GetU32(key, default_value);
  }
  int32_t GetI32(const char* key, int32_t default_value) const override {
    const uint32_t raw =
        prefs_.GetU32(key, static_cast<uint32_t>(default_value));
    return static_cast<int32_t>(raw);
  }
  uint8_t GetU8(const char* key, uint8_t default_value) const override {
    return static_cast<uint8_t>(prefs_.GetU32(key, default_value) & 0xFFu);
  }
  bool GetBool(const char* key, bool default_value) const override {
    return prefs_.GetBool(key, default_value);
  }

  // PrefsWriter
  void SetString(const char* key, const char* value) override {
    prefs_.SetString(key, value);
  }
  void SetU32(const char* key, uint32_t value) override {
    prefs_.SetU32(key, value);
  }
  void SetI32(const char* key, int32_t value) override {
    prefs_.SetU32(key, static_cast<uint32_t>(value));
  }
  void SetU8(const char* key, uint8_t value) override {
    prefs_.SetU32(key, static_cast<uint32_t>(value));
  }
  void SetBool(const char* key, bool value) override {
    prefs_.SetBool(key, value);
  }
  void Remove(const char* /*key*/) override {
    // Prefs component doesn't expose nvs_erase_key yet; defer on this
    // for now. The only caller is `txPower = 80` (reset to
    // default) and a follow-up can add Prefs::Remove when another
    // path also needs it.
  }

  // Force accumulated writes out to flash. Call exactly once at the
  // end of a PATCH so an oom-mid-write leaves NVS coherent.
  esp_err_t Commit() { return prefs_.Commit(); }

 private:
  mutable btclock::Prefs prefs_;
};

}  // namespace settings
}  // namespace btclock
