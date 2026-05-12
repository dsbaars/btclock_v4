// Host tests for the NWC-specific bits of /api/settings:
//   * URI redaction in BuildGetResponse (nwcUri masked, nwcUriSet/Masked
//     surfaced).
//   * ApplyPatch URI validation (well-formed vs malformed).
//   * ApplyPatch relay-budget invariant (NWC enable + nostrRelays
//     together must not exceed kMaxNostrRelays).
//   * ReadNwcConfig roundtrip via PrefsReader fake.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "cJSON.h"
#include "doctest.h"
#include "settings/api.hpp"
#include "settings/nostr_config.hpp"
#include "settings/nwc_config.hpp"
#include "settings/pref_keys.hpp"

namespace {

// A real NIP-47 URI: scheme + wallet pubkey + relay + secret. Built
// off the same shape Alby's faucet emits so the parser test surface
// matches a working live URI. The hex chars are arbitrary but valid
// (64 lowercase hex == 32-byte schnorr pubkey / secret).
constexpr const char* kValidUri =
    "nostr+walletconnect://"
    "abababababababababababababababababababababababababababababababab"
    "?relay=wss%3A%2F%2Frelay.example.com"
    "&secret="
    "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";

class FakePrefs final : public btclock::settings::PrefsReader,
                        public btclock::settings::PrefsWriter {
 public:
  std::string GetString(const char* key,
                        const char* default_value) const override {
    auto it = str_.find(key);
    return it != str_.end() ? it->second
                            : (default_value ? default_value : "");
  }
  uint32_t GetU32(const char* key, uint32_t default_value) const override {
    auto it = u32_.find(key);
    return it != u32_.end() ? it->second : default_value;
  }
  int32_t GetI32(const char* key, int32_t default_value) const override {
    auto it = i32_.find(key);
    return it != i32_.end() ? it->second : default_value;
  }
  uint8_t GetU8(const char* key, uint8_t default_value) const override {
    auto it = u8_.find(key);
    return it != u8_.end() ? it->second : default_value;
  }
  bool GetBool(const char* key, bool default_value) const override {
    auto it = b_.find(key);
    return it != b_.end() ? it->second : default_value;
  }
  void SetString(const char* key, const char* value) override {
    str_[key] = value ? value : "";
  }
  void SetU32(const char* key, uint32_t value) override { u32_[key] = value; }
  void SetI32(const char* key, int32_t value) override { i32_[key] = value; }
  void SetU8(const char* key, uint8_t value) override { u8_[key] = value; }
  void SetBool(const char* key, bool value) override { b_[key] = value; }
  void Remove(const char* key) override {
    str_.erase(key);
    u32_.erase(key);
    i32_.erase(key);
    u8_.erase(key);
    b_.erase(key);
  }

  std::map<std::string, std::string> str_;
  std::map<std::string, uint32_t> u32_;
  std::map<std::string, int32_t> i32_;
  std::map<std::string, uint8_t> u8_;
  std::map<std::string, bool> b_;
};

btclock::settings::DeviceContext DefaultCtx() {
  btclock::settings::DeviceContext ctx;
  ctx.hostname = "btclock-abc123";
  ctx.ip = "10.0.0.7";
  ctx.tx_power = 44;
  ctx.num_screens = 3;
  ctx.has_frontlight = true;
  ctx.has_light_level = false;
  ctx.hw_rev = "REV_B_EPD_2_13";
  ctx.fs_rev = "fs-42";
  ctx.git_rev = "deadbeef";
  ctx.available_currencies = {"USD", "EUR", "GBP"};
  return ctx;
}

}  // namespace

TEST_CASE("ReadNwcConfig: defaults when NVS empty") {
  FakePrefs prefs;
  const auto cfg = btclock::settings::ReadNwcConfig(prefs);
  CHECK_FALSE(cfg.enabled);
  CHECK(cfg.uri.empty());
  CHECK_FALSE(cfg.parsed_ok);
  CHECK(cfg.flash_on_payment == true);
}

TEST_CASE("ReadNwcConfig: roundtrip parsed URI") {
  FakePrefs prefs;
  prefs.b_["nwcEnabled"] = true;
  prefs.str_["nwcUri"] = kValidUri;
  prefs.u32_["nwcRefreshSecs"] = 120;
  prefs.b_["nwcFlashOnPay"] = false;

  const auto cfg = btclock::settings::ReadNwcConfig(prefs);
  CHECK(cfg.enabled);
  CHECK(cfg.uri == kValidUri);
  CHECK(cfg.parsed_ok);
  CHECK(cfg.refresh_secs == 120u);
  CHECK_FALSE(cfg.flash_on_payment);
  CHECK(cfg.parsed.wallet_pubkey_hex.size() == 64);
  REQUIRE(!cfg.parsed.relays.empty());
  CHECK(cfg.parsed.relays.front() == "wss://relay.example.com");
}

TEST_CASE("GET /api/settings redacts nwcUri") {
  FakePrefs prefs;
  prefs.str_["nwcUri"] = kValidUri;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);

  cJSON* uri = cJSON_GetObjectItemCaseSensitive(root, "nwcUri");
  // nwcUri is dropped from the response (mirrors httpAuthPass /
  // otaPass / proxyPass). If a future regression re-emits it, this
  // assertion catches the secret leak before it ships.
  if (uri != nullptr && cJSON_IsString(uri)) {
    CHECK(std::string(uri->valuestring).find("secret=") == std::string::npos);
    CHECK(std::string(uri->valuestring).find("cdcdcd") == std::string::npos);
  }
  cJSON* set = cJSON_GetObjectItemCaseSensitive(root, "nwcUriSet");
  REQUIRE(set != nullptr);
  CHECK(cJSON_IsTrue(set));
  cJSON* masked = cJSON_GetObjectItemCaseSensitive(root, "nwcUriMasked");
  REQUIRE(masked != nullptr);
  REQUIRE(cJSON_IsString(masked));
  const std::string m = masked->valuestring;
  // The mask keeps the literal `secret=` label as a UX cue but truncates
  // the value to an ellipsis + last 4 chars. Pin that the full 64-char
  // secret string is NOT echoed.
  CHECK(m.find("cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd") ==
        std::string::npos);
  // The full pubkey (64 chars) is also not echoed; only the leading 8.
  CHECK(m.find(
            "abababababababababababababababababababababababababababababababab") ==
        std::string::npos);
  cJSON_Delete(root);
}

TEST_CASE("GET /api/settings: nwcUriSet=false when unset") {
  FakePrefs prefs;
  cJSON* root = btclock::settings::BuildGetResponse(prefs, DefaultCtx());
  REQUIRE(root != nullptr);
  cJSON* set = cJSON_GetObjectItemCaseSensitive(root, "nwcUriSet");
  REQUIRE(set != nullptr);
  CHECK(cJSON_IsFalse(set));
  // No masked entry when URI is empty — saves wire bytes.
  CHECK(cJSON_GetObjectItemCaseSensitive(root, "nwcUriMasked") == nullptr);
  cJSON_Delete(root);
}

TEST_CASE("PATCH nwcUri: accepts well-formed URI") {
  FakePrefs prefs;
  std::string body = std::string("{\"nwcUri\":\"") + kValidUri + "\"}";
  const auto r =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(r.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nwcUri"] == kValidUri);
}

TEST_CASE("PATCH nwcUri: rejects malformed URI") {
  FakePrefs prefs;
  const std::string body =
      "{\"nwcUri\":\"http://wrong-scheme.example.com\"}";
  const auto r =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(r.status == btclock::settings::PatchStatus::kBadField);
  CHECK(r.error.find("nwcUri") != std::string::npos);
  // Failed PATCH must not touch NVS.
  CHECK(prefs.str_.count("nwcUri") == 0);
}

TEST_CASE("PATCH nwcUri: accepts empty string (clear)") {
  FakePrefs prefs;
  prefs.str_["nwcUri"] = kValidUri;
  const std::string body = "{\"nwcUri\":\"\"}";
  const auto r =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(r.status == btclock::settings::PatchStatus::kOk);
  CHECK(prefs.str_["nwcUri"].empty());
}

TEST_CASE("PATCH nwcEnabled=true: blocked when 4 data relays already set") {
  FakePrefs prefs;
  // 4 relays = kMaxNostrRelays; flipping nwcEnabled on would push over.
  prefs.str_["nostrRelays"] =
      "wss://r1.example.com,wss://r2.example.com,"
      "wss://r3.example.com,wss://r4.example.com";
  const std::string body = "{\"nwcEnabled\":true}";
  const auto r =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(r.status == btclock::settings::PatchStatus::kBadField);
  CHECK(r.error.find("nwcEnabled") != std::string::npos);
  CHECK(prefs.b_.count("nwcEnabled") == 0);
}

TEST_CASE("PATCH nostrRelays: blocked from 4th relay when NWC already on") {
  FakePrefs prefs;
  prefs.b_["nwcEnabled"] = true;
  prefs.str_["nostrRelays"] =
      "wss://r1.example.com,wss://r2.example.com,wss://r3.example.com";
  const std::string body =
      "{\"nostrRelays\":[\"wss://r1.example.com\",\"wss://r2.example.com\","
      "\"wss://r3.example.com\",\"wss://r4.example.com\"]}";
  const auto r =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(r.status == btclock::settings::PatchStatus::kBadField);
  CHECK(r.error.find("nostrRelays") != std::string::npos);
}

TEST_CASE("PATCH nwcEnabled+nostrRelays coherent submit succeeds") {
  // The single PATCH lowers the relay count to 3 AND turns NWC on. The
  // schema must evaluate the budget against the post-PATCH state, not
  // the pre-state, so a coherent submit is accepted.
  FakePrefs prefs;
  prefs.str_["nostrRelays"] =
      "wss://r1.example.com,wss://r2.example.com,"
      "wss://r3.example.com,wss://r4.example.com";
  const std::string body =
      "{\"nwcEnabled\":true,\"nostrRelays\":[\"wss://r1.example.com\","
      "\"wss://r2.example.com\",\"wss://r3.example.com\"]}";
  const auto r =
      btclock::settings::ApplyPatch(body.c_str(), DefaultCtx(), prefs, prefs);
  CHECK(r.status == btclock::settings::PatchStatus::kOk);
}
