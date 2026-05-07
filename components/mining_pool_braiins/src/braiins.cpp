#include "mining_pool_braiins/braiins.hpp"

#include "mining_pool_braiins/braiins_parser.hpp"
#include "prefs.hpp"
#include "settings/pref_keys.hpp"

namespace btclock {
namespace mining_pools {

std::string BraiinsPool::api_url() const {
  // URL is constant; the account is identified by the Pool-Auth-Token
  // header rather than a path segment.
  return "https://pool.braiins.com/accounts/profile/json/btc/";
}

std::string BraiinsPool::auth_token() const {
  // Shared `miningPoolUser` slot — same convention as ViaBTC / Foundry.
  // The Braiins token is a public account ID, not a secret, so
  // user_is_secret() stays false (no GET-side suppression).
  btclock::Prefs settings(btclock::prefs::kSettingsNs);
  return settings.GetString(btclock::prefs::kMiningPoolUser, "");
}

bool BraiinsPool::parse_response(const char* body, ParsedStats& out) const {
  return braiins::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
