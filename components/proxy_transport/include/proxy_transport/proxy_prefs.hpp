#pragma once

// Build a proxy::Config from the device's NVS settings. Header-only so
// callers don't pay a link-time dep on proxy_transport beyond what
// they already pull in. The PrefsReader interface lives in settings/,
// so the only extra `REQUIRES` for callers is `settings`.

#include <cstdint>
#include <string>

#include "proxy_transport/proxy_bypass.hpp"
#include "proxy_transport/proxy_config.hpp"
#include "settings/api.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace proxy {

inline Config LoadConfigFromPrefs(const settings::PrefsReader& prefs) {
  Config c;
  if (!prefs.GetBool(prefs::kProxyEnabled, false)) {
    return c;  // kind == kNone
  }
  uint8_t t = prefs.GetU8(prefs::kProxyType, 0);
  if (t > static_cast<uint8_t>(Kind::kSocks5)) t = 0;
  c.kind = static_cast<Kind>(t);
  c.host = prefs.GetString(prefs::kProxyHost, "");
  c.port = static_cast<uint16_t>(prefs.GetU32(prefs::kProxyPort, 1080));
  c.user = prefs.GetString(prefs::kProxyUser, "");
  c.pass = prefs.GetString(prefs::kProxyPass, "");
  std::string bypass_csv =
      prefs.GetString(prefs::kProxyBypass, "*.local,192.168.*,10.*,127.0.0.1");
  SplitBypassList(bypass_csv, &c.bypass);
  if (c.host.empty() || c.port == 0) c.kind = Kind::kNone;
  return c;
}

}  // namespace proxy
}  // namespace btclock
