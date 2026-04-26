// Host-side stub for pool_logos::HasResolvedLogo. The target build pulls
// in pool_logo_cache.cpp instead, which consults `/lfs/pool_logos/` and
// keeps a one-slot in-memory PSRAM buffer. Host tests have no LittleFS
// and don't run the EPD renderer, so the only consumer they care about
// is panel_texts::PoolLabelCellFor.
//
// After bd btclock_v4-5yi dropped the last vendored bitmap (`ocean`)
// the target-side `Lookup()` always returns nullptr, so the legacy
// `Lookup() != nullptr` short-circuit no longer covers the logo-path
// branch in panel_texts. Tests that need to exercise the logo branch
// register a synthetic pool name here via `RegisterTestLogo()`; the
// `kMiningPoolHashrate` "logo path" cases in test_panel_texts.cpp use
// it instead of relying on a real vendored entry.

#include "screens/assets/pool_logos.hpp"

#include <set>
#include <string>

namespace btclock {
namespace pool_logos {

namespace {

// Synthetic registry — only populated by the host test harness via
// `RegisterTestLogo`. Lives in this TU so the production target build
// (which links pool_logo_cache.cpp instead) is unaffected.
std::set<std::string>& TestRegistry() {
  static std::set<std::string> registry;
  return registry;
}

std::string ToLower(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    out.push_back(c);
  }
  return out;
}

}  // namespace

void RegisterTestLogo(const std::string& pool_name) {
  TestRegistry().insert(ToLower(pool_name));
}

void ClearTestLogos() {
  TestRegistry().clear();
}

bool HasResolvedLogo(const std::string& pool_name) {
  if (Lookup(pool_name) != nullptr) return true;
  return TestRegistry().count(ToLower(pool_name)) > 0;
}

}  // namespace pool_logos
}  // namespace btclock
