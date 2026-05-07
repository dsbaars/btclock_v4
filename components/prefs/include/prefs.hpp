#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "nvs.h"

namespace btclock {

// Thin RAII wrapper around an NVS namespace. Handles the "not-found"
// case by returning a default rather than surfacing as an error — that
// matches the production firmware's `preferences.get*(key, default)`
// ergonomics.
class Prefs {
 public:
  // Mount and format on corruption. Safe to call multiple times across
  // the program; no-op after the first success.
  static esp_err_t InitOnce();

  explicit Prefs(const char* ns_name);
  ~Prefs();

  Prefs(const Prefs&) = delete;
  Prefs& operator=(const Prefs&) = delete;

  std::string GetString(const char* key, const char* default_value = "") const;
  esp_err_t SetString(const char* key, const char* value);

  uint32_t GetU32(const char* key, uint32_t default_value = 0) const;
  esp_err_t SetU32(const char* key, uint32_t value);

  bool GetBool(const char* key, bool default_value = false) const;
  esp_err_t SetBool(const char* key, bool value);

  // Erase a single key. Returns ESP_ERR_NVS_NOT_FOUND if the key wasn't
  // there to begin with — callers that treat "remove a key that was
  // never set" as a no-op should ignore that result. Caller still has
  // to Commit() before close to make the deletion durable.
  esp_err_t Remove(const char* key);

  // Force pending writes out to flash.
  esp_err_t Commit();

 private:
  nvs_handle_t handle_ = 0;
  bool open_ = false;
};

}  // namespace btclock
