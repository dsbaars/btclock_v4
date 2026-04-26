#include "mining_pool_viabtc/viabtc.hpp"

#include "mining_pool_viabtc/viabtc_parser.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace mining_pools {

std::string ViaBtcPool::api_url() const {
  // ViaBTC's API key identifies the account, so the URL is constant —
  // no path component to template like Ocean's per-user statsnap.
  return "https://www.viabtc.com/res/openapi/v1/hashrate?coin=BTC";
}

std::string ViaBtcPool::api_key() const {
  // The shared `miningPoolUser` slot carries the API key for ViaBTC; the
  // GET emitter suppresses the raw value because user_is_secret() returns
  // true. See docs/WEBUI_MINING_POOL_FIELDS.md.
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  return settings.GetString(btclock::prefs::kMiningPoolUser, "");
}

bool ViaBtcPool::parse_response(const char* body, ParsedStats& out) const {
  return viabtc::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
