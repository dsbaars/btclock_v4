// See nwc_config.hpp. Pure-logic so host tests drive it via FakePrefs.

#include "settings/nwc_config.hpp"

#include "nwc/uri.hpp"
#include "settings/pref_keys.hpp"
#include "settings/schema.hpp"

namespace btclock {
namespace settings {

NwcConfig ReadNwcConfig(const PrefsReader& prefs) {
  NwcConfig out;
  out.enabled = ReadBool(prefs, prefs::kNwcEnabled);
  out.uri = ReadString(prefs, prefs::kNwcUri);
  out.refresh_secs = ReadU32(prefs, prefs::kNwcRefreshSecs);
  out.flash_on_payment = ReadBool(prefs, prefs::kNwcFlashOnPay);
  if (!out.uri.empty()) {
    out.parsed_ok =
        (nwc::ParsePairingUri(out.uri, out.parsed) == nwc::ParseError::kOk);
  }
  return out;
}

}  // namespace settings
}  // namespace btclock
