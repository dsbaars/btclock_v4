// JSON-level surface for GET / PATCH /api/settings. Pure-logic wrapper
// around cJSON so the host-test suite can drive it without linking
// ESP-IDF.
//
// The GET/PATCH contracts are defined by the existing WebUI, which
// talks to the old Arduino firmware (src/lib/net/webserver/settings.cpp).
// Field names, JSON types, and the distinction between string/uint/bool
// are fixed by that shape — changes ripple into every built
// settings-page tarball, so think twice before renaming anything here.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cJSON.h"
#include "settings/schema.hpp"

namespace btclock {
namespace settings {

// Read-side abstraction. In IDF builds the concrete implementation
// wraps btclock::Prefs; host tests inject an in-memory fake. Kept
// minimal — the schema table tells us which accessor to call.
class PrefsReader {
 public:
  virtual ~PrefsReader() = default;
  virtual std::string GetString(const char* key,
                                const char* default_value = "") const = 0;
  virtual uint32_t GetU32(const char* key,
                          uint32_t default_value = 0) const = 0;
  virtual int32_t GetI32(const char* key, int32_t default_value = 0) const = 0;
  virtual uint8_t GetU8(const char* key, uint8_t default_value = 0) const = 0;
  virtual bool GetBool(const char* key, bool default_value = false) const = 0;
};

// Write-side abstraction paired with PrefsReader. Split in two so a
// PATCH that hits no writable field can be serviced without opening
// an NVS handle for writing.
class PrefsWriter {
 public:
  virtual ~PrefsWriter() = default;
  virtual void SetString(const char* key, const char* value) = 0;
  virtual void SetU32(const char* key, uint32_t value) = 0;
  virtual void SetI32(const char* key, int32_t value) = 0;
  virtual void SetU8(const char* key, uint8_t value) = 0;
  virtual void SetBool(const char* key, bool value) = 0;
  virtual void Remove(const char* key) = 0;
};

// Device-side facts the GET handler embeds in the response — hostname,
// IP, hardware revision, etc. These can't come from NVS; the caller
// fills them in at the top of the request.
struct DeviceContext {
  std::string hostname;
  std::string ip;
  int32_t tx_power = 0;
  int32_t num_screens = 3;
  bool has_frontlight = false;
  bool has_light_level = false;
  // Most recent lux reading. Only emitted into the JSON when
  // `has_light_level` is also true, matching the old firmware's
  // hide-when-unavailable behaviour.
  float light_level = 0.0f;
  std::string hw_rev;
  std::string fs_rev;
  std::string git_rev;
  std::string git_tag;
  // Firmware build time as Unix seconds. Emitted as `lastBuildTime`
  // (integer); the WebUI treats a numeric value as seconds-since-epoch
  // and formats it locally. 0 means "unknown" and is omitted from the
  // response.
  int64_t last_build_time_unix = 0;
  // Fonts available to the renderer; propagated into `availableFonts`.
  std::vector<std::string> available_fonts;
  // Mining pool list propagated into `availablePools`.
  std::vector<std::string> available_pools;
  // Currencies propagated into `availableCurrencies`. The active subset
  // is read from prefs (kActCurrencies, comma-separated).
  std::vector<std::string> available_currencies;
  // Registered rotatable screens, in fallback order. PATCH handler
  // validates reorder payloads against this list.
  struct Screen {
    int id;
    std::string name;
  };
  std::vector<Screen> screens;
  // Screen api_ids the GET response must omit from `screens[]` even
  // though they are otherwise in the catalogue — e.g. the mining-pool
  // earnings slot when the active pool has no per-user payout stream.
  // PATCH validation still accepts these ids so a client posting the
  // full legacy shape is not rejected; the renderer simply skips the
  // hidden slot in rotation (main/app/screen_manager.cpp reads the
  // same capability directly).
  std::vector<int> hidden_screen_ids;
  // Feature-flag gates that also suppress screens from the emitted
  // `screens[]`. Same semantics as `hidden_screen_ids` — the catalogue
  // entries stay so PATCH reorders keep accepting the legacy full shape,
  // but a WebUI that consulted a fresh GET will never see the slots while
  // the feature is off.
  //   - `mining_pool_stats_enabled == false` drops ids 70 + 71.
  //   - `bitaxe_enabled == false` drops ids 80 + 81.
  // Default to `true` so host tests that don't populate the context
  // continue to emit the full catalogue; the IDF caller always sets
  // these explicitly from NVS.
  bool mining_pool_stats_enabled = true;
  bool bitaxe_enabled = true;
};

// Build the full GET /api/settings body. Caller owns the returned
// cJSON object and must cJSON_Delete it.
cJSON* BuildGetResponse(const PrefsReader& prefs, const DeviceContext& ctx);

// Outcome of ApplyPatch.
enum class PatchStatus : uint8_t {
  kOk,          // 200 OK
  kBadRequest,  // malformed JSON, out-of-range number, missing context
  kBadField,    // per-field type mismatch or catalog rejection
};

struct PatchResult {
  PatchStatus status = PatchStatus::kOk;
  // True when at least one touched field is boot_only. The controller
  // echoes this back as `{"rebootRequired": true}` alongside 200.
  bool reboot_required = false;
  // On kBadRequest, a short machine-readable error token. Stable
  // enough to match in tests: "json", "pool", "range:<key>",
  // "screens:partial_order", "screens:bad_id", ...
  std::string error;
  // Keys that were successfully written. Used by the controller to
  // fire change hooks (led brightness, dnd time set, etc.).
  std::vector<std::string> touched_keys;
};

// Parse + apply a PATCH body. `body_json` is the *entire* request body
// as a C string — caller owns it, ApplyPatch does not take ownership.
//
// Unknown fields are silently ignored (matches old firmware behaviour —
// the generic loop only writes keys it recognises from strSettings /
// uintSettings / boolSettings). Malformed JSON returns kBadRequest
// without touching NVS.
PatchResult ApplyPatch(const char* body_json, const DeviceContext& ctx,
                       const PrefsReader& prefs, PrefsWriter& writer);

}  // namespace settings
}  // namespace btclock
