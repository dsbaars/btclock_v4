// IDF-side glue for the HTTP Basic auth gate. Talks to httpd_req_t and
// NVS; delegates every byte-level decision to auth_gate_logic.hpp so
// the host tests can cover the parse/compare paths without ESP-IDF.

#include "auth_gate.hpp"
#include "auth_gate_logic.hpp"

#include <string>
#include <string_view>

#include "esp_log.h"
#include "settings/nvs_store.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace {
constexpr const char* kTag = "auth";

// Default username the factory firmware uses when the user hasn't
// customised it. Mirrors the old Arduino firmware behaviour so existing
// docs/tutorials keep working after the IDF port.
constexpr const char* kDefaultUser = "btclock";

// Sent on every 401 — browsers use the realm to scope saved credentials
// so a single WebUI origin only prompts once per session.
constexpr const char* kWwwAuthenticate = "Basic realm=\"btclock\"";

esp_err_t SendUnauthorized(httpd_req_t* req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "WWW-Authenticate", kWwwAuthenticate);
  // CORS echo so browser fetch() surfaces the 401 to JS instead of
  // opaque-failing. Every other response in this component sets `*`
  // too — keep it consistent.
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char kBody[] = "{\"error\":\"unauthorized\"}";
  httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  return ESP_OK;
}

}  // namespace

bool RequireHttpAuth(httpd_req_t* req) {
  // Read gate state from NVS on every call. NVS reads are cheap once
  // warmed and this avoids having to invalidate a snapshot whenever
  // PATCH /api/settings writes the auth fields.
  btclock::settings::NvsPrefs prefs(btclock::prefs::kSettingsNs);
  if (!prefs.GetBool(btclock::prefs::kHttpAuthEnabled, false)) {
    return true;
  }

  const std::string configured_user =
      prefs.GetString(btclock::prefs::kHttpAuthUser, kDefaultUser);
  const std::string configured_pass =
      prefs.GetString(btclock::prefs::kHttpAuthPass, "");

  // Recoverability: an empty stored password with auth enabled would
  // brick the user out of their own device. The WebUI validates this
  // before persisting, but we treat the state as misconfigured and
  // allow through with a loud log rather than require factory reset.
  if (configured_pass.empty()) {
    ESP_LOGW(kTag,
             "httpAuthEnabled=true but httpAuthPass empty — allowing "
             "request through to avoid lockout");
    return true;
  }

  const std::string_view user_view =
      configured_user.empty()
          ? std::string_view(kDefaultUser)
          : std::string_view(configured_user);

  // Read the Authorization header. `httpd_req_get_hdr_value_len`
  // returns the byte count excluding the terminator; the `_str` call
  // wants the destination sized one larger.
  const size_t hdr_len =
      httpd_req_get_hdr_value_len(req, "Authorization");
  if (hdr_len == 0) {
    return SendUnauthorized(req) == ESP_OK ? false : false;
  }

  // Guard against a pathological client sending a multi-kB header.
  // A legitimate Basic credential is ~50 bytes; 512 is enormous
  // headroom for long usernames + passwords without being a DoS vector.
  constexpr size_t kMaxHeader = 512;
  if (hdr_len > kMaxHeader) {
    SendUnauthorized(req);
    return false;
  }

  std::string hdr;
  hdr.resize(hdr_len + 1);
  if (httpd_req_get_hdr_value_str(req, "Authorization",
                                  hdr.data(), hdr.size()) != ESP_OK) {
    SendUnauthorized(req);
    return false;
  }
  // Trim the trailing NUL that `_str` writes — we want the C++ length
  // to match the byte count.
  hdr.resize(hdr_len);

  std::string_view token;
  if (!auth::ExtractBasicToken(hdr, &token)) {
    SendUnauthorized(req);
    return false;
  }

  std::string decoded;
  if (!auth::DecodeBase64(token, &decoded)) {
    SendUnauthorized(req);
    return false;
  }

  std::string supplied_user;
  std::string supplied_pass;
  if (!auth::ParseUserPass(decoded, &supplied_user, &supplied_pass)) {
    SendUnauthorized(req);
    return false;
  }

  if (!auth::CredentialsMatch(supplied_user, supplied_pass, user_view,
                              configured_pass)) {
    SendUnauthorized(req);
    return false;
  }

  return true;
}

bool RequireOtaEnabled(httpd_req_t* req) {
  btclock::settings::NvsPrefs prefs(btclock::prefs::kSettingsNs);
  // Default-true matches the factory state from the schema
  // (settings/schema.hpp: kOtaEnabled default=true).
  if (prefs.GetBool(btclock::prefs::kOtaEnabled, true)) {
    return true;
  }
  httpd_resp_set_status(req, "403 Forbidden");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  const char kBody[] = "{\"error\":\"ota_disabled\"}";
  httpd_resp_send(req, kBody, sizeof(kBody) - 1);
  return false;
}

}  // namespace btclock
