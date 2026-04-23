// Registered by mining_pool_stats_handler once pool-selection NVS pref
// lands (follow-up).

#include "mining_pool_public_pool/public_pool.hpp"

#include "mining_pool_public_pool/public_pool_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
constexpr const char* kLocalHostKey = "local_host";
// Default matches the old firmware DEFAULT_LOCAL_POOL_ENDPOINT
// ("btclock.local" / configurable on the settings page).
constexpr const char* kLocalHostDefault = "public-pool.local:40557";
}  // namespace

bool PublicPoolBase::parse_response(const char* body, ParsedStats& out) const {
  return public_pool::parse(body, out);
}

std::string PublicPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  // Port 40557 is non-standard and part of the documented API.
  return "https://public-pool.io:40557/api/client/" + user;
}

std::string LocalPublicPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  const std::string host =
      prefs.GetString(kLocalHostKey, kLocalHostDefault);
  // Plain HTTP — local deployments rarely have TLS.
  return "http://" + host + "/api/client/" + user;
}

}  // namespace mining_pools
}  // namespace btclock
