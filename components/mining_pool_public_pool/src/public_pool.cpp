#include "mining_pool_public_pool/public_pool.hpp"

#include "mining_pool_public_pool/public_pool_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
constexpr const char* kGlobalKey = "global";  // bool
constexpr const char* kLocalHostKey = "local_host";
// Default matches the old firmware DEFAULT_LOCAL_POOL_ENDPOINT
// ("btclock.local" / configurable on the settings page).
constexpr const char* kLocalHostDefault = "public-pool.local:40557";
// Port 40557 is non-standard and part of the documented public-pool
// API; both client and pool endpoints live behind it.
constexpr const char* kPublicPoolBase = "https://public-pool.io:40557";
}  // namespace

bool PublicPoolBase::parse_response(const char* body, ParsedStats& out) const {
  return public_pool::parse(body, out);
}

std::string PublicPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  if (prefs.GetBool(kGlobalKey, false)) {
    // Pool-wide endpoint — no address required.
    return std::string(kPublicPoolBase) + "/api/pool";
  }
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return std::string(kPublicPoolBase) + "/api/client/" + user;
}

bool PublicPool::parse_response(const char* body, ParsedStats& out) const {
  btclock::Prefs prefs(kPrefsNs);
  if (prefs.GetBool(kGlobalKey, false)) {
    return public_pool::parse_pool_global(body, out);
  }
  return public_pool::parse(body, out);
}

std::string LocalPublicPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  const std::string host = prefs.GetString(kLocalHostKey, kLocalHostDefault);
  // Plain HTTP — local deployments rarely have TLS.
  return "http://" + host + "/api/client/" + user;
}

}  // namespace mining_pools
}  // namespace btclock
