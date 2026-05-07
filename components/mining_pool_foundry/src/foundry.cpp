#include "mining_pool_foundry/foundry.hpp"

#include <ctime>
#include <string>

#include "mining_pool_foundry/foundry_parser.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace mining_pools {

std::string FoundryPool::api_url() const {
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  const std::string subacct =
      settings.GetString(btclock::prefs::kPoolWorker, "");
  if (subacct.empty()) return "";  // base class skips the poll

  // Anchor the day window at "48h ago in epoch ms". Foundry's v2
  // earnings endpoint returns one entry per UTC day (current day
  // partial); the parser walks to the latest entry. A 48h window is
  // wide enough to span a UTC midnight rollover and still surface a
  // current-day partial sample — 24h sometimes empties the window
  // briefly right after midnight. snprintf into a fixed buffer
  // matches the rest of the codebase (no std::to_string for long long).
  const long long now_ms = static_cast<long long>(std::time(nullptr)) * 1000LL;
  const long long start_ms = now_ms - 48LL * 60LL * 60LL * 1000LL;

  std::string url = "https://api.foundryusapool.com/v2/earnings/";
  url += subacct;
  url += "?coin=BTC&startDateUnixMs=";
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld", start_ms);
  url += buf;
  return url;
}

std::string FoundryPool::api_key() const {
  // miningPoolUser is the API key for Foundry; user_is_secret() drives
  // the GET-side suppression. See docs/WEBUI_MINING_POOL_FIELDS.md.
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  return settings.GetString(btclock::prefs::kMiningPoolUser, "");
}

bool FoundryPool::parse_response(const char* body, ParsedStats& out) const {
  return foundry::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
