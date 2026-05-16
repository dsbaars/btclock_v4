#include "mining_pool_blitzpool/blitzpool.hpp"

#include "mining_pool_public_pool/public_pool_parser.hpp"
#include "prefs.hpp"

namespace btclock {
namespace mining_pools {

namespace {
constexpr const char* kPrefsNs = "pool";
constexpr const char* kUserKey = "user";
constexpr const char* kGlobalKey = "global";  // bool
// Single canonical host + port. Blitzpool's Stratum endpoints live on
// other ports; the read-only API is on 3334 with TLS.
constexpr const char* kApiBase = "https://blitzpool.yourdevice.ch:3334";
}  // namespace

std::string BlitzPool::api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  if (prefs.GetBool(kGlobalKey, false)) {
    // Pool-wide endpoint — totalHashRate + totalMiners across all 3
    // payout modes. No address needed.
    return std::string(kApiBase) + "/api/pool";
  }
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  return std::string(kApiBase) + "/api/client/" + user;
}

std::string BlitzPool::secondary_api_url() const {
  btclock::Prefs prefs(kPrefsNs);
  // Global-stats mode is address-less, so there is no PPLNS balance
  // to fetch. The earnings screen has nothing useful to show in this
  // mode anyway (rendered "n/a" via has_daily_sats=false).
  if (prefs.GetBool(kGlobalKey, false)) return "";
  const std::string user = prefs.GetString(kUserKey, "");
  if (user.empty()) return "";
  // /api/pplns/<addr> always responds (even for solo addresses; the
  // body just reports balanceSats=0). One endpoint covers all 3
  // payout modes — solo, pplns, group-solo — so no per-mode probe is
  // needed for the earnings field. Group-specific stats (group
  // hashrate, member list) are a phase-3 follow-up.
  return std::string(kApiBase) + "/api/pplns/" + user;
}

bool BlitzPool::parse_response(const char* body, ParsedStats& out) const {
  btclock::Prefs prefs(kPrefsNs);
  if (prefs.GetBool(kGlobalKey, false)) {
    return public_pool::parse_pool_global(body, out);
  }
  return public_pool::parse(body, out);
}

}  // namespace mining_pools
}  // namespace btclock
