// Registered by mining_pool_stats_handler once pool-selection NVS pref
// lands (follow-up).

#include "mining_pool_satoshi_radio/satoshi_radio.hpp"

#include "mining_pool_common/ckpool_family_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
constexpr const char* kGlobalKey = "global";
constexpr const char* kBase = "https://pool.satoshiradio.nl/api/v1";
}  // namespace

std::string SatoshiRadioPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  if (prefs.GetBool(kGlobalKey, false)) {
    return std::string(kBase) + "/pool";
  }
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return std::string(kBase) + "/users/" + user;
}

bool SatoshiRadioPool::parse_response(const char* body,
                                      ParsedStats& out) const {
  return ckpool_family::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
