// Registered by mining_pool_stats_handler once pool-selection NVS pref
// lands (follow-up). For now the class compiles + runs but main.cpp
// does not instantiate it; the user wires up the selection logic after
// all seven pool sources exist.

#include "mining_pool_braiins/braiins.hpp"

#include "mining_pool_braiins/braiins_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
// NVS key names match the old firmware's intent so an in-place upgrade
// keeps the user's existing API token. The new prefs layer groups pool
// keys under a dedicated "pool" namespace; the upgrade shim rewrites
// `preferences.miningPoolUser` -> `pool.user` on first boot.
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
constexpr const char* kUserDefault = "";
}  // namespace

std::string BraiinsPool::api_url() const {
  // URL is constant; the account is identified by the Pool-Auth-Token
  // header rather than a path segment.
  return "https://pool.braiins.com/accounts/profile/json/btc/";
}

std::string BraiinsPool::auth_token() const {
  btclock::Prefs prefs(kPrefsNs);
  return prefs.GetString(kUserKey, kUserDefault);
}

bool BraiinsPool::parse_response(const char* body, ParsedStats& out) const {
  return braiins::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
